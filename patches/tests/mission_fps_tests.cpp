#include "mission_fps.hpp"
#include "mission_fps_hooks.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

using vcs::patches::MissionFpsPatches;
using psprecomp::GuestMemory;
namespace {
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
constexpr std::uint32_t base = 0x09000000;
const std::vector<std::uint8_t> boom{0x08,0,0xe2,0x9b,0x09,0x8f,0xc2,0x75,0x3c};
const std::vector<std::uint8_t> air{0x08,0,0xfb,0x6f,0x01,0x09,0x04,0x40,0x3f};
const std::vector<std::uint8_t> truck{0x2f,4,0x11,8,0x58,0x1b,0x4c,1,0x11,8,0x58,0x1b};
const std::vector<std::uint8_t> lance{0x0b,0,0xe3,0x0d,7,0x0b};
void copy(GuestMemory &m, std::uint32_t addr, const std::vector<std::uint8_t> &bytes) {
    for (auto byte : bytes) m.store8(addr++, byte);
}
std::uint32_t u32(const std::vector<std::uint8_t> &b, std::size_t o) {
    require(o + 4 <= b.size(), "fixture bounds");
    return unsigned(b[o]) | unsigned(b[o+1])<<8 | unsigned(b[o+2])<<16 | unsigned(b[o+3])<<24;
}

void transitions() {
    GuestMemory m; MissionFpsPatches patch;
    auto bytes = boom; bytes.insert(bytes.end(),truck.begin(),truck.end()); bytes.insert(bytes.end(),lance.begin(),lance.end());
    copy(m,base,bytes); patch.load(m,base,bytes.size());
    require(patch.patch_count()==1,"only frame-dependent script float is changed");
    const auto original = u32(boom,5), half=std::bit_cast<std::uint32_t>(std::bit_cast<float>(original)*.5f);
    for (unsigned fps : {30,60,60,0,120,240,30,60,30}) {
        patch.update(m,fps,true,true,false);
        require(m.load32(base+5)==(fps==30?original:half),"float restored without cumulative scaling");
        require(m.load16(base+13)==7000,"truck maximum health stays original at every FPS");
        require(m.load16(base+19)==7000,"truck current health stays original at every FPS");
        require(m.load8(base+26)==11,"Lance health stays original at every FPS");
        require(patch.effective_fps(fps)==(fps==30?30:60),"known missions never run fixed-60 values uncapped");
    }
    patch.update(m,0,false,false,false);
    require(patch.patch_count()==0 && patch.effective_fps(0)==0,"mission end restores requested uncapped");
    for (unsigned i=0;i<bytes.size();++i) require(m.load8(base+i)==bytes[i],"mission exit restores complete original bytes");

    copy(m,base,air); patch.load(m,base,air.size());
    patch.update(m,0,true,true,true);
    require(patch.effective_fps(0)==30,"concert text plus active mission forces stock pacing");
    patch.update(m,0,true,false,true);
    require(patch.effective_fps(0)==60,"concert guard requires on-mission flag");
    patch.update(m,60,true,true,false);
    require(patch.effective_fps(60)==60,"missing concert text releases fallback");
    patch.update(m,30,true,true,true);
    require(m.load32(base+5)==u32(air,5),"explicit 30 restores stock concert operand");
}

void bounds_and_ownership() {
    GuestMemory m; MissionFpsPatches patch;
    copy(m,base,boom); patch.load(m,base,boom.size()-1);
    require(!patch.patch_count(),"signature must fit inside actual mission read");
    patch.load(m,base,boom.size()); patch.apply(m,60);
    m.store32(base+5,0x12345678); patch.apply(m,30);
    require(m.load32(base+5)==0x12345678,"external writer is not overwritten");
    copy(m,base,boom); patch.load(m,base,boom.size()); patch.apply(m,60);
    std::vector<std::uint8_t> unrelated(boom.size(),0x44);
    copy(m,base,unrelated); patch.load(m,base,unrelated.size()); patch.apply(m,30);
    require(m.load32(base+5)==0x44444444 && patch.effective_fps(0)==0,"reused script storage never receives old originals");
    copy(m,base,boom); copy(m,base+boom.size(),boom); patch.load(m,base,2*boom.size());
    require(!patch.patch_count(),"ambiguous signature is rejected");
    patch.load(m,0xfffffff0u,128); require(!patch.patch_count(),"invalid memory rejected");
    patch.load(m,base,0xffffffffu); require(!patch.patch_count(),"unbounded script size rejected");
}

void text_lookup() {
    GuestMemory m;
    constexpr unsigned gp=0x08BB1D60, text=base, entries=base+128, value=base+256;
    m.store32(gp-9752,text); m.store32(text,entries); m.store32(text+4,1);
    m.store32(entries,value); const char key[]="REN7_O9";
    for(unsigned i=0;i<8;++i)m.store8(entries+4+i,key[i]);
    m.store16(value,'X');
    require(vcs::patches::concert_text_present(m,gp),"CText main table matches the concert key");
    m.store16(value,0);
    require(!vcs::patches::concert_text_present(m,gp),"empty translation does not trigger fallback");
    m.store32(text+4,0);m.store32(text+16,entries);m.store32(text+20,1);m.store16(value,'X');
    require(!vcs::patches::concert_text_present(m,gp),"secondary table requires load flags");
    m.store8(text+33,1);m.store8(text+34,1);
    require(vcs::patches::concert_text_present(m,gp),"CText mission table matches the concert key");
    m.store32(text+20,0xffffffffu);
    require(!vcs::patches::concert_text_present(m,gp),"malformed CText table bounded");
}

void real_scripts(const char *filename) {
    std::ifstream in(filename,std::ios::binary);
    require(bool(in),"cannot open user MAIN.SCM");
    const std::vector<std::uint8_t> file{std::istreambuf_iterator<char>(in),{}};
    const auto main_size=u32(file,0);
    const auto globals_end=u32(file,11);
    const auto mission_header=u32(file,8+globals_end+3);
    const auto count=u32(file,8+mission_header+16);
    require(count==99 && main_size==0x3a4d6,"supported ULUS10160 1.03 script header");
    // Independently known byte offsets in this user's unmodified MAIN.SCM.
    // No game script is checked into the repository.
    const std::array<unsigned,8> floats{0xc2e18,0x1a70a5,0xf1764,0xf1781,0xf1791,0x1139f4,0x198bf1,0x4c560};
    GuestMemory memory; MissionFpsPatches patch;
    unsigned edited_missions=0, edit_count=0;
    for(unsigned i=0;i<count;++i) {
        const auto start=u32(file,8+mission_header+20+i*4)+8;
        const auto end=i+1<count?u32(file,8+mission_header+20+(i+1)*4)+8:unsigned(file.size());
        require(start<=end && end<=file.size(),"mission table bounds");
        std::vector<std::uint8_t> bytes(file.begin()+start,file.begin()+end), expected=bytes;
        for (auto offset:floats) if(offset>=start && offset+4<=end) {
            const auto fixed=std::bit_cast<unsigned>(std::bit_cast<float>(u32(file,offset))*.5f);
            for(unsigned j=0;j<4;++j)expected[offset-start+j]=(fixed>>(j*8))&255;
        }
        copy(memory,base,bytes); patch.load(memory,base,bytes.size());
        if(patch.patch_count())++edited_missions;
        edit_count+=patch.patch_count();
        for(auto fps:{60u,60u,30u,0u,30u}) {
            patch.apply(memory,fps);
            const auto &want=fps==30?bytes:expected;
            for(unsigned j=0;j<want.size();++j)require(memory.load8(base+j)==want[j],"real mission differs from independent expected patch bytes");
        }
    }
    require(edited_missions==6 && edit_count==8,"six script-timing missions and eight float operands found");
    std::cout<<"99 real mission scripts: 6 affected, 8 float operands; all health operands unchanged; repeated 60/30/uncapped changes restore exact originals\n";
}
} // namespace
int main(int argc,char **argv) {
    try {
        transitions();bounds_and_ownership();text_lookup();
        if(argc>1)real_scripts(argv[1]);
        std::cout<<"Mission FPS patches passed\n";
        return 0;
    } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
