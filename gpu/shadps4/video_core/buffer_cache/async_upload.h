// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: SOTC_ASYNC_UPLOAD=1 (off by default). The CPU copy of a large buffer upload (guest
// memory into staging) runs on a pool of copy threads instead of the draw thread, which records
// the staging-to-arena copy at once and goes on.
//
// Two rules keep it as correct as the copy on the draw thread:
// - Submit: the GPU submit that holds the staging-to-arena copy waits for the jobs of its tick
//   (WaitUpTo, called by Scheduler::SubmitExecution before the queue submit).
// - Write fence: guest memory a job still has to read is not changed before it has: a guest
//   write fault near it (the page is write-protected since the upload; the write-ahead and fault
//   granule unprotect neighbours too), an unmap of it, the runtime zeroing freed memory and the
//   runtime's backing writes all wait for the job first (WaitVa / WaitPhys).
// A job only reads through the backing view, resolved on the draw thread, and takes no lock of
// the runtime or of the GPU side: every wait depends on the copy threads alone, so none can
// deadlock, whatever lock the waiting thread holds. Uploads with pages the tracker leaves
// unprotected (hot pages of the fault grace) stay on the draw thread.
//
// SOTC_FENCE_PIECES=1 (off by default) narrows the write fence: it waits only for the pieces
// (<= 512 KiB) that read the asked range, not for every piece of the jobs touching it, and the
// waiting thread copies the pieces no copy thread has started yet itself (each piece goes
// queued -> claimed -> done once, whoever claims it copies it). Guest read faults do not wait
// (bbgpu_handle_fault): a read cannot change what a copy reads and leaves the page protected.
// A job whose last piece a waiting thread copied is finished (staging flush, done) by a copy
// thread, so the waiting thread calls no Vulkan function.

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "common/types.h"

namespace VideoCore {

class Buffer;

class AsyncUpload {
public:
    /// From guest memory (src: its backing; nullptr: unmapped, zero filled) to staging.
    struct Piece {
        const u8* src;
        u8* dst;
        u64 size;
    };
    struct Range {
        u64 start;
        u64 end;
    };
    struct Job;

    /// Creates the pool when SOTC_ASYNC_UPLOAD=1 (the buffer cache calls it once).
    static void Init();
    /// Stops the pool (the buffer cache's destructor).
    static void Shutdown();
    /// The pool, or nullptr when both switches are off.
    static AsyncUpload* Get() noexcept {
        return instance.load(std::memory_order_acquire);
    }
    /// SOTC_ASYNC_UPLOAD=1: buffer uploads use the pool.
    static bool BuffersEnabled() noexcept {
        return buffers_enabled;
    }
    /// SOTC_ASYNC_TEXTURE_UPLOAD=1: the guest copies of texture uploads use the pool.
    static bool TexturesEnabled() noexcept {
        return textures_enabled;
    }

    /// SOTC_FENCE_PIECES=1: the write fence waits for (and helps with) single pieces.
    static bool FencePieces() noexcept {
        return fence_pieces;
    }

    [[nodiscard]] u64 MinBytes() const noexcept {
        return min_bytes;
    }

    /// Queues the copies of one upload. owner/tick: the scheduler and the tick being recorded,
    /// whose submit needs them. va/phys: the guest addresses and backing offsets they read.
    /// staging, flush_offset, flush_size: flushed once the copies are done.
    void Queue(const void* owner, u64 tick, std::vector<Piece>&& pieces, std::vector<Range>&& va,
               std::vector<Range>&& phys, Buffer* staging, u64 flush_offset, u64 flush_size);

    /// Waits for the jobs of owner up to tick (before that submit).
    void WaitUpTo(const void* owner, u64 tick);
    /// Waits for the jobs reading [start, start + size) of guest addresses / backing offsets.
    void WaitVa(u64 start, u64 size);
    void WaitPhys(u64 start, u64 size);

    [[nodiscard]] bool AnyPending() const noexcept {
        return active.load(std::memory_order_acquire) != 0;
    }

    ~AsyncUpload();

private:
    AsyncUpload(u32 threads, u64 min_bytes);
    void Worker(std::stop_token token);
    template <typename Pred>
    void WaitFor(Pred&& pred, bool fence);
    /// SOTC_FENCE_PIECES: waits for the pieces of the jobs job_pred selects that slot_pred
    /// selects (every piece of a job without per-piece ranges), copying the unclaimed ones.
    template <typename JobPred, typename SlotPred>
    void WaitPieces(JobPred&& job_pred, SlotPred&& slot_pred);
    /// A piece is copied: marks it done and finishes its job after the last one (inline on a
    /// copy thread, through a finish task otherwise).
    void PieceDone(const std::shared_ptr<Job>& job, u32 index, bool on_worker);
    void FinishJob(const std::shared_ptr<Job>& job);

    /// index: the job's piece, or FinishTask (the job's pieces are copied: finish it).
    static constexpr u32 FinishTask = ~u32{0};
    struct Task {
        std::shared_ptr<Job> job;
        u32 index;
    };

    static inline std::atomic<AsyncUpload*> instance{nullptr};
    static inline bool buffers_enabled{};
    static inline bool textures_enabled{};
    static inline bool fence_pieces{};

    u64 min_bytes;
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<Task> tasks;
    std::vector<std::shared_ptr<Job>> jobs; ///< queued and not done
    std::atomic<u32> active{0};
    std::vector<std::jthread> threads;
};

} // namespace VideoCore
