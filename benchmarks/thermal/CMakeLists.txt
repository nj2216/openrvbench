add_executable(bench_thermal thermal_bench.cpp)
target_include_directories(bench_thermal PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(bench_thermal PRIVATE openrv_result openrv_monitor)
install(TARGETS bench_thermal RUNTIME DESTINATION lib/openrvbench/modules)
