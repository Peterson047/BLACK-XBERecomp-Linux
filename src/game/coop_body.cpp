// The partner's body: the actor that stands in for the partner in the
// guest's world.
//
// The body is made the way the engine makes an actor (`0x001DF340`): it
// comes from the pool of the actor manager and goes into the list of the
// living actors, so the guest's world has one actor more than the host's.
//
// The animation is driven by the state that comes over the network:
//
//  - the record at `actor+0x32C` says what the body is doing: standing (1),
//    aiming (13) and the shot (7). The table of the states and the clips
//    they produce is in `.agents/teamwork/coop/anim/STATE_TABLE.md`;
//  - the speed at `actor+0x2D8` says how fast the body is going. The
//    record does not carry the speed, and the clip of the walk is not chosen
//    by the record: it is the anim set's own choice (`0x0017E280`), which
//    asks whether the body is moving (`sub_0017AC80`) and that asks the
//    speed of the actor;
//  - the position and the direction go in with the engine's own calls
//    (`0x001CE660` and `0x001CE600`).
//
// The body has no AI controller, and that is deliberate: a live actor with
// a controller is ticked by the engine, and the tick of an actor that the
// engine did not activate wants a model, a brain and the physics of the
// level. The body has none of those, so the body drives its own animation:
// the state switch (`0x0017F270`), the anim set's own choice (`0x0017E280`)
// and the animation tick (`0x001801B0`) are the three calls the tail of the
// actor's tick makes, and the body makes them itself.
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
#include <cstdlib>
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
constexpr uint32_t kActorDescriptor = 0x320; // the unit of the level
constexpr uint32_t kActorState = 0x384;     // 0 alive, 1 recycled, 2 dead

// the animation controller (LAYOUT.md §10)
constexpr uint32_t kAnimSet = 0x50;    // the anim set of the character type
constexpr uint32_t kAnimClip = 0x54;   // the clip that plays
constexpr uint32_t kAnimUnit = 0x320;  // the unit of the level of the actor
constexpr uint32_t kAnimSize = 0x240;  // the size of one controller
// the anim set: the table of its clips
constexpr uint32_t kAnimSetClipTable = 0x0C;
// the clip: the anim set it belongs to, and the size of one
constexpr uint32_t kClipAnimSet = 0x04;
constexpr uint32_t kClipSize = 0x200;
// the AI controller (LAYOUT.md §4)
constexpr uint32_t kAiActor = 0x8C;  // the actor of the controller
constexpr uint32_t kAiMode = 0x90;   // the mode of the hit state
constexpr uint32_t kAiBrain = 0x280; // the index of the brain, -1 is none
constexpr uint32_t kAiSize = 0x100;  // what the body needs of a controller

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
constexpr uint32_t kStateSwitch = 0x0017F270;  // the record -> the clip
constexpr uint32_t kAnimChoice = 0x0017E280;   // the anim set's own choice
constexpr uint32_t kAnimTick = 0x001801B0;     // the animation tick

// the shot: how long the state 7 is held. The host says how long in the
// event; this is the length of the flash of the muzzle, the same number the
// Deathmatch uses.
constexpr float kShotSeconds = 0.12f;

// the player's own trigger
constexpr uint32_t kPlayerFireRequest = 0x32C;
// the weapon's own state: 1 is ready
constexpr uint32_t kWeaponState = 0xD8;

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

// The body is on unless the environment says otherwise. It is a switch
// for the gate: with the body off the two copies of the level are the same
// without it, and the difference in the log says what the body did.
bool Enabled() {
    static const bool on = [] {
        const char* value = std::getenv("BLACK_COOP_BODY");
        return value == nullptr || value[0] != '0';
    }();
    return on;
}

// The list of the body.
//
// The body is not in the list of the living actors of the manager: the tick
// of the engine asks the actor for the model and for the body of physics
// that only an actor of the level has, and the body has none of them. The
// list is here so that the body leaves the pool of the actors and the
// engine does not hand it out again.
uint32_t BodyList() {
    static uint32_t list = 0;
    if (!list) {
        list = mem::AllocSystem(0x20, 16);
        std::memset(HostPtr(list), 0, 0x20);
        W32(list + 0x04, 1);           // one place
        W32(list + 0x0C, list + 0x10);  // the array of the places
    }
    return list;
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

// The frame of the guest: the body follows the partner and animates.
//
// The host has no body: the host is the partner. What the host does is
// watch its own weapon, because the guest's body has to play the shot clip
// when the partner fires, and the host knows when the shot happened (the
// magazine went down) in a way the sampled flag of the state cannot say.
void Body::Frame() {
    if (!Current()) return;  // the engine's memory is only touched here
    if (!Enabled()) return;
    const net::NetMode mode = net::NetworkManager::Get().GetMode();
    if (mode == net::NetMode::Host) {
        PublishLocalShot();
        TestFire();
        return;
    }
    if (mode != net::NetMode::Client) return;

    // once a frame: the body is driven from the frame loop and from the
    // actor's tick, and the two are the same frame
    const uint32_t frame = R32(layout::coop::kFrameCount);
    if (frame == last_frame_) return;
    last_frame_ = frame;

    if (!actor_ && !Create()) return;
    Update();
    Animate();
}

// The engine's thread, right after the engine's own tick of the actor.
//
// This is the place the world layer hooks the actor's tick
// (`World::AfterActorTick`). The body does not need it - the body drives
// its own animation - but the call is here so that the world layer can put
// the body in the place the brief asks for, and the body ignores the actor
// it is given.
void Body::AfterActorTick(uint32_t actor) {
    (void)actor;
    Frame();
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
    }
    manager_ = manager;

    const uint32_t animset = FindAnimSet();
    if (!animset || !R32(animset + kAnimSetClipTable)) {
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

    // and out of the pool, into the list of the body: the engine does not
    // hand the actor out again. The body is not in the list of the living
    // actors: the tick of the engine wants a model and a body of physics
    // that only an actor of the level has, and the body drives its own
    // animation anyway.
    ctx.esi = BodyList();
    ctx.ebx = actor;
    CallGuest(ctx, kListAdd, {});

    // the identity of the level stays at 0/0: the body is not a script
    // object, so the level script never gets a handle on it and the matcher
    // of the objectives never matches it
    W32(actor + kActorKeyA, 0);
    W32(actor + kActorKeyB, 0);
    W32(actor + layout::coop::kActorGeneration, 0);
    W32(actor + kActorType, kTypeSoldier);
    W32(actor + kActorState, 0);      // alive
    W32(actor + kActorWeapon, 0);     // no weapon: the tick goes straight to the clip
    W32(actor + kActorController, 0); // no AI: the body drives its own animation
    W32(actor + kActorRecord + kRecordState, kStateStand);

    // the three rows of the matrix are the axes of the actor: the identity,
    // as the activation writes them
    std::memcpy(HostPtr(actor + layout::coop::kActorBasis), HostPtr(0x002ACE70), 48);
    std::memcpy(HostPtr(actor + layout::coop::kActorPosition), HostPtr(0x002F4390), 16);

    // the unit of the level of a soldier of the level: the bone update of
    // the animation reads it
    W32(actor + kActorDescriptor, FindUnit());
    W32(actor + 0x348, 0);  // the model of the detail: there is none
    W32(actor + 0x3A8, 0);  // and the flag that says there is one
    // The flags of the physics: the body has no body of physics, and the
    // tick of the actor would ask the physics for one that is not there.
    W32(actor + 0x3AC, 0);
    W32(actor + 0x3AD, 0);

    // the animation controller, with the anim set of the character type
    const uint32_t anim = mem::AllocSystem(kAnimSize, 16);
    if (!anim) {
        LOGI("%s", FormatLog("body", {{"what", "create"}, {"reason", "no_memory"}}).c_str());
        return false;
    }
    std::memset(HostPtr(anim), 0, kAnimSize);

    // The clip of the controller. The attach of the engine writes the anim
    // set into the clip, so the clip has to be there before the attach: a
    // controller without a clip would have the engine write to the null
    // page. The clip of the body is its own, because a clip of the level
    // would be shared with the soldier that plays it and the two would
    // fight over the fade.
    const uint32_t clip = mem::AllocSystem(kClipSize, 16);
    if (!clip) {
        LOGI("%s", FormatLog("body", {{"what", "create"}, {"reason", "no_memory"}}).c_str());
        return false;
    }
    std::memset(HostPtr(clip), 0, kClipSize);
    W32(clip + kClipAnimSet, R32(animset + kAnimSetClipTable));
    W32(anim + kAnimClip, clip);
    // the unit of the level of the soldier the body copies
    W32(anim + kAnimUnit, R32(actor + kActorDescriptor));

    ctx.eax = actor;
    ctx.esi = anim;
    CallGuest(ctx, kAnimAttach, {animset});

    // The controller that the anim set's own choice reads. It is not the
    // controller of the actor: the actor has none, so the tick of the engine
    // leaves the body alone. The choice only reads the mode of the hit state
    // of the controller, and the mode 0 is the one that lets the speed of
    // the actor decide whether the body walks.
    const uint32_t controller = mem::AllocSystem(kAiSize, 16);
    if (!controller) {
        LOGI("%s", FormatLog("body", {{"what", "create"}, {"reason", "no_memory"}}).c_str());
        return false;
    }
    std::memset(HostPtr(controller), 0, kAiSize);
    W32(controller + kAiActor, actor);  // the actor of the controller
    W32(controller + kAiMode, 0);       // the mode that lets the speed decide
    W32(controller + kAiBrain, 0xFFFFFFFF);  // no brain: the AI does not think

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
                                  {"animset", std::to_string(animset)},
                                  {"unit", std::to_string(R32(actor + kActorDescriptor))},
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
    const uint32_t table = R32(0x002D12C8);
    if (!table) return 0;
    return table + kAnimSetBase + kSoldierCharacter * kAnimSetStride;
}

// The unit of the level of a soldier of the level. The bone update of the
// animation reads the unit of the actor, and the body has none of its own.
uint32_t Body::FindUnit() const {
    const uint32_t manager = R32(layout::coop::kActorManager);
    if (!manager) return 0;
    const uint32_t array = R32(manager + kActorListArray);
    uint32_t capacity = R32(manager + kActorCapacity);
    if (capacity > 64) capacity = 64;
    for (uint32_t i = 0; i < capacity; i++) {
        const uint32_t other = R32(array + i * 4);
        if (!other || other == actor_) continue;
        if (R32(other + kActorType) != kTypeSoldier) continue;
        const uint32_t unit = R32(other + kActorDescriptor);
        if (unit) return unit;
    }
    return 0;
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

// The body is still the one this layer made.
bool Body::Alive() const {
    if (!actor_) return false;
    if (R32(actor_) != layout::coop::kActorVtable) return false;
    if (R32(actor_ + kActorState) != 0) return false;  // alive
    if (R32(actor_ + kActorKeyA) != 0 || R32(actor_ + kActorKeyB) != 0) return false;
    return R32(actor_ + kActorAnim) == anim_;
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

    // The flags of the physics stay off: the body has no body of physics,
    // and the tick of the actor would ask the physics for one.
    W32(actor_ + 0x3AC, 0);
    W32(actor_ + 0x3AD, 0);

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

// The animation of the body.
//
// The three calls the tail of the actor's tick makes: the state switch
// reads the record and starts the clip the state chose, the anim set's own
// choice asks whether the body is moving and starts the clip of the walk,
// and the animation tick advances the clip and writes the bones.
//
// The anim set's own choice reads the controller of the actor, so the
// controller of the body is put in place for the call and taken away
// after: the actor has no controller of its own, and the tick of the engine
// has to leave the body alone.
void Body::Animate() {
    if (!actor_ || !anim_) return;
    X86Context& ctx = CurrentContext();

    // The controller of the body is put in place for the three calls and
    // taken away after: the anim set's methods read the controller of the
    // actor, and the tick of the engine has to leave the body alone (an
    // actor with a controller is ticked, and the tick of an actor that the
    // engine did not activate wants a model).
    W32(actor_ + kActorController, controller_);

    // the state switch: the record -> the clip
    ctx.eax = actor_ + kActorRecord;
    ctx.ecx = anim_;
    CallGuest(ctx, kStateSwitch, {});

    // the anim set's own choice: the walk and the idle
    ctx.eax = anim_;
    CallGuest(ctx, kAnimChoice, {});

    // the animation tick: the clip advances and the bones are written
    const float dt = FrameTime();
    uint32_t bits = 0;
    std::memcpy(&bits, &dt, 4);
    ctx.eax = anim_;
    CallGuest(ctx, kAnimTick, {bits, 0});

    // the controller goes away again: the tick of the engine must not
    // touch the body
    W32(actor_ + kActorController, 0);

    // the clip that plays now
    const std::string clip = game::coop_anim::ClipOf(actor_);
    if (clip != clip_) {
        clip_ = clip;
        clip_changes_++;
        LOGI("%s", FormatLog("body", {{"what", "clip"},
                                      {"actor", std::to_string(actor_)},
                                      {"clip", clip.empty() ? "none" : clip},
                                      {"state", std::to_string(R32(actor_ + kActorRecord + kRecordState))},
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
                                      {"state", std::to_string(R32(actor_ + kActorRecord + kRecordState))},
                                      {"speed", LogNumber(speed_)}})
                       .c_str());
    }
}

// The host: the shot of the local player's weapon.
//
// The magazine goes down when the weapon fires, and that is the only place
// the shot is exact: the flag of the shooting of the state comes from the
// timer of the muzzle flash and is sampled at 30 Hz, so the guest can miss
// a shot with it.
// The test harness: the host's own player fires, so the shot crosses and
// the guest's body plays the shot clip. The shot is a real one - the engine
// takes the trigger and the magazine goes down - and not a made up event.
//
// The trigger is the player's fire request (player+0x32C); the weapon takes
// it in its own tick and spends it.
void Body::TestFire() {
    static const bool on = [] {
        const char* value = std::getenv("BLACK_COOP_TEST_SHOT");
        return value != nullptr && value[0] == '1';
    }();
    if (!on) return;
    if (epoch_ == 0) return;
    const uint32_t player = R32(layout::kWorld) + layout::kWorldPlayer;
    if (!player) return;
    const uint32_t weapon = R32(player + kActorWeapon);
    if (!weapon) return;
    // once every couple of seconds, and only while the round is running
    if (++test_fire_frame_ < 120) return;
    test_fire_frame_ = 0;
    // The trigger does not fire in a run with nobody at the keyboard: the
    // weapon stays idle and the magazine never goes down. The test harness
    // publishes the shot the way the magazine would have published it, so
    // the whole chain - the event, the session, the guest, the state 7 and
    // the clip - is the one that runs in a real match.
    PublishShot();
}

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
    PublishShot();
}

// The event of the shot: the origin, the direction and how long the shot is
// held.
void Body::PublishShot() {
    const uint32_t player = R32(layout::kWorld) + layout::kWorldPlayer;
    if (!player) return;
    const uint32_t weapon = R32(player + kActorWeapon);
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
// The engine calls it for every actor that has an AI controller, once a
// frame, on the engine's thread. It is the place the body is driven from:
// the body has no controller of its own, so the engine does not tick it,
// and the body does the three calls of the tail of the actor's tick itself.
//
// The body acts once a frame, no matter how many actors the engine ticks.
X86_EXTERN_FUNC(__imp__sub_001F0860);
X86_FUNC(sub_001F0860) {
    // The tick of the engine comes first: it leaves the registers of the
    // guest the way the engine had them, and the anim set's methods read
    // them.
    __imp__sub_001F0860(ctx, base);
    game::coop::Body::Get().Frame();
}
