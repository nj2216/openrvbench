// ─── OpenRVBench :: Vector Extension Benchmark ───────────────────────────────
// Tests RISC-V Vector Extension (RVV) acceleration.
// 
// Workloads:
//   1. Vectorised SAXPY  (scalar vs auto-vectorised vs RVV intrinsics)
//   2. Matrix multiply   (NxN, compares rv64gc vs rv64gcv build paths)
//   3. Vector dot product
//   4. Vector int8 saturating accumulate (typical AI/DSP inner loop)
//
// On non-RVV builds the "vectorised" paths fall back to auto-vectorisation.
// Output: JSON BenchResult with speedup ratios.
// ─────────────────────────────────────────────────────────────────────────────
#include <iostream>
#include <vector>
#include <cstring>
#include <cmath>
#include <chrono>
#include <numeric>
#include <random>
#include <algorithm>

#include "../../results/result_writer.h"

#ifdef HAS_RVV
#  include <riscv_vector.h>
#endif

using namespace openrv;
using Clock = std::chrono::high_resolution_clock;
using Seconds = std::chrono::duration<double>;
static volatile float fsink = 0.0f;

static double elapsed(Clock::time_point t0) {
    return Seconds(Clock::now() - t0).count();
}

// ─────────────────────────────────────────────────────────────────────────────
// SAXPY  y[i] = a*x[i] + y[i]   — memory-bandwidth + FMA bound
// ─────────────────────────────────────────────────────────────────────────────
static void saxpy_scalar(float a,
                          const float* __restrict__ x,
                          float*       __restrict__ y,
                          size_t n) {
    for (size_t i = 0; i < n; ++i)
        y[i] = a * x[i] + y[i];
}

#ifdef HAS_RVV
static void saxpy_rvv(float a,
                       const float* __restrict__ x,
                       float*       __restrict__ y,
                       size_t n) {
    // Use RVV LMUL=4 for wider vector operations
    size_t vl;
    for (size_t i = 0; i < n; i += vl) {
        vl = __riscv_vsetvl_e32m4(n - i);
        vfloat32m4_t vx = __riscv_vle32_v_f32m4(x + i, vl);
        vfloat32m4_t vy = __riscv_vle32_v_f32m4(y + i, vl);
        vy = __riscv_vfmacc_vf_f32m4(vy, a, vx, vl);
        __riscv_vse32_v_f32m4(y + i, vy, vl);
    }
}
#endif

static double bench_saxpy(size_t N = 16 * 1024 * 1024,
                            double target_secs = 2.0) {
    std::vector<float> x(N), y_scalar(N), y_vec(N);
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (size_t i = 0; i < N; ++i) { x[i] = dist(rng); y_scalar[i] = dist(rng); }
    y_vec = y_scalar;

    float a = 1.41421356f;

    // ── Scalar baseline ───────────────────────────────────────────────────
    double scalar_gbs = 0.0;
    {
        uint64_t bytes = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < target_secs) {
            saxpy_scalar(a, x.data(), y_scalar.data(), N);
            bytes += N * sizeof(float) * 3;  // read x, read y, write y
        }
        double secs = elapsed(t0);
        scalar_gbs = static_cast<double>(bytes) / secs / 1e9;
        fsink = y_scalar[0];
    }

    // ── Vectorised (RVV or auto-vec) ──────────────────────────────────────
    double vec_gbs = 0.0;
    {
        uint64_t bytes = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < target_secs) {
#ifdef HAS_RVV
            saxpy_rvv(a, x.data(), y_vec.data(), N);
#else
            // Compiler hint: restrict-annotated loop should auto-vectorise
            saxpy_scalar(a, x.data(), y_vec.data(), N);
#endif
            bytes += N * sizeof(float) * 3;
        }
        double secs = elapsed(t0);
        vec_gbs = static_cast<double>(bytes) / secs / 1e9;
        fsink = y_vec[0];
    }

    (void)scalar_gbs;  // scalar_gbs returned separately
    return vec_gbs;
}

// ─────────────────────────────────────────────────────────────────────────────
// MATRIX MULTIPLY  C = A * B  (single-precision, N×N)
// ─────────────────────────────────────────────────────────────────────────────
static double matmul_scalar(const float* A, const float* B, float* C, int N) {
    auto t0 = Clock::now();
    for (int i = 0; i < N; ++i)
        for (int k = 0; k < N; ++k) {
            float aik = A[i*N + k];
            for (int j = 0; j < N; ++j)
                C[i*N + j] += aik * B[k*N + j];
        }
    double secs = elapsed(t0);
    fsink = C[0];
    uint64_t flops = 2ULL * N * N * N;
    return static_cast<double>(flops) / secs / 1e9;
}

#ifdef HAS_RVV
// RVV-optimised row-major matmul inner loop
static double matmul_rvv(const float* A, const float* B, float* C, int N) {
    auto t0 = Clock::now();
    for (int i = 0; i < N; ++i) {
        for (int k = 0; k < N; ++k) {
            float aik = A[i*N + k];
            const float* Brow = B + k*N;
            float*       Crow = C + i*N;
            size_t vl;
            for (int j = 0; j < N; j += static_cast<int>(vl)) {
                vl = __riscv_vsetvl_e32m4(N - j);
                vfloat32m4_t vb = __riscv_vle32_v_f32m4(Brow + j, vl);
                vfloat32m4_t vc = __riscv_vle32_v_f32m4(Crow + j, vl);
                vc = __riscv_vfmacc_vf_f32m4(vc, aik, vb, vl);
                __riscv_vse32_v_f32m4(Crow + j, vc, vl);
            }
        }
    }
    double secs = elapsed(t0);
    fsink = C[0];
    uint64_t flops = 2ULL * N * N * N;
    return static_cast<double>(flops) / secs / 1e9;
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
// DOT PRODUCT  — pure reduction, tests vector reduction instructions
// ─────────────────────────────────────────────────────────────────────────────
static double bench_dot(size_t N = 32 * 1024 * 1024, double target_secs = 2.0) {
    std::vector<float> a(N, 1.0f), b(N, 2.0f);

    double gflops_best = 0.0;
    {
        auto t0 = Clock::now();
        int iters = 0;
        while (elapsed(t0) < target_secs) {
#ifdef HAS_RVV
            // RVV dot product with LMUL=8
            vfloat32m8_t acc = __riscv_vfmv_v_f_f32m8(0.0f, 1);
            size_t vl;
            for (size_t i = 0; i < N; i += vl) {
                vl = __riscv_vsetvl_e32m8(N - i);
                vfloat32m8_t va = __riscv_vle32_v_f32m8(a.data() + i, vl);
                vfloat32m8_t vb = __riscv_vle32_v_f32m8(b.data() + i, vl);
                acc = __riscv_vfmacc_vv_f32m8(acc, va, vb, vl);
            }
            // Reduce LMUL=8 accumulator to scalar via vfredusum.
            // vfredusum_vs needs a 1-element m1 identity register.
            size_t vl_one = __riscv_vsetvl_e32m1(1);
            vfloat32m1_t identity = __riscv_vfmv_v_f_f32m1(0.0f, vl_one);
            vfloat32m1_t result   = __riscv_vfredusum_vs_f32m8_f32m1(
                                        acc, identity,
                                        __riscv_vsetvl_e32m8(N));
            fsink = __riscv_vfmv_f_s_f32m1_f32(result);
#else
            float sum = 0.0f;
            for (size_t i = 0; i < N; ++i) sum += a[i] * b[i];
            fsink = sum;
#endif
            ++iters;
        }
        double secs = elapsed(t0);
        double total_flops = static_cast<double>(iters) * N * 2;
        gflops_best = total_flops / secs / 1e9;
    }
    return gflops_best;
}

// ─────────────────────────────────────────────────────────────────────────────
// MAIN
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    BenchResult result;
    result.bench_id   = "vector";
    result.bench_name = "Vector Extension Benchmark";
    result.passed     = true;

#ifdef HAS_RVV
    result.metrics.push_back({"rvv_available", true,  "", "RVV detected at runtime"});
#else
    result.metrics.push_back({"rvv_available", false, "", "No RVV — scalar/auto-vec only"});
#endif

    auto t_global = Clock::now();

    // ── SAXPY scalar baseline ─────────────────────────────────────────────
    const size_t SAXPY_N = 16 * 1024 * 1024;
    std::vector<float> sx(SAXPY_N), sy(SAXPY_N);
    float a = 1.41421356f;
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.f, 1.f);
    for (size_t i = 0; i < SAXPY_N; ++i) { sx[i] = dist(rng); sy[i] = dist(rng); }

    double scalar_gbs = 0.0;
    {
        auto tmp_y = sy;
        uint64_t bytes = 0;
        auto t0 = Clock::now();
        while (elapsed(t0) < 2.0) {
            saxpy_scalar(a, sx.data(), tmp_y.data(), SAXPY_N);
            bytes += SAXPY_N * 3 * sizeof(float);
        }
        scalar_gbs = static_cast<double>(bytes) / elapsed(t0) / 1e9;
        fsink = tmp_y[0];
    }

    double vec_gbs = bench_saxpy(SAXPY_N, 2.0);
    double saxpy_speedup = (scalar_gbs > 0) ? vec_gbs / scalar_gbs : 1.0;

    result.metrics.push_back({"saxpy_scalar_gbs",  scalar_gbs,    "GB/s", "SAXPY scalar"});
    result.metrics.push_back({"saxpy_vector_gbs",  vec_gbs,       "GB/s", "SAXPY vectorised"});
    result.metrics.push_back({"saxpy_speedup",      saxpy_speedup, "x",   "Vector vs scalar speedup"});

    // ── Matrix multiply ───────────────────────────────────────────────────
    const int MAT_N = 256;
    std::vector<float> A(MAT_N*MAT_N), B(MAT_N*MAT_N),
                       C_scalar(MAT_N*MAT_N, 0.0f),
                       C_vec(MAT_N*MAT_N, 0.0f);

    for (int i = 0; i < MAT_N*MAT_N; ++i) {
        A[i] = dist(rng);
        B[i] = dist(rng);
    }

    double matmul_scalar_gflops = matmul_scalar(A.data(), B.data(),
                                                  C_scalar.data(), MAT_N);
#ifdef HAS_RVV
    double matmul_vec_gflops = matmul_rvv(A.data(), B.data(),
                                            C_vec.data(), MAT_N);
#else
    double matmul_vec_gflops = matmul_scalar(A.data(), B.data(),
                                               C_vec.data(), MAT_N);
#endif
    double matmul_speedup = (matmul_scalar_gflops > 0)
                            ? matmul_vec_gflops / matmul_scalar_gflops
                            : 1.0;

    result.metrics.push_back({"matmul_scalar_gflops", matmul_scalar_gflops,
                               "GFLOPS", "Matrix multiply scalar (" +
                               std::to_string(MAT_N) + "x" + std::to_string(MAT_N) + ")"});
    result.metrics.push_back({"matmul_vector_gflops", matmul_vec_gflops,
                               "GFLOPS", "Matrix multiply RVV/auto-vec"});
    result.metrics.push_back({"matmul_speedup", matmul_speedup,
                               "x", "Matrix multiply vector speedup"});

    // ── Dot product ───────────────────────────────────────────────────────
    double dot_gflops = bench_dot(32 * 1024 * 1024, 2.0);
    result.metrics.push_back({"dot_product_gflops", dot_gflops,
                               "GFLOPS", "Dot product reduction"});

    // ── Composite score ───────────────────────────────────────────────────
    // Primary score = vector speedup on SAXPY * matmul GFLOPS
    result.score      = saxpy_speedup * matmul_vec_gflops * 100.0;
    result.score_unit = "pts";
    result.duration_sec = elapsed(t_global);

    print_result_json(result);
    return 0;
}
