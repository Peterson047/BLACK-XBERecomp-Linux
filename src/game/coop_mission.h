#pragma once
// The co-op mission layer.
//
// The mission is not a script. It is a table of objectives (LevelDat.bin) and
// a bus of named events, and the two processes load the same table, so the
// table never crosses the network. What has to be the same is the live state
// of the objectives, and the live state advances because of the events. So the
// unit of replication is the named event: the host captures the event the
// engine fires and the guest fires the same event in its own bus, so that the
// guest's own matcher advances the guest's own objective exactly like the
// host's.
//
// The engine's addresses are in game/layout.h and the proof of each one is in
// .agents/teamwork/coop/LAYOUT.md, section 7.
//
// The pure part (the comparison of two readings, the end of the mission, the
// counters, the id of a name) is in coop_mission_logic.cpp, so that it can be
// tested without the game. The game part (the hooks, the reads of the world,
// the log) is in coop_mission.cpp.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>

#include "net/coop_protocol.h"

namespace game {
namespace coop {

// The table of objectives of a level has a handful of entries; 64 is the most
// the state keeps.
constexpr uint32_t kMaxObjectives = 64;

// How many events of the host the mission keeps while it waits for the
// engine's thread to apply them.
constexpr size_t kInboxCapacity = 64;

// How long the same object is not used again: the engine registers the
// request of use again and again while the player holds the button.
constexpr std::chrono::milliseconds kInteractionWindow{1000};

// One of the level script's named events, as the host sees it and the guest
// fires it again.
//
// The name and the text are the two strings the engine puts in the queue, the
// position is the 16 byte payload and the count is the dword that goes with
// it. The event id is the CRC-32 the engine hashes a name with, so the two
// sides agree on it without exchanging anything.
struct NamedEvent {
    uint32_t event_id = 0;
    uint8_t kind = 0;          // which of the engine's four shapes the event had
    uint32_t count = 0;        // the event's count
    float pos[3] = {0, 0, 0};  // the payload of the event: where it happened
    char name[96] = {0};       // the name of the event: FE_PRIMARYOBJ_DONE and the like
    char text[96] = {0};       // the event's own text
};

// The state of the mission, read from the world.
//
// The table lives at world+0x930 and the count of the objectives at
// world+0x934. Each objective is a record of 12 bytes: the file record it
// came from, a flag and the live state, which is 3 when the objective is
// done. The counters at world+0x938 .. world+0x94C are the totals of the
// categories and do not change while the level runs.
struct ObjectiveState {
    uint32_t count = 0;         // the number of objectives (world+0x934)
    uint32_t done = 0;          // the objectives with the state 3
    uint32_t complete = 0;      // "Objectives Complete": the sum of the counters
    uint32_t counters[6] = {};  // world+0x938 .. world+0x94C
    uint8_t states[kMaxObjectives] = {};  // the state of each objective
};

// The pure part. It is in coop_mission_logic.cpp.

// The id of a name: the CRC-32 the engine hashes a name with (the table is at
// 0x2AC908, the polynomial is the usual reflected one). The engine hashes the
// name without its last character; this does the same, so the id is the
// engine's own id of the name.
uint32_t NameHash(const char* name, size_t length);

// The objectives with the state 3.
uint32_t CountDone(const ObjectiveState& state);

// "Objectives Complete": the sum of the counters, the way the engine sums
// them (sub_001BE460).
uint32_t SumCounters(const ObjectiveState& state);

// True when the two readings of the mission are the same.
bool SameObjectives(const ObjectiveState& a, const ObjectiveState& b);

// True when the level is over: every objective of the table is done.
bool MissionFinished(const ObjectiveState& state);

// What the mission does with an event that arrives from the host.
//
// The mission acts on the events of the level script and on the end of the
// mission. The damage of an actor is the world layer's business: when it took
// a place in the mission's inbox, sixty four hits were enough to push out the
// event of a mission, and the mission stopped advancing on the guest.
enum class EventQueueing {
    kQueue,   // the mission acts on it
    kIgnore,  // it is not the mission's business
    kFull,    // the inbox is full: the event is dropped and counted
};
EventQueueing DecideQueueing(uint8_t event_type, size_t inbox_size, size_t capacity);

// The summary line of the session, in the log's vocabulary.
std::string SummaryLine(uint32_t epoch, uint32_t objectives, uint32_t done, uint32_t events, uint32_t unpaired,
                         uint32_t divergent);

// The mission of the co-op session.
//
// The host owns it: it captures the events the engine fires and publishes
// them. The guest is a replica: it fires the events the host sends and never
// publishes anything of its own. The two sides never read each other's
// memory: the host's world is the authority and the guest reacts to it.
//
// How the session layer drives it:
//
//   mission.SetHost(true);                       // or false, on the guest
//   // the host, once a frame
//   net::coop::Event event;
//   while (mission.NextEvent(&event)) session.PublishEvent(event);
//   // the guest
//   session.SetEventHandler([](const net::coop::Event& e) { mission.ApplyEvent(e); });
//   // a new round
//   mission.ResetRound(session.GetStartEpoch());
//   // the end of the session
//   mission.LogSummary();
class Mission {
public:
    static Mission& Get();

    // Which side this process is. The host decides; the guest reacts.
    // The thread that draws sets it and the thread that runs the engine reads
    // it, so it is atomic.
    void SetHost(bool host);
    bool IsHost() const { return host_.load(std::memory_order_acquire); }

    // The round. Everything that crosses the wire carries it. The guest stops
    // letting its own events happen once the round is known: the host's world
    // is the authority.
    // The round is written by the thread that draws and read by the thread
    // that runs the engine, so it is atomic.
    void SetEpoch(uint32_t epoch) { epoch_.store(epoch, std::memory_order_release); }
    uint32_t Epoch() const { return epoch_.load(std::memory_order_acquire); }

    // A new round: the counters start again and nothing of the last one is
    // left waiting.
    void ResetRound(uint32_t epoch);

    // The host: the engine fired a named event. It waits for the network.
    void OnNamedEvent(const NamedEvent& event);

    // The host: the engine registered an interaction, the "use" of an object.
    // The object is the one the engine used; it is a runtime object of this
    // process, so only its type crosses the network.
    void OnInteraction(uint32_t object, uint16_t object_type, const float pos[3], float radius);

    // The engine's calls into the mission layer. The hooks of the mission
    // call these; nothing else does.
    //
    // The level script is firing a named event: the kind of it, for the log.
    void BeginFire(uint32_t kind);
    void EndFire();
    // The bus took an event: the name, the text, the position and the count.
    void OnBusEvent(uint32_t bus, const char* name, const char* text, const float pos[3], uint32_t count);

    // The host: the level is over.
    void OnMissionEnded();

    // The guest: an event of the host arrived. It waits for the guest thread.
    void ApplyEvent(const net::coop::Event& event);

    // The host: the next event to publish. False when there is none.
    bool NextEvent(net::coop::Event* out);

    // The state of the mission in this process, read from the world.
    ObjectiveState ReadObjectives() const;

    // The guest: what the host says the state of the mission is.
    void SetRemoteState(uint32_t count, uint32_t done);

    // Compares the state of this process with the host's. True when they are
    // the same. A difference is logged and counted.
    bool CompareState();

    // Watches the state of the objectives and the end of the mission, once a
    // frame. It reads the table of the objectives, which is the engine's
    // memory, so it runs on the thread that draws, the same way the
    // The engine's memory is read from more than one thread. The state it keeps is under the
    // lock, because the thread that runs the engine writes part of it.
    void Pump();

    // The guest: the events of the host that arrived, applied in the guest's
    // own bus. It has to run on the thread that runs the engine: the call
    // into the engine is not safe anywhere else. When there is no such
    // thread, the events wait for one.
    void ApplyPending();

    // The counters of the session. They are read by the thread that draws,
    // while the thread that runs the engine writes them, so they are read
    // under the lock.
    uint32_t Events() const { return events_.load(std::memory_order_acquire); }
    uint32_t Unpaired() const { return unpaired_.load(std::memory_order_acquire); }
    uint32_t Divergent() const { return divergent_.load(std::memory_order_acquire); }

    // The mission is over: every objective of the table is done, or the host
    // said so. The round ends on both sides.
    bool Ended() const { return ended_.load(std::memory_order_acquire); }

    // The summary of the session, at the end of it.
    void LogSummary() const;

    // The summary, once. The mission prints it when the level is over; the
    // network adapter can call it when the session ends.
    void LogSummaryOnce();

private:
    Mission() = default;

    // The guest fires the event of the host in its own bus.
    void FireLocal(const NamedEvent& event);

    // The key of an event: the name, the text and the count. Two events with
    // the same key are the same event.
    static std::string EventKey(const NamedEvent& event);

    // True when the guest's own engine already fired this event.
    bool HasTwin(const NamedEvent& event);

    // The two sides do not agree about the state of the mission. The same
    // divergence is only counted and logged once.
    void NoteDivergence(uint32_t host_count, uint32_t host_done, const ObjectiveState& local);

    std::atomic<bool> host_{true};
    std::atomic<uint32_t> epoch_{0};

    // The counters are counted by the thread that runs the engine and read by
    // the thread that draws, so they are atomic.
    std::atomic<uint32_t> events_{0};     // the events the host published or the guest applied
    std::atomic<uint32_t> unpaired_{0};   // the events the host had and the guest did not
    std::atomic<uint32_t> divergent_{0};  // the divergences found

    bool has_remote_ = false;
    uint32_t remote_count_ = 0;
    uint32_t remote_done_ = 0;

    bool replaying_ = false;  // the guest is firing an event of the host
    uint32_t bus_ = 0;        // the bus of this process
    uint8_t kind_ = 0;        // the kind of the event being fired
    bool has_kind_ = false;
    // The end of the mission and the summary are written by the thread that
    // runs the engine and by the thread that draws, so they are atomic.
    std::atomic<bool> ended_{false};    // the mission was already announced as over
    std::atomic<bool> summary_{false};  // the summary of the session was already printed
    bool divergence_logged_ = false;  // the last divergence was already logged
    uint32_t divergence_count_ = 0;   // the state of the host in that divergence
    uint32_t divergence_done_ = 0;
    uint32_t last_object_ = 0;        // the last object the player used
    // When the last object was used. The wall clock is what says whether a
    // second went by: the engine's counter of frames is a count, and a count
    // of frames is a second only at sixty frames a second.
    std::chrono::steady_clock::time_point last_interaction_time_{};
    uint32_t last_world_ = 0;         // the world of the last reading
    bool objective_logged_ = false;   // the state of the mission was already logged
    uint32_t sequence_ = 0;   // the number of the events that went to the network
    uint32_t published_count_ = 0;  // the last state of the mission the host published
    uint32_t published_done_ = 0;

    // The guest's memory that the replayed events needed, and that went back
    // to the guest's heap. The two numbers have to be the same: a difference
    // is a leak of the guest's address space.
    std::atomic<uint32_t> scratch_allocs_{0};
    std::atomic<uint32_t> scratch_frees_{0};

    // The lock of the state that two threads touch: the thread that runs the
    // engine (which fires the events of the level script) and the thread that
    // draws (which reads the counters of the session).
    mutable std::mutex mutex_;
    std::deque<net::coop::Event> outbox_;  // the events the host has to publish
    std::deque<net::coop::Event> inbox_;   // the events the guest has to apply
    // The events the guest's own engine fired, in order. The event of the
    // host that has a twin in here is the one the guest already had: it is
    // not fired again, or the objective would be counted twice.
    std::deque<std::string> local_events_;
};
}  // namespace coop
}  // namespace game
