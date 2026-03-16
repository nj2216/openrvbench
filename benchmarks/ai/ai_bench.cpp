// ─── OpenRVBench :: AI Inference Benchmark ───────────────────────────────────
// Wraps llama.cpp and ONNX Runtime to measure inference performance.
// If libraries are not present, reports graceful "not available" result.
//
// Benchmarks:
//   1. TinyLlama-1.1B   — tokens/sec (llama.cpp backend)
//   2. MobileNetV2      — inferences/sec (ONNX Runtime backend)
//   3. Matrix FP16 GEMM — proxy for NPU/accelerator readiness
//
// Build: only included when ENABLE_AI_BENCH=ON and deps are found.
// Output: JSON BenchResult to stdout
// ─────────────────────────────────────────────────────────────────────────────
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <cstring>
#include <cmath>
#include <thread>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "../../results/result_writer.h"
#include "../../monitoring/system_monitor.h"

using namespace openrv;
using Clock = std::chrono::high_resolution_clock;
using Seconds = std::chrono::duration<double>;

static double elapsed(Clock::time_point t0) {
    return Seconds(Clock::now() - t0).count();
}

// ─────────────────────────────────────────────────────────────────────────────
// LLAMA.CPP WRAPPER
// Invokes llama-bench (part of llama.cpp) as a subprocess and parses output.
// ─────────────────────────────────────────────────────────────────────────────
struct LlamaResult {
    bool   available;
    double prompt_tps;    // tokens/sec prompt processing
    double gen_tps;       // tokens/sec generation
    int    n_threads;
    std::string model_name;
    std::string error;
};

static LlamaResult run_llama_bench(const std::string& model_path,
                                    int threads = -1) {
    LlamaResult r{};
    r.available = false;

    // Check if llama-bench or llama.cpp main is in PATH
    if (system("which llama-bench > /dev/null 2>&1") != 0 &&
        system("which llama-cli  > /dev/null 2>&1") != 0) {
        r.error = "llama-bench not found in PATH. "
                  "Install llama.cpp and add to PATH.";
        return r;
    }

    // Check model file
    std::ifstream f(model_path);
    if (!f.good()) {
        r.error = "Model file not found: " + model_path +
                  "\nDownload TinyLlama: "
                  "huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF";
        return r;
    }
    f.close();

    if (threads < 0)
        threads = static_cast<int>(std::thread::hardware_concurrency());

    // Build command
    // llama-bench -m <model> -t <threads> -p 512 -n 128 --output json
    std::ostringstream cmd;
    cmd << "llama-bench"
        << " -m " << model_path
        << " -t " << threads
        << " -p 512 -n 128"
        << " --output json 2>/dev/null";

    // Run and capture output
    FILE* pipe = popen(cmd.str().c_str(), "r");
    if (!pipe) {
        r.error = "Failed to run llama-bench";
        return r;
    }

    std::string output;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe))
        output += buf;
    pclose(pipe);

    // Parse JSON output — look for "pp" (prompt processing) and "tg" (token gen)
    // llama-bench JSON format: [{"model":"...","pp":XXX,"tg":XXX,...},...]
    auto extract = [&](const std::string& key) -> double {
        auto pos = output.find("\"" + key + "\":");
        if (pos == std::string::npos) return 0.0;
        pos += key.size() + 3;
        return std::stod(output.substr(pos, 20));
    };

    r.prompt_tps  = extract("pp");
    r.gen_tps     = extract("tg");
    r.n_threads   = threads;
    r.model_name  = "TinyLlama-1.1B";
    r.available   = (r.prompt_tps > 0 || r.gen_tps > 0);

    if (!r.available)
        r.error = "llama-bench ran but produced no parseable output";

    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// ONNX RUNTIME WRAPPER
// Invokes Python onnxruntime to run MobileNetV2 inference.
// Falls back gracefully if not installed.
// ─────────────────────────────────────────────────────────────────────────────
struct OnnxResult {
    bool   available;
    double inferences_per_sec;
    double avg_latency_ms;
    std::string model_name;
    std::string error;
};

static OnnxResult run_onnx_bench(const std::string& model_path,
                                   int n_runs = 100) {
    OnnxResult r{};
    r.available = false;

    // Check Python + onnxruntime
    if (system("python3 -c 'import onnxruntime' > /dev/null 2>&1") != 0) {
        r.error = "onnxruntime not installed. "
                  "Install: pip install onnxruntime";
        return r;
    }

    // Check model
    std::ifstream f(model_path);
    if (!f.good()) {
        r.error = "ONNX model not found: " + model_path +
                  "\nDownload MobileNetV2: "
                  "github.com/onnx/models/tree/main/validated/vision/classification/mobilenet";
        return r;
    }
    f.close();

    // Inline Python script passed via -c
    std::ostringstream py;
    py << "import onnxruntime as ort, numpy as np, time, json\n"
       << "sess = ort.InferenceSession('" << model_path << "')\n"
       << "inp  = sess.get_inputs()[0]\n"
       << "data = np.random.rand(*inp.shape).astype(np.float32)\n"
       << "# Warmup\n"
       << "for _ in range(5): sess.run(None, {inp.name: data})\n"
       << "# Benchmark\n"
       << "t0 = time.perf_counter()\n"
       << "for _ in range(" << n_runs << "): sess.run(None, {inp.name: data})\n"
       << "dt = time.perf_counter() - t0\n"
       << "print(json.dumps({'ips': " << n_runs << "/dt, "
       << "'lat_ms': dt/" << n_runs << "*1000}))\n";

    // Write temp script
    std::ofstream tmp("/tmp/openrv_onnx_bench.py");
    tmp << py.str();
    tmp.close();

    FILE* pipe = popen("python3 /tmp/openrv_onnx_bench.py 2>/dev/null", "r");
    if (!pipe) {
        r.error = "Failed to run ONNX bench script";
        return r;
    }

    std::string out;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    pclose(pipe);
    remove("/tmp/openrv_onnx_bench.py");

    // Parse {"ips": X, "lat_ms": Y}
    auto extract = [&](const std::string& key) -> double {
        auto pos = out.find("\"" + key + "\": ");
        if (pos == std::string::npos) return 0.0;
        pos += key.size() + 4;
        return std::stod(out.substr(pos, 20));
    };

    r.inferences_per_sec = extract("ips");
    r.avg_latency_ms     = extract("lat_ms");
    r.model_name         = "MobileNetV2";
    r.available          = (r.inferences_per_sec > 0);

    if (!r.available)
        r.error = "ONNX bench ran but produced no parseable output";

    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// GEMM FP16/FP32 PROXY BENCHMARK
// A pure C++ matrix multiply that acts as a proxy for AI accelerator perf.
// ─────────────────────────────────────────────────────────────────────────────
static double bench_gemm_proxy(int N = 512, int repeats = 3) {
    std::vector<float> A(N*N), B(N*N), C(N*N, 0.0f);

    for (int i = 0; i < N*N; ++i) {
        A[i] = static_cast<float>(i % 256) / 255.0f;
        B[i] = static_cast<float>((i * 7) % 256) / 255.0f;
    }

    double best_gflops = 0.0;
    for (int rep = 0; rep < repeats; ++rep) {
        std::fill(C.begin(), C.end(), 0.0f);
        auto t0 = Clock::now();

        // Tiled matrix multiply
        const int T = 32;  // tile size
        for (int i = 0; i < N; i += T)
        for (int k = 0; k < N; k += T)
        for (int j = 0; j < N; j += T) {
            int i_max = std::min(i + T, N);
            int k_max = std::min(k + T, N);
            int j_max = std::min(j + T, N);
            for (int ii = i; ii < i_max; ++ii)
            for (int kk = k; kk < k_max; ++kk) {
                float aik = A[ii*N + kk];
                for (int jj = j; jj < j_max; ++jj)
                    C[ii*N + jj] += aik * B[kk*N + jj];
            }
        }

        double secs = elapsed(t0);
        double gflops = (2.0 * N * N * N) / secs / 1e9;
        best_gflops = std::max(best_gflops, gflops);
    }
    return best_gflops;
}

// ─────────────────────────────────────────────────────────────────────────────
// MAIN
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    // Optional argument: model directory
    std::string model_dir = ".";
    if (argc > 1) model_dir = argv[1];

    std::string llama_model = model_dir + "/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf";
    std::string onnx_model  = model_dir + "/mobilenetv2-12.onnx";

    BenchResult result;
    result.bench_id   = "ai";
    result.bench_name = "AI Inference Benchmark";
    result.passed     = true;

    auto t_global = Clock::now();

    // ── GEMM proxy (always runs) ──────────────────────────────────────────
    double gemm_gflops = bench_gemm_proxy(512, 3);
    result.metrics.push_back({"gemm_proxy_gflops", gemm_gflops, "GFLOPS",
                               "Tiled FP32 GEMM (512x512) — AI workload proxy"});

    // ── llama.cpp ─────────────────────────────────────────────────────────
    auto llama = run_llama_bench(llama_model);
    result.metrics.push_back({"llama_available", llama.available, "",
                               "llama.cpp + TinyLlama model present"});
    if (llama.available) {
        result.metrics.push_back({"tinyllama_prompt_tps", llama.prompt_tps,
                                   "tok/s", "TinyLlama prompt processing speed"});
        result.metrics.push_back({"tinyllama_gen_tps",    llama.gen_tps,
                                   "tok/s", "TinyLlama token generation speed"});
        result.metrics.push_back({"llama_threads", (int64_t)llama.n_threads,
                                   "threads", "Threads used for inference"});
    } else {
        result.metrics.push_back({"llama_skip_reason", llama.error,
                                   "", "Reason llama.cpp bench was skipped"});
    }

    // ── ONNX Runtime ──────────────────────────────────────────────────────
    auto onnx = run_onnx_bench(onnx_model, 100);
    result.metrics.push_back({"onnx_available", onnx.available, "",
                               "ONNX Runtime + MobileNetV2 model present"});
    if (onnx.available) {
        result.metrics.push_back({"mobilenet_ips",     onnx.inferences_per_sec,
                                   "inf/s", "MobileNetV2 inferences per second"});
        result.metrics.push_back({"mobilenet_lat_ms",  onnx.avg_latency_ms,
                                   "ms",    "MobileNetV2 average inference latency"});
    } else {
        result.metrics.push_back({"onnx_skip_reason", onnx.error,
                                   "", "Reason ONNX bench was skipped"});
    }

    // ── Composite score ───────────────────────────────────────────────────
    double score = gemm_gflops * 100.0;
    if (llama.available) score += llama.gen_tps * 10.0;
    if (onnx.available)  score += onnx.inferences_per_sec;
    result.score      = score;
    result.score_unit = "pts";
    result.duration_sec = elapsed(t_global);

    print_result_json(result);
    return 0;
}
