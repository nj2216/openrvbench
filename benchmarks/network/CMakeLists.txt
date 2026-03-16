add_executable(bench_network network_bench.cpp)
target_include_directories(bench_network PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(bench_network PRIVATE openrv_result openrv_monitor)
install(TARGETS bench_network RUNTIME DESTINATION lib/openrvbench/modules)
