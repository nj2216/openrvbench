add_executable(bench_memory memory_bench.cpp)
target_include_directories(bench_memory PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(bench_memory PRIVATE openrv_result openrv_monitor)
install(TARGETS bench_memory RUNTIME DESTINATION lib/openrvbench/modules)
