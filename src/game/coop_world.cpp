#include "game/coop_world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"
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

}  // namespace

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
// Deathmatch code already uses for the player: the yaw turns around the up
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

void World::SetRole(Role role) { role_ = role; }

void World::SetEpoch(uint32_t epoch) {
    if (epoch == epoch_) return;
    epoch_ = epoch;
    order_.Reset(epoch);
}

void World::ResetRound(uint32_t epoch) {
    epoch_ = epoch;
    order_.Reset(epoch);
    counters_ = WorldCounters();
    std::lock_guard<std::mutex> lock(mutex_);
    pending_snapshot_ = net::coop::Snapshot();
    has_pending_snapshot_ = false;
    pending_events_.clear();
    outgoing_events_.clear();
}

// The actors the manager holds. The live list is an array of pointers with a
// fixed number of slots and the slots that are not in use are null, so they
// are skipped; the engine walks the same array the same way.
uint32_t World::EnumerateActors(std::vector<uint32_t>* actors) const {
    actors->clear();
    uint32_t manager = R32(layout::coop::kActorManager);
    if (!manager) return 0;
    uint32_t list = R32(manager + layout::coop::kActorList);
    if (!list) return 0;
    uint32_t capacity = R32(manager + layout::coop::kActorCapacity);
    uint32_t used = R32(manager + layout::coop::kActorCount);
    if (capacity > 64) capacity = 64;
    for (uint32_t i = 0; i < capacity; i++) {
        uint32_t actor = R32(list + i * 4);
        if (!actor) continue;
        if (R32(actor) != layout::coop::kActorVtable) continue;
        // a recycled actor is out of the world: its identity is cleared and
        // the engine does not draw it
        if (R32(actor + layout::coop::kActorState) == kActorRecycled) continue;
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
    out.header.start_epoch = epoch_;
    uint32_t count = 0;
    for (uint32_t actor : actors) {
        if (count >= net::coop::kMaxActorsPerSnapshot) break;
        ReadActorState(actor, &out.actors[count]);
        count++;
    }
    out.header.actor_count = count;
    counters_.snapshots++;
    LOGI("%s", FormatLog("snapshot", {{"epoch", std::to_string(epoch_)},
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
    if (snapshot.header.start_epoch < epoch_) {
        counters_.divergent++;
        LOGI("%s", FormatLog("divergence", {{"reason", "stale_epoch"},
                                             {"epoch", std::to_string(snapshot.header.start_epoch)},
                                             {"current", std::to_string(epoch_)}})
                           .c_str());
        return false;
    }
    if (snapshot.header.start_epoch > epoch_) SetEpoch(snapshot.header.start_epoch);

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
    counters_.unpaired = pairing.unpaired;
    counters_.snapshots++;
    LOGI("%s", FormatLog("pair", {{"epoch", std::to_string(epoch_)},
                                   {"paired", std::to_string(pairing.paired)},
                                   {"unpaired", std::to_string(pairing.unpaired)},
                                   {"missing", std::to_string(pairing.missing)}})
                       .c_str());
    // The two lines say when the pairing changed, not every frame: a log
    // that says the same thing a hundred times hides the one time it
    // changes, and the gate reads this log.
    if (pairing.unpaired != last_unpaired_) {
        last_unpaired_ = pairing.unpaired;
        LOGI("%s", FormatLog("unpaired", {{"side", "guest"},
                                          {"count", std::to_string(pairing.unpaired)},
                                          {"epoch", std::to_string(epoch_)}})
                           .c_str());
    }
    if (pairing.missing != last_missing_) {
        last_missing_ = pairing.missing;
        LOGI("%s", FormatLog("unpaired", {{"side", "host"},
                                          {"count", std::to_string(pairing.missing)},
                                          {"epoch", std::to_string(epoch_)}})
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
                                       {"epoch", std::to_string(epoch_)}})
                           .c_str());
    }
}

// The guest: the host's damage, through the engine's own damage routine. The
// routine runs the whole death chain, so the guest's mission script advances
// the way the host's does.
bool World::ReplayDamage(const net::coop::Event& event) {
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
        // the guest does not have the actor the event is about: it is either
        // already gone, which the death chain explains, or it never was
        // there, which is a divergence
        counters_.skipped++;
        {
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
        LOGI("%s", FormatLog("apply", {{"kind", "damage"},
                                       {"reason", "no_actor"},
                                       {"epoch", std::to_string(state.start_epoch)},
                                       {"seq", std::to_string(state.event_seq)}})
                           .c_str());
        return false;
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
        return false;
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
    if (guest_controller) {
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
    return true;
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
    if (state.event_type != static_cast<uint8_t>(net::coop::EventType::ActorDamaged)) {
        // the other kinds of event are the session's business
        return false;
    }
    if (Current()) {
        // the engine runs on a guest thread, and the routine is the engine's
        return ReplayDamage(event);
    }
    // not on a guest thread: the event waits for the next frame of the guest
    std::lock_guard<std::mutex> lock(mutex_);
    pending_events_.push_back(event);
    return true;
}

// The guest: the actor's own tick ran, so the host's transform goes in now.
// The tick of the guest would overwrite it otherwise (LAYOUT.md §6).
void World::AfterActorTick(uint32_t actor) {
    // The test harness's kill, on the engine's own thread: the damage goes
    // through the engine's routine, so the host publishes it and the guest
    // replays it. It is the host that kills, so it happens before the guest
    // check below.
    if (test_kill_actor_ && actor == test_kill_actor_) {
        const uint32_t target = test_kill_actor_;
        test_kill_actor_ = 0;
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

    // the events that arrived outside the guest thread
    std::vector<net::coop::Event> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events.swap(pending_events_);
    }
    for (const net::coop::Event& event : events) ReplayDamage(event);

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
    test_kill_actor_ = best;
    test_kill_key_a_ = R32(best + layout::coop::kActorKeyA);
    test_kill_key_b_ = R32(best + layout::coop::kActorKeyB);
    test_kill_generation_ = R32(best + layout::coop::kActorGeneration);
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
    event.state.start_epoch = epoch_;
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
    if (publisher_) {
        publisher_(event);
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
    LOGI("%s", FormatLog("summary", {{"epoch", std::to_string(epoch_)},
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
