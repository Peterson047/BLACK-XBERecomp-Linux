#include "game/coop_adapter.h"
#include "game/coop_mission.h"
#include "game/coop_world.h"
#include "game/multiplayer.h"
#include "game/layout.h"
#include "common/menu.h"
#include "core/log.h"
#include "xdk/hle.h"

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

static uint32_t Player() {
    uint32_t w = R32(kWorld);
    return w ? w + kWorldPlayer : 0;
}

} // namespace

const char* CoopPhaseName(net::coop::Phase phase) {
    switch (phase) {
        case net::coop::Phase::Idle: return "Idle";
        case net::coop::Phase::Lobby: return "Lobby";
        case net::coop::Phase::Loading: return "Loading";
        case net::coop::Phase::InMission: return "In mission";
        case net::coop::Phase::Ended: return "Ended";
    }
    return "Idle";
}

MultiplayerManager& MultiplayerManager::Get() {
    static MultiplayerManager instance;
    return instance;
}

void MultiplayerManager::Init() {
    direct_map_launch_requested_.store(false, std::memory_order_release);
    LOGI("MultiplayerManager: the co-op transport is ready");
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

// The engine's own shot: the magazine went down. The partner is told from
// the tick that draws, so the socket is not touched from the engine's thread.
void MultiplayerManager::NotifyLocalShot(const Vec3&, const Vec3&, uint32_t) {
    if (app::MenuOpen()) return;
    shoot_pending_.store(true, std::memory_order_relaxed);
}

void MultiplayerManager::Update(float dt) {
    auto& network = net::NetworkManager::Get();
    network.Update(dt);
    shot_flash_timer_ = std::max(0.0f, shot_flash_timer_ - dt);

    for (const auto& player : network.GetPlayers()) {
        if (player.id == network.GetLocalPlayerId()) {
            local_health_ = player.state.health;
        }
    }

    if (!IsInGame()) return;
    if (!network.IsConnected()) return;

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
    bool aiming = (G<float>(p + kPlayerZoom) > 1.05f);

    // The shot the engine made: the partner is told, and the flag of the
    // state says that there was one.
    if (shoot_pending_.exchange(false, std::memory_order_relaxed)) {
        shot_flash_timer_ = 0.12f;
        const float origin[3] = {eye.x, eye.y, eye.z};
        const float dir[3] = {forward.x, forward.y, forward.z};
        network.BroadcastShoot(origin, dir, weapon);
    }
    bool firing = shot_flash_timer_ > 0.0f;

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

        network.SendLocalState(data);
    }
}

void MultiplayerManager::DrawImGuiHUD() {
    if (!coop::Adapter::Get().IsEnabled()) return;
    if (!IsInGame() || !net::NetworkManager::Get().IsConnected()) return;

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (!draw) return;

    DrawCoopHUD(draw);
}

// The co-op panel: the partner, the mission, and whether the two copies of
// the world still agree.
void MultiplayerManager::DrawCoopHUD(void* draw_list) {
    auto* draw = static_cast<ImDrawList*>(draw_list);
    if (!draw) return;
    ImGuiIO& io = ImGui::GetIO();
    auto& session = coop::Adapter::Get().Session();
    auto& world = coop::World::Get();
    auto& mission = coop::Mission::Get();

    const auto& objectives = mission.ReadObjectives();
    const char* phase = CoopPhaseName(session.GetPhase());

    // The partner: the other player of the session.
    float partner_y = io.DisplaySize.y * 0.5f - 90.0f;
    for (const auto& pl : net::NetworkManager::Get().GetPlayers()) {
        if (pl.id == net::NetworkManager::Get().GetLocalPlayerId() || !pl.active) continue;
        char line[128];
        std::snprintf(line, sizeof(line), "PARTNER  %s   %.0f HP", pl.name.c_str(), pl.state.health);
        draw->AddText(ImVec2(20.0f, partner_y), IM_COL32(120, 200, 255, 230), line);
        partner_y += 20.0f;
    }

    // The mission and the state of the two copies of the world.
    char mission_line[160];
    std::snprintf(mission_line, sizeof(mission_line), "CO-OP  %s   ROUND %u   OBJECTIVES %u/%u", phase,
                  session.GetStartEpoch(), objectives.done, objectives.count);
    draw->AddText(ImVec2(20.0f, partner_y), IM_COL32(230, 210, 140, 230), mission_line);
    partner_y += 20.0f;

    // A divergence is not something to hide: the two copies of the world
    // have to agree, and when they do not, the player has to know.
    const uint32_t unpaired = world.UnpairedCount() + mission.Unpaired();
    const uint32_t divergent = world.DivergentCount() + mission.Divergent() + session.GetDivergenceCount();
    if (unpaired || divergent) {
        char warn[128];
        std::snprintf(warn, sizeof(warn), "CO-OP  UNPAIRED %u   DIVERGENT %u", unpaired, divergent);
        draw->AddText(ImVec2(20.0f, partner_y), IM_COL32(255, 120, 120, 240), warn);
    }
}

// The co-op page of the menu: the session, the mission and the chat.
void MultiplayerManager::DrawImGuiMenu() {
    auto& net = net::NetworkManager::Get();

    ImGui::TextColored(ImVec4(0.86f, 0.69f, 0.34f, 1.0f), "CO-OP");
    ImGui::Separator();
    ImGui::Spacing();

    auto& coop = coop::Adapter::Get();

    // What this machine does: it waits for a partner, or it goes looking for
    // one. The co-op is the only mode, so this is the whole page.
    ImGui::Text("This machine:");
    ImGui::SameLine();
    const char* roles[] = {"Host (wait for a partner)", "Join a partner"};
    if (ImGui::Combo("##coop_role", &coop_role_, roles, 2)) {
        // the choice changed
    }
    ImGui::InputText("Your name", name_buffer_, sizeof(name_buffer_));
    net.SetLocalPlayerName(name_buffer_);

    if (coop_role_ == 0) {
        ImGui::InputInt("Port", &port_);
        if (port_ < 1024) port_ = 1024;
        if (port_ > 65535) port_ = 65535;
        if (net.GetMode() != net::NetMode::Host) {
            if (ImGui::Button("Host a co-op session", ImVec2(220, 32))) {
                coop.Enable();
                net.StartHost(static_cast<uint16_t>(port_));
            }
        } else {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Hosting on port %u", net.GetPort());
            if (ImGui::Button("Stop hosting", ImVec2(220, 32))) {
                net.Disconnect();
            }
        }
    } else {
        ImGui::InputText("Partner's address", ip_buffer_, sizeof(ip_buffer_));
        ImGui::InputInt("Port", &port_);
        if (port_ < 1024) port_ = 1024;
        if (port_ > 65535) port_ = 65535;
        if (net.GetMode() != net::NetMode::Client) {
            if (ImGui::Button("Join a co-op session", ImVec2(220, 32))) {
                coop.Enable();
                net.ConnectTo(ip_buffer_, static_cast<uint16_t>(port_));
            }
        } else {
            if (net.IsConnected()) {
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Connected! (Ping: %.0f ms)", net.GetPing());
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Connecting to %s:%d...", ip_buffer_, port_);
            }
            if (ImGui::Button("Leave the session", ImVec2(220, 32))) {
                net.Disconnect();
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    if (!coop.IsEnabled()) {
        ImGui::TextDisabled("The co-op is the campaign with a partner. It is a session of its own.");
    } else {
        auto& session = coop.Session();
        ImGui::Text("Session: %s   Round: %u", CoopPhaseName(session.GetPhase()), session.GetStartEpoch());
        const auto& objectives = coop::Mission::Get().ReadObjectives();
        ImGui::Text("Objectives: %u of %u done", objectives.done, objectives.count);
        for (const auto& pl : net.GetPlayers()) {
            if (pl.id == net.GetLocalPlayerId() || !pl.active) continue;
            ImGui::Text("Partner: %s   %.0f HP", pl.name.c_str(), pl.state.health);
        }
        ImGui::Text("Actors without a pair: %u   Divergences: %u",
                    coop::World::Get().UnpairedCount() + coop::Mission::Get().Unpaired(),
                    coop::World::Get().DivergentCount() + coop::Mission::Get().Divergent() +
                        session.GetDivergenceCount());
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
    ImGui::TextColored(ImVec4(0.86f, 0.69f, 0.34f, 1.0f), "CHAT");

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
         ctx.eax, component, header, component_count, component_table, loader_stage, state, ctx.esp);
}
