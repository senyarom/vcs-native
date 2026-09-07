# Execution, guest memory, PSP imports, ELF loading, decompression and hashing.
# Decoder/program analysis/code generation are optional tools, not runtime code.
add_library(psprecomp_core STATIC
    src/deflate.cpp src/elf32.cpp src/guest_memory.cpp
    src/nid_registry.cpp src/runtime.cpp src/sha256.cpp)
target_include_directories(psprecomp_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(psprecomp_core PRIVATE $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
if(MSVC)
    target_compile_options(psprecomp_core PRIVATE /W4 /permissive- /EHsc ${PSPRECOMP_MSVC_MP_FLAG}
        $<$<CONFIG:Release>:/Ob3> $<$<CONFIG:RelWithDebInfo>:/Ob3>)
else()
    target_compile_options(psprecomp_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
