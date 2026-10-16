// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <ranges>
#include <unordered_map>
#include "common/assert.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_stats.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "video_core/sotc_stats.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

using namespace Vulkan;

Common::IncrementalIdProvider<u64> Image::global_image_uid{};

static vk::ImageUsageFlags ImageUsageFlags(const Vulkan::Instance& instance,
                                           const ImageInfo& info) {
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferSrc |
                                vk::ImageUsageFlagBits::eTransferDst |
                                vk::ImageUsageFlagBits::eSampled;
    if (!info.props.is_block) {
        if (info.props.is_depth) {
            usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        } else {
            usage |= vk::ImageUsageFlagBits::eColorAttachment;
            if (instance.IsAttachmentFeedbackLoopLayoutSupported()) {
                usage |= vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT;
            }
            // Always create images with storage flag to avoid needing re-creation in case of e.g
            // compute clears This sacrifices a bit of performance but is less work. ExtendedUsage
            // flag is also used. The exception here is for multisample images when storage is not
            // supported, where even with ExtendedUsage we may get only one supported sample back.
            if (info.num_samples == 1 || instance.IsMultisampleStorageImageSupported())
                usage |= vk::ImageUsageFlagBits::eStorage;
        }
    } else {
        // Similarly to above, we specify storage usage. This is typically not supported by
        // compressed formats, but may be used for uncompressed views. In order to satisfy this,
        // we will also specify the extended usage bit.
        usage |= vk::ImageUsageFlagBits::eStorage;
    }

    return usage;
}

static vk::ImageType ConvertImageType(AmdGpu::ImageType type) noexcept {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Cube:
        return vk::ImageType::e2D;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageType::e3D;
    default:
        UNREACHABLE();
    }
}

static vk::FormatFeatureFlags2 FormatFeatureFlags(const vk::ImageUsageFlags usage_flags) {
    vk::FormatFeatureFlags2 feature_flags{};
    if (usage_flags & vk::ImageUsageFlagBits::eTransferSrc) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferSrc;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eTransferDst) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferDst;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eSampled) {
        feature_flags |= vk::FormatFeatureFlagBits2::eSampledImage;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eColorAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eColorAttachment;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eDepthStencilAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eDepthStencilAttachment;
    }
    // Note: StorageImage is intentionally ignored for now since it is always set, and can mess up
    // compatibility checks.
    return feature_flags;
}

UniqueImage::~UniqueImage() {
    if (image) {
        vmaDestroyImage(allocator, image, allocation);
    }
}

void UniqueImage::Destroy() {
    if (image) {
        vmaDestroyImage(allocator, image, allocation);
        image = vk::Image{};
        allocation = {};
    }
}

void UniqueImage::Create(const vk::ImageCreateInfo& image_ci) {
    this->image_ci = image_ci;
    ASSERT(!image);
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    const VkImageCreateInfo image_ci_unsafe = static_cast<VkImageCreateInfo>(image_ci);
    VkImage unsafe_image{};
    VmaAllocationInfo alloc_info{};
    const SotcStats::ScopedTimer alloc_timer{SotcStats::image_alloc_ns};
    VkResult result = vmaCreateImage(allocator, &image_ci_unsafe, &alloc_ci, &unsafe_image,
                                     &allocation, &alloc_info);
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        // Over the video memory budget (loading screens peak before the cache collects old
        // images). Going over budget lets the driver page memory out instead of aborting.
        static bool warned = false;
        if (!std::exchange(warned, true)) {
            LOG_WARNING(Render_Vulkan, "Video memory budget exceeded, allocating images past it");
        }
        VmaAllocationCreateInfo over_budget_ci = alloc_ci;
        over_budget_ci.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        result = vmaCreateImage(allocator, &image_ci_unsafe, &over_budget_ci, &unsafe_image,
                                &allocation, &alloc_info);
    }
    ASSERT_MSG(result == VK_SUCCESS, "Failed allocating image with error {}",
               vk::to_string(vk::Result{result}));
    image = vk::Image{unsafe_image};
    size_bytes = alloc_info.size;
    VmaAllocationInfo2 alloc_info2{};
    vmaGetAllocationInfo2(allocator, allocation, &alloc_info2);
    if (alloc_info2.dedicatedMemory) {
        SotcStats::image_dedicated.fetch_add(1, std::memory_order_relaxed);
    }
}

// sotc: vkGetPhysicalDeviceImageFormatProperties2 takes about 5 ms per call on a Radeon (driver
// 32.0.21045), and the game creates dozens of images in a frame when the camera turns: frames of
// 200-500 ms. The answer only depends on these parameters, so it is asked once per combination
// (and the unsupported-format error is logged once).
struct FormatQuery {
    vk::Result result;
    vk::SampleCountFlags sample_counts;
};
static FormatQuery QueryImageFormat(const Vulkan::Instance& instance,
                                    const vk::PhysicalDeviceImageFormatInfo2& format_info) {
    static std::mutex mutex;
    // Exact key (the tiling is always optimal here): format and type in one word, flags and
    // usage in the other.
    struct KeyHash {
        size_t operator()(const std::pair<u64, u64>& k) const noexcept {
            return std::hash<u64>{}(k.first * 0x9E3779B97F4A7C15ULL ^ k.second);
        }
    };
    static std::unordered_map<std::pair<u64, u64>, FormatQuery, KeyHash> answers;
    const std::pair<u64, u64> key{
        (u64(format_info.format) << 32) | u64(format_info.type),
        (u64(u32(format_info.flags)) << 32) | u64(u32(format_info.usage))};
    std::scoped_lock lock{mutex};
    if (const auto it = answers.find(key); it != answers.end()) {
        return it->second;
    }
    const SotcStats::ScopedTimer query_timer{SotcStats::image_query_ns};
    const auto properties = instance.GetPhysicalDevice().getImageFormatProperties2(format_info);
    if (properties.result == vk::Result::eErrorFormatNotSupported) {
        LOG_ERROR(Render_Vulkan, "image format {} type {} is not supported (flags {}, usage {})",
                  vk::to_string(format_info.format), vk::to_string(format_info.type),
                  vk::to_string(format_info.flags), vk::to_string(format_info.usage));
    }
    const FormatQuery answer{properties.result,
                             properties.result == vk::Result::eSuccess
                                 ? properties.value.imageFormatProperties.sampleCounts
                                 : vk::SampleCountFlags{}};
    answers.emplace(key, answer);
    return answer;
}

Image::Image(const Vulkan::Instance& instance, Vulkan::Runtime& runtime_,
             Common::SlotVector<ImageView>& slot_image_views_, const ImageInfo& info_)
    : runtime{&runtime_}, slot_image_views{&slot_image_views_}, info{info_} {
    if (info.pixel_format == vk::Format::eUndefined) {
        return;
    }

    image_uid = global_image_uid.Next();
    mip_hashes.resize(info.resources.levels);
    vk::ImageCreateFlags flags{vk::ImageCreateFlagBits::eMutableFormat |
                               vk::ImageCreateFlagBits::eExtendedUsage};
    if (info.props.is_volume) {
        flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;
        if (instance.Is2dViewOf3dSupported()) {
            flags |= vk::ImageCreateFlagBits::e2DViewCompatibleEXT;
        }
    }
    if (info.props.is_block && instance.IsBlockTexelViewSupported()) {
        flags |= vk::ImageCreateFlagBits::eBlockTexelViewCompatible;
    }

    usage_flags = ImageUsageFlags(instance, info);
    format_features = FormatFeatureFlags(usage_flags);
    if (info.props.is_depth) {
        aspect_mask = vk::ImageAspectFlagBits::eDepth;
        if (info.props.has_stencil) {
            aspect_mask |= vk::ImageAspectFlagBits::eStencil;
        }
    }

    constexpr auto tiling = vk::ImageTiling::eOptimal;
    const auto supported_format = instance.GetSupportedFormat(info.pixel_format, format_features);
    const vk::PhysicalDeviceImageFormatInfo2 format_info{
        .format = supported_format,
        .type = ConvertImageType(info.type),
        .tiling = tiling,
        .usage = usage_flags,
        .flags = flags,
    };
    const FormatQuery format_query = QueryImageFormat(instance, format_info);
    supported_samples = format_query.result == vk::Result::eSuccess
                            ? format_query.sample_counts
                            : vk::SampleCountFlagBits::e1;

    const vk::ImageCreateInfo image_ci = {
        .flags = flags,
        .imageType = ConvertImageType(info.type),
        .format = supported_format,
        .extent{
            .width = info.size.width,
            .height = info.size.height,
            .depth = info.size.depth,
        },
        .mipLevels = static_cast<u32>(info.resources.levels),
        .arrayLayers = static_cast<u32>(info.resources.layers),
        .samples = LiverpoolToVK::NumSamples(info.num_samples, supported_samples),
        .tiling = tiling,
        .usage = usage_flags,
        .initialLayout = vk::ImageLayout::eUndefined,
    };

    backing = &backing_images.emplace_back();
    backing->num_samples = info.num_samples;
    backing->image = UniqueImage{instance.GetDevice(), instance.GetAllocator()};
    backing->image.Create(image_ci);

    Vulkan::SetObjectName(instance.GetDevice(), GetImage(),
                          "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{}", info.size.width,
                          info.size.height, info.size.depth, AmdGpu::NameOf(info.tile_mode),
                          vk::to_string(info.pixel_format), info.guest_address, info.guest_size,
                          info.resources.layers, info.resources.levels, info.num_samples);
}

Image::~Image() = default;

vk::ImageUsageFlags Image::UsageFlagsFor(const Vulkan::Instance& instance,
                                         const ImageInfo& info) {
    return ImageUsageFlags(instance, info);
}

void Image::RenewForReuse(const ImageInfo& new_info) {
    info = new_info;
    flags = ImageFlagBits::Dirty; // unregistered: no other thread can reach it
    lru_touched_tick = ~0ULL;
    meta_registered_gen = ~0ULL;
    meta_registered_cmask = 0;
    meta_registered_fmask = 0;
    meta_registered_gen_depth = ~0ULL;
    meta_registered_htile = 0;
    track_addr = 0;
    track_addr_end = 0;
    depth_id = {};
    depth_uid = {};
    mip_hashes.clear();
    image_uid = global_image_uid.Next();
    lru_id = 0;
    tick_accessed_last = 0;
    hash = 0;
    usage = {};
    binding = {};
}

ImageView& Image::FindView(const ImageViewInfo& view_info, bool ensure_guest_samples) {
    if (ensure_guest_samples && backing->num_samples > 1 != info.num_samples > 1) {
        runtime->SetBackingSamples(this, info.num_samples);
    }
    const auto& view_infos = backing->image_view_infos;
    // Consecutive draws mostly bind the same view: try the last one before the linear search.
    if (backing->last_view < view_infos.size() && view_infos[backing->last_view] == view_info) {
        return (*slot_image_views)[backing->image_view_ids[backing->last_view]];
    }
    const auto it = std::ranges::find(view_infos, view_info);
    if (it != view_infos.end()) {
        backing->last_view = static_cast<u32>(std::distance(view_infos.begin(), it));
        const auto view_id = backing->image_view_ids[backing->last_view];
        return (*slot_image_views)[view_id];
    }
    const auto view_id = slot_image_views->insert(runtime->GetInstance(), view_info, *this);
    backing->image_view_infos.emplace_back(view_info);
    backing->image_view_ids.emplace_back(view_id);
    backing->last_view = static_cast<u32>(backing->image_view_infos.size() - 1);
    return (*slot_image_views)[view_id];
}

static bool EnvOn(const char* name) {
    const char* v = std::getenv(name);
    return !v || v[0] != '0';
}

// Read once at startup: these are checked for every image bound to every draw.
static const bool g_aspect_barriers = EnvOn("SOTC_ASPECT_BARRIERS");
static const bool g_game_sync = EnvOn("SOTC_GAME_SYNC");

bool Image::AspectBarriers() {
    return g_aspect_barriers;
}

/// sotc: whether an aspect in `state` (its layout, and the accesses since its last barrier) needs
/// a barrier before an access `new_mask` in `new_layout`: a layout transition, or a hazard, i.e.
/// a write on either side together with an access that is not an attachment access. Reads after
/// reads need none, and accesses to one attachment in one layout are ordered among themselves
/// (rasterization order; the tracking always assumed it for an attachment used again with the
/// same accesses). Before, any change of the access mask cost a barrier, and inside a render pass
/// a break. With SOTC_ASPECT_BARRIERS=0, that rule again.
static bool NeedsBarrier(const Image::State& state, vk::ImageLayout new_layout,
                         vk::AccessFlags2 new_mask) {
    if (state.layout != new_layout) {
        return true;
    }
    if (!Image::AspectBarriers()) {
        constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                     vk::AccessFlagBits2::eShaderWrite |
                                     vk::AccessFlagBits2::eMemoryWrite;
        return state.access_mask != new_mask || bool(state.access_mask & write_flags);
    }
    constexpr auto any_write =
        vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eShaderWrite |
        vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eMemoryWrite |
        vk::AccessFlagBits2::eColorAttachmentWrite |
        vk::AccessFlagBits2::eDepthStencilAttachmentWrite | vk::AccessFlagBits2::eHostWrite;
    constexpr auto attachment =
        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite |
        vk::AccessFlagBits2::eDepthStencilAttachmentRead |
        vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    const auto both = state.access_mask | new_mask;
    if (!(both & any_write) || !(both & ~attachment)) {
        return false;
    }
    // sotc: shader accesses to a storage image (General) with no synchronization command of the
    // game since the last shader write: the PS4 GPU does not order them either (draws and
    // dispatches overlap, and UAV writes are not ordered between them). The game orders them with
    // cache flushes, ACQUIRE_MEM, end-of-shader and end-of-pipe events, waits on memory and
    // partial flushes, which advance SotcStats::sync_epoch. Without this, the heavy frames paid
    // ~630 barriers (each breaking a render pass) where the game asked for none and ~18 where it
    // synchronized. SOTC_GAME_SYNC=0: a barrier between any of them.
    constexpr auto shader_access =
        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
        vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageRead |
        vk::AccessFlagBits2::eShaderStorageWrite;
    if (g_game_sync && new_layout == vk::ImageLayout::eGeneral && !(both & ~shader_access) &&
        state.write_epoch == SotcStats::sync_epoch.load(std::memory_order_relaxed)) {
        return false;
    }
    return true;
}

/// One aspect (or all, for images tracked as one block) of `image`: its whole-image state and
/// per-subresource states.
static void TransitAspect(Image& image, Image::State& last_state,
                           std::vector<Image::State>& subresource_states,
                           vk::ImageAspectFlags aspect, Image::Barriers& barriers,
                           vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                           vk::PipelineStageFlags2 dst_stage,
                           std::optional<SubresourceRange> subres_range) {
    const auto& resources = image.info.resources;
    constexpr auto shader_write =
        vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderStorageWrite;
    const u64 epoch = SotcStats::sync_epoch.load(std::memory_order_relaxed);
    // The common case (every binding of every draw): the aspect is in dst_layout with these
    // accesses already recorded and needs no barrier. Nothing changes, and a partial range need
    // not split the state into subresources that would all be equal.
    const auto unchanged = [&](const Image::State& state) {
        return state.layout == dst_layout && (state.access_mask & dst_mask) == dst_mask &&
               (state.pl_stage & dst_stage) == dst_stage &&
               (!(dst_mask & shader_write) || state.write_epoch == epoch) &&
               !NeedsBarrier(state, dst_layout, dst_mask);
    };
    const bool needs_partial_transition =
        subres_range &&
        (subres_range->base != SubresourceBase{} || subres_range->extent != resources);
    const bool partially_transited = !subresource_states.empty();
    if (!partially_transited && unchanged(last_state)) {
        return;
    }
    const u32 mip_begin = needs_partial_transition ? subres_range->base.level : 0;
    const u32 mip_end =
        needs_partial_transition ? mip_begin + subres_range->extent.levels : resources.levels;
    const u32 layer_begin = needs_partial_transition ? subres_range->base.layer : 0;
    const u32 layer_end =
        needs_partial_transition ? layer_begin + subres_range->extent.layers : resources.layers;
    // sotc: the same for an image tracked per subresource (one sampled through a view of some of
    // its mips while the others are in another layout): when every subresource of the range is
    // already where this access needs it, nothing changes either. Before, every such binding took
    // the per-subresource walk and changed the state version though the state stayed the same.
    if (partially_transited) {
        bool all_unchanged = true;
        for (u32 mip = mip_begin; all_unchanged && mip < mip_end; ++mip) {
            for (u32 layer = layer_begin; layer < layer_end; ++layer) {
                const auto subres_idx = mip * resources.layers + layer;
                if (subres_idx >= subresource_states.size() ||
                    !unchanged(subresource_states[subres_idx])) {
                    all_unchanged = false;
                    break;
                }
            }
        }
        if (all_unchanged) {
            return;
        }
    }
    ++image.backing->state_version;
    SotcStats::Bump(SotcStats::image_state_changes[partially_transited ? 1 : 0]);
    const auto apply = [&](Image::State& state, u32 mip, u32 levels, u32 layer, u32 layers) {
        if (!NeedsBarrier(state, dst_layout, dst_mask)) {
            // No transition and no hazard: later accesses must wait for these ones as well. The
            // same accesses again (most bindings) change nothing.
            if ((state.access_mask & dst_mask) != dst_mask) {
                state.access_mask |= dst_mask;
            }
            if ((state.pl_stage & dst_stage) != dst_stage) {
                state.pl_stage |= dst_stage;
            }
            if ((dst_mask & shader_write) && state.write_epoch != epoch) {
                state.write_epoch = epoch;
            }
            return;
        }
        barriers.emplace_back(vk::ImageMemoryBarrier2{
            .srcStageMask = state.pl_stage,
            .srcAccessMask = state.access_mask,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_mask,
            .oldLayout = state.layout,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image.GetImage(),
            .subresourceRange{
                .aspectMask = aspect,
                .baseMipLevel = mip,
                .levelCount = levels,
                .baseArrayLayer = layer,
                .layerCount = layers,
            },
        });
        state.layout = dst_layout;
        state.access_mask = dst_mask;
        state.pl_stage = dst_stage;
        state.write_epoch = (dst_mask & shader_write) ? epoch : ~0ULL;
    };

    if (!needs_partial_transition && !partially_transited) {
        apply(last_state, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS);
        return;
    }
    if (!partially_transited) {
        subresource_states.resize(resources.levels * resources.layers);
        std::fill(subresource_states.begin(), subresource_states.end(), last_state);
    }
    // A partial transition changes the given subresources only; a full one brings them all to
    // the same state, after which one state stands for the whole image again.
    for (u32 mip = mip_begin; mip < mip_end; ++mip) {
        for (u32 layer = layer_begin; layer < layer_end; ++layer) {
            // NOTE: these loops may produce a lot of small barriers.
            // If this becomes a problem, we can optimize it by merging adjacent barriers.
            const auto subres_idx = mip * resources.layers + layer;
            ASSERT(subres_idx < subresource_states.size());
            apply(subresource_states[subres_idx], mip, 1, layer, 1);
        }
    }
    if (!needs_partial_transition) {
        // Every subresource is in dst_layout now; the whole-image state gathers their accesses
        // (they may differ where reads were merged without a barrier).
        Image::State whole{.pl_stage = vk::PipelineStageFlagBits2::eNone,
                           .access_mask = vk::AccessFlagBits2::eNone,
                           .layout = dst_layout};
        for (const auto& state : subresource_states) {
            whole.pl_stage |= state.pl_stage;
            whole.access_mask |= state.access_mask;
            if (state.write_epoch == epoch) {
                whole.write_epoch = epoch;
            }
        }
        last_state = whole;
        subresource_states.clear();
    } else if (std::all_of(subresource_states.begin() + 1, subresource_states.end(),
                           [&](const Image::State& state) {
                               const auto& first = subresource_states.front();
                               return state.layout == first.layout &&
                                      state.access_mask == first.access_mask &&
                                      state.pl_stage == first.pl_stage &&
                                      state.write_epoch == first.write_epoch;
                           })) {
        // sotc: a partial transition that brought the last different subresources to the state
        // of the others: one state stands for the whole image again (the same state, so nothing
        // a later access sees changes).
        last_state = subresource_states.front();
        subresource_states.clear();
    }
}

void Image::GetBarriers(Barriers& barriers, vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                        vk::PipelineStageFlags2 dst_stage,
                        std::optional<SubresourceRange> subres_range,
                        vk::ImageAspectFlags aspects) {
    if (!SplitAspects()) {
        TransitAspect(*this, backing->state, backing->subresource_states, aspect_mask, barriers,
                       dst_layout, dst_mask, dst_stage, subres_range);
        return;
    }
    if (!aspects) {
        aspects = aspect_mask;
    }
    const size_t first = barriers.size();
    if (aspects & vk::ImageAspectFlagBits::eDepth) {
        TransitAspect(*this, backing->state, backing->subresource_states,
                       vk::ImageAspectFlagBits::eDepth, barriers, dst_layout, dst_mask, dst_stage,
                       subres_range);
    }
    const size_t depth_end = barriers.size();
    if (aspects & vk::ImageAspectFlagBits::eStencil) {
        TransitAspect(*this, backing->stencil_state, backing->stencil_subresource_states,
                       vk::ImageAspectFlagBits::eStencil, barriers, dst_layout, dst_mask,
                       dst_stage, subres_range);
    }
    // Both aspects moved the same way (whole-image uses: copies, clears): one barrier each pair.
    if (depth_end - first == barriers.size() - depth_end) {
        bool same = true;
        for (size_t i = 0; same && i < depth_end - first; ++i) {
            const auto& d = barriers[first + i];
            const auto& s = barriers[depth_end + i];
            same = d.oldLayout == s.oldLayout && d.srcStageMask == s.srcStageMask &&
                   d.srcAccessMask == s.srcAccessMask &&
                   d.subresourceRange.baseMipLevel == s.subresourceRange.baseMipLevel &&
                   d.subresourceRange.levelCount == s.subresourceRange.levelCount &&
                   d.subresourceRange.baseArrayLayer == s.subresourceRange.baseArrayLayer &&
                   d.subresourceRange.layerCount == s.subresourceRange.layerCount;
        }
        if (same && depth_end != first) {
            for (size_t i = first; i < depth_end; ++i) {
                barriers[i].subresourceRange.aspectMask |= vk::ImageAspectFlagBits::eStencil;
            }
            barriers.resize(depth_end);
        }
    }
}

vk::ImageLayout Image::LayoutOf(vk::ImageAspectFlags aspect,
                                std::optional<SubresourceRange> range) const {
    const bool stencil = SplitAspects() && aspect == vk::ImageAspectFlagBits::eStencil;
    const auto& state = stencil ? backing->stencil_state : backing->state;
    const auto& subresource_states =
        stencil ? backing->stencil_subresource_states : backing->subresource_states;
    if (subresource_states.empty()) {
        return state.layout;
    }
    const u32 mip = range ? range->base.level : 0;
    const u32 layer = range ? range->base.layer : 0;
    const u32 index = mip * info.resources.layers + layer;
    return index < subresource_states.size() ? subresource_states[index].layout : state.layout;
}

} // namespace VideoCore
