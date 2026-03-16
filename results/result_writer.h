#pragma once
// ─── OpenRVBench :: Result Writer ─────────────────────────────────────────────
// Each benchmark binary writes a JSON result fragment to stdout.
// The Python orchestrator collects and merges them.
// ─────────────────────────────────────────────────────────────────────────────
#include <string>
#include <vector>
#include <map>
#include <variant>
#include <iostream>
#include <sstream>
#include <iomanip>

namespace openrv {

using MetricValue = std::variant<double, int64_t, std::string, bool>;

struct Metric {
    std::string  name;
    MetricValue  value;
    std::string  unit;       // "MB/s", "ms", "tok/s", etc.
    std::string  description;
};

struct BenchResult {
    std::string              bench_id;    // "cpu", "memory", etc.
    std::string              bench_name;  // Human name
    double                   score;       // Normalised composite score (higher = better)
    std::string              score_unit;
    std::vector<Metric>      metrics;
    double                   duration_sec;
    bool                     passed;
    std::string              error_msg;
};

// ─── Simple JSON serialiser (no external deps) ────────────────────────────────
inline std::string json_escape(const std::string& s) {
    std::ostringstream o;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\n': o << "\\n";  break;
            case '\r': o << "\\r";  break;
            case '\t': o << "\\t";  break;
            default:
                if (c < 0x20) o << "\\u" << std::hex << std::setw(4)
                                << std::setfill('0') << (int)c;
                else          o << c;
        }
    }
    return o.str();
}

inline std::string metric_to_json(const Metric& m) {
    std::ostringstream o;
    o << "  {\n";
    o << "    \"name\": \""  << json_escape(m.name) << "\",\n";
    o << "    \"unit\": \""  << json_escape(m.unit) << "\",\n";
    o << "    \"desc\": \""  << json_escape(m.description) << "\",\n";
    o << "    \"value\": ";
    std::visit([&](auto&& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, double>)
            o << std::fixed << std::setprecision(3) << v;
        else if constexpr (std::is_same_v<T, int64_t>)
            o << v;
        else if constexpr (std::is_same_v<T, bool>)
            o << (v ? "true" : "false");
        else
            o << "\"" << json_escape(v) << "\"";
    }, m.value);
    o << "\n  }";
    return o.str();
}

inline void print_result_json(const BenchResult& r) {
    std::cout << "{\n";
    std::cout << "  \"bench_id\":   \"" << json_escape(r.bench_id)    << "\",\n";
    std::cout << "  \"bench_name\": \"" << json_escape(r.bench_name)  << "\",\n";
    std::cout << "  \"score\":       " << std::fixed << std::setprecision(2)
              << r.score << ",\n";
    std::cout << "  \"score_unit\": \"" << json_escape(r.score_unit)  << "\",\n";
    std::cout << "  \"duration_sec\": " << std::fixed << std::setprecision(3)
              << r.duration_sec << ",\n";
    std::cout << "  \"passed\":      " << (r.passed ? "true" : "false") << ",\n";
    std::cout << "  \"error\":       \"" << json_escape(r.error_msg) << "\",\n";
    std::cout << "  \"metrics\": [\n";
    for (size_t i = 0; i < r.metrics.size(); ++i) {
        std::cout << metric_to_json(r.metrics[i]);
        if (i + 1 < r.metrics.size()) std::cout << ",";
        std::cout << "\n";
    }
    std::cout << "  ]\n}\n";
}

} // namespace openrv
