// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cstdint>

namespace SotcFrameStats {

enum class Kind : std::size_t { Shader, Graphics, Compute, Readback, SubmitWait, CacheSnapshot, Count };
inline constexpr auto Count = static_cast<std::size_t>(Kind::Count);
inline std::array<std::atomic<std::uint64_t>, Count> elapsed{};
inline std::array<std::atomic<std::uint64_t>, Count> calls{};

inline bool Enabled() {
    static const bool value = [] {
        const char* flag = std::getenv("SOTC_FRAME_STATS");
        const char* csv = std::getenv("SOTC_FRAME_TIMES");
        return (flag && flag[0] == '1') || (csv && csv[0]);
    }();
    return value;
}

inline std::uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

class Scope {
public:
    explicit Scope(Kind kind_) : kind(kind_), start(Enabled() ? Now() : 0) {}
    ~Scope() {
        if (start) {
            const auto i = static_cast<std::size_t>(kind);
            elapsed[i].fetch_add(Now() - start, std::memory_order_relaxed);
            calls[i].fetch_add(1, std::memory_order_relaxed);
        }
    }
private:
    Kind kind;
    std::uint64_t start;
};

// Called only for successful presentation of a new game frame. Global worker totals
// are sampled here: overlapping operations are not additive GPU/frame costs.
class Reporter {
public:
    Reporter() {
        const char* flag = std::getenv("SOTC_FRAME_STATS");
        print_stalls = flag && flag[0] == '1';
        if (const char* path = std::getenv("SOTC_FRAME_TIMES"); path && path[0]) {
#ifdef _WIN32
            csv = _wfopen(std::filesystem::path(path).c_str(), L"wb");
#else
            csv = std::fopen(path, "wb");
#endif
            if (csv) {
                std::setvbuf(csv, nullptr, _IOFBF, 64 * 1024);
                std::fprintf(csv, "frame,time_ns,interval_ns,present_ns,shader_ns,graphics_ns,compute_ns,readback_ns,submit_wait_ns,cache_snapshot_ns,shader_calls,graphics_calls,compute_calls,readback_calls\n");
            } else {
                std::fprintf(stderr, "SOTC_FRAME_TIMES: could not open %s\n", path);
            }
        }
    }
    ~Reporter() { if (csv) std::fclose(csv); }
    void Present(std::uint64_t present_ns) {
        const auto now = Now();
        const auto interval = last ? now - last : 0;
        last = now;
        ++frame;
        std::array<std::uint64_t, Count> ns{}, n{};
        for (std::size_t i = 0; i < Count; ++i) {
            const auto total = elapsed[i].load(std::memory_order_relaxed);
            const auto count = calls[i].load(std::memory_order_relaxed);
            ns[i] = total - previous_ns[i];
            n[i] = count - previous_calls[i];
            previous_ns[i] = total;
            previous_calls[i] = count;
        }
        if (csv) {
            std::fprintf(csv, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                U(frame), U(now), U(interval), U(present_ns), U(ns[0]), U(ns[1]), U(ns[2]),
                U(ns[3]), U(ns[4]), U(ns[5]), U(n[0]), U(n[1]), U(n[2]), U(n[3]));
            if (frame % 60 == 0 || interval > 100000000) std::fflush(csv);
        }
        if (print_stalls && frame > 1 && interval > 40000000) {
            std::fprintf(stderr, "SOTCSTALL frame=%llu interval=%.2fms present=%.2fms shader=%.2fms graphics=%.2fms compute=%.2fms readback=%.2fms submit_wait=%.2fms cache_snapshot=%.2fms\n",
                U(frame), interval / 1e6, present_ns / 1e6, ns[0] / 1e6, ns[1] / 1e6,
                ns[2] / 1e6, ns[3] / 1e6, ns[4] / 1e6, ns[5] / 1e6);
        }
    }
private:
    static unsigned long long U(std::uint64_t value) { return value; }
    FILE* csv{};
    bool print_stalls{};
    std::uint64_t frame{}, last{};
    std::array<std::uint64_t, Count> previous_ns{}, previous_calls{};
};

inline void Presented(std::uint64_t present_ns) {
    if (Enabled()) {
        static Reporter reporter;
        reporter.Present(present_ns);
    }
}

} // namespace SotcFrameStats
