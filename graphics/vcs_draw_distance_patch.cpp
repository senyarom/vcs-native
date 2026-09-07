// GTA Vice City Stories (PSP, ULUS-10160) - VCSNative camera-distance patches
// Target: VCSRecomp Stage 40 / load base 0x08804000
//
// INI (VCSNative.ini):
//
// [DrawDistance]
// Enabled = 1
// World = 1.50
// Vehicles = 1.50
// NPCs = 1.50
//
// 1.0 = original PSP distance. Values below 1.0 are clamped to 1.0.
// World is allowed up to 8.0; Vehicles/NPCs up to 4.0.
// For mission compatibility, keep Vehicles/NPCs <= 2.0 unless tested.
//
// Integration (no header required):
//   1) Add this .cpp to VCSNative's target sources.
//   2) In host/main.cpp, after register_generated_functions(runtime), declare/call:
//        namespace vcs {
//        void install_draw_distance_patch(psprecomp::Runtime &,
//                                         const std::filesystem::path &);
//        }
//        vcs::install_draw_distance_patch(runtime, configuration.source_path);
//
// The install call MUST be after register_generated_functions(runtime), because this
// file intentionally replaces a few AOT entry labels in Runtime's function table.

#include "psprecomp/runtime.hpp"
#include "vcs_config.hpp"
#include "psprecomp/common.hpp"   // hex32, para o relatorio de hooks vivos/mortos

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace vcs {
namespace {

struct DrawDistanceConfig {
    bool enabled{false};
    float world{1.0f};
    float vehicles{1.0f};
    float npcs{1.0f};
};

DrawDistanceConfig g_config{};

// ULUS-10160 / VCSNative AOT guest addresses.
constexpr std::uint32_t kFarClipSetter          = 0x08A1AD6Cu;
constexpr std::uint32_t kCameraLodGetter        = 0x08A1E990u;
constexpr std::uint32_t kCameraLodContinue = 0x08A24138u;
constexpr std::uint32_t kVisibilityCameraSetup = 0x08946158u;
constexpr std::uint32_t kVisibilityCameraContinue = 0x08946168u;
constexpr std::uint32_t kNpcRangeSetup          = 0x089CB38Cu;
constexpr std::uint32_t kNpcRangeContinue       = 0x089CB3C8u;
constexpr std::uint32_t kVehicleRangeSetup      = 0x08B45AC0u;
constexpr std::uint32_t kVehicleRangeContinue   = 0x08B45AC8u;

// VCS globals, addressed from $gp (r28).
constexpr std::uint32_t kGpFarClipOffset  = 7796u; // CDraw::ms_fFarClipZ

// CCamera, not CEntity. The camera recomputes +0x7A8 from FOV/height each
// frame before the real cross-unit call at 0x08A24130.
constexpr std::uint32_t kTheCamera = 0x08BC7E30u;
constexpr std::uint32_t kCameraBaseLodDistance = 0x7A8u;

std::string trim_copy(std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string strip_comment(std::string value) {
    bool quoted = false;
    char quote = '\0';
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '\'' || c == '"') {
            if (!quoted) {
                quoted = true;
                quote = c;
            } else if (quote == c) {
                quoted = false;
            }
        } else if (!quoted && (c == ';' || c == '#')) {
            value.resize(i);
            break;
        }
    }
    return trim_copy(std::move(value));
}

bool parse_bool(std::string value, bool &out) {
    value = lower_copy(trim_copy(std::move(value)));
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        out = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        out = false;
        return true;
    }
    return false;
}

bool parse_float(std::string value, float &out) {
    value = trim_copy(std::move(value));
    if (value.empty()) return false;
    char *end = nullptr;
    const float v = std::strtof(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

DrawDistanceConfig load_config(const std::filesystem::path &path) {
    DrawDistanceConfig cfg{};
    std::ifstream in(path);
    if (!in) return cfg;

    bool in_section = false;
    std::string line;
    while (std::getline(in, line)) {
        line = strip_comment(std::move(line));
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            const std::string section = lower_copy(trim_copy(line.substr(1, line.size() - 2)));
            in_section = (section == "drawdistance" || section == "draw distance");
            continue;
        }
        if (!in_section) continue;

        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower_copy(trim_copy(line.substr(0, eq)));
        const std::string value = trim_copy(line.substr(eq + 1));

        if (key == "enabled") {
            parse_bool(value, cfg.enabled);
        } else if (key == "world" || key == "worldmultiplier") {
            parse_float(value, cfg.world);
        } else if (key == "vehicles" || key == "cars" || key == "vehiclemultiplier") {
            parse_float(value, cfg.vehicles);
        } else if (key == "npcs" || key == "peds" || key == "npcmultiplier") {
            parse_float(value, cfg.npcs);
        }
    }

    cfg.world = std::clamp(cfg.world, 1.0f, 8.0f);
    cfg.vehicles = std::clamp(cfg.vehicles, 1.0f, 4.0f);
    cfg.npcs = std::clamp(cfg.npcs, 1.0f, 4.0f);
    return cfg;
}

float load_float(psprecomp::Runtime &runtime, std::uint32_t address) {
    return std::bit_cast<float>(runtime.memory().load32(address));
}

void store_float(psprecomp::Runtime &runtime, std::uint32_t address, float value) {
    runtime.memory().store32(address, std::bit_cast<std::uint32_t>(value));
}

// Far clip controls the camera's outer clipping plane. Individual object LOD
// checks also need the camera multiplier below; extending the plane alone cannot
// make an already culled object visible.
void far_clip_setter_patch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const float multiplier = vcs_configuration().draw_distance.world;
    const float far_clip = ctx.fpr[12] * multiplier;
    store_float(runtime, ctx.gpr[28] + kGpFarClipOffset, far_clip);
    static float last_world = -1.0f;
    if (last_world != multiplier) {
        std::cerr << "[graphics-distance] world=" << multiplier
                  << " far_clip=" << ctx.fpr[12] << " -> " << far_clip << "\n";
        last_world = multiplier;
    }
    ctx.pc = ctx.gpr[31];
}

// Replace the actual called leaf, not its caller's local resume label
// 0x08A24128, which generated gotos bypass. Preserve the stock getter's return
// (gp - 5504). Its caller multiplies the freshly computed +0x7A8 by this return.
// Leave +0x7A0 unchanged: population/gameplay also read that companion value.
// This follows the camera path identified by ThirteenAG's VCS WidescreenFix.
void camera_lod_getter_patch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    ctx.fpr[0] = load_float(runtime, ctx.gpr[28] - 5504u);
    if (ctx.gpr[31] == kCameraLodContinue && ctx.gpr[16] == kTheCamera) {
        const float multiplier = vcs_configuration().draw_distance.lod;
        const float base = load_float(runtime, kTheCamera + kCameraBaseLodDistance);
        ctx.fpr[0] *= multiplier;
        static float last_lod = -1.0f;
        if (last_lod != multiplier) {
            std::cerr << "[graphics-distance] camera_lod=" << multiplier
                      << " base=" << base << " getter=" << ctx.fpr[0]
                      << " effective=" << base * ctx.fpr[0] << "\n";
            last_lod = multiplier;
        }
    }
    ctx.pc = ctx.gpr[31];
}

// The vehicle atomic callbacks use their own squared-distance thresholds.
// Stock SetRenderWareCamera derives these from +0x7A0, whereas ordinary
// model/ped selection uses +0x7A8. Scale only the renderer's copies: +0x7A0
// itself also controls population/despawning and must remain untouched.
void visibility_camera_setup(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    ctx.gpr[29] -= 16u;
    runtime.memory().store32(ctx.gpr[29], ctx.gpr[31]);
    runtime.memory().store32(ctx.gpr[28] - 18096u, ctx.gpr[4]);
    ctx.gpr[31] = kVisibilityCameraContinue;
    ctx.pc = 0x08890828u; // Original Rsl camera frame lookup, then our continuation.
}

void visibility_camera_continue(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    auto &mem = runtime.memory();
    const auto gp = ctx.gpr[28];
    mem.store32(gp - 18092u, ctx.gpr[2] + 64u);
    const auto mode = mem.load16(kTheCamera + 112u + 608u * mem.load8(kTheCamera + 80u));
    const float ordinary = load_float(runtime, kTheCamera + 0x7A8u);
    const float vehicle = load_float(runtime, kTheCamera + 0x7A0u) * vcs_configuration().draw_distance.lod;
    const auto square = [](float x) { return x * x; };
    store_float(runtime, gp + 8944u, mode == 1u || mode == 37u ? 1000000.0f : square(20.0f * ordinary));
    store_float(runtime, gp + 8916u, square(50.0f * vehicle));
    store_float(runtime, gp + 8928u, square(50.0f * vehicle));
    store_float(runtime, gp + 8920u, square(70.0f * vehicle));
    store_float(runtime, gp + 8924u, square(90.0f * vehicle));
    store_float(runtime, gp + 8932u, square(110.0f * vehicle));
    store_float(runtime, gp + 8936u, square(70.0f * ordinary));
    store_float(runtime, gp + 8940u, square(70.0f * ordinary));
    ctx.gpr[31] = mem.load32(ctx.gpr[29]);
    ctx.gpr[29] += 16u;
    ctx.pc = ctx.gpr[31];
}

// VCS vehicle off-screen despawn/culling constant: original 60.0f.
void vehicle_range_patch(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    ctx.fpr[12] = 60.0f * g_config.vehicles;
    ctx.pc = kVehicleRangeContinue;
}

// VCS population/ped range block. Re-emulates the whole original AOT label,
// changing only 51/25/80; the original 120 constant and integer setup are preserved.
void npc_range_patch(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    ctx.set_gpr(19, ctx.gpr[29] + 64u);
    ctx.set_gpr(30, ctx.gpr[29] + 16u);
    ctx.set_gpr(23, ctx.gpr[29] + 32u);

    ctx.fpr[22] = 120.0f;
    ctx.fpr[28] = 51.0f * g_config.npcs;
    ctx.fpr[26] = 25.0f * g_config.npcs;
    ctx.fpr[24] = 80.0f * g_config.npcs;

    ctx.set_gpr(4, ctx.gpr[18] << 5u);
    ctx.set_gpr(20, ctx.gpr[4]);
    ctx.set_gpr(4, ctx.gpr[4] << 4u);
    ctx.set_gpr(20, ctx.gpr[20] + ctx.gpr[4]);
    ctx.pc = kNpcRangeContinue;
}

} // namespace

void install_draw_distance_patch(psprecomp::Runtime &runtime,
                                 const std::filesystem::path &ini_path) {
    g_config = load_config(ini_path);

    if (!g_config.enabled) {
        std::cerr << "[draw-distance] legacy traffic patches disabled; live far-clip/model-LOD controls installed\n";
    }

    // These replacements are intentionally registered AFTER generated functions.
    // Runtime::register_function overwrites the existing address in both the hash
    // registry and direct dispatch table.
    //
    // But it only has any effect when the address already IS an AOT entry point.
    // Registering an address that lands mid-block succeeds silently and is never
    // dispatched -- vcs_project2dfx.cpp hit exactly this with the heli-height
    // kit's continuation address. The header of this file says it targets
    // "Stage 40 / load base 0x08804000", i.e. a different recompilation, so every
    // address here is a candidate. Report which ones are live instead of leaving
    // a dead hook looking installed.
    std::uint32_t live = 0u;
    std::uint32_t dead = 0u;
    const auto hook = [&](std::uint32_t address, psprecomp::Runtime::RecompiledFunction function,
                          std::string name, const char *what) {
        const bool exists = runtime.has_function(address);
        if (exists) {
            runtime.register_function(address, function, std::move(name));
            ++live;
        } else {
            ++dead;
        }
        std::cerr << "[draw-distance] hook " << what << " em " << psprecomp::hex32(address)
                  << (exists ? " OK" : " MORTO (nao e ponto de entrada AOT nesta recompilacao)")
                  << "\n";
    };

    { // Always reachable: live settings can switch from stock distance at runtime.
        hook(kFarClipSetter, &far_clip_setter_patch, "vcs_draw_distance_far_clip", "far_clip");
        hook(kCameraLodGetter, &camera_lod_getter_patch, "vcs_draw_distance_camera_lod", "camera_lod");
        hook(kVisibilityCameraSetup, &visibility_camera_setup, "vcs_visibility_camera_setup", "vehicle_lod_setup");
        hook(kVisibilityCameraContinue, &visibility_camera_continue, "vcs_visibility_camera_continue", "vehicle_lod_thresholds");
    }

    if (g_config.enabled && g_config.vehicles > 1.0f) {
        hook(kVehicleRangeSetup, &vehicle_range_patch, "vcs_draw_distance_vehicle_range", "vehicle_range");
    }
    if (g_config.enabled && g_config.npcs > 1.0f) {
        hook(kNpcRangeSetup, &npc_range_patch, "vcs_draw_distance_npc_range", "npc_range");
    }
    std::cerr << "[draw-distance] hooks vivos=" << live << " mortos=" << dead << "\n";

    std::cerr << "[draw-distance] enabled"
              << " world=" << g_config.world
              << " vehicles=" << g_config.vehicles
              << " npcs=" << g_config.npcs
              << " entity_lod=" << std::max(g_config.vehicles, g_config.npcs)
              << "\n";
}

} // namespace vcs
