// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_write_log.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#undef near
#else
#include <ucontext.h>
#include <unistd.h>
#endif
#include <x86intrin.h>

namespace BbWriteLog {
namespace {
struct Entry {
    std::uint64_t address, size, first, tsc;
    std::uint32_t source, tid;
};
constexpr std::size_t Size = 1 << 20;
std::array<Entry, Size> ring;
std::atomic<std::uint64_t> head{0};
// Writes that contained the suspicious qword (see Note), with the exact address.
std::array<Entry, 256> hits;
std::atomic<std::uint64_t> hits_head{0};
constexpr std::uint64_t Pattern = 0x0000005300000000ull;

void Push(std::array<Entry, Size>& r, std::atomic<std::uint64_t>& h, const Entry& e) {
    r[h.fetch_add(1, std::memory_order_relaxed) % r.size()] = e;
}

// BB_WRITE_LOG=3: the last writes per 4 KiB page of the guest arena. The ring above covers about
// a minute of writes; this answers "what last wrote near this heap chunk" whatever its age.
constexpr std::uint64_t PageBase = 0x1000000000ull; // SOTC arena, 16 GB
constexpr std::uint64_t PageCount = 1ull << 22;
constexpr std::uint64_t PageSlots = 4;
constexpr std::uint64_t PageSpanCap = 2048; // pages of one write that are recorded (8 MiB)
struct PageSlot {
    std::uint64_t address, tsc;
    std::uint32_t size;
    std::uint16_t source, tid;
};
PageSlot* page_slots; // calloc: pages the game never writes through the GPU stay uncommitted
std::uint8_t* page_next;
std::atomic<std::uint64_t> page_outside{0};
std::uint64_t page_tsc0;
std::chrono::steady_clock::time_point page_time0;

void PageInit() {
    static const bool once = [] {
        page_slots = static_cast<PageSlot*>(std::calloc(PageCount * PageSlots, sizeof(PageSlot)));
        page_next = static_cast<std::uint8_t*>(std::calloc(PageCount, 1));
        page_tsc0 = __rdtsc();
        page_time0 = std::chrono::steady_clock::now();
        return true;
    }();
    (void)once;
}

void PageNote(std::uint64_t address, std::uint64_t size, std::uint32_t source) {
    PageInit();
    if (!page_slots || !page_next || size == 0 || address < PageBase ||
        address + size > PageBase + (PageCount << 12)) {
        page_outside.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    static thread_local const std::uint16_t tid = static_cast<std::uint16_t>(gettid());
    const std::uint64_t tsc = __rdtsc();
    const std::uint64_t first = (address - PageBase) >> 12;
    const std::uint64_t last = (address + size - 1 - PageBase) >> 12;
    for (std::uint64_t p = first; p <= last && p < first + PageSpanCap; ++p) {
        PageSlot& s = page_slots[p * PageSlots + (page_next[p]++ & (PageSlots - 1))];
        s = {address, tsc, static_cast<std::uint32_t>(size > 0xffffffffull ? 0xffffffffull : size),
             static_cast<std::uint16_t>(source), tid};
    }
}

// The 4 KiB pages around each pointer register, newest write first: the ones that overlap the
// 0x40 bytes before and 0x60 after the pointer (a heap chunk's header and footer) are marked.
void PageDump(const std::uint64_t* regs, const char* const* names, int count) {
    std::fprintf(stderr, "Write trace: %llu writes outside the arena\n",
                 static_cast<unsigned long long>(page_outside.load()));
    if (!page_slots || !page_next) {
        return;
    }
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - page_time0).count();
    const double hz = dt > 0 ? double(__rdtsc() - page_tsc0) / dt : 3.0e9;
    const std::uint64_t now = __rdtsc();
    const char* sources[] = {"backing", "WriteData", "fence", "EOP", "WriteData", "EOS"};
    for (int i = 0; i < count; ++i) {
        const std::uint64_t r = regs[i];
        if (r < PageBase + 0x1000 || r + 0x1000 >= PageBase + (PageCount << 12)) {
            continue;
        }
        std::fprintf(stderr, "Write trace: %s=%#llx\n", names[i], (unsigned long long)r);
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION m;
        if (VirtualQuery(reinterpret_cast<const void*>(r - 8), &m, sizeof(m)) &&
            m.State == MEM_COMMIT && (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0 &&
            m.Protect != 0) {
            std::uint64_t header = 0;
            std::memcpy(&header, reinterpret_cast<const void*>(r - 8), 8);
            std::fprintf(stderr, "Write trace:   [%s-8] = %#llx\n", names[i],
                         (unsigned long long)header);
        }
#endif
        const std::uint64_t lo = r - 0x40, hi = r + 0x60;
        std::array<const PageSlot*, 16> found{};
        std::array<bool, 16> overlap{};
        int n = 0;
        for (std::uint64_t p = (lo - PageBase) >> 12; p <= (hi - PageBase) >> 12; ++p) {
            for (std::uint64_t k = 0; k < PageSlots; ++k) {
                const PageSlot& s = page_slots[p * PageSlots + k];
                if (!s.address || n == 16) {
                    continue;
                }
                bool dup = false;
                for (int j = 0; j < n; ++j) {
                    dup |= found[j]->address == s.address && found[j]->tsc == s.tsc;
                }
                if (!dup) {
                    overlap[n] = s.address < hi && s.address + s.size > lo;
                    found[n++] = &s;
                }
            }
        }
        for (int a = 0; a < n; ++a) { // newest first
            for (int b = a + 1; b < n; ++b) {
                if (found[b]->tsc > found[a]->tsc) {
                    std::swap(found[a], found[b]);
                    std::swap(overlap[a], overlap[b]);
                }
            }
        }
        for (int a = 0; a < n; ++a) {
            const PageSlot& s = *found[a];
            std::fprintf(stderr,
                         "Write trace:   %s %s %#llx +%u tid %u, %.3f s before the fault\n",
                         overlap[a] ? "OVERLAP" : "page   ",
                         s.source < 6 ? sources[s.source]
                         : s.source == 11 ? "buffer readback"
                         : s.source == 12 ? "fence/label"
                         : s.source == 13 ? "image download"
                         : s.source == 20 ? "replace from CPU"
                                          : "backing (other)",
                         (unsigned long long)s.address, s.size, s.tid, double(now - s.tsc) / hz);
        }
        if (n == 0) {
            std::fprintf(stderr, "Write trace:   no logged GPU write in the pages around it\n");
        }
    }
}
} // namespace

void Record(std::uint64_t address, const void* data, std::uint64_t size, Source source);

int Mode() {
    static const int mode = [] {
        const char* env = std::getenv("BB_WRITE_LOG");
        return env ? std::atoi(env) : 0;
    }();
    return mode;
}

bool Enabled() {
    return Mode() == 1;
}

void NoteIntent(std::uint64_t address, const void* data, std::uint64_t size, Source source) {
    if (Mode() == 2) {
        Record(address, data, size, source);
    }
}

void Note(std::uint64_t address, const void* data, std::uint64_t size, Source source) {
    if (Enabled()) {
        Record(address, data, size, source);
    } else if (Mode() == 3) {
        PageNote(address, size, source);
    }
}

void Record(std::uint64_t address, const void* data, std::uint64_t size, Source source) {
    // Per-source totals every 10 s (which kind of write runs when the guest's data breaks).
    static std::atomic<std::uint64_t> counts[32], byte_totals[32], last_report{0};
    const std::uint32_t slot = source < 32 ? source : 31;
    counts[slot].fetch_add(1, std::memory_order_relaxed);
    byte_totals[slot].fetch_add(size, std::memory_order_relaxed);
    const std::uint64_t tsc_now = __rdtsc();
    std::uint64_t prev = last_report.load(std::memory_order_relaxed);
    if (tsc_now - prev > 30'000'000'000ull && last_report.compare_exchange_strong(prev, tsc_now)) {
        std::string line = "Write log totals:";
        for (std::uint32_t i = 0; i < 32; ++i) {
            if (const std::uint64_t c = counts[i].exchange(0)) {
                line += " src" + std::to_string(i) + "=" + std::to_string(c) + "/" +
                        std::to_string(byte_totals[i].exchange(0) >> 10) + "KiB";
            }
        }
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    static thread_local const std::uint32_t tid = static_cast<std::uint32_t>(gettid());
    Entry e{address, size, 0, __rdtsc(), source, tid};
    std::memcpy(&e.first, data, size < 8 ? size : 8);
    Push(ring, head, e);
    // Only small writes are scanned: scanning downloads of megabytes delays them enough to hide
    // the race this log is for.
    if (size > 64) {
        return;
    }
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::uint64_t at = (8 - (address & 7)) & 7; at + 8 <= size; at += 8) {
        std::uint64_t v;
        std::memcpy(&v, bytes + at, 8);
        if (v == Pattern) {
            Entry hit = e;
            hit.address = address + at;
            hit.first = v;
            hits[hits_head.fetch_add(1, std::memory_order_relaxed) % hits.size()] = hit;
        }
    }
}
} // namespace BbWriteLog

extern "C" void bbgpu_dump_guest_writes(void* ucontext) {
    using namespace BbWriteLog;
    if (Mode() == 0) {
        return;
    }
#ifdef _WIN32
    // The loader passes EXCEPTION_POINTERS (see signal_context.cpp).
    const CONTEXT* c = static_cast<const EXCEPTION_POINTERS*>(ucontext)->ContextRecord;
    const std::uint64_t regs[] = {c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->R14, c->R15};
#else
    const auto* uc = static_cast<const ucontext_t*>(ucontext);
    const auto* g = uc->uc_mcontext.gregs;
    const std::uint64_t regs[] = {std::uint64_t(g[REG_RAX]), std::uint64_t(g[REG_RBX]),
                                  std::uint64_t(g[REG_RCX]), std::uint64_t(g[REG_RDX]),
                                  std::uint64_t(g[REG_RSI]), std::uint64_t(g[REG_RDI]),
                                  std::uint64_t(g[REG_R14]), std::uint64_t(g[REG_R15])};
#endif
    const char* names[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r14", "r15"};
    if (Mode() == 3) {
        // BB_WRITE_LOG=3: no dereference of a register here (the fault may be a garbage pointer).
#ifdef _WIN32
        const std::uint64_t more[] = {c->R12, c->R13, c->Rbp};
#else
        const std::uint64_t more[] = {std::uint64_t(g[REG_R12]), std::uint64_t(g[REG_R13]),
                                      std::uint64_t(g[REG_RBP])};
#endif
        const char* more_names[] = {"r12", "r13", "rbp"};
        std::uint64_t all[11];
        const char* all_names[11];
        for (int i = 0; i < 8; ++i) {
            all[i] = regs[i];
            all_names[i] = names[i];
        }
        for (int i = 0; i < 3; ++i) {
            all[8 + i] = more[i];
            all_names[8 + i] = more_names[i];
        }
        PageDump(all, all_names, 11);
        return;
    }
    for (int i = 0; i < 8; ++i) {
        std::fprintf(stderr, "Write log: %s=%#llx\n", names[i], (unsigned long long)regs[i]);
    }
    const char* sources[] = {"backing",      "WriteData",           "fence",
                             "EOP (decoded)", "WriteData (decoded)", "EOS (decoded)"};
    const std::uint64_t now = __rdtsc();
    const auto print = [&](const Entry& e, const char* what) {
        std::fprintf(stderr,
                     "Write log: %s %s %#llx +%llu first %#llx tid %u, %.3f s before the fault\n",
                     what, e.source < 6 ? sources[e.source]
                           : e.source == 11 ? "buffer readback"
                           : e.source == 12 ? "fence/label"
                           : e.source == 13 ? "image download"
                           : e.source == 20 ? "replace from CPU"
                           : "backing (other)",
                     (unsigned long long)e.address,
                     (unsigned long long)e.size, (unsigned long long)e.first, e.tid,
                     double(now - e.tsc) / 3.0e9);
    };
    const std::uint64_t nh = hits_head.load();
    for (std::uint64_t i = nh > hits.size() ? nh - hits.size() : 0; i < nh; ++i) {
        print(hits[i % hits.size()], "pattern");
    }
    // The block the guest read (rax) and all logged writes into it or its neighbours.
    const std::uint64_t block = regs[0];
    for (std::uint64_t at = block - 0x30; at < block + 0x60; at += 8) {
        std::uint64_t value = 0;
        std::memcpy(&value, reinterpret_cast<const void*>(at), 8);
        std::fprintf(stderr, "Write log: [%#llx] = %#llx\n", (unsigned long long)at,
                     (unsigned long long)value);
    }
    {
        const std::uint64_t n = head.load();
        int shown = 0;
        for (std::uint64_t i = n; i-- > (n > Size ? n - Size : 0) && shown < 200;) {
            const Entry& e = ring[i % Size];
            if (e.address + e.size > block - 0x30 && e.address < block + 0x60) {
                print(e, "in block");
                ++shown;
            }
        }
        shown = 0;
        for (std::uint64_t i = n; i-- > (n > Size ? n - Size : 0) && shown < 40;) {
            const Entry& e = ring[i % Size];
            if ((e.first & 0xffffffffull) == 0x53 || (e.first >> 32) == 0x53) {
                print(e, "value 0x53");
                ++shown;
            }
        }
    }
    // Writes that cover the chunk header the guest read (rax..rax+0x40) or the registers.
    const std::uint64_t n = head.load();
    int shown = 0;
    for (std::uint64_t i = n; i-- > (n > Size ? n - Size : 0) && shown < 64;) {
        const Entry& e = ring[i % Size];
        bool near = false;
        for (const auto r : regs) {
            near |= r + 0x1000 > e.address && r < e.address + e.size + 0x1000;
        }
        if (near) {
            print(e, "near");
            ++shown;
        }
    }
    std::fprintf(stderr, "Write log: %llu writes logged\n", (unsigned long long)n);
}
