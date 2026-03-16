// ─── OpenRVBench :: CPU Benchmark ────────────────────────────────────────────
// Measures: integer throughput, FP throughput, multi-thread scaling,
//           and a compression-style (LZ77 window) workload.
//
// Output: JSON BenchResult to stdout
// ─────────────────────────────────────────────────────────────────────────────
#include <iostream>
#include <vector>
#include <cmath>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <numeric>
#include <algorithm>
#include <random>
#include <functional>

#include "../../results/result_writer.h"

#ifdef HAS_OPENMP
#  include <omp.h>
#endif

using namespace openrv;
using Clock = std::chrono::high_resolution_clock;
using Seconds = std::chrono::duration<double>;

// ─── Helpers ──────────────────────────────────────────────────────────────────
static double elapsed(Clock::time_point t0) {
    return Seconds(Clock::now() - t0).count();
}

// Prevent dead-code elimination
static volatile uint64_t sink = 0;

// ─────────────────────────────────────────────────────────────────────────────
// 1. INTEGER ARITHMETIC
//    Workload: mix of multiply-accumulate, div, mod, bit ops.
//    Result: millions of integer ops per second (MOPS).
// ─────────────────────────────────────────────────────────────────────────────
static double bench_integer(double target_secs = 3.0) {
    uint64_t acc = 1;
    uint64_t ops = 0;

    auto t0 = Clock::now();
    while (elapsed(t0) < target_secs) {
        for (int i = 0; i < 100000; ++i) {
            acc ^= acc << 13;
            acc ^= acc >> 7;
            acc ^= acc << 17;
            acc += (acc | 0xDEADBEEFULL) * 6364136223846793005ULL;
            acc = (acc >> 33) ^ acc;
            acc += 2862933555777941757ULL;
            ops += 6;
        }
    }
    double secs = elapsed(t0);
    sink = acc;

    return static_cast<double>(ops) / secs / 1e6;  // MOPS
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. FLOATING POINT
//    Workload: Mandelbrot inner loop — heavy FP multiply-add, compare.
//    Result: GFLOPS (approx).
// ─────────────────────────────────────────────────────────────────────────────
static double bench_float(double target_secs = 3.0) {
    const int N = 512;
    const int MAX_ITER = 64;
    uint64_t total_flops = 0;

    auto t0 = Clock::now();
    int iterations = 0;

    while (elapsed(t0) < target_secs) {
        double sum = 0.0;
        for (int py = 0; py < N; ++py) {
            double cy = -1.5 + 3.0 * py / N;
            for (int px = 0; px < N; ++px) {
                double cx = -2.0 + 3.0 * px / N;
                double x = 0.0, y = 0.0;
                int iter = 0;
                while (x*x + y*y <= 4.0 && iter < MAX_ITER) {
                    double xt = x*x - y*y + cx;
                    y = 2.0*x*y + cy;
                    x = xt;
                    ++iter;
                    total_flops += 8;  // 2 mul, 2 add, 1 mul, 1 add, 1 cmp, 1 add
                }
                sum += iter;
            }
        }
        sink = static_cast<uint64_t>(sum);
        ++iterations;
    }
    double secs = elapsed(t0);
    return static_cast<double>(total_flops) / secs / 1e9;  // GFLOPS
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. MULTI-THREAD SCALING
//    Runs the integer workload across 1..N threads.
//    Returns: N-thread score / 1-thread score  (scaling factor).
// ─────────────────────────────────────────────────────────────────────────────
static double bench_thread(int n_threads, double target_secs = 2.0) {
    std::atomic<uint64_t> global_ops{0};

    auto worker = [&]() {
        uint64_t acc = 1;
        uint64_t ops = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < target_secs) {
            for (int i = 0; i < 50000; ++i) {
                acc ^= acc << 13;
                acc ^= acc >> 7;
                acc ^= acc << 17;
                acc += 2862933555777941757ULL;
                ops += 4;
            }
        }
        global_ops.fetch_add(ops, std::memory_order_relaxed);
        sink = acc;
    };

    std::vector<std::thread> threads;
    auto t0 = Clock::now();
    for (int i = 0; i < n_threads; ++i)
        threads.emplace_back(worker);
    for (auto& t : threads) t.join();
    double secs = elapsed(t0);

    return static_cast<double>(global_ops.load()) / secs / 1e6;
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. COMPRESSION WORKLOAD (LZ77-style hash chain)
//    Simulates a dictionary compressor's hot path: hashing + memcmp.
//    Result: MB/s of input processed.
// ─────────────────────────────────────────────────────────────────────────────
static double bench_compression(double target_secs = 3.0) {
    const size_t BUF_SIZE = 4 * 1024 * 1024;  // 4 MB
    std::vector<uint8_t> src(BUF_SIZE);
    std::vector<uint8_t> dst(BUF_SIZE * 2);

    // Fill with compressible data (repeated pattern + noise)
    std::mt19937 rng(42);
    for (size_t i = 0; i < BUF_SIZE; ++i)
        src[i] = static_cast<uint8_t>((i % 256) ^ (rng() & 0x1F));

    const int HASH_SIZE = 65536;
    std::vector<int> hash_table(HASH_SIZE, -1);

    uint64_t bytes_processed = 0;
    auto t0 = Clock::now();

    while (elapsed(t0) < target_secs) {
        std::fill(hash_table.begin(), hash_table.end(), -1);
        size_t src_pos = 0;
        size_t dst_pos = 0;

        while (src_pos + 4 < BUF_SIZE && dst_pos < BUF_SIZE * 2 - 8) {
            // Hash 4 bytes
            uint32_t h = ( (uint32_t)src[src_pos]
                         | ((uint32_t)src[src_pos+1] << 8)
                         | ((uint32_t)src[src_pos+2] << 16)
                         | ((uint32_t)src[src_pos+3] << 24) );
            uint32_t hash = ((h * 2654435769U) >> 16) & (HASH_SIZE - 1);

            int match_pos = hash_table[hash];
            hash_table[hash] = static_cast<int>(src_pos);

            if (match_pos >= 0 && src_pos - match_pos < 32768 &&
                memcmp(&src[match_pos], &src[src_pos], 4) == 0) {
                // Back-reference: write (offset, length) token
                uint16_t offset = static_cast<uint16_t>(src_pos - match_pos);
                dst[dst_pos++] = 0x80 | (offset >> 8);
                dst[dst_pos++] = offset & 0xFF;
                src_pos += 4;
            } else {
                // Literal
                dst[dst_pos++] = src[src_pos++];
            }
        }
        bytes_processed += BUF_SIZE;
    }
    double secs = elapsed(t0);
    sink = dst[0];
    return static_cast<double>(bytes_processed) / secs / (1024*1024);  // MB/s
}

// ─────────────────────────────────────────────────────────────────────────────
// MAIN
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    BenchResult result;
    result.bench_id   = "cpu";
    result.bench_name = "CPU Benchmark";
    result.passed     = true;

    auto t_global = Clock::now();

    // ── Integer ───────────────────────────────────────────────────────────
    double int_mops = bench_integer(3.0);
    result.metrics.push_back({
        "integer_mops", int_mops, "MOPS",
        "Integer arithmetic (XOR-shift + multiply-accumulate) throughput"
    });

    // ── Float ─────────────────────────────────────────────────────────────
    double fp_gflops = bench_float(3.0);
    result.metrics.push_back({
        "fp_gflops", fp_gflops, "GFLOPS",
        "Floating-point throughput (Mandelbrot kernel)"
    });

    // ── Multi-thread scaling ──────────────────────────────────────────────
    int max_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (max_threads < 1) max_threads = 1;

    double single_mops  = bench_thread(1,           2.0);
    double multi_mops   = bench_thread(max_threads, 2.0);
    double scale_factor = multi_mops / single_mops;

    result.metrics.push_back({
        "mt_single_mops", single_mops, "MOPS",
        "Single-thread integer throughput"
    });
    result.metrics.push_back({
        "mt_multi_mops", multi_mops, "MOPS",
        std::to_string(max_threads) + "-thread integer throughput"
    });
    result.metrics.push_back({
        "mt_scaling_factor", scale_factor, "x",
        "Multi-thread scaling vs single-thread (" +
        std::to_string(max_threads) + " cores)"
    });
    result.metrics.push_back({
        "thread_count", (int64_t)max_threads, "cores",
        "Number of logical cores used"
    });

    // ── Compression ───────────────────────────────────────────────────────
    double comp_mbs = bench_compression(3.0);
    result.metrics.push_back({
        "compression_mbs", comp_mbs, "MB/s",
        "LZ77-style compression workload throughput"
    });

    // ── Composite score ───────────────────────────────────────────────────
    // Weighted geometric mean of normalised sub-scores
    // Baselines: 200 MOPS int, 0.5 GFLOPS FP, 4x MT scaling, 200 MB/s comp
    double s_int  = int_mops   / 200.0;
    double s_fp   = fp_gflops  / 0.5;
    double s_mt   = scale_factor / 4.0;
    double s_comp = comp_mbs   / 200.0;
    double geo_mean = std::pow(s_int * s_fp * s_mt * s_comp, 0.25);
    result.score      = geo_mean * 1000.0;
    result.score_unit = "pts";
    result.duration_sec = elapsed(t_global);

    print_result_json(result);
    return 0;
}
