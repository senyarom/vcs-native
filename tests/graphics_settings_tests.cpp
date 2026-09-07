#include "graphics_settings.hpp"
#include "graphics_clock.hpp"
#include "ge_gpu_backend.hpp"
#include "psprecomp/runtime.hpp"
#include <bit>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace vcs { void install_draw_distance_patch(psprecomp::Runtime &, const std::filesystem::path &); }
static void require(bool b, const char *m) { if (!b) throw std::runtime_error(m); }
static void mipmap_parity() {
    // Scalar reference: clamp each of the four source texels independently
    // and round each channel once. Covers alpha and carry/rounding boundaries.
    for (unsigned width : {1,2,3,4,7,16,31,64,127,256})
    for (unsigned height : {1,2,3,5,8,17,32,129}) {
        std::uint32_t random = width * 65537 + height;
        std::vector<std::byte> source(std::size_t(width)*height*4);
        for (auto &byte : source) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            byte = std::byte(random & 255);
        }
        auto expected = source;
        unsigned w = width, h = height, count = 1;
        std::size_t previous = 0;
        while (w > 1 || h > 1) {
            const auto nw = std::max(1u,w/2), nh = std::max(1u,h/2);
            const auto next = expected.size();
            expected.resize(next+std::size_t(nw)*nh*4);
            for (unsigned y=0;y<nh;++y) for (unsigned x=0;x<nw;++x) for (unsigned c=0;c<4;++c) {
                unsigned sum = 0;
                for (unsigned dy=0;dy<2;++dy) for (unsigned dx=0;dx<2;++dx)
                    sum += std::to_integer<unsigned>(expected[previous +
                        (std::size_t(std::min(h-1,2*y+dy))*w + std::min(w-1,2*x+dx))*4+c]);
                expected[next+(std::size_t(y)*nw+x)*4+c] = std::byte((sum+2)/4);
            }
            previous=next; w=nw; h=nh; ++count;
        }
        unsigned levels = 99;
        vcs::generate_graphics_mipmaps(source,width,height,levels);
        require(source==expected && levels==count,"mip pixels must match scalar reference at every level");
        vcs::generate_graphics_mipmaps(source,width,height,levels);
        require(source==expected && levels==count,"rebuilding a mip chain must be idempotent");
    }
}
int main() {
    const auto temp=std::filesystem::temp_directory_path()/ ("vcs-graphics-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        mipmap_parity();
        std::filesystem::create_directory(temp);
        const auto ini=temp/"settings.ini", link=temp/"linked.ini";
        { std::ofstream f(ini); f << "; keep me\n[Controls]\nMouseSensitivity=17\n[Textures]\nDirectory=custom-pack\n[Rendering]\nInternalMode=PSP ; keep this comment\nAnisotropy=1\n[Timing]\nFrameRate=30\n[Other]\nMySetting=hello\n"; }
        std::filesystem::create_symlink(ini,link);
        auto c=vcs::load_vcs_configuration(ini); auto s=vcs::graphics_settings_from(c);
        vcs::graphics_preset(s,2); s.frame_rate=0; s.distance={10,10};
        std::string error;
        require(vcs::save_graphics_settings(link,s,error),error.c_str());
        require(std::filesystem::is_symlink(link),"save preserves app INI symlink");
        auto loaded=vcs::load_vcs_configuration(ini);
        require(loaded.timing.frame_rate==0 && loaded.rendering.texture_filter==vcs::TextureFilter::Bilinear && loaded.rendering.mipmapping==vcs::MipmapMode::On && loaded.rendering.mipmap_filter==vcs::MipmapFilter::Linear,"sampling and uncapped round trip");
        require(loaded.controls.mouse_sensitivity==17 && loaded.texture_directory=="custom-pack","unrelated settings preserved");
        require(loaded.draw_distance.world==10 && loaded.draw_distance.lod==10,"10x distances survive save and reload");
        require(loaded.rendering.internal_resolution_mode==vcs::InternalResolutionMode::Desktop && loaded.rendering.anisotropic_filtering==16,"aliases updated");
        const auto size=std::filesystem::file_size(ini);
        require(vcs::save_graphics_settings(link,s,error) && std::filesystem::file_size(ini)==size,"repeated save doesn't grow INI");
        std::ifstream in(ini); std::string contents((std::istreambuf_iterator<char>(in)),{});
        require(contents.find("keep this comment")!=std::string::npos && contents.find("MySetting=hello")!=std::string::npos,"comments and unknown settings preserved");
        for (float invalid : {0.99f,10.01f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
            s.distance={invalid,1};
            require(!vcs::save_graphics_settings(link,s,error) && std::filesystem::file_size(ini)==size,"invalid far clip cannot overwrite INI");
            s.distance={1,invalid};
            require(!vcs::save_graphics_settings(link,s,error) && std::filesystem::file_size(ini)==size,"invalid LOD cannot overwrite INI");
        }
        s=vcs::graphics_settings_from(loaded);
        require(!vcs::save_graphics_settings(temp/"missing"/"settings.ini",s,error),"write error reported");
        vcs::GeGpuDrawDescriptor d{}; d.texture_enabled=true;
        vcs::apply_graphics_sampling(d,s.rendering,false);
        require(d.texture_mipmap_enabled && d.texture_min_linear && d.texture_mag_linear && d.texture_mipmap_linear,"world policy applied");
        d={};d.texture_enabled=true;d.through=true;vcs::apply_graphics_sampling(d,s.rendering,false);
        require(!d.texture_mipmap_enabled && !d.texture_mag_linear,"HUD unchanged");
        d.through=false;vcs::apply_graphics_sampling(d,s.rendering,true);
        require(!d.texture_mipmap_enabled && !d.texture_mag_linear,"framebuffer effects unchanged");
        std::vector<std::byte> pixels(4*4*4,std::byte{200}); unsigned levels=1;
        vcs::generate_graphics_mipmaps(pixels,4,4,levels);
        require(levels==3 && pixels.size()==84 && pixels.back()==std::byte{200},"complete mip chain for un-mipped texture");
        vcs::GraphicsRealtimeClock clock; using Clock=vcs::GraphicsRealtimeClock::Clock;
        auto now=Clock::time_point{}; std::uint64_t guest=1000;clock.reset(guest,now);
        for (unsigned frame=1;frame<=500;++frame) clock.update(guest,now+std::chrono::microseconds(frame*2000));
        require(guest==1'001'000,"500 uncapped frames advance exactly one real second");
        clock.reset(guest,now+std::chrono::seconds(60));
        clock.update(guest,now+std::chrono::seconds(61));
        require(guest==2'001'000,"panel pause doesn't advance game clock");
        vcs::GraphicsFramePacer pacer;const auto start=Clock::time_point{};
        require(pacer.next(start,16683)==start,"first paced frame never waits");
        require(pacer.next(start+std::chrono::microseconds(8000),16683)==start+std::chrono::microseconds(16683),"fast frame waits only until its presentation deadline");
        require(pacer.next(start+std::chrono::microseconds(42000),16683)==start+std::chrono::microseconds(42000),"late frame cannot add another wait");
        require(pacer.next(start+std::chrono::microseconds(45000),16683)==start+std::chrono::microseconds(50049),"small overruns preserve the vblank phase");
        require(pacer.next(start+std::chrono::seconds(10),16683)==start+std::chrono::seconds(10),"large overrun skips expired vblanks");
        require(pacer.next(start+std::chrono::seconds(10),16683)==start+std::chrono::seconds(10)+std::chrono::microseconds(16683),"large overrun cannot replay a backlog");
        pacer.reset();
        auto wake=pacer.next(start,16683);
        for(unsigned i=1;i<=120;++i) {
            // A heavy rendered frame alternates with a cheap second vblank
            // in the stock 30-FPS loop; both must share the display phase.
            wake=pacer.next(wake+std::chrono::microseconds(i%2 ? 20000 : 2000),16683);
        }
        require(wake==start+std::chrono::microseconds(120*16683),"stock 30-FPS loop must not lose time after every heavy frame");
        pacer.reset();require(pacer.next(start+std::chrono::seconds(60),16683)==start+std::chrono::seconds(60),"resume resets the presentation deadline");
        require(pacer.next(start+std::chrono::seconds(60),33366)==start+std::chrono::seconds(60),"rate change resets deadline");
        std::filesystem::remove_all(temp);
        std::cout << "Graphics persistence, sampling, realtime clock passed\n";
        return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; std::filesystem::remove_all(temp);return 1; }
}
