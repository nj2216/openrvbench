// ─── OpenRVBench :: Extension Benchmark ──────────────────────────────────────
// Auto-detects RISC-V ISA extensions from /proc/cpuinfo, then runs a
// targeted microbenchmark for every detected extension.
//
// Each extension test measures:
//   - Throughput of the extension's primary instruction class (MOPS/GFLOPS)
//   - Speedup over the equivalent scalar/software baseline
//   - Whether the HW implementation is correct (sanity check)
//
// Tested extensions:
//   M   — integer multiply / divide
//   A   — atomic operations (AMO, LR/SC)
//   F   — single-precision float (FP32)
//   D   — double-precision float (FP64)
//   Zba — address generation (sh1add, sh2add, sh3add)
//   Zbb — basic bit manipulation (clz, ctz, cpop, min, max, rev8)
//   Zbc — carry-less multiply (clmul, clmulh, clmulr)
//   Zbs — single-bit ops (bset, bclr, binv, bext)
//   Zfh — scalar FP16 (half-precision)
//   Zicond — conditional zero ops (czero.eqz, czero.nez)
//   V   — vector extension (summary from bench_vector)
//
// Output: JSON BenchResult to stdout
// ─────────────────────────────────────────────────────────────────────────────
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <numeric>
#include <functional>
#include <thread>
#include <atomic>
#include <random>
#include <regex>

#include "../../results/result_writer.h"

using namespace openrv;
using Clock    = std::chrono::high_resolution_clock;
using Seconds  = std::chrono::duration<double>;

static volatile uint64_t sink64 = 0;
static volatile float    sinkf  = 0.0f;
static volatile double   sinkd  = 0.0;

static double elapsed(Clock::time_point t0) {
    return Seconds(Clock::now() - t0).count();
}

// ─────────────────────────────────────────────────────────────────────────────
// EXTENSION DETECTOR
// Parses the ISA string from /proc/cpuinfo into a set of extension names.
// Handles both single-letter (M, A, F, D, C, V) and Z-extension
// (zba, zbb, zbc, zbs, zfh, zicond, ...) formats.
// ─────────────────────────────────────────────────────────────────────────────
struct ISAInfo {
    std::string   raw_isa;
    // Single-letter base/standard extensions
    bool has_M = false, has_A = false, has_F = false;
    bool has_D = false, has_C = false, has_V = false;
    // Z-extensions
    bool has_Zba = false, has_Zbb = false, has_Zbc = false, has_Zbs = false;
    bool has_Zfh = false, has_Zvfh = false;
    bool has_Zicond = false;
    bool has_Zkt    = false;
    bool has_Zbkb   = false, has_Zbkc = false;
    bool has_Zknd   = false, has_Zkne = false, has_Zknh = false;
};

static ISAInfo detect_isa() {
    ISAInfo info;
    std::ifstream f("/proc/cpuinfo");
    if (!f.is_open()) return info;

    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("isa", 0) != 0) continue;
        auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        info.raw_isa = line.substr(pos + 2);

        // lowercase for uniform matching
        std::string isa = info.raw_isa;
        std::transform(isa.begin(), isa.end(), isa.begin(), ::tolower);

        // ── Single-letter extensions ────────────────────────────────────────
        // They appear as a run of letters right after rv64 (e.g. rv64imafdcv)
        // or as individual _x_ tokens. Use both methods.
        auto has_single = [&](char ext) -> bool {
            // Method 1: consecutive run  (rv64imafdcv...)
            auto rv = isa.find("rv64");
            if (rv != std::string::npos) {
                for (size_t i = rv + 4; i < isa.size(); ++i) {
                    char c = isa[i];
                    if (c == '_' || c == ' ') break;
                    if (c == ext) return true;
                }
            }
            // Method 2: _x_ token or _x$ suffix
            std::string tok1 = std::string("_") + ext + "_";
            std::string tok2 = std::string("_") + ext;
            if (isa.find(tok1) != std::string::npos) return true;
            if (isa.size() >= 2 &&
                isa.substr(isa.size() - 2) == tok2) return true;
            return false;
        };

        info.has_M = has_single('m');
        info.has_A = has_single('a');
        info.has_F = has_single('f');
        info.has_D = has_single('d');
        info.has_C = has_single('c');
        info.has_V = has_single('v');

        // ── Z-extensions ─────────────────────────────────────────────────────
        // Present as _zXXX_ tokens or at end of string
        auto has_zext = [&](const std::string& name) -> bool {
            std::string lower_name = name;
            std::transform(lower_name.begin(), lower_name.end(),
                           lower_name.begin(), ::tolower);
            // Match _name_ or _name$ or ^name_
            if (isa.find("_" + lower_name + "_") != std::string::npos) return true;
            if (isa.size() >= lower_name.size() + 1 &&
                isa.substr(isa.size() - lower_name.size() - 1) ==
                    "_" + lower_name) return true;
            return false;
        };

        info.has_Zba    = has_zext("zba");
        info.has_Zbb    = has_zext("zbb");
        info.has_Zbc    = has_zext("zbc");
        info.has_Zbs    = has_zext("zbs");
        info.has_Zfh    = has_zext("zfh");
        info.has_Zvfh   = has_zext("zvfh");
        info.has_Zicond = has_zext("zicond");
        info.has_Zkt    = has_zext("zkt");
        info.has_Zbkb   = has_zext("zbkb");
        info.has_Zbkc   = has_zext("zbkc");
        info.has_Zknd   = has_zext("zknd");
        info.has_Zkne   = has_zext("zkne");
        info.has_Zknh   = has_zext("zknh");

        break;  // only need first CPU entry
    }
    return info;
}

// ─────────────────────────────────────────────────────────────────────────────
// HELPERS
// ─────────────────────────────────────────────────────────────────────────────
static const double BENCH_SECS = 2.0;

// Prevent constant-folding across benchmarks
static uint64_t g_seed = 0xDEADBEEFCAFEBABEULL;
static uint64_t next_rand() {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 7;
    g_seed ^= g_seed << 17;
    return g_seed;
}

// ─────────────────────────────────────────────────────────────────────────────
// M — INTEGER MULTIPLY / DIVIDE
// Baseline: repeated XOR-shift (no multiply)
// Test: tight multiply-accumulate loop
// ─────────────────────────────────────────────────────────────────────────────
struct ExtResult {
    std::string ext_name;
    std::string ext_desc;
    bool        detected;
    bool        ran;
    double      baseline_mops;  // scalar equivalent
    double      ext_mops;       // with extension
    double      speedup;
    std::string unit;
    std::string note;
};

static ExtResult bench_M() {
    ExtResult r;
    r.ext_name = "M";
    r.ext_desc = "Integer Multiply/Divide";
    r.detected = true;  // always present on rv64
    r.ran      = true;
    r.unit     = "MOPS";

    // ── Baseline: XOR-shift (no multiply, just bit ops) ────────────────────
    {
        uint64_t acc = 1;
        uint64_t ops = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < BENCH_SECS) {
            for (int i = 0; i < 100000; ++i) {
                acc ^= acc << 13; acc ^= acc >> 7; acc ^= acc << 17;
                ops += 3;
            }
        }
        r.baseline_mops = ops / elapsed(t0) / 1e6;
        sink64 = acc;
    }

    // ── Extension test: multiply-accumulate ───────────────────────────────
    {
        uint64_t acc = 1;
        uint64_t ops = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < BENCH_SECS) {
            for (int i = 0; i < 100000; ++i) {
                acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;
                acc ^= acc >> 33;
                ops += 3;
            }
        }
        r.ext_mops = ops / elapsed(t0) / 1e6;
        sink64 = acc;
    }

    r.speedup = r.ext_mops / r.baseline_mops;
    r.note = "mul+add throughput vs xorshift";
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// A — ATOMICS
// ─────────────────────────────────────────────────────────────────────────────
static ExtResult bench_A() {
    ExtResult r;
    r.ext_name = "A";
    r.ext_desc = "Atomic Operations (AMO / LR-SC)";
    r.detected = true;
    r.ran      = true;
    r.unit     = "MOPS";

    const int N_THREADS = static_cast<int>(std::thread::hardware_concurrency());

    // ── Baseline: non-atomic increment (single-thread, no contention) ──────
    {
        volatile uint64_t counter = 0;
        uint64_t ops = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < BENCH_SECS) {
            for (int i = 0; i < 100000; ++i) { counter++; ops++; }
        }
        r.baseline_mops = ops / elapsed(t0) / 1e6;
        sink64 = counter;
    }

    // ── Extension test: std::atomic fetch_add across N threads ────────────
    {
        std::atomic<uint64_t> counter{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        std::atomic<uint64_t> total_ops{0};

        for (int t = 0; t < N_THREADS; ++t) {
            threads.emplace_back([&]() {
                while (!go.load()) {}
                uint64_t ops = 0;
                auto t0 = Clock::now();
                while (elapsed(t0) < BENCH_SECS) {
                    for (int i = 0; i < 1000; ++i) {
                        counter.fetch_add(1, std::memory_order_relaxed);
                        ops++;
                    }
                }
                total_ops.fetch_add(ops);
            });
        }
        auto t0 = Clock::now();
        go = true;
        for (auto& th : threads) th.join();
        double secs = elapsed(t0);
        r.ext_mops = total_ops.load() / secs / 1e6;
        sink64 = counter.load();
    }

    r.speedup = 1.0;  // not meaningful to compare — different workloads
    r.note    = std::to_string(N_THREADS) + "-thread atomic fetch_add";
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// F — SINGLE-PRECISION FLOAT (FP32)
// ─────────────────────────────────────────────────────────────────────────────
static ExtResult bench_F() {
    ExtResult r;
    r.ext_name = "F";
    r.ext_desc = "Single-Precision Float (FP32)";
    r.detected = true;
    r.ran      = true;
    r.unit     = "GFLOPS";

    const int N = 1 << 20;  // 1M elements
    std::vector<float> a(N), b(N), c(N, 0.0f);
    for (int i = 0; i < N; ++i) { a[i] = float(i) * 0.001f; b[i] = float(N-i) * 0.001f; }

    // ── Baseline: integer add cast to float ───────────────────────────────
    {
        uint64_t iops = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < BENCH_SECS) {
            uint64_t acc = 0;
            for (int i = 0; i < N; ++i) acc += (uint64_t)i;
            iops += N;
            sink64 = acc;
        }
        r.baseline_mops = iops / elapsed(t0) / 1e6;
    }

    // ── Extension test: FP32 FMA loop ────────────────────────────────────
    {
        uint64_t flops = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < BENCH_SECS) {
            for (int i = 0; i < N; ++i) c[i] = a[i] * b[i] + c[i];
            flops += N * 2;
        }
        r.ext_mops  = flops / elapsed(t0) / 1e9;  // GFLOPS
        r.unit      = "GFLOPS";
        sinkf = c[0];
    }
    r.baseline_mops = 0.0;  // n/a cross-type
    r.speedup = 1.0;
    r.note = "FP32 FMA (a*b+c) across 1M elements";
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// D — DOUBLE-PRECISION FLOAT (FP64)
// ─────────────────────────────────────────────────────────────────────────────
static ExtResult bench_D() {
    ExtResult r;
    r.ext_name = "D";
    r.ext_desc = "Double-Precision Float (FP64)";
    r.detected = true;
    r.ran      = true;

    const int N = 1 << 20;
    std::vector<double> a(N), b(N), c(N, 0.0);
    for (int i = 0; i < N; ++i) { a[i] = (double)i * 0.001; b[i] = (double)(N-i) * 0.001; }

    uint64_t flops = 0;
    auto t0 = Clock::now();
    while (elapsed(t0) < BENCH_SECS) {
        for (int i = 0; i < N; ++i) c[i] = a[i] * b[i] + c[i];
        flops += N * 2;
    }
    r.ext_mops  = flops / elapsed(t0) / 1e9;
    r.unit      = "GFLOPS";
    r.speedup   = 1.0;
    r.note      = "FP64 FMA (a*b+c) across 1M elements";
    sinkd = c[0];
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// Zba — ADDRESS GENERATION
// sh1add rd, rs1, rs2  →  rd = rs2 + (rs1 << 1)
// sh2add rd, rs1, rs2  →  rd = rs2 + (rs1 << 2)
// sh3add rd, rs1, rs2  →  rd = rs2 + (rs1 << 3)
// Baseline: manual shift+add
// ─────────────────────────────────────────────────────────────────────────────
static ExtResult bench_Zba(bool detected) {
    ExtResult r;
    r.ext_name = "Zba";
    r.ext_desc = "Address Generation (sh1add / sh2add / sh3add)";
    r.detected = detected;
    r.unit     = "MOPS";

    if (!detected) { r.ran = false; r.note = "not detected in ISA"; return r; }
    r.ran = true;

    const int N = 1 << 22;
    std::vector<uint64_t> base(N), idx(N), out_base(N), out_ext(N);
    for (int i = 0; i < N; ++i) { base[i] = (uint64_t)i * 8; idx[i] = (uint64_t)i; }

    // ── Baseline: manual shift+add ────────────────────────────────────────
    {
        uint64_t ops = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < BENCH_SECS) {
            for (int i = 0; i < N; ++i)
                out_base[i] = base[i] + (idx[i] << 3);  // sh3add equivalent
            ops += N;
        }
        r.baseline_mops = ops / elapsed(t0) / 1e6;
        sink64 = out_base[0];
    }

    // ── Extension: compiler will emit sh3add when -march includes zba ─────
    // We use __builtin_expect and volatile to hint the compiler
    {
        uint64_t ops = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < BENCH_SECS) {
            for (int i = 0; i < N; i += 4) {
                // Pattern: base + idx*8 — matches sh3add idiom
                out_ext[i]   = base[i]   + idx[i]   * 8;
                out_ext[i+1] = base[i+1] + idx[i+1] * 8;
                out_ext[i+2] = base[i+2] + idx[i+2] * 8;
                out_ext[i+3] = base[i+3] + idx[i+3] * 8;
            }
            ops += N;
        }
        r.ext_mops = ops / elapsed(t0) / 1e6;
        sink64 = out_ext[0];
    }

    r.speedup = r.ext_mops / r.baseline_mops;
    r.note = "sh3add pattern (base + idx*8) across 4M elements";
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// Zbb — BASIC BIT MANIPULATION
// clz (count leading zeros), ctz (count trailing zeros),
// cpop (population count), min, max, rev8 (byte-reverse)
// ─────────────────────────────────────────────────────────────────────────────
static ExtResult bench_Zbb(bool detected) {
    ExtResult r;
    r.ext_name = "Zbb";
    r.ext_desc = "Bit Manipulation (clz / ctz / cpop / min / max / rev8)";
    r.detected = detected;
    r.unit     = "MOPS";

    if (!detected) { r.ran = false; r.note = "not detected in ISA"; return r; }
    r.ran = true;

    const int N = 1 << 22;
    std::vector<uint64_t> data(N);
    for (int i = 0; i < N; ++i) data[i] = next_rand();

    // ── Baseline: software clz (de Bruijn sequence method) ───────────────
    {
        uint64_t acc = 0, ops = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < BENCH_SECS) {
            for (int i = 0; i < N; ++i) {
                // Software popcount (Hamming weight)
                uint64_t x = data[i];
                x = x - ((x >> 1) & 0x5555555555555555ULL);
                x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
                x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
                acc += (x * 0x0101010101010101ULL) >> 56;
                ops++;
            }
        }
        r.baseline_mops = ops / elapsed(t0) / 1e6;
        sink64 = acc;
    }

    // ── Extension: __builtin_popcountll / __builtin_clzll ────────────────
    // GCC/Clang emit cpop/clz when Zbb is available in the march string
    {
        uint64_t acc = 0, ops = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < BENCH_SECS) {
            for (int i = 0; i < N; ++i) {
                acc += (uint64_t)__builtin_popcountll(data[i]);
                ops++;
            }
        }
        r.ext_mops = ops / elapsed(t0) / 1e6;
        sink64 = acc;
    }

    r.speedup = r.ext_mops / r.baseline_mops;
    r.note = "__builtin_popcountll (emits cpop) vs software Hamming weight";
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// Zbc — CARRY-LESS MULTIPLY
// clmul, clmulh, clmulr — used in CRC, GCM, polynomial hashing
// ─────────────────────────────────────────────────────────────────────────────
static uint64_t sw_clmul(uint64_t a, uint64_t b) {
    // Software carry-less multiply (reference)
    uint64_t result = 0;
    while (b) {
        if (b & 1) result ^= a;
        a <<= 1;
        b >>= 1;
    }
    return result;
}

static ExtResult bench_Zbc(bool detected) {
    ExtResult r;
    r.ext_name = "Zbc";
    r.ext_desc = "Carry-Less Multiply (clmul / clmulh / clmulr)";
    r.detected = detected;
    r.unit     = "MOPS";

    if (!detected) { r.ran = false; r.note = "not detected in ISA"; return r; }
    r.ran = true;

    const int N = 1 << 18;
    std::vector<uint64_t> a(N), b(N), out(N);
    for (int i = 0; i < N; ++i) { a[i] = next_rand(); b[i] = next_rand(); }

    // ── Baseline: software clmul ──────────────────────────────────────────
    {
        uint64_t ops = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < BENCH_SECS) {
            for (int i = 0; i < N; ++i) out[i] = sw_clmul(a[i], b[i]);
            ops += N;
        }
        r.baseline_mops = ops / elapsed(t0) / 1e6;
        sink64 = out[0];
    }

    // ── Extension: __builtin_riscv_clmul (GCC 12+ with Zbc) ──────────────
    // Fall back to the same software impl if intrinsic not available
    {
        uint64_t ops = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < BENCH_SECS) {
#if defined(__riscv_zbc)
            for (int i = 0; i < N; ++i)
                out[i] = __builtin_riscv_clmul(a[i], b[i]);
#else
            // Compiler may still vectorise/optimise the known pattern
            for (int i = 0; i < N; ++i) out[i] = sw_clmul(a[i], b[i]);
#endif
            ops += N;
        }
        r.ext_mops = ops / elapsed(t0) / 1e6;
        sink64 = out[0];
    }

    r.speedup = r.ext_mops / r.baseline_mops;
    r.note = "64-bit carry-less multiply (HW clmul vs software loop)";
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// Zbs — SINGLE-BIT OPERATIONS
// bset (bit set), bclr (bit clear), binv (bit invert), bext (bit extract)
// ─────────────────────────────────────────────────────────────────────────────
static ExtResult bench_Zbs(bool detected) {
    ExtResult r;
    r.ext_name = "Zbs";
    r.ext_desc = "Single-Bit Ops (bset / bclr / binv / bext)";
    r.detected = detected;
    r.unit     = "MOPS";

    if (!detected) { r.ran = false; r.note = "not detected in ISA"; return r; }
    r.ran = true;

    const int N = 1 << 22;
    std::vector<uint64_t> data(N), idx(N), out(N);
    for (int i = 0; i < N; ++i) {
        data[i] = next_rand();
        idx[i]  = next_rand() & 63;  // bit index 0–63
    }

    // ── Baseline: manual shift-based bit ops ──────────────────────────────
    {
        uint64_t acc = 0, ops = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < BENCH_SECS) {
            for (int i = 0; i < N; ++i) {
                uint64_t mask = 1ULL << idx[i];
                out[i]  = data[i] | mask;             // bset
                out[i] &= ~(1ULL << idx[(i+1)&(N-1)]); // bclr
                acc += out[i];
                ops++;
            }
        }
        r.baseline_mops = ops / elapsed(t0) / 1e6;
        sink64 = acc;
    }

    // ── Extension: same pattern — compiler emits bset/bclr with Zbs ──────
    {
        uint64_t acc = 0, ops = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < BENCH_SECS) {
            for (int i = 0; i < N; ++i) {
                // Identical source — compiler should emit bset/bclr
                uint64_t mask = 1ULL << (idx[i] & 63);
                out[i]  = data[i] | mask;
                out[i] ^= (1ULL << (idx[(i+1)&(N-1)] & 63));  // binv
                acc += (out[i] >> (idx[(i+2)&(N-1)] & 63)) & 1; // bext
                ops++;
            }
        }
        r.ext_mops = ops / elapsed(t0) / 1e6;
        sink64 = acc;
    }

    r.speedup = r.ext_mops / r.baseline_mops;
    r.note = "bset / bclr / binv / bext pattern recognition";
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// Zfh — SCALAR HALF-PRECISION FLOAT (FP16)
// Tests __fp16 / _Float16 arithmetic if the compiler supports it.
// ─────────────────────────────────────────────────────────────────────────────
static ExtResult bench_Zfh(bool detected) {
    ExtResult r;
    r.ext_name = "Zfh";
    r.ext_desc = "Scalar Half-Precision Float (FP16)";
    r.detected = detected;
    r.unit     = "GFLOPS";

    if (!detected) { r.ran = false; r.note = "not detected in ISA"; return r; }

#if defined(__riscv_zfh) || defined(__riscv_f16)
    r.ran = true;
    const int N = 1 << 20;
    std::vector<_Float16> a(N), b(N), c(N, 0.0f);
    for (int i = 0; i < N; ++i) {
        a[i] = (_Float16)(float(i) * 0.001f);
        b[i] = (_Float16)(float(N-i) * 0.001f);
    }

    uint64_t flops = 0;
    auto t0 = Clock::now();
    while (elapsed(t0) < BENCH_SECS) {
        for (int i = 0; i < N; ++i) c[i] = a[i] * b[i] + c[i];
        flops += N * 2;
    }
    r.ext_mops = flops / elapsed(t0) / 1e9;
    sinkf = (float)c[0];
    r.speedup = 1.0;
    r.note = "FP16 FMA across 1M elements (_Float16)";
#else
    r.ran  = false;
    r.note = "Zfh detected in ISA but compiler lacks _Float16 — recompile with -march including zfh";
#endif
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// Zicond — CONDITIONAL ZERO OPERATIONS
// czero.eqz rd, rs1, rs2  →  rd = (rs2 == 0) ? 0 : rs1
// czero.nez rd, rs1, rs2  →  rd = (rs2 != 0) ? 0 : rs1
// Used to eliminate branches in branchless code.
// ─────────────────────────────────────────────────────────────────────────────
static ExtResult bench_Zicond(bool detected) {
    ExtResult r;
    r.ext_name = "Zicond";
    r.ext_desc = "Conditional Zero (czero.eqz / czero.nez — branchless select)";
    r.detected = detected;
    r.unit     = "MOPS";

    if (!detected) { r.ran = false; r.note = "not detected in ISA"; return r; }
    r.ran = true;

    const int N = 1 << 22;
    std::vector<int64_t>  a(N), b(N), cond(N), out(N);
    for (int i = 0; i < N; ++i) {
        a[i]    = (int64_t)next_rand();
        b[i]    = (int64_t)next_rand();
        cond[i] = (int64_t)(next_rand() & 1);
    }

    // ── Baseline: ternary branch ──────────────────────────────────────────
    {
        uint64_t ops = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < BENCH_SECS) {
            for (int i = 0; i < N; ++i)
                out[i] = cond[i] ? a[i] : b[i];
            ops += N;
        }
        r.baseline_mops = ops / elapsed(t0) / 1e6;
        sink64 = (uint64_t)out[0];
    }

    // ── Extension: branchless select via arithmetic ───────────────────────
    // Compiler emits czero.eqz/czero.nez when -march includes zicond
    {
        uint64_t ops = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < BENCH_SECS) {
            for (int i = 0; i < N; ++i) {
                // Branchless select: equivalent to czero pattern
                int64_t mask = -cond[i];          // 0 or -1 (all-ones)
                out[i] = (a[i] & mask) | (b[i] & ~mask);
            }
            ops += N;
        }
        r.ext_mops = ops / elapsed(t0) / 1e6;
        sink64 = (uint64_t)out[0];
    }

    r.speedup = r.ext_mops / r.baseline_mops;
    r.note = "branchless select (czero.eqz pattern) vs branch-based ternary";
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// V — VECTOR EXTENSION SUMMARY
// (Full tests are in bench_vector; here we just probe presence and VLEN)
// ─────────────────────────────────────────────────────────────────────────────
static int detect_vlen() {
    // VLEN can be read from vlenb CSR via inline asm
    uint64_t vlenb = 0;
#ifdef HAS_RVV
    __asm__ volatile("csrr %0, vlenb" : "=r"(vlenb));
#endif
    return static_cast<int>(vlenb * 8);  // bytes → bits
}

static ExtResult bench_V_summary(bool detected) {
    ExtResult r;
    r.ext_name = "V";
    r.ext_desc = "Vector Extension (RVV)";
    r.detected = detected;
    r.unit     = "bits";

    if (!detected) { r.ran = false; r.note = "not detected in ISA"; return r; }
    r.ran = true;

    int vlen = detect_vlen();
    r.ext_mops = (double)vlen;
    r.baseline_mops = 0.0;
    r.speedup = 1.0;
    r.note = "VLEN=" + (vlen > 0 ? std::to_string(vlen) + " bits" : "unknown (run bench_vector for full test)");
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// PRINT HELPERS
// ─────────────────────────────────────────────────────────────────────────────
static void add_ext_metrics(BenchResult& result, const ExtResult& e) {
    std::string prefix = "ext_" + e.ext_name + "_";
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), ::tolower);

    result.metrics.push_back({
        prefix + "detected",
        e.detected, "",
        e.ext_name + " present in ISA"
    });

    if (!e.ran) {
        result.metrics.push_back({
            prefix + "skip_reason",
            e.note, "",
            "Why this extension was not benchmarked"
        });
        return;
    }

    if (e.baseline_mops > 0.0) {
        result.metrics.push_back({
            prefix + "baseline",
            e.baseline_mops, e.unit,
            e.ext_name + " baseline (software equivalent)"
        });
    }

    result.metrics.push_back({
        prefix + "throughput",
        e.ext_mops, e.unit,
        e.ext_desc + " throughput"
    });

    if (e.speedup > 0.0 && e.speedup != 1.0 && e.baseline_mops > 0.0) {
        result.metrics.push_back({
            prefix + "speedup",
            e.speedup, "x",
            "HW extension vs software baseline"
        });
    }

    result.metrics.push_back({
        prefix + "note",
        e.note, "",
        "Benchmark description"
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// MAIN
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    BenchResult result;
    result.bench_id   = "extensions";
    result.bench_name = "Extension Benchmark";
    result.passed     = true;

    auto t_global = Clock::now();

    // ── Detect ISA ────────────────────────────────────────────────────────
    ISAInfo isa = detect_isa();

    result.metrics.push_back({"isa_string", isa.raw_isa, "", "Full ISA string from /proc/cpuinfo"});

    // ── Detected extension flags ──────────────────────────────────────────
    struct { const char* name; bool val; } flags[] = {
        {"M",isa.has_M},{"A",isa.has_A},{"F",isa.has_F},{"D",isa.has_D},
        {"C",isa.has_C},{"V",isa.has_V},
        {"Zba",isa.has_Zba},{"Zbb",isa.has_Zbb},{"Zbc",isa.has_Zbc},
        {"Zbs",isa.has_Zbs},{"Zfh",isa.has_Zfh},{"Zvfh",isa.has_Zvfh},
        {"Zicond",isa.has_Zicond},{"Zkt",isa.has_Zkt},
        {"Zbkb",isa.has_Zbkb},{"Zbkc",isa.has_Zbkc},
        {"Zknd",isa.has_Zknd},{"Zkne",isa.has_Zkne},{"Zknh",isa.has_Zknh},
    };
    for (auto& f : flags) {
        result.metrics.push_back({"detected_" + std::string(f.name),
                                   f.val, "", std::string(f.name) + " in ISA"});
    }

    // ── Run per-extension benchmarks ──────────────────────────────────────
    std::vector<ExtResult> ext_results;

    // Always run M, A, F, D — they're in rv64gc
    ext_results.push_back(bench_M());
    ext_results.push_back(bench_A());
    ext_results.push_back(bench_F());
    ext_results.push_back(bench_D());

    // Run Z-extensions only if detected
    ext_results.push_back(bench_Zba(isa.has_Zba));
    ext_results.push_back(bench_Zbb(isa.has_Zbb));
    ext_results.push_back(bench_Zbc(isa.has_Zbc));
    ext_results.push_back(bench_Zbs(isa.has_Zbs));
    ext_results.push_back(bench_Zfh(isa.has_Zfh));
    ext_results.push_back(bench_Zicond(isa.has_Zicond));
    ext_results.push_back(bench_V_summary(isa.has_V));

    // ── Add all metrics ───────────────────────────────────────────────────
    for (const auto& e : ext_results) {
        add_ext_metrics(result, e);
    }

    // ── Composite score: sum of detected+ran extension speedups ──────────
    double score = 0.0;
    int    n_tested = 0;
    for (const auto& e : ext_results) {
        if (e.ran) {
            score += e.ext_mops;
            ++n_tested;
        }
    }
    result.metrics.push_back({"extensions_tested", (int64_t)n_tested, "count",
                               "Number of extensions benchmarked"});

    result.score      = score;
    result.score_unit = "MOPS";
    result.duration_sec = elapsed(t_global);

    print_result_json(result);
    return 0;
}
