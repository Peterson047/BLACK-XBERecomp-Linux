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
// How often the guest says it is on it. The datagram can be lost, and the
// pair would wait for it for the rest of the session.
constexpr float kReadyInterval = 0.5f;
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
    enabled_.store(enabled, std::memory_order_release);
    if (enabled) {
        session_id_ = NowMs();
        revision_ = 0;
        start_epoch_.store(0, std::memory_order_release);
        phase_.store(coop::Phase::Lobby, std::memory_order_release);
        present_mask_ = 0;
        ready_mask_ = 0;
        loaded_mask_ = 0;
        room_revision_ = 0;
        divergence_count_ = 0;
        hole_count_ = 0;
        stale_count_ = 0;
        applied_events_ = 0;
        sent_events_ = 0;
        crossed_events_ = 0;
        skipped_events_.store(0, std::memory_order_release);
        dropped_events_ = 0;
        {
            // The list of what was not acknowledged and the number of the
            // sequence are shared with the thread that publishes the events
            // of the world, so they are cleared under the lock: a push_back
            // that reallocates while the vector is cleared is the heap
            // corruption that shows up in the shader cache.
            std::lock_guard<std::mutex> lock(mutex_);
            last_applied_seq_ = 0;
            next_event_seq_ = 0;
            unacked_.clear();
            to_send_.clear();
            pending_events_.clear();
            open_holes_.clear();
            filled_holes_.clear();
        }
        has_snapshot_ = false;
        if (IsHost()) {
            // The host knows who is in the room from the moment it starts.
            present_mask_ = 1;
            PublishRoom();
        }
    } else {
        network_.SetCoopHandler(nullptr);
        phase_.store(coop::Phase::Idle, std::memory_order_release);
    }
}

void CoopSession::Update(float dt) {
    if (!enabled_.load(std::memory_order_acquire)) return;

    // The holes that turned up are hiccups, not divergences: the guest waits
    // for the event that was missing and the sequence closes. The hole is
    // only a divergence when the event never turns up, and that is decided
    // here, when the tick says that the hole is still open.
    if (!filled_holes_.empty()) {
        for (size_t i = 0; i < filled_holes_.size(); i++) {
            if (divergence_count_ > 0) divergence_count_--;
        }
        filled_holes_.clear();
    }

    if (IsHost()) {
        // A peer that is loading the level is not silent because it is
        // broken, it is silent because it is reading a level. The timeout
        // has to follow the phase.
        network_.SetPeerTimeout(phase_.load(std::memory_order_acquire) == coop::Phase::Loading ? loading_timeout_ : lobby_timeout_);

        // The room goes out again now and then, so a guest that joined late
        // still learns which mission the host named.
        room_timer_ += dt;
        if (room_timer_ >= 0.5f) {
            room_timer_ = 0.0f;
            // Before the round, the room goes out even when it did not
            // change: the datagram that names the mission can be lost, and
            // the guest would wait for a mission it never heard of.
            const coop::Phase phase = phase_.load(std::memory_order_acquire);
            PublishRoom(phase == coop::Phase::Loading || phase == coop::Phase::Lobby);
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
        network_.SetPeerTimeout(phase_.load(std::memory_order_acquire) == coop::Phase::Loading ? loading_timeout_ : lobby_timeout_);

        // The guest says it is on it again and again, until the host's room
        // says the host knows: the datagram that says it can be lost, and
        // the pair would wait for it for the rest of the session.
        ready_timer_ += dt;
        if (ready_timer_ >= kReadyInterval) {
            ready_timer_ = 0.0f;
            if (NeedsReady()) SendReady();
        }
    }
}

// The guest: does the host know that this process is on it?
//
// The answer is the host's own room: the guest's bit in the loaded mask is
// the host saying it heard the guest say it loaded. Without that the guest
// keeps saying it, because the datagram can be lost.
bool CoopSession::NeedsReady() const {
    const coop::Phase phase = phase_.load(std::memory_order_acquire);
    if (phase == coop::Phase::Idle || phase == coop::Phase::Ended) return false;
    // The round started: the host knows the guest is there.
    if (phase == coop::Phase::InMission) return false;
    const uint8_t bit = static_cast<uint8_t>(1u << network_.GetLocalPlayerId());
    if ((room_.state.loaded_mask & bit) != 0) return false;
    return true;
}

void CoopSession::PublishSnapshot(const coop::Snapshot& snapshot) {
    if (!enabled_.load(std::memory_order_acquire) || !IsHost()) return;
    if (snapshot.header.actor_count > coop::kMaxActorsPerSnapshot) {
        // A snapshot that does not fit is not sent: the guest would take it
        // for the whole world and the actors that are missing would be
        // missing for good. The world layer refuses to build one.
        LOGE("coop: error reason=snapshot_too_big actors=%u", snapshot.header.actor_count);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_snapshot_ = snapshot;
        pending_snapshot_.header.start_epoch = start_epoch_.load(std::memory_order_acquire);
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
    if (!enabled_.load(std::memory_order_acquire) || !IsHost()) return;
    coop::Event outgoing = event;
    {
        // The events come from two threads: the damage of an actor is
        // published from the engine's thread, inside the damage routine, and
        // the events of the level script are published from the thread that
        // draws. The number of the sequence and the list of what was not
        // acknowledged are shared, so they are under a lock.
        //
        // The phase and the round are read under the lock too: the round
        // starts under it, and an event that took its number before the
        // round started would carry the number of the round before it.
        //
        // The lock is not held while the packet goes out: the transport has
        // its own lock, and the transport calls back into the session with
        // its lock held. Holding both at once, in two orders, is a deadlock.
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_.load(std::memory_order_acquire) != coop::Phase::InMission) {
            // An event outside a round has no place: the round counts the
            // events from one, and the guest would count an event that the
            // host does not count, because the guest counts the events it
            // applied and the host counts the ones the guest acknowledged.
            skipped_events_.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
        outgoing.state.start_epoch = start_epoch_.load(std::memory_order_acquire);
        outgoing.state.event_seq = ++next_event_seq_;
        PendingEvent pending;
        pending.event = outgoing;
        pending.seq = outgoing.state.event_seq;
        unacked_.push_back(pending);
        to_send_.push_back(outgoing);
        sent_events_++;
    }
    FlushEvents();
    LOGI("coop: event epoch=%u seq=%u type=%u", outgoing.state.start_epoch, outgoing.state.event_seq,
         outgoing.state.event_type);
}

// The packets go out in the order of the sequence numbers.
//
// The host publishes from two threads, and the number of the sequence is
// given under the lock while the packet goes out after it: without this, the
// packet of the sequence 6 can reach the guest before the one of the
// sequence 5, and the guest has to wait for the resend of the 5.
//
// The lock of the sending is its own and it is only about the order. The
// lock of the session is not held here, and the transport's lock is taken
// with it: the transport calls back into the session with its lock held, but
// it never asks for the lock of the sending there, so the two orders do not
// cross.
void CoopSession::FlushEvents() {
    std::lock_guard<std::mutex> send_lock(send_mutex_);
    while (true) {
        coop::Event event;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (to_send_.empty()) break;
            event = to_send_.front();
            to_send_.erase(to_send_.begin());
            for (PendingEvent& pending : unacked_) {
                if (pending.seq == event.state.event_seq) {
                    pending.sent = true;
                    break;
                }
            }
        }
        SendEvent(event);
    }
}

void CoopSession::SetTimeouts(float lobby_seconds, float loading_seconds) {
    lobby_timeout_ = lobby_seconds;
    loading_timeout_ = loading_seconds;
}

void CoopSession::HostStartMission(uint8_t mission_id) {
    if (!enabled_.load(std::memory_order_acquire) || !IsHost()) return;
    mission_id_ = mission_id;
    phase_.store(coop::Phase::Loading, std::memory_order_release);
    PublishRoom();
    LOGI("coop: mission epoch=%u mission=%u", start_epoch_.load(std::memory_order_acquire), mission_id_);
}

void CoopSession::SetLoaded(bool loaded) {
    if (!enabled_.load(std::memory_order_acquire)) return;
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
    if (!enabled_.load(std::memory_order_acquire)) return;
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
    if (!enabled_.load(std::memory_order_acquire) || !IsHost()) return;
    if (phase_.load(std::memory_order_acquire) == coop::Phase::Ended) return;
    phase_.store(coop::Phase::Ended, std::memory_order_release);
    PublishRoom();
    LOGI("coop: round epoch=%u ended", start_epoch_.load(std::memory_order_acquire));
}

void CoopSession::TryStartRound() {
    if (!enabled_.load(std::memory_order_acquire) || !IsHost()) return;
    if (phase_.load(std::memory_order_acquire) != coop::Phase::Loading) return;
    present_mask_ = network_.GetPresentMask();
    if (present_mask_ == 0) return;
    // Every peer that is present has to have loaded before the round starts.
    const uint8_t everyone = static_cast<uint8_t>(present_mask_ & loaded_mask_);
    if (everyone == present_mask_) StartRound();
}

void CoopSession::PublishRoom(bool force) {
    if (!enabled_.load(std::memory_order_acquire) || !IsHost()) return;
    present_mask_ = network_.GetPresentMask();
    coop::Room room{};
    room.state.session_id = session_id_;
    room.state.start_epoch = start_epoch_.load(std::memory_order_acquire);
    room.state.mission_id = mission_id_;
    room.state.phase = static_cast<uint8_t>(phase_.load(std::memory_order_acquire));
    room.state.present_mask = present_mask_;
    room.state.ready_mask = ready_mask_;
    room.state.loaded_mask = loaded_mask_;

    // The revision only moves when the room really changes, so the guest
    // does not have to look at a room that says the same thing. A room that
    // is forced goes out with the number it has: the guest is asking, and
    // the answer is the room as it is.
    if (std::memcmp(&room.state, &published_, sizeof(coop::RoomState)) == 0) {
        if (!force) return;
        room.state.revision = revision_;
    } else {
        std::memcpy(&published_, &room.state, sizeof(coop::RoomState));
        room.state.revision = ++revision_;
    }

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
    if (!enabled_.load(std::memory_order_acquire) || !IsGuest()) return;
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
    if (!enabled_.load(std::memory_order_acquire) || !IsHost() || !has_snapshot_) return;
    coop::Snapshot snapshot = pending_snapshot_;
    snapshot.header.start_epoch = start_epoch_.load(std::memory_order_acquire);
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
    if (!enabled_.load(std::memory_order_acquire) || !IsHost()) return;
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
        for (const PendingEvent& item : unacked_) {
            // The event that has a number but has not gone out yet is not
            // resent: it is still in the queue of the sending, and sending
            // it here would put it before the events that came after it.
            if (!item.sent) continue;
            pending.push_back(item);
        }
    }
    for (const PendingEvent& item : pending) {
        SendEvent(item.event);
    }
    if (!pending.empty()) {
        LOGI("coop: resend events=%zu from=%u", pending.size(), pending.front().seq);
    }
}

void CoopSession::StartRound() {
    {
        // The list of what was not acknowledged and the number of the
        // sequence are shared with the thread that publishes the events of
        // the world, so they are cleared under the lock. The phase moves
        // under it too: the window between the phase of the round and the
        // number of the sequence would let an event take a number of the
        // round before it, and the guest would drop it as one it already
        // applied.
        std::lock_guard<std::mutex> lock(mutex_);
        start_epoch_.fetch_add(1, std::memory_order_acq_rel);
        next_event_seq_ = 0;
        last_applied_seq_ = 0;
        unacked_.clear();
        to_send_.clear();
        pending_events_.clear();
        open_holes_.clear();
        filled_holes_.clear();
        phase_.store(coop::Phase::InMission, std::memory_order_release);
    }
    PublishRoom();
    if (round_handler_) round_handler_(start_epoch_.load(std::memory_order_acquire), true);
    LOGI("coop: round epoch=%u started", start_epoch_.load(std::memory_order_acquire));
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
    if (!enabled_.load(std::memory_order_acquire)) return;
    coop::Room room{};
    if (!coop::DecodeRoom(data, size, &room)) return;
    // A room that is not newer than the one we have is not news. The same
    // room sent twice is applied twice, and that is harmless.
    if (room.state.revision <= room_revision_ && room.state.session_id == room_.state.session_id) return;

    const bool was_loading = phase_.load(std::memory_order_acquire) == coop::Phase::Loading;
    (void)was_loading;
    room_ = room;
    room_revision_ = room.state.revision;
    phase_.store(static_cast<coop::Phase>(room.state.phase), std::memory_order_release);
    mission_id_ = room.state.mission_id;

    LOGI("coop: room revision=%u epoch=%u phase=%u", room.state.revision, room.state.start_epoch,
         room.state.phase);

    if (phase_.load(std::memory_order_acquire) == coop::Phase::Loading) {
        // The host named the mission: tell it we are on it.
        //
        // The guest says it on every room that names the mission, and not
        // only on the transition: the datagram that names the mission can be
        // lost, and the guest has to be able to learn the mission from a
        // room it gets at any time.
        SendReady();
    }
    if (phase_.load(std::memory_order_acquire) == coop::Phase::InMission && room.state.start_epoch != start_epoch_.load(std::memory_order_acquire)) {
        start_epoch_.store(room.state.start_epoch, std::memory_order_release);
        last_applied_seq_ = 0;
        pending_events_.clear();
        if (round_handler_) round_handler_(start_epoch_.load(std::memory_order_acquire), true);
        LOGI("coop: round epoch=%u started", start_epoch_.load(std::memory_order_acquire));
    }
}

void CoopSession::OnCoopReady(uint8_t player_id, const uint8_t* data, size_t size) {
    if (!enabled_.load(std::memory_order_acquire) || !IsHost()) return;
    if (player_id == 0 || player_id >= kMaxPlayers) return;
    coop::Ready ready{};
    if (!coop::DecodeReady(data, size, &ready)) return;
    if (ready.state.start_epoch != start_epoch_.load(std::memory_order_acquire)) {
        // The guest is answering about another round.
        stale_count_++;
        return;
    }
    const uint8_t bit = static_cast<uint8_t>(1u << player_id);
    if (ready.state.ready) ready_mask_ |= bit; else ready_mask_ &= static_cast<uint8_t>(~bit);
    if (ready.state.loaded) loaded_mask_ |= bit; else loaded_mask_ &= static_cast<uint8_t>(~bit);
    TryStartRound();
    // The guest is asking: it says it is on it, and it has to learn the
    // mission from a room it gets at any time. The room goes out again, even
    // when it did not change, so a guest that never heard the mission can
    // hear it now.
    PublishRoom(true);
}

void CoopSession::OnCoopSnapshot(const uint8_t* data, size_t size) {
    if (!enabled_.load(std::memory_order_acquire)) return;
    coop::Snapshot snapshot{};
    if (!coop::DecodeSnapshot(data, size, &snapshot)) return;
    if (snapshot.header.start_epoch != start_epoch_.load(std::memory_order_acquire)) {
        stale_count_++;
        return;
    }
    received_snapshots_++;
    if (snapshot_handler_) snapshot_handler_(snapshot);
}

void CoopSession::OnCoopEvent(const uint8_t* data, size_t size) {
    if (!enabled_.load(std::memory_order_acquire)) return;
    coop::Event event{};
    if (!coop::DecodeEvent(data, size, &event)) return;
    if (event.state.start_epoch != start_epoch_.load(std::memory_order_acquire)) {
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
        // A hole in the sequence. The guest waits for the missing one.
        //
        // The hole is not a divergence: the host publishes from two threads,
        // and the packet of the sequence 6 can reach the guest before the
        // one of the sequence 5. The guest waits for the 5, the 5 turns up
        // and the hole closes: that is a hiccup. It is counted apart, and
        // it only becomes a divergence when the event never turns up.
        if (open_holes_.insert(last_applied_seq_ + 1).second) {
            hole_count_++;
            divergence_count_++;
        }
        pending_events_[event.state.event_seq] = event;
        return;
    }
    // The event that was missing turned up: the hole closes. The count of
    // the divergences goes down on the next tick, so a hole that closes is
    // a hiccup and not a divergence.
    if (open_holes_.erase(event.state.event_seq) > 0) {
        filled_holes_.push_back(event.state.event_seq);
    }
    last_applied_seq_ = event.state.event_seq;
    ApplyEvent(event);
    DrainPending();

    // The mission is over: the round ends on the guest too.
    if (event.state.event_type == static_cast<uint8_t>(coop::EventType::MissionEnded)) {
        phase_.store(coop::Phase::Ended, std::memory_order_release);
        LOGI("coop: round epoch=%u ended", start_epoch_.load(std::memory_order_acquire));
    }

    // Tell the host how far we got, so it can stop sending.
    SendAck();
}

// The guest tells the host how far it got. The acknowledgement is
// idempotent: it says "everything up to this sequence is applied", so the
// guest can send it again without harm.
void CoopSession::SendAck() {
    coop::AckState ack{};
    ack.start_epoch = start_epoch_.load(std::memory_order_acquire);
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
    if (!enabled_.load(std::memory_order_acquire) || !IsHost()) return;
    coop::AckState ack{};
    if (!coop::DecodeAck(data, size, &ack)) return;
    if (ack.start_epoch != start_epoch_.load(std::memory_order_acquire)) {
        stale_count_++;
        return;
    }
    (void)player_id;
    // Everything up to the acknowledged sequence is done with.
    std::lock_guard<std::mutex> lock(mutex_);
    // The events that crossed: the guest says how far it got, and the
    // number it says is the number of the events both sides handled.
    if (ack.event_seq > crossed_events_) crossed_events_ = ack.event_seq;
    unacked_.erase(std::remove_if(unacked_.begin(), unacked_.end(),
                                  [ack](const PendingEvent& pending) { return pending.seq <= ack.event_seq; }),
                   unacked_.end());
}

}  // namespace net
