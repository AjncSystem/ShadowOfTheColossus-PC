// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_TRACE_SAMPLE=<Hz>, the continuous stack sampler (trace v2, layer L3a; see sotc_sampler.h).
//
// One thread visits every real thread of the trace (SotcTrace::Threads) at its own rate: cp, draw
// and recorder threads at <Hz>, guest threads at min(<Hz>, SOTC_TRACE_SAMPLE_GUEST, default 200).
// A thread is sampled only when its cycle count moved, so idle threads cost nothing. A sample:
//   1. Suspended (a few microseconds): GetThreadContext, then a memcpy of up to 64 KiB of stack from
//      rsp, when rsp lies in the stack range known for the thread (its hint). No lock, no heap, no
//      stdio, no loader call inside. The window is measured (SuspendThread to memcpy end).
//   2. Resumed: SotcUnwind::Walk over the copy, the stack interned, and one EvSample on the
//      thread's "samples" track (aux = stack id, a = leaf address, flags bit 0 = walk cut short).
// Everything that takes a lock or allocates (thread and module lists, GuestStackRange, interning)
// runs outside step 1. Only this thread calls SotcUnwind and AddHostModule.
#include "sotc_sampler.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../sotc/unwind.h"
#include "bbport_threads.h"
#include "video_core/page_manager.h"
#include "video_core/sotc_trace.h"

// src/trace_imports.h is a C header: declared here with C linkage (see that header for the contract).
extern "C" void trace_imports_thunk_range(uint64_t* begin, uint64_t* end);

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace SotcSampler {
namespace {

constexpr uint32_t kMaxTargets = 128;
constexpr uint32_t kMaxStacks = 1u << 16;
constexpr uint32_t kIndexSize = 1u << 17; // more slots than stacks: the probe always ends
constexpr size_t kMaxFrames = size_t{1} << 20; // 8 MiB of return addresses, reserved at Start
constexpr uint32_t kMaxDepth = 64;
constexpr size_t kCopyBytes = 64 * 1024;
constexpr uint32_t kMaxHz = 4000;
constexpr uint32_t kDefaultGuestHz = 200;
constexpr uint64_t kRefreshNs = 1'000'000'000; // thread and module lists: once a second
constexpr uint32_t kMaxHostModules = 256; ///< MODULES chunk (TRACE-FORMAT.md type 9)

struct Stack {
    uint64_t hash;
    uint32_t offset; ///< into g_frames
    uint32_t length;
};

struct Target {
    bool alive = false;
    uint32_t os_tid = 0;
    HANDLE handle = nullptr;
    uint32_t track = 0;
    uint64_t period_ns = 0;
    uint64_t next_ns = 0;
    uint64_t cycles = 0;               ///< cycle count at the last sample attempt
    uint64_t hint_lo = 0, hint_hi = 0; ///< stack range a copy may read (empty: no copy)
    uint64_t teb_lo = 0, teb_hi = 0;   ///< TEB stack bounds when known at add time
};

// Configuration, written by Start before the thread exists.
std::atomic<bool> g_started{false};
std::atomic<bool> g_running{false};
uint32_t g_hz = 0;
uint32_t g_guest_hz = kDefaultGuestHz;

// Sampler thread only (no other thread touches these).
Target g_targets[kMaxTargets];
uint32_t g_target_count = 0;
HANDLE g_timer = nullptr;
uint8_t g_copy[kCopyBytes];

// Interned stacks. Filled by the sampler thread and read by the writer's chunk provider, under
// g_stacks_mutex. Outside suspended threads only. Reserved at Start, so no push reallocates.
std::mutex g_stacks_mutex;
std::vector<Stack> g_stacks;
std::vector<uint64_t> g_frames;
std::vector<uint32_t> g_index; ///< open addressing: stack id (index + 1), 0 = empty
size_t g_emitted = 0;          ///< stacks already returned by StacksChunk

// Host modules from the Toolhelp snapshot (RefreshModules). The sampler thread is the only writer;
// the MODULES provider reads under g_modules_mutex. Reserved at Start.
struct HostModule {
    std::string name; ///< base name, UTF-8
    uint64_t base = 0;
    uint64_t size = 0;
    uint64_t preferred_base = 0; ///< PE OptionalHeader.ImageBase; 0 when the header did not parse
};
std::mutex g_modules_mutex;
std::vector<HostModule> g_host_modules;

std::atomic<uint64_t> s_rounds{0}, s_samples{0}, s_idle{0}, s_truncated{0}, s_no_copy{0};
std::atomic<uint64_t> s_hint_miss{0}, s_failed{0}, s_suspend_ns{0}, s_max_suspend_ns{0};
std::atomic<uint64_t> s_stacks{0}, s_dropped_stacks{0};

uint32_t EnvUint(const char* name, uint32_t fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long value = std::strtoul(v, &end, 10);
    if (end == v) {
        return fallback;
    }
    return static_cast<uint32_t>(std::min(value, 1'000'000ul));
}

// TEB bounds of a thread outside the suspended window. NT_TIB: StackBase at 0x08, StackLimit at 0x10.
using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, LONG, PVOID, ULONG, PULONG);
struct ThreadBasicInfo {
    LONG status;
    PVOID teb;
    ULONG_PTR client[2];
    ULONG_PTR affinity;
    LONG priority;
    LONG base_priority;
};

bool ReadTebBounds(HANDLE thread, uint64_t& lo, uint64_t& hi) {
    static const auto query = reinterpret_cast<NtQueryInformationThreadFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    if (!query) {
        return false;
    }
    ThreadBasicInfo info{};
    if (query(thread, 0 /* ThreadBasicInformation */, &info, sizeof(info), nullptr) != 0 || !info.teb) {
        return false;
    }
    uint64_t bounds[2] = {}; // StackBase, StackLimit
    SIZE_T read = 0;
    const uint64_t teb = reinterpret_cast<uint64_t>(info.teb);
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(teb + 0x08), bounds,
                           sizeof(bounds), &read) ||
        read != sizeof(bounds) || bounds[0] <= bounds[1]) {
        return false;
    }
    lo = bounds[1];
    hi = bounds[0];
    return true;
}

/// The preferred load address (PE OptionalHeader.ImageBase) of the image at base. False, and
/// preferred untouched, when the mapped header does not parse. Sampler thread only.
// True when [p, p+len) is committed and readable right now. A module enumerated by Toolhelp can be
// mid-load or mid-unload at boot, so its header memory is not safe to dereference by size alone;
// reading it unguarded faulted the game (an unreadable xmllite.dll header during boot).
bool Readable(const void* p, size_t len) {
    const auto* a = reinterpret_cast<const uint8_t*>(p);
    size_t done = 0;
    while (done < len) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(a + done, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT) {
            return false;
        }
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mbi.Protect & readable)) {
            return false;
        }
        const auto* end = reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        if (end <= a + done) {
            return false;
        }
        done = static_cast<size_t>(end - (a));
    }
    return true;
}

bool PreferredBase(uint64_t base, uint64_t size, uint64_t& preferred) {
    const auto* image = reinterpret_cast<const uint8_t*>(base);
    if (!Readable(image, sizeof(IMAGE_DOS_HEADER))) {
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (size < sizeof(IMAGE_DOS_HEADER) || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
        return false;
    }
    const uint64_t nt = static_cast<uint64_t>(dos->e_lfanew);
    const uint64_t nt_span = sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + sizeof(IMAGE_OPTIONAL_HEADER64);
    if (nt + nt_span > size || !Readable(image + nt, nt_span) ||
        *reinterpret_cast<const DWORD*>(image + nt) != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const uint8_t* optional = image + nt + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    const WORD magic = *reinterpret_cast<const WORD*>(optional);
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        preferred = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(optional)->ImageBase;
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        preferred = reinterpret_cast<const IMAGE_OPTIONAL_HEADER32*>(optional)->ImageBase;
    } else {
        return false;
    }
    return true;
}

/// A UTF-16 name as UTF-8 (the Toolhelp module name).
std::string Utf8(const wchar_t* text) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    out.resize(static_cast<size_t>(bytes - 1));
    return out;
}

/// A JSON string literal for s (quotes, backslashes and control characters escaped).
std::string JsonString(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (u < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", u);
            out += buf;
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

/// Host modules (the image and every DLL): the unwinder's .pdata tables and the MODULES chunk.
/// Sampler thread only.
void RefreshModules() {
    HANDLE snap = INVALID_HANDLE_VALUE;
    for (int tries = 0; tries < 3 && snap == INVALID_HANDLE_VALUE; ++tries) {
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    }
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Module32FirstW(snap, &entry); ok; ok = Module32NextW(snap, &entry)) {
        const uint64_t base = reinterpret_cast<uint64_t>(entry.modBaseAddr);
        const bool known = std::any_of(g_host_modules.begin(), g_host_modules.end(),
                                       [base](const HostModule& m) { return m.base == base; });
        if (known || g_host_modules.size() == kMaxHostModules) {
            continue;
        }
        HostModule m;
        m.name = Utf8(entry.szModule);
        m.base = base;
        m.size = entry.modBaseSize;
        PreferredBase(base, m.size, m.preferred_base); // stays 0 when the header does not parse
        {
            std::lock_guard<std::mutex> lock(g_modules_mutex);
            g_host_modules.push_back(std::move(m));
        }
        SotcUnwind::AddHostModule(base); // false past the unwinder's own limit: frames stay Unknown
    }
    CloseHandle(snap);
}

uint64_t PeriodNs(const std::string& kind, uint32_t hz) {
    const bool port = kind == "cp" || kind == "draw" || kind == "recorder";
    const uint32_t rate = std::max<uint32_t>(1, port ? hz : std::min(hz, g_guest_hz));
    return 1'000'000'000ull / rate;
}

Target* FreeSlot() {
    for (uint32_t i = 0; i < g_target_count; ++i) {
        if (!g_targets[i].alive) {
            return &g_targets[i];
        }
    }
    return g_target_count < kMaxTargets ? &g_targets[g_target_count++] : nullptr;
}

void Retire(Target& t) {
    t.alive = false;
    if (t.handle) {
        CloseHandle(t.handle);
        t.handle = nullptr;
    }
}

/// New real threads of the trace get a handle, a samples track and a rate. Sampler thread only.
void RefreshThreads(uint64_t now) {
    const std::vector<SotcTrace::ThreadInfo> threads = SotcTrace::Threads();
    for (const SotcTrace::ThreadInfo& info : threads) {
        if (info.os_tid == 0 || info.os_tid == GetCurrentThreadId()) {
            continue;
        }
        bool known = false;
        for (uint32_t i = 0; i < g_target_count && !known; ++i) {
            known = g_targets[i].alive && g_targets[i].os_tid == info.os_tid;
        }
        if (known) {
            continue;
        }
        HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION |
                                       THREAD_QUERY_LIMITED_INFORMATION,
                                   FALSE, info.os_tid);
        if (!handle) {
            continue;
        }
        DWORD code = 0;
        ULONG64 cycles = 0;
        Target* slot = nullptr;
        uint64_t teb_lo = 0, teb_hi = 0;
        const bool running = GetExitCodeThread(handle, &code) && code == STILL_ACTIVE &&
                             QueryThreadCycleTime(handle, &cycles);
        if (running) {
            slot = FreeSlot();
        }
        if (!slot) {
            CloseHandle(handle); // exited, or the table is full
            continue;
        }
        const char* name = SotcTrace::InternString(info.name.c_str(), info.name.size());
        const uint32_t track = SotcTrace::NewTrack(name, "samples", info.os_tid);
        if (!track) {
            CloseHandle(handle);
            continue;
        }
        const bool teb = ReadTebBounds(handle, teb_lo, teb_hi);
        *slot = Target{};
        slot->alive = true;
        slot->os_tid = info.os_tid;
        slot->handle = handle;
        slot->track = track;
        slot->cycles = cycles;
        slot->period_ns = PeriodNs(info.kind, g_hz);
        slot->next_ns = now;
        slot->teb_lo = teb ? teb_lo : 0;
        slot->teb_hi = teb ? teb_hi : 0;
        slot->hint_lo = slot->teb_lo;
        slot->hint_hi = slot->teb_hi;
    }
}

void AddMax(std::atomic<uint64_t>& max, uint64_t value) {
    uint64_t cur = max.load(std::memory_order_relaxed);
    while (value > cur && !max.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
    }
}

/// Interns a stack and returns its id (1-based, 0 when the tables are full). Sampler thread only.
uint32_t Intern(const uint64_t* frames, uint32_t count) {
    if (count == 0) {
        return 0;
    }
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t i = 0; i < count; ++i) {
        hash ^= frames[i];
        hash *= 1099511628211ull;
    }
    std::lock_guard<std::mutex> lock(g_stacks_mutex);
    uint32_t slot = static_cast<uint32_t>(hash) & (kIndexSize - 1);
    while (const uint32_t id = g_index[slot]) {
        const Stack& s = g_stacks[id - 1];
        if (s.hash == hash && s.length == count &&
            std::memcmp(&g_frames[s.offset], frames, count * sizeof(uint64_t)) == 0) {
            return id;
        }
        slot = (slot + 1) & (kIndexSize - 1);
    }
    if (g_stacks.size() >= kMaxStacks || g_frames.size() + count > kMaxFrames) {
        s_dropped_stacks.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    g_stacks.push_back({hash, static_cast<uint32_t>(g_frames.size()), count});
    g_frames.insert(g_frames.end(), frames, frames + count);
    g_index[slot] = static_cast<uint32_t>(g_stacks.size());
    s_stacks.store(g_stacks.size(), std::memory_order_relaxed);
    return static_cast<uint32_t>(g_stacks.size());
}

/// One sample of one thread: the suspended copy, then the walk on the copy.
void SampleTarget(Target& t) {
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(t.handle, &cycles)) {
        Retire(t); // the thread has exited
        return;
    }
    if (cycles == t.cycles) {
        s_idle.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    t.cycles = cycles;

    // Suspended window: no lock, no heap, no stdio, no loader. The hint is read before it.
    const uint64_t hint_lo = t.hint_lo, hint_hi = t.hint_hi;
    const uint64_t t_start = SotcTrace::Now();
    if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) {
        s_failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    const bool have = GetThreadContext(t.handle, &ctx) != 0;
    uint64_t copied = 0;
    if (have && ctx.Rsp >= hint_lo && ctx.Rsp < hint_hi) {
        copied = std::min<uint64_t>(kCopyBytes, hint_hi - ctx.Rsp);
        std::memcpy(g_copy, reinterpret_cast<const void*>(ctx.Rsp), copied);
    }
    const uint64_t t_stop = SotcTrace::Now();
    ResumeThread(t.handle);
    // Outside the window again.
    const uint64_t stopped = t_stop - t_start;
    s_suspend_ns.fetch_add(stopped, std::memory_order_relaxed);
    AddMax(s_max_suspend_ns, stopped);
    if (!have) {
        s_failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (ctx.Rsp < t.hint_lo || ctx.Rsp >= t.hint_hi) {
        // The thread moved to another stack (a fiber). The next sample copies from it; this one
        // walks registers only. Fiber stacks come from the registry, host stacks from the TEB.
        s_hint_miss.fetch_add(1, std::memory_order_relaxed);
        uint64_t lo = 0, hi = 0;
        if (!VideoCore::GuestStackRange(ctx.Rsp, lo, hi) && ctx.Rsp >= t.teb_lo && ctx.Rsp < t.teb_hi) {
            lo = t.teb_lo;
            hi = t.teb_hi;
        }
        t.hint_lo = lo;
        t.hint_hi = hi;
    }

    const SotcUnwind::Regs regs{ctx.Rip, ctx.Rsp, ctx.Rbp};
    const SotcUnwind::StackCopy stack{ctx.Rsp, g_copy, static_cast<size_t>(copied)};
    uint64_t frames[kMaxDepth];
    bool truncated = false;
    const uint32_t depth = SotcUnwind::Walk(regs, stack, frames, kMaxDepth, &truncated);
    const uint32_t id = Intern(frames, depth);
    const bool cut = truncated || !copied;
    if (cut) {
        (copied ? s_truncated : s_no_copy).fetch_add(1, std::memory_order_relaxed);
    }
    SotcTrace::PushEventOn(t.track, SotcTrace::EvSample, "sample", t_start, t_start, ctx.Rip, 0, id,
                           cut ? 1 : 0);
    s_samples.fetch_add(1, std::memory_order_relaxed);
}

/// Sleeps until `when` (SotcTrace::Now clock): a high-resolution waitable timer, spinning for the
/// last few hundred microseconds. Without the timer, Sleep(1) for long waits.
void WaitUntil(uint64_t when) {
    for (;;) {
        const uint64_t now = SotcTrace::Now();
        if (now >= when || !g_running.load(std::memory_order_relaxed)) {
            return;
        }
        const uint64_t left = when - now;
        if (g_timer && left > 300'000) {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>((left - 150'000) / 100); // 100 ns units, relative
            SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(g_timer, INFINITE);
        } else if (!g_timer && left > 2'000'000) {
            Sleep(1);
        } else {
            YieldProcessor();
        }
    }
}

DWORD WINAPI Run(void*) {
    using SetDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (const auto set = reinterpret_cast<SetDescriptionFn>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))) {
        set(GetCurrentThread(), L"sotc:Sampler");
    }
    // Off the reserved CPU sets (SOTC_CPUSETS): the helper placement, which never uses the four
    // exclusive cores of the port threads. Does nothing when placement is off.
    BbThreads::PlaceThread(BbThreads::Role::Copy);

    RefreshModules();
    RefreshThreads(SotcTrace::Now());
    uint64_t next_refresh = SotcTrace::Now() + kRefreshNs;
    while (g_running.load(std::memory_order_relaxed)) {
        const uint64_t now = SotcTrace::Now();
        if (now >= next_refresh) {
            RefreshModules();
            RefreshThreads(now);
            next_refresh = now + kRefreshNs;
        }
        s_rounds.fetch_add(1, std::memory_order_relaxed);
        uint64_t soonest = next_refresh;
        for (uint32_t i = 0; i < g_target_count; ++i) {
            Target& t = g_targets[i];
            if (!t.alive) {
                continue;
            }
            if (t.next_ns <= now) {
                SampleTarget(t);
                if (t.alive) {
                    const uint64_t after = SotcTrace::Now();
                    t.next_ns += t.period_ns;
                    if (t.next_ns <= after) {
                        t.next_ns = after + t.period_ns; // no catch-up bursts after a stall
                    }
                }
            }
            if (t.alive) {
                soonest = std::min(soonest, t.next_ns);
            }
        }
        WaitUntil(soonest);
    }
    return 0;
}

} // namespace

uint32_t ConfiguredHz() {
    const uint32_t hz = EnvUint("SOTC_TRACE_SAMPLE", 0);
    if (hz > kMaxHz) {
        std::fprintf(stderr, "sotc: SOTC_TRACE_SAMPLE=%u clamped to %u Hz\n", hz, kMaxHz);
        return kMaxHz;
    }
    return hz;
}

bool Start(const Info& info) {
    if (!SotcTrace::kOn) {
        return false;
    }
    const uint32_t hz = ConfiguredHz();
    if (!hz) {
        return false;
    }
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) {
        return false;
    }
    g_hz = hz;
    g_guest_hz = std::clamp<uint32_t>(EnvUint("SOTC_TRACE_SAMPLE_GUEST", kDefaultGuestHz), 1, kMaxHz);
    if (!SotcUnwind::InitGuest(info.image_base, info.image_size, info.eh_frame_hdr,
                               info.eh_frame_hdr_size)) {
        std::fprintf(stderr, "sotc: SOTC_TRACE_SAMPLE: eh_frame_hdr not usable, guest frames use the rbp chain\n");
    }
    // The import thunk common code (src/runtime_trace_imports.c): a return address there is a Thunk
    // frame. Zero when SOTC_TRACE_IMPORTS is off.
    uint64_t thunk_begin = 0;
    uint64_t thunk_end = 0;
    trace_imports_thunk_range(&thunk_begin, &thunk_end);
    if (thunk_end > thunk_begin) {
        SotcUnwind::AddThunkRange(thunk_begin, thunk_end - thunk_begin);
    }
    g_stacks.reserve(kMaxStacks);
    g_host_modules.reserve(kMaxHostModules);
    g_frames.reserve(kMaxFrames);
    g_index.assign(kIndexSize, 0);
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    g_running.store(true, std::memory_order_relaxed);
    const HANDLE thread = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr);
    if (!thread) {
        g_running.store(false, std::memory_order_relaxed);
        g_started.store(false, std::memory_order_relaxed);
        return false;
    }
    CloseHandle(thread);
    SotcTrace::RegisterChunkProvider(7, &StacksChunk); // STACKS, TRACE-FORMAT.md
    SotcTrace::RegisterChunkProvider(9, &ModulesChunk); // MODULES, TRACE-FORMAT.md
    std::fprintf(stderr, "sotc: SOTC_TRACE_SAMPLE: %u Hz (guest %u Hz), stacks on sotc:Sampler\n", g_hz,
                 g_guest_hz);
    return true;
}

void Stop() {
    g_running.store(false, std::memory_order_relaxed);
}

std::string StatsJson() {
    const auto get = [](const std::atomic<uint64_t>& v) {
        return std::to_string(v.load(std::memory_order_relaxed));
    };
    return "{\"hz\":" + std::to_string(g_hz) + ",\"guest_hz\":" + std::to_string(g_guest_hz) +
           ",\"rounds\":" + get(s_rounds) + ",\"samples\":" + get(s_samples) +
           ",\"idle\":" + get(s_idle) + ",\"truncated\":" + get(s_truncated) +
           ",\"no_copy\":" + get(s_no_copy) + ",\"hint_miss\":" + get(s_hint_miss) +
           ",\"failed\":" + get(s_failed) + ",\"suspend_ns\":" + get(s_suspend_ns) +
           ",\"max_suspend_ns\":" + get(s_max_suspend_ns) + ",\"stacks\":" + get(s_stacks) +
           ",\"dropped_stacks\":" + get(s_dropped_stacks) + "}";
}

std::string StacksChunk() {
    std::lock_guard<std::mutex> lock(g_stacks_mutex);
    if (g_emitted >= g_stacks.size()) {
        return {};
    }
    std::string out = "{";
    for (size_t id = g_emitted; id < g_stacks.size(); ++id) {
        const Stack& s = g_stacks[id];
        if (id != g_emitted) {
            out += ',';
        }
        out += '"' + std::to_string(id + 1) + "\":[";
        for (uint32_t i = 0; i < s.length; ++i) {
            if (i) {
                out += ',';
            }
            out += std::to_string(g_frames[s.offset + i]);
        }
        out += ']';
    }
    out += '}';
    g_emitted = g_stacks.size();
    return out;
}

std::string ModulesChunk() {
    std::lock_guard<std::mutex> lock(g_modules_mutex);
    if (g_host_modules.empty()) {
        return {};
    }
    std::string out = "[";
    for (size_t i = 0; i < g_host_modules.size(); ++i) {
        const HostModule& m = g_host_modules[i];
        if (i) {
            out += ',';
        }
        out += "{\"name\":" + JsonString(m.name) + ",\"base\":" + std::to_string(m.base) +
               ",\"size\":" + std::to_string(m.size) +
               ",\"preferred_base\":" + std::to_string(m.preferred_base) + '}';
    }
    out += ']';
    return out;
}

} // namespace SotcSampler
