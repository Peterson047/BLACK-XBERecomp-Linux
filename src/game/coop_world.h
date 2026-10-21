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

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "game/coop_world_pure.h"
#include "net/coop_protocol.h"

namespace game {
namespace coop {

// Which side of the co-op this process is. Offline is the state before the session starts.
enum class Role : uint8_t {
    kOffline = 0,
    kHost = 1,
    kGuest = 2,
};

// What the replay of a damage did.
enum class ReplayResult {
    kApplied,   // the damage went into the actor
    kSkipped,   // the actor was not there to be damaged, and it will not be
    kNoActor,   // the actor is not there yet: it may come
};

// The damage the engine's own routine was given, as the hook caught it.
struct DamageCall {
    uint32_t actor = 0;         // the actor the routine was called on
    // The identity, taken before the call: the death clears the pair at
    // actor+0x08 and actor+0x0C, so reading it afterwards finds nothing.
    uint32_t native_a = 0;
    uint32_t native_b = 0;
    uint32_t generation = 0;
    uint32_t actor_type = 0;
    float origin[3] = {0, 0, 0};    // where the hit landed, in world space
    float direction[3] = {0, 0, 0}; // where the hit came from
    float damage = 0.0f;
    float radius = 0.0f;
    uint32_t type = 0;  // 1, 2 or 3
    uint32_t flag = 0;
    // The controller's hit state: the mode in the low byte and the body
    // part in the next. The damage routine reads them and skips the damage
    // for some body parts, so the guest has to have the same.
    uint32_t hit_state = 0;
};

// The activation of an actor, as the engine's own routine was given it.
//
// The activation is 0x001DA4F0(actor, descriptor, third, ...). The actor is
// the one the engine gives, the descriptor is the unit of the level the actor
// comes from, and the third is the value the routine takes in a register.
// The rest of the arguments are the identity of the object, the transform of
// the spawn and four values of the level script.
struct ActivationCall {
    uint32_t actor = 0;
    uint32_t descriptor = 0;
    uint32_t third = 0;
    uint32_t key_a = 0;
    uint32_t key_b = 0;
    uint32_t arg6 = 0;
    uint32_t arg7 = 0;
    uint32_t arg8 = 0;
    uint32_t arg9 = 0;
    float transform[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

class World {
public:
    static World& Get();

    // The session layer tells the world layer which side it is. The host
    // publishes, the guest reacts, and the offline process does neither.
    void SetRole(Role role);
    Role GetRole() const { return role_.load(std::memory_order_acquire); }
    bool IsHost() const { return GetRole() == Role::kHost; }
    bool IsGuest() const { return GetRole() == Role::kGuest; }

    // The round. Everything that crosses the network carries it, and a
    // packet of an older round is dropped.
    void SetEpoch(uint32_t epoch);
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
    //
    // The publisher is called from the thread that runs the engine and it is
    // set from the thread that draws, so it is under the lock. The call
    // itself is made with a copy and with the lock let go: the publisher
    // goes to the session and to the network, and the lock of the world
    // layer is never held while another lock is taken.
    using EventPublisher = std::function<void(const net::coop::Event&)>;
    void SetEventPublisher(EventPublisher publisher);
    std::vector<net::coop::Event> TakePendingEvents();

    // The host: the engine activated an actor.
    //
    // The two copies of the level do not activate the same objects at the
    // same time - the level script reacts to where the player is, and the
    // two players are in different places - so the host tells the guest
    // which actor of the level it activated. The guest is never a source:
    // it activates only what the host named.
    void PublishActivation(const ActivationCall& call);

    // The guest: the host's activation, applied on the engine's thread.
    //
    // It runs in the actor's tick, like the damage: the engine's memory is
    // not to be touched from any other thread.
    bool ApplyActivation(const net::coop::Event& event);

    // The guest: the engine's own level script activated an actor.
    //
    // The guest's script runs the whole level, so it activates the objects
    // of the level on its own. When it activates one the host has already
    // activated, the guest's actor takes the host's generation, so the two
    // sides agree on which actor is which.
    void AfterGuestActivation(uint32_t actor, uint32_t key_a, uint32_t key_b);

    // What the host published about one object of the level.
    //
    // The host activates the same object more than once - the level script
    // brings it back - so the generations of the host are a queue, in the
    // order the host made them. The guest's own level script activates the
    // object too, and each of its actors takes the generation of the host
    // that goes with it.
    struct HostActivation {
        uint32_t key_a = 0;
        uint32_t key_b = 0;
        ActivationLedger ledger;  // the generations of the host, in order
    };

    // What the world layer counted during the round.
    uint32_t UnpairedCount() const { return counters_.unpaired.load(std::memory_order_acquire); }
    uint32_t DivergentCount() const { return counters_.divergent.load(std::memory_order_acquire); }

    // The end of the session: one summary line, so a test can read the
    // round without reading the whole log.
    void LogSummary();

    // The host: the engine's own damage routine fired. The hook in
    // coop_world.cpp hands the call over here.
    void PublishDamage(const DamageCall& call);

    // The test harness: the host kills the nearest living actor, so the
    // replication of the death can be checked without a player at the
    // keyboard. The kill goes through the engine's own damage routine, so
    // it is published and replayed like any other damage.
    // It returns false when there is no living actor to kill yet.
    bool ArmTestKill();

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
    ReplayResult ReplayDamage(const net::coop::Event& event, uint32_t waits);
    // The guest: the living actor with this identity, or zero.
    uint32_t FindLiveActor(uint32_t key_a, uint32_t key_b) const;
    // The guest: what the host published about this object of the level.
    // The pointer is only good while the lock is held: the caller keeps it.
    HostActivation* FindHostActivation(uint32_t key_a, uint32_t key_b);
    // The same, with the lock already held.
    HostActivation* FindHostActivationLocked(uint32_t key_a, uint32_t key_b);
    // The guest: did the host say it has an actor of this object of the
    // level? The snapshot that carries the actor can be late; the word of
    // the activation cannot.
    bool HostPublished(uint32_t key_a, uint32_t key_b);
    void NoteHostPublished(uint32_t key_a, uint32_t key_b);
    // The same, with the lock already held.
    void NoteHostPublishedLocked(uint32_t key_a, uint32_t key_b);

    // The role and the round are written by the thread that draws and read
    // by the thread that runs the engine: they are atomic.
    std::atomic<Role> role_{Role::kOffline};
    std::atomic<uint32_t> epoch_{0};
    WorldCounters counters_;
    EventOrder order_;

    // The lock of the state that the thread that runs the engine and the
    // thread that draws both touch. It is never held while the code calls
    // out - into the guest, into the session or into the network - because
    // those take locks of their own and the orders would cross.
    std::mutex mutex_;
    uint32_t last_unpaired_ = 0;  // what the last pairing said, to log the change
    uint32_t last_missing_ = 0;
    // The kill of the test harness: the thread that draws arms it and the
    // thread that runs the engine fires it.
    uint32_t test_kill_actor_ = 0;   // the actor the test harness is going to kill
    uint32_t test_kill_key_a_ = 0;
    uint32_t test_kill_key_b_ = 0;
    uint32_t test_kill_generation_ = 0;
    net::coop::Snapshot pending_snapshot_;
    bool has_pending_snapshot_ = false;
    // The events that arrived outside the engine's thread, and how many
    // times each of them waited for the actor it is about.
    struct PendingEvent {
        net::coop::Event event{};
        uint32_t waits = 0;
    };
    std::vector<PendingEvent> pending_events_;
    // The frame the events were last taken in. The tick of the actor runs
    // once per actor, and the events are applied once per frame.
    uint32_t last_event_frame_ = 0;
    std::vector<net::coop::Event> outgoing_events_;
    // The activations the host published and the guest has not seen the
    // actor of yet. The guest's own script activates the objects of the
    // level on the engine's thread and the host's word arrives on the
    // thread that draws, so the list is under the lock.
    std::vector<HostActivation> host_activations_;
    // The objects of the level the host said it activated. The snapshot
    // that carries the actor can be late; this cannot.
    std::vector<std::pair<uint32_t, uint32_t>> host_published_;
    EventPublisher publisher_;
};

}  // namespace coop
}  // namespace game
