// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: per-hook counters for the engine hooks (hooks.h). Each hook has three atomics: pre
// calls, post calls and pre calls that returned HookAction::Return. The SOTC_FPS report takes
// them every 5 s and resets them; hooks without activity print nothing.
#include <atomic>
#include <cstdio>
#include <memory>

#include "hook_stats.h"
#include "hooks.h"

namespace SotcHookStats {
namespace {

struct Slot {
    std::string name;
    std::atomic<uint64_t> pre{0};
    std::atomic<uint64_t> post{0};
    std::atomic<uint64_t> returned{0};
};

std::unique_ptr<Slot[]> g_slots;
uint32_t g_count = 0;

} // namespace

void Configure(const std::vector<std::string>& names) {
    g_slots = std::make_unique<Slot[]>(names.size());
    g_count = static_cast<uint32_t>(names.size());
    for (uint32_t i = 0; i < g_count; ++i) {
        g_slots[i].name = names[i];
    }
}

void CountPre(uint32_t hook_id, bool returned) noexcept {
    if (hook_id >= g_count) {
        return;
    }
    Slot& slot = g_slots[hook_id];
    slot.pre.fetch_add(1, std::memory_order_relaxed);
    if (returned) {
        slot.returned.fetch_add(1, std::memory_order_relaxed);
    }
}

void CountPost(uint32_t hook_id) noexcept {
    if (hook_id >= g_count) {
        return;
    }
    g_slots[hook_id].post.fetch_add(1, std::memory_order_relaxed);
}

} // namespace SotcHookStats

namespace SotcHookStats {

void Report(std::string& out, double seconds) {
    if (seconds <= 0.0) {
        seconds = 1.0;
    }
    for (uint32_t i = 0; i < g_count; ++i) {
        Slot& slot = g_slots[i];
        const uint64_t pre = slot.pre.exchange(0, std::memory_order_relaxed);
        const uint64_t post = slot.post.exchange(0, std::memory_order_relaxed);
        const uint64_t returned = slot.returned.exchange(0, std::memory_order_relaxed);
        if (pre == 0 && post == 0 && returned == 0) {
            continue;
        }
        char line[256];
        std::snprintf(line, sizeof line,
                      "SOTC HOOKS: %s %.1f/s pre, %.1f/s post, %.1f/s return\n",
                      slot.name.c_str(), pre / seconds, post / seconds, returned / seconds);
        out += line;
    }
}

} // namespace SotcHookStats
