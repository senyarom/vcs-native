#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "graphics_settings_sdl.hpp"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <iostream>
#include <stdexcept>
static void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main(int argc, char **argv) {
    SDL_Window *window=nullptr; SDL_Renderer *renderer=nullptr;
    try {
        SDL_setenv("SDL_VIDEODRIVER","dummy",1); SDL_SetMainReady();
        require(SDL_Init(SDL_INIT_VIDEO)==0,SDL_GetError());
        window=SDL_CreateWindow("graphics test",0,0,1024,900,SDL_WINDOW_HIDDEN);
        require(window,SDL_GetError()); renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);
        require(renderer,SDL_GetError());
        ImGui::CreateContext(); auto &io=ImGui::GetIO(); io.IniFilename=nullptr;
        vcs::configure_graphics_panel_style();
        ImGui_ImplSDL2_InitForSDLRenderer(window,renderer);ImGui_ImplSDLRenderer2_Init(renderer);
        vcs::GraphicsPanelState panel{vcs::graphics_settings_from(vcs::vcs_configuration()),{}};
        vcs::graphics_preset(panel.draft,2);panel.draft.fullscreen=true;panel.draft.show_fps=true;
        unsigned vertices=0;
        for(int i=0;i<4;++i) {
            ImGui_ImplSDLRenderer2_NewFrame();ImGui_ImplSDL2_NewFrame();ImGui::NewFrame();
            require(vcs::draw_graphics_panel(panel)==vcs::GraphicsPanelAction::None,"no spontaneous Apply");
            ImGui::Render();vertices=ImGui::GetDrawData()->TotalVtxCount;
            SDL_SetRenderDrawColor(renderer,25,20,34,255);SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(),renderer);
            if (i==3 && argc>1) {
                auto *surface=SDL_CreateRGBSurfaceWithFormat(0,1024,900,32,SDL_PIXELFORMAT_RGBA32);
                require(surface,"capture surface");
                require(SDL_RenderReadPixels(renderer,nullptr,surface->format->format,surface->pixels,surface->pitch)==0,SDL_GetError());
                require(SDL_SaveBMP(surface,argv[1])==0,SDL_GetError());SDL_FreeSurface(surface);
            }
            SDL_RenderPresent(renderer);
        }
        require(vertices>1000,"settings panel contains rendered controls and text");
        const auto click = [&](float x, float y) {
            vcs::GraphicsPanelAction action{};
            for (bool down : {true,false}) {
                ImGui_ImplSDLRenderer2_NewFrame();ImGui_ImplSDL2_NewFrame();
                io.AddMousePosEvent(x,y);io.AddMouseButtonEvent(0,down);
                ImGui::NewFrame();action=vcs::draw_graphics_panel(panel);ImGui::Render();
            }
            return action;
        };
        require(click(215,230)==vcs::GraphicsPanelAction::None && panel.draft.frame_rate==30 &&
                panel.draft.rendering.internal_scale==2 && !panel.draft.hd_textures,"Performance preset button edits draft");
        require(click(854,680)==vcs::GraphicsPanelAction::None && panel.draft.distance.world==10,
                "Far clip slider reaches 10x");
        require(click(854,711)==vcs::GraphicsPanelAction::None && panel.draft.distance.lod==10,
                "Model LOD slider reaches 10x");
        require(click(225,772)==vcs::GraphicsPanelAction::Apply,"Apply button submits draft");

        ImGui_ImplSDLRenderer2_Shutdown();ImGui_ImplSDL2_Shutdown();ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
        std::cout<<"Graphics panel rendered with SDL software test driver\n";return 0;
    } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';SDL_Quit();return 1;}
}
