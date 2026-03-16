add_executable(bench_extensions extensions_bench.cpp)
target_include_directories(bench_extensions PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(bench_extensions PRIVATE openrv_result openrv_monitor)

# Enable all detected extensions so the compiler can emit their instructions.
# On rv64gcv boards, this covers Zba/Zbb/Zbc/Zbs/Zfh/Zicond etc.
# The benchmark detects extensions at runtime from /proc/cpuinfo, so
# it's safe to compile with a wider march — the tests that need a
# specific extension check for it before running.
if(IS_RISCV64)
    # Try the full ISA string — if the board has it, compile for it
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(
        "-march=rv64gcv_zba_zbb_zbc_zbs_zfh_zicond"
        COMPILER_SUPPORTS_EXT_MARCH)

    if(COMPILER_SUPPORTS_EXT_MARCH)
        target_compile_options(bench_extensions PRIVATE
            -march=rv64gcv_zba_zbb_zbc_zbs_zfh_zicond
            -mabi=lp64d)
        message(STATUS "Extensions bench: full ISA march enabled")
    elseif(COMPILER_SUPPORTS_RVV)
        # Fallback: just RVV
        target_compile_options(bench_extensions PRIVATE
            -march=rv64gcv -mabi=lp64d)
    endif()
endif()

install(TARGETS bench_extensions RUNTIME DESTINATION lib/openrvbench/modules)
