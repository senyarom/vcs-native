set(VCS_PROFILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT EXISTS "${VCS_PROFILE_DIR}/game/generated/generated_registry.cpp")
    message(FATAL_ERROR
        "Local game code is missing. Run tools/import_game.py with a supported "
        "ULUS10160 1.03 ISO before building VCSNative.")
endif()
if(APPLE)
    link_libraries("-framework CoreGraphics")
endif()

if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"
        AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 17
        AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18
        AND (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64" OR "arm64" IN_LIST CMAKE_OSX_ARCHITECTURES))
    message(FATAL_ERROR
        "Apple Clang 17 miscompiles VCS AOT unit 0143 on ARM64 (machine instruction "
        "verifier: virtual register definition does not dominate its uses). "
        "Use LLVM Clang 21 with the macOS SDK; tools/build.sh configures it. "
        "Do not disable AOT optimization to hide this failure.")
endif()

set(VCS_GENERATED_DEBUG_OPTION "-g0")
if(PSPRECOMP_ENABLE_SANITIZERS)
    set(VCS_GENERATED_DEBUG_OPTION "-g")
endif()

# Keep the enormous generated AOT corpus out of MSVC whole-program/LTCG IR by
# default.  Stage 45.8/45.9 cross-unit state makes whole-program analysis of all
# 234 generated units extremely memory hungry on Windows.  The AOT units are
# still fully optimized per translation unit; host/runtime code may still use
# PSPRECOMP_LTO.
option(PSPRECOMP_VCS_AOT_LTO
    "Include generated VCS AOT units in MSVC whole-program optimization" OFF)

option(PSPRECOMP_PROFILE_GUIDED_AOT
    "Optimize the measured VCS hot AOT units above the base generated level" ON)
set(PSPRECOMP_HOT_GENERATED_OPT_LEVEL "1" CACHE STRING
    "Optimization level for profile-guided VCS AOT units (0, 1, 2 or 3)")
set_property(CACHE PSPRECOMP_HOT_GENERATED_OPT_LEVEL PROPERTY STRINGS 0 1 2 3)
if(NOT PSPRECOMP_HOT_GENERATED_OPT_LEVEL MATCHES "^[0-3]$")
    message(FATAL_ERROR "PSPRECOMP_HOT_GENERATED_OPT_LEVEL must be 0, 1, 2 or 3")
endif()

# Full /Ob3 is valuable at runtime but several large, cold units still make
# MSVC spend minutes and close to a gigabyte per compiler process. Keep those
# units buildable, then override this level only for the measured hot corpus.
set(PSPRECOMP_GENERATED_INLINE_LEVEL "0" CACHE STRING
    "MSVC /Ob level for generated VCS AOT units (0, 1, 2 or 3)")
set_property(CACHE PSPRECOMP_GENERATED_INLINE_LEVEL PROPERTY STRINGS 0 1 2 3)
if(NOT PSPRECOMP_GENERATED_INLINE_LEVEL MATCHES "^[0-3]$")
    message(FATAL_ERROR "PSPRECOMP_GENERATED_INLINE_LEVEL must be 0, 1, 2 or 3")
endif()
set(PSPRECOMP_HOT_GENERATED_INLINE_LEVEL "3" CACHE STRING
    "MSVC /Ob level for measured hot VCS AOT units (0, 1, 2 or 3)")
set_property(CACHE PSPRECOMP_HOT_GENERATED_INLINE_LEVEL PROPERTY STRINGS 0 1 2 3)
if(NOT PSPRECOMP_HOT_GENERATED_INLINE_LEVEL MATCHES "^[0-3]$")
    message(FATAL_ERROR "PSPRECOMP_HOT_GENERATED_INLINE_LEVEL must be 0, 1, 2 or 3")
endif()

file(GLOB VCS_GENERATED CONFIGURE_DEPENDS "${VCS_PROFILE_DIR}/game/generated/*.cpp")
if(MSVC)
    if(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "0")
        set(VCS_GENERATED_MSVC_OPT "/Od")
    elseif(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "3")
        set(VCS_GENERATED_MSVC_OPT "/Ox")
    else()
        set(VCS_GENERATED_MSVC_OPT "/O${PSPRECOMP_GENERATED_OPT_LEVEL}")
    endif()
    set(VCS_GENERATED_MSVC_OPTIONS
        "${VCS_GENERATED_MSVC_OPT};/Ob${PSPRECOMP_GENERATED_INLINE_LEVEL};/bigobj;${PSPRECOMP_MSVC_MP_FLAG}")
    if(PSPRECOMP_LTO AND NOT PSPRECOMP_VCS_AOT_LTO)
        # CMAKE_INTERPROCEDURAL_OPTIMIZATION adds /GL at target scope.  /GL- on
        # these sources overrides it so the linker receives native .obj code
        # instead of 234 giant LTCG IR modules.
        string(APPEND VCS_GENERATED_MSVC_OPTIONS ";/GL-")
    endif()
    set_source_files_properties(${VCS_GENERATED} PROPERTIES COMPILE_OPTIONS
        "${VCS_GENERATED_MSVC_OPTIONS}")
else()
    set_source_files_properties(${VCS_GENERATED} PROPERTIES COMPILE_OPTIONS
        "-O${PSPRECOMP_GENERATED_OPT_LEVEL};${VCS_GENERATED_DEBUG_OPTION}")
endif()

if(PSPRECOMP_PROFILE_GUIDED_AOT)
    # This unit is hot in the current capture and finishes /Ob3 without
    # triggering MSVC's pathological optimizer growth. It also contains the
    # validated native collision leaf at guest PC 0x088B1554. Other measured
    # units remain /Ob0 until their oversized helpers are split/noinline.
    set(VCS_HOT_UNIT_IDS
        0043)
    set(VCS_HOT_SOURCES)
    foreach(VCS_UNIT_ID IN LISTS VCS_HOT_UNIT_IDS)
        set(VCS_UNIT_SOURCE "${VCS_PROFILE_DIR}/game/generated/generated_unit_${VCS_UNIT_ID}.cpp")
        if(NOT EXISTS "${VCS_UNIT_SOURCE}")
            message(FATAL_ERROR "VCS profile-guided AOT unit is missing: ${VCS_UNIT_SOURCE}")
        endif()
        list(APPEND VCS_HOT_SOURCES "${VCS_UNIT_SOURCE}")
    endforeach()
    if(MSVC)
        if(PSPRECOMP_HOT_GENERATED_OPT_LEVEL STREQUAL "0")
            set(VCS_HOT_MSVC_OPT "/Od")
        elseif(PSPRECOMP_HOT_GENERATED_OPT_LEVEL STREQUAL "3")
            set(VCS_HOT_MSVC_OPT "/Ox")
        else()
            set(VCS_HOT_MSVC_OPT "/O${PSPRECOMP_HOT_GENERATED_OPT_LEVEL}")
        endif()
        set(VCS_HOT_MSVC_OPTIONS
            "${VCS_HOT_MSVC_OPT};/Ob${PSPRECOMP_HOT_GENERATED_INLINE_LEVEL};/bigobj;${PSPRECOMP_MSVC_MP_FLAG}")
        if(PSPRECOMP_LTO AND NOT PSPRECOMP_VCS_AOT_LTO)
            string(APPEND VCS_HOT_MSVC_OPTIONS ";/GL-")
        endif()
        set_source_files_properties(${VCS_HOT_SOURCES} PROPERTIES COMPILE_OPTIONS
            "${VCS_HOT_MSVC_OPTIONS}")
    else()
        set_source_files_properties(${VCS_HOT_SOURCES} PROPERTIES COMPILE_OPTIONS
            "-O${PSPRECOMP_HOT_GENERATED_OPT_LEVEL};${VCS_GENERATED_DEBUG_OPTION}")
    endif()
endif()

if(MSVC AND PSPRECOMP_NATIVE_AVX2)
    # MSVC 19.44's AVX2 optimizer crashes (C1001/c2.dll) on this translation
    # unit, while the identical /Ox /Ob0 compile completes in ~3 s with AVX.
    # Keep AVX2 for the host and the other AOT units; append the weaker ISA only
    # to the known compiler-bug trigger (the last /arch switch wins in cl.exe).
    set(PSPRECOMP_VCS_MSVC_AVX_FALLBACK_UNITS "0018;0022;0035;0089;0091;0117;0164" CACHE STRING
        "Generated VCS units forced to AVX because MSVC 19.44 crashes with AVX2")
    set(VCS_MSVC_AVX_FALLBACK_SOURCES)
    foreach(VCS_UNIT_ID IN LISTS PSPRECOMP_VCS_MSVC_AVX_FALLBACK_UNITS)
        set(VCS_UNIT_SOURCE "${VCS_PROFILE_DIR}/game/generated/generated_unit_${VCS_UNIT_ID}.cpp")
        if(NOT EXISTS "${VCS_UNIT_SOURCE}")
            message(FATAL_ERROR "VCS AVX fallback unit is missing: ${VCS_UNIT_SOURCE}")
        endif()
        list(APPEND VCS_MSVC_AVX_FALLBACK_SOURCES "${VCS_UNIT_SOURCE}")
    endforeach()
    set_property(SOURCE ${VCS_MSVC_AVX_FALLBACK_SOURCES} APPEND PROPERTY
        COMPILE_OPTIONS "/arch:AVX")
endif()

set(PSPRECOMP_HOST_OPT_LEVEL "2" CACHE STRING
    "Optimization level for the PSP host implementation (0, 1, 2 or 3)")
set_property(CACHE PSPRECOMP_HOST_OPT_LEVEL PROPERTY STRINGS 0 1 2 3)
if(NOT PSPRECOMP_HOST_OPT_LEVEL MATCHES "^[0-3]$")
    message(FATAL_ERROR "PSPRECOMP_HOST_OPT_LEVEL must be 0, 1, 2 or 3")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties("${VCS_PROFILE_DIR}/host/vcs_profile.cpp" PROPERTIES
        COMPILE_OPTIONS "-O${PSPRECOMP_HOST_OPT_LEVEL}")
endif()

set(VCS_GPU_BACKEND_SOURCE graphics/ge_gpu_backend_dx12.cpp)
if(NOT WIN32)
    include("${VCS_PROFILE_DIR}/cmake/VulkanBackend.cmake")
    set(VCS_GPU_BACKEND_SOURCE)
endif()
set(VCS_HDR_POST_SOURCE graphics/vcs_hdr_post_dx12_stub.cpp)
if(WIN32)
    set(VCS_WINDOW_SOURCE host/display_window.cpp)
    set(VCS_DEFAULT_CONFIG config/VCSNative.ini)
    set(VCS_DEFAULT_SHADER_CONFIG config/ProperShaders.ini)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VCS_SDL REQUIRED IMPORTED_TARGET sdl2)
    find_package(PNG REQUIRED)
    set_property(SOURCE graphics/texture_replacements.cpp APPEND PROPERTY INCLUDE_DIRECTORIES ${PNG_INCLUDE_DIRS})
    set_property(SOURCE graphics/texture_replacements.cpp APPEND PROPERTY COMPILE_DEFINITIONS PSPRECOMP_PNG_TEXTURES=1)
    add_library(vcs_graphics_ui STATIC
        graphics/graphics_settings_sdl.cpp
        third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
        third_party/imgui/backends/imgui_impl_sdl2.cpp
        third_party/imgui/backends/imgui_impl_sdlrenderer2.cpp)
    target_include_directories(vcs_graphics_ui PRIVATE host graphics third_party/imgui third_party/imgui/backends)
    target_link_libraries(vcs_graphics_ui PRIVATE PkgConfig::VCS_SDL)
    set(VCS_WINDOW_SOURCE host/display_window_sdl.cpp)
    set(VCS_DEFAULT_CONFIG config/VCSNative.sdl.ini)
    set(VCS_DEFAULT_SHADER_CONFIG config/ProperShaders.sdl.ini)
    set_property(SOURCE host/display_window_sdl.cpp host/audio_output.cpp
        APPEND PROPERTY INCLUDE_DIRECTORIES ${VCS_SDL_INCLUDE_DIRS})
    set_property(SOURCE host/audio_output.cpp
        APPEND PROPERTY COMPILE_DEFINITIONS PSPRECOMP_USE_SDL=1)
endif()
set(VCS_HOST_SOURCES
    host/vcs_bootstrap_paths.cpp
    host/vcs_profile.cpp
    host/vcs_native_fast_paths.cpp
    graphics/framebuffer_capture.cpp
    ${VCS_WINDOW_SOURCE}
    host/audio_output.cpp
    host/vcs_config.cpp
    graphics/graphics_settings.cpp
    graphics/graphics_settings_runtime.cpp
    host/vcs_camera_input.cpp
    host/vcs_vehicle_input.cpp
    graphics/vcs_fps_overlay.cpp
    graphics/texture_replacements.cpp
    host/vcs_media_decoder.cpp
    graphics/vcs_project2dfx.cpp
    graphics/vcs_project2dfx_lights.cpp
    graphics/vcs_world_streaming.cpp
    graphics/vcs_world_lod.cpp
    graphics/vcs_draw_distance_patch.cpp
    host/vcs_runtime_log.cpp
    ${VCS_HDR_POST_SOURCE}
    graphics/ge_renderer.cpp
    ${VCS_GPU_BACKEND_SOURCE}
    graphics/dx12_presenter.cpp
)

add_subdirectory(patches)

function(vcs_target_common target)
    target_include_directories(${target} PRIVATE
        "${VCS_PROFILE_DIR}/host" "${VCS_PROFILE_DIR}/graphics"
        "${VCS_PROFILE_DIR}/game/generated")
    target_link_libraries(${target} PRIVATE psprecomp_core vcs_patches ${CMAKE_DL_LIBS})
    if(WIN32)
        target_include_directories(${target} PRIVATE
            "${VCS_PROFILE_DIR}/third_party/ffmpeg/include")
        foreach(VCS_FFMPEG_LIB avcodec avformat avutil swresample swscale)
            target_link_libraries(${target} PRIVATE
                "${VCS_PROFILE_DIR}/third_party/ffmpeg/lib/${VCS_FFMPEG_LIB}.lib")
        endforeach()
        target_link_libraries(${target} PRIVATE winmm d3d12 dxgi d3dcompiler dxguid)
    else()
        # Use headers and libraries from the same installation. The bundled
        # Windows FFmpeg 7 headers are not ABI-compatible with FFmpeg 8 on macOS.
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(VCS_FFMPEG REQUIRED IMPORTED_TARGET
            libavcodec libavformat libavutil libswresample libswscale)
        target_link_libraries(${target} PRIVATE PkgConfig::VCS_FFMPEG)
        target_link_libraries(${target} PRIVATE ${VCS_SDL_LINK_LIBRARIES})
        target_link_libraries(${target} PRIVATE vcs_vulkan vcs_graphics_ui)
        target_link_libraries(${target} PRIVATE ${PNG_LIBRARIES})
    endif()
    psprecomp_enable_host_avx(${target})
endfunction()


function(vcs_set_runtime_output target)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
endfunction()

set(VCS_APP_RESOURCES)
if(WIN32)
    list(APPEND VCS_APP_RESOURCES resources/VCSNative.rc)
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(PSPRECOMP_BUILD_PROFILE_TESTS)
    add_test(NAME vcs_import_game_tests COMMAND "${Python3_EXECUTABLE}"
        "${VCS_PROFILE_DIR}/tests/import_game_tests.py")
endif()
set(VCS_BOOT_IMAGE_CPP "${CMAKE_CURRENT_BINARY_DIR}/game_initial_memory.cpp")
add_custom_command(OUTPUT "${VCS_BOOT_IMAGE_CPP}"
    COMMAND "${Python3_EXECUTABLE}" "${VCS_PROFILE_DIR}/tools/embed_boot_image.py"
        "${VCS_PROFILE_DIR}/game/bootstrap" "${VCS_BOOT_IMAGE_CPP}"
    DEPENDS tools/embed_boot_image.py game/bootstrap/initial-memory.bin game/bootstrap/metadata.json
    VERBATIM)
add_library(vcs_boot_image STATIC host/vcs_boot_image.cpp "${VCS_BOOT_IMAGE_CPP}")
target_include_directories(vcs_boot_image PUBLIC "${VCS_PROFILE_DIR}/host")
target_link_libraries(vcs_boot_image PUBLIC psprecomp_core)

add_executable(VCSNative
    ${VCS_APP_RESOURCES}
    host/main.cpp
    ${VCS_HOST_SOURCES}
    ${VCS_GENERATED}
)
vcs_target_common(VCSNative)
target_link_libraries(VCSNative PRIVATE vcs_boot_image)
vcs_set_runtime_output(VCSNative)
if(APPLE)
    target_link_options(VCSNative PRIVATE "-Wl,-stack_size,0x4000000")
endif()
if(MSVC)
    # /Ob3 must stay off the target: target_compile_options lands in the vcxproj
    # AdditionalOptions, which cl.exe sees *after* the per-source
    # InlineFunctionExpansion, so a target-wide /Ob3 silently overrides the
    # /Ob${PSPRECOMP_GENERATED_INLINE_LEVEL} set on the generated corpus above
    # (MSVC reports this as "D9025: overriding '/Ob0' with '/Ob3'").  Apply it to
    # the host sources only.
    target_compile_options(VCSNative PRIVATE ${PSPRECOMP_MSVC_MP_FLAG})
    set_source_files_properties(host/main.cpp ${VCS_HOST_SOURCES}
        DIRECTORY "${VCS_PROFILE_DIR}" PROPERTIES COMPILE_OPTIONS
        "$<$<CONFIG:Release>:/Ob3>;$<$<CONFIG:RelWithDebInfo>:/Ob3>")
    target_link_options(VCSNative PRIVATE /STACK:67108864)
    if(PSPRECOMP_LTO)
        # Make the otherwise silent LTCG phase visible in the console.
        target_link_options(VCSNative PRIVATE /LTCG:STATUS /INCREMENTAL:NO)
        if(NOT PSPRECOMP_MSVC_CGTHREADS STREQUAL "0")
            target_link_options(VCSNative PRIVATE "/CGTHREADS:${PSPRECOMP_MSVC_CGTHREADS}")
        endif()
    endif()
endif()

if(WIN32)
    add_custom_command(TARGET VCSNative POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${VCS_PROFILE_DIR}/third_party/ffmpeg/bin" "$<TARGET_FILE_DIR:VCSNative>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VCS_PROFILE_DIR}/third_party/ffmpeg/COPYING.LGPLv2.1"
            "$<TARGET_FILE_DIR:VCSNative>/COPYING.LGPLv2.1")
endif()
if(PSPRECOMP_BUILD_PROFILE_TESTS)
    if(NOT PSPRECOMP_BUILD_TESTS)
        enable_testing()
    endif()
    # Exercise the actual generated function, keeping its complete control-flow
    # graph: reducing it changes instruction selection and can hide the bug.
    # Foreign AOT entries are unreachable with chaining disabled; abort if that
    # assumption changes instead of silently succeeding through a stub.
    set(VCS_AOT_RESUME_SOURCE "${VCS_PROFILE_DIR}/game/generated/generated_unit_0143.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VCS_AOT_RESUME_SOURCE}")
    file(READ "${VCS_AOT_RESUME_SOURCE}" VCS_AOT_RESUME_CODE)
    string(REGEX MATCHALL "recomp_unit_[0-9]+_entry" VCS_AOT_RESUME_ENTRIES "${VCS_AOT_RESUME_CODE}")
    list(REMOVE_DUPLICATES VCS_AOT_RESUME_ENTRIES)
    list(REMOVE_ITEM VCS_AOT_RESUME_ENTRIES recomp_unit_0143_entry)
    set(VCS_AOT_RESUME_STUBS "#include \"psprecomp/runtime.hpp\"\n#include <cstdlib>\nnamespace psprecomp {\n")
    foreach(VCS_AOT_ENTRY IN LISTS VCS_AOT_RESUME_ENTRIES)
        string(APPEND VCS_AOT_RESUME_STUBS
            "void ${VCS_AOT_ENTRY}(Runtime&, AllegrexContext&, std::uint16_t, GuestMemory::AotFastView&) { std::abort(); }\n")
    endforeach()
    string(APPEND VCS_AOT_RESUME_STUBS "}\n")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/aot_resume_stubs.cpp" CONTENT "${VCS_AOT_RESUME_STUBS}")
    add_executable(vcs_aot_resume_tests tests/aot_resume_tests.cpp
        "${VCS_AOT_RESUME_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/aot_resume_stubs.cpp")
    target_include_directories(vcs_aot_resume_tests PRIVATE game/generated)
    target_link_libraries(vcs_aot_resume_tests PRIVATE psprecomp_core)
    add_test(NAME vcs_aot_resume_tests COMMAND vcs_aot_resume_tests)
    set_tests_properties(vcs_aot_resume_tests PROPERTIES
        ENVIRONMENT "PSPRECOMP_NO_CHAIN=1" TIMEOUT 30)

    # Run the unmodified AOT camera caller and model visibility routines. This
    # catches a hook registered at a local goto label and tests actual culling.
    set(VCS_DISTANCE_AOT_SOURCES game/generated/generated_unit_0076.cpp game/generated/generated_unit_0080.cpp
        game/generated/generated_unit_0136.cpp game/generated/generated_unit_0170.cpp)
    set(VCS_DISTANCE_ENTRIES "")
    foreach(VCS_DISTANCE_SOURCE IN LISTS VCS_DISTANCE_AOT_SOURCES)
        file(READ "${VCS_PROFILE_DIR}/${VCS_DISTANCE_SOURCE}" VCS_DISTANCE_CODE)
        string(REGEX MATCHALL "recomp_unit_[0-9]+_entry" VCS_DISTANCE_FOUND "${VCS_DISTANCE_CODE}")
        list(APPEND VCS_DISTANCE_ENTRIES ${VCS_DISTANCE_FOUND})
    endforeach()
    list(REMOVE_DUPLICATES VCS_DISTANCE_ENTRIES)
    list(REMOVE_ITEM VCS_DISTANCE_ENTRIES recomp_unit_0076_entry recomp_unit_0080_entry recomp_unit_0136_entry recomp_unit_0170_entry)
    set(VCS_DISTANCE_STUBS "#include \"psprecomp/runtime.hpp\"\n#include <cstdlib>\nnamespace psprecomp {\n")
    foreach(VCS_DISTANCE_ENTRY IN LISTS VCS_DISTANCE_ENTRIES)
        string(APPEND VCS_DISTANCE_STUBS "void ${VCS_DISTANCE_ENTRY}(Runtime&, AllegrexContext&, std::uint16_t, GuestMemory::AotFastView&) { std::abort(); }\n")
    endforeach()
    string(APPEND VCS_DISTANCE_STUBS "}\n")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/distance_stubs.cpp" CONTENT "${VCS_DISTANCE_STUBS}")
    add_executable(vcs_world_streaming_tests tests/world_streaming_tests.cpp graphics/vcs_world_streaming.cpp)
    target_include_directories(vcs_world_streaming_tests PRIVATE graphics)
    target_link_libraries(vcs_world_streaming_tests PRIVATE psprecomp_core)
    add_test(NAME vcs_world_streaming_tests COMMAND vcs_world_streaming_tests)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME vcs_world_streaming_data_tests COMMAND "${Python3_EXECUTABLE}"
        "${VCS_PROFILE_DIR}/tests/world_streaming_data_tests.py")

    add_executable(vcs_draw_distance_tests tests/draw_distance_tests.cpp ${VCS_DISTANCE_AOT_SOURCES}
        "${CMAKE_CURRENT_BINARY_DIR}/distance_stubs.cpp" graphics/vcs_draw_distance_patch.cpp
        host/vcs_config.cpp host/vcs_camera_input.cpp host/vcs_vehicle_input.cpp)
    target_include_directories(vcs_draw_distance_tests PRIVATE game/generated host graphics)
    target_link_libraries(vcs_draw_distance_tests PRIVATE psprecomp_core)
    add_test(NAME vcs_draw_distance_tests COMMAND vcs_draw_distance_tests)
    set_tests_properties(vcs_draw_distance_tests PROPERTIES TIMEOUT 30)

    add_executable(vcs_world_lod_tests tests/world_lod_tests.cpp graphics/vcs_world_lod.cpp
        host/vcs_config.cpp host/vcs_camera_input.cpp host/vcs_vehicle_input.cpp)
    target_include_directories(vcs_world_lod_tests PRIVATE host graphics)
    target_link_libraries(vcs_world_lod_tests PRIVATE psprecomp_core)
    add_test(NAME vcs_world_lod_tests COMMAND vcs_world_lod_tests)
    add_test(NAME vcs_world_catalog_tests COMMAND "${Python3_EXECUTABLE}"
        "${VCS_PROFILE_DIR}/tests/world_catalog_tests.py")
    add_test(NAME vcs_packaging_tests COMMAND "${Python3_EXECUTABLE}"
        "${VCS_PROFILE_DIR}/tests/packaging_tests.py")

    add_executable(vcs_profile_tests tests/vcs_profile_tests.cpp ${VCS_HOST_SOURCES})
    vcs_target_common(vcs_profile_tests)
    vcs_set_runtime_output(vcs_profile_tests)
    add_test(NAME vcs_profile_tests COMMAND vcs_profile_tests)

    add_executable(vcs_atrac_stream_tests tests/atrac_stream_tests.cpp ${VCS_HOST_SOURCES})
    vcs_target_common(vcs_atrac_stream_tests)
    vcs_set_runtime_output(vcs_atrac_stream_tests)
    add_test(NAME vcs_atrac_metadata_tests COMMAND vcs_atrac_stream_tests)
    # The disc dump is optional/private; synthetic metadata tests always run.
    foreach(clip NEWS_1 NEWS_2 CROWD CONSTRUCTION)
        set(atrac_fixture "${VCS_PROFILE_DIR}/assets/game/PSP_GAME/USRDIR/AUDIO/MUSIC/${clip}.AT3")
        if(EXISTS "${atrac_fixture}" AND NOT CMAKE_CROSSCOMPILING)
            add_test(NAME vcs_atrac_${clip}_tests COMMAND vcs_atrac_stream_tests "${atrac_fixture}")
        endif()
    endforeach()

    add_executable(vcs_config_tests
        tests/vcs_config_tests.cpp
        host/vcs_config.cpp
        host/vcs_camera_input.cpp
        host/vcs_vehicle_input.cpp)
    target_include_directories(vcs_config_tests PRIVATE host graphics)
    add_test(NAME vcs_config_tests COMMAND vcs_config_tests)

    add_executable(vcs_graphics_settings_tests tests/graphics_settings_tests.cpp
        graphics/graphics_settings.cpp host/vcs_config.cpp graphics/vcs_draw_distance_patch.cpp
        host/vcs_camera_input.cpp host/vcs_vehicle_input.cpp)
    target_include_directories(vcs_graphics_settings_tests PRIVATE host graphics)
    target_link_libraries(vcs_graphics_settings_tests PRIVATE psprecomp_core)
    add_test(NAME vcs_graphics_settings_tests COMMAND vcs_graphics_settings_tests)

    add_executable(audio_resampler_tests tests/audio_resampler_tests.cpp)
    target_include_directories(audio_resampler_tests PRIVATE host graphics)
    add_test(NAME audio_resampler_tests COMMAND audio_resampler_tests)

    add_executable(vcs_bootstrap_paths_tests
        tests/vcs_bootstrap_paths_tests.cpp host/vcs_bootstrap_paths.cpp)
    target_include_directories(vcs_bootstrap_paths_tests PRIVATE host graphics)
    target_link_libraries(vcs_bootstrap_paths_tests PRIVATE psprecomp_core)
    add_test(NAME vcs_bootstrap_paths_tests COMMAND vcs_bootstrap_paths_tests)
    add_executable(vcs_boot_image_tests tests/boot_image_tests.cpp)
    target_link_libraries(vcs_boot_image_tests PRIVATE vcs_boot_image)
    add_test(NAME vcs_boot_image_tests COMMAND vcs_boot_image_tests)
    if(EXISTS "${VCS_PROFILE_DIR}/assets/game/PSP_GAME/SYSDIR/EBOOT_DECRYPTED.ELF")
        add_test(NAME vcs_boot_image_reference_tests COMMAND vcs_boot_image_tests
            "${VCS_PROFILE_DIR}/assets/game/PSP_GAME/SYSDIR/EBOOT_DECRYPTED.ELF")
    endif()
    if(NOT WIN32)
        add_executable(vcs_texture_tests tests/texture_replacement_tests.cpp
            graphics/texture_replacements.cpp graphics/graphics_settings.cpp host/vcs_config.cpp
            host/vcs_camera_input.cpp host/vcs_vehicle_input.cpp)
        target_include_directories(vcs_texture_tests PRIVATE host graphics ${PNG_INCLUDE_DIRS})
        target_link_libraries(vcs_texture_tests PRIVATE psprecomp_core ${PNG_LIBRARIES})
        add_test(NAME vcs_texture_tests COMMAND vcs_texture_tests)
        add_executable(vcs_graphics_panel_tests tests/graphics_panel_tests.cpp
            graphics/graphics_settings.cpp host/vcs_config.cpp
            host/vcs_camera_input.cpp host/vcs_vehicle_input.cpp)
        target_include_directories(vcs_graphics_panel_tests PRIVATE host graphics third_party/imgui third_party/imgui/backends ${VCS_SDL_INCLUDE_DIRS})
        target_link_libraries(vcs_graphics_panel_tests PRIVATE vcs_graphics_ui psprecomp_core ${VCS_SDL_LINK_LIBRARIES})
        add_test(NAME vcs_graphics_panel_tests COMMAND vcs_graphics_panel_tests)
        add_executable(vcs_sdl_host_tests tests/sdl_host_tests.cpp
            host/display_window_sdl.cpp host/audio_output.cpp
            graphics/framebuffer_capture.cpp host/vcs_config.cpp graphics/graphics_settings.cpp
            host/vcs_camera_input.cpp host/vcs_vehicle_input.cpp)
        vcs_target_common(vcs_sdl_host_tests)
        target_include_directories(vcs_sdl_host_tests PRIVATE ${VCS_SDL_INCLUDE_DIRS})
        add_test(NAME vcs_sdl_host_tests COMMAND vcs_sdl_host_tests)
    endif()
endif()
