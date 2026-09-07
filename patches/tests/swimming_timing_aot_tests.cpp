#include "swimming_timing.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace psprecomp {
void register_generated_unit_67(Runtime &);
}
namespace {
constexpr unsigned gp = 0x08BB1D60, ped = 0x09000000, info = 0x09100000, stack = 0x09FE0000,
                   done = 0x08000000;
void require(bool ok, const char *msg) {
    if (!ok)
        throw std::runtime_error(msg);
}
float simulate(int fps, bool water, int ped_type, float speed, bool unlimited = false, bool fixed = false) {
    psprecomp::Runtime rt;
    psprecomp::register_generated_unit_67(rt);
    // Only the two identity queries are mocked; all consumption, regeneration,
    // VFPU velocity magnitude, timestep scaling and clamps execute original AOT.
    for (unsigned address : {0x0890D44Cu, 0x089474C4u})
        rt.register_function(
            address, [](auto &, auto &) { throw std::runtime_error("query boundary"); }, "swim-query");
    if (fixed)
        vcs::patches::install_swimming_timing(rt);
    auto &m = rt.memory();
    auto put = [&](unsigned a, float f) { m.store32(a, std::bit_cast<unsigned>(f)); };
    m.store32(ped + 0xEC, water ? 256 : 0);
    m.store32(ped + 0x550, ped_type);
    m.store8(info + 332, unlimited);
    put(ped + 0x82C, water ? 1.f : .1f);
    put(ped + 0x140, speed * .6f);
    put(ped + 0x144, speed * .8f);
    put(gp + 7676, 50.f / fps);
    for (int i = 0; i < fps * 10; ++i) {
        psprecomp::AllegrexContext c{};
        c.vfpu_ctrl[0] = c.vfpu_ctrl[1] = 0xE4;
        c.gpr[4] = ped;
        c.gpr[28] = gp;
        c.gpr[29] = stack;
        c.gpr[31] = done;
        c.pc = 0x08912A70;
        for (int budget = 0; c.pc != done; ++budget) {
            require(budget < 20, "swim AOT failed to return");
            if (c.pc == 0x0890D44C || c.pc == 0x089474C4) {
                c.gpr[2] = c.pc == 0x0890D44C ? (ped_type == 0) : info;
                c.pc = c.gpr[31];
            } else
                require(rt.invoke_isolated_aot(c.pc, c), "missing swim AOT block");
        }
    }
    return std::bit_cast<float>(m.load32(ped + 0x82C));
}
} // namespace
void test_swimming_timing() {
    for (int ped_type : {0, 6})
        for (float speed : {0.f, .1f, .3f}) {
            float reference = simulate(30, true, ped_type, speed);
            const float expected =
                std::max(.001f, 1.f - 500.f * (.00005f + .005f * speed) * (ped_type == 6 ? 3.f : 1.f));
            require(std::fabs(reference - expected) < .0001f, "30 FPS stamina differs from original formula");
            for (int fps : {30, 60, 120, 240}) {
                float actual = simulate(fps, true, ped_type, speed);
                require(simulate(fps, true, ped_type, speed, false, true) == actual,
                        "without a matching water-drag sample the hook must preserve original stamina");
                require((expected == .001f ? (actual >= 0.f && actual <= .001f)
                                           : std::fabs(actual - reference) < .00015f),
                        "swimming stamina depends on FPS at fixed speed");
                std::cout << "swim fps=" << fps << " ped_type=" << ped_type << " speed=" << speed
                          << " stamina=" << actual << '\n';
            }
        }
    for (int fps : {30, 60, 120, 240}) {
        require(std::fabs(simulate(fps, false, 1, 0, false, true) - 1.f) < 1.e-6f,
                "regeneration must clamp to full");
        require(simulate(fps, true, 0, .3f, true, true) == 1.f, "unlimited swimming must preserve stamina");
    }
}
