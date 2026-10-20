#include "game/coop_adapter.h"
#include "game/coop_mission.h"
#include "game/coop_world.h"
#include "game/multiplayer.h"
#include "game/layout.h"
#include "common/aspect.h"
#include "common/options.h"
#include "common/menu.h"
#include "core/log.h"
#include "xdk/d3d8.h"

#include <imgui.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <random>

using namespace kernel;
using namespace layout;

namespace game {

namespace {

constexpr uint32_t kPlayerForward = 0xF0;
constexpr uint32_t kPlayerEye = 0x100;
constexpr uint32_t kPlayerWeapon = 0x29C;
constexpr uint32_t kPlayerZoom = 0x2A4;
constexpr uint32_t kPlayerFireRequest = 0x32C;

static uint32_t Player() {
    uint32_t w = R32(kWorld);
    return w ? w + kWorldPlayer : 0;
}

// 10 Tactical Spawns distributed across Mission 1 (Veblensk City Street)
const Vec3 kMission1Spawns[kNumSpawns] = {
    Vec3(-4.0f, 0.0f, 2.0f),       // Spawn 0: Start alley
    Vec3(12.0f, 0.0f, 18.0f),      // Spawn 1: Behind burned truck
    Vec3(-14.0f, 0.0f, 32.0f),     // Spawn 2: Inside bombed house
    Vec3(8.0f, 0.0f, 48.0f),       // Spawn 3: Sandbag barricade
    Vec3(-2.0f, 0.0f, 65.0f),      // Spawn 4: Central plaza entry
    Vec3(16.0f, 0.0f, 82.0f),      // Spawn 5: Right flanking alley
    Vec3(-12.0f, 0.0f, 98.0f),     // Spawn 6: Sniper window ruin
    Vec3(0.0f, 0.0f, 115.0f),      // Spawn 7: Main crossroads
    Vec3(10.0f, 0.0f, 130.0f),     // Spawn 8: Memorial square
    Vec3(-8.0f, 0.0f, 145.0f)      // Spawn 9: Street end barricade
};

Vec3 Normalize(const Vec3& v) {
    float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len <= 0.0001f) return Vec3(0, 0, 1);
    return Vec3(v.x / len, v.y / len, v.z / len);
}

Vec3 Cross(const Vec3& a, const Vec3& b) {
    return Vec3(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    );
}

float Dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

} // namespace

MultiplayerManager& MultiplayerManager::Get() {
    static MultiplayerManager instance;
    return instance;
}

MultiplayerManager::MultiplayerManager() {
    std::strncpy(name_buffer_, "Player 1", sizeof(name_buffer_));
}

MultiplayerManager::~MultiplayerManager() = default;

void MultiplayerManager::Init() {
    direct_map_launch_requested_.store(false, std::memory_order_release);
    was_network_connected_ = false;
    LOGI("MultiplayerManager: Initialized CS 1.6 Deathmatch System for Veblensk");
}

void MultiplayerManager::RequestDirectMapLaunch() {
    bool already_requested = direct_map_launch_requested_.exchange(true, std::memory_order_acq_rel);
    if (!already_requested) {
        LOGI("MultiplayerManager: Queued direct launch to Veblensk");
    }
}

bool MultiplayerManager::ConsumeDirectMapLaunchRequest() {
    return direct_map_launch_requested_.exchange(false, std::memory_order_acq_rel);
}

bool MultiplayerManager::HasDirectMapLaunchRequest() const {
    return direct_map_launch_requested_.load(std::memory_order_acquire);
}

bool MultiplayerManager::IsInGame() const {
    uint32_t game_obj = R32(kGame);
    uint32_t world = R32(kWorld);
    if (!game_obj || !world) return false;
    uint32_t state = R32(game_obj + kGameState);
    return state != game_obj + kFrontEnd;
}

void MultiplayerManager::TeleportPlayer(const Vec3& pos) {
    uint32_t p = Player();
    if (!p) return;
    G<float>(p + kPlayerEye) = pos.x;
    G<float>(p + kPlayerEye + 4) = pos.y + 1.7f; // Eye height
    G<float>(p + kPlayerEye + 8) = pos.z;
}

// Clear Campaign singleplayer AI so only multiplayer human players populate Veblensk
void MultiplayerManager::ClearCampaignEnemies() {
    uint32_t mgr = R32(0x002D1980);
    if (!mgr) return;

    uint32_t count = R32(mgr + 0x79B4);
    uint32_t list = R32(mgr + 0x79BC);
    if (!list) return;

    if (actor_manager_ != mgr) {
        native_actor_types_.clear();
        actor_manager_ = mgr;
    }
    // Record initialized actor categories without disabling native components.
    for (uint32_t i = 0; i < count && i < 64; ++i) {
        const uint32_t actor = R32(list + i * 4);
        static uint32_t logged[8]{};
        if (actor && i < 8 && logged[i] != actor) {
            LOGI("MP actor slot %u: object=%08X vtable=%08X type=%u used=%u capacity=%u", i,
                 actor, R32(actor), R32(actor + 0xC4), R32(mgr + 0x79B8), count);
            logged[i] = actor;
        }
        if (!actor || R32(actor) != 0x28C1E8) continue;
        const uint32_t type = R32(actor + 0xC4);
        if (type) native_actor_types_.emplace(actor, type);
        // Native type is a component-dispatch discriminator, not an enable bit.
        // Keep it intact; zeroing it routes actors through incompatible handlers.

    }

    // +79B4 is capacity and +79B8 is the used size of the native container.
    // Preserve both: changing capacity here corrupts later actor registration.
}

// Pick optimal random spawn point away from active enemies (CS DM style)
void MultiplayerManager::RespawnLocalPlayer() {
    local_health_ = 100.0f;
    local_deaths_++;
    spawn_protection_timer_ = 3.0f; // 3 seconds spawn protection

    auto players = net::NetworkManager::Get().GetPlayers();
    uint8_t my_id = net::NetworkManager::Get().GetLocalPlayerId();

    std::vector<size_t> safe_spawns;
    for (size_t i = 0; i < kNumSpawns; ++i) {
        Vec3 sp = kMission1Spawns[i];
        float min_dist = 9999.0f;

        for (const auto& other : players) {
            if (other.id != my_id && other.active) {
                Vec3 opos(other.state.pos[0], other.state.pos[1], other.state.pos[2]);
                Vec3 diff = sp - opos;
                float d = std::sqrt(Dot(diff, diff));
                if (d < min_dist) min_dist = d;
            }
        }

        // Keep spawns farther than 15 meters from any enemy
        if (min_dist > 15.0f || players.size() <= 1) {
            safe_spawns.push_back(i);
        }
    }

    size_t chosen_spawn = 0;
    if (!safe_spawns.empty()) {
        static std::mt19937 rng(1337);
        std::uniform_int_distribution<size_t> dist(0, safe_spawns.size() - 1);
        chosen_spawn = safe_spawns[dist(rng)];
    } else {
        chosen_spawn = (my_id * 3) % kNumSpawns;
    }

    TeleportPlayer(kMission1Spawns[chosen_spawn]);
    net::NetworkManager::Get().LogEvent("Respawned at Spawn [" + std::to_string(chosen_spawn) + "] with 3s Protection");
}

void MultiplayerManager::AddDamagePopup(const Vec3& pos, float damage, HitZone zone) {
    DamagePopup dp{};
    dp.world_pos = pos;
    dp.damage = damage;
    dp.timer = 1.3f;
    dp.max_timer = 1.3f;
    dp.zone = zone;
    damage_popups_.push_back(dp);
}

void MultiplayerManager::AddTracer(const Vec3& start, const Vec3& end) {
    TracerEffect tr{};
    tr.start_pos = start;
    tr.end_pos = end;
    tr.timer = 0.25f;
    tr.max_timer = 0.25f;
    bullet_tracers_.push_back(tr);
}

void MultiplayerManager::AddKillfeed(const std::string& killer, const std::string& victim, bool headshot) {
    KillfeedEntry kf{};
    kf.killer = killer;
    kf.victim = victim;
    kf.headshot = headshot;
    kf.timer = 5.0f;
    killfeed_entries_.push_back(kf);
    if (killfeed_entries_.size() > 6) {
        killfeed_entries_.erase(killfeed_entries_.begin());
    }
}

void MultiplayerManager::TriggerHitmarker() {
    hitmarker_timer_ = 0.20f;
}


void MultiplayerManager::UpdateEnemyPuppet(uint8_t slot_idx, const Vec3& pos, const Vec3& forward, float yaw, float pitch, float move_rate, bool is_crouching) {
    uint32_t mgr = R32(0x002D1980);
    if (!mgr) return;

    // Only use an actor already registered and initialized by the engine.
    // The old fallback fabricated an actor inside the manager and overwrote its
    // vtable, risking guest-state corruption when a remote client joined.
    const uint32_t count = R32(mgr + 0x79B4);
    const uint32_t list = R32(mgr + 0x79BC);
    if (!list || slot_idx >= 32) return;
    uint32_t actor = 0;
    uint32_t ordinal = 0;
    for (uint32_t i = 0; i < count && i < 64; ++i) {
        const uint32_t candidate = R32(list + i * 4);
        if (!candidate || R32(candidate) != 0x28C1E8) continue;
        if (ordinal++ == slot_idx) { actor = candidate; break; }
    }
    if (!actor) return;
    const auto native_type = native_actor_types_.find(actor);
    if (native_type == native_actor_types_.end()) return;
    W32(actor + 0xC4, native_type->second);

    // Orient character's 3D affine matrix to face look direction
    Vec3 fwd = Normalize(Vec3(forward.x, 0.0f, forward.z));
    Vec3 up(0.0f, 1.0f, 0.0f);
    Vec3 right = Normalize(Cross(up, fwd));

    // Row 0: Right (0x70)
    G<float>(actor + 0x70) = right.x;
    G<float>(actor + 0x74) = right.y;
    G<float>(actor + 0x78) = right.z;
    G<float>(actor + 0x7C) = 0.0f;

    // Row 1: Up (0x80)
    G<float>(actor + 0x80) = up.x;
    G<float>(actor + 0x84) = up.y;
    G<float>(actor + 0x88) = up.z;
    G<float>(actor + 0x8C) = 0.0f;

    // Row 2: Forward (0x90)
    G<float>(actor + 0x90) = fwd.x;
    G<float>(actor + 0x94) = fwd.y;
    G<float>(actor + 0x98) = fwd.z;
    G<float>(actor + 0x9C) = 0.0f;

    // Row 3: Position (0xA0) - base of feet
    float base_y = pos.y - (is_crouching ? 1.05f : 1.65f);
    G<float>(actor + 0xA0) = pos.x;
    G<float>(actor + 0xA4) = base_y;
    G<float>(actor + 0xA8) = pos.z;
    G<float>(actor + 0xAC) = 1.0f;

    // Keep native type, AI/action state and animation controllers intact. The
    // previous writes treated unverified controller offsets as speed and stance
    // controls, continually forcing action state while moving the model.
    G<uint8_t>(actor + 0xC8) = 0; // Render at full detail.

}

// Check Hit Detection with CS 1.6 Hitbox Zones (Head, Chest, Stomach, Legs)
void MultiplayerManager::CheckHitDetection(const Vec3& origin, const Vec3& dir, uint32_t weapon) {
    auto players = net::NetworkManager::Get().GetPlayers();
    uint8_t my_id = net::NetworkManager::Get().GetLocalPlayerId();

    for (const auto& other : players) {
        if (other.id == my_id || !other.active || !other.has_state) continue;

        Vec3 target(other.state.pos[0], other.state.pos[1], other.state.pos[2]);
        Vec3 to_target = target - origin;

        float proj = Dot(to_target, dir);
        if (proj <= 0.0f || proj > 120.0f) continue;

        Vec3 closest_point = origin + (dir * proj);
        Vec3 diff = target - closest_point;
        float dist_sq = diff.x * diff.x + diff.z * diff.z;

        // Cylindrical body hitbox: 0.85m radius
        if (dist_sq < (0.85f * 0.85f)) {
            bool is_crouching = (other.state.flags & (1 << 2)) != 0;
            float feet_y = target.y - (is_crouching ? 1.05f : 1.65f);
            float hit_h = closest_point.y - feet_y;
            float max_h = is_crouching ? 1.25f : 1.85f;

            if (hit_h < -0.1f || hit_h > max_h) continue;

            HitZone zone = HitZone::Chest;
            float damage = 34.0f; // Base rifle damage

            if (is_crouching) {
                if (hit_h >= 0.90f) {
                    zone = HitZone::Head;
                    damage = 85.0f;
                } else if (hit_h >= 0.60f) {
                    zone = HitZone::Chest;
                    damage = 34.0f;
                } else if (hit_h >= 0.35f) {
                    zone = HitZone::Stomach;
                    damage = 30.0f;
                } else {
                    zone = HitZone::Legs;
                    damage = 22.0f;
                }
            } else {
                if (hit_h >= 1.45f) {
                    zone = HitZone::Head;
                    damage = 85.0f;
                } else if (hit_h >= 1.05f) {
                    zone = HitZone::Chest;
                    damage = 34.0f;
                } else if (hit_h >= 0.70f) {
                    zone = HitZone::Stomach;
                    damage = 30.0f;
                } else {
                    zone = HitZone::Legs;
                    damage = 22.0f;
                }
            }

            float hit_pos[3] = {closest_point.x, closest_point.y, closest_point.z};
            net::NetworkManager::Get().SendHit(other.id, damage, hit_pos);

            total_damage_dealt_ += damage;
            TriggerHitmarker();
            AddDamagePopup(closest_point, damage, zone);

            bool killed = (other.state.health - damage) <= 0.0f;
            if (killed) {
                local_frags_++;
                AddKillfeed(net::NetworkManager::Get().GetLocalPlayerName(), other.name, zone == HitZone::Head);
            }

            const char* zone_str = (zone == HitZone::Head) ? "[HEADSHOT]" :
                                  (zone == HitZone::Chest) ? "[CHEST]" :
                                  (zone == HitZone::Stomach) ? "[STOMACH]" : "[LEGS]";

            net::NetworkManager::Get().LogEvent(
                std::string("HIT ") + other.name + " " + zone_str + " (-" + std::to_string(int(damage)) + " HP)"
            );
            break;
        }
    }
}

void MultiplayerManager::NotifyLocalShot(const Vec3& origin, const Vec3& direction, uint32_t weapon) {
    if (!net::NetworkManager::Get().IsConnected()) return;
    std::lock_guard<std::mutex> lock(shots_mutex_);
    // Preserve the aim at the native shot, before the next mouse/recoil update.
    if (local_shots_.size() >= 64) return;
    local_shots_.push_back({origin, Normalize(direction), weapon});
    pending_shots_.store(static_cast<uint32_t>(local_shots_.size()), std::memory_order_relaxed);
    confirmed_shots_.fetch_add(1, std::memory_order_relaxed);
}

void MultiplayerManager::CheckLocalPlayerShooting(const Vec3&, const Vec3&, uint32_t) {
    std::vector<LocalShot> shots;
    {
        std::lock_guard<std::mutex> lock(shots_mutex_);
        shots.swap(local_shots_);
        pending_shots_.store(0, std::memory_order_relaxed);
    }
    if (app::MenuOpen()) return;
    for (const auto& shot : shots) {
        spawn_protection_timer_ = 0.0f;
        shot_flash_timer_ = 0.12f;
        const float origin[3] = {shot.origin.x, shot.origin.y, shot.origin.z};
        const float dir[3] = {shot.direction.x, shot.direction.y, shot.direction.z};
        net::NetworkManager::Get().BroadcastShoot(origin, dir, shot.weapon);
        // The co-op has no player against player: the damage to the campaign
        // is resolved by the engine and replicated by the world layer.
        if (!coop::Adapter::Get().IsEnabled()) {
            CheckHitDetection(shot.origin, shot.direction, shot.weapon);
        }
        AddTracer(shot.origin, shot.origin + shot.direction * 90.0f);
    }
}

void MultiplayerManager::Update(float dt) {
    auto& network = net::NetworkManager::Get();
    network.Update(dt);
    shot_flash_timer_ = std::max(0.0f, shot_flash_timer_ - dt);
    for (const auto& player : network.GetPlayers()) {
        if (player.id == network.GetLocalPlayerId()) {
            local_health_ = player.state.health;
            local_frags_ = player.state.frags;
            local_deaths_ = player.state.deaths;
        }
    }

    const bool network_connected = network.IsConnected();
    if (network_connected && !was_network_connected_) {
        // In the co-op the mission is named by the session, and the level is
        // loaded when both sides are ready for it.
        if (!coop::Adapter::Get().IsEnabled()) RequestDirectMapLaunch();
    }
    was_network_connected_ = network_connected;

    if (spawn_protection_timer_ > 0.0f) {
        spawn_protection_timer_ -= dt;
    }

    if (hitmarker_timer_ > 0.0f) {
        hitmarker_timer_ -= dt;
    }

    // Update damage popups
    for (auto it = damage_popups_.begin(); it != damage_popups_.end();) {
        it->timer -= dt;
        it->world_pos.y += dt * 0.80f;
        if (it->timer <= 0.0f) {
            it = damage_popups_.erase(it);
        } else {
            ++it;
        }
    }

    // Update bullet tracers
    for (auto it = bullet_tracers_.begin(); it != bullet_tracers_.end();) {
        it->timer -= dt;
        if (it->timer <= 0.0f) {
            it = bullet_tracers_.erase(it);
        } else {
            ++it;
        }
    }

    // Update killfeed entries
    for (auto it = killfeed_entries_.begin(); it != killfeed_entries_.end();) {
        it->timer -= dt;
        if (it->timer <= 0.0f) {
            it = killfeed_entries_.erase(it);
        } else {
            ++it;
        }
    }

    if (!IsInGame()) return;
    if (!net::NetworkManager::Get().IsConnected()) return;

    // Collect initialized native actors for remote proxies. In the co-op the
    // campaign actors are the content of the mission, not bodies to lend out.
    if (!coop::Adapter::Get().IsEnabled()) ClearCampaignEnemies();

    uint32_t p = Player();
    if (!p) return;

    Vec3 eye(
        G<float>(p + kPlayerEye),
        G<float>(p + kPlayerEye + 4),
        G<float>(p + kPlayerEye + 8)
    );

    Vec3 forward(
        G<float>(p + kPlayerForward),
        G<float>(p + kPlayerForward + 4),
        G<float>(p + kPlayerForward + 8)
    );

    uint32_t weapon = R32(p + kPlayerWeapon);
    bool firing = shot_flash_timer_ > 0.0f && !app::MenuOpen();
    bool aiming = (G<float>(p + kPlayerZoom) > 1.05f);

    CheckLocalPlayerShooting(eye, forward, weapon);
    firing = shot_flash_timer_ > 0.0f && !app::MenuOpen();

    // Update puppets for active remote players with GoldSrc entity interpolation
    auto remote_players = net::NetworkManager::Get().GetPlayers();
    uint8_t local_id = net::NetworkManager::Get().GetLocalPlayerId();
    uint8_t puppet_slot = 0;
    for (const auto& rpl : remote_players) {
        if (rpl.id != local_id && rpl.active && rpl.has_state) {
            Vec3 rpos(rpl.state.pos[0], rpl.state.pos[1], rpl.state.pos[2]);
            if (rpl.interp_alpha < 1.0f) {
                float a = rpl.interp_alpha;
                rpos.x = rpl.prev_pos[0] + (rpl.current_pos[0] - rpl.prev_pos[0]) * a;
                rpos.y = rpl.prev_pos[1] + (rpl.current_pos[1] - rpl.prev_pos[1]) * a;
                rpos.z = rpl.prev_pos[2] + (rpl.current_pos[2] - rpl.prev_pos[2]) * a;
            }
            Vec3 rfwd(rpl.state.forward[0], rpl.state.forward[1], rpl.state.forward[2]);

            // Check authoritative movement state from network flags
            bool r_crouching = (rpl.state.flags & (1 << 2)) != 0;
            bool r_moving = (rpl.state.flags & (1 << 4)) != 0;
            float move_rate = r_moving ? (r_crouching ? 2.5f : 4.5f) : 0.0f;

            float r_yaw = rpl.state.yaw;
            float r_pitch = rpl.state.pitch;

            if (!coop::Adapter::Get().IsEnabled()) {
                UpdateEnemyPuppet(puppet_slot++, rpos, rfwd, r_yaw, r_pitch, move_rate, r_crouching);
            }

            if (rpl.just_fired) {
                Vec3 end_tr = rpos + (rfwd * 70.0f);
                AddTracer(rpos, end_tr);
            }
        }
    }

    // Local crouch detection (when standing eye height is ~1.70m; when crouching ~1.10m)
    float base_feet_y = G<float>(p + 0xA0 + 4);
    float eye_height = (base_feet_y > -100.0f && base_feet_y < 100.0f) ? (eye.y - base_feet_y) : 1.70f;
    bool is_crouching = (eye_height < 1.35f);

    // Body translation avoids treating camera sway/recoil as walking.
    const Vec3 body(G<float>(p + 0xA0), G<float>(p + 0xA4), G<float>(p + 0xA8));
    static Vec3 s_last_local_body = body;
    Vec3 move_delta = body - s_last_local_body;
    s_last_local_body = body;
    float horizontal_move = std::sqrt(move_delta.x * move_delta.x + move_delta.z * move_delta.z);
    float local_speed = (dt > 0.0001f) ? (horizontal_move / dt) : 0.0f;
    bool is_moving = (local_speed > 0.35f);

    float local_yaw = std::atan2(forward.x, forward.z);
    float local_pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));

    // Send local state at 30Hz
    state_send_timer_ += dt;
    if (state_send_timer_ >= 0.033f) {
        state_send_timer_ = 0.0f;

        net::PlayerStateData data{};
        data.pos[0] = eye.x;
        data.pos[1] = eye.y;
        data.pos[2] = eye.z;
        data.forward[0] = forward.x;
        data.forward[1] = forward.y;
        data.forward[2] = forward.z;
        data.yaw = local_yaw;
        data.pitch = local_pitch;
        data.weapon_id = weapon;
        data.flags = (firing ? 1 : 0) | (aiming ? 2 : 0) | (is_crouching ? (1 << 2) : 0) | (is_moving ? (1 << 4) : 0);
        data.health = local_health_;
        data.frags = static_cast<uint16_t>(local_frags_);
        data.deaths = static_cast<uint16_t>(local_deaths_);

        net::NetworkManager::Get().SendLocalState(data);
    }
}

void MultiplayerManager::DrawOverlay() {
    // Guest frame hook
}

// 3D Tactical Soldier Model: in-engine native character asset is rendered by the engine.
// This function renders additional dynamic effects such as muzzle flash upon firing.
void MultiplayerManager::Draw3DPlayerModel(void* draw_list_ptr, const Vec3& pos, const Vec3& forward, float yaw, bool is_firing, const char* name, float health) {
    auto* draw = static_cast<ImDrawList*>(draw_list_ptr);
    if (!draw) return;

    if (is_firing) {
        Vec3 fwd = Normalize(Vec3(forward.x, 0.0f, forward.z));
        Vec3 muzzle = pos + (fwd * 0.75f) - Vec3(0, 0.25f, 0);
        float sx, sy, dist;
        if (WorldToScreen(muzzle, sx, sy, dist)) {
            float rad = std::clamp(35.0f / (dist + 1.0f), 5.0f, 25.0f);
            draw->AddCircleFilled(ImVec2(sx, sy), rad, IM_COL32(255, 190, 40, 230));
            draw->AddCircleFilled(ImVec2(sx, sy), rad * 0.5f, IM_COL32(255, 255, 230, 255));
        }
    }
}

void MultiplayerManager::DrawImGuiHUD() {
    if (!IsInGame() || !net::NetworkManager::Get().IsConnected()) return;

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (!draw) return;

    ImGuiIO& io = ImGui::GetIO();
    auto players = net::NetworkManager::Get().GetPlayers();
    uint8_t my_id = net::NetworkManager::Get().GetLocalPlayerId();

    // 1. Draw 3D Bullet Tracers
    for (const auto& tr : bullet_tracers_) {
        float sx1, sy1, d1;
        float sx2, sy2, d2;
        if (WorldToScreen(tr.start_pos, sx1, sy1, d1) && WorldToScreen(tr.end_pos, sx2, sy2, d2)) {
            float alpha_pct = std::clamp(tr.timer / tr.max_timer, 0.0f, 1.0f);
            ImU32 tr_col = IM_COL32(255, 220, 100, int(255.0f * alpha_pct));
            draw->AddLine(ImVec2(sx1, sy1), ImVec2(sx2, sy2), tr_col, 2.5f);
        }
    }

    // 2. Draw 3D Player Models & Tags
    for (const auto& pl : players) {
        if (pl.id == my_id || !pl.active || !pl.has_state) continue;

        Vec3 pos(pl.state.pos[0], pl.state.pos[1], pl.state.pos[2]);
        if (pl.interp_alpha < 1.0f) {
            float a = pl.interp_alpha;
            pos.x = pl.prev_pos[0] + (pl.current_pos[0] - pl.prev_pos[0]) * a;
            pos.y = pl.prev_pos[1] + (pl.current_pos[1] - pl.prev_pos[1]) * a;
            pos.z = pl.prev_pos[2] + (pl.current_pos[2] - pl.prev_pos[2]) * a;
        }
        Vec3 fwd(pl.state.forward[0], pl.state.forward[1], pl.state.forward[2]);
        bool is_firing = (pl.state.flags & 1) || pl.just_fired;

        Draw3DPlayerModel(draw, pos, fwd, pl.state.yaw, is_firing, pl.name.c_str(), pl.state.health);

        // Overhead Tag & Health Bar
        bool is_crouching = (pl.state.flags & (1 << 2)) != 0;
        Vec3 tag_pos(pos.x, pos.y + (is_crouching ? 0.05f : 0.35f), pos.z);
        float sx, sy, dist;
        if (WorldToScreen(tag_pos, sx, sy, dist)) {
            char tag[64];
            std::snprintf(tag, sizeof(tag), "[P%u] %s (%.1fm)", pl.id + 1, pl.name.c_str(), dist);
            ImVec2 txt_size = ImGui::CalcTextSize(tag);

            ImVec2 p_min(sx - txt_size.x * 0.5f - 8.0f, sy - txt_size.y - 14.0f);
            ImVec2 p_max(sx + txt_size.x * 0.5f + 8.0f, sy - 2.0f);

            draw->AddRectFilled(p_min, p_max, IM_COL32(15, 15, 15, 210), 4.0f);
            draw->AddRect(p_min, p_max, IM_COL32(218, 178, 87, 230), 4.0f);
            draw->AddText(ImVec2(sx - txt_size.x * 0.5f, sy - txt_size.y - 10.0f), IM_COL32(255, 255, 255, 255), tag);

            // Health bar
            float bar_w = 90.0f;
            float bar_h = 7.0f;
            ImVec2 bar_min(sx - bar_w * 0.5f, sy);
            ImVec2 bar_max(sx + bar_w * 0.5f, sy + bar_h);
            draw->AddRectFilled(bar_min, bar_max, IM_COL32(30, 30, 30, 220), 2.0f);

            float hp_pct = std::clamp(pl.state.health / 100.0f, 0.0f, 1.0f);
            ImVec2 hp_max(bar_min.x + bar_w * hp_pct, bar_max.y);
            ImU32 hp_col = (hp_pct > 0.5f) ? IM_COL32(40, 210, 40, 240) : IM_COL32(230, 40, 40, 240);
            draw->AddRectFilled(bar_min, hp_max, hp_col, 2.0f);
            draw->AddRect(bar_min, bar_max, IM_COL32(0, 0, 0, 255), 2.0f);
        }
    }

    // 3. Floating Damage Numbers (CS 1.6 HitZone Feedback)
    for (const auto& dp : damage_popups_) {
        float sx, sy, dist;
        if (WorldToScreen(dp.world_pos, sx, sy, dist)) {
            float alpha = std::clamp(dp.timer / dp.max_timer, 0.0f, 1.0f);
            int a = static_cast<int>(255.0f * alpha);

            char dmg_str[48];
            const char* zlabel = (dp.zone == HitZone::Head) ? " HEADSHOT!" :
                                 (dp.zone == HitZone::Chest) ? " (Chest)" :
                                 (dp.zone == HitZone::Stomach) ? " (Stomach)" : " (Legs)";
            std::snprintf(dmg_str, sizeof(dmg_str), "-%d%s", int(dp.damage), zlabel);
            ImVec2 sz = ImGui::CalcTextSize(dmg_str);

            ImU32 col = (dp.zone == HitZone::Head) ? IM_COL32(255, 45, 45, a) :
                        (dp.zone == HitZone::Chest) ? IM_COL32(255, 215, 0, a) : IM_COL32(230, 180, 50, a);
            ImU32 shadow = IM_COL32(0, 0, 0, a);

            draw->AddText(ImVec2(sx - sz.x * 0.5f + 1, sy + 1), shadow, dmg_str);
            draw->AddText(ImVec2(sx - sz.x * 0.5f, sy), col, dmg_str);
        }
    }

    // 4. Hitmarker in Screen Center
    if (hitmarker_timer_ > 0.0f) {
        float cx = io.DisplaySize.x * 0.5f;
        float cy = io.DisplaySize.y * 0.5f;
        float hm_size = 11.0f;
        float hm_gap = 4.0f;
        ImU32 hm_col = IM_COL32(255, 45, 45, 255);

        draw->AddLine(ImVec2(cx - hm_size, cy - hm_size), ImVec2(cx - hm_gap, cy - hm_gap), hm_col, 2.2f);
        draw->AddLine(ImVec2(cx + hm_size, cy - hm_size), ImVec2(cx + hm_gap, cy - hm_gap), hm_col, 2.2f);
        draw->AddLine(ImVec2(cx - hm_size, cy + hm_size), ImVec2(cx - hm_gap, cy + hm_gap), hm_col, 2.2f);
        draw->AddLine(ImVec2(cx + hm_size, cy + hm_size), ImVec2(cx + hm_gap, cy + hm_gap), hm_col, 2.2f);
    }

    // 5. Spawn Protection Indicator
    if (spawn_protection_timer_ > 0.0f) {
        char prot_str[48];
        std::snprintf(prot_str, sizeof(prot_str), "SPAWN PROTECTION: %.1fs", spawn_protection_timer_);
        ImVec2 sz = ImGui::CalcTextSize(prot_str);
        float px = (io.DisplaySize.x - sz.x) * 0.5f;
        float py = io.DisplaySize.y - 120.0f;

        draw->AddRectFilled(ImVec2(px - 10, py - 4), ImVec2(px + sz.x + 10, py + sz.y + 4), IM_COL32(10, 10, 10, 180), 4.0f);
        draw->AddRect(ImVec2(px - 10, py - 4), ImVec2(px + sz.x + 10, py + sz.y + 4), IM_COL32(0, 220, 255, 230), 4.0f);
        draw->AddText(ImVec2(px, py), IM_COL32(0, 240, 255, 255), prot_str);
    }

    // 6. CS 1.6 Style Killfeed (Top Right, above scoreboard)
    float kf_y = 15.0f;
    for (const auto& kf : killfeed_entries_) {
        float alpha = std::clamp(kf.timer / 1.0f, 0.0f, 1.0f);
        int a = static_cast<int>(255.0f * alpha);

        char line[128];
        if (kf.headshot) {
            std::snprintf(line, sizeof(line), "%s [HEADSHOT] %s", kf.killer.c_str(), kf.victim.c_str());
        } else {
            std::snprintf(line, sizeof(line), "%s [FRAG] %s", kf.killer.c_str(), kf.victim.c_str());
        }

        ImVec2 sz = ImGui::CalcTextSize(line);
        float kf_x = io.DisplaySize.x - sz.x - 25.0f;

        draw->AddRectFilled(ImVec2(kf_x - 8, kf_y - 3), ImVec2(kf_x + sz.x + 8, kf_y + sz.y + 3), IM_COL32(10, 10, 10, int(190 * alpha)), 4.0f);
        draw->AddRect(ImVec2(kf_x - 8, kf_y - 3), ImVec2(kf_x + sz.x + 8, kf_y + sz.y + 3), kf.headshot ? IM_COL32(255, 50, 50, a) : IM_COL32(184, 148, 77, a), 4.0f);

        draw->AddText(ImVec2(kf_x, kf_y), kf.headshot ? IM_COL32(255, 70, 70, a) : IM_COL32(230, 230, 230, a), line);
        kf_y += 26.0f;
    }

    // 7. Deathmatch Scoreboard (Top Right)
    float sb_w = 280.0f;
    float sb_x = io.DisplaySize.x - sb_w - 20.0f;
    float sb_y = std::max(kf_y + 10.0f, 80.0f);

    ImVec2 sb_min(sb_x, sb_y);
    ImVec2 sb_max(sb_x + sb_w, sb_y + 130.0f);
    draw->AddRectFilled(sb_min, sb_max, IM_COL32(12, 12, 12, 225), 6.0f);
    draw->AddRect(sb_min, sb_max, IM_COL32(184, 148, 77, 230), 6.0f, 0, 1.5f);

    draw->AddText(ImVec2(sb_x + 12.0f, sb_y + 8.0f), IM_COL32(218, 178, 87, 255), "DEATHMATCH - MISSION 1: VEBLENSK");
    draw->AddLine(ImVec2(sb_x + 8.0f, sb_y + 28.0f), ImVec2(sb_x + sb_w - 8.0f, sb_y + 28.0f), IM_COL32(184, 148, 77, 120));

    float row_y = sb_y + 34.0f;
    for (const auto& pl : players) {
        if (!pl.active) continue;
        char line[64];
        std::snprintf(line, sizeof(line), "%s: %u Kills | %u Deaths", pl.name.c_str(), pl.state.frags, pl.state.deaths);
        ImU32 text_col = (pl.id == my_id) ? IM_COL32(255, 215, 0, 255) : IM_COL32(220, 220, 220, 255);
        draw->AddText(ImVec2(sb_x + 12.0f, row_y), text_col, line);
        row_y += 18.0f;
    }

    char dmg_summary[64];
    std::snprintf(dmg_summary, sizeof(dmg_summary), "Damage Dealt: %.0f HP", total_damage_dealt_);
    draw->AddText(ImVec2(sb_x + 12.0f, sb_max.y - 24.0f), IM_COL32(255, 180, 50, 255), dmg_summary);

    // 8. Network status (Bottom Left)
    float ping = net::NetworkManager::Get().GetPing();
    char status_line[64];
    if (net::NetworkManager::Get().GetMode() == net::NetMode::Host) {
        std::snprintf(status_line, sizeof(status_line), "SERVER HOST [PORT %u] | PLAYERS: %zu",
                      net::NetworkManager::Get().GetPort(), players.size());
    } else {
        std::snprintf(status_line, sizeof(status_line), "CONNECTED | PING: %.0f ms", ping);
    }
    draw->AddText(ImVec2(20.0f, io.DisplaySize.y - 30.0f), IM_COL32(80, 255, 80, 230), status_line);
}

void MultiplayerManager::DrawImGuiMenu() {
    auto& net = net::NetworkManager::Get();

    ImGui::TextColored(ImVec4(0.86f, 0.69f, 0.34f, 1.0f), "MULTIPLAYER DEATHMATCH - MISSION 1 (VEBLENSK)");
    ImGui::Separator();
    ImGui::Spacing();

    // Mode Selector
    ImGui::Text("Network Mode:");
    ImGui::SameLine();
    const char* modes[] = {"Offline", "Host (Server)", "Client (Join)"};
    if (ImGui::Combo("##net_mode", &selected_mode_, modes, 3)) {
        // Mode changed
    }

    ImGui::Spacing();
    ImGui::InputText("Player Name", name_buffer_, sizeof(name_buffer_));
    net.SetLocalPlayerName(name_buffer_);

    if (selected_mode_ == 1) { // Host
        ImGui::InputInt("Server Port", &port_);
        if (port_ < 1024) port_ = 1024;
        if (port_ > 65535) port_ = 65535;

        if (net.GetMode() != net::NetMode::Host) {
            if (ImGui::Button("Start Host Server", ImVec2(220, 32))) {
                net.StartHost(static_cast<uint16_t>(port_));
            }
        } else {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Server Running on Port %u", net.GetPort());
            if (ImGui::Button("Stop Server", ImVec2(220, 32))) {
                net.Disconnect();
            }
        }
    } else if (selected_mode_ == 2) { // Client
        ImGui::InputText("Server IP", ip_buffer_, sizeof(ip_buffer_));
        ImGui::InputInt("Server Port", &port_);

        if (net.GetMode() != net::NetMode::Client) {
            if (ImGui::Button("Connect to Server", ImVec2(220, 32))) {
                net.ConnectTo(ip_buffer_, static_cast<uint16_t>(port_));
            }
        } else {
            if (net.IsConnected()) {
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Connected! (Ping: %.0f ms)", net.GetPing());
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Connecting to %s:%d...", ip_buffer_, port_);
            }
            if (ImGui::Button("Disconnect", ImVec2(220, 32))) {
                net.Disconnect();
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.86f, 0.69f, 0.34f, 1.0f), "CO-OP");

    auto& coop = coop::Adapter::Get();
    if (!coop.IsEnabled()) {
        ImGui::TextDisabled("The co-op is the campaign with a partner. It is a session of its own.");
        if (ImGui::Button("Start Co-op Session", ImVec2(220, 32))) {
            coop.Enable();
        }
    } else {
        auto& session = coop.Session();
        const char* phase = "Idle";
        switch (session.GetPhase()) {
            case net::coop::Phase::Idle: phase = "Idle"; break;
            case net::coop::Phase::Lobby: phase = "Lobby"; break;
            case net::coop::Phase::Loading: phase = "Loading"; break;
            case net::coop::Phase::InMission: phase = "In mission"; break;
            case net::coop::Phase::Ended: phase = "Ended"; break;
        }
        ImGui::Text("Session: %s   Round: %u", phase, session.GetStartEpoch());
        const auto& objectives = coop::Mission::Get().ReadObjectives();
        ImGui::Text("Objectives: %u of %u done", objectives.done, objectives.count);
        ImGui::Text("Actors without a pair: %u   Divergences: %u", coop::World::Get().UnpairedCount(),
                    coop::World::Get().DivergentCount());
        if (net.GetMode() == net::NetMode::Host) {
            if (session.GetPhase() == net::coop::Phase::Lobby) {
                if (ImGui::Button("Start Co-op Mission", ImVec2(220, 32))) {
                    coop.StartMission();
                }
            } else {
                ImGui::TextDisabled("The mission is running.");
            }
        } else if (net.GetMode() == net::NetMode::Client) {
            ImGui::TextDisabled("Waiting for the host to start the mission.");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.86f, 0.69f, 0.34f, 1.0f), "CONNECTED PLAYERS");

    auto players = net.GetPlayers();
    if (players.empty() || net.GetMode() == net::NetMode::Offline) {
        ImGui::TextDisabled("No active multiplayer session");
    } else {
        if (ImGui::BeginTable("##players_table", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Endpoint", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Kills", ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableSetupColumn("Health", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableHeadersRow();

            for (const auto& pl : players) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%u", pl.id);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s %s", pl.name.c_str(), (pl.id == net.GetLocalPlayerId()) ? "(You)" : "");
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", pl.endpoint_str.empty() ? "-" : pl.endpoint_str.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%u", pl.state.frags);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%.0f HP", pl.state.health);
            }
            ImGui::EndTable();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.86f, 0.69f, 0.34f, 1.0f), "NETWORK EVENT LOG");

    auto logs = net.GetEventLogs();
    ImGui::BeginChild("##event_log_box", ImVec2(0, 100), true);
    for (const auto& log : logs) {
        ImGui::TextUnformatted(log.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::Spacing();
    if (ImGui::InputText("Chat", chat_buffer_, sizeof(chat_buffer_), ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (chat_buffer_[0]) {
            net.SendChatMessage(chat_buffer_);
            chat_buffer_[0] = '\0';
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Send")) {
        if (chat_buffer_[0]) {
            net.SendChatMessage(chat_buffer_);
            chat_buffer_[0] = '\0';
        }
    }
}

} // namespace game

// Runs on the guest thread at the start of the game's frame tick. UI and network
// code only queue a request; all guest memory writes and the transition call stay
// on this thread.
void DirectMapLaunchTick(X86Context& ctx, uint8_t*) {
    auto& multiplayer = game::MultiplayerManager::Get();
    if (!multiplayer.HasDirectMapLaunchRequest()) return;

    uint32_t game_obj = kernel::R32(layout::kGame);
    if (!game_obj) return;

    // This entry point is intended for the front-end. Do not interrupt a level
    // that is already loading or active.
    const uint32_t game_state = kernel::R32(game_obj + layout::kGameState);
    if (!game_state) return;
    if (game_state != game_obj + layout::kFrontEnd) {
        // Keep the connection request through boot, logos and intro films. The
        // world pointer is initialized before the front-end and cannot identify
        // whether a playable level is active.
        return;
    }
    // PressStart normally selects a controller and binds the game's action map.
    // Direct launch bypasses that screen. Port 0 is the keyboard/mouse virtual
    // controller; use its descriptor only after the engine has initialized it.
    if (!kernel::R32(game_obj + 0x21060)) {
        const uint32_t input_manager = kernel::R32(0x002D45E8);
        if (!input_manager) return;
        const uint32_t controller = input_manager + 0x518;
        if (kernel::R32(controller + 0x0C) != input_manager + 0x98) return;
        kernel::W32(game_obj + 0x21060, controller);
        kernel::G<uint8_t>(controller + 0x08) = 1;
        const uint32_t ui = kernel::R32(layout::kUi);
        if (ui) kernel::W32(ui + layout::kWaitsForStart, 0);
        LOGI("DirectMapLaunchTick: Selected initialized controller on port 0");
    }
    if (!kernel::R32(0x002F3BD4)) {
        X86Context bind = ctx;
        bind.eax = game_obj + 0x20174;
        kernel::CallGuest(bind, 0x001F1550, {});
    }
    if (!multiplayer.ConsumeDirectMapLaunchRequest()) return;

    // Match the values written by the game's own Mission 1 launch routine.
    kernel::G<uint8_t>(game_obj + 0x2020C) = 0; // Level 0: Veblensk
    kernel::G<uint8_t>(game_obj + 0x2020D) = 1;
    kernel::G<uint8_t>(game_obj + 0x2020E) = 1;
    kernel::W32(0x002D0D68, game_obj + 0x20F78);

    LOGI("DirectMapLaunchTick: Starting Veblensk through the engine state transition");
    kernel::CallGuest(ctx, 0x0020DDD0, {0});
}

// Runs immediately before sub_001513C0 dereferences its resource table. Keep
// this diagnostic conditional so normal loader activity does not flood logs.
// Capture the resource header and the active loader state before the null read.
void TraceNullResourceLookup(X86Context& ctx, uint8_t*) {
    if (ctx.edx != 0) return;

    const uint32_t component = ctx.ecx;
    const uint32_t header = component >= 0x10000 ? kernel::R32(component) : 0;
    const uint32_t component_count = component >= 0x10000 ? kernel::R32(component + 0x54) : 0;
    const uint32_t component_table = component >= 0x10000 ? kernel::R32(component + 0x5C) : 0;
    const uint32_t loader_stage = ctx.ebp >= 0x10000 ? kernel::R32(ctx.ebp + 0x854) : 0;
    const uint32_t state = ctx.esi >= 0x10000 ? kernel::R32(ctx.esi) : 0;

    LOGE("ResourceLookup: null table at guest 00151408 index=%08X component=%08X header=%08X count=%08X table=%08X loader=%08X stage=%08X state=%08X esp=%08X",
         ctx.eax, component, header, component_count, component_table, ctx.ebp, loader_stage, state, ctx.esp);
}
