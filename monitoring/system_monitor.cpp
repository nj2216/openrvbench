// ─── OpenRVBench :: System Monitor Implementation ────────────────────────────
#include "system_monitor.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <regex>
#include <unistd.h>
#include <sys/sysinfo.h>

namespace fs = std::filesystem;
namespace openrv {

// ─────────────────────────────────────────────────────────────────────────────
SystemMonitor::SystemMonitor() {}

// ─── File read helper ─────────────────────────────────────────────────────────
std::string SystemMonitor::read_file(const std::string& path) const {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    // strip trailing newline
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

// ─── Kernel / OS ──────────────────────────────────────────────────────────────
std::string SystemMonitor::get_kernel_version() {
    std::ifstream f("/proc/version");
    if (!f.is_open()) return "unknown";
    std::string line;
    std::getline(f, line);
    // "Linux version X.Y.Z ..."
    std::smatch m;
    if (std::regex_search(line, m, std::regex(R"(Linux version (\S+))")))
        return m[1].str();
    return line.substr(0, 40);
}

std::string SystemMonitor::get_os_release() {
    std::ifstream f("/etc/os-release");
    if (!f.is_open()) return "unknown";
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            std::string val = line.substr(12);
            // Remove quotes
            val.erase(std::remove(val.begin(), val.end(), '"'), val.end());
            return val;
        }
    }
    return "unknown";
}

uint32_t SystemMonitor::get_nprocs() {
    return static_cast<uint32_t>(sysconf(_SC_NPROCESSORS_ONLN));
}

// ─── Board detection from device-tree ────────────────────────────────────────
std::string SystemMonitor::detect_board_from_dt() const {
    // DT model string is the gold standard on RISC-V Linux
    std::string model = read_file("/proc/device-tree/model");
    if (!model.empty()) return model;

    model = read_file("/sys/firmware/devicetree/base/model");
    if (!model.empty()) return model;

    return "";
}

std::string SystemMonitor::detect_board_from_cpuinfo() const {
    std::ifstream f("/proc/cpuinfo");
    if (!f.is_open()) return "";
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("Hardware", 0) == 0 || line.rfind("machine", 0) == 0) {
            auto pos = line.find(':');
            if (pos != std::string::npos)
                return line.substr(pos + 2);
        }
    }
    return "";
}

// ─── ISA string from /proc/cpuinfo ───────────────────────────────────────────
std::string SystemMonitor::detect_isa_string() const {
    std::ifstream f("/proc/cpuinfo");
    if (!f.is_open()) return "rv64gc";
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("isa", 0) == 0) {
            auto pos = line.find(':');
            if (pos != std::string::npos) {
                std::string isa = line.substr(pos + 2);
                // Trim whitespace
                isa.erase(0, isa.find_first_not_of(" \t"));
                return isa;
            }
        }
    }
    return "rv64gc";
}

// ─── RVV detection ────────────────────────────────────────────────────────────
bool SystemMonitor::detect_rvv() const {
    std::string isa = detect_isa_string();
    return isa.find('v') != std::string::npos ||
           isa.find('V') != std::string::npos;
}

// ─── Full board info ──────────────────────────────────────────────────────────
BoardInfo SystemMonitor::detect_board() const {
    BoardInfo info;

    // Board name
    info.board_name = detect_board_from_dt();
    if (info.board_name.empty())
        info.board_name = detect_board_from_cpuinfo();
    if (info.board_name.empty())
        info.board_name = "Unknown RISC-V Board";

    // Known SoC mappings
    struct SoCMap { const char* board_substr; const char* soc; };
    static const SoCMap soc_map[] = {
        {"Orange Pi RV2",   "SpacemiT K1"},
        {"VisionFive 2",    "StarFive JH7110"},
        {"VisionFive",      "StarFive JH7100"},
        {"Lichee Pi 4A",    "T-Head TH1520"},
        {"Milk-V Pioneer",  "SG2042"},
        {"HiFive Unmatched","SiFive FU740"},
        {"HiFive Premier",  "SiFive P550"},
        {nullptr, nullptr}
    };

    info.soc_name = "Unknown SoC";
    for (int i = 0; soc_map[i].board_substr; ++i) {
        if (info.board_name.find(soc_map[i].board_substr) != std::string::npos) {
            info.soc_name = soc_map[i].soc;
            break;
        }
    }

    info.cpu_arch       = detect_isa_string();
    info.kernel_version = get_kernel_version();
    info.os_release     = get_os_release();
    info.num_cores      = get_nprocs();
    info.has_rvv        = detect_rvv();
    info.has_fpu        = info.cpu_arch.find('d') != std::string::npos ||
                          info.cpu_arch.find('f') != std::string::npos;

    // RAM from sysinfo
    struct sysinfo si;
    if (::sysinfo(&si) == 0)
        info.total_ram_mb = (si.totalram * si.mem_unit) / (1024 * 1024);
    else
        info.total_ram_mb = 0;

    // CPU max freq
    std::string freq_str = read_file(
        "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    if (!freq_str.empty()) {
        try {
            uint64_t khz = std::stoull(freq_str);
            info.cpu_freq_mhz = std::to_string(khz / 1000);
        } catch (...) {}
    }
    if (info.cpu_freq_mhz.empty()) info.cpu_freq_mhz = "unknown";

    return info;
}

// ─── Thermal zone reading ─────────────────────────────────────────────────────
std::optional<float> SystemMonitor::read_thermal_zone(int zone_id) const {
    std::string path = "/sys/class/thermal/thermal_zone" +
                       std::to_string(zone_id) + "/temp";
    std::string val = read_file(path);
    if (val.empty()) return std::nullopt;
    try {
        return std::stof(val) / 1000.0f;  // millidegrees → degrees C
    } catch (...) {
        return std::nullopt;
    }
}

// ─── CPU freq reading ─────────────────────────────────────────────────────────
std::optional<uint64_t> SystemMonitor::read_cpufreq(int cpu_id) const {
    std::string path = "/sys/devices/system/cpu/cpu" +
                       std::to_string(cpu_id) + "/cpufreq/scaling_cur_freq";
    std::string val = read_file(path);
    if (val.empty()) return std::nullopt;
    try {
        return std::stoull(val) / 1000;  // kHz → MHz
    } catch (...) {
        return std::nullopt;
    }
}

uint64_t SystemMonitor::read_cpu_freq_mhz() const {
    auto f = read_cpufreq(0);
    return f.value_or(0);
}

// ─── Full thermal snapshot ────────────────────────────────────────────────────
ThermalSnapshot SystemMonitor::read_thermal() const {
    ThermalSnapshot snap{};

    // Try thermal zones 0–5 for CPU zone
    for (int i = 0; i <= 5; ++i) {
        auto t = read_thermal_zone(i);
        if (t && *t > 0.0f && *t < 150.0f) {
            snap.cpu_temp_celsius = *t;
            break;
        }
    }

    // Try zone 1 for separate SoC temp
    auto soc = read_thermal_zone(1);
    snap.soc_temp_celsius = soc.value_or(snap.cpu_temp_celsius);

    // Current CPU freq
    auto cf = read_cpufreq(0);
    snap.cpu_freq_mhz = static_cast<uint32_t>(cf.value_or(0));

    // Max freq
    std::string max_str = read_file(
        "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    if (!max_str.empty()) {
        try {
            snap.cpu_freq_max_mhz =
                static_cast<uint32_t>(std::stoull(max_str) / 1000);
        } catch (...) {}
    }

    // Throttle detection: current < 90% of max
    snap.throttled = (snap.cpu_freq_max_mhz > 0) &&
                     (snap.cpu_freq_mhz <
                      static_cast<uint32_t>(snap.cpu_freq_max_mhz * 0.90f));
    return snap;
}

// ─── Mem info ─────────────────────────────────────────────────────────────────
MemInfo SystemMonitor::read_meminfo() const {
    MemInfo m{};
    std::ifstream f("/proc/meminfo");
    if (!f.is_open()) return m;
    std::string line;
    while (std::getline(f, line)) {
        uint64_t val = 0;
        if (sscanf(line.c_str(), "MemTotal: %lu kB", &val) == 1)
            m.total_kb = val;
        else if (sscanf(line.c_str(), "MemFree: %lu kB", &val) == 1)
            m.free_kb = val;
        else if (sscanf(line.c_str(), "MemAvailable: %lu kB", &val) == 1)
            m.available_kb = val;
        else if (sscanf(line.c_str(), "Cached: %lu kB", &val) == 1)
            m.cached_kb = val;
    }
    return m;
}

// ─── Per-core stats ───────────────────────────────────────────────────────────
std::vector<CoreStats> SystemMonitor::read_cores() const {
    std::vector<CoreStats> cores;
    uint32_t n = get_nprocs();
    for (uint32_t i = 0; i < n; ++i) {
        CoreStats cs{};
        cs.core_id = i;
        auto f = read_cpufreq(static_cast<int>(i));
        cs.freq_mhz = f.value_or(0);
        cs.usage_pct = 0.0f;  // Instantaneous usage requires two samples
        cores.push_back(cs);
    }
    return cores;
}

// ─── Thermal monitoring loop ──────────────────────────────────────────────────
void SystemMonitor::start_thermal_monitor(int interval_ms,
                                           ThermalCallback cb) {
    monitoring_active_ = true;
    std::thread([this, interval_ms, cb]() {
        while (monitoring_active_) {
            cb(read_thermal());
            std::this_thread::sleep_for(
                std::chrono::milliseconds(interval_ms));
        }
    }).detach();
}

void SystemMonitor::stop_thermal_monitor() {
    monitoring_active_ = false;
}

} // namespace openrv
