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
          image{std::exchange(other.image, VK_NULL_HANDLE)}, image_ci{std::move(other.image_ci)},
          size_bytes{std::exchange(other.size_bytes, 0)} {}
    UniqueImage& operator=(UniqueImage&& other) {
        // sotc: release an image this one still holds (it was overwritten and leaked before).
        Destroy();
        device = other.device;
        image = std::exchange(other.image, VK_NULL_HANDLE);
        allocator = std::exchange(other.allocator, VK_NULL_HANDLE);
        allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
        image_ci = std::move(other.image_ci);
        size_bytes = std::exchange(other.size_bytes, 0);
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

    ImageView& FindView(const ImageViewInfo& view_info, bool ensure_guest_samples = true);

    using Barriers = boost::container::small_vector<vk::ImageMemoryBarrier2, 32>;
    void GetBarriers(Barriers& out_barriers, vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                     vk::PipelineStageFlags2 dst_stage,
                     std::optional<SubresourceRange> subres_range = {});

public:
    Vulkan::Runtime* runtime;
    Common::SlotVector<ImageView>* slot_image_views;
    ImageInfo info;
    vk::ImageAspectFlags aspect_mask = vk::ImageAspectFlagBits::eColor;
    vk::SampleCountFlags supported_samples = vk::SampleCountFlagBits::e1;
    ImageFlagBits flags = ImageFlagBits::Dirty;
    VAddr track_addr = 0;
    VAddr track_addr_end = 0;
    ImageId depth_id{};
    /// sotc: the depth/colour twin parked by ResolveDepthOverlap (unregistered, untracked, kept
    /// for the next conversion back); deleted together with this image.
    ImageId parked_twin{};
    u64 depth_uid{};

    vk::ImageUsageFlags usage_flags;
    vk::FormatFeatureFlags2 format_features;
    struct State {
        vk::PipelineStageFlags2 pl_stage = vk::PipelineStageFlagBits2::eAllCommands;
        vk::AccessFlags2 access_mask = vk::AccessFlagBits2::eNone;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    };
    struct BackingImage {
        UniqueImage image;
        State state;
        std::vector<State> subresource_states;
        boost::container::small_vector<ImageViewInfo, 4> image_view_infos;
        boost::container::small_vector<ImageViewId, 4> image_view_ids;
        u32 num_samples;
        // sotc: index of the view FindView returned last (SOTC_TEX_BIND_P1). The view lists
        // only grow and their entries never change, so an index stays valid for the backing's
        // lifetime; ~0u = none yet.
        u32 last_view = ~0u;
    };
    std::deque<BackingImage> backing_images;
    BackingImage* backing{};
    boost::container::static_vector<u64, 16> mip_hashes{};
    u64 image_uid{};
    /// sotc: a new identity for a reused (unparked) image: associations made with the old uid
    /// (stencil -> depth) no longer match, as with a newly created image.
    void RenewIdentity() {
        image_uid = global_image_uid.Next();
    }
    u64 lru_id{};
    u64 tick_accessed_last{};
    u64 hash{};
    /// sotc: host bytes this full chain would save as a mip tail (census "projected" only).
    u64 sotc_shadow_saving{};

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

/// sotc: moves when an image first gains GPU writes outside a writer FindImage (copy and clear
/// fast paths): memoized mip-tail decisions are made again (TextureCache::MemoLookup), since
/// they refuse a tail that a GPU-written image overlaps.
inline std::atomic<u64> gpu_modified_epoch{0};

/// sotc: counts and reports a GPU write to a mip-tail image (they must never be written).
void NoteMipTailWrite(const Image& image);

inline void MarkGpuModified(Image& image, bool bump_epoch = true) {
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        if (bump_epoch) {
            gpu_modified_epoch.fetch_add(1, std::memory_order_relaxed);
        }
        if (image.info.sotc_first_mip) {
            NoteMipTailWrite(image);
        }
    }
    image.flags |= ImageFlagBits::GpuModified;
}

} // namespace VideoCore
