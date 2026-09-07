#include "vcs_world_streaming.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vcs {
namespace {
constexpr std::uint32_t kWorld = 0x08E91200u;
constexpr std::uint32_t kCameraPosition = 0x08BC7E30u + 0x9B0u;
constexpr std::uint32_t kMetadataSize = 96u;
float number(const psprecomp::GuestMemory &mem, std::uint32_t address) {
    return std::bit_cast<float>(mem.load32(address));
}

void update_sector(psprecomp::GuestMemory &mem, std::uint32_t record, float lod,
                   const std::array<float, 3> &camera) {
    if (!mem.contains(record, 36u) || mem.load32(record + 8u) == 0u) return;
    const auto root = mem.load32(record + 32u);
    if (!mem.contains(root, 52u)) return;
    const auto overlays = mem.load32(root);
    if (overlays < kMetadataSize) return;
    const auto meta = overlays - kMetadataSize;
    if (!mem.contains(meta, kMetadataSize) || mem.load32(meta) != 0x42534356u ||
        mem.load32(meta + 4u) != 0x314C4C49u) return; // "VCSBILL1"
    const auto count = mem.load32(meta + 80u);
    const auto table = mem.load32(meta + 84u);
    const float base_range = number(mem, meta + 88u);
    const float max_lod = number(mem, meta + 92u);
    if (count == 0u || count > 10u || !mem.contains(table, count * 20u) ||
        base_range != 160.0f || max_lod != 3.0f) return;
    std::array<std::uint32_t, 18> passes{};
    for (unsigned i = 0; i < passes.size(); ++i) {
        passes[i] = mem.load32(meta + 8u + i * 4u);
        if (!mem.contains(passes[i], 0u) || passes[i] < root) return;
        if (i % 9u != 0u && (passes[i] < passes[i-1u] ||
                             (passes[i] - passes[i-1u]) % 68u != 0u)) return;
    }
    // Validate all descriptors before changing any live list or record.
    for (unsigned i = 0; i < count; ++i) {
        const auto item = table + i * 20u;
        const auto instance = mem.load32(item);
        const auto resource = mem.load32(item + 4u);
        if (instance < passes[9] || instance >= passes[17] ||
            (instance - passes[9]) % 68u != 0u || !mem.contains(instance, 68u) ||
            resource < 3379u || resource > 3385u) return;
    }
    const bool extended = lod > 1.0f;
    const bool was_extended = mem.load32(root + 8u) == passes[9];
    for (unsigned pass = 0; pass < 9u; ++pass)
        if (mem.load32(root + 8u + pass * 4u) != passes[pass + (was_extended ? 9u : 0u)]) return;
    if (was_extended != extended) {
        // The guest fixes visibility/resource flags in the loaded records (for
        // example, bit 15 of the instance ID). The offline copy still contains
        // disk flags. Carry live state across a list switch in either direction;
        // otherwise activating it hides already-resolved coarse LOD buildings.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> copies;
        for (unsigned pass = 0; pass < 8u; ++pass) {
            auto extra = passes[9u + pass];
            for (auto original = passes[pass]; original < passes[pass + 1u]; original += 68u) {
                const auto id = mem.load16(original) & 0x7FFFu;
                while (extra < passes[10u + pass] && (mem.load16(extra) & 0x7FFFu) < id) extra += 68u;
                if (extra >= passes[10u + pass] || (mem.load16(extra) & 0x7FFFu) != id) return;
                copies.emplace_back(extended ? original : extra, extended ? extra : original);
                extra += 68u;
            }
        }
        for (const auto &[source, destination] : copies)
            for (unsigned offset = 0; offset < 68u; offset += 4u)
                mem.store32(destination + offset, mem.load32(source + offset));
    }
    const float range = base_range * std::clamp(lod, 1.0f, max_lod);
    for (unsigned i = 0; i < count; ++i) {
        const auto item = table + i * 20u;
        float distance_squared = 0.0f;
        for (unsigned axis = 0; axis < 3u; ++axis) {
            const float delta = camera[axis] - number(mem, item + 8u + axis * 4u);
            distance_squared += delta * delta;
        }
        // Resource zero is the original guest renderer's empty instance. Only
        // added render copies are changed; resource ownership stays intact. Switching to 1x restores their exact original pointers.
        const auto resource = extended && distance_squared <= range * range
            ? mem.load32(item + 4u) : 0u;
        mem.store16(mem.load32(item) + 2u, static_cast<std::uint16_t>(resource));
    }
    for (unsigned pass = 0; pass < 9u; ++pass)
        mem.store32(root + 8u + pass * 4u, passes[pass + (extended ? 9u : 0u)]);
}
} // namespace

void update_world_streaming(psprecomp::GuestMemory &memory, float lod) {
    if (!std::isfinite(lod) || !memory.contains(kWorld, 612u) ||
        !memory.contains(kCameraPosition, 12u)) return;
    std::array<float, 3> camera{};
    for (unsigned axis = 0; axis < 3u; ++axis) {
        camera[axis] = number(memory, kCameraPosition + axis * 4u);
        if (!std::isfinite(camera[axis])) return;
    }
    update_sector(memory, memory.load32(kWorld + 580u), lod, camera);
    // The old stream buffer may already have been released once the fade ends.
    if (memory.load32(kWorld + 608u) != 0u)
        update_sector(memory, memory.load32(kWorld + 588u), lod, camera);
}
} // namespace vcs
