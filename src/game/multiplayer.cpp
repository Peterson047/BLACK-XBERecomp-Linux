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

// Spawn coordinates for Mission 1 (Veblensk City Street)
const Vec3 kMission1Spawns[kNumSpawns] = {
    Vec3(0.0f, 0.0f, 0.0f),       // Spawn 0: Start of level
    Vec3(15.0f, 0.0f, 25.0f),     // Spawn 1: Street advance
    Vec3(-10.0f, 0.0f, 40.0f),    // Spawn 2: Beside barricades
    Vec3(5.0f, 0.0f, 60.0f)       // Spawn 3: Near plaza
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
    LOGI("MultiplayerManager: Initialized");
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

void MultiplayerManager::RespawnLocalPlayer() {
    local_health_ = 100.0f;
    local_deaths_++;
    uint8_t id = net::NetworkManager::Get().GetLocalPlayerId();
    size_t spawn_idx = id % kNumSpawns;
    // Don't teleport if spawn 0 is default 0,0,0 and level has custom origin
    if (spawn_idx != 0) {
        TeleportPlayer(kMission1Spawns[spawn_idx]);
    }
    net::NetworkManager::Get().LogEvent("You respawned at point " + std::to_string(spawn_idx));
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

    Vec3 rel(world_pos.x - cam_pos.x, world_pos.y - cam_pos.y, world_pos.z - cam_pos.z);
    dist = std::sqrt(Dot(rel, rel));

    float z = Dot(rel, cam_fwd);
    if (z <= 0.2f) return false; // Behind camera

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

    return (screen_x >= -100 && screen_x <= screen_w + 100 && screen_y >= -100 && screen_y <= screen_h + 100);
}

void MultiplayerManager::CheckHitDetection(const Vec3& origin, const Vec3& dir, uint32_t weapon) {
    auto players = net::NetworkManager::Get().GetPlayers();
    uint8_t my_id = net::NetworkManager::Get().GetLocalPlayerId();

    for (const auto& other : players) {
        if (other.id == my_id || !other.active) continue;

        Vec3 target(other.state.pos[0], other.state.pos[1], other.state.pos[2]);
        Vec3 to_target(target.x - origin.x, target.y - origin.y, target.z - origin.z);

        float proj = Dot(to_target, dir);
        if (proj <= 0.0f || proj > 100.0f) continue; // Behind or out of range

        Vec3 closest_point(origin.x + dir.x * proj, origin.y + dir.y * proj, origin.z + dir.z * proj);
        Vec3 diff(target.x - closest_point.x, target.y - closest_point.y, target.z - closest_point.z);
        float dist_sq = Dot(diff, diff);

        // Player hitbox radius: 0.9m
        if (dist_sq < (0.9f * 0.9f)) {
            float damage = 34.0f; // 3 hits kill
            float hit_pos[3] = {closest_point.x, closest_point.y, closest_point.z};
            net::NetworkManager::Get().SendHit(other.id, damage, hit_pos);
            net::NetworkManager::Get().LogEvent("HIT on " + other.name + "! (" + std::to_string(int(damage)) + " damage)");
            break;
        }
    }
}

void MultiplayerManager::CheckLocalPlayerShooting(const Vec3& eye, const Vec3& forward, uint32_t weapon) {
    uint32_t p = Player();
    if (!p) return;

    bool is_firing = (R32(p + kPlayerFireRequest) != 0);

    if (is_firing && !was_firing_) {
        // Player pressed fire trigger
        float origin[3] = {eye.x, eye.y, eye.z};
        float dir[3] = {forward.x, forward.y, forward.z};
        net::NetworkManager::Get().BroadcastShoot(origin, dir, weapon);
        CheckHitDetection(eye, forward, weapon);
    }
    was_firing_ = is_firing;
}

void MultiplayerManager::Update(float dt) {
    net::NetworkManager::Get().Update(dt);

    if (!IsInGame()) return;
    if (!net::NetworkManager::Get().IsConnected()) return;

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
        data.weapon_id = weapon;
        data.flags = (firing ? 1 : 0) | (aiming ? 2 : 0);
        data.health = local_health_;
        data.frags = static_cast<uint16_t>(local_frags_);
        data.deaths = static_cast<uint16_t>(local_deaths_);

        net::NetworkManager::Get().SendLocalState(data);
    }
}

void MultiplayerManager::DrawOverlay() {
    // Called during guest DrawOverlay
}

void MultiplayerManager::DrawImGuiHUD() {
    if (!IsInGame() || !net::NetworkManager::Get().IsConnected()) return;

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (!draw) return;

    auto players = net::NetworkManager::Get().GetPlayers();
    uint8_t my_id = net::NetworkManager::Get().GetLocalPlayerId();

    // 1. Draw Remote Players tags & health bars in 3D world space
    for (const auto& pl : players) {
        if (pl.id == my_id || !pl.active) continue;

        Vec3 head_pos(pl.state.pos[0], pl.state.pos[1] + 0.3f, pl.state.pos[2]);
        float sx = 0, sy = 0, dist = 0;

        if (WorldToScreen(head_pos, sx, sy, dist)) {
            // Draw Player Tag
            char tag[64];
            std::snprintf(tag, sizeof(tag), "[P%u] %s (%.1fm)", pl.id + 1, pl.name.c_str(), dist);
            ImVec2 txt_size = ImGui::CalcTextSize(tag);

            ImVec2 p_min(sx - txt_size.x * 0.5f - 6.0f, sy - txt_size.y - 12.0f);
            ImVec2 p_max(sx + txt_size.x * 0.5f + 6.0f, sy - 2.0f);

            // Tag background
            draw->AddRectFilled(p_min, p_max, IM_COL32(20, 20, 20, 200), 4.0f);
            draw->AddRect(p_min, p_max, IM_COL32(210, 180, 90, 230), 4.0f);
            draw->AddText(ImVec2(sx - txt_size.x * 0.5f, sy - txt_size.y - 8.0f), IM_COL32(255, 255, 255, 255), tag);

            // Health bar
            float bar_w = 80.0f;
            float bar_h = 6.0f;
            ImVec2 bar_min(sx - bar_w * 0.5f, sy);
            ImVec2 bar_max(sx + bar_w * 0.5f, sy + bar_h);
            draw->AddRectFilled(bar_min, bar_max, IM_COL32(40, 40, 40, 200));

            float hp_pct = std::clamp(pl.state.health / 100.0f, 0.0f, 1.0f);
            ImVec2 hp_max(bar_min.x + bar_w * hp_pct, bar_max.y);
            ImU32 hp_col = (hp_pct > 0.5f) ? IM_COL32(50, 205, 50, 230) : IM_COL32(220, 50, 50, 230);
            draw->AddRectFilled(bar_min, hp_max, hp_col);
            draw->AddRect(bar_min, bar_max, IM_COL32(0, 0, 0, 255));

            // Firing indicator
            if (pl.just_fired) {
                draw->AddCircle(ImVec2(sx, sy - 20.0f), 12.0f, IM_COL32(255, 80, 0, 255), 16, 2.0f);
                draw->AddText(ImVec2(sx - 18.0f, sy - 40.0f), IM_COL32(255, 100, 0, 255), "FIRING!");
            }
        }
    }

    // 2. Deathmatch Scoreboard (Top Right)
    ImGuiIO& io = ImGui::GetIO();
    float sb_w = 260.0f;
    float sb_x = io.DisplaySize.x - sb_w - 20.0f;
    float sb_y = 20.0f;

    ImVec2 sb_min(sb_x, sb_y);
    ImVec2 sb_max(sb_x + sb_w, sb_y + 110.0f);
    draw->AddRectFilled(sb_min, sb_max, IM_COL32(15, 15, 15, 220), 4.0f);
    draw->AddRect(sb_min, sb_max, IM_COL32(184, 148, 77, 230), 4.0f);

    draw->AddText(ImVec2(sb_x + 10.0f, sb_y + 8.0f), IM_COL32(218, 178, 87, 255), "DEATHMATCH - MISSION 1");
    draw->AddLine(ImVec2(sb_x + 8.0f, sb_y + 26.0f), ImVec2(sb_x + sb_w - 8.0f, sb_y + 26.0f), IM_COL32(184, 148, 77, 100));

    float row_y = sb_y + 32.0f;
    for (const auto& pl : players) {
        if (!pl.active) continue;
        char line[64];
        std::snprintf(line, sizeof(line), "%s: %u Kills / %u Deaths", pl.name.c_str(), pl.state.frags, pl.state.deaths);
        ImU32 text_col = (pl.id == my_id) ? IM_COL32(255, 215, 0, 255) : IM_COL32(220, 220, 220, 255);
        draw->AddText(ImVec2(sb_x + 12.0f, row_y), text_col, line);
        row_y += 18.0f;
    }

    // 3. Connection Status / Ping indicator (Bottom Left)
    float ping = net::NetworkManager::Get().GetPing();
    char status_line[64];
    if (net::NetworkManager::Get().GetMode() == net::NetMode::Host) {
        std::snprintf(status_line, sizeof(status_line), "SERVER (HOST) - PORT %u | PLAYERS: %zu",
                      net::NetworkManager::Get().GetPort(), players.size());
    } else {
        std::snprintf(status_line, sizeof(status_line), "CONNECTED - PING: %.0f ms", ping);
    }
    draw->AddText(ImVec2(20.0f, io.DisplaySize.y - 30.0f), IM_COL32(100, 255, 100, 230), status_line);
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
    int current_mode = static_cast<int>(net.GetMode());
    if (ImGui::Combo("##net_mode", &selected_mode_, modes, 3)) {
        // Mode changed in UI
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
