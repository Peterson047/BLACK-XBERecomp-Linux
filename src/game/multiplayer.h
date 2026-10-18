#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "net/network.h"
#include "xdk/hle.h"

namespace game {

// Represents a 3D coordinate in game space
struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
};

// Hitbox anatomical zones (CS 1.6 formula)
enum class HitZone : uint8_t {
    Legs = 0,
    Stomach = 1,
    Chest = 2,
    Head = 3
};

// Floating damage number displayed on hit
struct DamagePopup {
    Vec3 world_pos;
    float damage;
    float timer;
    float max_timer;
    HitZone zone;
};

// Visual bullet tracer in 3D world space
struct TracerEffect {
    Vec3 start_pos;
    Vec3 end_pos;
    float timer;
    float max_timer;
};

// CS 1.6 Style Killfeed entry
struct KillfeedEntry {
    std::string killer;
    std::string victim;
    bool headshot;
    float timer; // 5.0s display
};

// Total spawn points distributed across Mission 1 (Veblensk City Street)
constexpr size_t kNumSpawns = 10;

class MultiplayerManager {
public:
    static MultiplayerManager& Get();

    void Init();
    void Update(float dt);
    void DrawOverlay();
    void DrawImGuiMenu();
    void DrawImGuiHUD();

    bool IsInGame() const;
    void TeleportPlayer(const Vec3& pos);
    void RespawnLocalPlayer();

    void AddDamagePopup(const Vec3& pos, float damage, HitZone zone);
    void AddTracer(const Vec3& start, const Vec3& end);
    void AddKillfeed(const std::string& killer, const std::string& victim, bool headshot);
    void TriggerHitmarker();

    // Stats
    uint32_t GetLocalFrags() const { return local_frags_; }
    uint32_t GetLocalDeaths() const { return local_deaths_; }
    float GetTotalDamageDealt() const { return total_damage_dealt_; }
    bool HasSpawnProtection() const { return spawn_protection_timer_ > 0.0f; }

private:
    MultiplayerManager();
    ~MultiplayerManager();

    bool WorldToScreen(const Vec3& world_pos, float& screen_x, float& screen_y, float& dist) const;
    void CheckLocalPlayerShooting(const Vec3& eye, const Vec3& forward, uint32_t weapon);
    void CheckHitDetection(const Vec3& origin, const Vec3& dir, uint32_t weapon);

    void ClearCampaignEnemies();
    void Draw3DPlayerModel(void* draw_list, const Vec3& pos, const Vec3& forward, float yaw, bool is_firing, const char* name, float health);
    void UpdateEnemyPuppet(uint8_t slot_idx, const Vec3& pos, const Vec3& forward, float yaw, float pitch, float move_rate, bool is_crouching);

    float local_health_ = 100.0f;
    uint32_t local_frags_ = 0;
    uint32_t local_deaths_ = 0;
    float total_damage_dealt_ = 0.0f;
    bool was_firing_ = false;
    float state_send_timer_ = 0.0f;
    float spawn_protection_timer_ = 0.0f;

    // Visual combat feedback
    float hitmarker_timer_ = 0.0f;
    std::vector<DamagePopup> damage_popups_;
    std::vector<TracerEffect> bullet_tracers_;
    std::vector<KillfeedEntry> killfeed_entries_;

    // UI state for the ImGui multiplayer tab
    int selected_mode_ = 0; // 0: None, 1: Host, 2: Client
    char ip_buffer_[64] = "127.0.0.1";
    int port_ = net::kDefaultPort;
    char name_buffer_[32] = "Player 1";
    char chat_buffer_[64] = "";
    bool auto_respawn_ = true;
};

} // namespace game
