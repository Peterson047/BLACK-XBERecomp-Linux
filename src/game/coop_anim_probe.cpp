// The animation probe: the state a character is in and the clip that state
// picks.
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
// This file hooks the two functions of that chain and writes down what it
// sees: the state, the four parameters of the record and the name of the
// clip. The table of the states and the poses was measured with it
// (`.agents/teamwork/coop/anim/STATE_TABLE.md`).
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
constexpr uint32_t kAnimSet = 0x50;    // the character's set of clips
constexpr uint32_t kAnimClip = 0x54;   // the clip that plays
constexpr uint32_t kAnimName = 0x5C;   // the name of that clip, 13 bytes
constexpr uint32_t kAnimNameSize = 13;

// The actor (LAYOUT.md §2 and §10)
constexpr uint32_t kActorType = 0xC4;
constexpr uint32_t kActorGeneration = 0x374;
constexpr uint32_t kActorAnim = 0x328;      // the animation controller
constexpr uint32_t kActorRecord = 0x32C;    // the animation state record
constexpr uint32_t kActorPosition = 0xA0;   // x, y, z: the matrix's last row

// The animation state record, at actor+0x32C: the state and four parameters
constexpr uint32_t kRecordState = 0x00;  // dword: 1..0x18
constexpr uint32_t kRecordP04 = 0x04;    // dword
constexpr uint32_t kRecordP08 = 0x08;    // float
constexpr uint32_t kRecordP0C = 0x0C;    // dword
constexpr uint32_t kRecordP10 = 0x10;    // byte

// What the probe remembers of an actor
struct Seen {
    uint32_t generation = 0;  // the actor's generation: a new actor is a new one
    uint32_t state = 0;
    uint32_t changes = 0;  // how many times the clip changed
    char clip[24] = {0};
    float x = 0, y = 0, z = 0;  // the last position
    uint32_t frame = 0;         // the frame of that position
    float speed = 0;            // units a frame
    bool placed = false;        // the position is known
    bool known = false;         // the actor was seen at least once
};

std::mutex g_lock;
std::unordered_map<uint32_t, Seen> g_seen;

// The record the state switch is reading, and the switch itself. `0x0017F270`
// takes the record in eax and the controller in ecx, and the arm the state
// takes starts a clip: the clip start inside the switch is the one the
// state chose.
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

// The clip of the actor, and the state that chose it. The record is the one
// the switch is reading, or 0 when the clip came from somewhere else (the
// idle fallback of the anim set, the level script).
void Report(uint32_t anim, uint32_t record, uint32_t name_at) {
    if (!Enabled()) return;
    uint32_t actor = R32(anim + kAnimOwner);
    if (!actor) return;
    std::string name = ReadCString(name_at, kAnimNameSize);
    uint32_t state = record ? R32(record + kRecordState) : 0;
    uint32_t frame = R32(layout::coop::kFrameCount);

    std::lock_guard<std::mutex> lock(g_lock);
    Seen& seen = g_seen[actor];
    uint32_t generation = R32(actor + kActorGeneration);
    if (seen.known && seen.generation != generation) seen = Seen{};  // a new actor took the place
    seen.generation = generation;

    bool changed = !seen.known || seen.state != state || std::strcmp(seen.clip, name.c_str()) != 0;
    bool sample = seen.known && SamplePeriod() && (frame % SamplePeriod()) == 0;

    // the speed is measured every time, so the line that is written later
    // still holds the speed of the frame the character moved in
    float speed = Speed(actor, seen, frame);
    if (!changed && !sample) return;

    if (changed) {
        seen.state = state;
        std::snprintf(seen.clip, sizeof(seen.clip), "%s", name.c_str());
        seen.changes++;
        seen.known = true;
    }

    uint32_t type = R32(actor + kActorType);
    if (record) {
        LOGI("ANIM a=%08X t=%u st=%u p04=%u p08=%.4f p0c=%u p10=%u clip=%s vf=%.5f", actor, type, state,
             R32(record + kRecordP04), G<float>(record + kRecordP08), R32(record + kRecordP0C),
             G<uint8_t>(record + kRecordP10), name.c_str(), speed);
    } else {
        LOGI("CLIP a=%08X t=%u st=%u clip=%s vf=%.5f", actor, type, state, name.c_str(), speed);
    }
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

}  // namespace coop_anim
}  // namespace game

namespace game {
namespace coop_anim {

// The state switch is running: the clip that starts inside it is the one the
// state chose. A clip that starts outside it is the idle fallback of the
// anim set or a clip the level script asked for.
void BeginSwitch(uint32_t record) {
    g_record = record;
    g_switching = true;
}

void EndSwitch() {
    g_switching = false;
    g_record = 0;
}

// A clip is starting: the controller and the name of the clip.
void OnClipStart(uint32_t anim, uint32_t name_at) {
    if (!Enabled()) return;
    static bool announced = false;
    if (!announced) {
        announced = true;
        LOGI("ANIM probe: frame time %.6f s, frame %u", G<float>(layout::kGameFrameTime),
             R32(layout::coop::kFrameCount));
    }
    Report(anim, g_switching ? g_record : 0, name_at);
}

}  // namespace coop_anim
}  // namespace game

// The state switch: `0x0017F270` takes the record in eax and the controller
// in ecx, reads the state out of the record and starts the clip the state
// chose.
X86_EXTERN_FUNC(__imp__sub_0017F270);
X86_FUNC(sub_0017F270) {
    game::coop_anim::BeginSwitch(ctx.eax);
    __imp__sub_0017F270(ctx, base);
    game::coop_anim::EndSwitch();
}

// The clip start: `0x0017E130` takes the controller in eax and the name of
// the clip (13 bytes) in the first argument.
X86_EXTERN_FUNC(__imp__sub_0017E130);
X86_FUNC(sub_0017E130) {
    game::coop_anim::OnClipStart(ctx.eax, R32(ctx.esp + 4));
    __imp__sub_0017E130(ctx, base);
}
