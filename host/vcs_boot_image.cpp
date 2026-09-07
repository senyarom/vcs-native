#include "vcs_boot_image.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/common.hpp"

namespace vcs {

void initialize_game_memory(psprecomp::GuestMemory &memory) {
    const auto &image = game_boot_image();
    if (!memory.contains(image.address, image.image_size))
        throw psprecomp::Error("Built-in game image does not fit guest RAM");
    std::uint32_t offset = 0;
    for (const auto &chunk : image.chunks) {
        if (chunk.size > image.image_size - offset)
            throw psprecomp::Error("Invalid built-in game image chunk");
        memory.copy_in(image.address + offset, {chunk.data, chunk.size});
        offset += chunk.size;
    }
    // Explicitly restore BSS too, so initialization is independent of prior RAM.
    memory.zero(image.address + offset, image.image_size - offset);
}

} // namespace vcs
