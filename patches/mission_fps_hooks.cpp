#include "mission_fps_hooks.hpp"
#include "mission_fps.hpp"
#include <array>
#include <iostream>
#include <limits>

namespace vcs::patches {
namespace {
MissionFpsPatches patches;
FrameRateProvider frame_rate{};
std::uint32_t loaded_script_space{}, loaded_main_size{};
constexpr std::uint32_t start_script = 0x08863470;
constexpr std::uint32_t mission_loader_return = 0x08ABC138;

void frame_limiter_hook(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    // Preserve the original delay-slot load. This resume PC follows a PSP
    // import, so unlike an internal label it always crosses runtime dispatch.
    ctx.set_gpr(4, rt.memory().load32(ctx.gpr[28] - 8852));
    ctx.pc = mission_frame_rate(frame_rate()) == 30 ? 0x08A070A8 : 0x08A070D0;
}

bool valid_gp(const psprecomp::GuestMemory &memory, std::uint32_t gp) {
    return gp >= 29148u && memory.contains(gp - 29148u, 37176u);
}
void start_script_hook(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    auto &m = rt.memory();
    const auto gp = ctx.gpr[28];
    if (ctx.gpr[31] == mission_loader_return && valid_gp(m, gp)) {
        loaded_script_space = m.load32(gp - 29148);
        loaded_main_size = m.load32(gp + 8004);
        const std::uint64_t base = static_cast<std::uint64_t>(loaded_script_space) + loaded_main_size;
        // s0 still contains the exact byte count passed to the completed read.
        // Scanning LargestMissionScriptSize would also scan stale tail bytes.
        const auto size = ctx.gpr[16];
        const auto maximum = m.load32(gp + 8016);
        patches.load(m, base <= std::numeric_limits<std::uint32_t>::max() ? static_cast<std::uint32_t>(base) : 0,
                     size <= maximum ? size : 0);
        patches.apply(m, frame_rate());
        std::cerr << "[patches:mission-fps] loaded bytes=" << size
                  << " edits=" << patches.patch_count()
                  << " requested=" << frame_rate() << " effective=" << mission_frame_rate(frame_rate()) << '\n';
    }
    // Original 08863470..0886348c prologue and local call. A runtime override of
    // 08ABC180 would be bypassed by local AOT gotos; this real cross-unit callee
    // is reached after the read and before the mission executes its first opcode.
    ctx.gpr[29] -= 16;
    m.store32(ctx.gpr[29], ctx.gpr[16]);
    m.store32(ctx.gpr[29] + 4, ctx.gpr[17]);
    ctx.gpr[17] = m.load32(gp + 7960);
    ctx.gpr[16] = ctx.gpr[4];
    ctx.gpr[5] = gp + 7960;
    m.store32(ctx.gpr[29] + 8, ctx.gpr[31]);
    ctx.gpr[31] = 0x08863494;
    ctx.gpr[4] = ctx.gpr[17];
    ctx.pc = 0x088626C0;
}

struct TextResult { bool found{}; bool nonempty{}; };
TextResult find_text(const psprecomp::GuestMemory &m, std::uint32_t table) {
    if (!m.contains(table, 8)) return {};
    const auto entries = m.load32(table), count = m.load32(table + 4);
    if (!count || count > 32767 || !m.contains(entries, static_cast<std::size_t>(count) * 12)) return {};
    constexpr std::array<std::uint8_t, 8> key{'R','E','N','7','_','O','9',0};
    std::uint32_t lo = 0, hi = count;
    while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2;
        const auto entry = entries + mid * 12;
        int comparison = 0;
        for (unsigned i = 0; i < key.size(); ++i) {
            comparison = int(key[i]) - int(m.load8(entry + 4 + i));
            if (comparison || !key[i]) break;
        }
        if (comparison < 0) hi = mid;
        else if (comparison > 0) lo = mid + 1;
        else {
            const auto value = m.load32(entry);
            return {true, m.contains(value, 2) && m.load16(value) != 0};
        }
    }
    return {};
}
} // namespace

bool concert_text_present(const psprecomp::GuestMemory &m, std::uint32_t gp) {
    if (!valid_gp(m, gp)) return false;
    const auto text = m.load32(gp - 9752);
    if (!m.contains(text, 35)) return false;
    const auto main = find_text(m, text);
    if (main.found) return main.nonempty;
    return m.load8(text + 33) && m.load8(text + 34) && find_text(m, text + 16).nonempty;
}

void install_mission_fps_patches(psprecomp::Runtime &rt, FrameRateProvider provider) {
    patches = {};
    loaded_script_space = loaded_main_size = 0;
    frame_rate = provider;
    rt.register_function(start_script, &start_script_hook, "vcs_mission_fps_start_script");
    rt.register_function(0x08A070C8, &frame_limiter_hook, "vcs_mission_fps_frame_limiter");
}
std::uint32_t mission_frame_rate(std::uint32_t requested) noexcept { return patches.effective_fps(requested); }
void update_mission_fps_patches(psprecomp::Runtime &rt, std::uint32_t gp) {
    if (!frame_rate || !patches.patch_count() || !valid_gp(rt.memory(), gp)) return;
    auto &m = rt.memory();
    const auto script = m.load32(gp - 29148), flag = m.load32(gp + 7968);
    if (script != loaded_script_space || m.load32(gp + 8004) != loaded_main_size) {
        // A new game/save can replace the entire script allocation. Never write
        // cached originals into its former address, even if those bytes survive.
        patches = {};
        return;
    }
    const std::uint64_t mission_flag = static_cast<std::uint64_t>(script) + flag;
    const bool on_mission = flag && mission_flag <= std::numeric_limits<std::uint32_t>::max() &&
        m.contains(static_cast<std::uint32_t>(mission_flag), 4) && m.load32(static_cast<std::uint32_t>(mission_flag)) == 1;
    const auto before = mission_frame_rate(frame_rate());
    patches.update(m, frame_rate(), m.load8(gp + 8024) != 0, on_mission,
                   patches.concert() && concert_text_present(m, gp));
    const auto after = mission_frame_rate(frame_rate());
    if (before != after) std::cerr << "[patches:mission-fps] effective=" << after
                                  << " requested=" << frame_rate() << '\n';
}
} // namespace vcs::patches
