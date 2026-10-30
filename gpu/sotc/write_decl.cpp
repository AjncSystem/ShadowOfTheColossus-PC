// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_WRITE_DECL statistics (write_decl.h). Fixed-size tables with atomic slots, no lock:
// the caller table is open addressing keyed by the return address (1024 slots; a caller that does
// not fit is counted as overflow), the declared-page table is direct mapped by page (a collision
// can only over-count the verify signal).
#include "write_decl.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "hooks.h"
#include "video_core/sotc_stats.h"
#include "video_core/sotc_trace.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace SotcWriteDecl {
namespace {

// A byte count from the environment, as src/runtime.c reads it: what strtoull parses, or the
// fallback when no digit is there (a malformed value must not give a different limit in each).
uint64_t ByteEnv(const char* name, uint64_t fallback) {
    const char* v = std::getenv(name);
    if (!v) {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long long n = std::strtoull(v, &end, 10);
    return end == v ? fallback : static_cast<uint64_t>(n);
}

constexpr uint64_t kCallerSlots = 1024; // power of two
constexpr uint64_t kDeclaredSlots = 1ULL << 16;
constexpr uint32_t kRecentMs = 5; // a write fault this soon after a declare is counted as recent
constexpr uint64_t kHashMul = 0x9E3779B97F4A7C15ULL;

struct CallerSlot {
    std::atomic<uint64_t> key{0}; // return address (1 for a null one), 0 = free
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> pages{0};
    std::atomic<uint64_t> bytes{0};
};
std::array<CallerSlot, kCallerSlots> g_callers;
std::atomic<uint64_t> g_caller_overflow{0};
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_pages{0};
std::atomic<uint64_t> g_bytes{0};
std::atomic<uint64_t> g_recent{0};

// Write faults by the class of the faulting instruction (0b).
enum FaultClass : uint32_t { kEboot, kHeap, kHost, kOther, kFaultClasses };
std::array<std::atomic<uint64_t>, kFaultClasses> g_faults{};

// Declare time per page, ms + 1 (0 = never declared), by hashed page number.
std::array<std::atomic<uint32_t>, kDeclaredSlots> g_declared_ms{};
static_assert(kDeclaredSlots == 1ULL << 16); // the index below takes the top 16 bits

// The BPE heap allocator range (image offsets), once the hook "bpe_heap_alloc" resolves.
std::atomic<bool> g_heap_known{false};
std::atomic<uint64_t> g_heap_begin{0};
std::atomic<uint64_t> g_heap_end{0};

uint32_t NowMs() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

std::atomic<uint32_t>& DeclaredSlot(uint64_t page) {
    return g_declared_ms[(page * kHashMul) >> 48];
}

// The eboot image: its offset when addr lies in it.
bool InImage(uint64_t addr, uint64_t* offset) {
    const uint64_t base = SotcTrace::ImageBase();
    const uint64_t size = SotcTrace::ImageSize();
    if (!base || !size || addr < base || addr - base >= size) {
        return false;
    }
    *offset = addr - base;
    return true;
}

bool InHeapRange(uint64_t image_offset) {
    if (!g_heap_known.load(std::memory_order_acquire)) {
        // Resolved once the hooks are installed; until then the range is unknown.
        uint64_t begin = 0;
        uint64_t end = 0;
        if (!SotcHooks::ResolvedRange("bpe_heap_alloc", &begin, &end)) {
            return false;
        }
        g_heap_begin.store(begin, std::memory_order_relaxed);
        g_heap_end.store(end, std::memory_order_relaxed);
        g_heap_known.store(true, std::memory_order_release);
    }
    return image_offset >= g_heap_begin.load(std::memory_order_relaxed) &&
           image_offset < g_heap_end.load(std::memory_order_relaxed);
}

// Whether addr lies in a loaded host module. A small per-thread cache of the module ranges last
// found: a fault stays in the same few modules.
bool InHostModule(uint64_t addr) {
#ifdef _WIN32
    struct Range {
        uint64_t begin = 0;
        uint64_t end = 0;
    };
    thread_local std::array<Range, 4> cache{};
    thread_local uint32_t next = 0;
    for (const Range& range : cache) {
        if (range.begin && addr >= range.begin && addr < range.end) {
            return true;
        }
    }
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(addr), &module) ||
        !module) {
        return false;
    }
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    cache[next] = Range{uint64_t(base), uint64_t(base) + nt->OptionalHeader.SizeOfImage};
    next = (next + 1) % cache.size();
    return addr >= uint64_t(base) && addr < uint64_t(base) + nt->OptionalHeader.SizeOfImage;
#else
    (void)addr;
    return false;
#endif
}

// Counts the call under its return address (lock-free, open addressing).
void NoteCaller(uint64_t return_address, uint64_t size, uint64_t pages) {
    const uint64_t key = return_address ? return_address : 1;
    uint64_t index = (key * kHashMul) >> 54; // 10 bits: kCallerSlots = 1024
    for (uint64_t probe = 0; probe < kCallerSlots; ++probe) {
        CallerSlot& slot = g_callers[index];
        uint64_t current = slot.key.load(std::memory_order_acquire);
        if (current == 0) {
            uint64_t expected = 0;
            if (slot.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                current = key;
            } else {
                current = expected; // another thread took the slot: its key, maybe ours
            }
        }
        if (current == key) {
            slot.calls.fetch_add(1, std::memory_order_relaxed);
            slot.pages.fetch_add(pages, std::memory_order_relaxed);
            slot.bytes.fetch_add(size, std::memory_order_relaxed);
            return;
        }
        index = (index + 1) & (kCallerSlots - 1);
    }
    g_caller_overflow.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

uint32_t Mode() {
    static const uint32_t mode = [] {
        const char* v = std::getenv("SOTC_WRITE_DECL");
        const int m = v ? std::atoi(v) : 0;
        return m == 1 || m == 2 ? static_cast<uint32_t>(m) : 0u;
    }();
    return mode;
}

uint64_t MinBytes() {
    static const uint64_t min = ByteEnv("SOTC_WRITE_DECL_MIN", 256);
    return min;
}

uint64_t MaxBytes() {
    static const uint64_t max = ByteEnv("SOTC_WRITE_DECL_MAX", uint64_t{1} << 20);
    return max;
}

void NoteCall(uint64_t return_address, uint64_t size, uint64_t pages) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    g_pages.fetch_add(pages, std::memory_order_relaxed);
    g_bytes.fetch_add(size, std::memory_order_relaxed);
    NoteCaller(return_address, size, pages);
}

void NoteDeclared(uint64_t addr, uint64_t size) {
    if (size == 0) {
        return;
    }
    const uint32_t stamp = NowMs() + 1;
    const uint64_t first = addr >> 12;
    const uint64_t last = (addr + size - 1) >> 12;
    for (uint64_t page = first; page <= last; ++page) {
        DeclaredSlot(page).store(stamp, std::memory_order_relaxed);
    }
}

void NoteWriteFault(uint64_t rip, uint64_t addr) {
    const uint32_t stamp = DeclaredSlot(addr >> 12).load(std::memory_order_relaxed);
    if (stamp != 0 && NowMs() - (stamp - 1) <= kRecentMs) {
        g_recent.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t offset = 0;
    if (InImage(rip, &offset)) {
        g_faults[InHeapRange(offset) ? kHeap : kEboot].fetch_add(1, std::memory_order_relaxed);
    } else if (InHostModule(rip)) {
        g_faults[kHost].fetch_add(1, std::memory_order_relaxed);
    } else {
        g_faults[kOther].fetch_add(1, std::memory_order_relaxed);
    }
}

void Report(std::string& out, double seconds) {
    const double per_second = seconds > 0 ? 1.0 / seconds : 1.0;
    const uint32_t mode = Mode();
    char line[320];
    if (mode != 0) {
        struct Row {
            uint64_t return_address;
            uint64_t calls;
            uint64_t pages;
        };
        std::vector<Row> rows;
        for (CallerSlot& slot : g_callers) {
            const uint64_t key = slot.key.load(std::memory_order_acquire);
            if (key == 0) {
                continue;
            }
            const uint64_t calls = slot.calls.exchange(0, std::memory_order_relaxed);
            const uint64_t pages = slot.pages.exchange(0, std::memory_order_relaxed);
            slot.bytes.exchange(0, std::memory_order_relaxed);
            if (calls) {
                rows.push_back(Row{key == 1 ? 0 : key, calls, pages});
            }
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return a.pages != b.pages ? a.pages > b.pages : a.calls > b.calls;
        });
        const uint64_t calls = g_calls.exchange(0, std::memory_order_relaxed);
        const uint64_t pages = g_pages.exchange(0, std::memory_order_relaxed);
        const uint64_t bytes = g_bytes.exchange(0, std::memory_order_relaxed);
        const uint64_t recent = g_recent.exchange(0, std::memory_order_relaxed);
        const uint64_t overflow = g_caller_overflow.exchange(0, std::memory_order_relaxed);
        const char* const full = overflow ? " (caller table full)" : "";
        // Mode 1: the faults on settled pages. Mode 2: the faults a declare would remove.
        if (mode == 1) {
            std::snprintf(line, sizeof(line),
                          "SOTC WRITE DECL (mode 1): %.0f calls/s, %.0f pages/s, %.1f MB/s; "
                          "faults on declared pages (within %u ms; re-protects after a GPU sync "
                          "can still land here): %llu%s\n",
                          double(calls) * per_second, double(pages) * per_second,
                          double(bytes) * per_second / 1048576.0, kRecentMs,
                          static_cast<unsigned long long>(recent), full);
        } else {
            std::snprintf(line, sizeof(line),
                          "SOTC WRITE DECL (mode 2): %.0f calls/s, %.0f pages/s, %.1f MB/s; "
                          "faults a declare would remove (within %u ms): %llu%s\n",
                          double(calls) * per_second, double(pages) * per_second,
                          double(bytes) * per_second / 1048576.0, kRecentMs,
                          static_cast<unsigned long long>(recent), full);
        }
        out += line;
        for (size_t i = 0; i < rows.size() && i < 10; ++i) {
            uint64_t offset = 0;
            if (InImage(rows[i].return_address, &offset)) {
                std::snprintf(line, sizeof(line), "  eboot+0x%llx %.1f calls/s %.0f pages/s\n",
                              static_cast<unsigned long long>(offset),
                              double(rows[i].calls) * per_second,
                              double(rows[i].pages) * per_second);
            } else {
                std::snprintf(line, sizeof(line), "  0x%llx %.1f calls/s %.0f pages/s\n",
                              static_cast<unsigned long long>(rows[i].return_address),
                              double(rows[i].calls) * per_second,
                              double(rows[i].pages) * per_second);
            }
            out += line;
        }
    }
    if (SotcStats::kFpsLog) {
        std::array<uint64_t, kFaultClasses> faults{};
        for (uint32_t c = 0; c < kFaultClasses; ++c) {
            faults[c] = g_faults[c].exchange(0, std::memory_order_relaxed);
        }
        std::snprintf(line, sizeof(line),
                      "SOTC WRITE FAULTS: eboot %.0f/s, bpe heap %.0f/s, host module %.0f/s, "
                      "other %.0f/s\n",
                      double(faults[kEboot]) * per_second, double(faults[kHeap]) * per_second,
                      double(faults[kHost]) * per_second, double(faults[kOther]) * per_second);
        out += line;
    }
}

} // namespace SotcWriteDecl
