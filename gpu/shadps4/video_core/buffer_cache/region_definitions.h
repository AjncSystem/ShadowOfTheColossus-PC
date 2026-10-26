// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <utility>

#include "common/types.h"

namespace VideoCore {

constexpr u64 PAGES_PER_WORD = 64;
constexpr u64 BYTES_PER_PAGE = 4_KB;
constexpr u64 BYTES_PER_WORD = PAGES_PER_WORD * BYTES_PER_PAGE;

constexpr u64 HIGHER_PAGE_BITS = 24;
constexpr u64 HIGHER_PAGE_SIZE = 1ULL << HIGHER_PAGE_BITS;
constexpr u64 HIGHER_PAGE_MASK = HIGHER_PAGE_SIZE - 1ULL;

constexpr u64 NUM_REGION_PAGES = HIGHER_PAGE_SIZE / BYTES_PER_PAGE;
constexpr u64 NUM_REGION_WORDS = HIGHER_PAGE_SIZE / BYTES_PER_WORD;

class RegionManager;

/// sotc: BbToggle::BufferBindMemo. The CPU-bit generation (RegionManager::CpuGenSum) of the 256 KiB
/// words a range covers, remembered by a bind that found no CPU-modified page in it
/// (MemoryTracker::SnapshotCpuGen / CpuGenUnchanged).
struct CpuGenKey {
    RegionManager* region{};
    u32 first_word{};
    u32 words{};
    u64 sum{};
};

enum class Type : u8 {
    CPU = 1 << 0,
    GPU = 1 << 1,
};

enum class StateOp : u8 {
    None = 0,
    Set = 1,
    Clear = 2,
};

constexpr bool operator&(Type a, Type b) noexcept {
    return std::to_underlying(a) & std::to_underlying(b);
}

constexpr Type operator|(Type a, Type b) noexcept {
    return static_cast<Type>(std::to_underlying(a) | std::to_underlying(b));
}

struct Bounds {
    u64 start_word;
    u64 start_page;
    u64 end_word;
    u64 end_page;
};

struct RegionBits {
    constexpr void Fill(u64 value) {
        data.fill(value);
    }

    constexpr bool GetPage(u64 page) const {
        return data[page / PAGES_PER_WORD] & (1ULL << (page % PAGES_PER_WORD));
    }

    constexpr u64& operator[](u64 index) {
        return data[index];
    }

private:
    alignas(64) std::array<u64, NUM_REGION_WORDS> data;
};

} // namespace VideoCore
