#include "vcs_world_streaming.hpp"
#include <bit>
#include <iostream>
#include <stdexcept>

static void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    try {
        psprecomp::GuestMemory mem;
        constexpr unsigned world=0x08E91200, cam=0x08BC7E30+0x9B0;
        constexpr unsigned root=0x09000000, meta=root+0x100, table=root+0x200;
        constexpr unsigned original=root+0x1000, extended=root+0x2000, record=root+0x5000;
        auto put=[&](unsigned at,float value){mem.store32(at,std::bit_cast<unsigned>(value));};
        mem.store32(world+580,record);mem.store32(record+8,6);mem.store32(record+32,root);
        mem.store32(root,meta+96);mem.store32(meta,0x42534356);mem.store32(meta+4,0x314C4C49);
        for(unsigned i=0;i<9;++i) {
            mem.store32(meta+8+i*4,original+(i<5?0:68));
            mem.store32(meta+44+i*4,extended+(i<5?0:136));
            mem.store32(root+8+i*4,original+(i<5?0:68));
        }
        mem.store32(meta+80,1);mem.store32(meta+84,table);put(meta+88,160);put(meta+92,3);
        mem.store32(table,extended+68);mem.store32(table+4,3380);
        put(table+8,-1700);put(table+12,-351);put(table+16,25);
        mem.store16(original,4000);mem.store16(extended,4000|0x8000);mem.store16(extended+68,4051);
        mem.store16(original+2,3380);mem.store16(extended+70,3380);
        put(cam,-1700);put(cam+4,-130);put(cam+8,16);
        for(float lod : {1.f,1.25f,1.5f,3.f,3.f,1.f,3.f,1.f}) {
            vcs::update_world_streaming(mem,lod);
            require(mem.load32(root+24)==(lod>1?extended:original),"LOD switches the actual render list");
            require(mem.load16(extended+70)==(lod>=1.5f?3380:0),"Extra instance obeys camera distance and live LOD");
            require(mem.load16(original+2)==3380,"Original instance is never rewritten");
            require(mem.load32(meta+24)==original,"Original list descriptor is immutable");
        }
        // Disk flags differ from the guest's resolved visibility state. Carry
        // both initial fixups and subsequent mutations through every switch.
        vcs::update_world_streaming(mem,3);
        require(mem.load16(extended)==4000,"Loaded visibility fixup reaches extended list");
        mem.store16(extended,4000|0x8000);mem.store32(extended+12,0x12345678);
        vcs::update_world_streaming(mem,3);
        require(mem.load32(extended+12)==0x12345678,"No stale copy overwrites active guest state");
        vcs::update_world_streaming(mem,1);
        require(mem.load16(original)==(4000|0x8000) && mem.load32(original+12)==0x12345678,
                "Restore carries active flags and record changes back to original list");
        mem.store16(original,4000);
        vcs::update_world_streaming(mem,3);
        require(mem.load16(extended)==4000,"Later original-side fixup reaches extended list again");
        vcs::update_world_streaming(mem,1);
        // Rejected metadata must not partially rewrite any sector state.
        mem.store32(table,original);
        vcs::update_world_streaming(mem,3);
        require(mem.load32(root+24)==original,"Out-of-list instance descriptor is rejected");
        mem.store32(table,extended+68);mem.store32(meta,0);
        vcs::update_world_streaming(mem,3);
        require(mem.load32(root+24)==original,"Unpatched sectors are inert");
        std::cout<<"Static world lists, distance filtering and exact live 1x restoration passed\n";
        return 0;
    } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
