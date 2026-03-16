add_executable(bench_ai ai_bench.cpp)
target_include_directories(bench_ai PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(bench_ai PRIVATE openrv_result openrv_monitor)
install(TARGETS bench_ai RUNTIME DESTINATION lib/openrvbench/modules)
