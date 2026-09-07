#include "vcs_bootstrap_paths.hpp"
#include "psprecomp/common.hpp"
#include <string_view>

namespace vcs {

BootstrapPaths resolve_bootstrap_paths(int argc, const char *const *argv,
                                      const std::filesystem::path &executable_directory) {
    if (argc > 1) {
        if (argc == 3 && argv && argv[1] && std::string_view(argv[1]) == "--game-root"
            && argv[2] && *argv[2])
            return {std::filesystem::path(argv[2]), false};
        throw psprecomp::Error("Usage: VCSNative [--game-root <extracted game directory>]\n"
                              "The game image is built in; an EBOOT argument is no longer used.");
    }
    const auto root = executable_directory / "PSP_DATA";
    std::error_code error;
    if (std::filesystem::is_directory(root / "PSP_GAME/USRDIR", error))
        return {root, true};
    throw psprecomp::Error("Game resources were not found at " + root.string() +
                          "/PSP_GAME/USRDIR. Use VCSNative --game-root <extracted game directory>.");
}

} // namespace vcs
