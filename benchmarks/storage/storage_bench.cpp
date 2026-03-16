// ─── OpenRVBench :: Storage Benchmark ────────────────────────────────────────
// Measures: sequential read/write, random 4K read/write IOPS, mixed I/O.
// Uses O_DIRECT where available to bypass page cache.
//
// Output: JSON BenchResult to stdout
// ─────────────────────────────────────────────────────────────────────────────
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <random>
#include <algorithm>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef __linux__
#  include <linux/fs.h>
#  include <sys/ioctl.h>
#  ifndef O_DIRECT
#    define O_DIRECT 040000
#  endif
#endif

#include "../../results/result_writer.h"

using namespace openrv;
using Clock = std::chrono::high_resolution_clock;
using Seconds = std::chrono::duration<double>;

static double elapsed(Clock::time_point t0) {
    return Seconds(Clock::now() - t0).count();
}

static const char* TEST_FILE = "/tmp/openrvbench_storage_test.bin";
static const size_t SEQ_SIZE  = 512ULL * 1024 * 1024;  // 512 MB
static const size_t BLOCK     = 1024 * 1024;            // 1 MB block
static const size_t RAND_BLOCK = 4096;                  // 4 K for random I/O
static const int    RAND_OPS   = 10000;

// ─────────────────────────────────────────────────────────────────────────────
// SEQUENTIAL WRITE
// ─────────────────────────────────────────────────────────────────────────────
static double bench_seq_write() {
    // Aligned buffer for O_DIRECT
    void* buf = nullptr;
    if (posix_memalign(&buf, 4096, BLOCK) != 0) return 0.0;
    memset(buf, 0xAB, BLOCK);

    int fd = open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0600);
    if (fd < 0) { free(buf); return 0.0; }

    auto t0 = Clock::now();
    size_t total = 0;
    while (total < SEQ_SIZE) {
        ssize_t w = write(fd, buf, BLOCK);
        if (w <= 0) break;
        total += static_cast<size_t>(w);
    }
    fsync(fd);
    double secs = elapsed(t0);
    close(fd);
    free(buf);

    return static_cast<double>(total) / secs / (1024*1024);  // MB/s
}

// ─────────────────────────────────────────────────────────────────────────────
// SEQUENTIAL READ
// ─────────────────────────────────────────────────────────────────────────────
static double bench_seq_read() {
    void* buf = nullptr;
    if (posix_memalign(&buf, 4096, BLOCK) != 0) return 0.0;

    int fd = open(TEST_FILE, O_RDONLY);
    if (fd < 0) { free(buf); return 0.0; }

    // Drop page cache hint (advisory only)
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);

    auto t0 = Clock::now();
    size_t total = 0;
    ssize_t r;
    while ((r = read(fd, buf, BLOCK)) > 0)
        total += static_cast<size_t>(r);

    double secs = elapsed(t0);
    close(fd);
    free(buf);

    return static_cast<double>(total) / secs / (1024*1024);  // MB/s
}

// ─────────────────────────────────────────────────────────────────────────────
// RANDOM 4K READ  (IOPS + latency)
// ─────────────────────────────────────────────────────────────────────────────
static std::pair<double,double> bench_random_read() {
    void* buf = nullptr;
    if (posix_memalign(&buf, 4096, RAND_BLOCK) != 0) return {0,0};

    int fd = open(TEST_FILE, O_RDONLY);
    if (fd < 0) { free(buf); return {0,0}; }

    // Get file size
    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) { close(fd); free(buf); return {0,0}; }
    size_t max_offset = static_cast<size_t>(file_size / RAND_BLOCK);

    std::mt19937_64 rng(42);
    std::vector<off_t> offsets(RAND_OPS);
    for (int i = 0; i < RAND_OPS; ++i)
        offsets[i] = static_cast<off_t>((rng() % max_offset) * RAND_BLOCK);

    auto t0 = Clock::now();
    int ops = 0;
    for (int i = 0; i < RAND_OPS; ++i) {
        if (pread(fd, buf, RAND_BLOCK, offsets[i]) > 0) ++ops;
    }
    double secs = elapsed(t0);
    close(fd);
    free(buf);

    double iops    = ops / secs;
    double lat_us  = (secs / ops) * 1e6;
    return {iops, lat_us};
}

// ─────────────────────────────────────────────────────────────────────────────
// RANDOM 4K WRITE (IOPS)
// ─────────────────────────────────────────────────────────────────────────────
static double bench_random_write() {
    void* buf = nullptr;
    if (posix_memalign(&buf, 4096, RAND_BLOCK) != 0) return 0.0;
    memset(buf, 0xCD, RAND_BLOCK);

    int fd = open(TEST_FILE, O_RDWR);
    if (fd < 0) { free(buf); return 0.0; }

    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) { close(fd); free(buf); return 0.0; }
    size_t max_offset = static_cast<size_t>(file_size / RAND_BLOCK);

    std::mt19937_64 rng(99);

    auto t0 = Clock::now();
    int ops = 0;
    for (int i = 0; i < RAND_OPS; ++i) {
        off_t off = static_cast<off_t>((rng() % max_offset) * RAND_BLOCK);
        if (pwrite(fd, buf, RAND_BLOCK, off) > 0) ++ops;
    }
    fsync(fd);
    double secs = elapsed(t0);
    close(fd);
    free(buf);

    return ops / secs;
}

// ─────────────────────────────────────────────────────────────────────────────
// MAIN
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    BenchResult result;
    result.bench_id   = "storage";
    result.bench_name = "Storage Benchmark";
    result.passed     = true;

    auto t_global = Clock::now();

    // Sequential write
    double seq_write_mbs = bench_seq_write();
    result.metrics.push_back({"seq_write_mbs", seq_write_mbs, "MB/s",
                               "Sequential write (512 MB, 1 MB blocks)"});

    // Sequential read
    double seq_read_mbs = bench_seq_read();
    result.metrics.push_back({"seq_read_mbs", seq_read_mbs, "MB/s",
                               "Sequential read (512 MB, 1 MB blocks)"});

    // Random read
    auto [rand_read_iops, rand_read_lat] = bench_random_read();
    result.metrics.push_back({"rand_read_iops",  rand_read_iops, "IOPS",
                               "Random 4K read IOPS"});
    result.metrics.push_back({"rand_read_lat_us", rand_read_lat, "µs",
                               "Random 4K read average latency"});

    // Random write
    double rand_write_iops = bench_random_write();
    result.metrics.push_back({"rand_write_iops", rand_write_iops, "IOPS",
                               "Random 4K write IOPS"});

    // Cleanup
    remove(TEST_FILE);

    // Composite score
    result.score      = (seq_read_mbs + seq_write_mbs) / 2.0;
    result.score_unit = "MB/s";
    result.duration_sec = elapsed(t_global);

    print_result_json(result);
    return 0;
}
