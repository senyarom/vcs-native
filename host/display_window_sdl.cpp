#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "display_window.hpp"
#include "vcs_config.hpp"
#include "graphics_settings_sdl.hpp"
#include "ge_gpu_backend.hpp"
#include "audio_output.hpp"
#include "vcs_vehicle_input.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace vcs {
namespace {
struct WindowState {
    SDL_Window *window{};
    SDL_Renderer *renderer{};
    SDL_Texture *texture{};
    SDL_GameController *controller{};
    std::array<bool, SDL_NUM_SCANCODES> keys{};
    std::uint32_t width{}, height{};
    int mouse_x{}, mouse_y{}, wheel{}, wheel_polls{};
    std::uint32_t mouse_buttons{}, wheel_button{};
    bool closed{}, aspect_lock{}, focused{true}, graphics_requested{};
    HostInputState cached{};
    std::chrono::steady_clock::time_point cached_at{};
};
WindowState &state() { static WindowState value; return value; }

void open_controller() {
    auto &s = state();
    if (s.controller) return;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            s.controller = SDL_GameControllerOpen(i);
            if (s.controller) {
                std::cout << "[SDL] controller: " << SDL_GameControllerName(s.controller)
                          << " (L1/R1 -> PSP L/R, L2/R2 -> vehicle brake/throttle)\n";
                break;
            }
        }
    }
}

void pump() {
    auto &s = state();
    if (!s.window) return;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) s.closed = true;
        if (event.type == SDL_WINDOWEVENT) {
            if (event.window.event == SDL_WINDOWEVENT_CLOSE) s.closed = true;
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) s.focused = true;
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                s.focused = false;
                s.keys.fill(false);
                s.mouse_buttons = 0;
                s.mouse_x = s.mouse_y = 0;
                SDL_SetRelativeMouseMode(SDL_FALSE);
            }
        }
        if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_F10 && !event.key.repeat) {
            s.graphics_requested = true;
            continue;
        }
        if (s.graphics_requested) continue; // Do not leak menu navigation to PSP input.
        if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
            const auto key = event.key.keysym.scancode;
            if (key < SDL_NUM_SCANCODES) s.keys[key] = event.type == SDL_KEYDOWN;
            if (event.type == SDL_KEYDOWN && key == SDL_SCANCODE_ESCAPE)
                SDL_SetRelativeMouseMode(SDL_FALSE);
            if (event.type == SDL_KEYDOWN && key == SDL_SCANCODE_Q &&
                (event.key.keysym.mod & KMOD_GUI)) s.closed = true;
        }
        if (event.type == SDL_MOUSEBUTTONDOWN) {
            s.mouse_buttons |= SDL_BUTTON(event.button.button);
            if (event.button.button == SDL_BUTTON_LEFT && s.focused)
                SDL_SetRelativeMouseMode(SDL_TRUE);
        }
        if (event.type == SDL_MOUSEBUTTONUP)
            s.mouse_buttons &= ~SDL_BUTTON(event.button.button);
        if (event.type == SDL_MOUSEMOTION && SDL_GetRelativeMouseMode()) {
            s.mouse_x += event.motion.xrel;
            s.mouse_y += event.motion.yrel;
        }
        if (event.type == SDL_MOUSEWHEEL) s.wheel += event.wheel.y;
        if (event.type == SDL_CONTROLLERDEVICEADDED) open_controller();
        if (event.type == SDL_CONTROLLERDEVICEREMOVED && s.controller &&
            !SDL_GameControllerGetAttached(s.controller)) {
            SDL_GameControllerClose(s.controller);
            s.controller = nullptr;
            open_controller();
        }
    }
}
} // namespace

bool display_window_enabled() {
    if (const char *value = std::getenv("PSPRECOMP_WINDOW"))
        return *value && std::string(value) != "0";
    return vcs_configuration().initialized && vcs_configuration().display.enabled;
}

void display_window_start() {
    auto &s = state();
    if (!display_window_enabled() || s.window) return;
    SDL_SetMainReady();
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
        throw std::runtime_error(std::string("SDL initialization: ") + SDL_GetError());
    const auto &config = vcs_configuration().display;
    int width = static_cast<int>(config.custom_width);
    int height = static_cast<int>(config.custom_height);
    if (config.resolution_mode == DisplayResolutionMode::PspNative) {
        width = 480; height = 272;
    } else if (config.resolution_mode == DisplayResolutionMode::Desktop) {
        // Window sizes use logical points; backing pixels are queried from the
        // renderer. Passing Retina pixel dimensions here makes an oversized window.
        SDL_Rect bounds{};
        if (SDL_GetDisplayUsableBounds(0, &bounds) == 0) {
            width = bounds.w;
            height = bounds.h;
        }
    }
    s.window = SDL_CreateWindow("VCSNative — macOS/Linux preview", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!s.window) throw std::runtime_error(std::string("SDL window: ") + SDL_GetError());
    if (config.fullscreen) SDL_SetWindowFullscreen(s.window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
        config.upscale_filter == DisplayUpscaleFilter::Bilinear ? "linear" : "nearest");
    s.renderer = SDL_CreateRenderer(s.window, -1, SDL_RENDERER_ACCELERATED);
    if (!s.renderer) s.renderer = SDL_CreateRenderer(s.window, -1, SDL_RENDERER_SOFTWARE);
    if (!s.renderer) throw std::runtime_error(std::string("SDL renderer: ") + SDL_GetError());
    SDL_SetRenderDrawColor(s.renderer, 0, 0, 0, 255);
    SDL_RenderClear(s.renderer);
    SDL_RenderPresent(s.renderer);
    open_controller();
    int pixel_width{}, pixel_height{};
    SDL_GetRendererOutputSize(s.renderer, &pixel_width, &pixel_height);
    std::cout << "[SDL] window ready, drawable=" << pixel_width << 'x' << pixel_height << " pixels\n";
}

void display_window_set_status(const char *status) {
    if (state().window && status) SDL_SetWindowTitle(state().window, status);
    pump();
}
void display_window_set_aspect_lock(bool locked) noexcept { state().aspect_lock = locked; }

void display_window_present_rgba(std::span<const std::byte> rgba,
                                std::uint32_t width, std::uint32_t height) {
    if (!display_window_enabled() || !width || !height ||
        rgba.size() < static_cast<std::size_t>(width) * height * 4u) return;
    display_window_start();
    pump();
    auto &s = state();
    if (s.closed) return;
    if (!s.texture || s.width != width || s.height != height) {
        if (s.texture) SDL_DestroyTexture(s.texture);
        s.texture = SDL_CreateTexture(s.renderer, SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING, static_cast<int>(width), static_cast<int>(height));
        if (!s.texture) throw std::runtime_error(std::string("SDL texture: ") + SDL_GetError());
        SDL_SetTextureBlendMode(s.texture, SDL_BLENDMODE_NONE);
        s.width = width; s.height = height;
    }
    if (SDL_UpdateTexture(s.texture, nullptr, rgba.data(), static_cast<int>(width * 4u)) != 0)
        throw std::runtime_error(std::string("SDL texture upload: ") + SDL_GetError());
    int output_width{}, output_height{};
    SDL_GetRendererOutputSize(s.renderer, &output_width, &output_height);
    const auto &config = vcs_configuration().display;
    const auto rectangle = calculate_presentation_rectangle(output_width, output_height, width,
        height, s.aspect_lock ? DisplayAspectMode::Preserve : config.aspect_mode, config.integer_scale);
    const SDL_Rect destination{rectangle.x, rectangle.y, rectangle.width, rectangle.height};
    SDL_RenderClear(s.renderer);
    SDL_RenderCopy(s.renderer, s.texture, nullptr, &destination);
    SDL_RenderPresent(s.renderer);
}

void display_window_present(const psprecomp::GuestMemory &memory,
                            const FramebufferDescription &description) {
    if (!display_window_enabled() || !description.address) return;
    std::vector<std::byte> rgba;
    try { rgba = decode_framebuffer_rgba(memory, description); }
    catch (const std::exception &) { return; }
    display_window_present_rgba(rgba, description.width, description.height);
}

HostInputState display_window_input() {
    auto &s = state();
    pump();
    if (!s.window || !s.focused || s.graphics_requested) return {};
    const auto now = std::chrono::steady_clock::now();
    if (now - s.cached_at < std::chrono::milliseconds(4)) return s.cached;
    s.cached_at = now;
    HostInputState input{};
    constexpr std::pair<SDL_Scancode, std::uint32_t> bindings[]{
        {SDL_SCANCODE_SPACE,0x4000}, {SDL_SCANCODE_LSHIFT,0x8000},
        {SDL_SCANCODE_RSHIFT,0x8000}, {SDL_SCANCODE_F,0x1000},
        {SDL_SCANCODE_RETURN,0x1000}, {SDL_SCANCODE_Q,0x80}, {SDL_SCANCODE_E,0x20},
        {SDL_SCANCODE_H,0x100}, {SDL_SCANCODE_UP,0x10}, {SDL_SCANCODE_DOWN,0x40},
        {SDL_SCANCODE_LEFT,0x80}, {SDL_SCANCODE_RIGHT,0x20},
        {SDL_SCANCODE_ESCAPE,0x8}, {SDL_SCANCODE_TAB,0x1}};
    for (const auto &[key, button] : bindings) if (s.keys[key]) input.buttons |= button;
    if (s.mouse_buttons & SDL_BUTTON_LMASK) input.buttons |= 0x2000;
    if (s.mouse_buttons & SDL_BUTTON_RMASK) input.buttons |= 0x200;
    if (s.mouse_buttons & SDL_BUTTON_MMASK) input.buttons |= 0x100;
    const bool driving = vcs_player_in_vehicle();
    const int reach = s.keys[SDL_SCANCODE_LALT] ? 60 : 127;
    const int x = int(s.keys[SDL_SCANCODE_D]) - int(s.keys[SDL_SCANCODE_A]);
    const int y = driving ? int(s.keys[SDL_SCANCODE_DOWN]) - int(s.keys[SDL_SCANCODE_UP])
                         : int(s.keys[SDL_SCANCODE_S]) - int(s.keys[SDL_SCANCODE_W]);
    input.analog_x = static_cast<std::uint8_t>(128 + x * reach);
    input.analog_y = static_cast<std::uint8_t>(128 + y * reach);
    input.accelerate = s.keys[SDL_SCANCODE_W];
    input.brake = s.keys[SDL_SCANCODE_S];
    if (s.wheel) { s.wheel_button = s.wheel > 0 ? 0x80 : 0x20; s.wheel_polls = 4; s.wheel = 0; }
    if (s.wheel_polls > 0) { --s.wheel_polls; input.buttons |= s.wheel_button; }
    const auto &controls = vcs_configuration().controls;
    const auto response = [&](int delta) {
        const double scaled = std::abs(delta) * (controls.mouse_sensitivity / 12.0);
        return static_cast<int>(std::lround((delta < 0 ? -127.0 : 127.0) * scaled / (scaled + 12.0)));
    };
    input.camera_x = response(s.mouse_x);
    input.camera_y = response(-s.mouse_y) * (controls.invert_camera_y ? -1 : 1);
    s.mouse_x = s.mouse_y = 0;
    if (s.controller) {
        constexpr std::pair<SDL_GameControllerButton, std::uint32_t> buttons[]{
            {SDL_CONTROLLER_BUTTON_A, 0x4000}, {SDL_CONTROLLER_BUTTON_B, 0x2000},
            {SDL_CONTROLLER_BUTTON_X, 0x8000}, {SDL_CONTROLLER_BUTTON_Y, 0x1000},
            {SDL_CONTROLLER_BUTTON_BACK, 0x1}, {SDL_CONTROLLER_BUTTON_START, 0x8},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x100}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x200},
            {SDL_CONTROLLER_BUTTON_DPAD_UP, 0x10}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x40},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x80}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x20}};
        for (const auto &[button, psp_mask] : buttons)
            if (SDL_GameControllerGetButton(s.controller, button)) input.buttons |= psp_mask;
        const auto axis = [&](SDL_GameControllerAxis which) {
            int value = SDL_GameControllerGetAxis(s.controller, which);
            return std::abs(value) < 8000 ? 0 : std::clamp(value / 256, -127, 127);
        };
        if (int value = axis(SDL_CONTROLLER_AXIS_LEFTX)) input.analog_x = static_cast<std::uint8_t>(128 + value);
        if (int value = axis(SDL_CONTROLLER_AXIS_LEFTY)) input.analog_y = static_cast<std::uint8_t>(128 + value);
        if (int value = axis(SDL_CONTROLLER_AXIS_RIGHTX)) input.camera_x = value;
        if (int value = axis(SDL_CONTROLLER_AXIS_RIGHTY)) input.camera_y = controls.invert_camera_y ? value : -value;
        const bool rt = SDL_GameControllerGetAxis(s.controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000;
        const bool lt = SDL_GameControllerGetAxis(s.controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000;
        // Modern on-foot layout: LT aims (PSP R), RT fires (PSP Circle).
        // Vehicles consume drive flags without also aiming or firing.
        input.accelerate |= rt;
        input.brake |= lt;
        if (!driving) {
            if (lt) input.buttons |= 0x200;
            if (rt) input.buttons |= 0x2000;
        }
    }
    return s.cached = input;
}

std::uint32_t display_window_buttons() { return display_window_input().buttons; }
void display_window_analog(std::uint8_t &x, std::uint8_t &y) {
    const auto input = display_window_input(); x = input.analog_x; y = input.analog_y;
}
bool display_window_close_requested() { pump(); return state().closed; }
DisplayWindowSurface display_window_surface() {
    auto &s = state();
    if (!s.window) return {};
    int width{}, height{}; SDL_GetWindowSize(s.window, &width, &height);
    return {nullptr, nullptr, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
}
bool display_window_graphics_panel(const ApplyGraphicsSettings &apply) {
    auto &s = state();
    if (!s.window || !s.graphics_requested || s.closed) return false;
    const bool relative = SDL_GetRelativeMouseMode();
    SDL_SetRelativeMouseMode(SDL_FALSE);
    audio_output_pause(true);
    const auto clear_input = [&] {
        s.keys.fill(false); s.cached = {}; s.cached_at = {};
        s.mouse_buttons = 0; s.mouse_x = s.mouse_y = s.wheel = s.wheel_polls = 0;
    };
    clear_input();
    const auto background = [&] {
        SDL_SetRenderDrawColor(s.renderer, 0, 0, 0, 255);
        SDL_RenderClear(s.renderer);
        if (!s.texture) return;
        int w{}, h{}; SDL_GetRendererOutputSize(s.renderer, &w, &h);
        const auto rectangle = calculate_presentation_rectangle(w,h,s.width,s.height,
            DisplayAspectMode::Preserve, false);
        SDL_Rect destination{rectangle.x,rectangle.y,rectangle.width,rectangle.height};
        SDL_RenderCopy(s.renderer,s.texture,nullptr,&destination);
    };
    const auto backend = ge_gpu_backend_report();
    const std::string renderer_error = backend.requested != GeGpuBackendKind::Software && !ge_gpu_backend_active()
        ? "Hardware rendering stopped. Showing 480 x 272 software output. Your settings are unchanged."
        : "";
    run_graphics_panel(s.window,s.renderer,background,s.closed,[&](const auto &settings, std::string &error) {
        const auto old_fullscreen = vcs_configuration().display.fullscreen;
        if (settings.fullscreen != old_fullscreen && SDL_SetWindowFullscreen(s.window,
                settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0) {
            error = SDL_GetError(); return false;
        }
        if (apply(settings,error)) return true;
        if (settings.fullscreen != old_fullscreen)
            SDL_SetWindowFullscreen(s.window,old_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        return false;
    }, renderer_error);
    clear_input();
    s.graphics_requested = false;
    s.focused = (SDL_GetWindowFlags(s.window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    if (relative && s.focused && !s.closed) SDL_SetRelativeMouseMode(SDL_TRUE);
    audio_output_pause(false);
    return true;
}

void display_window_shutdown() {
    auto &s = state();
    if (s.controller) SDL_GameControllerClose(s.controller);
    if (s.texture) SDL_DestroyTexture(s.texture);
    if (s.renderer) SDL_DestroyRenderer(s.renderer);
    if (s.window) SDL_DestroyWindow(s.window);
    s = {};
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
}
} // namespace vcs
