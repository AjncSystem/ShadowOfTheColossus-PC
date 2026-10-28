// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: vendor-neutral low latency mode (SOTC_LOW_LATENCY=1, alias SOTC_REFLEX=1). Default off.
// The backend is picked once, from what the device enabled (vk_instance.cpp):
//   1. NVIDIA: VK_NV_low_latency2 (Reflex without Streamline): sleep mode on the swapchain,
//      vkLatencySleepNV paced on a timeline semaphore, latency markers keyed by frame id.
//   2. AMD: VK_AMD_anti_lag: vkAntiLagUpdateAMD at the input and present stages.
//   3. Generic: no extension. The next game flip waits (at most 50 ms) for the previous frame's
//      final submit to finish on the GPU.
// A backend whose setup fails falls back to the generic one. Everything is a no-op when the
// switch is off.

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;

class LowLatency {
public:
    enum class Backend : u8 { Off, Nvidia, Amd, Generic };

    explicit LowLatency(const Instance& instance);
    ~LowLatency();

    LowLatency(const LowLatency&) = delete;
    LowLatency& operator=(const LowLatency&) = delete;

    Backend GetBackend() const {
        return backend;
    }

    /// Start of a game frame, before the flip is released (driver.cpp). Sleeps or waits
    /// according to the backend. `swapchain` is the swapchain the frame presents to.
    void FrameStart(VkSwapchainKHR swapchain);

    /// Around the frame's submit (vk_presenter.cpp, Present). NVIDIA only.
    void SubmitStart();
    void SubmitEnd();

    /// Right before the queue present. AMD sets the present stage; NVIDIA marks PRESENT_START.
    /// present_id: the VkPresentIdKHR of this present (0 when present ids are off).
    void BeforePresent(u64 present_id);

    /// Right after the queue present. `submitted` is the fence the frame's final submit signals:
    /// the generic backend waits on it at the next FrameStart.
    void AfterPresent(VkFence submitted);

private:
    /// Drops to the generic backend after a failed setup, and logs why.
    void Degrade(const char* reason);

    /// NVIDIA: sets the sleep mode on `swapchain` if not yet set. False when that failed.
    bool ConfigureNvidia(VkSwapchainKHR swapchain);

    /// NVIDIA: sets a latency marker for the current frame.
    void Marker(VkLatencyMarkerNV marker);

    const Instance& instance;
    Backend backend{Backend::Off};
    u64 frame_id{};                    ///< counts FrameStart calls, used as the AMD frame id
    u64 marker_id{};    ///< sotc: NVIDIA marker presentID: the real present id (expected next one
                        ///< before the present), so Reflex can match markers to presents
    u64 last_present_id{}; ///< sotc: the last VkPresentIdKHR seen by BeforePresent
    u64 sleep_value{};                 ///< signal value of the timeline semaphore for vkLatencySleepNV
    VkSemaphore timeline{};            ///< NVIDIA: timeline semaphore signalled by vkLatencySleepNV
    VkSwapchainKHR configured{};       ///< NVIDIA: swapchain the sleep mode was set on
    VkFence last_fence{};              ///< generic: fence of the last frame's final submit
};

} // namespace Vulkan
