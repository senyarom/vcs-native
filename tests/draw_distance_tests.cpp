#include "psprecomp/runtime.hpp"
#include "generated_units.hpp"
#include "vcs_config.hpp"
#include <bit>
#include <array>
#include <iostream>
#include <stdexcept>

namespace vcs { void install_draw_distance_patch(psprecomp::Runtime &, const std::filesystem::path &); }
namespace psprecomp { void register_generated_unit_76(Runtime &); void register_generated_unit_80(Runtime &);
    void register_generated_unit_136(Runtime &); void register_generated_unit_170(Runtime &); }
static void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }

int main() {
    try {
        psprecomp::Runtime rt;
        psprecomp::register_generated_unit_76(rt);
        psprecomp::register_generated_unit_80(rt);
        psprecomp::register_generated_unit_136(rt);
        psprecomp::register_generated_unit_170(rt);
        constexpr unsigned gp=0x08900000, camera=0x08BC7E30, model=0x09000000, atomics=0x09000100;
        auto &mem=rt.memory();
        auto put=[&](unsigned addr,float x){mem.store32(addr,std::bit_cast<unsigned>(x));};
        auto get=[&](unsigned addr){return std::bit_cast<float>(mem.load32(addr));};
        constexpr std::array<unsigned,8> offsets{8916,8920,8924,8928,8932,8936,8940,8944};
        std::array<std::array<unsigned,8>,3> stock_thresholds{};
        const unsigned modes[]{0,1,37};
        put(camera+0x7a0,1.25f); put(camera+0x7a8,1.0f);
        for (unsigned i=0;i<3;++i) {
            mem.store8(camera+80,0); mem.store16(camera+112,modes[i]);
            psprecomp::AllegrexContext ctx{};ctx.gpr[28]=gp;ctx.gpr[29]=0x09fefff0;
            ctx.gpr[2]=0x09001000;mem.store32(ctx.gpr[29],0xdead0000);
            require(rt.invoke_isolated_aot(0x08946168,ctx),"stock vehicle setup continuation");
            require(ctx.pc==0xdead0000 && ctx.gpr[29]==0x09ff0000,"stock vehicle setup returns");
            for (unsigned j=0;j<offsets.size();++j) stock_thresholds[i][j]=mem.load32(gp+offsets[j]);
        }
        rt.register_function(0x08A1E990, [](auto &r,auto &c){c.fpr[0]=std::bit_cast<float>(r.memory().load32(c.gpr[28]-5504));c.pc=c.gpr[31];}, "stock-getter");
        rt.register_function(0x08A1AD6C, [](auto &,auto &){}, "stock-farclip");
        rt.register_function(0x0895BBA4, [](auto &,auto &c){c.pc=c.gpr[31];}, "vehicle-render-boundary");
        vcs::install_draw_distance_patch(rt, {});
        put(gp-5504, 0.8f); // Preserve the game's own multiplier as well as ours.
        mem.store8(gp-7080, 1); // Yield at the next foreign camera-mode query.
        mem.store32(model+0x28,atomics);mem.store8(model+0x38,2);mem.store16(model+0x3a,0);
        mem.store32(atomics,0x12340000);mem.store32(atomics+4,0x56780000);
        put(model+0x2c,100);put(model+0x30,200);
        auto visible=[&](unsigned pc,float distance) {
            psprecomp::AllegrexContext ctx{};ctx.gpr[4]=model;ctx.gpr[31]=0xdead0000;ctx.fpr[12]=distance;
            require(rt.invoke_isolated_aot(pc,ctx),"actual AOT model selection dispatched");
            require(ctx.pc==0xdead0000,"AOT visibility function returned normally");
            return ctx.gpr[2];
        };
        vcs::VcsConfiguration config{};
        for(float multiplier : {1.f,3.f,10.f,10.f,3.f,1.5f,1.f}) {
            config.draw_distance={2.f,multiplier};vcs::publish_graphics_configuration(config);
            // The camera computes this fresh each frame (08A23FA0/08A23FB8).
            put(camera+0x7a8,1.25f);
            psprecomp::AllegrexContext ctx{};ctx.gpr[28]=gp;ctx.gpr[16]=camera;ctx.gpr[29]=0x09ff0000;
            require(rt.invoke_isolated_aot(0x08A24128,ctx),"actual camera caller dispatched");
            require(ctx.pc==0x08A1E990 && ctx.gpr[31]==0x08A24138,"caller reaches the hooked leaf, not a local-label override");
            require(rt.invoke_isolated_aot(ctx.pc,ctx) && ctx.pc==0x08A24138,"camera getter preserves return PC");
            require(rt.invoke_isolated_aot(ctx.pc,ctx),"actual camera continuation dispatched");
            require(std::abs(get(camera+0x7a8)-multiplier)<0.0001f,"camera applies live LOD once, including original multiplier");
            require(get(camera+0x7a0)==1.25f,"population/gameplay companion distance stays stock");
            for (unsigned i=0;i<3;++i) {
                mem.store16(camera+112,modes[i]);mem.store32(gp+8896,0x09002000);
                psprecomp::AllegrexContext render{};render.gpr[28]=gp;render.gpr[29]=0x09ff0000;
                require(rt.invoke_isolated_aot(0x08934AE4,render),"actual world-render camera caller");
                require(render.pc==0x08946158 && render.gpr[31]==0x08934AEC,"world caller reaches vehicle LOD hook");
                require(rt.invoke_isolated_aot(render.pc,render) && render.pc==0x08890828,"camera setup calls original frame lookup");
                require(mem.load32(gp-18096)==0x09002000,"original render camera pointer preserved");
                render.gpr[2]=0x09001000; // Camera-frame lookup result.
                require(rt.invoke_isolated_aot(0x08946168,render),"vehicle setup continuation");
                require(render.pc==0x08934AEC && render.gpr[29]==0x09ff0000,"vehicle patch preserves return and stack");
                require(mem.load32(gp-18092)==0x09001040,"camera position pointer preserved");
                for (unsigned j=0;j<offsets.size();++j) {
                    const float stock=std::bit_cast<float>(stock_thresholds[i][j]);
                    const float expected=offsets[j]==8944 && modes[i]!=0 ? stock : stock*multiplier*multiplier;
                    require(std::abs(get(gp+offsets[j])-expected)<0.01f,"all vehicle/ped renderer thresholds scale once and restore");
                }
                require(get(camera+0x7a0)==1.25f,"vehicle patch does not alter population distance");
            }
            // Execute the real high-detail vehicle callbacks at a fixed distance.
            // A visible atomic reaches the foreign renderer; a culled one returns.
            for (const auto [pc,distance] : std::array<std::pair<unsigned,float>,2>{{{0x08945FC8,70.f},{0x0894604C,150.f}}}) {
                put(gp+6572,distance*distance);
                psprecomp::AllegrexContext vehicle{};vehicle.gpr[28]=gp;vehicle.gpr[29]=0x09ff0000;
                vehicle.gpr[31]=0xdead0000;vehicle.gpr[4]=atomics;
                require(rt.invoke_isolated_aot(pc,vehicle),"actual high-detail vehicle callback");
                require(vehicle.pc==(multiplier>1 ? 0x0895BBA4u : 0xdead0000u),"vehicle detail follows LOD at the same camera distance");
            }
            require(get(model+0x2c)==100 && get(model+0x30)==200,"model table is not multiplied a second time");
            require(visible(0x08AAE058,90*multiplier)==0x12340000,"detailed atomic threshold follows LOD");
            require(visible(0x08AAE058,150*multiplier)==0x56780000,"lower detail atomic threshold follows LOD");
            require(visible(0x08AAE058,210*multiplier)==0,"objects beyond scaled range are culled");
            require(visible(0x08AAE140,350)==(multiplier>=3 ? 0x56780000u : 0u),"coarsest-atomic callback extends visibility at 3x/10x and restores");
            require(visible(0x08AAE058,350)==(multiplier==10 ? 0x12340000u : multiplier==3 ? 0x56780000u : 0u),"same camera distance changes detail at 1x/3x/10x and restores");
            ctx.gpr[31]=0xdead0000;ctx.fpr[12]=500;
            require(rt.invoke_isolated_aot(0x08A1AD6C,ctx) && get(gp+7796)==1000,"far clip stays independent of LOD");
        }
        for (float far_clip : {1.f,10.f,3.f,1.f}) {
            config.draw_distance.world=far_clip;vcs::publish_graphics_configuration(config);
            psprecomp::AllegrexContext c{};c.gpr[28]=gp;c.gpr[31]=0xdead0000;c.fpr[12]=500;
            require(rt.invoke_isolated_aot(0x08A1AD6C,c) && get(gp+7796)==500*far_clip,"far clip accepts 10x and restores without compounding");
        }
        std::cout << "Actual VCS AOT camera dispatch, atomic selection, culling and live restoration passed\n";
        return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n';return 1; }
}
