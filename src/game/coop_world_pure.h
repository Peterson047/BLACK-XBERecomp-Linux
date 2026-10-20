#pragma once
// The pure part of the co-op world layer.
//
// The world layer reads and writes the game's memory, so it can only be
// exercised with the game running. The decisions it makes - which actor is
// which, which actor of a snapshot pairs with which actor of the guest, how
// many actors have no pair, whether an event still arrives in order - are
// pure, and they live here so that a standalone test can reach them without
// the game and without its assets.
//
// The protocol these decisions serve is in .agents/teamwork/coop/PROTOCOL.md;
// the engine offsets they are about are in game/layout.h.

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "net/coop_protocol.h"

namespace game {
namespace coop {

// The identity of an actor: the engine's own object pair, the generation and
// the type. Every part of it is a property of the engine, so two processes
// that run the same level script agree on it on their own.
//
// The pair at actor+0x08 / actor+0x0C is the key the engine matches objects
// with, the generation at actor+0x374 is the manager's activation counter,
// and the type at actor+0xC4 tells two actors of the same script object apart.
// The group id at actor+0x39C is not part of it: it is 1 for every scripted
// actor.
struct ActorIdentity {
    uint32_t native_a = 0;    // actor+0x08
    uint32_t native_b = 0;    // actor+0x0C
    uint32_t generation = 0;  // actor+0x374
    uint16_t type = 0;        // actor+0xC4

    bool operator==(const ActorIdentity& other) const {
        return native_a == other.native_a && native_b == other.native_b && generation == other.generation &&
               type == other.type;
    }
    bool operator!=(const ActorIdentity& other) const { return !(*this == other); }
};

// The identity of an actor of a snapshot.
inline ActorIdentity IdentityOf(const net::coop::ActorState& state) {
    return ActorIdentity{state.native_a, state.native_b, state.generation, state.type};
}

// The index of the actor of the snapshot with this identity, or -1.
inline int FindSnapshotActor(const net::coop::Snapshot& snapshot, const ActorIdentity& identity) {
    uint32_t count = snapshot.header.actor_count;
    if (count > net::coop::kMaxActorsPerSnapshot) count = net::coop::kMaxActorsPerSnapshot;
    for (uint32_t i = 0; i < count; i++) {
        if (IdentityOf(snapshot.actors[i]) == identity) return static_cast<int>(i);
    }
    return -1;
}

// Pairs the guest's actors with the actors of a snapshot, by identity.
//
// The result has one entry per guest actor: the index of the snapshot actor
// it matches, or -1 when it matches nothing. The number of those -1 is the
// "actors without a pair" the protocol asks to be visible: a silent mismatch
// is the failure that killed the previous attempt.
//
// The snapshot actors the guest does not have are counted apart: they are
// the host's actors the guest has not activated yet, and they are not the
// same thing as a guest actor without a pair.
struct Pairing {
    std::vector<int> guest_to_snapshot;  // one entry per guest actor
    uint32_t paired = 0;
    uint32_t unpaired = 0;  // the guest actors that matched nothing
    uint32_t missing = 0;   // the snapshot actors the guest does not have
};

inline Pairing PairActors(const std::vector<ActorIdentity>& guests, const net::coop::Snapshot& snapshot) {
    Pairing pairing;
    pairing.guest_to_snapshot.reserve(guests.size());
    for (const ActorIdentity& guest : guests) {
        int index = FindSnapshotActor(snapshot, guest);
        pairing.guest_to_snapshot.push_back(index);
        if (index >= 0) {
            pairing.paired++;
        } else {
            pairing.unpaired++;
        }
    }
    uint32_t count = snapshot.header.actor_count;
    if (count > net::coop::kMaxActorsPerSnapshot) count = net::coop::kMaxActorsPerSnapshot;
    pairing.missing = count - pairing.paired;
    return pairing;
}

// The order the events have to arrive in.
//
// The session guards the order too, but the world layer keeps its own: the
// engine's damage routine is not safe to run twice for the same hit, and a
// packet that comes back is a packet that arrives again.
enum class EventOrderResult {
    kAccepted,    // the event is the next one
    kStaleEpoch,  // it belongs to a round that is over
    kOutOfOrder,  // it was already applied, or one arrives before another
};

class EventOrder {
public:
    // A new round: the sequence starts again.
    void Reset(uint32_t epoch) {
        epoch_ = epoch;
        last_seq_ = 0;
        has_seq_ = false;
    }

    EventOrderResult Accept(uint32_t epoch, uint32_t seq) {
        if (epoch < epoch_) return EventOrderResult::kStaleEpoch;
        if (epoch > epoch_) Reset(epoch);
        if (has_seq_ && seq <= last_seq_) return EventOrderResult::kOutOfOrder;
        last_seq_ = seq;
        has_seq_ = true;
        return EventOrderResult::kAccepted;
    }

    uint32_t epoch() const { return epoch_; }
    uint32_t last_seq() const { return last_seq_; }
    bool has_seq() const { return has_seq_; }

private:
    uint32_t epoch_ = 0;
    uint32_t last_seq_ = 0;
    bool has_seq_ = false;
};

// What the world layer counted during the round.
struct WorldCounters {
    uint32_t snapshots = 0;  // the snapshots the host published
    uint32_t events = 0;     // the events the host published
    uint32_t unpaired = 0;   // the guest actors that matched nothing
    uint32_t divergent = 0;  // the events that arrived out of order or stale
    uint32_t skipped = 0;    // the damages that were not applied
    uint32_t applied = 0;    // the damages that were replayed
};

// The log vocabulary of the co-op: "coop: <verb> key=value key=value ...".
// The keys are lowercase, there is no space around the "=" and no value
// carries a space, so a strict regular expression can read a line.
inline std::string FormatLog(const char* verb, const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string line = "coop: ";
    line += verb;
    for (const auto& field : fields) {
        line += ' ';
        line += field.first;
        line += '=';
        line += field.second;
    }
    return line;
}

// A number the log can carry: no space, no sign that a parser would trip on.
inline std::string LogNumber(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", value);
    return text;
}

}  // namespace coop
}  // namespace game
