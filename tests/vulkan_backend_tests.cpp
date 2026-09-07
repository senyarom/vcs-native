#include "ge_gpu_backend.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace vcs;
namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
GeGpuDrawDescriptor draw(std::uint32_t address) {
    GeGpuDrawDescriptor d;
    d.framebuffer_address = address;
    d.framebuffer_stride = 512;
    d.framebuffer_format = 3;
    d.vertex_count = 6;
    d.primitive = 3;
    d.through = true;
    return d;
}
void quad(GeGpuDrawDescriptor d, std::uint32_t color, float depth = 0) {
    std::array<GeGpuVertex, 6> v;
    constexpr std::array<std::array<float, 2>, 6> xy{
        {{0, 0}, {480, 0}, {0, 272}, {480, 0}, {480, 272}, {0, 272}}};
    for (unsigned i = 0; i < 6; ++i) {
        v[i].x = xy[i][0];
        v[i].y = xy[i][1];
        v[i].z = depth;
        v[i].rgba = color;
        // PSP through-mode UV coordinates are expressed in texels.
        v[i].u = xy[i][0] / 480 * d.texture_width;
        v[i].v = xy[i][1] / 272 * d.texture_height;
    }
    ge_gpu_backend_record_draw(d);
    ge_gpu_backend_accumulate_color_triangles(d, v);
}
void pixel(unsigned x, unsigned y, int red, int green, int blue, int tolerance = 2) {
    auto image = ge_gpu_backend_game_frame_rgba();
    require(image.size() == 64 * 64 * 4, "readback dimensions");
    auto offset = (y * 64 + x) * 4;
    if (std::abs(int(image[offset]) - red) > tolerance ||
        std::abs(int(image[offset + 1]) - green) > tolerance ||
        std::abs(int(image[offset + 2]) - blue) > tolerance) {
        std::cerr << "pixel " << x << ',' << y << ": " << int(image[offset]) << ','
                  << int(image[offset + 1]) << ',' << int(image[offset + 2]) << " expected " << red
                  << ',' << green << ',' << blue << '\n';
        throw std::runtime_error("GPU pixel mismatch");
    }
}
void finish() { require(ge_gpu_backend_finish_color_frame(0), "GPU frame produced"); }
GeGpuDrawDescriptor texture_draw(unsigned id, unsigned size = 1024) {
    auto d = draw(0);
    d.texture_enabled = true;
    d.texture_address = 0x08800000u + id * 0x1000u;
    d.texture_width = d.texture_height = d.texture_buffer_width = size;
    d.texture_format = 3;
    d.texture_function = 3;
    d.texture_use_alpha = true;
    d.texture_content_signature = id + 1;
    return d;
}
void upload_solid(const GeGpuDrawDescriptor &d, unsigned red, unsigned blue) {
    std::vector<std::byte> data(d.texture_width * d.texture_height * 4);
    for (std::size_t i = 0; i < data.size(); i += 4) {
        data[i] = std::byte(red);
        data[i + 2] = std::byte(blue);
        data[i + 3] = std::byte{255};
    }
    require(ge_gpu_backend_upload_decoded_texture_chain_packed(
        d, d.texture_width, d.texture_height, 1, std::move(data)), "solid texture accepted");
}
// Exercise the frontend's hash-once-per-interval contract, including pending
// uploads. The missing signature on a repeated draw must never invalidate a
// valid entry; a real content or palette change must still refresh the image.
void texture_validation_test(bool asynchronous) {
    VulkanBackendConfiguration config;
    config.width = config.height = 64;
    config.async_readback = asynchronous;
    std::string error;
    require(initialize_vulkan_backend(config, error), error.c_str());
    ge_gpu_backend_set_display_framebuffer(0);
    auto complete = [&] {
        if (asynchronous) (void)ge_gpu_backend_finish_color_frame(0);
        finish();
    };
    auto d = texture_draw(30, 4);
    require(ge_gpu_backend_texture_signature_needed(d), "first draw needs hash");
    require(ge_gpu_backend_texture_needed(d), "first draw needs decode");
    upload_solid(d, 255, 0);
    for (unsigned frame = 0; frame < 4; ++frame) {
        if (frame) {
            require(ge_gpu_backend_texture_signature_needed(d), "new interval revalidates content");
            require(!ge_gpu_backend_texture_needed(d), "unchanged real signature reuses image");
        }
        for (unsigned repeat = 0; repeat < 8; ++repeat) {
            auto repeated = d;
            repeated.texture_content_signature = 0;
            require(!ge_gpu_backend_texture_signature_needed(repeated), "same interval needs no second hash");
            require(!ge_gpu_backend_texture_needed(repeated), "omitted hash must reuse cached pixels");
            quad(repeated, 0xffffffff);
        }
        complete();
        pixel(32, 32, 255, 0, 0);
    }
    // A sampler change may share an image, but must not mask changed content.
    auto sampler = d;
    sampler.texture_mag_linear = true;
    require(ge_gpu_backend_texture_signature_needed(sampler), "new sampler needs validation");
    require(ge_gpu_backend_texture_needed(sampler), "new sampler entry is missing");
    require(ge_gpu_backend_adopt_shared_texture(sampler), "same content can share image");
    auto repeated = sampler;
    repeated.texture_content_signature = 0;
    require(!ge_gpu_backend_texture_needed(repeated), "shared image honors omitted hash");
    ++d.texture_content_signature;
    require(ge_gpu_backend_texture_needed(d), "actual content change requires decode");
    require(!ge_gpu_backend_adopt_shared_texture(d), "changed content cannot reuse old image");
    upload_solid(d, 0, 255);
    quad(d, 0xffffffff);
    complete();
    pixel(32, 32, 0, 0, 255);
    ++d.clut_checksum;
    require(ge_gpu_backend_texture_needed(d), "changed palette invalidates cache key");
    shutdown_ge_gpu_backend();
}
void memory_pressure_test(bool asynchronous) {
    VulkanBackendConfiguration config;
    config.width = config.height = 64;
    config.async_readback = asynchronous;
    std::string error;
    require(initialize_vulkan_backend(config, error), error.c_str());
    ge_gpu_backend_set_display_framebuffer(0);
    std::uint64_t frame = 300;
    auto complete = [&] {
        const bool ready = ge_gpu_backend_finish_color_frame(frame++);
        if (asynchronous) require(ge_gpu_backend_finish_color_frame(frame++), "pressure frame drained");
        else require(ready, "pressure frame completed");
        require(ge_gpu_backend_active(), "memory pressure must not disable GPU");
    };
    // More than the entire 64 MiB staging arena in one frame, followed by
    // geometry/uniforms. All draws must survive the intermediate submissions.
    for (unsigned i = 0; i < 17; ++i) {
        auto d = texture_draw(i);
        upload_solid(d, i == 16 ? 255 : 0, i == 16 ? 0 : 255);
        quad(d, 0xffffffff);
    }
    complete();
    pixel(32, 32, 255, 0, 0);
    require(ge_gpu_backend_report().upload_batch_flushes > 0, "staging rollover exercised");
    require(ge_gpu_backend_report().transfer_bytes >= 17ull * 1024 * 1024 * 4,
            "all HD uploads retained");

    // Same dimensions and cache key, new content: an old image must never be
    // published under the new signature and incorrectly reused.
    auto d = texture_draw(16);
    ++d.texture_content_signature;
    upload_solid(d, 0, 255);
    quad(d, 0xffffffff);
    complete();
    pixel(32, 32, 0, 0, 255);

    // Two sampler variants decoded in the same frame share the uploaded image.
    d = texture_draw(20);
    upload_solid(d, 255, 0);
    quad(d, 0xffffffff);
    d.texture_mag_linear = true;
    upload_solid(d, 255, 0);
    quad(d, 0xffffffff);
    const auto before = ge_gpu_backend_report().transfer_bytes;
    complete();
    pixel(32, 32, 255, 0, 0);
    require(ge_gpu_backend_report().transfer_bytes - before == 4ull * 1024 * 1024,
            "sampler variants must upload just one image");
    shutdown_ge_gpu_backend();

    // The byte limit must apply even when the entry limit has ample space.
    config.texture_cache_mb = 8;
    require(initialize_vulkan_backend(config, error), error.c_str());
    ge_gpu_backend_set_display_framebuffer(0);
    std::uint64_t attachment_bytes_per_target = 0;
    for (unsigned i = 0; i < 24; ++i) {
        d = texture_draw(i);
        upload_solid(d, i % 2 ? 0 : 255, i % 2 ? 255 : 0);
        quad(d, 0xffffffff);
        // Scratch framebuffer addresses also change as the PSP reuses VRAM.
        quad(draw(0x10000 + i * 0x1000), 0xff00ff00u);
        complete();
        pixel(32, 32, i % 2 ? 0 : 255, 0, i % 2 ? 255 : 0);
        const auto report = ge_gpu_backend_report();
        if (i == 0) attachment_bytes_per_target = report.resident_target_bytes / report.resident_target_count;
        require(report.resident_texture_bytes <= 8ull * 1024 * 1024, "texture byte budget enforced");
        require(report.resident_target_count <= 10, "historical scratch targets retired");
        require(report.resident_target_bytes <= 10 * attachment_bytes_per_target,
                "scratch attachment memory bounded");
        if(i>=2) {
            const auto evicted=texture_draw(i-2);
            require(!ge_gpu_backend_texture_available(evicted) && ge_gpu_backend_texture_needed(evicted),
                    "lookup cache cannot retain an evicted texture pointer");
        }
    }
    require(ge_gpu_backend_report().evicted_textures >= 22, "byte budget eviction exercised");
    d=texture_draw(0);
    require(!ge_gpu_backend_texture_available(d),"old texture remains a cached miss");
    upload_solid(d,255,0);
    require(ge_gpu_backend_texture_available(d),"upload replaces a cached missing lookup");
    quad(d,0xffffffff);complete();pixel(32,32,255,0,0);
    shutdown_ge_gpu_backend();
}
void display_flip_test(bool asynchronous) {
    VulkanBackendConfiguration config;
    config.width = config.height = 64;
    config.async_readback = asynchronous;
    std::string error;
    require(initialize_vulkan_backend(config, error), error.c_str());
    constexpr std::uint32_t front = 0x40000, back = 0x80000, scratch = 0x140000;
    std::uint64_t frame = 200;
    auto complete = [&] {
        const bool ready = ge_gpu_backend_finish_color_frame(frame++);
        if (asynchronous)
            require(ge_gpu_backend_finish_color_frame(frame++), "flip frame drained");
        else
            require(ready, "flip frame completed");
    };
    // Leave recognizable old content in both buffers, like the legal splash
    // screen which leaked below the vertically compressed pause menu.
    for (const auto address : {front, back}) {
        ge_gpu_backend_set_display_framebuffer(address);
        quad(draw(address), 0xffff0000u);
        complete();
    }
    for (unsigned flip = 0; flip < 6; ++flip) {
        const auto destination = flip % 2 ? front : back;
        const auto previous = flip % 2 ? back : front;
        ge_gpu_backend_set_display_framebuffer(previous);
        // A framebuffer can be sampled through a padded 512x512 texture while
        // it is not selected for display. This must not resize the next menu.
        auto sample = draw(scratch);
        sample.texture_enabled = true;
        sample.texture_address = 0x04000000u | destination;
        sample.texture_width = sample.texture_height = 512;
        sample.texture_function = 3;
        quad(sample, 0xffffffffu);
        const auto color = flip % 2 ? 0xff00ff00u : 0xff0000ffu;
        quad(draw(destination), color);
        // sceDisplaySetFrameBuf selects the completed buffer after its GE
        // commands have been collected, rather than before drawing begins.
        ge_gpu_backend_set_display_framebuffer(destination);
        complete();
        for (auto [x,y] : std::array<std::pair<unsigned,unsigned>,3>{{{2,2},{61,32},{32,61}}})
            pixel(x, y, flip % 2 ? 0 : 255, flip % 2 ? 255 : 0, 0);
    }
    shutdown_ge_gpu_backend();
}
void geometry_test(bool asynchronous) {
    VulkanBackendConfiguration config;
    config.width = config.height = 64;
    config.async_readback = asynchronous;
    std::string error;
    require(initialize_vulkan_backend(config, error), error.c_str());
    ge_gpu_backend_set_display_framebuffer(0);
    std::uint64_t frame = 1000;
    auto complete = [&] {
        const auto ready = ge_gpu_backend_finish_color_frame(frame++);
        if (asynchronous) require(ge_gpu_backend_finish_color_frame(frame++), "geometry frame drained");
        else require(ready, "geometry frame completed");
        const auto pixels = ge_gpu_backend_game_frame_rgba();
        return std::vector<std::byte>(pixels.begin(), pixels.end());
    };
    auto d = draw(0);
    d.through = false;
    d.texture_enabled = true;
    d.texture_address = 0x08800000;
    d.texture_width = d.texture_height = d.texture_buffer_width = 4;
    d.texture_function = 0;
    d.texture_use_alpha = true;
    d.texture_content_signature = 1;
    d.alpha_test_enabled = true;
    d.alpha_function = 6;
    d.alpha_reference = 64;
    d.alpha_mask = 255;
    d.fog_enabled = true;
    d.fog_color = 0x805040;
    std::array<std::byte, 64> texture;
    for (unsigned i = 0; i < 16; ++i) {
        texture[4*i] = std::byte(i * 17);
        texture[4*i+1] = std::byte(255 - i * 11);
        texture[4*i+2] = std::byte(i % 2 ? 100 : 240);
        texture[4*i+3] = std::byte{255};
    }
    require(ge_gpu_backend_upload_decoded_texture(d, 4, 4, texture), "geometry checker texture");
    GeGpuHardwareTransform h;
    h.model_to_clip = {.9f, 0, 0, 0, 0, .9f, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    h.model_to_view_z = {0, 0, 1, 0};
    h.viewport_scale_x = h.viewport_center_x = 240;
    h.viewport_scale_y = h.viewport_center_y = 136;
    h.viewport_scale_z = 65535;
    h.depth_clip_enabled = true;
    h.uv_scale_u = .75f; h.uv_scale_v = .5f;
    h.uv_offset_u = .125f; h.uv_offset_v = -.25f;
    h.fog_end = .25f; h.fog_slope = .8f;
    h.vertex_color_mul = {.625f, 1.25f, .5f, 1};
    h.vertex_color_add = {.125f, -.125f, .25f, 0};
    constexpr std::array<std::array<std::int16_t, 3>, 6> xyz{{
        {-32768,-32768,8192}, {32767,-32768,16384}, {-32768,32767,24576},
        {32767,32767,16384}, {-8192,4096,8192}, {8192,-4096,24576}}};
    constexpr std::array<std::uint16_t,6> colors{0xffff,0x8000,0x9234,0xa5ce,0x7fff,0xcc73};
    std::vector<std::byte> packed(60);
    std::vector<GeGpuVertex> vertices(6);
    for (unsigned i = 0; i < 6; ++i) {
        auto put16 = [&](unsigned at, std::uint16_t n) {
            packed[10*i+at] = std::byte(n & 255);
            packed[10*i+at+1] = std::byte(n >> 8);
        };
        packed[10*i] = std::byte(i * 43);
        packed[10*i+1] = std::byte(255 - i * 47);
        put16(2, colors[i]);
        for (unsigned c = 0; c < 3; ++c) put16(4+2*c, std::uint16_t(xyz[i][c]));
        auto &v = vertices[i];
        v.x = xyz[i][0]/32768.0f; v.y = xyz[i][1]/32768.0f; v.z = xyz[i][2]/32768.0f;
        v.u = std::to_integer<unsigned>(packed[10*i])/128.0f;
        v.v = std::to_integer<unsigned>(packed[10*i+1])/128.0f;
        auto expand = [&](unsigned shift) { unsigned n = (colors[i] >> shift) & 31; return (n << 3) | (n >> 2); };
        v.rgba = expand(0) | (expand(5) << 8) | (expand(10) << 16) |
            (colors[i] & 0x8000 ? 0xff000000u : 0);
    }
    for (auto primitive : {3u, 4u, 5u}) for (bool indexed : {false, true})
    for (unsigned cull = 0; cull < 3; ++cull) for (bool affine : {false, true}) {
        h.primitive = primitive;
        h.cull_enabled = cull != 0;
        h.accept_counter_clockwise = cull == 2;
        h.vertex_color_affine = affine;
        std::vector<std::uint32_t> indices = indexed
            ? std::vector<std::uint32_t>{4,2,0,5,1,3,4,4,0} : std::vector<std::uint32_t>{};
        const auto count = indexed ? indices.size() : vertices.size();
        // Reference is the old explicit triangle list, independent of native
        // topology, index buffers, packed fetch and base-vertex addressing.
        std::vector<GeGpuVertex> expanded;
        auto append = [&](std::size_t i) { expanded.push_back(vertices[indexed ? indices[i] : i]); };
        if (primitive == 3) for (std::size_t i = 0; i < count; ++i) append(i);
        else for (std::size_t i = 2; i < count; ++i) {
            append(primitive == 5 ? 0 : (i % 2 ? i-1 : i-2));
            append(primitive == 5 ? i-1 : (i % 2 ? i-2 : i-1));
            append(i);
        }
        // Two draws exercise nonzero offsets and transitions between packed,
        // decoded and through-mode layouts in the same command buffer.
        auto render = [&](unsigned mode) {
            quad(draw(0), 0xff202020);
            const auto before = ge_gpu_backend_report();
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                auto transform = h;
                if (repeat == 0) transform.viewport_center_x -= 80;
                if (mode == 0) {
                    transform.primitive = 3;
                    ge_gpu_backend_accumulate_hardware_triangles(d, transform, expanded, {});
                } else if (mode == 1) {
                    ge_gpu_backend_accumulate_hardware_triangles(d, transform, vertices, indices);
                } else {
                    auto snapshot = packed;
                    require(ge_gpu_backend_accumulate_hardware_packed_0115(d, transform, snapshot, 6, indices),
                            "packed 0115 accepted");
                    std::fill(snapshot.begin(), snapshot.end(), std::byte{});
                }
            }
            const auto pixels = complete();
            const auto after = ge_gpu_backend_report();
            require(after.game_triangles - before.game_triangles == 2 * expanded.size()/3,
                    "native topology must preserve triangle count");
            if (mode) require(after.game_vertices - before.game_vertices == 12,
                              "indexed geometry must not duplicate vertices");
            return pixels;
        };
        const auto reference = render(0);
        require(render(1) == reference, "indexed/strip/fan pixels must match expanded triangles");
        require(render(2) == reference, "packed GPU decode must match CPU decode pixels");
    }
    require(!ge_gpu_backend_accumulate_hardware_packed_0115(d,h,std::span(packed).first(59),6,{}),
            "truncated packed input rejected before reading");
    quad(draw(0),0xffff0000);
    h.primitive = 5;
    ge_gpu_backend_accumulate_hardware_triangles(d,h,std::span(vertices).first(2),{});
    require(ge_gpu_backend_accumulate_hardware_packed_0115(d,h,std::span(packed).first(20),2,{}),
            "short fan is a valid empty primitive");
    complete();
    pixel(32,32,0,0,255);
    shutdown_ge_gpu_backend();
    std::cout << "Indexed geometry, strip winding, fans, packed 0115, lighting and texture parity passed\n";
}
// Compare complete images across state changes, target switches and command
// buffer resets. The reference deliberately emits all bindings for every draw.
void command_state_test(bool asynchronous) {
    auto render = [&](bool cache) {
        setenv("PSPRECOMP_VULKAN_STATE_CACHE", cache ? "1" : "0", 1);
        VulkanBackendConfiguration config;
        config.width = config.height = 64;
        config.async_readback = asynchronous;
        std::string error;
        require(initialize_vulkan_backend(config, error), error.c_str());
        ge_gpu_backend_set_display_framebuffer(0);
        auto texture = texture_draw(20, 4);
        upload_solid(texture, 190, 70);
        std::vector<std::byte> images;
        for (unsigned frame = 0; frame < 3; ++frame) {
            quad(draw(0), 0xff203040);
            // Identical state with different vertex data must reuse uniforms.
            for (unsigned i = 0; i < 16; ++i) {
                auto d = i % 3 ? texture : draw(0);
                d.scissor_x0 = (i % 4) * 110;
                d.scissor_x1 = d.scissor_x0 + 109;
                d.scissor_y0 = (i / 4) * 60;
                d.scissor_y1 = d.scissor_y0 + 59;
                d.blend_enabled = true;
                d.blend_source_factor = 2;
                d.blend_dest_factor = 3;
                for (unsigned repeat = 0; repeat < 3; ++repeat)
                    quad(d, 0x40302010u + frame * 0x00070707u);
            }
            // Feedback ends/restarts a render pass; bindings must remain valid.
            quad(draw(0x80000), 0xff502010);
            auto feedback = texture_draw(21, 512);
            feedback.texture_address = 0x80000;
            feedback.scissor_x0 = 180; feedback.scissor_x1 = 280;
            feedback.scissor_y0 = 100; feedback.scissor_y1 = 150;
            quad(feedback, 0xffffffff);
            if (asynchronous) (void)ge_gpu_backend_finish_color_frame(frame);
            finish();
            auto rgba = ge_gpu_backend_game_frame_rgba();
            images.insert(images.end(), rgba.begin(), rgba.end());
        }
        const auto report = ge_gpu_backend_report();
        require(cache ? report.command_state_reused > 100 : report.command_state_reused == 0,
                "state cache must avoid API calls only when enabled");
        require(cache ? report.uniform_uploads < report.game_draw_calls :
                        report.uniform_uploads == report.game_draw_calls,
                "uniform cache must preserve data while reducing repeated uploads");
        shutdown_ge_gpu_backend();
        return images;
    };
    const auto reference = render(false);
    require(render(true) == reference, "command-state reuse must preserve every rendered pixel");
    unsetenv("PSPRECOMP_VULKAN_STATE_CACHE");
    std::cout << "Command-state/uniform reuse matches uncached output across frame/target changes\n";
}

void warm_texture_cache_test(bool asynchronous) {
    VulkanBackendConfiguration config;
    config.width = config.height = 64;
    config.texture_cache_mb = 64;
    config.async_readback = asynchronous;
    std::string error;
    require(initialize_vulkan_backend(config, error), error.c_str());
    ge_gpu_backend_set_display_framebuffer(0);
    for (unsigned i = 0; i < 400; ++i) upload_solid(texture_draw(i, 4), 60, 150);
    auto visible = texture_draw(0, 4);
    auto complete = [&] {
        if (asynchronous) (void)ge_gpu_backend_finish_color_frame(0);
        finish();
        pixel(32, 32, 60, 0, 150);
    };
    quad(visible, 0xffffffff);
    complete();
    const auto initial = ge_gpu_backend_report();
    require(initial.pending_texture_entries_checked == 400, "all queued uploads processed");
    for (unsigned frame = 0; frame < 8; ++frame) {
        for (unsigned draw_index = 0; draw_index < 20; ++draw_index)
            quad(visible, 0xffffffff);
        complete();
    }
    const auto warm = ge_gpu_backend_report();
    require(warm.texture_eviction_entries_scanned == initial.texture_eviction_entries_scanned,
            "warm cache under budget must not scan historical entries for eviction");
    require(warm.pending_texture_entries_checked == initial.pending_texture_entries_checked,
            "unchanged cache must not revisit historical uploads");
    require(warm.resident_texture_bytes == initial.resident_texture_bytes,
            "incremental shared-image byte accounting must remain stable");
    require(warm.inflight_image_references - initial.inflight_image_references == (asynchronous ? 8 : 0),
            "retain only one used image per frame, not the whole texture cache");
    // A second sampler entry shares storage and must not double the byte budget.
    auto alias = visible;
    alias.texture_min_linear = true;
    require(ge_gpu_backend_adopt_shared_texture(alias), "warm sampler alias adopts uploaded image");
    quad(alias, 0xffffffff);
    complete();
    require(ge_gpu_backend_report().resident_texture_bytes == initial.resident_texture_bytes,
            "sampler alias must not count shared memory twice");
    // Replacing a just-uploaded but undrawn texture must also respect the fence.
    auto unused = texture_draw(200, 8);
    upload_solid(unused, 180, 20);
    quad(visible, 0xffffffff);
    (void)ge_gpu_backend_finish_color_frame(0);
    unused.texture_width = unused.texture_height = unused.texture_buffer_width = 16;
    ++unused.texture_content_signature;
    upload_solid(unused, 20, 180);
    quad(visible, 0xffffffff);
    complete();
    shutdown_ge_gpu_backend();
    std::cout << "Warm cache avoids full scans; aliases and pending uploads preserve image lifetime\n";
}
} // namespace
int main() {
    try {
        warm_texture_cache_test(false);
        warm_texture_cache_test(true);
        command_state_test(false);
        command_state_test(true);
        geometry_test(false);
        geometry_test(true);
        VulkanBackendConfiguration config;
        config.width = config.height = 64;
        std::string error;
        require(initialize_vulkan_backend(config, error), error.c_str());
        require(ge_gpu_backend_report().active == GeGpuBackendKind::Vulkan, "Vulkan active");
        ge_gpu_backend_set_display_framebuffer(0);
        auto d = draw(0);
        quad(d, 0xff0000ffu);
        auto transparent = d;
        transparent.alpha_test_enabled = true;
        transparent.alpha_function = 6;
        transparent.alpha_reference = 128;
        transparent.alpha_mask = 255;
        quad(transparent, 0x0000ff00u);
        finish();
        pixel(32, 32, 255, 0, 0);

        ge_gpu_backend_set_display_framebuffer(0x40000);
        d = draw(0x40000);
        d.depth_test_enabled = d.depth_write_enabled = true;
        d.depth_function = 6;
        quad(d, 0xff0000ffu, 45000);
        quad(d, 0xff00ff00u, 10000);
        finish();
        pixel(32, 32, 255, 0, 0);

        ge_gpu_backend_set_display_framebuffer(0x80000);
        d = draw(0x80000);
        quad(d, 0xff0000ffu);
        d.blend_enabled = true;
        d.blend_source_factor = 2;
        d.blend_dest_factor = 3;
        quad(d, 0x8000ff00u);
        finish();
        pixel(32, 32, 127, 128, 0);

        ge_gpu_backend_set_display_framebuffer(0xc0000);
        d = draw(0xc0000);
        d.texture_enabled = true;
        d.texture_address = 0x089f0000;
        d.texture_width = d.texture_height = d.texture_buffer_width = 1;
        d.texture_format = 3;
        d.texture_function = 3;
        d.texture_use_alpha = true;
        d.texture_content_signature = 1;
        constexpr std::array<std::byte, 4> blue{std::byte{0}, std::byte{0}, std::byte{255},
                                                std::byte{255}};
        require(ge_gpu_backend_upload_decoded_texture(d, 1, 1, blue), "texture upload accepted");
        quad(d, 0xffffffff);
        finish();
        pixel(32, 32, 0, 0, 255);

        // The PSP uses nonzero LOD bias; MoltenVK cannot put it in VkSampler.
        // At one texel per pixel, +1 bias must choose the green 32x32 mip.
        d.texture_width = d.texture_height = d.texture_buffer_width = 64;
        d.texture_mipmap_enabled = true;
        d.texture_max_level = 2;
        d.texture_level_offset16 = 16;
        std::vector<std::byte> mips;
        for (unsigned level = 0; level < 3; ++level) {
            const unsigned size = 64u >> level;
            for (unsigned i = 0; i < size * size; ++i)
                for (unsigned channel = 0; channel < 4; ++channel)
                    mips.push_back(std::byte{static_cast<unsigned char>(
                        channel == level || channel == 3 ? 255 : 0)});
        }
        require(ge_gpu_backend_upload_decoded_texture_chain_packed(d, 64, 64, 3, std::move(mips)),
                "mip chain upload accepted");
        quad(d, 0xffffffff);
        finish();
        pixel(32, 32, 0, 255, 0);

        // GPU-to-GPU feedback must see an earlier pass from the same frame.
        ge_gpu_backend_set_display_framebuffer(0x100000);
        quad(draw(0x140000), 0xff0000ffu);
        d = draw(0x100000);
        d.texture_enabled = true;
        d.texture_address = 0x04140000;
        d.texture_width = d.texture_height = 1;
        d.texture_function = 3;
        d.texture_use_alpha = true;
        quad(d, 0xffffffff);
        finish();
        pixel(32, 32, 255, 0, 0);
        // Sampling the current target requires a GPU snapshot, not an attachment hazard.
        d.texture_address = 0x04100000;
        d.texture_function = 0;
        quad(d, 0xff808080);
        finish();
        pixel(32, 32, 128, 0, 0);

        ge_gpu_backend_set_display_framebuffer(0x180000);
        d = draw(0x180000);
        d.through = false;
        d.vertex_count = 3;
        GeGpuHardwareTransform h;
        h.model_to_clip = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        h.viewport_scale_x = h.viewport_center_x = 240;
        h.viewport_scale_y = h.viewport_center_y = 136;
        h.viewport_scale_z = 65535;
        h.depth_clip_enabled = true;
        std::array<GeGpuVertex, 3> vertices;
        vertices[0].x = -1;
        vertices[0].y = -1;
        vertices[1].x = 1;
        vertices[1].y = -1;
        vertices[2].x = -1;
        vertices[2].y = 1;
        for (auto &v : vertices) {
            v.z = .5f;
            v.rgba = 0xffff00ff;
        }
        ge_gpu_backend_record_draw(d);
        ge_gpu_backend_accumulate_hardware_triangles(d, h, vertices, {});
        finish();
        pixel(8, 8, 255, 0, 255);
        pixel(56, 56, 0, 0, 0);
        auto report = ge_gpu_backend_report();
        require(report.hw_transform_draw_calls > 0, "GPU transform exercised");
        require(report.vram_feedback_refreshes >= 2, "GPU feedback exercised");
        std::cout << "Vulkan texture, mip LOD bias, alpha test, depth, blend, framebuffer feedback, "
                     "self-feedback and hardware transform passed on "
                  << report.message << '\n';
        shutdown_ge_gpu_backend();
        config.async_readback = true;
        config.anisotropic_filtering = 16;
        require(initialize_vulkan_backend(config,error),error.c_str());
        ge_gpu_backend_set_display_framebuffer(0);
        d = draw(0);
        d.texture_enabled = true;
        // Main RAM must not alias framebuffer zero despite identical low bits.
        d.texture_address = 0x08800000;
        d.texture_width = d.texture_height = 1;
        d.texture_function = 3;
        d.texture_use_alpha = true;
        std::vector<std::byte> red{std::byte{255},std::byte{0},std::byte{0},std::byte{255}};
        require(ge_gpu_backend_upload_decoded_texture_chain_packed(d,1,1,1,red),"async texture upload");
        quad(d,0xffffffff);
        require(!ge_gpu_backend_finish_color_frame(100),"first asynchronous frame must remain pending");
        // Replacing an in-flight texture must retain the old native image until
        // the fence signals; validation catches premature image/descriptor frees.
        d.texture_content_signature = 2;
        std::vector<std::byte> async_blue(2*2*4);
        for (unsigned i=0;i<4;++i) {async_blue[i*4+2]=std::byte{255};async_blue[i*4+3]=std::byte{255};}
        require(ge_gpu_backend_upload_decoded_texture_chain_packed(d,2,2,1,std::move(async_blue)),"async resized texture");
        quad(d,0xffffffff);
        require(ge_gpu_backend_finish_color_frame(101),"previous asynchronous frame completed");
        pixel(32,32,255,0,0);
        require(ge_gpu_backend_report().game_frame_vblank==100,"readback must retain its original frame index");
        require(ge_gpu_backend_finish_color_frame(102),"pending frame drains without new draws");
        pixel(32,32,0,0,255);
        require(ge_gpu_backend_report().game_frame_vblank==101,"second readback frame index");
        shutdown_ge_gpu_backend();
        std::cout << "Asynchronous frame completion and in-flight texture lifetime passed\n";
        display_flip_test(false);
        display_flip_test(true);
        std::cout << "Padded framebuffer textures and alternating menu buffers passed\n";
        texture_validation_test(false);
        texture_validation_test(true);
        std::cout << "Texture validation, repeated draws and content invalidation passed\n";
        memory_pressure_test(false);
        memory_pressure_test(true);
        std::cout << "HD upload rollover, content replacement, shared images and cache budgets passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        shutdown_ge_gpu_backend();
        return 1;
    }
}
