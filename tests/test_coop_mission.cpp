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

    // Two names of the same length are two ids.
    const char* other = "FE_PRIMARYOBJ_NEW_";
    CHECK(NameHash(name, std::strlen(name)) != NameHash(other, std::strlen(other)));

    // The engine hashes the name without its last character, so two names that
    // differ only in the last character have the same id.
    const char* twin = "FE_PRIMARYOBJ_DONF";
    CHECK(std::strlen(twin) == std::strlen(name));
    CHECK(NameHash(name, std::strlen(name)) == NameHash(twin, std::strlen(twin)));

    // An empty name has the id of nothing.
    CHECK(NameHash("", 0) == 0xFFFFFFFFu);
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

}  // namespace

int main() {
    TestCountDone();
    TestSumCounters();
    TestSameObjectives();
    TestMissionFinished();
    TestNameHash();
    TestSummaryLine();
    puts("coop mission test passed");
    return 0;
}
