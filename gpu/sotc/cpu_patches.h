// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "common/types.h"

// ============================================================================
// Windows static guest red-zone protection
// ============================================================================

enum class WindowsGuestRedZoneProtectionMode : u32 {
    Disabled,
    StaticPatching,
};

namespace Core::WindowsGuestRedZoneProtection {

void SetActiveMode(WindowsGuestRedZoneProtectionMode mode) noexcept;
WindowsGuestRedZoneProtectionMode GetActiveMode() noexcept;
bool IsStaticPatchingEnabled() noexcept;

} // namespace Core::WindowsGuestRedZoneProtection

// ============================================================================
// End Windows static guest red-zone protection
// ============================================================================

namespace Core {

// Windows static guest red-zone protection
struct RedZonePatchResult {
    u64 function_count{};
    u64 instruction_count{};
    u64 red_zone_function_count{};
    u64 memory_instruction_count{};
    u64 short_memory_instruction_count{};
    u64 patched_memory_instruction_count{};
    u64 stack_dependent_memory_instruction_count{};
    u64 control_flow_memory_instruction_count{};
    u64 unrelocatable_memory_instruction_count{};
    u64 indirect_red_zone_function_count{};
    u64 cpu_patch_instruction_count{};
    u64 patched_cpu_patch_instruction_count{};
    u64 unsupported_cpu_patch_instruction_count{};
};

/// Registers a module for patching, providing an area to generate trampoline code.
void RegisterPatchModule(void* module_ptr, u64 module_size, void* trampoline_area_ptr,
                         u64 trampoline_area_size);

/// Applies CPU patches that need to be done before beginning executions.
void PrePatchInstructions(u64 segment_addr, u64 segment_size);

// Windows static guest red-zone protection
/// Keeps Windows exception dispatch outside live guest red zones at faultable memory accesses.
/// When selected_addresses is not empty only functions containing one of them are patched.
RedZonePatchResult PatchRedZoneMemoryInstructions(u64 segment_addr, u64 segment_size,
                                                  std::span<const uintptr_t> function_starts,
                                                  std::span<const uintptr_t> selected_addresses = {});

/// Returns true when the host CPU lacks SSE4a and EXTRQ/INSERTQ must be emulated.
bool NeedsSse4aEmulation();

/// Relocates 4-byte EXTRQ/INSERTQ (too short for a near jump) together with the following
/// instructions into trampolines, so they never reach the illegal-instruction handler. On Windows
/// exception dispatch writes below RSP and clobbers the guest red zone, which corrupts leaf
/// functions that keep temporaries there. Returns the number of instructions patched.
u64 PatchShortSse4aInstructions(u64 segment_addr, u64 segment_size,
                                std::span<const uintptr_t> function_starts);

/// sotc: points every rdtsc at `clock` (a host function returning the TSC replacement), for
/// systems whose cores disagree on the TSC. Returns the number of sites patched; `unpatched`
/// counts the sites that could not be (they keep reading the host TSC). Windows only.
u64 PatchRdtscInstructions(u64 segment_addr, u64 segment_size,
                           std::span<const uintptr_t> function_starts, void* clock,
                           u64* unpatched);

} // namespace Core
