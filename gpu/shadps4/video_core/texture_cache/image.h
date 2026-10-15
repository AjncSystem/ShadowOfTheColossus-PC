// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/enum.h"
#include "common/incremental_id.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"

#include <atomic>
#include <deque>
#include <optional>
#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

namespace Vulkan {
class Instance;
class Runtime;
} // namespace Vulkan

VK_DEFINE_HANDLE(VmaAllocation)
VK_DEFINE_HANDLE(VmaAllocator)

namespace VideoCore {

enum ImageFlagBits : u32 {
    Empty = 0,
    MaybeCpuDirty = 1 << 0, ///< The page this image is in was touched before the image address
    CpuDirty = 1 << 1,      ///< Contents have been modified from the CPU
    GpuDirty = 1 << 2, ///< Contents have been modified from the GPU (valid data in buffer cache)
    Dirty = MaybeCpuDirty | CpuDirty | GpuDirty,
    GpuModified = 1 << 3, ///< Contents have been modified from the GPU
    Registered = 1 << 6,  ///< True when the image is registered
    Picked = 1 << 7,      ///< Temporary flag to mark the image as picked
};
DECLARE_ENUM_FLAG_OPERATORS(ImageFlagBits)

struct UniqueImage {
    explicit UniqueImage() = default;
    explicit UniqueImage(vk::Device device, VmaAllocator allocator)
        : device{device}, allocator{allocator} {}
    ~UniqueImage();

    UniqueImage(const UniqueImage&) = delete;
    UniqueImage& operator=(const UniqueImage&) = delete;

    UniqueImage(UniqueImage&& other)
        : allocator{std::exchange(other.allocator, VK_NULL_HANDLE)},
          allocation{std::exchange(other.allocation, VK_NULL_HANDLE)},
          image{std::exchange(other.image, VK_NULL_HANDLE)}, image_ci{std::move(other.image_ci)} {}
    UniqueImage& operator=(UniqueImage&& other) {
        image = std::exchange(other.image, VK_NULL_HANDLE);
        allocator = std::exchange(other.allocator, VK_NULL_HANDLE);
        allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
        image_ci = std::move(other.image_ci);
        return *this;
    }

    void Create(const vk::ImageCreateInfo& image_ci);

    void Destroy();

    operator vk::Image() const {
        return image;
    }

    operator bool() const {
        return image;
    }

public:
    vk::Device device{};
    VmaAllocator allocator{};
    VmaAllocation allocation{};
    vk::Image image{};
    vk::ImageCreateInfo image_ci{};
    vk::DeviceSize size_bytes{};
};

struct Image {
    explicit Image(const Vulkan::Instance& instance, Vulkan::Runtime& runtime,
                   Common::SlotVector<ImageView>& slot_image_views, const ImageInfo& info);
    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    Image(Image&&) = default;
    Image& operator=(Image&&) = default;

    bool Overlaps(VAddr overlap_cpu_addr, size_t overlap_size) const noexcept {
        const VAddr overlap_end = overlap_cpu_addr + overlap_size;
        const auto image_addr = info.guest_address;
        const auto image_end = info.guest_address + info.guest_size;
        return image_addr < overlap_end && overlap_cpu_addr < image_end;
    }

    vk::Image GetImage() const {
        return backing->image.image;
    }

    vk::DeviceSize GetHostImageSize() const {
        return backing->image.size_bytes;
    }

    bool IsTracked() {
        return track_addr != 0 && track_addr_end != 0;
    }

    bool SafeToDownload() const {
        return True(flags & ImageFlagBits::GpuModified) && False(flags & (ImageFlagBits::Dirty));
    }

    void AssociateDepth(ImageId depth_image_id, u64 depth_image_uid) {
        depth_id = depth_image_id;
        depth_uid = depth_image_uid;
    }

    void DisassociateDepth() {
        depth_id = {};
        depth_uid = {};
    }

    /// sotc: reuse an unregistered image whose Vulkan image matches new_info: bookkeeping is
    /// reset to what the constructor sets (new uid, Dirty flags, no hashes or associations);
    /// the VkImage, its views and its tracked layout are kept.
    void RenewForReuse(const ImageInfo& new_info);
    /// The Vulkan usage flags an image created for info gets.
    static vk::ImageUsageFlags UsageFlagsFor(const Vulkan::Instance& instance,
                                             const ImageInfo& info);

    ImageView& FindView(const ImageViewInfo& view_info, bool ensure_guest_samples = true);

    using Barriers = boost::container::small_vector<vk::ImageMemoryBarrier2, 32>;
    /// Moves `aspects` (all of the image's aspects when empty) of the subresources in
    /// `subres_range` (all when empty) to `dst_layout` for an access `dst_mask` at `dst_stage`,
    /// adding the barriers that needs.
    void GetBarriers(Barriers& out_barriers, vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                     vk::PipelineStageFlags2 dst_stage,
                     std::optional<SubresourceRange> subres_range = {},
                     vk::ImageAspectFlags aspects = {});

    /// sotc: the depth and stencil aspects of a depth/stencil image are tracked separately (own
    /// layout and accesses, own barriers; separateDepthStencilLayouts), as Vulkan models them.
    /// Tracked as one block, a draw writing stencil over a read-only depth while another samples
    /// the depth flipped the whole image between layouts, two barriers and a render pass break
    /// per draw. SOTC_ASPECT_BARRIERS=0: one state for both aspects and the barrier rule of
    /// before (see NeedsBarrier in image.cpp).
    [[nodiscard]] static bool AspectBarriers();
    [[nodiscard]] bool SplitAspects() const noexcept {
        return AspectBarriers() && (aspect_mask & vk::ImageAspectFlagBits::eDepth) &&
               (aspect_mask & vk::ImageAspectFlagBits::eStencil);
    }
    /// The layout `aspect` (the depth's for both) of the subresource at `range`'s base is in.
    [[nodiscard]] vk::ImageLayout LayoutOf(vk::ImageAspectFlags aspect,
                                           std::optional<SubresourceRange> range = {}) const;

public:
    Vulkan::Runtime* runtime;
    Common::SlotVector<ImageView>* slot_image_views;
    ImageInfo info;
    vk::ImageAspectFlags aspect_mask = vk::ImageAspectFlagBits::eColor;
    vk::SampleCountFlags supported_samples = vk::SampleCountFlagBits::e1;
    ImageFlagBits flags = ImageFlagBits::Dirty;
    /// sotc: flags are changed by the command processor and by guest fault handlers (under the
    /// texture cache mutex), and read without it by the UpdateImage fast path: atomic updates
    /// so one side cannot drop the other's bits.
    void SetFlags(ImageFlagBits bits) noexcept {
        std::atomic_ref<u32>(reinterpret_cast<u32&>(flags))
            .fetch_or(static_cast<u32>(bits), std::memory_order_acq_rel);
    }
    void ClearFlags(ImageFlagBits bits) noexcept {
        std::atomic_ref<u32>(reinterpret_cast<u32&>(flags))
            .fetch_and(~static_cast<u32>(bits), std::memory_order_acq_rel);
    }
    [[nodiscard]] ImageFlagBits LoadFlags() const noexcept {
        return static_cast<ImageFlagBits>(
            std::atomic_ref<u32>(const_cast<u32&>(reinterpret_cast<const u32&>(flags)))
                .load(std::memory_order_acquire));
    }
    /// gc tick of the last LRU touch: one touch per tick is enough (the LRU orders by tick).
    u64 lru_touched_tick = ~0ULL;
    /// sotc: metadata addresses this image last registered in surface_metas, and the
    /// metadata generation at that time (see TextureCache::meta_generation).
    u64 meta_registered_gen = ~0ULL;
    VAddr meta_registered_cmask = 0;
    VAddr meta_registered_fmask = 0;
    u64 meta_registered_gen_depth = ~0ULL;
    VAddr meta_registered_htile = 0;
    VAddr track_addr = 0;
    VAddr track_addr_end = 0;
    ImageId depth_id{};
    u64 depth_uid{};

    vk::ImageUsageFlags usage_flags;
    vk::FormatFeatureFlags2 format_features;
    struct State {
        vk::PipelineStageFlags2 pl_stage = vk::PipelineStageFlagBits2::eAllCommands;
        vk::AccessFlags2 access_mask = vk::AccessFlagBits2::eNone;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
        /// sotc: SotcStats::sync_epoch at the last shader write (see NeedsBarrier, image.cpp).
        u64 write_epoch = ~0ULL;
    };
    struct BackingImage {
        UniqueImage image;
        /// Color, or depth (both aspects when not SplitAspects()).
        State state;
        std::vector<State> subresource_states;
        /// sotc: the stencil aspect when SplitAspects().
        State stencil_state;
        std::vector<State> stencil_subresource_states;
        boost::container::small_vector<ImageViewInfo, 4> image_view_infos;
        boost::container::small_vector<ImageViewId, 4> image_view_ids;
        u32 num_samples;
        u32 last_view = ~0u; ///< Index of the view FindView returned last (search hint).
        /// sotc: changes whenever the states above may have changed (TransitAspect,
        /// SetBackingSamples); equal values mean equal states (Rasterizer::BeginRendering).
        u32 state_version{};
    };
    std::deque<BackingImage> backing_images;
    BackingImage* backing{};
    boost::container::static_vector<u64, 16> mip_hashes{};
    u64 image_uid{};
    u64 lru_id{};
    u64 tick_accessed_last{};
    u64 hash{};

    struct {
        u32 texture : 1;
        u32 storage : 1;
        u32 render_target : 1;
        u32 depth_target : 1;
        u32 vo_surface : 1;
    } usage{};

    struct {
        u32 is_bound : 1;
        u32 is_target : 1;
        u32 needs_rebind : 1;
        u32 force_general : 1;
    } binding{};

private:
    static Common::IncrementalIdProvider<u64> global_image_uid;
};

} // namespace VideoCore
