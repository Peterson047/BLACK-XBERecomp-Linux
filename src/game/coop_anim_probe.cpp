// The animation probe: the state a character is in and the clip that plays.
//
// The engine drives the bones of a character from a five field record its
// controller writes at `actor+0x32C`. `0x0017F270` reads that record: the
// state in it picks a method of the character's anim set, the method picks
// the name of the clip, and `0x0017E130` starts the clip. The clip is what
// moves the bones.
//
// The world layer only writes the position and the direction of the partner
// (`sub_001CE600`, `sub_001CE660`), and neither of them touches the record,
// so a body the network drives slides with a frozen pose.
//
// The probe reads the record and the clip of every actor that animates and
// writes down what changed. The table of the states and the poses was
// measured with it (`.agents/teamwork/coop/anim/STATE_TABLE.md`).
//
// It is quiet unless `BLACK_COOP_ANIM_PROBE=1` is in the environment. A log
// that speaks on every frame is a log nobody reads, so a line is written
// only when the state or the clip of an actor changes, plus a slow sample
// line (one every `BLACK_COOP_ANIM_PROBE_SAMPLE` frames, 15 by default) for
// the measurement of the speed.

#include "game/coop_anim_probe.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "core/log.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;

namespace game {
namespace coop_anim {
namespace {

// The animation controller, at actor+0x328 (LAYOUT.md §10)
constexpr uint32_t kAnimOwner = 0x00;  // the actor the controller belongs to
constexpr uint32_t kAnimName = 0x5C;   // the name of the clip that plays, 13 bytes
constexpr uint32_t kAnimNameSize = 13;

// The actor (LAYOUT.md §2 and §10)
constexpr uint32_t kActorType = 0xC4;
constexpr uint32_t kActorGeneration = 0x374;
constexpr uint32_t kActorAnim = 0x328;     // the animation controller
constexpr uint32_t kActorRecord = 0x32C;   // the animation state record
constexpr uint32_t kActorPosition = 0xA0;  // x, y, z: the matrix's last row

// The animation state record, at actor+0x32C: the state and four parameters
constexpr uint32_t kRecordState = 0x00;  // dword: 1..0x18
constexpr uint32_t kRecordP04 = 0x04;    // dword
constexpr uint32_t kRecordP08 = 0x08;    // float
constexpr uint32_t kRecordP0C = 0x0C;    // dword
constexpr uint32_t kRecordP10 = 0x10;    // byte

// What the probe remembers of an actor
struct Seen {
    uint32_t generation = 0;  // the actor's generation: a new actor is a new one
    uint32_t state = 0;       // the state of the record
    bool has_state = false;   // the state is known
    uint32_t changes = 0;     // how many times the clip changed
    char clip[24] = {0};      // the clip that plays
    char chosen[48] = {0};    // the clip the state chose, and the arm that chose it
    float x = 0, y = 0, z = 0;  // the last position
    uint32_t frame = 0;         // the frame of that position
    float speed = 0;            // units a frame
    bool placed = false;        // the position is known
    bool known = false;         // the actor was seen at least once
};

std::mutex g_lock;
std::unordered_map<uint32_t, Seen> g_seen;

// The record the state switch is reading, and the switch itself.
// `0x0017F270` takes the record in eax and the controller in ecx, and the
// arm the state takes starts a clip: the clip that starts inside the
// switch is the one the state chose.
uint32_t g_record = 0;
bool g_switching = false;

// The probe is off unless the environment asks for it
bool EnvOn(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != 0 && value[0] != '0';
}

// How many frames between two sample lines
uint32_t SamplePeriod() {
    static const uint32_t period = [] {
        const char* value = std::getenv("BLACK_COOP_ANIM_PROBE_SAMPLE");
        int n = value ? std::atoi(value) : 15;
        return n > 0 ? uint32_t(n) : 15u;
    }();
    return period;
}

// The distance the actor moved since the last frame the probe saw it, in
// units a frame. The position is the matrix's last row (LAYOUT.md §2).
float Speed(uint32_t actor, Seen& seen, uint32_t frame) {
    float x = G<float>(actor + kActorPosition);
    float y = G<float>(actor + kActorPosition + 4);
    float z = G<float>(actor + kActorPosition + 8);
    if (!seen.placed) {
        seen.x = x;
        seen.y = y;
        seen.z = z;
        seen.frame = frame;
        seen.placed = true;
        return 0.0f;
    }
    if (frame == seen.frame) return seen.speed;  // the same frame: the same speed
    uint32_t frames = frame - seen.frame;
    float dx = x - seen.x;
    float dy = y - seen.y;
    float dz = z - seen.z;
    seen.x = x;
    seen.y = y;
    seen.z = z;
    seen.frame = frame;
    seen.speed = std::sqrt(dx * dx + dy * dy + dz * dz) / float(frames);
    return seen.speed;
}

}  // namespace

bool Enabled() {
    static const bool on = EnvOn("BLACK_COOP_ANIM_PROBE");
    return on;
}

std::string ClipOf(uint32_t actor) {
    uint32_t anim = R32(actor + kActorAnim);
    if (!anim) return std::string();
    return ReadCString(anim + kAnimName, kAnimNameSize);
}

uint32_t StateOf(uint32_t actor) {
    return R32(actor + kActorRecord + kRecordState);
}

uint32_t ClipChanges(uint32_t actor) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_seen.find(actor);
    return it == g_seen.end() ? 0u : it->second.changes;
}

void Report(uint32_t actor) {
    if (!Enabled() || !actor) return;
    uint32_t anim = R32(actor + kActorAnim);
    if (!anim) return;
    std::string name = ReadCString(anim + kAnimName, kAnimNameSize);
    uint32_t state = R32(actor + kActorRecord + kRecordState);
    uint32_t frame = R32(layout::coop::kFrameCount);

    std::lock_guard<std::mutex> lock(g_lock);
    Seen& seen = g_seen[actor];
    uint32_t generation = R32(actor + kActorGeneration);
    if (seen.known && seen.generation != generation) seen = Seen{};  // a new actor took the place
    seen.generation = generation;

    bool changed = !seen.known || std::strcmp(seen.clip, name.c_str()) != 0;
    bool state_changed = !seen.has_state || seen.state != state;
    bool sample = seen.known && SamplePeriod() && (frame % SamplePeriod()) == 0;

    // the speed is measured every time, so the line that is written later
    // still holds the speed of the frame the character moved in
    float speed = Speed(actor, seen, frame);
    if (!changed && !state_changed && !sample) return;

    if (state_changed) {
        seen.state = state;
        seen.has_state = true;
    }
    if (changed) {
        std::snprintf(seen.clip, sizeof(seen.clip), "%s", name.c_str());
        seen.changes++;
        seen.known = true;
    }

    uint32_t type = R32(actor + kActorType);
    LOGI("ANIM a=%08X t=%u st=%u p04=%u p08=%.4f p0c=%u p10=%u clip=%s vf=%.5f", actor, type, state,
         R32(actor + kActorRecord + kRecordP04), G<float>(actor + kActorRecord + kRecordP08),
         R32(actor + kActorRecord + kRecordP0C), G<uint8_t>(actor + kActorRecord + kRecordP10),
         name.c_str(), speed);
}

void Chosen(uint32_t actor, uint32_t record, const std::string& clip, uint32_t from) {
    if (!Enabled() || !actor || !record) return;
    uint32_t state = R32(record + kRecordState);
    uint32_t frame = R32(layout::coop::kFrameCount);

    std::lock_guard<std::mutex> lock(g_lock);
    Seen& seen = g_seen[actor];
    // the clip the state chose, and the arm of the switch that chose it
    char key[48];
    std::snprintf(key, sizeof(key), "%s|%08X", clip.c_str(), from);
    if (seen.known && std::strcmp(seen.chosen, key) == 0) return;
    std::snprintf(seen.chosen, sizeof(seen.chosen), "%s", key);
    seen.known = true;

    uint32_t type = R32(actor + kActorType);
    LOGI("CHOSEN a=%08X t=%u st=%u p04=%u p08=%.4f p0c=%u p10=%u clip=%s from=%08X f=%u", actor, type, state,
         R32(record + kRecordP04), G<float>(record + kRecordP08), R32(record + kRecordP0C),
         G<uint8_t>(record + kRecordP10), clip.c_str(), from, frame);
}

// The record the state switch is reading, and the switch itself.
// `0x0017F270` takes the record in eax and the controller in ecx, and the
// arm the state takes starts a clip: the clip that starts inside the
// switch is the one the state chose.
void BeginSwitch(uint32_t record) {
    g_record = record;
    g_switching = true;
}

void EndSwitch() {
    g_switching = false;
    g_record = 0;
}

uint32_t Record() { return g_record; }

bool Switching() { return g_switching; }

}  // namespace coop_anim
}  // namespace game

// The animation tick: `0x001801B0` takes the animation controller in eax.
// It runs once a frame for every actor that animates, and the clip state
// machine inside it has just resolved the names, so the name in the
// controller and the state in the record are the ones of this frame.
X86_EXTERN_FUNC(__imp__sub_001801B0);
X86_FUNC(sub_001801B0) {
    uint32_t anim = ctx.eax;
    __imp__sub_001801B0(ctx, base);
    if (!game::coop_anim::Enabled()) return;
    game::coop_anim::Report(R32(anim + 0x00));
}

// The state switch: `0x0017F270` takes the record in eax and the
// controller in ecx, reads the state out of the record and starts the clip
// the state chose.
X86_EXTERN_FUNC(__imp__sub_0017F270);
X86_FUNC(sub_0017F270) {
    game::coop_anim::BeginSwitch(ctx.eax);
    __imp__sub_0017F270(ctx, base);
    game::coop_anim::EndSwitch();
}

// The start of a clip: `0x0017E130` takes the animation controller in eax
// and the name of the clip (13 bytes) in the first argument. The engine
// calls it with the clip the state chose, so this is where the state and
// the clip it produced meet.
X86_EXTERN_FUNC(__imp__sub_0017E130);
X86_FUNC(sub_0017E130) {
    uint32_t anim = ctx.eax;
    uint32_t name_at = R32(ctx.esp + 4);
    uint32_t from = R32(ctx.esp);
    if (game::coop_anim::Enabled() && game::coop_anim::Switching()) {
        uint32_t owner = R32(anim + 0x00);
        if (owner) {
            game::coop_anim::Chosen(owner, game::coop_anim::Record(), ReadCString(name_at, 13), from);
        }
    }
    __imp__sub_0017E130(ctx, base);
}
