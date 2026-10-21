#include "game/coop_world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "game/coop_body.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// how deep the guest is inside the replay of a host event. The damage
// routine has to run then; everywhere else on the guest it is cut.
int g_replay_depth = 0;

// The guest's scratch memory: the engine reads 16 byte vectors with movaps,
// so they have to live in the guest's memory and be aligned. The transform
// and the damage keep their own places, so a call inside a call cannot
// overwrite what the outer one is reading.
constexpr uint32_t kScratchDirection = 0x00;   // the direction of the transform
constexpr uint32_t kScratchPosition = 0x10;   // the position of the transform
constexpr uint32_t kScratchHit = 0x20;        // the direction of the damage
constexpr uint32_t kScratchEvent = 0x30;      // the damage event of the engine
constexpr uint32_t kScratchSize = 0x80;

uint32_t Scratch() {
    static uint32_t scratch = mem::AllocSystem(kScratchSize, 16);
    return scratch;
}

// the float the engine's routine takes as a 32 bit word
uint32_t Bits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    return bits;
}

float AsFloat(uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, 4);
    return value;
}

// the state of an actor: 0 alive, 1 recycled, 2 dead (LAYOUT.md §2)
constexpr uint32_t kActorAlive = 0;
constexpr uint32_t kActorRecycled = 1;
constexpr uint32_t kActorDead = 2;

// How many frames a damage waits for the actor it is about. The two copies
// of the level activate the objects of the level at their own pace, so the
// guest's actor can be a few frames behind the host's.
constexpr uint32_t kMaxActorWaits = 240;

}  // namespace

// The numbers in the pure header are the mirror of the ones in layout.h, so
// the pure decisions can be tested without the game. The world layer checks
// that they are the same when it is built.
static_assert(layout::coop::kActorUnitTable == game::coop::kUnitTableOffset, "the unit table moved");
static_assert(layout::coop::kActorUnitStride == game::coop::kUnitStride, "the stride of a unit moved");
static_assert(layout::coop::kActorManagerSize == game::coop::kManagerSize, "the size of the manager moved");

namespace game {
namespace coop {
namespace {

// the world direction of an actor: the engine keeps it at actor+0x20, and it
// is the transform of the actor's own direction (actor+0x60) by the actor's
// matrix, which is what sub_00012600 computes.
void ReadDirection(uint32_t actor, float* direction) {
    for (int i = 0; i < 3; i++) direction[i] = G<float>(actor + layout::coop::kActorDirection + i * 4);
}

// the yaw and the pitch of a world direction. The convention is the one the
// the game already uses for the player: the yaw turns around the up
// axis and the pitch tilts it.
void DirectionToAngles(const float* direction, float* yaw, float* pitch) {
    float length =
        std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    if (length <= 1e-4f) {
        *yaw = 0.0f;
        *pitch = 0.0f;
        return;
    }
    *yaw = std::atan2(direction[0], direction[2]);
    *pitch = std::asin(std::clamp(direction[1] / length, -1.0f, 1.0f));
}

// the world direction of a yaw and a pitch, the way round of the above.
void AnglesToDirection(float yaw, float pitch, float* direction) {
    float cos_pitch = std::cos(pitch);
    direction[0] = std::sin(yaw) * cos_pitch;
    direction[1] = std::sin(pitch);
    direction[2] = std::cos(yaw) * cos_pitch;
}

// The actor's own frame: the matrix's three rows at actor+0x70, +0x80 and
// +0x90 are the actor's axes. The engine turns a direction of the actor's own
// frame into a world direction with the sum of the rows scaled by it, so the
// transpose of that sum is the way back.
void WorldToLocalDirection(uint32_t actor, const float* world, float* local) {
    for (int i = 0; i < 3; i++) {
        local[i] = world[0] * G<float>(actor + layout::coop::kActorBasis + i * 4) +
                   world[1] * G<float>(actor + layout::coop::kActorBasis + 16 + i * 4) +
                   world[2] * G<float>(actor + layout::coop::kActorBasis + 32 + i * 4);
    }
}

}  // namespace

World& World::Get() {
    static World instance;
    return instance;
}

void World::SetRole(Role role) { role_.store(role, std::memory_order_release); }

void World::SetEpoch(uint32_t epoch) {
    if (epoch == epoch_.load(std::memory_order_acquire)) return;
    epoch_.store(epoch, std::memory_order_release);
    order_.Reset(epoch);
}

void World::ResetRound(uint32_t epoch) {
    epoch_.store(epoch, std::memory_order_release);
    order_.Reset(epoch);
    counters_.Reset();
    std::lock_guard<std::mutex> lock(mutex_);
    pending_snapshot_ = net::coop::Snapshot();
    has_pending_snapshot_ = false;
    pending_events_.clear();
    outgoing_events_.clear();
}

// The actors the manager holds. The live list is an array of pointers with a
// fixed number of slots and the slots that are not in use are null, so they
// are skipped; the engine walks the same array the same way.
//
// The body of the partner is in that list too - it has to be, or the engine
// would not draw it - and it is not an actor of the level: it is not in the
// snapshot of the host and it is not paired with anything. It is skipped
// here, so the pairing of the two copies of the level does not see an actor
// that the other side does not have.
uint32_t World::EnumerateActors(std::vector<uint32_t>* actors) const {
    actors->clear();
    uint32_t manager = R32(layout::coop::kActorManager);
    if (!manager) return 0;
    uint32_t list = R32(manager + layout::coop::kActorList);
    if (!list) return 0;
    uint32_t capacity = R32(manager + layout::coop::kActorCapacity);
    uint32_t used = R32(manager + layout::coop::kActorCount);
    if (capacity > 64) capacity = 64;
    const uint32_t body = Body::Get().Actor();
    for (uint32_t i = 0; i < capacity; i++) {
        uint32_t actor = R32(list + i * 4);
        if (!actor) continue;
        if (R32(actor) != layout::coop::kActorVtable) continue;
        // a recycled actor is out of the world: its identity is cleared and
        // the engine does not draw it
        if (R32(actor + layout::coop::kActorState) == kActorRecycled) continue;
        // the body of the partner is not an actor of the level
        if (body && actor == body) continue;
        actors->push_back(actor);
    }
    return used;
}

// The host: what the snapshot says about one actor.
void World::ReadActorState(uint32_t actor, net::coop::ActorState* state) const {
    state->native_a = R32(actor + layout::coop::kActorKeyA);
    state->native_b = R32(actor + layout::coop::kActorKeyB);
    state->generation = R32(actor + layout::coop::kActorGeneration);
    state->type = static_cast<uint16_t>(R32(actor + layout::coop::kActorType));
    for (int i = 0; i < 3; i++) {
        state->pos[i] = G<float>(actor + layout::coop::kActorPosition + i * 4);
    }
    float direction[3] = {0.0f, 0.0f, 0.0f};
    ReadDirection(actor, direction);
    DirectionToAngles(direction, &state->yaw, &state->pitch);
    state->health = G<float>(actor + layout::coop::kActorHealth);
    uint32_t actor_state = R32(actor + layout::coop::kActorState);
    bool dead = actor_state == kActorDead || state->health <= 0.0f;
    state->flags = dead ? net::coop::kActorDead : 0;
}

bool World::BuildSnapshot(net::coop::Snapshot& out) {
    if (!IsHost()) {
        LOGE("%s", FormatLog("error", {{"reason", "not_the_host"}, {"what", "snapshot"}}).c_str());
        return false;
    }
    std::vector<uint32_t> actors;
    uint32_t used = EnumerateActors(&actors);
    out = net::coop::Snapshot();
    out.header.start_epoch = epoch_.load(std::memory_order_acquire);
    uint32_t count = 0;
    for (uint32_t actor : actors) {
        if (count >= net::coop::kMaxActorsPerSnapshot) break;
        ReadActorState(actor, &out.actors[count]);
        count++;
    }
    out.header.actor_count = count;
    counters_.snapshots++;
    LOGI("%s", FormatLog("snapshot", {{"epoch", std::to_string(epoch_.load(std::memory_order_acquire))},
                                       {"actors", std::to_string(count)},
                                       {"used", std::to_string(used)}})
                       .c_str());
    return true;
}

bool World::ApplySnapshot(const net::coop::Snapshot& snapshot) {
    if (!IsGuest()) {
        LOGE("%s", FormatLog("error", {{"reason", "not_the_guest"}, {"what", "snapshot"}}).c_str());
        return false;
    }
    if (snapshot.header.start_epoch < epoch_.load(std::memory_order_acquire)) {
        counters_.divergent++;
        LOGI("%s", FormatLog("divergence", {{"reason", "stale_epoch"},
                                             {"epoch", std::to_string(snapshot.header.start_epoch)},
                                             {"current", std::to_string(epoch_.load(std::memory_order_acquire))}})
                           .c_str());
        return false;
    }
    if (snapshot.header.start_epoch > epoch_.load(std::memory_order_acquire))
        SetEpoch(snapshot.header.start_epoch);

    // the guest's actors and their identities
    std::vector<uint32_t> actors;
    EnumerateActors(&actors);
    std::vector<ActorIdentity> identities;
    identities.reserve(actors.size());
    for (uint32_t actor : actors) {
        ActorIdentity identity;
        identity.native_a = R32(actor + layout::coop::kActorKeyA);
        identity.native_b = R32(actor + layout::coop::kActorKeyB);
        identity.generation = R32(actor + layout::coop::kActorGeneration);
        identity.type = static_cast<uint16_t>(R32(actor + layout::coop::kActorType));
        identity.pos[0] = G<float>(actor + layout::coop::kActorPosition);
        identity.pos[1] = G<float>(actor + layout::coop::kActorPosition + 4);
        identity.pos[2] = G<float>(actor + layout::coop::kActorPosition + 8);
        identities.push_back(identity);
    }
    Pairing pairing = PairActors(identities, snapshot);
    // The counter is the state of this frame, not a running total: the gate
    // reads it to know whether the two sides agree right now.
    //
    // An actor the host published an activation for is not unpaired, even
    // when the snapshot that carries it has not arrived yet: the host said
    // it has that actor, and the word of the activation cannot be late.
    uint32_t unpaired = pairing.unpaired;
    if (unpaired > 0) {
        for (size_t i = 0; i < identities.size(); i++) {
            if (pairing.guest_to_snapshot[i] >= 0) continue;
            if (!HostPublished(identities[i].native_a, identities[i].native_b)) continue;
            unpaired--;
        }
    }
    counters_.unpaired = unpaired;
    counters_.snapshots++;
    LOGI("%s", FormatLog("pair", {{"epoch", std::to_string(epoch_.load(std::memory_order_acquire))},
                                   {"paired", std::to_string(pairing.paired)},
                                   {"unpaired", std::to_string(unpaired)},
                                   {"missing", std::to_string(pairing.missing)}})
                       .c_str());
    // The two lines say when the pairing changed, not every frame: a log
    // that says the same thing a hundred times hides the one time it
    // changes, and the gate reads this log.
    if (unpaired != last_unpaired_) {
        last_unpaired_ = unpaired;
        LOGI("%s", FormatLog("unpaired", {{"side", "guest"},
                                          {"count", std::to_string(pairing.unpaired)},
                                          {"epoch", std::to_string(epoch_.load(std::memory_order_acquire))}})
                           .c_str());
    }
    if (pairing.missing != last_missing_) {
        last_missing_ = pairing.missing;
        LOGI("%s", FormatLog("unpaired", {{"side", "host"},
                                          {"count", std::to_string(pairing.missing)},
                                          {"epoch", std::to_string(epoch_.load(std::memory_order_acquire))}})
                           .c_str());
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_snapshot_ = snapshot;
        has_pending_snapshot_ = true;
    }
    // the transform goes in after the actor's own tick, but the state of the
    // actors that are not ticking is written now, so nothing waits for a tick
    // that does not come
    if (Current()) {
        for (size_t i = 0; i < actors.size(); i++) {
            if (pairing.guest_to_snapshot[i] < 0) continue;
            WriteActorState(actors[i], snapshot.actors[pairing.guest_to_snapshot[i]]);
        }
    }
    LOGI("%s", FormatLog("apply", {{"kind", "snapshot"},
                                   {"epoch", std::to_string(snapshot.header.start_epoch)},
                                   {"actors", std::to_string(snapshot.header.actor_count)}})
                       .c_str());
    return true;
}

// The guest: the host's state onto one actor, through the engine's own
// calls. The engine rebuilds its matrix and its bounding box, and the guest
// gets the same bookkeeping the host has.
void World::WriteActorState(uint32_t actor, const net::coop::ActorState& state) {
    // The engine's own "set the position" writes into the actor's matrix and
    // its box. An actor that is dead or recycled has no business getting
    // one: the slot may already belong to another object, and the engine
    // would write over it.
    const uint32_t alive = R32(actor + layout::coop::kActorState);
    if (alive != kActorAlive) {
        counters_.skipped++;
        return;
    }
    X86Context& ctx = CurrentContext();
    uint32_t scratch = Scratch();

    // the direction the host's actor has, in the guest actor's own frame
    float world[3] = {0.0f, 0.0f, 0.0f};
    AnglesToDirection(state.yaw, state.pitch, world);
    float local[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    WorldToLocalDirection(actor, world, local);
    std::memcpy(HostPtr(scratch + kScratchDirection), local, 16);

    // the position of the host's actor
    float position[4] = {state.pos[0], state.pos[1], state.pos[2], 1.0f};
    std::memcpy(HostPtr(scratch + kScratchPosition), position, 16);

    // the engine's own "set the direction" and "set the position"
    ctx.eax = scratch + kScratchDirection;
    ctx.ecx = actor;
    CallGuest(ctx, layout::coop::kSetDirection, {});
    ctx.eax = scratch + kScratchPosition;
    ctx.ecx = actor;
    CallGuest(ctx, layout::coop::kSetPosition, {});

    // the life of the host's actor: the guest's own damage is cut, so the
    // life only moves with the host's
    uint32_t actor_state = R32(actor + layout::coop::kActorState);
    if (actor_state == kActorAlive && !(state.flags & net::coop::kActorDead)) {
        G<float>(actor + layout::coop::kActorHealth) = state.health;
    } else if (actor_state == kActorAlive && (state.flags & net::coop::kActorDead)) {
        // the host's actor is dead and the guest's is not: the death travels
        // as an event, and the disagreement is worth seeing
        counters_.skipped++;
        LOGI("%s", FormatLog("apply", {{"kind", "health"},
                                       {"reason", "dead_mismatch"},
                                       {"actor", std::to_string(actor)},
                                       {"epoch", std::to_string(epoch_.load(std::memory_order_acquire))}})
                           .c_str());
    }
}

// The guest: the host's damage, through the engine's own damage routine. The
// routine runs the whole death chain, so the guest's mission script advances
// the way the host's does.
ReplayResult World::ReplayDamage(const net::coop::Event& event, uint32_t waits) {
    const net::coop::EventState& state = event.state;

    // the actor the event is about
    uint32_t actor = 0;
    std::vector<uint32_t> actors;
    EnumerateActors(&actors);
    for (uint32_t candidate : actors) {
        if (R32(candidate + layout::coop::kActorKeyA) != state.native_a) continue;
        if (R32(candidate + layout::coop::kActorKeyB) != state.native_b) continue;
        if (R32(candidate + layout::coop::kActorGeneration) != state.generation) continue;
        if (static_cast<uint16_t>(R32(candidate + layout::coop::kActorType)) != state.object_type) continue;
        actor = candidate;
        break;
    }
    if (!actor) {
        // The same script object, activated in another order on the other
        // side: the generation is the manager's activation counter and the
        // two processes do not count in the same order.
        //
        // The pair at actor+0x08 / actor+0x0C is the engine's own key for
        // the script object, so one candidate with that pair and that type
        // is the actor. When there are several, the position tells them
        // apart: the same script object can be activated again, and the
        // second one is somewhere else.
        std::vector<uint32_t> candidates;
        for (uint32_t candidate : actors) {
            if (R32(candidate + layout::coop::kActorKeyA) != state.native_a) continue;
            if (R32(candidate + layout::coop::kActorKeyB) != state.native_b) continue;
            if (static_cast<uint16_t>(R32(candidate + layout::coop::kActorType)) != state.object_type) continue;
            candidates.push_back(candidate);
        }
        if (candidates.size() == 1) {
            actor = candidates.front();
        } else if (candidates.size() > 1) {
            float best_distance = 0.0f;
            for (uint32_t candidate : candidates) {
                const float dx = G<float>(candidate + layout::coop::kActorPosition) - state.pos[0];
                const float dy = G<float>(candidate + layout::coop::kActorPosition + 4) - state.pos[1];
                const float dz = G<float>(candidate + layout::coop::kActorPosition + 8) - state.pos[2];
                const float distance = dx * dx + dy * dy + dz * dz;
                if (!actor || distance < best_distance) {
                    actor = candidate;
                    best_distance = distance;
                }
            }
        }
    }
    if (!actor) {
        // The guest does not have the actor the event is about: it is either
        // already gone, which the death chain explains, or it is not there
        // yet, which the level script of the guest will fix.
        //
        // The line is written once, when the wait starts: the event is
        // tried again in every frame, and a log that says the same thing
        // every frame hides the one thing that matters.
        if (waits == 0) {
            counters_.skipped++;
            std::string keys;
            for (uint32_t candidate : actors) {
                keys += " " + std::to_string(R32(candidate + layout::coop::kActorKeyA)) + "/" +
                        std::to_string(R32(candidate + layout::coop::kActorKeyB)) + "/" +
                        std::to_string(R32(candidate + layout::coop::kActorGeneration)) + "/" +
                        std::to_string(R32(candidate + layout::coop::kActorType));
            }
            LOGI("%s", game::coop::FormatLog("apply", {{"kind", "damage"},
                                                       {"reason", "no_actor"},
                                                       {"epoch", std::to_string(state.start_epoch)},
                                                       {"want", std::to_string(state.native_a) + "/" +
                                                                     std::to_string(state.native_b) + "/" +
                                                                     std::to_string(state.generation) + "/" +
                                                                     std::to_string(state.object_type)},
                                                       {"have", keys}})
                               .c_str());
        }
        // The actor may still come: the two copies of the level activate the
        // objects of the level at their own pace, so the guest's actor can
        // be a frame or two behind the host's.
        return ReplayResult::kNoActor;
    }

    // a dead actor takes no more damage, and the routine is not safe to run
    // twice for the same hit
    uint32_t actor_state = R32(actor + layout::coop::kActorState);
    if (actor_state != kActorAlive) {
        counters_.skipped++;
        LOGI("%s", FormatLog("apply", {{"kind", "damage"},
                                       {"reason", "not_alive"},
                                       {"actor", std::to_string(actor)},
                                       {"state", std::to_string(actor_state)}})
                           .c_str());
        return ReplayResult::kSkipped;
    }

    // the guest's memory for what the routine reads: the direction and the
    // damage event, both 16 byte vectors in the guest's memory
    uint32_t scratch = Scratch();
    float direction[4] = {state.dir[0], state.dir[1], state.dir[2], 0.0f};
    std::memcpy(HostPtr(scratch + kScratchHit), direction, 16);
    uint32_t event_ptr = scratch + kScratchEvent;
    float origin[4] = {state.pos[0], state.pos[1], state.pos[2], 0.0f};
    std::memcpy(HostPtr(event_ptr), origin, 16);
    float damage = state.value;
    float radius = state.radius;
    std::memcpy(HostPtr(event_ptr + layout::coop::kDamageEventDamage), &damage, 4);
    std::memcpy(HostPtr(event_ptr + layout::coop::kDamageEventRadius), &radius, 4);
    W32(event_ptr + layout::coop::kDamageEventType, state.damage_type);
    W32(event_ptr + layout::coop::kDamageEventFlag, state.flags);
    W32(event_ptr + 0x20, 0);  // the countdown of the event
    W32(event_ptr + 0x24, 0);

    // the call: the engine's own damage routine, with the arguments it takes
    X86Context& ctx = CurrentContext();
    const float before = G<float>(actor + layout::coop::kActorHealth);
    const uint32_t controller = R32(actor + layout::coop::kActorController);
    const uint32_t mode = controller ? R32(controller + 0x90) : 0;
    const uint32_t part = controller ? R32(controller + 0xCA8) : 0;
    // The controller's hit state, the way the host had it: the routine
    // reads the body part and skips the damage for some of them.
    const uint32_t guest_controller = R32(actor + layout::coop::kActorController);
    uint32_t saved_mode = 0;
    uint32_t saved_part = 0;
    if (guest_controller) {
        saved_mode = R32(guest_controller + 0x90);
        saved_part = R32(guest_controller + 0xCA8);
        W32(guest_controller + 0x90, state.hit_state & 0xFF);
        W32(guest_controller + 0xCA8, (state.hit_state >> 8) & 0xFF);
    }
    g_replay_depth++;
    ctx.ecx = actor;
    CallGuest(ctx, layout::coop::kDamageMethod,
              {actor + layout::coop::kActorPosition, scratch + kScratchHit, Bits(damage), event_ptr,
               Bits(radius), state.damage_type, state.flags});
    g_replay_depth--;
    // The restore only happens when the actor is still alive. The death
    // chain releases the controller and the physics, and writing to a
    // released controller is a write into freed memory: the host's heap
    // comes apart a moment later, far away from here.
    if (guest_controller && R32(actor + layout::coop::kActorState) == kActorAlive) {
        W32(guest_controller + 0x90, saved_mode);
        W32(guest_controller + 0xCA8, saved_part);
    }
    const float after = G<float>(actor + layout::coop::kActorHealth);
    LOGI("%s", game::coop::FormatLog("damage", {{"side", "guest"},
                                    {"actor", std::to_string(actor)},
                                    {"before", game::coop::LogNumber(before)},
                                    {"after", game::coop::LogNumber(after)},
                                    {"mode", std::to_string(mode)},
                                    {"part", std::to_string(part)}})
                           .c_str());
    counters_.applied++;
    LOGI("%s", FormatLog("apply", {{"kind", "damage"},
                                   {"actor", std::to_string(actor)},
                                   {"key_a", std::to_string(state.native_a)},
                                   {"key_b", std::to_string(state.native_b)},
                                   {"generation", std::to_string(state.generation)},
                                   {"damage", LogNumber(damage)},
                                   // The state word, not the life: a dead actor
                                   // is recycled and its slot says nothing
                                   // about the life any more.
                                   {"state", std::to_string(R32(actor + layout::coop::kActorState))},
                                   {"seq", std::to_string(state.event_seq)}})
                       .c_str());
    return ReplayResult::kApplied;
}

// The guest: the living actor with this identity, or zero.
//
// The pair at actor+0x08 / actor+0x0C is the engine's own key for the script
// object, so two actors with the same pair are the same object of the level.
uint32_t World::FindLiveActor(uint32_t key_a, uint32_t key_b) const {
    if (key_a == 0 && key_b == 0) return 0;  // the player of this process
    std::vector<uint32_t> actors;
    EnumerateActors(&actors);
    for (uint32_t actor : actors) {
        if (R32(actor + layout::coop::kActorKeyA) != key_a) continue;
        if (R32(actor + layout::coop::kActorKeyB) != key_b) continue;
        return actor;
    }
    return 0;
}

// The host: the engine activated an actor, and the guest has to activate its
// own copy of it.
//
// The event carries what the activation needs and what does not travel: the
// identity and the type are already in the event, the descriptor travels as
// its place in the manager's table of units, and the transform of the spawn
// travels as its content, because it lives in the stack of the caller.
void World::SetEventPublisher(EventPublisher publisher) {
    std::lock_guard<std::mutex> lock(mutex_);
    publisher_ = std::move(publisher);
}

void World::PublishActivation(const ActivationCall& call) {
    if (!IsHost() || !call.actor) return;
    // The player of this process is not an object of the level: each side
    // has its own, and the engine gives it no identity.
    if (call.key_a == 0 && call.key_b == 0) return;
    const uint32_t manager = R32(layout::coop::kActorManager);
    if (!manager) return;
    const int32_t offset = static_cast<int32_t>(call.descriptor - manager);
    if (!IsUnitDescriptor(offset)) {
        // The descriptor is not a unit of the level: the guest would have no
        // way of finding its own copy, so the activation is not published.
        // It has to be visible: a silent divergence is the worst outcome.
        counters_.divergent++;
        LOGI("%s", FormatLog("divergence", {{"reason", "descriptor"},
                                             {"epoch", std::to_string(epoch_.load(std::memory_order_acquire))},
                                             {"descriptor", std::to_string(call.descriptor)},
                                             {"manager", std::to_string(manager)}})
                           .c_str());
        return;
    }
    net::coop::Event event;
    event.state.start_epoch = epoch_.load(std::memory_order_acquire);
    event.state.event_type = static_cast<uint8_t>(net::coop::EventType::ActorActivated);
    event.state.native_a = call.key_a;
    event.state.native_b = call.key_b;
    event.state.generation = R32(call.actor + layout::coop::kActorGeneration);
    event.state.object_type = static_cast<uint16_t>(R32(call.actor + layout::coop::kActorType));
    for (int i = 0; i < 3; i++) {
        event.state.pos[i] = G<float>(call.actor + layout::coop::kActorPosition + i * 4);
    }
    event.state.activation.descriptor_offset = offset;
    event.state.activation.third = call.third;
    event.state.activation.arg6 = call.arg6;
    event.state.activation.arg7 = call.arg7;
    event.state.activation.arg8 = call.arg8;
    event.state.activation.arg9 = call.arg9;
    for (int i = 0; i < 4; i++) event.state.activation.transform[i] = call.transform[i];
    counters_.events++;
    LOGI("%s", FormatLog("activate", {{"side", "host"},
                                       {"actor", std::to_string(call.actor)},
                                       {"key_a", std::to_string(call.key_a)},
                                       {"key_b", std::to_string(call.key_b)},
                                       {"generation", std::to_string(event.state.generation)},
                                       {"type", std::to_string(event.state.object_type)},
                                       {"unit", std::to_string(offset)}})
                           .c_str());
    // The event goes out now, from the engine's thread, so it keeps the
    // place of the activation in the order of the events. The damage of the
    // actor that was just activated has to come after it.
    // The publisher is called with a copy of it and with the lock let go:
    // it goes to the session and to the network, and the lock of the world
    // layer is never held while another lock is taken.
    EventPublisher publisher;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        publisher = publisher_;
    }
    if (publisher) {
        publisher(event);
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    outgoing_events_.push_back(event);
}

// The guest: did the host say it has an actor of this object of the level?
bool World::HostPublished(uint32_t key_a, uint32_t key_b) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& published : host_published_) {
        if (published.first == key_a && published.second == key_b) return true;
    }
    return false;
}

void World::NoteHostPublished(uint32_t key_a, uint32_t key_b) {
    std::lock_guard<std::mutex> lock(mutex_);
    NoteHostPublishedLocked(key_a, key_b);
}

void World::NoteHostPublishedLocked(uint32_t key_a, uint32_t key_b) {
    for (const auto& published : host_published_) {
        if (published.first == key_a && published.second == key_b) return;
    }
    host_published_.emplace_back(key_a, key_b);
}

// The guest: what the host published about this object of the level.
// The list of the activations is under the lock: the guest's own script
// activates the objects of the level on the engine's thread and the word of
// the host arrives on the thread that draws. The pointer is only good while
// the lock is held, so the caller keeps it.
World::HostActivation* World::FindHostActivation(uint32_t key_a, uint32_t key_b) {
    std::lock_guard<std::mutex> lock(mutex_);
    return FindHostActivationLocked(key_a, key_b);
}

World::HostActivation* World::FindHostActivationLocked(uint32_t key_a, uint32_t key_b) {
    for (HostActivation& known : host_activations_) {
        if (known.key_a == key_a && known.key_b == key_b) return &known;
    }
    return nullptr;
}

// The guest: the host activated an actor, and the guest has to have it too.
//
// The guest's own level script runs the whole level, so it activates the
// objects of the level on its own - but it does not do it at the same time as
// the host, because the level script reacts to where the player is and the
// two players are in different places.
//
// The word of the host is what tells the guest which generation the object
// of the level has. The guest's engine gives the actor the generation of its
// own counter, and the two counters do not count in the same order, so the
// guest takes the host's number: the host is the authority of the world,
// and the two sides have to agree on which actor is which.
bool World::ApplyActivation(const net::coop::Event& event) {
    if (!IsGuest()) return false;
    const net::coop::EventState& state = event.state;
    const net::coop::ActivationState& activation = state.activation;

    // The list of the activations of the host is under the lock: the guest's
    // own script activates the objects of the level on the engine's thread
    // and the word of the host arrives on the thread that draws. Two threads
    // growing the same vector would corrupt the heap, and the corruption
    // would show up far away from here.
    uint32_t existing = 0;
    ActivationAction action = ActivationAction::kWait;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The host has an actor of this object of the level: the guest knows
        // it now, whatever the snapshot says.
        NoteHostPublishedLocked(state.native_a, state.native_b);
        HostActivation* known = FindHostActivationLocked(state.native_a, state.native_b);
        if (!known) {
            known = &host_activations_.emplace_back();
            known->key_a = state.native_a;
            known->key_b = state.native_b;
        }
        // The guest's own level script was ahead of the host: it activated
        // the object before the host's word came, and its actor takes the
        // host's generation now, so the two sides agree on which actor is
        // which.
        existing = FindLiveActor(state.native_a, state.native_b);
        action = DecideActivation(state, known->ledger, existing != 0);
        if (action == ActivationAction::kAdopt) {
            known->ledger.own--;
            W32(existing + layout::coop::kActorGeneration, state.generation);
        } else if (action == ActivationAction::kWait) {
            known->ledger.generations.push_back(state.generation);
        }
    }
    if (action == ActivationAction::kReject) {
        // The descriptor is not a unit of the level: the guest has no way of
        // finding its own copy of it.
        counters_.skipped++;
        LOGI("%s", FormatLog("apply", {{"kind", "activation"},
                                       {"reason", "descriptor"},
                                       {"epoch", std::to_string(state.start_epoch)},
                                       {"unit", std::to_string(activation.descriptor_offset)}})
                           .c_str());
        return false;
    }
    if (action == ActivationAction::kAdopt) {
        counters_.adopted++;
        LOGI("%s", FormatLog("apply", {{"kind", "activation"},
                                       {"reason", "adopted"},
                                       {"actor", std::to_string(existing)},
                                       {"key_a", std::to_string(state.native_a)},
                                       {"key_b", std::to_string(state.native_b)},
                                       {"generation", std::to_string(state.generation)}})
                           .c_str());
        return true;
    }
    // The guest does not have the actor yet: its own level script will
    // activate it. The host's generation is kept, and the actor the guest's
    // script makes takes it.
    LOGI("%s", FormatLog("apply", {{"kind", "activation"},
                                   {"reason", "waiting"},
                                   {"key_a", std::to_string(state.native_a)},
                                   {"key_b", std::to_string(state.native_b)},
                                   {"generation", std::to_string(state.generation)},
                                   {"unit", std::to_string(activation.descriptor_offset)}})
                       .c_str());
    return true;
}

// The guest: the engine's own level script activated an actor.
//
// The guest's script activates the objects of the level on its own. When it
// activates one the host has already activated, the two copies would have
// two actors of the same object of the level, and the engine matches objects
// with the pair of the identity, so the two would fight over the same key.
//
// The engine's own actor wins: it is the one the script knows about. It
// takes the generation the host gave, so the two sides still agree on which
// actor is which.
void World::AfterGuestActivation(uint32_t actor, uint32_t key_a, uint32_t key_b) {
    if (!IsGuest() || !actor) return;
    if (key_a == 0 && key_b == 0) return;  // the player of this process
    // The list of the activations of the host is under the lock: the word of
    // the host arrives on the thread that draws and the guest's own script
    // takes a generation out of it on the engine's thread.
    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        HostActivation* known = FindHostActivationLocked(key_a, key_b);
        if (!known) return;
        // The generation of the host that goes with this actor: the queue is
        // in the order the host made them, and the guest's script activates
        // the objects in the same order.
        if (!TakeGeneration(&known->ledger, &generation)) {
            // The host has not activated this object yet: the guest's actor
            // keeps the generation of the guest's own counter.
            return;
        }
    }
    W32(actor + layout::coop::kActorGeneration, generation);
    counters_.adopted++;
    LOGI("%s", FormatLog("apply", {{"kind", "activation"},
                                   {"reason", "took"},
                                   {"actor", std::to_string(actor)},
                                   {"key_a", std::to_string(key_a)},
                                   {"key_b", std::to_string(key_b)},
                                   {"generation", std::to_string(generation)}})
                       .c_str());
}

bool World::ApplyEvent(const net::coop::Event& event) {
    if (!IsGuest()) {
        LOGE("%s", FormatLog("error", {{"reason", "not_the_guest"}, {"what", "event"}}).c_str());
        return false;
    }
    const net::coop::EventState& state = event.state;

    // the order: a packet of a round that is over, or one that was already
    // applied, is dropped. The routine is not safe to run twice.
    EventOrderResult result = order_.Accept(state.start_epoch, state.event_seq);
    if (result != EventOrderResult::kAccepted) {
        counters_.divergent++;
        LOGI("%s", FormatLog("divergence",
                             {{"reason", result == EventOrderResult::kStaleEpoch ? "stale_epoch" : "out_of_order"},
                              {"epoch", std::to_string(state.start_epoch)},
                              {"seq", std::to_string(state.event_seq)}})
                           .c_str());
        return false;
    }
    // the event counts on both sides, so the two summaries agree
    counters_.events++;
    if (state.event_type == static_cast<uint8_t>(net::coop::EventType::ActorActivated)) {
        // The activation of an actor: it is the engine's memory, so it waits
        // for the engine's thread.
        if (Current()) return ApplyActivation(event);
        std::lock_guard<std::mutex> lock(mutex_);
        pending_events_.push_back(PendingEvent{event, 0});
        return true;
    }
    if (state.event_type != static_cast<uint8_t>(net::coop::EventType::ActorDamaged)) {
        // the other kinds of event are the session's business
        return false;
    }
    if (Current()) {
        // the engine runs on a guest thread, and the routine is the engine's
        return ReplayDamage(event, 0) == ReplayResult::kApplied;
    }
    // not on a guest thread: the event waits for the next frame of the guest
    std::lock_guard<std::mutex> lock(mutex_);
    pending_events_.push_back(PendingEvent{event, 0});
    return true;
}

// The guest: the actor's own tick ran, so the host's transform goes in now.
// The tick of the guest would overwrite it otherwise (LAYOUT.md §6).
void World::AfterActorTick(uint32_t actor) {
    // The test harness's kill, on the engine's own thread: the damage goes
    // through the engine's routine, so the host publishes it and the guest
    // replays it. It is the host that kills, so it happens before the guest
    // check below.
    // The kill of the test harness: the thread that draws arms it and the
    // thread that runs the engine fires it, so the actor is taken under the
    // lock and the arm is taken with it.
    uint32_t target = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (test_kill_actor_ && actor == test_kill_actor_) {
            target = test_kill_actor_;
            test_kill_actor_ = 0;
        }
    }
    if (target) {
        X86Context& kill_ctx = CurrentContext();
        uint32_t scratch = Scratch();
        float direction[4] = {0.0f, 0.0f, 1.0f, 0.0f};
        std::memcpy(HostPtr(scratch + kScratchHit), direction, 16);
        uint32_t event_ptr = scratch + kScratchEvent;
        float origin[4] = {G<float>(target + layout::coop::kActorPosition),
                           G<float>(target + layout::coop::kActorPosition + 4),
                           G<float>(target + layout::coop::kActorPosition + 8), 0.0f};
        std::memcpy(HostPtr(event_ptr), origin, 16);
        float damage = 1000.0f;
        float radius = 1.0f;
        std::memcpy(HostPtr(event_ptr + layout::coop::kDamageEventDamage), &damage, 4);
        std::memcpy(HostPtr(event_ptr + layout::coop::kDamageEventRadius), &radius, 4);
        W32(event_ptr + layout::coop::kDamageEventType, 1);
        W32(event_ptr + layout::coop::kDamageEventFlag, 0);
        W32(event_ptr + 0x20, 0);
        kill_ctx.ecx = target;
        CallGuest(kill_ctx, layout::coop::kDamageMethod,
                  {target + layout::coop::kActorPosition, scratch + kScratchHit, Bits(damage), event_ptr, Bits(radius),
                   1, 0});
        LOGI("%s", FormatLog("test", {{"what", "kill"},
                                       {"reason", "applied"},
                                       {"actor", std::to_string(target)},
                                       {"key_a", std::to_string(test_kill_key_a_)},
                                       {"key_b", std::to_string(test_kill_key_b_)},
                                       {"generation", std::to_string(test_kill_generation_)}})
                           .c_str());
    }

    if (!IsGuest()) return;

    // The events that arrived outside the guest thread.
    //
    // The tick of the actor runs once per actor, and the events are applied
    // once per frame: a damage that waits for its actor is tried again in
    // the next frame, not in the next actor.
    std::vector<PendingEvent> events;
    const uint32_t frame = R32(layout::coop::kFrameCount);
    if (frame != last_event_frame_) {
        last_event_frame_ = frame;
        std::lock_guard<std::mutex> lock(mutex_);
        events.swap(pending_events_);
    }
    std::vector<PendingEvent> waiting;
    for (PendingEvent& item : events) {
        if (item.event.state.event_type == static_cast<uint8_t>(net::coop::EventType::ActorActivated)) {
            // The activation comes before the damage of the actor it
            // activated, or the guest would have no actor to damage.
            ApplyActivation(item.event);
            continue;
        }
        if (ReplayDamage(item.event, item.waits) == ReplayResult::kNoActor && item.waits < kMaxActorWaits) {
            // The actor of the damage is not in the world yet: the guest's
            // own level script activates the objects of the level at its own
            // pace, so the actor comes a frame or two later. The damage
            // waits for it.
            item.waits++;
            waiting.push_back(item);
        }
    }
    if (!waiting.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const PendingEvent& item : waiting) pending_events_.push_back(item);
    }

    // the host's state of this actor
    net::coop::ActorState state;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (has_pending_snapshot_) {
            ActorIdentity identity;
            identity.native_a = R32(actor + layout::coop::kActorKeyA);
            identity.native_b = R32(actor + layout::coop::kActorKeyB);
            identity.generation = R32(actor + layout::coop::kActorGeneration);
            identity.type = static_cast<uint16_t>(R32(actor + layout::coop::kActorType));
            identity.pos[0] = G<float>(actor + layout::coop::kActorPosition);
            identity.pos[1] = G<float>(actor + layout::coop::kActorPosition + 4);
            identity.pos[2] = G<float>(actor + layout::coop::kActorPosition + 8);
            int index = FindSnapshotActor(pending_snapshot_, identity);
            if (index >= 0) {
                state = pending_snapshot_.actors[index];
                have = true;
            } else {
                // The same script object, activated in another order on the
                // other side: the position tells them apart.
                const auto& actors = pending_snapshot_.actors;
                const uint32_t count = pending_snapshot_.header.actor_count > net::coop::kMaxActorsPerSnapshot
                                           ? net::coop::kMaxActorsPerSnapshot
                                           : pending_snapshot_.header.actor_count;
                float best = 0.0f;
                for (uint32_t j = 0; j < count; j++) {
                    if (actors[j].native_a != identity.native_a) continue;
                    if (actors[j].native_b != identity.native_b) continue;
                    if (actors[j].type != identity.type) continue;
                    const float distance = Distance(identity.pos, actors[j].pos);
                    if (!have || distance < best) {
                        best = distance;
                        index = static_cast<int>(j);
                        state = actors[j];
                        have = true;
                    }
                }
            }
        }
    }
    if (have) WriteActorState(actor, state);
}

// The test harness: the host picks the living actor that is nearest to the
// player and kills it, through the engine's own damage routine. The damage
// is then published like any other damage, and the guest replays it, so the
// death chain runs on both sides.
bool World::ArmTestKill() {
    if (!IsHost()) return false;
    std::vector<uint32_t> actors;
    EnumerateActors(&actors);
    uint32_t best = 0;
    uint32_t best_generation = 0xFFFFFFFF;
    for (uint32_t actor : actors) {
        if (R32(actor + layout::coop::kActorState) != kActorAlive) continue;
        // An actor with a life: a prop has a life field full of rubbish.
        const float life = G<float>(actor + layout::coop::kActorHealth);
        if (!(life > 0.0f) || life > 1000.0f) continue;
        // And an actor the engine gave an identity: the pair at actor+0x08
        // and actor+0x0C is the key it matches objects with, and an actor
        // without one is not a script object.
        const uint32_t key_a = R32(actor + layout::coop::kActorKeyA);
        const uint32_t key_b = R32(actor + layout::coop::kActorKeyB);
        if (key_a == 0 && key_b == 0) continue;
        // The oldest activation: the two copies of the level bring their
        // actors in at their own pace, and the first ones are the ones
        // both have.
        const uint32_t generation = R32(actor + layout::coop::kActorGeneration);
        if (best == 0 || generation < best_generation) {
            best = actor;
            best_generation = generation;
        }
    }
    // The level spawns its actors while it comes up, so there may be none
    // yet. The caller asks again later.
    if (!best) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        test_kill_actor_ = best;
        test_kill_key_a_ = R32(best + layout::coop::kActorKeyA);
        test_kill_key_b_ = R32(best + layout::coop::kActorKeyB);
        test_kill_generation_ = R32(best + layout::coop::kActorGeneration);
    }
    LOGI("%s", FormatLog("test", {{"what", "kill"},
                                   {"reason", "armed"},
                                   {"actor", std::to_string(best)},
                                   {"key_a", std::to_string(test_kill_key_a_)},
                                   {"key_b", std::to_string(test_kill_key_b_)},
                                   {"generation", std::to_string(test_kill_generation_)}})
                       .c_str());
    return true;
}

void World::PublishDamage(const DamageCall& call) {
    if (!IsHost() || !call.actor) return;
    net::coop::Event event;
    event.state.start_epoch = epoch_.load(std::memory_order_acquire);
    event.state.event_type = static_cast<uint8_t>(net::coop::EventType::ActorDamaged);
    event.state.damage_type = static_cast<uint8_t>(call.type);
    event.state.native_a = call.native_a;
    event.state.native_b = call.native_b;
    event.state.generation = call.generation;
    event.state.object_type = static_cast<uint16_t>(call.actor_type);
    event.state.value = call.damage;
    event.state.radius = call.radius;
    for (int i = 0; i < 3; i++) {
        event.state.pos[i] = call.origin[i];
        event.state.dir[i] = call.direction[i];
    }
    event.state.flags = static_cast<uint16_t>(call.flag);
    event.state.hit_state = call.hit_state;
    counters_.events++;
    LOGI("%s", FormatLog("event", {{"kind", "damage"},
                                   {"actor", std::to_string(call.actor)},
                                   {"damage", LogNumber(call.damage)},
                                   {"radius", LogNumber(call.radius)},
                                   {"type", std::to_string(call.type)}})
                       .c_str());
    // The publisher is called with a copy of it and with the lock let go:
    // it goes to the session and to the network, and the lock of the world
    // layer is never held while another lock is taken.
    EventPublisher publisher;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        publisher = publisher_;
    }
    if (publisher) {
        publisher(event);
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    outgoing_events_.push_back(event);
}

std::vector<net::coop::Event> World::TakePendingEvents() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<net::coop::Event> events;
    events.swap(outgoing_events_);
    return events;
}

void World::LogSummary() {
    LOGI("%s", FormatLog("summary", {{"epoch", std::to_string(epoch_.load(std::memory_order_acquire))},
                                      {"snapshots", std::to_string(counters_.snapshots)},
                                      {"events", std::to_string(counters_.events)},
                                      {"unpaired", std::to_string(counters_.unpaired)},
                                      {"divergent", std::to_string(counters_.divergent)}})
                       .c_str());
}

}  // namespace coop
}  // namespace game

// The engine's own damage routine, 0x001DF830.
//
// The host publishes the damage it applies; the guest does not publish and
// does not apply its own: the only damage that counts on the guest is the one
// the host replicated. The replay of a host event is let through, so the
// engine runs its death chain on the guest as it does on the host.
namespace {
void CaptureDamageCall(X86Context& ctx, game::coop::DamageCall* call) {
    call->actor = ctx.ecx;
    // The identity, before the call. The death clears the pair at actor+0x08
    // and actor+0x0C, so an event published after the call would carry an
    // identity of zero and the guest would find nothing.
    call->native_a = R32(ctx.ecx + layout::coop::kActorKeyA);
    call->native_b = R32(ctx.ecx + layout::coop::kActorKeyB);
    call->generation = R32(ctx.ecx + layout::coop::kActorGeneration);
    call->actor_type = R32(ctx.ecx + layout::coop::kActorType);
    uint32_t direction = R32(ctx.esp + 8);
    uint32_t event = R32(ctx.esp + 16);
    call->damage = AsFloat(R32(ctx.esp + 12));
    call->radius = AsFloat(R32(ctx.esp + 20));
    call->type = R32(ctx.esp + 24);
    call->flag = R32(ctx.esp + 28);
    if (direction) {
        for (int i = 0; i < 3; i++) call->direction[i] = G<float>(direction + i * 4);
    }
    if (event) {
        // the damage event the routine was given: its first vector is where
        // the hit landed
        for (int i = 0; i < 3; i++) {
            call->origin[i] = G<float>(event + layout::coop::kDamageEventOrigin + i * 4);
        }
    }
    // The controller's hit state. The routine reads the body part and skips
    // the damage for some of them, so the guest has to have the same one.
    const uint32_t controller = R32(call->actor + layout::coop::kActorController);
    if (controller) {
        const uint32_t mode = R32(controller + 0x90);
        const uint32_t part = R32(controller + 0xCA8);
        call->hit_state = (mode & 0xFF) | ((part & 0xFF) << 8);
    }
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_001DF830);
X86_FUNC(sub_001DF830) {
    game::coop::World& world = game::coop::World::Get();
    if (world.IsGuest()) {
        // the guest's own damage is cut: only the damage the host replicated
        // counts. The replay of a host event is let through.
        if (g_replay_depth > 0) {
            __imp__sub_001DF830(ctx, base);
            return;
        }
        // the routine's "ret 0x1C": the caller pops the seven arguments
        ctx.esp += 32;
        return;
    }
    if (world.IsHost()) {
        game::coop::DamageCall call;
        CaptureDamageCall(ctx, &call);
        const float before = G<float>(call.actor + layout::coop::kActorHealth);
        const uint32_t controller = R32(call.actor + layout::coop::kActorController);
        const uint32_t mode = controller ? R32(controller + 0x90) : 0;
        const uint32_t part = controller ? R32(controller + 0xCA8) : 0;
        __imp__sub_001DF830(ctx, base);
        const float after = G<float>(call.actor + layout::coop::kActorHealth);
        LOGI("%s", game::coop::FormatLog("damage", {{"side", "host"},
                                        {"actor", std::to_string(call.actor)},
                                        {"before", game::coop::LogNumber(before)},
                                        {"after", game::coop::LogNumber(after)},
                                        {"mode", std::to_string(mode)},
                                        {"part", std::to_string(part)}})
                                   .c_str());
        if (before != after) world.PublishDamage(call);
        return;
    }
    __imp__sub_001DF830(ctx, base);
}

// The engine's own actor tick, 0x001F08D0. The guest writes the host's
// transform right after the tick of the actor, because the tick would
// overwrite it (LAYOUT.md §6).
X86_EXTERN_FUNC(__imp__sub_001F08D0);
X86_FUNC(sub_001F08D0) {
    uint32_t actor = ctx.ecx;
    __imp__sub_001F08D0(ctx, base);
    game::coop::World::Get().AfterActorTick(actor);
}

// The engine's own activation, 0x001DA4F0.
//
// The host publishes the activation: the guest's level script does not
// activate the same objects at the same time, so the guest has to be told.
// The guest's own script activation is left alone - the engine's own actor
// wins over the copy the host made, and the copy is dropped.
X86_EXTERN_FUNC(__imp__sub_001DA4F0);
X86_FUNC(sub_001DA4F0) {
    game::coop::World& world = game::coop::World::Get();
    // The arguments, before the call: the routine pops them, so they are
    // gone after it.
    const uint32_t actor = R32(ctx.esp + 4);
    const uint32_t key_a = R32(ctx.esp + 0x10);
    const uint32_t key_b = R32(ctx.esp + 0x14);
    if (world.IsGuest()) {
        __imp__sub_001DA4F0(ctx, base);
        world.AfterGuestActivation(actor, key_a, key_b);
        return;
    }
    if (world.IsHost()) {
        game::coop::ActivationCall call;
        call.actor = actor;
        call.descriptor = R32(ctx.esp + 8);
        call.third = ctx.eax;
        call.key_a = key_a;
        call.key_b = key_b;
        call.arg6 = R32(ctx.esp + 0x18);
        call.arg7 = R32(ctx.esp + 0x1C);
        call.arg8 = R32(ctx.esp + 0x20);
        call.arg9 = R32(ctx.esp + 0x24);
        const uint32_t transform = R32(ctx.esp + 0xC);
        for (int i = 0; i < 4; i++) {
            call.transform[i] = transform ? G<float>(transform + i * 4) : 0.0f;
        }
        __imp__sub_001DA4F0(ctx, base);
        world.PublishActivation(call);
        return;
    }
    __imp__sub_001DA4F0(ctx, base);
}
