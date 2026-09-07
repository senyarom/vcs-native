set(PSPRECOMP_RECOMPILER_VENDOR "${PROJECT_SOURCE_DIR}/third_party/psprecomp")

add_library(psprecomp_analysis STATIC
    "${PSPRECOMP_RECOMPILER_VENDOR}/src/decoder.cpp"
    "${PSPRECOMP_RECOMPILER_VENDOR}/src/program_analysis.cpp")
target_include_directories(psprecomp_analysis PUBLIC "${PSPRECOMP_RECOMPILER_VENDOR}/include")
target_link_libraries(psprecomp_analysis PUBLIC psprecomp_core)
add_executable(psp_recomp "${PSPRECOMP_RECOMPILER_VENDOR}/tools/codegen_main.cpp")
target_link_libraries(psp_recomp PRIVATE psprecomp_analysis)
if(VCS_BUILD_RECOMPILER)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(VCS_ANALYSIS_PATCH_ROOT "${CMAKE_CURRENT_BINARY_DIR}/patched_psprecomp")
    set(VCS_ANALYSIS_HEADER
        "${VCS_ANALYSIS_PATCH_ROOT}/include/psprecomp/program_analysis.hpp")
    set(VCS_ANALYSIS_SOURCE "${VCS_ANALYSIS_PATCH_ROOT}/src/program_analysis.cpp")
    add_custom_command(OUTPUT "${VCS_ANALYSIS_HEADER}" "${VCS_ANALYSIS_SOURCE}"
        COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tools/recompiler/prepare_program_analysis.py"
            "${PSPRECOMP_RECOMPILER_VENDOR}/include/psprecomp/program_analysis.hpp"
            "${PSPRECOMP_RECOMPILER_VENDOR}/src/program_analysis.cpp"
            "${VCS_ANALYSIS_HEADER}" "${VCS_ANALYSIS_SOURCE}"
        DEPENDS
            "${PROJECT_SOURCE_DIR}/tools/recompiler/prepare_program_analysis.py"
            "${PSPRECOMP_RECOMPILER_VENDOR}/include/psprecomp/program_analysis.hpp"
            "${PSPRECOMP_RECOMPILER_VENDOR}/src/program_analysis.cpp"
        VERBATIM)
    add_custom_target(vcs_recomp_analysis_sources
        DEPENDS "${VCS_ANALYSIS_HEADER}" "${VCS_ANALYSIS_SOURCE}")
    add_library(vcs_recomp_analysis STATIC
        "${PSPRECOMP_RECOMPILER_VENDOR}/src/decoder.cpp" "${VCS_ANALYSIS_SOURCE}")
    add_dependencies(vcs_recomp_analysis vcs_recomp_analysis_sources)
    target_include_directories(vcs_recomp_analysis PUBLIC
        "${VCS_ANALYSIS_PATCH_ROOT}/include" "${PSPRECOMP_RECOMPILER_VENDOR}/include")
    target_link_libraries(vcs_recomp_analysis PUBLIC psprecomp_core)

    set(VCS_RECOMPILER_UPSTREAM_SOURCE
        "${PSPRECOMP_RECOMPILER_VENDOR}/profiles/vcs/tools/vcs_codegen_main.cpp")
    set(VCS_RECOMPILER_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/vcs_codegen_main.cpp")
    add_custom_command(OUTPUT "${VCS_RECOMPILER_SOURCE}"
        COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tools/recompiler/prepare_vcs_codegen.py"
            "${VCS_RECOMPILER_UPSTREAM_SOURCE}" "${VCS_RECOMPILER_SOURCE}"
        DEPENDS "${VCS_RECOMPILER_UPSTREAM_SOURCE}"
            "${PROJECT_SOURCE_DIR}/tools/recompiler/prepare_vcs_codegen.py"
        VERBATIM)
    add_executable(psp_analyze "${PSPRECOMP_RECOMPILER_VENDOR}/tools/analyzer_main.cpp")
    add_executable(dump_function "${PSPRECOMP_RECOMPILER_VENDOR}/tools/dump_function.cpp")
    add_executable(vcs_recomp "${VCS_RECOMPILER_SOURCE}")
    add_dependencies(vcs_recomp vcs_recomp_analysis_sources)
    foreach(target psp_analyze dump_function)
        target_link_libraries(${target} PRIVATE psprecomp_analysis)
    endforeach()
    target_link_libraries(vcs_recomp PRIVATE vcs_recomp_analysis)
endif()
if(PSPRECOMP_BUILD_TESTS)
    add_executable(psprecomp_tests "${PROJECT_SOURCE_DIR}/tests/runtime/test_main.cpp")
    target_link_libraries(psprecomp_tests PRIVATE psprecomp_analysis)
    add_dependencies(psprecomp_tests psp_recomp)
    if(CMAKE_CROSSCOMPILING)
        # Keep the target test suite relocatable when copied to Steam Deck.
        target_compile_definitions(psprecomp_tests PRIVATE PSPRECOMP_CODEGEN_PATH="./psp_recomp")
    else()
        target_compile_definitions(psprecomp_tests PRIVATE PSPRECOMP_CODEGEN_PATH="$<TARGET_FILE:psp_recomp>")
    endif()
    add_test(NAME psprecomp_tests COMMAND psprecomp_tests)
    if(CMAKE_CROSSCOMPILING)
        set_tests_properties(psprecomp_tests PROPERTIES WORKING_DIRECTORY "$<TARGET_FILE_DIR:psp_recomp>")
    endif()
endif()
