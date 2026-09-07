#pragma once
#include <array>
#include <cstdint>

namespace vcs {
// std140 contract with ge_model.glsl. Snapshot per draw; never points at guest
// memory. The backend supplies format[2], the byte offset in its raw buffer.
struct alignas(16) GeGpuModelLight {
    std::array<float, 4> vector{}, attenuation{}, spot{}, ambient{}, diffuse{}, specular{};
};
struct alignas(16) GeGpuModelState {
    std::array<std::uint32_t, 4> format{}, offsets{}, control{}, texture_control{};
    std::array<std::array<float, 4>, 3> world{}, texture{};
    std::array<std::array<float, 4>, 24> bones{};
    std::array<float, 4> base{}, ambient{}, diffuse{}, specular{}, emissive{}, global{}, shade_s{},
        shade_t{};
    std::array<GeGpuModelLight, 4> lights{};
};
static_assert(sizeof(GeGpuModelLight) == 96);
static_assert(sizeof(GeGpuModelState) == 1056);
} // namespace vcs
