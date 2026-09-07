#include "graphics_settings_sdl.hpp"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <SDL.h>
#include <algorithm>

namespace vcs {
namespace {
void help(const char *text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
const char *label(const char *text, const char *description) {
    ImGui::TableNextRow(); ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(text);
    help(description);
    ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
    return description;
}
}
GraphicsPanelAction draw_graphics_panel(GraphicsPanelState &state) {
    const auto &io = ImGui::GetIO();
    auto &s = state.draft;
    const auto w = std::max(280.0f, std::min(740.0f, io.DisplaySize.x - 24));
    const auto h = std::max(180.0f, std::min(720.0f, io.DisplaySize.y - 24));
    ImGui::SetNextWindowPos({io.DisplaySize.x * .5f, io.DisplaySize.y * .5f}, ImGuiCond_Always, {.5f,.5f});
    ImGui::SetNextWindowSize({w,h}, ImGuiCond_Always);
    ImGui::Begin("Graphics settings", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
    ImGui::TextColored({.26f,.86f,.86f,1}, "VICE CITY STORIES");
    ImGui::TextDisabled("Game paused  /  F10 or Esc to return");
    ImGui::Separator();
    const float footer = ImGui::GetFrameHeightWithSpacing() + (state.error.empty() ? 8 : 65);
    ImGui::BeginChild("Options", {0, -footer});
    ImGui::TextUnformatted("Presets");
    if (ImGui::Button("Performance")) graphics_preset(s,0);
    help("30 FPS, 2x resolution, bilinear filtering with mipmaps, 2x anisotropy, original textures and distances.");
    ImGui::SameLine(); if (ImGui::Button("Balanced")) graphics_preset(s,1);
    help("60 FPS, 3x resolution, bilinear filtering with mipmaps, 4x anisotropy, HD textures and original distances.");
    ImGui::SameLine(); if (ImGui::Button("Native HD")) graphics_preset(s,2);
    help("60 FPS at the display's native resolution, bilinear filtering with mipmaps, 16x anisotropy, HD textures and original distances.");
    ImGui::Spacing(); ImGui::SeparatorText("Display & performance");
    if (ImGui::BeginTable("Display", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Label", 0, .44f); ImGui::TableSetupColumn("Value", 0, .56f);
        auto description = label("Frame rate", "Limits frames per second. Higher limits give smoother motion but require more CPU and GPU work. Uncapped removes the limit. Mission-specific compatibility limits take priority.");
        const unsigned rates[]{30,60,0,120,200,240};
        int rate = 1; for (int i=0;i<6;++i) if (rates[i] == s.frame_rate) rate=i;
        if (ImGui::Combo("##fps", &rate, "30 FPS\0" "60 FPS\0" "Uncapped\0" "120 FPS\0" "200 FPS\0" "240 FPS\0")) s.frame_rate=rates[rate];
        help(description);
        description = label("Internal resolution", "Sets the resolution used to render the game. Higher values make edges clearer and use more GPU power and memory. Native display uses physical display pixels, including Retina; lower values can improve FPS.");
        int resolution = s.rendering.internal_resolution_mode == InternalResolutionMode::Desktop ? 0 :
            s.rendering.internal_resolution_mode == InternalResolutionMode::PspNative ? 1 :
            s.rendering.internal_resolution_mode == InternalResolutionMode::Custom ? 9 : int(s.rendering.internal_scale);
        if (ImGui::Combo("##resolution", &resolution, "Native display\0" "1x  /  480 x 272\0" "2x  /  960 x 544\0" "3x  /  1440 x 816\0" "4x  /  1920 x 1088\0" "5x  /  2400 x 1360\0" "6x  /  2880 x 1632\0" "7x  /  3360 x 1904\0" "8x  /  3840 x 2176\0" "Custom (INI)\0")) {
            s.rendering.internal_resolution_mode = resolution == 0 ? InternalResolutionMode::Desktop : resolution == 9 ? InternalResolutionMode::Custom : InternalResolutionMode::Scale;
            if (resolution > 0 && resolution < 9) s.rendering.internal_scale = resolution;
        }
        help(description);
        description = label("Fullscreen", "Fills the display with the game. Turn off to play in a window.");
        ImGui::Checkbox("##fullscreen", &s.fullscreen); help(description);
        description = label("FPS counter", "Shows the current frame rate in the corner of the screen to help compare performance.");
        ImGui::Checkbox("##counter", &s.show_fps); help(description);
        ImGui::EndTable();
    }
    ImGui::Spacing(); ImGui::SeparatorText("Textures");
    if (ImGui::BeginTable("Textures", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Label", 0, .44f); ImGui::TableSetupColumn("Value", 0, .56f);
        auto description = label("Texture filtering", "Controls how texture pixels blend. Nearest keeps hard pixel edges; bilinear smooths them. Game default uses each texture's original filtering. Applies to world textures.");
        int filter=int(s.rendering.texture_filter);
        if (ImGui::Combo("##filter", &filter, "Game default\0Nearest\0Bilinear (linear)\0")) s.rendering.texture_filter=TextureFilter(filter);
        help(description);
        description = label("Mipmapping", "Uses smaller texture versions in the distance to reduce shimmer. On generates missing levels; Off disables mipmaps; Game default follows the original settings. HUD and framebuffer effects keep their original settings.");
        int mip=int(s.rendering.mipmapping);
        if (ImGui::Combo("##mips", &mip, "Game default\0Off\0On\0")) s.rendering.mipmapping=MipmapMode(mip);
        help(description);
        description = label("Mip level filtering", "Nearest selects one mip level. Linear blends neighboring levels for smoother distance transitions, giving trilinear filtering with bilinear texture filtering. Game default follows the original setting. Requires mipmapping.");
        int mf=int(s.rendering.mipmap_filter);
        ImGui::BeginDisabled(s.rendering.mipmapping == MipmapMode::Off);
        if (ImGui::Combo("##mipfilter", &mf, "Game default\0Nearest level\0Linear (trilinear with bilinear)\0")) s.rendering.mipmap_filter=MipmapFilter(mf);
        ImGui::EndDisabled();
        help(description);
        description = label("Anisotropic filtering", "Keeps textures sharper at shallow viewing angles, especially roads and ground. Higher values can cost more GPU time. Requires mipmapping and a texture filter other than Nearest; the GPU may limit the maximum.");
        const unsigned levels[]{1,2,4,8,16}; int af=0;
        for(int i=0;i<5;++i) if(levels[i] <= s.rendering.anisotropic_filtering) af=i;
        ImGui::BeginDisabled(s.rendering.mipmapping == MipmapMode::Off || s.rendering.texture_filter == TextureFilter::Nearest);
        if (ImGui::Combo("##af", &af, "Off\0" "2x\0" "4x\0" "8x\0" "16x\0")) s.rendering.anisotropic_filtering=levels[af];
        ImGui::EndDisabled();
        help(description);
        description = label("HD texture pack", "Uses higher-resolution textures from the installed pack where replacements exist. Uses more texture memory. Turn off to use original textures; textures without replacements remain original.");
        ImGui::Checkbox("##hd", &s.hd_textures); help(description);
        ImGui::EndTable();
    }
    ImGui::Spacing(); ImGui::SeparatorText("World distance  /  experimental");
    if (ImGui::BeginTable("Distance", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Label", 0, .44f); ImGui::TableSetupColumn("Value", 0, .56f);
        auto description = label("Far clip", "Multiplies the camera's maximum drawing distance. Higher values allow more distant geometry to be shown and can increase rendering work. Fog and model visibility limits still apply. 1x uses the original range.");
        ImGui::SliderFloat("##world", &s.distance.world, kMinGraphicsDistance, kMaxGraphicsDistance, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
        help(description);
        description = label("Model LOD", "Keeps detailed world models, vehicles and characters visible farther away before switching to simpler models. Higher values increase CPU/GPU work and memory use. 1x restores original distances; interior world visibility stays unchanged.");
        ImGui::SliderFloat("##lod", &s.distance.lod, kMinGraphicsDistance, kMaxGraphicsDistance, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
        help(description);
        ImGui::EndTable();
    }
    ImGui::TextDisabled("1x = original range. Extended ranges cost CPU, GPU and memory.");
    ImGui::EndChild();
    if (!state.error.empty()) { ImGui::PushStyleColor(ImGuiCol_Text,{1,.48f,.55f,1}); ImGui::TextWrapped("%s",state.error.c_str()); ImGui::PopStyleColor(); }
    GraphicsPanelAction result = GraphicsPanelAction::None;
    if (ImGui::Button("Apply & resume")) result=GraphicsPanelAction::Apply;
    help("Applies your changes, saves them and returns to the game.");
    ImGui::SameLine(); if (ImGui::Button("Cancel")) result=GraphicsPanelAction::Cancel;
    help("Returns to the game without applying changes made in this panel.");
    ImGui::SameLine(); if (ImGui::Button("Reset changes")) s=graphics_settings_from(vcs_configuration());
    help("Discards edits in this panel and restores the settings currently used by the game.");
    ImGui::End();
    return result;
}
void configure_graphics_panel_style() {
    auto &io=ImGui::GetIO(); io.IniFilename=nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    auto &style=ImGui::GetStyle(); style.FontSizeBase=17;
    style.WindowPadding={20,16}; style.FramePadding={8,5}; style.ItemSpacing={10,8};
    style.WindowRounding=10; style.FrameRounding=4;
    io.Fonts->AddFontDefaultVector();
    style.Colors[ImGuiCol_TitleBg]=style.Colors[ImGuiCol_TitleBgActive]={.20f,.09f,.17f,1};
    style.Colors[ImGuiCol_FrameBg]={.15f,.12f,.21f,1};
    style.Colors[ImGuiCol_Button]={.30f,.15f,.26f,1};
    style.Colors[ImGuiCol_ButtonHovered]={.45f,.21f,.36f,1};
    style.Colors[ImGuiCol_ButtonActive]={.55f,.25f,.42f,1};
    style.Colors[ImGuiCol_WindowBg]={.065f,.055f,.09f,.98f};
    style.Colors[ImGuiCol_CheckMark]={.22f,.85f,.86f,1};
    style.Colors[ImGuiCol_SliderGrab]={.75f,.34f,.58f,1};
}
void run_graphics_panel(SDL_Window *window, SDL_Renderer *renderer,
                        const std::function<void()> &background, bool &closed,
                        const ApplyGraphicsSettings &apply, std::string initial_error) {
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    configure_graphics_panel_style();
    auto &io=ImGui::GetIO();
    ImGui_ImplSDL2_InitForSDLRenderer(window,renderer); ImGui_ImplSDLRenderer2_Init(renderer);
    GraphicsPanelState state{graphics_settings_from(vcs_configuration()), std::move(initial_error)};
    bool done=false;
    while (!done && !closed) {
        const auto start=SDL_GetTicks64();
        SDL_Event event;
        while(SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) closed=true;
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                const auto key=event.key.keysym.scancode;
                if (key == SDL_SCANCODE_ESCAPE || key == SDL_SCANCODE_F10) done=true;
                if (key == SDL_SCANCODE_Q && (event.key.keysym.mod & KMOD_GUI)) closed=true;
            }
        }
        if (closed || done) break;
        ImGui_ImplSDLRenderer2_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
        const auto action=draw_graphics_panel(state);
        ImGui::Render();
        SDL_RenderSetScale(renderer,1,1); background();
        SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer,0,0,0,150); SDL_RenderFillRect(renderer,nullptr);
        SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE);
        SDL_RenderSetScale(renderer,io.DisplayFramebufferScale.x,io.DisplayFramebufferScale.y);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(),renderer); SDL_RenderPresent(renderer);
        SDL_RenderSetScale(renderer,1,1); SDL_SetRenderDrawColor(renderer,0,0,0,255);
        if (action == GraphicsPanelAction::Cancel) done=true;
        if (action == GraphicsPanelAction::Apply) done=apply(state.draft,state.error);
        const auto elapsed=SDL_GetTicks64()-start;
        if (elapsed < 16) SDL_Delay(16-Uint32(elapsed));
    }
    ImGui_ImplSDLRenderer2_Shutdown(); ImGui_ImplSDL2_Shutdown(); ImGui::DestroyContext();
}
} // namespace vcs
