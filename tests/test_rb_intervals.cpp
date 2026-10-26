// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: interval subtraction of the asynchronous readback finish (docs/sotc/READBACK-PLAN.md)
// against a per-byte oracle: 4000 random cases with overlaps, adjacent, empty and out-of-range
// intervals.
// sotc: Build: clang++ -std=c++20 -Wall -Wextra -Werror -Igpu/shadps4/video_core/buffer_cache
// sotc:        tests/test_rb_intervals.cpp -o out/rb-intervals-test.exe
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>
#include "rb_intervals.h"

namespace {
using RbIntervals::Interval;

constexpr std::uint64_t Universe = 192;
int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL line %d: %s (case %d)\n", __LINE__, #cond, current_case);   \
            ++failures;                                                                            \
            return;                                                                                \
        }                                                                                          \
    } while (0)

int current_case = 0;

std::vector<bool> Bytes(const std::vector<Interval>& v) {
    std::vector<bool> bits(Universe + 64, false);
    for (const Interval& i : v) {
        for (std::uint64_t x = i.lo; x < i.hi && x < bits.size(); ++x) {
            bits[x] = true;
        }
    }
    return bits;
}

void OneCase(std::mt19937_64& rng) {
    const auto rnd = [&](std::uint64_t n) { return rng() % n; };
    std::vector<Interval> raw;
    const std::uint64_t count = rnd(12);
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint64_t lo = rnd(Universe + 16);
        std::uint64_t len = rnd(6) == 0 ? 0 : rnd(40);
        if (rnd(5) == 0 && !raw.empty()) { // adjacent to a previous one
            const Interval& p = raw[rnd(raw.size())];
            raw.push_back({p.hi, p.hi + len});
            continue;
        }
        raw.push_back({lo, lo + len});
    }
    const std::vector<bool> stale = Bytes(raw);
    std::vector<Interval> merged = raw;
    RbIntervals::Normalize(merged);
    // Normalized: ascending, disjoint, not adjacent, non-empty, same bytes.
    for (std::size_t i = 0; i < merged.size(); ++i) {
        CHECK(merged[i].lo < merged[i].hi);
        if (i != 0) {
            CHECK(merged[i - 1].hi < merged[i].lo);
        }
    }
    CHECK(Bytes(merged) == stale);

    const std::uint64_t lo = rnd(Universe);
    const std::uint64_t len = rnd(8) == 0 ? 0 : rnd(Universe + 32 - lo);
    const std::uint64_t hi = lo + len;
    std::vector<bool> outside(Universe + 64, false), inside(Universe + 64, false);
    std::uint64_t last_end = 0;
    bool any = false;
    RbIntervals::ForEachOutside(lo, hi, merged, [&](std::uint64_t a, std::uint64_t b) {
        CHECK(a < b);
        CHECK(a >= lo && b <= hi);
        CHECK(!any || a > last_end); // ascending and maximal (a stale byte lies between pieces)
        any = true;
        last_end = b;
        for (std::uint64_t x = a; x < b; ++x) {
            CHECK(!outside[x]);
            outside[x] = true;
        }
    });
    last_end = 0;
    any = false;
    RbIntervals::ForEachInside(lo, hi, merged, [&](std::uint64_t a, std::uint64_t b) {
        CHECK(a < b);
        CHECK(a >= lo && b <= hi);
        CHECK(!any || a > last_end);
        any = true;
        last_end = b;
        for (std::uint64_t x = a; x < b; ++x) {
            CHECK(!inside[x]);
            inside[x] = true;
        }
    });
    for (std::uint64_t x = 0; x < Universe + 64; ++x) {
        const bool in_range = x >= lo && x < hi;
        CHECK(outside[x] == (in_range && !stale[x]));
        CHECK(inside[x] == (in_range && stale[x]));
    }

    // Copy splitting keeps the destination displacement: byte i of the copy maps to dst + i.
    const std::uint64_t dst = rnd(1000);
    std::vector<std::int64_t> mapped(Universe + 64, -1);
    RbIntervals::ForEachCopyOutside(lo, dst, len, merged,
                                    [&](std::uint64_t a, std::uint64_t d, std::uint64_t n) {
                                        CHECK(n != 0);
                                        for (std::uint64_t i = 0; i < n; ++i) {
                                            CHECK(mapped[a + i] < 0);
                                            mapped[a + i] = static_cast<std::int64_t>(d + i);
                                        }
                                    });
    for (std::uint64_t x = 0; x < Universe + 64; ++x) {
        const bool kept = x >= lo && x < hi && !stale[x];
        CHECK((mapped[x] >= 0) == kept);
        if (kept) {
            CHECK(static_cast<std::uint64_t>(mapped[x]) == dst + (x - lo));
        }
    }
}
} // namespace

int main() {
    std::mt19937_64 rng(0x5eed1234);
    for (current_case = 0; current_case < 4000; ++current_case) {
        OneCase(rng);
    }
    // A boost-style small container works too: Normalize only needs begin/end/resize/index.
    std::vector<Interval> empty;
    RbIntervals::Normalize(empty);
    int pieces = 0;
    RbIntervals::ForEachOutside(5, 9, empty, [&](std::uint64_t a, std::uint64_t b) {
        pieces += (a == 5 && b == 9);
    });
    if (pieces != 1) {
        std::fprintf(stderr, "FAIL: empty stale list\n");
        ++failures;
    }
    if (failures) {
        std::fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    std::printf("rb intervals: 4000 cases ok\n");
    return 0;
}
