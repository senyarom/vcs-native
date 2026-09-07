#include "psprecomp/runtime.hpp"
#include "generated_units.hpp"

#include <cstdint>
#include <cstdio>

// Leave a recognizable pattern in dead stack storage before entering the large
// AOT function. This is legal C++; the faulty Apple Clang backend subsequently
// reads one of its own spill slots before writing it (Apple Clang 17 on ARM64).
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void mark_stack() {
    volatile std::uint64_t words[1024];
    for (auto& word : words) {
        word = UINT64_C(0x40f5a25f40f5a25f);
    }
}

int main() {
    using namespace psprecomp;
    Runtime rt;
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        AllegrexContext ctx{};
        // A legal dispatcher resume point after the foreign call at 08A415F8.
        // Guest RAM and registers are initialized without loading a game image.
        ctx.pc = 0x08A41600u;
        ctx.gpr[28] = 0x08808000u;
        ctx.gpr[29] = 0x09ff0000u;
        ctx.gpr[17] = ctx.gpr[4] = 0x08900000u;
        ctx.gpr[16] = ctx.gpr[5] = 0x08910000u;
        ctx.gpr[6] = 0x08920000u;
        ctx.gpr[31] = 0xdead0000u;
        rt.memory().store32(ctx.gpr[28] - 6352u, 1234u);
        rt.memory().store32(ctx.gpr[28] - 6348u, 5678u);
        auto memory = rt.memory().aot_fast_view();
        mark_stack();
        recomp_unit_0143_entry(rt, ctx, 0, memory);
        // Chaining is disabled by CTest. The next call must yield to the
        // dispatcher with this destination and argument, never hit a stub.
        if (ctx.pc != 0x0890043cu || ctx.gpr[4] != 0x46u) {
            std::fprintf(stderr, "AOT resume %u: pc=%08x r4=%08x\n",
                iteration, ctx.pc, ctx.gpr[4]);
            return 1;
        }
    }
    std::puts("AOT resume at 0x08A41600 passed 100 iterations");
}
