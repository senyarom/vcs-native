#include "swimming_timing.hpp"
#include <bit>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

namespace vcs::patches {
namespace {
struct DragSample {
    unsigned stack{}, ms{}, passes{};
};
std::unordered_map<unsigned, DragSample> drag_samples;

// ProcessBuoyancy applies pow(0.9, timestep) to horizontal velocity at one
// or both of these calls, then passes that damped velocity to UpdateSwimStamina.
// Record only the paths actually taken; shallow water can skip either pass.
void set_move_speed(psprecomp::Runtime &rt, psprecomp::AllegrexContext &c) {
    auto &m = rt.memory();
    if (c.gpr[31] == 0x0891B308u || c.gpr[31] == 0x0891B518u) {
        if (drag_samples.size() > 512)
            drag_samples.clear();
        auto &sample = drag_samples[c.gpr[4]];
        const auto ms = m.load32(c.gpr[28] + 7660);
        if (c.gpr[31] == 0x0891B308u || sample.stack != c.gpr[29] || sample.ms != ms)
            sample = {c.gpr[29], ms, 0};
        ++sample.passes;
    }
    // Exact original 08A666C0..08A666D0 leaf, including its VFPU/GPR clobbers.
    float velocity[4];
    for (unsigned i = 0; i < 4; ++i)
        velocity[i] = std::bit_cast<float>(m.load32(c.gpr[5] + 4 * i));
    c.write_vfpu_vector_ct<0, 4>(velocity);
    c.gpr[4] += 0x140;
    for (unsigned i = 0; i < 4; ++i)
        m.store32(c.gpr[4] + 4 * i, std::bit_cast<unsigned>(velocity[i]));
    c.pc = c.gpr[31];
}

void swim_stamina(psprecomp::Runtime &rt, psprecomp::AllegrexContext &c) {
    auto &m = rt.memory();
    const auto ped = c.gpr[4], gp = c.gpr[28];
    unsigned passes = 0;
    if (auto it = drag_samples.find(ped); it != drag_samples.end()) {
        const auto sample = it->second;
        drag_samples.erase(it); // never reuse another frame's/character's drag
        if (c.gpr[31] == 0x0891B7A8u && sample.stack == c.gpr[29] && sample.ms == m.load32(gp + 7660))
            passes = sample.passes;
    }
    const float step = std::bit_cast<float>(m.load32(gp + 7676));
    const auto flags = m.load32(ped + 0xEC);
    // Preserve the original prologue and keep original regeneration, player
    // queries, unlimited-swimming behavior, clamps and drowning continuation.
    c.gpr[29] -= 16;
    c.gpr[5] = flags;
    m.store32(c.gpr[29] + 4, c.gpr[16]);
    c.gpr[16] = ped;
    c.gpr[4] = (flags & 256) ? 1 : 0;
    m.store32(c.gpr[29], std::bit_cast<unsigned>(c.fpr[20]));
    m.store32(c.gpr[29] + 8, c.gpr[31]);
    c.fpr[20] = 0;
    c.pc = c.gpr[4] ? 0x08912ADC : 0x08912A9C;
    if (!c.gpr[4] || passes == 0 || passes > 2 || !std::isfinite(step) || step <= 0 || step > 10)
        return;
    const auto type = m.load32(ped + 0x550);
    if (type == 0 && (m.load32(0x08BDE4B0) != ped || m.load8(0x08BDE4B0 + 332)))
        return;

    // Only the stamina sample is projected to the original 30 Hz drag step.
    // Actual velocity, buoyancy, input, bullet damage and the global timestep
    // are untouched. Normalizing both applied passes fixes the extra cost at
    // 60+ FPS without undercharging shallow water that took just one pass.
    const float correction = std::pow(0.9f, float(passes) * (50.f / 30.f - step));
    float velocity[4];
    for (unsigned i = 0; i < 4; ++i)
        velocity[i] = std::bit_cast<float>(m.load32(ped + 0x140 + 4 * i));
    c.write_vfpu_vector_ct<0, 4>(velocity);
    c.execute_vfpu_vdot_ct<28, 0, 0, 2>();
    float squared[4]{}, magnitude[4]{};
    c.read_vfpu_vector_with_source_prefix_ct<28, 1, 0>(squared);
    magnitude[0] = std::fabs(std::sqrt(squared[0]));
    c.write_vfpu_vector_with_destination_prefix_ct<28, 1>(magnitude);
    c.fpr[12] = std::bit_cast<float>(c.vfpu_scalar_bits_ct<28>()) * correction;
    c.fpr[13] = 0.005f;
    c.fpr[12] *= c.fpr[13];
    c.fpr[14] = std::bit_cast<float>(m.load32(gp + 7760)) * c.fpr[20];
    c.fpr[15] = 0.00005f;
    c.fpr[12] = (c.fpr[15] + c.fpr[12]) + c.fpr[14];
    c.gpr[4] = type;
    c.gpr[5] = 6;
    if (type == 6) {
        c.gpr[4] = 0x40400000;
        c.fpr[12] *= 3.f;
    }
    c.fpr[13] = step;
    c.pc = 0x08912B6C; // original dt multiplication, subtraction, clamps, death
}
} // namespace
void install_swimming_timing(psprecomp::Runtime &rt) {
    drag_samples.clear();
    if (std::getenv("PSPRECOMP_DISABLE_SWIM_STAMINA_FIX"))
        return;
    rt.register_function(0x08A666C0, set_move_speed, "vcs_swim_drag_sample");
    rt.register_function(0x08912A70, swim_stamina, "vcs_swim_stamina_timing");
}
} // namespace vcs::patches
