# OpenRVBench 🔬

**The open-source benchmark suite for RISC-V hardware.**

OpenRVBench is a modular, lightweight, and extensible benchmarking framework
designed specifically for the RISC-V ecosystem. It measures real-world
performance across CPU, vector extensions (RVV), memory, AI inference,
cryptography, storage, networking, and thermal behaviour.

---

## Supported Hardware

| Board | SoC | RVV | Tested |
|---|---|---|---|
| Orange Pi RV2 | SpacemiT K1 | ✓ | ✓ |
| VisionFive 2 | StarFive JH7110 | ✗ | ✓ |
| Lichee Pi 4A | T-Head TH1520 | ✓ | ✓ |
| Milk-V Pioneer | SG2042 | ✓ | ✓ |
| HiFive Unmatched | SiFive FU740 | ✗ | ✓ |
| Any rv64gc/rv64gcv Linux SBC | — | optional | ✓ |

---

## Quick Start

```bash
# 1. Clone
git clone https://github.com/your-org/openrvbench.git
cd openrvbench

# 2. Build (auto-detects RVV support)
./scripts/build.sh

# 3. Run all benchmarks
openrvbench run all

# 4. Run a specific benchmark
openrvbench run cpu
openrvbench run memory
openrvbench run ai --model-dir /path/to/models

# 5. View system info
openrvbench info

# 6. Compare results across boards
openrvbench compare results/

# 7. Generate HTML report
openrvbench report results/OrangePiRV2_20250301.json
```

---

## Example Output

```
  OpenRVBench Results Summary
  ══════════════════════════════════════════════════════════════════════
  Board    : Orange Pi RV2
  Date     : 2025-03-01 14:22

  Benchmark                      Score         Unit    Status
  ─────────────────────────────────────────────────────────────────
  CPU (integer, FP, multi-thread)        1120.0  pts     ✓
  Vector Extension (RVV / SAXPY)          420.0  pts     ✓
  Memory (bandwidth, latency)            1840.0  pts     ✓
  Cryptography (AES-256, SHA-256)         650.0  MB/s    ✓
  Storage (sequential + random I/O)       380.0  MB/s    ✓
  Network (TCP throughput, UDP)          4820.0  MB/s    ✓
  AI Inference (GEMM + llama.cpp)         720.0  pts     ✓
  ─────────────────────────────────────────────────────────────────
  Benchmarks run                               7

  Overall Score : 9950.0 pts
  ══════════════════════════════════════════════════════════════════════
```

---

## Benchmark Modules

### ⚙️ CPU Benchmark (`bench_cpu`)
- **Integer throughput** — XOR-shift + multiply-accumulate (MOPS)
- **Floating-point throughput** — Mandelbrot inner loop (GFLOPS)
- **Multi-thread scaling** — 1-to-N-core scaling factor
- **Compression workload** — LZ77-style hash chain (MB/s)

### 🧮 Vector Extension Benchmark (`bench_vector`)
- **SAXPY** — scalar vs RVV-intrinsic comparison (GB/s, speedup)
- **Matrix multiply** — tiled FP32 GEMM scalar vs RVV (GFLOPS, speedup)
- **Dot product** — reduction benchmark
- Requires: `rv64gcv` compiler support (gracefully degrades to auto-vec)

### 💾 Memory Benchmark (`bench_memory`)
- **Sequential bandwidth** — read, write, copy (GB/s)
- **Random latency** — pointer-chasing across L1/L2/LLC/DRAM levels
- **Cache hierarchy** — sweeps 16 KB → 64 MB buffers

### 🔐 Crypto Benchmark (`bench_crypto`)
- **AES-256-CTR** — pure C++ T-table implementation (MB/s)
- **SHA-256** — FIPS 180-4 reference (MB/s)
- **ChaCha20** — RFC 7539 stream cipher (MB/s)
- No external library required (OpenSSL optional for hw-accel comparison)

### 📀 Storage Benchmark (`bench_storage`)
- **Sequential read/write** — 512 MB, 1 MB blocks (MB/s)
- **Random 4K read/write** — IOPS and latency
- Uses `O_DIRECT` and `O_SYNC` to bypass page cache

### 🌐 Network Benchmark (`bench_network`)
- **TCP loopback throughput** — 512 MB transfer (MB/s)
- **UDP round-trip latency** — 5000 ping-pong packets (µs)
- No external network required (loopback only)

### 🤖 AI Inference Benchmark (`bench_ai`)
- **GEMM FP32 proxy** — tiled 512×512 matrix multiply (always runs)
- **TinyLlama-1.1B** — tokens/sec via llama.cpp (optional)
- **MobileNetV2** — inferences/sec via ONNX Runtime (optional)

### 🌡️ Thermal Benchmark (`bench_thermal`)
- **60-second CPU stress** — all cores, FPU-heavy workload
- **Temperature monitoring** — peak, average, idle, cooldown
- **Throttle detection** — reports throttling percentage

---

## Architecture

```
openrvbench/
├── cli/
│   └── openrvbench          # Python CLI orchestrator
├── benchmarks/
│   ├── cpu/                 # bench_cpu.cpp
│   ├── vector/              # bench_vector.cpp  (RVV intrinsics)
│   ├── memory/              # bench_memory.cpp
│   ├── crypto/              # bench_crypto.cpp  (AES/SHA/ChaCha20)
│   ├── storage/             # bench_storage.cpp
│   ├── network/             # bench_network.cpp
│   ├── ai/                  # bench_ai.cpp      (llama.cpp + ONNX)
│   └── thermal/             # bench_thermal.cpp
├── monitoring/
│   ├── system_monitor.h     # Board detect, thermal, CPU freq
│   └── system_monitor.cpp
├── results/
│   └── result_writer.h      # JSON output primitives
├── scripts/
│   ├── build.sh             # Build & install script
│   ├── board_detect.py      # Standalone board probe
│   ├── compare_results.py   # Multi-board comparison
│   └── report_generator.py  # HTML report generator
├── docs/
│   ├── ARCHITECTURE.md
│   └── CONTRIBUTING.md
└── CMakeLists.txt
```

### Data Flow

```
openrvbench run all
        │
        ├── detect_board()          ← /proc/device-tree, /proc/cpuinfo
        │
        └── for each benchmark:
              run bench_XXX binary  ← C++ subprocess
                    │
                    └── prints JSON result to stdout
                              │
                    Python parses & collects
                              │
                    ┌─────────┴──────────┐
                    │                    │
              print summary       save .json file
                                        │
                                  generate .html
```

### Adding a New Benchmark

1. Create `benchmarks/mybench/mybench_bench.cpp`
2. Output a `BenchResult` JSON using `print_result_json()` from `result_writer.h`
3. Add a `CMakeLists.txt` with `add_executable(bench_mybench ...)`
4. Register in `cli/openrvbench` under `BENCH_REGISTRY`
5. Done — the orchestrator auto-discovers and runs it

---

## Build Options

```bash
./scripts/build.sh [options]

  --with-rvv      Force enable RVV (default: auto-detect)
  --no-rvv        Disable RVV
  --with-ai       Build AI benchmark (llama.cpp/ONNX optional)
  --release       Full optimisation (-O3 + LTO)
  --jobs N        Parallel build jobs (default: nproc)
  --prefix PATH   Install prefix (default: /usr/local)
  --clean         Clean build dir first
```

Or use CMake directly:
```bash
cmake -B build -DENABLE_RVV=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

---

## Output Formats

### CLI Summary
Human-readable tables printed to stdout.

### JSON (`results/*.json`)
Machine-readable result files stored in `results/`. Structure:
```json
{
  "version": "1.0",
  "timestamp": "2025-03-01T14:22:00",
  "board": { "board": "Orange Pi RV2", "isa": "rv64gcv...", ... },
  "benchmarks": [
    {
      "bench_id": "cpu",
      "score": 1120.0,
      "metrics": [ { "name": "fp_gflops", "value": 0.82, "unit": "GFLOPS" } ]
    }
  ]
}
```

### HTML Report
Self-contained, dark-mode HTML with:
- System info panel
- Score hero
- Radar chart (% of baseline per category)
- Bar chart per benchmark
- Per-benchmark metric tables

Generate: `openrvbench report results/myboard.json`

---

## AI Benchmark Setup

The AI benchmark degrades gracefully — the GEMM proxy always runs.
For full inference benchmarks:

**TinyLlama (llama.cpp):**
```bash
# Install llama.cpp
git clone https://github.com/ggerganov/llama.cpp && cd llama.cpp
cmake -B build -DGGML_RISCV=ON && cmake --build build -j$(nproc)
sudo cp build/bin/llama-bench /usr/local/bin/

# Download model
wget https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf

# Run
openrvbench run ai --model-dir .
```

**MobileNetV2 (ONNX Runtime):**
```bash
pip install onnxruntime numpy
wget https://github.com/onnx/models/raw/main/validated/vision/classification/mobilenet/model/mobilenetv2-12.onnx
openrvbench run ai --model-dir .
```

---

## CLI Reference

```
openrvbench run all                     Run all benchmarks
openrvbench run cpu                     Run only CPU benchmark
openrvbench run cpu,memory,crypto       Run subset (comma-separated)
openrvbench run all --include-thermal   Include 60s thermal stress
openrvbench run all --html report.html  Generate HTML report after run
openrvbench run all --json              Also print full JSON to stdout
openrvbench run ai --model-dir DIR      Specify AI model directory

openrvbench compare results/            Compare all results in directory
openrvbench report results/file.json    Generate HTML from saved result
openrvbench leaderboard results/        Show ranked leaderboard
openrvbench info                        Show system info + binary status
```

---

## Contributing

See [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) for:
- Coding style
- How to add a new benchmark
- How to add a new board mapping
- Testing requirements

---

## License

MIT License — see [LICENSE](LICENSE).

---

## Acknowledgements

Inspired by Phoronix Test Suite, Geekbench, and the broader RISC-V open-source community.
Built for boards like the Orange Pi RV2, VisionFive 2, Milk-V Pioneer, and every RISC-V
SBC that deserves a proper benchmark.
