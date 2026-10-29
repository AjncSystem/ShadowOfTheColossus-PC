// bbport: replaces shadPS4's spdlog-based logger with a small fmt sink.
// Level from BB_GPU_LOG (trace|debug|info|warning|error; default warning).
#pragma once
#include <algorithm>
#include <atomic>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <fmt/ranges.h>
#include <fmt/std.h>
#include <string_view>
#include <fmt/format.h>
#include "common/logging/classes.h"
#include "common/path_util.h"
#include "common/thread.h"

// Level type keeps shadPS4's spdlog spelling so vendored callers compile unchanged.
namespace spdlog {
enum class level : int { trace, debug, info, warn, err, critical, off };
}
namespace Common::Log {
using Level = spdlog::level;
/// sotc: the BB_GPU_LOG threshold, -1 until first read. ShouldLog runs for every LOG_* that is
/// evaluated (per draw, per Gnm call): inline, one relaxed load instead of an out-of-line call
/// behind a function-local static guard (~1.5% of the draw thread). Constant-initialised, so a
/// log from another static initialiser still reads it correctly.
inline std::atomic<int> g_log_threshold{-1};
int ReadLogThreshold();
inline bool ShouldLog(Level level) {
    int threshold = g_log_threshold.load(std::memory_order_relaxed);
    if (threshold < 0) [[unlikely]] {
        threshold = ReadLogThreshold();
    }
    return static_cast<int>(level) >= threshold;
}
void Write(std::string_view log_class, Level level, const char* file, int line, const char* function,
           std::string&& message);
inline void Flush() {}
} // namespace Common::Log

#define LOG_GENERIC(log_class, log_level, fmt_str, ...)                                             \
    do {                                                                                           \
        if (Common::Log::ShouldLog(log_level)) {                                                   \
            Common::Log::Write(log_class, log_level, __FILE__, __LINE__, __func__,                 \
                               fmt::format(fmt_str __VA_OPT__(, ) __VA_ARGS__));                    \
        }                                                                                          \
    } while (false)
#define LOG_TRACE(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::trace, __VA_ARGS__)
#define LOG_DEBUG(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::debug, __VA_ARGS__)
#define LOG_INFO(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::info, __VA_ARGS__)
#define LOG_WARNING(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::warn, __VA_ARGS__)
#define LOG_ERROR(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::err, __VA_ARGS__)
#define LOG_CRITICAL(log_class, ...) LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::critical, __VA_ARGS__)
