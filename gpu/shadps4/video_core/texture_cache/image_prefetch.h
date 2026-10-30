// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: SOTC_ASYNC_IMAGE_CREATE=1 (off by default). Images are created ahead of use.
//
// While an area loads, the draw thread spent ~1 ms per new texture in Image::Image, almost all of
// it in vmaCreateImage (vkCreateImage plus a memory allocation, often a dedicated one). The
// command processor resolves every draw before the draw thread binds it, so it can read the
// draw's T#s first (Rasterizer::PrefetchImages): for a T# whose image does not exist yet it works
// out the exact Vulkan creation parameters the draw thread will use (PlanImage), and an image
// worker creates that VkImage with its memory meanwhile. When the draw thread's Image::Image
// reaches the same parameters it takes the finished image instead of creating one.
//
// Only Vulkan objects cross threads: a prefetched image is not in the texture cache (no ImageId,
// no page table entry, no LRU node) until a constructor takes it, so nothing the cache does
// (lookups, overlaps, collection) can see or free it. Two images with identical
// VkImageCreateInfo and the same allocation parameters are interchangeable (both are new, with
// undefined contents and layout), so a prefetched image is matched by its parameters alone and
// any guess of the command processor that does not come true only costs a worker's time and
// some video memory for ExpireNs; the draw thread then creates its image as before.

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {
class Instance;
}

namespace VideoCore {

/// SOTC_ASYNC_IMAGE_CREATE=1 (read once).
bool AsyncImageCreateEnabled();

class ImagePrefetcher {
public:
    explicit ImagePrefetcher(const Vulkan::Instance& instance);
    ~ImagePrefetcher();

    ImagePrefetcher(const ImagePrefetcher&) = delete;
    ImagePrefetcher& operator=(const ImagePrefetcher&) = delete;

    /// Command processor: queues the creation of an image with these parameters for a worker.
    /// Dropped (counted) when too much is outstanding, video memory is near the budget, or the
    /// parameters are outside the device limits. size_hint: the guest size (for the cap).
    /// Queues the creation of an image. False when it was not queued (limits, budget, stopping).
    bool Request(const vk::ImageCreateInfo& image_ci, u64 size_hint);

    /// sotc: SOTC_AREA_PREWARM=3 (area_prewarm.h): as Request for a record of an area being
    /// replayed. Such an entry does not expire by time (it waits for its draw until DropReplay or
    /// DropUnclaimed), and it has its own count cap, not the prefetch caps. Same plausibility and
    /// video memory checks as Request.
    bool RequestReplay(const vk::ImageCreateInfo& image_ci, u64 size_hint);

    /// Any thread constructing an Image: moves a prefetched image with exactly these parameters
    /// into `out` and returns true. A matching image a worker is creating right now is waited
    /// for; one still queued is taken back (false: the caller creates it). False also when none
    /// matches or the worker failed.
    bool Take(const vk::ImageCreateInfo& image_ci, UniqueImage& out);

    /// Images registered in the texture cache by guest address (RegisterImage/UnregisterImage,
    /// under the texture cache lock), read by the command processor to skip T#s whose image
    /// exists. A leaf lock of its own.
    void NoteRegistered(VAddr address);
    void NoteUnregistered(VAddr address);
    bool IsRegistered(VAddr address);

    /// Video memory is short (TextureCache::EmergencyCollect): destroys every prefetched image
    /// nobody claimed and cancels the queued ones.
    void DropUnclaimed();

    /// sotc: destroys the unclaimed replay entries (the next area's replay replaces them). Ones a
    /// worker is making now stay until the next call.
    void DropReplay();

private:
    enum class State : u8 { Queued, Running, Ready, Failed, Cancelled };
    struct Entry {
        vk::ImageCreateInfo image_ci{};
        u64 key{};
        u64 size_hint{};
        State state{State::Queued};
        bool claimed{}; ///< a Take waits for it: expiry and other Takes leave it alone
        bool replay{};  ///< sotc: SOTC_AREA_PREWARM replay (RequestReplay): no time expiry
        u64 ready_ns{};
        UniqueImage image;
    };
    using EntryPtr = std::shared_ptr<Entry>;

    void WorkerLoop(u32 index);
    /// Removes `entry` from `entries` (and its size from outstanding_bytes). Under `mutex`.
    void EraseLocked(const EntryPtr& entry);
    /// Unclaimed failed entries, and ready ones older than ExpireNs, leave; the images go to
    /// `out` so they are destroyed after `mutex` is released. Under `mutex`.
    void ExpireLocked(u64 now, std::vector<UniqueImage>& out);
    /// sotc: the accounting of an entry that leaves `entries`: its bytes (or its replay count).
    void ReleaseLocked(const Entry& entry);
    bool FitsLimits(const vk::ImageCreateInfo& image_ci) const;
    static u64 KeyOf(const vk::ImageCreateInfo& image_ci);
    static bool SameImage(const vk::ImageCreateInfo& a, const vk::ImageCreateInfo& b);

    const Vulkan::Instance& instance;
    vk::PhysicalDeviceLimits limits{};
    std::mutex mutex;
    std::condition_variable work_cv;
    std::condition_variable done_cv;
    bool stopping{};
    std::deque<EntryPtr> queue;
    std::unordered_multimap<u64, EntryPtr> entries; ///< not taken yet (any state but Cancelled)
    u64 outstanding_bytes{};
    size_t replay_outstanding{}; ///< sotc: replay entries in `entries` (not in outstanding_bytes)
    std::mutex alive_mutex;
    std::unordered_map<VAddr, u32> alive;
    std::vector<std::thread> workers;
};

/// The active prefetcher (TextureCache owns it), or null when SOTC_ASYNC_IMAGE_CREATE is off.
inline std::atomic<ImagePrefetcher*> g_image_prefetcher{nullptr};

/// Image::Image: takes a prefetched image with these parameters (see ImagePrefetcher::Take).
inline bool TakePrefetchedImage(const vk::ImageCreateInfo& image_ci, UniqueImage& out) {
    ImagePrefetcher* prefetcher = g_image_prefetcher.load(std::memory_order_acquire);
    return prefetcher && prefetcher->Take(image_ci, out);
}

} // namespace VideoCore
