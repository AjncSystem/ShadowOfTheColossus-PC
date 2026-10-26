// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <unordered_map>
#include <optional>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <boost/container/small_vector.hpp>
#include <queue>
#include <tsl/robin_map.h>

#include "common/lru_cache.h"
#include "video_core/sotc_stats.h"
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

/// sotc: SOTC_MIP_TAIL: 0 off, 1 mip-tail images for T#s with LOD streaming counters, 2 (default)
/// for every read-only colour T# whose base_level is above 0 (TextureCache::DecideRebase). Mode 2
/// left out 2.5 GB of never-sampled mips in the attract demo: peak video memory 6.1 -> 3.3-3.5 GB
/// on a 6 GB card, frames over 50 ms 150 -> 102-112, the worst streaming frame 460 -> 250-310 ms.
inline u32 MipTailMode() {
    static const u32 mode = [] {
        const char* v = std::getenv("SOTC_MIP_TAIL");
        return v && (v[0] == '0' || v[0] == '1' || v[0] == '2') ? static_cast<u32>(v[0] - '0')
                                                                  : 2u;
    }();
    return mode;
}

/// sotc: the texture cache lock, re-entrant on its owner thread. The command processor holds it
/// over a whole binding pass (Rasterizer::BindTextures, BeginRendering, PrepareRenderState), so the
/// FindImage and UpdateImage calls inside (two per texture, ~62,000 per frame at the Shrine) only
/// count their depth. SOTC_TC_BATCH=0 drops the outer holds (every call locks as before).
///
/// It is also biased to the command processor (the first thread to take a batch hold): there it
/// is a plain store and load. Any atomic read-modify-write on that thread waits for its pending
/// stores to drain (write-combined staging copies among them): the uncontended std::mutex, and
/// then a spinlock's xchg, each cost ~5% of the thread at ~10,000 holds per frame. Other threads
/// (guest write faults, ~200 per frame) take a spinlock, announce themselves and run an
/// asymmetric barrier (FlushProcessWriteBuffers) before waiting for the command processor to
/// leave: either its store of fast_held is visible to them after the barrier, or its following
/// load of slow_wants sees theirs. SOTC_TC_BIASED=0: every thread takes the spinlock.
class alignas(64) ReentrantMutex {
public:
    /// Makes the calling thread the biased owner (once).
    void SetFastThread() {
        if (!fast_token.load(std::memory_order_relaxed) && Biased() && !HeldByThisThread()) {
            // Under the spinlock: a thread holding it read fast_token as null and skipped the
            // announcement, so the biased path must not start before it leaves.
            RawLock();
            fast_token.store(Token(), std::memory_order_relaxed);
            word.store(0, std::memory_order_release);
        }
    }
    void lock() {
        const void* self = Token();
        if (owner.load(std::memory_order_relaxed) == self) {
            ++depth;
            return;
        }
        if (self == fast_token.load(std::memory_order_relaxed)) {
            if (FastTryAcquire()) {
                mode = Mode::Fast;
            } else {
                RawLock(); // a guest holds it or is waiting for us: queue behind it
                mode = Mode::FastSlow;
            }
        } else {
            RawLock();
            AnnounceAndWait();
            mode = Mode::Slow;
        }
        owner.store(self, std::memory_order_relaxed);
        depth = 1;
    }
    bool try_lock() {
        const void* self = Token();
        if (owner.load(std::memory_order_relaxed) == self) {
            ++depth;
            return true;
        }
        if (self == fast_token.load(std::memory_order_relaxed)) {
            if (!FastTryAcquire()) {
                return false;
            }
            mode = Mode::Fast;
        } else {
            if (!RawTryLock()) {
                return false;
            }
            if (!AnnounceAndTry()) {
                return false;
            }
            mode = Mode::Slow;
        }
        owner.store(self, std::memory_order_relaxed);
        depth = 1;
        return true;
    }
    bool HeldByThisThread() const {
        return owner.load(std::memory_order_relaxed) == Token();
    }
    void unlock() {
        if (--depth != 0) {
            return;
        }
        owner.store(nullptr, std::memory_order_relaxed);
        switch (mode) {
        case Mode::Fast:
            fast_held.store(0, std::memory_order_release);
            break;
        case Mode::Slow:
            slow_wants.store(0, std::memory_order_release);
            word.store(0, std::memory_order_release);
            break;
        case Mode::FastSlow:
            word.store(0, std::memory_order_release);
            break;
        }
    }

private:
    enum class Mode : u8 { Fast, Slow, FastSlow };

    static bool Biased() {
        static const bool on = [] {
            const char* v = std::getenv("SOTC_TC_BIASED");
            return !v || v[0] != '0';
        }();
        return on;
    }
    /// Only the owner stores its own token, and clears it before unlocking, so a thread reads
    /// its token back exactly while it holds the lock.
    static const void* Token() {
        static thread_local char token;
        return &token;
    }
    /// FlushProcessWriteBuffers on Windows, a full fence elsewhere (texture_cache.cpp).
    static void AsymmetricBarrier();
    static void Pause(u32& spins) {
        if (++spins < 4096) {
            __builtin_ia32_pause();
        } else {
            std::this_thread::yield();
        }
    }
    bool FastTryAcquire() {
        fast_held.store(1, std::memory_order_relaxed);
#ifdef _WIN32
        std::atomic_signal_fence(std::memory_order_seq_cst); // the barrier is on the other side
#else
        std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
        if (slow_wants.load(std::memory_order_acquire) == 0) {
            return true;
        }
        fast_held.store(0, std::memory_order_release);
        return false;
    }
    void AnnounceAndWait() {
        if (!fast_token.load(std::memory_order_relaxed)) {
            return; // no biased owner (SOTC_TC_BIASED=0, or before the first batch hold)
        }
        slow_wants.store(1, std::memory_order_relaxed);
        AsymmetricBarrier();
        for (u32 spins = 0; fast_held.load(std::memory_order_acquire);) {
            Pause(spins);
        }
    }
    bool AnnounceAndTry() {
        if (!fast_token.load(std::memory_order_relaxed)) {
            return true;
        }
        slow_wants.store(1, std::memory_order_relaxed);
        AsymmetricBarrier();
        if (!fast_held.load(std::memory_order_acquire)) {
            return true;
        }
        slow_wants.store(0, std::memory_order_release);
        word.store(0, std::memory_order_release);
        return false;
    }
    void RawLock() {
        if (!word.exchange(1, std::memory_order_acquire)) {
            return;
        }
        for (u32 spins = 0;;) {
            while (word.load(std::memory_order_relaxed)) {
                Pause(spins);
            }
            if (!word.exchange(1, std::memory_order_acquire)) {
                return;
            }
        }
    }
    bool RawTryLock() {
        return !word.load(std::memory_order_relaxed) &&
               !word.exchange(1, std::memory_order_acquire);
    }
    std::atomic<u32> word{0};       ///< spinlock of the other threads (and of the biased owner
                                    ///< when it had to queue behind them)
    std::atomic<u32> slow_wants{0}; ///< a thread holding `word` waits for or holds the lock
    std::atomic<const void*> fast_token{nullptr};
    std::atomic<const void*> owner{nullptr};
    u32 depth = 0;
    Mode mode = Mode::Fast;
    alignas(64) std::atomic<u32> fast_held{0}; ///< the biased owner holds it (written by it only)
};

inline bool TextureLockBatching() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_TC_BATCH");
        return !v || v[0] != '0';
    }();
    return on;
}

class TextureCache {
    // sotc: fallback thresholds when the driver cannot report a memory budget (V14 derives the
    // normal ones from the budget, see the constructor).
    static constexpr s64 DEFAULT_PRESSURE_GC_MEMORY = 1_GB + 512_MB;
    static constexpr s64 DEFAULT_CRITICAL_GC_MEMORY = 3_GB;
    static constexpr s64 TARGET_GC_THRESHOLD = 8_GB; // sotc: upstream pressure/critical thresholds

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

        // sotc: mip-tail candidate (SOTC_MIP_TAIL, DecideRebase): the T#'s base_level (0: not a
        // candidate) and min_lod, whether `info` holds the tail now, and the other info (the
        // tail while `info` is the full chain, the full chain while `info` is the tail).
        u8 rebase_mip{};
        bool rebased_now{};
        u16 orig_min_lod{};
        std::optional<ImageInfo> other_info;

        ImageDesc() = default;
        ImageDesc(const AmdGpu::Image& image, const Shader::ImageResource& desc)
            : info{image, desc}, view_info{image, desc},
              type{desc.is_written ? BindingType::Storage : BindingType::Texture} {
            InitMipTail(image, desc);
        }
        void InitMipTail(const AmdGpu::Image& image, const Shader::ImageResource& resource);
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

    /// sotc: changes when an image is registered/unregistered or a sampler is destroyed, i.e.
    /// when a previously bound texture descriptor may hold a stale handle (SOTC_FAST_BIND).
    u64 BindGeneration() const noexcept {
        return image_set_generation ^
               (sampler_epoch.load(std::memory_order_acquire) * 0x9E3779B97F4A7C15ULL);
    }

    /// Frees cached images not used in the last few GC ticks (one per submit), regardless of
    /// memory thresholds. Called when a video memory allocation fails. Higher levels go deeper
    /// (sotc): level 0 frees up to 512 images last used 4 or more ticks ago, level 1 up to 4096
    /// last used 2 or more ticks ago, level 2 every image not used in the current tick.
    void EmergencyCollect(u32 level = 0);

    /// sotc: EmergencyCollect, unless this thread is inside RefreshImage (holding `mutex`): then
    /// the collection is deferred to the next RunGarbageCollector and false is returned.
    bool TryEmergencyCollect(u32 level);

    /// Invalidates any image in the logical page range.
    void InvalidateMemory(VAddr addr, size_t size);

    /// Marks an image as dirty if it exists at the provided address.
    void InvalidateMemoryFromGPU(VAddr address, size_t max_size);

    /// Evicts any images that overlap the unmapped range.
    void UnmapMemory(VAddr cpu_addr, size_t size);

    /// Schedules a copy of pending images for download back to CPU memory.
    void ProcessDownloadImages();

    /// Retrieves the image handle of the image with the provided attributes.
    [[nodiscard]] ImageId FindImage(ImageDesc& desc, bool exact_fmt = false);

    /// Retrieves image whose address matches provided
    [[nodiscard]] ImageId FindImageFromRange(VAddr address, size_t size, bool ensure_valid = true);

    /// Retrieves an image view with the properties of the specified image id.
    [[nodiscard]] ImageView& FindTexture(ImageId image_id, const ImageDesc& desc);

    /// sotc: FindTexture from the parts of the descriptor it reads (the draw bind memo keeps only
    /// those, not the ImageInfo).
    [[nodiscard]] ImageView& FindTexture(ImageId image_id, BindingType type,
                                         const ImageViewInfo& view_info);

    /// sotc: what FindImage does for an image it found by memo: marks it used this tick. For
    /// callers that already know the image (Rasterizer draw bind memo).
    void NoteFound(Image& image);

    /// sotc: true when `image_id` (what FindImage returned for `desc`) covers exactly the guest
    /// range the descriptor asks for, i.e. FindImage matched it as a whole and not as a part of a
    /// larger image or after an overlap was resolved.
    [[nodiscard]] bool FoundExactly(ImageId image_id, const ImageDesc& desc) const {
        const ImageInfo& found = slot_images[image_id].info;
        const ImageInfo& asked = desc.info;
        return found.guest_address == asked.guest_address &&
               found.guest_size == asked.guest_size && found.size == asked.size;
    }

    /// sotc: the image with this id, without the LRU touch GetImage does (callers that touch it
    /// otherwise: NoteFound, UpdateImage).
    [[nodiscard]] Image& PeekImage(ImageId id) {
        return slot_images[id];
    }

    /// sotc: moves on every RegisterImage/UnregisterImage: while it stays, a lookup that found an
    /// image for the same descriptor finds it again (see the draw bind memo, vk_rasterizer.cpp).
    [[nodiscard]] u64 ImageSetGeneration() const noexcept {
        return image_set_generation;
    }

    /// sotc: FindImage's own per-address generation (MemoGeneration): moves when an image is
    /// registered or unregistered at `address`. While it stays, a lookup that matched an image
    /// exactly at `address` matches it again.
    [[nodiscard]] u64 FindGeneration(VAddr address) const {
        return MemoGeneration(address);
    }

    /// Retrieves the render target with specified properties
    /// sotc: update=false (BeginRendering, SOTC_RT_FAST) skips the second UpdateImage when the
    /// caller has just run it and the image needs no upload or re-tracking since.
    [[nodiscard]] ImageView& FindRenderTarget(ImageId image_id, const ImageDesc& desc,
                                              bool update = true);

    /// sotc: holds the cache lock over a binding pass (ReentrantMutex); empty with
    /// SOTC_TC_BATCH=0.
    [[nodiscard]] std::unique_lock<ReentrantMutex> BatchLock() {
        mutex.SetFastThread();
        if (!TextureLockBatching()) {
            return {};
        }
        std::unique_lock lk{mutex, std::defer_lock};
        LockCounted(lk);
        return lk;
    }

    /// sotc: locks `mutex`, counting contention (SOTC_CMD_STATS, SOTC sys line).
    static void LockCounted(std::unique_lock<ReentrantMutex>& lk) {
        if (SotcStats::kCmdStats && !lk.mutex()->HeldByThisThread()) {
            SotcStats::sys_tc_locks.fetch_add(1, std::memory_order_relaxed);
            if (lk.try_lock()) return;
            SotcStats::sys_tc_contended.fetch_add(1, std::memory_order_relaxed);
            const auto start = std::chrono::steady_clock::now();
            lk.lock();
            SotcStats::sys_tc_wait_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count(),
                std::memory_order_relaxed);
            return;
        }
        lk.lock();
    }

    /// Retrieves the depth target with specified properties
    [[nodiscard]] ImageView& FindDepthTarget(ImageId image_id, const ImageDesc& desc);

    /// Updates image contents if it was modified by CPU.
    void UpdateImage(ImageId image_id) {
        std::unique_lock lk{mutex, std::defer_lock};
        LockCounted(lk);
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

    /// Retrieves the image with the specified id.
    [[nodiscard]] Image& GetImage(ImageId id) {
        auto& image = slot_images[id];
        // sotc: the LRU is only safe under `mutex`: unlocked, a guest unmap can unregister the
        // image (freeing its LRU node) between TouchImage's Registered check and the touch. Calls
        // outside a batch hold skip it; the locked FindImage/UpdateImage of a bound image touch it.
        if (mutex.HeldByThisThread()) {
            TouchImage(image);
        }
        return image;
    }

    /// sotc: the image with this id if it is still the registered image `uid`, else nullptr.
    [[nodiscard]] Image* TryGetImage(ImageId id, u64 uid) {
        if (!id || !slot_images.is_allocated(id)) {
            return nullptr;
        }
        auto& image = slot_images[id];
        if (image.image_uid != uid || !(image.flags & ImageFlagBits::Registered)) {
            return nullptr;
        }
        return &image;
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

    /// sotc: IsMetaCleared + TouchMeta(address, slice, false) with a single lookup
    /// (SOTC_RT_FAST): returns the old clear bit of the slice (false when the address is not a
    /// metadata surface, like IsMetaCleared) and clears it.
    bool TestAndTouchMeta(VAddr address, u32 slice) {
        auto it = surface_metas.find(address);
        if (it == surface_metas.end()) {
            return false;
        }
        auto& clear_mask = it.value().clear_mask;
        const bool was_clear = (clear_mask & (1u << slice)) != 0;
        clear_mask &= ~(1u << slice);
        return was_clear;
    }

    /// Clears all slices of the specified metadata surface.
    bool ClearMeta(VAddr address) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            it.value().clear_mask = u32(-1);
            meta_epoch.fetch_add(1, std::memory_order_relaxed); // sotc: DepthFastPath
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
                meta_epoch.fetch_add(1, std::memory_order_relaxed); // sotc: DepthFastPath
            } else {
                it.value().clear_mask &= ~(1u << slice);
            }
            return true;
        }
        return false;
    }

    /// sotc: BbToggle::DepthFastPath. Moves whenever a metadata surface (HTile/CMask/FMask entry)
    /// is added, removed or gets a clear bit set; while it stays, the clear bit TestAndTouchMeta
    /// left cleared for a slice is still clear and a missing entry is still missing.
    [[nodiscard]] u64 MetaEpoch() const noexcept {
        return meta_epoch.load(std::memory_order_relaxed);
    }
    /// sotc: the stencil image FindDepthTarget linked to the depth target of its last call (and
    /// touched in the LRU), or the null id.
    [[nodiscard]] ImageId LastDepthStencil() const noexcept {
        return last_depth_stencil_id;
    }
    /// sotc: SOTC_DEPTHFAST_VERIFY: the tick an image was last touched in the LRU with (0 when it
    /// has no LRU node), and the clear mask of a metadata surface (0 when there is none).
    [[nodiscard]] u64 LruTickOf(ImageId id) {
        const Image& image = slot_images[id];
        return True(image.flags & ImageFlagBits::Registered) ? lru_cache.TickOf(image.lru_id) : 0;
    }
    [[nodiscard]] u32 MetaClearMask(VAddr address) const {
        const auto it = surface_metas.find(address);
        return it != surface_metas.end() ? it->second.clear_mask : 0;
    }
    /// sotc: BbToggle::GBufferFastPath. True when FindRenderTarget(image) has no side effect that
    /// a repeated call would have to redo (its download_images queueing, only for linear or
    /// narrow images under readback_linear_images).
    [[nodiscard]] bool RenderTargetReplayable(const Image& image) const noexcept {
        return !readback_linear_images ||
               (image.info.props.is_tiled && image.info.size.width > 8);
    }
    /// sotc: the LRU touch FindDepthTarget gives the stencil image it links, for a caller that
    /// replays the call (needs the batch hold like GetImage).
    void TouchLinked(ImageId id) {
        if (id) {
            TouchImage(slot_images[id]);
        }
    }
    /// sotc: UpdateImage(image_id) apart from its LRU touch changes nothing: the image is not
    /// Dirty (RefreshImage returns at once) and is tracked over its whole range (TrackImage
    /// returns at once). The same test FindRenderTarget(update=false) makes.
    [[nodiscard]] static bool UpdateIsNoop(const Image& image) noexcept {
        return False(image.flags & ImageFlagBits::Dirty) && image.info.num_samples <= 1 &&
               (False(image.flags & ImageFlagBits::Registered) ||
                (image.track_addr == image.info.guest_address &&
                 image.track_addr_end == image.info.guest_address + image.info.guest_size));
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
                image.flags |= ImageFlagBits::Picked;
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
            slot_images[image_id].flags &= ~ImageFlagBits::Picked;
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

    /// sotc: SOTC_TEX_DUMP=1 saves each small sampled texture once (user/texdump), keyed by the
    /// hash of its guest bytes, to find the game's controller glyphs.
    void SotcGlyphHook(Image& image);

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

    void FreeImage(ImageId image_id, const char* why = "other");

    /// sotc: depth/colour twins (ResolveDepthOverlap). Park: what FreeImage does except the
    /// reclaim of the slot and its Vulkan objects. Unpark: registers it again as if new.
    void ParkImage(ImageId image_id);
    void UnparkImage(ImageId image_id);
    void ReclaimImage(ImageId image_id);

    void GarbageCollectImages();

    /// sotc: ColdImageSweep (MEMORIA-PLAN.md), see the definitions.
    bool IsColdSweepable(const Image& image) const;
    void SweepColdImages();
    void MemoryCensus();
    void NoteTickClock(std::chrono::steady_clock::time_point now);
    /// The newest gc_tick whose wall time is at least `seconds` ago; false when the cache has not
    /// run that long.
    bool TickAtAge(std::chrono::steady_clock::time_point now, double seconds, u64& tick) const;

    /// sotc: image memory by class, every 5 s with SOTC_FPS (telemetry only).
    void Census();

    /// sotc: mip-tail images (SOTC_MIP_TAIL), see DecideRebase.
    bool IsSafeToDelete(const Image& image) const;
    bool DecideRebase(ImageDesc& desc, bool exact_fmt);
    void MipTailCreated(ImageId image_id, const ImageDesc& desc);
    void FreeRebased(ImageId image_id, std::atomic<std::uint64_t>& counter);
    /// First GPU write of a target or storage image (FindTexture/RenderTarget/DepthTarget).
    void MarkWritten(Image& image);
    void GarbageCollectSamplers();

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
    /// sotc: (gc_tick, wall time) about once a second, newest last: the LRU counts submits, the
    /// cold sweep and the idle census need seconds.
    struct TickMark {
        u64 tick;
        std::chrono::steady_clock::time_point time;
    };
    std::deque<TickMark> tick_marks;
    std::chrono::steady_clock::time_point next_sweep{};
    std::chrono::steady_clock::time_point next_mem_census{};
    /// sotc: guest address -> (time, host bytes) of the images the cold sweep freed, to count the
    /// ones that came back.
    struct SweptImage {
        std::chrono::steady_clock::time_point time;
        u64 bytes;
    };
    std::unordered_map<VAddr, SweptImage> swept_images;
    const bool readback_linear_images;
    PageTable page_table;
    /// sotc: images registered per page_table page (1 MiB), read without the lock by
    /// InvalidateMemory (guest write faults).
    static constexpr u64 NumImagePages = u64{1} << (Traits::ADDRESS_SPACE_BITS - Traits::PAGE_BITS);
    std::unique_ptr<std::atomic<u32>[]> page_images{new std::atomic<u32>[NumImagePages]()};
    ReentrantMutex mutex;
    /// sotc: RefreshImage nesting on this thread (it runs under `mutex`).
    static inline thread_local u32 refresh_depth = 0;
    /// sotc: deferred EmergencyCollect level + 1 (0: none).
    std::atomic<u32> pending_emergency{0};
    /// sotc: FindImage's perfect matches, valid while no image is registered or unregistered
    /// (a mip-tail image's also while gpu_modified_epoch stays). `writer`: stored by a writer
    /// lookup (with mip tails on, writers only take those).
    struct FindMemo {
        VAddr guest_address{};
        u64 guest_size{};
        Extent3D size{};
        vk::Format pixel_format{};
        AmdGpu::ImageType type{};
        bool exact_fmt{};
        bool writer{};
        u64 generation{~0ULL};
        ImageId image_id{};
        u64 gpu_epoch{};
    };
    static constexpr size_t NumFindMemos = 1024;
    std::array<FindMemo, NumFindMemos> find_memos{};
    FindMemo& MemoSlot(const ImageInfo& info, bool exact_fmt, bool writer);
    /// The memoized perfect match for a lookup, or none (SOTC_FIND_MEMO=0: always none).
    ImageId MemoLookup(const ImageInfo& info, bool exact_fmt, bool writer);
    /// The generation a memo for `address` is valid in (per-address unless SOTC_MEMO_ADDR_GEN=0).
    u64 MemoGeneration(VAddr address) const;
    u64 image_set_generation{};
    std::atomic<u64> meta_epoch{0};  ///< sotc: see MetaEpoch
    ImageId last_depth_stencil_id{}; ///< sotc: see LastDepthStencil
    /// sotc: per-address generations for FindMemo. A perfect match needs the same
    /// guest_address, so only (un)registering an image at that address can change the
    /// result; bumped in RegisterImage/UnregisterImage at the image's guest_address.
    static constexpr size_t NumAddrGens = 4096;
    std::array<u32, NumAddrGens> addr_gens{};
    static size_t AddrGenSlot(VAddr addr) {
        return static_cast<size_t>((u64{addr >> 8} * 0x9E3779B97F4A7C15ULL) >> 52);
    }
    std::mutex samplers_mutex;
    // sotc: bumped whenever a sampler is erased (GetSampler's per-thread memo).
    std::atomic<u64> sampler_epoch{0};
    std::mutex download_images_mutex;
    struct MetaDataInfo {
        MetaType type;
        s32 clear_mask = -1;
    };
    tsl::robin_map<VAddr, MetaDataInfo> surface_metas;
};

} // namespace VideoCore
