#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "audio_output.hpp"
#include "display_window.hpp"
#include "graphics_settings.hpp"
#include "vcs_vehicle_input.hpp"
#include <thread>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        setenv("SDL_VIDEODRIVER", "dummy", 1);
        setenv("SDL_AUDIODRIVER", "dummy", 1);
        setenv("PSPRECOMP_WINDOW", "1", 1);
        setenv("PSPRECOMP_AUDIO", "1", 1);
        // Only our virtual pad (VID/PID 0/0) may be opened by the host. In
        // particular, a connected Deck pad must not capture synthetic input.
        setenv("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT", "0x0000/0x0000", 1);
        const auto temporary = std::filesystem::temp_directory_path() /
            ("vcs-sdl-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(temporary), "temporary directory");
        const auto capture = std::filesystem::path(temporary) / "mixed.wav";
        setenv("PSPRECOMP_AUDIO_WAV", capture.c_str(), 1);

        vcs::display_window_start();
        const std::array<std::byte, 16> frame{
            std::byte{255},std::byte{0},std::byte{0},std::byte{255},
            std::byte{0},std::byte{255},std::byte{0},std::byte{255},
            std::byte{0},std::byte{0},std::byte{255},std::byte{255},
            std::byte{255},std::byte{255},std::byte{255},std::byte{255}};
        vcs::display_window_present_rgba(frame, 2, 2);
        const auto surface = vcs::display_window_surface();
        require(surface.width > 0 && surface.height > 0, "window dimensions");
        SDL_Event key{};
        key.type = SDL_KEYDOWN;
        key.key.keysym.scancode = SDL_SCANCODE_SPACE;
        SDL_PushEvent(&key);
        require(vcs::display_window_input().buttons & 0x4000u, "PSP Cross input");
        key.type = SDL_KEYUP;
        SDL_PushEvent(&key);
        SDL_Delay(5);
        require(!(vcs::display_window_input().buttons & 0x4000u), "key release");
        key.type = SDL_KEYDOWN;
        key.key.keysym.scancode = SDL_SCANCODE_W;
        SDL_PushEvent(&key);
        SDL_Delay(5);
        require(vcs::display_window_input().analog_y < 128u, "walk forward");
        SDL_Event focus{};
        focus.type = SDL_WINDOWEVENT;
        focus.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
        SDL_PushEvent(&focus);
        require(vcs::display_window_input().analog_y == 128u, "focus loss releases input");

        // Exercise SDL's actual controller polling, including shoulder buttons,
        // trigger axes, held inputs and the on-foot/vehicle transition.
        focus.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
        SDL_PushEvent(&focus);
        const int device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
            SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
        require(device >= 0, "attach virtual gamepad");
        auto *pad = SDL_JoystickOpen(device);
        require(pad != nullptr, "open virtual gamepad");
        const auto poll = [&] {
            SDL_JoystickUpdate();
            SDL_Delay(5);
            return vcs::display_window_input();
        };
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
        poll();
        constexpr std::pair<SDL_GameControllerButton, std::uint32_t> pad_buttons[]{
            {SDL_CONTROLLER_BUTTON_A,0x4000}, {SDL_CONTROLLER_BUTTON_B,0x2000},
            {SDL_CONTROLLER_BUTTON_X,0x8000}, {SDL_CONTROLLER_BUTTON_Y,0x1000},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,0x100}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,0x200},
            {SDL_CONTROLLER_BUTTON_START,8}, {SDL_CONTROLLER_BUTTON_BACK,1},
            {SDL_CONTROLLER_BUTTON_DPAD_UP,0x10}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN,0x40},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT,0x80}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT,0x20}};
        for (const auto &[button, expected] : pad_buttons) {
            SDL_JoystickSetVirtualButton(pad, button, 1);
            require(poll().buttons == expected, "gamepad button mapping");
            require(poll().buttons == expected, "held gamepad button lost");
            SDL_JoystickSetVirtualButton(pad, button, 0);
            require(poll().buttons == 0, "gamepad button release");
        }
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_LEFTX, 32767);
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY, -32768);
        auto input = poll();
        require(input.analog_x == 255 && input.camera_y == 127, "gamepad stick mapping");
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_LEFTX, 0);
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY, 0);
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
        require(poll().buttons == 0x2000, "on-foot right trigger -> fire (PSP Circle)");
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
        require(poll().buttons == 0x2200, "left aim and right fire work together");
        require(poll().buttons == 0x2200, "held trigger aim/fire lost");
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
        require(poll().buttons == 0x200, "releasing fire keeps left-trigger aim");
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
        require(poll().buttons == 0, "releasing aim clears combat input");
        // Physical R1/B remain usable while the new trigger aliases are held,
        // and releasing the triggers must not release the physical buttons.
        SDL_JoystickSetVirtualButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 1);
        SDL_JoystickSetVirtualButton(pad, SDL_CONTROLLER_BUTTON_B, 1);
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
        require(poll().buttons == 0x2200, "legacy buttons and trigger aliases coexist");
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
        require(poll().buttons == 0x2200, "trigger release cleared held R1/B");
        SDL_JoystickSetVirtualButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0);
        SDL_JoystickSetVirtualButton(pad, SDL_CONTROLLER_BUTTON_B, 0);
        require(poll().buttons == 0, "legacy button release");
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
        vcs::vcs_note_vehicle_control_read();
        input = poll();
        require(input.accelerate && input.brake && input.buttons == 0,
                "vehicle triggers must not also aim or fire");
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
        SDL_JoystickSetVirtualAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
        input = poll();
        require(input.brake && !input.accelerate && input.buttons == 0, "vehicle trigger brake");
        for (int i=0; i<21; ++i) input = poll();
        require(input.buttons == 0x200, "left-trigger aim restored after leaving vehicle");
        SDL_JoystickDetachVirtual(device);
        SDL_JoystickClose(pad);
        require(poll().buttons == 0, "disconnect releases gamepad");

        key.type=SDL_KEYDOWN; key.key.keysym.scancode=SDL_SCANCODE_F10;
        SDL_PushEvent(&key);
        require(vcs::display_window_input().buttons==0, "F10 does not press a PSP button");
        std::thread cancel_panel([] {
            SDL_Delay(150);
            SDL_Event e{};e.type=SDL_KEYDOWN;e.key.keysym.scancode=SDL_SCANCODE_ESCAPE;
            SDL_PushEvent(&e);
        });
        bool applied=false;
        const bool opened=vcs::display_window_graphics_panel([&](const auto &, std::string &){applied=true;return true;});
        cancel_panel.join();
        require(opened && !applied,"F10 panel opens, Escape cancels without applying");
        require(!(vcs::display_window_input().buttons & 8),"panel Escape never leaks into PSP Start");

        std::vector<std::int16_t> pcm(4096 * 2, 1000);
        vcs::audio_output_submit(pcm, 4096, true, 0x8000, 0x8000, 44100, 0, 0, 0);
        vcs::audio_output_submit(pcm, 4096, true, 0x8000, 0x8000, 44100, 1, 0, 0);
        vcs::audio_output_advance(100000);
        vcs::audio_output_shutdown();
        std::ifstream wav(capture, std::ios::binary);
        require(bool(wav), "WAV capture created");
        wav.seekg(44);
        std::array<unsigned char, 4> sample{};
        wav.read(reinterpret_cast<char *>(sample.data()), sample.size());
        require(wav.gcount() == 4, "audio frames written");
        const int left = sample[0] | sample[1] << 8;
        const int right = sample[2] | sample[3] << 8;
        require(left == 2000 && right == 2000, "simultaneous PSP channels mix in stereo");
        wav.close();

        // The SRC thread queues its next radio block ahead of real guest time.
        // That must not seal the effect channel before it submits at 2.3 ms.
        std::vector<std::int16_t> radio(2048 * 2, 500), effect(512 * 2, 1000);
        vcs::audio_output_submit(radio, 2048, true, 0x8000, 0x8000, 44100, 8, 0, 0);
        vcs::audio_output_advance(2300);
        vcs::audio_output_submit(radio, 2048, true, 0x8000, 0x8000, 44100, 8, 46439, 2300);
        vcs::audio_output_submit(effect, 512, true, 0x8000, 0x8000, 44100, 0, 2300, 2300);
        vcs::audio_output_advance(100000);
        vcs::audio_output_shutdown();
        wav.open(capture, std::ios::binary);
        wav.seekg(44 + 200 * 4);
        wav.read(reinterpret_cast<char *>(sample.data()), sample.size());
        require(wav.gcount() == 4, "scheduled audio frames written");
        require((sample[0] | sample[1] << 8) == 1500,
                "future radio submission shifted the current sound effect");
        wav.close();

        // A long host stall must not leave the sound effect a second behind
        // the visible event. Device queue and the upstream mixer both count.
        vcs::audio_output_submit(radio, 2048, true, 0x8000, 0x8000, 44100, 8, 0, 0);
        vcs::audio_output_advance(100000);
        vcs::audio_output_submit(effect, 512, true, 0x8000, 0x8000, 44100, 0, 1000000, 1000000);
        vcs::audio_output_advance(1040000);
        const auto audio = vcs::audio_output_status();
        require(audio.backlog_frames_discarded > 0, "stale audio discarded after a long stall");
        require(audio.latency_frames * 1000 / 44100 < 180,
                "effect stayed behind a stale mixer backlog");
        require(audio.device_frames <= 6 * 512, "device queue exceeds startup latency target");
        vcs::audio_output_shutdown();

        SDL_Event quit{}; quit.type = SDL_QUIT; SDL_PushEvent(&quit);
        require(vcs::display_window_close_requested(), "window close event");
        vcs::display_window_shutdown();
        std::filesystem::remove(capture);
        std::filesystem::remove(temporary);
        std::cout << "SDL presentation, input and audio mixing passed\n";
        return 0;
    } catch (const std::exception &error) {
        vcs::audio_output_shutdown();
        vcs::display_window_shutdown();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
