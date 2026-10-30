// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: memory classes (plan step 6). See mem_classes.h. The allocator probe calls OnAlloc/OnFree;
// the draw thread calls Snapshot + NoteShadow/NoteShadowPool in shadow mode. Everything is gated on
// Mode() != 0, so with SOTC_CLASS_TRACK unset this does nothing.

#include "mem_classes.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace SotcMemClasses {
namespace {

constexpr size_t kNameMax = 48;

struct Entry {
    uint64_t size;
    uint32_t pool;
};

std::mutex g_mutex;                    // guards the range table and the pool names
std::map<uint64_t, Entry> g_cpu_only;  // base -> live CPU-only allocation
std::atomic<uint64_t> g_version{0};    // bumps on every change to the table
std::array<std::array<char, kNameMax>, kMaxPools> g_pool_names{};
uint32_t g_pool_count = 0;

// Shadow window: the last totals the draw thread reported, plus window peaks.
std::atomic<uint64_t> g_shadow_ranges{0};
std::atomic<uint64_t> g_shadow_bytes{0};
std::atomic<uint64_t> g_shadow_gpu_last{0};
std::atomic<uint64_t> g_shadow_gpu_peak{0};
std::atomic<uint64_t> g_shadow_samples{0};
std::array<std::atomic<uint64_t>, kMaxPools> g_pool_peak{}; // window peak per pool
std::array<uint64_t, kMaxPools> g_pool_session_peak{};      // session peak per pool (Report only)

void RaisePeak(std::atomic<uint64_t>& peak, uint64_t value) {
    uint64_t cur = peak.load(std::memory_order_relaxed);
    while (value > cur && !peak.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
    }
}

// Index of a pool name, added on first use. Caller holds g_mutex.
uint32_t PoolIndex(const char* name) {
    const char* key = (name && name[0]) ? name : "?";
    for (uint32_t i = 0; i < g_pool_count; ++i) {
        if (std::strncmp(g_pool_names[i].data(), key, kNameMax) == 0) {
            return i;
        }
    }
    if (g_pool_count == kMaxPools) {
        return kMaxPools - 1; // full: the last slot collects the rest
    }
    std::snprintf(g_pool_names[g_pool_count].data(), kNameMax, "%s", key);
    return g_pool_count++;
}

} // namespace

int Mode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_CLASS_TRACK");
        if (!v || !v[0]) {
            return 0;
        }
        const int n = std::atoi(v);
        return (n == 1 || n == 2) ? n : 0;
    }();
    return mode;
}

void OnAlloc(uint64_t base, uint64_t size, bool cpu_only, const char* pool) {
    if (Mode() == 0 || !cpu_only || base == 0 || size == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cpu_only.insert_or_assign(base, Entry{size, PoolIndex(pool)});
    g_version.fetch_add(1, std::memory_order_release);
}

void OnFree(uint64_t base) {
    if (Mode() == 0 || base == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cpu_only.erase(base) != 0) {
        g_version.fetch_add(1, std::memory_order_release);
    }
}

std::vector<Range> Snapshot(uint64_t& version) {
    std::lock_guard<std::mutex> lock(g_mutex);
    version = g_version.load(std::memory_order_relaxed);
    std::vector<Range> out;
    out.reserve(g_cpu_only.size());
    for (const auto& [base, e] : g_cpu_only) {
        out.push_back(Range{base, e.size, e.pool});
    }
    return out;
}

void NoteShadow(uint64_t ranges, uint64_t cpu_only_bytes, uint64_t gpu_pages) {
    g_shadow_ranges.store(ranges, std::memory_order_relaxed);
    g_shadow_bytes.store(cpu_only_bytes, std::memory_order_relaxed);
    g_shadow_gpu_last.store(gpu_pages, std::memory_order_relaxed);
    g_shadow_samples.fetch_add(1, std::memory_order_relaxed);
    RaisePeak(g_shadow_gpu_peak, gpu_pages);
}

void NoteShadowPool(uint32_t pool, uint64_t gpu_pages) {
    if (pool < kMaxPools) {
        RaisePeak(g_pool_peak[pool], gpu_pages);
    }
}

void Report(std::string& out, double /*seconds*/) {
    const int mode = Mode();
    if (mode == 0) {
        return;
    }
    if (mode == 1) {
        static bool said = false;
        if (!said) {
            said = true;
            out += "SOTC CLASS: mode 1 (apply) is not wired yet; run shadow mode "
                   "(SOTC_CLASS_TRACK=2) first to validate the classification\n";
        }
        return;
    }
    // Shadow mode.
    const uint64_t samples = g_shadow_samples.exchange(0, std::memory_order_relaxed);
    if (samples == 0) {
        return; // the draw thread took no sample this window
    }
    const uint64_t ranges = g_shadow_ranges.load(std::memory_order_relaxed);
    const uint64_t bytes = g_shadow_bytes.load(std::memory_order_relaxed);
    const uint64_t last = g_shadow_gpu_last.load(std::memory_order_relaxed);
    const uint64_t peak = g_shadow_gpu_peak.exchange(0, std::memory_order_relaxed);
    char line[256];
    std::snprintf(line, sizeof line,
                  "SOTC CLASS shadow: %llu CPU-only ranges (%.1f MiB), GPU pages inside them: "
                  "%llu now, %llu peak (want 0), %llu samples\n",
                  static_cast<unsigned long long>(ranges), bytes / 1048576.0,
                  static_cast<unsigned long long>(last), static_cast<unsigned long long>(peak),
                  static_cast<unsigned long long>(samples));
    out += line;

    // Per pool: live CPU-only ranges and MiB, window peak and session peak of GPU pages.
    std::array<uint64_t, kMaxPools> count{}, pool_bytes{};
    uint32_t pools = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& [base, e] : g_cpu_only) {
            count[e.pool] += 1;
            pool_bytes[e.pool] += e.size;
        }
        pools = g_pool_count;
        std::string text = "SOTC CLASS pools:";
        bool any = false;
        for (uint32_t i = 0; i < pools; ++i) {
            const uint64_t w = g_pool_peak[i].exchange(0, std::memory_order_relaxed);
            g_pool_session_peak[i] = std::max(g_pool_session_peak[i], w);
            if (count[i] == 0 && g_pool_session_peak[i] == 0) {
                continue;
            }
            char item[160];
            std::snprintf(item, sizeof item, "%s %s %llu (%.1f MiB) gpu %llu/%llu", any ? ";" : "",
                          g_pool_names[i].data(), static_cast<unsigned long long>(count[i]),
                          pool_bytes[i] / 1048576.0, static_cast<unsigned long long>(w),
                          static_cast<unsigned long long>(g_pool_session_peak[i]));
            text += item;
            any = true;
        }
        if (any) {
            out += text;
            out += " (gpu = window peak / session peak; want 0)\n";
        }
    }
}

} // namespace SotcMemClasses
