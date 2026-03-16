add_executable(bench_vector vector_bench.cpp)
target_include_directories(bench_vector PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(bench_vector PRIVATE openrv_result openrv_monitor)

# Compile with RVV flags
if(COMPILER_SUPPORTS_RVV)
    target_compile_options(bench_vector PRIVATE -march=rv64gcv -mabi=lp64d)
endif()

install(TARGETS bench_vector RUNTIME DESTINATION lib/openrvbench/modules)
