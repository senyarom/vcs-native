#include "texture_replacements.hpp"
#include "vcs_config.hpp"
#include "graphics_settings.hpp"
#define XXH_INLINE_ALL
#include "../third_party/xxhash/xxhash.h"
#if defined(PSPRECOMP_PNG_TEXTURES)
#include <png.h>
#endif

#include <algorithm>
#include <bit>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_map>

namespace vcs {
namespace {
std::uint32_t word(const std::uint8_t *p) noexcept {
    return p[0] | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    return first == s.npos ? "" : s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
struct Key {
    std::uint32_t address{}, palette{}, data{};
    auto operator<=>(const Key &) const = default;
};
struct Picture {
    std::uint32_t width{}, height{}, levels{};
    std::vector<std::byte> pixels;
};
struct Pack {
    bool initialized{}, enabled{};
    std::filesystem::path directory;
    std::map<Key, std::string> entries;
    std::unordered_map<std::string, std::shared_ptr<Picture>> pictures;
    std::uint64_t hits{}, misses{};
};
Pack &pack() { static Pack p; return p; }
void initialize() {
    auto &p = pack();
    if (p.initialized) return;
    p.initialized = true;
    const auto &config = vcs_configuration();
    if (!config.texture_replacements) return;
    p.directory = config.texture_directory;
    if (p.directory.is_relative())
        p.directory = std::filesystem::weakly_canonical(config.source_path).parent_path() / p.directory;
    std::ifstream input(p.directory / "textures.ini");
    if (!input) {
        std::fprintf(stderr, "[HD textures] missing textures.ini: %s\n", p.directory.string().c_str());
        return;
    }
    std::string line, section, hash = "quick";
    bool unsupported = false;
    while (std::getline(input, line)) {
        line = trim(line.substr(0, line.find_first_of("#;")));
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') { section = line.substr(1, line.size()-2); continue; }
        const auto equal = line.find('=');
        if (equal == line.npos) continue;
        const auto left = trim(line.substr(0, equal)), right = trim(line.substr(equal+1));
        if (section == "options" && left == "hash") hash = right;
        if (section == "hashranges" || (section == "options" && left == "reduceHash" && right != "false")) unsupported = true;
        if (section != "hashes" || left.size() != 24) continue;
        if (left.find_first_not_of("0123456789abcdefABCDEF") != left.npos) continue;
        const std::filesystem::path file(right);
        if (file.is_absolute()) continue;
        bool parent = false;
        for (const auto &part : file) parent |= part == "..";
        if (parent) continue;
        Key key{static_cast<std::uint32_t>(std::stoul(left.substr(0,8), nullptr, 16)),
                static_cast<std::uint32_t>(std::stoul(left.substr(8,8), nullptr, 16)),
                static_cast<std::uint32_t>(std::stoul(left.substr(16,8), nullptr, 16))};
        p.entries[key] = right;
    }
    p.enabled = hash == "quick" && !unsupported && !p.entries.empty();
    std::fprintf(stderr, "[HD textures] %s entries=%zu directory=%s\n",
        p.enabled ? "enabled" : "unsupported pack settings", p.entries.size(), p.directory.string().c_str());
}

std::shared_ptr<Picture> load_picture(const std::string &name) {
    auto &p = pack();
    if (auto it = p.pictures.find(name); it != p.pictures.end()) return it->second;
    auto &result = p.pictures[name];
#if defined(PSPRECOMP_PNG_TEXTURES)
    png_image png{}; png.version = PNG_IMAGE_VERSION;
    const auto file = p.directory / name;
    if (!png_image_begin_read_from_file(&png, file.string().c_str())) return {};
    if (!png.width || !png.height || png.width > 4096 || png.height > 4096) {
        png_image_free(&png); return {};
    }
    png.format = PNG_FORMAT_RGBA;
    auto image = std::make_shared<Picture>();
    image->width = png.width; image->height = png.height;
    image->levels = 1;
    image->pixels.resize(PNG_IMAGE_SIZE(png));
    if (!png_image_finish_read(&png, nullptr, image->pixels.data(), 0, nullptr)) {
        png_image_free(&png); return {};
    }
    png_image_free(&png);
    // Build replacement mip levels from the HD image, never mix in PSP mips.
    generate_graphics_mipmaps(image->pixels, image->width, image->height, image->levels);
    result = image;
    std::fprintf(stderr, "[HD textures] loaded %s %ux%u mips=%u\n", name.c_str(), image->width, image->height, image->levels);
#endif
    return result;
}
} // namespace

std::uint32_t replacement_quick_hash(std::span<const std::uint8_t> bytes,
                                    std::uint32_t guest_address) noexcept {
    // Scalar implementation of the PPSSPP quick texture hash format, using
    // explicit little-endian lanes and defined unsigned modular arithmetic.
    if ((guest_address & 15) || (bytes.size() & 63)) {
        std::uint32_t sum = 0;
        for (std::size_t i=0; i+8<=bytes.size(); i+=8) sum = (sum + word(bytes.data()+i)) ^ word(bytes.data()+i+4);
        return sum;
    }
    std::array<std::uint32_t,4> lanes{};
    std::array<std::uint32_t,4> factors{0x9bd9c00bu, 0xb6514b73u, 0x43094d9bu, 0x00010083u};
    const auto multiply_halves = [](std::uint32_t a, std::uint32_t b) {
        return ((a*b)&0xffffu) | (((a>>16)*(b>>16))<<16);
    };
    const auto add_halves = [](std::uint32_t a, std::uint32_t b) {
        return ((a+b)&0xffffu) | ((((a>>16)+(b>>16))&0xffffu)<<16);
    };
    for (std::size_t block=0; block<bytes.size(); block+=64) for (unsigned lane=0; lane<4; ++lane) {
        const auto *base = bytes.data()+block+lane*4;
        auto value = add_halves(lanes[lane], multiply_halves(word(base), factors[lane]));
        value = (value ^ word(base+16)) + word(base+32);
        lanes[lane] = value ^ multiply_halves(word(base+48), factors[lane]);
        factors[lane] = add_halves(factors[lane], 0x24552455u);
    }
    std::uint32_t sum=0;
    for (unsigned i=0; i<4; ++i) sum += lanes[i] + factors[i];
    return sum;
}

bool replace_texture(const psprecomp::GuestMemory &memory,
                     const std::array<std::uint32_t,256> &commands,
                     const GeGpuDrawDescriptor &d, std::uint32_t &width,
                     std::uint32_t &height, std::uint32_t &levels,
                     std::vector<std::byte> &pixels) {
    if (!vcs_configuration().texture_replacements) return false;
    initialize();
    auto &p=pack();
    if (!p.enabled || d.texture_format > 10 || !d.texture_buffer_width ||
        !d.texture_width || !d.texture_height) return false;
    constexpr unsigned bits[]{16,16,16,32,4,8,16,32,4,8,8};
    const auto bpp=bits[d.texture_format], stride=d.texture_buffer_width*bpp/8, row=d.texture_width*bpp/8;
    const std::uint64_t size = d.texture_buffer_width <= d.texture_width
        ? (std::uint64_t(d.texture_buffer_width)*d.texture_height + d.texture_width-d.texture_buffer_width)*bpp/8
        : std::uint64_t(d.texture_height-1)*stride+row;
    if (size > 8*1024*1024) return false;
    const auto *raw=memory.raw_pointer(d.texture_address, size);
    if (!raw) return false;
    std::uint32_t data=0;
    if (d.texture_buffer_width <= d.texture_width) data=replacement_quick_hash({raw,static_cast<std::size_t>(size)},d.texture_address);
    else for (unsigned y=0;y<d.texture_height;++y)
        data=(data*11)^replacement_quick_hash({raw+std::size_t(y)*stride,row},d.texture_address+y*stride);
    std::uint32_t palette=commands[0xB8]&0x0f0fu;
    if (d.texture_format >= 4 && d.texture_format <= 7) {
        const auto loaded=(commands[0xC4]&0x3fu)*32u;
        const auto bytes=std::min(1024u,loaded+d.clut_start*(d.clut_format==3?4u:2u));
        const auto *clut=memory.raw_pointer(d.clut_address,bytes);
        if (!clut || !bytes) return false;
        palette ^= XXH32(clut,bytes,0xC0108888u) ^ commands[0xC5];
    }
    // Prefer the most specific key. Zero fields in textures.ini are wildcards.
    const std::string *file=nullptr;
    for (auto address : {d.texture_address&0x3fffffffu,0u}) {
        for (auto pal : {palette,0u}) {
            for (auto hash : {data,0u}) {
                if (auto it=p.entries.find({address,pal,hash});it!=p.entries.end()) { file=&it->second; break; }
            }
            if (file) break;
        }
        if (file) break;
    }
    if (!file || file->empty()) { ++p.misses; return false; }
    auto image=load_picture(*file);
    if (!image) return false;
    // Keep normalized UVs and all guest descriptors unchanged. Only native
    // image storage grows, so HUD atlases and world mapping retain their layout.
    width=image->width; height=image->height; levels=image->levels; pixels=image->pixels;
    ++p.hits;
    return true;
}

void reset_texture_replacements() {
    auto &p=pack();
    if (p.initialized) std::fprintf(stderr,"[HD textures] hits=%llu misses=%llu loaded_files=%zu\n",
        static_cast<unsigned long long>(p.hits), static_cast<unsigned long long>(p.misses),
        static_cast<std::size_t>(std::count_if(p.pictures.begin(),p.pictures.end(),[](const auto &entry){return bool(entry.second);})));
    p=Pack{};
}
} // namespace vcs
