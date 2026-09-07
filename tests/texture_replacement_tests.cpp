#include "texture_replacements.hpp"
#include "vcs_config.hpp"
#include <png.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

static void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    using namespace vcs;
    const auto root = std::filesystem::temp_directory_path() / "vcs_texture_replacement_tests";
    try {
        std::array<std::uint8_t, 2048> raw{};
        for (unsigned i=0;i<raw.size();++i) raw[i]=static_cast<std::uint8_t>(i*37+11);
        // Golden vectors from the independent PPSSPP scalar reference, not
        // values calculated by the implementation under test.
        for (auto [length,expected] : std::array<std::pair<unsigned,std::uint32_t>,6>{{
            {0,0x9535599c},{8,0x74bcf494},{16,0x212ba000},
            {64,0x9543e480},{128,0x6d186a8c},{1024,0x2b28cbbc}}})
            require(replacement_quick_hash(std::span(raw).first(length))==expected, "PPSSPP quick hash mismatch");
        require(replacement_quick_hash(std::span(raw).subspan(1,64),1)==0x3d013bc0,"unaligned hash mismatch");
        std::filesystem::create_directories(root);
        std::vector<std::byte> red(32*32*4);
        for (unsigned i=0;i<32*32;++i) {red[i*4]=std::byte{255};red[i*4+3]=std::byte{255};}
        png_image png{};png.version=PNG_IMAGE_VERSION;png.width=32;png.height=32;png.format=PNG_FORMAT_RGBA;
        require(png_image_write_to_file(&png,(root/"red.png").string().c_str(),0,red.data(),0,nullptr),"fixture PNG write failed");
        {std::ofstream out(root/"textures.ini");out << "[options]\nhash=quick\n[hashes]\n00000000000004042b28cbbc = red.png\n";}
        {std::ofstream out(root/"VCSNative.ini");out << "[Textures]\nEnabled=true\nDirectory=.\n";}
        setenv("PSPRECOMP_CONFIG",(root/"VCSNative.ini").string().c_str(),1);
        initialize_vcs_configuration(root);
        reset_texture_replacements();
        psprecomp::GuestMemory memory;
        constexpr std::uint32_t address=0x08810000;
        for (unsigned i=0;i<1024;++i) memory.store8(address+i,raw[i]);
        GeGpuDrawDescriptor draw{};draw.texture_address=address;
        draw.texture_width=draw.texture_height=draw.texture_buffer_width=16;
        draw.texture_format=3;
        std::array<std::uint32_t,256> commands{};commands[0xB8]=0xB8000404;
        std::uint32_t width=16,height=16,levels=1;
        std::vector<std::byte> pixels;
        require(replace_texture(memory,commands,draw,width,height,levels,pixels),"mapped PNG was not loaded");
        require(width==32 && height==32 && levels==6,"replacement dimensions/mip chain incorrect");
        require(pixels[0]==std::byte{255} && pixels[1]==std::byte{0} && pixels.back()==std::byte{255},"replacement mip pixels incorrect");
        require(draw.texture_width==16,"guest texture dimensions were modified");
        memory.store8(address,0);
        require(!replace_texture(memory,commands,draw,width,height,levels,pixels),"changed guest texture reused stale replacement");
        reset_texture_replacements();
        std::filesystem::remove_all(root);
        std::puts("Texture hash compatibility, PNG replacement, mip generation and invalidation passed");
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr,"%s\n",e.what());
        std::filesystem::remove_all(root);
        return 1;
    }
}
