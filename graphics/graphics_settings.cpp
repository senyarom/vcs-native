#include "graphics_settings.hpp"
#include "ge_gpu_backend.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace vcs {
GraphicsSettings graphics_settings_from(const VcsConfiguration &c) {
    return {c.rendering, c.timing.frame_rate, c.display.fullscreen,
            c.display.show_fps, c.texture_replacements, c.draw_distance};
}
VcsConfiguration graphics_configuration(const VcsConfiguration &base, const GraphicsSettings &s) {
    auto c = base;
    c.rendering = s.rendering;
    c.timing.frame_rate = s.frame_rate;
    c.display.fullscreen = s.fullscreen;
    c.display.show_fps = s.show_fps;
    c.texture_replacements = s.hd_textures;
    c.draw_distance = s.distance;
    return c;
}
bool validate_graphics_settings(const GraphicsSettings &s, std::string &error) {
    error.clear();
    if (s.frame_rate != 0 && s.frame_rate != 30 && s.frame_rate != 60 &&
        s.frame_rate != 120 && s.frame_rate != 200 && s.frame_rate != 240)
        error = "Choose a supported frame rate.";
    else if (s.rendering.internal_scale < 1 || s.rendering.internal_scale > 8 ||
             s.rendering.internal_width < 480 || s.rendering.internal_width > 16384 ||
             s.rendering.internal_height < 272 || s.rendering.internal_height > 16384)
        error = "Internal resolution is outside the supported range.";
    else if (s.rendering.anisotropic_filtering < 1 || s.rendering.anisotropic_filtering > 16)
        error = "Anisotropic filtering must be between 1 and 16.";
    else if (unsigned(s.rendering.texture_filter) > 2 || unsigned(s.rendering.mipmapping) > 2 ||
             unsigned(s.rendering.mipmap_filter) > 2 || unsigned(s.rendering.internal_resolution_mode) > 3)
        error = "Unknown graphics mode.";
    else if (!std::isfinite(s.distance.world) || !std::isfinite(s.distance.lod) ||
             s.distance.world < kMinGraphicsDistance || s.distance.world > kMaxGraphicsDistance ||
             s.distance.lod < kMinGraphicsDistance || s.distance.lod > kMaxGraphicsDistance)
        error = "Far clip and Model LOD must be between 1x and 10x.";
    return error.empty();
}
void graphics_preset(GraphicsSettings &s, unsigned preset) {
    s.rendering.internal_resolution_mode = preset == 2 ? InternalResolutionMode::Desktop : InternalResolutionMode::Scale;
    s.rendering.internal_scale = preset == 0 ? 2 : 3;
    s.rendering.texture_filter = TextureFilter::Bilinear;
    s.rendering.mipmapping = MipmapMode::On;
    s.rendering.mipmap_filter = MipmapFilter::Linear;
    s.rendering.anisotropic_filtering = preset == 0 ? 2 : preset == 1 ? 4 : 16;
    s.hd_textures = preset != 0;
    s.frame_rate = preset == 0 ? 30 : 60;
    s.distance = {}; // Extended distances are an explicit choice, not a preset side effect.
}
namespace {
std::string lower_trim(std::string v) {
    const auto start = v.find_first_not_of(" \t\r");
    if (start == std::string::npos) return {};
    v = v.substr(start, v.find_last_not_of(" \t\r") - start + 1);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c){ return std::tolower(c); });
    return v;
}
std::string number(float n) { std::ostringstream o; o << n; return o.str(); }
}
bool save_graphics_settings(const std::filesystem::path &path, const GraphicsSettings &s, std::string &error) {
    if (!validate_graphics_settings(s, error)) return false;
    std::filesystem::path temporary;
    try {
        // Resolve the app bundle's INI symlink before atomic replacement.
        auto target = std::filesystem::weakly_canonical(path);
        const bool exists = std::filesystem::exists(target);
        std::ifstream input(target);
        if (exists && !input) throw std::runtime_error("Cannot read the current INI.");
        const auto bool_text = [](bool b) { return b ? "true" : "false"; };
        const char *filters[]{"Game", "Nearest", "Bilinear"};
        const char *mips[]{"Game", "Off", "On"};
        const char *mipfilters[]{"Game", "Nearest", "Linear"};
        using Values = std::map<std::string, std::string>;
        std::map<std::string, Values> values{
            {"display", {{"fullscreen",bool_text(s.fullscreen)}, {"showfps",bool_text(s.show_fps)}}},
            {"rendering", {{"internalresolutionmode",internal_resolution_mode_name(s.rendering.internal_resolution_mode)},
                {"internalscale",std::to_string(s.rendering.internal_scale)},
                {"internalwidth",std::to_string(s.rendering.internal_width)},
                {"internalheight",std::to_string(s.rendering.internal_height)},
                {"texturefilter",filters[unsigned(s.rendering.texture_filter)]},
                {"mipmapping",mips[unsigned(s.rendering.mipmapping)]},
                {"mipmapfilter",mipfilters[unsigned(s.rendering.mipmap_filter)]},
                {"anisotropicfiltering",std::to_string(s.rendering.anisotropic_filtering)}}},
            {"timing", {{"framerate",s.frame_rate ? std::to_string(s.frame_rate) : "Uncapped"}}},
            {"textures", {{"enabled",bool_text(s.hd_textures)}}},
            {"graphicsdistance", {{"world",number(s.distance.world)}, {"lod",number(s.distance.lod)}}}
        };
        auto missing = values;
        std::ostringstream output;
        std::string line, section;
        while (std::getline(input, line)) {
            const auto normalized = lower_trim(line);
            if (!normalized.empty() && normalized[0] == '[') {
                auto end = normalized.find(']');
                if (end != std::string::npos) section = lower_trim(normalized.substr(1, end - 1));
            } else if (!normalized.empty() && normalized[0] != ';' && normalized[0] != '#') {
                const auto equal = line.find('=');
                if (equal != std::string::npos) {
                    auto key = lower_trim(line.substr(0, equal));
                    if (section == "rendering") {
                        if (key == "internalmode") key = "internalresolutionmode";
                        if (key == "scale") key = "internalscale";
                        if (key == "anisotropy") key = "anisotropicfiltering";
                    }
                    if (auto group = values.find(section); group != values.end()) {
                        if (auto item = group->second.find(key); item != group->second.end()) {
                            const auto comment = line.find_first_of(";#", equal + 1);
                            line = line.substr(0, equal + 1) + item->second +
                                (comment == std::string::npos ? "" : " " + line.substr(comment));
                            missing[section].erase(key);
                        }
                    }
                }
            }
            output << line << '\n';
        }
        if (input.bad()) throw std::runtime_error("Cannot read the current INI.");
        for (const auto &[section, keys] : missing) {
            if (keys.empty()) continue;
            output << '\n' << '[' << section << "]\n";
            for (const auto &[key, value] : keys) output << key << '=' << value << '\n';
        }
        temporary = target;
        temporary += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        { std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
          if (!file || !(file << output.str()) || !file.flush()) throw std::runtime_error("Cannot write graphics settings.");
          file.close(); if (!file) throw std::runtime_error("Cannot close graphics settings."); }
        if (exists) std::filesystem::permissions(temporary, std::filesystem::status(target).permissions());
        std::filesystem::rename(temporary, target);
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        std::error_code ignored;
        if (!temporary.empty()) std::filesystem::remove(temporary, ignored);
        return false;
    }
}
void apply_graphics_sampling(GeGpuDrawDescriptor &d, const RenderingConfiguration &c, bool feedback) {
    if (d.through || feedback || !d.texture_enabled || d.clear_mode) return;
    if (c.texture_filter != TextureFilter::Game) {
        d.texture_min_linear = d.texture_mag_linear = d.texture_linear = c.texture_filter == TextureFilter::Bilinear;
    }
    if (c.mipmapping != MipmapMode::Game) d.texture_mipmap_enabled = c.mipmapping == MipmapMode::On;
    if (c.mipmap_filter != MipmapFilter::Game) d.texture_mipmap_linear = c.mipmap_filter == MipmapFilter::Linear;
}
void generate_graphics_mipmaps(std::vector<std::byte> &pixels, std::uint32_t width,
                              std::uint32_t height, std::uint32_t &levels) {
    if (!width || !height || std::size_t(width) > pixels.size() / 4 / height) return;
    const std::size_t base_bytes = std::size_t(width) * height * 4;
    std::size_t total = base_bytes;
    for (auto w = width, h = height; w > 1 || h > 1;) {
        w = std::max(1u, w / 2); h = std::max(1u, h / 2);
        const auto bytes = std::size_t(w) * h * 4;
        if (bytes > pixels.max_size() - total) throw std::length_error("Mip chain too large");
        total += bytes;
    }
    // Size the entire chain before taking row pointers. Compute all four
    // channels with two independent 16-bit lanes,
    // retaining the exact (sum + 2) / 4 box filter, including alpha.
    pixels.resize(total);
    levels = 1;
    std::size_t previous = 0, next = base_bytes;
    auto load = [](const std::byte *p) { std::uint32_t v; std::memcpy(&v,p,4); return v; };
    constexpr std::uint32_t mask = 0x00ff00ffu;
    while (width > 1 || height > 1) {
        const auto w = std::max(1u, width / 2), h = std::max(1u, height / 2);
        for (unsigned y = 0; y < h; ++y) {
            const auto *row0 = pixels.data() + previous + std::size_t(y * 2) * width * 4;
            const auto *row1 = row0 + (height > 1 ? std::size_t(width) * 4 : 0);
            auto *out = pixels.data() + next + std::size_t(y) * w * 4;
            for (unsigned x = 0; x < w; ++x) {
                const auto left = std::size_t(x) * 8, right = left + (width > 1 ? 4 : 0);
                const auto a = load(row0+left), b = load(row0+right), c = load(row1+left), d = load(row1+right);
                const auto rb = (a & mask) + (b & mask) + (c & mask) + (d & mask) + 0x00020002u;
                const auto ga = ((a >> 8) & mask) + ((b >> 8) & mask) +
                    ((c >> 8) & mask) + ((d >> 8) & mask) + 0x00020002u;
                const std::uint32_t result = ((rb >> 2) & mask) | (((ga >> 2) & mask) << 8);
                std::memcpy(out + std::size_t(x) * 4, &result, 4);
            }
        }
        previous = next; next += std::size_t(w) * h * 4; width = w; height = h; ++levels;
    }
}
#if defined(_WIN32)
bool display_window_graphics_panel(const ApplyGraphicsSettings &) { return false; }
#endif
} // namespace vcs
