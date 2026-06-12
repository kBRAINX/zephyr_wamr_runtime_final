cmake_minimum_required(VERSION 3.20)

set(WAMR_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../wasm-micro-runtime)

if(NOT EXISTS "${WAMR_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "WAMR introuvable dans ${WAMR_ROOT}\n"
        "  git clone https://github.com/bytecodealliance/wasm-micro-runtime.git")
endif()

set(WAMR_BUILD_PLATFORM      "zephyr" CACHE STRING "" FORCE)
set(WAMR_BUILD_TARGET        "XTENSA" CACHE STRING "" FORCE)
set(WAMR_BUILD_INTERP        1        CACHE BOOL   "" FORCE)
set(WAMR_BUILD_FAST_INTERP   1        CACHE BOOL   "" FORCE)
set(WAMR_BUILD_AOT           0        CACHE BOOL   "" FORCE)
set(WAMR_BUILD_JIT           0        CACHE BOOL   "" FORCE)
set(WAMR_BUILD_LIBC_WASI     0        CACHE BOOL   "" FORCE)
set(WAMR_BUILD_LIBC_BUILTIN  1        CACHE BOOL   "" FORCE)
set(WAMR_BUILD_SHARED_MEMORY 0        CACHE BOOL   "" FORCE)
set(WAMR_BUILD_BULK_MEMORY   1        CACHE BOOL   "" FORCE)
set(WAMR_BUILD_REF_TYPES     1        CACHE BOOL   "" FORCE)
# SHRUNK_MEMORY alloue ~1 MB par module (16 pages + overhead)
# au lieu de 1 page exacte (64 KB) — incompatible avec notre pool de 128 KB
set(WAMR_BUILD_SHRUNK_MEMORY 0        CACHE BOOL   "" FORCE)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyr_wamr_runtime)

include(${WAMR_ROOT}/build-scripts/runtime_lib.cmake)

target_sources(app PRIVATE
    src/main.c
    ${WAMR_RUNTIME_LIB_SOURCE}
)

target_include_directories(app PRIVATE
    ${WAMR_ROOT}/core/iwasm/include
    ${WAMR_ROOT}/core/shared/include
    ${WAMR_ROOT}/core/shared/platform/zephyr
    ${WAMR_ROOT}/core/shared/platform/include
    ${WAMR_ROOT}/core/shared/mem-alloc
    ${WAMR_ROOT}/core/shared/utils
    ${WAMR_ROOT}/core/iwasm/common
    ${WAMR_ROOT}/core/iwasm/interpreter
    ${WAMR_ROOT}/core/iwasm/libraries/libc-builtin
)