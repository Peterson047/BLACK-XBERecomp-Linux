// The partner's body: the actor that stands in for the partner in the
// guest's world.
//
// The body is made the way the engine makes an actor (`0x001DF340`): it
// comes from the pool of the actor manager and goes into the list of the
// living actors, and being in that list is what makes the engine tick it,
// and the tick is what runs the animation.
//
// The animation is driven by the state that comes over the network:
//
//  - the record at `actor+0x32C` says what the body is doing: standing (1),
//    aiming (13) and the shot (7). The table of the states and the clips
//    they produce is in `.agents/teamwork/coop/anim/STATE_TABLE.md`;
//  - the speed at `actor+0x2D8` says how fast the body is going. The record
//    does not carry the speed, and the clip of the walk is not chosen by the
//    record: it is the anim set's own choice (`0x0017E280`), which asks
//    whether the body is moving (`sub_0017AC80`) and that asks the speed;
//  - the position and the direction go in with the engine's own calls
//    (`0x001CE660` and `0x001CE600`).
//
// The body is not an object of the level: the pair of the identity stays at
// 0/0, so the level script never gets a handle on it and the matcher of the
// objectives never matches it (LAYOUT.md §2 and §8). The activation is not
// called: it would write the identity and the generation, and the body would
// be an actor of the mission.
//
// The engine's memory is only touched on the engine's thread:
// `CurrentContext()` dereferences a thread local that is null on the thread
// that draws.

#include "game/coop_body.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "game/coop_anim_probe.h"
#include "game/coop_world_pure.h"
#include "game/layout.h"
#include "net/network.h"
#include "xdk/hle.h"

using namespace kernel;

namespace game {
namespace coop {
namespace {

// The engine's addresses and offsets. Every one of them is proved in
// .agents/teamwork/coop/LAYOUT.md, section 10 and the section 11 that this
// layer added.

// the manager of the AI (LAYOUT.md §4)
constexpr uint32_t kAiManager = 0x002D1988;
// the table of the character types and of their anim sets (LAYOUT.md §10)
constexpr uint32_t kCharacterTable = 0x002D12C8;

// the actor manager (LAYOUT.md §1)
constexpr uint32_t kActorPool = 0x79A0;       // the pool of the free actors
constexpr uint32_t kActorLiveList = 0x79B0;   // the list of the living actors
constexpr uint32_t kActorListArray = 0x79BC;  // the array of the living actors
constexpr uint32_t kActorCapacity = 0x79B4;   // how many places the list has

// the actor (LAYOUT.md §2 and §10)
constexpr uint32_t kActorKeyA = 0x08;
constexpr uint32_t kActorKeyB = 0x0C;
constexpr uint32_t kActorType = 0xC4;
constexpr uint32_t kActorWeapon = 0x29C;     // the weapon of the actor
constexpr uint32_t kActorSpeed = 0x2D8;      // float: the speed, units a second
constexpr uint32_t kActorController = 0x324; // the AI controller
constexpr uint32_t kActorAnim = 0x328;      // the animation controller
constexpr uint32_t kActorRecord = 0x32C;    // the animation state record
constexpr uint32_t kActorBrain = 0x344;     // the brain of the actor
constexpr uint32_t kActorSlot = 0x378;      // the place of the actor in the AI
constexpr uint32_t kActorState = 0x384;     // 0 alive, 1 recycled, 2 dead

// the animation controller (LAYOUT.md §10)
constexpr uint32_t kAnimSet = 0x50;    // the anim set of the character type
constexpr uint32_t kAnimSize = 0x240;  // the size of one controller

// the AI controller (LAYOUT.md §4)
constexpr uint32_t kAiActor = 0x8C;   // the actor of the controller
constexpr uint32_t kAiMode = 0x90;   // the mode of the hit state
constexpr uint32_t kAiBrainIndex = 0x280; // the index of the brain, -1 is none
constexpr uint32_t kAiNext = 0x1F00; // the next controller of the free list
constexpr uint32_t kAiFreeList = 0x22CDC;  // the free list of the controllers
constexpr uint32_t kAiSlotTable = 0xFA8;   // the table of the places
constexpr uint32_t kAiBrainTable = 0x22C48;  // the table of the brains

// the animation state record (LAYOUT.md §10)
constexpr uint32_t kRecordState = 0x00;  // dword
constexpr uint32_t kRecordP04 = 0x04;    // dword
constexpr uint32_t kRecordP08 = 0x08;    // float
constexpr uint32_t kRecordP0C = 0x0C;    // dword
constexpr uint32_t kRecordP10 = 0x10;    // byte

// the states of the record (STATE_TABLE.md §2)
constexpr uint32_t kStateStand = 1;  // the standing poses
constexpr uint32_t kStateShot = 7;   // the shot
constexpr uint32_t kStateAim = 13;   // the aim, the pitch in the +0x08

// the type of the body: a soldier (the player of the process is 2)
constexpr uint32_t kTypeSoldier = 1;
// the character type of the soldier: the anim set at table+0x398
constexpr uint32_t kSoldierCharacter = 7;
constexpr uint32_t kAnimSetBase = 0xF8;
constexpr uint32_t kAnimSetStride = 0x60;

// the engine's own calls (LAYOUT.md section 11)
constexpr uint32_t kPoolPop = 0x001C4A90;    // one actor out of the pool
constexpr uint32_t kListAdd = 0x001B6770;    // one actor into the living list
constexpr uint32_t kAnimAttach = 0x0017E420; // the animation controller
constexpr uint32_t kAiSetup = 0x001D21F0;    // the AI controller of an actor
constexpr uint32_t kAiSlot = 0x001880C0;     // the place of the actor in the AI
constexpr uint32_t kAiBrainCall = 0x001899F0;  // the brain of the actor

// the shot: how long the state 7 is held. The host says how long in the
// event; this is the length of the flash of the muzzle, the same number the
// Deathmatch uses.
constexpr float kShotSeconds = 0.12f;

// the eye of a character is this far above its feet
constexpr float kEyeHeight = 1.70f;
constexpr float kEyeHeightCrouched = 1.10f;

// the flags of the state of a player (network.h)
constexpr uint32_t kFlagCrouching = 1 << 2;
constexpr uint32_t kFlagAiming = 1 << 1;

// The scratch of the engine: the position and the direction go into the
// guest's memory, because the engine reads them with movaps.
uint32_t Scratch() {
    static uint32_t scratch = mem::AllocSystem(0x40, 16);
    return scratch;
}

// The frame time of the game, in seconds.
float FrameTime() {
    const float dt = G<float>(R32(layout::kWorld) + layout::kTimerDt);
    if (dt > 0.0f && dt < 1.0f) return dt;
    return G<float>(layout::kGameFrameTime);
}

}  // namespace

Body& Body::Get() {
    static Body instance;
    return instance;
}

// The events of the world. The only one the body cares about is the shot:
// the flag of the shooting of the player's state is a level trigger (the
// timer of the muzzle flash, sampled at 30 Hz) and the guest can miss a
// shot with it.
bool Body::ApplyEvent(const net::coop::Event& event) {
    if (event.state.event_type != static_cast<uint8_t>(net::coop::EventType::Shot)) return false;
    shots_++;
    shot_left_ = event.state.value > 0.0f ? event.state.value : kShotSeconds;
    LOGI("%s", FormatLog("body", {{"what", "shot"},
                                  {"event", "shot"},
                                  {"seq", std::to_string(event.state.event_seq)},
                                  {"seconds", LogNumber(shot_left_)}})
                   .c_str());
    return true;
}

// The engine's thread, right after the engine's own tick of the actor.
//
// The host has no body: the host is the partner. What the host does is
// watch its own weapon, because the guest's body has to play the shot clip
// when the partner fires, and the host knows when the shot happened (the
// magazine went down) in a way the sampled flag of the state cannot say.
void Body::AfterActorTick(uint32_t actor) {
    if (!Current()) return;  // the engine's memory is only touched here
    const net::NetMode mode = net::NetworkManager::Get().GetMode();
    if (mode == net::NetMode::Host) {
        PublishLocalShot();
        return;
    }
    if (mode != net::NetMode::Client) return;

    // the body is made once, on the first tick of the round
    if (!actor_ && !Create()) return;
    if (actor != actor_) return;
    // the transform goes in again: the tick of the engine may have moved
    // the actor, and the position of the partner is the truth
    WriteTransform();
}

// The engine's thread, right before the engine's own tick of the actor.
//
// The engine's tick of a living actor (0x001F0860) reads the record and
// hands it to the state switch that picks the clip. The body writes the
// record here, so the clip of the frame is the clip of the state of the
// frame. The body has no weapon and the tick of an actor without a weapon
// does not touch the record, so what is written here is what the switch
// reads.
void Body::BeforeActorTick(uint32_t actor) {
    if (!Current()) return;
    const net::NetMode mode = net::NetworkManager::Get().GetMode();
    if (mode != net::NetMode::Client) return;
    if (!actor_ && !Create()) return;
    if (actor != actor_) return;
    Update();
}

// The body is still the one this layer made.
bool Body::Alive() const {
    if (!actor_) return false;
    if (R32(actor_) != layout::coop::kActorVtable) return false;
    if (R32(actor_ + kActorState) != 0) return false;  // alive
    if (R32(actor_ + kActorKeyA) != 0 || R32(actor_ + kActorKeyB) != 0) return false;
    return R32(actor_ + kActorAnim) == anim_;
}

// The guest: the body of the partner.
//
// The actor comes from the pool of the manager and goes into the list of
// the living actors, the way `0x001DF340` does it. The identity stays at
// 0/0, the activation is not called, and the weapon is left empty: with no
// weapon the tick of the actor goes straight from the record to the clip.
bool Body::Create() {
    const uint32_t manager = R32(layout::coop::kActorManager);
    if (!manager) return false;
    if (manager_ && manager_ != manager) {
        // the level came again: the actor of the old level is gone
        actor_ = 0;
        anim_ = 0;
        controller_ = 0;
    }
    manager_ = manager;

    const uint32_t animset = FindAnimSet();
    if (!animset) {
        if (failed_++ < 4) {
            LOGI("%s", FormatLog("body", {{"what", "create"},
                                          {"reason", "no_anim_set"},
                                          {"manager", std::to_string(manager)}})
                           .c_str());
        }
        return false;
    }

    X86Context& ctx = CurrentContext();

    // one actor out of the pool
    ctx.edx = manager + kActorPool;
    const uint32_t actor = CallGuest(ctx, kPoolPop, {});
    if (!actor) {
        if (failed_++ < 4) {
            LOGI("%s", FormatLog("body", {{"what", "create"}, {"reason", "no_actor"}}).c_str());
        }
        return false;
    }

    // and into the list of the living actors: that is what makes the engine
    // tick it, and the tick is what runs the animation
    ctx.esi = manager + kActorLiveList;
    ctx.ebx = actor;
    CallGuest(ctx, kListAdd, {});

    // the identity of the level stays at 0/0: the body is not a script
    // object, so the level script never gets a handle on it and the matcher
    // of the objectives never matches it
    W32(actor + kActorKeyA, 0);
    W32(actor + kActorKeyB, 0);
    W32(actor + layout::coop::kActorGeneration, 0);
    W32(actor + kActorType, kTypeSoldier);
    W32(actor + kActorState, 0);     // alive
    W32(actor + kActorWeapon, 0);    // no weapon: the tick goes straight to the clip
    W32(actor + kActorRecord + kRecordState, kStateStand);

    // the three rows of the matrix are the axes of the actor: the identity,
    // as the activation writes them
    std::memcpy(HostPtr(actor + layout::coop::kActorBasis), HostPtr(0x002ACE70), 48);
    std::memcpy(HostPtr(actor + layout::coop::kActorPosition), HostPtr(0x002F4390), 16);

    // the AI controller of the actor. Without one the tick of the actor
    // returns before the tick of the animation and the body never animates
    // (LAYOUT.md section 11). The brain index is -1, so the AI does not
    // think: the body is not a soldier, it is the partner.
    const uint32_t ai = R32(kAiManager);
    if (!ai) {
        LOGI("%s", FormatLog("body", {{"what", "create"}, {"reason", "no_ai"}}).c_str());
        return false;
    }

    // the place of the actor in the table of the AI
    ctx.edx = ai + kAiSlotTable;
    const uint32_t slot = CallGuest(ctx, kAiSlot, {actor});
    W32(actor + kActorSlot, slot);

    // the brain of the actor: the table of the brains is at ai+0x22C48 and
    // the one of the type of the actor is inside it. The controller reads it
    // when it is set up, so it comes first.
    ctx.ecx = ai + kAiBrainTable;
    ctx.edi = actor;
    CallGuest(ctx, kAiBrainCall, {});
    const uint32_t brain = R32(actor + kActorBrain);
    if (!brain) {
        if (failed_++ < 4) {
            LOGI("%s", FormatLog("body", {{"what", "create"}, {"reason", "no_brain"}}).c_str());
        }
        return false;
    }

    const uint32_t controller = R32(ai + kAiFreeList);
    if (!controller) {
        if (failed_++ < 4) {
            LOGI("%s", FormatLog("body", {{"what", "create"}, {"reason", "no_controller"}}).c_str());
        }
        return false;
    }
    W32(ai + kAiFreeList, R32(controller + kAiNext));  // the controller leaves the free list
    ctx.edi = controller;
    ctx.eax = actor;
    CallGuest(ctx, kAiSetup, {actor});
    W32(actor + kActorController, controller);
    W32(controller + kAiMode, 0);  // the mode that lets the speed decide
    W32(controller + kAiBrainIndex, 0xFFFFFFFF);

    // the animation controller, with the anim set of the character type
    const uint32_t anim = mem::AllocSystem(kAnimSize, 16);
    if (!anim) {
        LOGI("%s", FormatLog("body", {{"what", "create"}, {"reason", "no_memory"}}).c_str());
        return false;
    }
    std::memset(HostPtr(anim), 0, kAnimSize);
    ctx.eax = actor;
    ctx.esi = anim;
    CallGuest(ctx, kAnimAttach, {animset});

    actor_ = actor;
    anim_ = anim;
    controller_ = controller;
    animset_ = animset;
    created_++;
    have_pos_ = false;
    speed_ = 0.0f;
    shot_left_ = 0.0f;
    clip_.clear();
    clip_changes_ = 0;
    LOGI("%s", FormatLog("body", {{"what", "created"},
                                  {"actor", std::to_string(actor)},
                                  {"anim", std::to_string(anim)},
                                  {"controller", std::to_string(controller)},
                                  {"animset", std::to_string(animset)},
                                  {"slot", std::to_string(slot)},
                                  {"threshold", LogNumber(G<float>(0x00295CE4))}})
                   .c_str());
    return true;
}

// The anim set of a character type.
//
// The anim set of a soldier of the level is the one of the level: the body
// walks like the soldiers of the level walk. When there is no soldier yet
// (the level is still coming up) the table's own soldier is used: the anim
// set at table+0xF8+7*0x60, the one whose vtable is 0x28D0B8.
uint32_t Body::FindAnimSet() const {
    const uint32_t manager = R32(layout::coop::kActorManager);
    if (manager) {
        const uint32_t array = R32(manager + kActorListArray);
        uint32_t capacity = R32(manager + kActorCapacity);
        if (capacity > 64) capacity = 64;
        for (uint32_t i = 0; i < capacity; i++) {
            const uint32_t other = R32(array + i * 4);
            if (!other || other == actor_) continue;
            if (R32(other + kActorType) != kTypeSoldier) continue;
            const uint32_t anim = R32(other + kActorAnim);
            if (!anim) continue;
            const uint32_t animset = R32(anim + kAnimSet);
            if (animset) return animset;
        }
    }
    const uint32_t table = R32(kCharacterTable);
    if (!table) return 0;
    return table + kAnimSetBase + kSoldierCharacter * kAnimSetStride;
}

// The partner's state, as the network has it.
//
// The guest's own player is not in the list of the network: the list holds
// the other side. The position of the list is the interpolated one, so the
// speed of the partner does not step with the packets.
const net::PlayerStateData* Body::PartnerState() const {
    static thread_local net::PlayerStateData state;
    const std::vector<net::RemotePlayer> players = net::NetworkManager::Get().GetPlayers();
    const uint8_t local = net::NetworkManager::Get().GetLocalPlayerId();
    for (const net::RemotePlayer& player : players) {
        if (!player.active || !player.has_state) continue;
        if (player.id == local) continue;
        state = player.state;
        // the position the network carries is the eye: the body stands on
        // the floor, under it
        const float height = (state.flags & kFlagCrouching) ? kEyeHeightCrouched : kEyeHeight;
        state.pos[1] -= height;
        return &state;
    }
    return nullptr;
}

// The guest: the body follows the partner.
//
// The position and the direction go in with the engine's own calls, the
// speed goes into the field the anim set's own choice reads, and the record
// says what the body is doing.
void Body::Update() {
    if (!Alive()) {
        // the level came again, or the actor was taken away
        actor_ = 0;
        anim_ = 0;
        controller_ = 0;
        clip_.clear();
        return;
    }
    const net::PlayerStateData* partner = PartnerState();
    if (!partner) return;

    const uint32_t frame = R32(layout::coop::kFrameCount);

    // the speed of the partner, in units a second. The record does not
    // carry it and the clip of the walk is not chosen by the record: the
    // anim set's own choice asks the speed of the actor.
    if (have_pos_ && frame != last_frame_) {
        const float dt = FrameTime();
        const float dx = partner->pos[0] - last_pos_[0];
        const float dy = partner->pos[1] - last_pos_[1];
        const float dz = partner->pos[2] - last_pos_[2];
        const float measured = std::sqrt(dx * dx + dy * dy + dz * dz) / dt;
        // the position of the network comes in packets, so the speed is
        // smoothed: a body that walks does not stop every other frame
        speed_ = 0.5f * speed_ + 0.5f * measured;
        last_pos_[0] = partner->pos[0];
        last_pos_[1] = partner->pos[1];
        last_pos_[2] = partner->pos[2];
        last_frame_ = frame;
    } else if (!have_pos_) {
        last_pos_[0] = partner->pos[0];
        last_pos_[1] = partner->pos[1];
        last_pos_[2] = partner->pos[2];
        last_frame_ = frame;
        have_pos_ = true;
    }
    G<float>(actor_ + kActorSpeed) = speed_;

    // the record: what the body is doing. The shot is the state 7 and it is
    // held for the length of the shot; the aim is the state 13 and the pitch
    // is in the +0x08, in degrees, the way the player writes it.
    uint32_t state = kStateStand;
    if (partner->flags & kFlagAiming) state = kStateAim;
    if (shot_left_ > 0.0f) {
        state = kStateShot;
        shot_left_ = std::max(0.0f, shot_left_ - FrameTime());
    }
    W32(actor_ + kActorRecord + kRecordState, state);
    W32(actor_ + kActorRecord + kRecordP04, 0);
    G<float>(actor_ + kActorRecord + kRecordP08) =
        (state == kStateAim) ? partner->pitch * 180.0f / 3.14159265f : 1.0f;
    W32(actor_ + kActorRecord + kRecordP0C, 0);
    W32(actor_ + kActorRecord + kRecordP10, 0);

    // the transform of the body
    WriteTransform();

    // the clip that plays now. The engine chose it in the tick that ran
    // with the record of the frame before: the log is one frame late.
    const std::string clip = game::coop_anim::ClipOf(actor_);
    if (clip != clip_) {
        clip_ = clip;
        clip_changes_++;
        LOGI("%s", FormatLog("body", {{"what", "clip"},
                                      {"actor", std::to_string(actor_)},
                                      {"clip", clip.empty() ? "none" : clip},
                                      {"state", std::to_string(state)},
                                      {"speed", LogNumber(speed_)}})
                       .c_str());
    }

    // a line every second, so the log says what the body was doing while
    // it was doing nothing: the clip that did not change is the proof that
    // the body stands still when the partner stands still.
    if (++heartbeat_ >= 60) {
        heartbeat_ = 0;
        LOGI("%s", FormatLog("body", {{"what", "alive"},
                                      {"actor", std::to_string(actor_)},
                                      {"clip", clip_.empty() ? "none" : clip_},
                                      {"state", std::to_string(state)},
                                      {"speed", LogNumber(speed_)},
                                      {"x", LogNumber(partner->pos[0])},
                                      {"y", LogNumber(partner->pos[1])},
                                      {"z", LogNumber(partner->pos[2])}})
                       .c_str());
    }
}

// The position and the direction of the body, with the engine's own calls.
//
// The engine's own "set the position" and "set the direction" rebuild the
// spatial structure of the world and the bounding box of the actor, so the
// body is in the world the way the level's actors are.
void Body::WriteTransform() {
    if (!actor_) return;
    const net::PlayerStateData* partner = PartnerState();
    if (!partner) return;

    // the position of the partner: the network carries the eye, and the
    // body stands on the floor, under it
    float position[4] = {partner->pos[0], partner->pos[1], partner->pos[2], 1.0f};
    std::memcpy(HostPtr(Scratch()), position, 16);

    // the direction of the partner: the yaw and the pitch of the state make
    // a world direction, and the basis of the body is the identity, so the
    // world direction is the direction of the body
    const float cos_pitch = std::cos(partner->pitch);
    float direction[4] = {std::sin(partner->yaw) * cos_pitch, std::sin(partner->pitch),
                           std::cos(partner->yaw) * cos_pitch, 0.0f};
    std::memcpy(HostPtr(Scratch() + 0x10), direction, 16);

    X86Context& ctx = CurrentContext();
    ctx.eax = Scratch();
    ctx.ecx = actor_;
    CallGuest(ctx, layout::coop::kSetPosition, {});
    ctx.eax = Scratch() + 0x10;
    ctx.ecx = actor_;
    CallGuest(ctx, layout::coop::kSetDirection, {});
}

// The host: the shot of the local player's weapon.
//
// The magazine goes down when the weapon fires, and that is the only place
// the shot is exact: the flag of the shooting of the state comes from the
// timer of the muzzle flash and is sampled at 30 Hz, so the guest can miss
// a shot with it.
void Body::PublishLocalShot() {
    const uint32_t player = R32(layout::kWorld) + layout::kWorldPlayer;
    if (!player) return;
    const uint32_t weapon = R32(player + kActorWeapon);
    if (!weapon) {
        have_ammo_ = false;
        return;
    }
    const uint32_t inventory = R32(weapon + 0xF4);
    const uint16_t ammo = inventory ? G<uint16_t>(inventory + 0x18) : 0;
    if (!have_ammo_) {
        last_ammo_ = ammo;
        have_ammo_ = true;
        return;
    }
    if (ammo >= last_ammo_) {
        last_ammo_ = ammo;
        return;
    }
    last_ammo_ = ammo;

    // the event of the shot: the origin, the direction and how long the
    // shot is held
    net::coop::Event event;
    event.state.start_epoch = epoch_;
    event.state.event_type = static_cast<uint8_t>(net::coop::EventType::Shot);
    event.state.value = kShotSeconds;
    event.state.extra = ++shot_serial_;
    event.state.pos[0] = G<float>(player + 0x100);
    event.state.pos[1] = G<float>(player + 0x104);
    event.state.pos[2] = G<float>(player + 0x108);
    event.state.dir[0] = G<float>(player + 0xF0);
    event.state.dir[1] = G<float>(player + 0xF4);
    event.state.dir[2] = G<float>(player + 0xF8);
    LOGI("%s", FormatLog("body", {{"what", "publish"},
                                  {"event", "shot"},
                                  {"epoch", std::to_string(epoch_)},
                                  {"seconds", LogNumber(kShotSeconds)},
                                  {"weapon", std::to_string(weapon)}})
                   .c_str());
    if (publisher_) publisher_(event);
}

void Body::LogSummary() {
    LOGI("%s", FormatLog("body", {{"what", "summary"},
                                  {"actor", std::to_string(actor_)},
                                  {"created", std::to_string(created_)},
                                  {"shots", std::to_string(shots_)},
                                  {"clip_changes", std::to_string(clip_changes_)},
                                  {"clip", clip_.empty() ? "none" : clip_},
                                  {"failed", std::to_string(failed_)}})
                   .c_str());
}

}  // namespace coop
}  // namespace game

// The engine's own tick of a living actor, 0x001F0860.
//
// It reads the weapon of the actor, asks the body for the state and hands
// the record to the state switch that picks the clip. The body writes its
// record here, before the switch reads it, so the clip of the frame is the
// clip of the state of the frame.
//
// The body has no weapon, and the tick of an actor without a weapon does not
// touch the record: what the body writes here is what the switch reads
// (LAYOUT.md section 11).
X86_EXTERN_FUNC(__imp__sub_001F0860);
X86_FUNC(sub_001F0860) {
    game::coop::Body::Get().BeforeActorTick(ctx.esi);
    __imp__sub_001F0860(ctx, base);
}
