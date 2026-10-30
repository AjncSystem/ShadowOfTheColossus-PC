// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: AMD FSR 3.1 temporal upscaler, see vk_fsr3up.h.

#include "video_core/renderer_vulkan/vk_fsr3up.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>
#include <vk_mem_alloc.h>

#include <ffx_vk_fsr3_3_1_5_bridge.h>

#include "video_core/renderer_vulkan/vk_dlssg.h"
#include "video_core/renderer_vulkan/vk_fsrfg.h"
#include "video_core/host_shaders/upscale_hud_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/sotc_live.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan::Fsr3Up {

namespace {

constexpr u32 kMaxFrames = 16; // presentation frames, as vk_dlssg.cpp
constexpr vk::Format kColorFormat = vk::Format::eR16G16B16A16Sfloat;
constexpr vk::ImageUsageFlags kUsage =
    vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
    vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
using Clock = std::chrono::steady_clock;

struct OwnedImage {
    vk::Image image;
    VmaAllocation alloc{};
    vk::ImageView view;
    vk::ImageView swapped_view; ///< red and blue exchanged (outputs only)
    FfxVkFsr3_3_1_5Resource resource{}; ///< imported once, with the context
    u32 width{}, height{};
};

struct InFlight {
    u64 frame_id;
    u64 tick;
    FfxVkFsr3_3_1_5Resource color, depth, mvec;
};

const Instance* g_instance{};
bool wanted = false;
bool supported = false;
bool failed = false;
int log_level = 0; // SOTC_FSR3UP_LOG

FfxVkFsr3_3_1_5UpscalerContext* context{};
FfxVkFsr3_3_1_5Bridge* bridge{};
u32 ctx_render_w{}, ctx_render_h{}, ctx_out_w{}, ctx_out_h{};
// The game image of each presentation frame (command processor thread writes, present thread reads).
std::array<OwnedImage, kMaxFrames> colors;
std::array<bool, kMaxFrames> color_ready{};
std::array<bool, kMaxFrames> color_swapped{}; ///< the game image holds red and blue exchanged
std::array<OwnedImage, 3> shared; // dilated depth, dilated motion vectors, previous nearest depth
std::array<OwnedImage, kMaxFrames> outputs;
u64 frame_counter = 0; // dispatches so far, also the SDK frame id
bool shared_ready = false;
Scheduler* retire_scheduler{};
std::deque<InFlight> in_flight;
Clock::time_point last_time{};
float interval_ms = 16.6f;

// sotc: a frame the upscaler skips is shown as the bilinear stretch of the game image (blurry,
// blocky at 720p), with nothing in the log. Counted per reason and reported every 600 frames
// when any frame was skipped (always with SOTC_FSR3UP_LOG=1).
enum Skip { SkipColour, SkipInputs, SkipSize, SkipDlss, SkipError, SkipCount };
std::array<u32, SkipCount> skipped{};
u32 upscaled = 0;

void CountFrame(int skip) {
    if (skip < 0) {
        ++upscaled;
    } else {
        ++skipped[skip];
    }
    u32 total = upscaled;
    u32 missed = 0;
    for (const u32 n : skipped) {
        total += n;
        missed += n;
    }
    if (total < 600) {
        return;
    }
    if (missed || log_level >= 1) {
        std::fprintf(stderr,
                     "SOTC FSR3 upscaler: last %u frames %u upscaled, %u bilinear (no colour %u, no "
                     "depth/motion %u, size %u, DLSS %u, error %u)\n",
                     total, upscaled, missed, skipped[SkipColour], skipped[SkipInputs],
                     skipped[SkipSize], skipped[SkipDlss], skipped[SkipError]);
    }
    upscaled = 0;
    skipped = {};
}

bool CreateImage(OwnedImage& img, vk::Format format, u32 width, u32 height) {
    const VkImageCreateInfo info = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = kUsage,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    const VmaAllocationCreateInfo alloc_info = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
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
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    });
    if (result != vk::Result::eSuccess) {
        vmaDestroyImage(g_instance->GetAllocator(), img.image, img.alloc);
        img = {};
        return false;
    }
    img.view = view;
    // The game's frame is viewed as B8G8R8A8 while its image is R8G8B8A8 (or the other way round):
    // the copy keeps the stored order, so the result is read back through exchanged channels.
    auto [swap_result, swapped] = g_instance->GetDevice().createImageView(vk::ImageViewCreateInfo{
        .image = img.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .components = {vk::ComponentSwizzle::eB, vk::ComponentSwizzle::eG, vk::ComponentSwizzle::eR,
                       vk::ComponentSwizzle::eA},
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    });
    if (swap_result == vk::Result::eSuccess) {
        img.swapped_view = swapped;
    }
    img.width = width;
    img.height = height;
    return true;
}

/// Hands an image to the SDK; layout and state are what it is in at each dispatch.
bool Import(FfxVkFsr3_3_1_5Resource& resource, vk::Image image, u32 width, u32 height,
            vk::Format format, vk::ImageLayout layout, u32 state) {
    FfxVkFsr3_3_1_5ImportedImageDescription d{};
    d.image = static_cast<VkImage>(image);
    d.format = static_cast<VkFormat>(format);
    d.width = width;
    d.height = height;
    d.mipCount = 1;
    d.arrayLayers = 1;
    d.layout = static_cast<VkImageLayout>(layout);
    d.state = state;
    d.usage = static_cast<VkImageUsageFlags>(kUsage);
    resource = ffxVkFsr3_3_1_5BridgeImportImage(bridge, &d);
    return resource.resource != nullptr;
}

void DestroyImage(OwnedImage& img) {
    if (bridge && img.resource.resource) {
        ffxVkFsr3_3_1_5BridgeReleaseImportedImage(bridge, img.resource);
    }
    if (img.view) {
        g_instance->GetDevice().destroyImageView(img.view);
    }
    if (img.swapped_view) {
        g_instance->GetDevice().destroyImageView(img.swapped_view);
    }
    if (img.image) {
        vmaDestroyImage(g_instance->GetAllocator(), img.image, img.alloc);
    }
    img = {};
}

void Retire(bool wait_all) {
    while (!in_flight.empty() && retire_scheduler) {
        const InFlight f = in_flight.front();
        if (wait_all) {
            retire_scheduler->Wait(f.tick);
        } else if (!retire_scheduler->IsFree(f.tick)) {
            break;
        }
        ffxVkFsr3_3_1_5UpscalerContextRetireFrame(context, f.frame_id);
        ffxVkFsr3_3_1_5BridgeReleaseImportedImage(bridge, f.color);
        ffxVkFsr3_3_1_5BridgeReleaseImportedImage(bridge, f.depth);
        ffxVkFsr3_3_1_5BridgeReleaseImportedImage(bridge, f.mvec);
        in_flight.pop_front();
    }
}

void DestroyContext() {
    if (!context) {
        return;
    }
    Retire(true);
    g_instance->GetDevice().waitIdle();
    for (auto& s : shared) {
        DestroyImage(s);
    }
    for (auto& o : outputs) {
        DestroyImage(o);
    }
    ffxVkFsr3_3_1_5UpscalerContextDestroy(context);
    context = nullptr;
    bridge = nullptr;
    shared_ready = false;
}

bool EnsureContext(u32 render_w, u32 render_h, u32 out_w, u32 out_h) {
    if (context && render_w <= ctx_render_w && render_h <= ctx_render_h && out_w == ctx_out_w &&
        out_h == ctx_out_h) {
        return true;
    }
    DestroyContext();
    const FfxVkFsr3_3_1_5UpscalerCreateInfo info = {
        .physicalDevice = static_cast<VkPhysicalDevice>(g_instance->GetPhysicalDevice()),
        .device = static_cast<VkDevice>(g_instance->GetDevice()),
        .maxRenderWidth = render_w,
        .maxRenderHeight = render_h,
        .maxUpscaleWidth = out_w,
        .maxUpscaleHeight = out_h,
        .hdrColorInput = VK_TRUE,
        .autoExposure = VK_TRUE,
    };
    const auto start = Clock::now();
    if (ffxVkFsr3_3_1_5UpscalerContextCreate(&info, &context) != FFX_VK_FSR3_3_1_5_OK) {
        std::fprintf(stderr, "SOTC FSR3 upscaler: context creation failed, off\n");
        context = nullptr;
        return false;
    }
    bridge = ffxVkFsr3_3_1_5UpscalerContextGetBridge(context);
    FfxVkFsr3_3_1_5SharedResourceDescriptions sd{};
    bool ok = ffxVkFsr3_3_1_5UpscalerContextGetSharedResourceDescriptions(context, &sd) ==
              FFX_VK_FSR3_3_1_5_OK;
    const FfxVkFsr3_3_1_5SharedResourceDescription* descs[3] = {
        &sd.dilatedDepth, &sd.dilatedMotionVectors, &sd.reconstructedPrevNearestDepth};
    for (int i = 0; ok && i < 3; ++i) {
        const auto format = static_cast<vk::Format>(descs[i]->format);
        ok = format != vk::Format::eUndefined &&
             CreateImage(shared[i], format, descs[i]->width, descs[i]->height) &&
             Import(shared[i].resource, shared[i].image, shared[i].width, shared[i].height, format,
                    vk::ImageLayout::eGeneral, FFX_VK_FSR3_3_1_5_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (!ok) {
        std::fprintf(stderr, "SOTC FSR3 upscaler: cannot create its images, off\n");
        DestroyContext();
        return false;
    }
    ctx_render_w = render_w;
    ctx_render_h = render_h;
    ctx_out_w = out_w;
    ctx_out_h = out_h;
    FfxVkFsr3_3_1_5MemoryUsage usage{};
    ffxVkFsr3_3_1_5UpscalerContextGetMemoryUsage(context, &usage);
    std::fprintf(stderr, "SOTC FSR3 upscaler: context %ux%u -> %ux%u, %.0f MB, created in %.0f ms\n",
                 render_w, render_h, out_w, out_h, usage.totalUsageInBytes / 1048576.0,
                 std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    return true;
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
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
}

// sotc: the game frame without its UI layer (Rasterizer::TakeHudless) is what gets upscaled;
// upscale_hud.comp then puts the UI back from the game frame. With the UI in the upscaler's input,
// text and bars trailed behind when the camera turned. SOTC_UPSCALER_HUD=0: the game frame, UI and
// all, as before.
std::array<OwnedImage, kMaxFrames> hudless_colors;
std::array<bool, kMaxFrames> hudless_ready{};
vk::ShaderModule hud_shader;
vk::UniqueDescriptorSetLayout hud_set_layout;
vk::UniquePipelineLayout hud_layout;
vk::UniquePipeline hud_pipeline;
vk::UniqueSampler hud_sampler;
bool hud_failed = false;

bool HudSplitAsked() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_UPSCALER_HUD");
        return !(v && v[0] == '0');
    }();
    return on;
}

bool EnsureHudPipeline() {
    if (hud_pipeline || hud_failed) {
        return bool(hud_pipeline);
    }
    hud_failed = true; // until every part below exists
    const vk::Device device = g_instance->GetDevice();
    hud_shader = CompileSPV(UPSCALE_HUD_COMP, device);
    const std::array bindings{
        vk::DescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        vk::DescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
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
    auto [set_result, set_layout] = device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    });
    const vk::PushConstantRange push_range = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = 2 * sizeof(u32),
    };
    if (!hud_shader || set_result != vk::Result::eSuccess) {
        std::fprintf(stderr, "SOTC FSR3 upscaler: UI pass unavailable, the UI is upscaled too\n");
        return false;
    }
    const vk::DescriptorSetLayout layouts[] = {*set_layout};
    auto [layout_result, layout] = device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = layouts,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    });
    auto [sampler_result, sampler] = device.createSamplerUnique({
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    });
    if (layout_result != vk::Result::eSuccess || sampler_result != vk::Result::eSuccess) {
        std::fprintf(stderr, "SOTC FSR3 upscaler: UI pass unavailable, the UI is upscaled too\n");
        return false;
    }
    auto [pipeline_result, pipeline] = device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage{
                    .stage = vk::ShaderStageFlagBits::eCompute,
                    .module = hud_shader,
                    .pName = "main",
                },
                .layout = *layout,
            });
    if (pipeline_result != vk::Result::eSuccess) {
        std::fprintf(stderr, "SOTC FSR3 upscaler: UI pass unavailable, the UI is upscaled too\n");
        return false;
    }
    hud_set_layout = std::move(set_layout);
    hud_layout = std::move(layout);
    hud_sampler = std::move(sampler);
    hud_pipeline = std::move(pipeline);
    hud_failed = false;
    std::fprintf(stderr, "SOTC FSR3 upscaler: the UI layer is put back after upscaling\n");
    return true;
}

/// The upscaled frame without the UI (`out`, General) gets the UI back from the game frame.
void PutUiBack(vk::CommandBuffer cmdbuf, u32 frame_id, const OwnedImage& out, u32 out_width,
               u32 out_height) {
    const auto before = Barrier(out.image, vk::PipelineStageFlagBits2::eAllCommands,
                                vk::AccessFlagBits2::eMemoryWrite, vk::ImageLayout::eGeneral,
                                vk::PipelineStageFlagBits2::eComputeShader,
                                vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                                vk::ImageLayout::eGeneral);
    cmdbuf.pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &before});
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *hud_pipeline);
    const vk::DescriptorImageInfo with_ui = {
        .sampler = *hud_sampler,
        .imageView = colors[frame_id].view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::DescriptorImageInfo without_ui = {
        .sampler = *hud_sampler,
        .imageView = hudless_colors[frame_id].view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::DescriptorImageInfo target = {
        .imageView = out.view,
        .imageLayout = vk::ImageLayout::eGeneral,
    };
    const std::array writes{
        vk::WriteDescriptorSet{
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &with_ui,
        },
        vk::WriteDescriptorSet{
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &without_ui,
        },
        vk::WriteDescriptorSet{
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .pImageInfo = &target,
        },
    };
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *hud_layout, 0, writes);
    const u32 params[2] = {out_width, out_height};
    cmdbuf.pushConstants(*hud_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), params);
    cmdbuf.dispatch((out_width + 7) / 8, (out_height + 7) / 8, 1);
}

} // namespace

void Init(const Instance& instance) {
    g_instance = &instance;
    if (const char* l = std::getenv("SOTC_FSR3UP_LOG")) {
        log_level = std::atoi(l);
    }
    const char* v = std::getenv("SOTC_UPSCALER");
    wanted = v && (_stricmp(v, "fsr3") == 0 || _stricmp(v, "fsr") == 0 || _stricmp(v, "dlss") == 0);
    // The depth conversion pipeline of the frame generation does the depth for both.
    supported = wanted && FsrFg::Supported() &&
                (instance.GetPhysicalDevice().getFormatProperties(kColorFormat).optimalTilingFeatures &
                 vk::FormatFeatureFlagBits::eBlitDst);
    if (wanted) {
        std::fprintf(stderr, "SOTC FSR3 upscaler: %s\n", supported ? "on" : "not supported here");
    }
}

bool Wanted() {
    return supported && !failed && SotcLive::fsr3.load(std::memory_order_relaxed) != 0;
}

bool AskedAtStart() {
    return wanted;
}

float MipBias() {
    static const float bias = [] {
        if (const char* v = std::getenv("SOTC_MIP_BIAS")) {
            return static_cast<float>(std::atof(v));
        }
        if (!supported) {
            return 0.0f;
        }
        // The game's render size (1080p unless SOTC_RENDER_RES) against the output size.
        unsigned render_w = 1920, render_h = 1080, out_w = 0, out_h = 0;
        if (const char* v = std::getenv("SOTC_RENDER_RES")) {
            std::sscanf(v, "%ux%u", &render_w, &render_h);
        }
        if (const char* v = std::getenv("SOTC_OUTPUT_RES")) {
            std::sscanf(v, "%ux%u", &out_w, &out_h);
        }
        if (render_w == 0 || out_w == 0) {
            return 0.0f;
        }
        // sotc: at most -1. Finer mips fill the game's texture page pool: DLSS Performance at 4K
        // (-2) left it 0-1% free and the far ground black and the grass as pale cards; -1 kept the
        // same scene normal (RTX 5060 Ti 8 GB, user test 29 Oct).
        const float value = std::max(
            std::log2(static_cast<float>(render_w) / static_cast<float>(out_w)) - 1.0f, -1.0f);
        if (log_level > 0) {
            std::fprintf(stderr, "SOTC FSR3 upscaler: texture LOD bias %.2f (render %ux%u, output %ux%u)\n",
                         value, render_w, render_h, out_w, out_h);
        }
        return value;
    }();
    return bias;
}

void Prepare(vk::CommandBuffer cmdbuf, u32 frame_id, vk::Image game_image, u32 width, u32 height,
             bool swap_red_blue, vk::Image hudless_image) {
    if (!Wanted() || frame_id >= kMaxFrames) {
        return;
    }
    color_ready[frame_id] = false;
    hudless_ready[frame_id] = false;
    color_swapped[frame_id] = swap_red_blue;
    OwnedImage& color = colors[frame_id];
    if (color.width != width || color.height != height) {
        // Its last reader is done: the frame was handed out again after its present.
        if (color.image) {
            DestroyImage(color);
        }
        if (!CreateImage(color, kColorFormat, width, height)) {
            return;
        }
    }
    const vk::MemoryBarrier2 earlier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    const auto to_dst = Barrier(color.image, vk::PipelineStageFlagBits2::eAllCommands,
                                vk::AccessFlagBits2::eNone, vk::ImageLayout::eUndefined,
                                vk::PipelineStageFlagBits2::eBlit,
                                vk::AccessFlagBits2::eTransferWrite,
                                vk::ImageLayout::eTransferDstOptimal);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1,
                                               .pMemoryBarriers = &earlier,
                                               .imageMemoryBarrierCount = 1,
                                               .pImageMemoryBarriers = &to_dst});
    const vk::ImageBlit blit{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                 vk::Offset3D{static_cast<s32>(width), static_cast<s32>(height), 1}},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                 vk::Offset3D{static_cast<s32>(width), static_cast<s32>(height), 1}},
    };
    cmdbuf.blitImage(game_image, vk::ImageLayout::eTransferSrcOptimal, color.image,
                     vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eNearest);
    const auto to_read = Barrier(color.image, vk::PipelineStageFlagBits2::eBlit,
                                 vk::AccessFlagBits2::eTransferWrite,
                                 vk::ImageLayout::eTransferDstOptimal,
                                 vk::PipelineStageFlagBits2::eAllCommands,
                                 vk::AccessFlagBits2::eShaderRead,
                                 vk::ImageLayout::eShaderReadOnlyOptimal);
    cmdbuf.pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    color_ready[frame_id] = true;

    // The same frame without its UI layer (ShaderReadOnlyOptimal, left so for frame generation).
    OwnedImage& hudless = hudless_colors[frame_id];
    if (!hudless_image || !HudSplitAsked()) {
        return;
    }
    if (hudless.width != width || hudless.height != height) {
        if (hudless.image) {
            DestroyImage(hudless);
        }
        if (!CreateImage(hudless, kColorFormat, width, height)) {
            return;
        }
    }
    const std::array pre{
        Barrier(hudless_image, vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eNone,
                vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eBlit,
                vk::AccessFlagBits2::eTransferRead, vk::ImageLayout::eTransferSrcOptimal),
        Barrier(hudless.image, vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eNone,
                vk::ImageLayout::eUndefined, vk::PipelineStageFlagBits2::eBlit,
                vk::AccessFlagBits2::eTransferWrite, vk::ImageLayout::eTransferDstOptimal),
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = static_cast<u32>(pre.size()),
                                               .pImageMemoryBarriers = pre.data()});
    cmdbuf.blitImage(hudless_image, vk::ImageLayout::eTransferSrcOptimal, hudless.image,
                     vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eNearest);
    const std::array post{
        Barrier(hudless_image, vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eNone,
                vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eAllCommands,
                vk::AccessFlagBits2::eShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal),
        Barrier(hudless.image, vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferWrite,
                vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eAllCommands,
                vk::AccessFlagBits2::eShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal),
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = static_cast<u32>(post.size()),
                                               .pImageMemoryBarriers = post.data()});
    hudless_ready[frame_id] = true;
}

bool SplitsUi() {
    return Wanted() && HudSplitAsked();
}

vk::ImageView Dispatch(vk::CommandBuffer cmdbuf, Scheduler& scheduler, u32 frame_id, u32 out_width,
                       u32 out_height) {
    if (!Wanted()) {
        return {};
    }
    if (frame_id >= kMaxFrames || !color_ready[frame_id]) {
        CountFrame(SkipColour);
        return {};
    }
    // The frame without its UI when Prepare had it: PutUiBack adds the UI after upscaling.
    const bool split_ui = hudless_ready[frame_id] && EnsureHudPipeline();
    const OwnedImage& color = split_ui ? hudless_colors[frame_id] : colors[frame_id];
    const u32 width = color.width, height = color.height;
    Dlssg::FrameInputs in{};
    vk::Image depth;
    u32 depth_w = 0, depth_h = 0;
    if (!Dlssg::GetFrameInputs(frame_id, in) || !FsrFg::DepthImage(frame_id, depth, depth_w, depth_h)) {
        CountFrame(SkipInputs);
        return {};
    }
    if (in.width != width || in.height != height || depth_w != width || depth_h != height) {
        CountFrame(SkipSize);
        return {};
    }
    retire_scheduler = &scheduler;
    // sotc: with DLSS Super Resolution the FSR 3.1 context (its shared images: 64 MB at 720p ->
    // 1080p, 200-250 MB at 4K) was created and never used; DLSS stays on for the whole session.
    const bool dlss = Dlssg::SuperResReady();
    if (!dlss && !EnsureContext(width, height, out_width, out_height)) {
        failed = true;
        CountFrame(SkipError);
        return {};
    }
    Retire(false);
    OwnedImage& out = outputs[frame_id];
    if (out.width != out_width || out.height != out_height) {
        DestroyImage(out);
        if (!CreateImage(out, kColorFormat, out_width, out_height) ||
            (!dlss && !Import(out.resource, out.image, out_width, out_height, kColorFormat,
                    vk::ImageLayout::eGeneral, FFX_VK_FSR3_3_1_5_RESOURCE_STATE_UNORDERED_ACCESS))) {
            std::fprintf(stderr, "SOTC FSR3 upscaler: cannot create the %ux%u output\n", out_width,
                         out_height);
            DestroyImage(out);
            failed = true;
            CountFrame(SkipError);
            return {};
        }
    }
    const auto now = Clock::now();
    if (last_time.time_since_epoch().count() != 0) {
        const float ms = std::chrono::duration<float, std::milli>(now - last_time).count();
        if (ms > 2.0f && ms < 100.0f) {
            interval_ms = interval_ms * 0.85f + ms * 0.15f;
        }
    }
    last_time = now;
    const bool reset = frame_counter == 0 || in.reset;
    const u64 sdk_frame = ++frame_counter;

    // Shared images start in General; the output is fully rewritten.
    const vk::MemoryBarrier2 earlier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    std::vector<vk::ImageMemoryBarrier2> pre;
    pre.push_back(Barrier(out.image, vk::PipelineStageFlagBits2::eAllCommands,
                          vk::AccessFlagBits2::eNone, vk::ImageLayout::eUndefined,
                          vk::PipelineStageFlagBits2::eComputeShader,
                          vk::AccessFlagBits2::eShaderWrite, vk::ImageLayout::eGeneral));
    if (!shared_ready && !dlss) {
        shared_ready = true;
        for (auto& sh : shared) {
            pre.push_back(Barrier(sh.image, vk::PipelineStageFlagBits2::eAllCommands,
                                  vk::AccessFlagBits2::eNone, vk::ImageLayout::eUndefined,
                                  vk::PipelineStageFlagBits2::eComputeShader,
                                  vk::AccessFlagBits2::eShaderWrite, vk::ImageLayout::eGeneral));
        }
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &earlier,
        .imageMemoryBarrierCount = static_cast<u32>(pre.size()),
        .pImageMemoryBarriers = pre.data(),
    });

    if (dlss) {
        // sotc: DLSS Super Resolution (Streamline) instead of FSR 3.1 when asked and available.
        if (Dlssg::SuperResEvaluate(cmdbuf, frame_id, color.image, color.view, kColorFormat, width, height,
                                    out.image, out.view, kColorFormat, out_width, out_height)) {
            if (split_ui) {
                PutUiBack(cmdbuf, frame_id, out, out_width, out_height);
            }
            const auto to_sample_dlss = Barrier(
                out.image, vk::PipelineStageFlagBits2::eAllCommands,
                vk::AccessFlagBits2::eMemoryWrite, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
                vk::AccessFlagBits2::eShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal);
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                                       .pImageMemoryBarriers = &to_sample_dlss});
            CountFrame(-1);
            return color_swapped[frame_id] && out.swapped_view ? out.swapped_view : out.view;
        }
        CountFrame(SkipDlss);
        return {};
    }
    // The frame's colour (fp16, ShaderReadOnlyOptimal), depth (R32F, General) and motion vectors
    // (R16G16F, ShaderReadOnlyOptimal), imported for this dispatch and released once the GPU is done.
    FfxVkFsr3_3_1_5Resource color_res{}, depth_res{}, mvec_res{};
    if (!Import(color_res, color.image, width, height, kColorFormat,
                vk::ImageLayout::eShaderReadOnlyOptimal,
                FFX_VK_FSR3_3_1_5_RESOURCE_STATE_COMPUTE_READ) ||
        !Import(depth_res, depth, width, height, vk::Format::eR32Sfloat, vk::ImageLayout::eGeneral,
                FFX_VK_FSR3_3_1_5_RESOURCE_STATE_COMPUTE_READ) ||
        !Import(mvec_res, in.mvec, width, height, vk::Format::eR16G16Sfloat,
                vk::ImageLayout::eShaderReadOnlyOptimal,
                FFX_VK_FSR3_3_1_5_RESOURCE_STATE_COMPUTE_READ)) {
        static u32 errors = 0;
        if (errors++ < 5) {
            std::fprintf(stderr, "SOTC FSR3 upscaler: cannot import the frame inputs\n");
        }
        for (auto* r : {&color_res, &depth_res, &mvec_res}) {
            if (r->resource) {
                ffxVkFsr3_3_1_5BridgeReleaseImportedImage(bridge, *r);
            }
        }
        CountFrame(SkipError);
        return {};
    }
    FfxVkFsr3_3_1_5UpscalerDispatchInfo d{};
    d.commandBuffer = static_cast<VkCommandBuffer>(cmdbuf);
    d.color = color_res;
    d.depth = depth_res;
    d.motionVectors = mvec_res;
    d.dilatedDepth = shared[0].resource;
    d.dilatedMotionVectors = shared[1].resource;
    d.reconstructedPrevNearestDepth = shared[2].resource;
    d.output = out.resource;
    d.jitterOffsetX = in.jitter[0];
    d.jitterOffsetY = in.jitter[1];
    // The game's motion vectors are curUV - prevUV; FSR 3 wants prev - cur, in render pixels.
    d.motionVectorScaleX = -static_cast<float>(width);
    d.motionVectorScaleY = -static_cast<float>(height);
    d.renderWidth = width;
    d.renderHeight = height;
    d.upscaleWidth = out_width;
    d.upscaleHeight = out_height;
    d.frameTimeMilliseconds = interval_ms;
    d.preExposure = 1.0f;
    d.enableSharpening = VK_FALSE;
    d.reset = reset ? VK_TRUE : VK_FALSE;
    d.cameraNear = in.z_near;
    d.cameraFar = std::min(in.z_far, 1.0e6f);
    d.cameraVerticalFovRadians = in.fov_y;
    d.viewSpaceToMeters = 1.0f;
    d.frameId = sdk_frame;
    const FfxVkFsr3_3_1_5Result r = ffxVkFsr3_3_1_5UpscalerContextRecordDispatch(context, &d);
    in_flight.push_back({sdk_frame, scheduler.CurrentTick(), color_res, depth_res, mvec_res});
    if (r != FFX_VK_FSR3_3_1_5_OK) {
        static u32 errors = 0;
        if (errors++ < 5) {
            std::fprintf(stderr, "SOTC FSR3 upscaler: dispatch failed (%d)\n", static_cast<int>(r));
        }
        CountFrame(SkipError);
        return {};
    }
    if (log_level >= 1 && (sdk_frame % 600 == 1 || sdk_frame < 4)) {
        std::fprintf(stderr,
                     "SOTC FSR3 upscaler: frame %llu %ux%u -> %ux%u jitter %.3f %.3f px near %.3f "
                     "far %.1f fov %.3f reset %d frame time %.1f ms\n",
                     static_cast<unsigned long long>(sdk_frame), width, height, out_width,
                     out_height, in.jitter[0], in.jitter[1], in.z_near, in.z_far, in.fov_y,
                     reset ? 1 : 0, interval_ms);
    }
    if (split_ui) {
        PutUiBack(cmdbuf, frame_id, out, out_width, out_height);
    }
    const auto to_sample = Barrier(out.image, vk::PipelineStageFlagBits2::eComputeShader,
                                   vk::AccessFlagBits2::eShaderWrite, vk::ImageLayout::eGeneral,
                                   vk::PipelineStageFlagBits2::eFragmentShader |
                                       vk::PipelineStageFlagBits2::eComputeShader,
                                   vk::AccessFlagBits2::eShaderRead,
                                   vk::ImageLayout::eShaderReadOnlyOptimal);
    cmdbuf.pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_sample});
    CountFrame(-1);
    return color_swapped[frame_id] && out.swapped_view ? out.swapped_view : out.view;
}

} // namespace Vulkan::Fsr3Up
