#pragma once

#include "psprecomp/guest_memory.hpp"
#include "ge_gpu_backend.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace vcs {
// Interoperable PPSSPP quick hash; byte order and guest alignment are explicit.
std::uint32_t replacement_quick_hash(std::span<const std::uint8_t> bytes,
                                    std::uint32_t guest_address = 0) noexcept;
bool replace_texture(const psprecomp::GuestMemory &memory,
                     const std::array<std::uint32_t, 256> &commands,
                     const GeGpuDrawDescriptor &draw, std::uint32_t &width,
                     std::uint32_t &height, std::uint32_t &levels,
                     std::vector<std::byte> &pixels);
void reset_texture_replacements();
} // namespace vcs
