// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: SOTC_ASYNC_IMAGE_CREATE (see image_prefetch.h).

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <string>

#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/sotc_stats.h"
#include "video_core/texture_cache/image_prefetch.h"
#include "../../../sotc/area_prewarm.h" // sotc: SOTC_AREA_PREWARM replay hits

namespace VideoCore {

namespace {
/// Image workers. vmaCreateImage is thread safe (the allocator is not created externally
/// synchronized) and so is vkCreateImage; two keep a burst of new textures going.
constexpr u32 NumWorkers = 2;
/// Outstanding prefetches (queued, being made, or made and not taken): at most this many, and
/// guest bytes. A guess that does not come true holds its memory until ExpireNs.
constexpr size_t MaxEntries = 128;
constexpr u64 MaxBytes = u64{384} << 20;
/// A made image nobody took within this time is destroyed. The command processor runs at most
/// the draw thread's ring (512 operations) ahead, well under a second even while loading.
constexpr u64 ExpireNs = 2'000'000'000ULL;
/// No prefetch while the reported video memory use is above this share of the budget (permille):
/// the collector and the over-budget path of UniqueImage::Create keep that case.
constexpr u64 BudgetPermille = 900;
/// sotc: replay entries outstanding (SOTC_AREA_PREWARM=3). Not in MaxEntries / MaxBytes: a replay
/// has its own per-area budget (texture_cache.cpp), and it is dropped at each area change.
constexpr size_t MaxReplayEntries = 4096;

/// sotc: SOTC_IMAGE_PREFETCH_MAX=<n> (read once): outstanding prefetches allowed, instead of
/// MaxEntries (1-4096). The byte cap grows in the same proportion as MaxBytes.
size_t MaxEntriesLimit() {
    static const size_t limit = [] {
        const char* v = std::getenv("SOTC_IMAGE_PREFETCH_MAX");
        const u64 n = v ? std::strtoull(v, nullptr, 10) : 0;
        return n == 0 ? MaxEntries : size_t(std::clamp<u64>(n, 1, 4096));
    }();
    return limit;
}
u64 MaxBytesLimit() {
    // sotc: at most 4x the default byte cap, also when the VRAM budget is unknown (budget 0, where
    // Request applies no budget/16 cap)
    return std::min<u64>(MaxBytes * MaxEntriesLimit() / MaxEntries, MaxBytes * 4);
}
} // namespace

bool AsyncImageCreateEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_ASYNC_IMAGE_CREATE");
        return v && v[0] == '1';
    }();
    return enabled;
}

ImagePrefetcher::ImagePrefetcher(const Vulkan::Instance& instance_) : instance{instance_} {
    limits = instance.GetPhysicalDevice().getProperties().limits;
    workers.reserve(NumWorkers);
    for (u32 i = 0; i < NumWorkers; ++i) {
        workers.emplace_back([this, i] { WorkerLoop(i); });
    }
    g_image_prefetcher.store(this, std::memory_order_release);
}

ImagePrefetcher::~ImagePrefetcher() {
    g_image_prefetcher.store(nullptr, std::memory_order_release);
    {
        std::scoped_lock lk{mutex};
        stopping = true;
    }
    work_cv.notify_all();
    for (auto& worker : workers) {
        worker.join();
    }
    // Workers are gone: whatever is left (made, failed or queued) is destroyed here, before the
    // texture cache (and later the allocator) goes.
    queue.clear();
    entries.clear();
}

u64 ImagePrefetcher::KeyOf(const vk::ImageCreateInfo& ci) {
    u64 h = u64(ci.format) * 0x9E3779B97F4A7C15ULL;
    h ^= (u64{ci.extent.width} << 32 | ci.extent.height) * 0xC2B2AE3D27D4EB4FULL;
    h ^= (u64{ci.extent.depth} << 40 | u64{ci.mipLevels} << 24 | ci.arrayLayers) *
         0x165667B19E3779F9ULL;
    h ^= (u64(u32(ci.usage)) << 32 | u32(ci.flags)) * 0x27D4EB2F165667C5ULL;
    h ^= u64(u32(ci.samples)) << 3 ^ u64(ci.imageType) << 11 ^ u64(ci.tiling) << 17;
    return h ^ (h >> 31);
}

bool ImagePrefetcher::SameImage(const vk::ImageCreateInfo& a, const vk::ImageCreateInfo& b) {
    // Every field vkCreateImage reads. Both come from PlanImage, which chains nothing.
    return a.pNext == nullptr && b.pNext == nullptr && a.flags == b.flags &&
           a.imageType == b.imageType && a.format == b.format &&
           a.extent.width == b.extent.width && a.extent.height == b.extent.height &&
           a.extent.depth == b.extent.depth && a.mipLevels == b.mipLevels &&
           a.arrayLayers == b.arrayLayers && a.samples == b.samples && a.tiling == b.tiling &&
           a.usage == b.usage && a.sharingMode == b.sharingMode &&
           a.queueFamilyIndexCount == b.queueFamilyIndexCount &&
           a.initialLayout == b.initialLayout;
}

bool ImagePrefetcher::FitsLimits(const vk::ImageCreateInfo& ci) const {
    // A guessed image is only made when it is plainly valid: single-sampled, non-empty, within
    // the device limits. The draw thread creates anything else itself, as before.
    const auto& e = ci.extent;
    if (ci.pNext != nullptr || ci.samples != vk::SampleCountFlagBits::e1 || e.width == 0 ||
        e.height == 0 || e.depth == 0 || ci.mipLevels == 0 || ci.arrayLayers == 0 ||
        ci.format == vk::Format::eUndefined) {
        return false;
    }
    if (ci.imageType == vk::ImageType::e3D) {
        if (e.width > limits.maxImageDimension3D || e.height > limits.maxImageDimension3D ||
            e.depth > limits.maxImageDimension3D || ci.arrayLayers != 1) {
            return false;
        }
    } else if (ci.imageType == vk::ImageType::e2D) {
        if (e.width > limits.maxImageDimension2D || e.height > limits.maxImageDimension2D ||
            e.depth != 1 || ci.arrayLayers > limits.maxImageArrayLayers) {
            return false;
        }
    } else {
        return false;
    }
    const u32 largest = std::max({e.width, e.height, e.depth});
    return ci.mipLevels <= u32(std::bit_width(largest));
}

void ImagePrefetcher::ReleaseLocked(const Entry& entry) {
    if (entry.replay) {
        --replay_outstanding;
    } else {
        outstanding_bytes -= entry.size_hint;
    }
}

bool ImagePrefetcher::RequestReplay(const vk::ImageCreateInfo& image_ci, u64 size_hint) {
    if (!FitsLimits(image_ci)) {
        return false;
    }
    const u64 budget = SotcStats::vram_budget.load(std::memory_order_relaxed);
    const u64 used = SotcStats::vram_used.load(std::memory_order_relaxed);
    if (budget != 0 && used + size_hint > budget / 1000 * BudgetPermille) {
        return false;
    }
    {
        std::scoped_lock lk{mutex};
        if (stopping || replay_outstanding >= MaxReplayEntries) {
            return false;
        }
        auto entry = std::make_shared<Entry>();
        entry->image_ci = image_ci;
        entry->key = KeyOf(image_ci);
        entry->size_hint = size_hint;
        entry->replay = true;
        entries.emplace(entry->key, entry);
        queue.push_back(std::move(entry));
        ++replay_outstanding;
    }
    work_cv.notify_one();
    return true;
}

bool ImagePrefetcher::Request(const vk::ImageCreateInfo& image_ci, u64 size_hint) {
    if (!FitsLimits(image_ci)) {
        SotcStats::async_image_skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const u64 budget = SotcStats::vram_budget.load(std::memory_order_relaxed);
    const u64 used = SotcStats::vram_used.load(std::memory_order_relaxed);
    if (budget != 0 && used + size_hint > budget / 1000 * BudgetPermille) {
        SotcStats::async_image_skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // sotc: at most 1/16 of the budget ahead (256 MiB on a 4 GB card), and never over MaxBytes.
    const u64 max_bytes = budget != 0 ? std::min(MaxBytesLimit(), budget / 16) : MaxBytesLimit();
    {
        std::scoped_lock lk{mutex};
        if (stopping || entries.size() - replay_outstanding >= MaxEntriesLimit() ||
            outstanding_bytes + size_hint > max_bytes) {
            SotcStats::async_image_skipped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        auto entry = std::make_shared<Entry>();
        entry->image_ci = image_ci;
        entry->key = KeyOf(image_ci);
        entry->size_hint = size_hint;
        entries.emplace(entry->key, entry);
        queue.push_back(std::move(entry));
        outstanding_bytes += size_hint;
    }
    SotcStats::async_image_requested.fetch_add(1, std::memory_order_relaxed);
    work_cv.notify_one();
    return true;
}

void ImagePrefetcher::EraseLocked(const EntryPtr& entry) {
    auto [first, last] = entries.equal_range(entry->key);
    for (auto it = first; it != last; ++it) {
        if (it->second == entry) {
            ReleaseLocked(*entry);
            entries.erase(it);
            return;
        }
    }
}

void ImagePrefetcher::ExpireLocked(u64 now, std::vector<UniqueImage>& out) {
    for (auto it = entries.begin(); it != entries.end();) {
        Entry& entry = *it->second;
        const bool failed = entry.state == State::Failed;
        // sotc: a replay entry waits for its draw (no time expiry)
        const bool stale =
            entry.state == State::Ready && !entry.replay && now - entry.ready_ns > ExpireNs;
        if (entry.claimed || (!failed && !stale)) {
            ++it;
            continue;
        }
        if (stale) {
            out.push_back(std::move(entry.image));
            SotcStats::async_image_expired.fetch_add(1, std::memory_order_relaxed);
        }
        ReleaseLocked(entry);
        it = entries.erase(it);
    }
}

bool ImagePrefetcher::Take(const vk::ImageCreateInfo& image_ci, UniqueImage& out) {
    const u64 key = KeyOf(image_ci);
    std::unique_lock lk{mutex};
    EntryPtr best;
    int best_rank = 0;
    std::vector<EntryPtr> failed;
    auto [first, last] = entries.equal_range(key);
    for (auto it = first; it != last; ++it) {
        const EntryPtr& entry = it->second;
        if (entry->claimed || !SameImage(entry->image_ci, image_ci)) {
            continue;
        }
        // A made image first, then one being made, then a queued one (taken back).
        const int rank = entry->state == State::Ready     ? 3
                         : entry->state == State::Running ? 2
                         : entry->state == State::Queued  ? 1
                                                          : 0;
        if (rank == 0 && entry->state == State::Failed) {
            failed.push_back(entry);
        }
        if (rank > best_rank) {
            best_rank = rank;
            best = entry;
        }
    }
    for (const EntryPtr& entry : failed) {
        EraseLocked(entry);
    }
    if (!best) {
        SotcStats::async_image_sync.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (best->state == State::Queued) {
        // No worker started it: making it here is quicker than waiting behind the queue. The
        // worker skips a cancelled entry when it pops it.
        best->state = State::Cancelled;
        EraseLocked(best);
        SotcStats::async_image_taken_back.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    bool waited = false;
    if (best->state == State::Running) {
        // The worker is inside vmaCreateImage for it and needs nothing this thread may hold
        // (it takes only `mutex`, released by the wait, and VMA's own locks).
        best->claimed = true;
        waited = true;
        const u64 start = SotcStats::NowNs();
        done_cv.wait(lk, [&] { return best->state != State::Running; });
        SotcStats::async_image_waits.fetch_add(1, std::memory_order_relaxed);
        SotcStats::async_image_wait_ns.fetch_add(SotcStats::NowNs() - start,
                                                 std::memory_order_relaxed);
    }
    EraseLocked(best);
    if (best->state != State::Ready) {
        SotcStats::async_image_sync.fetch_add(1, std::memory_order_relaxed);
        return false; // the worker failed (budget): the caller creates it as before
    }
    out = std::move(best->image);
    if (best->replay) {
        SotcAreaPrewarm::NoteReplayHit(); // sotc: SOTC_AREA_PREWARM: a draw took a replayed image
    }
    if (!waited) {
        SotcStats::async_image_taken.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void ImagePrefetcher::NoteRegistered(VAddr address) {
    std::scoped_lock lk{alive_mutex};
    ++alive[address];
}

void ImagePrefetcher::NoteUnregistered(VAddr address) {
    std::scoped_lock lk{alive_mutex};
    if (const auto it = alive.find(address); it != alive.end() && --it->second == 0) {
        alive.erase(it);
    }
}

bool ImagePrefetcher::IsRegistered(VAddr address) {
    std::scoped_lock lk{alive_mutex};
    return alive.contains(address);
}

void ImagePrefetcher::DropUnclaimed() {
    std::vector<UniqueImage> dropped;
    {
        std::scoped_lock lk{mutex};
        for (auto it = entries.begin(); it != entries.end();) {
            Entry& entry = *it->second;
            if (entry.claimed || entry.state == State::Running) {
                ++it;
                continue;
            }
            if (entry.state == State::Ready) {
                dropped.push_back(std::move(entry.image));
                SotcStats::async_image_expired.fetch_add(1, std::memory_order_relaxed);
            } else if (entry.state == State::Queued) {
                entry.state = State::Cancelled;
            }
            ReleaseLocked(entry);
            it = entries.erase(it);
        }
    }
    // `dropped` is destroyed here, outside the lock.
}

void ImagePrefetcher::DropReplay() {
    std::vector<UniqueImage> dropped;
    {
        std::scoped_lock lk{mutex};
        for (auto it = entries.begin(); it != entries.end();) {
            Entry& entry = *it->second;
            if (!entry.replay || entry.claimed || entry.state == State::Running) {
                ++it;
                continue;
            }
            if (entry.state == State::Ready) {
                dropped.push_back(std::move(entry.image));
            } else if (entry.state == State::Queued) {
                entry.state = State::Cancelled;
            }
            ReleaseLocked(entry);
            it = entries.erase(it);
        }
    }
    // `dropped` is destroyed here, outside the lock.
}

void ImagePrefetcher::WorkerLoop(u32 index) {
    Common::SetCurrentThreadName(("shadPS4:ImageCreate" + std::to_string(index)).c_str());
    for (;;) {
        EntryPtr job;
        std::vector<UniqueImage> expired;
        {
            std::unique_lock lk{mutex};
            work_cv.wait_for(lk, std::chrono::milliseconds{250},
                             [&] { return stopping || !queue.empty(); });
            if (stopping) {
                return;
            }
            ExpireLocked(SotcStats::NowNs(), expired);
            while (!queue.empty()) {
                EntryPtr entry = std::move(queue.front());
                queue.pop_front();
                if (entry->state == State::Queued) {
                    job = std::move(entry);
                    break;
                }
            }
            if (job) {
                job->state = State::Running;
            }
        }
        expired.clear(); // vmaDestroyImage outside the lock
        if (!job) {
            continue;
        }
        UniqueImage image{instance.GetDevice(), instance.GetAllocator()};
        const u64 start = SotcStats::NowNs();
        const bool ok = image.TryCreateWithinBudget(job->image_ci);
        const u64 now = SotcStats::NowNs();
        SotcStats::async_image_build_ns.fetch_add(now - start, std::memory_order_relaxed);
        {
            std::scoped_lock lk{mutex};
            if (ok) {
                job->image = std::move(image);
                job->state = State::Ready;
                job->ready_ns = now;
                SotcStats::async_image_built.fetch_add(1, std::memory_order_relaxed);
            } else {
                job->state = State::Failed;
                SotcStats::async_image_failed.fetch_add(1, std::memory_order_relaxed);
            }
        }
        done_cv.notify_all();
    }
}

} // namespace VideoCore
