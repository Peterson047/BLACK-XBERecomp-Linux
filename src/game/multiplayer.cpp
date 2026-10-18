#include "game/multiplayer.h"
#include "game/layout.h"
#include "common/aspect.h"
#include "common/options.h"
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
    LOGI("MultiplayerManager: Initialized CS 1.6 Deathmatch System for Veblensk");
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

    auto remote_players = net::NetworkManager::Get().GetPlayers();
    uint8_t local_id = net::NetworkManager::Get().GetLocalPlayerId();
    uint32_t active_puppets = 0;
    for (const auto& rpl : remote_players) {
        if (rpl.id != local_id && rpl.active) {
            active_puppets++;
        }
    }

    // Deactivate and hide all campaign enemies past the active puppets
    for (uint32_t i = active_puppets; i < count && i < 64; ++i) {
        uint32_t enemy = R32(list + i * 4);
        if (enemy) {
            W32(enemy + 0xC4, 0); // Inactive
            G<float>(enemy + 0xA0) = 9999.0f;
            G<float>(enemy + 0xA4) = 9999.0f;
            G<float>(enemy + 0xA8) = 9999.0f;
        }
    }

    // Ensure the AI list only processes the active puppets
    W32(mgr + 0x79B4, active_puppets);
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

bool MultiplayerManager::WorldToScreen(const Vec3& world_pos, float& screen_x, float& screen_y, float& dist) const {
    uint32_t p = Player();
    if (!p) return false;

    Vec3 cam_pos(
        G<float>(p + kPlayerEye),
        G<float>(p + kPlayerEye + 4),
        G<float>(p + kPlayerEye + 8)
    );

    Vec3 cam_fwd = Normalize(Vec3(
        G<float>(p + kPlayerForward),
        G<float>(p + kPlayerForward + 4),
        G<float>(p + kPlayerForward + 8)
    ));

    Vec3 up(0.0f, 1.0f, 0.0f);
    if (std::abs(cam_fwd.y) > 0.99f) {
        up = Vec3(0.0f, 0.0f, 1.0f);
    }
    Vec3 right = Normalize(Cross(cam_fwd, up));
    Vec3 real_up = Normalize(Cross(right, cam_fwd));

    Vec3 rel = world_pos - cam_pos;
    dist = std::sqrt(Dot(rel, rel));

    float z = Dot(rel, cam_fwd);
    if (z <= 0.15f) return false;

    float x = Dot(rel, right);
    float y = Dot(rel, real_up);

    ImGuiIO& io = ImGui::GetIO();
    float screen_w = io.DisplaySize.x;
    float screen_h = io.DisplaySize.y;
    if (screen_w <= 0 || screen_h <= 0) return false;

    float fov_deg = float(std::clamp(game::Opt().fov, 45u, 140u));
    float aspect = screen_w / screen_h;
    float half_fov_rad = (fov_deg * 0.5f) * (3.14159265f / 180.0f);
    float tan_half = std::tan(half_fov_rad);

    screen_x = (screen_w * 0.5f) + (x / (z * tan_half)) * (screen_w * 0.5f);
    screen_y = (screen_h * 0.5f) - (y / (z * tan_half / aspect)) * (screen_h * 0.5f);

    return (screen_x >= -300 && screen_x <= screen_w + 300 && screen_y >= -300 && screen_y <= screen_h + 300);
}

void MultiplayerManager::UpdateEnemyPuppet(uint8_t slot_idx, const Vec3& pos, const Vec3& forward, float yaw, float pitch, float move_rate, bool is_crouching) {
    uint32_t mgr = R32(0x002D1980);
    if (!mgr) return;

    uint32_t list = R32(mgr + 0x79BC);
    uint32_t actor = 0;
    if (list && slot_idx < 32) {
        actor = R32(list + slot_idx * 4);
    }
    if (!actor) {
        actor = mgr + 0xA0 + (slot_idx % 32) * 0x3C0;
    }
    if (!actor) return;

    if (R32(actor) != 0x28C1E8) {
        W32(actor, 0x28C1E8);
    }

    // Set soldier coordinates & ground base (accounting for crouching posture)
    float base_y = pos.y - (is_crouching ? 1.05f : 1.65f);
    G<float>(actor + 0xA0) = pos.x;
    G<float>(actor + 0xA4) = base_y;
    G<float>(actor + 0xA8) = pos.z;
    G<float>(actor + 0xAC) = 1.0f;

    // Stance & Crouch state
    G<uint8_t>(actor + 0x3A1) = is_crouching ? 1 : 0;
    G<uint8_t>(actor + 0x3A2) = 0;
    G<uint8_t>(actor + 0x3A4) = 0;

    // Locomotion and animation controller
    uint32_t loc = R32(actor + 0x324);
    if (loc) {
        W32(loc + 0x90, 0); // Disable campaign AI autonomous movement/logic
        G<uint8_t>(loc + 0x40) = is_crouching ? 1 : 0;
        G<float>(loc + 0x84) = move_rate;
        G<float>(loc + 0x2D8) = move_rate;
        G<float>(loc + 0x80) = yaw;
        G<float>(loc + 0x14) = yaw;
        G<float>(loc + 0x10) = yaw;
        G<float>(loc + 0x1C) = pitch;
    }

    // Mark active and highest detail level
    G<uint8_t>(actor + 0xC8) = 0; // Force LOD 0
    W32(actor + 0xC4, 1);        // Marked active
}

// Check Hit Detection with CS 1.6 Hitbox Zones (Head, Chest, Stomach, Legs)
void MultiplayerManager::CheckHitDetection(const Vec3& origin, const Vec3& dir, uint32_t weapon) {
    auto players = net::NetworkManager::Get().GetPlayers();
    uint8_t my_id = net::NetworkManager::Get().GetLocalPlayerId();

    for (const auto& other : players) {
        if (other.id == my_id || !other.active) continue;

        Vec3 target(other.state.pos[0], other.state.pos[1], other.state.pos[2]);
        Vec3 to_target = target - origin;

        float proj = Dot(to_target, dir);
        if (proj <= 0.0f || proj > 120.0f) continue;

        Vec3 closest_point = origin + (dir * proj);
        Vec3 diff = target - closest_point;
        float dist_sq = Dot(diff, diff);

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

void MultiplayerManager::CheckLocalPlayerShooting(const Vec3& eye, const Vec3& forward, uint32_t weapon) {
    uint32_t p = Player();
    if (!p) return;

    bool is_firing = (R32(p + kPlayerFireRequest) != 0);

    if (is_firing && !was_firing_) {
        // Cancel spawn protection on shooting
        spawn_protection_timer_ = 0.0f;

        float origin[3] = {eye.x, eye.y, eye.z};
        float dir[3] = {forward.x, forward.y, forward.z};
        net::NetworkManager::Get().BroadcastShoot(origin, dir, weapon);
        CheckHitDetection(eye, forward, weapon);

        Vec3 end_pos = eye + (forward * 90.0f);
        AddTracer(eye, end_pos);
    }
    was_firing_ = is_firing;
}

void MultiplayerManager::Update(float dt) {
    net::NetworkManager::Get().Update(dt);

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

    // Remove singleplayer AI wave enemies from Mission 1
    ClearCampaignEnemies();

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
    bool firing = (R32(p + kPlayerFireRequest) != 0);
    bool aiming = (G<float>(p + kPlayerZoom) > 1.05f);

    CheckLocalPlayerShooting(eye, forward, weapon);

    // Update puppets for active remote players with GoldSrc entity interpolation
    auto remote_players = net::NetworkManager::Get().GetPlayers();
    uint8_t local_id = net::NetworkManager::Get().GetLocalPlayerId();
    uint8_t puppet_slot = 0;
    for (const auto& rpl : remote_players) {
        if (rpl.id != local_id && rpl.active) {
            Vec3 rpos(rpl.state.pos[0], rpl.state.pos[1], rpl.state.pos[2]);
            if (rpl.interp_alpha < 1.0f) {
                float a = rpl.interp_alpha;
                rpos.x = rpl.prev_pos[0] + (rpl.current_pos[0] - rpl.prev_pos[0]) * a;
                rpos.y = rpl.prev_pos[1] + (rpl.current_pos[1] - rpl.prev_pos[1]) * a;
                rpos.z = rpl.prev_pos[2] + (rpl.current_pos[2] - rpl.prev_pos[2]) * a;
            }
            Vec3 rfwd(rpl.state.forward[0], rpl.state.forward[1], rpl.state.forward[2]);

            // Calculate movement rate (speed)
            Vec3 delta = rpos - Vec3(rpl.prev_pos[0], rpl.prev_pos[1], rpl.prev_pos[2]);
            float move_dist = std::sqrt(delta.x * delta.x + delta.z * delta.z);
            float move_rate = std::clamp(move_dist / (dt * 4.5f), 0.0f, 1.0f);

            bool r_crouching = (rpl.state.flags & (1 << 2)) != 0;
            float r_yaw = rpl.state.yaw;
            float r_pitch = rpl.state.pitch;

            UpdateEnemyPuppet(puppet_slot++, rpos, rfwd, r_yaw, r_pitch, move_rate, r_crouching);

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
        data.flags = (firing ? 1 : 0) | (aiming ? 2 : 0) | (is_crouching ? (1 << 2) : 0);
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
        if (pl.id == my_id || !pl.active) continue;

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
