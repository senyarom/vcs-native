#pragma once
#include "graphics_settings.hpp"
struct SDL_Window;
struct SDL_Renderer;
namespace vcs {
struct GraphicsPanelState {
    GraphicsSettings draft;
    std::string error;
};
enum class GraphicsPanelAction { None, Apply, Cancel };
void configure_graphics_panel_style();
GraphicsPanelAction draw_graphics_panel(GraphicsPanelState &);
void run_graphics_panel(SDL_Window *, SDL_Renderer *, const std::function<void()> &background,
                        bool &closed, const ApplyGraphicsSettings &apply, std::string initial_error = {});
} // namespace vcs
