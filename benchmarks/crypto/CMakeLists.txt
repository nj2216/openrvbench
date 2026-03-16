add_executable(bench_crypto crypto_bench.cpp)
target_include_directories(bench_crypto PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(bench_crypto PRIVATE openrv_result openrv_monitor)
install(TARGETS bench_crypto RUNTIME DESTINATION lib/openrvbench/modules)
