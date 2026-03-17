# Changelog

All notable changes to OpenRVBench are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [1.1.1] - 2026-03-17

### Fixed
- `bench_extensions` Zbc carry-less multiply test was incorrectly using a software fallback instead of the intended inline assembly version. This has been corrected to properly benchmark the hardware `clmul` instruction when the Zbc extension is present.

---

## [1.1.0] - 2026-03-16

### Added
- **Extension Benchmark** (`bench_extensions`) — auto-detects RISC-V ISA extensions
  from `/proc/cpuinfo` and runs a targeted microbenchmark for each detected one
  - `M`  — integer multiply-accumulate vs XOR-shift baseline
  - `A`  — multi-thread atomic `fetch_add` throughput (all cores)
  - `F`  — FP32 FMA throughput across 1M elements
  - `D`  — FP64 FMA throughput across 1M elements
  - `Zba` — `sh3add` address generation pattern (`base + idx*8`)
  - `Zbb` — `cpop`/`clz`/`ctz` via `__builtin_popcountll` vs software Hamming weight
  - `Zbc` — carry-less multiply (`clmul`) vs 64-bit software loop
  - `Zbs` — `bset`/`bclr`/`binv`/`bext` pattern recognition
  - `Zfh` — scalar FP16 FMA via `_Float16` (skips gracefully if compiler lacks support)
  - `Zicond` — branchless select (`czero` pattern) vs branch-based ternary
  - `V`  — VLEN probe via `vlenb` CSR, reports vector register width in bits
  - Extensions not present in ISA are skipped with a clear note rather than failing
  - CMakeLists compiles with widest available `-march` so the compiler emits real HW instructions
- `openrvbench run extensions` — new CLI subcommand entry in `BENCH_REGISTRY`

### Fixed
- `ModuleNotFoundError: No module named 'scripts'` — report generator is now fully
  inlined inside `cli/openrvbench`, no external `scripts/` directory needed at runtime
- Results saving to `/usr/local/bin/results/` when CLI installed system-wide —
  `results_dir()` now uses `$XDG_DATA_HOME/openrvbench/results/`
  (defaults to `~/.local/share/openrvbench/results/`)
- `/usr/bin/env: 'python3\r': No such file or directory` — Windows CRLF line endings
  in the CLI shebang line (fix: `sed -i 's/\r//' /usr/local/bin/openrvbench`)
- `unused variable 'red'` warning in `bench_vector` — replaced incomplete RVV
  reduction stub with correct `vfredusum_vs_f32m8_f32m1` + `vfmv_f_s` sequence
- Orphaned `tmp_shift:;` goto label in `bench_crypto` causing `-Wpedantic` error
- Missing `#include <functional>` in `system_monitor.h` for `std::function`
- `posix_fadvise` declaration missing `#include <sys/mman.h>` in `bench_storage`
  on strict POSIX RISC-V toolchains

---

## [1.0.1] - 2026-03-16

### Added
- `build.sh` compiler selection flags:
  - `--compiler PROG` — derive both `CC` and `CXX` from one name
    (e.g. `--compiler riscv64-linux-gnu-gcc` → auto-derives `riscv64-linux-gnu-g++`)
  - `--cc PROG` / `--cxx PROG` — fine-grained independent control
  - `CC` / `CXX` environment variable support (standard UNIX convention)
  - Auto-detect priority: RISC-V cross-compilers → versioned native GCC → plain `gcc`/`g++`
  - RVV probe now uses the chosen compiler, not hardcoded `g++`
  - Passes `-DCMAKE_CROSSCOMPILING=TRUE` automatically when host ≠ riscv64

### Fixed
- `#include <functional>` missing in `system_monitor.h` (`std::function` undefined)
- Orphaned `tmp_shift:;` label in `crypto_bench.cpp` (no matching `goto`, `-Wpedantic` error)
- Added `#include <sys/mman.h>` to `storage_bench.cpp` for `posix_fadvise` on strict targets

---

## [1.0.0] - 2026-03-16

### Added
- **CPU Benchmark** (`bench_cpu`)
  - Integer throughput: XOR-shift + multiply-accumulate (MOPS)
  - Floating-point throughput: Mandelbrot inner loop (GFLOPS)
  - Multi-thread scaling: 1-to-N-core scaling factor and per-thread MOPS
  - Compression workload: LZ77-style hash chain (MB/s)
- **Vector Extension Benchmark** (`bench_vector`)
  - SAXPY: scalar vs RVV intrinsic (`vfmacc`, LMUL=4) with speedup ratio
  - Matrix multiply: tiled FP32 GEMM scalar vs RVV (`vfmacc`, LMUL=4)
  - Dot product: `vfredusum_vs_f32m8_f32m1` reduction
  - Compiles with `-march=rv64gcv` when supported; graceful scalar fallback
- **Memory Benchmark** (`bench_memory`)
  - Sequential read, write, and copy bandwidth (GB/s) on 256 MB buffer
  - Random latency via pointer-chasing at L1 (16 KB), L2 (256 KB),
    LLC (4 MB), and DRAM (64 MB) working sets
- **Cryptography Benchmark** (`bench_crypto`)
  - AES-256-CTR: pure C++ T-table implementation (MB/s)
  - SHA-256: FIPS 180-4 reference implementation (MB/s)
  - ChaCha20: RFC 7539 stream cipher (MB/s)
  - Zero external dependencies — no OpenSSL required
- **Storage Benchmark** (`bench_storage`)
  - Sequential write: 512 MB, 1 MB blocks, `O_SYNC` (MB/s)
  - Sequential read: 512 MB, 1 MB blocks with `POSIX_FADV_DONTNEED` (MB/s)
  - Random 4K read: 10,000 `pread` ops — IOPS and average latency (µs)
  - Random 4K write: 10,000 `pwrite` ops — IOPS
- **Network Benchmark** (`bench_network`)
  - TCP loopback throughput: 512 MB transfer (MB/s)
  - UDP round-trip latency: 5,000 ping-pong packets (µs RTT and PPS)
- **AI Inference Benchmark** (`bench_ai`)
  - GEMM FP32 proxy: tiled 512×512 matrix multiply (always runs, no deps)
  - TinyLlama-1.1B: tokens/sec via `llama-bench` subprocess (optional)
  - MobileNetV2: inferences/sec via Python ONNX Runtime subprocess (optional)
  - Graceful fallback with clear skip reasons when models/tools are absent
- **Thermal Benchmark** (`bench_thermal`)
  - 60-second full-core CPU stress (FPU-heavy transcendental loop)
  - Thermal sampling every 500 ms via `/sys/class/thermal/thermal_zone*/temp`
  - Reports: idle temp, peak temp, average temp, temperature rise, throttle %
  - Cooling measurement: CPU temp 10 s after stress ends
  - Throttle detection: current freq < 90% of `cpuinfo_max_freq`
- **Python CLI orchestrator** (`cli/openrvbench`)
  - `run [all|cpu|vector|...]` — run one or more benchmarks
  - `compare results/` — side-by-side comparison table across result files
  - `report result.json` — generate HTML report from saved result
  - `leaderboard results/` — ranked table by total score
  - `info` — system information and binary availability
  - `--html FILE` — generate HTML report after run
  - `--json` — print full JSON to stdout after run
  - `--include-thermal` — opt-in flag for 60 s thermal stress
  - `--model-dir DIR` — path to AI model files
  - ANSI colour output with `NO_COLOR` env var support
- **HTML Report Generator** (inlined in CLI, no external deps)
  - Self-contained dark-mode HTML with embedded CSS and Chart.js
  - Radar chart: per-category score as % of reference baseline
  - Bar chart: absolute score per benchmark module
  - Per-benchmark detail cards with full metric tables
- **System Monitor** (`monitoring/system_monitor.cpp`)
  - Board detection via `/proc/device-tree/model` (DT) and `/proc/cpuinfo`
  - Known SoC mappings: SpacemiT K1, StarFive JH7110/JH7100, T-Head TH1520,
    SG2042, SiFive FU740/P550
  - RVV detection from ISA string `v` extension token
  - Thermal zone reading from `/sys/class/thermal/thermal_zone*/temp`
  - CPU frequency reading from `/sys/devices/system/cpu/cpu0/cpufreq/`
  - Per-core stats and background thermal monitoring thread
- **Result JSON format** — versioned schema with board info, per-benchmark
  scores, and typed metric arrays saved to `~/.local/share/openrvbench/results/`
- **Build system** (`CMakeLists.txt` + `scripts/build.sh`)
  - CMake 3.16+ with C++17
  - `ENABLE_RVV` — auto-detect via `check_cxx_compiler_flag(-march=rv64gcv)`
  - `ENABLE_OPENMP` — multi-thread benchmark support
  - `ENABLE_AI_BENCH` — opt-in AI benchmark build
  - `ENABLE_NETWORK_BENCH` — on by default
  - Cross-compile support with `-DCMAKE_CROSSCOMPILING`
- **Scripts**
  - `scripts/build.sh` — build and install with compiler selection
  - `scripts/install.sh` — one-liner curl installer for end users
  - `scripts/board_detect.py` — standalone board probe (thermal, storage, network)
  - `scripts/compare_results.py` — multi-board comparison with CSV export
  - `scripts/report_generator.py` — standalone HTML report generator
- **GitHub Actions CI** (`.github/workflows/ci.yml`)
  - x86 build matrix (no RVV) — compile all modules, Python syntax check
  - RISC-V cross-compile (`riscv64-linux-gnu-g++`) — verifies ELF target
  - Python lint via `pyflakes`
- **GitHub Actions Release** (`.github/workflows/release.yml`)
  - Triggered on `v*.*.*` tag push
  - Cross-compiles `rv64gc` release tarball
  - Creates GitHub Release with tarball attached and CHANGELOG as body

### Supported boards (tested)
- Orange Pi RV2 (SpacemiT K1, rv64gcv, 8-core, 4 GB)
- VisionFive 2 (StarFive JH7110, rv64gc, 4-core)