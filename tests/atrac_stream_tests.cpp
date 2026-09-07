#include "vcs_profile.hpp"
#include "vcs_media_decoder.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
constexpr std::uint32_t buffer = 0x08800000u, output = 0x08900000u, info = 0x08910000u;
constexpr std::array<std::uint8_t, 16> plus_guid{
    0xbf,0xaa,0x23,0xe9,0x58,0xcb,0x71,0x44,0xa1,0x19,0xff,0xfa,0x01,0xe4,0xce,0x62};
void require(bool ok, const char *text) { if (!ok) throw std::runtime_error(text); }
std::uint32_t call(psprecomp::Runtime &rt, std::uint32_t nid,
                   std::initializer_list<std::uint32_t> args,
                   const char *module = "sceAtrac3plus") {
    psprecomp::AllegrexContext ctx{};
    unsigned reg = 4;
    for (auto value : args) ctx.set_gpr(reg++, value);
    rt.invoke_import(module, nid, ctx);
    require(!rt.stopped(), "HLE import stopped the runtime");
    return ctx.gpr[2];
}
std::uint32_t le32(const std::vector<std::uint8_t> &b, std::size_t at) {
    std::uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= std::uint32_t(b.at(at+i)) << (8*i);
    return n;
}
void put16(std::vector<std::uint8_t> &b, std::size_t at, std::uint16_t v) {
    b.at(at) = v; b.at(at+1) = v >> 8;
}
void put32(std::vector<std::uint8_t> &b, std::size_t at, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) b.at(at+i) = v >> (8*i);
}
std::vector<std::uint8_t> header(std::uint16_t tag, std::uint16_t block, unsigned channels) {
    std::vector<std::uint8_t> b(256);
    std::memcpy(b.data(), "RIFF", 4); put32(b,4,96+block*10-8);
    std::memcpy(b.data()+8, "WAVEfmt ", 8); put32(b,16,52);
    put16(b,20,tag); put16(b,22,channels); put32(b,24,44100);
    put16(b,32,block); put16(b,36,34);
    std::copy(plus_guid.begin(),plus_guid.end(),b.begin()+44);
    std::memcpy(b.data()+72,"fact",4); put32(b,76,8);
    // Zero fact count exercises the frame-count fallback too.
    std::memcpy(b.data()+88,"data",4); put32(b,92,block*10);
    return b;
}
void metadata_tests() {
    psprecomp::Runtime rt;
    vcs::install_profile(rt, 0x08E8AC00u);
    for (const auto &[tag, block, channels, samples, bitrate] :
         std::array<std::array<unsigned,5>,5>{{{0xfffe,280,1,2048,48},
                                              {0xfffe,280,2,2048,48},
                                              {0xfffe,376,2,2048,64},
                                              {0xfffe,560,2,2048,96},
                                              {0x270,192,2,1024,66}}}) {
        auto bytes = header(tag,block,channels);
        rt.memory().copy_in(buffer,bytes);
        const auto id = call(rt,0x0FAE370E,{buffer,256,4096});
        require(id < 6,"valid ATRAC header rejected");
        require(call(rt,0xA554A158,{id,info}) == 0,"bitrate query failed");
        const auto actual_bitrate = rt.memory().load32(info);
        require(call(rt,0xA2BBA8BE,{id,info,0,0}) == 0,"sample count query failed");
        const auto actual_samples = rt.memory().load32(info)+1;
        require(call(rt,0x2DD3E298,{id,samples*3,info}) == 0,"reset info failed");
        const auto offset = rt.memory().load32(info+12);
        std::cout << "metadata block=" << block << " bitrate=" << actual_bitrate
                  << " total_samples=" << actual_samples << " reset_offset=" << offset << '\n';
        require(actual_bitrate == bitrate && actual_samples == samples*10 && offset == 96+block*3,
                "ATRAC format/frame size/seek accounting mismatch");
        require(call(rt,0x61EB33F5,{id}) == 0,"release failed");
    }
    for (unsigned malformed : {0u,1u,2u}) {
        auto bytes = header(0xfffe,280,1);
        if (malformed == 0) bytes[44] ^= 1; // Unknown subformat, not ATRAC3+.
        if (malformed == 1) put16(bytes,36,0); // Missing extensible fields.
        if (malformed == 2) put32(bytes,16,16); // Short fmt chunk.
        rt.memory().copy_in(buffer,bytes);
        require(call(rt,0x0FAE370E,{buffer,256,4096}) == 0x80630006u,
                "malformed/unknown extensible format accepted as ATRAC");
    }
}
void register_directory(psprecomp::Runtime &rt, const std::filesystem::path &dir) {
    rt.set_game_root(dir);
    const std::array<std::uint8_t,8> name{'d','i','s','c','0',':','/',0};
    rt.memory().copy_in(info,name);
    const auto fd = call(rt,0xB29DDF9C,{info},"IoFileMgrForUser");
    require(fd < 0x80000000u,"directory open failed");
    for (unsigned i=0; i<4096; ++i) {
        const auto r = call(rt,0xE3EB004C,{fd,info+256},"IoFileMgrForUser");
        require(r < 0x80000000u,"directory enumeration failed");
        if (r == 0) { call(rt,0xEB092469,{fd},"IoFileMgrForUser"); return; }
    }
    throw std::runtime_error("directory enumeration did not finish");
}
bool asset_test(const std::filesystem::path &path, unsigned capacity,
                const std::filesystem::path &artifacts) {
    std::ifstream in(path,std::ios::binary);
    require(bool(in),"asset open failed");
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in),{}};
    std::uint32_t data_offset=0, block=0, total=0, rate=0;
    for (std::size_t at=12; at+8<=bytes.size();) {
        const auto size=le32(bytes,at+4);
        if (!std::memcmp(bytes.data()+at,"fmt ",4)) {
            block=bytes.at(at+20) | unsigned(bytes.at(at+21))<<8;
            rate=le32(bytes,at+12);
        }
        if (!std::memcmp(bytes.data()+at,"fact",4)) total=le32(bytes,at+8);
        if (!std::memcmp(bytes.data()+at,"data",4)) { data_offset=at+8; break; }
        at+=8+size+(size&1);
    }
    require(data_offset && block && total && rate,"asset has no usable RIFF metadata");
    const auto frame_samples=2048u; // These real fixtures are independently identified ATRAC3+.
    // Keep guest input below the separate PCM region even for hour-long radio files.
    capacity=capacity ? capacity : std::min<std::size_t>(bytes.size(),512u*1024u);
    const auto initial=std::min<std::size_t>(capacity,bytes.size());
    psprecomp::Runtime rt;
    vcs::install_profile(rt,0x08E8AC00u);
    register_directory(rt,path.parent_path());
    rt.memory().copy_in(buffer,std::span(bytes.data(),initial));
    const auto id=call(rt,0x0FAE370E,{buffer,unsigned(initial),capacity});
    require(id<6,"real ATRAC set-halfway failed");
    vcs::AudioStreamDecoder reference;
    require(reference.open(path,rate,2,0),"reference decoder failed");
    std::ofstream trace, pcm;
    if (!artifacts.empty()) {
        std::filesystem::create_directories(artifacts);
        const auto stem=path.stem().string()+"-"+std::to_string(capacity);
        trace.open(artifacts/(stem+".csv"));
        pcm.open(artifacts/(stem+".s16le"),std::ios::binary);
        trace << "call,samples,position,loaded,remaining_frames,expected_frames,finished\n";
    }
    unsigned loaded=initial, calls=0, bad_size=0, bad_accounting=0, refills=0;
    std::uint64_t position=0, early_empty=0;
    bool finished=false;
    std::array<std::uint8_t,8192> got{}, expected{};
    while (!finished && calls < (total/1024+16)) {
        require(call(rt,0x6A8C3CD5,{id,output,info,info+4,info+8})==0,"decode failed");
        const auto samples=rt.memory().load32(info);
        const auto remaining=rt.memory().load32(info+8);
        finished=rt.memory().load32(info+4)!=0;
        ++calls;
        if (samples != std::min<std::uint64_t>(frame_samples,total-position)) ++bad_size;
        require(samples>0 && samples<=2048,"unexpected empty/oversized PCM before end");
        const auto count=samples*4;
        rt.memory().copy_out(output,std::span(got.data(),count));
        require(reference.read(std::span(expected.data(),count))==count,"reference ended early");
        require(std::equal(got.begin(),got.begin()+count,expected.begin()),"PCM skipped/repeated/changed samples");
        if (pcm.is_open()) pcm.write(reinterpret_cast<char*>(got.data()),count);
        position+=samples;
        const auto consumed=((position+frame_samples-1)/frame_samples)*block;
        const auto supplied=loaded-data_offset;
        const auto expected_frames=supplied>consumed ? (supplied-consumed)/block : 0;
        if (remaining!=expected_frames) ++bad_accounting;
        if (!remaining && position+frame_samples<total && loaded==bytes.size() && !early_empty)
            early_empty=position;
        if (trace.is_open()) trace << calls << ',' << samples << ',' << position << ',' << loaded
                                 << ',' << remaining << ',' << expected_frames << ',' << finished << '\n';
        // Refill through the same advertised ring-buffer windows the game uses.
        if (loaded < bytes.size()) {
            require(call(rt,0x5D268707,{id,info+16,info+20,info+24})==0,"stream info failed");
            const auto address=rt.memory().load32(info+16), size=rt.memory().load32(info+20);
            const auto offset=rt.memory().load32(info+24);
            require(offset==loaded && address>=buffer && address+size<=buffer+capacity,
                    "invalid refill range/offset");
            if (size) {
                require(std::uint64_t(offset)+size<=bytes.size(),"refill exceeds file");
                rt.memory().copy_in(address,std::span(bytes.data()+offset,size));
                require(call(rt,0x7DB31251,{id,size})==0,"add-stream failed");
                loaded+=size; ++refills;
            }
        }
    }
    require(finished && position==total,"clip failed to finish at its fact sample count");
    for (int n=0;n<3;++n) {
        require(call(rt,0x6A8C3CD5,{id,output,info,info+4,info+8})==0 &&
                rt.memory().load32(info)==0 && rt.memory().load32(info+4)==1,
                "finished clip replayed or produced extra PCM");
    }
    require(call(rt,0x61EB33F5,{id})==0,"release real stream failed");
    std::cout << path.filename().string() << " capacity=" << capacity << " samples=" << position
              << " seconds=" << double(position)/rate << " calls=" << calls << " refills=" << refills
              << " wrong_frame_size=" << bad_size << " wrong_buffer_count=" << bad_accounting
              << " early_empty_seconds=" << double(early_empty)/rate << " PCM=identical\n";
    return !bad_size && !bad_accounting && !early_empty;
}
}
int main(int argc,char **argv) {
    try {
        if (argc==1) { metadata_tests(); std::cout << "ATRAC metadata tests passed\n"; return 0; }
        require(argc==2 || argc==3,"usage: vcs_atrac_stream_tests [asset.at3 [artifact-directory]]");
        const auto path=std::filesystem::absolute(argv[1]);
        const auto artifacts=argc==3 ? std::filesystem::path(argv[2]) : std::filesystem::path{};
        bool ok=true;
        for (const auto capacity : {0u,4096u,16384u}) ok=asset_test(path,capacity,artifacts) && ok;
        return ok ? 0 : 1;
    } catch (const std::exception &e) { std::cerr << "ATRAC test failure: " << e.what() << '\n'; return 1; }
}
