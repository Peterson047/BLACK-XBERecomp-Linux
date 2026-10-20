#pragma once
// The co-op world layer: who exists, where it is, and how it dies.
//
// The design is "synchronized copies, not a merged world": each process runs
// the whole retail level, the host is the authority of the world the level
// script reads, and the guest is a replica that reacts and never decides.
//
// This layer is the one that reads the engine's memory and writes it back:
//  - the host enumerates the live actors and builds the snapshot of them;
//  - the guest pairs the snapshot against its own actors and writes the
//    host's state onto them;
//  - the host turns the engine's own damage calls into events, and the guest
//    replays those events through the engine's own damage routine.
//
// The engine offsets are in game/layout.h and their proofs in
// .agents/teamwork/coop/LAYOUT.md; the wire contract is in
// .agents/teamwork/coop/PROTOCOL.md.
//
// How the session layer drives it:
//
//   world.SetRole(Role::kHost) or Role::kGuest
//   world.SetEventPublisher([&session](const net::coop::Event& e) {
//       session.PublishEvent(e);
//   });
//   // the host, ten times a second
//   net::coop::Snapshot snapshot;
//   if (world.BuildSnapshot(snapshot)) session.PublishSnapshot(snapshot);
//   // the guest
//   session.SetSnapshotHandler([](const net::coop::Snapshot& s) {
//       world.ApplySnapshot(s);
//   });
//   session.SetEventHandler([](const net::coop::Event& e) { world.ApplyEvent(e); });
//   // a new round
//   world.ResetRound(session.GetStartEpoch());
//   // the end of the session
//   world.LogSummary();

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "game/coop_world_pure.h"
#include "net/coop_protocol.h"

namespace game {
namespace coop {

// Which side of the co-op this process is. The Deathmatch path is offline.
enum class Role : uint8_t {
    kOffline = 0,
    kHost = 1,
    kGuest = 2,
};

// The damage the engine's own routine was given, as the hook caught it.
struct DamageCall {
    uint32_t actor = 0;         // the actor the routine was called on
    float origin[3] = {0, 0, 0};    // where the hit landed, in world space
    float direction[3] = {0, 0, 0}; // where the hit came from
    float damage = 0.0f;
    float radius = 0.0f;
    uint32_t type = 0;  // 1, 2 or 3
    uint32_t flag = 0;
};

class World {
public:
    static World& Get();

    // The session layer tells the world layer which side it is. The host
    // publishes, the guest reacts, and the offline process does neither.
    void SetRole(Role role);
    Role GetRole() const { return role_; }
    bool IsHost() const { return role_ == Role::kHost; }
    bool IsGuest() const { return role_ == Role::kGuest; }

    // The round. Everything that crosses the network carries it, and a
    // packet of an older round is dropped.
    void SetEpoch(uint32_t epoch);
    uint32_t GetEpoch() const { return epoch_; }
    // A new round: the counters start again.
    void ResetRound(uint32_t epoch);

    // The host: the snapshot of the live actors. The session layer sends it
    // at 10 Hz.
    bool BuildSnapshot(net::coop::Snapshot& out);

    // The guest: the host's state onto the guest's actors.
    //
    // The transform has to reach the actors after the engine's own tick of
    // the frame, or the guest's tick overwrites it (LAYOUT.md §6). The world
    // layer therefore keeps the snapshot and writes it in the actor's tick,
    // right after the engine's own; the call itself only has to happen
    // before the next tick of the frame.
    //
    // The actors of the guest that match nothing in the snapshot are
    // counted: a silent mismatch is the failure that killed the last
    // attempt, so it has to be visible.
    bool ApplySnapshot(const net::coop::Snapshot& snapshot);

    // The guest: the host's damage, replayed through the engine's own damage
    // routine, so the engine runs the whole death chain in its own order.
    //
    // The routine is not safe to run twice for the same hit: a second death
    // takes a second corpse and removes the actor from the manager again. The
    // world layer therefore checks the order of the events and the state of
    // the actor before it applies anything, and counts what it skips.
    bool ApplyEvent(const net::coop::Event& event);

    // The host: the events the world layer produced. The session layer takes
    // them, gives them their sequence number and sends them.
    using EventPublisher = std::function<void(const net::coop::Event&)>;
    void SetEventPublisher(EventPublisher publisher) { publisher_ = std::move(publisher); }
    std::vector<net::coop::Event> TakePendingEvents();

    // What the world layer counted during the round.
    const WorldCounters& Counters() const { return counters_; }
    uint32_t UnpairedCount() const { return counters_.unpaired; }
    uint32_t DivergentCount() const { return counters_.divergent; }

    // The end of the session: one summary line, so a test can read the
    // round without reading the whole log.
    void LogSummary();

    // The host: the engine's own damage routine fired. The hook in
    // coop_world.cpp hands the call over here.
    void PublishDamage(const DamageCall& call);

    // The guest: the actor's own tick ran. The host's transform goes in
    // now, and the events that arrived are applied.
    void AfterActorTick(uint32_t actor);

private:
    World() = default;

    // The actors the manager holds, and their identities.
    uint32_t EnumerateActors(std::vector<uint32_t>* actors) const;
    // The host: one actor's state for the snapshot.
    void ReadActorState(uint32_t actor, net::coop::ActorState* state) const;
    // The guest: the host's state onto one actor, through the engine's own
    // calls.
    void WriteActorState(uint32_t actor, const net::coop::ActorState& state);
    // The guest: one damage, through the engine's own damage routine.
    bool ReplayDamage(const net::coop::Event& event);

    Role role_ = Role::kOffline;
    uint32_t epoch_ = 0;
    WorldCounters counters_;
    EventOrder order_;

    std::mutex mutex_;
    net::coop::Snapshot pending_snapshot_;
    bool has_pending_snapshot_ = false;
    std::vector<net::coop::Event> pending_events_;
    std::vector<net::coop::Event> outgoing_events_;
    EventPublisher publisher_;
};

}  // namespace coop
}  // namespace game
