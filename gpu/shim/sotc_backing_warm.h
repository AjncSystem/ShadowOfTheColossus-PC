// sotc: SOTC_BACKING_WARM=1 (off by default). Guest memory is one section mapped twice: the
// guest views and the backing view the GPU uploads read (CopySparseSerial). Each view has its own
// page-table entries, so the first backing read of a page the guest wrote through its own view is
// a soft page fault (~0.8 us per 4 KiB page, measured), which held the draw thread's upload memcpy
// at ~3 GB/s. The guest write-fault handler queues the range it makes writable (Note), and a
// below-normal priority thread reads one byte per page of that range through the backing view, so
// the upload that follows finds the entries present and the soft faults are paid off the draw
// thread. Measured on 30 Oct 2026 (one area-load route each): no gain (slow-frame copies 6.4
// GB/s against 6.2 without it and 7.9 with SOTC_COPY_GUEST_VIEW), 1.66M pages warmed. Kept, off.
#pragma once

#include <cstdint>

namespace SotcBackingWarm {

/// SOTC_BACKING_WARM=1, read at static initialisation.
bool Enabled() noexcept;

/// Starts the warm thread when enabled (bbgpu_init). Idempotent.
void Start();

/// Queues [addr, addr + size) (guest addresses). Lock-free, no allocation, no system call: safe
/// inside the exception handler. A full queue drops the range (counted).
void Note(std::uint64_t addr, std::uint64_t size) noexcept;

} // namespace SotcBackingWarm
