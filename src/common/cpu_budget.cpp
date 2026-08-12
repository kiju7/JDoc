#include "cpu_budget.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace jdoc { namespace util {

namespace {

// Population count without an intrinsic (Kernighan): clears the lowest set bit
// each round, so it costs one iteration per allowed CPU.
template <typename T>
unsigned count_bits(T mask) {
    unsigned n = 0;
    for (; mask; mask &= mask - 1) n++;
    return n;
}

#if defined(__linux__)
// A CPU quota of "2.5 cores" can keep 3 runnable threads busy, so round up —
// but a share below one full core still gets a single worker, never zero.
unsigned quota_to_cpus(double quota, double period) {
    if (quota <= 0 || period <= 0) return 0;   // unlimited or unreadable
    double cpus = quota / period;
    if (cpus <= 1.0) return 1;
    if (cpus > 4096.0) return 0;               // implausible: ignore the value
    return static_cast<unsigned>(cpus + 0.999);
}

// cgroup v2 (unified): "<quota> <period>", or "max <period>" when unlimited.
unsigned cgroup_v2_cpus() {
    FILE* f = std::fopen("/sys/fs/cgroup/cpu.max", "r");
    if (!f) return 0;
    char quota[32] = {};
    long long period = 0;
    int fields = std::fscanf(f, "%31s %lld", quota, &period);
    std::fclose(f);
    if (fields != 2 || std::strcmp(quota, "max") == 0) return 0;
    return quota_to_cpus(std::atof(quota), static_cast<double>(period));
}

// cgroup v1: quota and period in separate files; quota -1 means unlimited.
unsigned cgroup_v1_cpus() {
    auto read_ll = [](const char* path, long long& out) {
        FILE* f = std::fopen(path, "r");
        if (!f) return false;
        bool ok = std::fscanf(f, "%lld", &out) == 1;
        std::fclose(f);
        return ok;
    };
    long long quota = 0, period = 0;
    if (!read_ll("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", quota)) return 0;
    if (!read_ll("/sys/fs/cgroup/cpu/cpu.cfs_period_us", period)) return 0;
    if (quota < 0) return 0; // unlimited
    return quota_to_cpus(static_cast<double>(quota), static_cast<double>(period));
}
#endif

unsigned compute_available_cpus() {
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) n = 1; // the standard permits 0 when the count is unknown

#if defined(_WIN32)
    DWORD_PTR process_mask = 0, system_mask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
        unsigned allowed = count_bits(process_mask);
        if (allowed > 0) n = std::min(n, allowed);
    }
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        int allowed = CPU_COUNT(&set);
        if (allowed > 0) n = std::min(n, static_cast<unsigned>(allowed));
    }
    unsigned quota = cgroup_v2_cpus();
    if (quota == 0) quota = cgroup_v1_cpus();
    if (quota > 0) n = std::min(n, quota);
#endif
    // macOS exposes no affinity API and no cgroups; hardware_concurrency is
    // authoritative there. Docker Desktop runs containers inside a Linux VM,
    // so containerized macOS hosts take the Linux path above.

    return std::max(1u, n);
}

} // namespace

unsigned available_cpus() {
    // The budget cannot change meaningfully mid-process, and reading it walks
    // /sys files — resolve once. Function-local statics are thread-safe.
    static const unsigned cached = compute_available_cpus();
    return cached;
}

}} // namespace jdoc::util
