// One-time import tool. ELF loading/relocation is deliberately outside the game.
#include "psprecomp/elf32.hpp"
#include "psprecomp/sha256.hpp"
#include "psprecomp/common.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

int main(int argc, char **argv) {
    try {
        if (argc != 3) throw psprecomp::Error("Usage: vcs_export_boot_image <original ELF> <output directory>");
        constexpr auto expected = "fee2e86c7fe457ab6da463d9fdc16f156a0900adcdaa2fd260206c11907c5d65";
        if (psprecomp::sha256_file(argv[1]) != expected)
            throw psprecomp::Error("Expected the original ULUS10160 1.03 executable matching game/generated");
        const auto elf = psprecomp::Elf32Image::from_file(argv[1]);
        psprecomp::GuestMemory memory(32u * 1024u * 1024u);
        const auto reloc = elf.load_and_relocate(memory, psprecomp::kDefaultPspUserLoadBase);
        if (reloc.invalid || reloc.unsupported) throw psprecomp::Error("Incomplete ELF relocations");
        std::uint64_t first = std::numeric_limits<std::uint32_t>::max(), end = 0;
        for (std::size_t i = 0; i < elf.segments().size(); ++i) {
            const auto &s = elf.segments()[i];
            if (s.type != 1u) continue;
            const auto address = elf.segment_runtime_address(i, psprecomp::kDefaultPspUserLoadBase);
            first = std::min(first, std::uint64_t(address));
            end = std::max(end, std::uint64_t(address) + s.memory_size);
        }
        if (first >= end || end > 0x0A000000u || !memory.contains(first, end - first))
            throw psprecomp::Error("Invalid initial memory extent");
        const auto module = elf.find_module_info(memory, psprecomp::kDefaultPspUserLoadBase);
        if (!module) throw psprecomp::Error("Missing relocated module metadata");
        std::vector<std::uint8_t> bytes(end - first);
        memory.copy_out(first, bytes);
        // BSS and trailing zero bytes are represented by image_size, not stored.
        while (!bytes.empty() && bytes.back() == 0) bytes.pop_back();
        const std::filesystem::path output = argv[2];
        std::filesystem::create_directories(output);
        const auto blob = output / "initial-memory.bin";
        std::ofstream data(blob, std::ios::binary);
        data.exceptions(std::ios::badbit | std::ios::failbit);
        data.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        data.close();
        std::ofstream manifest(output / "metadata.json");
        manifest.exceptions(std::ios::badbit | std::ios::failbit);
        manifest << "{\n  \"format_version\": 1,\n  \"game\": \"ULUS10160 1.03\",\n"
                 << "  \"source_sha256\": \"" << expected << "\",\n"
                 << "  \"image_sha256\": \"" << psprecomp::sha256_file(blob) << "\",\n"
                 << "  \"load_address\": " << first << ",\n"
                 << "  \"image_size\": " << end - first << ",\n"
                 << "  \"data_size\": " << bytes.size() << ",\n"
                 << "  \"entry\": " << elf.runtime_entry() << ",\n"
                 << "  \"gp\": " << module->gp << ",\n"
                 << "  \"arena_start\": " << ((end + 255u) & ~std::uint64_t(255)) << ",\n"
                 << "  \"relocations_applied\": " << reloc.total << "\n}\n";
        manifest.close();
        std::cout << "Exported " << bytes.size() << " bytes + " << end - first - bytes.size()
                  << " zero bytes; " << reloc.total << " relocations resolved\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
