// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: detailed texture-cache diagnostic log (SOTC_TEX_DIAG=<file>, the launcher's "Registro
// detalhado"): every image creation, drop, upload and the reason an upload was skipped, with the
// local clock so a line can be matched to a video. SOTC_TEX_DIAG_LEVEL=1 (default) logs images;
// 2 also logs every CPU write fault that touched an image. Stops at 400 MB.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>

namespace SotcTexDiag {

struct State {
    std::FILE* file{};
    int level{};
    std::mutex lock;
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    size_t written{};
    // Counters written once a second as a summary line.
    std::atomic<unsigned> created{}, freed{}, uploads{}, hash_skips{}, gpu_skips{}, faults{};
    long long last_summary_ms{};

    State() {
        const char* path = std::getenv("SOTC_TEX_DIAG");
        if (path && *path) {
            file = std::fopen(path, "w");
            const char* l = std::getenv("SOTC_TEX_DIAG_LEVEL");
            level = l && *l ? std::atoi(l) : 1;
        }
    }
};

inline State& Get() {
    static State state;
    return state;
}

inline int Level() {
    static const int level = Get().file ? Get().level : 0;
    return level;
}

inline void Log(const char* format, ...) {
    State& s = Get();
    if (!s.file) {
        return;
    }
    char message[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    const auto now = std::chrono::steady_clock::now();
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - s.start).count();
    std::time_t t = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    std::scoped_lock guard{s.lock};
    if (s.written > (400u << 20)) {
        return;
    }
    const int n = std::fprintf(s.file, "[%02d:%02d:%02d +%lld.%03llds] %s\n", local.tm_hour,
                               local.tm_min, local.tm_sec, ms / 1000, ms % 1000, message);
    if (n > 0) {
        s.written += size_t(n);
    }
    if (ms - s.last_summary_ms >= 1000) {
        s.last_summary_ms = ms;
        std::fprintf(s.file,
                     "  summary: created %u, freed %u, uploads %u, hash skips %u, gpu-hash skips %u, "
                     "cpu faults on images %u\n",
                     s.created.exchange(0), s.freed.exchange(0), s.uploads.exchange(0),
                     s.hash_skips.exchange(0), s.gpu_skips.exchange(0), s.faults.exchange(0));
        std::fflush(s.file);
    }
}

} // namespace SotcTexDiag
