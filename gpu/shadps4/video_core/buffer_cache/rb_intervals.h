// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: interval arithmetic of the asynchronous readback finish (docs/sotc/READBACK-PLAN.md). A
// readback in flight collects the byte intervals other events changed meanwhile ("stale"); what
// is applied is its copies minus those intervals, what stays GPU-modified is the rest. Header only
// and free of the emulator's types so tests/test_rb_intervals.cpp checks it against a per-byte
// oracle.
#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>

namespace RbIntervals {

struct Interval {
    std::uint64_t lo;
    std::uint64_t hi; ///< exclusive; empty when hi <= lo
};

/// Sorts `v` by `lo` and unions overlapping and adjacent intervals in place; empty ones are
/// dropped. Works on std::vector and boost small_vector of Interval (anything with begin, end,
/// resize and random access). The result is the "normalized" form the functions below need:
/// ascending, disjoint, not adjacent, non-empty.
template <typename Vector>
void Normalize(Vector& v) {
    std::sort(v.begin(), v.end(), [](const Interval& a, const Interval& b) {
        return a.lo != b.lo ? a.lo < b.lo : a.hi < b.hi;
    });
    std::size_t out = 0;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i].hi <= v[i].lo) {
            continue;
        }
        if (out != 0 && v[i].lo <= v[out - 1].hi) {
            v[out - 1].hi = std::max(v[out - 1].hi, v[i].hi);
        } else {
            v[out++] = v[i];
        }
    }
    v.resize(out);
}

/// Calls emit(lo, hi) for each maximal piece of [lo, hi) that no interval of `normalized` covers,
/// in ascending order.
template <typename Vector, typename Emit>
void ForEachOutside(std::uint64_t lo, std::uint64_t hi, const Vector& normalized, Emit&& emit) {
    if (hi <= lo) {
        return;
    }
    // First interval that can reach lo: binary search on hi.
    std::size_t first = 0, count = normalized.size();
    while (count > 0) {
        const std::size_t half = count / 2;
        if (normalized[first + half].hi <= lo) {
            first += half + 1;
            count -= half + 1;
        } else {
            count = half;
        }
    }
    std::uint64_t cursor = lo;
    for (std::size_t i = first; i < normalized.size() && normalized[i].lo < hi; ++i) {
        if (normalized[i].lo > cursor) {
            emit(cursor, normalized[i].lo);
        }
        cursor = std::max(cursor, normalized[i].hi);
        if (cursor >= hi) {
            return;
        }
    }
    if (cursor < hi) {
        emit(cursor, hi);
    }
}

/// Calls emit(lo, hi) for each maximal piece of [lo, hi) that intervals of `normalized` cover.
template <typename Vector, typename Emit>
void ForEachInside(std::uint64_t lo, std::uint64_t hi, const Vector& normalized, Emit&& emit) {
    if (hi <= lo) {
        return;
    }
    for (const Interval& interval : normalized) {
        if (interval.hi <= lo) {
            continue;
        }
        if (interval.lo >= hi) {
            break;
        }
        emit(std::max(lo, interval.lo), std::min(hi, interval.hi));
    }
}

/// A copy of `size` bytes from guest address `lo` (staging offset `dst`) without the bytes
/// `normalized` covers: emit(piece_lo, piece_dst, piece_size) for each remaining piece, the
/// destination offset keeping its displacement from `dst`.
template <typename Vector, typename Emit>
void ForEachCopyOutside(std::uint64_t lo, std::uint64_t dst, std::uint64_t size,
                        const Vector& normalized, Emit&& emit) {
    ForEachOutside(lo, lo + size, normalized, [&](std::uint64_t a, std::uint64_t b) {
        emit(a, dst + (a - lo), b - a);
    });
}

} // namespace RbIntervals
