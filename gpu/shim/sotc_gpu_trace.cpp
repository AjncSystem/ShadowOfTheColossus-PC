// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_TRACE_GPU=1, trace layer L4 (see sotc_gpu_trace.h, docs/sotc/TRACE-FORMAT.md).

#include "sotc_gpu_trace.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "video_core/renderer_vulkan/vk_instance.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace SotcGpuTrace {

namespace {

constexpr u64 kCalibrationEveryNs = 1'000'000'000ull;
constexpr std::size_t kMaxPairs = 64;

// Read by the writer thread (GPU_CALIB) and written by the watch thread. Never freed, so the
// provider stays valid until the process exits.
struct Shared {
    std::mutex mutex;
    bool active{};
    double period_ns{};
    u32 valid_bits{};
    bool calibrated{};
    std::vector<std::pair<u64, u64>> pairs; ///< (masked GPU ticks, steady ns), newest last
    std::atomic<u64> events{0};
    std::atomic<u64> not_ready{0};
    std::atomic<u64> lagged{0};
    std::atomic<u64> no_pair{0};
};

Shared& GetShared() {
    static Shared* shared = new Shared;
    return *shared;
}

// GPU_CALIB chunk (type 8): the JSON object of TRACE-FORMAT.md, plus the counters.
std::string Provide() {
    Shared& s = GetShared();
    const std::scoped_lock lk{s.mutex};
    if (!s.active) {
        return {};
    }
    char head[512];
    std::snprintf(head, sizeof head,
                  "{\"timestamp_period_ns\": %.6f, \"valid_bits\": %u, \"calibrated\": %s, "
                  "\"events\": %llu, \"not_ready\": %llu, \"lagged\": %llu, \"no_pair\": %llu, "
                  "\"pairs\": [",
                  s.period_ns, s.valid_bits, s.calibrated ? "true" : "false",
                  static_cast<unsigned long long>(s.events.load()),
                  static_cast<unsigned long long>(s.not_ready.load()),
                  static_cast<unsigned long long>(s.lagged.load()),
                  static_cast<unsigned long long>(s.no_pair.load()));
    std::string out = head;
    bool first = true;
    for (const auto& [ticks, cpu] : s.pairs) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s[%llu, %llu]", first ? "" : ", ",
                      static_cast<unsigned long long>(ticks),
                      static_cast<unsigned long long>(cpu));
        out += buf;
        first = false;
    }
    out += "]}";
    return out;
}

u64 QpcFrequency() {
#if defined(_WIN32)
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    return static_cast<u64>(freq.QuadPart);
#else
    return 0;
#endif
}

u64 QpcToNs(u64 counts, u64 freq) {
    if (freq == 0) {
        return 0;
    }
    return (counts / freq) * 1'000'000'000ull + (counts % freq) * 1'000'000'000ull / freq;
}

// vkGetCalibratedTimestamps of whichever name the device enabled (KHR, or EXT from Tracy).
VkResult CalibratedTimestamps(bool khr, VkDevice device, u32 count,
                              const VkCalibratedTimestampInfoKHR* infos, u64* stamps,
                              u64* max_deviation) {
    if (khr) {
        return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetCalibratedTimestampsKHR(device, count, infos,
                                                                          stamps, max_deviation);
    }
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetCalibratedTimestampsEXT(device, count, infos,
                                                                      stamps, max_deviation);
}

VkCalibratedTimestampInfoKHR DeviceDomain() {
    VkCalibratedTimestampInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
    info.timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
    return info;
}

} // namespace

std::unique_ptr<Timeline> Timeline::Create(const Vulkan::Instance& instance) {
    if (!kOn) {
        return nullptr;
    }
    const u32 bits = instance.GetTimestampValidBits();
    if (bits == 0) {
        std::fprintf(stderr, "SOTC TRACE GPU: the graphics queue has no timestamps "
                             "(timestampValidBits 0), SOTC_TRACE_GPU is off\n");
        return nullptr;
    }

    const vk::Device device = instance.GetDevice();
    const vk::QueryPoolCreateInfo pool_info{
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = 2 * kPairs,
    };
    auto [result, pool] = device.createQueryPoolUnique(pool_info);
    if (result != vk::Result::eSuccess) {
        std::fprintf(stderr, "SOTC TRACE GPU: query pool creation failed (%s), SOTC_TRACE_GPU "
                             "is off\n", vk::to_string(result).c_str());
        return nullptr;
    }

    std::unique_ptr<Timeline> timeline(new Timeline{instance});
    timeline->pool = std::move(pool);

    {
        Shared& s = GetShared();
        const std::scoped_lock lk{s.mutex};
        s.active = true;
        s.period_ns = timeline->period_ns;
        s.valid_bits = bits;
        s.calibrated = false;
    }
    SotcTrace::RegisterChunkProvider(8, &Provide);

    std::fprintf(stderr, "SOTC TRACE GPU: GPU timestamps on (timestampValidBits %u, period %.3f "
                         "ns, calibration %s)\n",
                 bits, timeline->period_ns,
                 !timeline->can_calibrate ? "off, anchored at the first submit"
                 : timeline->host_domain  ? "QueryPerformanceCounter"
                                          : "device domain only");
    return timeline;
}

Timeline::Timeline(const Vulkan::Instance& instance_)
    : instance{instance_}, device{static_cast<VkDevice>(instance_.GetDevice())},
      recs{std::make_unique<Rec[]>(kRecords)} {
    valid_bits = instance.GetTimestampValidBits();
    mask = valid_bits >= 64 ? ~u64{0} : (u64{1} << valid_bits) - 1;
    period_ns = static_cast<double>(instance.GetTimestampPeriodNs());
    can_calibrate = instance.IsGpuTraceCalibrated();
    calibrated_khr = instance.IsGpuTraceCalibratedKhr();
    host_domain = can_calibrate && instance.IsGpuTraceHostDomain();
    qpc_frequency = host_domain ? QpcFrequency() : 0;
    host_domain = host_domain && qpc_frequency != 0;
    track = SotcTrace::NewTrack("GPU (draw queue)", "gpu", 0);
}

Timeline::~Timeline() {
    // The watch thread is stopped before this runs (the scheduler's member order), and the GPU may
    // still hold pairs of the last submits: wait before the pool is destroyed.
    if (pool) {
        static_cast<void>(instance.GetDevice().waitIdle());
    }
}

u32 Timeline::Begin(vk::CommandBuffer cmd) {
    const u32 pair = pair_next;
    pair_next = (pair_next + 1) % kPairs;
    cmd.resetQueryPool(*pool, pair * 2, 2);
    cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *pool, pair * 2);
    return pair;
}

void Timeline::End(vk::CommandBuffer cmd, u32 pair) {
    if (pair == kNoPair) {
        return;
    }
    cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *pool, pair * 2 + 1);
}

void Timeline::Submitted(u32 pair, u64 tick, u64 submit_ns) {
    // Seqlock-style: the reader checks the tick before and after it reads the fields.
    Rec& r = recs[tick % kRecords];
    r.tick.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    r.pair.store(pair, std::memory_order_relaxed);
    r.submit_ns.store(submit_ns, std::memory_order_relaxed);
    r.tick.store(tick, std::memory_order_release);
}

void Timeline::Poll(u64 now_ns) {
    if (!can_calibrate) {
        return;
    }
    if (tried_calibration && now_ns - last_calibration_ns < kCalibrationEveryNs) {
        return;
    }
    tried_calibration = true;
    last_calibration_ns = now_ns;
    Calibrate();
}

void Timeline::Calibrate() {
    const VkDevice dev = device;
    if (host_domain) {
        VkCalibratedTimestampInfoKHR infos[2]{DeviceDomain(), DeviceDomain()};
        infos[1].timeDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
        u64 stamps[2]{};
        u64 max_deviation = 0;
        const u64 ta = SotcTrace::Now();
        if (CalibratedTimestamps(calibrated_khr, dev, 2, infos, stamps, &max_deviation) !=
            VK_SUCCESS) {
            return;
        }
        const u64 tb = SotcTrace::Now();
        // The device stamp and the QPC count were read together inside the call, which ran
        // between ta and tb. The steady-minus-QPC offset is taken from the narrowest call seen
        // (the two clocks differ by a constant).
        const u64 qpc_ns = QpcToNs(stamps[1], qpc_frequency);
        const u64 bracket = tb - ta;
        if (!have_qpc_offset || bracket <= qpc_bracket) {
            qpc_offset = static_cast<s64>((ta + tb) / 2) - static_cast<s64>(qpc_ns);
            qpc_bracket = bracket;
            have_qpc_offset = true;
        }
        SetCalibration(stamps[0] & mask, static_cast<u64>(static_cast<s64>(qpc_ns) + qpc_offset));
    } else {
        VkCalibratedTimestampInfoKHR info = DeviceDomain();
        u64 stamp = 0;
        u64 max_deviation = 0;
        const u64 ta = SotcTrace::Now();
        if (CalibratedTimestamps(calibrated_khr, dev, 1, &info, &stamp, &max_deviation) !=
            VK_SUCCESS) {
            return;
        }
        const u64 tb = SotcTrace::Now();
        SetCalibration(stamp & mask, (ta + tb) / 2);
    }
}

void Timeline::SetCalibration(u64 ticks, u64 cpu_ns) {
    cal_ticks = ticks;
    cal_cpu = cpu_ns;
    have_map = true;
    calibrated = true;
    Shared& s = GetShared();
    const std::scoped_lock lk{s.mutex};
    s.calibrated = true;
    if (s.pairs.size() >= kMaxPairs) {
        s.pairs.erase(s.pairs.begin());
    }
    s.pairs.emplace_back(ticks, cpu_ns);
}

u64 Timeline::ToCpu(u64 ticks) const {
    // Signed distance from the calibration point, modulo the counter width (the stamps wrap at
    // valid_bits).
    const u64 d = (ticks - cal_ticks) & mask;
    const u64 half = mask / 2;
    const double ns = period_ns * (d > half ? -static_cast<double>(mask - d + 1)
                                            : static_cast<double>(d));
    const double cpu = static_cast<double>(cal_cpu) + ns;
    return cpu <= 0.0 ? 0 : static_cast<u64>(cpu);
}

void Timeline::Completed(u64 first, u64 last) {
    Shared& s = GetShared();
    const VkQueryPool qp = static_cast<VkQueryPool>(*pool);
    for (u64 tick = first; tick <= last; ++tick) {
        const Rec& r = recs[tick % kRecords];
        if (r.tick.load(std::memory_order_acquire) != tick) {
            s.lagged.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const u32 pair = r.pair.load(std::memory_order_relaxed);
        const u64 submit_ns = r.submit_ns.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (r.tick.load(std::memory_order_relaxed) != tick) {
            s.lagged.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (pair == kNoPair) {
            s.no_pair.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // With WITH_AVAILABILITY each query gives its value and its availability: 2 u64 each.
        u64 q[4]{};
        const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetQueryPoolResults(
            device, qp, pair * 2, 2, sizeof(q), q, 2 * sizeof(u64),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result != VK_SUCCESS) {
            s.not_ready.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        if (!have_map) {
            // No calibration yet: the first pair's begin is taken as its submit time.
            cal_ticks = q[0] & mask;
            cal_cpu = submit_ns;
            have_map = true;
        }
        const u64 t0 = ToCpu(q[0] & mask);
        u64 t1 = ToCpu(q[2] & mask);
        if (t1 < t0) {
            t1 = t0;
        }
        SotcTrace::PushEventOn(track, SotcTrace::EvGpu, "cmdbuf", t0, t1, tick, 0);
        s.events.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace SotcGpuTrace
