// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: per-hook counters (gpu/sotc/hook_stats.cpp). The report is SotcHookStats::Report in
// hooks.h; these are the calls the hook loader and dispatcher make.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SotcHookStats {

/// Sizes one counter slot per hook, indexed by hook id. Called once by InstallHooks.
void Configure(const std::vector<std::string>& names);

/// One pre-phase call. returned: the handler answered HookAction::Return.
void CountPre(uint32_t hook_id, bool returned) noexcept;

/// One post-phase call.
void CountPost(uint32_t hook_id) noexcept;

} // namespace SotcHookStats
