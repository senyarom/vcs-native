#include "framebuffer_capture.hpp"
#include "ge_gpu_backend.hpp"
#include "ge_renderer.hpp"
#include "ge_matrix_stream.hpp"
#include "ge_texture_signature.hpp"
#include "vcs_profile.hpp"
#include "vcs_test_camera.hpp"
#include "vcs_config.hpp"

#include "psprecomp/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void test_savedata_autoload_filesystem() {
    // Production keeps saves beside the executable, outside the disc dump.
    const auto root = std::filesystem::current_path() / ("savedata-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() {
        std::error_code ignored; std::filesystem::remove_all(path, ignored);
    }} cleanup{root};
    std::filesystem::create_directories(root / "run");
    std::filesystem::create_directories(root / "disc/PSP/SAVEDATA/DISC_ONLY");
    vcs::initialize_vcs_configuration(root / "run");
    psprecomp::Runtime rt;
    rt.set_game_root(root / "disc");
    vcs::install_profile(rt, 0x08E8AC00u);
    auto &m = rt.memory();
    constexpr unsigned param = 0x08900000, data = 0x08901000,
                       path = 0x08902000, entry = 0x08903000, stat = 0x08904000,
                       tick = 0x08905000, best = tick + 8;
    auto string = [&](unsigned address, const std::string &s) {
        m.copy_in(address, std::span(reinterpret_cast<const std::uint8_t *>(s.c_str()), s.size()+1));
    };
    auto call = [&](const char *module, unsigned nid, std::initializer_list<unsigned> args) {
        psprecomp::AllegrexContext c{}; unsigned reg = 4;
        for (auto arg : args) c.gpr[reg++] = arg;
        rt.invoke_import(module, nid, c);
        require(!rt.stopped(), "savedata import stopped the runtime");
        return c.gpr[2];
    };
    auto utility = [&](unsigned mode, const std::string &slot) {
        m.zero(param, 0x600); m.store32(param, 0x600); m.store32(param+0x30, mode);
        string(param+0x3C, "ULUS10160"); string(param+0x4C, slot); string(param+0x64, "DATA.BIN");
        m.store32(param+0x74, data); m.store32(param+0x78, 4); m.store32(param+0x7C, 4);
        require(call("sceUtility", 0x50C4CD57, {param}) == 0, "savedata init failed");
        require(call("sceUtility", 0x8874DBE0, {}) == 1, "savedata INIT missing");
        require(call("sceUtility", 0xD4B95FFB, {1}) == 0, "savedata update failed");
        const auto result = m.load32(param+0x1C);
        require(call("sceUtility", 0x8874DBE0, {}) == 3, "savedata QUIT missing");
        require(call("sceUtility", 0x9790B33C, {}) == 0, "savedata shutdown failed");
        require(call("sceUtility", 0x8874DBE0, {}) == 4, "savedata FINISHED missing");
        return result;
    };
    for (unsigned i = 0; i < 3; ++i) {
        const auto slot = "S92F" + std::to_string(i);
        m.store32(data, 100 + i);
        require(utility(1, slot) == 0, "save failed");
        const auto directory = root / "run/SAVEDATA" / ("ULUS10160" + slot);
        require(std::filesystem::file_size(directory / "DATA.BIN") == 4, "save written outside userdata");
        // F1 is newest, not the first/last slot in directory order.
        const auto time = std::chrono::sys_days(std::chrono::year(2024)/1/(i == 1 ? 3 : i == 2 ? 2 : 1));
        // Reproduce an old build's stale folder date after overwriting DATA.BIN.
        std::filesystem::last_write_time(directory, std::filesystem::file_time_type::clock::from_sys(
            std::chrono::sys_days(std::chrono::year(2023)/1/(i+1))));
        std::filesystem::last_write_time(directory / "DATA.BIN",
            std::filesystem::file_time_type::clock::from_sys(time + std::chrono::hours(i)));
    }
    for (const std::string prefix : {"MS0:PSP/SAVEDATA/", "ms0:/PSP/SAVEDATA/",
                                      "fatms0:/psp/savedata/", "ms0:\\PSP\\SAVEDATA\\"}) {
        string(path, prefix);
        const auto fd = call("IoFileMgrForUser", 0xB29DDF9C, {path});
        require(fd < 0x80000000u, "startup cannot open memory-stick saves");
        m.zero(best, 8); std::string newest; unsigned count = 0;
        while (call("IoFileMgrForUser", 0xE3EB004C, {fd, entry}) == 1) {
            const auto name = m.read_c_string(entry+0x58);
            require(name.starts_with("ULUS10160S92F"), "startup enumerated disc data instead of saves");
            string(path, prefix + name);
            require(call("IoFileMgrForUser", 0xACE946E8, {path, stat}) == 0, "save stat failed");
            // Original VCS startup converts st_mtime and selects the largest tick.
            require(call("sceRtc", 0x6FF40ACC, {stat+0x30, tick}) == 0, "save timestamp invalid");
            if (static_cast<int>(call("sceRtc", 0x9ED0AE87, {tick, best})) > 0) {
                newest = name.substr(9); m.store32(best, m.load32(tick)); m.store32(best+4, m.load32(tick+4));
            }
            ++count;
        }
        require(call("IoFileMgrForUser", 0xEB092469, {fd}) == 0, "save directory close failed");
        require(count == 3 && newest == "S92F1", "latest save selection failed");
        m.store32(data, 0);
        require(utility(0, newest) == 0 && m.load32(data) == 101, "autoload and file listing disagree");
    }
    require(utility(2, "S92F0") == 0 && m.load32(data) == 100, "explicit slot load regressed");
    require(utility(0, "S92F9") == 0x80110307u, "missing save must report no data");
    const auto old_time = std::filesystem::last_write_time(root / "run/SAVEDATA/ULUS10160S92F0");
    m.store32(data, 999);
    require(utility(1, "S92F0") == 0 &&
            std::filesystem::last_write_time(root / "run/SAVEDATA/ULUS10160S92F0") > old_time,
            "overwriting a slot must refresh its timestamp for next startup");
    string(path, "MS0:PSP/SAVEDATA/ULUS10160S92F0/DATA.BIN");
    const auto file = call("IoFileMgrForUser", 0x109F50BC, {path, 1, 0});
    require(file < 0x80000000u, "direct savedata file open failed");
    m.store32(data, 0);
    require(call("IoFileMgrForUser", 0x6A638D83, {file, data, 4}) == 4 && m.load32(data) == 999,
            "direct file read and utility save disagree");
    call("IoFileMgrForUser", 0x810C4BC3, {file});
    string(path, "disc0:/PSP/SAVEDATA/");
    const auto fd = call("IoFileMgrForUser", 0xB29DDF9C, {path});
    require(call("IoFileMgrForUser", 0xE3EB004C, {fd, entry}) == 1 &&
            m.read_c_string(entry+0x58) == "DISC_ONLY", "disc paths were remapped to memory stick");
    call("IoFileMgrForUser", 0xEB092469, {fd});
    std::cout << "Savedata discovery/latest-slot/autoload/manual-load tests passed.\n";
}

void test_texture_hash_validation() {
    vcs::detail::PaletteChecksumCache cache;
    std::vector<std::uint8_t> palette(1024);
    for (std::size_t i = 0; i < palette.size(); ++i) palette[i] = (i * 173 + 37) & 255;
    for (unsigned size : {2, 4, 6, 32, 64, 512, 1024}) {
        std::span<const std::uint8_t> bytes{palette.data(), size};
        const auto expected = vcs::detail::palette_checksum(bytes);
        require(cache.get(0x08060000, bytes) == expected, "cold palette checksum");
        require(cache.get(0x48060000, bytes) == expected, "cached/uncached palette alias");
        for (unsigned i = 0; i < size; ++i) {
            palette[i] ^= 1;
            const auto changed = cache.get(0x08060000, bytes);
            require(changed != expected && changed == vcs::detail::palette_checksum(bytes),
                    "in-place palette byte write missed");
            palette[i] ^= 1;
            require(cache.get(0x08060000, bytes) == expected, "palette restore missed");
        }
    }
    // Address churn exceeds the fixed cache capacity; eviction cannot retain
    // another palette's hash even if its storage is recycled at the same address.
    for (unsigned i = 0; i < 2048; ++i) {
        palette[0] = i & 255;
        require(cache.get(0x08060000 + 1024 * i, palette) == vcs::detail::palette_checksum(palette),
                "palette cache collision reused different content");
    }
    // Exercise all associative ways, eviction and an in-place write while
    // repeatedly alternating distinct addresses that share one bucket.
    std::array<std::uint32_t, 6> colliding{};
    const auto bucket = [](std::uint32_t a) { return ((a >> 4) * 2654435761u) >> 24; };
    unsigned found = 0;
    for (std::uint32_t a = 0x08060000; found < colliding.size(); a += 16)
        if (bucket(a) == bucket(0x08060000)) colliding[found++] = a;
    std::array<std::array<std::uint8_t, 1024>, 6> palettes{};
    for (unsigned i = 0; i < palettes.size(); ++i) palettes[i].fill(i * 17);
    for (unsigned round = 0; round < 20; ++round) {
        for (unsigned i = 0; i < palettes.size(); ++i) {
            auto &bytes = palettes[(i + round) % palettes.size()];
            const auto a = colliding[(i + round) % palettes.size()];
            require(cache.get(a, bytes) == vcs::detail::palette_checksum(bytes), "associative palette lookup");
            bytes[round] ^= 1;
            require(cache.get(a, bytes) == vcs::detail::palette_checksum(bytes), "associative palette mutation");
        }
    }
    for (unsigned size : {1, 2, 63, 64, 255, 4096, 4097, 8192, 13017}) {
        std::vector<std::uint8_t> bytes(size);
        for (unsigned i = 0; i < size; ++i) bytes[i] = (i * 79 + 13) & 255;
        const auto expected = vcs::detail::texture_validation_hash(bytes);
        auto mutate = [&](unsigned offset) {
            bytes[offset] ^= 1;
            require(vcs::detail::texture_validation_hash(bytes) != expected,
                    "texture validation lost previously covered bytes");
            bytes[offset] ^= 1;
        };
        if (size <= 4096) {
            for (unsigned i = 0; i < size; ++i) mutate(i);
        } else {
            for (unsigned block = 0; block < 16; ++block) {
                const unsigned center = (size - 1) * block / 15;
                const unsigned start = center > 32 ? center - 32 : 0;
                for (unsigned i = start; i < std::min(size, start + 64); ++i) mutate(i);
            }
        }
    }
}

void require_rgb(const std::vector<std::uint8_t> &rgb, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                 const char *message) {
    require(rgb.size() == 3u, message);
    require(rgb[0] == r && rgb[1] == g && rgb[2] == b, message);
}



std::uint32_t ge_command(std::uint32_t command, std::uint32_t data) {
    return (command << 24u) | (data & 0x00FFFFFFu);
}

void put_sprite_vertex(psprecomp::GuestMemory &memory, std::uint32_t address,
                       std::uint16_t u, std::uint16_t v,
                       std::int16_t x, std::int16_t y, std::uint16_t z) {
    memory.store16(address + 0u, u);
    memory.store16(address + 2u, v);
    memory.store16(address + 4u, static_cast<std::uint16_t>(x));
    memory.store16(address + 6u, static_cast<std::uint16_t>(y));
    memory.store16(address + 8u, z);
}

std::array<std::uint32_t, 256> make_sprite_commands(std::uint32_t texture_address) {
    std::array<std::uint32_t, 256> commands{};
    commands[0x12] = ge_command(0x12u, 0x00800102u);  // through, tc16, pos16
    commands[0x1E] = ge_command(0x1Eu, 1u);           // texture enabled
    commands[0x9C] = ge_command(0x9Cu, 0u);           // framebuffer at VRAM base
    commands[0x9D] = ge_command(0x9Du, 4u);           // stride 4
    commands[0xA0] = ge_command(0xA0u, texture_address & 0x00FFFFF0u);
    commands[0xA8] = ge_command(0xA8u, ((texture_address >> 8u) & 0x000F0000u) | 2u);
    commands[0xB8] = ge_command(0xB8u, 0x0101u);      // 2x2
    commands[0xC2] = ge_command(0xC2u, 0u);           // linear layout
    commands[0xC3] = ge_command(0xC3u, 3u);           // RGBA8888
    commands[0xC6] = ge_command(0xC6u, 0u);           // nearest
    commands[0xC7] = ge_command(0xC7u, 0x0101u);      // clamp U/V
    commands[0xC9] = ge_command(0xC9u, 0x0103u);      // replace, use texture alpha
    commands[0xD2] = ge_command(0xD2u, 3u);           // framebuffer RGBA8888
    commands[0xD4] = ge_command(0xD4u, 0u);
    commands[0xD5] = ge_command(0xD5u, 3u | (3u << 10u));
    commands[0xE8] = ge_command(0xE8u, 0u);
    commands[0xE9] = ge_command(0xE9u, 0u);
    return commands;
}

void test_ge_sprite_renderer() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t texture = 0x08010000u;
    constexpr std::uint32_t vertices = 0x08020000u;
    memory.store32(texture + 0u, 0xFF0000FFu);  // red
    memory.store32(texture + 4u, 0xFF00FF00u);  // green
    memory.store32(texture + 8u, 0xFFFF0000u);  // blue
    memory.store32(texture + 12u, 0xFFFFFFFFu); // white
    put_sprite_vertex(memory, vertices + 0u, 0u, 0u, 0, 0, 0u);
    put_sprite_vertex(memory, vertices + 10u, 2u, 2u, 2, 2, 0u);

    auto commands = make_sprite_commands(texture);
    vcs::GeTransformState transform{};
    vcs::reset_ge_transform_state(transform);
    vcs::GeRenderStats stats{};
    std::string error;
    require(vcs::render_ge_primitive(memory, commands, transform, vertices, 0u, 0x00060002u, stats, error),
            error.empty() ? "GE sprite renderer failed" : error.c_str());
    require(stats.rectangles == 1u && stats.pixels_written == 4u, "GE sprite did not rasterize four pixels");
    require(memory.load32(0x04000000u + 0u) == 0xFF0000FFu, "GE sprite top-left texel mismatch");
    require(memory.load32(0x04000000u + 4u) == 0xFF00FF00u, "GE sprite top-right texel mismatch");
    require(memory.load32(0x04000000u + 16u) == 0xFFFF0000u, "GE sprite bottom-left texel mismatch");
    require(memory.load32(0x04000000u + 20u) == 0xFFFFFFFFu, "GE sprite bottom-right texel mismatch");
    require(stats.next_vertex_address == vertices + 20u, "GE vertex address did not advance by the packed stride");
}

void test_ge_scissor_and_blend() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t texture = 0x08010000u;
    constexpr std::uint32_t vertices = 0x08020000u;
    for (std::uint32_t i = 0; i < 4u; ++i) memory.store32(texture + i * 4u, 0x800000FFu);
    put_sprite_vertex(memory, vertices + 0u, 0u, 0u, 0, 0, 0u);
    put_sprite_vertex(memory, vertices + 10u, 2u, 2u, 2, 2, 0u);
    for (std::uint32_t y = 0; y < 2u; ++y)
        for (std::uint32_t x = 0; x < 2u; ++x)
            memory.store32(0x04000000u + (y * 4u + x) * 4u, 0xFFFF0000u); // blue

    auto commands = make_sprite_commands(texture);
    commands[0x21] = ge_command(0x21u, 1u);
    commands[0xDF] = ge_command(0xDFu, 2u | (3u << 4u)); // src alpha, inverse src alpha
    commands[0xD4] = ge_command(0xD4u, 1u);
    commands[0xD5] = ge_command(0xD5u, 1u | (1u << 10u));
    vcs::GeTransformState transform{};
    vcs::reset_ge_transform_state(transform);
    vcs::GeRenderStats stats{};
    std::string error;
    require(vcs::render_ge_primitive(memory, commands, transform, vertices, 0u, 0x00060002u, stats, error),
            error.empty() ? "GE blend renderer failed" : error.c_str());
    require(stats.pixels_written == 2u, "GE scissor did not restrict the sprite to one column");
    require(memory.load32(0x04000000u) == 0xFFFF0000u, "GE scissor modified the excluded column");
    const std::uint32_t blended = memory.load32(0x04000004u);
    const std::uint8_t red = static_cast<std::uint8_t>(blended);
    const std::uint8_t blue = static_cast<std::uint8_t>(blended >> 16u);
    require(red >= 126u && red <= 129u && blue >= 126u && blue <= 129u,
            "GE alpha blend did not produce the expected red/blue mix");
}


std::uint32_t ge_float24(float value) {
    return (std::bit_cast<std::uint32_t>(value) >> 8u) & 0x00FFFFFFu;
}

void put_3d_vertex(psprecomp::GuestMemory &memory, std::uint32_t address,
                   std::uint32_t color, float x, float y, float z) {
    memory.store32(address + 0u, color);
    memory.store32(address + 4u, std::bit_cast<std::uint32_t>(x));
    memory.store32(address + 8u, std::bit_cast<std::uint32_t>(y));
    memory.store32(address + 12u, std::bit_cast<std::uint32_t>(z));
}

void test_ge_matrix_persistence() {
    vcs::GeTransformState transform{};
    vcs::reset_ge_transform_state(transform);
    vcs::update_ge_transform_state(transform, 0x3Au, 0u);
    vcs::update_ge_transform_state(transform, 0x3Bu, ge_float24(2.0f));
    vcs::update_ge_transform_state(transform, 0x3Bu, ge_float24(3.0f));
    vcs::update_ge_transform_state(transform, 0x3Bu, ge_float24(4.0f));
    require(transform.world[0] == 2.0f && transform.world[1] == 3.0f && transform.world[2] == 4.0f,
            "GE matrix DATA words were not persisted");
    require(transform.world_cursor == 3u, "GE matrix cursor did not auto-increment");
}

std::array<std::uint32_t, 256> make_3d_point_commands(std::uint32_t vertex_type);
std::uint32_t point_pixel(const psprecomp::GuestMemory &memory, std::uint32_t x, std::uint32_t y);

void test_ge_matrix_stream() {
    for(unsigned command:{0x2b,0x3b,0x3d,0x3f,0x41}) {
        for(unsigned initial:{0,10,14,94,126}) {
            vcs::GeTransformState reference,stream;
            vcs::reset_ge_transform_state(reference);
            vcs::update_ge_transform_state(reference,command-1,initial);
            stream=reference;
            std::vector<std::uint8_t> bytes;
            const auto append=[&](unsigned op) { for(unsigned b=0;b<4;++b) bytes.push_back(op>>(b*8)); };
            for(unsigned i=0;i<150;++i) {
                const auto op=ge_command(command,ge_float24(float(i)*0.125f-3));
                append(op);vcs::update_ge_transform_state(reference,command,op&0xffffff);
            }
            append(ge_command(0x12,0x121)); // never consume another register
            require(vcs::consume_ge_matrix_stream(stream,command,bytes.data(),bytes.size()/4)==150,
                "matrix stream crossed next command");
            require(stream.bones==reference.bones && stream.world==reference.world && stream.view==reference.view &&
                stream.projection==reference.projection && stream.texture==reference.texture &&
                stream.bone_cursor==reference.bone_cursor && stream.world_cursor==reference.world_cursor &&
                stream.view_cursor==reference.view_cursor && stream.projection_cursor==reference.projection_cursor &&
                stream.texture_cursor==reference.texture_cursor,"matrix stream differs from scalar cursor behavior");
        }
    }
}

void test_ge_matrix_reserved_cursor_ranges() {
    vcs::GeTransformState transform{};
    vcs::reset_ge_transform_state(transform);

    transform.bones[0] = 7.0f;
    vcs::update_ge_transform_state(transform, 0x2Au, 96u);
    vcs::update_ge_transform_state(transform, 0x2Bu, ge_float24(9.0f));
    require(transform.bones[0] == 7.0f && transform.bone_cursor == 97u,
            "reserved GE bone cursor wrapped into bone zero");
    vcs::update_ge_transform_state(transform, 0x2Au, 127u);
    vcs::update_ge_transform_state(transform, 0x2Bu, ge_float24(11.0f));
    require(transform.bones[0] == 7.0f && transform.bone_cursor == 0u,
            "GE bone cursor did not wrap at its seven-bit boundary");
    vcs::update_ge_transform_state(transform, 0x2Bu, ge_float24(13.0f));
    require(transform.bones[0] == 13.0f && transform.bone_cursor == 1u,
            "GE bone cursor did not resume at zero after the hardware wrap");

    transform.world[0] = 17.0f;
    vcs::update_ge_transform_state(transform, 0x3Au, 12u);
    for (std::uint32_t i = 0u; i < 4u; ++i)
        vcs::update_ge_transform_state(transform, 0x3Bu, ge_float24(20.0f + static_cast<float>(i)));
    require(transform.world[0] == 17.0f && transform.world_cursor == 0u,
            "reserved GE world cursor wrapped through modulo 12");
    vcs::update_ge_transform_state(transform, 0x3Bu, ge_float24(29.0f));
    require(transform.world[0] == 29.0f && transform.world_cursor == 1u,
            "GE world cursor did not resume after the four-bit wrap");
}

void test_ge_bounding_box_visibility_and_streams() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t vertices = 0x0802E000u;
    put_3d_vertex(memory, vertices + 0u, 0xFFFFFFFFu, -0.5f, -0.5f, 0.0f);
    put_3d_vertex(memory, vertices + 16u, 0xFFFFFFFFu,  0.5f, -0.5f, 0.0f);
    put_3d_vertex(memory, vertices + 32u, 0xFFFFFFFFu,  0.0f,  0.5f, 0.0f);

    std::array<std::uint32_t, 256> commands{};
    commands[0x12] = ge_command(0x12u, (7u << 2u) | (3u << 7u));
    commands[0x1C] = ge_command(0x1Cu, 1u);
    vcs::GeTransformState transform{};
    vcs::reset_ge_transform_state(transform);

    vcs::GeBoundingBoxResult result{};
    std::string error;
    require(vcs::test_ge_bounding_box(memory, commands, transform, vertices, 0u, 3u, result, error),
            error.empty() ? "GE BBOX inside test failed" : error.c_str());
    require(result.visible && result.next_vertex_address == vertices + 48u && result.next_index_address == 0u,
            "GE BBOX rejected visible geometry or advanced the wrong stream");

    transform.world[9] = 4.0f;
    require(vcs::test_ge_bounding_box(memory, commands, transform, vertices, 0u, 3u, result, error),
            error.empty() ? "GE BBOX outside test failed" : error.c_str());
    require(!result.visible && result.next_vertex_address == vertices + 48u,
            "GE BBOX failed to reject a box fully outside one clip plane");

    result = {};
    require(vcs::test_ge_bounding_box(memory, commands, transform, vertices, 0u, 0u, result, error),
            "GE BBOX count-zero reset failed");
    require(!result.visible && result.next_vertex_address == vertices && result.next_index_address == 0u,
            "GE BBOX count zero did not reset without consuming vertices");

    constexpr std::uint32_t indices = 0x0802F000u;
    memory.store16(indices + 0u, 0u);
    memory.store16(indices + 2u, 1u);
    memory.store16(indices + 4u, 2u);
    commands[0x12] = ge_command(0x12u, (7u << 2u) | (3u << 7u) | (2u << 11u));
    vcs::reset_ge_transform_state(transform);
    require(vcs::test_ge_bounding_box(memory, commands, transform, vertices, indices, 3u, result, error),
            error.empty() ? "GE indexed BBOX test failed" : error.c_str());
    require(result.visible && result.next_vertex_address == vertices && result.next_index_address == indices + 6u,
            "GE indexed BBOX did not advance only the index stream");
}

void test_ge_depth_clip_enable_state() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t vertices = 0x0802F800u;
    put_3d_vertex(memory, vertices, 0xFF3366CCu, 0.0f, 0.0f, 2.0f);
    auto commands = make_3d_point_commands((7u << 2u) | (3u << 7u));
    vcs::GeTransformState transform{};
    vcs::reset_ge_transform_state(transform);

    vcs::GeRenderStats stats{};
    std::string error;
    commands[0x1C] = ge_command(0x1Cu, 1u);
    require(vcs::render_ge_primitive(memory, commands, transform, vertices, 0u, 0x00000001u, stats, error),
            error.empty() ? "GE depth-clipped point failed" : error.c_str());
    require(stats.pixels_written == 0u,
            "GE rendered a point beyond +W while DEPTHCLIPENABLE was set");

    memory.zero(0x04000000u, 8u * 8u * 4u);
    stats = {};
    commands[0x1C] = ge_command(0x1Cu, 0u);
    require(vcs::render_ge_primitive(memory, commands, transform, vertices, 0u, 0x00000001u, stats, error),
            error.empty() ? "GE depth-clamp point failed" : error.c_str());
    require(stats.pixels_written == 1u && point_pixel(memory, 4u, 4u) == 0xFF3366CCu,
            "GE incorrectly applied Z clip planes while DEPTHCLIPENABLE was clear");
}

void test_ge_unsigned_raster_offset() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t vertices = 0x0802FC00u;
    put_3d_vertex(memory, vertices, 0xFF55AA11u, 0.0f, 0.0f, 0.0f);
    auto commands = make_3d_point_commands((7u << 2u) | (3u << 7u));
    commands[0x42] = ge_command(0x42u, ge_float24(0.0f));
    commands[0x45] = ge_command(0x45u, ge_float24(2048.0f));
    commands[0x4C] = ge_command(0x4Cu, 0x8000u);  // unsigned 12.4 == 2048.0
    commands[0x46] = ge_command(0x46u, ge_float24(1.0f));

    vcs::GeTransformState transform{};
    vcs::reset_ge_transform_state(transform);
    vcs::GeRenderStats stats{};
    std::string error;
    require(vcs::render_ge_primitive(memory, commands, transform, vertices, 0u, 0x00000001u, stats, error),
            error.empty() ? "GE raster offset point failed" : error.c_str());
    require(stats.pixels_written == 1u && point_pixel(memory, 0u, 1u) == 0xFF55AA11u,
            "GE OFFSETX bit 15 was interpreted as a sign bit instead of unsigned 12.4");
}

void test_ge_non_through_triangle() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t vertices = 0x08030000u;
    put_3d_vertex(memory, vertices + 0u, 0xFF0000FFu, -0.75f, -0.75f, 0.0f);
    put_3d_vertex(memory, vertices + 16u, 0xFF00FF00u, 0.75f, -0.75f, 0.0f);
    put_3d_vertex(memory, vertices + 32u, 0xFFFF0000u, 0.0f, 0.75f, 0.0f);

    std::array<std::uint32_t, 256> commands{};
    commands[0x12] = ge_command(0x12u, (7u << 2u) | (3u << 7u)); // color8888, pos float, transform 3D
    commands[0x42] = ge_command(0x42u, ge_float24(2.0f));
    commands[0x43] = ge_command(0x43u, ge_float24(2.0f));
    commands[0x44] = ge_command(0x44u, ge_float24(32767.5f));
    commands[0x45] = ge_command(0x45u, ge_float24(2.0f));
    commands[0x46] = ge_command(0x46u, ge_float24(2.0f));
    commands[0x47] = ge_command(0x47u, ge_float24(32767.5f));
    commands[0x9C] = ge_command(0x9Cu, 0u);
    commands[0x9D] = ge_command(0x9Du, 4u);
    commands[0xD2] = ge_command(0xD2u, 3u);
    commands[0xD4] = ge_command(0xD4u, 0u);
    commands[0xD5] = ge_command(0xD5u, 3u | (3u << 10u));
    commands[0xE8] = ge_command(0xE8u, 0u);
    commands[0xE9] = ge_command(0xE9u, 0u);

    vcs::GeTransformState transform{};
    vcs::reset_ge_transform_state(transform);
    vcs::GeRenderStats stats{};
    std::string error;
    require(vcs::render_ge_primitive(memory, commands, transform, vertices, 0u,
                                     0x00030003u, stats, error),
            error.empty() ? "GE transformed triangle failed" : error.c_str());
    require(stats.triangles == 1u, "GE triangle topology was not emitted");
    require(stats.pixels_written > 0u, "GE transformed triangle produced no framebuffer pixels");
    require(stats.next_vertex_address == vertices + 48u, "GE transformed vertex stream did not advance");
}


std::array<std::uint32_t, 256> make_3d_point_commands(std::uint32_t vertex_type) {
    std::array<std::uint32_t, 256> commands{};
    commands[0x12] = ge_command(0x12u, vertex_type);
    commands[0x42] = ge_command(0x42u, ge_float24(4.0f));
    commands[0x43] = ge_command(0x43u, ge_float24(4.0f));
    commands[0x44] = ge_command(0x44u, ge_float24(32767.5f));
    commands[0x45] = ge_command(0x45u, ge_float24(4.0f));
    commands[0x46] = ge_command(0x46u, ge_float24(4.0f));
    commands[0x47] = ge_command(0x47u, ge_float24(32767.5f));
    commands[0x9C] = ge_command(0x9Cu, 0u);
    commands[0x9D] = ge_command(0x9Du, 8u);
    commands[0xD2] = ge_command(0xD2u, 3u);
    commands[0xD4] = ge_command(0xD4u, 0u);
    commands[0xD5] = ge_command(0xD5u, 7u | (7u << 10u));
    commands[0xE8] = ge_command(0xE8u, 0u);
    commands[0xE9] = ge_command(0xE9u, 0u);
    return commands;
}

std::uint32_t point_pixel(const psprecomp::GuestMemory &memory, std::uint32_t x, std::uint32_t y) {
    return memory.load32(0x04000000u + (y * 8u + x) * 4u);
}

#if !defined(_WIN32)
void test_ge_indexed_packed_frontend() {
    // These controls are cached by the frontend on first use. After shutdown,
    // the software tests below still rasterize because no GPU backend is active.
    setenv("PSPRECOMP_GE_GPU_HW_TRANSFORM", "1", 1);
    setenv("PSPRECOMP_GE_GPU_SKIP_SOFTWARE_RASTER", "1", 1);
    setenv("PSPRECOMP_GE_DIRECT_NONINDEXED_DRAW", "0", 1);
    setenv("PSPRECOMP_DX12_PACKED_0115", "1", 1);
    constexpr std::uint32_t vertices = 0x08040000u, indices = 0x08044000u;
    constexpr std::array<std::uint32_t, 6> order{0, 2, 1, 2, 3, 1};
    constexpr std::array<std::array<std::int16_t, 2>, 4> positions{{
        {-16384, -16384}, {16384, -16384}, {-16384, 16384}, {16384, 16384}}};
    constexpr std::array<std::uint16_t, 4> colors{0x801F, 0x83E0, 0xFC00, 0xFFFF};
    std::vector<std::byte> reference;
    for (std::uint32_t index_type = 0; index_type <= 2; ++index_type) {
        psprecomp::GuestMemory memory;
        const std::uint32_t first = index_type == 2 ? 300u : 3u;
        const std::uint32_t source_count = index_type == 0 ? 6u : 4u;
        for (std::uint32_t i = 0; i < source_count; ++i) {
            const auto vertex = index_type == 0 ? order[i] : i;
            const auto address = vertices + (index_type == 0 ? i : first + i) * 10u;
            memory.store8(address, 0);
            memory.store8(address + 1, 0);
            memory.store16(address + 2, colors[vertex]);
            memory.store16(address + 4, static_cast<std::uint16_t>(positions[vertex][0]));
            memory.store16(address + 6, static_cast<std::uint16_t>(positions[vertex][1]));
            memory.store16(address + 8, 0);
        }
        for (std::uint32_t i = 0; i < order.size(); ++i) {
            if (index_type == 1) memory.store8(indices + i, first + order[i]);
            if (index_type == 2) memory.store16(indices + i * 2, first + order[i]);
        }
        auto commands = make_3d_point_commands(0x0115u | (index_type << 11u));
        commands[0x42] = ge_command(0x42, ge_float24(240));
        commands[0x43] = ge_command(0x43, ge_float24(136));
        commands[0x45] = ge_command(0x45, ge_float24(240));
        commands[0x46] = ge_command(0x46, ge_float24(136));
        commands[0x50] = ge_command(0x50, 1); // smooth shading
        commands[0x9D] = ge_command(0x9D, 512);
        commands[0xD5] = ge_command(0xD5, 479u | (271u << 10u));
        vcs::GeTransformState transform{};
        vcs::reset_ge_transform_state(transform);
        vcs::VulkanBackendConfiguration config;
        config.width = config.height = 64;
        std::string error;
        require(vcs::initialize_vulkan_backend(config, error), error.c_str());
        vcs::ge_gpu_backend_set_display_framebuffer(0);
        vcs::GeRenderStats stats{};
        require(vcs::render_ge_primitive(memory, commands, transform, vertices,
                    index_type == 0 ? 0 : indices, 0x00030006u, stats, error), error.c_str());
        require(vcs::ge_gpu_backend_finish_color_frame(1), "indexed frontend frame failed");
        const auto &report = vcs::ge_gpu_backend_report();
        require(report.hw_transform_draw_calls == 1, "indexed frontend missed hardware transform");
        require(report.staged_bytes == source_count * 10u + order.size() * 4u,
                "indexed frontend expanded packed vertices on CPU");
        const auto image = vcs::ge_gpu_backend_game_frame_rgba();
        require(image.size() == 64 * 64 * 4, "indexed frontend image dimensions");
        require(std::to_integer<unsigned>(image[(32 * 64 + 32) * 4 + 1]) > 30,
                "indexed frontend produced no visible geometry");
        if (index_type == 0) reference.assign(image.begin(), image.end());
        else require(std::equal(reference.begin(), reference.end(), image.begin()),
                     "indexed packed frontend differs from expanded reference");
        if (index_type == 0) {
            constexpr std::uint32_t texture = 0x08050000u, palette = 0x08060000u;
            auto textured = make_sprite_commands(texture);
            for (unsigned reg : {0x1e, 0xa0, 0xa8, 0xb8, 0xc2, 0xc6, 0xc7, 0xc9})
                commands[reg] = textured[reg];
            commands[0xc3] = ge_command(0xc3, 5); // T8 indices
            commands[0xb0] = ge_command(0xb0, palette & 0xffffff);
            commands[0xb1] = ge_command(0xb1, (palette >> 8) & 0xf0000);
            commands[0xc5] = ge_command(0xc5, 3 | (1 << 8)); // two RGBA entries
            memory.store32(texture, 0);
            memory.store32(palette, 0xff0000ff);
            memory.store32(palette + 4, 0xffff0000);
            auto draw_color = [&](unsigned frame, unsigned channel) {
                require(vcs::render_ge_primitive(memory, commands, transform, vertices, 0,
                            0x00030006u, stats, error), error.c_str());
                require(vcs::ge_gpu_backend_finish_color_frame(frame), "mutable texture frame");
                const auto pixels = vcs::ge_gpu_backend_game_frame_rgba();
                const auto center = (32 * 64 + 32) * 4;
                for (unsigned c = 0; c < 3; ++c)
                    require(std::to_integer<unsigned>(pixels[center + c]) == (c == channel ? 255 : 0),
                            "GPU image retained stale palette/texture content");
                return std::vector<std::byte>(pixels.begin(), pixels.end());
            };
            const auto red = draw_color(2, 0);
            require(draw_color(3, 0) == red, "warm validation changed image bytes");
            memory.store32(palette, 0xff00ff00);
            draw_color(4, 1);
            // Direct-pointer writes bypass GE commands and memory accessors.
            *memory.raw_pointer(texture, 1) = 1;
            draw_color(5, 2);
            *memory.raw_pointer(texture, 1) = 0;
            memory.store32(palette, 0xff0000ff);
            require(draw_color(6, 0) == red, "restored texture/palette image differs");
        }
        vcs::shutdown_ge_gpu_backend();
    }
}
void test_ge_raw_model_frontend() {
    // Compare the real GE frontend in one binary with GPU model decode on/off.
    // Independent packed records cover integer/float attributes, eight bones,
    // all color encodings, direct/projective/environment UVs and all light types.
    constexpr unsigned vertices=0x08040000, indices=0x08070000, texture=0x08050000;
    constexpr std::array<unsigned,6> order{0,2,1,2,3,1};
    struct Case { unsigned tc,color,normal,pos,weight,uv,light,update; };
    const std::array<Case,12> cases{{
        {1,0,1,2,0,0,0,0}, {2,4,2,1,0,0,1,7}, {3,5,3,3,0,0,2,3},
        {1,6,1,2,1,0,3,0}, {2,7,2,3,2,0,4,7}, {3,0,3,3,3,0,1,0},
        {1,7,1,2,1,1,0,0}, {2,4,2,3,2,2,1,7}, {3,6,3,3,3,3,2,1},
        {1,5,0,2,0,4,3,2}, {2,7,1,2,1,5,0,0}, {3,0,2,3,2,5,4,0}}};
    for(unsigned ci=0;ci<cases.size();++ci) {
        const auto c=cases[ci];
        psprecomp::GuestMemory memory;
        const auto size=[](unsigned t) { return t==3?4u:t; };
        const auto align=[](unsigned v,unsigned a) { return (v+a-1)&~(a-1); };
        unsigned offset=c.weight?8*size(c.weight):0;
        offset=align(offset,size(c.tc));const unsigned tc=offset;offset+=2*size(c.tc);
        const unsigned color_size=c.color==7?4:2;
        offset=align(offset,c.color?color_size:1);const unsigned col=offset;if(c.color)offset+=color_size;
        offset=align(offset,c.normal?size(c.normal):1);const unsigned norm=offset;offset+=3*size(c.normal);
        offset=align(offset,size(c.pos));const unsigned pos=offset;offset+=3*size(c.pos);
        const unsigned stride=align(offset,std::max({size(c.tc),c.color?color_size:1u,
            c.normal?size(c.normal):1u,size(c.pos),c.weight?size(c.weight):1u}));
        const unsigned index_type=ci%3,first=index_type==2?300:3,source_count=index_type?4:6;
        const unsigned type=c.tc|(c.color<<2)|(c.normal<<5)|(c.pos<<7)|(c.weight<<9)|
            (index_type<<11)|(c.weight?7u<<14:0);
        const auto scalar=[&](unsigned p,unsigned t,float v) {
            if(t==1) memory.store8(p,static_cast<unsigned>(static_cast<int>(v*128)));
            if(t==2) memory.store16(p,static_cast<unsigned>(static_cast<int>(v*32768)));
            if(t==3) memory.store32(p,std::bit_cast<unsigned>(v));
        };
        const auto write_vertices=[&]() {
            for(unsigned i=0;i<source_count;++i) {
                const unsigned v=index_type?i:order[i],p=vertices+(index_type?first+i:i)*stride;
                const float x=(v&1)?0.5f:-0.5f,y=(v&2)?0.5f:-0.5f;
                if(c.weight) for(unsigned b=0;b<8;++b) scalar(p+b*size(c.weight),c.weight,b==0?0.25f:(b==7?0.75f:0));
                scalar(p+tc,c.tc,(v&1)?0.75f:0.125f);scalar(p+tc+size(c.tc),c.tc,(v&2)?0.75f:0.125f);
                if(c.color==7) memory.store32(p+col,0xdd638ba7u+v*0x03050201u);
                else if(c.color) memory.store16(p+col,0xab93u+v*0x123u);
                if(c.normal) {
                    scalar(p+norm,c.normal,0.25f);scalar(p+norm+size(c.normal),c.normal,0.25f);
                    scalar(p+norm+2*size(c.normal),c.normal,0.75f);
                }
                scalar(p+pos,c.pos,x);scalar(p+pos+size(c.pos),c.pos,y);scalar(p+pos+2*size(c.pos),c.pos,0);
            }
        };
        for(unsigned i=0;i<6;++i) {
            if(index_type==1) memory.store8(indices+i,first+order[i]);
            if(index_type==2) memory.store16(indices+i*2,first+order[i]);
        }
        auto commands=make_3d_point_commands(type);
        const auto reg=[&](unsigned r,unsigned v) { commands[r]=ge_command(r,v); };
        const auto fl=[&](unsigned r,float v) { reg(r,ge_float24(v)); };
        fl(0x42,240);fl(0x43,136);fl(0x45,240);fl(0x46,136);
        reg(0x50,1);reg(0x9D,512);reg(0xD5,479|(271u<<10));
        reg(0x55,0x5e837b);reg(0x58,221);reg(0x56,0x697483);reg(0x57,0x333739);
        reg(0x54,0x040607);reg(0x5c,0x283946);reg(0x5d,239);fl(0x5b,3.5f);
        reg(0x17,c.light?1:0);reg(0x53,c.update);reg(0x51,ci%2);
        for(unsigned i=0;i<4;++i) {
            reg(0x18+i,c.light?1:0);
            reg(0x5f+i,((c.light+i-1)%4<<8)|((ci+i)%3));
            fl(0x63+i*3,0.5f);fl(0x64+i*3,-0.5f);fl(0x65+i*3,1.5f);
            fl(0x7b+i*3,0.8f);fl(0x7c+i*3,0.1f);fl(0x7d+i*3,0.02f);
            fl(0x6f+i*3,0);fl(0x70+i*3,0);fl(0x71+i*3,1);
            fl(0x8b+i,-0.75f);fl(0x87+i,2);
            reg(0x8f+i*3,0x0b0907);reg(0x90+i*3,0x171d23);reg(0x91+i*3,0x101316);
        }
        if(c.uv) {
            const auto textured=make_sprite_commands(texture);
            for(unsigned r:{0x1e,0xa0,0xa8,0xb8,0xc2,0xc3,0xc6,0xc7}) commands[r]=textured[r];
            reg(0xc9,0x100); // modulate, preserving lighting in this comparison
            reg(0xc0,c.uv==5?2:(1|((c.uv-1)<<8)));reg(0xc1,0x0100);
            for(unsigned i=0;i<4;++i)memory.store32(texture+i*4,0xff456789+i*0x122319);
        }
        vcs::GeTransformState transform;vcs::reset_ge_transform_state(transform);
        transform.world[0]=0.75f;transform.world[4]=0.75f;transform.world[8]=1.25f;
        transform.world[9]=0.0625f;transform.texture[11]=1;
        transform.bones[7*12+9]=0.125f;transform.bones[7*12+4]=0.75f;
        std::vector<std::byte> reference;
        for(unsigned gpu=0;gpu<2;++gpu) {
            setenv("PSPRECOMP_GE_GPU_RAW_MODEL",gpu?"1":"0",1);
            vcs::VulkanBackendConfiguration config;config.width=config.height=64;
            config.async_readback=(ci%2)==1;
            std::string error;require(vcs::initialize_vulkan_backend(config,error),error.c_str());
            vcs::ge_gpu_backend_set_display_framebuffer(0);
            write_vertices();
            vcs::GeRenderStats stats{};
            require(vcs::render_ge_primitive(memory,commands,transform,vertices,index_type?indices:0,
                0x00030006,stats,error),error.c_str());
            // Guest is allowed to overwrite its stream immediately after PRIM.
            // Backend must have snapshotted both records and per-draw constants.
            std::fill_n(memory.raw_pointer(vertices,(first+source_count)*stride),(first+source_count)*stride,0);
            const bool ready=vcs::ge_gpu_backend_finish_color_frame(1);
            if(config.async_readback) {
                require(!ready,"raw asynchronous submission unexpectedly read back immediately");
                require(vcs::ge_gpu_backend_finish_color_frame(2),"raw asynchronous fence/readback failed");
            } else require(ready,"raw frontend frame failed");
            const auto report=vcs::ge_gpu_backend_report();
            require(report.raw_model_draw_calls==gpu,"raw model gate did not select expected path");
            require(report.staged_vertices==source_count,"raw upload omitted vertices from accounting");
            auto image=vcs::ge_gpu_backend_game_frame_rgba();
            require(std::to_integer<unsigned>(image[(32*64+32)*4+3])>0,"raw test has no visible model");
            if(!gpu)reference.assign(image.begin(),image.end());
            else {
                unsigned max_delta=0,changed=0;
                for(unsigned i=0;i<image.size();++i) {
                    unsigned d=std::abs(int(std::to_integer<unsigned>(image[i]))-int(std::to_integer<unsigned>(reference[i])));
                    max_delta=std::max(max_delta,d);changed+=d!=0;
                }
                std::cout<<"raw-model parity case="<<ci<<" max_channel_delta="<<max_delta<<" changed="<<changed<<'\n';
                // GPU pow/sqrt can move a rounded vertex color by one 8-bit unit.
                // Coverage, UV sampling and geometry must still agree.
                require(max_delta<=1,"GPU raw model differs from CPU reference");
            }
            vcs::shutdown_ge_gpu_backend();
        }
    }
    unsetenv("PSPRECOMP_GE_GPU_RAW_MODEL");
}

#endif

void test_ge_two_bone_skinning() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t vertices = 0x08031000u;
    // weight8 x2, then alignment padding, color8888, position float.
    memory.store8(vertices + 0u, 64u);
    memory.store8(vertices + 1u, 64u);
    memory.store32(vertices + 4u, 0xFF0000FFu);
    memory.store32(vertices + 8u, std::bit_cast<std::uint32_t>(-0.5f));
    memory.store32(vertices + 12u, std::bit_cast<std::uint32_t>(0.0f));
    memory.store32(vertices + 16u, std::bit_cast<std::uint32_t>(0.0f));

    const std::uint32_t type = (7u << 2u) | (3u << 7u) | (1u << 9u) | (1u << 14u);
    auto commands = make_3d_point_commands(type);
    vcs::GeTransformState transform{};
    vcs::reset_ge_transform_state(transform);
    transform.bones[12u + 9u] = 1.0f;

    vcs::GeRenderStats stats{};
    std::string error;
    require(vcs::render_ge_primitive(memory, commands, transform, vertices, 0u, 0x00000001u, stats, error),
            error.empty() ? "GE skinned point failed" : error.c_str());
    require(point_pixel(memory, 4u, 4u) == 0xFF0000FFu,
            "GE two-bone blend did not move the point to the weighted position");
    require(stats.skinned_vertices == 1u && stats.next_vertex_address == vertices + 20u,
            "GE skinning statistics or packed stride are incorrect");
}

void test_ge_morph_targets() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t vertices = 0x08032000u;
    put_3d_vertex(memory, vertices + 0u, 0xFF0000FFu, -0.5f, 0.0f, 0.0f);
    put_3d_vertex(memory, vertices + 16u, 0xFFFF0000u, 0.5f, 0.0f, 0.0f);

    const std::uint32_t type = (7u << 2u) | (3u << 7u) | (1u << 18u);
    auto commands = make_3d_point_commands(type);
    vcs::GeTransformState transform{};
    vcs::reset_ge_transform_state(transform);
    transform.morph_weights[0] = 0.25f;
    transform.morph_weights[1] = 0.75f;

    vcs::GeRenderStats stats{};
    std::string error;
    require(vcs::render_ge_primitive(memory, commands, transform, vertices, 0u, 0x00000001u, stats, error),
            error.empty() ? "GE morphed point failed" : error.c_str());
    require(point_pixel(memory, 5u, 4u) == 0xFFBF003Fu,
            "GE morph weights did not blend point position and color");
    require(stats.morphed_vertices == 1u && stats.next_vertex_address == vertices + 32u,
            "GE morph statistics or repeated target stride are incorrect");
}

void test_ge_directional_lighting() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t vertices = 0x08033000u;
    // color8888, normal float3, position float3: 28-byte stride.
    memory.store32(vertices + 0u, 0xFF0000FFu);
    memory.store32(vertices + 4u, std::bit_cast<std::uint32_t>(0.0f));
    memory.store32(vertices + 8u, std::bit_cast<std::uint32_t>(0.0f));
    memory.store32(vertices + 12u, std::bit_cast<std::uint32_t>(1.0f));
    memory.store32(vertices + 16u, std::bit_cast<std::uint32_t>(0.0f));
    memory.store32(vertices + 20u, std::bit_cast<std::uint32_t>(0.0f));
    memory.store32(vertices + 24u, std::bit_cast<std::uint32_t>(0.0f));

    const std::uint32_t type = (7u << 2u) | (3u << 5u) | (3u << 7u);
    auto commands = make_3d_point_commands(type);
    commands[0x17] = ge_command(0x17u, 1u);
    commands[0x18] = ge_command(0x18u, 1u);
    commands[0x53] = ge_command(0x53u, 2u);       // vertex color supplies diffuse material
    commands[0x58] = ge_command(0x58u, 255u);
    commands[0x5D] = ge_command(0x5Du, 255u);
    commands[0x5F] = ge_command(0x5Fu, 0u);       // directional, diffuse
    commands[0x63] = ge_command(0x63u, ge_float24(0.0f));
    commands[0x64] = ge_command(0x64u, ge_float24(0.0f));
    commands[0x65] = ge_command(0x65u, ge_float24(1.0f));
    commands[0x90] = ge_command(0x90u, 0x00FFFFFFu);

    vcs::GeTransformState transform{};
    vcs::reset_ge_transform_state(transform);
    vcs::GeRenderStats stats{};
    std::string error;
    require(vcs::render_ge_primitive(memory, commands, transform, vertices, 0u, 0x00000001u, stats, error),
            error.empty() ? "GE lit point failed" : error.c_str());
    require(point_pixel(memory, 4u, 4u) == 0xFF0000FFu,
            "GE directional diffuse lighting did not preserve the red material color");
    require(stats.lit_vertices == 1u, "GE lighting statistics did not record the vertex");

    memory.store32(0x04000000u + (4u * 8u + 4u) * 4u, 0u);
    commands[0x51] = ge_command(0x51u, 1u);
    stats = {};
    require(vcs::render_ge_primitive(memory, commands, transform, vertices, 0u, 0x00000001u, stats, error),
            "GE reversed-normal lighting failed");
    require((point_pixel(memory, 4u, 4u) & 0x00FFFFFFu) == 0u,
            "GE reverse-normal state did not suppress the opposing directional diffuse light");
}


void configure_test_texture(std::array<std::uint32_t, 256> &commands, std::uint32_t texture) {
    commands[0x1E] = ge_command(0x1Eu, 1u);
    commands[0xA0] = ge_command(0xA0u, texture & 0x00FFFFF0u);
    commands[0xA8] = ge_command(0xA8u, ((texture >> 8u) & 0x000F0000u) | 2u);
    commands[0xB8] = ge_command(0xB8u, 0x0101u);
    commands[0xC2] = ge_command(0xC2u, 0u);
    commands[0xC3] = ge_command(0xC3u, 3u);
    commands[0xC6] = ge_command(0xC6u, 0u);
    commands[0xC7] = ge_command(0xC7u, 0x0101u);
    commands[0xC9] = ge_command(0xC9u, 0x0103u);
}

void put_lit_3d_vertex(psprecomp::GuestMemory &memory, std::uint32_t address,
                       std::uint32_t color, float nx, float ny, float nz,
                       float x, float y, float z) {
    memory.store32(address + 0u, color);
    memory.store32(address + 4u, std::bit_cast<std::uint32_t>(nx));
    memory.store32(address + 8u, std::bit_cast<std::uint32_t>(ny));
    memory.store32(address + 12u, std::bit_cast<std::uint32_t>(nz));
    memory.store32(address + 16u, std::bit_cast<std::uint32_t>(x));
    memory.store32(address + 20u, std::bit_cast<std::uint32_t>(y));
    memory.store32(address + 24u, std::bit_cast<std::uint32_t>(z));
}

void test_ge_texture_matrix_uv_generation() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t texture = 0x08014000u;
    constexpr std::uint32_t vertices = 0x08034000u;
    memory.store32(texture + 0u, 0xFF0000FFu);
    memory.store32(texture + 4u, 0xFF00FF00u);
    memory.store32(texture + 8u, 0xFFFF0000u);
    memory.store32(texture + 12u, 0xFFFFFFFFu);
    put_3d_vertex(memory, vertices, 0xFFFFFFFFu, 0.0f, 0.0f, 0.0f);

    auto commands = make_3d_point_commands((7u << 2u) | (3u << 7u));
    configure_test_texture(commands, texture);
    commands[0xC0] = ge_command(0xC0u, 1u); // texture matrix, position source

    vcs::GeTransformState transform{};
    vcs::reset_ge_transform_state(transform);
    transform.texture.fill(0.0f);
    transform.texture[9u] = 1.5f;
    transform.texture[10u] = 0.5f;
    transform.texture[11u] = 2.0f;

    vcs::GeRenderStats stats{};
    std::string error;
    require(vcs::render_ge_primitive(memory, commands, transform, vertices, 0u, 0x00000001u, stats, error),
            error.empty() ? "GE texture-matrix point failed" : error.c_str());
    require(point_pixel(memory, 4u, 4u) == 0xFF00FF00u,
            "GE texture matrix or projective Q did not select the expected texel");
    require(stats.generated_uv_vertices == 1u, "GE texture-matrix UV generation was not recorded");
}

void test_ge_environment_map_uv_generation() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t texture = 0x08015000u;
    constexpr std::uint32_t vertices = 0x08035000u;
    memory.store32(texture + 0u, 0xFF0000FFu);
    memory.store32(texture + 4u, 0xFF00FF00u);
    memory.store32(texture + 8u, 0xFFFF0000u);
    memory.store32(texture + 12u, 0xFFFFFFFFu);
    put_lit_3d_vertex(memory, vertices, 0xFFFFFFFFu, 1.0f, 0.0f, 0.0f,
                      0.0f, 0.0f, 0.0f);

    auto commands = make_3d_point_commands((7u << 2u) | (3u << 5u) | (3u << 7u));
    configure_test_texture(commands, texture);
    commands[0xC0] = ge_command(0xC0u, 2u);       // environment map
    commands[0xC1] = ge_command(0xC1u, 0u | (1u << 8u));
    commands[0x63] = ge_command(0x63u, ge_float24(1.0f));
    commands[0x64] = ge_command(0x64u, ge_float24(0.0f));
    commands[0x65] = ge_command(0x65u, ge_float24(0.0f));
    commands[0x66] = ge_command(0x66u, ge_float24(-1.0f));
    commands[0x67] = ge_command(0x67u, ge_float24(0.0f));
    commands[0x68] = ge_command(0x68u, ge_float24(0.0f));

    vcs::GeTransformState transform{};
    vcs::reset_ge_transform_state(transform);
    vcs::GeRenderStats stats{};
    std::string error;
    require(vcs::render_ge_primitive(memory, commands, transform, vertices, 0u, 0x00000001u, stats, error),
            error.empty() ? "GE environment-map point failed" : error.c_str());
    require(point_pixel(memory, 4u, 4u) == 0xFF00FF00u,
            "GE environment-map light selection did not generate the expected ST coordinates");
    require(stats.generated_uv_vertices == 1u, "GE environment-map UV generation was not recorded");
}

void put_through_triangle_vertex(psprecomp::GuestMemory &memory, std::uint32_t address,
                                 std::uint32_t color, std::int16_t x, std::int16_t y,
                                 std::uint16_t z = 0u) {
    memory.store32(address + 0u, color);
    memory.store16(address + 4u, static_cast<std::uint16_t>(x));
    memory.store16(address + 6u, static_cast<std::uint16_t>(y));
    memory.store16(address + 8u, z);
}

std::array<std::uint32_t, 256> make_through_triangle_commands() {
    std::array<std::uint32_t, 256> commands{};
    commands[0x12] = ge_command(0x12u, (1u << 23u) | (7u << 2u) | (2u << 7u));
    commands[0x9C] = ge_command(0x9Cu, 0u);
    commands[0x9D] = ge_command(0x9Du, 8u);
    commands[0xD2] = ge_command(0xD2u, 3u);
    commands[0xD4] = ge_command(0xD4u, 0u);
    commands[0xD5] = ge_command(0xD5u, 7u | (7u << 10u));
    commands[0xE8] = ge_command(0xE8u, 0u);
    commands[0xE9] = ge_command(0xE9u, 0u);
    return commands;
}

void put_test_triangle(psprecomp::GuestMemory &memory, std::uint32_t vertices) {
    put_through_triangle_vertex(memory, vertices + 0u, 0xFF0000FFu, 1, 1);
    put_through_triangle_vertex(memory, vertices + 12u, 0xFF00FF00u, 6, 1);
    put_through_triangle_vertex(memory, vertices + 24u, 0xFFFF0000u, 1, 6);
}

void clear_test_framebuffer(psprecomp::GuestMemory &memory) {
    for (std::uint32_t pixel = 0u; pixel < 64u; ++pixel)
        memory.store32(0x04000000u + pixel * 4u, 0u);
}

void test_ge_cull_face_state() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t vertices = 0x08036000u;
    put_test_triangle(memory, vertices);
    auto commands = make_through_triangle_commands();
    commands[0x50] = ge_command(0x50u, 1u); // gouraud, isolate culling
    commands[0x1D] = ge_command(0x1Du, 1u);
    commands[0x9B] = ge_command(0x9Bu, 1u); // accept counter-clockwise

    vcs::GeTransformState transform{};
    vcs::reset_ge_transform_state(transform);
    vcs::GeRenderStats stats{};
    std::string error;
    require(vcs::render_ge_primitive(memory, commands, transform, vertices, 0u, 0x00030003u, stats, error),
            error.empty() ? "GE CCW culling test failed" : error.c_str());
    require(stats.pixels_written > 0u && stats.culled_triangles == 0u,
            "GE counter-clockwise cull mode rejected the selected winding");

    clear_test_framebuffer(memory);
    commands[0x9B] = ge_command(0x9Bu, 0u); // accept clockwise
    stats = {};
    require(vcs::render_ge_primitive(memory, commands, transform, vertices, 0u, 0x00030003u, stats, error),
            "GE clockwise culling test failed");
    require(stats.pixels_written == 0u && stats.culled_triangles == 1u,
            "GE clockwise cull mode failed to reject the opposing winding");
}

void test_ge_flat_shading_provoking_vertex() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t vertices = 0x08037000u;
    put_test_triangle(memory, vertices);
    auto commands = make_through_triangle_commands();
    commands[0x50] = ge_command(0x50u, 0u); // flat

    vcs::GeTransformState transform{};
    vcs::reset_ge_transform_state(transform);
    vcs::GeRenderStats stats{};
    std::string error;
    require(vcs::render_ge_primitive(memory, commands, transform, vertices, 0u, 0x00030003u, stats, error),
            error.empty() ? "GE flat-shaded triangle failed" : error.c_str());
    const std::uint32_t flat = memory.load32(0x04000000u + (2u * 8u + 2u) * 4u);
    require(flat == 0xFFFF0000u,
            "GE flat shading did not use the third/provoking vertex color");
    require(stats.flat_shaded_primitives == 1u, "GE flat-shaded primitive was not recorded");

    clear_test_framebuffer(memory);
    commands[0x50] = ge_command(0x50u, 1u); // gouraud
    stats = {};
    require(vcs::render_ge_primitive(memory, commands, transform, vertices, 0u, 0x00030003u, stats, error),
            "GE Gouraud triangle failed");
    const std::uint32_t gouraud = memory.load32(0x04000000u + (2u * 8u + 2u) * 4u);
    require(gouraud != 0u && gouraud != 0xFFFF0000u,
            "GE Gouraud mode incorrectly retained the provoking vertex color");
}


void clear_test_framebuffer(psprecomp::GuestMemory &memory, std::uint32_t width, std::uint32_t height,
                            std::uint32_t stride = 8u) {
    for (std::uint32_t y = 0u; y < height; ++y)
        for (std::uint32_t x = 0u; x < width; ++x)
            memory.store32(0x04000000u + (y * stride + x) * 4u, 0u);
}

void configure_4x4_sprite(std::array<std::uint32_t, 256> &commands,
                          std::uint32_t texture, std::uint32_t format) {
    commands = make_sprite_commands(texture);
    commands[0x9D] = ge_command(0x9Du, 8u);
    commands[0xA8] = ge_command(0xA8u, ((texture >> 8u) & 0x000F0000u) | 4u);
    commands[0xB8] = ge_command(0xB8u, 0x0202u); // 4x4
    commands[0xC3] = ge_command(0xC3u, format);
    commands[0xD5] = ge_command(0xD5u, 3u | (3u << 10u));
}

void write_psp_dxt_color_block(psprecomp::GuestMemory &memory, std::uint32_t address,
                               std::uint16_t c1, std::uint16_t c2) {
    // PSP order: four selector rows first, then the two 565 endpoints.
    for (std::uint32_t i = 0u; i < 4u; ++i) memory.store8(address + i, 0u);
    memory.store16(address + 4u, c1);
    memory.store16(address + 6u, c2);
}

void test_ge_psp_dxt_formats() {
    constexpr std::uint32_t vertices = 0x08026000u;
    constexpr std::uint32_t texture = 0x08027000u;
    const auto render = [&](std::uint32_t format) {
        psprecomp::GuestMemory memory;
        put_sprite_vertex(memory, vertices + 0u, 0u, 0u, 0, 0, 0u);
        put_sprite_vertex(memory, vertices + 10u, 4u, 4u, 4, 4, 0u);
        write_psp_dxt_color_block(memory, texture, 0xF800u, 0x001Fu); // selector 0 = red
        if (format == 9u) {
            for (std::uint32_t row = 0u; row < 4u; ++row)
                memory.store16(texture + 8u + row * 2u, 0xFFFFu);
        } else if (format == 10u) {
            memory.store32(texture + 8u, 0u);
            memory.store16(texture + 12u, 0u);
            memory.store8(texture + 14u, 200u);
            memory.store8(texture + 15u, 10u);
        }
        clear_test_framebuffer(memory, 4u, 4u);
        std::array<std::uint32_t,256> commands{};
        configure_4x4_sprite(commands, texture, format);
        vcs::GeTransformState transform{}; vcs::reset_ge_transform_state(transform);
        vcs::GeRenderStats stats{}; std::string error;
        require(vcs::render_ge_primitive(memory, commands, transform, vertices, 0u,
                                         0x00060002u, stats, error),
                error.empty() ? "GE PSP DXT sprite failed" : error.c_str());
        return memory.load32(0x04000000u);
    };
    require(render(8u) == 0xFF0000F8u, "PSP DXT1 reversed block decode mismatch");
    require(render(9u) == 0xFF0000F8u, "PSP DXT3 alpha/color decode mismatch");
    require(render(10u) == 0xC80000F8u, "PSP DXT5 alpha palette decode mismatch");
}

void test_ge_fixed_mip_level_selection() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t level0 = 0x08028000u;
    constexpr std::uint32_t level1 = 0x08029000u;
    constexpr std::uint32_t vertices = 0x0802A000u;
    for (std::uint32_t i = 0u; i < 16u; ++i) memory.store32(level0 + i * 4u, 0xFF0000FFu);
    for (std::uint32_t i = 0u; i < 4u; ++i) memory.store32(level1 + i * 4u, 0xFF00FF00u);
    put_sprite_vertex(memory, vertices + 0u, 0u, 0u, 0, 0, 0u);
    put_sprite_vertex(memory, vertices + 10u, 2u, 2u, 2, 2, 0u);
    clear_test_framebuffer(memory, 2u, 2u);
    std::array<std::uint32_t,256> commands{};
    configure_4x4_sprite(commands, level0, 3u);
    commands[0xA1] = ge_command(0xA1u, level1 & 0x00FFFFF0u);
    commands[0xA9] = ge_command(0xA9u, ((level1 >> 8u) & 0x000F0000u) | 2u);
    commands[0xB9] = ge_command(0xB9u, 0x0101u);
    commands[0xC2] = ge_command(0xC2u, 1u << 16u); // max mip level 1
    commands[0xC6] = ge_command(0xC6u, 4u);         // mipmapping enabled, nearest
    commands[0xC8] = ge_command(0xC8u, 1u | (16u << 16u)); // constant LOD = 1.0
    vcs::GeTransformState transform{}; vcs::reset_ge_transform_state(transform);
    vcs::GeRenderStats stats{}; std::string error;
    require(vcs::render_ge_primitive(memory, commands, transform, vertices, 0u,
                                     0x00060002u, stats, error),
            error.empty() ? "GE fixed mip selection failed" : error.c_str());
    require(memory.load32(0x04000000u) == 0xFF00FF00u,
            "constant PSP LOD did not select mip level 1");
}

void test_framebuffer_formats() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t address = 0x04000000u;
    const vcs::FramebufferDescription base{address, 1u, 1u, 1u, 0u};

    memory.store16(address, 0x001Fu);
    require_rgb(vcs::decode_framebuffer_rgb(memory, base), 255u, 0u, 0u, "RGB565 red conversion failed");
    memory.store16(address, 0x07E0u);
    require_rgb(vcs::decode_framebuffer_rgb(memory, base), 0u, 255u, 0u, "RGB565 green conversion failed");
    memory.store16(address, 0xF800u);
    require_rgb(vcs::decode_framebuffer_rgb(memory, base), 0u, 0u, 255u, "RGB565 blue conversion failed");

    auto format = base;
    format.pixel_format = 1u;
    memory.store16(address, 0x001Fu);
    require_rgb(vcs::decode_framebuffer_rgb(memory, format), 255u, 0u, 0u, "RGBA5551 conversion failed");

    format.pixel_format = 2u;
    memory.store16(address, 0x0F00u);
    require_rgb(vcs::decode_framebuffer_rgb(memory, format), 0u, 0u, 255u, "RGBA4444 conversion failed");

    format.pixel_format = 3u;
    memory.store32(address, 0x7F563412u);
    require_rgb(vcs::decode_framebuffer_rgb(memory, format), 0x12u, 0x34u, 0x56u, "RGBA8888 conversion failed");

    bool rejected = false;
    try {
        auto invalid = format;
        invalid.stride = 0u;
        (void)vcs::decode_framebuffer_rgb(memory, invalid);
    } catch (...) {
        rejected = true;
    }
    require(rejected, "invalid framebuffer stride was accepted");

    const auto path = std::filesystem::temp_directory_path() / "psprecomp_framebuffer_test.ppm";
    vcs::write_framebuffer_ppm(path, format, vcs::decode_framebuffer_rgb(memory, format));
    require(std::filesystem::file_size(path) > 3u, "PPM frame dump was not written");
    std::filesystem::remove(path);
}
} // namespace

int main() {
    try {
        for(const char *direction:{"0,-1","0.99267,-0.00756,-0.12062","0,0,1","0,0,-1","nan,0,0","0,0,0"}) {
            const auto b=vcs::detail::test_camera_basis(direction);
            auto dot=[](const auto &a,const auto &b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
            require(std::abs(dot(b.forward,b.forward)-1)<1e-5f && std::abs(dot(b.right,b.right)-1)<1e-5f && std::abs(dot(b.up,b.up)-1)<1e-5f,"replay camera basis is normalized");
            require(std::abs(dot(b.forward,b.up))<1e-5f && std::abs(dot(b.forward,b.right))<1e-5f && std::abs(dot(b.right,b.up))<1e-5f,"pitched and vertical replay cameras are orthogonal");
        }
        require(vcs::detail::test_camera_basis("0.99267,-0.00756,-0.12062").forward[2]<-.12f,"replay retains captured downward pitch");
        test_texture_hash_validation();
#if !defined(_WIN32)
        test_ge_indexed_packed_frontend();
        test_ge_raw_model_frontend();
#endif
        test_framebuffer_formats();
        test_ge_matrix_persistence();
        test_ge_matrix_reserved_cursor_ranges();
        test_ge_matrix_stream();
        test_ge_bounding_box_visibility_and_streams();
        test_ge_depth_clip_enable_state();
        test_ge_unsigned_raster_offset();
        test_ge_sprite_renderer();
        test_ge_scissor_and_blend();
        test_ge_psp_dxt_formats();
        test_ge_fixed_mip_level_selection();
        test_ge_non_through_triangle();
        test_ge_two_bone_skinning();
        test_ge_morph_targets();
        test_ge_directional_lighting();
        test_ge_texture_matrix_uv_generation();
        test_ge_environment_map_uv_generation();
        test_ge_cull_face_state();
        test_ge_flat_shading_provoking_vertex();
        std::string error;
        require(vcs::run_profile_self_tests(error), error.empty() ? "VCS profile self-test failed" : error.c_str());
        test_savedata_autoload_filesystem();
        std::cout << "All VCS scheduler/callback/framebuffer tests passed.\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << "VCS profile test failure: " << exception.what() << "\n";
        return 1;
    }
}
