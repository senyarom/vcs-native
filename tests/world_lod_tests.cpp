#include "vcs_world_lod.hpp"
#include "vcs_config.hpp"
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

void require(bool b,const char *s) {if (!b) throw std::runtime_error(s);}
void append(std::vector<std::uint8_t> &b,unsigned n) {for(int i=0;i<4;++i)b.push_back(n>>(8*i));}
void set(std::vector<std::uint8_t> &b,unsigned p,unsigned n) {for(int i=0;i<4;++i)b.at(p+i)=n>>(8*i);}

int main() {
    const auto directory=std::filesystem::temp_directory_path()/
        ("vcs-world-lod-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directory(directory);
        std::vector<std::uint8_t> cache{'V','C','S','W','L','D','0','2'};
        append(cache,6167);append(cache,80);append(cache,1);append(cache,4);
        for(unsigned i=0;i<6167;++i)append(cache,i>=1 && i<=3 ? 16+16*i : 0);
        append(cache,36); // In-resource pointer, whose alignment must survive relocation.
        std::vector<std::uint8_t> blob(80);set(blob,36,48);cache.insert(cache.end(),blob.begin(),blob.end());
        auto variant=[&](unsigned pass,unsigned id,unsigned resource,unsigned row,unsigned radius=0) {
            std::vector<std::uint8_t> v(384);
            set(v,0,pass);set(v,4,6);set(v,8,15);
            set(v,12,std::bit_cast<unsigned>(-1700.f));set(v,16,std::bit_cast<unsigned>(-350.f));
            set(v,24,id|(resource<<16));
            set(v,24+8,radius<<16); // half-float bounding radius at record +10
            set(v,24+12,0x3A000000);set(v,24+52,0x3C000000);set(v,24+56,0x3C000000);set(v,24+64,0x0B000000);
            set(v,92+8*row,1<<6);
            cache.insert(cache.end(),v.begin(),v.end());
        };
        variant(0,60,3,17,psprecomp::AllegrexContext::vfpu_shrink_to_half_bits(1533.f));
        variant(2,50,3,17);variant(4,40,1,15);variant(4,40,2,15);
        std::ofstream file(directory/"MAINLA.wld",std::ios::binary);
        file.write(reinterpret_cast<const char*>(cache.data()),cache.size());file.close();
#ifdef _WIN32
        _putenv_s("PSPRECOMP_WORLD_CATALOG",directory.string().c_str());
#else
        setenv("PSPRECOMP_WORLD_CATALOG",directory.c_str(),1);
#endif
        psprecomp::Runtime rt(64*1024*1024);
        auto &m=rt.memory();
        constexpr unsigned world=0x08E91200,level=0x09000000,resources=0x09002000;
        constexpr unsigned record=0x09020000,root=0x09020100,instances=0x09021000,gp=0x08900000;
        m.store32(world+24,level);m.store32(level,resources);m.store32(level+300,6167);
        m.store32(level+696,1);m.store32(level+700,level+800);
        m.store32(level+800,(40<<16)|(6<<8)|0x94); // visible from 06:00 until 20:00
        m.store8(gp+7648,12);
        m.store32(world+580,record);m.store32(world+588,record);
        m.store32(world+1440,0x09040000);m.store32(world+1456,37);m.store32(world+1460,8192);
        m.store32(resources+16,0x0971C5C0); // original PSP command cache must not be reused
        m.store32(record+28,4); // live sectors may own several streaming overlays
        m.store32(root,0x09050000);m.store16(root+4,9);
        m.store32(record+8,6);m.store32(record+12,17);m.store32(record+32,root);
        for(unsigned i=0;i<9;++i)m.store32(root+8+4*i,instances+(i<=2?0:i<=4?68:136));
        m.store32(instances,50|(3<<16));m.store32(instances+68,1|(1<<16));
        m.store32(instances+68+12,0xDEADBEEF); // live moving-object state
        constexpr unsigned camera=0x08BC7E30+0x9B0;
        m.store32(camera,std::bit_cast<unsigned>(-1700.f));m.store32(camera+4,std::bit_cast<unsigned>(-130.f));
        m.store8(0x08BC7230+301,1);
        rt.register_function(0x08958D28,[](auto&,auto&){},"original-world-entry");
        vcs::install_world_lod(rt);
        auto snapshot=[&] {return std::vector<std::uint8_t>(m.bytes().begin()+world-0x08000000,m.bytes().begin()+world-0x08000000+11772);};
        const auto before=snapshot();
        vcs::VcsConfiguration config{};
        std::map<unsigned,unsigned> slot_commands;
        for(float lod:{1.f,3.f,10.f,10.f,3.f,1.f}) {
            config.draw_distance.lod=lod;vcs::publish_graphics_configuration(config);
            psprecomp::AllegrexContext c{};c.gpr[4]=world;c.gpr[5]=0;c.gpr[28]=gp;c.gpr[29]=0x09FFF000;c.gpr[31]=0xDEAD0000;
            c.gpr[16]=0x12345678;c.fpr[20]=2.5;
            require(rt.invoke_isolated_aot(0x08958D28,c),"native render entry");
            require(c.pc==0x08958D88 && c.gpr[29]==0x09FFF000-624,"original render prologue and ready branch");
            require(m.load32(c.gpr[29]+580)==0x12345678 && m.load32(c.gpr[29]+576)==std::bit_cast<unsigned>(2.5f),"callee saved registers preserved");
            require(snapshot()==before,"original world ownership is untouched during rendering");
            require(m.load32(level)==resources && m.load32(resources+12)==0,"original level/resources stay untouched");
            if(lod==1) {
                require(c.gpr[21]==world && m.load32(c.gpr[29]+616)==0xDEAD0000,"1x uses the original world and return address");
            } else {
                unsigned clone=c.gpr[21];require(clone>=0x0B800000,"extended render uses upper reserved RAM");
                auto table=m.load32(m.load32(clone+24));
                require(m.load32(table+16)==slot_commands[clone] && m.load32(resources+16)==0x0971C5C0,"native command cache survives slot reuse without sharing original ownership");
                require(m.load32(clone+1440)!=m.load32(world+1440) && m.load32(clone+1456)==0,"deferred-free ring is private and empty");
                require(m.load32(table+12)==0x0A000020 && m.load32(0x0A000024)==0x0A000030,"resource payload pointers relocated into owned arena");
                auto r=m.load32(m.load32(clone+580)+32);
                require(m.load32(m.load32(clone+580)+28)==1 && m.load16(r+4)==0,"synthetic sector has one root and no streaming overlays");
                require(m.load32(r+12)-m.load32(r+8)==68 && m.load16(m.load32(r+8))==60,
                        "island-sized super-LOD remains visible when camera is inside its bounds");
                require(m.load32(r+16)==m.load32(r+20),"coarse model disappears at increased LOD");
                auto begin=m.load32(r+24),end=m.load32(r+28);
                require(end-begin==3*68,"detailed model components plus live dynamic object selected");
                require(m.load16(begin)==1 && m.load32(begin+12)==0xDEADBEEF,"dynamic object keeps current guest transform");
                require(m.load16(begin+68)==40 && m.load16(begin+136)==40,"both same-ID mesh components survive");
                require(m.load16(begin+70)==1 && m.load16(begin+138)==2,"mesh resource identity preserved");
                require(m.load32(c.gpr[29]+616)==0x10,"native return hook on saved stack");
                unsigned previous=0;
                for(unsigned ra:{0x08955744u,0x0895AE88u}) {
                    psprecomp::AllegrexContext alloc{};
                    alloc.gpr[4]=0x08BC7230;alloc.gpr[5]=100;alloc.gpr[31]=ra;alloc.gpr[29]=0x09FFF000;
                    require(rt.invoke_isolated_aot(0x08ABE57C,alloc),"native command allocation");
                    require(alloc.pc==ra && alloc.gpr[29]==0x09FFF000,"native allocation returns with caller stack unchanged");
                    require(alloc.gpr[2]>=clone && alloc.gpr[2]+100<=clone+2*1024*1024 && !(alloc.gpr[2]&15),"command allocation stays aligned within frame arena");
                    require(!previous || alloc.gpr[2]>=previous+100,"texture and geometry command allocations do not overlap");
                    previous=alloc.gpr[2];
                }
                m.store32(table+16,previous);slot_commands[clone]=previous;
                c.gpr[29]+=624;require(rt.invoke_isolated_aot(0x10,c),"native render return");
                require(c.pc==0xDEAD0000 && c.gpr[31]==0xDEAD0000,"return restores original RA");
            }
            require(snapshot()==before,"1x/3x/1x never mutates original stream lists");
        }
        // A new streamed resource version must invalidate all referring
        // command chains, even if its raw address was recycled by the PSP heap.
        m.store32(resources+20,1234);
        for(unsigned size:{0u,64u,4096u}) {
            psprecomp::AllegrexContext c{};
            c.gpr[4]=0x08BC7230;c.gpr[5]=size;c.gpr[29]=0x09FFF000;c.gpr[31]=0x12345678;
            c.gpr[16]=16;c.gpr[17]=17;
            require(rt.invoke_isolated_aot(0x08ABE57C,c),"original allocator fallback");
            require(c.pc==(size?0x08ABE594:0x08ABE5A8) && c.gpr[29]==0x09FFF000-16,"normal allocator resumes original branch");
            require(m.load32(c.gpr[29])==16 && m.load32(c.gpr[29]+4)==17 && m.load32(c.gpr[29]+8)==0x12345678,"original allocator preserves callee-saved state");
        }
        // Timed records use the live game hour; both same-ID components hide.
        config.draw_distance.lod=3;vcs::publish_graphics_configuration(config);m.store8(gp+7648,22);
        psprecomp::AllegrexContext c{};c.gpr[4]=world;c.gpr[28]=gp;c.gpr[29]=0x09FFF000;c.gpr[31]=0xDEAD0000;
        require(rt.invoke_isolated_aot(0x08958D28,c),"night render");
        require(m.load32(m.load32(m.load32(c.gpr[21]+24))+16)==0,"stream ownership change invalidates cached commands");
        auto r=m.load32(m.load32(c.gpr[21]+580)+32),p=m.load32(r+24);
        require(m.load16(p+68)==(40|0x8000) && m.load16(p+136)==(40|0x8000),"timed visibility applies at night");
        require(rt.invoke_isolated_aot(0x10,c),"night return");
        // Still-camera selection reuse must not cache mission swaps, time,
        // resource availability, or transforms. Later matching swaps win.
        auto check_live=[&](unsigned expected_id) {
            psprecomp::AllegrexContext check{};check.gpr[4]=world;check.gpr[28]=gp;
            check.gpr[29]=0x09FFF000;check.gpr[31]=0xDEAD0000;
            require(rt.invoke_isolated_aot(0x08958D28,check),"live swap render");
            for(unsigned sector:{580u,588u}) {
                auto mesh=m.load32(m.load32(check.gpr[21]+sector)+32);
                auto begin=m.load32(mesh+24);
                require(m.load16(begin+68)==expected_id && m.load16(begin+136)==expected_id,"live swap result for both sectors/components");
                require(m.load32(begin+12)==m.load32(instances+68+12),"cached selection reads current dynamic transform");
            }
            require(rt.invoke_isolated_aot(0x10,check),"live swap return");
        };
        m.store32(instances+68+12,0xCAFEBABE);
        m.store32(level+696,2);m.store32(level+804,(40<<16)|(7<<8)|2);
        m.store32(world+1040+8,7);check_live(40);
        m.store32(world+1040+8,6);check_live(40|0x8000);
        m.store32(level+804,(40<<16)|255);check_live(40|0x8000);
        m.store32(level+804,(40<<16)|(20<<8)|0x86); // 20:00 to 06:00 crosses midnight
        check_live(40);m.store8(gp+7648,3);check_live(40);
        m.store8(gp+7648,6);check_live(40|0x8000);
        m.store32(level+696,0);check_live(40); // removal at the same list address
        // Camera movement must invalidate static variant selection immediately.
        m.store32(camera+4,std::bit_cast<unsigned>(-1300.f));
        psprecomp::AllegrexContext moved{};moved.gpr[4]=world;moved.gpr[28]=gp;
        moved.gpr[29]=0x09FFF000;moved.gpr[31]=0xDEAD0000;
        require(rt.invoke_isolated_aot(0x08958D28,moved),"moved camera render");
        auto moved_root=m.load32(m.load32(moved.gpr[21]+580)+32);
        require(m.load32(moved_root+28)-m.load32(moved_root+24)==68,"moved camera drops out-of-range catalog components");
        require(rt.invoke_isolated_aot(0x10,moved),"moved camera return");
        m.store32(camera+4,std::bit_cast<unsigned>(-130.f));check_live(40);
        // A farther camera position distinguishes real 10x selection from a
        // UI-only change that still silently clamps the world renderer to 3x.
        m.store32(camera+4,std::bit_cast<unsigned>(200.f));
        const auto before_extended=snapshot();
        for (float lod : {3.f,10.f,3.f}) {
            config.draw_distance.lod=lod;vcs::publish_graphics_configuration(config);
            psprecomp::AllegrexContext far{};far.gpr[4]=world;far.gpr[28]=gp;
            far.gpr[29]=0x09FFF000;far.gpr[31]=0xDEAD0000;
            require(rt.invoke_isolated_aot(0x08958D28,far),"extended world render");
            const auto mesh=m.load32(m.load32(far.gpr[21]+580)+32);
            require(m.load32(mesh+28)-m.load32(mesh+24)==(lod==10 ? 3u : 1u)*68,
                    "10x selects details beyond 3x and changing back restores the old range");
            require(rt.invoke_isolated_aot(0x10,far),"extended world return");
            require(snapshot()==before_extended,"10x selection preserves original world state");
        }
        m.store32(camera+4,std::bit_cast<unsigned>(-130.f));
        // Duplicate live keys keep the final record, and do not merge other
        // resources with the same ID. This matches the previous ordered map.
        for(unsigned i=5;i<9;++i)m.store32(root+8+4*i,instances+4*68);
        m.store32(instances+136,40|(1<<16));m.store32(instances+136+12,0x11111111);
        m.store32(instances+204,40|(1<<16));m.store32(instances+204+12,0x22222222);
        moved={};moved.gpr[4]=world;moved.gpr[28]=gp;moved.gpr[29]=0x09FFF000;moved.gpr[31]=0xDEAD0000;
        require(rt.invoke_isolated_aot(0x08958D28,moved),"duplicate live keys render");
        moved_root=m.load32(m.load32(moved.gpr[21]+580)+32);
        auto duplicate_begin=m.load32(moved_root+24);
        require(m.load32(moved_root+28)-duplicate_begin==3*68,"duplicate live keys do not duplicate a mesh");
        require(m.load32(duplicate_begin+68+12)==0x22222222,"last live record wins for same ID/resource");
        require(m.load16(duplicate_begin+136+2)==2,"same-ID different resource remains independent");
        require(rt.invoke_isolated_aot(0x10,moved),"duplicate return");
        // Interior worlds retain original lists; geographic expansion is exterior-only.
        m.store32(world+604,1);c.gpr[4]=world;c.gpr[5]=0;
        require(rt.invoke_isolated_aot(0x08958D28,c) && c.gpr[21]==world,"interior selection remains original");
        std::filesystem::remove_all(directory);
        std::cout<<"Native world cache, ownership, mesh components, timed swaps and 1x restoration passed\n";
        return 0;
    } catch(const std::exception &e) {
        std::filesystem::remove_all(directory);std::cerr<<e.what()<<'\n';return 1;
    }
}
