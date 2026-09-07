#pragma once
#include "psprecomp/runtime.hpp"
namespace vcs {
// An explicit environment path overrides the packaged NativeWorld directory.
// An absent packaged cache keeps standalone/runtime tests on original RAM.
std::filesystem::path world_lod_cache_directory();
// Native static-world rendering uses the reserved upper 32 MiB. The PSP heap,
// stacks and streamed sectors retain their original lower-32-MiB ownership.
void install_world_lod(psprecomp::Runtime &runtime);
}
