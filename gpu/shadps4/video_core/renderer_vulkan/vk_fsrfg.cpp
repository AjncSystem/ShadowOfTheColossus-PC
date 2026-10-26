// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: AMD FSR 3 frame generation, see vk_fsrfg.h. POSIX port of the upstream fork's
// vk_fsrfg.cpp (the Windows high-resolution waitable timer became a sleep + spin).

#include "video_core/renderer_vulkan/vk_fsrfg.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <thread>
#include <utility>
#include <vk_mem_alloc.h>

#include <ffx_vk_fsr3_3_1_5_bridge.h>

#include "video_core/renderer_vulkan/vk_dlssg.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/sotc_stats.h"

#include "video_core/host_shaders/fg_depth_comp.h"
#include "video_core/host_shaders/fg_ui_mask_comp.h"
#include "video_core/host_shaders/fg_ui_inpaint_comp.h"

namespace Vulkan::FsrFg {

namespace {

constexpr u32 kMaxFrames = 16; // presentation frames, as vk_framegen_inputs.cpp
constexpr vk::Format kColorFormat = vk::Format::eR8G8B8A8Unorm;
constexpr vk::ImageUsageFlags kImageUsage =
    vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
    vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;

using Clock = std::chrono::steady_clock;

struct OwnedImage {
    vk::Image image;
    VmaAllocation alloc{};
    vk::ImageView view;
    u32 width{}, height{};
};

struct DepthSlot {
    OwnedImage r32;
    bool valid{};
};

const Instance* g_instance{};
bool supported = false;
int log_level = 0; // SOTC_FSRFG_LOG
std::atomic<int> state{0};

// Depth conversion, command processor thread.
vk::ShaderModule depth_shader;
vk::UniqueDescriptorSetLayout depth_set_layout;
vk::UniquePipelineLayout depth_layout;
vk::UniquePipeline depth_pipeline;
std::array<DepthSlot, kMaxFrames> depth_slots;

// UI inpaint compute pipeline (pre-dispatch), present thread.
vk::ShaderModule ui_inpaint_shader;
vk::UniqueDescriptorSetLayout ui_inpaint_set_layout;
vk::UniquePipelineLayout ui_inpaint_layout;
vk::UniquePipeline ui_inpaint_pipeline;

// UI mask compute pipeline (post-dispatch stamp), present thread.
vk::ShaderModule ui_mask_shader;
vk::UniqueDescriptorSetLayout ui_mask_set_layout;
vk::UniquePipelineLayout ui_mask_layout;
vk::UniquePipeline ui_mask_pipeline;
struct UiStatsBuffer {
    vk::Buffer buffer{};
    VmaAllocation alloc{};
    volatile uint32_t* mapped{nullptr};
};
static std::array<UiStatsBuffer, 2> ui_stats{};
static u32 ui_stats_slot{0};

// Present thread.
FfxVkFsr3_3_1_6FrameGenerationContext* context{};
u32 ctx_display_w{}, ctx_display_h{}, ctx_render_w{}, ctx_render_h{};
OwnedImage color, hudless_color, output;
bool output_initialised = false;
bool failed = false;
u64 fsr_frame = 0;              // FSR 3 frame id of the last Generate
bool recorded = false;          // Generate recorded work not yet given to Submitted
u64 game_frames = 0;            // NoteGameFrame calls
u64 history_game_frame = ~0ULL; // the game frame FSR 3 last saw
std::deque<std::pair<u64, u64>> in_flight; // FSR 3 frame id, scheduler tick
Scheduler* submit_scheduler{};
Clock::time_point frame_arrival{};
Clock::time_point last_present{}; // of any frame (NotePresent)
double interval_ms = 33.3;
double delay_sum_ms = 0.0; // generated frame presented after the game frame arrived
u32 delay_count = 0;
u64 generated = 0, generated_logged = 0;

bool CreateImage(OwnedImage& img, vk::Format format, u32 width, u32 height) {
    const VkImageCreateInfo info = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = kImageUsage,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    const VmaAllocationCreateInfo alloc_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    VkImage raw{};
    if (vmaCreateImage(g_instance->GetAllocator(), &info, &alloc_info, &raw, &img.alloc, nullptr) !=
        VK_SUCCESS) {
        img = {};
        return false;
    }
    img.image = vk::Image{raw};
    auto [result, view] = g_instance->GetDevice().createImageView(vk::ImageViewCreateInfo{
        .image = img.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    });
    if (result != vk::Result::eSuccess) {
        vmaDestroyImage(g_instance->GetAllocator(), img.image, img.alloc);
        img = {};
        return false;
    }
    img.view = view;
    img.width = width;
    img.height = height;
    return true;
}

void DestroyImage(OwnedImage& img) {
    if (img.view) {
        g_instance->GetDevice().destroyImageView(img.view);
    }
    if (img.image) {
        vmaDestroyImage(g_instance->GetAllocator(), img.image, img.alloc);
    }
    img = {};
}

vk::ImageMemoryBarrier2 Barrier(vk::Image image, vk::PipelineStageFlags2 src_stage,
                                vk::AccessFlags2 src_access, vk::ImageLayout old_layout,
                                vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access,
                                vk::ImageLayout new_layout) {
    return vk::ImageMemoryBarrier2{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .image = image,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
}

void SetState(int value) {
    state.store(value, std::memory_order_relaxed);
    SotcStats::ov_fsr_state.store(value, std::memory_order_relaxed);
}

/// Retires the FSR 3 frames whose commands the GPU finished.
void RetireCompleted(bool wait_all) {
    while (!in_flight.empty() && submit_scheduler) {
        const auto [id, tick] = in_flight.front();
        if (wait_all) {
            submit_scheduler->Wait(tick);
        } else if (!submit_scheduler->IsFree(tick)) {
            break;
        }
        ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(context, id);
        in_flight.pop_front();
    }
}

void DestroyContext() {
    if (!context) {
        return;
    }
    RetireCompleted(true);
    ffxVkFsr3_3_1_6FrameGenerationContextDestroy(context);
    context = nullptr;
    DestroyImage(color);
    DestroyImage(hudless_color);
    DestroyImage(output);
    output_initialised = false;
}

bool EnsureContext(u32 display_w, u32 display_h, u32 render_w, u32 render_h) {
    if (context && ctx_display_w == display_w && ctx_display_h == display_h &&
        render_w <= ctx_render_w && render_h <= ctx_render_h) {
        return true;
    }
    DestroyContext();
    const FfxVkFsr3_3_1_6FrameGenerationCreateInfo info = {
        .physicalDevice = static_cast<VkPhysicalDevice>(g_instance->GetPhysicalDevice()),
        .device = static_cast<VkDevice>(g_instance->GetDevice()),
        .maxRenderWidth = render_w,
        .maxRenderHeight = render_h,
        .displayWidth = display_w,
        .displayHeight = display_h,
        .colorFormat = static_cast<VkFormat>(kColorFormat),
    };
    const auto start = Clock::now();
    const FfxVkFsr3_3_1_6FrameGenerationResult r =
        ffxVkFsr3_3_1_6FrameGenerationContextCreate(&info, &context);
    if (r != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        std::fprintf(stderr, "SOTC FSR3 FG: context creation failed (%d), frame generation off\n",
                     static_cast<int>(r));
        context = nullptr;
        return false;
    }
    if (!CreateImage(color, kColorFormat, display_w, display_h) ||
        !CreateImage(hudless_color, kColorFormat, display_w, display_h) ||
        !CreateImage(output, kColorFormat, display_w, display_h)) {
        std::fprintf(stderr, "SOTC FSR3 FG: cannot create the %ux%u images\n", display_w, display_h);
        DestroyContext();
        return false;
    }
    ctx_display_w = display_w;
    ctx_display_h = display_h;
    ctx_render_w = render_w;
    ctx_render_h = render_h;
    history_game_frame = ~0ULL;
    FfxVkFsr3_3_1_6FrameGenerationMemoryUsage usage{};
    ffxVkFsr3_3_1_6FrameGenerationContextGetMemoryUsage(context, &usage);
    std::fprintf(stderr,
                 "SOTC FSR3 FG: context %ux%u (render %ux%u), %.0f MB, created in %.0f ms\n",
                 display_w, display_h, render_w, render_h,
                 (usage.totalUsageInBytes + 2ull * display_w * display_h * 4) / 1048576.0,
                 std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    return true;
}

FfxVkFsr3_3_1_6FrameGenerationImage FgImage(vk::Image image, vk::Format format, u32 width,
                                            u32 height, vk::ImageLayout layout,
                                            vk::ImageUsageFlags usage) {
    return FfxVkFsr3_3_1_6FrameGenerationImage{
        .image = static_cast<VkImage>(image),
        .format = static_cast<VkFormat>(format),
        .width = width,
        .height = height,
        .layout = static_cast<VkImageLayout>(layout),
        .usage = static_cast<VkImageUsageFlags>(usage),
    };
}

/// Sleeps until `due`: a coarse sleep, then a short spin for the sub-millisecond part.
void SleepUntil(Clock::time_point due) {
    using namespace std::chrono_literals;
    for (;;) {
        const auto left = due - Clock::now();
        if (left <= 0ns) {
            return;
        }
        if (left > 1500us) {
            std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::microseconds>(
                                            left - 1000us));
            continue;
        }
        std::this_thread::yield();
    }
}

} // namespace

void Init(const Instance& instance) {
    g_instance = &instance;
    if (const char* l = std::getenv("SOTC_FSRFG_LOG")) {
        log_level = std::atoi(l);
    }
    const vk::PhysicalDevice gpu = instance.GetPhysicalDevice();
    const auto features = gpu.getFeatures();
    const auto can = [&](vk::Format format, vk::FormatFeatureFlags needed) {
        return (gpu.getFormatProperties(format).optimalTilingFeatures & needed) == needed;
    };
    const vk::FormatFeatureFlags storage =
        vk::FormatFeatureFlagBits::eStorageImage | vk::FormatFeatureFlagBits::eSampledImage;
    if (instance.ApiVersion() < VK_API_VERSION_1_3 || !features.shaderStorageImageWriteWithoutFormat ||
        !can(kColorFormat, storage | vk::FormatFeatureFlagBits::eBlitDst) ||
        !can(vk::Format::eR32Sfloat, storage) || !can(vk::Format::eR16G16Sfloat, storage)) {
        std::fprintf(stderr, "SOTC FSR3 FG: not supported by this GPU (Vulkan 1.3 and storage "
                             "image formats needed)\n");
        return;
    }
    const vk::Device device = instance.GetDevice();
    depth_shader = CompileSPV(FG_DEPTH_COMP, device);
    const std::array bindings{
        vk::DescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        vk::DescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    };
    auto [set_result, set_layout] = device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    });
    const vk::PushConstantRange push_range = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = 3 * sizeof(u32),
    };
    if (set_result != vk::Result::eSuccess) {
        std::fprintf(stderr, "SOTC FSR3 FG: descriptor set layout failed\n");
        return;
    }
    depth_set_layout = std::move(set_layout);
    const vk::DescriptorSetLayout layouts[] = {*depth_set_layout};
    auto [layout_result, layout] = device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = layouts,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    });
    if (layout_result != vk::Result::eSuccess) {
        std::fprintf(stderr, "SOTC FSR3 FG: pipeline layout failed\n");
        return;
    }
    depth_layout = std::move(layout);
    auto [pipeline_result, pipeline] = device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage{
                    .stage = vk::ShaderStageFlagBits::eCompute,
                    .module = depth_shader,
                    .pName = "main",
                },
                .layout = *depth_layout,
            });
    if (pipeline_result != vk::Result::eSuccess) {
        std::fprintf(stderr, "SOTC FSR3 FG: depth pipeline failed\n");
        return;
    }
    depth_pipeline = std::move(pipeline);

    ui_mask_shader = CompileSPV(FG_UI_MASK_COMP, device);
    const std::array ui_bindings{
        vk::DescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        vk::DescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        vk::DescriptorSetLayoutBinding{
            .binding = 2,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        vk::DescriptorSetLayoutBinding{
            .binding = 3,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    };
    auto [ui_set_result, ui_set_layout] = device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(ui_bindings.size()),
        .pBindings = ui_bindings.data(),
    });
    const vk::PushConstantRange ui_push_range = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = 2 * sizeof(u32),
    };
    if (ui_set_result != vk::Result::eSuccess) {
        std::fprintf(stderr, "SOTC FSR3 FG: ui mask descriptor set layout failed\n");
        return;
    }
    ui_mask_set_layout = std::move(ui_set_layout);
    const vk::DescriptorSetLayout ui_layouts[] = {*ui_mask_set_layout};
    auto [ui_layout_result, ui_layout] = device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = ui_layouts,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &ui_push_range,
    });
    if (ui_layout_result != vk::Result::eSuccess) {
        std::fprintf(stderr, "SOTC FSR3 FG: ui mask pipeline layout failed\n");
        return;
    }
    ui_mask_layout = std::move(ui_layout);
    auto [ui_pipe_result, ui_pipe] = device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage{
                    .stage = vk::ShaderStageFlagBits::eCompute,
                    .module = ui_mask_shader,
                    .pName = "main",
                },
                .layout = *ui_mask_layout,
            });
    if (ui_pipe_result != vk::Result::eSuccess) {
        std::fprintf(stderr, "SOTC FSR3 FG: ui mask pipeline failed\n");
        return;
    }
    ui_mask_pipeline = std::move(ui_pipe);

    ui_inpaint_shader = CompileSPV(FG_UI_INPAINT_COMP, device);
    const std::array inpaint_bindings{
        vk::DescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        vk::DescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        vk::DescriptorSetLayoutBinding{
            .binding = 2,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    };
    auto [inpaint_set_result, inpaint_set_layout] = device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(inpaint_bindings.size()),
        .pBindings = inpaint_bindings.data(),
    });
    const vk::PushConstantRange inpaint_push_range = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = 2 * sizeof(u32),
    };
    if (inpaint_set_result == vk::Result::eSuccess) {
        ui_inpaint_set_layout = std::move(inpaint_set_layout);
        const vk::DescriptorSetLayout inpaint_layouts[] = {*ui_inpaint_set_layout};
        auto [inpaint_layout_result, inpaint_layout] = device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = inpaint_layouts,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &inpaint_push_range,
        });
        if (inpaint_layout_result == vk::Result::eSuccess) {
            ui_inpaint_layout = std::move(inpaint_layout);
            auto [inpaint_pipe_result, inpaint_pipe] = device.createComputePipelineUnique(
                {}, vk::ComputePipelineCreateInfo{
                    .stage{
                        .stage = vk::ShaderStageFlagBits::eCompute,
                        .module = ui_inpaint_shader,
                        .pName = "main",
                    },
                    .layout = *ui_inpaint_layout,
                });
            if (inpaint_pipe_result == vk::Result::eSuccess) {
                ui_inpaint_pipeline = std::move(inpaint_pipe);
            }
        }
    }

    for (auto& s : ui_stats) {
        const VkBufferCreateInfo buffer_ci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 5 * sizeof(uint32_t),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        const VmaAllocationCreateInfo alloc_ci = {
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        };
        VkBuffer buf{};
        VmaAllocationInfo alloc_info{};
        if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buf, &s.alloc, &alloc_info) == VK_SUCCESS) {
            s.buffer = vk::Buffer{buf};
            s.mapped = static_cast<volatile uint32_t*>(alloc_info.pMappedData);
            if (s.mapped) {
                s.mapped[0] = 0;
                s.mapped[1] = 0xFFFFFFFF;
                s.mapped[2] = 0;
                s.mapped[3] = 0xFFFFFFFF;
                s.mapped[4] = 0;
            }
        }
    }

    supported = true;
}

bool Supported() {
    return supported && !failed;
}

void ConvertDepth(u32 frame_id, vk::CommandBuffer cmdbuf, vk::ImageView depth_view, u32 width,
                  u32 height, bool inverted) {
    if (!supported || frame_id >= kMaxFrames) {
        return;
    }
    DepthSlot& slot = depth_slots[frame_id];
    slot.valid = false;
    if (slot.r32.width != width || slot.r32.height != height) {
        DestroyImage(slot.r32);
        if (!CreateImage(slot.r32, vk::Format::eR32Sfloat, width, height)) {
            std::fprintf(stderr, "SOTC FSR3 FG: cannot create the %ux%u depth image\n", width,
                         height);
            return;
        }
    }
    const auto to_write = Barrier(slot.r32.image, vk::PipelineStageFlagBits2::eAllCommands,
                                  vk::AccessFlagBits2::eNone, vk::ImageLayout::eUndefined,
                                  vk::PipelineStageFlagBits2::eComputeShader,
                                  vk::AccessFlagBits2::eShaderWrite, vk::ImageLayout::eGeneral);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_write,
    });
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *depth_pipeline);
    const vk::DescriptorImageInfo src_info = {
        .imageView = depth_view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::DescriptorImageInfo dst_info = {
        .imageView = slot.r32.view,
        .imageLayout = vk::ImageLayout::eGeneral,
    };
    const std::array writes{
        vk::WriteDescriptorSet{
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .pImageInfo = &src_info,
        },
        vk::WriteDescriptorSet{
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .pImageInfo = &dst_info,
        },
    };
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *depth_layout, 0, writes);
    const u32 params[3] = {width, height, inverted ? 1u : 0u};
    cmdbuf.pushConstants(*depth_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params),
                         params);
    cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
    const auto to_read = Barrier(slot.r32.image, vk::PipelineStageFlagBits2::eComputeShader,
                                 vk::AccessFlagBits2::eShaderWrite, vk::ImageLayout::eGeneral,
                                 vk::PipelineStageFlagBits2::eAllCommands,
                                 vk::AccessFlagBits2::eShaderRead, vk::ImageLayout::eGeneral);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_read,
    });
    slot.valid = true;
}

void NoDepth(u32 frame_id) {
    if (frame_id < kMaxFrames) {
        depth_slots[frame_id].valid = false;
    }
}

void NoteGameFrame() {
    const auto now = Clock::now();
    if (frame_arrival.time_since_epoch().count() != 0) {
        const double ms = std::chrono::duration<double, std::milli>(now - frame_arrival).count();
        if (ms > 4.0 && ms < 70.0) {
            interval_ms = interval_ms * 0.85 + ms * 0.15;
        }
    }
    frame_arrival = now;
    ++game_frames;
}

vk::Image Generate(vk::CommandBuffer cmdbuf, u32 frame_id, vk::Image frame_image,
                   vk::Image hudless_image, vk::ImageView hudless_view,
                   bool frame_has_hudless, u32 width, u32 height) {
    if (!Supported()) {
        return {};
    }
    Dlssg::FrameInputs in{};
    if (frame_id >= kMaxFrames || !depth_slots[frame_id].valid ||
        !Dlssg::GetFrameInputs(frame_id, in) || depth_slots[frame_id].r32.width != in.width ||
        depth_slots[frame_id].r32.height != in.height) {
        history_game_frame = ~0ULL;
        SetState(0);
        return {};
    }
    if (!EnsureContext(width, height, in.width, in.height)) {
        failed = true;
        SetState(2);
        return {};
    }
    RetireCompleted(false);
    // Interpolation needs the previous game frame to have gone through FSR 3.
    const bool reset = in.reset || history_game_frame + 1 != game_frames;
    history_game_frame = game_frames;
    ++fsr_frame;

    // The game frame -> the RGBA8 colour input (the swapchain format may be BGRA). Earlier
    // presents still reading `output` and FSR 3 work still reading `color` come first.
    const vk::MemoryBarrier2 earlier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    const bool use_hudless = frame_has_hudless && hudless_image && hudless_view;

    std::array<vk::ImageMemoryBarrier2, 3> pre{};
    u32 pre_count = 0;
    pre[pre_count++] = Barrier(frame_image, vk::PipelineStageFlagBits2::eAllCommands,
                               vk::AccessFlagBits2::eMemoryWrite, vk::ImageLayout::eGeneral,
                               vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferRead,
                               vk::ImageLayout::eTransferSrcOptimal);
    pre[pre_count++] = Barrier(color.image, vk::PipelineStageFlagBits2::eAllCommands,
                               vk::AccessFlagBits2::eNone, vk::ImageLayout::eUndefined,
                               vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferWrite,
                               vk::ImageLayout::eTransferDstOptimal);
    if (!output_initialised) {
        pre[pre_count++] = Barrier(output.image, vk::PipelineStageFlagBits2::eAllCommands,
                                   vk::AccessFlagBits2::eNone, vk::ImageLayout::eUndefined,
                                   vk::PipelineStageFlagBits2::eComputeShader,
                                   vk::AccessFlagBits2::eShaderWrite, vk::ImageLayout::eGeneral);
        output_initialised = true;
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &earlier,
        .imageMemoryBarrierCount = pre_count,
        .pImageMemoryBarriers = pre.data(),
    });
    const vk::ImageBlit blit{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                 vk::Offset3D{static_cast<s32>(width), static_cast<s32>(height), 1}},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                 vk::Offset3D{static_cast<s32>(width), static_cast<s32>(height), 1}},
    };
    cmdbuf.blitImage(frame_image, vk::ImageLayout::eTransferSrcOptimal, color.image,
                     vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eNearest);
    std::array<vk::ImageMemoryBarrier2, 2> post{};
    u32 post_count = 0;
    post[post_count++] = Barrier(frame_image, vk::PipelineStageFlagBits2::eBlit,
                                 vk::AccessFlagBits2::eTransferRead, vk::ImageLayout::eTransferSrcOptimal,
                                 vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryRead,
                                 vk::ImageLayout::eGeneral);
    post[post_count++] = Barrier(color.image, vk::PipelineStageFlagBits2::eBlit,
                                 vk::AccessFlagBits2::eTransferWrite, vk::ImageLayout::eTransferDstOptimal,
                                 vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead,
                                 vk::ImageLayout::eGeneral);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = post_count,
        .pImageMemoryBarriers = post.data(),
    });

    if (use_hudless && *ui_inpaint_pipeline) {
        // Inpaint HUD in hudless_color.image so FSR 3 receives a clean 3D scene with no yellow HUD.
        const auto to_inpaint_dst = Barrier(hudless_color.image,
                                            vk::PipelineStageFlagBits2::eAllCommands,
                                            vk::AccessFlagBits2::eNone, vk::ImageLayout::eUndefined,
                                            vk::PipelineStageFlagBits2::eComputeShader,
                                            vk::AccessFlagBits2::eShaderWrite, vk::ImageLayout::eGeneral);
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &to_inpaint_dst,
        });

        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *ui_inpaint_pipeline);
        const vk::DescriptorImageInfo mask_info = {
            .imageView = hudless_view,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        };
        const vk::DescriptorImageInfo color_info = {
            .imageView = color.view,
            .imageLayout = vk::ImageLayout::eGeneral,
        };
        const vk::DescriptorImageInfo clean_out_info = {
            .imageView = hudless_color.view,
            .imageLayout = vk::ImageLayout::eGeneral,
        };
        const std::array inpaint_writes{
            vk::WriteDescriptorSet{
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eSampledImage,
                .pImageInfo = &mask_info,
            },
            vk::WriteDescriptorSet{
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eSampledImage,
                .pImageInfo = &color_info,
            },
            vk::WriteDescriptorSet{
                .dstBinding = 2,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageImage,
                .pImageInfo = &clean_out_info,
            },
        };
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *ui_inpaint_layout, 0, inpaint_writes);
        const u32 params[2] = {width, height};
        cmdbuf.pushConstants(*ui_inpaint_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), params);
        cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);

        const auto post_inpaint = Barrier(hudless_color.image,
                                          vk::PipelineStageFlagBits2::eComputeShader,
                                          vk::AccessFlagBits2::eShaderWrite, vk::ImageLayout::eGeneral,
                                          vk::PipelineStageFlagBits2::eComputeShader,
                                          vk::AccessFlagBits2::eShaderRead, vk::ImageLayout::eGeneral);
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &post_inpaint,
        });
    }

    const auto fsr_color_image = (use_hudless && *ui_inpaint_pipeline) ? hudless_color.image : color.image;
    const auto color_in = FgImage(fsr_color_image, kColorFormat, width, height,
                                  vk::ImageLayout::eGeneral, kImageUsage);
    FfxVkFsr3_3_1_6FrameGenerationPrepareInfo prepare{};
    prepare.commandBuffer = static_cast<VkCommandBuffer>(cmdbuf);
    prepare.color = color_in;
    // Optical flow runs on true scene color:
    prepare.depth = FgImage(depth_slots[frame_id].r32.image, vk::Format::eR32Sfloat, in.width,
                            in.height, vk::ImageLayout::eGeneral, kImageUsage);
    prepare.motionVectors = FgImage(in.mvec, vk::Format::eR16G16Sfloat, in.width, in.height,
                                    vk::ImageLayout::eShaderReadOnlyOptimal, in.mvec_usage);
    prepare.renderWidth = in.width;
    prepare.renderHeight = in.height;
    prepare.jitterOffsetX = in.jitter[0];
    prepare.jitterOffsetY = in.jitter[1];
    // The game's motion vectors are curUV - prevUV; FSR 3 wants prev - cur, in render pixels
    // before its 1 / render size.
    prepare.motionVectorScaleX = -static_cast<float>(in.width);
    prepare.motionVectorScaleY = -static_cast<float>(in.height);
    prepare.frameTimeMilliseconds = static_cast<float>(interval_ms);
    prepare.minLuminance = 0.0f;
    prepare.maxLuminance = 1.0f;
    prepare.transferFunction = FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB;
    prepare.cameraNear = in.z_near;
    prepare.cameraFar = std::min(in.z_far, 1.0e6f);
    prepare.viewSpaceToMeters = 1.0f;
    prepare.cameraVerticalFovRadians = in.fov_y;
    std::copy_n(in.pos, 3, prepare.cameraPosition);
    std::copy_n(in.up, 3, prepare.cameraUp);
    std::copy_n(in.right, 3, prepare.cameraRight);
    std::copy_n(in.fwd, 3, prepare.cameraForward);
    prepare.frameId = fsr_frame;
    prepare.reset = reset ? VK_TRUE : VK_FALSE;
    recorded = true;
    FfxVkFsr3_3_1_6FrameGenerationResult r =
        ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(context, &prepare);
    if (r == FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        FfxVkFsr3_3_1_6FrameGenerationDispatchInfo dispatch{};
        dispatch.commandBuffer = static_cast<VkCommandBuffer>(cmdbuf);
        dispatch.color = color_in;
        dispatch.output = FgImage(output.image, kColorFormat, width, height,
                                  vk::ImageLayout::eGeneral, kImageUsage);
        dispatch.displayWidth = width;
        dispatch.displayHeight = height;
        dispatch.frameTimeMilliseconds = prepare.frameTimeMilliseconds;
        dispatch.cameraNear = prepare.cameraNear;
        dispatch.cameraFar = prepare.cameraFar;
        dispatch.viewSpaceToMeters = prepare.viewSpaceToMeters;
        dispatch.cameraVerticalFovRadians = prepare.cameraVerticalFovRadians;
        dispatch.minLuminance = prepare.minLuminance;
        dispatch.maxLuminance = prepare.maxLuminance;
        dispatch.transferFunction = prepare.transferFunction;
        dispatch.frameId = fsr_frame;
        dispatch.reset = prepare.reset;
        r = ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(context, &dispatch);
    }

    if (r == FFX_VK_FSR3_3_1_6_FRAMEGEN_OK && use_hudless) {
        // Read back stamped UI pixels and bounding box from previous frame's slot
        const u32 slot_idx = ui_stats_slot;
        const u32 prev_slot_idx = 1 - slot_idx;
        ui_stats_slot = prev_slot_idx;

        if (ui_stats[prev_slot_idx].mapped) {
            const uint32_t count = ui_stats[prev_slot_idx].mapped[0];
            const uint32_t min_x = ui_stats[prev_slot_idx].mapped[1];
            const uint32_t max_x = ui_stats[prev_slot_idx].mapped[2];
            const uint32_t min_y = ui_stats[prev_slot_idx].mapped[3];
            const uint32_t max_y = ui_stats[prev_slot_idx].mapped[4];
            SotcStats::ov_hudless_ui_px.store(count, std::memory_order_relaxed);
            if (count > 0 && min_x <= max_x && min_y <= max_y) {
                SotcStats::ov_hudless_min_x.store(min_x, std::memory_order_relaxed);
                SotcStats::ov_hudless_max_x.store(max_x, std::memory_order_relaxed);
                SotcStats::ov_hudless_min_y.store(min_y, std::memory_order_relaxed);
                SotcStats::ov_hudless_max_y.store(max_y, std::memory_order_relaxed);
            }
        }

        // Initialize current slot's stats buffer via updateBuffer
        if (ui_stats[slot_idx].buffer) {
            const uint32_t init_stats[5] = {0, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0};
            cmdbuf.updateBuffer(ui_stats[slot_idx].buffer, 0, sizeof(init_stats), init_stats);
            const vk::BufferMemoryBarrier2 fill_barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                .buffer = ui_stats[slot_idx].buffer,
                .offset = 0,
                .size = 5 * sizeof(uint32_t),
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .bufferMemoryBarrierCount = 1,
                .pBufferMemoryBarriers = &fill_barrier,
            });
        }

        // Barrier: ensure FSR 3 compute writes to output.image are visible before our UI stamp,
        // and ensure color.image (with authentic static UI) is readable.
        const std::array post_fsr_barriers{
            Barrier(output.image,
                    vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderWrite,
                    vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                    vk::ImageLayout::eGeneral),
            Barrier(color.image,
                    vk::PipelineStageFlagBits2::eAllCommands,
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferWrite,
                    vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead,
                    vk::ImageLayout::eGeneral),
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = static_cast<u32>(post_fsr_barriers.size()),
            .pImageMemoryBarriers = post_fsr_barriers.data(),
        });

        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *ui_mask_pipeline);
        const vk::DescriptorImageInfo mask_info = {
            .imageView = hudless_view,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        };
        const vk::DescriptorImageInfo color_info = {
            .imageView = color.view,
            .imageLayout = vk::ImageLayout::eGeneral,
        };
        const vk::DescriptorImageInfo out_info = {
            .imageView = output.view,
            .imageLayout = vk::ImageLayout::eGeneral,
        };
        const vk::DescriptorBufferInfo buf_info = {
            .buffer = ui_stats[slot_idx].buffer,
            .offset = 0,
            .range = 5 * sizeof(uint32_t),
        };
        const std::array writes{
            vk::WriteDescriptorSet{
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eSampledImage,
                .pImageInfo = &mask_info,
            },
            vk::WriteDescriptorSet{
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eSampledImage,
                .pImageInfo = &color_info,
            },
            vk::WriteDescriptorSet{
                .dstBinding = 2,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageImage,
                .pImageInfo = &out_info,
            },
            vk::WriteDescriptorSet{
                .dstBinding = 3,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &buf_info,
            },
        };
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *ui_mask_layout, 0, writes);
        const u32 params[2] = {width, height};
        cmdbuf.pushConstants(*ui_mask_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), params);
        cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);

        if (ui_stats[slot_idx].buffer) {
            const vk::BufferMemoryBarrier2 post_stats_barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead,
                .buffer = ui_stats[slot_idx].buffer,
                .offset = 0,
                .size = 5 * sizeof(uint32_t),
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .bufferMemoryBarrierCount = 1,
                .pBufferMemoryBarriers = &post_stats_barrier,
            });
        }
    }
    if (use_hudless) {
        if (r == FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
            SotcStats::ov_hudless_state.store(1, std::memory_order_relaxed);
        } else {
            SotcStats::ov_hudless_state.store(2, std::memory_order_relaxed);
            SotcStats::ov_hudless_reason.store(
                static_cast<uint32_t>(SotcStats::HudlessReason::DispatchRejected),
                std::memory_order_relaxed);
        }
    } else {
        SotcStats::ov_hudless_ui_px.store(0, std::memory_order_relaxed);
        SotcStats::ov_hudless_min_x.store(0, std::memory_order_relaxed);
        SotcStats::ov_hudless_max_x.store(0, std::memory_order_relaxed);
        SotcStats::ov_hudless_min_y.store(0, std::memory_order_relaxed);
        SotcStats::ov_hudless_max_y.store(0, std::memory_order_relaxed);
        SotcStats::ov_hudless_state.store(0, std::memory_order_relaxed);
    }
    if (r != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        static u32 errors = 0;
        if (errors++ < 5) {
            std::fprintf(stderr,
                         "SOTC FSR3 FG: recording failed (%d): render %ux%u near %.3f far %.1f "
                         "fov %.3f frame time %.1f\n",
                         static_cast<int>(r), in.width, in.height, prepare.cameraNear,
                         prepare.cameraFar, prepare.cameraVerticalFovRadians,
                         prepare.frameTimeMilliseconds);
        }
        history_game_frame = ~0ULL;
        SetState(2);
        return {};
    }
    const auto to_present = Barrier(output.image, vk::PipelineStageFlagBits2::eComputeShader,
                                    vk::AccessFlagBits2::eShaderWrite, vk::ImageLayout::eGeneral,
                                    vk::PipelineStageFlagBits2::eAllCommands,
                                    vk::AccessFlagBits2::eMemoryRead, vk::ImageLayout::eGeneral);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_present,
    });
    if (reset) {
        SetState(0);
        return {};
    }
    SetState(1);
    if (log_level >= 1 && ++generated - generated_logged >= 600) {
        generated_logged = generated;
        std::fprintf(stderr, "SOTC FSR3 FG: %llu frames generated, game frame every %.1f ms\n",
                     static_cast<unsigned long long>(generated), interval_ms);
    }
    return output.image;
}

void Submitted(Scheduler& scheduler, u64 tick) {
    if (!recorded) {
        return;
    }
    recorded = false;
    submit_scheduler = &scheduler;
    in_flight.emplace_back(fsr_frame, tick);
}

void NotePresent() {
    static Clock::time_point window_start{};
    static std::array<float, 1024> intervals;
    static u32 count = 0;
    const auto now = Clock::now();
    if (now - last_present > std::chrono::seconds(1)) {
        count = 0; // FSR 3 was off: a new measurement
        window_start = now;
    } else if (count < intervals.size()) {
        intervals[count++] = std::chrono::duration<float, std::milli>(now - last_present).count();
    }
    last_present = now;
    if (window_start.time_since_epoch().count() == 0) {
        window_start = now;
    }
    const double seconds = std::chrono::duration<double>(now - window_start).count();
    if (seconds < 5.0) {
        return;
    }
    if (log_level >= 1 && count > 8) {
        std::sort(intervals.begin(), intervals.begin() + count);
        const float median = intervals[count / 2];
        u32 uneven = 0;
        for (u32 i = 0; i < count; ++i) {
            uneven += intervals[i] > median * 1.5f || intervals[i] < median * 0.5f;
        }
        std::fprintf(stderr,
                     "SOTC FSR3 PACING %.1f presents/s, interval p10 %.1f ms, p50 %.1f, p90 %.1f, "
                     "max %.1f; %u uneven (<0.5x or >1.5x p50); generated %.1f ms after the "
                     "game frame\n",
                     count / seconds, intervals[count / 10], median, intervals[count * 9 / 10],
                     intervals[count - 1], uneven,
                     delay_count ? delay_sum_ms / delay_count : 0.0);
        delay_sum_ms = 0.0;
        delay_count = 0;
    }
    count = 0;
    window_start = now;
}

void WaitForGameFrame() {
    delay_sum_ms += std::chrono::duration<double, std::milli>(last_present - frame_arrival).count();
    ++delay_count;
    // sotc: target the real frame at exactly 50% of the game frame interval from arrival,
    // automatically discounting the time spent generating and presenting the interpolated frame.
    const auto due =
        frame_arrival + std::chrono::microseconds(static_cast<s64>(interval_ms * 500.0));
    SleepUntil(due);
}

} // namespace Vulkan::FsrFg
