#pragma once

#include "psprecomp/guest_memory.hpp"
#include <cstdint>
#include <vector>

namespace vcs::patches {

// ULUS10160 1.03 mission bytecode only. No writes to MAIN.SCM on disk.
class MissionFpsPatches {
public:
    void load(psprecomp::GuestMemory &memory, std::uint32_t address, std::uint32_t size);
    void update(psprecomp::GuestMemory &memory, std::uint32_t requested_fps,
                bool mission_running, bool on_mission, bool concert_text_present);
    void apply(psprecomp::GuestMemory &memory, std::uint32_t requested_fps);
    [[nodiscard]] std::uint32_t effective_fps(std::uint32_t requested_fps) const noexcept;
    [[nodiscard]] std::size_t patch_count() const noexcept { return edits_.size(); }
    [[nodiscard]] bool concert() const noexcept { return concert_; }

private:
    struct Edit {
        std::uint32_t address{}, original{}, replacement{}, written{};
        std::uint8_t width{};
        bool valid{true};
    };
    std::vector<Edit> edits_;
    bool concert_{}, concert_30_{}, running_seen_{};
};

} // namespace vcs::patches
