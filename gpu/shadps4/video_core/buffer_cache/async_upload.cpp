// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include "common/thread.h"
#include "video_core/buffer_cache/async_upload.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/sotc_stats.h"
#include "bbport_threads.h"

namespace VideoCore {

struct AsyncUpload::Job {
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
    if (!env || env[0] != '1' || Get()) {
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
    job->va = std::move(va);
    job->phys = std::move(phys);
    job->staging = staging;
    job->flush_offset = flush_offset;
    job->flush_size = flush_size;
    // Split into pieces of up to 512 KiB, spread over the threads.
    constexpr u64 Split = 512_KB;
    std::vector<Piece> split;
    for (const auto& piece : pieces) {
        for (u64 done = 0; done < piece.size; done += Split) {
            split.push_back({piece.src ? piece.src + done : nullptr, piece.dst + done,
                             std::min(Split, piece.size - done)});
        }
    }
    if (split.empty()) {
        return;
    }
    job->left.store(split.size(), std::memory_order_relaxed);
    {
        std::scoped_lock lock{mutex};
        jobs.push_back(job);
        active.fetch_add(1, std::memory_order_acq_rel);
        for (const auto& piece : split) {
            tasks.push_back({job, piece});
        }
    }
    cv.notify_all();
    SotcStats::async_upload_jobs.fetch_add(1, std::memory_order_relaxed);
    SotcStats::async_upload_bytes.fetch_add(flush_size, std::memory_order_relaxed);
}

void AsyncUpload::Worker(std::stop_token token) {
    Common::SetCurrentThreadName("sotc:AsyncUpload");
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
        if (task.piece.src) {
            std::memcpy(task.piece.dst, task.piece.src, task.piece.size);
        } else {
            std::memset(task.piece.dst, 0, task.piece.size);
        }
        Job& job = *task.job;
        if (job.left.fetch_sub(1, std::memory_order_acq_rel) != 1) {
            continue;
        }
        if (job.staging) {
            job.staging->Flush(job.flush_offset, job.flush_size);
        }
        {
            std::scoped_lock lock{mutex};
            std::erase(jobs, task.job);
            active.fetch_sub(1, std::memory_order_acq_rel);
        }
        job.done.store(true, std::memory_order_release);
        job.done.notify_all();
    }
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
    WaitFor([owner, tick](const Job& job) { return job.owner == owner && job.tick <= tick; },
            false);
}

static bool Intersects(const std::vector<AsyncUpload::Range>& ranges, u64 start, u64 end) {
    return std::ranges::any_of(ranges, [start, end](const AsyncUpload::Range& range) {
        return range.start < end && start < range.end;
    });
}

void AsyncUpload::WaitVa(u64 start, u64 size) {
    if (!AnyPending()) {
        return;
    }
    const u64 end = start + size;
    WaitFor([start, end](const Job& job) { return Intersects(job.va, start, end); }, true);
}

void AsyncUpload::WaitPhys(u64 start, u64 size) {
    if (!AnyPending()) {
        return;
    }
    const u64 end = start + size;
    WaitFor([start, end](const Job& job) { return Intersects(job.phys, start, end); }, true);
}

} // namespace VideoCore
