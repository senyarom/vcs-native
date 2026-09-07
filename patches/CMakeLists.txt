add_library(vcs_patches STATIC mission_fps.cpp mission_fps_hooks.cpp weapon_timing.cpp swimming_timing.cpp)
target_include_directories(vcs_patches PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vcs_patches PUBLIC psprecomp_core)

if(PSPRECOMP_BUILD_PROFILE_TESTS)
    enable_testing()
    add_executable(vcs_mission_fps_tests tests/mission_fps_tests.cpp)
    target_link_libraries(vcs_mission_fps_tests PRIVATE vcs_patches)
    add_test(NAME vcs_mission_fps_tests COMMAND vcs_mission_fps_tests)
    set_tests_properties(vcs_mission_fps_tests PROPERTIES TIMEOUT 30)

    # Exercise the unchanged loader and its actual cross-unit call, not only
    # an artificial direct call to the registered hook.
    set(mission_aot_sources
        "${VCS_PROFILE_DIR}/game/generated/generated_unit_0023.cpp"
        "${VCS_PROFILE_DIR}/game/generated/generated_unit_0128.cpp"
        "${VCS_PROFILE_DIR}/game/generated/generated_unit_0174.cpp")
    set(mission_entries "")
    foreach(mission_source IN LISTS mission_aot_sources)
        file(READ "${mission_source}" mission_code)
        string(REGEX MATCHALL "recomp_unit_[0-9]+_entry" mission_found "${mission_code}")
        list(APPEND mission_entries ${mission_found})
    endforeach()
    list(REMOVE_DUPLICATES mission_entries)
    list(REMOVE_ITEM mission_entries recomp_unit_0023_entry recomp_unit_0128_entry recomp_unit_0174_entry)
    set(mission_stubs "#include \"psprecomp/runtime.hpp\"\n#include <cstdlib>\nnamespace psprecomp {\n")
    foreach(mission_entry IN LISTS mission_entries)
        string(APPEND mission_stubs "void ${mission_entry}(Runtime&, AllegrexContext&, std::uint16_t, GuestMemory::AotFastView&) { std::abort(); }\n")
    endforeach()
    string(APPEND mission_stubs "}\n")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mission_stubs.cpp" CONTENT "${mission_stubs}")
    add_executable(vcs_mission_fps_aot_tests tests/mission_fps_aot_tests.cpp
        ${mission_aot_sources} "${CMAKE_CURRENT_BINARY_DIR}/mission_stubs.cpp")
    target_include_directories(vcs_mission_fps_aot_tests PRIVATE "${VCS_PROFILE_DIR}/game/generated")
    target_link_libraries(vcs_mission_fps_aot_tests PRIVATE vcs_patches)
    add_test(NAME vcs_mission_fps_aot_tests COMMAND vcs_mission_fps_aot_tests)
    set_tests_properties(vcs_mission_fps_aot_tests PROPERTIES TIMEOUT 30)
    # Uses the user's extracted scripts in place; no game data becomes a fixture.
    if(EXISTS "${PROJECT_SOURCE_DIR}/assets/game/PSP_GAME/USRDIR/RUNDATA/MAIN.SCM")
        add_test(NAME vcs_mission_fps_real_scripts COMMAND vcs_mission_fps_tests
            "${PROJECT_SOURCE_DIR}/assets/game/PSP_GAME/USRDIR/RUNDATA/MAIN.SCM")
    endif()
endif()

if(PSPRECOMP_BUILD_PROFILE_TESTS)
    add_executable(vcs_weapon_timing_tests tests/weapon_timing_tests.cpp)
    target_link_libraries(vcs_weapon_timing_tests PRIVATE vcs_patches)
    add_test(NAME vcs_weapon_timing_tests COMMAND vcs_weapon_timing_tests)
endif()

if(PSPRECOMP_BUILD_PROFILE_TESTS)
    set(weapon_aot_sources "${VCS_PROFILE_DIR}/game/generated/generated_unit_0067.cpp"
                           "${VCS_PROFILE_DIR}/game/generated/generated_unit_0144.cpp"
                           "${VCS_PROFILE_DIR}/game/generated/generated_unit_0069.cpp"
                           "${VCS_PROFILE_DIR}/game/generated/generated_unit_0152.cpp")
    set(weapon_entries "")
    foreach(source IN LISTS weapon_aot_sources)
        file(READ "${source}" code)
        string(REGEX MATCHALL "recomp_unit_[0-9]+_entry" found "${code}")
        list(APPEND weapon_entries ${found})
    endforeach()
    list(REMOVE_DUPLICATES weapon_entries)
    list(REMOVE_ITEM weapon_entries recomp_unit_0067_entry recomp_unit_0144_entry recomp_unit_0069_entry recomp_unit_0152_entry)
    set(stubs "#include \"psprecomp/runtime.hpp\"\n#include <cstdlib>\nnamespace psprecomp {\n")
    foreach(entry IN LISTS weapon_entries)
        string(APPEND stubs "void ${entry}(Runtime&, AllegrexContext&, std::uint16_t, GuestMemory::AotFastView&) { std::abort(); }\n")
    endforeach()
    string(APPEND stubs "}\n")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/weapon_stubs.cpp" CONTENT "${stubs}")
    add_executable(vcs_weapon_timing_aot_tests tests/weapon_timing_aot_tests.cpp tests/swimming_timing_aot_tests.cpp tests/swimming_drag_aot_tests.cpp
        ${weapon_aot_sources} "${CMAKE_CURRENT_BINARY_DIR}/weapon_stubs.cpp")
    target_include_directories(vcs_weapon_timing_aot_tests PRIVATE "${VCS_PROFILE_DIR}/game/generated")
    target_link_libraries(vcs_weapon_timing_aot_tests PRIVATE vcs_patches)
    add_test(NAME vcs_weapon_timing_aot_tests COMMAND vcs_weapon_timing_aot_tests)
endif()
