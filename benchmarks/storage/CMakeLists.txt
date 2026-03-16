add_executable(bench_storage storage_bench.cpp)
target_include_directories(bench_storage PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(bench_storage PRIVATE openrv_result openrv_monitor)
install(TARGETS bench_storage RUNTIME DESTINATION lib/openrvbench/modules)
