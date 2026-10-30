// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: see vk_low_latency.h.

#include <cstdint>
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_low_latency.h"

namespace Vulkan {

namespace {

// Longest the NVIDIA sleep may hold the present thread on its timeline wait. The sleep itself
// is paced by the driver; this only keeps a misbehaving driver from stalling the flips.
constexpr u64 kNvSleepWaitNs = 100'000'000;

// Longest the generic limiter holds a flip back for the previous frame's GPU work.


const char* BackendName(LowLatency::Backend backend) {
    switch (backend) {
    case LowLatency::Backend::Nvidia:
        return "NVIDIA VK_NV_low_latency2";
    case LowLatency::Backend::Amd:
        return "AMD VK_AMD_anti_lag";
    case LowLatency::Backend::Generic:
        return "generic CPU limiter";
    default:
        return "off";
    }
}

} // namespace

LowLatency::LowLatency(const Instance& instance_) : instance{instance_} {
    if (!instance.IsLowLatencyRequested()) {
        return;
    }
    const vk::Device device = instance.GetDevice();

    // NVIDIA: the sleep, marker and mode entry points must be loaded, and the timeline semaphore
    // that vkLatencySleepNV signals must be created.
    if (instance.IsNvLowLatencyEnabled()) {
        const auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;
        if (dispatch.vkSetLatencySleepModeNV && dispatch.vkLatencySleepNV &&
            dispatch.vkSetLatencyMarkerNV) {
            const vk::SemaphoreTypeCreateInfo type_info{
                .semaphoreType = vk::SemaphoreType::eTimeline,
                .initialValue = 0,
            };
            const vk::SemaphoreCreateInfo create_info{
                .pNext = &type_info,
            };
            auto [result, semaphore] = device.createSemaphore(create_info);
            if (result == vk::Result::eSuccess) {
                timeline = static_cast<VkSemaphore>(semaphore);
                backend = Backend::Nvidia;
            } else {
                LOG_WARNING(Render_Vulkan, "Low latency: timeline semaphore failed ({})",
                            vk::to_string(result));
            }
        } else {
            LOG_WARNING(Render_Vulkan, "Low latency: VK_NV_low_latency2 entry points missing");
        }
    }

    // AMD: the anti-lag entry point must be loaded.
    if (backend == Backend::Off && instance.IsAmdAntiLagEnabled()) {
        if (VULKAN_HPP_DEFAULT_DISPATCHER.vkAntiLagUpdateAMD) {
            backend = Backend::Amd;
        } else {
            LOG_WARNING(Render_Vulkan, "Low latency: vkAntiLagUpdateAMD missing");
        }
    }

    // Every other device, and any failed setup above, takes the generic limiter.
    if (backend == Backend::Off) {
        backend = Backend::Generic;
    }
    LOG_INFO(Render_Vulkan, "Low latency: backend {} (SOTC_LOW_LATENCY=1)", BackendName(backend));
}

LowLatency::~LowLatency() {
    if (timeline) {
        instance.GetDevice().destroySemaphore(static_cast<vk::Semaphore>(timeline));
    }
}

void LowLatency::Degrade(const char* reason) {
    LOG_WARNING(Render_Vulkan, "Low latency: {}, falling back to {}", reason,
                BackendName(Backend::Generic));
    if (timeline) {
        instance.GetDevice().destroySemaphore(static_cast<vk::Semaphore>(timeline));
        timeline = {};
    }
    configured = {};
    backend = Backend::Generic;
}

bool LowLatency::ConfigureNvidia(VkSwapchainKHR swapchain) {
    if (swapchain == configured) {
        return true;
    }
    const VkLatencySleepModeInfoNV mode{
        .sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV,
        .pNext = nullptr,
        .lowLatencyMode = VK_TRUE,
        .lowLatencyBoost = VK_FALSE,
        .minimumIntervalUs = 0,
    };
    const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkSetLatencySleepModeNV(
        static_cast<VkDevice>(instance.GetDevice()), swapchain, &mode);
    if (result != VK_SUCCESS) {
        return false;
    }
    configured = swapchain;
    return true;
}

void LowLatency::Marker(VkLatencyMarkerNV marker) {
    if (!configured) {
        return;
    }
    const VkSetLatencyMarkerInfoNV info{
        .sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV,
        .pNext = nullptr,
        .presentID = marker_id,
        .marker = marker,
    };
    VULKAN_HPP_DEFAULT_DISPATCHER.vkSetLatencyMarkerNV(static_cast<VkDevice>(instance.GetDevice()),
                                                       configured, &info);
}

void LowLatency::FrameStart(VkSwapchainKHR swapchain) {
    if (backend == Backend::Off) {
        return;
    }
    ++frame_id;
    marker_id = last_present_id + 1; // sotc: the present this frame should become

    switch (backend) {
    case Backend::Nvidia: {
        if (!ConfigureNvidia(swapchain)) {
            Degrade("vkSetLatencySleepModeNV failed");
            break;
        }
        // Sleep, then wait on the timeline the sleep signals: the driver holds the frame back
        // until it is due, and the present thread only continues after that.
        const VkDevice device = static_cast<VkDevice>(instance.GetDevice());
        ++sleep_value;
        const VkLatencySleepInfoNV sleep_info{
            .sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV,
            .pNext = nullptr,
            .signalSemaphore = timeline,
            .value = sleep_value,
        };
        if (VULKAN_HPP_DEFAULT_DISPATCHER.vkLatencySleepNV(device, swapchain, &sleep_info) !=
            VK_SUCCESS) {
            Degrade("vkLatencySleepNV failed");
            break;
        }
        const VkSemaphoreWaitInfo wait_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .pNext = nullptr,
            .flags = 0,
            .semaphoreCount = 1,
            .pSemaphores = &timeline,
            .pValues = &sleep_value,
        };
        // A timeout only skips the wait for this frame; the next sleep waits on its own value.
        const VkResult wait = VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitSemaphores(device, &wait_info,
                                                                            kNvSleepWaitNs);
        if (wait != VK_SUCCESS && wait != VK_TIMEOUT) {
            Degrade("waiting for the latency sleep failed");
            break;
        }
        Marker(VK_LATENCY_MARKER_SIMULATION_START_NV);
        break;
    }
    case Backend::Amd: {
        const VkAntiLagPresentationInfoAMD present{
            .sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD,
            .pNext = nullptr,
            .stage = VK_ANTI_LAG_STAGE_INPUT_AMD,
            .frameIndex = frame_id,
        };
        const VkAntiLagDataAMD data{
            .sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD,
            .pNext = nullptr,
            .mode = VK_ANTI_LAG_MODE_ON_AMD,
            .maxFPS = 0,
            .pPresentationInfo = &present,
        };
        VULKAN_HPP_DEFAULT_DISPATCHER.vkAntiLagUpdateAMD(static_cast<VkDevice>(instance.GetDevice()),
                                                         &data);
        break;
    }
    case Backend::Generic: {
        // sotc: no wait here. Waiting (up to 50 ms) for the previous frame's fence ran on the
        // vblank thread, whose timer then caught up with back-to-back vblanks: uneven pacing
        // (review of 2026-10-27). On other GPUs SOTC_PRESENT_WAIT paces on the display instead.
        break;
    }
    default:
        break;
    }
}

void LowLatency::SubmitStart() {
    if (backend == Backend::Nvidia) {
        Marker(VK_LATENCY_MARKER_RENDERSUBMIT_START_NV);
    }
}

void LowLatency::SubmitEnd() {
    if (backend == Backend::Nvidia) {
        Marker(VK_LATENCY_MARKER_RENDERSUBMIT_END_NV);
    }
}

void LowLatency::BeforePresent(u64 present_id) {
    if (present_id != 0) {
        marker_id = present_id;
        last_present_id = present_id;
    }
    switch (backend) {
    case Backend::Nvidia:
        Marker(VK_LATENCY_MARKER_PRESENT_START_NV);
        break;
    case Backend::Amd: {
        const VkAntiLagPresentationInfoAMD present{
            .sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD,
            .pNext = nullptr,
            .stage = VK_ANTI_LAG_STAGE_PRESENT_AMD,
            .frameIndex = frame_id,
        };
        const VkAntiLagDataAMD data{
            .sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD,
            .pNext = nullptr,
            .mode = VK_ANTI_LAG_MODE_ON_AMD,
            .maxFPS = 0,
            .pPresentationInfo = &present,
        };
        VULKAN_HPP_DEFAULT_DISPATCHER.vkAntiLagUpdateAMD(static_cast<VkDevice>(instance.GetDevice()),
                                                         &data);
        break;
    }
    default:
        break;
    }
}

void LowLatency::AfterPresent(VkFence submitted) {
    if (backend == Backend::Off) {
        return;
    }
    if (backend == Backend::Nvidia) {
        Marker(VK_LATENCY_MARKER_PRESENT_END_NV);
    }
    if (backend == Backend::Generic) {
        last_fence = submitted;
    }
}

} // namespace Vulkan
