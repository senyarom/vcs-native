#include "graphics_settings.hpp"
#include "ge_gpu_backend.hpp"
#include "vcs_runtime_log.hpp"
#include <sstream>
#include "psprecomp/runtime.hpp"
#include <iostream>

namespace vcs {
bool apply_live_graphics_settings(psprecomp::Runtime &rt, const GraphicsSettings &settings, std::string &error) {
    if (!validate_graphics_settings(settings, error)) return false;
    const auto previous = vcs_configuration();
    const auto next = graphics_configuration(previous, settings);
    std::ostringstream requested;
    requested << "[graphics-apply] resolution=" << internal_resolution_mode_name(settings.rendering.internal_resolution_mode)
              << " scale=" << settings.rendering.internal_scale << " fps=" << settings.frame_rate
              << " counter=" << settings.show_fps << " hd=" << settings.hd_textures
              << " far_clip=" << settings.distance.world << " lod=" << settings.distance.lod;
    runtime_log_line(requested.str());
    // The GE worker was drained above; shutdown waits for GPU fences.
    shutdown_ge_gpu_backend();
    publish_graphics_configuration(next);
    const bool ready = initialize_ge_gpu_backend(error);
    if (ready && save_graphics_settings(next.source_path, settings, error)) {
        runtime_log_line("[graphics-apply] success; " + ge_gpu_backend_report().message);
        std::cerr << "[graphics] applied fps=" << settings.frame_rate
                  << " world=" << settings.distance.world << " lod=" << settings.distance.lod << "\n";
        return true;
    }
    runtime_log_line("[graphics-apply] failed: " + error);
    shutdown_ge_gpu_backend();
    publish_graphics_configuration(previous);
    std::string rollback_error;
    if (!initialize_ge_gpu_backend(rollback_error)) {
        error += "; renderer recovery failed: " + rollback_error;
        rt.stop(error);
    }
    return false;

}
} // namespace vcs
