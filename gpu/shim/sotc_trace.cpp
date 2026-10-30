// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_TRACE flight recorder (see video_core/sotc_trace.h, docs/sotc/TRACE-FORMAT.md).
//
// Producers push 48-byte events into per-thread rings: one store per event, no lock, no
// allocation once the thread's ring exists. A writer thread does all the I/O and the compression.
// Mode 1 copies the window around each slow frame (or RequestDump) out of the rings into a
// <session>/frameNNNNNN_XXms.sotctrace or manualNNN.sotctrace file. Mode 2 drains every ring from
// its cursor to its head about every 50 ms into <session>/session.sotctrace.
// The frames track (FrameEnd, FlipMark, Mark) is one ring shared by several threads: its pushes
// take a small spinlock. Everything else pushes to the calling thread's own ring.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif
#include <pthread.h>

#include "miniz.h"
#include "video_core/sotc_stats.h"
#include "video_core/sotc_trace.h"

static_assert(std::endian::native == std::endian::little, "the trace file is little endian");

namespace SotcTrace {

namespace {

using Clock = std::chrono::steady_clock;

// File chunk types (TRACE-FORMAT.md).
constexpr uint32_t kChunkMeta = 1;
constexpr uint32_t kChunkThreads = 2;
constexpr uint32_t kChunkNames = 3;
constexpr uint32_t kChunkEvents = 4;
constexpr uint32_t kChunkZoneStats = 5;
constexpr uint32_t kChunkEnd = 0xFFFFFFFFu;
constexpr char kMagic[8] = {'S', 'O', 'T', 'C', 'T', 'R', 'C', 0x01};
constexpr size_t kRecordSize = 48;

// SOTC_TRACE_JSON=1: mode 1 also writes the Chrome JSON next to each dump.
const bool kJsonDumps = [] {
    const char* v = std::getenv("SOTC_TRACE_JSON");
    return v && v[0] == '1';
}();

// Track order in the viewer: game frames, guest threads, then the GPU side.
enum Kind : uint8_t { KindFrames, KindGuest, KindCp, KindDraw, KindRecorder, KindGpuOther, KindQueue, KindExtra };

const char* KindName(Kind k) {
    switch (k) {
    case KindFrames:
        return "frames";
    case KindGuest:
        return "guest";
    case KindCp:
        return "cp";
    case KindDraw:
        return "draw";
    case KindRecorder:
        return "recorder";
    case KindGpuOther:
        return "gpu";
    case KindQueue:
        return "queue";
    case KindExtra:
        return "extra";
    }
    return "gpu";
}

// One record in memory. The field order matches the file's record (PutRecord packs the same
// fields in the same order, with the reserved bytes as zero).
struct Event {
    uint64_t t0, t1;
    const char* name;
    uint64_t a, b;
    uint32_t aux;
    uint8_t kind;
    uint8_t pad;
    uint16_t flags;
};
static_assert(sizeof(Event) == kRecordSize);

Event MakeEvent(uint8_t kind, uint64_t t0, uint64_t t1, const char* name, uint64_t a = 0,
                uint64_t b = 0, uint32_t aux = 0, uint16_t flags = 0) {
    return Event{t0, t1, name, a, b, aux, kind, 0, flags};
}

struct Ring {
    std::unique_ptr<Event[]> ev;
    uint64_t mask = 0;
    std::atomic<uint64_t> head{0}; // events written so far; slot = index & mask
    uint64_t cursor = 0;           // mode 2 writer only: events already written to the stream
    std::string name;
    uint32_t os_tid = 0;
    Kind kind = KindGuest;
    uint32_t id = 0;
    std::string kind_name; // KindExtra tracks: the kind string written in THREADS

    Ring(uint64_t size, Kind k) : ev{new Event[size]}, mask{size - 1}, kind{k} {}
    void Push(const Event& e) {
        const uint64_t h = head.load(std::memory_order_relaxed);
        ev[h & mask] = e;
        head.store(h + 1, std::memory_order_release);
    }
    uint64_t Capacity() const {
        return mask + 1;
    }
};

struct Queue {
    Ring* ring = nullptr;
    std::unique_ptr<std::atomic<uint64_t>[]> submit_ns{new std::atomic<uint64_t>[4096]{}};
    uint64_t last_done_ns = 0;
};

// A dump waiting for the writer: a slow frame (its window grows while slow frames join it) or a
// RequestDump (window = the last pre_ns up to the request).
struct Job {
    uint64_t frame = 0;
    uint64_t start = 0, end = 0; // the slow frame(s): first start, last end
    uint64_t worst_ns = 0;
    uint32_t slow_frames = 0;
    bool manual = false;
    uint32_t manual_index = 0;
    std::string reason; // "slow" or "manual: <text>" (META)
};

// Ring ids index a fixed table of rings, so PushEventOn finds its ring with one load.
constexpr uint32_t kMaxSlots = 4096;

struct State {
    std::mutex rings_mutex; // the lists of rings and queues, not the events
    std::vector<Ring*> rings; // never freed: a dump may read a ring after its thread exited
    std::vector<Queue*> queues;

    std::atomic<Ring*> frames{nullptr};
    std::atomic_flag frames_lock; // guards pushes to the frames ring (any thread)

    std::mutex job_mutex;
    std::condition_variable job_cv;
    std::deque<Job> jobs; // guarded by job_mutex
    std::atomic<bool> writer_started{false};
    std::atomic<bool> stop{false};      // set by the exit handler
    std::atomic<bool> flush_now{false}; // mode 2: RequestDump asks for a drain
    std::mutex done_mutex;
    std::condition_variable done_cv;
    bool writer_done = false; // guarded by done_mutex

    std::atomic<uint64_t> image_base{0}, image_size{0};
    std::string user_dir = "user";
    std::string session_dir; // writer thread only

    std::atomic<uint32_t> dumps{0};
    std::atomic<uint32_t> manual_serial{0};
    std::atomic<uint64_t> frame_now{0}; // last frame number, for manual dumps
    std::atomic<uint64_t> dropped{0}, windows_overwritten{0};
    std::atomic<uint64_t> bytes_written{0}, stream_bytes{0};
    std::atomic<uint64_t> events_per_s{0}, bytes_per_s{0}; // set by the writer every 5 s
    std::atomic<const char*> segment{nullptr};              // interned; nullptr: "frame"
    std::atomic<uint32_t> lap{0};

    std::atomic<uint64_t> threshold_ns{60'000'000};
    uint64_t pre_ns = 150'000'000, post_ns = 40'000'000;
    uint32_t max_dumps = 40;
    uint64_t ring_mul = 1;

    std::mutex zone_mutex;
    std::vector<ZoneSite*> zones; // guarded by zone_mutex

    std::mutex intern_mutex;
    std::unordered_set<std::string> interned; // node-based: the c_str() pointers stay valid

    std::mutex last_mutex;
    DumpInfo last; // guarded by last_mutex

    std::atomic<Ring*> slots[kMaxSlots]{}; // ring id -> ring (filled by NewRing, read lock-free)
    std::mutex providers_mutex;
    std::vector<std::pair<uint32_t, ChunkProvider>> providers; // guarded by providers_mutex
    struct CSlot {
        uint32_t type = 0;
        const char* (*fn)(size_t*) = nullptr;
    };
    CSlot c_slots[16];  // guarded by providers_mutex
    size_t c_count = 0; // guarded by providers_mutex
};

State& S() {
    static State* s = [] {
        auto* st = new State;
        const auto ms = [](const char* name, uint64_t def) {
            const char* v = std::getenv(name);
            return (v ? std::strtoull(v, nullptr, 10) : def) * 1'000'000ULL;
        };
        st->threshold_ns = ms("SOTC_TRACE_MS", 60);
        st->pre_ns = ms("SOTC_TRACE_PRE_MS", 150);
        st->post_ns = ms("SOTC_TRACE_POST_MS", 40);
        if (const char* v = std::getenv("SOTC_TRACE_MAX")) {
            st->max_dumps = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
        }
        if (const char* v = std::getenv("SOTC_TRACE_RING")) {
            const unsigned long n = std::strtoul(v, nullptr, 10);
            if (n == 2 || n == 4 || n == 8) {
                st->ring_mul = n;
            }
        }
        return st;
    }();
    return *s;
}

std::string CurrentThreadName() {
    std::string name;
#ifdef _WIN32
    PWSTR wide = nullptr;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &wide)) && wide) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            name.resize(static_cast<size_t>(n - 1));
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, name.data(), n, nullptr, nullptr);
        }
        LocalFree(wide);
    }
#endif
    if (name.empty()) {
        char buf[64] = "";
        if (pthread_getname_np(pthread_self(), buf, sizeof(buf)) == 0) {
            name = buf;
        }
    }
    return name;
}

uint32_t CurrentOsTid() {
#ifdef _WIN32
    return static_cast<uint32_t>(GetCurrentThreadId());
#else
    return 0;
#endif
}

void EnsureWriter();

// Creates a ring (base size times SOTC_TRACE_RING) and makes sure the writer runs.
Ring* NewRing(uint64_t base_size, Kind kind, std::string name, uint32_t os_tid,
              const char* kind_name = nullptr) {
    State& s = S();
    auto* r = new Ring(base_size * s.ring_mul, kind);
    r->name = std::move(name);
    r->os_tid = os_tid;
    if (kind_name) {
        r->kind_name = kind_name; // set before the ring is published
    }
    {
        std::scoped_lock lk{s.rings_mutex};
        r->id = static_cast<uint32_t>(s.rings.size()) + 1;
        s.rings.push_back(r);
        if (r->id < kMaxSlots) {
            s.slots[r->id].store(r, std::memory_order_release);
        }
    }
    EnsureWriter();
    return r;
}

thread_local Ring* tl_ring = nullptr;
thread_local const char* tl_fiber_name = nullptr;
thread_local uint64_t tl_fiber_t0 = 0;

Ring* ThisRing() {
    if (tl_ring) {
        return tl_ring;
    }
    Kind kind = KindGuest;
    switch (SotcStats::tl_role) {
    case SotcStats::RoleCp:
        kind = KindCp;
        break;
    case SotcStats::RoleDraw:
        kind = KindDraw;
        break;
    case SotcStats::RoleRecorder:
        kind = KindRecorder;
        break;
    case SotcStats::RoleOther:
        kind = KindGuest;
        break;
    default:
        kind = KindGpuOther;
        break;
    }
    // Guest threads only record their waits and faults; the GPU threads record far more.
    const uint64_t size = kind == KindGuest ? (uint64_t{1} << 14) : (uint64_t{1} << 17);
    tl_ring = NewRing(size, kind, CurrentThreadName(), CurrentOsTid());
    return tl_ring;
}

std::vector<Ring*> RingList() {
    State& s = S();
    std::scoped_lock lk{s.rings_mutex};
    return s.rings;
}

// The frames ring, created on first use. Callers hold frames_lock.
Ring* FramesRing() {
    State& s = S();
    Ring* r = s.frames.load(std::memory_order_acquire);
    if (!r) {
        r = NewRing(uint64_t{1} << 12, KindFrames, "game frames (flip to flip)", 0);
        s.frames.store(r, std::memory_order_release);
    }
    return r;
}

void PushFrames(const Event& e) {
    State& s = S();
    while (s.frames_lock.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    FramesRing()->Push(e);
    s.frames_lock.clear(std::memory_order_release);
}

// JSON string literal (quotes included). Control characters become \u escapes.
std::string JsonQuoted(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
            out += buf;
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

// Appends a little-endian value (the host is little endian, see the static_assert).
template <class T>
void PutRaw(std::string& out, T v) {
    out.append(reinterpret_cast<const char*>(&v), sizeof(T));
}

// Writes the 48-byte record of TRACE-FORMAT.md (reserved bytes are 0).
void PutRecord(std::string& out, const Event& e, uint32_t name_id) {
    PutRaw<uint64_t>(out, e.t0);
    PutRaw<uint64_t>(out, e.t1);
    PutRaw<uint32_t>(out, name_id);
    PutRaw<uint8_t>(out, e.kind);
    PutRaw<uint8_t>(out, 0);
    PutRaw<uint16_t>(out, e.flags);
    PutRaw<uint32_t>(out, e.aux);
    PutRaw<uint32_t>(out, 0);
    PutRaw<uint64_t>(out, e.a);
    PutRaw<uint64_t>(out, e.b);
}

// Name pointers to file ids. Ids start at 1 in each file; equal texts at different pointers
// get different ids, which the format allows.
struct NameTable {
    std::unordered_map<const char*, uint32_t> ids;
    std::vector<std::pair<uint32_t, const char*>> pending; // ids added since the last NAMES chunk

    uint32_t Id(const char* name) {
        if (!name) {
            return 0;
        }
        const auto it = ids.find(name);
        if (it != ids.end()) {
            return it->second;
        }
        const uint32_t id = static_cast<uint32_t>(ids.size()) + 1;
        ids.emplace(name, id);
        pending.emplace_back(id, name);
        return id;
    }
};

// EVENTS payload: the ring's THREADS id, the count, then the records.
std::string EventsPayload(uint32_t ring_id, const Event* ev, size_t count, NameTable& names) {
    std::string out;
    out.reserve(8 + count * kRecordSize);
    PutRaw<uint32_t>(out, ring_id);
    PutRaw<uint32_t>(out, static_cast<uint32_t>(count));
    for (size_t i = 0; i < count; ++i) {
        PutRecord(out, ev[i], names.Id(ev[i].name));
    }
    return out;
}

// NAMES chunk: JSON object, decimal id -> name, for the ids added since the last one.
std::string NamesChunk(NameTable& names) {
    std::string out = "{";
    bool first = true;
    for (const auto& [id, name] : names.pending) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += '"' + std::to_string(id) + "\":" + JsonQuoted(name);
    }
    out += '}';
    names.pending.clear();
    return out;
}

// Writes one chunk: 16-byte header, then the payload deflated with miniz (level 1) when that is
// smaller. Payloads of 256 bytes or less are stored. Returns the bytes written.
uint64_t WriteChunk(std::FILE* f, uint32_t type, std::string_view payload) {
    uint32_t flags = 0;
    std::string_view data = payload;
    std::string packed;
    if (payload.size() > 256) {
        mz_ulong len = mz_compressBound(static_cast<mz_ulong>(payload.size()));
        packed.resize(len);
        if (mz_compress2(reinterpret_cast<unsigned char*>(packed.data()), &len,
                         reinterpret_cast<const unsigned char*>(payload.data()),
                         static_cast<mz_ulong>(payload.size()), 1) == MZ_OK &&
            len < payload.size()) {
            flags = 1;
            data = std::string_view(packed.data(), len);
        }
    }
    const uint32_t hdr[4] = {type, flags, static_cast<uint32_t>(data.size()),
                             static_cast<uint32_t>(payload.size())};
    std::fwrite(hdr, sizeof(hdr), 1, f);
    if (!data.empty()) {
        std::fwrite(data.data(), 1, data.size(), f);
    }
    return sizeof(hdr) + data.size();
}

// The thread's name and track kind as the viewer shows them (game threads get a "game: " prefix,
// shadPS4's helper threads are GPU-side).
struct ThreadView {
    std::string name;
    Kind kind;
};

ThreadView ViewOf(const Ring* r) {
    std::string name = r->name;
    if (name.empty()) {
        name = "thread " + std::to_string(r->os_tid);
    }
    Kind kind = r->kind;
    if (kind == KindGuest && name.rfind("shadPS4", 0) == 0) {
        kind = KindGpuOther;
    }
    if (kind == KindGuest) {
        name = "game: " + name;
    }
    return {std::move(name), kind};
}

// One THREADS entry (no brackets).
std::string ThreadJson(const Ring* r) {
    const ThreadView v = ViewOf(r);
    const uint32_t sort = static_cast<uint32_t>(v.kind) * 100000u + r->id;
    return "{\"id\":" + std::to_string(r->id) + ",\"os_tid\":" + std::to_string(r->os_tid) +
           ",\"name\":" + JsonQuoted(v.name) + ",\"kind\":" +
           JsonQuoted(r->kind == KindExtra ? r->kind_name : std::string(KindName(v.kind))) +
           ",\"sort\":" + std::to_string(sort) + "}";
}

// ZONE_STATS chunk: JSON array, cumulative counters of every registered zone site.
std::string ZoneStatsJson() {
    State& s = S();
    std::scoped_lock lk{s.zone_mutex};
    std::string out = "[";
    bool first = true;
    for (const ZoneSite* z : s.zones) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += "{\"name\":" + JsonQuoted(z->name ? z->name : "") +
               ",\"file\":" + JsonQuoted(z->file ? z->file : "") +
               ",\"line\":" + std::to_string(z->line) +
               ",\"calls\":" + std::to_string(z->calls.load(std::memory_order_relaxed)) +
               ",\"total_ns\":" + std::to_string(z->total_ns.load(std::memory_order_relaxed)) +
               ",\"max_ns\":" + std::to_string(z->max_ns.load(std::memory_order_relaxed)) + "}";
    }
    out += ']';
    return out;
}

// Writes the extra chunks of the registered providers (writer thread only).
uint64_t WriteProviders(std::FILE* f) {
    State& s = S();
    std::vector<std::pair<uint32_t, ChunkProvider>> list;
    {
        std::scoped_lock lk{s.providers_mutex};
        list = s.providers;
    }
    uint64_t bytes = 0;
    for (const auto& [type, provider] : list) {
        const std::string payload = provider();
        if (!payload.empty()) {
            bytes += WriteChunk(f, type, payload);
        }
    }
    return bytes;
}

struct StatsSnapshot {
    uint64_t rings = 0, events = 0, events_per_s = 0, bytes_per_s = 0;
    uint64_t dropped = 0, windows = 0, dumps = 0, stream_bytes = 0, threshold_ms = 0;
};

StatsSnapshot Snapshot() {
    State& s = S();
    StatsSnapshot v;
    {
        std::scoped_lock lk{s.rings_mutex};
        v.rings = s.rings.size();
        for (const Ring* r : s.rings) {
            v.events += r->head.load(std::memory_order_acquire);
        }
    }
    v.events_per_s = s.events_per_s.load(std::memory_order_relaxed);
    v.bytes_per_s = s.bytes_per_s.load(std::memory_order_relaxed);
    v.dropped = s.dropped.load(std::memory_order_relaxed);
    v.windows = s.windows_overwritten.load(std::memory_order_relaxed);
    v.dumps = s.dumps.load(std::memory_order_relaxed);
    v.stream_bytes = s.stream_bytes.load(std::memory_order_relaxed);
    v.threshold_ms = s.threshold_ns.load(std::memory_order_relaxed) / 1'000'000;
    return v;
}

std::string StatsObject(const StatsSnapshot& v) {
    return "{\"mode\":" + std::to_string(kMode) + ",\"rings\":" + std::to_string(v.rings) +
           ",\"events\":" + std::to_string(v.events) +
           ",\"events_per_s\":" + std::to_string(v.events_per_s) +
           ",\"bytes_per_s\":" + std::to_string(v.bytes_per_s) +
           ",\"dropped\":" + std::to_string(v.dropped) +
           ",\"windows_overwritten\":" + std::to_string(v.windows) +
           ",\"dumps\":" + std::to_string(v.dumps) +
           ",\"stream_bytes\":" + std::to_string(v.stream_bytes) +
           ",\"threshold_ms\":" + std::to_string(v.threshold_ms) + "}";
}

// The keys every META chunk carries, without braces.
std::string MetaSettings() {
    State& s = S();
    return "\"clock\":\"steady_ns\",\"image_base\":" + std::to_string(s.image_base.load()) +
           ",\"image_size\":" + std::to_string(s.image_size.load()) +
           ",\"threshold_ms\":" + std::to_string(s.threshold_ns.load() / 1'000'000) +
           ",\"pre_ms\":" + std::to_string(s.pre_ns / 1'000'000) +
           ",\"post_ms\":" + std::to_string(s.post_ns / 1'000'000) +
           ",\"created_unix\":" + std::to_string(static_cast<long long>(std::time(nullptr)));
}

// Takes the name the thread gave itself (Windows only; the writer thread calls it).
void RefreshName(Ring* r) {
#ifdef _WIN32
    if (r->os_tid == 0 || r->kind == KindQueue || r->kind == KindFrames || r->kind == KindExtra) {
        return;
    }
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, r->os_tid);
    if (!h) {
        return;
    }
    PWSTR wide = nullptr;
    if (SUCCEEDED(GetThreadDescription(h, &wide)) && wide) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            std::string name(static_cast<size_t>(n - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, name.data(), n, nullptr, nullptr);
            // Threads() (the sampler, another thread) reads r->name under rings_mutex; take it
            // here too, since this runs on the writer thread.
            std::scoped_lock lk{S().rings_mutex};
            r->name = std::move(name);
        }
        LocalFree(wide);
    }
    CloseHandle(h);
#else
    (void)r;
#endif
}

// The session folder, created at its first file (writer thread only).
const std::string& SessionDir() {
    State& s = S();
    if (s.session_dir.empty()) {
        std::string base;
        if (const char* v = std::getenv("SOTC_TRACE_DIR"); v && *v) {
            base = v;
        } else {
            base = s.user_dir + "/trace";
        }
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
        s.session_dir = base + "/" + stamp;
        std::error_code ec;
        std::filesystem::create_directories(s.session_dir, ec);
    }
    return s.session_dir;
}

// The events of one ring inside a dump window, in the order the ring holds them.
struct Window {
    Ring* ring;
    std::vector<Event> events;
};

// Copies the events of [win_start, win_end] out of every ring. A ring's events are in end-time
// order (each span is pushed when it ends), so the walk back stops at the first run of events
// that ended before the window. overwritten is set when a ring wrapped past win_start.
std::vector<Window> CopyWindow(const std::vector<Ring*>& rings, uint64_t win_start,
                               uint64_t win_end, bool& overwritten) {
    std::vector<Window> outs;
    for (Ring* r : rings) {
        const uint64_t h1 = r->head.load(std::memory_order_acquire);
        const uint64_t cap = r->Capacity();
        const uint64_t lowest = h1 > cap ? h1 - cap : 0;
        if (h1 > cap && r->ev[lowest & r->mask].t0 > win_start) {
            overwritten = true;
        }
        std::vector<Event> copied;
        std::vector<uint64_t> index;
        // A merged batch (OpBatch) is pushed after spans that ended later, so the walk stops only
        // after a run of events that all ended before the window.
        uint32_t old_run = 0;
        for (uint64_t i = h1; i > lowest; --i) {
            const Event e = r->ev[(i - 1) & r->mask];
            if (e.t1 < win_start) {
                if (++old_run >= 64) {
                    break;
                }
                continue;
            }
            old_run = 0;
            if (e.t0 <= win_end) {
                copied.push_back(e);
                index.push_back(i - 1);
            }
        }
        // Slots the thread overwrote while they were copied are dropped (index h2 may be
        // half-written over index h2 - cap, so that one goes too).
        const uint64_t h2 = r->head.load(std::memory_order_acquire);
        const uint64_t valid_from = h2 + 1 > cap ? h2 + 1 - cap : 0;
        Window out{r, {}};
        for (size_t k = copied.size(); k-- > 0;) {
            if (index[k] >= valid_from) {
                out.events.push_back(copied[k]);
            }
        }
        if (!out.events.empty() || r->kind == KindFrames) {
            outs.push_back(std::move(out));
        }
    }
    return outs;
}

// The old Chrome JSON of a dump (SOTC_TRACE_JSON=1), for ui.perfetto.dev.
void WriteChromeJson(const std::string& path, const Job& job, uint64_t win_start,
                     uint64_t win_end, const std::vector<Window>& outs) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return;
    }
    std::fprintf(f, "{\"displayTimeUnit\":\"ms\",\"otherData\":{\"frame\":%llu,\"worst_ms\":%.1f,"
                    "\"slow_frames\":%u,\"window_ms\":%.1f},\"traceEvents\":[\n",
                 static_cast<unsigned long long>(job.frame), job.worst_ns / 1e6, job.slow_frames,
                 (win_end - win_start) / 1e6);
    std::fprintf(f, "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,\"args\":{\"name\":"
                    "\"Shadow of the Colossus (sotc-pc)\"}}");
    for (const Window& out : outs) {
        const Ring* r = out.ring;
        const ThreadView v = ViewOf(r);
        const uint32_t sort = static_cast<uint32_t>(v.kind) * 100000u + r->id;
        std::fprintf(f, ",\n{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":%u,\"args\":"
                        "{\"name\":%s}},\n{\"name\":\"thread_sort_index\",\"ph\":\"M\",\"pid\":1,"
                        "\"tid\":%u,\"args\":{\"sort_index\":%u}}",
                     r->id, JsonQuoted(v.name).c_str(), r->id, sort);
        for (const Event& e : out.events) {
            const uint64_t t0 = std::max(e.t0, win_start);
            const double ts = (t0 - win_start) / 1e3;
            const std::string name = JsonQuoted(e.name ? e.name : "");
            if (e.t1 == e.t0) {
                std::fprintf(f, ",\n{\"name\":%s,\"ph\":\"i\",\"s\":\"t\",\"pid\":1,\"tid\":%u,"
                                "\"ts\":%.3f,\"args\":{",
                             name.c_str(), r->id, ts);
            } else {
                std::fprintf(f, ",\n{\"name\":%s,\"ph\":\"X\",\"pid\":1,\"tid\":%u,\"ts\":%.3f,"
                                "\"dur\":%.3f,\"args\":{",
                             name.c_str(), r->id, ts, (e.t1 - t0) / 1e3);
            }
            std::fprintf(f, "\"a\":%llu,\"b\":%llu}}", static_cast<unsigned long long>(e.a),
                         static_cast<unsigned long long>(e.b));
        }
    }
    std::fputs("\n]}\n", f);
    std::fclose(f);
}

// Mode 1: writes the dump file of a job (and the Chrome JSON when SOTC_TRACE_JSON=1).
void WriteDump(const Job& job, uint64_t win_start, uint64_t win_end) {
    State& s = S();
    bool overwritten = false;
    std::vector<Window> outs = CopyWindow(RingList(), win_start, win_end, overwritten);
    if (overwritten) {
        s.windows_overwritten.fetch_add(1, std::memory_order_relaxed);
    }
    size_t total = 0;
    for (Window& o : outs) {
        total += o.events.size();
        RefreshName(o.ring);
    }

    std::string stem = SessionDir();
    if (job.manual) {
        char name[32];
        std::snprintf(name, sizeof(name), "/manual%03u", job.manual_index);
        stem += name;
    } else {
        char name[96];
        std::snprintf(name, sizeof(name), "/frame%06llu_%llums",
                      static_cast<unsigned long long>(job.frame),
                      static_cast<unsigned long long>(job.worst_ns / 1'000'000));
        stem += name;
    }
    const std::string path = stem + ".sotctrace";

    // The EVENTS payloads come first: they assign the name ids the NAMES chunk lists.
    NameTable names;
    std::vector<std::string> events;
    for (const Window& o : outs) {
        events.push_back(EventsPayload(o.ring->id, o.events.data(), o.events.size(), names));
    }
    std::string threads = "[";
    for (size_t i = 0; i < outs.size(); ++i) {
        threads += (i ? "," : "") + ThreadJson(outs[i].ring);
    }
    threads += ']';

    char worst[32];
    std::snprintf(worst, sizeof(worst), "%.1f", job.worst_ns / 1e6);
    const std::string meta =
        "{\"version\":1,\"mode\":1,\"kind\":\"dump\",\"reason\":" + JsonQuoted(job.reason) +
        ",\"frame\":" + std::to_string(job.frame) + ",\"worst_ms\":" + worst +
        ",\"slow_frames\":" + std::to_string(job.slow_frames) + ",\"window_ns\":[" +
        std::to_string(win_start) + "," + std::to_string(win_end) + "]," + MetaSettings() +
        ",\"stats\":" + StatsObject(Snapshot()) + "}";

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "SOTC trace: cannot write %s\n", path.c_str());
        return;
    }
    uint64_t bytes = std::fwrite(kMagic, 1, sizeof(kMagic), f);
    bytes += WriteChunk(f, kChunkMeta, meta);
    bytes += WriteChunk(f, kChunkThreads, threads);
    if (!names.pending.empty()) {
        bytes += WriteChunk(f, kChunkNames, NamesChunk(names));
    }
    for (const std::string& payload : events) {
        bytes += WriteChunk(f, kChunkEvents, payload);
    }
    bytes += WriteChunk(f, kChunkZoneStats, ZoneStatsJson());
    bytes += WriteProviders(f);
    bytes += WriteChunk(f, kChunkEnd, {});
    std::fclose(f);
    s.bytes_written.fetch_add(bytes, std::memory_order_relaxed);

    if (kJsonDumps) {
        WriteChromeJson(stem + ".json", job, win_start, win_end, outs);
    }

    if (job.manual) {
        std::fprintf(stderr, "SOTC trace: manual %u (%zu events) -> %s\n", job.manual_index, total,
                     path.c_str());
    } else {
        std::fprintf(stderr, "SOTC trace: frame %llu (%.0f ms, %u slow) -> %s (%zu events)\n",
                     static_cast<unsigned long long>(job.frame), job.worst_ns / 1e6,
                     job.slow_frames, path.c_str(), total);
    }
    std::scoped_lock lk{s.last_mutex};
    s.last.serial++;
    s.last.path = path;
    s.last.frame = job.frame;
    s.last.worst_ms = job.worst_ns / 1e6;
    s.last.reason = job.reason;
}

// Mode 2: the session stream (writer thread only).
struct Stream {
    std::FILE* f = nullptr;
    bool failed = false;
    NameTable names;
    std::unordered_map<uint32_t, std::string> thread_sent; // ring id -> last THREADS entry
};

void OpenStream(Stream& st) {
    const std::string path = SessionDir() + "/session.sotctrace";
    st.f = std::fopen(path.c_str(), "wb");
    if (!st.f) {
        st.failed = true;
        std::fprintf(stderr, "SOTC trace: cannot write %s\n", path.c_str());
        return;
    }
    std::fwrite(kMagic, 1, sizeof(kMagic), st.f);
    const std::string meta = "{\"version\":1,\"mode\":2,\"kind\":\"stream\"," + MetaSettings() +
                             ",\"stats\":" + StatsObject(Snapshot()) + "}";
    WriteChunk(st.f, kChunkMeta, meta);
    std::fflush(st.f);
    std::fprintf(stderr, "SOTC trace: streaming to %s\n", path.c_str());
}

// Mode 2: writes what each ring got since the last drain (its cursor up to its head), with the
// THREADS entries that changed and the names that are new. Events the producers overwrote
// before they were read count as dropped.
void Drain(Stream& st) {
    State& s = S();
    if (!st.f && !st.failed) {
        OpenStream(st);
    }
    std::vector<std::pair<uint32_t, std::string>> payloads; // ring id, EVENTS payload
    std::string threads;
    uint64_t lost = 0;
    for (Ring* r : RingList()) {
        const uint64_t h = r->head.load(std::memory_order_acquire);
        const uint64_t cap = r->Capacity();
        uint64_t cur = r->cursor;
        if (h - cur > cap) {
            lost += h - cur - cap; // the producer lapped the cursor: those are gone
            cur = h - cap;
        }
        if (h == cur) {
            continue;
        }
        std::vector<Event> copy;
        copy.reserve(h - cur);
        for (uint64_t i = cur; i < h; ++i) {
            copy.push_back(r->ev[i & r->mask]);
        }
        r->cursor = h;
        // Slots the producer overwrote during the copy: index below h2 + 1 - cap.
        const uint64_t h2 = r->head.load(std::memory_order_acquire);
        const uint64_t valid_from = h2 + 1 > cap ? h2 + 1 - cap : 0;
        const size_t skip = valid_from > cur ? std::min<size_t>(valid_from - cur, copy.size()) : 0;
        lost += skip;
        if (!st.f) {
            continue;
        }
        const auto sent = st.thread_sent.find(r->id);
        if (sent == st.thread_sent.end()) {
            RefreshName(r);
        }
        std::string json = ThreadJson(r);
        if (sent == st.thread_sent.end() || sent->second != json) {
            threads += (threads.empty() ? "" : ",") + json;
            st.thread_sent[r->id] = json;
        }
        if (copy.size() > skip) {
            payloads.emplace_back(r->id, EventsPayload(r->id, copy.data() + skip,
                                                       copy.size() - skip, st.names));
        }
    }
    s.dropped.fetch_add(lost, std::memory_order_relaxed);
    if (!st.f) {
        return;
    }
    uint64_t bytes = 0;
    if (!st.names.pending.empty()) {
        bytes += WriteChunk(st.f, kChunkNames, NamesChunk(st.names));
    }
    if (!threads.empty()) {
        bytes += WriteChunk(st.f, kChunkThreads, "[" + threads + "]");
    }
    for (const auto& [id, payload] : payloads) {
        (void)id;
        bytes += WriteChunk(st.f, kChunkEvents, payload);
    }
    std::fflush(st.f);
    s.stream_bytes.fetch_add(bytes, std::memory_order_relaxed);
    s.bytes_written.fetch_add(bytes, std::memory_order_relaxed);
}

// Every 5 s: the rates, the self-stats line, and (mode 2) META and ZONE_STATS chunks.
struct Rate {
    Clock::time_point at = Clock::now();
    uint64_t events = 0, bytes = 0;
};

void Tick(Stream& st, Rate& prev) {
    State& s = S();
    const StatsSnapshot snap = Snapshot();
    const uint64_t bytes = s.bytes_written.load(std::memory_order_relaxed);
    const auto now = Clock::now();
    const double dt = std::max(1e-3, std::chrono::duration<double>(now - prev.at).count());
    const uint64_t eps = static_cast<uint64_t>((snap.events - prev.events) / dt);
    const uint64_t bps = static_cast<uint64_t>((bytes - prev.bytes) / dt);
    s.events_per_s.store(eps, std::memory_order_relaxed);
    s.bytes_per_s.store(bps, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "SOTC TRACE: mode %d, %llu events/s, %llu KB/s written, %llu dropped, %llu windows "
                 "overwritten, %llu dumps\n",
                 kMode, static_cast<unsigned long long>(eps),
                 static_cast<unsigned long long>(bps / 1024),
                 static_cast<unsigned long long>(snap.dropped),
                 static_cast<unsigned long long>(snap.windows),
                 static_cast<unsigned long long>(snap.dumps));
    for (Ring* r : RingList()) {
        RefreshName(r); // a thread may have named itself since its first span
    }
    if (st.f) {
        const std::string meta = "{\"version\":1,\"mode\":2,\"kind\":\"stream\"," +
                                 MetaSettings() + ",\"stats\":" + StatsObject(Snapshot()) + "}";
        uint64_t written = WriteChunk(st.f, kChunkMeta, meta);
        written += WriteChunk(st.f, kChunkZoneStats, ZoneStatsJson());
        written += WriteProviders(st.f);
        std::fflush(st.f);
        s.stream_bytes.fetch_add(written, std::memory_order_relaxed);
        s.bytes_written.fetch_add(written, std::memory_order_relaxed);
    }
    prev = Rate{now, snap.events, bytes};
}

// The writer thread: runs from the first ring on, does every write. Waits up to 50 ms for a
// flush or a job, then drains (mode 2), prints the stats (every 5 s) and dumps due jobs.
void WriterThread() {
#ifdef _WIN32
    SetThreadDescription(GetCurrentThread(), L"sotc:TraceWriter");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    State& s = S();
    Stream st;
    Rate rate;
    Clock::time_point last_drain = Clock::now();
    for (;;) {
        Job job;
        bool have = false;
        bool stopping = false;
        {
            std::unique_lock lk{s.job_mutex};
            s.job_cv.wait_for(lk, std::chrono::milliseconds(50),
                              [&] { return s.stop.load() || s.flush_now.load(); });
            stopping = s.stop.load();
            // A slow frame's job waits out the post window: a later slow frame inside it joins.
            if (!s.jobs.empty()) {
                const Job& front = s.jobs.front();
                if (stopping || front.manual || Now() >= front.end + s.post_ns) {
                    job = std::move(s.jobs.front());
                    s.jobs.pop_front();
                    have = true;
                }
            }
        }
        if (have) {
            const uint64_t win_start = job.start > s.pre_ns ? job.start - s.pre_ns : 0;
            const uint64_t win_end = job.manual ? job.end : job.end + s.post_ns;
            WriteDump(job, win_start, win_end);
        }
        const auto now = Clock::now();
        if (kMode == 2 && (s.flush_now.exchange(false) ||
                           now - last_drain >= std::chrono::milliseconds(50))) {
            Drain(st);
            last_drain = now;
        }
        if (now - rate.at >= std::chrono::seconds(5)) {
            Tick(st, rate);
        }
        if (stopping) {
            std::scoped_lock lk{s.job_mutex};
            if (s.jobs.empty()) {
                break;
            }
        }
    }
    if (st.f) {
        Drain(st);
        WriteChunk(st.f, kChunkZoneStats, ZoneStatsJson());
        WriteProviders(st.f);
        WriteChunk(st.f, kChunkEnd, {});
        std::fclose(st.f);
        st.f = nullptr;
    }
    {
        std::scoped_lock lk{s.done_mutex};
        s.writer_done = true;
    }
    s.done_cv.notify_all();
}

// Exit handler: stops the writer, which writes the pending jobs and the stream's END chunk.
void AtExit() {
    State& s = S();
    if (!s.writer_started.load()) {
        return;
    }
    s.stop = true;
    s.job_cv.notify_all();
    std::unique_lock lk{s.done_mutex};
    s.done_cv.wait_for(lk, std::chrono::seconds(3), [&] { return s.writer_done; });
}

// Starts the writer thread once, at the first ring (any mode) and never with the recorder off.
void EnsureWriter() {
    if (!kOn) {
        return;
    }
    State& s = S();
    if (s.writer_started.exchange(true)) {
        return;
    }
    std::atexit(AtExit);
    std::thread{WriterThread}.detach();
}

// Extra tracks and chunk providers. Each C provider takes a slot; its adapter is a fixed function
// (template index), since a ChunkProvider cannot carry the C function pointer.
template <size_t N>
std::string CProviderAdapter() {
    const State::CSlot slot = S().c_slots[N];
    if (!slot.fn) {
        return {};
    }
    size_t len = 0;
    const char* p = slot.fn(&len);
    return (p && len) ? std::string(p, len) : std::string();
}

template <size_t... I>
std::array<ChunkProvider, sizeof...(I)> MakeCAdapters(std::index_sequence<I...>) {
    return {&CProviderAdapter<I>...};
}

const std::array<ChunkProvider, 16> kCAdapters = MakeCAdapters(std::make_index_sequence<16>{});

// bbgpu_trace_register_chunk: stores the C provider and publishes its adapter.
void RegisterCChunk(uint32_t type, const char* (*provider)(size_t*)) {
    if (!provider) {
        return;
    }
    State& s = S();
    std::scoped_lock lk{s.providers_mutex};
    if (s.c_count >= 16 || s.providers.size() >= 16) {
        return;
    }
    s.c_slots[s.c_count] = State::CSlot{type, provider};
    s.providers.emplace_back(type, kCAdapters[s.c_count]);
    ++s.c_count;
}

} // namespace

uint64_t Now() {
    return SotcStats::NowNs();
}

void Span(const char* name, uint64_t t0, uint64_t t1, uint64_t a, uint64_t b) {
    if (!kOn) {
        return;
    }
    ThisRing()->Push(MakeEvent(EvSpan, t0, t1 < t0 ? t0 : t1, name, a, b));
}

void SpanIfReady(const char* name, uint64_t t0, uint64_t t1, uint64_t a, uint64_t b) {
    if (kOn && tl_ring) {
        tl_ring->Push(MakeEvent(EvSpan, t0, t1 < t0 ? t0 : t1, name, a, b));
    }
}

void ThreadInit() {
    if (kOn) {
        ThisRing();
    }
}

void Instant(const char* name, uint64_t a, uint64_t b) {
    if (!kOn) {
        return;
    }
    const uint64_t now = Now();
    ThisRing()->Push(MakeEvent(EvInstant, now, now, name, a, b));
}

void Counter(const char* name, uint64_t value) {
    if (!kOn) {
        return;
    }
    const uint64_t now = Now();
    ThisRing()->Push(MakeEvent(EvCounter, now, now, name, value));
}

void Mark(const char* text) {
    if (!kOn || !text) {
        return;
    }
    const char* name = InternString(text, std::strlen(text));
    const uint64_t now = Now();
    PushFrames(MakeEvent(EvMark, now, now, name));
}

void SetSegment(const char* segment, uint32_t lap) {
    if (!kOn) {
        return;
    }
    const char* name =
        (segment && *segment) ? InternString(segment, std::strlen(segment)) : nullptr;
    S().lap.store(lap, std::memory_order_relaxed);
    S().segment.store(name, std::memory_order_release);
}

void SetImage(uint64_t base, uint64_t size) {
    State& s = S();
    s.image_base.store(base, std::memory_order_relaxed);
    s.image_size.store(size, std::memory_order_relaxed);
}

uint64_t ImageBase() {
    return S().image_base.load(std::memory_order_relaxed);
}

uint64_t ImageSize() {
    return S().image_size.load(std::memory_order_relaxed);
}

void SetUserDir(const char* user_dir) {
    if (user_dir && *user_dir) {
        S().user_dir = user_dir;
    }
}

void FrameEnd(uint64_t frame, uint64_t start_ns, uint64_t end_ns) {
    if (!kOn) {
        return;
    }
    State& s = S();
    const uint64_t dur = end_ns - start_ns;
    const uint64_t thr = s.threshold_ns.load(std::memory_order_relaxed);
    const bool slow = thr != 0 && dur >= thr;
    const char* seg = s.segment.load(std::memory_order_acquire);
    const uint32_t lap = s.lap.load(std::memory_order_relaxed);
    s.frame_now.store(frame, std::memory_order_relaxed);
    PushFrames(MakeEvent(EvFrame, start_ns, end_ns, seg ? seg : "frame", frame, lap, 0,
                         slow ? 1 : 0));
    // Mode 2 streams every frame; only mode 1 queues dumps.
    if (!slow || kMode != 1) {
        return;
    }
    std::scoped_lock lk{s.job_mutex};
    // A slow frame inside the pending job's window joins it (one file for both), up to a second
    // of slow frames: in a long stall the file is still written while the rings hold its start.
    if (!s.jobs.empty() && !s.jobs.back().manual &&
        start_ns <= s.jobs.back().end + s.post_ns &&
        end_ns - s.jobs.back().start <= 1'000'000'000ULL) {
        Job& last = s.jobs.back();
        last.end = end_ns;
        ++last.slow_frames;
        if (dur > last.worst_ns) {
            last.worst_ns = dur;
            last.frame = frame;
        }
    } else {
        if (s.dumps.load(std::memory_order_relaxed) >= s.max_dumps) {
            return;
        }
        s.dumps.fetch_add(1, std::memory_order_relaxed);
        Job job;
        job.frame = frame;
        job.start = start_ns;
        job.end = end_ns;
        job.worst_ns = dur;
        job.slow_frames = 1;
        job.reason = "slow";
        s.jobs.push_back(std::move(job));
    }
    s.job_cv.notify_one();
}

void FlipMark(uint64_t flip) {
    if (!kOn) {
        return;
    }
    const uint64_t now = Now();
    PushFrames(MakeEvent(EvInstant, now, now, "flip", flip));
}

bool RequestDump(const char* reason) {
    if (!kOn) {
        return false;
    }
    State& s = S();
    const std::string text = reason ? reason : "";
    {
        std::scoped_lock lk{s.job_mutex};
        if (s.dumps.load(std::memory_order_relaxed) >= s.max_dumps) {
            return false;
        }
        s.dumps.fetch_add(1, std::memory_order_relaxed);
    }
    if (kMode == 2) {
        // The stream gets a mark, and the writer drains the rings now.
        Mark(("dump: " + text).c_str());
        s.flush_now.store(true);
        s.job_cv.notify_one();
        return true;
    }
    const uint64_t now = Now();
    Job job;
    job.frame = s.frame_now.load(std::memory_order_relaxed);
    job.start = now;
    job.end = now;
    job.manual = true;
    job.manual_index = s.manual_serial.fetch_add(1, std::memory_order_relaxed) + 1;
    job.reason = "manual: " + text;
    {
        std::scoped_lock lk{s.job_mutex};
        s.jobs.push_back(std::move(job));
    }
    s.job_cv.notify_one();
    return true;
}

void SetThresholdMs(uint64_t ms) {
    S().threshold_ns.store(ms * 1'000'000ULL, std::memory_order_relaxed);
}

uint64_t ThresholdMs() {
    return S().threshold_ns.load(std::memory_order_relaxed) / 1'000'000ULL;
}

const char* InternString(const char* text, size_t len) {
    State& s = S();
    std::scoped_lock lk{s.intern_mutex};
    return s.interned.emplace(text ? std::string(text, len) : std::string()).first->c_str();
}

void Message(const char* text, size_t len) {
    if (!kOn) {
        return;
    }
    const char* name = InternString(text, len);
    const uint64_t now = Now();
    ThisRing()->Push(MakeEvent(EvInstant, now, now, name));
}

DumpInfo LastDump() {
    State& s = S();
    std::scoped_lock lk{s.last_mutex};
    return s.last;
}

std::string StatsJson() {
    return StatsObject(Snapshot());
}

void RegisterZoneSite(ZoneSite* site) {
    if (!site) {
        return;
    }
    bool expected = false;
    if (!site->registered.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    State& s = S();
    std::scoped_lock lk{s.zone_mutex};
    s.zones.push_back(site);
}

void FiberEnter(const char* name) {
    if (!kOn) {
        return;
    }
    tl_fiber_name = name;
    tl_fiber_t0 = Now();
}

void FiberExit() {
    if (!kOn || !tl_fiber_name) {
        return;
    }
    Span(tl_fiber_name, tl_fiber_t0, Now());
    tl_fiber_name = nullptr;
}

uint32_t RegisterQueue(const char* name) {
    if (!kOn) {
        return 0;
    }
    State& s = S();
    auto* q = new Queue;
    q->ring = NewRing(uint64_t{1} << 14, KindQueue, name, 0);
    std::scoped_lock lk{s.rings_mutex};
    s.queues.push_back(q);
    return static_cast<uint32_t>(s.queues.size() - 1);
}

void Submitted(uint32_t queue, uint64_t tick, uint64_t now_ns) {
    if (!kOn) {
        return;
    }
    State& s = S();
    Queue* q;
    {
        std::scoped_lock lk{s.rings_mutex};
        if (queue >= s.queues.size()) {
            return;
        }
        q = s.queues[queue];
    }
    q->submit_ns[tick & 4095].store(now_ns, std::memory_order_relaxed);
}

void Completed(uint32_t queue, uint64_t first_tick, uint64_t last_tick, uint64_t now_ns) {
    if (!kOn) {
        return;
    }
    State& s = S();
    Queue* q;
    {
        std::scoped_lock lk{s.rings_mutex};
        if (queue >= s.queues.size()) {
            return;
        }
        q = s.queues[queue];
    }
    // Ticks seen complete together are one span (a = first tick, b = last): the watcher cannot
    // tell where one ended and the next began. The submit time is stored before the submit, so
    // it is never missing for a tick that completed.
    const uint64_t submitted = q->submit_ns[first_tick & 4095].load(std::memory_order_relaxed);
    const uint64_t start = std::max(submitted, q->last_done_ns);
    q->ring->Push(MakeEvent(EvSpan, start ? start : now_ns, now_ns, "gpu", first_tick, last_tick));
    q->last_done_ns = now_ns;
}

void OpBatch::Flush() {
    if (count) {
        Span(count == 1 ? "op" : "ops", start, end, count);
        count = 0;
    }
}

void OpBatch::Note(const char* name, uint64_t t0, uint64_t t1, uint64_t a) {
    if (t1 - t0 >= kLongNs) {
        Flush();
        Span(name, t0, t1, a);
        return;
    }
    if (count && t0 > end + kGapNs) {
        Flush();
    }
    if (!count) {
        start = t0;
    }
    end = t1;
    if (++count >= 512) {
        Flush();
    }
}

void PushEvent(uint8_t kind, const char* name, uint64_t t0, uint64_t t1, uint64_t a, uint64_t b,
               uint32_t aux, uint16_t flags) {
    if (!kOn) {
        return;
    }
    ThisRing()->Push(MakeEvent(kind, t0, t1 < t0 ? t0 : t1, name, a, b, aux, flags));
}

uint32_t NewTrack(const char* name, const char* kind, uint32_t os_tid) {
    if (!kOn) {
        return 0;
    }
    Ring* r = NewRing(uint64_t{1} << 17, KindExtra, name ? name : "track", os_tid,
                      kind ? kind : "extra");
    return r->id < kMaxSlots ? r->id : 0;
}

void PushEventOn(uint32_t track, uint8_t kind, const char* name, uint64_t t0, uint64_t t1,
                 uint64_t a, uint64_t b, uint32_t aux, uint16_t flags) {
    if (!kOn || track == 0 || track >= kMaxSlots) {
        return;
    }
    // One load: the slot was filled before the id was handed out. The caller is the track's only
    // writer.
    Ring* r = S().slots[track].load(std::memory_order_acquire);
    if (r) {
        r->Push(MakeEvent(kind, t0, t1 < t0 ? t0 : t1, name, a, b, aux, flags));
    }
}

void RegisterChunkProvider(uint32_t chunk_type, ChunkProvider provider) {
    if (!provider) {
        return;
    }
    State& s = S();
    std::scoped_lock lk{s.providers_mutex};
    if (s.providers.size() < 16) {
        s.providers.emplace_back(chunk_type, provider);
    }
}

std::vector<ThreadInfo> Threads() {
    State& s = S();
    std::vector<ThreadInfo> out;
    std::scoped_lock lk{s.rings_mutex};
    for (const Ring* r : s.rings) {
        if (r->kind == KindFrames || r->kind == KindQueue || r->kind == KindExtra) {
            continue;
        }
        const ThreadView v = ViewOf(r);
        out.push_back(ThreadInfo{r->id, r->os_tid, v.name, KindName(v.kind)});
    }
    return out;
}

} // namespace SotcTrace

// C interface for the loader (src/*.c binds these weakly; see src/trace.h).
extern "C" uint64_t bbgpu_trace_now(void) {
    return SotcTrace::kOn ? SotcTrace::Now() : 0;
}

extern "C" void bbgpu_trace_span(const char* name, uint64_t t0, uint64_t a, uint64_t b) {
    if (SotcTrace::kOn && t0) {
        SotcTrace::Span(name, t0, SotcTrace::Now(), a, b);
    }
}

extern "C" void bbgpu_trace_image(uint64_t base, uint64_t size) {
    SotcTrace::SetImage(base, size);
}

extern "C" void bbgpu_trace_thread_init(void) {
    SotcTrace::ThreadInit();
}

extern "C" void bbgpu_trace_mark(const char* text) {
    SotcTrace::Mark(text);
}

extern "C" void bbgpu_trace_push(uint8_t kind, const char* name, uint64_t t0, uint64_t t1,
                                 uint64_t a, uint64_t b, uint32_t aux) {
    SotcTrace::PushEvent(kind, name, t0, t1, a, b, aux, 0);
}

extern "C" void bbgpu_trace_register_chunk(uint32_t type, const char* (*provider)(size_t* len)) {
    SotcTrace::RegisterCChunk(type, provider);
}
