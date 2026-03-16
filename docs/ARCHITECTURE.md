# OpenRVBench Architecture

## Overview

OpenRVBench is built on three principles:

1. **Portability** — works on any rv64gc Linux system without root
2. **Modularity** — each benchmark is an independent binary
3. **Extensibility** — adding a new benchmark requires ~4 files

---

## Component Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    openrvbench CLI (Python)                      │
│  ┌────────────┐  ┌──────────────┐  ┌────────────────────────┐  │
│  │  Argument  │  │  Board       │  │  Result Manager        │  │
│  │  Parser    │  │  Detector    │  │  (save/load JSON)      │  │
│  └────────────┘  └──────────────┘  └────────────────────────┘  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │              Benchmark Runner                            │    │
│  │  subprocess → bench_XXX → capture stdout → parse JSON   │    │
│  └─────────────────────────────────────────────────────────┘    │
│  ┌────────────────────────┐  ┌───────────────────────────────┐  │
│  │  Terminal Formatter    │  │  HTML Report Generator        │  │
│  └────────────────────────┘  └───────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
                         │ subprocess per bench
         ┌───────────────┼───────────────────────────────┐
         ▼               ▼               ▼               ▼
   ┌──────────┐  ┌──────────────┐  ┌──────────┐  ┌──────────┐
   │bench_cpu │  │bench_memory  │  │bench_...│  │bench_ai  │
   │(C++/CMake│  │(C++/CMake)  │  │         │  │(C++ +    │
   │ binary)  │  │             │  │         │  │ Python   │
   └──────────┘  └──────────────┘  └──────────┘  └──────────┘
         │               │               │               │
         └───────────────┴───────────────┴───────────────┘
                         │ JSON to stdout
                  ┌──────▼──────┐
                  │SystemMonitor│  ← /proc/cpuinfo, sysfs, thermal
                  │(shared lib) │
                  └─────────────┘
```

---

## Module Interactions

### Benchmark Binary Protocol

Each `bench_XXX` binary:
- Accepts optional command-line arguments (e.g., model directory for AI)
- Runs its workloads
- Prints **one JSON object** to stdout matching `BenchResult`
- Exits 0 on success, non-zero on fatal error

```
{
  "bench_id":    "cpu",
  "bench_name":  "CPU Benchmark",
  "score":       1120.0,
  "score_unit":  "pts",
  "duration_sec": 14.2,
  "passed":      true,
  "error":       "",
  "metrics": [
    { "name": "fp_gflops", "value": 0.82, "unit": "GFLOPS", "desc": "..." },
    ...
  ]
}
```

### SystemMonitor (shared library)

All binaries link against `libopenrv_monitor.a`, which provides:
- `detect_board()` — reads `/proc/device-tree/model`, `/proc/cpuinfo`
- `detect_rvv()` — parses ISA string for 'v' extension
- `read_thermal()` — reads `/sys/class/thermal/thermal_zone*/temp`
- `read_cpufreq()` — reads `/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq`
- `start_thermal_monitor()` — background thread for thermal polling

### Result Flow

```
bench binary stdout
    → subprocess.run(capture_output=True)
    → json.loads(proc.stdout)
    → BenchResult dict
    → print_result_summary() → terminal
    → save_result()          → results/BoardName_YYYYMMDD.json
    → generate_html_report() → report.html  (if --html flag)
```

---

## RVV Build Strategy

The vector benchmark uses two compilation paths:

| Build | Flag | Path used |
|---|---|---|
| Base RISC-V | `-march=rv64gc` | All code uses scalar/auto-vec |
| RVV enabled | `-march=rv64gcv` | `#ifdef HAS_RVV` intrinsic paths |

CMake detects RVV support via `check_cxx_compiler_flag("-march=rv64gcv")`.
The `bench_vector` binary is compiled with `-march=rv64gcv` when available.
All other binaries use `-march=rv64gc` for maximum portability.

```cmake
if(COMPILER_SUPPORTS_RVV)
    target_compile_options(bench_vector PRIVATE -march=rv64gcv -mabi=lp64d)
endif()
```

---

## Scoring Model

Each benchmark produces:
- A **raw score** in domain units (MB/s, GFLOPS, IOPS, etc.)
- A **normalised composite score** in `pts` (points)

The composite score is designed so:
- Higher is always better
- A "baseline" RISC-V board (VisionFive 2 class) scores ~1000 pts per module
- A high-end board (SpacemiT K1 with RVV) scores ~1500–2500 pts per module

Scoring formulas are documented in each benchmark's source file.

The overall score is the **sum** of per-module scores (not weighted average),
so running more benchmarks produces a higher total. This is intentional —
use per-module scores for category comparisons.

---

## Adding a New Benchmark

### Step 1 — Create the C++ source

```cpp
// benchmarks/mybench/mybench_bench.cpp
#include "../../results/result_writer.h"
#include "../../monitoring/system_monitor.h"
using namespace openrv;

int main() {
    BenchResult r;
    r.bench_id   = "mybench";
    r.bench_name = "My Custom Benchmark";
    r.passed     = true;

    // ... do work ...

    r.metrics.push_back({"my_metric", 42.0, "units", "Description"});
    r.score      = 42.0;
    r.score_unit = "units";
    r.duration_sec = /* elapsed time */;

    print_result_json(r);
    return 0;
}
```

### Step 2 — CMakeLists.txt

```cmake
# benchmarks/mybench/CMakeLists.txt
add_executable(bench_mybench mybench_bench.cpp)
target_include_directories(bench_mybench PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(bench_mybench PRIVATE openrv_result openrv_monitor)
install(TARGETS bench_mybench RUNTIME DESTINATION lib/openrvbench/modules)
```

### Step 3 — Add to root CMakeLists.txt

```cmake
add_subdirectory(benchmarks/mybench)
```

### Step 4 — Register in CLI

```python
# cli/openrvbench  →  BENCH_REGISTRY
BENCH_REGISTRY = {
    ...
    "mybench": ("bench_mybench", "My Custom Benchmark", 60),
    #            binary name      description            timeout_secs
}
```

That's it. The CLI will auto-discover the binary, run it, display results,
save to JSON, and include it in HTML reports.

---

## File Format — Result JSON

```
{
  "version":    string,             // "1.0"
  "timestamp":  ISO-8601 string,
  "board": {
    "board":    string,             // e.g. "Orange Pi RV2"
    "isa":      string,             // e.g. "rv64gcv_zicsr_..."
    "has_rvv":  bool,
    "cores":    int,
    "ram_gb":   float,
    "kernel":   string,
    "os":       string
  },
  "benchmarks": [
    {
      "bench_id":    string,        // "cpu", "memory", etc.
      "bench_name":  string,        // human-readable
      "score":       float,
      "score_unit":  string,        // "pts", "MB/s", "GFLOPS"
      "duration_sec": float,
      "passed":      bool,
      "error":       string,        // empty if passed
      "metrics": [
        {
          "name":  string,
          "value": float | int | bool | string,
          "unit":  string,
          "desc":  string
        }
      ]
    }
  ]
}
```

---

## Security & Safety Notes

- The storage benchmark creates and deletes `/tmp/openrvbench_storage_test.bin`
- The thermal benchmark stresses all cores for 60 seconds — ensure adequate cooling
- Network benchmark uses only loopback (127.0.0.1), no external connections
- All benchmarks run as the invoking user — no root required
- No network communication, no telemetry, fully offline

---

## Known Limitations

1. **Cache sizes**: L1/L2/LLC sizes are inferred from buffer sweep, not read from hardware topology. Use `lscpu` or `/sys/devices/system/cpu/cpu0/cache/` for exact values.

2. **AI benchmark**: Requires manual installation of llama.cpp and/or ONNX Runtime. The GEMM proxy always runs as a lightweight substitute.

3. **Storage benchmark**: Uses `/tmp` by default — this may be tmpfs (RAM-backed) on some boards. Set `TEST_FILE` to a real block device path for accurate disk I/O measurements.

4. **RVV VLEN**: The vector benchmark uses LMUL=4/8 intrinsics. Actual VLEN (64, 128, 256, 512 bits) depends on the hardware; performance will vary accordingly.
