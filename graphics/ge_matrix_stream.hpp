#pragma once
#include "ge_renderer.hpp"
#include <bit>
#include <cstddef>
#include <cstring>

namespace vcs {
// Consume only consecutive DATA writes to one matrix. The caller bounds the
// span by guest memory, GE stall address and execution budget. No state command
// or draw can be crossed. Reserved cursor slots discard writes but still advance.
inline std::size_t consume_ge_matrix_stream(GeTransformState &state, unsigned command,
                                            const std::uint8_t *words, std::size_t count) {
    float *matrix{};
    std::uint32_t *cursor{};
    unsigned size{}, mask = 15;
    switch (command) {
    case 0x2b:
        matrix = state.bones.data();
        cursor = &state.bone_cursor;
        size = 96;
        mask = 127;
        break;
    case 0x3b:
        matrix = state.world.data();
        cursor = &state.world_cursor;
        size = 12;
        break;
    case 0x3d:
        matrix = state.view.data();
        cursor = &state.view_cursor;
        size = 12;
        break;
    case 0x3f:
        matrix = state.projection.data();
        cursor = &state.projection_cursor;
        size = 16;
        break;
    case 0x41:
        matrix = state.texture.data();
        cursor = &state.texture_cursor;
        size = 12;
        break;
    default:
        return 0;
    }
    unsigned index = *cursor & mask;
    std::size_t consumed = 0;
    while (consumed < count) {
        std::uint32_t op;
        std::memcpy(&op, words + consumed * 4, 4);
        if constexpr (std::endian::native == std::endian::big)
            op = ((op & 255u) << 24) | ((op & 0xff00u) << 8) | ((op >> 8) & 0xff00u) | (op >> 24);
        if ((op >> 24) != command)
            break;
        if (index < size)
            matrix[index] = std::bit_cast<float>(op << 8);
        index = (index + 1) & mask;
        ++consumed;
    }
    *cursor = index;
    return consumed;
}
} // namespace vcs
