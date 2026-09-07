#pragma once
#include "psprecomp/runtime.hpp"

namespace vcs::patches {
using FrameRateProvider = std::uint32_t (*)();
void install_mission_fps_patches(psprecomp::Runtime &, FrameRateProvider);
void update_mission_fps_patches(psprecomp::Runtime &, std::uint32_t gp);
[[nodiscard]] std::uint32_t mission_frame_rate(std::uint32_t requested) noexcept;
// Read-only equivalent of the CText lookup used by the upstream concert guard.
[[nodiscard]] bool concert_text_present(const psprecomp::GuestMemory &, std::uint32_t gp);
} // namespace vcs::patches
