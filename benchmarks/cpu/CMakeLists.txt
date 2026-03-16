add_executable(bench_cpu cpu_bench.cpp)
target_include_directories(bench_cpu PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(bench_cpu PRIVATE openrv_result openrv_monitor)

if(OpenMP_CXX_FOUND)
    target_link_libraries(bench_cpu PRIVATE OpenMP::OpenMP_CXX)
endif()

install(TARGETS bench_cpu RUNTIME DESTINATION lib/openrvbench/modules)
