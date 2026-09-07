#include "vcs_boot_image.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/sha256.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char **argv) {
    try {
        const auto &image = vcs::game_boot_image();
        psprecomp::GuestMemory actual(32u * 1024u * 1024u);
        require(image.address == 0x08804000u && image.image_size == 6843172u,
                "wrong ULUS10160 memory extent");
        require(image.entry == 146098648u && image.gp == 146480480u
                && image.arena_start == 149466112u, "wrong initial registers/heap boundary");
        vcs::initialize_game_memory(actual);
        const auto pristine = actual.bytes();
        std::fill_n(actual.raw_pointer(image.address, image.image_size), image.image_size, 0xCC);
        actual.store8(image.address - 1, 0xA5);
        actual.store8(image.address + image.image_size, 0x5A);
        vcs::initialize_game_memory(actual);
        require(actual.load8(image.address - 1) == 0xA5
                && actual.load8(image.address + image.image_size) == 0x5A,
                "initialization touched memory outside the image");
        actual.store8(image.address - 1, 0);
        actual.store8(image.address + image.image_size, 0);
        require(actual.bytes() == pristine, "initialization failed to restore data/BSS");
        if (argc == 2) {
            require(psprecomp::sha256_file(argv[1]) == image.source_sha256, "wrong reference ELF");
            const auto elf = psprecomp::Elf32Image::from_file(argv[1]);
            psprecomp::GuestMemory reference(32u * 1024u * 1024u);
            const auto reloc = elf.load_and_relocate(reference, psprecomp::kDefaultPspUserLoadBase);
            require(!reloc.invalid && !reloc.unsupported, "reference relocation failed");
            require(reference.bytes() == actual.bytes(), "built-in image differs from ELF-loaded RAM");
            require(reference.vram_bytes() == actual.vram_bytes(), "VRAM changed");
            const auto module = elf.find_module_info(reference, psprecomp::kDefaultPspUserLoadBase);
            require(module && module->gp == image.gp && elf.runtime_entry() == image.entry,
                    "bootstrap registers differ from ELF loader");
            std::uint64_t end = 0;
            for (std::size_t i = 0; i < elf.segments().size(); ++i) {
                if (elf.segments()[i].type == 1u)
                    end = std::max(end, std::uint64_t(elf.segment_runtime_address(i,
                        psprecomp::kDefaultPspUserLoadBase)) + elf.segments()[i].memory_size);
            }
            require(((end + 255u) & ~std::uint64_t(255)) == image.arena_start, "heap boundary differs");
            std::cout << "Entire 32 MiB RAM + VRAM and bootstrap metadata match the original ELF loader\n";
        } else {
            require(argc == 1, "usage: vcs_boot_image_tests [reference ELF]");
        }
        std::cout << "Built-in image initialization/reinitialization passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
