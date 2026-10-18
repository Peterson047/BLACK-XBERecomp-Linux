#pragma once

#include <cstdint>
#include <string>
#include "net/network.h"
#include "xdk/hle.h"

namespace game {

// Represents a 3D coordinate in game space
struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

// Spawn points for Deathmatch on Mission 1 (Veblensk City Street)
constexpr size_t kNumSpawns = 4;

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

    // Stats
    uint32_t GetLocalFrags() const { return local_frags_; }
    uint32_t GetLocalDeaths() const { return local_deaths_; }

private:
    MultiplayerManager();
    ~MultiplayerManager();

    bool WorldToScreen(const Vec3& world_pos, float& screen_x, float& screen_y, float& dist) const;
    void CheckLocalPlayerShooting(const Vec3& eye, const Vec3& forward, uint32_t weapon);
    void CheckHitDetection(const Vec3& origin, const Vec3& dir, uint32_t weapon);

    float local_health_ = 100.0f;
    uint32_t local_frags_ = 0;
    uint32_t local_deaths_ = 0;
    bool was_firing_ = false;
    float state_send_timer_ = 0.0f;

    // UI state for the ImGui multiplayer tab
    int selected_mode_ = 0; // 0: None, 1: Host, 2: Client
    char ip_buffer_[64] = "127.0.0.1";
    int port_ = net::kDefaultPort;
    char name_buffer_[32] = "Player 1";
    char chat_buffer_[64] = "";
    bool auto_respawn_ = true;
};

} // namespace game
