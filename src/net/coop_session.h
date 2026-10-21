#pragma once

// The co-op session: the conversation between the host and the guests.
//
// The host owns the world and the guest reacts to it, so the room, the
// snapshot and the world events travel host to guest, and only the ready and
// the acknowledgement travel the other way.
//
// Nothing here knows about the game. The world layer feeds the session a
// snapshot to send and receives the snapshot and the events the host sent.
//
// The design is in .agents/teamwork/coop/PROTOCOL.md.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include "net/coop_protocol.h"
#include "net/network.h"

namespace net {

// What the transport hands the session when a co-op packet arrives. The
// payload is the bytes after the packet header. The player id is the one the
// transport matched to the address the packet came from, so a guest can
// never speak for another one.
class CoopHandler {
public:
    virtual ~CoopHandler() = default;
    virtual void OnCoopRoom(const uint8_t* data, size_t size) = 0;
    virtual void OnCoopReady(uint8_t player_id, const uint8_t* data, size_t size) = 0;
    virtual void OnCoopSnapshot(const uint8_t* data, size_t size) = 0;
    virtual void OnCoopEvent(const uint8_t* data, size_t size) = 0;
    virtual void OnCoopAck(uint8_t player_id, const uint8_t* data, size_t size) = 0;
};

class CoopSession : public CoopHandler {
public:
    explicit CoopSession(NetworkManager& network);
    ~CoopSession() override;

    // The session does nothing until it is enabled, so the path before it is
    // untouched when the co-op is off.
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return enabled_.load(std::memory_order_acquire); }

    // The tick. The host publishes the room and the snapshot and resends what
    // was not acknowledged; the guest answers and applies what arrived.
    void Update(float dt);

    // What the world layer calls.
    // The host hands over the snapshot of the world; it goes out at 10 Hz.
    void PublishSnapshot(const coop::Snapshot& snapshot);
    // The host hands over an event of the world. The session gives it the
    // next sequence number and keeps it until the guest acknowledges it.
    void PublishEvent(const coop::Event& event);

    // What the world layer listens to.
    using SnapshotHandler = std::function<void(const coop::Snapshot& snapshot)>;
    using EventHandler = std::function<void(const coop::Event& event)>;
    using RoundHandler = std::function<void(uint32_t start_epoch, bool started)>;
    void SetSnapshotHandler(SnapshotHandler handler) { snapshot_handler_ = std::move(handler); }
    void SetEventHandler(EventHandler handler) { event_handler_ = std::move(handler); }
    void SetRoundHandler(RoundHandler handler) { round_handler_ = std::move(handler); }

    // How long a peer may be quiet. A peer that is loading a level does not
    // answer for a while, so the timeout follows the phase. The test needs
    // to be able to shorten it.
    void SetTimeouts(float lobby_seconds, float loading_seconds);

    // The host names the mission. The guests load it and answer.
    void HostStartMission(uint8_t mission_id);
    // The guest says it finished loading the level.
    void SetLoaded(bool loaded);
    // The peer is ready: the level is loaded and the round can begin.
    void SetReady(bool ready);
    // The host: the mission is over and the round ends.
    void EndRound();

    coop::Phase GetPhase() const { return phase_.load(std::memory_order_acquire); }
    uint32_t GetStartEpoch() const { return start_epoch_.load(std::memory_order_acquire); }
    // The room as the guest last saw it.
    const coop::Room& GetRoom() const { return room_; }

    // What went wrong, and how much. A silent divergence is the worst
    // outcome, so these are visible.
    uint32_t GetDivergenceCount() const { return divergence_count_; }
    uint32_t GetStaleCount() const { return stale_count_; }
    uint32_t GetAppliedEventCount() const { return applied_events_; }
    // The counters that the other threads touch are read under the lock.
    uint32_t GetSentEventCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sent_events_;
    }
    uint32_t GetUnackedCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<uint32_t>(unacked_.size());
    }
    uint32_t GetDroppedEventCount() const { return dropped_events_; }
    // The events that were not published because there was no round.
    uint32_t GetSkippedEventCount() const { return skipped_events_; }
    uint32_t GetSentSnapshotCount() const { return sent_snapshots_; }
    uint32_t GetReceivedSnapshotCount() const { return received_snapshots_; }
    // The number of the events that crossed to the other side.
    //
    // The guest counts the events it applied, and the host counts the events
    // the guest acknowledged. It is the same number on the two sides, and it
    // is the number that says whether the two sides handled the same events:
    // the host publishes a few events more than the guest applies, because
    // the guest leaves the session a little before the host does, and the
    // event that was on its way is counted by one side only.
    uint32_t GetCrossedEventCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (IsHost()) return crossed_events_;
        return applied_events_;
    }
    // The acknowledgements the guest sent. A repeated event is acknowledged
    // again, so a lost acknowledgement does not become a resend that never
    // ends.
    uint32_t GetAckCount() const { return acks_sent_; }

    // The transport calls these.
    void OnCoopRoom(const uint8_t* data, size_t size) override;
    void OnCoopReady(uint8_t player_id, const uint8_t* data, size_t size) override;
    void OnCoopSnapshot(const uint8_t* data, size_t size) override;
    void OnCoopEvent(const uint8_t* data, size_t size) override;
    void OnCoopAck(uint8_t player_id, const uint8_t* data, size_t size) override;

private:
    bool IsHost() const { return network_.GetMode() == NetMode::Host; }
    bool IsGuest() const { return network_.GetMode() == NetMode::Client; }

    void PublishRoom();
    void TryStartRound();
    void SendReady();
    void SendSnapshot();
    void SendEvent(const coop::Event& event);
    // The guest tells the host how far it got.
    void SendAck();
    void ResendUnacked();
    void StartRound();
    void ApplyEvent(const coop::Event& event);
    void DrainPending();

    NetworkManager& network_;

    // The state of the session is read by the thread that publishes the
    // events of the world (the thread that runs the engine) and written by
    // the thread that runs the session (the thread that draws), so the three
    // that both of them touch are atomic.
    std::atomic<bool> enabled_{false};
    std::atomic<coop::Phase> phase_{coop::Phase::Idle};
    uint32_t session_id_ = 0;
    uint32_t revision_ = 0;
    std::atomic<uint32_t> start_epoch_{0};
    uint8_t mission_id_ = 0;

    // The host's view of the room.
    uint8_t present_mask_ = 0;
    uint8_t ready_mask_ = 0;
    uint8_t loaded_mask_ = 0;

    // The room as the host last published it, so the revision only moves
    // when the room really changes.
    coop::RoomState published_{};

    // The guest's view of the room.
    coop::Room room_{};
    uint32_t room_revision_ = 0;
    bool loaded_ = false;
    bool ready_ = false;

    // The snapshot the world layer handed over. It goes out as soon as it
    // arrives: the world layer already decides how often the world is read.
    coop::Snapshot pending_snapshot_{};
    bool has_snapshot_ = false;

    // The events the host sent and the guest has not acknowledged.
    struct PendingEvent {
        coop::Event event{};
        uint32_t seq = 0;
    };
    // The lock of the state that two threads touch: the thread that publishes
    // the events of the world (the engine's thread, inside the damage
    // routine) and the thread that runs the session (the thread that draws).
    // It is never held while a packet goes out, because the transport has its
    // own lock and calls back into the session with it held.
    mutable std::mutex mutex_;
    std::vector<PendingEvent> unacked_;
    uint32_t next_event_seq_ = 0;
    float resend_timer_ = 0.0f;

    // The guest's view of the events: the last one applied, and the ones
    // that arrived early and wait for their turn.
    uint32_t last_applied_seq_ = 0;
    std::map<uint32_t, coop::Event> pending_events_;

    uint32_t divergence_count_ = 0;
    uint32_t stale_count_ = 0;
    uint32_t applied_events_ = 0;
    uint32_t sent_events_ = 0;
    // The events the guest acknowledged: the number of the events that
    // crossed, seen from the host.
    uint32_t crossed_events_ = 0;
    // The events that were published outside a round and did not go out.
    uint32_t skipped_events_ = 0;
    uint32_t dropped_events_ = 0;
    uint32_t sent_snapshots_ = 0;
    uint32_t received_snapshots_ = 0;
    uint32_t acks_sent_ = 0;
    float room_timer_ = 0.0f;

    float lobby_timeout_ = 5.0f;
    float loading_timeout_ = 30.0f;

    SnapshotHandler snapshot_handler_;
    EventHandler event_handler_;
    RoundHandler round_handler_;
};

}  // namespace net
