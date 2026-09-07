#include "vcs_world_lod.hpp"
#include "vcs_config.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace vcs {
namespace {
constexpr std::uint32_t kWorld=0x08E91200, kCamera=0x08BC7E30+0x9B0;
constexpr std::uint32_t kResourceBase=0x0A000000, kSlotBase=0x0B800000;
constexpr std::uint32_t kSlotBytes=2*1024*1024, kWorldBytes=11772;
constexpr std::uint32_t kCommandOffset=1536*1024;
constexpr std::uint32_t kRender=0x08958D28, kReturn=0x10;
constexpr std::size_t kRows=36;
using Memory=psprecomp::GuestMemory;
using InstanceKey=std::pair<std::uint16_t,std::uint16_t>;
using LiveInstance=std::pair<InstanceKey,std::uint32_t>;
float f32(const Memory &m,std::uint32_t p) {return std::bit_cast<float>(m.load32(p));}
std::uint32_t word(const std::vector<std::uint8_t>& b,std::size_t p) {
    if (p+4>b.size()) throw std::runtime_error("Truncated world LOD cache");
    return std::uint32_t(b[p]) | (std::uint32_t(b[p+1])<<8) | (std::uint32_t(b[p+2])<<16) | (std::uint32_t(b[p+3])<<24);
}
struct Variant {
    std::uint32_t pass{}, x{}, y{};
    std::array<float,3> center{};
    std::array<std::uint8_t,68> data{};
    std::array<std::uint64_t,kRows> rows{};
    std::uint16_t id() const {return (data[0] | (std::uint16_t(data[1])<<8))&0x7FFF;}
    std::uint16_t resource() const {return data[2] | (std::uint16_t(data[3])<<8);}
    bool selected(float cx,float cy,float lod) const {
        float factor=1.0f/lod;
        if (pass==0) {
            // Super-LODs may represent an entire remote island. Their origin
            // is not a meaningful near-detail sample point: moving the probe
            // inside their bounds can remove the whole skyline while the
            // current level has no replacement for it. Stop at the bounds,
            // and leave the probe unchanged when already inside them.
            const auto half=std::uint16_t(data[10]|(std::uint16_t(data[11])<<8));
            const float radius=std::bit_cast<float>(psprecomp::AllegrexContext::vfpu_expand_half_bits(half));
            const float distance=std::hypot(cx-center[0],cy-center[1]);
            if (std::isfinite(radius) && radius>0 && distance>0)
                factor=std::max(factor,std::min(1.0f,radius/distance));
        }
        const float vx=center[0]+(cx-center[0])*factor, vy=center[1]+(cy-center[1])*factor;
        const int sy=int(std::floor((vy+2000.0f)/108.25f));
        const int sx=int(std::floor((vx+2400.0f)/125.0f+(sy&1)*0.5f));
        return sy>=0 && sy<int(kRows) && sx>=0 && sx<64 && (rows[sy]&(std::uint64_t(1)<<sx));
    }
};
struct Catalog {
    std::uint32_t resources{};
    std::vector<std::uint32_t> pointers, relocations;
    std::vector<std::uint8_t> blob;
    std::vector<Variant> variants;
};
Catalog read_catalog(const std::filesystem::path &path) {
    std::ifstream in(path,std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read world LOD cache: "+path.string());
    std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(in)),{});
    if (b.size()<24 || std::memcmp(b.data(),"VCSWLD02",8)) throw std::runtime_error("Invalid world LOD cache header");
    Catalog c;
    c.resources=word(b,8);
    const auto bytes=word(b,12), relocs=word(b,16), variants=word(b,20);
    if (!c.resources || c.resources>8192 || bytes>24*1024*1024 || variants>10000 || relocs>bytes/4 ||
        24ull+4ull*(c.resources+relocs)+bytes+384ull*variants!=b.size())
        throw std::runtime_error("World LOD cache exceeds its arena or has inconsistent sizes");
    std::size_t at=24;
    for (unsigned i=0;i<c.resources;++i,at+=4) {
        auto p=word(b,at);
        if (p && (p<32 || p>=bytes)) throw std::runtime_error("Invalid world resource pointer");
        c.pointers.push_back(p);
    }
    for (unsigned i=0;i<relocs;++i,at+=4) c.relocations.push_back(word(b,at));
    c.blob.assign(b.begin()+at,b.begin()+at+bytes);at+=bytes;
    for (auto p:c.relocations)
        if (p<32 || p+4ull>bytes || word(c.blob,p)<32 || word(c.blob,p)>=bytes)
            throw std::runtime_error("Invalid world resource relocation");
    for (unsigned i=0;i<variants;++i,at+=384) {
        Variant v;v.pass=word(b,at);v.x=word(b,at+4);v.y=word(b,at+8);
        for (unsigned j=0;j<3;++j) v.center[j]=std::bit_cast<float>(word(b,at+12+4*j));
        std::copy_n(b.begin()+at+24,68,v.data.begin());
        for (unsigned j=0;j<kRows;++j) v.rows[j]=word(b,at+92+8*j)|(std::uint64_t(word(b,at+96+8*j))<<32);
        if (v.pass>=8 || v.resource()>=c.resources || v.x>=64 || v.y>=kRows ||
            !std::all_of(v.center.begin(),v.center.end(),[](float f){return std::isfinite(f);}))
            throw std::runtime_error("Invalid world instance descriptor");
        if (!c.variants.empty() && std::pair(v.pass,v.id())<std::pair(c.variants.back().pass,c.variants.back().id()))
            throw std::runtime_error("World instances are not sorted by pass and ID");
        c.variants.push_back(v);
    }
    return c;
}
struct State {
    struct FrameCache {
        std::vector<std::uint32_t> signature, commands;
        std::uint32_t end{};
    };
    std::filesystem::path directory;
    Catalog catalog;
    std::uint32_t level{}, slot{}, clone{}, original_world{}, return_pc{}, queue{}, queue_end{}, queue_head{};
    std::uint32_t resource_table{}, count{}, last_count{}, command_cursor{}, command_end{};
    std::array<std::vector<const Variant *>,8> selected;
    std::vector<LiveInstance> live_instances;
    float selected_x{}, selected_y{}, selected_lod{};
    bool selection_valid{};
    float last_lod{};
    std::array<FrameCache,3> frames;
    bool active{}, enabled{};
} state;

void copy_guest(Memory &m,std::uint32_t destination,std::uint32_t source,std::size_t size) {
    const auto *bytes=m.raw_pointer(source,size);
    if (!bytes) throw std::runtime_error("Invalid native world copy source");
    m.copy_in(destination,std::span<const std::uint8_t>(bytes,size));
}
struct Arena {
    Memory &m;
    std::uint32_t cursor,end;
    std::uint32_t allocate(std::uint32_t size) {
        cursor=(cursor+15)&~15u;
        if (size>end-cursor) throw std::runtime_error("Native world frame arena exhausted");
        auto p=cursor;cursor+=size;return p;
    }
    std::uint32_t clone(std::uint32_t p,std::uint32_t size) {
        auto q=allocate(size);
        copy_guest(m,q,p,size);return q;
    }
};

// Swaps are live mission/time state. Resolve them once for this render, keeping
// the original last-matching-entry rule and rebuilding even for a still camera.
using Visibility=std::array<bool,32768>;
Visibility resolve_visibility(const Memory &m,std::uint32_t level,std::uint32_t gp) {
    Visibility hidden{};
    const auto count=m.load32(level+696), swaps=m.load32(level+700);
    if (count>32768 || !m.contains(swaps,4ull*count)) throw std::runtime_error("Invalid live world swaps");
    const auto hour=m.load8(gp+7648);
    for (unsigned i=0;i<count;++i) {
        const auto p=swaps+4*i;
        const auto id=m.load16(p+2);
        if (id>=hidden.size()) continue;
        const auto off=m.load8(p), on=m.load8(p+1);
        if (off==255) hidden[id]=true;
        else if (off&128) {
            const auto end=off&127;
            hidden[id]=!(end<on ? (hour>=on || hour<end) : (hour>=on && hour<end));
        } else hidden[id]=m.load32(kWorld+1040+4*off)!=on;
    }
    return hidden;
}
void select_variants(float cx,float cy,float lod) {
    if (state.selection_valid && cx==state.selected_x && cy==state.selected_y && lod==state.selected_lod) return;
    for(auto &pass:state.selected) pass.clear();
    for(const auto &v:state.catalog.variants)
        if(v.id()>=32 && v.selected(cx,cy,lod)) state.selected[v.pass].push_back(&v);
    state.selected_x=cx;state.selected_y=cy;state.selected_lod=lod;state.selection_valid=true;
}
float origin_x(unsigned x,unsigned y) {return -2400+62.5f+125*x-(y&1)*62.5f;}
float origin_y(unsigned y) {return -2000+54.125f+108.25f*y;}
void rebase(Memory &m,std::uint32_t instance,float dx,float dy) {
    for (unsigned i=0;i<2;++i) {
        const auto p=instance+52+4*i, w=m.load32(p);
        const float f=std::bit_cast<float>((w&0xFFFFFF)<<8)+(i?dy:dx);
        m.store32(p,(w&0xFF000000)|(std::bit_cast<std::uint32_t>(f)>>8));
    }
}
std::uint32_t make_sector(Arena &a,std::uint32_t record,const Visibility &hidden) {
    auto &m=a.m;
    if (!m.contains(record,36) || !m.contains(m.load32(record+32),52)) return record;
    const auto root=m.load32(record+32), x=m.load32(record+8), y=m.load32(record+12);
    if (x>=64 || y>=kRows) return record;
    auto copy=a.clone(record,36), out=a.clone(root,52);
    m.store32(copy+32,out);m.store32(copy+28,1);
    // This synthetic sector has a fully populated resource table. It owns no
    // streaming overlays (nor extra chunk pointers following this record).
    m.store32(out,0);m.store16(out+4,0);m.store16(out+44,0);m.store32(out+48,0);
    auto &live=state.live_instances;
    const auto memory=m.aot_fast_view();
    for (unsigned pass=0;pass<8;++pass) {
        auto begin=m.load32(root+8+4*pass),end=m.load32(root+12+4*pass);
        if (end<begin || (end-begin)%68 || end-begin>10000*68 || !m.contains(begin,end-begin))
            throw std::runtime_error("Invalid live world instance list");
        live.clear();
        for (auto p=begin;p<end;p+=68) live.push_back({{memory.aot_load16(p)&0x7FFF,memory.aot_load16(p+2)},p});
        // Flat storage avoids allocating tree nodes for every frame/sector.
        // The old map retained the last record for duplicate (ID, resource).
        std::sort(live.begin(),live.end(),[](const auto &a,const auto &b){
            return a.first!=b.first?a.first<b.first:a.second>b.second;
        });
        live.erase(std::unique(live.begin(),live.end(),[](const auto &a,const auto &b){return a.first==b.first;}),live.end());
        // Instance lists must be contiguous (68-byte stride), including empty
        // passes. Alignment applies only to the start of the combined array.
        if (pass==0) a.cursor=(a.cursor+15)&~15u;
        m.store32(out+8+4*pass,a.cursor);
        for (auto [key,p]:live) if (key.first<32) {
            if (a.cursor+68>a.end) throw std::runtime_error("Native world instances exceed arena");
            copy_guest(m,a.cursor,p,68);a.cursor+=68;++state.count;
        }
        for (const auto *selected:state.selected[pass]) {
            const auto id=selected->id();
            // Original sectors can contain multiple mesh resources with the
            // same ID/pass. They are separate components, not alternatives.
            const auto p=a.cursor;
            if (p+68>a.end) throw std::runtime_error("Native world instances exceed arena");
            a.cursor+=68;++state.count;
            const auto rid=selected->resource();
            const InstanceKey key{id,rid};
            auto it=std::lower_bound(live.begin(),live.end(),key,[](const auto &entry,const auto &key){return entry.first<key;});
            if (it!=live.end() && it->first==key) copy_guest(m,p,it->second,68);
            else {
                m.copy_in(p,selected->data);
                rebase(m,p,origin_x(selected->x,selected->y)-origin_x(x,y),origin_y(selected->y)-origin_y(y));
            }
            m.store16(p,id|((hidden[id] || !m.load32(state.resource_table+12*rid))?0x8000:0));
        }
    }
    m.store32(out+40,a.cursor);
    return copy;
}

void render_return(psprecomp::Runtime &rt,psprecomp::AllegrexContext &ctx) {
    if (!state.active) throw std::runtime_error("Native world return without a render invocation");
    auto &m=rt.memory();
    state.queue_head=m.load32(state.clone+11768);
    if (state.queue_head<state.queue || state.queue_head>state.queue_end)
        throw std::runtime_error("Native world transparent queue overflow");
    // The original draw mutates animation phases; retain them across LOD mode
    // changes. Stream ownership, dynamic entities and PSP allocator state are
    // never redirected to the native copies.
    for (unsigned offset:{1468u,1476u,1480u}) m.store32(state.original_world+offset,m.load32(state.clone+offset));
    auto &frame=state.frames[state.slot];
    frame.commands.resize(state.catalog.resources);
    const auto memory=m.aot_fast_view();
    for(unsigned rid=0;rid<state.catalog.resources;++rid)frame.commands[rid]=memory.aot_load32(state.resource_table+12*rid+4);
    frame.end=state.command_cursor;
    ctx.gpr[31]=state.return_pc;ctx.pc=state.return_pc;state.active=false;
}

void allocate_commands(psprecomp::Runtime &rt,psprecomp::AllegrexContext &ctx) {
    auto &m=rt.memory();
    // Both the mesh chain and its texture subchains belong to this frame's
    // resource table. Discarding that table must not leak PSP heap allocations.
    const auto ra=ctx.gpr[31];
    if (state.active && ctx.gpr[4]==0x08BC7230 &&
        (ra==0x08955744 || ra==0x0895AE88)) {
        Arena a{m,state.command_cursor,state.command_end};
        ctx.gpr[2]=ctx.gpr[5]?a.allocate(ctx.gpr[5]):0;
        state.command_cursor=a.cursor;ctx.pc=ra;return;
    }
    // Original allocator entry, retaining normal allocation semantics for
    // streaming, actors and all other callers.
    ctx.gpr[29]-=16;
    m.store32(ctx.gpr[29],ctx.gpr[16]);m.store32(ctx.gpr[29]+4,ctx.gpr[17]);
    m.store32(ctx.gpr[29]+8,ra);ctx.gpr[16]=ctx.gpr[4];
    ctx.pc=ctx.gpr[5]?0x08ABE594:0x08ABE5A8;
}

void render(psprecomp::Runtime &rt,psprecomp::AllegrexContext &ctx) {
    auto &m=rt.memory();
    const float lod=std::clamp(vcs_configuration().draw_distance.lod,kMinGraphicsDistance,kMaxGraphicsDistance);
    const auto world=ctx.gpr[4],pass=ctx.gpr[5];
    if (state.active) throw std::runtime_error("Nested native world render");
    if (pass==0) {
        state.enabled=false;
        const auto level=m.load32(world+24);
        if (lod>1 && world==kWorld && level && m.load32(world+604)==0 &&
            m.contains(level,736) && m.contains(kResourceBase,32*1024*1024)) {
            const auto resources=m.load32(level+300);
            if (resources==6167 || resources==5917) {
                if (state.level!=level || state.catalog.resources!=resources) {
                    state.catalog=read_catalog(state.directory/(resources==6167?"MAINLA.wld":"BEACH.wld"));
                    if (state.catalog.resources!=resources) throw std::runtime_error("World cache does not match live level");
                    m.copy_in(kResourceBase,state.catalog.blob);
                    for (auto p:state.catalog.relocations) m.store32(kResourceBase+p,kResourceBase+m.load32(kResourceBase+p));
                    state.level=level;
                    state.frames={};
                    state.selection_valid=false;
                    std::cerr<<"[world-lod] loaded resources="<<resources<<" bytes="<<state.catalog.blob.size()
                             <<" variants="<<state.catalog.variants.size()<<'\n';
                }
                state.slot=(state.slot+1)%3;
                const auto base=kSlotBase+state.slot*kSlotBytes;
                Arena a{m,base,base+kCommandOffset};
                state.clone=a.clone(world,kWorldBytes);
                auto level_copy=a.clone(level,736);
                state.resource_table=a.clone(m.load32(level),resources*12);
                m.store32(state.clone+24,level_copy);m.store32(level_copy,state.resource_table);
                std::vector<std::uint32_t> signature;
                signature.reserve(resources*3);
                const auto memory=m.aot_fast_view();
                for (unsigned rid=0;rid<resources;++rid) {
                    auto p=state.resource_table+12*rid;
                    const auto original_chain=memory.aot_load32(p+4);
                    memory.aot_store32(p+4,0);
                    if (!memory.aot_load32(p) && state.catalog.pointers[rid]) {
                        memory.aot_store32(p,kResourceBase+state.catalog.pointers[rid]);
                    }
                    signature.push_back(memory.aot_load32(p));signature.push_back(memory.aot_load32(p+8));
                    signature.push_back(original_chain);
                }
                auto &frame=state.frames[state.slot];
                const bool reuse=frame.signature==signature && frame.commands.size()==resources;
                if(reuse)for(unsigned rid=0;rid<resources;++rid)memory.aot_store32(state.resource_table+12*rid+4,frame.commands[rid]);
                else frame.signature=std::move(signature);
                state.count=0;
                select_variants(f32(m,kCamera),f32(m,kCamera+4),lod);
                const auto hidden=resolve_visibility(m,level,ctx.gpr[28]);
                auto current=make_sector(a,m.load32(world+580),hidden);
                auto previous=make_sector(a,m.load32(world+588),hidden);
                m.store32(state.clone+580,current);m.store32(state.clone+588,previous);
                state.queue=a.allocate((state.count+128)*12);
                state.queue_end=a.cursor;
                state.queue_head=state.queue;
                m.store32(state.clone+11764,state.queue);m.store32(state.clone+11768,state.queue);
                const auto ring=a.allocate(8192*4);
                m.store32(state.clone+1440,ring);m.store32(state.clone+1460,8192);
                for(unsigned off:{1444u,1448u,1452u,1456u})m.store32(state.clone+off,0);
                state.command_cursor=reuse?frame.end:base+kCommandOffset;
                state.command_end=base+kSlotBytes;
                state.enabled=true;
                if (state.last_lod!=lod || (std::getenv("PSPRECOMP_WORLD_LOD_TRACE") && state.last_count!=state.count)) {
                    std::cerr<<"[world-lod] lod="<<lod<<" instances="<<state.count<<" working_bytes="<<a.cursor-base
                             <<" clone=0x"<<std::hex<<state.clone<<std::dec<<'\n';
                    state.last_count=state.count;state.last_lod=lod;
                }
            }
        }
    }
    if (state.enabled && world==kWorld && pass<=2) {
        state.original_world=world;state.return_pc=ctx.gpr[31];state.active=true;
        ctx.gpr[4]=state.clone;ctx.gpr[31]=kReturn;
    }
    // Exact prologue of 0x08958D28. Resume at the original AOT wait/ready
    // branch; a saved synthetic RA unwinds the clone after all draw passes.
    ctx.gpr[29]-=624;
    m.store32(ctx.gpr[29]+576,std::bit_cast<std::uint32_t>(ctx.fpr[20]));
    for (unsigned i=0;i<8;++i) m.store32(ctx.gpr[29]+580+4*i,ctx.gpr[16+i]);
    m.store32(ctx.gpr[29]+612,ctx.gpr[30]);m.store32(ctx.gpr[29]+616,ctx.gpr[31]);
    ctx.gpr[16]=0x08BC7230;
    ctx.gpr[6]=m.load8(ctx.gpr[16]+301);
    ctx.gpr[21]=ctx.gpr[4];m.store32(ctx.gpr[29]+500,ctx.gpr[5]);
    ctx.pc=ctx.gpr[6]?0x08958D88:0x08958D70;
}
} // namespace
std::filesystem::path world_lod_cache_directory() {
    if(const char *directory=std::getenv("PSPRECOMP_WORLD_CATALOG"))return directory;
    const auto directory=vcs_configuration().executable_directory/"NativeWorld";
    if(std::filesystem::is_regular_file(directory/"MAINLA.wld") &&
       std::filesystem::is_regular_file(directory/"BEACH.wld"))return directory;
    return {};
}
void install_world_lod(psprecomp::Runtime &runtime) {
    const auto directory=world_lod_cache_directory();
    if (directory.empty()) return;
    state=State{};state.directory=directory;
    if (!runtime.has_function(kRender) || runtime.has_function(kReturn)) throw std::runtime_error("World LOD hook address unavailable");
    runtime.register_function(kRender,render,"vcs_native_world_render");
    runtime.register_function(kReturn,render_return,"vcs_native_world_return");
    runtime.register_function(0x08ABE57C,allocate_commands,"vcs_native_world_commands");
}
} // namespace vcs
