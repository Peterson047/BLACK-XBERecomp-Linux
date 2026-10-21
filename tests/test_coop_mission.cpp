// Standalone test of the pure part of the co-op mission layer. No game
// resources, no network: the comparison of two readings of the mission, the
// decision that the level is over, the counters and the id of a name.
//
// Build: clang++ -std=c++20 -Isrc -Isdk/runtime tests/test_coop_mission.cpp \
//        src/game/coop_mission_logic.cpp -o /tmp/test_coop_mission && \
//        /tmp/test_coop_mission
#include "game/coop_mission.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>

using game::coop::CountDone;
using game::coop::kMaxObjectives;
using game::coop::MissionFinished;
using game::coop::NameHash;
using game::coop::ObjectiveState;
using game::coop::SameObjectives;
using game::coop::SumCounters;
using game::coop::SummaryLine;

namespace {

[[noreturn]] void Fail(const char* message, int line) {
    throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + message);
}

#define CHECK(expr) do { if (!(expr)) Fail(#expr, __LINE__); } while (false)

// A table of three objectives, two of them done, and the counters of the
// categories.
ObjectiveState MakeState() {
    ObjectiveState state{};
    state.count = 3;
    state.states[0] = 3;
    state.states[1] = 3;
    state.states[2] = 0;
    state.counters[0] = 1;
    state.counters[1] = 1;
    state.counters[2] = 0;
    state.counters[3] = 1;
    state.counters[4] = 0;
    state.counters[5] = 0;
    state.done = CountDone(state);
    state.complete = SumCounters(state);
    return state;
}

void TestCountDone() {
    ObjectiveState state = MakeState();
    CHECK(state.done == 2);

    // Only the state 3 counts.
    state.states[2] = 2;
    state.done = CountDone(state);
    CHECK(state.done == 2);

    state.states[2] = 3;
    state.done = CountDone(state);
    CHECK(state.done == 3);

    // A table that is not loaded has no objectives.
    ObjectiveState empty{};
    CHECK(CountDone(empty) == 0);
    CHECK(SumCounters(empty) == 0);
}

void TestSumCounters() {
    ObjectiveState state = MakeState();
    // "Objectives Complete" is the sum of the counters.
    CHECK(SumCounters(state) == 3);
    CHECK(state.complete == 3);

    state.counters[2] = 4;
    CHECK(SumCounters(state) == 7);
}

void TestSameObjectives() {
    ObjectiveState a = MakeState();
    ObjectiveState b = MakeState();
    CHECK(SameObjectives(a, b));

    // One more objective done: the two sides do not agree.
    b.states[2] = 3;
    b.done = CountDone(b);
    CHECK(!SameObjectives(a, b));

    // The same objectives with a different count of the table.
    b = MakeState();
    b.count = 4;
    CHECK(!SameObjectives(a, b));

    // The same objectives with a different counter.
    b = MakeState();
    b.counters[0] = 2;
    CHECK(!SameObjectives(a, b));

    // The same objectives with a different state.
    b = MakeState();
    b.states[2] = 1;
    CHECK(!SameObjectives(a, b));
}

void TestMissionFinished() {
    ObjectiveState state = MakeState();
    CHECK(!MissionFinished(state));

    state.states[2] = 3;
    state.done = CountDone(state);
    CHECK(MissionFinished(state));

    // A level whose table is not there yet is not over.
    ObjectiveState empty{};
    CHECK(!MissionFinished(empty));

    // More objectives than are done is not over either.
    state.count = 4;
    CHECK(!MissionFinished(state));
}

void TestNameHash() {
    // The id of a name is a function of the name alone, so the two sides
    // agree on it without exchanging anything.
    const char* name = "FE_PRIMARYOBJ_DONE";
    CHECK(NameHash(name, std::strlen(name)) == NameHash(name, std::strlen(name)));
    CHECK(NameHash(name, std::strlen(name)) != 0);

    // The engine hashes the WHOLE name: sub_001F3AC0 counts the string to
    // its zero and calls sub_000BF2A0 with the name and its length, so the
    // id is the engine's own id of the name. The shift of the engine is
    // arithmetic, so the id is not the plain CRC-32.
    CHECK(NameHash(name, std::strlen(name)) == 0x0F465F7Du);
    CHECK(NameHash("FE_PRIMARYOBJ_NEW", std::strlen("FE_PRIMARYOBJ_NEW")) == 0x2A4A8C90u);

    // Two names of the same length are two ids.
    const char* other = "FE_PRIMARYOBJ_NEW_";
    CHECK(NameHash(name, std::strlen(name)) != NameHash(other, std::strlen(other)));

    // The last character is part of the name: two names that differ only in
    // it have different ids.
    const char* twin = "FE_PRIMARYOBJ_DONF";
    CHECK(std::strlen(twin) == std::strlen(name));
    CHECK(NameHash(name, std::strlen(name)) != NameHash(twin, std::strlen(twin)));

    // An empty name has the id of nothing.
    CHECK(NameHash("", 0) == 0xFFFFFFFFu);
}

// The twin check.
//
// The guest's own engine fires the same events as the host's, and the copy
// of the host that the guest already had is not fired again: the objective
// would be counted twice. The queue of the guest's own events is searched
// whole, because the two copies of the level run different scripts and the
// two sequences do not match one by one.
void TestTwinMatch() {
    using game::coop::MatchTwin;

    std::deque<std::string> local;
    local.push_back("FE_PRIMARYOBJ_NEW|0");
    local.push_back("FE_PRIMARYOBJ_DONE|0");
    local.push_back("LX_DESTRUCTION|0");

    // The event the guest already fired itself: the copy of the host is not
    // fired again, and the element that matched is the one that is erased.
    CHECK(MatchTwin(&local, "FE_PRIMARYOBJ_DONE|0"));
    CHECK(local.size() == 2);
    CHECK(local[0] == "FE_PRIMARYOBJ_NEW|0");
    CHECK(local[1] == "LX_DESTRUCTION|0");

    // The same event a second time: the guest had it only once, so the
    // second copy of the host is fired.
    CHECK(!MatchTwin(&local, "FE_PRIMARYOBJ_DONE|0"));

    // The event the guest has not seen: it is fired.
    CHECK(!MatchTwin(&local, "FE_PRIMARYOBJ_FAILED|0"));
    CHECK(local.size() == 2);

    // The order of the two copies of the level does not matter: the queue is
    // searched whole, so a host event that is not at the head of the queue
    // still finds its twin, and the queue does not lose the others.
    std::deque<std::string> other;
    other.push_back("A|0");
    other.push_back("B|0");
    other.push_back("C|0");
    CHECK(MatchTwin(&other, "C|0"));
    CHECK(other.size() == 2);
    CHECK(other[0] == "A|0");
    CHECK(other[1] == "B|0");
    CHECK(MatchTwin(&other, "A|0"));
    CHECK(other.size() == 1);
    CHECK(other[0] == "B|0");
    CHECK(!MatchTwin(&other, "A|0"));
    CHECK(other.size() == 1);

    // A queue that is empty has no twin: every event of the host is fired.
    std::deque<std::string> empty;
    CHECK(!MatchTwin(&empty, "A|0"));
    CHECK(!MatchTwin(nullptr, "A|0"));

    // The rule of the guest, end to end: the event of the host is fired
    // only when the guest did not have it. The queue of the guest's own
    // events is what says it, and the order of the two sequences does not
    // matter.
    std::deque<std::string> own;
    own.push_back("A|0");
    own.push_back("B|0");
    const char* const host_events[] = {"B|0", "C|0", "A|0"};
    int fired = 0;
    for (const char* host : host_events) {
        if (MatchTwin(&own, host)) continue;  // the guest had it: not fired again
        fired++;                             // the guest did not have it: fired
    }
    CHECK(fired == 1);  // only "C|0" was new to the guest
    CHECK(own.empty());
}

void TestSummaryLine() {
    const std::string line = SummaryLine(3, 6, 4, 42, 1, 0);
    CHECK(line.rfind("coop: summary ", 0) == 0);
    CHECK(line.find("epoch=3") != std::string::npos);
    CHECK(line.find("objectives=6") != std::string::npos);
    CHECK(line.find("done=4") != std::string::npos);
    CHECK(line.find("events=42") != std::string::npos);
    CHECK(line.find("unpaired=1") != std::string::npos);
    CHECK(line.find("divergent=0") != std::string::npos);

    // The vocabulary of the log: no spaces around the equal signs.
    size_t at = line.find('=');
    while (at != std::string::npos) {
        CHECK(at == 0 || line[at - 1] != ' ');
        CHECK(at + 1 >= line.size() || line[at + 1] != ' ');
        at = line.find('=', at + 1);
    }
}

// What the mission does with the events that arrive from the host.
//
// The damage of an actor is the world layer's business. When it took a place
// in the mission's inbox, sixty four hits were enough to push out the event
// of a mission, and the mission stopped advancing on the guest.
void TestEventQueueing() {
    using game::coop::DecideQueueing;
    using game::coop::EventQueueing;
    using game::coop::kInboxCapacity;

    // The events of the mission are queued.
    CHECK(DecideQueueing(static_cast<uint8_t>(net::coop::EventType::NamedEvent), 0, kInboxCapacity) ==
          EventQueueing::kQueue);
    CHECK(DecideQueueing(static_cast<uint8_t>(net::coop::EventType::ObjectiveChanged), 0, kInboxCapacity) ==
          EventQueueing::kQueue);
    CHECK(DecideQueueing(static_cast<uint8_t>(net::coop::EventType::MissionEnded), 0, kInboxCapacity) ==
          EventQueueing::kQueue);
    CHECK(DecideQueueing(static_cast<uint8_t>(net::coop::EventType::ObjectInteracted), 0, kInboxCapacity) ==
          EventQueueing::kQueue);

    // The damage of an actor is not the mission's business: it does not take
    // a place in the inbox, not even when the inbox is empty.
    CHECK(DecideQueueing(static_cast<uint8_t>(net::coop::EventType::ActorDamaged), 0, kInboxCapacity) ==
          EventQueueing::kIgnore);
    CHECK(DecideQueueing(static_cast<uint8_t>(net::coop::EventType::ActorDied), 0, kInboxCapacity) ==
          EventQueueing::kIgnore);
    CHECK(DecideQueueing(static_cast<uint8_t>(net::coop::EventType::ActorActivated), 0, kInboxCapacity) ==
          EventQueueing::kIgnore);

    // Sixty four hits do not fill the inbox, so the event of a mission that
    // arrives after them still gets in.
    CHECK(DecideQueueing(static_cast<uint8_t>(net::coop::EventType::ActorDamaged), 64, kInboxCapacity) ==
          EventQueueing::kIgnore);
    CHECK(DecideQueueing(static_cast<uint8_t>(net::coop::EventType::NamedEvent), 0, kInboxCapacity) ==
          EventQueueing::kQueue);

    // A full inbox drops the event and says so.
    CHECK(DecideQueueing(static_cast<uint8_t>(net::coop::EventType::NamedEvent), kInboxCapacity, kInboxCapacity) ==
          EventQueueing::kFull);
}

}  // namespace

int main() {
    TestCountDone();
    TestSumCounters();
    TestSameObjectives();
    TestMissionFinished();
    TestNameHash();
    TestTwinMatch();
    TestSummaryLine();
    TestEventQueueing();
    puts("coop mission test passed");
    return 0;
}
