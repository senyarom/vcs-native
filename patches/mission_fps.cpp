// Mission patterns and 60 FPS corrections adapted from ThirteenAG (MIT).
// See README.md and third_party/ThirteenAG-LICENSE.txt for source and scope.
#include "mission_fps.hpp"
#include <array>
#include <bit>
#include <optional>
#include <span>

namespace vcs::patches {
namespace {
using Memory = psprecomp::GuestMemory;
constexpr int any = -1;
constexpr std::array<std::array<int, 9>, 7> float_patterns{{
    {0x08,0x00,any,any,any,0x8f,0xc2,0x75,0x3c}, // Boomshine Blowout
    {0x0a,0x00,any,any,any,0x8f,0xc2,0x75,0x3d}, // The Exchange
    {0x0a,0x00,any,any,any,0x0a,0xd7,0x23,0x3d}, // Hose the Hoes
    {0x0a,0x00,any,any,any,0x8f,0xc2,0xf5,0x3c},
    {0x0a,0x00,any,any,any,0x0a,0xd7,0x23,0x3c},
    {0x08,0x00,any,any,any,0x0a,0xd7,0xa3,0x3b}, // Balls / Farewell to Arms
    {0x08,0x00,any,any,any,0x09,0x04,0x40,0x3f}, // In the Air Tonight
}};
// Fail closed on ambiguity instead of changing an arbitrary matching script.
std::optional<std::uint32_t> unique_match(const Memory &memory, std::uint32_t base,
                                         std::uint32_t size, std::span<const int> pattern) {
    if (size < pattern.size()) return {};
    std::optional<std::uint32_t> result;
    for (std::uint32_t offset = 0; offset <= size - pattern.size(); ++offset) {
        bool match = true;
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            if (pattern[i] != any && memory.load8(base + offset + i) != pattern[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            if (result) return {};
            result = base + offset;
        }
    }
    return result;
}
std::uint32_t read(const Memory &m, std::uint32_t addr, unsigned width) {
    return width == 1 ? m.load8(addr) : width == 2 ? m.load16(addr) : m.load32(addr);
}
void write(Memory &m, std::uint32_t addr, unsigned width, std::uint32_t value) {
    if (width == 1) m.store8(addr, static_cast<std::uint8_t>(value));
    else if (width == 2) m.store16(addr, static_cast<std::uint16_t>(value));
    else m.store32(addr, value);
}
} // namespace

void MissionFpsPatches::load(Memory &memory, std::uint32_t address, std::uint32_t size) {
    // The loader has already overwritten the old mission. Never restore its
    // cached bytes into the newly loaded script, even when its address is reused.
    *this = {};
    constexpr std::uint32_t maximum_mission_size = 1024u * 1024u;
    if (!size || size > maximum_mission_size || !memory.contains(address, size)) return;
    const auto add = [&](std::uint32_t addr, std::uint8_t width, std::uint32_t replacement) {
        const auto original = read(memory, addr, width);
        edits_.push_back({addr, original, replacement, original, width, true});
    };
    for (std::size_t i = 0; i < float_patterns.size(); ++i) {
        if (const auto addr = unique_match(memory, address, size, float_patterns[i])) {
            const auto value = std::bit_cast<float>(memory.load32(*addr + 5));
            add(*addr + 5, 4, std::bit_cast<std::uint32_t>(value * 0.5f));
            if (i == 6) concert_ = true;
        }
    }

}

std::uint32_t MissionFpsPatches::effective_fps(std::uint32_t requested) const noexcept {
    if (requested == 30 || concert_30_) return 30;
    // These are fixed-60 workarounds, not general timestep fixes. Keep their
    // coefficients honest when the user selects uncapped / 120 / 200 / 240.
    if (!edits_.empty() && (requested == 0 || requested > 60)) return 60;
    return requested;
}

void MissionFpsPatches::apply(Memory &memory, std::uint32_t requested) {
    // Keep the original upstream 60-FPS script corrections during its concert
    // fallback too; the upstream fallback changes only the executable limiter.
    const bool enabled = requested != 30;
    for (auto &edit : edits_) {
        if (!edit.valid) continue;
        if (!memory.contains(edit.address, edit.width) ||
            read(memory, edit.address, edit.width) != edit.written) {
            edit.valid = false; // Another writer owns these bytes now.
            continue;
        }
        const auto next = enabled ? edit.replacement : edit.original;
        if (next != edit.written) write(memory, edit.address, edit.width, next);
        edit.written = next;
    }
}

void MissionFpsPatches::update(Memory &memory, std::uint32_t requested,
                              bool running, bool on_mission, bool text_present) {
    if (running) running_seen_ = true;
    if (running_seen_ && !running) {
        apply(memory, 30); // Restore before releasing ownership, not after reuse.
        *this = {};
        return;
    }
    concert_30_ = concert_ && on_mission && text_present && requested != 30;
    apply(memory, requested);
}
} // namespace vcs::patches
