// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: stack unwinding over a COPY of a thread's stack, for the trace sampler (trace v2, phase 5).
//
// The sampler suspends a thread, copies its registers and the top of its stack, resumes it at
// once, and only then walks the copy. Nothing here takes a lock, allocates or calls the OS:
// every lookup table is built at init.
//   Guest frames: the eboot's .eh_frame CFI (exact CFA at any RIP, including prologues and
//                 frameless leaves), else the rbp chain. rbp == 0 is a fiber root: the walk stops.
//   Host frames:  the PE .pdata/UNWIND_INFO of every module registered with AddHostModule,
//                 interpreted on the copy (RtlVirtualUnwind is not used: it reads the live stack).
#pragma once

#include <cstddef>
#include <cstdint>

namespace SotcUnwind {

/// A copy of [base, base + size) of a thread's stack; reads outside it fail.
struct StackCopy {
    uint64_t base = 0; ///< address of the first copied byte (the rsp at the sample)
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    bool Read(uint64_t addr, uint64_t& out) const;
};

/// The registers a walk starts from (from GetThreadContext).
struct Regs {
    uint64_t rip = 0, rsp = 0, rbp = 0;
};

/// Builds the guest lookup from the eboot's eh_frame_hdr (the same data the CPU patcher uses:
/// image base, eh_frame_hdr address and size). Call once, before sampling. False on bad data.
bool InitGuest(uint64_t image_base, uint64_t image_size, uint64_t eh_frame_hdr, uint64_t hdr_size);
/// Registers a host PE module mapped at `base` (bb-probe.exe, ntdll, the Vulkan driver...): parses
/// its exception directory. Call outside the suspended window. Up to 64 modules.
bool AddHostModule(uint64_t base);
/// Trampoline/thunk areas (import thunks, detour stubs): a return address inside one is reported
/// as a frame of kind Thunk and the walk asks `thunk_ra` for the real return address.
void AddThunkRange(uint64_t start, uint64_t size);

enum FrameKind : uint8_t { Guest, Host, Thunk, Unknown };

/// Walks from `regs` over `stack`, writing up to `max` return addresses (leaf first: frames[0] =
/// regs.rip). `thunk_ra(i)` returns the guest return address saved for the i-th thunk frame met
/// (the shadow stack, copied with the sample), or 0. Returns the frame count; *truncated is set
/// when the walk stopped on an unknown frame or a failed read instead of a stack root.
using ThunkResolver = uint64_t (*)(void* ctx, uint32_t thunk_index);
uint32_t Walk(const Regs& regs, const StackCopy& stack, uint64_t* frames, uint32_t max,
              bool* truncated, ThunkResolver thunk_ra = nullptr, void* ctx = nullptr);

/// For tests and the sampler's own stats.
FrameKind KindOf(uint64_t addr);

} // namespace SotcUnwind
