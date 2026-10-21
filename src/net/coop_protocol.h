#pragma once

// The co-op wire format.
//
// The host owns the world and the guest reacts to it, so every message in
// this file travels host -> guest, except the ready and the acknowledgement,
// which travel the other way. The design is in .agents/teamwork/coop/PROTOCOL.md.
//
// Nothing here touches the game. The encoding is a set of free functions over
// plain bytes, so the whole protocol can be exercised by a standalone test
// without the game's assets.

#include <cstddef>
#include <cstdint>

#include "net/network.h"

namespace net {
namespace coop {

// The snapshot carries at most this many actors per datagram. 24 actors of
// 36 bytes plus the headers stay inside the 1200 byte path that survives a
// NAT, and the level uses fewer than ten.
constexpr size_t kMaxActorsPerSnapshot = 24;

constexpr size_t kActorStateSize = 40;
constexpr size_t kSnapshotHeaderSize = 12;
constexpr size_t kMaxSnapshotBytes = 12 + kSnapshotHeaderSize + kMaxActorsPerSnapshot * kActorStateSize;

// The state of the session. The host is the only one that changes it.
enum class Phase : uint8_t {
    Idle = 0,       // nothing happened yet
    Lobby = 1,      // the peers are in the room, nobody has loaded
    Loading = 2,    // the host named the mission, the processes are loading it
    InMission = 3,  // the mission started on both sides
    Ended = 4,      // the mission is over
};

// What the host tells the guest about the world.
enum class EventType : uint8_t {
    ActorDamaged = 0,
    ActorDied = 1,
    ObjectInteracted = 2,
    ItemPicked = 3,
    ObjectiveChanged = 4,
    MissionEnded = 5,
    NamedEvent = 6,  // one of the level script's own named events
    Destruction = 7,
    // The host activated an actor. The two copies of the level do not
    // activate the same objects at the same time - the level script reacts
    // to where the player is, and the two players are in different places -
    // so the guest has to be told, or the two worlds are not the same world.
    ActorActivated = 8,
    // The host fired a shot. The shooting flag of the player's state is a
    // level trigger: it comes from the timer of the muzzle flash and it is
    // sampled at 30 Hz, so the guest can miss a shot. The host knows when
    // the shot happened - the magazine went down - so the host publishes the
    // shot as an event and the guest puts the partner's body in the state 7
    // (the shot of the animation record) for the length of the shot.
    // The number is appended: the numbers are mirrored in tests/e2e.
    Shot = 9,
};

enum ActorFlags : uint16_t {
    kActorDead = 1 << 0,
    kActorCrouching = 1 << 1,
    kActorFiring = 1 << 2,
    kActorAiming = 1 << 3,
};

#pragma pack(push, 1)

// What the guest needs to reproduce an activation of the host.
//
// The activation is the engine's own 0x001DA4F0(actor, descriptor, third,
// ...): the actor is the one the engine gives, the descriptor is the unit of
// the level the actor comes from, and the third is the value the routine
// takes in a register.
//
// The descriptor is not an address that travels: the two processes run the
// same level and the manager's table of units is in the same place in the
// two, so what travels is the place in that table. The transform is not an
// address either - it lives in the stack of the caller - so what travels is
// its content: the position of the spawn and the angle of it.
//
// The identity (the pair and the generation) and the type are already in the
// event, and the activation writes them itself, so they do not travel twice.
struct ActivationState {
    int32_t descriptor_offset;  // the descriptor's place in the manager's table of units
    uint32_t third;              // the value the activation takes in the register
    uint32_t arg6;               // the sixth argument of the activation
    uint32_t arg7;
    uint32_t arg8;
    uint32_t arg9;
    float transform[4];          // the spawn: x, y, z and the angle
};

// One actor of the world, as the host sees it.
//
// The identity is the native pair the engine gives an actor, plus its
// generation. The pair at actor+8 / actor+0xC is the engine's own object key
// — the engine matches objects with it — and the generation at actor+0x374
// comes from the manager's activation counter at manager+0x8524, which the
// engine bumps once per activation. Two processes that run the same level
// script activate the same objects in the same order, so both the pair and
// the generation agree on their own.
//
// The generation is not a local counter and it is not optional: the death
// clears the pair, and the same script object can be activated again later.
struct ActorState {
    uint32_t native_a;  // actor+8
    uint32_t native_b;  // actor+C
    uint32_t generation;  // actor+0x374
    uint16_t type;  // actor+0xC4
    uint16_t flags;  // ActorFlags
    float pos[3];
    float yaw;
    float pitch;
    float health;  // actor+0x2F0
};

struct SnapshotHeader {
    uint32_t start_epoch;  // the round; a packet from an older round is dropped
    uint32_t actor_count;
    uint8_t part_index;
    uint8_t part_count;
    uint16_t reserved;
};

struct RoomState {
    uint32_t session_id;
    uint32_t revision;  // monotonic; the guest drops an older room
    uint32_t start_epoch;
    uint8_t mission_id;  // 0 is Veblensk
    uint8_t stage_id;
    uint8_t phase;  // Phase
    uint8_t present_mask;
    uint8_t ready_mask;
    uint8_t loaded_mask;
    uint8_t reserved;
};

// One event of the world, as the host tells it to the guest.
//
// The same struct carries the two kinds of event the level needs:
// - the damage of an actor, with the exact arguments the engine's own damage
//   call takes, so the guest replays the call and not a number;
// - one of the level script's own named events, with its id and its two
//   strings, so the guest fires it locally and its objective matcher advances
//   exactly like the host's.
//
// The objective table itself never crosses the wire: both processes load the
// same LevelDat.bin, so only the events have to be the same.
struct EventState {
    uint32_t start_epoch;
    uint32_t event_seq;  // monotonic inside the round; a repeated one is dropped
    uint8_t event_type;  // EventType
    uint8_t damage_type; // 1, 2 or 3, the type the engine used
    uint16_t object_type;
    uint32_t native_a;
    uint32_t native_b;
    uint32_t generation;  // the generation of the actor the event is about
    float value;          // the damage
    float radius;         // the radius of the damage call
    float pos[3];         // the impact point in world space
    float dir[3];         // the direction the damage came from
    uint16_t flags;       // the engine's seventh argument
    uint16_t reserved;
    // The controller's hit state: the mode in the low byte and the body
    // part in the next. The damage routine skips the damage when the body
    // part is one of 29, 30 and 31, so the guest has to have the same
    // state the host had, or the replay would not do what the host did.
    uint32_t hit_state;
    uint32_t extra;       // who caused it, or a counter
    uint32_t event_id;    // the named event's id
    char text[2][96];     // the named event's two strings
    // What an activation needs, when the event is one. It is at the end so
    // the places of the fields above do not move.
    ActivationState activation;
};

struct ReadyState {
    uint32_t start_epoch;
    uint8_t ready;
    uint8_t loaded;
    uint16_t reserved;
};

struct AckState {
    uint32_t start_epoch;
    uint32_t event_seq;  // the last event the guest applied
};

#pragma pack(pop)

// A snapshot of the world, with the actors in it.
struct Snapshot {
    SnapshotHeader header{};
    ActorState actors[kMaxActorsPerSnapshot]{};
};

// The guest's answer to the room: is it ready, has it loaded the mission.
struct Ready {
    ReadyState state{};
};

// The host's event about the world.
struct Event {
    EventState state{};
};

// The room as the host publishes it.
struct Room {
    RoomState state{};
};

// The encoding. Each returns the number of bytes written, or 0 when the
// buffer is too small or the data does not fit. The decoding returns false
// when the data is too short, when the actor count does not match the bytes
// that are there, or when it carries more actors than a snapshot can hold.
size_t EncodeRoom(const Room& room, uint8_t* out, size_t capacity);
bool DecodeRoom(const uint8_t* data, size_t size, Room* out);

size_t EncodeReady(const Ready& ready, uint8_t* out, size_t capacity);
bool DecodeReady(const uint8_t* data, size_t size, Ready* out);

size_t EncodeSnapshot(const Snapshot& snapshot, uint8_t* out, size_t capacity);
bool DecodeSnapshot(const uint8_t* data, size_t size, Snapshot* out);

size_t EncodeEvent(const Event& event, uint8_t* out, size_t capacity);
bool DecodeEvent(const uint8_t* data, size_t size, Event* out);

size_t EncodeAck(const AckState& ack, uint8_t* out, size_t capacity);
bool DecodeAck(const uint8_t* data, size_t size, AckState* out);

// The actor of a snapshot that carries the given native identity, or null.
const ActorState* FindActor(const Snapshot& snapshot, uint32_t native_a, uint32_t native_b, uint32_t generation);

// The actor of a snapshot that carries the given native identity, or null.
// The type has to match too, so a different actor that happens to share the
// pair is not mistaken for it.
const ActorState* FindActor(const Snapshot& snapshot, uint32_t native_a, uint32_t native_b, uint32_t generation,
                            uint16_t type);

// The key of an actor, as the snapshot carries it. The pair alone is the
// engine's object key; the generation tells two activations of the same
// script object apart.
inline uint64_t ActorKey(uint32_t native_a, uint32_t native_b) {
    return (static_cast<uint64_t>(native_a) << 32) | native_b;
}

}  // namespace coop
}  // namespace net
