// The pure part of the co-op mission layer.
//
// Nothing here touches the game: the comparison of two readings of the
// mission, the decision that the level is over, the counters and the id of an
// event's name. It is a file of its own so that tests/test_coop_mission.cpp
// can link it without the game.

#include "game/coop_mission.h"

#include <cstdio>
#include <cstring>

namespace game {
namespace coop {
namespace {

// The CRC-32 table the engine hashes the names of the events with
// (0x2AC908 in the image, the usual reflected polynomial).
struct Crc32Table {
    uint32_t entries[256];

    Crc32Table() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            entries[i] = c;
        }
    }
};

const Crc32Table& Table() {
    static const Crc32Table table;
    return table;
}

}  // namespace

uint32_t NameHash(const char* name, size_t length) {
    // The engine hashes the name without its last character: it counts the
    // string from its second byte to the byte before the zero
    // (sub_001F3AC0, x86_recomp.53.cpp:300). The id of a name is the same on
    // both sides because it is a function of the name alone.
    size_t bytes = length ? length - 1 : 0;
    const Crc32Table& table = Table();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < bytes; i++) {
        crc = table.entries[(crc ^ static_cast<uint8_t>(name[i])) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

uint32_t CountDone(const ObjectiveState& state) {
    uint32_t done = 0;
    for (uint32_t i = 0; i < state.count && i < kMaxObjectives; i++) {
        if (state.states[i] == 3) done++;
    }
    return done;
}

uint32_t SumCounters(const ObjectiveState& state) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 6; i++) sum += state.counters[i];
    return sum;
}

bool SameObjectives(const ObjectiveState& a, const ObjectiveState& b) {
    if (a.count != b.count) return false;
    if (a.done != b.done) return false;
    if (a.complete != b.complete) return false;
    for (uint32_t i = 0; i < 6; i++) {
        if (a.counters[i] != b.counters[i]) return false;
    }
    for (uint32_t i = 0; i < a.count && i < kMaxObjectives; i++) {
        if (a.states[i] != b.states[i]) return false;
    }
    return true;
}

bool MissionFinished(const ObjectiveState& state) {
    // The level is over when every objective of the table is done. A level
    // with no objectives is not over: the table is only there once the level
    // has been loaded.
    if (state.count == 0) return false;
    return state.done >= state.count;
}

EventQueueing DecideQueueing(uint8_t event_type, size_t inbox_size, size_t capacity) {
    switch (net::coop::EventType(event_type)) {
        case net::coop::EventType::NamedEvent:
        case net::coop::EventType::ObjectiveChanged:
        case net::coop::EventType::MissionEnded:
        case net::coop::EventType::ObjectInteracted:
            break;
        default:
            // The damage of an actor, the death of an actor, the activation
            // of an actor: the world layer's business, not the mission's.
            return EventQueueing::kIgnore;
    }
    if (inbox_size >= capacity) return EventQueueing::kFull;
    return EventQueueing::kQueue;
}

std::string SummaryLine(uint32_t epoch, uint32_t objectives, uint32_t done, uint32_t events, uint32_t unpaired,
                         uint32_t divergent) {
    char line[192];
    std::snprintf(line, sizeof(line), "coop: summary epoch=%u objectives=%u done=%u events=%u unpaired=%u divergent=%u",
                  epoch, objectives, done, events, unpaired, divergent);
    return line;
}

}  // namespace coop
}  // namespace game
