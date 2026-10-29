// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the per-thread accounting of video_core/sotc_stats.h (SOTC_FPS): thread registration
// and the snapshots the SOTC_FPS printer and the SOTCSTALL lines take.

#include "bbport_thread_cpu.h"
#include "video_core/sotc_stats.h"

namespace SotcStats {

static_assert(CpuRoles <= BbThreadCpu::MaxRoles);

void RegisterThreadRole(CpuRole role) {
    tl_role = role;
    if (kFpsLog) {
        BbThreadCpu::Register(role);
    }
}

FrameAccount TakeAccount() {
    FrameAccount a;
    uint64_t cpu[BbThreadCpu::MaxRoles];
    BbThreadCpu::Sample(cpu);
    for (int i = 0; i < CpuRoles; ++i) {
        a.cpu_ns[i] = cpu[i];
    }
    for (int i = 0; i < WaitKinds; ++i) {
        a.wait_ns[i] = draw_wait_ns[i].load(std::memory_order_relaxed);
        a.wait_count[i] = draw_wait_count[i].load(std::memory_order_relaxed);
    }
    a.parse_ns = cp_parse_all_ns.load(std::memory_order_relaxed);
    a.blocked_ns = cp_blocked_ns.load(std::memory_order_relaxed);
    a.spin_ns = cp_spin_ns.load(std::memory_order_relaxed);
    a.rec_busy = rec_busy_ns.load(std::memory_order_relaxed);
    a.rec_upload = rec_upload_wait_ns.load(std::memory_order_relaxed);
    a.rec_submit = rec_submit_ns.load(std::memory_order_relaxed);
    a.fence = async_upload_fence_ns.load(std::memory_order_relaxed);
    a.rb_wait_ns = rb_guest_all_ns.load(std::memory_order_relaxed);
    a.rb_wait_count = rb_guest_all_count.load(std::memory_order_relaxed);
    a.rb_adj = rb_pat_adjacent_all.load(std::memory_order_relaxed);
    a.rb_pf = rb_pf_consumed_all.load(std::memory_order_relaxed);
    a.fault_count = FaultStat::all_count.load(std::memory_order_relaxed);
    a.fault_ns = FaultStat::all_ns.load(std::memory_order_relaxed);
    return a;
}

FrameAccount DiffAccount(const FrameAccount& now, const FrameAccount& before) {
    const auto sub = [](uint64_t a, uint64_t b) { return a > b ? a - b : uint64_t{0}; };
    FrameAccount d;
    for (int i = 0; i < CpuRoles; ++i) {
        d.cpu_ns[i] = sub(now.cpu_ns[i], before.cpu_ns[i]);
    }
    for (int i = 0; i < WaitKinds; ++i) {
        d.wait_ns[i] = sub(now.wait_ns[i], before.wait_ns[i]);
        d.wait_count[i] = sub(now.wait_count[i], before.wait_count[i]);
    }
    d.parse_ns = sub(now.parse_ns, before.parse_ns);
    d.blocked_ns = sub(now.blocked_ns, before.blocked_ns);
    d.spin_ns = sub(now.spin_ns, before.spin_ns);
    d.rec_busy = sub(now.rec_busy, before.rec_busy);
    d.rec_upload = sub(now.rec_upload, before.rec_upload);
    d.rec_submit = sub(now.rec_submit, before.rec_submit);
    d.fence = sub(now.fence, before.fence);
    d.rb_wait_ns = sub(now.rb_wait_ns, before.rb_wait_ns);
    d.rb_wait_count = sub(now.rb_wait_count, before.rb_wait_count);
    d.rb_adj = sub(now.rb_adj, before.rb_adj);
    d.rb_pf = sub(now.rb_pf, before.rb_pf);
    d.fault_count = sub(now.fault_count, before.fault_count);
    d.fault_ns = sub(now.fault_ns, before.fault_ns);
    return d;
}

} // namespace SotcStats
