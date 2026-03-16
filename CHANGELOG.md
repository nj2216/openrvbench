# Changelog

All notable changes to OpenRVBench will be documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [1.0.0] - 2026-03-16

### Added
- CPU benchmark: integer, FP, multi-thread scaling, LZ77 compression
- Vector benchmark: SAXPY, matrix multiply, dot product (RVV intrinsics)
- Memory benchmark: sequential bandwidth, pointer-chase latency, cache hierarchy
- Cryptography benchmark: AES-256-CTR, SHA-256, ChaCha20 (pure C++, no deps)
- Storage benchmark: sequential read/write, random 4K IOPS
- Network benchmark: TCP loopback throughput, UDP round-trip latency
- AI benchmark: GEMM proxy + llama.cpp + ONNX Runtime wrappers
- Thermal benchmark: 60 s stress, throttle detection, cooldown measurement
- Python CLI orchestrator: run / compare / report / leaderboard / info
- Self-contained HTML reports with radar + bar charts (Chart.js)
- Board auto-detection via /proc/device-tree/model
- RVV auto-detection via ISA string parsing
- build.sh with --compiler / --cc / --cxx / CC / CXX support
- Results saved to ~/.local/share/openrvbench/results/

### Supported boards (tested)
- Orange Pi RV2 (SpacemiT K1, rv64gcv)
- VisionFive 2 (StarFive JH7110, rv64gc)
