// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the game's memory for the GPU in place, the "PC memory model" (docs/sotc/MODELO-MEMORIA-PC.md).
//
// The backing view of the game's memory (src/runtime_memory.c: the pagefile section's second view,
// which bypasses the guest's page protection) is imported into Vulkan with
// VK_EXT_external_memory_host, in 64 MiB chunks the first time one is asked for: importing pins
// the pages in RAM, so only the chunks in use are. Each chunk is a buffer the GPU can copy to and
// from: what it writes there is the game's memory at once, with no staging buffer and no memcpy.
// SOTC_PC_MEMORY=1 turns it on (the device extension is asked for only then).

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace Vulkan::SotcGuestMemory {

/// After the device exists (Rasterizer): on with the extension, the backing view found later.
void Init(const Instance& instance);

/// At each submission: looks for the backing view until the game has mapped memory, then
/// checks the import once (the first chunk). True when the game's memory can be used in place.
bool Ready();

/// True when the game's memory can be used in place (no lookup).
bool Enabled();

struct Span {
    vk::Buffer buffer; ///< null: not importable (off, outside the backing view, across two chunks)
    u64 offset{};      ///< of the first byte in buffer
};

/// The imported chunk holding [host, host + size) of the backing view.
Span Get(const u8* host, u64 size);

} // namespace Vulkan::SotcGuestMemory
