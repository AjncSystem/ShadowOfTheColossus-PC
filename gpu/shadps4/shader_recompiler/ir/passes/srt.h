// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <boost/container/set.hpp>
#include <boost/container/small_vector.hpp>
#include "common/types.h"

namespace Serialization {
struct Archive;
}

namespace Shader {

/// Host helper the walker calls to copy dynamic-index windows (see ReadConstDynamicWindowFlag).
void SrtCopyWindow(u32* dst, u64 src, u64 bytes);
using PFN_SrtCopyWindow = void (*)(u32*, u64, u64);

/// Fast path for clean walker reads (from srt_walker_clean_reads of Pink-shadPS4, luizgustavs):
/// pages where the fault handler served a clean read get a bit in clean_pages, and the walker
/// asks clean_load for loads in them instead of faulting again.
constexpr u64 SrtCleanPageLimit = 1ULL << 40;
struct SrtWalkerContext {
    u64* clean_pages;                                    // offset 0
    bool (*clean_load)(u64 address, u32 size, u64* out); // offset 8
};
const SrtWalkerContext* GetSrtWalkerContext();
/// sotc: for draw preparation helpers: the same clean page bitmap, with loads from those pages
/// served from the backing view (no readback, no shared cache; the command processor validates
/// whatever a helper read).
const SrtWalkerContext* GetSrtHelperWalkerContext();
void SetSrtCleanLoad(bool (*clean_load)(u64, u32, u64*));
void MarkSrtCleanPages(u64 address, u32 size);
/// sotc: keeps clean_pages equal to the pages currently read-protected (page manager), so
/// walker loads from pages no longer protected stop going through clean_load.
void SetSrtReadProtectedPages(u64 address, u64 size, bool read_protected);

using PFN_SrtWalker = void PS4_SYSV_ABI (*)(const u32* /*user_data*/, u32* /*flat_dst*/,
                                            PFN_SrtCopyWindow /*copy_window*/,
                                            const SrtWalkerContext* /*context*/);

/// ReadConst flag marking a load whose dword index is only known at shader run time (e.g. a
/// loop counter). The SRT walker copies a window of the pointed-to memory into the flattened
/// buffer; the low bits hold the window's first dword and the index is clamped to its size.
constexpr u32 ReadConstDynamicWindowFlag = 1u << 31;
constexpr u32 ReadConstDynamicWindowDwords = 512;
PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size);
/// True when code is inside the generated SRT walkers.
bool IsSrtWalkerCode(const void* code);

struct PersistentSrtInfo {
    // Special case when fetch shader uses step rates.
    struct SrtSharpReservation {
        u32 sgpr_base;
        u32 dword_offset;
        u32 num_dwords;
    };

    PFN_SrtWalker walker_func{};
    size_t walker_func_size{};
    u32 flattened_bufsize_dw = 16; // NumUserDataRegs

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader
