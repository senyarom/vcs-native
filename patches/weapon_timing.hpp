#pragma once
#include "psprecomp/runtime.hpp"
#include <cstdint>
#include <unordered_map>

namespace vcs::patches {
// The two simplified NPC shooting paths emit once per frame during a burst.
// Preserve the original 30 Hz cadence in simulation time, before ammo, traces
// or damage are produced. Other weapon callers (including the player) bypass it.
class NpcFireCadence {
public:
    void advance(double seconds);
    bool allow(std::uint64_t owner_and_weapon);
    void reset();
    double seconds() const noexcept { return seconds_; }
private:
    struct Shot { double next{}, last_attempt{-1}; };
    double seconds_{};
    std::unordered_map<std::uint64_t, Shot> shots_;
};
void install_weapon_timing(psprecomp::Runtime &);
void advance_weapon_timing(psprecomp::Runtime &, std::uint32_t gp);
} // namespace vcs::patches
