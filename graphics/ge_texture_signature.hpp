#pragma once

#define XXH_INLINE_ALL
#include "../third_party/xxhash/xxhash.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace vcs::detail {

// Preserve the existing palette checksum, including its two-byte tail.
inline std::uint32_t palette_checksum(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t hash = 2166136261u;
    std::size_t i = 0;
    for (; i + 4 <= bytes.size(); i += 4) {
        const auto word = std::uint32_t(bytes[i]) | (std::uint32_t(bytes[i + 1]) << 8) |
            (std::uint32_t(bytes[i + 2]) << 16) | (std::uint32_t(bytes[i + 3]) << 24);
        hash = (hash ^ word) * 16777619u;
    }
    if (i < bytes.size()) {
        std::uint32_t word = 0;
        for (std::size_t j = 0; j < bytes.size() - i; ++j)
            word |= std::uint32_t(bytes[i + j]) << (8 * j);
        hash = (hash ^ word) * 16777619u;
    }
    return hash;
}

class PaletteChecksumCache {
    struct Entry {
        std::uint32_t address{}, size{}, checksum{};
        std::array<std::uint8_t, 1024> bytes;
    };
    // Palettes from different textures frequently collide in an address-only
    // direct-mapped cache. Keep four alternatives so alternating materials do
    // not repeatedly run the serial legacy checksum over the same palettes.
    std::array<std::array<Entry, 4>, 256> entries_{};
    std::array<std::uint8_t, 256> victims_{};
public:
    std::uint32_t get(std::uint32_t address, std::span<const std::uint8_t> bytes) noexcept {
        if (bytes.empty() || bytes.size() > 1024) return palette_checksum(bytes);
        address &= 0x1fffffffu;
        const auto bucket = ((address >> 4) * 2654435761u) >> 24;
        auto &ways = entries_[bucket];
        Entry *selected = nullptr;
        for (auto &way : ways) {
            if (way.address == address) { selected = &way; break; }
        }
        if (!selected) {
            selected = &ways[victims_[bucket]];
            victims_[bucket] = (victims_[bucket] + 1) & 3u;
        }
        auto &entry = *selected;
        // Compare every relevant byte on every draw. No assumption about guest
        // writes, GE revisions, RAM addresses or palette immutability is needed.
        if (entry.address == address && entry.size == bytes.size() &&
            std::memcmp(entry.bytes.data(), bytes.data(), bytes.size()) == 0)
            return entry.checksum;
        entry.address = address;
        entry.size = static_cast<std::uint32_t>(bytes.size());
        entry.checksum = palette_checksum(bytes);
        std::memcpy(entry.bytes.data(), bytes.data(), bytes.size());
        return entry.checksum;
    }
};

// Process-local GPU cache validation, unrelated to the HD manifest's hashes.
// Keep the existing coverage: all bytes <= 4 KiB, sixteen distributed 64-byte
// windows otherwise. xxHash processes independent lanes instead of a serial
// multiply/shift chain for every eight bytes. Gather only those same windows.
inline std::uint64_t texture_validation_hash(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() <= 4096) return XXH3_64bits(bytes.data(), bytes.size());
    std::array<std::uint8_t, 16 * 64> sampled;
    std::size_t written = 0;
    for (std::size_t block = 0; block < 16; ++block) {
        const auto center = (bytes.size() - 1) * block / 15;
        const auto begin = center > 32 ? center - 32 : 0;
        const auto count = std::min<std::size_t>(64, bytes.size() - begin);
        std::memcpy(sampled.data() + written, bytes.data() + begin, count);
        written += count;
    }
    return XXH3_64bits(sampled.data(), written);
}

} // namespace vcs::detail
