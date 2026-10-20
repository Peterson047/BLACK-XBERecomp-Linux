#include "game/multiplayer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>
#include <imgui.h>

#include "app/app.h"
#include "common/aspect.h"
#include "gpu/state.h"
#include "gpu/texture.h"
#include "xdk/d3d8.h"

namespace game {
namespace {

struct CameraSnapshot {
    bool valid = false;
    std::array<float, 16> view{}, projection{};
    gpu::Viewport viewport{};
    uint32_t target_width = 0, target_height = 0;
    std::chrono::steady_clock::time_point captured{};
};

std::mutex camera_mutex;
CameraSnapshot camera;

std::array<float, 4> Transform(const std::array<float, 4>& point, const std::array<float, 16>& matrix) {
    std::array<float, 4> result{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) result[col] += point[row] * matrix[row * 4 + col];
    }
    return result;
}

} // namespace

// Called immediately after the title installs its camera matrices in sub_0002B830.
// The render thread subsequently changes these transforms for HUD and other passes.
void CaptureMultiplayerCamera() {
    CameraSnapshot next;
    {
        std::lock_guard lock(d3d::Lock());
        const auto& state = d3d::State();
        next.view = state.transforms[2];
        next.projection = state.transforms[3];
        // Ignore orthographic HUD cameras; retain the latest world perspective camera.
        if (std::abs(next.projection[11] - 1.0f) > 0.00001f ||
                std::abs(next.projection[15]) > 0.00001f) return;
        next.viewport = state.viewport;
        if (!state.rt.header) return;
        gpu::SurfaceSize(state.rt, next.target_width, next.target_height);
    }
    for (const auto value : next.view) if (!std::isfinite(value)) return;
    for (const auto value : next.projection) if (!std::isfinite(value)) return;
    if (!next.viewport.w || !next.viewport.h || !next.target_width || !next.target_height) return;
    next.valid = true;
    next.captured = std::chrono::steady_clock::now();
    std::lock_guard lock(camera_mutex);
    camera = next;
}

bool MultiplayerManager::WorldToScreen(const Vec3& world_pos, float& screen_x, float& screen_y, float& dist) const {
    CameraSnapshot snapshot;
    {
        std::lock_guard lock(camera_mutex);
        snapshot = camera;
    }
    if (!snapshot.valid || std::chrono::steady_clock::now() - snapshot.captured > std::chrono::seconds(1)) {
        return false;
    }
    const auto view = Transform({world_pos.x, world_pos.y, world_pos.z, 1.0f}, snapshot.view);
    const auto clip = Transform(view, snapshot.projection);
    if (!std::isfinite(clip[3]) || clip[3] <= 0.00001f) return false;
    const float ndc_x = clip[0] / clip[3];
    const float ndc_y = clip[1] / clip[3];
    const float ndc_z = clip[2] / clip[3];
    if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y) || !std::isfinite(ndc_z) || ndc_z < 0.0f) return false;
    dist = std::sqrt(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
    if (!std::isfinite(dist)) return false;

    const auto& viewport = snapshot.viewport;
    const float target_x = float(viewport.x) + (ndc_x + 1.0f) * 0.5f * float(viewport.w);
    const float target_y = float(viewport.y) + (1.0f - ndc_y) * 0.5f * float(viewport.h);
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0.0f || display.y <= 0.0f) return false;
#ifdef _WIN32
    const float aspect = app::Widescreen() ? 16.0f / 9.0f : 4.0f / 3.0f;
#else
    const float aspect = game::Aspect();
#endif
    const float frame_width = std::min(display.x, display.y * aspect);
    const float frame_height = frame_width / aspect;
    screen_x = (display.x - frame_width) * 0.5f + target_x / float(snapshot.target_width) * frame_width;
    screen_y = (display.y - frame_height) * 0.5f + target_y / float(snapshot.target_height) * frame_height;
    return screen_x >= -300.0f && screen_x <= display.x + 300.0f &&
           screen_y >= -300.0f && screen_y <= display.y + 300.0f;
}

} // namespace game
