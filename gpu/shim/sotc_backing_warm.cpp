// sotc: SOTC_BACKING_WARM, see sotc_backing_warm.h.
#include "sotc_backing_warm.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "common/thread.h"
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" {
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
}

namespace SotcBackingWarm {
namespace {

const bool kEnabled = [] {
    const char* v = std::getenv("SOTC_BACKING_WARM");
    return v && v[0] == '1';
}();

constexpr std::uint64_t kPage = 4096;
constexpr std::uint64_t kSlots = 4096; // power of two
constexpr std::uint64_t kMask = kSlots - 1;
// A queued range is clamped to this: a fault's extent is 64-128 KiB.
constexpr std::uint64_t kMaxRange = 1 << 20;

// Bounded multi-producer queue (Vyukov): each slot's sequence says whose turn it is. Producers
// are guest threads inside the fault handler; the warm thread is the single consumer.
struct Slot {
    std::atomic<std::uint64_t> seq;
    std::uint64_t addr;
    std::uint64_t size;
};
Slot slots[kSlots];
alignas(64) std::atomic<std::uint64_t> enqueue_pos{0};
alignas(64) std::uint64_t dequeue_pos = 0; // warm thread only

std::atomic<std::uint64_t> noted{0}, dropped{0}, pages{0};
std::atomic<bool> started{false};

bool Pop(std::uint64_t& addr, std::uint64_t& size) {
    Slot& s = slots[dequeue_pos & kMask];
    if (s.seq.load(std::memory_order_acquire) != dequeue_pos + 1) {
        return false;
    }
    addr = s.addr;
    size = s.size;
    s.seq.store(dequeue_pos + kSlots, std::memory_order_release);
    ++dequeue_pos;
    return true;
}

// Reads one byte per page of the backing view under [addr, addr + size). Unmapped and reserved
// runs are skipped; the backing view is mapped once for the whole section and never unmapped, so
// every backing address the runtime returns is readable (stale bytes do not matter here).
void Warm(std::uint64_t addr, std::uint64_t size) {
    std::uint64_t at = addr & ~(kPage - 1);
    const std::uint64_t end = addr + size;
    volatile unsigned char sink = 0;
    std::uint64_t touched = 0;
    while (at < end) {
        uintptr_t start = 0, stop = 0;
        int mapped = 0;
        if (!runtime_memory_region(at, &start, &stop, &mapped) || stop <= at) {
            break;
        }
        const std::uint64_t run_end = stop < end ? stop : end;
        if (mapped) {
            if (const unsigned char* backing = runtime_memory_backing_pointer(at)) {
                for (std::uint64_t off = 0; at + off < run_end; off += kPage) {
                    sink = sink + backing[off];
                    ++touched;
                }
            }
        }
        at = (run_end + kPage - 1) & ~(kPage - 1);
    }
    pages.fetch_add(touched, std::memory_order_relaxed);
}

void Loop() {
    Common::SetCurrentThreadName("shadPS4:BackingWarm");
#ifdef _WIN32
    // Below normal, not idle: an area load keeps every core busy, and an idle-priority thread
    // would warm the pages after the upload already paid for them.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    void Report();
    auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        std::uint64_t addr, size;
        bool any = false;
        while (Pop(addr, size)) {
            Warm(addr, size);
            any = true;
        }
        if (!any) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Every 5 s: the process may end without running atexit handlers.
        if (const auto now = std::chrono::steady_clock::now(); now >= next_report) {
            next_report = now + std::chrono::seconds(5);
            Report();
        }
    }
}

void Report() {
    std::fprintf(stderr, "SOTC BACKING WARM: %llu ranges noted, %llu dropped, %llu pages warmed\n",
                 static_cast<unsigned long long>(noted.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(dropped.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(pages.load(std::memory_order_relaxed)));
}

} // namespace

bool Enabled() noexcept {
    return kEnabled;
}

void Start() {
    static std::atomic<bool> once{false};
    if (!kEnabled || once.exchange(true)) {
        return;
    }
    // The slots are ready before Note can see `started` (release/acquire pair).
    for (std::uint64_t i = 0; i < kSlots; ++i) {
        slots[i].seq.store(i, std::memory_order_relaxed);
    }
    started.store(true, std::memory_order_release);
    std::thread(Loop).detach();
    std::fputs("SOTC BACKING WARM: on\n", stderr);
}

void Note(std::uint64_t addr, std::uint64_t size) noexcept {
    if (!kEnabled || !started.load(std::memory_order_acquire) || size == 0) {
        return;
    }
    if (size > kMaxRange) {
        size = kMaxRange;
    }
    std::uint64_t pos = enqueue_pos.load(std::memory_order_relaxed);
    for (;;) {
        Slot& s = slots[pos & kMask];
        const std::uint64_t seq = s.seq.load(std::memory_order_acquire);
        const auto diff = static_cast<std::int64_t>(seq - pos);
        if (diff == 0) {
            if (enqueue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                s.addr = addr;
                s.size = size;
                s.seq.store(pos + 1, std::memory_order_release);
                noted.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        } else if (diff < 0) {
            dropped.fetch_add(1, std::memory_order_relaxed); // full
            return;
        } else {
            pos = enqueue_pos.load(std::memory_order_relaxed);
        }
    }
}

} // namespace SotcBackingWarm
