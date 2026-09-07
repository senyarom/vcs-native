# Vulkan implementation of the VCS profile GPU interface.
find_package(Vulkan REQUIRED)
find_program(PSPRECOMP_GLSLANG glslangValidator REQUIRED)
set(PSPRECOMP_SHADER_DIR "${CMAKE_BINARY_DIR}/ge-shaders")
file(MAKE_DIRECTORY "${PSPRECOMP_SHADER_DIR}")
foreach(stage vert frag)
    add_custom_command(OUTPUT "${PSPRECOMP_SHADER_DIR}/ge_${stage}_spv.hpp"
        COMMAND "${PSPRECOMP_GLSLANG}" -V --target-env vulkan1.1
            "${VCS_PROFILE_DIR}/graphics/shaders/ge.${stage}"
            --vn "psp_ge_${stage}_spv"
            -o "${PSPRECOMP_SHADER_DIR}/ge_${stage}_spv.hpp"
        DEPENDS "${VCS_PROFILE_DIR}/graphics/shaders/ge.${stage}"
        VERBATIM)
endforeach()
add_custom_command(OUTPUT "${PSPRECOMP_SHADER_DIR}/ge_packed_vert_spv.hpp"
    COMMAND "${PSPRECOMP_GLSLANG}" -V --target-env vulkan1.1 -DPACKED_0115=1
        "${VCS_PROFILE_DIR}/graphics/shaders/ge.vert" --vn psp_ge_packed_vert_spv
        -o "${PSPRECOMP_SHADER_DIR}/ge_packed_vert_spv.hpp"
    DEPENDS "${VCS_PROFILE_DIR}/graphics/shaders/ge.vert"
    VERBATIM)
add_custom_command(OUTPUT "${PSPRECOMP_SHADER_DIR}/ge_model_vert_spv.hpp"
    COMMAND "${PSPRECOMP_GLSLANG}" -V --target-env vulkan1.1 -DRAW_MODEL=1
        "${VCS_PROFILE_DIR}/graphics/shaders/ge.vert" --vn psp_ge_model_vert_spv
        -o "${PSPRECOMP_SHADER_DIR}/ge_model_vert_spv.hpp"
    DEPENDS "${VCS_PROFILE_DIR}/graphics/shaders/ge.vert" "${VCS_PROFILE_DIR}/graphics/shaders/ge_model.glsl"
    VERBATIM)
add_library(vcs_vulkan STATIC
    "${VCS_PROFILE_DIR}/graphics/ge_gpu_backend_vulkan.cpp"
    "${PSPRECOMP_SHADER_DIR}/ge_vert_spv.hpp"
    "${PSPRECOMP_SHADER_DIR}/ge_packed_vert_spv.hpp"
    "${PSPRECOMP_SHADER_DIR}/ge_model_vert_spv.hpp"
    "${PSPRECOMP_SHADER_DIR}/ge_frag_spv.hpp")
target_include_directories(vcs_vulkan PUBLIC "${PROJECT_SOURCE_DIR}/runtime/include" "${VCS_PROFILE_DIR}/host" "${VCS_PROFILE_DIR}/graphics")
target_include_directories(vcs_vulkan PRIVATE "${PSPRECOMP_SHADER_DIR}" ${Vulkan_INCLUDE_DIRS})
target_link_libraries(vcs_vulkan PRIVATE ${Vulkan_LIBRARY})
target_compile_features(vcs_vulkan PUBLIC cxx_std_20)

if(PSPRECOMP_BUILD_PROFILE_TESTS)
    add_executable(vcs_vulkan_tests "${VCS_PROFILE_DIR}/tests/vulkan_backend_tests.cpp"
        "${VCS_PROFILE_DIR}/host/vcs_config.cpp"
        "${VCS_PROFILE_DIR}/host/vcs_camera_input.cpp"
        "${VCS_PROFILE_DIR}/host/vcs_vehicle_input.cpp")
    target_link_libraries(vcs_vulkan_tests PRIVATE vcs_vulkan)
    add_test(NAME vcs_vulkan_tests COMMAND vcs_vulkan_tests)
endif()
