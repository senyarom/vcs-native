#include "vcs_bootstrap_paths.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path temp =
        std::filesystem::temp_directory_path() /
        ("vcs_bootstrap_paths_" + std::to_string(nonce));

    try {
        // No SYSDIR or EBOOT exists: only game resources are required.
        std::filesystem::create_directories(temp / "PSP_DATA/PSP_GAME/USRDIR");

        const char *no_args[]{"VCSNative.exe"};
        const vcs::BootstrapPaths automatic =
            vcs::resolve_bootstrap_paths(1, no_args, temp);
        require(automatic.discovered_from_psp_data,
                "no-argument launch did not select PSP_DATA");
        require(automatic.game_root == temp / "PSP_DATA",
                "no-argument launch selected the wrong asset root");

        const char *explicit_args[]{"VCSNative.exe", "--game-root", "custom/root"};
        const vcs::BootstrapPaths explicit_paths =
            vcs::resolve_bootstrap_paths(3, explicit_args, temp);
        require(!explicit_paths.discovered_from_psp_data,
                "explicit command line unexpectedly selected PSP_DATA");
        require(explicit_paths.game_root == std::filesystem::path("custom/root"),
                "explicit game root was not preserved");

        const auto rejects = [&](int argc, const char *const *argv) {
            try { (void)vcs::resolve_bootstrap_paths(argc, argv, temp); }
            catch (const std::exception &) { return true; }
            return false;
        };
        const char *old_args[]{"VCSNative", "game.elf", "root"};
        require(rejects(3, old_args), "old ELF syntax silently treated as resources");
        require(rejects(2, explicit_args), "missing game-root value accepted");

        std::filesystem::remove_all(temp);
        require(rejects(1, no_args), "missing resource root accepted");
        std::cout << "vcs_bootstrap_paths_tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::error_code ignored;
        std::filesystem::remove_all(temp, ignored);
        std::cerr << "vcs_bootstrap_paths_tests failed: " << error.what() << '\n';
        return 1;
    }
}
