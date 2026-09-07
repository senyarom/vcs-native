#pragma once

#include <cstdint>
#include <span>

namespace psprecomp { class GuestMemory; }
namespace vcs {

struct BootImageChunk { const unsigned char *data; std::uint32_t size; };
struct BootImage {
    std::uint32_t address, image_size, entry, gp, arena_start;
    const char *source_sha256;
    std::span<const BootImageChunk> chunks;
};

// Defined by the build from game/bootstrap, with no runtime file dependency.
const BootImage &game_boot_image() noexcept;
void initialize_game_memory(psprecomp::GuestMemory &memory);

} // namespace vcs
