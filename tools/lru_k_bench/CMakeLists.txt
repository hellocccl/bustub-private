set(LRU_K_BENCH_SOURCES lru_k_bench.cpp)
add_executable(lru_k_bench ${LRU_K_BENCH_SOURCES})

target_link_libraries(lru_k_bench bustub argparse)
set_target_properties(lru_k_bench PROPERTIES OUTPUT_NAME bustub-lru-k-bench)
