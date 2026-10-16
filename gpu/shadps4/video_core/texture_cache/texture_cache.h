// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <boost/container/small_vector.hpp>
#include <queue>
#include <tsl/robin_map.h>

#include "common/lru_cache.h"
#include "common/ordered_work_queue.h"
#include "common/thread.h"
#include "common/multi_level_page_table.h"
#include "common/slot_vector.h"
#include "shader_recompiler/resource.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/sampler.h"
#include "video_core/texture_cache/tile_manager.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {
class Runtime;
}

namespace VideoCore {

class BufferCache;
class PageManager;

class TextureCache {
    // Default values for garbage collection
    static constexpr s64 DEFAULT_PRESSURE_GC_MEMORY = 1_GB + 512_MB;
    static constexpr s64 DEFAULT_CRITICAL_GC_MEMORY = 3_GB;
    static constexpr s64 TARGET_GC_THRESHOLD = 8_GB;

    using ImageIds = boost::container::small_vector<ImageId, 16>;

    struct Traits {
        using Entry = ImageIds;
        static constexpr size_t ADDRESS_SPACE_BITS = 40;
        static constexpr size_t L1_BITS = 10;
        static constexpr size_t PAGE_BITS = 20;
        static constexpr bool NULL_CHECK = true;
    };
    using PageTable = Common::MultiLevelPageTable<Traits>;

public:
    enum class BindingType : u32 {
        Texture,
        Storage,
        RenderTarget,
        DepthTarget,
        VideoOut,
    };

    struct ImageDesc {
        ImageInfo info;
        ImageViewInfo view_info;
        BindingType type{BindingType::Texture};

        ImageDesc() = default;
        ImageDesc(const AmdGpu::Image& image, const Shader::ImageResource& desc)
            : info{image, desc}, view_info{image, desc},
              type{desc.is_written ? BindingType::Storage : BindingType::Texture} {}
        ImageDesc(const AmdGpu::ColorBuffer& buffer, AmdGpu::CbDbExtent hint)
            : info{buffer, hint}, view_info{buffer}, type{BindingType::RenderTarget} {}
        ImageDesc(const AmdGpu::DepthBuffer& buffer, AmdGpu::DepthView view,
                  AmdGpu::DepthControl ctl, VAddr htile_address, AmdGpu::CbDbExtent hint,
                  bool write_buffer = false)
            : info{buffer, view.NumSlices(), htile_address, hint, write_buffer},
              view_info{buffer, view, ctl}, type{BindingType::DepthTarget} {}
        ImageDesc(const Libraries::VideoOut::BufferAttributeGroup& group, VAddr cpu_address)
            : info{group, cpu_address}, type{BindingType::VideoOut} {}
    };

public:
    TextureCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                 Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool, BufferCache& buffer_cache,
                 PageManager& tracker);
    ~TextureCache();

    TileManager& GetTileManager() noexcept {
        return tile_manager;
    }

    /// Frees cached images not used in the last few frames, regardless of memory thresholds.
    /// Called when a video memory allocation fails.
    void EmergencyCollect();

    /// Invalidates any image in the logical page range.
    void InvalidateMemory(VAddr addr, size_t size);

    /// Marks an image as dirty if it exists at the provided address.
    void InvalidateMemoryFromGPU(VAddr address, size_t max_size);

    /// Evicts any images that overlap the unmapped range.
    void UnmapMemory(VAddr cpu_addr, size_t size);

    /// sotc: false when no image lies in the range (a lock-free look at the registered images).
    [[nodiscard]] bool HasImagesIn(VAddr addr, size_t size) const {
        return MayHaveImages(addr, size);
    }

    /// Schedules a copy of pending images for download back to CPU memory.
    void ProcessDownloadImages();

    /// Retrieves the image handle of the image with the provided attributes.
    [[nodiscard]] ImageId FindImage(ImageDesc& desc, bool exact_fmt = false);

    /// Retrieves image whose address matches provided
    /// sotc: `any_at_address` (optional) tells whether an image starts at `address` in the
    /// range at all, whatever `ensure_valid` then filtered out.
    [[nodiscard]] ImageId FindImageFromRange(VAddr address, size_t size, bool ensure_valid = true,
                                             bool* any_at_address = nullptr);

    /// Retrieves an image view with the properties of the specified image id.
    [[nodiscard]] ImageView& FindTexture(ImageId image_id, const ImageDesc& desc);

    /// Retrieves the render target with specified properties
    [[nodiscard]] ImageView& FindRenderTarget(ImageId image_id, const ImageDesc& desc);

    /// Retrieves the depth target with specified properties
    [[nodiscard]] ImageView& FindDepthTarget(ImageId image_id, const ImageDesc& desc);

    /// Updates image contents if it was modified by CPU.
    void UpdateImage(ImageId image_id) {
        // sotc: most binds find the image clean, registered, fully tracked and already touched
        // this tick; TrackImage, TouchImage and RefreshImage would then do nothing. Skip the
        // mutex the guest fault handlers contend for. A fault dirtying the image right after
        // this check is the same as one taking the mutex right after the locked path.
        if (TexFastPath()) {
            const Image& image = slot_images[image_id];
            const ImageFlagBits flags = image.LoadFlags();
            if (True(flags & ImageFlagBits::Registered) && False(flags & ImageFlagBits::Dirty) &&
                std::atomic_ref<VAddr>(const_cast<VAddr&>(image.track_addr)).load(std::memory_order_relaxed) ==
                    image.info.guest_address &&
                std::atomic_ref<VAddr>(const_cast<VAddr&>(image.track_addr_end))
                        .load(std::memory_order_relaxed) ==
                    image.info.guest_address + image.info.guest_size &&
                image.lru_touched_tick == gc_tick) {
                return;
            }
        }
        std::scoped_lock lk{mutex};
        Image& image = slot_images[image_id];
        TrackImage(image_id);
        TouchImage(image);
        RefreshImage(image);
    }

    /// Resolves overlap between existing cache image and pending merged image
    [[nodiscard]] std::tuple<ImageId, int, int> ResolveOverlap(const ImageInfo& info,
                                                               BindingType binding,
                                                               ImageId cache_img_id,
                                                               ImageId merged_image_id);

    /// Resolves depth overlap and either re-creates the image or returns existing one
    [[nodiscard]] ImageId ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                              ImageId cache_img_id);

    /// Creates a new image with provided image info and copies subresources from image_id
    [[nodiscard]] ImageId ExpandImage(const ImageInfo& info, ImageId image_id);

    /// Reuploads image contents.
    void RefreshImage(Image& image);

    /// Retrieves the sampler that matches the provided S# descriptor.
    [[nodiscard]] vk::Sampler GetSampler(const AmdGpu::Sampler& sampler,
                                         AmdGpu::BorderColorBuffer border_color_base,
                                         bool is_depth);

    /// SOTC_TEX_FASTPATH=0 disables the lock-free UpdateImage path and the once-per-tick
    /// LRU touch (A/B).
    static bool TexFastPath() {
        static const bool enabled = [] {
            const char* v = std::getenv("SOTC_TEX_FASTPATH");
            return !v || *v != '0';
        }();
        return enabled;
    }

    /// Retrieves the image with the specified id.
    [[nodiscard]] Image& GetImage(ImageId id) {
        auto& image = slot_images[id];
        TouchImage(image);
        return image;
    }

    /// Retrieves the image view with the specified id.
    [[nodiscard]] ImageView& GetImageView(ImageId id) {
        return slot_image_views[id];
    }

    /// Get the associated depth stencil image if it is still valid.
    ImageId GetAssociatedDepth(Image& image) {
        if (!image.depth_id) {
            return {};
        }
        if (slot_images.is_allocated(image.depth_id)) {
            auto& depth_image = slot_images[image.depth_id];
            if (depth_image.image_uid == image.depth_uid &&
                depth_image.flags & ImageFlagBits::Registered) {
                return image.depth_id;
            }
        }
        // The linked depth image is no longer valid, disassociate it.
        image.DisassociateDepth();
        return {};
    }

    enum class MetaType {
        CMask,
        FMask,
        HTile,
    };

    /// Returns meta type if the specified address is a metadata surface.
    std::optional<MetaType> IsMeta(VAddr address) const {
        // sotc: most addresses asked about lie in a granule without metadata surfaces (texel
        // buffer reads, ~20K a heavy frame): answered without the map lookup (meta_granules).
        if (const u64 granule = address >> Traits::PAGE_BITS;
            granule < GranuleCount && meta_granules[granule] == 0) {
            return std::nullopt;
        }
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it->second.type;
        }
        return std::nullopt;
    }

    /// Returns true if a slice of the specified metadata surface has been cleared.
    bool IsMetaCleared(VAddr address, u32 slice) const {
        const auto& it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it.value().clear_mask & (1u << slice);
        }
        return false;
    }

    /// Clears all slices of the specified metadata surface.
    bool ClearMeta(VAddr address) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            it.value().clear_mask = u32(-1);
            ++meta_clear_generation;
            return true;
        }
        return false;
    }

    /// Updates the state of a slice of the specified metadata surface.
    bool TouchMeta(VAddr address, u32 slice, bool is_clear) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            if (is_clear) {
                it.value().clear_mask |= 1u << slice;
                ++meta_clear_generation;
            } else {
                it.value().clear_mask &= ~(1u << slice);
            }
            return true;
        }
        return false;
    }

    /// sotc: what a render target's state depends on besides its image (Rasterizer::BeginRendering
    /// reusing the render state of an earlier draw).
    [[nodiscard]] u64 MetaGeneration() const noexcept {
        return meta_generation;
    }
    [[nodiscard]] u64 MetaClearGeneration() const noexcept {
        return meta_clear_generation;
    }
    [[nodiscard]] bool ReadbackLinearImages() const noexcept {
        return readback_linear_images;
    }
    /// sotc: touches the stencil image FindDepthTarget last linked to the depth target `desc`, as
    /// FindDepthTarget would; false if that link is not the current one (no stencil: true).
    bool TouchLinkedStencil(const ImageDesc& desc) {
        if (desc.info.stencil_addr == 0) {
            return true;
        }
        if (stencil_memo.addr != desc.info.stencil_addr ||
            stencil_memo.size != desc.info.stencil_size ||
            stencil_memo.generation != image_set_generation) {
            return false;
        }
        TouchImage(slot_images[stencil_memo.id]);
        return true;
    }

    /// sotc: a slice of a metadata surface found not cleared (TakeMetaCleared), and
    /// meta_clear_generation then.
    struct MetaClearMemo {
        VAddr address{};
        u32 slice{};
        u64 generation{~0ULL};
    };

    /// sotc: IsMetaCleared and then TouchMeta(address, slice, false), which BeginRendering does
    /// for every target of every draw. A slice left not cleared stays so until some slice of some
    /// surface becomes cleared (meta_clear_generation): while `memo` holds it with the current
    /// generation, the answer is false and the touch a no-op, without the two lookups.
    /// SOTC_META_CLEAR_MEMO=0 always looks up.
    bool TakeMetaCleared(VAddr address, u32 slice, MetaClearMemo& memo) {
        static const bool enabled = [] {
            const char* v = std::getenv("SOTC_META_CLEAR_MEMO");
            return !v || *v != '0';
        }();
        if (!enabled) {
            const bool cleared = IsMetaCleared(address, slice);
            TouchMeta(address, slice, false);
            return cleared;
        }
        if (memo.generation == meta_clear_generation && memo.address == address &&
            memo.slice == slice) {
            return false;
        }
        bool cleared = false;
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            cleared = it.value().clear_mask & (1u << slice);
            it.value().clear_mask &= ~(1u << slice);
        }
        memo = {address, slice, meta_clear_generation};
        return cleared;
    }

    /// Runs the garbage collector.
    void RunGarbageCollector();

    template <typename Func>
    void ForEachImageInRegion(VAddr cpu_addr, size_t size, Func&& func) {
        using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        ImageIds images;
        ForEachPage(cpu_addr, size, [this, &images, cpu_addr, size, func](u64 page) {
            const auto it = page_table.find(page);
            if (it == nullptr) {
                if constexpr (BOOL_BREAK) {
                    return false;
                } else {
                    return;
                }
            }
            for (const ImageId image_id : *it) {
                Image& image = slot_images[image_id];
                if (image.flags & ImageFlagBits::Picked) {
                    continue;
                }
                if (!image.Overlaps(cpu_addr, size)) {
                    continue;
                }
                image.SetFlags(ImageFlagBits::Picked);
                images.push_back(image_id);
                if constexpr (BOOL_BREAK) {
                    if (func(image_id, image)) {
                        return true;
                    }
                } else {
                    func(image_id, image);
                }
            }
            if constexpr (BOOL_BREAK) {
                return false;
            }
        });
        for (const ImageId image_id : images) {
            slot_images[image_id].ClearFlags(ImageFlagBits::Picked);
        }
    }

private:
    /// Iterate over all page indices in a range
    template <typename Func>
    static void ForEachPage(PAddr addr, size_t size, Func&& func) {
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result<Func, u64>, bool>;
        const u64 page_end = (addr + size - 1) >> Traits::PAGE_BITS;
        for (u64 page = addr >> Traits::PAGE_BITS; page <= page_end; ++page) {
            if constexpr (RETURNS_BOOL) {
                if (func(page)) {
                    break;
                }
            } else {
                func(page);
            }
        }
    }

    /// Copies image memory back to CPU.
    void DownloadImageMemory(ImageId image_id, bool sync = false);

public:
    /// Diagnostic (SOTC_DUMP): copies every layer/slice of mip 0 to a raw file once the GPU is done.
    void DumpImage(ImageId image_id, const std::filesystem::path& path);

private:

    /// Thread function for copying downloaded images out to CPU memory.
    void DownloadedImagesThread(const std::stop_token& token);

    /// Create an image from the given parameters
    [[nodiscard]] ImageId InsertImage(const ImageInfo& info, VAddr cpu_addr);

    /// Register image in the page table
    void RegisterImage(ImageId image);

    /// Unregister image from the page table
    void UnregisterImage(ImageId image);

    /// Track CPU reads and writes for image
    void TrackImage(ImageId image_id);
    void TrackImageHead(ImageId image_id);
    void TrackImageTail(ImageId image_id);

    /// Stop tracking CPU reads and writes for image
    void UntrackImage(ImageId image_id);
    void UntrackImageHead(ImageId image_id);
    void UntrackImageTail(ImageId image_id);

    void MarkAsMaybeDirty(ImageId image_id, Image& image);

    /// Removes the image and any views/surface metas that reference it.
    void DeleteImage(ImageId image_id);

    /// Touch the image in the LRU cache.
    void TouchImage(const Image& image);

    void FreeImage(ImageId image_id) {
        UntrackImage(image_id);
        UnregisterImage(image_id);
        DeleteImage(image_id);
    }

    void GarbageCollectImages();
    void GarbageCollectSamplers();

    /// sotc: depth/color twins parked by ResolveDepthOverlap (SOTC_TWIN_REUSE=0 disables).
    static bool TwinReuse() {
        static const bool enabled = [] {
            const char* v = std::getenv("SOTC_TWIN_REUSE");
            return !v || *v != '0';
        }();
        return enabled;
    }
    void ParkImage(ImageId image_id);
    [[nodiscard]] ImageId TakeParkedImage(const ImageInfo& info);
    void ExpireParkedImages(bool all);
    void ReleaseImageMeta(ImageId image_id);
    void ReclaimImage(ImageId image_id);
    std::vector<std::pair<ImageId, u64>> parked_images;

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    Vulkan::Runtime& runtime;
    AmdGpu::Liverpool* liverpool;
    BufferCache& buffer_cache;
    PageManager& tracker;
    BlitHelper blit_helper;
    TileManager tile_manager;
    Common::SlotVector<Image> slot_images;
    Common::SlotVector<ImageView> slot_image_views;
    tsl::robin_map<u64, Sampler> samplers;
    std::unordered_set<ImageId> download_images;
    u64 total_used_memory = 0;
    u64 trigger_gc_memory = 0;
    u64 pressure_gc_memory = 0;
    u64 critical_gc_memory = 0;
    u64 total_used_samplers = 0;
    u64 trigger_gc_samplers = 0;
    u64 pressure_gc_samplers = 0;
    u64 critical_gc_samplers = 0;
    u64 gc_tick = 0;
    Common::LeastRecentlyUsedCache<ImageId, u64> lru_cache;
    Common::LeastRecentlyUsedCache<u64, u64> sampler_lru_cache;
    /// sotc: GetSampler's answers by S# and depth flag (its key), while no sampler was destroyed
    /// (sampler_generation, bumped by GarbageCollectSamplers), with the tick the sampler was last
    /// touched at. Every sampler binding of every draw asked again: a hash, the mutex and a map
    /// lookup each time.
    struct SamplerMemo {
        std::array<u64, 2> sharp{};
        bool is_depth{};
        u64 generation{~0ULL};
        u64 hash{};
        u64 touched_tick{~0ULL};
        vk::Sampler handle{};
    };
    static constexpr size_t NumSamplerMemos = 256;
    std::array<SamplerMemo, NumSamplerMemos> sampler_memos{};
    u64 sampler_generation{};
    const bool readback_linear_images;
    PageTable page_table;
    /// sotc: registered images per page_table granule (1 MiB), readable without the mutex.
    /// Guest write faults (thousands per second) on granules without images skip the mutex
    /// and the page table walk. SOTC_TEX_FAULT_FILTER=0 disables it.
    static constexpr u64 GranuleCount = u64{1} << (40 - Traits::PAGE_BITS);
    std::unique_ptr<std::atomic<u32>[]> granule_images{new std::atomic<u32>[GranuleCount]()};
    [[nodiscard]] bool MayHaveImages(VAddr addr, size_t size) const {
        static const bool filter = [] {
            const char* v = std::getenv("SOTC_TEX_FAULT_FILTER");
            return !v || *v != '0';
        }();
        if (!filter || size == 0) {
            return true;
        }
        const u64 first = addr >> Traits::PAGE_BITS;
        const u64 last = (addr + size - 1) >> Traits::PAGE_BITS;
        if (last >= GranuleCount) {
            return true;
        }
        for (u64 granule = first; granule <= last; ++granule) {
            if (granule_images[granule].load(std::memory_order_acquire) != 0) {
                return true;
            }
        }
        return false;
    }
    std::mutex mutex;
    /// sotc: FindImage's perfect matches, valid while no image is registered or unregistered.
    struct FindMemo {
        VAddr guest_address{};
        u64 guest_size{};
        Extent3D size{};
        vk::Format pixel_format{};
        AmdGpu::ImageType type{};
        bool exact_fmt{};
        u64 generation{~0ULL};
        ImageId image_id{};
    };
    static constexpr size_t NumFindMemos = 1024;
    std::array<FindMemo, NumFindMemos> find_memos{};
    u64 image_set_generation{};
    /// sotc: FindDepthTarget's last stencil image lookup (command processor only).
    struct StencilMemo {
        VAddr addr{};
        u32 size{};
        u64 generation{~0ULL};
        ImageId id{};
    } stencil_memo;

public:
    [[nodiscard]] bool OnCommandProcessor() const;
    /// sotc: changes whenever an image is registered or unregistered.
    [[nodiscard]] u64 ImageSetGeneration() const {
        return image_set_generation;
    }
    /// sotc (command processor only): if no image was registered or unregistered since
    /// `generation`, does what a FindImage hit on `image_id` does (access tick, LRU) and returns
    /// true; otherwise returns false and the caller looks the image up again.
    bool TouchIfCurrent(ImageId image_id, u64 generation);
    /// sotc: true if FindImage returned `image_id` for `info` as a perfect match (the case its
    /// own memo keeps): the lookup had no side effect besides the access tick and the LRU, which
    /// TouchIfCurrent repeats. Overlap resolutions (copies, merges) are never memoized.
    [[nodiscard]] bool IsPerfectMatch(ImageId image_id, const ImageInfo& info) {
        const Image& image = slot_images[image_id];
        return image.info.guest_address == info.guest_address &&
               image.info.guest_size == info.guest_size && image.info.size == info.size &&
               image.info.pixel_format == info.pixel_format && image.info.type == info.type &&
               !(image.info.resources < info.resources);
    }

private:
    /// sotc: images are created and destroyed (slot_images insert/erase, registration) on the
    /// command processor only, which reads the memos and the image set without the mutex; logs
    /// a call from another thread.
    void CheckImageSetOwner() const;
    std::mutex samplers_mutex;
    std::mutex download_images_mutex;
    struct MetaDataInfo {
        MetaType type;
        s32 clear_mask = -1;
    };
    tsl::robin_map<VAddr, MetaDataInfo> surface_metas;
    /// sotc: surface_metas entries per page_table granule (EmplaceMeta, EraseMeta; IsMeta).
    std::unique_ptr<u16[]> meta_granules{new u16[GranuleCount]()};
    /// sotc: surface_metas.emplace and erase keeping meta_granules and meta_clear_generation (a
    /// new surface starts cleared). EmplaceMeta returns whether it inserted.
    bool EmplaceMeta(VAddr address, MetaDataInfo info) {
        if (!surface_metas.emplace(address, info).second) {
            return false;
        }
        if (const u64 granule = address >> Traits::PAGE_BITS; granule < GranuleCount) {
            ++meta_granules[granule];
        }
        ++meta_clear_generation;
        return true;
    }
    void EraseMeta(VAddr address) {
        if (surface_metas.erase(address) == 0) {
            return;
        }
        if (const u64 granule = address >> Traits::PAGE_BITS; granule < GranuleCount) {
            --meta_granules[granule];
        }
    }
    /// sotc: Vulkan handles of deleted images, destroyed on the deleter thread.
    struct VkGarbage {
        std::vector<vk::UniqueImageView> views;
        std::vector<UniqueImage> images;
    };
    std::unique_ptr<Common::OrderedWorkQueue> vk_deleter = [] {
        const char* v = std::getenv("SOTC_IMAGE_DELETER");
        return v && *v == '0'
                   ? nullptr
                   : std::make_unique<Common::OrderedWorkQueue>(
                         [] { Common::SetCurrentThreadName("sotc:VkDeleter"); });
    }();
    /// sotc: bumped whenever surface_metas loses an entry. While it is unchanged, an address an
    /// image registered is still present, and emplace (which never overwrites) would be a
    /// no-op. SOTC_META_MEMO=0 always emplaces.
    u64 meta_generation = 0;
    /// sotc: bumped whenever a slice of a metadata surface may become cleared: an entry added
    /// (they start cleared), ClearMeta, TouchMeta(..., true). See TakeMetaCleared.
    u64 meta_clear_generation = 0;
    static bool MetaMemo() {
        static const bool enabled = [] {
            const char* v = std::getenv("SOTC_META_MEMO");
            return !v || *v != '0';
        }();
        return enabled;
    }
};

} // namespace VideoCore
