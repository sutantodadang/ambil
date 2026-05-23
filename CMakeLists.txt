# ambil — CMake build (mirror of the Makefile)
#
# Quickstart:
#   cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-cmake -j
#
# Targets:
#   ambil                — the binary
#   ambil_test           — runs test/run_tests.sh
#   ambil_parity         — runs test/test_simd_parity.sh
#
# MSVC TODO: ambil uses GCC-specific intrinsics (`__builtin_cpu_supports`,
#   `__attribute__((target(...)))`, `__builtin_ctz`). Porting to MSVC requires
#   `__cpuid` for feature detection and replacing target attributes with
#   explicit per-TU /arch:AVX2 compilation. Out of scope for v1.0.
cmake_minimum_required(VERSION 3.15)
project(ambil C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Warnings + native arch (matches Makefile)
if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes)
    add_compile_options(-pthread)
    if(NOT DEFINED AMBIL_NATIVE OR AMBIL_NATIVE)
        add_compile_options(-march=native -funroll-loops)
    endif()
    add_compile_options(-D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L)
endif()

# Include layout mirrors -I flags in the Makefile.
include_directories(
    src
    src/core
    src/search
    src/io
    src/walk
    src/platform
    src/exec
    src/cmd
)

set(AMBIL_SOURCES
    src/cmd/main.c
    src/cmd/grep.c
    src/cmd/cat.c
    src/cmd/wc.c
    src/cmd/ls.c
    src/cmd/find.c

    src/core/util.c
    src/core/env.c
    src/core/help.c

    src/search/search.c
    src/search/simd_search.c
    src/search/re_tiny.c

    src/io/binary.c
    src/io/file_reader.c
    src/io/json_emit.c
    src/io/filetype.c

    src/walk/ignore.c
    src/walk/walker.c

    src/platform/platform_stat.c
    src/platform/platform.c

    src/exec/thread_pool.c
)

add_executable(ambil ${AMBIL_SOURCES})

# pthread link (Linux). On Windows-MinGW pthread is part of CFLAGS+LDFLAGS.
find_package(Threads QUIET)
if(Threads_FOUND)
    target_link_libraries(ambil PRIVATE Threads::Threads)
endif()

# Test targets (drive the same shell scripts the Makefile uses).
enable_testing()
add_test(NAME ambil_test
    COMMAND bash test/run_tests.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(ambil_test PROPERTIES
    ENVIRONMENT "BIN=${CMAKE_BINARY_DIR}/ambil")

add_test(NAME ambil_parity
    COMMAND bash test/test_simd_parity.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(ambil_parity PROPERTIES
    ENVIRONMENT "BIN=${CMAKE_BINARY_DIR}/ambil")

add_test(NAME ambil_chunked
    COMMAND bash test/test_chunked.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(ambil_chunked PROPERTIES
    ENVIRONMENT "BIN=${CMAKE_BINARY_DIR}/ambil")
