// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: code layout of the mapped eboot, given to the hook loader by sotc_patch_eboot before
// InstallHooks (gpu/sotc/hooks.h). Kept out of hooks.h, which is the public interface.
#pragma once

#include <cstdint>
#include <span>

namespace SotcHooks {

/// A code segment of the eboot: image offsets, [begin, end).
struct CodeRange {
    uint64_t begin{};
    uint64_t end{};
};

/// Sets the scan ranges (code segments) and the function starts (eh_frame, image offsets,
/// any order; sorted here). Without a layout InstallHooks scans the whole module, and a hook
/// with an anchor is off (no function table).
void SetCodeLayout(std::span<const CodeRange> code, std::span<const uint64_t> function_starts);

} // namespace SotcHooks
