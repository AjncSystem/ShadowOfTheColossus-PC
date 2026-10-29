// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <boost/container/small_vector.hpp>
#include "common/thread.h"
#include "video_core/buffer_cache/async_upload.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/sotc_stats.h"
#include "bbport_threads.h"

namespace VideoCore {

namespace {
/// Piece state: claimed by exactly one thread (CAS from Queued), which copies it.
enum : u32 { StateQueued, StateClaimed, StateDone };
} // namespace

struct AsyncUpload::Job {
    /// One piece of up to 512 KiB, with the guest addresses and backing offsets it reads
    /// (SOTC_FENCE_PIECES; has_ranges of the job says whether they are known).
    struct Slot {
        Piece piece{};
        u64 va{};
        u64 phys{};
        std::atomic<u32> state{StateQueued};
    };
    std::unique_ptr<Slot[]> slots;
    u32 count{};
    bool has_ranges{};
    const void* owner{};
    u64 tick{};
    std::vector<Range> va;
    std::vector<Range> phys;
    Buffer* staging{};
    u64 flush_offset{};
    u64 flush_size{};
    std::atomic<u64> left{0}; ///< pieces not copied yet
    std::atomic<bool> done{false};
};

void AsyncUpload::Init() {
    const char* env = std::getenv("SOTC_ASYNC_UPLOAD");
    const char* texture_env = std::getenv("SOTC_ASYNC_TEXTURE_UPLOAD");
    buffers_enabled = env && env[0] == '1';
    textures_enabled = texture_env && texture_env[0] == '1';
    const char* pieces_env = std::getenv("SOTC_FENCE_PIECES");
    fence_pieces = pieces_env && pieces_env[0] == '1';
    if ((!buffers_enabled && !textures_enabled) || Get()) {
        return;
    }
    // SOTC_ASYNC_UPLOAD_THREADS: copy threads, default 3 (1-8).
    // SOTC_ASYNC_UPLOAD_MIN_KB: smaller uploads stay on the draw thread, default 256 KiB.
    const char* threads_env = std::getenv("SOTC_ASYNC_UPLOAD_THREADS");
    const char* min_env = std::getenv("SOTC_ASYNC_UPLOAD_MIN_KB");
    const u32 threads =
        threads_env ? std::clamp<u32>(static_cast<u32>(std::atoi(threads_env)), 1, 8) : 3;
    const u64 min_bytes = (min_env ? std::strtoull(min_env, nullptr, 10) : 256) << 10;
    instance.store(new AsyncUpload(threads, min_bytes), std::memory_order_release);
}

void AsyncUpload::Shutdown() {
    delete instance.exchange(nullptr, std::memory_order_acq_rel);
}

AsyncUpload::AsyncUpload(u32 count, u64 min_bytes_) : min_bytes{min_bytes_} {
    for (u32 i = 0; i < count; ++i) {
        threads.emplace_back([this](std::stop_token token) { Worker(token); });
    }
}

AsyncUpload::~AsyncUpload() {
    for (auto& thread : threads) {
        thread.request_stop();
    }
    cv.notify_all();
    threads.clear(); // joins; queued tasks are still copied (see Worker)
}

void AsyncUpload::Queue(const void* owner, u64 tick, std::vector<Piece>&& pieces,
                        std::vector<Range>&& va, std::vector<Range>&& phys, Buffer* staging,
                        u64 flush_offset, u64 flush_size) {
    auto job = std::make_shared<Job>();
    job->owner = owner;
    job->tick = tick;
    job->staging = staging;
    job->flush_offset = flush_offset;
    job->flush_size = flush_size;
    // sotc: SOTC_FENCE_PIECES: the guest address and backing offset of each piece. The pieces
    // follow the va ranges in order (BufferCache::QueueAsyncGuestCopy splits each range at its
    // mappings) and each piece with a backing has the next phys range; when they do not line
    // up, the fence treats every piece of the job as reading the job's ranges.
    std::vector<std::pair<u64, u64>> starts; // va, phys of each piece
    bool has_ranges = true;
    {
        size_t vi = 0, pi = 0;
        u64 va_off = 0;
        for (const auto& piece : pieces) {
            while (vi < va.size() && va[vi].start + va_off >= va[vi].end) {
                ++vi;
                va_off = 0;
            }
            if (vi == va.size() || va[vi].start + va_off + piece.size > va[vi].end) {
                has_ranges = false;
                break;
            }
            u64 phys_start = 0;
            if (piece.src) {
                if (pi == phys.size() || phys[pi].end - phys[pi].start != piece.size) {
                    has_ranges = false;
                    break;
                }
                phys_start = phys[pi++].start;
            }
            starts.emplace_back(va[vi].start + va_off, phys_start);
            va_off += piece.size;
        }
        has_ranges = has_ranges && pi == phys.size();
    }
    job->va = std::move(va);
    job->phys = std::move(phys);
    // Split into pieces of up to 512 KiB, spread over the threads.
    constexpr u64 Split = 512_KB;
    u64 count = 0;
    for (const auto& piece : pieces) {
        count += (piece.size + Split - 1) / Split;
    }
    if (count == 0) {
        return;
    }
    job->slots = std::make_unique<Job::Slot[]>(count);
    job->count = static_cast<u32>(count);
    job->has_ranges = has_ranges;
    u32 index = 0;
    for (size_t i = 0; i < pieces.size(); ++i) {
        const auto& piece = pieces[i];
        for (u64 done = 0; done < piece.size; done += Split) {
            auto& slot = job->slots[index++];
            slot.piece = {piece.src ? piece.src + done : nullptr, piece.dst + done,
                          std::min(Split, piece.size - done)};
            if (has_ranges) {
                slot.va = starts[i].first + done;
                slot.phys = starts[i].second + done;
            }
        }
    }
    job->left.store(count, std::memory_order_relaxed);
    {
        std::scoped_lock lock{mutex};
        jobs.push_back(job);
        active.fetch_add(1, std::memory_order_acq_rel);
        for (u32 i = 0; i < job->count; ++i) {
            tasks.push_back({job, i});
        }
    }
    cv.notify_all();
    SotcStats::async_upload_jobs.fetch_add(1, std::memory_order_relaxed);
    SotcStats::async_upload_bytes.fetch_add(flush_size, std::memory_order_relaxed);
}

void AsyncUpload::Worker(std::stop_token token) {
    Common::SetCurrentThreadName("sotc:AsyncUpload");
    SotcStats::RegisterThreadRole(SotcStats::RoleUpload); // sotc: SOTC_FPS accounting
    BbThreads::PlaceThread(BbThreads::Role::Copy);
    for (;;) {
        Task task;
        {
            std::unique_lock lock{mutex};
            cv.wait(lock, token, [this] { return !tasks.empty(); });
            if (tasks.empty()) {
                return; // stop requested and nothing left to copy
            }
            task = std::move(tasks.front());
            tasks.pop_front();
        }
        if (task.index == FinishTask) {
            FinishJob(task.job); // a fence waiter copied the last piece
            continue;
        }
        auto& slot = task.job->slots[task.index];
        u32 expected = StateQueued;
        if (!slot.state.compare_exchange_strong(expected, StateClaimed,
                                                std::memory_order_acq_rel)) {
            continue; // claimed by a fence waiter (SOTC_FENCE_PIECES), which copies it
        }
        if (slot.piece.src) {
            std::memcpy(slot.piece.dst, slot.piece.src, slot.piece.size);
        } else {
            std::memset(slot.piece.dst, 0, slot.piece.size);
        }
        PieceDone(task.job, task.index, true);
    }
}

void AsyncUpload::PieceDone(const std::shared_ptr<Job>& job, u32 index, bool on_worker) {
    auto& slot = job->slots[index];
    slot.state.store(StateDone, std::memory_order_release);
    if (fence_pieces) {
        slot.state.notify_all();
    }
    if (job->left.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }
    if (on_worker) {
        FinishJob(job);
        return;
    }
    // sotc: SOTC_FENCE_PIECES: the fence waiter may hold any runtime lock; the staging flush
    // (a Vulkan call) and the job's end are left to a copy thread.
    {
        std::scoped_lock lock{mutex};
        tasks.push_front({job, FinishTask});
    }
    cv.notify_one();
}

void AsyncUpload::FinishJob(const std::shared_ptr<Job>& job) {
    if (job->staging) {
        job->staging->Flush(job->flush_offset, job->flush_size);
    }
    {
        std::scoped_lock lock{mutex};
        std::erase(jobs, job);
        active.fetch_sub(1, std::memory_order_acq_rel);
    }
    job->done.store(true, std::memory_order_release);
    job->done.notify_all();
}

template <typename Pred>
void AsyncUpload::WaitFor(Pred&& pred, bool fence) {
    std::vector<std::shared_ptr<Job>> waiting;
    {
        std::scoped_lock lock{mutex};
        for (const auto& job : jobs) {
            if (pred(*job)) {
                waiting.push_back(job);
            }
        }
    }
    if (waiting.empty()) {
        return;
    }
    const SotcStats::DrawWait wait{SotcStats::WaitUpload}; // sotc: SOTC_FPS accounting
    const auto start = std::chrono::steady_clock::now();
    for (const auto& job : waiting) {
        while (!job->done.load(std::memory_order_acquire)) {
            job->done.wait(false, std::memory_order_acquire);
        }
    }
    const u64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    if (fence) {
        SotcStats::async_upload_fence_waits.fetch_add(1, std::memory_order_relaxed);
        SotcStats::async_upload_fence_ns.fetch_add(ns, std::memory_order_relaxed);
    } else {
        SotcStats::async_upload_submit_ns.fetch_add(ns, std::memory_order_relaxed);
    }
}

void AsyncUpload::WaitUpTo(const void* owner, u64 tick) {
    if (!AnyPending()) {
        return;
    }
    // sotc: SOTC_FPS accounting: the recording thread's wait before its queue submit.
    const SotcStats::RoleTimer rec_wait{SotcStats::RoleRecorder, SotcStats::rec_upload_wait_ns};
    WaitFor([owner, tick](const Job& job) { return job.owner == owner && job.tick <= tick; },
            false);
}

static bool Intersects(const std::vector<AsyncUpload::Range>& ranges, u64 start, u64 end) {
    return std::ranges::any_of(ranges, [start, end](const AsyncUpload::Range& range) {
        return range.start < end && start < range.end;
    });
}

template <typename JobPred, typename SlotPred>
void AsyncUpload::WaitPieces(JobPred&& job_pred, SlotPred&& slot_pred) {
    struct Hit {
        std::shared_ptr<Job> job;
        u32 index;
    };
    boost::container::small_vector<Hit, 16> hits;
    {
        std::scoped_lock lock{mutex};
        for (const auto& job : jobs) {
            if (!job_pred(*job)) {
                continue;
            }
            for (u32 i = 0; i < job->count; ++i) {
                const auto& slot = job->slots[i];
                // A zero-filled piece (unmapped at upload time) reads no guest memory.
                if (!slot.piece.src ||
                    slot.state.load(std::memory_order_acquire) == StateDone ||
                    (job->has_ranges && !slot_pred(slot))) {
                    continue;
                }
                hits.push_back({job, i});
            }
        }
    }
    if (hits.empty()) {
        return;
    }
    const SotcStats::DrawWait wait{SotcStats::WaitUpload}; // sotc: SOTC_FPS accounting
    const auto start = std::chrono::steady_clock::now();
    // Copy the pieces no copy thread has started, then wait for the ones being copied.
    u64 helped = 0;
    for (const auto& hit : hits) {
        auto& slot = hit.job->slots[hit.index];
        u32 expected = StateQueued;
        if (slot.state.compare_exchange_strong(expected, StateClaimed,
                                               std::memory_order_acq_rel)) {
            std::memcpy(slot.piece.dst, slot.piece.src, slot.piece.size);
            PieceDone(hit.job, hit.index, false);
            ++helped;
        }
    }
    for (const auto& hit : hits) {
        auto& slot = hit.job->slots[hit.index];
        for (u32 state; (state = slot.state.load(std::memory_order_acquire)) != StateDone;) {
            slot.state.wait(state, std::memory_order_acquire);
        }
    }
    const u64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    SotcStats::async_upload_fence_waits.fetch_add(1, std::memory_order_relaxed);
    SotcStats::async_upload_fence_ns.fetch_add(ns, std::memory_order_relaxed);
    SotcStats::fence_piece_waits.fetch_add(1, std::memory_order_relaxed);
    SotcStats::fence_piece_ns.fetch_add(ns, std::memory_order_relaxed);
    if (helped) {
        SotcStats::fence_pieces_helped.fetch_add(helped, std::memory_order_relaxed);
    }
}

void AsyncUpload::WaitVa(u64 start, u64 size) {
    if (!AnyPending()) {
        return;
    }
    const u64 end = start + size;
    if (fence_pieces) {
        WaitPieces([start, end](const Job& job) { return Intersects(job.va, start, end); },
                   [start, end](const Job::Slot& slot) {
                       return slot.va < end && start < slot.va + slot.piece.size;
                   });
        return;
    }
    WaitFor([start, end](const Job& job) { return Intersects(job.va, start, end); }, true);
}

void AsyncUpload::WaitPhys(u64 start, u64 size) {
    if (!AnyPending()) {
        return;
    }
    const u64 end = start + size;
    if (fence_pieces) {
        WaitPieces([start, end](const Job& job) { return Intersects(job.phys, start, end); },
                   [start, end](const Job::Slot& slot) {
                       return slot.phys < end && start < slot.phys + slot.piece.size;
                   });
        return;
    }
    WaitFor([start, end](const Job& job) { return Intersects(job.phys, start, end); }, true);
}

} // namespace VideoCore
