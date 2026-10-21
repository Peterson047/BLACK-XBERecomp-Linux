#include "game/multiplayer.h"

#include <array>
#include <chrono>
#include <mutex>

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

} // namespace game
