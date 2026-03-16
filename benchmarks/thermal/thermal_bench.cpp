// ─── OpenRVBench :: Thermal Benchmark ────────────────────────────────────────
// Stresses all CPU cores while monitoring temperature and frequency.
// Detects thermal throttling and reports peak/sustained temperatures.
//
// Output: JSON BenchResult to stdout
// ─────────────────────────────────────────────────────────────────────────────
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <mutex>
#include <functional>

#include "../../results/result_writer.h"
#include "../../monitoring/system_monitor.h"

using namespace openrv;
using Clock = std::chrono::high_resolution_clock;
using Seconds = std::chrono::duration<double>;

static double elapsed(Clock::time_point t0) {
    return Seconds(Clock::now() - t0).count();
}

// ─────────────────────────────────────────────────────────────────────────────
// CPU STRESS WORKER
// A computation that is hard to optimise away and keeps the ALU/FPU busy.
// ─────────────────────────────────────────────────────────────────────────────
static volatile double stress_sink = 0.0;

static void stress_worker(std::atomic<bool>& stop_flag) {
    double acc = 1.0;
    uint64_t i  = 0;
    while (!stop_flag.load(std::memory_order_relaxed)) {
        // Transcendental mix — forces the FPU to stay active
        acc = std::sqrt(std::fabs(std::sin(acc * 1.00001) +
                                   std::cos(acc * 0.99999)));
        acc += (double)(++i & 0xFF) * 1e-6;
        if (i % 1000000 == 0) stress_sink = acc;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// TEMPERATURE MONITOR
// Samples at 500 ms intervals during the stress run.
// ─────────────────────────────────────────────────────────────────────────────
struct ThermalSample {
    double   time_sec;
    float    temp_c;
    uint32_t freq_mhz;
    bool     throttled;
};

// ─────────────────────────────────────────────────────────────────────────────
// MAIN
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    BenchResult result;
    result.bench_id   = "thermal";
    result.bench_name = "Thermal Benchmark";
    result.passed     = true;

    auto t_global = Clock::now();

    SystemMonitor mon;
    auto board = mon.detect_board();

    int n_threads = static_cast<int>(board.num_cores);
    if (n_threads < 1) n_threads = 1;

    const double STRESS_DURATION = 60.0;   // seconds of stress
    const int    SAMPLE_INTERVAL_MS = 500; // thermal sample every 500ms

    result.metrics.push_back({"stress_threads",    (int64_t)n_threads, "threads",
                               "Number of stress threads (= logical cores)"});
    result.metrics.push_back({"stress_duration_s", STRESS_DURATION,    "s",
                               "Duration of stress phase"});

    // ── Idle temperature baseline ─────────────────────────────────────────
    auto idle_snap = mon.read_thermal();
    float idle_temp = idle_snap.cpu_temp_celsius;
    result.metrics.push_back({"idle_temp_c", (double)idle_temp, "°C",
                               "CPU temperature at idle (before stress)"});

    // ── Launch stress threads ─────────────────────────────────────────────
    std::atomic<bool> stop_flag{false};
    std::vector<std::thread> workers;
    for (int i = 0; i < n_threads; ++i)
        workers.emplace_back(stress_worker, std::ref(stop_flag));

    // ── Sample thermal during stress ──────────────────────────────────────
    std::vector<ThermalSample> samples;
    auto t_stress = Clock::now();

    while (elapsed(t_stress) < STRESS_DURATION) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(SAMPLE_INTERVAL_MS));
        auto snap = mon.read_thermal();
        samples.push_back({
            elapsed(t_stress),
            snap.cpu_temp_celsius,
            snap.cpu_freq_mhz,
            snap.throttled
        });
    }

    stop_flag = true;
    for (auto& t : workers) t.join();

    // ── Analyse samples ───────────────────────────────────────────────────
    if (!samples.empty()) {
        float peak_temp = 0.0f;
        float sum_temp  = 0.0f;
        float sum_freq  = 0.0f;
        int   throttle_count = 0;

        for (auto& s : samples) {
            peak_temp = std::max(peak_temp, s.temp_c);
            sum_temp += s.temp_c;
            sum_freq += static_cast<float>(s.freq_mhz);
            if (s.throttled) ++throttle_count;
        }
        float avg_temp        = sum_temp / samples.size();
        float avg_freq        = sum_freq / samples.size();
        float throttle_pct    = 100.0f * throttle_count / samples.size();
        float temp_delta      = avg_temp - idle_temp;
        bool  throttled_at_all = (throttle_count > 0);

        result.metrics.push_back({"peak_temp_c",      (double)peak_temp,    "°C",
                                   "Peak CPU temperature during stress"});
        result.metrics.push_back({"avg_temp_c",       (double)avg_temp,     "°C",
                                   "Average CPU temperature during stress"});
        result.metrics.push_back({"temp_rise_c",      (double)temp_delta,   "°C",
                                   "Temperature rise vs idle"});
        result.metrics.push_back({"avg_freq_mhz",     (double)avg_freq,     "MHz",
                                   "Average CPU frequency during stress"});
        result.metrics.push_back({"throttle_pct",     (double)throttle_pct, "%",
                                   "Percentage of time CPU was throttled"});
        result.metrics.push_back({"throttled",        throttled_at_all,     "",
                                   "Whether any thermal throttling occurred"});
        result.metrics.push_back({"sample_count",     (int64_t)samples.size(), "samples",
                                   "Number of thermal samples collected"});

        // ── Cooling-down measurement ──────────────────────────────────────
        // Wait 10 s post-stress and record cooldown temp
        std::this_thread::sleep_for(std::chrono::seconds(10));
        auto cooldown_snap = mon.read_thermal();
        result.metrics.push_back({"cooldown_10s_temp_c",
                                   (double)cooldown_snap.cpu_temp_celsius, "°C",
                                   "CPU temperature 10 s after stress ended"});

        // Score: higher is better — penalise throttling, reward lower temps
        // Score = avg_freq / max_freq * 1000 * (1 - throttle_pct/100)
        float max_freq_mhz = static_cast<float>(
            mon.read_thermal().cpu_freq_max_mhz);
        if (max_freq_mhz <= 0) max_freq_mhz = avg_freq;
        float perf_ratio = avg_freq / max_freq_mhz;
        result.score = static_cast<double>(perf_ratio * 1000.0f *
                                            (1.0f - throttle_pct / 100.0f));
    } else {
        result.error_msg = "No thermal samples collected (sysfs unavailable?)";
        result.score = 0.0;
    }

    result.score_unit   = "pts";
    result.duration_sec = elapsed(t_global);

    print_result_json(result);
    return 0;
}
