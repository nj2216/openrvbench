# Contributing to OpenRVBench

Thank you for your interest in contributing! OpenRVBench aims to become
the standard benchmark suite for RISC-V hardware.

---

## Ways to Contribute

### 1. Submit benchmark results
Run OpenRVBench on your RISC-V board and submit the JSON result file
to the [results repository](https://github.com/your-org/openrvbench-results).
This helps build the community leaderboard.

### 2. Add a new board mapping
Edit `monitoring/system_monitor.cpp`, add an entry to `soc_map[]`:
```cpp
{"My Board Name", "My SoC Name"},
```
Open a PR with the device-tree model string from `/proc/device-tree/model`.

### 3. Add a new benchmark module
See `docs/ARCHITECTURE.md` — Adding a New Benchmark (4 steps).

### 4. Fix a bug or improve accuracy
Open an issue describing the problem, then submit a PR.

---

## Code Style

- **C++17** throughout; no C++20 features (compiler support varies on RISC-V)
- No external dependencies in core benchmarks (crypto, cpu, memory)
- Each benchmark must be **self-contained** — no shared state with other modules
- Use `volatile` sinks to prevent dead-code elimination
- Always measure elapsed time with `std::chrono::high_resolution_clock`
- Score formulas must be documented with comments
- All benchmarks must handle `--json` stdout protocol

## Python Style
- Python 3.8+ compatible
- No third-party dependencies in `cli/` or `scripts/`
- Type hints encouraged
- 4-space indent

---

## Pull Request Checklist

- [ ] New benchmark compiles with `cmake -DENABLE_RVV=OFF` (no RVV)
- [ ] New benchmark compiles with `cmake -DENABLE_RVV=ON` (with RVV)  
- [ ] Outputs valid JSON matching the `BenchResult` schema
- [ ] Registered in `BENCH_REGISTRY` in `cli/openrvbench`
- [ ] `openrvbench info` shows the new benchmark
- [ ] `openrvbench run mybench` runs and prints a result
- [ ] Result appears in HTML report
- [ ] Added to `docs/ARCHITECTURE.md` benchmark table

---

## Reporting Issues

Please include:
- Board name and SoC
- Output of `openrvbench info`
- Output of `cat /proc/cpuinfo | head -30`
- Full error output with `--json` flag
