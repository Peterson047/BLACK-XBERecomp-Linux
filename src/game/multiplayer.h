#pragma once

#include <cstdint>
#include <atomic>
#include "net/coop_protocol.h"
#include "net/network.h"
#include "xdk/hle.h"

namespace game {

// Represents a 3D coordinate in game space
struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
};

// The name of a phase of the co-op session, for the panel and the window.
const char* CoopPhaseName(net::coop::Phase phase);

class MultiplayerManager {
public:
    static MultiplayerManager& Get();

    void Init();
    void Update(float dt);
    void RequestDirectMapLaunch();
    bool HasDirectMapLaunchRequest() const;
    bool ConsumeDirectMapLaunchRequest();
    void DrawImGuiMenu();
    void DrawImGuiHUD();
    // The co-op panel: the partner, the mission and the state of the two
    // copies of the world.
    void DrawCoopHUD(void* draw_list);
    void DrawAdminDebug();
    // The engine's own shot: the partner has to know that there was one.
    void NotifyLocalShot(const Vec3& origin, const Vec3& direction, uint32_t weapon);

    bool IsInGame() const;

private:
    // The life of the local player, as the transport carries it.
    float local_health_ = 100.0f;
    // How long the last shot still counts as a shot.
    float shot_flash_timer_ = 0.0f;
    float state_send_timer_ = 0.0f;
    // The engine fires on its own thread. The flag is the news, and the tick
    // that draws is what tells the partner.
    std::atomic<bool> shoot_pending_{false};
    std::atomic<bool> direct_map_launch_requested_{false};
    // The chat of the co-op panel.
    char chat_buffer_[64] = "";
    // the co-op page: this machine hosts, or joins a partner
    int coop_role_ = 0;
    char ip_buffer_[64] = "127.0.0.1";
    int port_ = net::kDefaultPort;
    char name_buffer_[32] = "Player";
};

} // namespace game
