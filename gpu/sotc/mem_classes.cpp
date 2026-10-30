// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: memory classes (plan step 6). See mem_classes.h. The allocator probe calls OnAlloc/OnFree;
// the draw thread calls Snapshot/ApplyRanges, RefreshLookup and NoteShadow* (shadow and apply modes).
// The guest write fault path calls NoteWriteFault, which takes no lock. Everything is gated on
// Mode() != 0, so with SOTC_CLASS_TRACK unset this does nothing.

#include "mem_classes.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace SotcMemClasses {
namespace {

constexpr size_t kNameMax = 48;

// Range bounds, the same as SotcUntrack::Parse (SOTC_UNTRACK_RANGES): size 1 B to 4 GiB, base below
// 2^48. A range outside them is skipped and counted (g_skipped_ranges).
constexpr uint64_t kMaxRangeSize = 1ULL << 32;
constexpr uint64_t kMaxRangeBase = 1ULL << 48;

// RefreshLookup throttle: a rebuild needs at least kRebuildSyncs syncs AND kRebuildSpacing since the
// last one. Until then the fault lookup is older than the table (see mem_classes.h).
constexpr uint32_t kRebuildSyncs = 16;
constexpr auto kRebuildSpacing = std::chrono::milliseconds(100);

struct Entry {
    uint64_t size;
    uint32_t pool;
};

std::mutex g_mutex;                    // guards the range table, the pool names and the pool flags
std::map<uint64_t, Entry> g_cpu_only;  // base -> live CPU-only allocation
std::atomic<uint64_t> g_version{0};    // bumps on every change to the table
std::atomic<bool> g_lookup_dirty{false};
std::array<std::array<char, kNameMax>, kMaxPools> g_pool_names{};
std::array<bool, kMaxPools> g_pool_apply{}; // under g_mutex: the pool is listed in SOTC_CLASS_POOLS
uint32_t g_pool_count = 0;

// Lock-free lookup for the fault path. Two slots: the writer (under g_mutex) fills the inactive
// slot once its reader count is zero, then makes it the active one. A reader counts itself on the
// slot it reads and re-checks that the slot is still active; a retry is rare and cheap. All
// accesses are seq_cst, which orders the reader count against the writer's wait.
struct LookupEntry {
    uint64_t addr;
    uint64_t end;
    uint32_t pool;
};
struct LookupTable {
    std::vector<LookupEntry> items;
};
std::array<LookupTable, 2> g_lookup{};
std::atomic<uint32_t> g_lookup_active{0};
std::array<std::atomic<uint32_t>, 2> g_lookup_readers{};
std::array<std::atomic<uint64_t>, kMaxPools> g_pool_faults{}; // write faults per pool, this window

// Lookup rebuild state. g_rebuild_mutex serializes RefreshLookup and is never held while g_mutex is
// taken inside it, except for the short copy; the slot write and the reader wait run without g_mutex.
std::mutex g_rebuild_mutex;
std::atomic<uint32_t> g_sync_calls{0};       // RefreshLookup calls (every sync)
uint32_t g_last_rebuild_sync = 0;            // under g_rebuild_mutex
std::chrono::steady_clock::time_point g_last_rebuild{}; // under g_rebuild_mutex
std::vector<LookupEntry> g_staging;          // under g_rebuild_mutex: the copied table, capacity kept
std::atomic<uint64_t> g_lookup_rebuilds{0};  // this window
std::atomic<uint64_t> g_skipped_ranges{0};   // this window: OnAlloc ranges outside the bounds

// Mode 1 state, set by the draw thread and read by Report.
std::atomic<uint64_t> g_apply_ranges{0};
std::atomic<uint64_t> g_apply_bytes{0};
std::atomic<uint64_t> g_apply_publishes{0}; // this window
std::atomic<bool> g_apply_tripped{false};
std::atomic<uint64_t> g_apply_faults_untracked{0};

// Shadow window: the last totals the draw thread reported, plus window peaks.
std::atomic<uint64_t> g_shadow_ranges{0};
std::atomic<uint64_t> g_shadow_bytes{0};
std::atomic<uint64_t> g_shadow_gpu_last{0};
std::atomic<uint64_t> g_shadow_gpu_peak{0};
std::atomic<uint64_t> g_shadow_samples{0};
std::array<std::atomic<uint64_t>, kMaxPools> g_pool_peak{}; // window peak per pool
// Ranges of a pool that overlap a texture-cache image (last count). Images do not set region GPU
// bits, so these are counted apart and never untracked in mode 1.
std::array<std::atomic<uint64_t>, kMaxPools> g_pool_image_ranges{};
std::array<uint64_t, kMaxPools> g_pool_session_peak{};      // session peak per pool (Report only)

void RaisePeak(std::atomic<uint64_t>& peak, uint64_t value) {
    uint64_t cur = peak.load(std::memory_order_relaxed);
    while (value > cur && !peak.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
    }
}

// SOTC_CLASS_POOLS, read once: the comma-separated pool names for mode 1 (default PageAllocator).
const std::vector<std::string>& ApplyPoolNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        const char* env = std::getenv("SOTC_CLASS_POOLS");
        const std::string text = (env && env[0]) ? env : "PageAllocator";
        size_t begin = 0;
        while (begin <= text.size()) {
            size_t end = text.find(',', begin);
            if (end == std::string::npos) {
                end = text.size();
            }
            const std::string item = text.substr(begin, end - begin);
            const size_t first = item.find_first_not_of(' ');
            const size_t last = item.find_last_not_of(' ');
            if (first != std::string::npos) {
                out.push_back(item.substr(first, last - first + 1));
            }
            begin = end + 1;
        }
        return out;
    }();
    return names;
}

bool IsApplyPool(const char* key) {
    for (const std::string& name : ApplyPoolNames()) {
        if (name == key) {
            return true;
        }
    }
    return false;
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
    g_pool_apply[g_pool_count] = IsApplyPool(key);
    return g_pool_count++;
}

// Copies `fresh` into the inactive lookup slot and makes it active. Caller holds g_rebuild_mutex,
// not g_mutex. Waits for the readers of that slot (they only binary-search it) to leave.
void PublishLookup(const std::vector<LookupEntry>& fresh) {
    const uint32_t next = g_lookup_active.load(std::memory_order_seq_cst) ^ 1u;
    while (g_lookup_readers[next].load(std::memory_order_seq_cst) != 0) {
        std::this_thread::yield(); // readers only binary-search here, so this ends at once
    }
    auto& items = g_lookup[next].items;
    items.assign(fresh.begin(), fresh.end()); // no allocation once the capacity is there
    g_lookup_active.store(next, std::memory_order_seq_cst);
}

// Per-pool lines of the report: live CPU-only ranges and MiB, the write faults of the window, and
// in shadow mode the GPU-page peaks (window and session). Appends one line when any pool shows.
void AppendPools(std::string& out, double secs, const std::array<uint64_t, kMaxPools>& faults,
                 bool shadow) {
    std::array<uint64_t, kMaxPools> count{}, pool_bytes{};
    std::string text = "SOTC CLASS pools:"; // fault counts come from the lookup, which may lag a sync
    bool any = false;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& [base, e] : g_cpu_only) {
        count[e.pool] += 1;
        pool_bytes[e.pool] += e.size;
    }
    for (uint32_t i = 0; i < g_pool_count; ++i) {
        uint64_t w = 0;
        if (shadow) {
            w = g_pool_peak[i].exchange(0, std::memory_order_relaxed);
            g_pool_session_peak[i] = std::max(g_pool_session_peak[i], w);
        }
        if (count[i] == 0 && g_pool_session_peak[i] == 0 && faults[i] == 0) {
            continue;
        }
        char item[200];
        int len = std::snprintf(item, sizeof item, "%s %s %llu (%.1f MiB)", any ? ";" : "",
                                g_pool_names[i].data(), static_cast<unsigned long long>(count[i]),
                                pool_bytes[i] / 1048576.0);
        if (shadow) {
            len += std::snprintf(item + len, sizeof item - len, " gpu %llu/%llu",
                                 static_cast<unsigned long long>(w),
                                 static_cast<unsigned long long>(g_pool_session_peak[i]));
        }
        len += std::snprintf(item + len, sizeof item - len, " faults %llu/s",
                             static_cast<unsigned long long>(faults[i] / secs + 0.5));
        if (const uint64_t img = g_pool_image_ranges[i].load(std::memory_order_relaxed)) {
            std::snprintf(item + len, sizeof item - len, " image-ranges %llu",
                          static_cast<unsigned long long>(img));
        }
        text += item;
        any = true;
    }
    if (any) {
        out += text;
        out += shadow ? " (gpu = window peak / session peak; want 0)\n" : "\n";
    }
}

} // namespace

int Mode() {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_CLASS_TRACK");
        if (!v || !v[0]) {
            return 0;
        }
        const int n = std::atoi(v);
        if (n == 1) {
            // Mode 1 publishes through SotcUntrack's auto list, so it cannot share the switch with
            // SOTC_UNTRACK_AUTO, and the CP ahead gate turns the untracking off (as Parse()).
            const char* untrack_auto = std::getenv("SOTC_UNTRACK_AUTO");
            const char* ahead = std::getenv("SOTC_CP_AHEAD");
            if (untrack_auto && untrack_auto[0] == '1') {
                std::fprintf(stderr, "SOTC CLASS: SOTC_CLASS_TRACK=1 refused, SOTC_UNTRACK_AUTO=1 "
                                     "is set; class mode stays off\n");
                return 0;
            }
            if (ahead && std::atoi(ahead) != 0) {
                std::fprintf(stderr, "SOTC CLASS: SOTC_CLASS_TRACK=1 refused, SOTC_CP_AHEAD is "
                                     "set; class mode stays off\n");
                return 0;
            }
            // Mode 1 untracks the CPU-only ranges only: the static SOTC_UNTRACK_RANGES list is not read.
            const char* static_ranges = std::getenv("SOTC_UNTRACK_RANGES");
            if (static_ranges && static_ranges[0]) {
                std::fprintf(stderr, "SOTC CLASS: SOTC_UNTRACK_RANGES is ignored in class mode "
                                     "(SOTC_CLASS_TRACK=1); the CPU-only ranges are the untrack list\n");
            }
        }
        return (n == 1 || n == 2) ? n : 0;
    }();
    return mode;
}

void OnAlloc(uint64_t base, uint64_t size, bool cpu_only, const char* pool) {
    if (Mode() == 0 || !cpu_only || base == 0) {
        return;
    }
    // Same bounds as SotcUntrack::Parse, and no wrap of base + size. Counted in the report.
    if (size == 0 || size > kMaxRangeSize || base >= kMaxRangeBase || base > UINT64_MAX - size) {
        g_skipped_ranges.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cpu_only.insert_or_assign(base, Entry{size, PoolIndex(pool)});
    g_version.fetch_add(1, std::memory_order_release);
    g_lookup_dirty.store(true, std::memory_order_release);
}

void OnFree(uint64_t base) {
    if (Mode() == 0 || base == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cpu_only.erase(base) != 0) {
        g_version.fetch_add(1, std::memory_order_release);
        g_lookup_dirty.store(true, std::memory_order_release);
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

std::vector<Range> ApplyRanges(uint64_t& version) {
    std::lock_guard<std::mutex> lock(g_mutex);
    version = g_version.load(std::memory_order_relaxed);
    std::vector<Range> out;
    for (const auto& [base, e] : g_cpu_only) {
        if (g_pool_apply[e.pool]) {
            out.push_back(Range{base, e.size, e.pool});
        }
    }
    return out;
}

uint64_t Version() {
    return g_version.load(std::memory_order_acquire);
}

void RefreshLookup() {
    g_sync_calls.fetch_add(1, std::memory_order_relaxed);
    if (!g_lookup_dirty.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> rebuild(g_rebuild_mutex);
    const uint32_t sync = g_sync_calls.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    if (sync - g_last_rebuild_sync < kRebuildSyncs || now - g_last_rebuild < kRebuildSpacing) {
        return; // throttled: the dirty flag stays set and the next sync tries again
    }
    {
        // Only the copy runs under g_mutex. The flag is cleared under it too, so an OnAlloc or
        // OnFree after this copy sets it again.
        std::lock_guard<std::mutex> lock(g_mutex);
        g_lookup_dirty.store(false, std::memory_order_release);
        g_staging.clear();
        g_staging.reserve(g_cpu_only.size());
        for (const auto& [base, e] : g_cpu_only) {
            g_staging.push_back(LookupEntry{base, base + e.size, e.pool});
        }
    }
    PublishLookup(g_staging);
    g_last_rebuild_sync = sync;
    g_last_rebuild = now;
    g_lookup_rebuilds.fetch_add(1, std::memory_order_relaxed);
}

void NoteWriteFault(uint64_t addr) {
    // Enter the active slot: count the reader, then check the slot did not change meanwhile.
    uint32_t slot = 0;
    for (;;) {
        slot = g_lookup_active.load(std::memory_order_seq_cst);
        g_lookup_readers[slot].fetch_add(1, std::memory_order_seq_cst);
        if (g_lookup_active.load(std::memory_order_seq_cst) == slot) {
            break;
        }
        g_lookup_readers[slot].fetch_sub(1, std::memory_order_seq_cst);
    }
    const auto& items = g_lookup[slot].items;
    const auto it = std::upper_bound(items.begin(), items.end(), addr,
                                     [](uint64_t a, const LookupEntry& e) { return a < e.addr; });
    if (it != items.begin()) {
        const LookupEntry& hit = *std::prev(it);
        if (addr < hit.end) {
            g_pool_faults[hit.pool].fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_lookup_readers[slot].fetch_sub(1, std::memory_order_seq_cst);
}

void NotePublish(uint64_t ranges, uint64_t bytes) {
    g_apply_ranges.store(ranges, std::memory_order_relaxed);
    g_apply_bytes.store(bytes, std::memory_order_relaxed);
    g_apply_publishes.fetch_add(1, std::memory_order_relaxed);
}

void NoteApplyState(bool tripped, uint64_t faults_untracked) {
    g_apply_tripped.store(tripped, std::memory_order_relaxed);
    g_apply_faults_untracked.store(faults_untracked, std::memory_order_relaxed);
}

void NoteShadow(uint64_t ranges, uint64_t cpu_only_bytes, uint64_t gpu_pages) {
    g_shadow_ranges.store(ranges, std::memory_order_relaxed);
    g_shadow_bytes.store(cpu_only_bytes, std::memory_order_relaxed);
    g_shadow_gpu_last.store(gpu_pages, std::memory_order_relaxed);
    g_shadow_samples.fetch_add(1, std::memory_order_relaxed);
    RaisePeak(g_shadow_gpu_peak, gpu_pages);
}

void NoteImageRanges(uint32_t pool, uint64_t ranges) {
    if (pool < kMaxPools) {
        g_pool_image_ranges[pool].store(ranges, std::memory_order_relaxed);
    }
}

void NoteShadowPool(uint32_t pool, uint64_t gpu_pages) {
    if (pool < kMaxPools) {
        RaisePeak(g_pool_peak[pool], gpu_pages);
    }
}

void Report(std::string& out, double seconds) {
    const int mode = Mode();
    if (mode == 0) {
        return;
    }
    const double secs = seconds > 0 ? seconds : 1.0;
    // The window's write faults, read and reset in both modes before any early return.
    std::array<uint64_t, kMaxPools> faults{};
    for (uint32_t i = 0; i < kMaxPools; ++i) {
        faults[i] = g_pool_faults[i].exchange(0, std::memory_order_relaxed);
    }
    // Table counters of the window, read and reset here. Shown only when something happened.
    const uint64_t rebuilds = g_lookup_rebuilds.exchange(0, std::memory_order_relaxed);
    const uint64_t skipped = g_skipped_ranges.exchange(0, std::memory_order_relaxed);
    if (rebuilds != 0 || skipped != 0) {
        char table[200];
        std::snprintf(table, sizeof table,
                      "SOTC CLASS table: %llu lookup rebuilds, %llu ranges skipped (size 0, size "
                      "> 4 GiB, base >= 2^48 or overflow)\n",
                      static_cast<unsigned long long>(rebuilds),
                      static_cast<unsigned long long>(skipped));
        out += table;
    }
    if (mode == 1) {
        const uint64_t publishes = g_apply_publishes.exchange(0, std::memory_order_relaxed);
        const bool tripped = g_apply_tripped.load(std::memory_order_relaxed);
        char line[320];
        std::snprintf(
            line, sizeof line,
            "SOTC CLASS apply: %llu ranges (%.1f MiB) untracked, %llu publishes this window, %s, "
            "faults untracked %llu\n",
            static_cast<unsigned long long>(g_apply_ranges.load(std::memory_order_relaxed)),
            g_apply_bytes.load(std::memory_order_relaxed) / 1048576.0,
            static_cast<unsigned long long>(publishes),
            tripped ? "TRIPPED (released, no more publishes)" : "not tripped",
            static_cast<unsigned long long>(g_apply_faults_untracked.load(std::memory_order_relaxed)));
        out += line;
        AppendPools(out, secs, faults, false);
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
    AppendPools(out, secs, faults, true);
}

} // namespace SotcMemClasses
