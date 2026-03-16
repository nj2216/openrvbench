#pragma once
// ─── OpenRVBench :: System Monitor ────────────────────────────────────────────
// Provides board detection, CPU info, temperature, and thermal throttle
// detection for RISC-V Linux systems.
// ─────────────────────────────────────────────────────────────────────────────
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace openrv {

// ─── Board info ───────────────────────────────────────────────────────────────
struct BoardInfo {
    std::string board_name;      // e.g. "Orange Pi RV2"
    std::string soc_name;        // e.g. "SpacemiT K1"
    std::string cpu_arch;        // e.g. "rv64gc", "rv64gcv"
    std::string kernel_version;
    std::string os_release;
    uint32_t    num_cores;
    uint64_t    total_ram_mb;
    bool        has_rvv;         // RISC-V Vector Extension
    bool        has_fpu;
    std::string cpu_freq_mhz;    // "1600" or "800-1600"
};

// ─── Thermal snapshot ─────────────────────────────────────────────────────────
struct ThermalSnapshot {
    float    cpu_temp_celsius;
    float    soc_temp_celsius;
    uint32_t cpu_freq_mhz;       // current frequency (throttle detection)
    uint32_t cpu_freq_max_mhz;   // max rated frequency
    bool     throttled;          // freq < 90% of max
};

// ─── Memory info ──────────────────────────────────────────────────────────────
struct MemInfo {
    uint64_t total_kb;
    uint64_t free_kb;
    uint64_t available_kb;
    uint64_t cached_kb;
};

// ─── CPU statistics (per core) ────────────────────────────────────────────────
struct CoreStats {
    uint32_t    core_id;
    uint64_t    freq_mhz;
    float       usage_pct;
};

// ─────────────────────────────────────────────────────────────────────────────
class SystemMonitor {
public:
    SystemMonitor();
    ~SystemMonitor() = default;

    // ── Detection ──────────────────────────────────────────────────────────
    BoardInfo          detect_board()    const;
    bool               detect_rvv()      const;
    std::string        detect_isa_string() const;

    // ── Real-time readings ─────────────────────────────────────────────────
    ThermalSnapshot    read_thermal()    const;
    MemInfo            read_meminfo()    const;
    std::vector<CoreStats> read_cores()  const;
    uint64_t           read_cpu_freq_mhz() const;

    // ── Helpers ────────────────────────────────────────────────────────────
    static std::string get_kernel_version();
    static std::string get_os_release();
    static uint32_t    get_nprocs();

    // ── Monitoring loop (used by thermal bench) ────────────────────────────
    using ThermalCallback = std::function<void(const ThermalSnapshot&)>;
    void start_thermal_monitor(int interval_ms, ThermalCallback cb);
    void stop_thermal_monitor();

private:
    bool monitoring_active_ = false;

    // Internal helpers
    std::optional<float> read_thermal_zone(int zone_id) const;
    std::optional<uint64_t> read_cpufreq(int cpu_id) const;
    std::string read_file(const std::string& path) const;
    std::string detect_board_from_dt()   const;  // /proc/device-tree
    std::string detect_board_from_cpuinfo() const;
};

} // namespace openrv
