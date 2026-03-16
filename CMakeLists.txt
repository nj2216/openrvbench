cmake_minimum_required(VERSION 3.16)
project(OpenRVBench VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# ─── Build options ────────────────────────────────────────────────────────────
option(ENABLE_RVV          "Enable RISC-V Vector Extension (RVV)"     ON)
option(ENABLE_OPENMP       "Enable OpenMP for multi-thread benchmarks" ON)
option(ENABLE_AI_BENCH     "Enable AI inference benchmarks"           OFF)
option(ENABLE_NETWORK_BENCH "Enable network benchmarks"               ON)
option(BUILD_TESTS         "Build unit tests"                         OFF)

# ─── Detect RISC-V architecture ───────────────────────────────────────────────
if(CMAKE_SYSTEM_PROCESSOR MATCHES "riscv64")
    set(IS_RISCV64 TRUE)
    message(STATUS "Target: RISC-V 64-bit detected")
    add_compile_definitions(RISCV64=1)

    if(ENABLE_RVV)
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-march=rv64gcv" COMPILER_SUPPORTS_RVV)
        if(COMPILER_SUPPORTS_RVV)
            message(STATUS "RVV: Compiler supports rv64gcv — vector benchmarks enabled")
            set(RVV_FLAGS "-march=rv64gcv -mabi=lp64d")
            add_compile_definitions(HAS_RVV=1)
        else()
            message(WARNING "RVV: Compiler does not support rv64gcv — disabling vector extension")
            set(ENABLE_RVV OFF)
        endif()
    endif()
else()
    set(IS_RISCV64 FALSE)
    message(STATUS "Target: Non-RISC-V (${CMAKE_SYSTEM_PROCESSOR}) — cross-compile or simulation mode")
endif()

# ─── Base compile flags ───────────────────────────────────────────────────────
add_compile_options(-O2 -Wall -Wextra -Wpedantic)

if(IS_RISCV64)
    add_compile_options(-march=rv64gc -mabi=lp64d)
endif()

# ─── OpenMP ───────────────────────────────────────────────────────────────────
if(ENABLE_OPENMP)
    find_package(OpenMP QUIET)
    if(OpenMP_CXX_FOUND)
        message(STATUS "OpenMP: Found — multi-thread benchmarks enabled")
        add_compile_definitions(HAS_OPENMP=1)
    else()
        message(WARNING "OpenMP: Not found — multi-thread scaling disabled")
    endif()
endif()

# ─── Shared monitoring library ────────────────────────────────────────────────
add_library(openrv_monitor STATIC
    monitoring/system_monitor.cpp
)
target_include_directories(openrv_monitor PUBLIC monitoring/)

# ─── Shared result library ────────────────────────────────────────────────────
add_library(openrv_result STATIC
    results/result_writer.cpp
)
target_include_directories(openrv_result PUBLIC results/)

# ─── Benchmark sub-modules ────────────────────────────────────────────────────
add_subdirectory(benchmarks/cpu)
add_subdirectory(benchmarks/memory)
add_subdirectory(benchmarks/crypto)
add_subdirectory(benchmarks/storage)
add_subdirectory(benchmarks/thermal)
add_subdirectory(benchmarks/extensions)

if(ENABLE_RVV)
    add_subdirectory(benchmarks/vector)
endif()

if(ENABLE_NETWORK_BENCH)
    add_subdirectory(benchmarks/network)
endif()

if(ENABLE_AI_BENCH)
    add_subdirectory(benchmarks/ai)
endif()

# ─── Install targets ──────────────────────────────────────────────────────────
install(PROGRAMS cli/openrvbench DESTINATION bin)
install(TARGETS
    bench_cpu bench_memory bench_crypto bench_storage bench_thermal
    RUNTIME DESTINATION lib/openrvbench/modules
)
