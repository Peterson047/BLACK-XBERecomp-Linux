// Standalone test of the co-op wire format. No game resources, no network.
// Build: clang++ -std=c++20 -Isrc -Isdk/runtime tests/test_coop_protocol.cpp \
//        src/net/coop_protocol.cpp -o /tmp/test_coop_protocol && /tmp/test_coop_protocol
#include "net/coop_protocol.h"

#include <cstdio>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace net;
using namespace net::coop;

namespace {

[[noreturn]] void Fail(const char* message, int line) {
    throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + message);
}

#define CHECK(expr) do { if (!(expr)) Fail(#expr, __LINE__); } while (false)

ActorState MakeActor(uint32_t a, uint32_t b, uint16_t type, float x) {
    ActorState actor{};
    actor.native_a = a;
    actor.native_b = b;
    actor.generation = 7;
    actor.type = type;
    actor.flags = kActorCrouching;
    actor.pos[0] = x;
    actor.pos[1] = 1.5f;
    actor.pos[2] = -3.25f;
    actor.yaw = 1.25f;
    actor.pitch = -0.5f;
    actor.health = 75.0f;
    return actor;
}

void TestRoomRoundTrip() {
    Room room{};
    room.state.session_id = 0xA1B2C3D4;
    room.state.revision = 42;
    room.state.start_epoch = 7;
    room.state.mission_id = 0;
    room.state.stage_id = 1;
    room.state.phase = static_cast<uint8_t>(Phase::InMission);
    room.state.present_mask = 0b11;
    room.state.ready_mask = 0b01;
    room.state.loaded_mask = 0b11;

    std::vector<uint8_t> buffer(64, 0xAA);
    const size_t written = EncodeRoom(room, buffer.data(), buffer.size());
    CHECK(written == sizeof(RoomState));

    Room decoded{};
    CHECK(DecodeRoom(buffer.data(), written, &decoded));
    CHECK(decoded.state.session_id == room.state.session_id);
    CHECK(decoded.state.revision == room.state.revision);
    CHECK(decoded.state.start_epoch == room.state.start_epoch);
    CHECK(decoded.state.phase == static_cast<uint8_t>(Phase::InMission));
    CHECK(decoded.state.present_mask == 0b11);
    CHECK(decoded.state.loaded_mask == 0b11);

    // A short buffer is refused instead of written past.
    CHECK(EncodeRoom(room, buffer.data(), sizeof(RoomState) - 1) == 0);
    // A short packet is refused instead of read past.
    CHECK(!DecodeRoom(buffer.data(), sizeof(RoomState) - 1, &decoded));
}

void TestSnapshotRoundTrip() {
    Snapshot snapshot{};
    snapshot.header.start_epoch = 3;
    snapshot.header.actor_count = 4;
    snapshot.header.part_index = 0;
    snapshot.header.part_count = 1;
    for (uint32_t i = 0; i < snapshot.header.actor_count; ++i) {
        snapshot.actors[i] = MakeActor(0x1000 + i, 0x2000 + i, 1, static_cast<float>(i) * 2.0f);
    }

    std::vector<uint8_t> buffer(kMaxSnapshotBytes, 0);
    const size_t written = EncodeSnapshot(snapshot, buffer.data(), buffer.size());
    CHECK(written == kSnapshotHeaderSize + 4 * kActorStateSize);
    CHECK(written <= kMaxSnapshotBytes);

    Snapshot decoded{};
    CHECK(DecodeSnapshot(buffer.data(), written, &decoded));
    CHECK(decoded.header.start_epoch == 3);
    CHECK(decoded.header.actor_count == 4);
    for (uint32_t i = 0; i < decoded.header.actor_count; ++i) {
        CHECK(decoded.actors[i].native_a == 0x1000 + i);
        CHECK(decoded.actors[i].native_b == 0x2000 + i);
        CHECK(decoded.actors[i].generation == 7);
        CHECK(decoded.actors[i].type == 1);
        CHECK(decoded.actors[i].flags == kActorCrouching);
        CHECK(decoded.actors[i].pos[0] == static_cast<float>(i) * 2.0f);
        CHECK(decoded.actors[i].pos[2] == -3.25f);
        CHECK(decoded.actors[i].health == 75.0f);
    }

    // The identity of an actor is the native pair plus the generation, so the
    // same actor is found and a different one is not.
    const ActorState* found = FindActor(decoded, 0x1002, 0x2002, 7);
    CHECK(found != nullptr);
    CHECK(found->pos[0] == 4.0f);
    CHECK(FindActor(decoded, 0x1002, 0x2002, 7, 1) != nullptr);
    CHECK(FindActor(decoded, 0x1002, 0x2002, 7, 2) == nullptr);
    // Another activation of the same script object is another actor.
    CHECK(FindActor(decoded, 0x1002, 0x2002, 8) == nullptr);
    CHECK(FindActor(decoded, 0x9999, 0x9999, 7) == nullptr);
}

void TestSnapshotRefusesBadCounts() {
    Snapshot snapshot{};
    snapshot.header.actor_count = 3;
    snapshot.header.part_index = 0;
    snapshot.header.part_count = 1;

    std::vector<uint8_t> buffer(kMaxSnapshotBytes, 0);
    const size_t written = EncodeSnapshot(snapshot, buffer.data(), buffer.size());
    CHECK(written > 0);

    // A packet that claims more actors than it carries is refused.
    Snapshot decoded{};
    CHECK(!DecodeSnapshot(buffer.data(), written - 1, &decoded));

    // A packet that claims more actors than a snapshot can hold is refused.
    Snapshot too_many{};
    too_many.header.actor_count = static_cast<uint32_t>(kMaxActorsPerSnapshot + 1);
    too_many.header.part_index = 0;
    too_many.header.part_count = 1;
    CHECK(EncodeSnapshot(too_many, buffer.data(), buffer.size()) == 0);
    std::vector<uint8_t> forged = buffer;
    std::memcpy(forged.data() + offsetof(SnapshotHeader, actor_count), &too_many.header.actor_count, 4);
    CHECK(!DecodeSnapshot(forged.data(), written, &decoded));

    // A packet with no part is refused: part zero of zero parts is nothing.
    Snapshot no_parts{};
    no_parts.header.actor_count = 0;
    no_parts.header.part_index = 0;
    no_parts.header.part_count = 0;
    CHECK(EncodeSnapshot(no_parts, buffer.data(), buffer.size()) == 0);
    std::memcpy(forged.data() + offsetof(SnapshotHeader, part_count), &no_parts.header.part_count, 1);
    CHECK(!DecodeSnapshot(forged.data(), written, &decoded));

    // A packet whose part index is past its part count is refused too.
    std::memcpy(forged.data() + offsetof(SnapshotHeader, part_count), &too_many.header.part_count, 1);
    forged[8] = 7;  // part_index
    CHECK(!DecodeSnapshot(forged.data(), written, &decoded));
}

void TestFullSnapshotFitsTheDatagram() {
    Snapshot snapshot{};
    snapshot.header.actor_count = static_cast<uint32_t>(kMaxActorsPerSnapshot);
    snapshot.header.part_index = 0;
    snapshot.header.part_count = 1;
    std::vector<uint8_t> buffer(kMaxSnapshotBytes, 0);
    const size_t written = EncodeSnapshot(snapshot, buffer.data(), buffer.size());
    // The snapshot is the payload; the datagram adds the packet header.
    CHECK(written + sizeof(PacketHeader) == kMaxSnapshotBytes);
    // The datagram has to stay inside the path a NAT keeps.
    CHECK(written + sizeof(PacketHeader) < 1200);
}

void TestEventRoundTrip() {
    Event event{};
    event.state.start_epoch = 2;
    event.state.event_seq = 99;
    event.state.event_type = static_cast<uint8_t>(EventType::ActorDamaged);
    event.state.object_type = 0;
    event.state.native_a = 0xABCD;
    event.state.native_b = 0x1234;
    event.state.generation = 3;
    event.state.value = 25.0f;
    event.state.radius = 0.5f;
    event.state.pos[0] = 1.0f;
    event.state.pos[1] = 2.0f;
    event.state.pos[2] = 3.0f;
    event.state.dir[0] = 0.0f;
    event.state.dir[1] = 0.0f;
    event.state.dir[2] = 1.0f;
    event.state.damage_type = 2;
    event.state.flags = 3;
    event.state.extra = 1;  // the player that caused it
    event.state.event_id = 77;
    std::memcpy(event.state.text[0], "door", 5);
    std::memcpy(event.state.text[1], "museum", 7);
    // The activation of an actor: the place of the descriptor in the table
    // of the units, the value the routine takes in the register, the rest of
    // the arguments and the transform of the spawn.
    event.state.activation.descriptor_offset = 0x7A20 + 35 * 0x40;
    event.state.activation.third = 1;
    event.state.activation.arg6 = 0x97C60000u;
    event.state.activation.arg7 = 0x54461272u;
    event.state.activation.arg8 = 2;
    event.state.activation.arg9 = 1;
    event.state.activation.transform[0] = -79.5f;
    event.state.activation.transform[1] = 0.25f;
    event.state.activation.transform[2] = 15.1f;
    event.state.activation.transform[3] = 3.0f;

    std::vector<uint8_t> buffer(sizeof(EventState) + 64, 0);
    const size_t written = EncodeEvent(event, buffer.data(), buffer.size());
    CHECK(written == sizeof(EventState));

    Event decoded{};
    CHECK(DecodeEvent(buffer.data(), written, &decoded));
    CHECK(decoded.state.event_seq == 99);
    CHECK(decoded.state.event_type == static_cast<uint8_t>(EventType::ActorDamaged));
    CHECK(decoded.state.native_a == 0xABCD);
    CHECK(decoded.state.generation == 3);
    CHECK(decoded.state.value == 25.0f);
    CHECK(decoded.state.radius == 0.5f);
    CHECK(decoded.state.pos[2] == 3.0f);
    CHECK(decoded.state.dir[2] == 1.0f);
    CHECK(decoded.state.damage_type == 2);
    CHECK(decoded.state.flags == 3);
    CHECK(decoded.state.extra == 1);
    CHECK(decoded.state.event_id == 77);
    CHECK(std::string(decoded.state.text[0]) == "door");
    CHECK(std::string(decoded.state.text[1]) == "museum");
    // The activation travels whole.
    CHECK(decoded.state.activation.descriptor_offset == 0x7A20 + 35 * 0x40);
    CHECK(decoded.state.activation.third == 1);
    CHECK(decoded.state.activation.arg6 == 0x97C60000u);
    CHECK(decoded.state.activation.arg7 == 0x54461272u);
    CHECK(decoded.state.activation.arg8 == 2);
    CHECK(decoded.state.activation.arg9 == 1);
    CHECK(decoded.state.activation.transform[0] == -79.5f);
    CHECK(decoded.state.activation.transform[3] == 3.0f);
}

// The event of an activation has to stay inside the path a NAT keeps: the
// datagram is the payload plus the header of the packet.
void TestEventFitsTheDatagram() {
    Event event{};
    event.state.event_type = static_cast<uint8_t>(EventType::ActorActivated);
    event.state.activation.descriptor_offset = 0x7A20;
    std::vector<uint8_t> buffer(sizeof(EventState) + 64, 0);
    const size_t written = EncodeEvent(event, buffer.data(), buffer.size());
    CHECK(written == sizeof(EventState));
    CHECK(written + sizeof(PacketHeader) < 1200);
    // A short buffer is refused instead of written past.
    CHECK(EncodeEvent(event, buffer.data(), sizeof(EventState) - 1) == 0);
}

void TestReadyAndAck() {
    Ready ready{};
    ready.state.start_epoch = 5;
    ready.state.ready = 1;
    ready.state.loaded = 0;
    std::vector<uint8_t> buffer(16, 0);
    const size_t written = EncodeReady(ready, buffer.data(), buffer.size());
    Ready decoded_ready{};
    CHECK(DecodeReady(buffer.data(), written, &decoded_ready));
    CHECK(decoded_ready.state.ready == 1);
    CHECK(decoded_ready.state.loaded == 0);
    CHECK(decoded_ready.state.start_epoch == 5);

    AckState ack{};
    ack.start_epoch = 5;
    ack.event_seq = 1234;
    const size_t ack_written = EncodeAck(ack, buffer.data(), buffer.size());
    AckState decoded_ack{};
    CHECK(DecodeAck(buffer.data(), ack_written, &decoded_ack));
    CHECK(decoded_ack.event_seq == 1234);
    CHECK(decoded_ack.start_epoch == 5);
}

void TestMessageTypesAreDistinct() {
    // The co-op types were appended, so the old ones kept their numbers.
    CHECK(static_cast<int>(MsgType::Ping) == 0);
    CHECK(static_cast<int>(MsgType::Chat) == 8);
    CHECK(static_cast<int>(MsgType::CoopRoom) == 9);
    CHECK(static_cast<int>(MsgType::CoopReady) == 10);
    CHECK(static_cast<int>(MsgType::CoopSnapshot) == 11);
    CHECK(static_cast<int>(MsgType::CoopEvent) == 12);
    CHECK(static_cast<int>(MsgType::CoopAck) == 13);
}

}  // namespace

int main() {
    TestRoomRoundTrip();
    TestSnapshotRoundTrip();
    TestSnapshotRefusesBadCounts();
    TestFullSnapshotFitsTheDatagram();
    TestEventRoundTrip();
    TestEventFitsTheDatagram();
    TestReadyAndAck();
    TestMessageTypesAreDistinct();
    puts("coop protocol test passed");
    return 0;
}
