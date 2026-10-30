// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_TRACE flight recorder (see video_core/sotc_trace.h).

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif
#include <pthread.h>

#include "video_core/sotc_stats.h"
#include "video_core/sotc_trace.h"

namespace SotcTrace {

namespace {

struct Event {
    uint64_t t0, t1;
    const char* name;
    uint64_t a, b;
};

// Track order in the viewer: game frames, guest threads, then the GPU side.
enum Kind : uint8_t { KindFrames, KindGuest, KindCp, KindDraw, KindRecorder, KindGpuOther, KindQueue };

struct Ring {
    std::unique_ptr<Event[]> ev;
    uint64_t mask = 0;
    std::atomic<uint64_t> head{0}; // events written so far; slot = index & mask
    std::string name;
    uint32_t os_tid = 0;
    Kind kind = KindGuest;
    uint32_t id = 0;

    Ring(uint64_t size, Kind k) : ev{new Event[size]}, mask{size - 1}, kind{k} {}
    void Push(const Event& e) {
        const uint64_t h = head.load(std::memory_order_relaxed);
        ev[h & mask] = e;
        head.store(h + 1, std::memory_order_release);
    }
};

struct Queue {
    Ring* ring = nullptr;
    std::unique_ptr<std::atomic<uint64_t>[]> submit_ns{new std::atomic<uint64_t>[4096]{}};
    uint64_t last_done_ns = 0;
};

struct Job {
    uint64_t frame = 0;
    uint64_t start = 0, end = 0; // the slow frame(s): first start, last end
    uint64_t worst_ns = 0;
    uint32_t slow_frames = 0;
};

struct State {
    std::mutex rings_mutex;
    std::vector<Ring*> rings; // never freed: a dump may read a ring after its thread exited
    std::vector<Queue*> queues;
    Ring* frames = nullptr;

    std::mutex job_mutex;
    std::condition_variable job_cv;
    std::deque<Job> jobs;
    std::thread writer;
    bool writer_started = false;

    uint64_t image_base = 0, image_size = 0;
    std::string user_dir = "user";
    std::string session_dir; // created at the first dump
    uint32_t dumps = 0;

    uint64_t threshold_ns = 60'000'000, pre_ns = 150'000'000, post_ns = 40'000'000;
    uint32_t max_dumps = 40;
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

Ring* NewRing(uint64_t size, Kind kind, std::string name, uint32_t os_tid) {
    State& s = S();
    auto* r = new Ring(size, kind);
    r->name = std::move(name);
    r->os_tid = os_tid;
    std::scoped_lock lk{s.rings_mutex};
    r->id = static_cast<uint32_t>(s.rings.size()) + 1;
    s.rings.push_back(r);
    return r;
}

thread_local Ring* tl_ring = nullptr;

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

void JsonString(std::FILE* f, const std::string& text) {
    std::fputc('"', f);
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            std::fputc('\\', f);
            std::fputc(c, f);
        } else if (static_cast<unsigned char>(c) < 0x20) {
            std::fprintf(f, "\\u%04x", c);
        } else {
            std::fputc(c, f);
        }
    }
    std::fputc('"', f);
}

// Small values (counts, ticks, queue numbers) in decimal; b, which carries the guest return
// address or faulting instruction, as an eboot offset when it falls in the game image.
void Arg(std::FILE* f, const char* key, uint64_t v, bool code) {
    const State& s = S();
    if (v < 1'000'000) {
        std::fprintf(f, "\"%s\":%llu", key, static_cast<unsigned long long>(v));
    } else if (code && s.image_size && v >= s.image_base && v < s.image_base + s.image_size) {
        std::fprintf(f, "\"%s\":\"eboot+0x%llx\"", key,
                     static_cast<unsigned long long>(v - s.image_base));
    } else {
        std::fprintf(f, "\"%s\":\"0x%llx\"", key, static_cast<unsigned long long>(v));
    }
}

#ifdef _WIN32
// A thread may name itself after its first span (or never call it on this ring's creation).
void RefreshName(Ring* r) {
    if (r->os_tid == 0 || r->kind == KindQueue || r->kind == KindFrames) {
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
            r->name = std::move(name);
        }
        LocalFree(wide);
    }
    CloseHandle(h);
}
#endif

void WriteDump(const Job& job, uint64_t win_start, uint64_t win_end) {
    State& s = S();
    std::vector<Ring*> rings;
    {
        std::scoped_lock lk{s.rings_mutex};
        rings = s.rings;
    }
    // Copy each ring's events of the window. A ring's events are in end-time order (each span is
    // pushed when it ends), so the walk back stops at the first one that ended before the window.
    struct Out {
        Ring* ring;
        std::vector<Event> events;
    };
    std::vector<Out> outs;
    size_t total = 0;
    for (Ring* r : rings) {
        const uint64_t h1 = r->head.load(std::memory_order_acquire);
        const uint64_t cap = r->mask + 1;
        const uint64_t lowest = h1 > cap ? h1 - cap : 0;
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
        Out out{r, {}};
        for (size_t k = copied.size(); k-- > 0;) {
            if (index[k] >= valid_from) {
                out.events.push_back(copied[k]);
            }
        }
        if (!out.events.empty() || r->kind == KindFrames) {
            total += out.events.size();
            outs.push_back(std::move(out));
        }
    }
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
    char file_name[96];
    std::snprintf(file_name, sizeof(file_name), "/frame%06llu_%llums.json",
                  static_cast<unsigned long long>(job.frame),
                  static_cast<unsigned long long>(job.worst_ns / 1'000'000));
    const std::string path = s.session_dir + file_name;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "SOTC trace: cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(f, "{\"displayTimeUnit\":\"ms\",\"otherData\":{\"frame\":%llu,\"worst_ms\":%.1f,"
                    "\"slow_frames\":%u,\"window_ms\":%.1f},\"traceEvents\":[\n",
                 static_cast<unsigned long long>(job.frame), job.worst_ns / 1e6, job.slow_frames,
                 (win_end - win_start) / 1e6);
    std::fprintf(f, "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,\"args\":{\"name\":"
                    "\"Shadow of the Colossus (sotc-pc)\"}}");
    uint32_t guest_order = 0;
    for (Out& out : outs) {
        Ring* r = out.ring;
#ifdef _WIN32
        RefreshName(r);
#endif
        std::string name = r->name;
        if (name.empty()) {
            name = "thread " + std::to_string(r->os_tid);
        }
        // GPU-side helper threads without a role (shadPS4 names them) are not game threads.
        Kind kind = r->kind;
        if (kind == KindGuest && name.rfind("shadPS4", 0) == 0) {
            kind = KindGpuOther;
        }
        if (kind == KindGuest) {
            name = "game: " + name;
        }
        const uint32_t sort = kind * 1000 + (kind == KindGuest ? guest_order++ : r->id);
        std::fprintf(f, ",\n{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":%u,\"args\":"
                        "{\"name\":",
                     r->id);
        JsonString(f, name);
        std::fprintf(f, "}},\n{\"name\":\"thread_sort_index\",\"ph\":\"M\",\"pid\":1,\"tid\":%u,"
                        "\"args\":{\"sort_index\":%u}}",
                     r->id, sort);
        for (const Event& e : out.events) {
            const uint64_t t0 = std::max(e.t0, win_start);
            const double ts = (t0 - win_start) / 1e3;
            if (e.t1 == e.t0) {
                std::fprintf(f, ",\n{\"name\":\"%s\",\"ph\":\"i\",\"s\":\"t\",\"pid\":1,\"tid\":%u,"
                                "\"ts\":%.3f,\"args\":{",
                             e.name, r->id, ts);
            } else {
                std::fprintf(f, ",\n{\"name\":\"%s\",\"ph\":\"X\",\"pid\":1,\"tid\":%u,\"ts\":%.3f,"
                                "\"dur\":%.3f,\"args\":{",
                             e.name, r->id, ts, (e.t1 - t0) / 1e3);
            }
            Arg(f, "a", e.a, false);
            std::fputc(',', f);
            Arg(f, "b", e.b, true);
            std::fputs("}}", f);
        }
    }
    std::fputs("\n]}\n", f);
    std::fclose(f);
    std::fprintf(stderr, "SOTC trace: frame %llu (%.0f ms, %u slow) -> %s (%zu events)\n",
                 static_cast<unsigned long long>(job.frame), job.worst_ns / 1e6, job.slow_frames,
                 path.c_str(), total);
}

void WriterThread() {
    State& s = S();
    for (;;) {
        Job job;
        {
            std::unique_lock lk{s.job_mutex};
            s.job_cv.wait(lk, [&] { return !s.jobs.empty(); });
            // Spans that were still open at the slow frame's end are pushed when they close:
            // wait out the post window (a later slow frame inside it joins this job).
            for (;;) {
                const uint64_t due = s.jobs.front().end + s.post_ns;
                const uint64_t now = Now();
                if (now >= due) {
                    break;
                }
                s.job_cv.wait_for(lk, std::chrono::nanoseconds(due - now));
            }
            job = s.jobs.front();
            s.jobs.pop_front();
        }
        const uint64_t win_start = job.start > s.pre_ns ? job.start - s.pre_ns : 0;
        WriteDump(job, win_start, job.end + s.post_ns);
    }
}

} // namespace

uint64_t Now() {
    return SotcStats::NowNs();
}

void Span(const char* name, uint64_t t0, uint64_t t1, uint64_t a, uint64_t b) {
    if (!kOn) {
        return;
    }
    ThisRing()->Push(Event{t0, t1 < t0 ? t0 : t1, name, a, b});
}

void SpanIfReady(const char* name, uint64_t t0, uint64_t t1, uint64_t a, uint64_t b) {
    if (kOn && tl_ring) {
        tl_ring->Push(Event{t0, t1 < t0 ? t0 : t1, name, a, b});
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
    ThisRing()->Push(Event{now, now, name, a, b});
}

void SetImage(uint64_t base, uint64_t size) {
    State& s = S();
    s.image_base = base;
    s.image_size = size;
}

uint64_t ImageBase() {
    return S().image_base;
}

uint64_t ImageSize() {
    return S().image_size;
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
    if (!s.frames) {
        s.frames = NewRing(uint64_t{1} << 12, KindFrames, "game frames (flip to flip)", 0);
    }
    const uint64_t dur = end_ns - start_ns;
    const bool slow = dur >= s.threshold_ns;
    s.frames->Push(Event{start_ns, end_ns, slow ? "SLOW FRAME" : "frame", frame, 0});
    if (!slow) {
        return;
    }
    std::scoped_lock lk{s.job_mutex};
    // A slow frame inside the pending job's window joins it (one file for both), up to a second
    // of slow frames: in a long stall the file is still written while the rings hold its start.
    if (!s.jobs.empty() && start_ns <= s.jobs.back().end + s.post_ns &&
        end_ns - s.jobs.back().start <= 1'000'000'000ULL) {
        Job& last = s.jobs.back();
        last.end = end_ns;
        ++last.slow_frames;
        if (dur > last.worst_ns) {
            last.worst_ns = dur;
            last.frame = frame;
        }
    } else {
        if (s.dumps >= s.max_dumps) {
            return;
        }
        ++s.dumps;
        s.jobs.push_back(Job{frame, start_ns, end_ns, dur, 1});
    }
    if (!s.writer_started) {
        s.writer_started = true;
        s.writer = std::thread{WriterThread};
        s.writer.detach();
    }
    s.job_cv.notify_one();
}

uint32_t RegisterQueue(const char* name) {
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
    q->ring->Push(Event{start ? start : now_ns, now_ns, "gpu", first_tick, last_tick});
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
