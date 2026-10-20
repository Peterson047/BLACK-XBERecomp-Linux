#include "net/coop_protocol.h"

#include <cstring>

namespace net {
namespace coop {
namespace {

// The packets are packed, so a copy is a copy. The size is checked before
// the copy, so a short buffer is rejected instead of read past.
bool Copy(const void* from, size_t size, uint8_t* out, size_t capacity) {
    if (capacity < size) return false;
    std::memcpy(out, from, size);
    return true;
}

bool Restore(const uint8_t* data, size_t size, void* to, size_t expected) {
    if (size < expected) return false;
    std::memcpy(to, data, expected);
    return true;
}

}  // namespace

size_t EncodeRoom(const Room& room, uint8_t* out, size_t capacity) {
    if (!Copy(&room.state, sizeof(RoomState), out, capacity)) return 0;
    return sizeof(RoomState);
}

bool DecodeRoom(const uint8_t* data, size_t size, Room* out) {
    return Restore(data, size, &out->state, sizeof(RoomState));
}

size_t EncodeReady(const Ready& ready, uint8_t* out, size_t capacity) {
    if (!Copy(&ready.state, sizeof(ReadyState), out, capacity)) return 0;
    return sizeof(ReadyState);
}

bool DecodeReady(const uint8_t* data, size_t size, Ready* out) {
    return Restore(data, size, &out->state, sizeof(ReadyState));
}

size_t EncodeSnapshot(const Snapshot& snapshot, uint8_t* out, size_t capacity) {
    if (snapshot.header.actor_count > kMaxActorsPerSnapshot) return 0;
    if (snapshot.header.part_count == 0) return 0;
    if (snapshot.header.part_index >= snapshot.header.part_count) return 0;
    if (!Copy(&snapshot.header, sizeof(SnapshotHeader), out, capacity)) return 0;
    const size_t actors = static_cast<size_t>(snapshot.header.actor_count) * kActorStateSize;
    if (!Copy(snapshot.actors, actors, out + sizeof(SnapshotHeader), capacity - sizeof(SnapshotHeader))) return 0;
    return sizeof(SnapshotHeader) + actors;
}

bool DecodeSnapshot(const uint8_t* data, size_t size, Snapshot* out) {
    if (!Restore(data, size, &out->header, sizeof(SnapshotHeader))) return false;
    if (out->header.actor_count > kMaxActorsPerSnapshot) return false;
    if (out->header.part_count == 0) return false;
    if (out->header.part_index >= out->header.part_count) return false;
    const size_t actors = static_cast<size_t>(out->header.actor_count) * kActorStateSize;
    if (size < sizeof(SnapshotHeader) + actors) return false;
    std::memcpy(out->actors, data + sizeof(SnapshotHeader), actors);
    return true;
}

size_t EncodeEvent(const Event& event, uint8_t* out, size_t capacity) {
    if (!Copy(&event.state, sizeof(EventState), out, capacity)) return 0;
    return sizeof(EventState);
}

bool DecodeEvent(const uint8_t* data, size_t size, Event* out) {
    return Restore(data, size, &out->state, sizeof(EventState));
}

size_t EncodeAck(const AckState& ack, uint8_t* out, size_t capacity) {
    if (!Copy(&ack, sizeof(AckState), out, capacity)) return 0;
    return sizeof(AckState);
}

bool DecodeAck(const uint8_t* data, size_t size, AckState* out) {
    return Restore(data, size, out, sizeof(AckState));
}

const ActorState* FindActor(const Snapshot& snapshot, uint32_t native_a, uint32_t native_b, uint32_t generation) {
    for (size_t i = 0; i < snapshot.header.actor_count && i < kMaxActorsPerSnapshot; ++i) {
        const ActorState& actor = snapshot.actors[i];
        if (actor.native_a == native_a && actor.native_b == native_b && actor.generation == generation) return &actor;
    }
    return nullptr;
}

const ActorState* FindActor(const Snapshot& snapshot, uint32_t native_a, uint32_t native_b, uint32_t generation,
                            uint16_t type) {
    for (size_t i = 0; i < snapshot.header.actor_count && i < kMaxActorsPerSnapshot; ++i) {
        const ActorState& actor = snapshot.actors[i];
        if (actor.native_a == native_a && actor.native_b == native_b && actor.generation == generation &&
            actor.type == type) {
            return &actor;
        }
    }
    return nullptr;
}

}  // namespace coop
}  // namespace net
