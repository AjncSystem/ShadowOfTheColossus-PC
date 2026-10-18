// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: see fg_pass.h.

#include <algorithm>
#include <limits>
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/host_passes/fg_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
// After vk_common.h (via fg_pass.h), which sets the Vulkan header defines first.
#include <vk_mem_alloc.h>
#include "ffx_vk_fsr3_3_1_5_bridge.h"

// Local fsr-vulkan bridge (gpu/sotc/fsr): pins the FI/OF compute pipelines to this subgroup size
// (0 = driver default). Must be set before the context is created.
extern "C" void ffxVkSotcSetRequiredSubgroupSize(uint32_t size);

namespace Vulkan::HostPasses {

namespace {

constexpr vk::Format kColorFormat = vk::Format::eR8G8B8A8Unorm;
// Depth and motion are neutral, so their size barely matters; capped to keep the FI's
// render-size passes at the cost measured by fg-bench (render 1920x1080).
constexpr u32 kMaxRenderWidth = 1920;
// Constant depth of the neutral input (non-inverted depth, the fixed 3.1.6 shader profile).
constexpr float kNeutralDepth = 0.5f;
// Neutral camera: the bridge requires finite values, positive near/far/FOV/scale.
constexpr float kNear = 0.1f, kFar = 10000.0f, kFovY = 1.0f;

// Frame images are created with these (Presenter::RecreateFrame).
constexpr VkImageUsageFlags kFrameUsage =
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageUsageFlags kInputUsage =
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
// The output is written by the FI and blitted to the swapchain.
constexpr VkImageUsageFlags kOutputUsage =
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

bool CreateImage(const Instance& instance, u32 width, u32 height, vk::Format format,
                 VkImageUsageFlags usage, vk::Image& image, VmaAllocation& allocation,
                 const char* name) {
    const VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = static_cast<VkFormat>(format),
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    VkImage raw{};
    if (vmaCreateImage(instance.GetAllocator(), &image_info, &alloc_info, &raw, &allocation,
                       nullptr) != VK_SUCCESS) {
        return false;
    }
    image = vk::Image{raw};
    SetObjectName(instance.GetDevice(), image, "{}", name);
    return true;
}

FfxVkFsr3_3_1_6FrameGenerationImage Desc(vk::Image image, vk::Format format, u32 width,
                                         u32 height, VkImageLayout layout,
                                         VkImageUsageFlags usage) {
    return {static_cast<VkImage>(image), static_cast<VkFormat>(format), width, height, layout,
            usage};
}

} // namespace

FgPass::FgPass(const Instance& instance_, Scheduler& scheduler_, Scheduler& draw_scheduler_)
    : instance{instance_}, scheduler{scheduler_}, draw_scheduler{draw_scheduler_} {
    const vk::Device device = instance.GetDevice();
    for (u32 i = 0; i < slots.size(); ++i) {
        slots[i].frame.present_done = Check<"create fg present fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        slots[i].frame.id = static_cast<u8>(0xF0 + i);
    }
    // GPU time of each interpolation, when the graphics queue has timestamps.
    const auto limits = instance.GetPhysicalDevice().getProperties().limits;
    if (limits.timestampComputeAndGraphics) {
        timestamp_period_ns = limits.timestampPeriod;
        queries = Check<"create fg query pool">(device.createQueryPool({
            .queryType = vk::QueryType::eTimestamp,
            .queryCount = static_cast<u32>(2 * slots.size()),
        }));
    }
}

FgPass::~FgPass() {
    Destroy();
    const vk::Device device = instance.GetDevice();
    for (Slot& slot : slots) {
        device.destroyFence(slot.frame.present_done);
    }
    if (queries) {
        device.destroyQueryPool(queries);
    }
}

void FgPass::Destroy() {
    const vk::Device device = instance.GetDevice();
    for (Slot& slot : slots) {
        if (slot.pending) {
            scheduler.Wait(slot.tick);
        }
        (void)device.waitForFences(slot.frame.present_done, true, std::numeric_limits<u64>::max());
    }
    Retire();
    if (context) {
        ffxVkFsr3_3_1_6FrameGenerationContextDestroy(context);
        context = nullptr;
    }
    const auto destroy = [&](vk::Image& image, VmaAllocation& allocation) {
        if (image) {
            vmaDestroyImage(instance.GetAllocator(), static_cast<VkImage>(image), allocation);
        }
        image = vk::Image{};
        allocation = {};
    };
    destroy(depth.image, depth.allocation);
    destroy(motion.image, motion.allocation);
    for (Slot& slot : slots) {
        destroy(slot.frame.image, slot.allocation);
        slot.frame.width = slot.frame.height = 0;
    }
}

bool FgPass::Recreate(u32 width, u32 height) {
    Destroy();
    render_width = std::min(width, kMaxRenderWidth);
    render_height = std::max(1u, static_cast<u32>(u64{height} * render_width / width));
    bool ok = CreateImage(instance, render_width, render_height, vk::Format::eR32Sfloat,
                          kInputUsage, depth.image, depth.allocation, "FG neutral depth") &&
              CreateImage(instance, render_width, render_height, vk::Format::eR16G16Sfloat,
                          kInputUsage, motion.image, motion.allocation, "FG zero motion");
    for (Slot& slot : slots) {
        ok = ok && CreateImage(instance, width, height, kColorFormat, kOutputUsage,
                               slot.frame.image, slot.allocation, "FG output");
        slot.frame.width = width;
        slot.frame.height = height;
    }
    if (ok) {
        ffxVkSotcSetRequiredSubgroupSize(instance.FrameGenSubgroupSize());
        const FfxVkFsr3_3_1_6FrameGenerationCreateInfo create = {
            .physicalDevice = static_cast<VkPhysicalDevice>(instance.GetPhysicalDevice()),
            .device = static_cast<VkDevice>(instance.GetDevice()),
            .maxRenderWidth = render_width,
            .maxRenderHeight = render_height,
            .displayWidth = width,
            .displayHeight = height,
            .colorFormat = static_cast<VkFormat>(kColorFormat),
        };
        const auto result = ffxVkFsr3_3_1_6FrameGenerationContextCreate(&create, &context);
        if (result != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
            LOG_ERROR(Render_Vulkan, "Frame generation (SOTC_FG): context {}x{} failed ({})",
                      width, height, static_cast<int>(result));
            context = nullptr;
            ok = false;
        }
    }
    if (!ok) {
        LOG_ERROR(Render_Vulkan, "Frame generation (SOTC_FG): disabled, real frames only");
        failed = true;
        Destroy();
        return false;
    }

    // Logged at every (re)creation, which happens only when the output size changes.
    FfxVkFsr3_3_1_6FrameGenerationMemoryUsage usage{};
    ffxVkFsr3_3_1_6FrameGenerationContextGetMemoryUsage(context, &usage);
    const double own_mib =
        (2.0 * width * height * 4 + 2.0 * render_width * render_height * 4) / 1048576.0;
    LOG_INFO(Render_Vulkan,
             "Frame generation (SOTC_FG): FSR 3.1.6 frame interpolation {}x{} (neutral depth and "
             "motion {}x{}), FI/OF memory {:.1f} MiB + own images {:.1f} MiB, subgroup {}, "
             "timestamps {}",
             width, height, render_width, render_height, usage.totalUsageInBytes / 1048576.0,
             own_mib, instance.FrameGenSubgroupSize(), queries ? true : false);

    // Neutral inputs, set once: constant depth, zero motion. They go out with the first
    // interpolation submit (same command buffer).
    scheduler.EndRendering();
    const vk::CommandBuffer cmdbuf = scheduler.CommandBuffer();
    constexpr vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    for (const auto& [image, value] : {std::pair{depth.image, kNeutralDepth},
                                       std::pair{motion.image, 0.0f}}) {
        const vk::ImageMemoryBarrier to_clear{
            .srcAccessMask = vk::AccessFlagBits::eNone,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                               vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_clear);
        cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal,
                               vk::ClearColorValue{std::array{value, value, value, value}},
                               range);
        const vk::ImageMemoryBarrier to_read{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eComputeShader, {}, {}, {}, to_read);
    }
    reset = true;
    return true;
}

void FgPass::Retire() {
    for (u32 i = 0; i < slots.size(); ++i) {
        Slot& slot = slots[i];
        if (!slot.pending || !scheduler.IsFree(slot.tick)) {
            continue;
        }
        if (queries) {
            u64 ts[2]{};
            if (instance.GetDevice().getQueryPoolResults(queries, 2 * i, 2, sizeof(ts), ts,
                                                         sizeof(u64),
                                                         vk::QueryResultFlagBits::e64) ==
                    vk::Result::eSuccess &&
                ts[1] > ts[0]) {
                gpu_ms.push_back(static_cast<float>((ts[1] - ts[0]) * timestamp_period_ns * 1e-6));
            }
        }
        if (context) {
            ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(context, slot.frame_id);
        }
        slot.pending = false;
    }
}

Frame* FgPass::Generate(const Frame& frame, double frame_time_ms) {
    if (failed) {
        return nullptr;
    }
    if (frame.is_hdr) {
        // A2B10G10R10 / PQ is not the fixed 3.1.6 color format: real frames only meanwhile.
        reset = true;
        return nullptr;
    }
    if ((!context || frame.width != slots[0].frame.width ||
         frame.height != slots[0].frame.height) &&
        !Recreate(frame.width, frame.height)) {
        return nullptr;
    }
    Retire();

    const u32 index = next_slot;
    next_slot = (next_slot + 1) % slots.size();
    Slot& slot = slots[index];
    // The slot was used two real frames ago: its interpolation and the present that read its
    // output are normally long done.
    if (slot.pending) {
        scheduler.Wait(slot.tick);
        Retire();
    }
    const vk::Device device = instance.GetDevice();
    (void)device.waitForFences(slot.frame.present_done, true, std::numeric_limits<u64>::max());

    scheduler.EndRendering();
    const vk::CommandBuffer cmdbuf = scheduler.CommandBuffer();
    if (queries) {
        cmdbuf.resetQueryPool(queries, 2 * index, 2);
        cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, queries, 2 * index);
    }
    const vk::ImageMemoryBarrier to_output{
        .srcAccessMask = vk::AccessFlagBits::eNone,
        .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = slot.frame.image,
        .subresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                           vk::PipelineStageFlagBits::eComputeShader, {}, {}, {}, to_output);

    const float ms = static_cast<float>(std::max(frame_time_ms, 1.0));
    const bool new_history = std::exchange(reset, false);
    ++frame_id;
    const VkCommandBuffer cmd = static_cast<VkCommandBuffer>(cmdbuf);
    // The frame image stays in General (left there by the post process pass); the bridge
    // restores each imported image's layout after its passes.
    const auto color = Desc(frame.image, kColorFormat, frame.width, frame.height,
                            VK_IMAGE_LAYOUT_GENERAL, kFrameUsage);
    FfxVkFsr3_3_1_6FrameGenerationPrepareInfo prepare{};
    prepare.commandBuffer = cmd;
    prepare.color = color;
    prepare.depth = Desc(depth.image, vk::Format::eR32Sfloat, render_width, render_height,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kInputUsage);
    prepare.motionVectors = Desc(motion.image, vk::Format::eR16G16Sfloat, render_width,
                                 render_height, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 kInputUsage);
    prepare.renderWidth = render_width;
    prepare.renderHeight = render_height;
    prepare.motionVectorScaleX = static_cast<float>(render_width);
    prepare.motionVectorScaleY = static_cast<float>(render_height);
    prepare.frameTimeMilliseconds = ms;
    prepare.minLuminance = 0.0f;
    prepare.maxLuminance = 1.0f;
    prepare.transferFunction = FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB;
    prepare.cameraNear = kNear;
    prepare.cameraFar = kFar;
    prepare.viewSpaceToMeters = 1.0f;
    prepare.cameraVerticalFovRadians = kFovY;
    prepare.cameraUp[1] = 1.0f;
    prepare.cameraRight[0] = 1.0f;
    prepare.cameraForward[2] = -1.0f;
    prepare.frameId = frame_id;
    prepare.reset = new_history ? VK_TRUE : VK_FALSE;

    FfxVkFsr3_3_1_6FrameGenerationDispatchInfo dispatch{};
    dispatch.commandBuffer = cmd;
    dispatch.color = color;
    dispatch.output = Desc(slot.frame.image, kColorFormat, frame.width, frame.height,
                           VK_IMAGE_LAYOUT_GENERAL, kOutputUsage);
    dispatch.displayWidth = frame.width;
    dispatch.displayHeight = frame.height;
    dispatch.frameTimeMilliseconds = ms;
    dispatch.cameraNear = kNear;
    dispatch.cameraFar = kFar;
    dispatch.viewSpaceToMeters = 1.0f;
    dispatch.cameraVerticalFovRadians = kFovY;
    dispatch.minLuminance = 0.0f;
    dispatch.maxLuminance = 1.0f;
    dispatch.transferFunction = FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB;
    dispatch.frameId = frame_id;
    dispatch.reset = prepare.reset;

    auto result = ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(context, &prepare);
    if (result == FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        result = ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(context, &dispatch);
    }
    if (result != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        // ponytail: no recovery, FG stays off for the session; the partial work is still
        // submitted below and retired with the slot.
        LOG_ERROR(Render_Vulkan, "Frame generation (SOTC_FG): interpolation failed ({}), "
                                 "disabled, real frames only",
                  static_cast<int>(result));
        failed = true;
    }
    // The interpolation's reads of the real frame (compute, and the history copy) and its writes
    // of the output before anything later on the queue: the presents' layout transitions start
    // at the color attachment stage, and the draw thread rewrites the frame once it is released.
    const vk::MemoryBarrier after_fi{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader |
                               vk::PipelineStageFlagBits::eTransfer,
                           vk::PipelineStageFlagBits::eAllCommands, {}, after_fi, {}, {});
    if (queries) {
        cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, queries, 2 * index + 1);
    }

    // Own submit, after the real frame's: the generated frame is then ready at this tick
    // whether or not it is presented.
    SubmitInfo info{};
    if (frame.ready_semaphore == draw_scheduler.GetWorkSemaphore()->Handle()) {
        draw_scheduler.WaitSubmitted(frame.ready_tick, false);
    }
    info.AddWait(frame.ready_semaphore, frame.ready_tick);
    slot.tick = scheduler.CurrentTick();
    slot.frame_id = frame_id;
    slot.pending = true;
    scheduler.Flush(info);
    slot.frame.ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    slot.frame.ready_tick = slot.tick;
    return new_history || failed ? nullptr : &slot.frame;
}

} // namespace Vulkan::HostPasses
