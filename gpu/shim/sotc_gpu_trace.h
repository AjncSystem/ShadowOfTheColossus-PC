// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_TRACE_GPU=1 (needs SOTC_TRACE), trace layer L4: the real GPU execution time of the
// draw scheduler's command buffers. Every submit's primary command buffer gets a timestamp pair
// (TOP_OF_PIPE at its begin, BOTTOM_OF_PIPE at its end) from a query pool ring. The scheduler's
// trace watch thread reads a pair once the submit's timeline tick completed, without waiting,
// maps the GPU ticks to the steady clock (SotcTrace::Now) with calibration pairs taken about once a
// second, and pushes one EvGpu event per command buffer on the "GPU (draw queue)" track.
// The GPU_CALIB chunk (type 8, docs/sotc/TRACE-FORMAT.md) is registered by Timeline::Create.
//
// Off (the default): Timeline::Create returns null, the scheduler only tests a null pointer.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/sotc_trace.h"

namespace Vulkan {
class Instance;
}

namespace SotcGpuTrace {

/// SOTC_TRACE_GPU=1 or 2, with the recorder on. 2 is accepted but behaves as 1: the per-op-batch
/// stamps are not implemented.
inline const bool kOn = SotcTrace::kOn && [] {
    const char* v = std::getenv("SOTC_TRACE_GPU");
    return v && (v[0] == '1' || v[0] == '2');
}();

class Timeline {
public:
    static constexpr u32 kNoPair = ~u32{0};

    /// Null when the switch is off, the graphics queue family has no timestamps
    /// (timestampValidBits 0) or the query pool cannot be created. Also registers the GPU_CALIB
    /// chunk provider. Called on the thread that owns the draw scheduler, before its first session.
    static std::unique_ptr<Timeline> Create(const Vulkan::Instance& instance);

    ~Timeline();

    /// Records the reset and the begin stamp of a primary command buffer. Call first in the
    /// session, outside any render pass. cmd may be a recorder fake handle (the commands keep
    /// their order in the stream). Returns the pair index for Submitted and End.
    [[nodiscard]] u32 Begin(vk::CommandBuffer cmd);

    /// Records the end stamp of a pair. Call after the last command of the session, outside any
    /// render pass. kNoPair is ignored.
    void End(vk::CommandBuffer cmd, u32 pair);

    /// Called right before the queue submit that signals tick and carries the pair's command
    /// buffer, on the thread that submits (the recording thread when there is one). submit_ns is
    /// SotcTrace::Now() taken just before the submit.
    void Submitted(u32 pair, u64 tick, u64 submit_ns);

    /// Watch thread, every poll: calibrates when due (about once a second).
    void Poll(u64 now_ns);

    /// Watch thread: the ticks first..last are complete. Reads their pairs without waiting and
    /// pushes one EvGpu event per pair. A pair that is not available yet is counted and dropped.
    void Completed(u64 first, u64 last);

private:
    explicit Timeline(const Vulkan::Instance& instance);

    void Calibrate();
    void SetCalibration(u64 ticks, u64 cpu_ns);
    [[nodiscard]] u64 ToCpu(u64 ticks) const;

    struct Rec {
        std::atomic<u64> tick{0}; ///< tick this record belongs to; 0: free
        std::atomic<u32> pair{kNoPair};
        std::atomic<u64> submit_ns{0};
    };

    static constexpr u32 kPairs = 4096;   ///< query pairs (8192 queries) in the ring
    static constexpr u32 kRecords = 4096; ///< tick records, indexed by tick % kRecords

    const Vulkan::Instance& instance;
    VkDevice device{};
    vk::UniqueQueryPool pool;
    u32 track{};
    u32 valid_bits{};
    u64 mask{};
    double period_ns{};
    bool can_calibrate{};    ///< a calibrated time domain extension is enabled
    bool calibrated_khr{};   ///< VK_KHR_calibrated_timestamps enabled (else EXT)
    bool host_domain{};      ///< the QueryPerformanceCounter time domain is listed (Windows)
    u64 qpc_frequency{};     ///< QueryPerformanceFrequency (Windows)
    u32 pair_next{};         ///< producer: next pair to hand out
    std::unique_ptr<Rec[]> recs;

    // Watch thread only.
    bool have_map{};         ///< cal_ticks/cal_cpu hold a mapping (calibrated or anchored)
    bool calibrated{};       ///< the mapping comes from vkGetCalibratedTimestamps
    bool tried_calibration{};
    u64 last_calibration_ns{};
    u64 cal_ticks{};         ///< masked GPU ticks of the mapping
    u64 cal_cpu{};           ///< steady ns of cal_ticks
    bool have_qpc_offset{};  ///< steady - QueryPerformanceCounter ns, from the narrowest bracket
    s64 qpc_offset{};
    u64 qpc_bracket{};
};

} // namespace SotcGpuTrace
