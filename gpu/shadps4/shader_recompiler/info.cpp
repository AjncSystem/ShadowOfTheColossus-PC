// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: dynamic_tsharp_array_size from Pink-shadPS4 (luizgustavs): T# tables a shader indexes at
// run time (SotC's local IBL reads each reflection probe's cubemap from such a table).

#include <algorithm>
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

// SOTC_TSHARP_ARRAY=<n> (0 = off); SotC defaults to 16 like Pink's preset.
u32 DynamicTsharpArraySize() {
    static const u32 size = [] {
        if (const char* v = std::getenv("SOTC_TSHARP_ARRAY")) {
            return static_cast<u32>(std::strtoul(v, nullptr, 10));
        }
        return Common::ElfInfo::Instance().GameSerial() == "CUSA08809" ? 16u : 0u;
    }();
    return std::min(size, MAX_TSHARP_ARRAY_SIZE);
}

} // namespace Shader
