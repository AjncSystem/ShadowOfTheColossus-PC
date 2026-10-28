// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: dynamic_tsharp_array_size from Pink-shadPS4 (luizgustavs): T# tables a shader indexes at
// run time (SotC's local IBL reads each reflection probe's cubemap from such a table).

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include "common/elf_info.h"
#include "core/memory.h"
#include "shader_recompiler/info.h"

namespace Shader {

void Info::RefreshSharpTables() {
    constexpr VAddr AddressMask = 0xFFFFFFFFFFFFULL;
    auto* memory = Core::Memory::Instance();
    auto& flattened_ud_buf = FlatBuf();
    for (const SharpTable& table : sharp_tables) {
        u32* dst = flattened_ud_buf.data() + table.flatbuf_off;
        const u64 size = u64{table.NumDwords()} * sizeof(u32);
        const VAddr ptr = (VAddr{flattened_ud_buf[table.ptr_lo]} |
                           VAddr{flattened_ud_buf[table.ptr_hi]} << 32) &
                          AddressMask;
        const VAddr src = ptr + u64{table.offset_dw} * sizeof(u32);
        // Through the backing: a garbage table pointer may point at free address space, where a
        // direct read is a host access violation. The tables are written by the CPU, so the
        // backing is current.
        if (ptr == 0 || !memory->TryReadBacking(src, dst, size)) {
            // Null T#s: the array binds null images, like the unflattened sharp did
            std::memset(dst, 0, size);
        }
    }
}

bool StageBindingsEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_STAGE_BINDINGS");
        return v && v[0] == '1';
    }();
    return enabled;
}

static std::atomic<bool> g_bindless_samplers{false};

void SetBindlessSamplersEnabled(bool enabled) {
    g_bindless_samplers.store(enabled, std::memory_order_relaxed);
}

bool BindlessSamplersEnabled() {
    return g_bindless_samplers.load(std::memory_order_relaxed);
}

bool UsesBindlessSamplers(const Info& info) {
    if (!BindlessSamplersEnabled() || info.samplers.empty()) {
        return false;
    }
    return info.samplers.size() <= GetBindlessSamplerRange(info.sw_stage).count;
}

bool LodFeedbackEnabled() {
    static const bool enabled = [] {
        // Off by default: using the measured mips made the game's streaming churn (one 150 s
        // run at 21 FPS average, 22 Oct). SOTC_LOD_FEEDBACK=1 turns it on for tests.
        const char* v = std::getenv("SOTC_LOD_FEEDBACK");
        return v && v[0] == '1';
    }();
    return enabled;
}

// SOTC_TSHARP_ARRAY=<n> (0 = off); SotC defaults to 16 like Pink's preset.
u32 DynamicTsharpArraySize() {
    static const u32 size = [] {
        if (const char* v = std::getenv("SOTC_TSHARP_ARRAY")) {
            return static_cast<u32>(std::strtoul(v, nullptr, 10));
        }
        return Common::ElfInfo::Instance().IsSotc() ? 16u : 0u;
    }();
    return std::min(size, MAX_TSHARP_ARRAY_SIZE);
}

} // namespace Shader
