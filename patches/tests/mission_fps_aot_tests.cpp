#include "mission_fps_hooks.hpp"
#include "generated_units.hpp"
#include <array>
#include <bit>
#include <iostream>
#include <stdexcept>

namespace psprecomp {
void register_generated_unit_23(Runtime &);
void register_generated_unit_128(Runtime &);
void register_generated_unit_174(Runtime &);
}
namespace {
unsigned selected_rate=60;
void require(bool b,const char *message){if(!b)throw std::runtime_error(message);}
constexpr unsigned gp=0x08BB1D60,script=0x09000000,main_size=0x1000,object=0x09100000,stack=0x09ff0000;
constexpr std::array<unsigned char,9> boom{8,0,0xe2,0x9b,9,0x8f,0xc2,0x75,0x3c};
void fixture(psprecomp::Runtime &rt) {
    auto &m=rt.memory();
    m.store32(gp-29148,script);m.store32(gp+8004,main_size);m.store32(gp+8016,0x969c);
    m.store32(gp+7960,object);m.store32(object,0);m.store32(object+4,0);
    for(unsigned i=0;i<boom.size();++i)m.store8(script+main_size+i,boom[i]);
}
}
int main() {
    try {
        psprecomp::Runtime rt;
        psprecomp::register_generated_unit_23(rt);
        psprecomp::register_generated_unit_128(rt);
        psprecomp::register_generated_unit_174(rt);
        // Name formatting is outside the code under test; make the foreign
        // routine yield through dispatch, like a native host replacement.
        rt.register_function(0x08B562D8,[](auto &,auto &ctx){ctx.pc=ctx.gpr[31];},"test-script-name");
        vcs::patches::install_mission_fps_patches(rt,[]{return selected_rate;});
        fixture(rt);
        auto &m=rt.memory();
        psprecomp::AllegrexContext ctx{};
        ctx.gpr[28]=gp;ctx.gpr[29]=stack;ctx.gpr[16]=boom.size();ctx.gpr[17]=42;
        // Actual loader continuation after synchronous file read, with chaining
        // enabled: it MUST unwind to the registered StartNewScript override.
        require(rt.invoke_isolated_aot(0x08ABC130,ctx),"AOT loader registered");
        require(ctx.pc==0x08863470 && ctx.gpr[31]==0x08ABC138,"actual loader reaches the hook");
        require(rt.invoke_isolated_aot(ctx.pc,ctx),"StartNewScript hook invoked");
        require(m.load32(script+main_size+5)==std::bit_cast<unsigned>(.015f*.5f),"mission fixed before first opcode");
        require(ctx.pc==0x088626C0 && m.load32(stack-16)==boom.size(),"original prologue/stack preserved");
        unsigned steps=0;
        while(ctx.pc!=0x08ABC138 && steps++<16)require(rt.invoke_isolated_aot(ctx.pc,ctx),"original StartNewScript continuation exists");
        require(ctx.pc==0x08ABC138 && ctx.gpr[29]==stack,"original function returns to loader");
        require(ctx.gpr[16]==boom.size() && ctx.gpr[17]==42,"callee-saved registers restored");
        require(ctx.gpr[2]==object && m.load32(object+16)==main_size,"script object initialized by original AOT");

        m.store8(gp+8024,1);
        for(auto fps:{30u,60u,0u,240u,30u}) {
            selected_rate=fps;vcs::patches::update_mission_fps_patches(rt,gp);
            ctx={};ctx.gpr[28]=gp;ctx.gpr[29]=stack;ctx.gpr[17]=0;
            m.store32(gp-8852,1);
            require(rt.invoke_isolated_aot(0x08A070C8,ctx),"frame-limiter override dispatched");
            require(ctx.gpr[4]==1,"limiter delay slot preserved");
            require(ctx.pc==(fps==30?0x08A070A8u:0x08A070D0u),"mission policy selects original/unlocked branch");
            require(rt.invoke_isolated_aot(ctx.pc,ctx),"actual AOT limiter continuation dispatched");
            require(ctx.pc==(fps==30?0x08B73224u:0x08B73514u),"actual branch waits another vblank only at 30");
        }
        m.store8(gp+8024,0);selected_rate=0;vcs::patches::update_mission_fps_patches(rt,gp);
        require(vcs::patches::mission_frame_rate(0)==0,"mission end releases uncapped policy");
        fixture(rt);
        ctx={};ctx.gpr[28]=gp;ctx.gpr[29]=stack;ctx.gpr[16]=boom.size();
        ctx.gpr[4]=main_size;ctx.gpr[31]=0x08ABC138;
        require(rt.invoke_isolated_aot(0x08863470,ctx),"reload observed");
        const auto patched_word=m.load32(script+main_size+5);
        m.store32(gp-29148,script+0x20000);
        selected_rate=30;vcs::patches::update_mission_fps_patches(rt,gp);
        require(m.load32(script+main_size+5)==patched_word,"replaced allocation is not written during restoration");
        require(vcs::patches::mission_frame_rate(0)==0,"replaced allocation releases policy");
        std::cout<<"Actual AOT mission loader, StartNewScript continuation and frame-limiter paths passed\n";
        return 0;
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
