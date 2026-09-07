#pragma once
#include "psprecomp/guest_memory.hpp"

namespace vcs {
// Called once per game frame, on the guest thread. Only descriptors produced by
// tools/prepare_world_streaming.py are recognized; unmodified sectors are inert.
void update_world_streaming(psprecomp::GuestMemory &memory, float lod);
}
