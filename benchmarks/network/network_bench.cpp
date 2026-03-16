// ─── OpenRVBench :: Network Benchmark ────────────────────────────────────────
// Tests: loopback TCP throughput, loopback UDP latency (ping-pong),
//        and optionally an external iperf3 wrapper.
//
// Output: JSON BenchResult to stdout
// ─────────────────────────────────────────────────────────────────────────────
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>

#include "../../results/result_writer.h"

using namespace openrv;
using Clock = std::chrono::high_resolution_clock;
using Seconds = std::chrono::duration<double>;

static double elapsed(Clock::time_point t0) {
    return Seconds(Clock::now() - t0).count();
}

// ─── Port helpers ──────────────────────────────────────────────────────────── 
static const uint16_t TCP_PORT = 19876;
static const uint16_t UDP_PORT = 19877;
static const size_t   TCP_BUF  = 128 * 1024;       // 128 KB send buffer
static const size_t   TOTAL_TX = 512ULL * 1024 * 1024;  // 512 MB transfer

// ─────────────────────────────────────────────────────────────────────────────
// TCP THROUGHPUT  (loopback: server thread + client thread)
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<double> tcp_server_bytes{0.0};
static std::atomic<bool>   tcp_done{false};

static void tcp_server_thread() {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return;

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(TCP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) { close(srv); return; }
    listen(srv, 1);

    int cli = accept(srv, nullptr, nullptr);
    close(srv);
    if (cli < 0) return;

    // Set receive buffer large
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(cli, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    std::vector<char> buf(TCP_BUF);
    uint64_t total = 0;
    ssize_t n;
    while ((n = recv(cli, buf.data(), buf.size(), 0)) > 0)
        total += static_cast<uint64_t>(n);
    tcp_server_bytes.store(static_cast<double>(total));
    close(cli);
}

static double bench_tcp_throughput() {
    tcp_done    = false;
    tcp_server_bytes = 0.0;

    std::thread server(tcp_server_thread);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let server bind

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { server.join(); return 0.0; }

    int sndbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    // Disable Nagle for throughput test
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(TCP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd); server.join(); return 0.0;
    }

    std::vector<char> buf(TCP_BUF, 0xAA);
    uint64_t sent = 0;
    auto t0 = Clock::now();

    while (sent < TOTAL_TX) {
        size_t to_send = std::min(TCP_BUF, TOTAL_TX - sent);
        ssize_t w = send(fd, buf.data(), to_send, 0);
        if (w <= 0) break;
        sent += static_cast<uint64_t>(w);
    }
    close(fd);
    double secs = elapsed(t0);
    server.join();

    return static_cast<double>(sent) / secs / (1024*1024);  // MB/s
}

// ─────────────────────────────────────────────────────────────────────────────
// UDP LATENCY  (ping-pong on loopback)
// ─────────────────────────────────────────────────────────────────────────────
static void udp_echo_server(int pings) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(UDP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (sockaddr*)&addr, sizeof(addr));

    char buf[64];
    sockaddr_in client{};
    socklen_t clen = sizeof(client);

    for (int i = 0; i < pings; ++i) {
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                             (sockaddr*)&client, &clen);
        if (n > 0)
            sendto(fd, buf, static_cast<size_t>(n), 0,
                   (sockaddr*)&client, clen);
    }
    close(fd);
}

static double bench_udp_latency(int pings = 5000) {
    std::thread server(udp_echo_server, pings);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { server.join(); return 0.0; }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(UDP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    char buf[8] = "PING";

    std::vector<double> rtts;
    rtts.reserve(pings);

    for (int i = 0; i < pings; ++i) {
        auto t0 = Clock::now();
        sendto(fd, buf, 4, 0, (sockaddr*)&addr, sizeof(addr));
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0)
            rtts.push_back(elapsed(t0) * 1e6);  // µs
    }
    close(fd);
    server.join();

    if (rtts.empty()) return 0.0;

    double sum = 0.0;
    for (double v : rtts) sum += v;
    return sum / rtts.size();  // avg RTT in µs
}

// ─────────────────────────────────────────────────────────────────────────────
// MAIN
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    // Ignore SIGPIPE (broken pipe during TCP test)
    signal(SIGPIPE, SIG_IGN);

    BenchResult result;
    result.bench_id   = "network";
    result.bench_name = "Network Benchmark";
    result.passed     = true;

    auto t_global = Clock::now();

    // TCP throughput
    double tcp_mbs = bench_tcp_throughput();
    result.metrics.push_back({"tcp_loopback_mbs", tcp_mbs, "MB/s",
                               "TCP loopback throughput (512 MB transfer)"});

    // UDP latency
    double udp_rtt_us = bench_udp_latency(5000);
    result.metrics.push_back({"udp_loopback_lat_us", udp_rtt_us, "µs",
                               "UDP loopback round-trip latency (5000 pings)"});

    // Derived: packets per second
    double udp_pps = (udp_rtt_us > 0) ? 1e6 / udp_rtt_us : 0.0;
    result.metrics.push_back({"udp_loopback_pps", udp_pps, "pps",
                               "Estimated UDP packets per second"});

    result.score      = tcp_mbs;
    result.score_unit = "MB/s";
    result.duration_sec = elapsed(t_global);

    print_result_json(result);
    return 0;
}
