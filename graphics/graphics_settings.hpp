#pragma once
#include "vcs_config.hpp"
#include <functional>
#include <span>

namespace psprecomp { class Runtime; }

namespace vcs {
struct GeGpuDrawDescriptor;
// The panel edits this copy. Only Apply publishes it to the running game.
struct GraphicsSettings {
    RenderingConfiguration rendering;
    std::uint32_t frame_rate{60};
    bool fullscreen{}, show_fps{}, hd_textures{};
    DrawDistanceConfiguration distance;
};
GraphicsSettings graphics_settings_from(const VcsConfiguration &config);
VcsConfiguration graphics_configuration(const VcsConfiguration &base, const GraphicsSettings &settings);
bool validate_graphics_settings(const GraphicsSettings &, std::string &error);
bool save_graphics_settings(const std::filesystem::path &, const GraphicsSettings &, std::string &error);
void graphics_preset(GraphicsSettings &, unsigned preset);
void apply_graphics_sampling(GeGpuDrawDescriptor &, const RenderingConfiguration &, bool feedback);
void generate_graphics_mipmaps(std::vector<std::byte> &, std::uint32_t width,
                              std::uint32_t height, std::uint32_t &levels);
// Guest-thread transaction, called only after draining GE work.
bool apply_live_graphics_settings(psprecomp::Runtime &, const GraphicsSettings &, std::string &error);
using ApplyGraphicsSettings = std::function<bool(const GraphicsSettings &, std::string &)>;
// SDL host implementation; false if no panel was requested. Windows currently
// keeps its existing frontend and has no F10 panel.
bool display_window_graphics_panel(const ApplyGraphicsSettings &apply);
} // namespace vcs
