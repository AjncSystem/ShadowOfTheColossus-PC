// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// sotc: SOTC_BIND_CACHE_FRAME. A log of "something changed in [lo, hi)" events, answered per range:
// "did any event overlapping this range happen after sequence number `seq`?". The bind cache
// keeps entries across frames and checks them against this instead of being wiped by a global
// generation: registering an unrelated image or writing an unrelated range leaves them alone.
//
// Conservative by construction (a wrong answer may only be "yes"): the ranges are tracked in
// 64 KiB and 1 MiB slots indexed by address bits, so two distant addresses can share a slot, and
// an event too large for the fine table is kept in the coarse one (and in `big_seq_`).
//
// Threads: Note() may be called from any thread (a mutex serializes the writers; the guest
// unmaps images on its own threads, the draw thread notes its GPU writes). Readers are lock
// free. Note() stores the slot stamps first and publishes the sequence number last (release),
// so a reader that loaded Seq() (acquire) sees the stamps of every event up to that number; an
// event published after is seen either now (stamp > the caller's checkpoint: reported changed)
// or at the next check (the checkpoint is the sequence number read before the check).

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

#include "common/types.h"

namespace VideoCore {

class RangeStamps {
public:
    static constexpr u32 FineShift = 16; ///< 64 KiB
    static constexpr u32 FineBits = 14;
    static constexpr u32 MidShift = 20; ///< 1 MiB
    static constexpr u32 MidBits = 13;
    /// An event spanning more fine slots than this is kept in the coarse table only.
    static constexpr u64 MaxFineSpan = 256;
    /// An event spanning more coarse slots than this is only `huge_seq_`.
    static constexpr u64 MaxMidSpan = 4096;
    /// A query spanning more fine slots than this reads the coarse table; more coarse slots than
    /// MaxQueryMid answers from the global sequence number (anything changed since).
    static constexpr u64 MaxQueryFine = 16;
    static constexpr u64 MaxQueryMid = 512;

    RangeStamps()
        : fine_{std::make_unique<std::atomic<u64>[]>(u64{1} << FineBits)},
          mid_{std::make_unique<std::atomic<u64>[]>(u64{1} << MidBits)} {}

    /// The number of events noted so far (acquire).
    [[nodiscard]] u64 Seq() const noexcept {
        return seq_.load(std::memory_order_acquire);
    }

    /// Notes an event on [lo, hi) and returns its sequence number. `extra`: a stamp slot of the
    /// caller's that gets the sequence number before it is published (a per-object stamp).
    u64 Note(VAddr lo, VAddr hi, std::atomic<u64>* extra = nullptr) {
        std::scoped_lock lock{mutex_};
        const u64 s = seq_.load(std::memory_order_relaxed) + 1;
        if (hi <= lo) {
            hi = lo + 1;
        }
        const u64 fine_first = lo >> FineShift;
        const u64 fine_last = (hi - 1) >> FineShift;
        if (fine_last - fine_first < MaxFineSpan) {
            for (u64 r = fine_first; r <= fine_last; ++r) {
                fine_[r & FineMask].store(s, std::memory_order_relaxed);
            }
        } else {
            big_seq_.store(s, std::memory_order_relaxed);
        }
        const u64 mid_first = lo >> MidShift;
        const u64 mid_last = (hi - 1) >> MidShift;
        if (mid_last - mid_first < MaxMidSpan) {
            for (u64 r = mid_first; r <= mid_last; ++r) {
                mid_[r & MidMask].store(s, std::memory_order_relaxed);
            }
        } else {
            huge_seq_.store(s, std::memory_order_relaxed);
        }
        if (extra) {
            extra->store(s, std::memory_order_relaxed);
        }
        seq_.store(s, std::memory_order_release);
        return s;
    }

    /// true when an event noted after `seq` may overlap [lo, hi).
    [[nodiscard]] bool ChangedSince(VAddr lo, VAddr hi, u64 seq) const noexcept {
        if (hi <= lo) {
            hi = lo + 1;
        }
        if (huge_seq_.load(std::memory_order_relaxed) > seq) {
            return true;
        }
        const u64 fine_first = lo >> FineShift;
        const u64 fine_last = (hi - 1) >> FineShift;
        if (fine_last - fine_first < MaxQueryFine) {
            if (big_seq_.load(std::memory_order_relaxed) > seq) {
                return true;
            }
            for (u64 r = fine_first; r <= fine_last; ++r) {
                if (fine_[r & FineMask].load(std::memory_order_relaxed) > seq) {
                    return true;
                }
            }
            return false;
        }
        const u64 mid_first = lo >> MidShift;
        const u64 mid_last = (hi - 1) >> MidShift;
        if (mid_last - mid_first >= MaxQueryMid) {
            return seq_.load(std::memory_order_acquire) > seq;
        }
        for (u64 r = mid_first; r <= mid_last; ++r) {
            if (mid_[r & MidMask].load(std::memory_order_relaxed) > seq) {
                return true;
            }
        }
        return false;
    }

private:
    static constexpr u64 FineMask = (u64{1} << FineBits) - 1;
    static constexpr u64 MidMask = (u64{1} << MidBits) - 1;

    std::mutex mutex_;
    std::atomic<u64> seq_{0};
    std::atomic<u64> big_seq_{0};  ///< newest event too wide for the fine table
    std::atomic<u64> huge_seq_{0}; ///< newest event too wide for the coarse table
    std::unique_ptr<std::atomic<u64>[]> fine_;
    std::unique_ptr<std::atomic<u64>[]> mid_;
};

} // namespace VideoCore
