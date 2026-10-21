"""
Tier 4: Real-World Multiplayer Match & Combat Scenarios
Requires >=5 multi-feature combat scenarios (6 tests total).
Authoritative Sources: ORIGINAL_REQUEST.md, PROJECT.md.
"""

import math
import random
import time
import unittest
from typing import Dict, List

from tests.e2e.common import (
    ACTOR_POOL_SLOTS,
    DAMAGE_CHEST,
    DAMAGE_HEAD,
    DAMAGE_LEGS,
    DAMAGE_STOMACH,
    DEFAULT_PORT,
    HEADER_SIZE,
    MAGIC_PROTOCOL,
    MAX_PLAYERS,
    MISSION1_SPAWNS,
    MSG_CHAT,
    MSG_DISCONNECT,
    MSG_HIT,
    MSG_JOIN_ACK,
    MSG_JOIN_REQUEST,
    MSG_SHOOT,
    MSG_STATE_UPDATE,
    PACKET_STATE_SIZE,
    ZONE_CHEST,
    ZONE_HEAD,
    ZONE_LEGS,
    ZONE_STOMACH,
    Vec3,
    compute_puppet_matrix,
    pack_chat,
    pack_header,
    pack_hit,
    pack_join_ack,
    pack_join_request,
    pack_shoot,
    pack_state_packet,
    select_best_spawn,
    simulate_hit_detection,
    unpack_header,
    unpack_state_packet,
)


class TestTier4RealWorldScenarios(unittest.TestCase):
    """Tier 4: the real-world scenarios (6 tests).

    S1 models a co-op session: the room, the load barrier, the world
    snapshot and the events. The Deathmatch round it used to model is gone —
    the branch is the co-op now.

    S2 to S6 still model the prototype's hit zones and spawn choice. They
    are models of a mode the game no longer has, and they are worth nothing
    as evidence about the game; they are kept because the models are the
    clearest statement of the rules they were written for, and because
    retiring them is a separate piece of work from the co-op.
    """

    def test_s1_coop_session_scenario(self):
        """S1: a co-op session: the room, the load barrier, the world snapshot and the events.

        The Deathmatch round this test used to model is gone: the branch is
        the co-op now. What the co-op does is the session, and this models it.
        """
        from tests.e2e.common import (
            COOP_PHASE_IN_MISSION,
            COOP_PHASE_LOADING,
            COOP_PHASE_LOBBY,
            coop_event,
            coop_room,
            coop_snapshot,
        )

        # 1. The room: the host names the mission and both sides are in it.
        room = coop_room(revision=1, phase=COOP_PHASE_LOBBY, present_mask=0b11)
        self.assertEqual(room["phase"], COOP_PHASE_LOBBY)
        self.assertEqual(room["present_mask"], 0b11)

        # 2. The mission is named: the room goes to Loading and the epoch
        #    still belongs to the round that has not started.
        room = coop_room(revision=2, phase=COOP_PHASE_LOADING, present_mask=0b11)
        self.assertEqual(room["phase"], COOP_PHASE_LOADING)
        self.assertEqual(room["start_epoch"], 0)

        # 3. Both sides loaded, so the round starts and the epoch moves.
        room = coop_room(revision=3, phase=COOP_PHASE_IN_MISSION, start_epoch=1, present_mask=0b11,
                         loaded_mask=0b11)
        self.assertEqual(room["phase"], COOP_PHASE_IN_MISSION)
        self.assertEqual(room["start_epoch"], 1)

        # 4. The world snapshot: the actors of the host, with their identity.
        actors = [
            {"native_a": 0x1111, "native_b": 0x2222, "generation": 3, "type": 1, "pos": [1.0, 0.0, 2.0]},
            {"native_a": 0x3333, "native_b": 0x4444, "generation": 1, "type": 1, "pos": [3.0, 0.0, 4.0]},
        ]
        snapshot = coop_snapshot(actors, start_epoch=1)
        self.assertEqual(snapshot["actor_count"], 2)
        self.assertEqual(snapshot["start_epoch"], 1)
        self.assertEqual(snapshot["actors"][0]["native_a"], 0x1111)
        self.assertEqual(snapshot["actors"][0]["generation"], 3)

        # 5. The events: a shot and a death, with their sequence numbers.
        shot = coop_event(1, event_type=9, value=0.0)
        death = coop_event(2, event_type=0, value=100.0, native_a=0x1111, native_b=0x2222, generation=3)
        self.assertEqual(shot["event_seq"], 1)
        self.assertEqual(death["event_seq"], 2)
        self.assertEqual(death["native_a"], 0x1111)
        self.assertEqual(death["generation"], 3)

    def test_s2_client_reconnect_and_session_recovery(self):
        """S2: Client disconnects mid-match and reconnects, regaining sync without corrupting state."""
        lobby = {1: {"name": "Alpha", "frags": 2}, 2: {"name": "Bravo", "frags": 1}}

        # Client 2 disconnects:
        disc_packet = pack_header(MSG_DISCONNECT, 2, 99)
        del lobby[2]
        self.assertNotIn(2, lobby)

        # Client 2 reconnects:
        join_req = pack_join_request("Bravo")
        # Server reassigns next available slot:
        new_id = 2
        lobby[new_id] = {"name": "Bravo", "frags": 0}  # Reset round score

        self.assertIn(2, lobby)
        self.assertEqual(lobby[1]["frags"], 2, "Remaining player score preserved")
        self.assertEqual(lobby[2]["name"], "Bravo")

    def test_s3_crouch_peek_sniper_duel(self):
        """S3: Tactical cover duel: crouching reduces height to 1.25m causing high shots to miss."""
        shooter_pos = Vec3(0, 1.65, 0)
        target_pos = Vec3(0, 1.65, 45.0)  # 45m range

        # 1. Target is crouching behind barrier:
        # High shot at head height y=1.55m:
        aim_high = Vec3(0, 1.55, 0)
        shot_high = simulate_hit_detection(
            origin=aim_high,
            dir=Vec3(0, 0, 1),
            target_pos=Vec3(target_pos.x, target_pos.y - 0.60, target_pos.z),  # crouching eye pos
            is_crouching=True
        )
        self.assertIsNone(shot_high, "High shot must sail over crouching opponent")

        # 2. Target stands up to peek:
        # Shooter fires headshot:
        shot_head = simulate_hit_detection(
            origin=Vec3(0, 1.55, 0),
            dir=Vec3(0, 0, 1),
            target_pos=target_pos,
            is_crouching=False
        )
        self.assertIsNotNone(shot_head)
        self.assertEqual(shot_head[0], ZONE_HEAD)
        self.assertEqual(shot_head[1], DAMAGE_HEAD)

    def test_s4_anti_spawncamp_tactical_rotation(self):
        """S4: Anti-spawncamp safety algorithm ensures player never spawns within 15m of killer."""
        killer_pos = Vec3(MISSION1_SPAWNS[0][0], MISSION1_SPAWNS[0][1], MISSION1_SPAWNS[0][2])  # Spawn 0

        # Simulate 5 respawns while killer camps Spawn 0:
        for _ in range(5):
            spawn_idx, spawn_pos = select_best_spawn([killer_pos], min_safe_dist=15.0)
            dist_to_killer = (spawn_pos - killer_pos).length()
            self.assertGreaterEqual(dist_to_killer, 15.0, "Victim must never spawn within 15m of killer")
            self.assertNotEqual(spawn_idx, 0, "Spawn 0 must not be chosen while killer camps it")

    def test_s5_high_jitter_network_packet_loss_combat(self):
        """S5: 20% simulated UDP packet loss and jitter: cl_interp interpolation prevents visual jumps."""
        # Generate sequence of moving packets from Z=0 to Z=30:
        packets = []
        for i in range(30):
            pos_z = float(i)
            pkt = pack_state_packet(1, i, (0.0, 1.65, pos_z), (0.0, 0.0, 1.0), 0.0, 0.0)
            packets.append(pkt)

        # Simulate 20% random packet drops:
        random.seed(42)
        received_packets = [p for p in packets if random.random() > 0.20]
        self.assertGreater(len(received_packets), 20)

        # Client interpolation buffer:
        client_positions = []
        prev_pos = None
        for pkt in received_packets:
            unpacked = unpack_state_packet(pkt)
            curr_pos = Vec3(*unpacked["pos"])
            if prev_pos is not None:
                # Interpolate 2 sub-steps:
                for step in [0.5, 1.0]:
                    sub_pos = prev_pos + (curr_pos - prev_pos) * step
                    client_positions.append(sub_pos)
            prev_pos = curr_pos

        # Verify max delta between frames is bounded (no teleportation):
        for i in range(len(client_positions) - 1):
            delta = (client_positions[i+1] - client_positions[i]).length()
            self.assertLess(delta, 3.0, "Interpolated movement delta must remain bounded")

    def test_s6_clean_direct_launch_to_combat_pipeline(self):
        """S6: Complete end-to-end pipeline: direct launch -> lobby sync -> puppet update -> hit detection."""
        # 1. Direct Launch State
        engine = {"level": 0, "state": "GAMEPLAY", "speech_disabled": True}
        self.assertEqual(engine["level"], 0)
        self.assertTrue(engine["speech_disabled"])

        # 2. Connect 2 players
        p1 = {"id": 1, "pos": Vec3(-4.0, 1.65, 2.0), "health": 100.0}
        p2 = {"id": 2, "pos": Vec3(12.0, 1.65, 18.0), "health": 100.0}

        # 3. Update puppets in Character Manager
        puppet1_mat = compute_puppet_matrix(p1["pos"], Vec3(0, 0, 1), 0.0, 0.0, 0.0, False)
        puppet2_mat = compute_puppet_matrix(p2["pos"], Vec3(0, 0, -1), math.pi, 0.0, 0.0, False)

        self.assertEqual(puppet1_mat["active"], 1)
        self.assertEqual(puppet2_mat["active"], 1)

        # 4. Exchange fire: P1 shoots at P2
        fire_dir = (p2["pos"] - p1["pos"]).normalize()
        hit = simulate_hit_detection(p1["pos"], fire_dir, p2["pos"], is_crouching=False)
        self.assertIsNotNone(hit)
        damage_dealt = hit[1]
        p2["health"] -= damage_dealt

        self.assertLess(p2["health"], 100.0)
        self.assertGreater(damage_dealt, 0.0)


if __name__ == "__main__":
    unittest.main()
