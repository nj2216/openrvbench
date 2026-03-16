// ─── OpenRVBench :: Memory Benchmark ─────────────────────────────────────────
// Measures: sequential bandwidth (read/write/copy), random latency,
//           and L1/L2/L3/DRAM cache hierarchy stress.
//
// Output: JSON BenchResult to stdout
// ─────────────────────────────────────────────────────────────────────────────
#include <iostream>
#include <vector>
#include <cstring>
#include <cmath>
#include <chrono>
#include <numeric>
#include <random>
#include <algorithm>
#include <cstdlib>

#include "../../results/result_writer.h"

using namespace openrv;
using Clock = std::chrono::high_resolution_clock;
using Seconds = std::chrono::duration<double>;
static volatile uint64_t sink = 0;

static double elapsed(Clock::time_point t0) {
    return Seconds(Clock::now() - t0).count();
}

// ─────────────────────────────────────────────────────────────────────────────
// SEQUENTIAL BANDWIDTH
// ─────────────────────────────────────────────────────────────────────────────
struct BandwidthResult {
    double read_gbs;
    double write_gbs;
    double copy_gbs;
};

static BandwidthResult bench_bandwidth(size_t buf_size_mb = 256,
                                        double target_secs = 3.0) {
    const size_t N = buf_size_mb * 1024 * 1024 / sizeof(uint64_t);

    // Use aligned allocation for better DRAM performance
    uint64_t* A = static_cast<uint64_t*>(
        std::aligned_alloc(64, N * sizeof(uint64_t)));
    uint64_t* B = static_cast<uint64_t*>(
        std::aligned_alloc(64, N * sizeof(uint64_t)));

    if (!A || !B) {
        free(A); free(B);
        return {0,0,0};
    }

    // Warm up + initialise
    for (size_t i = 0; i < N; ++i) { A[i] = i; B[i] = 0; }

    BandwidthResult bw{};

    // ── Sequential Read ───────────────────────────────────────────────────
    {
        uint64_t bytes = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < target_secs) {
            uint64_t sum = 0;
            for (size_t i = 0; i < N; i += 8) {
                sum += A[i]   + A[i+1] + A[i+2] + A[i+3] +
                       A[i+4] + A[i+5] + A[i+6] + A[i+7];
            }
            sink = sum;
            bytes += N * sizeof(uint64_t);
        }
        bw.read_gbs = static_cast<double>(bytes) / elapsed(t0) / 1e9;
    }

    // ── Sequential Write ──────────────────────────────────────────────────
    {
        uint64_t bytes = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < target_secs) {
            for (size_t i = 0; i < N; i += 8) {
                B[i]   = i;   B[i+1] = i+1; B[i+2] = i+2; B[i+3] = i+3;
                B[i+4] = i+4; B[i+5] = i+5; B[i+6] = i+6; B[i+7] = i+7;
            }
            bytes += N * sizeof(uint64_t);
        }
        sink = B[0];
        bw.write_gbs = static_cast<double>(bytes) / elapsed(t0) / 1e9;
    }

    // ── Memory Copy ───────────────────────────────────────────────────────
    {
        uint64_t bytes = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < target_secs) {
            memcpy(B, A, N * sizeof(uint64_t));
            bytes += N * sizeof(uint64_t);
        }
        sink = B[0];
        bw.copy_gbs = static_cast<double>(bytes) / elapsed(t0) / 1e9;
    }

    free(A);
    free(B);
    return bw;
}

// ─────────────────────────────────────────────────────────────────────────────
// RANDOM LATENCY (pointer chasing)
// Measures average DRAM random-access latency by traversing a
// randomly-permuted linked list that fits in the target memory level.
// ─────────────────────────────────────────────────────────────────────────────
static double bench_latency_ns(size_t buf_size_kb, double target_secs = 2.0) {
    const size_t stride   = 64;   // cache-line size
    const size_t n_lines  = (buf_size_kb * 1024) / stride;

    // Build random permutation of line indices (Fisher-Yates)
    std::vector<size_t> next(n_lines);
    std::iota(next.begin(), next.end(), 0);
    std::mt19937 rng(0xDEADBEEF);
    for (size_t i = n_lines - 1; i > 0; --i) {
        size_t j = rng() % (i + 1);
        std::swap(next[i], next[j]);
    }

    // Pointer-chase buffer: each slot holds the byte offset of the next slot
    std::vector<char> buf(buf_size_kb * 1024, 0);
    for (size_t i = 0; i < n_lines; ++i) {
        size_t cur  = i            * stride;
        size_t nxt  = next[i]      * stride;
        *reinterpret_cast<uint64_t*>(&buf[cur]) =
            reinterpret_cast<uint64_t>(&buf[nxt]);
    }

    // Chase the pointer chain
    uint64_t ptr = *reinterpret_cast<uint64_t*>(&buf[0]);
    uint64_t accesses = 0;

    auto t0 = Clock::now();
    while (elapsed(t0) < target_secs) {
        for (int i = 0; i < 1000; ++i) {
            ptr = *reinterpret_cast<uint64_t*>(ptr);
            ++accesses;
        }
    }
    double secs = elapsed(t0);
    sink = ptr;

    return (secs * 1e9) / static_cast<double>(accesses);  // ns/access
}

// ─────────────────────────────────────────────────────────────────────────────
// CACHE STRESS
// Sweeps through several buffer sizes to expose each cache level.
// Returns struct with L1/L2/DRAM latency in ns.
// ─────────────────────────────────────────────────────────────────────────────
struct CacheResult {
    double l1_ns;    // ~32 KB
    double l2_ns;    // ~256 KB
    double llc_ns;   // ~4 MB  (L3 or last-level)
    double dram_ns;  // ~256 MB
};

static CacheResult bench_cache(double target_secs = 1.5) {
    return {
        bench_latency_ns(16,    target_secs),   // L1 (16 KB)
        bench_latency_ns(256,   target_secs),   // L2 (256 KB)
        bench_latency_ns(4096,  target_secs),   // LLC (4 MB)
        bench_latency_ns(65536, target_secs),   // DRAM (64 MB)
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// MAIN
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    BenchResult result;
    result.bench_id   = "memory";
    result.bench_name = "Memory Benchmark";
    result.passed     = true;

    auto t_global = Clock::now();

    // ── Bandwidth ─────────────────────────────────────────────────────────
    auto bw = bench_bandwidth(256, 3.0);
    result.metrics.push_back({"read_bandwidth_gbs",  bw.read_gbs,  "GB/s",
                               "Sequential read (256 MB buffer)"});
    result.metrics.push_back({"write_bandwidth_gbs", bw.write_gbs, "GB/s",
                               "Sequential write (256 MB buffer)"});
    result.metrics.push_back({"copy_bandwidth_gbs",  bw.copy_gbs,  "GB/s",
                               "Memory copy (memcpy) throughput"});

    // ── Cache / Latency ───────────────────────────────────────────────────
    auto cache = bench_cache(1.5);
    result.metrics.push_back({"l1_latency_ns",   cache.l1_ns,   "ns",
                               "L1 cache random latency (~16 KB)"});
    result.metrics.push_back({"l2_latency_ns",   cache.l2_ns,   "ns",
                               "L2 cache random latency (~256 KB)"});
    result.metrics.push_back({"llc_latency_ns",  cache.llc_ns,  "ns",
                               "LLC/L3 random latency (~4 MB)"});
    result.metrics.push_back({"dram_latency_ns", cache.dram_ns, "ns",
                               "DRAM random latency (~64 MB)"});

    // ── Composite score ───────────────────────────────────────────────────
    // Score based on bandwidth (higher = better) weighted against DRAM latency
    double bw_score  = (bw.read_gbs + bw.write_gbs + bw.copy_gbs) / 3.0;
    double lat_score = 100.0 / (cache.dram_ns / 100.0);  // invert latency
    result.score      = (bw_score * 200.0) + lat_score;
    result.score_unit = "pts";
    result.duration_sec = elapsed(t_global);

    print_result_json(result);
    return 0;
}
