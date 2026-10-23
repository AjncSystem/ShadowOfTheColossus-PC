// bbport: the MemoryManager surface used by the video core and GnmDriver.
// Guest memory lives at identical host addresses; the C runtime owns the VMAs.
#pragma once
#include "common/types.h"
#include "common/singleton.h"
#include "core/address_space.h"
#include "video_core/sotc_stats.h"

namespace Vulkan { class Rasterizer; }

namespace Core {
class MemoryManager {
public:
    void SetRasterizer(Vulkan::Rasterizer* rasterizer_);
    AddressSpace& GetAddressSpace() { return impl; }
    bool IsValidGpuMapping(VAddr virtual_addr, u64 size) {
        // The PS4's GPU can only handle 40 bit addresses.
        return virtual_addr + size < 0x100'0000'0000ULL;
    }
    u64 ClampRangeSize(VAddr virtual_addr, u64 size);
    void CopySparseMemory(VAddr source, u8* dest, u64 size);
    /// sotc: source tags the writer in the shadPS4 SotC fork's diagnostics; unused here.
    bool TryWriteBacking(void* address, const void* data, u64 size, u32 source = 0);
    /// sotc: the API of the shadPS4 SotC fork's GPU fixes, over the C runtime's VMAs.
    bool IsValidMapping(VAddr virtual_addr, u64 size = 0);
    bool IsMappedAddress(VAddr virtual_addr);
    bool TryCopySparseMemory(VAddr source, u8* dest, u64 size);
    const u8* GetBackingPointer(VAddr address) const;
    /// sotc (bpe_heap_guard): the backing view of the mapping holding address, as one run.
    const u8* GetBackingRun(VAddr address, VAddr* run_start, u64* run_size) const;
    bool TryReadBacking(VAddr address, void* out, u64 size) {
        return TryCopySparseMemory(address, static_cast<u8*>(out), size);
    }
    static void NoteHostWrite(VAddr, u64, u32, u64 = ~0ULL) {
        SotcStats::mem_epoch.fetch_add(1, std::memory_order_relaxed); // reused binds (sotc_stats.h)
    }
    /// CPU wrote guest memory outside page tracking (e.g. decoded video frames).
    void InvalidateMemory(VAddr address, u64 size);
    Vulkan::Rasterizer* GetRasterizer() const { return rasterizer; }

private:
    AddressSpace impl;
    Vulkan::Rasterizer* rasterizer{};
};
using Memory = Common::Singleton<MemoryManager>;
} // namespace Core
