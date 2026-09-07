#pragma once

#include <filesystem>

namespace vcs {

struct BootstrapPaths {
    std::filesystem::path game_root;
    bool discovered_from_psp_data{};
};

// --game-root <directory>, or assets below <exe_dir>/PSP_DATA with no arguments.
[[nodiscard]] BootstrapPaths resolve_bootstrap_paths(
    int argc, const char *const *argv,
    const std::filesystem::path &executable_directory);

} // namespace vcs
