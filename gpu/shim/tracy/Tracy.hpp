// sotc: the Tracy macros feed the SOTC_TRACE recorder (video_core/sotc_trace.h) instead of
// compiling to nothing. Each ZoneScoped* use gets a static SotcTrace::ZoneSite (named after the
// literal, or __func__ for the unnamed forms) and a SotcTrace::ZoneScope on it: calls and total
// time go to the zone stats, and a call of at least SOTC_TRACE_ZONE_MIN_US becomes a span. This
// is all off unless SOTC_TRACE_ZONES=1 (the scope reads no clock then). TracyMessage and TracyPlot
// are recorded only when SOTC_TRACE is on; their arguments are not evaluated otherwise.
// FrameMark is a no-op: frames are recorded by the presenter (SotcTrace::FrameEnd).
// ZoneText, ZoneName, TracyAllocN/TracyFreeN and the Vulkan and fiber macros stay no-ops. The
// tracy:: stubs at the end are referenced by the vendored code and must stay as they are.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "video_core/sotc_trace.h"

#define TracyFile __FILE__
#define TracyLine __LINE__
#define TracyFunction __func__

// Names unique per use: the line number is pasted in.
#define SOTC_ZONE_CAT_(a, b) a##b
#define SOTC_ZONE_CAT(a, b) SOTC_ZONE_CAT_(a, b)
#define SOTC_ZONE_(zone_name)                                                                      \
    static ::SotcTrace::ZoneSite SOTC_ZONE_CAT(sotc_zone_site_, __LINE__){zone_name, __FILE__,     \
                                                                          __LINE__};              \
    ::SotcTrace::ZoneScope SOTC_ZONE_CAT(sotc_zone_scope_, __LINE__){                             \
        &SOTC_ZONE_CAT(sotc_zone_site_, __LINE__)}

#define ZoneScoped SOTC_ZONE_(__func__)
#define ZoneScopedN(name) SOTC_ZONE_(name)
#define ZoneScopedC(color) SOTC_ZONE_(__func__)
#define ZoneScopedNC(name, color) SOTC_ZONE_(name)
#define ZoneText(text, size) ((void)(text), (void)(size))
#define ZoneName(text, size)
#define FrameMark
#define FrameMarkNamed(name)
#define TracyMessageC(text, size, color)                                                           \
    do {                                                                                           \
        if (::SotcTrace::kOn) {                                                                    \
            ::SotcTrace::Message((text), (size));                                                  \
        }                                                                                          \
    } while (0)
#define TracyMessageL(text)                                                                        \
    do {                                                                                           \
        if (::SotcTrace::kOn) {                                                                    \
            ::SotcTrace::Message((text), std::strlen((text)));                                     \
        }                                                                                          \
    } while (0)
#define TracyAllocN(ptr, size, name) ((void)(ptr), (void)(size))
#define TracyFreeN(ptr, name) ((void)(ptr))
#define TracyFiberEnter(name)
#define TracyFiberLeave
#define TracyPlot(name, value)                                                                     \
    do {                                                                                           \
        if (::SotcTrace::kOn) {                                                                    \
            ::SotcTrace::Counter((name), (uint64_t)(value));                                       \
        }                                                                                          \
    } while (0)
namespace tracy {
struct SourceLocationData { const char* name; const char* function; const char* file; uint32_t line; uint32_t color; };
struct Color { enum : uint32_t { Red = 0xff0000, HotPink = 0xff69b4, DarkOrange = 0xff8c00 }; };
struct LockableCtx { explicit LockableCtx(const SourceLocationData*) {} void BeforeLock() {} void AfterLock() {} void AfterUnlock() {} void AfterTryLock(bool) {} void Mark(const SourceLocationData*) {} void CustomName(const char*, size_t) {} };
struct Profiler { bool IsConnected() const { return false; } };
inline Profiler& GetProfiler() { static Profiler p; return p; }
} // namespace tracy
