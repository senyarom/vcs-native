#include "swimming_timing.hpp"
#include <bit>
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace psprecomp {
void register_generated_unit_67(Runtime &);
void register_generated_unit_69(Runtime &);
void register_generated_unit_152(Runtime &);
void recomp_unit_0152(Runtime &, AllegrexContext &);
} // namespace psprecomp
namespace {
constexpr unsigned gp = 0x08BB1D60, ped = 0x09000000, stack = 0x09FE0000, player_info = 0x08BDE4B0;
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
float run(int fps, int passes, bool fixed, bool unlimited = false, bool second_only = false,
          bool stale = false) {
    psprecomp::Runtime rt;
    psprecomp::register_generated_unit_67(rt);
    psprecomp::register_generated_unit_69(rt);
    psprecomp::register_generated_unit_152(rt);
    for (unsigned address : {0x0890D44Cu, 0x089474C4u, 0x08A666C0u})
        rt.register_function(
            address, [](auto &, auto &) { throw std::runtime_error("unexpected helper execution"); },
            "swim-test-boundary");
    if (fixed)
        vcs::patches::install_swimming_timing(rt);
    auto &m = rt.memory();
    auto put = [&](unsigned address, float value) { m.store32(address, std::bit_cast<unsigned>(value)); };
    auto get = [&](unsigned address) { return std::bit_cast<float>(m.load32(address)); };
    m.store32(player_info, ped);
    m.store8(player_info + 332, unlimited);
    m.store32(ped + 0xEC, 256);
    m.store32(ped + 0x550, 0);
    put(ped + 0x82C, 1);
    put(ped + 0x4E4, 100);
    put(gp + 7676, 50.f / fps);
    for (int frame = 0; frame < fps * 10; ++frame) {
        m.store32(gp + 7660, frame * 1000 / fps);
        put(ped + 0x140, .06f);
        put(ped + 0x144, .08f);
        put(ped + 0x148, 0);
        put(ped + 0x14C, 0);
        psprecomp::AllegrexContext c{};
        c.vfpu_ctrl[0] = c.vfpu_ctrl[1] = 0xE4;
        c.gpr[16] = ped;
        c.gpr[28] = gp;
        c.gpr[29] = stack;
        for (int pass = 0; pass < passes; ++pass) {
            // Execute the actual original water-drag calculation and cross-unit call.
            c.pc = second_only ? 0x0891B368 : (pass == 0 ? 0x0891B2B0 : 0x0891B39C);
            require(rt.invoke_isolated_aot(c.pc, c), "water-drag AOT block exists");
            require(c.pc == 0x08A666C0, "actual water-drag call reaches setter override");
            if (fixed)
                require(rt.invoke_isolated_aot(c.pc, c), "record actual drag pass");
            else
                psprecomp::recomp_unit_0152(rt, c); // unchanged original setter leaf
            require(c.pc == (!second_only && pass == 0 ? 0x0891B308u : 0x0891B518u),
                    "setter preserves original return");
        }
        const float physical_x = get(ped + 0x140), physical_y = get(ped + 0x144);
        const float expected_drag = std::pow(.9f, passes * 50.f / fps);
        require(std::fabs(physical_x - .06f * expected_drag) < .00001f &&
                    std::fabs(physical_y - .08f * expected_drag) < .00001f,
                "physical drag must stay unchanged");
        if (stale)
            m.store32(gp + 7660, m.load32(gp + 7660) + 1);
        c.gpr[4] = ped;
        c.gpr[31] = 0x0891B7A8;
        c.pc = 0x08912A70;
        for (int budget = 0; c.pc != 0x0891B7A8; ++budget) {
            require(budget < 20, "stamina AOT returns");
            if (c.pc == 0x0890D44C || c.pc == 0x089474C4) {
                c.gpr[2] = c.pc == 0x0890D44C ? 1 : player_info;
                c.pc = c.gpr[31];
            } else
                require(rt.invoke_isolated_aot(c.pc, c), "stamina AOT continuation exists");
        }
        require(c.gpr[29] == stack && c.gpr[16] == ped, "stamina preserves caller stack and saved register");
        require(get(ped + 0x140) == physical_x && get(ped + 0x144) == physical_y,
                "stamina correction cannot change movement");
        require(get(ped + 0x4E4) == 100, "stamina correction cannot change bullet damage");
    }
    return get(ped + 0x82C);
}
} // namespace
void test_swimming_drag() {
    for (bool second_only : {false, true})
        for (int passes : {1, 2}) {
            if (second_only && passes == 2)
                continue;
            const float baseline = run(30, passes, false, false, second_only);
            require(run(60, passes, false, false, second_only) < baseline - .01f,
                    "original drag/stamina coupling reproduces FPS bug");
            for (int fps : {30, 60, 120, 240}) {
                const float actual = run(fps, passes, true, false, second_only);
                require(std::fabs(actual - baseline) < .00015f,
                        "corrected stamina matches original 30 FPS after actual drag");
                require(run(fps, passes, true, true, second_only) == 1,
                        "unlimited-swimming behavior preserved");
                require(run(fps, passes, true, false, second_only, true) ==
                            run(fps, passes, false, false, second_only),
                        "stale drag samples must not change stamina");
                std::cout << "drag second_only=" << second_only << " passes=" << passes << " fps=" << fps
                          << " original30=" << baseline << " corrected=" << actual << '\n';
            }
        }
}
