#include "net/coop_session.h"

#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace net {
namespace {

// How often the events that were not acknowledged are sent again.
constexpr float kResendInterval = 0.2f;
uint32_t NowMs() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

CoopSession::CoopSession(NetworkManager& network) : network_(network) {
    network_.SetCoopHandler(this);
}

CoopSession::~CoopSession() {
    if (network_.GetMode() != NetMode::Offline) network_.SetCoopHandler(nullptr);
}

void CoopSession::SetEnabled(bool enabled) {
    enabled_ = enabled;
    if (enabled) {
        session_id_ = NowMs();
        revision_ = 0;
        start_epoch_ = 0;
        phase_ = coop::Phase::Lobby;
        present_mask_ = 0;
        ready_mask_ = 0;
        loaded_mask_ = 0;
        room_revision_ = 0;
        last_applied_seq_ = 0;
        next_event_seq_ = 0;
        divergence_count_ = 0;
        stale_count_ = 0;
        applied_events_ = 0;
        sent_events_ = 0;
        dropped_events_ = 0;
        unacked_.clear();
        pending_events_.clear();
        has_snapshot_ = false;
        if (IsHost()) {
            // The host knows who is in the room from the moment it starts.
            present_mask_ = 1;
            PublishRoom();
        }
    } else {
        network_.SetCoopHandler(nullptr);
        phase_ = coop::Phase::Idle;
    }
}

void CoopSession::Update(float dt) {
    if (!enabled_) return;

    if (IsHost()) {
        // A peer that is loading the level is not silent because it is
        // broken, it is silent because it is reading a level. The timeout
        // has to follow the phase.
        network_.SetPeerTimeout(phase_ == coop::Phase::Loading ? loading_timeout_ : lobby_timeout_);

        // The room goes out again now and then, so a guest that joined late
        // still learns which mission the host named.
        room_timer_ += dt;
        if (room_timer_ >= 0.5f) {
            room_timer_ = 0.0f;
            PublishRoom();
        }

        resend_timer_ += dt;
        if (resend_timer_ >= kResendInterval) {
            resend_timer_ = 0.0f;
            // The list of what was not acknowledged is shared with the thread
            // that publishes the events, so it is read under the lock. The
            // packets go out after the lock is let go.
            bool pending = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pending = !unacked_.empty();
            }
            if (pending) ResendUnacked();
        }
    } else if (IsGuest()) {
        network_.SetPeerTimeout(phase_ == coop::Phase::Loading ? loading_timeout_ : lobby_timeout_);
    }
}

void CoopSession::PublishSnapshot(const coop::Snapshot& snapshot) {
    if (!enabled_ || !IsHost()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_snapshot_ = snapshot;
        pending_snapshot_.header.start_epoch = start_epoch_;
        has_snapshot_ = true;
    }
    // The snapshot goes out now. The world layer built it because the world
    // changed, and it already decided how often that happens: a second clock
    // here would make the rate depend on when the two clocks happen to agree,
    // and the guest would see the actors in steps of two hundred milliseconds
    // instead of a hundred.
    SendSnapshot();
}

void CoopSession::PublishEvent(const coop::Event& event) {
    if (!enabled_ || !IsHost()) return;
    coop::Event outgoing = event;
    outgoing.state.start_epoch = start_epoch_;
    {
        // The events come from two threads: the damage of an actor is
        // published from the engine's thread, inside the damage routine, and
        // the events of the level script are published from the thread that
        // draws. The number of the sequence and the list of what was not
        // acknowledged are shared, so they are under a lock.
        //
        // The lock is not held while the packet goes out: the transport has
        // its own lock, and the transport calls back into the session with
        // its lock held. Holding both at once, in two orders, is a deadlock.
        std::lock_guard<std::mutex> lock(mutex_);
        outgoing.state.event_seq = ++next_event_seq_;
        PendingEvent pending;
        pending.event = outgoing;
        pending.seq = outgoing.state.event_seq;
        unacked_.push_back(pending);
        sent_events_++;
    }
    SendEvent(outgoing);
    LOGI("coop: event epoch=%u seq=%u type=%u", outgoing.state.start_epoch, outgoing.state.event_seq,
         outgoing.state.event_type);
}

void CoopSession::SetTimeouts(float lobby_seconds, float loading_seconds) {
    lobby_timeout_ = lobby_seconds;
    loading_timeout_ = loading_seconds;
}

void CoopSession::HostStartMission(uint8_t mission_id) {
    if (!enabled_ || !IsHost()) return;
    mission_id_ = mission_id;
    phase_ = coop::Phase::Loading;
    PublishRoom();
    LOGI("coop: mission epoch=%u mission=%u", start_epoch_, mission_id_);
}

void CoopSession::SetLoaded(bool loaded) {
    if (!enabled_) return;
    loaded_ = loaded;
    if (IsGuest()) {
        SendReady();
        return;
    }
    if (IsHost()) {
        // The host loads the level too, so its own bit counts.
        const uint8_t bit = 1;
        if (loaded) loaded_mask_ |= bit; else loaded_mask_ &= static_cast<uint8_t>(~bit);
        TryStartRound();
    }
}

void CoopSession::SetReady(bool ready) {
    if (!enabled_) return;
    ready_ = ready;
    if (IsGuest()) {
        SendReady();
        return;
    }
    if (IsHost()) {
        // The host is ready when it is: its own bit counts, the same way the
        // bit of the loaded level does.
        const uint8_t bit = 1;
        if (ready) {
            ready_mask_ |= bit;
        } else {
            ready_mask_ &= static_cast<uint8_t>(~bit);
        }
        PublishRoom();
    }
}

// The mission is over: the round ends on both sides.
void CoopSession::EndRound() {
    if (!enabled_ || !IsHost()) return;
    if (phase_ == coop::Phase::Ended) return;
    phase_ = coop::Phase::Ended;
    PublishRoom();
    LOGI("coop: round epoch=%u ended", start_epoch_);
}

void CoopSession::TryStartRound() {
    if (!enabled_ || !IsHost()) return;
    if (phase_ != coop::Phase::Loading) return;
    present_mask_ = network_.GetPresentMask();
    if (present_mask_ == 0) return;
    // Every peer that is present has to have loaded before the round starts.
    const uint8_t everyone = static_cast<uint8_t>(present_mask_ & loaded_mask_);
    if (everyone == present_mask_) StartRound();
}

void CoopSession::PublishRoom() {
    if (!enabled_ || !IsHost()) return;
    present_mask_ = network_.GetPresentMask();
    coop::Room room{};
    room.state.session_id = session_id_;
    room.state.start_epoch = start_epoch_;
    room.state.mission_id = mission_id_;
    room.state.phase = static_cast<uint8_t>(phase_);
    room.state.present_mask = present_mask_;
    room.state.ready_mask = ready_mask_;
    room.state.loaded_mask = loaded_mask_;

    // The revision only moves when the room really changes, so the guest
    // does not have to look at a room that says the same thing.
    if (std::memcmp(&room.state, &published_, sizeof(coop::RoomState)) == 0) return;
    std::memcpy(&published_, &room.state, sizeof(coop::RoomState));
    room.state.revision = ++revision_;

    LOGI("coop: publish room revision=%u phase=%u epoch=%u", room.state.revision, room.state.phase,
         room.state.start_epoch);
    uint8_t buffer[sizeof(PacketHeader) + sizeof(coop::RoomState)];
    PacketHeader header{};
    header.magic = kProtocolMagic;
    header.type = static_cast<uint8_t>(MsgType::CoopRoom);
    header.player_id = 0;
    header.sequence = 0;
    header.timestamp_ms = NowMs();
    std::memcpy(buffer, &header, sizeof(header));
    const size_t written = coop::EncodeRoom(room, buffer + sizeof(header), sizeof(buffer) - sizeof(header));
    if (written == 0) return;
    network_.BroadcastCoop(buffer, sizeof(header) + written);
}

void CoopSession::SendReady() {
    if (!enabled_ || !IsGuest()) return;
    coop::Ready ready{};
    ready.state.start_epoch = room_.state.start_epoch;
    ready.state.ready = ready_ ? 1 : 0;
    ready.state.loaded = loaded_ ? 1 : 0;

    uint8_t buffer[sizeof(PacketHeader) + sizeof(coop::ReadyState)];
    PacketHeader header{};
    header.magic = kProtocolMagic;
    header.type = static_cast<uint8_t>(MsgType::CoopReady);
    header.player_id = network_.GetLocalPlayerId();
    header.sequence = 0;
    header.timestamp_ms = NowMs();
    std::memcpy(buffer, &header, sizeof(header));
    const size_t written = coop::EncodeReady(ready, buffer + sizeof(header), sizeof(buffer) - sizeof(header));
    if (written == 0) return;
    network_.SendCoopToHost(buffer, sizeof(header) + written);
}

void CoopSession::SendSnapshot() {
    if (!enabled_ || !IsHost() || !has_snapshot_) return;
    coop::Snapshot snapshot = pending_snapshot_;
    snapshot.header.start_epoch = start_epoch_;
    snapshot.header.part_index = 0;
    snapshot.header.part_count = 1;

    uint8_t buffer[sizeof(PacketHeader) + coop::kMaxSnapshotBytes];
    PacketHeader header{};
    header.magic = kProtocolMagic;
    header.type = static_cast<uint8_t>(MsgType::CoopSnapshot);
    header.player_id = 0;
    header.sequence = 0;
    header.timestamp_ms = NowMs();
    std::memcpy(buffer, &header, sizeof(header));
    const size_t written = coop::EncodeSnapshot(snapshot, buffer + sizeof(header), sizeof(buffer) - sizeof(header));
    if (written == 0) return;
    sent_snapshots_++;
    network_.BroadcastCoop(buffer, sizeof(header) + written);
}

void CoopSession::SendEvent(const coop::Event& event) {
    if (!enabled_ || !IsHost()) return;
    uint8_t buffer[sizeof(PacketHeader) + sizeof(coop::EventState)];
    PacketHeader header{};
    header.magic = kProtocolMagic;
    header.type = static_cast<uint8_t>(MsgType::CoopEvent);
    header.player_id = 0;
    header.sequence = 0;
    header.timestamp_ms = NowMs();
    std::memcpy(buffer, &header, sizeof(header));
    const size_t written = coop::EncodeEvent(event, buffer + sizeof(header), sizeof(buffer) - sizeof(header));
    if (written == 0) return;
    network_.BroadcastCoop(buffer, sizeof(header) + written);
}

void CoopSession::ResendUnacked() {
    // The list is copied under the lock: the thread that publishes the events
    // may be adding to it while the packets go out.
    std::vector<PendingEvent> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending = unacked_;
    }
    for (const PendingEvent& item : pending) {
        SendEvent(item.event);
    }
    if (!pending.empty()) {
        LOGI("coop: resend events=%zu from=%u", pending.size(), pending.front().seq);
    }
}

void CoopSession::StartRound() {
    start_epoch_++;
    phase_ = coop::Phase::InMission;
    next_event_seq_ = 0;
    last_applied_seq_ = 0;
    unacked_.clear();
    pending_events_.clear();
    PublishRoom();
    if (round_handler_) round_handler_(start_epoch_, true);
    LOGI("coop: round epoch=%u started", start_epoch_);
}

void CoopSession::ApplyEvent(const coop::Event& event) {
    applied_events_++;
    if (event_handler_) event_handler_(event);
}

void CoopSession::DrainPending() {
    // The events that arrived early wait for their turn. The host sends the
    // ones that were not acknowledged again, so the gap fills.
    while (true) {
        auto it = pending_events_.find(last_applied_seq_ + 1);
        if (it == pending_events_.end()) break;
        last_applied_seq_ = it->first;
        ApplyEvent(it->second);
        pending_events_.erase(it);
    }
}

void CoopSession::OnCoopRoom(const uint8_t* data, size_t size) {
    if (!enabled_) return;
    coop::Room room{};
    if (!coop::DecodeRoom(data, size, &room)) return;
    // A room that is not newer than the one we have is not news. The same
    // room sent twice is applied twice, and that is harmless.
    if (room.state.revision <= room_revision_ && room.state.session_id == room_.state.session_id) return;

    const bool was_loading = phase_ == coop::Phase::Loading;
    room_ = room;
    room_revision_ = room.state.revision;
    phase_ = static_cast<coop::Phase>(room.state.phase);
    mission_id_ = room.state.mission_id;

    LOGI("coop: room revision=%u epoch=%u phase=%u", room.state.revision, room.state.start_epoch,
         room.state.phase);

    if (phase_ == coop::Phase::Loading && !was_loading) {
        // The host named the mission: tell it we are on it.
        SendReady();
    }
    if (phase_ == coop::Phase::InMission && room.state.start_epoch != start_epoch_) {
        start_epoch_ = room.state.start_epoch;
        last_applied_seq_ = 0;
        pending_events_.clear();
        if (round_handler_) round_handler_(start_epoch_, true);
        LOGI("coop: round epoch=%u started", start_epoch_);
    }
}

void CoopSession::OnCoopReady(uint8_t player_id, const uint8_t* data, size_t size) {
    if (!enabled_ || !IsHost()) return;
    if (player_id == 0 || player_id >= kMaxPlayers) return;
    coop::Ready ready{};
    if (!coop::DecodeReady(data, size, &ready)) return;
    if (ready.state.start_epoch != start_epoch_) {
        // The guest is answering about another round.
        stale_count_++;
        return;
    }
    const uint8_t bit = static_cast<uint8_t>(1u << player_id);
    if (ready.state.ready) ready_mask_ |= bit; else ready_mask_ &= static_cast<uint8_t>(~bit);
    if (ready.state.loaded) loaded_mask_ |= bit; else loaded_mask_ &= static_cast<uint8_t>(~bit);
    TryStartRound();
}

void CoopSession::OnCoopSnapshot(const uint8_t* data, size_t size) {
    if (!enabled_) return;
    coop::Snapshot snapshot{};
    if (!coop::DecodeSnapshot(data, size, &snapshot)) return;
    if (snapshot.header.start_epoch != start_epoch_) {
        stale_count_++;
        return;
    }
    received_snapshots_++;
    if (snapshot_handler_) snapshot_handler_(snapshot);
}

void CoopSession::OnCoopEvent(const uint8_t* data, size_t size) {
    if (!enabled_) return;
    coop::Event event{};
    if (!coop::DecodeEvent(data, size, &event)) return;
    if (event.state.start_epoch != start_epoch_) {
        // An event from another round belongs to a mission that is over.
        stale_count_++;
        return;
    }
    if (event.state.event_seq <= last_applied_seq_) {
        // Already applied. A repeated packet must not kill an actor twice.
        //
        // The acknowledgement goes out anyway. The host resends what it has
        // not seen acknowledged, so if the first acknowledgement was lost the
        // host would resend this event for the rest of the session, and the
        // guest would drop it again and again.
        dropped_events_++;
        SendAck();
        return;
    }
    if (event.state.event_seq > last_applied_seq_ + 1) {
        // A hole in the sequence. The guest waits for the missing one, and
        // the hole is counted so it is visible.
        divergence_count_++;
        pending_events_[event.state.event_seq] = event;
        return;
    }
    last_applied_seq_ = event.state.event_seq;
    ApplyEvent(event);
    DrainPending();

    // The mission is over: the round ends on the guest too.
    if (event.state.event_type == static_cast<uint8_t>(coop::EventType::MissionEnded)) {
        phase_ = coop::Phase::Ended;
        LOGI("coop: round epoch=%u ended", start_epoch_);
    }

    // Tell the host how far we got, so it can stop sending.
    SendAck();
}

// The guest tells the host how far it got. The acknowledgement is
// idempotent: it says "everything up to this sequence is applied", so the
// guest can send it again without harm.
void CoopSession::SendAck() {
    coop::AckState ack{};
    ack.start_epoch = start_epoch_;
    ack.event_seq = last_applied_seq_;
    uint8_t buffer[sizeof(PacketHeader) + sizeof(coop::AckState)];
    PacketHeader header{};
    header.magic = kProtocolMagic;
    header.type = static_cast<uint8_t>(MsgType::CoopAck);
    header.player_id = network_.GetLocalPlayerId();
    header.sequence = 0;
    header.timestamp_ms = NowMs();
    std::memcpy(buffer, &header, sizeof(header));
    const size_t written = coop::EncodeAck(ack, buffer + sizeof(header), sizeof(buffer) - sizeof(header));
    if (written == 0) return;
    acks_sent_++;
    network_.SendCoopToHost(buffer, sizeof(header) + written);
}

void CoopSession::OnCoopAck(uint8_t player_id, const uint8_t* data, size_t size) {
    if (!enabled_ || !IsHost()) return;
    coop::AckState ack{};
    if (!coop::DecodeAck(data, size, &ack)) return;
    if (ack.start_epoch != start_epoch_) {
        stale_count_++;
        return;
    }
    (void)player_id;
    // Everything up to the acknowledged sequence is done with.
    std::lock_guard<std::mutex> lock(mutex_);
    unacked_.erase(std::remove_if(unacked_.begin(), unacked_.end(),
                                  [ack](const PendingEvent& pending) { return pending.seq <= ack.event_seq; }),
                   unacked_.end());
}

}  // namespace net
