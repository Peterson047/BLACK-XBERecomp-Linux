// The pure part of the co-op mission layer.
//
// Nothing here touches the game: the comparison of two readings of the
// mission, the decision that the level is over, the counters and the id of an
// event's name. It is a file of its own so that tests/test_coop_mission.cpp
// can link it without the game.

#include "game/coop_mission.h"

#include <cstdio>
#include <cstring>
#include <deque>

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
    // The engine hashes the WHOLE name.
    //
    // sub_001F3AC0 (x86_recomp.53.cpp:300) does "lea esi, [eax+1]" only to
    // have where the count of the string starts: it walks the string to its
    // zero, takes the difference ("sub eax,esi" -> the length) and calls
    // sub_000BF2A0 with ecx = the name and edx = the length. So the hash
    // covers every byte of the name and not the zero at the end.
    //
    // The shift of the engine is arithmetic ("sar eax, 8"), so the id is
    // not the plain CRC-32: the table of the image is at 0x2AC908 (the
    // usual reflected table) and the options menu hashes with the same
    // recipe (src/game/options_menu.cpp:132). The id of a name is the same
    // on both sides because it is a function of the name alone.
    const Crc32Table& table = Table();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; i++) {
        // The engine reads the byte with "movsx", so a byte with the high
        // bit set would take the index out of the table. The names of the
        // events are ascii, and the low byte is what the table is indexed
        // with.
        crc = uint32_t(int32_t(crc) >> 8) ^ table.entries[(crc ^ static_cast<uint8_t>(name[i])) & 0xFF];
    }
    return crc;
}

bool MatchTwin(std::deque<std::string>* local_events, const std::string& key) {
    // The twin check: does the guest's own engine already have this event?
    //
    // The queue of the guest's own events is searched WHOLE and the element
    // that matches is the one that is erased. The two copies of the level
    // run different scripts, so the two sequences do not match one by one:
    // looking only at the head of the queue gives up at the first
    // difference, and from then on the guest fires the host's copy of every
    // event and keeps its own, which counts every objective twice.
    if (!local_events) return false;
    for (auto it = local_events->begin(); it != local_events->end(); ++it) {
        if (*it != key) continue;
        local_events->erase(it);
        return true;
    }
    return false;
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
