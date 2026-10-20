#include "game/multiplayer.h"

#include <array>
#include <cmath>
#include <imgui.h>

#include "common/menu.h"
#include "game/layout.h"

namespace game {

void MultiplayerManager::DrawAdminDebug() {
    auto& network = net::NetworkManager::Get();
    const bool authorized = network.GetMode() == net::NetMode::Host && IsInGame();
    if (!authorized) {
        if (app::AdminDebugOpen()) app::SetAdminDebugOpen(false);
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F8, false) && !app::Capturing()) {
        app::SetAdminDebugOpen(!app::AdminDebugOpen());
    }
    if (!app::AdminDebugOpen()) {
        ImGui::SetNextWindowPos(ImVec2(12, 78), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.65f);
        if (ImGui::Begin("##admin_hint", nullptr, ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextUnformatted("F8  ADMIN DEBUG");
        }
        ImGui::End();
        return;
    }

    auto players = network.GetPlayers();
    // A short sampling window avoids reporting a stop between individual state packets.
    struct MotionSample {
        bool valid = false;
        float pos[3]{};
        float speed = 0.0f;
    };
    static std::array<MotionSample, net::kMaxPlayers> motion{};
    static double previous_sample = 0.0;
    const double now = ImGui::GetTime();
    const double elapsed = now - previous_sample;
    if (elapsed >= 0.2) {
        std::array<bool, net::kMaxPlayers> seen{};
        for (const auto& player : players) {
            if (player.id >= motion.size()) continue;
            auto& sample = motion[player.id];
            const auto& pos = player.state.pos;
            const float dx = pos[0] - sample.pos[0];
            const float dy = pos[1] - sample.pos[1];
            const float dz = pos[2] - sample.pos[2];
            sample.speed = sample.valid && elapsed < 1.0
                ? std::sqrt(dx * dx + dy * dy + dz * dz) / float(elapsed) : 0.0f;
            for (int i = 0; i < 3; ++i) sample.pos[i] = pos[i];
            sample.valid = true;
            seen[player.id] = true;
        }
        for (size_t i = 0; i < motion.size(); ++i) {
            if (!seen[i]) motion[i] = {};
        }
        previous_sample = now;
    }

    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.82f, viewport->WorkSize.y * 0.78f),
                             ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("Multiplayer - Admin Debug", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("Host UDP %u | %zu players | F8 / Escape to close", network.GetPort(), players.size());
        ImGui::TextWrapped("Live session: other players keep playing while this panel is open.");
        ImGui::SeparatorText("Native replication diagnostics");
        const uint32_t actors = kernel::R32(0x002D1980);
        if (actors) {
            ImGui::Text("Actor container: %u used / %u capacity", kernel::R32(actors + 0x79B8),
                        kernel::R32(actors + 0x79B4));
        }
        ImGui::Text("Confirmed local shots: %u | queued: %u",
                    confirmed_shots_.load(std::memory_order_relaxed),
                    pending_shots_.load(std::memory_order_relaxed));
        const uint32_t world = kernel::R32(layout::kWorld);
        if (world) {
            const uint32_t player = world + layout::kWorldPlayer;
            const uint32_t weapon = kernel::R32(player + 0x29C);
            if (weapon) {
                const uint32_t magazine = kernel::R32(weapon + 0xF4);
                ImGui::Text("Native weapon state %u | ammo %u", kernel::R32(weapon + 0xD8),
                            magazine ? unsigned(kernel::G<uint16_t>(magazine + 0x18)) : 0u);
            }
        }
        ImGui::TextWrapped("Native skeletal firing and ally targeting still require validation. Shot count tracks magazine consumption.");
        ImGui::SeparatorText("Combat");
        bool damage = network.IsDamageEnabled();
        if (ImGui::Checkbox("Player damage", &damage)) network.SetDamageEnabled(damage);
        float scale = network.GetDamageScale();
        if (ImGui::SliderFloat("Damage multiplier", &scale, 0.0f, 5.0f, "%.2fx")) {
            network.SetDamageScale(scale);
        }
        if (ImGui::Button("Reset kills / deaths")) network.AdminResetStats();
        ImGui::SameLine();
        if (ImGui::Button("Heal all players")) {
            for (const auto& player : players) network.AdminHealPlayer(player.id);
        }

        ImGui::SeparatorText("Players and replication");
        for (const auto& player : players) {
            ImGui::PushID(int(player.id));
            if (ImGui::TreeNodeEx("player", ImGuiTreeNodeFlags_DefaultOpen, "P%u %s%s", player.id,
                    player.name.c_str(), player.id == network.GetLocalPlayerId() ? " (host / admin)" : "")) {
                const auto& state = player.state;
                ImGui::Text("HP %.1f | Kills %u | Deaths %u | Ping %.0f ms", state.health,
                            state.frags, state.deaths, player.ping_ms);
                ImGui::SameLine();
                if (ImGui::SmallButton("Heal")) network.AdminHealPlayer(player.id);
                if (player.id != network.GetLocalPlayerId()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Test hit: 10 HP")) {
                        network.SendHit(player.id, 10.0f, player.state.pos);
                    }
                }
                ImGui::Text("Position %.2f, %.2f, %.2f | Speed %.2f units/s", state.pos[0],
                    state.pos[1], state.pos[2], player.id < motion.size() ? motion[player.id].speed : 0.0f);
                ImGui::Text("Yaw %.2f | Pitch %.2f | Weapon %u | Flags 0x%08X", state.yaw,
                            state.pitch, state.weapon_id, state.flags);
                ImGui::Text("Firing %s | Aiming %s | Crouching %s | Reloading %s",
                    state.flags & 1 ? "yes" : "no", state.flags & 2 ? "yes" : "no",
                    state.flags & 4 ? "yes" : "no", state.flags & 8 ? "yes" : "no");
                ImGui::Text("Endpoint: %s", player.endpoint_str.empty() ? "local" : player.endpoint_str.c_str());
                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        ImGui::SeparatorText("Connection events");
        ImGui::BeginChild("events", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 6), true);
        const bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY();
        for (const auto& event : network.GetEventLogs()) ImGui::TextUnformatted(event.c_str());
        if (at_bottom) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        if (ImGui::Button("Close (F8)")) open = false;
    }
    ImGui::End();
    if (!open) app::SetAdminDebugOpen(false);
}

} // namespace game
