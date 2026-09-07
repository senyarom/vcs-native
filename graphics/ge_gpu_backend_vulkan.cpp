// Vulkan PSP GE backend for Linux and macOS/MoltenVK.
// The shaders and GE state translation follow this project's DX12 backend.
// GPU render targets persist across frames, including framebuffer feedback.
// The final RGBA frame is read back once for the existing SDL presentation host.
#include "ge_gpu_backend.hpp"
#include "vcs_config.hpp"
#include <vulkan/vulkan.h>

// glslang's generated headers use uint32_t without including its definition.
#include <stdint.h>
#include "ge_frag_spv.hpp"
#include "ge_vert_spv.hpp"
#include "ge_packed_vert_spv.hpp"
#include "ge_model_vert_spv.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vcs {
namespace {
constexpr VkFormat kColor = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepth = VK_FORMAT_D32_SFLOAT;
constexpr VkDeviceSize kArenaBytes = 64u * 1024u * 1024u;
constexpr std::uint32_t address_mask = 0x001FFFF0u;
void check(VkResult r, const char *operation) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(r));
}
std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    return h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
}
std::uint64_t texture_key(const GeGpuDrawDescriptor &d) {
    if (d.texture_cache_key_hint)
        return d.texture_cache_key_hint;
    std::uint64_t h = 0xcbf29ce484222325ull;
    auto add = [&](std::uint64_t n) { h = mix(h, n); };
    add(d.texture_address);
    add(d.texture_format);
    add(d.texture_buffer_width);
    add(d.texture_width);
    add(d.texture_height);
    add(d.texture_swizzled);
    add(d.clut_address);
    add(d.clut_format);
    add(d.clut_shift);
    add(d.clut_mask);
    add(d.clut_start);
    add(d.clut_checksum);
    add(d.texture_mipmap_enabled);
    add(d.texture_max_level);
    for (unsigned i = 0; i < 8; ++i) {
        add(d.texture_level_addresses[i]);
        add(d.texture_level_buffer_widths[i]);
        add(d.texture_level_widths[i]);
        add(d.texture_level_heights[i]);
    }
    add(d.texture_min_linear);
    add(d.texture_mag_linear);
    add(d.texture_mipmap_linear);
    add(d.texture_level_mode);
    add(std::uint32_t(d.texture_level_offset16));
    add(d.texture_selected_level);
    add(d.texture_clamp_u);
    add(d.texture_clamp_v);
    return h ? h : 1;
}
std::uint64_t image_key(GeGpuDrawDescriptor d) {
    d.texture_cache_key_hint = 0;
    d.texture_image_key_hint = 0;
    d.texture_min_linear = d.texture_mag_linear = d.texture_mipmap_linear = false;
    d.texture_level_mode = 0;
    d.texture_level_offset16 = 0;
    d.texture_selected_level = 0;
    d.texture_clamp_u = d.texture_clamp_v = false;
    return texture_key(d);
}
struct Image {
    VkDevice device{};
    VkDescriptorPool pool{};
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    std::uint32_t width{}, height{}, levels{1};
    bool depth{};
    VkDeviceSize allocated_bytes{};
    std::uint32_t cache_references{};
    std::uint64_t retained_epoch{};
    std::unordered_map<std::uint64_t, VkDescriptorSet> descriptors;
    ~Image() {
        for (auto [key, set] : descriptors)
            vkFreeDescriptorSets(device, pool, 1, &set);
        if (view)
            vkDestroyImageView(device, view, nullptr);
        if (image)
            vkDestroyImage(device, image, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
    }
};
struct Target {
    VkDevice device{};
    VkFramebuffer framebuffer{};
    std::shared_ptr<Image> color, depth, snapshot;
    std::uint32_t logical_width{480}, logical_height{272};
    std::uint32_t address{};
    std::uint64_t epoch{};
    std::uint64_t used_epoch{};
    bool rendered{};
    ~Target() {
        if (framebuffer)
            vkDestroyFramebuffer(device, framebuffer, nullptr);
    }
};
struct Texture {
    std::shared_ptr<Image> image;
    std::vector<std::byte> pending;
    std::uint32_t width{}, height{}, levels{1};
    std::uint64_t signature{}, checked_epoch{}, used_epoch{};
    std::uint64_t share_key{};
    bool pending_queued{};
};
struct TextureLookup {std::uint64_t key{};Texture *texture{};};
struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    std::byte *mapped{};
    VkDeviceSize size{};
};
struct Batch {
    GeGpuDrawDescriptor draw;
    GeGpuHardwareTransform transform{};
    std::uint32_t first{}, count{};
    std::uint32_t first_index{};
    VkPrimitiveTopology topology{VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    bool hardware{}, indexed{}, packed{}, raw{};
    std::uint32_t model_index{};
};
struct alignas(16) Uniform {
    std::array<float, 4> row0{}, row1{}, row2{}, row3{}, view_z{}, uv{}, fog{};
    std::array<std::uint32_t, 4> mode{};
    std::array<float, 4> color_mul{1, 1, 1, 1}, color_add{};
    std::array<std::uint32_t, 4> pixel{}, format{};
};
static_assert(sizeof(Uniform) == 192);
// Vulkan bindings persist across compatible render passes, but never across a
// command-pool reset. Keep this cache with the command buffer, not a framebuffer.
struct CommandState {
    VkPipeline pipeline{};
    VkViewport viewport{};
    VkRect2D scissor{};
    std::array<float, 4> blend{};
    VkDescriptorSet texture{};
    VkDeviceSize vertex_offset{};
    Uniform uniform{};
    std::uint32_t uniform_offset{};
    bool have_viewport{}, have_scissor{}, have_blend{}, have_vertex{}, have_uniform{};
};

struct State {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkCommandPool commands{};
    VkCommandBuffer command{};
    VkFence fence{};
    VkRenderPass render_pass{};
    VkDescriptorPool descriptors{};
    VkDescriptorSetLayout texture_layout{}, uniform_layout{}, model_layout{};
    VkPipelineLayout pipeline_layout{};
    VkDescriptorSet uniform_set{}, model_set{};
    VkShaderModule vertex_shader{}, packed_vertex_shader{}, model_vertex_shader{}, fragment_shader{};
    VkDeviceSize uniform_alignment{256}, cursor{};
    Buffer arena, readback, raw_buffer;
    VkDeviceSize max_raw_bytes{};
    bool raw_enabled{true};
    std::string raw_control_path;
    CommandState bound;
    bool cache_commands{true};
    std::unordered_map<std::uint64_t, VkPipeline> pipelines;
    std::unordered_map<std::uint64_t, VkSampler> samplers;
    std::unordered_map<std::uint32_t, std::unique_ptr<Target>> targets;
    std::unordered_map<std::uint64_t, Texture> textures;
    std::array<TextureLookup,4096> texture_lookups{};
    std::vector<std::uint64_t> pending_texture_keys;
    VkDeviceSize cached_image_bytes{}, pending_texture_bytes{};
    std::unordered_map<std::uint64_t, std::weak_ptr<Image>> shared_images;
    std::shared_ptr<Image> white;
    std::vector<GeGpuVertex> vertices;
    std::vector<std::byte> packed_vertices, raw_vertices;
    std::vector<GeGpuModelState> models;
    std::vector<std::uint32_t> indices;
    std::vector<Batch> batches;
    std::vector<std::shared_ptr<Image>> inflight_images;
    std::vector<std::byte> rgba, last_texture;
    std::uint32_t width{480}, height{272}, display{}, owned{}, queue_family{};
    std::uint64_t epoch{1};
    std::uint64_t packed_draws{}, packed_vertex_bytes{}, frame_raw_vertices{};
    std::uint32_t texture_cache_entries{4096};
    VkDeviceSize texture_cache_bytes{256u * 1024u * 1024u};
    float anisotropy{1.0f};
    std::chrono::steady_clock::time_point perf_started{};
    double assemble_ms{}, wait_ms{}, readback_ms{};
    bool async_readback{}, pending_submission{}, pending_frame{};
    std::uint64_t pending_vblank{};
    std::uint32_t pending_display{};
    bool enabled{}, in_pass{}, recording{}, validation{}, packed_supported{};
    Target *current{};
    GeGpuBackendReport report;
};
State &state() {
    static State s;
    return s;
}

// A descriptor visits validation, decode availability and drawing in turn.
// Reuse the map lookup, never the content validation result. unordered_map
// preserves element pointers on rehash; erase/clear explicitly invalidate them.
std::size_t texture_lookup_bucket(std::uint64_t key) {
    return (key * 0x9e3779b97f4a7c15ull) >> 52;
}
Texture *find_texture(std::uint64_t key) {
    auto &s=state();auto &cached=s.texture_lookups[texture_lookup_bucket(key)];
    if(cached.key==key) return cached.texture;
    const auto it=s.textures.find(key);
    cached={key,it==s.textures.end()?nullptr:&it->second};
    return cached.texture;
}
void remember_texture(std::uint64_t key,Texture &texture) {
    state().texture_lookups[texture_lookup_bucket(key)]={key,&texture};
}
std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags flags,
                          VkMemoryPropertyFlags preferred = 0) {
    const auto &m = state().memory_properties;
    for (const auto wanted : {flags | preferred, flags})
        for (std::uint32_t i = 0; i < m.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & wanted) == wanted)
                return i;
    throw std::runtime_error("No suitable Vulkan memory type");
}
void create_buffer(Buffer &b, VkDeviceSize size, VkBufferUsageFlags usage,
                   VkMemoryPropertyFlags preferred = 0) {
    auto &s = state();
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    check(vkCreateBuffer(s.device, &ci, nullptr, &b.buffer), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(s.device, b.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                    preferred);
    check(vkAllocateMemory(s.device, &ai, nullptr, &b.memory), "vkAllocateMemory(buffer)");
    check(vkBindBufferMemory(s.device, b.buffer, b.memory, 0), "vkBindBufferMemory");
    void *ptr{};
    check(vkMapMemory(s.device, b.memory, 0, VK_WHOLE_SIZE, 0, &ptr), "vkMapMemory");
    b.mapped = static_cast<std::byte *>(ptr);
    if (preferred & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
        std::cerr << "[Vulkan memory] readback_cached="
                  << bool(s.memory_properties.memoryTypes[ai.memoryTypeIndex].propertyFlags &
                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT) << '\n';
}
void destroy_buffer(Buffer &b) {
    auto &s = state();
    if (b.mapped)
        vkUnmapMemory(s.device, b.memory);
    if (b.buffer)
        vkDestroyBuffer(s.device, b.buffer, nullptr);
    if (b.memory)
        vkFreeMemory(s.device, b.memory, nullptr);
    b = {};
}
std::shared_ptr<Image> create_image(std::uint32_t w, std::uint32_t h, std::uint32_t levels = 1,
                                    bool depth = false, bool render = false) {
    auto &s = state();
    auto i = std::make_shared<Image>();
    i->device = s.device;
    i->pool = s.descriptors;
    i->width = w;
    i->height = h;
    i->levels = levels;
    i->depth = depth;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = depth ? kDepth : kColor;
    ci.extent = {w, h, 1};
    ci.mipLevels = levels;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.usage |= depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_SAMPLED_BIT;
    if (render && !depth)
        ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    check(vkCreateImage(s.device, &ci, nullptr, &i->image), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(s.device, i->image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(s.device, &ai, nullptr, &i->memory), "vkAllocateMemory(image)");
    i->allocated_bytes = req.size;
    check(vkBindImageMemory(s.device, i->image, i->memory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = i->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ci.format;
    vi.subresourceRange = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, levels,
                           0, 1};
    check(vkCreateImageView(s.device, &vi, nullptr, &i->view), "vkCreateImageView");
    return i;
}
void end_pass() {
    auto &s = state();
    if (s.in_pass) {
        vkCmdEndRenderPass(s.command);
        s.in_pass = false;
        s.current = nullptr;
    }
}
void transition(Image &i, VkImageLayout layout) {
    if (i.layout == layout)
        return;
    end_pass();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = i.layout == VK_IMAGE_LAYOUT_UNDEFINED
                          ? 0
                          : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = i.layout;
    b.newLayout = layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = i.image;
    b.subresourceRange = {i.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0,
                          i.levels, 0, 1};
    vkCmdPipelineBarrier(state().command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    i.layout = layout;
}
VkDeviceSize allocate(VkDeviceSize bytes, VkDeviceSize alignment = 16) {
    auto &s = state();
    auto offset = (s.cursor + alignment - 1) / alignment * alignment;
    if (offset + bytes > s.arena.size)
        throw std::runtime_error("Vulkan upload arena exhausted");
    s.cursor = offset + bytes;
    return offset;
}
void update_model_descriptors() {
    auto &s=state();
    std::array<VkDescriptorBufferInfo,2> infos{{
        {s.raw_buffer.buffer,0,s.raw_buffer.size},{s.arena.buffer,0,sizeof(GeGpuModelState)}}};
    std::array<VkWriteDescriptorSet,2> writes{};
    for(unsigned i=0;i<2;++i) {
        writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet=s.model_set;writes[i].dstBinding=i;writes[i].descriptorCount=1;
        writes[i].descriptorType=i==0 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[i].pBufferInfo=&infos[i];
    }
    vkUpdateDescriptorSets(s.device,writes.size(),writes.data(),0,nullptr);
}
void reserve_raw_buffer() {
    auto &s=state();
    if(s.raw_vertices.size()>s.raw_buffer.size) {
        VkDeviceSize capacity=s.raw_buffer.size;
        while(capacity<s.raw_vertices.size()) capacity*=2;
        capacity=std::min(capacity,s.max_raw_bytes);
        if(capacity<s.raw_vertices.size()) throw std::runtime_error("GE raw vertex storage limit");
        destroy_buffer(s.raw_buffer);
        create_buffer(s.raw_buffer,capacity,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        update_model_descriptors();
    }
    if(!s.raw_vertices.empty()) std::memcpy(s.raw_buffer.mapped,s.raw_vertices.data(),s.raw_vertices.size());
}
void reserve_arena(VkDeviceSize bytes) {
    auto &s = state();
    if (bytes <= s.arena.size) return;
    // Called only after the previous fence and before recording. Keep dynamic
    // uniform offsets representable; never resize a buffer referenced by a GPU submission.
    if (bytes > std::numeric_limits<std::uint32_t>::max() / 2u)
        throw std::runtime_error("Vulkan frame exceeds the addressable upload buffer size");
    VkDeviceSize capacity = s.arena.size;
    while (capacity < bytes) capacity *= 2u;
    destroy_buffer(s.arena);
    create_buffer(s.arena, capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    VkDescriptorBufferInfo bi{s.arena.buffer, 0, sizeof(Uniform)};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = s.uniform_set;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    wr.descriptorCount = 1;
    wr.pBufferInfo = &bi;
    vkUpdateDescriptorSets(s.device, 1, &wr, 0, nullptr);
    update_model_descriptors();
    s.report.upload_capacity_bytes = capacity;
}
void begin_commands() {
    auto &s = state();
    check(vkResetCommandPool(s.device, s.commands, 0), "vkResetCommandPool");
    s.cursor = 0;
    s.bound = {};
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(s.command, &bi), "vkBeginCommandBuffer");
    s.recording = true;
}
void submit(bool wait = true) {
    auto &s = state();
    end_pass();
    check(vkEndCommandBuffer(s.command), "vkEndCommandBuffer");
    s.recording = false;
    check(vkResetFences(s.device, 1, &s.fence), "vkResetFences");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.command;
    check(vkQueueSubmit(s.queue, 1, &si, s.fence), "vkQueueSubmit");
    if (wait)
        check(vkWaitForFences(s.device, 1, &s.fence, VK_TRUE, 10'000'000'000ull), "vkWaitForFences");
    else
        s.pending_submission = true;
}
void upload(Image &image, std::span<const std::byte> pixels) {
    auto &s = state();
    std::vector<VkBufferImageCopy> copies;
    std::size_t offset = 0;
    // Texture-heavy HD frames may exceed the staging capacity cumulatively.
    // Submit completed uploads and reuse their memory only after the fence.
    if (pixels.size() > s.arena.size - ((s.cursor + 3u) & ~VkDeviceSize(3u))) {
        submit();
        ++s.report.upload_batch_flushes;
        begin_commands();
    }
    auto base = allocate(pixels.size(), 4);
    std::memcpy(s.arena.mapped + base, pixels.data(), pixels.size());
    auto w = image.width, h = image.height;
    for (unsigned level = 0; level < image.levels; ++level) {
        const auto bytes = std::size_t(w) * h * 4;
        if (offset + bytes > pixels.size())
            throw std::runtime_error("Incomplete Vulkan texture mip chain");
        VkBufferImageCopy c{};
        c.bufferOffset = base + offset;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        c.imageExtent = {w, h, 1};
        copies.push_back(c);
        offset += bytes;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    transition(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(s.command, s.arena.buffer, image.image, image.layout, copies.size(),
                           copies.data());
    transition(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    s.report.transfer_bytes += pixels.size();
    s.report.uploaded_mip_levels += image.levels;
}
Target &target_for(std::uint32_t address) {
    auto &s = state();
    auto &p = s.targets[address & address_mask];
    if (!p) {
        p = std::make_unique<Target>();
        p->device = s.device;
        p->address = address & address_mask;
    }
    p->used_epoch = s.epoch;
    return *p;
}
void ensure_target(Target &t) {
    auto &s = state();
    // Small offscreen effects must not each allocate/rasterize a full Retina
    // screen. Preserve the configured pixel density at their logical GE size.
    const bool small = t.address != s.display && t.logical_width < 480 && t.logical_height < 272;
    const auto width = small ? std::max(1u, (s.width * t.logical_width + 240u) / 480u) : s.width;
    const auto height = small ? std::max(1u, (s.height * t.logical_height + 136u) / 272u) : s.height;
    if (t.color && t.color->width == width && t.color->height == height) return;
    if (t.framebuffer) vkDestroyFramebuffer(s.device, t.framebuffer, nullptr);
    t.framebuffer = VK_NULL_HANDLE;
    t.snapshot.reset();
    t.epoch = 0;
    t.color = create_image(width, height, 1, false, true);
    t.depth = create_image(width, height, 1, true);
    std::array<VkImageView, 2> views{t.color->view, t.depth->view};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = s.render_pass;
    fi.attachmentCount = 2;
    fi.pAttachments = views.data();
    fi.width = width;
    fi.height = height;
    fi.layers = 1;
    check(vkCreateFramebuffer(s.device, &fi, nullptr, &t.framebuffer), "vkCreateFramebuffer");
    transition(*t.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black{};
    VkImageSubresourceRange cr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(s.command, t.color->image, t.color->layout, &black, 1, &cr);
}
void begin_target(Target &t) {
    auto &s = state();
    if (s.in_pass && s.current == &t)
        return;
    end_pass();
    ensure_target(t);
    if (t.epoch != s.epoch) {
        transition(*t.depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearDepthStencilValue zero{0, 0};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(s.command, t.depth->image, t.depth->layout, &zero, 1, &range);
        t.epoch = s.epoch;
    }
    transition(*t.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition(*t.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = s.render_pass;
    bi.framebuffer = t.framebuffer;
    bi.renderArea.extent = {t.color->width, t.color->height};
    vkCmdBeginRenderPass(s.command, &bi, VK_SUBPASS_CONTENTS_INLINE);
    s.in_pass = true;
    s.current = &t;
    t.rendered = true;
}
bool is_feedback(const GeGpuDrawDescriptor &d) {
    // Only VRAM can alias a framebuffer. Main RAM can have identical low
    // address bits, but its textures must still be decoded and uploaded.
    const auto physical = d.texture_address & 0x3fffffffu;
    return d.texture_enabled && (physical & 0x3f000000u) == 0x04000000u &&
           state().targets.contains(physical & address_mask);
}
VkSampler sampler_for(const GeGpuDrawDescriptor &d, bool feedback, std::uint32_t image_levels) {
    auto &s = state();
    std::uint64_t key = 0;
    key |= d.texture_min_linear;
    key |= std::uint64_t(d.texture_mag_linear) << 1;
    key |= std::uint64_t(d.texture_clamp_u) << 2;
    key |= std::uint64_t(d.texture_clamp_v) << 3;
    key |= std::uint64_t(d.texture_mipmap_linear) << 4;
    key |= std::uint64_t(d.texture_mipmap_enabled) << 5;
    key |= std::uint64_t(feedback) << 6;
    key |= std::uint64_t(feedback ? 0 : image_levels-1) << 8;
    key |= std::uint64_t(feedback ? 0 : d.texture_level_mode) << 16;
    key |= std::uint64_t(feedback ? 0 : d.texture_selected_level) << 24;
    auto found = s.samplers.find(key);
    if (found != s.samplers.end())
        return found->second;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = d.texture_mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = d.texture_min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode =
        d.texture_mipmap_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU =
        d.texture_clamp_u ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.addressModeV =
        d.texture_clamp_v ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.maxLod = !feedback && d.texture_mipmap_enabled ? float(image_levels-1) : 0;
    ci.anisotropyEnable = !feedback && d.texture_mipmap_enabled &&
        d.texture_min_linear && d.texture_mag_linear && s.anisotropy > 1.0f;
    ci.maxAnisotropy = ci.anisotropyEnable ? s.anisotropy : 1.0f;
    // MoltenVK's portability subset does not support sampler LOD bias. Apply
    // it in the fragment shader, which also works on ordinary Vulkan drivers.
    ci.mipLodBias = 0;
    if (!feedback && d.texture_level_mode == 1) {
        ci.minLod = ci.maxLod = std::clamp(float(d.texture_selected_level), 0.0f, ci.maxLod);
        ci.mipLodBias = 0;
    }
    VkSampler sampler{};
    check(vkCreateSampler(s.device, &ci, nullptr, &sampler), "vkCreateSampler");
    s.samplers.emplace(key, sampler);
    return sampler;
}
VkDescriptorSet descriptor(Image &i, VkSampler sampler) {
    auto key = std::uint64_t(reinterpret_cast<std::uintptr_t>(sampler));
    auto it = i.descriptors.find(key);
    if (it != i.descriptors.end())
        return it->second;
    auto &s = state();
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = s.descriptors;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &s.texture_layout;
    VkDescriptorSet set{};
    check(vkAllocateDescriptorSets(s.device, &ai, &set), "vkAllocateDescriptorSets(texture)");
    VkDescriptorImageInfo ii{sampler, i.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = set;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(s.device, 1, &wr, 0, nullptr);
    i.descriptors.emplace(key, set);
    ++s.report.texture_descriptor_sets_allocated;
    return set;
}
unsigned blend_variant(const GeGpuDrawDescriptor &d) {
    if (!d.blend_enabled || d.clear_mode)
        return 0;
    auto eq = d.blend_equation & 7u, src = d.blend_source_factor & 15u,
         dst = d.blend_dest_factor & 15u;
    if (eq == 0 && src == 2 && dst == 3)
        return 1;
    if (eq == 0 && src == 10 && dst == 10) {
        auto a = d.blend_fix_source & 0xffffffu, b = d.blend_fix_dest & 0xffffffu;
        if (a == 0xffffffu && b == 0)
            return 0;
        if (a == 0xffffffu && b == 0xffffffu)
            return 2;
        if (((a & 255) + (b & 255) == 255) && (((a >> 8) & 255) + ((b >> 8) & 255) == 255) &&
            (((a >> 16) & 255) + ((b >> 16) & 255) == 255))
            return 3;
    }
    if (eq == 0 && src == 2 && dst == 10 && (d.blend_fix_dest & 0xffffffu) == 0xffffffu)
        return 4;
    return 5;
}
VkCompareOp compare(std::uint32_t fn) {
    constexpr VkCompareOp ops[] = {VK_COMPARE_OP_NEVER,   VK_COMPARE_OP_ALWAYS,
                                   VK_COMPARE_OP_EQUAL,   VK_COMPARE_OP_NOT_EQUAL,
                                   VK_COMPARE_OP_LESS,    VK_COMPARE_OP_LESS_OR_EQUAL,
                                   VK_COMPARE_OP_GREATER, VK_COMPARE_OP_GREATER_OR_EQUAL};
    return ops[fn & 7u];
}
VkColorComponentFlags color_mask(const GeGpuDrawDescriptor &d) {
    if (d.clear_mode)
        return (d.clear_color ? 7u : 0u) | (d.clear_alpha ? 8u : 0u);
    VkColorComponentFlags mask{};
    for (unsigned i = 0; i < 4; ++i)
        if (((d.color_write_mask >> (8 * i)) & 255) != 255)
            mask |= 1u << i;
    return mask;
}
VkPipeline pipeline_for(const Batch &batch) {
    auto &s = state();
    auto &d = batch.draw;
    bool clear = d.clear_mode;
    bool depth = clear ? d.clear_depth : (d.depth_test_enabled || d.depth_write_enabled);
    bool write = clear ? d.clear_depth : d.depth_write_enabled;
    auto fn = clear || !d.depth_test_enabled ? 1u : d.depth_function;
    bool cull = batch.hardware && batch.transform.cull_enabled && !clear;
    unsigned blend = blend_variant(d);
    std::uint64_t key = color_mask(d) | (std::uint64_t(depth) << 4) | (std::uint64_t(write) << 5) |
                        (std::uint64_t(fn & 7u) << 6) | (std::uint64_t(blend) << 9) |
                        (std::uint64_t(cull) << 12) |
                        (std::uint64_t(batch.transform.accept_counter_clockwise) << 13) |
                        (std::uint64_t(batch.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP) << 14) |
                        (std::uint64_t(batch.packed) << 15) | (std::uint64_t(batch.raw) << 16);
    auto it = s.pipelines.find(key);
    if (it != s.pipelines.end())
        return it->second;
    std::array<VkPipelineShaderStageCreateInfo, 2> shaders{};
    for (auto &sh : shaders) {
        sh.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        sh.pName = "main";
    }
    shaders[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaders[0].module = batch.raw ? s.model_vertex_shader : (batch.packed ? s.packed_vertex_shader : s.vertex_shader);
    shaders[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaders[1].module = s.fragment_shader;
    VkVertexInputBindingDescription binding{0, sizeof(GeGpuVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription, 5> attrs{
        {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GeGpuVertex, x)},
         {1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(GeGpuVertex, rgba)},
         {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GeGpuVertex, u)},
         {3, 0, VK_FORMAT_R32_SFLOAT, offsetof(GeGpuVertex, q)},
         {4, 0, VK_FORMAT_R32_SFLOAT, offsetof(GeGpuVertex, fog_factor)}}};
    // The GE's 0x0115 layout is ten bytes. Integer vertex fetch plus the
    // packed shader replaces the per-vertex CPU decode, without changing
    // normalization, 5551 colour expansion or affine lighting rounding.
    constexpr std::array<VkVertexInputAttributeDescription, 4> packed_attrs{{
        {0, 0, VK_FORMAT_R8G8_UINT, 0}, {1, 0, VK_FORMAT_R16_UINT, 2},
        {2, 0, VK_FORMAT_R16G16_SINT, 4}, {3, 0, VK_FORMAT_R16_SINT, 8}}};
    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = attrs.size();
    vi.pVertexAttributeDescriptions = attrs.data();
    if (batch.packed) {
        binding.stride = 10;
        vi.vertexAttributeDescriptionCount = packed_attrs.size();
        vi.pVertexAttributeDescriptions = packed_attrs.data();
    }
    if (batch.raw) { vi.vertexBindingDescriptionCount=0; vi.vertexAttributeDescriptionCount=0; }
    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = batch.topology;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = batch.transform.accept_counter_clockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                            : VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = depth;
    ds.depthWriteEnable = write;
    ds.depthCompareOp = compare(fn);
    VkPipelineColorBlendAttachmentState blend_state{};
    blend_state.colorWriteMask = color_mask(d);
    blend_state.blendEnable = blend != 0 && blend != 5;
    blend_state.colorBlendOp = blend_state.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_state.srcColorBlendFactor = blend == 1 || blend == 4 ? VK_BLEND_FACTOR_SRC_ALPHA
                                      : blend == 3             ? VK_BLEND_FACTOR_CONSTANT_COLOR
                                                               : VK_BLEND_FACTOR_ONE;
    blend_state.dstColorBlendFactor = blend == 1   ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
                                      : blend == 3 ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR
                                      : blend == 2 || blend == 4 ? VK_BLEND_FACTOR_ONE
                                                                 : VK_BLEND_FACTOR_ZERO;
    blend_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_state.dstAlphaBlendFactor = blend == 1 ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
                                      : blend == 2 || blend == 4 ? VK_BLEND_FACTOR_ONE
                                                                 : VK_BLEND_FACTOR_ZERO;
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend_state;
    std::array<VkDynamicState, 3> dyn{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                      VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = dyn.size();
    dy.pDynamicStates = dyn.data();
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = shaders.data();
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = s.pipeline_layout;
    ci.renderPass = s.render_pass;
    VkPipeline p{};
    check(vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p),
          "vkCreateGraphicsPipelines");
    s.pipelines.emplace(key, p);
    s.report.unique_pipeline_keys = s.pipelines.size();
    return p;
}

Uniform uniforms(const Batch &b, const Target &t, bool textured) {
    Uniform u;
    const auto &d = b.draw;
    if (b.hardware) {
        const auto &h = b.transform;
        auto row = [&](int r) {
            return std::array<float, 4>{h.model_to_clip[r], h.model_to_clip[r + 4],
                                        h.model_to_clip[r + 8], h.model_to_clip[r + 12]};
        };
        auto combine = [](auto a, float sa, auto c, float sc) {
            for (int i = 0; i < 4; ++i)
                a[i] = a[i] * sa + c[i] * sc;
            return a;
        };
        auto w = row(3);
        float sx = 2.0f / t.logical_width, sy = 2.0f / t.logical_height;
        u.row0 = combine(row(0), h.viewport_scale_x * sx, w,
                         (h.viewport_center_x - h.viewport_offset_x) * sx - 1);
        // Vulkan framebuffer Y increases downward, as PSP screen coordinates do.
        u.row1 = combine(row(1), h.viewport_scale_y * sy, w,
                         (h.viewport_center_y - h.viewport_offset_y) * sy - 1);
        u.row2 = combine(row(2), h.viewport_scale_z / 65535.0f, w, h.viewport_center_z / 65535.0f);
        u.row3 = w;
        u.view_z = h.model_to_view_z;
        u.uv = {h.uv_scale_u, h.uv_scale_v, h.uv_offset_u, h.uv_offset_v};
        u.fog = {h.fog_end, h.fog_slope, 0, 0};
        u.mode = {1u, h.depth_clip_enabled ? 1u : 0u, h.vertex_color_affine ? 1u : 0u, 0u};
        u.color_mul = h.vertex_color_mul;
        u.color_add = h.vertex_color_add;
    } else
        u.uv = {2.0f / t.logical_width, 2.0f / t.logical_height, 0, 0};
    if (!d.clear_mode) {
        u.pixel[0] = (d.alpha_test_enabled ? 1u : 0u) | ((d.alpha_function & 7u) << 8) |
                     ((d.alpha_reference & 255u) << 16) | ((d.alpha_mask & 255u) << 24);
        u.pixel[1] = (d.texture_function & 7u) | (std::uint32_t(d.texture_use_alpha) << 8) |
                     (std::uint32_t(d.texture_double_color) << 16) |
                     (std::uint32_t(textured) << 24);
        u.pixel[2] = d.texture_env;
        u.pixel[3] = (d.fog_color & 0xffffffu) | (std::uint32_t(d.fog_enabled) << 24);
    }
    u.format[0] = d.framebuffer_format;
    if (textured && !is_feedback(d) && d.texture_level_mode != 1)
        u.fog[2] = std::clamp(d.texture_level_offset16 / 16.0f, -2.0f, 2.0f);
    return u;
}
void fail(const char *where, const std::exception &e) {
    auto &s = state();
    s.report.message = std::string(where) + ": " + e.what();
    s.report.message += "; texture_bytes=" + std::to_string(s.report.resident_texture_bytes) +
        " target_bytes=" + std::to_string(s.report.resident_target_bytes) +
        " targets=" + std::to_string(s.targets.size()) +
        " pipelines=" + std::to_string(s.pipelines.size()) +
        " upload_bytes=" + std::to_string(s.cursor) +
        " batches=" + std::to_string(s.batches.size());
    std::cerr << "[Vulkan] " << s.report.message << '\n';
}

// Account for shared images when references actually change. Reconstructing
// this set twice per frame walked thousands of cold historical cache entries.
void set_texture_image(Texture &texture, std::shared_ptr<Image> image) {
    auto &s = state();
    if (texture.image == image) return;
    if (texture.image && --texture.image->cache_references == 0)
        s.cached_image_bytes -= texture.image->allocated_bytes;
    texture.image = std::move(image);
    if (texture.image && texture.image->cache_references++ == 0)
        s.cached_image_bytes += texture.image->allocated_bytes;
}

void clear_pending_texture(Texture &texture) {
    state().pending_texture_bytes -= texture.pending.size();
    texture.pending.clear();
}

void retain_frame_image(const std::shared_ptr<Image> &image) {
    auto &s = state();
    if (!s.async_readback || image->retained_epoch == s.epoch) return;
    s.inflight_images.push_back(image);
    image->retained_epoch = s.epoch;
    ++s.report.inflight_image_references;
}

void prune_caches() {
    auto &s = state();
    // GPU work has completed here. Mark this frame's actual references before
    // eviction; merely counting entries allowed a few large HD images to use
    // gigabytes even though TextureCacheMB was set to 256.
    const auto over_budget = [&] {
        return s.cached_image_bytes + s.pending_texture_bytes > s.texture_cache_bytes ||
               s.textures.size() > s.texture_cache_entries;
    };
    if (over_budget()) {
        for (const auto &b : s.batches) {
            if (!b.draw.texture_enabled || is_feedback(b.draw)) continue;
            if (auto *texture=find_texture(texture_key(b.draw))) texture->used_epoch=s.epoch;
        }
        std::vector<std::pair<std::uint64_t, std::uint64_t>> unused;
        for (const auto &[key, t] : s.textures) {
            ++s.report.texture_eviction_entries_scanned;
            if (t.used_epoch != s.epoch) unused.emplace_back(t.used_epoch, key);
        }
        std::sort(unused.begin(), unused.end());
        for (const auto &[age, key] : unused) {
            if (!over_budget()) break;
            auto &t = s.textures.at(key);
            clear_pending_texture(t);
            set_texture_image(t, {});
            auto &cached=s.texture_lookups[texture_lookup_bucket(key)];
            if(cached.key==key) cached={};
            s.textures.erase(key);
            ++s.report.evicted_textures;
        }
    }
    // Weak aliases own no GPU memory. Clean them periodically, not every frame.
    if (s.epoch % 120 == 0)
        std::erase_if(s.shared_images, [](const auto &item) { return item.second.expired(); });
    // PSP reuses VRAM addresses for scratch targets. Native-size attachments
    // at every historical address must not accumulate for the whole session.
    std::erase_if(s.targets, [&](const auto &item) {
        return item.first != s.display && item.second->used_epoch + 8u < s.epoch;
    });
}

void report_cache_memory() {
    auto &s = state();
    s.report.resident_texture_bytes = s.cached_image_bytes;
    s.report.resident_target_count = s.targets.size();
    s.report.resident_target_bytes = 0;
    for (const auto &[key, target] : s.targets)
        for (const auto &image : {target->color, target->depth, target->snapshot})
            if (image) s.report.resident_target_bytes += image->allocated_bytes;
}
bool extension(const std::vector<VkExtensionProperties> &list, const char *name) {
    return std::any_of(list.begin(), list.end(),
                       [&](auto &p) { return std::strcmp(p.extensionName, name) == 0; });
}
void create_backend() {
    auto &s = state();
    std::uint32_t count{};
    check(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr),
          "vkEnumerateInstanceExtensionProperties");
    std::vector<VkExtensionProperties> extensions(count);
    check(vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()),
          "vkEnumerateInstanceExtensionProperties");
    std::vector<const char *> enabled;
    bool portability = extension(extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    if (portability)
        enabled.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "PSPRecomp GE";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = enabled.size();
    ci.ppEnabledExtensionNames = enabled.data();
    if (portability)
        ci.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    const char *validation = "VK_LAYER_KHRONOS_validation";
    if (const char *value = std::getenv("PSPRECOMP_VULKAN_VALIDATION");
        value && *value && std::strcmp(value, "0") != 0) {
        std::uint32_t layers{};
        check(vkEnumerateInstanceLayerProperties(&layers, nullptr),
              "vkEnumerateInstanceLayerProperties");
        std::vector<VkLayerProperties> list(layers);
        check(vkEnumerateInstanceLayerProperties(&layers, list.data()),
              "vkEnumerateInstanceLayerProperties");
        if (std::any_of(list.begin(), list.end(),
                        [&](auto &x) { return std::strcmp(x.layerName, validation) == 0; })) {
            ci.enabledLayerCount = 1;
            ci.ppEnabledLayerNames = &validation;
            s.validation = true;
        } else
            throw std::runtime_error(
                "Vulkan validation requested, but VK_LAYER_KHRONOS_validation is missing");
    }
    check(vkCreateInstance(&ci, nullptr, &s.instance), "vkCreateInstance");
    s.report.instance_created = true;
    check(vkEnumeratePhysicalDevices(s.instance, &count, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(s.instance, &count, devices.data()),
          "vkEnumeratePhysicalDevices");
    s.report.physical_device_count = count;
    VkPhysicalDeviceProperties selected{};
    int best = -1;
    for (auto device : devices) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(device, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU &&
            !std::getenv("PSPRECOMP_VULKAN_ALLOW_CPU_DEVICE"))
            continue;
        std::uint32_t n{};
        vkGetPhysicalDeviceQueueFamilyProperties(device, &n, nullptr);
        std::vector<VkQueueFamilyProperties> queues(n);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &n, queues.data());
        for (unsigned i = 0; i < n; ++i)
            if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                            : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                                     : 1;
                if (score > best) {
                    best = score;
                    s.physical = device;
                    s.queue_family = i;
                    selected = p;
                }
                break;
            }
    }
    if (!s.physical)
        throw std::runtime_error("No Vulkan hardware graphics device found");
    s.packed_supported = true;
    for (auto format : {VK_FORMAT_R8G8_UINT, VK_FORMAT_R16_UINT,
                        VK_FORMAT_R16G16_SINT, VK_FORMAT_R16_SINT}) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(s.physical, format, &properties);
        s.packed_supported &= (properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
    }
    s.uniform_alignment =
        std::max<VkDeviceSize>(16, selected.limits.minUniformBufferOffsetAlignment);
    if (s.width > selected.limits.maxImageDimension2D ||
        s.height > selected.limits.maxImageDimension2D)
        throw std::runtime_error("Internal resolution exceeds Vulkan device limits");
    vkGetPhysicalDeviceMemoryProperties(s.physical, &s.memory_properties);
    VkFormatProperties color_support{}, depth_support{};
    vkGetPhysicalDeviceFormatProperties(s.physical, kColor, &color_support);
    vkGetPhysicalDeviceFormatProperties(s.physical, kDepth, &depth_support);
    if (!(color_support.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ||
        !(depth_support.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        throw std::runtime_error("Required Vulkan color/depth formats are unsupported");
    check(vkEnumerateDeviceExtensionProperties(s.physical, nullptr, &count, nullptr),
          "vkEnumerateDeviceExtensionProperties");
    extensions.resize(count);
    check(vkEnumerateDeviceExtensionProperties(s.physical, nullptr, &count, extensions.data()),
          "vkEnumerateDeviceExtensionProperties");
    enabled.clear();
    if (extension(extensions, "VK_KHR_portability_subset"))
        enabled.push_back("VK_KHR_portability_subset");
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = s.queue_family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkPhysicalDeviceFeatures available{}, features{};
    vkGetPhysicalDeviceFeatures(s.physical, &available);
    features.samplerAnisotropy = available.samplerAnisotropy && s.anisotropy > 1.0f;
    s.anisotropy = features.samplerAnisotropy
        ? std::clamp(s.anisotropy, 1.0f, selected.limits.maxSamplerAnisotropy) : 1.0f;
    di.pEnabledFeatures = &features;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = enabled.size();
    di.ppEnabledExtensionNames = enabled.data();
    check(vkCreateDevice(s.physical, &di, nullptr, &s.device), "vkCreateDevice");
    vkGetDeviceQueue(s.device, s.queue_family, 0, &s.queue);
    s.report.device_created = true;
    s.report.graphics_queue_family = s.queue_family;
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = s.queue_family;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    check(vkCreateCommandPool(s.device, &pi, nullptr, &s.commands), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = s.commands;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(s.device, &ca, &s.command), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(s.device, &fi, nullptr, &s.fence), "vkCreateFence");
    std::array<VkDescriptorPoolSize, 3> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 65536},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = 65538;
    dp.poolSizeCount = sizes.size();
    dp.pPoolSizes = sizes.data();
    check(vkCreateDescriptorPool(s.device, &dp, nullptr, &s.descriptors), "vkCreateDescriptorPool");
    VkDescriptorSetLayoutBinding tb{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &tb;
    check(vkCreateDescriptorSetLayout(s.device, &li, nullptr, &s.texture_layout),
          "vkCreateDescriptorSetLayout(texture)");
    VkDescriptorSetLayoutBinding ub{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                    nullptr};
    li.pBindings = &ub;
    check(vkCreateDescriptorSetLayout(s.device, &li, nullptr, &s.uniform_layout),
          "vkCreateDescriptorSetLayout(uniform)");
    std::array<VkDescriptorSetLayoutBinding,2> mb{{
        {0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr}}};
    li.bindingCount=mb.size();li.pBindings=mb.data();
    check(vkCreateDescriptorSetLayout(s.device,&li,nullptr,&s.model_layout),"vkCreateDescriptorSetLayout(model)");
    std::array<VkDescriptorSetLayout, 3> layouts{s.texture_layout, s.uniform_layout, s.model_layout};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = layouts.size();
    pli.pSetLayouts = layouts.data();
    check(vkCreatePipelineLayout(s.device, &pli, nullptr, &s.pipeline_layout),
          "vkCreatePipelineLayout");
    create_buffer(s.arena, kArenaBytes,
                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    s.max_raw_bytes=selected.limits.maxStorageBufferRange;
    create_buffer(s.raw_buffer,std::min<VkDeviceSize>(1024*1024,s.max_raw_bytes),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    // CPU reads need cached memory. RADV's first coherent host-visible type
    // can be write-combined: reading a 1280x725 frame from it takes ~31 ms on
    // Deck. Keep coherence (and the existing fence/barrier synchronization),
    // preferring a cached type when available. Upload buffers keep their policy.
    create_buffer(s.readback, VkDeviceSize(s.width) * s.height * 4,
                  VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    VkDescriptorSetAllocateInfo ua{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ua.descriptorPool = s.descriptors;
    ua.descriptorSetCount = 1;
    ua.pSetLayouts = &s.uniform_layout;
    check(vkAllocateDescriptorSets(s.device, &ua, &s.uniform_set),
          "vkAllocateDescriptorSets(uniform)");
    VkDescriptorBufferInfo bi{s.arena.buffer, 0, sizeof(Uniform)};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = s.uniform_set;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    wr.descriptorCount = 1;
    wr.pBufferInfo = &bi;
    vkUpdateDescriptorSets(s.device, 1, &wr, 0, nullptr);
    ua.pSetLayouts=&s.model_layout;
    check(vkAllocateDescriptorSets(s.device,&ua,&s.model_set),"vkAllocateDescriptorSets(model)");
    update_model_descriptors();
    std::array<VkAttachmentDescription, 2> attachments{};
    attachments[0].format = kColor;
    attachments[1].format = kDepth;
    for (auto &a : attachments) {
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    attachments[0].initialLayout = attachments[0].finalLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].initialLayout = attachments[1].finalLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color;
    sub.pDepthStencilAttachment = &depth;
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    deps[0].dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = deps[0].dstStageMask;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    deps[1].srcAccessMask = deps[0].dstAccessMask;
    deps[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ri.attachmentCount = 2;
    ri.pAttachments = attachments.data();
    ri.subpassCount = 1;
    ri.pSubpasses = &sub;
    ri.dependencyCount = deps.size();
    ri.pDependencies = deps.data();
    check(vkCreateRenderPass(s.device, &ri, nullptr, &s.render_pass), "vkCreateRenderPass");
    auto shader = [&](const std::uint32_t *code, std::size_t bytes, VkShaderModule &out) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes;
        ci.pCode = code;
        check(vkCreateShaderModule(s.device, &ci, nullptr, &out), "vkCreateShaderModule");
    };
    shader(psp_ge_vert_spv, sizeof(psp_ge_vert_spv), s.vertex_shader);
    if (s.packed_supported)
        shader(psp_ge_packed_vert_spv, sizeof(psp_ge_packed_vert_spv), s.packed_vertex_shader);
    shader(psp_ge_model_vert_spv, sizeof(psp_ge_model_vert_spv), s.model_vertex_shader);
    shader(psp_ge_frag_spv, sizeof(psp_ge_frag_spv), s.fragment_shader);
    begin_commands();
    s.white = create_image(1, 1);
    std::array<std::byte, 4> white{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    upload(*s.white, white);
    submit();
    s.report.message =
        std::string(selected.deviceName) + "; Vulkan GPU GE, final-frame readback to SDL";
    s.report.loader_opened = s.report.transfer_buffer_created = s.report.transfer_memory_mapped =
        s.report.command_pool_created = s.report.shader_modules_created = true;
    s.report.offscreen_width = s.width;
    s.report.offscreen_height = s.height;
    s.report.upload_capacity_bytes = kArenaBytes;
    s.report.active = GeGpuBackendKind::Vulkan;
    s.enabled = true;
}
void destroy_backend() {
    auto &s = state();
    if (s.device) {
        vkDeviceWaitIdle(s.device);
        s.targets.clear();
        s.textures.clear();
        s.texture_lookups={};
        s.shared_images.clear();
        s.inflight_images.clear();
        s.white.reset();
        for (auto [key, p] : s.pipelines)
            vkDestroyPipeline(s.device, p, nullptr);
        for (auto [key, p] : s.samplers)
            vkDestroySampler(s.device, p, nullptr);
        if (s.vertex_shader)
            vkDestroyShaderModule(s.device, s.vertex_shader, nullptr);
        if (s.packed_vertex_shader)
            vkDestroyShaderModule(s.device, s.packed_vertex_shader, nullptr);
        if (s.model_vertex_shader) vkDestroyShaderModule(s.device,s.model_vertex_shader,nullptr);
        if (s.fragment_shader)
            vkDestroyShaderModule(s.device, s.fragment_shader, nullptr);
        if (s.pipeline_layout)
            vkDestroyPipelineLayout(s.device, s.pipeline_layout, nullptr);
        if (s.descriptors)
            vkDestroyDescriptorPool(s.device, s.descriptors, nullptr);
        if (s.texture_layout)
            vkDestroyDescriptorSetLayout(s.device, s.texture_layout, nullptr);
        if (s.uniform_layout)
            vkDestroyDescriptorSetLayout(s.device, s.uniform_layout, nullptr);
        if(s.model_layout) vkDestroyDescriptorSetLayout(s.device,s.model_layout,nullptr);
        destroy_buffer(s.raw_buffer);
        if (s.render_pass)
            vkDestroyRenderPass(s.device, s.render_pass, nullptr);
        destroy_buffer(s.arena);
        destroy_buffer(s.readback);
        if (s.fence)
            vkDestroyFence(s.device, s.fence, nullptr);
        if (s.commands)
            vkDestroyCommandPool(s.device, s.commands, nullptr);
        vkDestroyDevice(s.device, nullptr);
    }
    if (s.instance)
        vkDestroyInstance(s.instance, nullptr);
    s = State{};
}
} // namespace

bool initialize_vulkan_backend(const VulkanBackendConfiguration &configuration,
                               std::string &error) {
    auto &s = state();
    destroy_backend();
    if (const char *value = std::getenv("PSPRECOMP_VULKAN_STATE_CACHE"))
        s.cache_commands = std::strcmp(value, "0") != 0;
    if (const char *value = std::getenv("PSPRECOMP_GE_GPU_RAW_MODEL"))
        s.raw_enabled = std::strcmp(value, "0") != 0;
    if (const char *value=std::getenv("PSPRECOMP_GE_GPU_RAW_MODEL_CONTROL")) s.raw_control_path=value;
    s.report.requested =
        configuration.enabled ? GeGpuBackendKind::Vulkan : GeGpuBackendKind::Software;
    if (!configuration.enabled) {
        s.report.message = "Software PSP GE rasterizer";
        return true;
    }
    try {
        s.width = configuration.width;
        s.height = configuration.height;
        s.texture_cache_entries = configuration.texture_cache_entries;
        s.texture_cache_bytes = VkDeviceSize(configuration.texture_cache_mb) * 1024u * 1024u;
        s.anisotropy = static_cast<float>(configuration.anisotropic_filtering);
        s.async_readback = configuration.async_readback;
        if (!s.width || !s.height)
            throw std::runtime_error("Vulkan target size must be positive");
        create_backend();
        std::cout << "[Vulkan] " << s.report.message << "; validation="
                  << (s.validation ? "on" : "off") << "; anisotropy=" << s.anisotropy
                  << "x; async_readback=" << s.async_readback << '\n';
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        destroy_backend();
        return false;
    }
}
bool initialize_ge_gpu_backend(std::string &error) {
    const auto &rendering = vcs_configuration().rendering;
    bool enabled = rendering.backend == RenderingBackend::Vulkan;
    if (const char *mode = std::getenv("PSPRECOMP_GE_BACKEND"))
        enabled = std::string(mode) == "vulkan";
    if (enabled && rendering.msaa != 1) {
        error = "Vulkan GE currently requires MSAA=1";
        return false;
    }
    const auto size = resolve_internal_resolution(rendering);
    VulkanBackendConfiguration configuration;
    configuration.enabled = enabled;
    configuration.width = size.width;
    configuration.height = size.height;
    configuration.texture_cache_entries = rendering.texture_cache_entries;
    configuration.texture_cache_mb = rendering.texture_cache_mb;
    configuration.anisotropic_filtering = rendering.anisotropic_filtering;
    configuration.async_readback = true;
    if (const char *value = std::getenv("PSPRECOMP_VULKAN_ASYNC"))
        configuration.async_readback = *value && std::strcmp(value, "0") != 0;
    return initialize_vulkan_backend(configuration, error);
}

void shutdown_ge_gpu_backend() noexcept { destroy_backend(); }
bool ge_gpu_backend_active() noexcept { return state().enabled; }
bool ge_gpu_backend_transfer_ready() noexcept { return state().enabled; }
bool ge_gpu_backend_graphics_ready() noexcept { return state().enabled; }
void ge_gpu_backend_record_draw(const GeGpuDrawDescriptor &d) noexcept {
    auto &s = state();
    if (!s.enabled)
        return;
    try {
        ++s.report.draw_calls;
        s.report.vertices += d.vertex_count;
        if (d.texture_enabled)
            ++s.report.textured_draw_calls;
        auto address = d.framebuffer_address & address_mask;
        if (d.framebuffer_stride || address == s.display) {
            auto &t = target_for(address);
            if (address == s.display) {
                t.logical_width = 480;
                t.logical_height = 272;
            } else {
                t.logical_width =
                    std::max(t.logical_width, d.framebuffer_stride
                                                  ? d.framebuffer_stride
                                                  : std::uint32_t(std::max(1, d.scissor_x1 + 1)));
                t.logical_height =
                    std::max(t.logical_height, std::uint32_t(std::max(1, d.scissor_y1 + 1)));
            }
        }
        if (is_feedback(d)) {
            auto &t = target_for(d.texture_address);
            if ((d.texture_address & address_mask) != s.display) {
                if (d.texture_width)
                    t.logical_width = d.texture_width;
                if (d.texture_height)
                    t.logical_height = d.texture_height;
            }
        }
        s.report.framebuffer_targets_observed = s.targets.size();
    } catch (const std::exception &e) {
        fail("record draw", e);
    }
}
void ge_gpu_backend_observe_camera(const std::array<float, 12> &, const std::array<float, 16> &,
                                   const std::array<float, 6> &, const std::array<float, 3> &,
                                   const GeGpuDrawDescriptor &, std::uint32_t) noexcept {}
bool ge_gpu_backend_stage_vertices(const GeGpuDrawDescriptor &,
                                   std::span<const GeGpuVertex>) noexcept {
    return state().enabled;
}
void ge_gpu_backend_prepare_texture_keys(GeGpuDrawDescriptor &d) noexcept {
    d.texture_cache_key_hint = d.texture_image_key_hint = 0;
    if(d.texture_enabled) d.texture_cache_key_hint = texture_key(d);
    // Image sharing is consulted only when pixels need uploading/adopting.
    // Leave its key lazy; ordinary cache hits should not hash the same 50+
    // identity fields a second time just to discard the result.
}
bool ge_gpu_backend_is_framebuffer_feedback_texture(const GeGpuDrawDescriptor &d) noexcept {
    return state().enabled && is_feedback(d);
}
bool ge_gpu_backend_texture_signature_needed(const GeGpuDrawDescriptor &d) noexcept {
    auto &s = state();
    if (!s.enabled || is_feedback(d))
        return false;
    const auto *texture=find_texture(texture_key(d));
    return !texture || texture->checked_epoch!=s.epoch;
}
bool ge_gpu_backend_texture_needed(const GeGpuDrawDescriptor &d) noexcept {
    auto &s = state();
    if (!s.enabled || !d.texture_enabled || is_feedback(d))
        return false;
    ++s.report.texture_decode_requests;
    auto *texture=find_texture(texture_key(d));
    if (!texture)
        return true;
    texture->checked_epoch = texture->used_epoch = s.epoch;
    // Zero means the frontend omitted hashing: this entry was already checked
    // in this display interval. It is not a new content hash. Comparing it to
    // the cached hash caused an unchanged texture to be decoded/uploaded again
    // on the second draw, and again on the first draw of every following frame.
    const bool needed = (!texture->image && texture->pending.empty()) ||
        (d.texture_content_signature != 0 && texture->signature != d.texture_content_signature);
    if (!needed) ++s.report.texture_cache_hits;
    return needed;
}
bool ge_gpu_backend_adopt_shared_texture(const GeGpuDrawDescriptor &d) noexcept {
    auto &s = state();
    if (!s.enabled || is_feedback(d))
        return false;
    auto key = mix(d.texture_image_key_hint ? d.texture_image_key_hint : image_key(d),
                   d.texture_content_signature);
    auto found = s.shared_images.find(key);
    if (found == s.shared_images.end())
        return false;
    auto image = found->second.lock();
    if (!image)
        return false;
    const auto texture_identity=texture_key(d);
    auto &t = s.textures[texture_identity];
    remember_texture(texture_identity,t);
    set_texture_image(t, image);
    t.width = image->width;
    t.height = image->height;
    t.levels = image->levels;
    clear_pending_texture(t);
    t.signature = d.texture_content_signature;
    t.share_key = key;
    t.checked_epoch = t.used_epoch = s.epoch;
    ++s.report.shared_texture_images;
    return true;
}
bool ge_gpu_backend_texture_available(const GeGpuDrawDescriptor &d) noexcept {
    auto &s = state();
    if (!s.enabled)
        return false;
    if (is_feedback(d))
        return true;
    const auto *texture=find_texture(texture_key(d));
    return texture && (texture->image || !texture->pending.empty());
}
bool ge_gpu_backend_upload_decoded_texture_chain_packed(const GeGpuDrawDescriptor &d,
                                                        std::uint32_t w, std::uint32_t h,
                                                        std::uint32_t levels,
                                                        std::vector<std::byte> bytes) noexcept {
    if (!state().enabled)
        return false;
    try {
        if (!w || !h || !levels || levels > 15)
            return false;
        auto &s = state();
        const auto key = texture_key(d);
        auto &t = s.textures[key];
        remember_texture(key,t);
        if (t.width != w || t.height != h || t.levels != levels)
            set_texture_image(t, {});
        t.width = w;
        t.height = h;
        t.levels = levels;
        if (!t.pending_queued) {
            s.pending_texture_keys.push_back(key);
            t.pending_queued = true;
        }
        clear_pending_texture(t);
        t.pending = std::move(bytes);
        s.pending_texture_bytes += t.pending.size();
        t.signature = d.texture_content_signature;
        t.share_key = mix(d.texture_image_key_hint ? d.texture_image_key_hint : image_key(d),
                          d.texture_content_signature);
        t.checked_epoch = t.used_epoch = s.epoch;
        s.last_texture.assign(t.pending.begin(),
                              t.pending.begin() +
                                  std::min(t.pending.size(), std::size_t(w) * h * 4));
        // Publish this signature only after its pixels have been uploaded.
        // t.image may still contain the old content (including an in-flight frame).
        s.report.last_texture_width = w;
        s.report.last_texture_height = h;
        s.report.unique_texture_keys = s.textures.size();
        return true;
    } catch (const std::exception &e) {
        fail("texture upload", e);
        return false;
    }
}
bool ge_gpu_backend_upload_decoded_texture_chain(
    const GeGpuDrawDescriptor &d, std::span<const GeGpuDecodedMipLevel> levels) noexcept {
    if (levels.empty())
        return false;
    try {
        std::vector<std::byte> data;
        for (auto &l : levels)
            data.insert(data.end(), l.rgba8.begin(), l.rgba8.end());
        return ge_gpu_backend_upload_decoded_texture_chain_packed(
            d, levels[0].width, levels[0].height, levels.size(), std::move(data));
    } catch (...) {
        return false;
    }
}
bool ge_gpu_backend_upload_decoded_texture(const GeGpuDrawDescriptor &d, std::uint32_t w,
                                           std::uint32_t h,
                                           std::span<const std::byte> bytes) noexcept {
    GeGpuDecodedMipLevel level{w, h, bytes};
    return ge_gpu_backend_upload_decoded_texture_chain(d, std::span(&level, 1));
}
bool ge_gpu_backend_copy_last_texture_rgba(std::span<std::byte> out) noexcept {
    auto &v = state().last_texture;
    if (out.size() < v.size() || v.empty())
        return false;
    std::copy(v.begin(), v.end(), out.begin());
    return true;
}
GeGpuWidescreenHud ge_gpu_backend_widescreen_hud(const GeGpuDrawDescriptor &) noexcept {
    return {};
}
void ge_gpu_backend_note_through_extent(const GeGpuDrawDescriptor &, float, float) noexcept {}
void ge_gpu_backend_accumulate_color_triangles(const GeGpuDrawDescriptor &d,
                                               std::span<const GeGpuVertex> vertices) noexcept {
    auto &s = state();
    if (!s.enabled || vertices.empty())
        return;
    try {
        Batch b;
        b.draw = d;
        b.first = s.vertices.size();
        b.count = vertices.size();
        for (auto v : vertices) {
            if (d.through && d.texture_enabled && d.texture_width && d.texture_height) {
                v.u /= d.texture_width;
                v.v /= d.texture_height;
            }
            s.vertices.push_back(v);
        }
        s.batches.push_back(b);
        ++s.report.game_draw_calls;
        s.report.game_triangles += vertices.size() / 3;
        s.report.game_vertices += vertices.size();
    } catch (const std::exception &e) {
        fail("accumulate triangles", e);
    }
}
namespace {
Batch hardware_batch(const GeGpuDrawDescriptor &d, const GeGpuHardwareTransform &h,
                     std::size_t vertex_count, std::span<const std::uint32_t> indices,
                     bool packed) {
    auto &s = state();
    Batch b;
    b.draw = d;
    b.hardware = true;
    b.packed = packed;
    b.transform = h;
    const auto first = packed ? s.packed_vertices.size() / 10 : s.vertices.size();
    const auto count = indices.empty() ? vertex_count : indices.size();
    if (first + vertex_count > std::size_t(std::numeric_limits<std::int32_t>::max()) ||
        count > std::numeric_limits<std::uint32_t>::max() / 3u ||
        s.indices.size() + count * 3 > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("GE geometry exceeds Vulkan draw addressing");
    for (auto index : indices)
        if (index >= vertex_count)
            throw std::runtime_error("Out-of-range GE vertex index");
    b.first = first;
    b.first_index = s.indices.size();
    b.count = count;
    b.indexed = !indices.empty();
    if (h.primitive == 5) {
        // Fans are not available on every MoltenVK device. Expand only the
        // small index stream; preserve the shared vertices and triangle order.
        b.indexed = true;
        auto at = [&](std::size_t i) { return indices.empty() ? std::uint32_t(i) : indices[i]; };
        for (std::size_t i = 2; i < count; ++i) {
            s.indices.push_back(at(0));
            s.indices.push_back(at(i - 1));
            s.indices.push_back(at(i));
        }
        b.count = s.indices.size() - b.first_index;
    } else {
        if (h.primitive == 4)
            b.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        else if (h.primitive != 3)
            throw std::runtime_error("Unsupported GE hardware topology");
        s.indices.insert(s.indices.end(), indices.begin(), indices.end());
    }
    return b;
}
void record_hardware_batch(const Batch &b, std::size_t vertices) {
    auto &s = state();
    s.batches.push_back(b);
    ++s.report.game_draw_calls;
    ++s.report.hw_transform_draw_calls;
    s.report.game_triangles += b.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
        ? (b.count > 2 ? b.count - 2 : 0) : b.count / 3;
    s.report.game_vertices += vertices;
    s.report.hw_transform_vertices += vertices;
    if (b.packed) {
        ++s.packed_draws;
        s.packed_vertex_bytes += vertices * 10;
    }
}
} // namespace
void ge_gpu_backend_accumulate_hardware_triangles(const GeGpuDrawDescriptor &d,
                                                  const GeGpuHardwareTransform &h,
                                                  std::span<const GeGpuVertex> vertices,
                                                  std::span<const std::uint32_t> indices) noexcept {
    auto &s = state();
    if (!s.enabled || vertices.empty())
        return;
    try {
        const auto b = hardware_batch(d, h, vertices.size(), indices, false);
        s.vertices.insert(s.vertices.end(), vertices.begin(), vertices.end());
        record_hardware_batch(b, vertices.size());
    } catch (const std::exception &e) {
        fail("accumulate hardware triangles", e);
    }
}
bool ge_gpu_backend_accumulate_hardware_packed_0115(const GeGpuDrawDescriptor &d,
                                                    const GeGpuHardwareTransform &h,
                                                    std::span<const std::byte> vertices,
                                                    std::uint32_t count,
                                                    std::span<const std::uint32_t> indices) noexcept {
    auto &s = state();
    const auto bytes = std::size_t(count) * 10;
    if (!s.enabled || !s.packed_supported || !count || vertices.size() < bytes)
        return false;
    const auto previous_indices = s.indices.size();
    const auto previous_vertices = s.packed_vertices.size();
    try {
        const auto b = hardware_batch(d, h, count, indices, true);
        s.packed_vertices.insert(s.packed_vertices.end(), vertices.begin(), vertices.begin() + bytes);
        record_hardware_batch(b, count);
        return true;
    } catch (const std::exception &e) {
        s.indices.resize(previous_indices);
        s.packed_vertices.resize(previous_vertices);
        fail("accumulate packed vertices", e);
        return false;
    }
}
bool ge_gpu_backend_raw_model_supported() noexcept { return state().enabled && state().raw_enabled; }
bool ge_gpu_backend_accumulate_raw_model(const GeGpuDrawDescriptor &d,
    const GeGpuHardwareTransform &h,const GeGpuModelState &model,std::span<const std::byte> vertices,
    std::uint32_t count,std::span<const std::uint32_t> indices) noexcept {
    auto &s=state();
    const auto type=model.format[0],stride=model.format[1];
    const auto scalar_size=[](unsigned t) { return t==3 ? 4u : t; };
    const auto fits=[&](unsigned offset,unsigned size) { return offset<=stride && size<=stride-offset; };
    const auto tc=type&3u,color=(type>>2)&7u,normal=(type>>5)&3u,position=(type>>7)&3u,weight=(type>>9)&3u;
    if((type&0x9c0000u) || !position || model.format[3]!=((type>>14)&7u)+1u ||
        !fits(model.offsets[0],scalar_size(tc)*2) ||
        !fits(model.offsets[1],color<4?0u:(color==7?4u:2u)) ||
        !fits(model.offsets[2],scalar_size(normal)*3) ||
        !fits(model.offsets[3],scalar_size(position)*3) ||
        !fits(0,scalar_size(weight)*model.format[3])) return false;
    // Word loads in the shader require f32 and RGBA8888 components, and each
    // subsequent record containing them, to retain their PSP four-byte alignment.
    if((tc==3 && model.offsets[0]%4) || (color==7 && model.offsets[1]%4) ||
        (normal==3 && model.offsets[2]%4) || (position==3 && model.offsets[3]%4) ||
        ((tc==3 || color==7 || normal==3 || position==3 || weight==3) && stride%4)) return false;
    const auto bytes=std::size_t(count)*stride;
    const auto start=(s.raw_vertices.size()+3u)&~std::size_t(3u);
    const auto end=(start+bytes+3u)&~std::size_t(3u);
    if(!ge_gpu_backend_raw_model_supported() || !count || !model.format[1] || vertices.size()<bytes || end>s.max_raw_bytes) return false;
    const auto old_indices=s.indices.size(),old_vertices=s.raw_vertices.size(),old_models=s.models.size();
    try {
        auto b=hardware_batch(d,h,count,indices,false);
        b.raw=true;b.first=0;b.model_index=s.models.size();
        auto snapshot=model;snapshot.format[2]=start;
        s.models.push_back(snapshot);
        s.raw_vertices.resize(end);
        std::memcpy(s.raw_vertices.data()+start,vertices.data(),bytes);
        record_hardware_batch(b,count);
        ++s.report.raw_model_draw_calls;s.report.raw_model_vertices+=count;
        s.frame_raw_vertices+=count;
        return true;
    } catch(const std::exception &e) {
        s.indices.resize(old_indices);s.raw_vertices.resize(old_vertices);s.models.resize(old_models);
        fail("accumulate raw model",e);return false;
    }
}
void ge_gpu_backend_set_native_window(void *) noexcept {}
void ge_gpu_backend_set_display_framebuffer(std::uint32_t address) noexcept {
    state().display = address & address_mask;
}

bool ge_gpu_backend_finish_color_frame(std::uint64_t vblank) noexcept {
    auto &s = state();
    if (!s.enabled)
        return false;
    try {
        if(!s.raw_control_path.empty() && s.epoch%60==0) {
            int enabled=-1;std::ifstream input(s.raw_control_path);input>>enabled;
            if((enabled==0 || enabled==1) && s.raw_enabled!=bool(enabled)) {
                s.raw_enabled=bool(enabled);
                std::cerr<<"[Vulkan raw model] enabled="<<enabled<<" vblank="<<vblank<<'\n';
            }
        }
        bool previous_ready = false;
        // Wait at the following GE boundary, after the CPU has prepared the
        // next frame. Never reset the arena or retire in-flight images sooner.
        if (s.pending_submission) {
            const auto waiting = std::chrono::steady_clock::now();
            check(vkWaitForFences(s.device, 1, &s.fence, VK_TRUE, 10'000'000'000ull), "vkWaitForFences");
            const auto completed = std::chrono::steady_clock::now();
            s.wait_ms += std::chrono::duration<double, std::milli>(completed-waiting).count();
            s.pending_submission = false;
            previous_ready = s.pending_frame;
            if (previous_ready) {
                s.rgba.assign(s.readback.mapped, s.readback.mapped+s.readback.size);
                s.readback_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-completed).count();
                s.owned = s.pending_display;
                s.report.game_frame_readback_bytes = s.rgba.size();
                s.report.presented_framebuffer_target = s.pending_display;
                s.report.game_frame_vblank = s.pending_vblank;
            }
            s.pending_frame = false;
            s.inflight_images.clear();
        }
        if (s.batches.empty()) return previous_ready;
        // sceDisplaySetFrameBuf can select a back buffer after its GE draws
        // were collected. While offscreen, that buffer may have been sampled
        // as a padded 512x512 texture. Neither texture dimensions nor memory
        // stride define the visible screen: restore 480x272 before emitting
        // viewport/scissor/transform state for the newly selected buffer.
        if (auto found = s.targets.find(s.display); found != s.targets.end()) {
            found->second->logical_width = 480;
            found->second->logical_height = 272;
        }
        const auto frame_started = std::chrono::steady_clock::now();
        prune_caches();
        const VkDeviceSize uniform_stride =
            (sizeof(Uniform) + s.uniform_alignment - 1u) & ~(s.uniform_alignment - 1u);
        const VkDeviceSize model_stride=(sizeof(GeGpuModelState)+s.uniform_alignment-1u)&~(s.uniform_alignment-1u);
        const VkDeviceSize geometry_bytes = s.vertices.size() * sizeof(GeGpuVertex) +
            s.packed_vertices.size() + s.indices.size() * sizeof(std::uint32_t) +
            s.batches.size() * uniform_stride + s.models.size()*model_stride + 2*s.uniform_alignment + 48u;
        VkDeviceSize largest_upload = 4;
        for (const auto key : s.pending_texture_keys)
            if (const auto it = s.textures.find(key); it != s.textures.end())
                largest_upload = std::max(largest_upload, VkDeviceSize(it->second.pending.size()));
        reserve_arena(std::max(geometry_bytes, largest_upload));
        reserve_raw_buffer();
        begin_commands();
        for (const auto key : s.pending_texture_keys) {
            ++s.report.pending_texture_entries_checked;
            const auto it = s.textures.find(key);
            if (it == s.textures.end()) continue; // Evicted before its upload.
            auto &t = it->second;
            t.pending_queued = false;
            if (!t.pending.empty()) {
                // Allocate a fresh image on content change: another sampler key may
                // still legitimately refer to the previous shared image.
                auto shared = s.shared_images[t.share_key].lock();
                if (shared && shared->width == t.width && shared->height == t.height && shared->levels == t.levels) {
                    set_texture_image(t, shared);
                    ++s.report.shared_texture_images;
                } else {
                    set_texture_image(t, create_image(t.width, t.height, t.levels));
                    upload(*t.image, t.pending);
                    s.shared_images[t.share_key] = t.image;
                }
                // Even an upload with no draw must survive the submission fence.
                retain_frame_image(t.image);
                clear_pending_texture(t);
                t.pending.shrink_to_fit();
            }
        }
        s.pending_texture_keys.clear();
        if (geometry_bytes > s.arena.size - s.cursor) {
            submit();
            ++s.report.upload_batch_flushes;
            begin_commands();
        }
        // Upload/adoption publishes the immutable content signature once.
        // Unchanged draws need neither a texture lookup nor a weak-map rewrite
        // here; their descriptor lookup below already resolves the same image.
        auto vertex_offset = allocate(s.vertices.size() * sizeof(GeGpuVertex));
        if (!s.vertices.empty()) std::memcpy(s.arena.mapped + vertex_offset, s.vertices.data(),
                    s.vertices.size() * sizeof(GeGpuVertex));
        const auto packed_offset = allocate(s.packed_vertices.size());
        if (!s.packed_vertices.empty())
            std::memcpy(s.arena.mapped + packed_offset, s.packed_vertices.data(), s.packed_vertices.size());
        const auto index_offset = allocate(s.indices.size() * sizeof(std::uint32_t));
        if (!s.indices.empty()) {
            std::memcpy(s.arena.mapped + index_offset, s.indices.data(), s.indices.size() * sizeof(std::uint32_t));
            vkCmdBindIndexBuffer(s.command, s.arena.buffer, index_offset, VK_INDEX_TYPE_UINT32);
        }
        const auto model_offset=allocate(s.models.size()*model_stride,s.uniform_alignment);
        for(std::size_t i=0;i<s.models.size();++i)
            std::memcpy(s.arena.mapped+model_offset+i*model_stride,&s.models[i],sizeof(GeGpuModelState));
        s.report.staged_vertices += s.vertices.size() + s.packed_vertices.size() / 10 + s.frame_raw_vertices;
        s.report.staged_bytes += s.vertices.size() * sizeof(GeGpuVertex) +
            s.packed_vertices.size() + s.raw_vertices.size() + s.indices.size() * sizeof(std::uint32_t);
        for (const auto &b : s.batches) {
            auto &t = target_for(b.draw.framebuffer_address);
            ensure_target(t);
            std::shared_ptr<Image> texture = s.white;
            bool textured = false;
            if (b.draw.texture_enabled && !b.draw.clear_mode) {
                if (is_feedback(b.draw)) {
                    auto &source = target_for(b.draw.texture_address);
                    ensure_target(source);
                    if (&source == &t) {
                        if (!t.snapshot)
                            t.snapshot = create_image(source.color->width, source.color->height);
                        transition(*source.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                        transition(*t.snapshot, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        VkImageCopy copy{};
                        copy.srcSubresource =
                            copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                        copy.extent = {source.color->width, source.color->height, 1};
                        vkCmdCopyImage(s.command, source.color->image, source.color->layout,
                                       t.snapshot->image, t.snapshot->layout, 1, &copy);
                        texture = t.snapshot;
                    } else
                        texture = source.color;
                    ++s.report.vram_feedback_refreshes;
                    textured = true;
                } else {
                    auto *entry=find_texture(texture_key(b.draw));
                    if (entry && entry->image) {
                        texture = entry->image;
                        textured = true;
                        entry->used_epoch = s.epoch;
                    }
                }
            }
            retain_frame_image(texture);
            transition(*texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            begin_target(t);
            auto pipeline = pipeline_for(b);
            auto &bound = s.bound;
            // Count emitted/reused state calls separately from actual draws.
            const auto changed = [&](bool different) {
                const bool emit = !s.cache_commands || different;
                if (emit) ++s.report.command_state_emitted; else ++s.report.command_state_reused;
                return emit;
            };
            if (changed(bound.pipeline != pipeline)) {
                vkCmdBindPipeline(s.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                bound.pipeline = pipeline;
            }
            const auto target_width = t.color->width, target_height = t.color->height;
            VkViewport viewport{0, 0, float(target_width), float(target_height), 0, 1};
            if (changed(!bound.have_viewport || std::memcmp(&bound.viewport, &viewport, sizeof(viewport)) != 0)) {
                vkCmdSetViewport(s.command, 0, 1, &viewport);
                bound.viewport = viewport;
                bound.have_viewport = true;
            }
            auto scale = [](int v, std::uint32_t logical, std::uint32_t physical) {
                return std::int32_t(std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(std::ceil(double(v) * physical / std::max(1u, logical) - 0.5)), 0, physical));
            };
            auto x0 = scale(b.draw.scissor_x0, t.logical_width, target_width),
                 x1 = scale(b.draw.scissor_x1 + 1, t.logical_width, target_width);
            auto y0 = scale(b.draw.scissor_y0, t.logical_height, target_height),
                 y1 = scale(b.draw.scissor_y1 + 1, t.logical_height, target_height);
            if (x1 <= x0 || y1 <= y0)
                continue;
            VkRect2D scissor{{x0, y0}, {std::uint32_t(x1 - x0), std::uint32_t(y1 - y0)}};
            if (changed(!bound.have_scissor || std::memcmp(&bound.scissor, &scissor, sizeof(scissor)) != 0)) {
                vkCmdSetScissor(s.command, 0, 1, &scissor);
                bound.scissor = scissor;
                bound.have_scissor = true;
            }
            auto fix = b.draw.blend_fix_source;
            float constants[] = {float(fix & 255) / 255, float((fix >> 8) & 255) / 255,
                                 float((fix >> 16) & 255) / 255, 1};
            if (changed(!bound.have_blend || std::memcmp(bound.blend.data(), constants, sizeof(constants)) != 0)) {
                vkCmdSetBlendConstants(s.command, constants);
                std::copy_n(constants, 4, bound.blend.begin());
                bound.have_blend = true;
            }
            auto u = uniforms(b, t, textured);
            const bool uniform_changed = !s.cache_commands || !bound.have_uniform ||
                std::memcmp(&bound.uniform, &u, sizeof(u)) != 0;
            if (uniform_changed) {
                const auto offset = allocate(sizeof(u), s.uniform_alignment);
                std::memcpy(s.arena.mapped + offset, &u, sizeof(u));
                bound.uniform = u;
                bound.uniform_offset = static_cast<std::uint32_t>(offset);
                bound.have_uniform = true;
                ++s.report.uniform_uploads;
            }
            std::array<VkDescriptorSet, 2> sets{
                descriptor(*texture, sampler_for(b.draw, is_feedback(b.draw), texture->levels)), s.uniform_set};
            const bool texture_changed = !s.cache_commands || bound.texture != sets[0];
            if (changed(texture_changed || uniform_changed)) {
                // Bind both in one call when both changed; otherwise preserve
                // the unchanged set, including its dynamic uniform offset.
                const unsigned first = texture_changed ? 0 : 1;
                const unsigned count = texture_changed && uniform_changed ? 2 : 1;
                vkCmdBindDescriptorSets(s.command, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipeline_layout,
                    first, count, sets.data() + first, uniform_changed ? 1 : 0,
                    uniform_changed ? &bound.uniform_offset : nullptr);
                bound.texture = sets[0];
            }
            if(b.raw) {
                const auto offset=static_cast<std::uint32_t>(model_offset+b.model_index*model_stride);
                vkCmdBindDescriptorSets(s.command,VK_PIPELINE_BIND_POINT_GRAPHICS,s.pipeline_layout,
                    2,1,&s.model_set,1,&offset);
            }
            const auto offset_in_buffer = b.packed ? packed_offset : vertex_offset;
            if (!b.raw && changed(!bound.have_vertex || bound.vertex_offset != offset_in_buffer)) {
                vkCmdBindVertexBuffers(s.command, 0, 1, &s.arena.buffer, &offset_in_buffer);
                bound.vertex_offset = offset_in_buffer;
                bound.have_vertex = true;
            }
            // A fan with fewer than three vertices has no index stream.
            // Do not issue an indexed draw without a bound index buffer.
            if (b.count) {
                if (b.indexed)
                    vkCmdDrawIndexed(s.command, b.count, 1, b.first_index, std::int32_t(b.first), 0);
                else
                    vkCmdDraw(s.command, b.count, 1, b.first, 0);
            }
            if (b.draw.texture_enabled && !textured)
                ++s.report.missing_texture_draw_calls;
            if (blend_variant(b.draw) == 5)
                ++s.report.unsupported_blend_game_draw_calls;
            if (textured)
                ++s.report.textured_game_draw_calls;
        }
        end_pass();
        report_cache_memory();
        auto found = s.targets.find(s.display);
        bool ready = found != s.targets.end() && found->second->rendered;
        if (ready) {
            auto &image = *found->second->color;
            transition(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {s.width, s.height, 1};
            vkCmdCopyImageToBuffer(s.command, image.image, image.layout, s.readback.buffer, 1,
                                   &copy);
            VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = s.readback.buffer;
            barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(s.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &barrier, 0,
                                 nullptr);
        }
        const auto assembled = std::chrono::steady_clock::now();
        if (s.async_readback) {
            // Uploaded and sampled images were retained once per image while
            // recording. Unused cache entries need no per-frame ownership work.
            s.pending_frame = ready;
            s.pending_vblank = vblank;
            s.pending_display = s.display;
        }
        submit(!s.async_readback);
        const auto completed = std::chrono::steady_clock::now();
        s.assemble_ms += std::chrono::duration<double, std::milli>(assembled-frame_started).count();
        s.wait_ms += std::chrono::duration<double, std::milli>(completed-assembled).count();
        s.vertices.clear();
        s.packed_vertices.clear();
        s.raw_vertices.clear();s.models.clear();s.frame_raw_vertices=0;
        s.indices.clear();
        s.batches.clear();
        ++s.epoch;
        if (ready) {
            if (!s.async_readback) {
                s.rgba.assign(s.readback.mapped, s.readback.mapped + s.readback.size);
                s.readback_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-completed).count();
                s.owned = s.display;
                s.report.game_frame_readback_bytes = s.rgba.size();
                s.report.presented_framebuffer_target = s.display;
                s.report.game_frame_vblank = vblank;
            }
            ++s.report.game_frames;
            if (s.report.game_frames == 1)
                s.perf_started = std::chrono::steady_clock::now();
            if (s.report.game_frames % 120 == 0 && std::getenv("PSPRECOMP_VULKAN_STATS")) {
                auto now = std::chrono::steady_clock::now();
                auto seconds = std::chrono::duration<double>(now - s.perf_started).count();
                std::cerr << "[Vulkan perf] frames=" << s.report.game_frames
                          << " fps=" << 120 / seconds << " draws=" << s.report.game_draw_calls
                          << " hw=" << s.report.hw_transform_draw_calls
                          << " geometry_bytes=" << s.report.staged_bytes
                          << " raw_draws=" << s.report.raw_model_draw_calls
                          << " raw_vertices=" << s.report.raw_model_vertices
                          << " packed_draws=" << s.packed_draws
                          << " packed_vertex_bytes=" << s.packed_vertex_bytes
                          << " feedback=" << s.report.vram_feedback_refreshes
                          << " textures=" << s.textures.size()
                          << " texture_bytes=" << s.report.resident_texture_bytes
                          << " target_bytes=" << s.report.resident_target_bytes
                          << " targets=" << s.targets.size()
                          << " upload_flushes=" << s.report.upload_batch_flushes
                          << " unsupported_blend=" << s.report.unsupported_blend_game_draw_calls
                          << " state_emitted=" << s.report.command_state_emitted << " state_reused=" << s.report.command_state_reused
                          << " uniform_writes=" << s.report.uniform_uploads
                          << " eviction_scans=" << s.report.texture_eviction_entries_scanned
                          << " pending_checks=" << s.report.pending_texture_entries_checked
                          << " retained_images=" << s.report.inflight_image_references
                          << " assemble_ms=" << s.assemble_ms/120 << " wait_ms=" << s.wait_ms/120
                          << " readback_ms=" << s.readback_ms/120
                          << '\n';
                s.assemble_ms = s.wait_ms = s.readback_ms = 0;
                s.perf_started = now;
            }
        }
        return s.async_readback ? previous_ready : ready;
    } catch (const std::exception &e) {
        fail("frame", e);
        s.vertices.clear();
        s.packed_vertices.clear();
        s.raw_vertices.clear();s.models.clear();s.frame_raw_vertices=0;
        s.indices.clear();
        s.batches.clear();
        s.enabled = false;
        s.report.active = GeGpuBackendKind::Software;
        return false;
    }
}
bool ge_gpu_backend_copy_game_frame_rgba(std::span<std::byte> out) noexcept {
    auto &v = state().rgba;
    if (out.size() < v.size() || v.empty())
        return false;
    std::copy(v.begin(), v.end(), out.begin());
    return true;
}
std::span<const std::byte> ge_gpu_backend_game_frame_rgba() noexcept { return state().rgba; }
bool ge_gpu_backend_presents_directly() noexcept { return false; }
std::uint32_t ge_gpu_backend_owned_framebuffer() noexcept { return state().owned; }
std::uint32_t ge_gpu_backend_display_framebuffer() noexcept { return state().display; }
bool ge_gpu_backend_copy_offscreen_rgba(std::span<std::byte>) noexcept { return false; }
void ge_gpu_backend_mark_window_presented() noexcept {
    state().report.gpu_frame_presented_to_window = true;
}
GeGpuBackendReport ge_gpu_backend_report() { return state().report; }
const char *ge_gpu_backend_name(GeGpuBackendKind kind) noexcept {
    switch (kind) {
    case GeGpuBackendKind::Vulkan:
        return "Vulkan";
    case GeGpuBackendKind::DirectX12:
        return "DirectX 12";
    default:
        return "software";
    }
}
} // namespace vcs
