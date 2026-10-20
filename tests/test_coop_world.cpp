// Standalone test of the pure part of the co-op world layer. No game
// resources, no guest memory, no network.
//
// The world layer itself reads and writes the engine's memory, so it can only
// be exercised with the game running. The decisions it makes - which actor is
// which, which actor of a snapshot pairs with which actor of the guest, how
// many actors have no pair, whether an event still arrives in order - are
// pure and live in game/coop_world_pure.h, which is what this test uses.
//
// Build: clang++ -std=c++20 -Isrc -Isdk/runtime tests/test_coop_world.cpp \
//        -o /tmp/test_coop_world && /tmp/test_coop_world
#include "game/coop_world_pure.h"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using game::coop::ActorIdentity;
using game::coop::EventOrder;
using game::coop::EventOrderResult;
using game::coop::FormatLog;
using game::coop::IdentityOf;
using game::coop::PairActors;
using net::coop::ActorState;
using net::coop::Snapshot;

namespace {

[[noreturn]] void Fail(const char* message, int line) {
    throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + message);
}

#define CHECK(expr) do { if (!(expr)) Fail(#expr, __LINE__); } while (false)

ActorState MakeActor(uint32_t a, uint32_t b, uint32_t generation, uint16_t type) {
    ActorState actor{};
    actor.native_a = a;
    actor.native_b = b;
    actor.generation = generation;
    actor.type = type;
    actor.pos[0] = 1.0f;
    actor.pos[1] = 2.0f;
    actor.pos[2] = 3.0f;
    actor.yaw = 0.5f;
    actor.pitch = -0.25f;
    actor.health = 100.0f;
    return actor;
}

Snapshot MakeSnapshot(const std::vector<ActorState>& actors) {
    Snapshot snapshot{};
    snapshot.header.start_epoch = 3;
    snapshot.header.actor_count = static_cast<uint32_t>(actors.size());
    for (size_t i = 0; i < actors.size(); i++) snapshot.actors[i] = actors[i];
    return snapshot;
}

// The identity is the engine's own: the object pair, the generation and the
// type. Two of them are only the same when all four parts are.
void TestIdentityComparison() {
    ActorIdentity first{0x11112222, 0x33334444, 5, 1};
    ActorIdentity same{0x11112222, 0x33334444, 5, 1};
    CHECK(first == same);
    CHECK(!(first != same));

    // A different generation is a different actor: the same script object
    // activated again.
    ActorIdentity other_generation{0x11112222, 0x33334444, 6, 1};
    CHECK(first != other_generation);

    // The same generation with a different type is a different actor too.
    ActorIdentity other_type{0x11112222, 0x33334444, 5, 2};
    CHECK(first != other_type);

    // The pair alone is not the identity.
    ActorIdentity other_pair{0x11112222, 0x33335555, 5, 1};
    CHECK(first != other_pair);

    // The identity of an actor of a snapshot is the same thing.
    ActorState state = MakeActor(0x11112222, 0x33334444, 5, 1);
    CHECK(IdentityOf(state) == first);
}

// The pairing matches the actors of the guest with the actors of the
// snapshot, and counts the ones that match nothing.
void TestPairing() {
    Snapshot snapshot = MakeSnapshot({MakeActor(1, 2, 5, 1), MakeActor(3, 4, 6, 1), MakeActor(5, 6, 7, 1)});
    std::vector<ActorIdentity> guests = {
        ActorIdentity{1, 2, 5, 1},
        ActorIdentity{3, 4, 6, 1},
        ActorIdentity{9, 9, 9, 1},  // the guest has an actor the host does not
    };
    auto pairing = PairActors(guests, snapshot);
    CHECK(pairing.guest_to_snapshot.size() == 3);
    CHECK(pairing.guest_to_snapshot[0] == 0);
    CHECK(pairing.guest_to_snapshot[1] == 1);
    CHECK(pairing.guest_to_snapshot[2] == -1);
    CHECK(pairing.paired == 2);
    CHECK(pairing.unpaired == 1);
    // the host's third actor is not on the guest: that is counted apart
    CHECK(pairing.missing == 1);

    // An actor of another type is not a match: the type is part of the
    // identity and nothing can rescue it.
    std::vector<ActorIdentity> wrong = {
        ActorIdentity{1, 2, 5, 7},
        ActorIdentity{3, 4, 6, 7},
    };
    auto unmatched = PairActors(wrong, snapshot);
    CHECK(unmatched.paired == 0);
    CHECK(unmatched.unpaired == 2);
    CHECK(unmatched.missing == 3);

    // The same script object, activated in another order on the other side,
    // is the same actor: the generation is the manager's activation counter
    // and the two processes do not count in the same order, so the pair and
    // the position are what is left.
    // The snapshot's actors are all at (1, 2, 3).
    ActorIdentity permuted{1, 2, 99, 1};
    permuted.pos[0] = 1.0f;
    permuted.pos[1] = 2.0f;
    permuted.pos[2] = 3.5f;
    ActorIdentity other{3, 4, 6, 1};
    other.pos[0] = 1.5f;
    other.pos[1] = 2.0f;
    other.pos[2] = 3.0f;
    std::vector<ActorIdentity> shifted = {permuted, other};
    auto by_position = PairActors(shifted, snapshot);
    CHECK(by_position.paired == 2);
    // only the permuted one needed the second chance
    CHECK(by_position.by_position == 1);
    CHECK(by_position.unpaired == 0);

    // The same script object, but far away: that is another actor, and the
    // second chance does not reach it.
    ActorIdentity far = permuted;
    far.pos[0] = 500.0f;
    std::vector<ActorIdentity> distant = {far};
    auto too_far = PairActors(distant, snapshot);
    CHECK(too_far.paired == 0);
    CHECK(too_far.unpaired == 1);
    CHECK(too_far.missing == 3);

    // An empty snapshot leaves every guest actor without a pair.
    auto empty = PairActors(guests, MakeSnapshot({}));
    CHECK(empty.paired == 0);
    CHECK(empty.unpaired == 3);
    CHECK(empty.missing == 0);
}

// The order of the events: a packet of a round that is over is dropped, and
// an event that was already applied is dropped, because the engine's damage
// routine is not safe to run twice.
void TestEventOrder() {
    EventOrder order;
    order.Reset(3);
    CHECK(order.epoch() == 3);
    CHECK(order.Accept(3, 1) == EventOrderResult::kAccepted);
    CHECK(order.Accept(3, 2) == EventOrderResult::kAccepted);
    CHECK(order.last_seq() == 2);

    // the same event again
    CHECK(order.Accept(3, 2) == EventOrderResult::kOutOfOrder);
    CHECK(order.Accept(3, 1) == EventOrderResult::kOutOfOrder);
    // and the order did not move
    CHECK(order.last_seq() == 2);

    // a round that is over
    CHECK(order.Accept(2, 9) == EventOrderResult::kStaleEpoch);
    CHECK(order.epoch() == 3);
    CHECK(order.last_seq() == 2);

    // a new round starts the sequence again
    CHECK(order.Accept(4, 1) == EventOrderResult::kAccepted);
    CHECK(order.epoch() == 4);
    CHECK(order.last_seq() == 1);
    CHECK(order.Accept(4, 1) == EventOrderResult::kOutOfOrder);
    CHECK(order.Accept(3, 5) == EventOrderResult::kStaleEpoch);
}

// The log vocabulary: "coop: <verb> key=value key=value ...". A strict
// regular expression has to be able to read it.
void TestLogVocabulary() {
    std::string line = FormatLog("snapshot", {{"epoch", "3"}, {"actors", "7"}});
    CHECK(line == "coop: snapshot epoch=3 actors=7");

    // no spaces around the "=" and no value with a space in it
    for (char c : line) CHECK(c != '\t');
    size_t space = line.find(' ');
    while (space != std::string::npos) {
        CHECK(line.find('=', space) == space + 1 || line[space + 1] != '=');
        space = line.find(' ', space + 1);
    }

    // the summary the session ends with
    std::string summary = FormatLog("summary", {{"epoch", "3"},
                                                {"snapshots", "30"},
                                                {"events", "12"},
                                                {"unpaired", "4"},
                                                {"divergent", "0"}});
    CHECK(summary == "coop: summary epoch=3 snapshots=30 events=12 unpaired=4 divergent=0");

    // the numbers the log carries have no space in them
    CHECK(game::coop::LogNumber(12.5f) == "12.500");
    CHECK(game::coop::LogNumber(-0.25f) == "-0.250");
}

}  // namespace

int main() {
    TestIdentityComparison();
    TestPairing();
    TestEventOrder();
    TestLogVocabulary();
    puts("coop world test passed");
    return 0;
}
