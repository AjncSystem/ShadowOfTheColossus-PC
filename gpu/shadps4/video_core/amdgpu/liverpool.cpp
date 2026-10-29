// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <utility>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <new>
#ifdef _WIN32
#include <windows.h>
#endif
#include <boost/preprocessor/stringize.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/videoout/driver.h"
#include "core/memory.h"
#include "core/platform.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderdoc.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/sotc_lod_stats.h"
#include "video_core/sotc_stats.h"
#include "bbport_threads.h"
#include "video_core/amdgpu/draw_prep.h"
#include "video_core/amdgpu/op_footprint.h" // sotc: SOTC_CULL_EXPRESS
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include <xmmintrin.h>

// sotc: writes guest memory through the backing view (runtime_memory.c).
extern "C" int runtime_memory_write_backing(unsigned long long address, const void* data,
                                            unsigned long long size);

// Counted for the SOTC_PROBE diagnostic log (emulator.cpp).
std::atomic<u64> g_sotc_zpass_dumps{0};

namespace AmdGpu {

// sotc: SOTC_RB_EXACT: the Liverpool the wait observer charges (set in the constructor).
static std::atomic<Liverpool*> g_rbx_self{nullptr};

// sotc: SOTC_GPU_THREAD_HIGH=1 runs the command processor and draw threads above normal priority,
// so the pipeline workers and game job threads cannot delay them during streaming. Off by
// default: both threads poll, and above normal they could starve normal-priority game threads.
static void SotcRaiseGpuThread() {
    static const bool high = [] {
        const char* v = std::getenv("SOTC_GPU_THREAD_HIGH");
        return v && v[0] == '1';
    }();
    if (high) {
        Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
    }
}

static const char* dcb_task_name{"DCB_TASK"};
static const char* ccb_task_name{"CCB_TASK"};

#define MAX_NAMES 56
static_assert(Liverpool::NumComputeRings <= MAX_NAMES);

#define NAME_NUM(z, n, name) BOOST_PP_STRINGIZE(name) BOOST_PP_STRINGIZE(n),
#define NAME_ARRAY(name, num) {BOOST_PP_REPEAT(num, NAME_NUM, name)}

static const char* acb_task_name[] = NAME_ARRAY(ACB_TASK, MAX_NAMES);

#define YIELD(name)                                                                                \
    FIBER_EXIT;                                                                                    \
    co_yield {};                                                                                   \
    FIBER_ENTER(name);

#define YIELD_CE() YIELD(ccb_task_name)
#define YIELD_GFX() YIELD(dcb_task_name)
#define YIELD_ASC(id) YIELD(acb_task_name[id])

#define RESUME(task, name)                                                                         \
    FIBER_EXIT;                                                                                    \
    task.handle.resume();                                                                          \
    FIBER_ENTER(name);

#define RESUME_CE(task) RESUME(task, ccb_task_name)
#define RESUME_GFX(task) RESUME(task, dcb_task_name)
#define RESUME_ASC(task, id) RESUME(task, acb_task_name[id])

std::array<u8, 48_KB> Liverpool::ConstantEngine::constants_heap;

static std::span<const u32> NextPacket(std::span<const u32> span, size_t offset) {
    if (offset > span.size()) {
        LOG_ERROR(
            Lib_GnmDriver,
            ": packet length exceeds remaining submission size. Packet dword count={}, remaining "
            "submission dwords={}",
            offset, span.size());
        // Return empty subspan so check for next packet bails out
        return {};
    }

    return span.subspan(offset);
}

Liverpool::Liverpool() : guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    num_counter_pairs = Libraries::Kernel::sceKernelIsNeoMode() ? 16 : 8;
    // sotc: SOTC_CP_THREADS=0 binds and records the draws on the command processor thread.
    const char* threads = std::getenv("SOTC_CP_THREADS");
    back_threaded = !threads || threads[0] != '0';
    const char* strict = std::getenv("SOTC_CP_STRICT");
    back_strict = strict && strict[0] == '1';
    // sotc: SOTC_WAIT_NOSPIN=1: BackLoop spins ~20 us, then blocks on back_wake (off by default).
    const char* nospin = std::getenv("SOTC_WAIT_NOSPIN");
    back_nospin = nospin && nospin[0] == '1';
    // sotc: SOTC_WD_FUSE=1 (off by default; needs the draw thread): small WRITE_DATA packets ride
    // along with the next indirect draw instead of being queued as functions. SOTC_INLINE_FUNC=1
    // (also set by SOTC_WD_FUSE) keeps small closures of the op ring out of the heap.
    {
        const char* fuse = std::getenv("SOTC_WD_FUSE");
        const char* inline_func = std::getenv("SOTC_INLINE_FUNC");
        wd_fuse = back_threaded && fuse && fuse[0] == '1';
        Common::g_inline_function_enabled =
            back_threaded && ((fuse && fuse[0] == '1') || (inline_func && inline_func[0] == '1'));
    }
    // sotc: SOTC_CP_IDLE_US / SOTC_CP_IDLE_TIMEOUT_US, see Process and CpIdleSleep. Read before
    // any thread that calls NotifyCpWait starts.
    if (const char* v = std::getenv("SOTC_CP_IDLE_US")) {
        cp_idle_ns = std::strtoull(v, nullptr, 10) * 1000ULL;
    }
    if (const char* v = std::getenv("SOTC_CP_IDLE_TIMEOUT_US")) {
        cp_idle_timeout_ns = std::max<u64>(std::strtoull(v, nullptr, 10), 10) * 1000ULL;
    }
#ifdef _WIN32
    if (cp_idle_ns) {
        cp_wake_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!cp_wake_event) {
            cp_idle_ns = 0;
        }
    }
#endif
    if (cp_idle_ns) {
        LOG_INFO(Render, "SOTC_CP_IDLE_US: the command processor sleeps after {} us without "
                         "progress, for at most {} us",
                 cp_idle_ns / 1000, cp_idle_timeout_ns / 1000);
    }
    // sotc: SOTC_EOP_TICK=1 (default off). The labels are polled by BackLoop, so it needs the
    // draw thread; without it the command processor would have to poll while idle.
    const char* eop_tick_env = std::getenv("SOTC_EOP_TICK");
    eop_tick = back_threaded && eop_tick_env && eop_tick_env[0] == '1';
    // sotc: measured 2026-10-27: the game writes ~130 EOP labels per frame and waits for each, so
    // honest completion costs one GPU round trip per label (~300 ms frames at the default 2 ms
    // limit, ~133 ms at SOTC_EOP_TICK_US=0). Diagnostic only; kept off by default.
    // sotc: SOTC_RB_EXACT=1: the label side (see QueueDeferredLabel). Needs the draw thread for
    // the same reason as SOTC_EOP_TICK (BackLoop polls the deferred labels).
    rbx = back_threaded && VideoCore::BufferCache::RbExactEnabled();
    eop_poll = eop_tick || rbx;
    VideoCore::BufferCache::SetLabelSideActive(rbx); // sotc: SOTC_RB_EXACT (cache side agrees)
    if (rbx) {
        g_rbx_self.store(this, std::memory_order_release);
        VideoCore::BufferCache::SetWaitObserver(&Liverpool::RbxWaitObserver);
    }
    if (eop_tick) {
        LOG_WARNING(Render, "SOTC_EOP_TICK=1 is diagnostic only: ~130 GPU round trips per frame "
                            "(3-7 FPS). Use it only to test streaming corruption.");
    }
    // sotc: SOTC_CP_AHEAD=2 (frame tickets) and SOTC_ASC_RPTR, both off by default.
    frame_tickets = CpAheadMode() == 2;
    asc_rptr_mode = AscRptrMode();
    if (frame_tickets || asc_rptr_mode) {
        LOG_INFO(Render, "SOTC_CP_AHEAD={} SOTC_ASC_RPTR={}", CpAheadMode(), asc_rptr_mode);
    }
    // sotc: SOTC_CULL_EXPRESS=1 (default off; needs the draw thread and SOTC_PARALLEL_RECORD=1 for
    // the footprints). Read before the draw thread starts (XpPoll).
    if (back_threaded && SotcStats::kCullExpress) {
        xp_on = true;
        const auto env_u64 = [](const char* name, u64 fallback) {
            const char* v = std::getenv(name);
            return v ? std::strtoull(v, nullptr, 0) : fallback;
        };
        xp_signals = env_u64("SOTC_CULL_EXPRESS_SIGNALS", 1) != 0;
        xp_marks = env_u64("SOTC_CULL_EXPRESS_MARKS", 1) != 0;
        xp_pattern = env_u64("SOTC_CULL_EXPRESS_PATTERN", 1) != 0;
        xp_forced_addr = env_u64("SOTC_CULL_EXPRESS_ADDR", 0);
        xp_max_seg = static_cast<u32>(std::clamp<u64>(env_u64("SOTC_CULL_EXPRESS_SEG", 256), 1, 480));
        xp_max_skip = static_cast<u32>(
            std::clamp<u64>(env_u64("SOTC_CULL_EXPRESS_SKIP", 480), 1, BackRingSize - 2));
        xp_budget_ns = env_u64("SOTC_CULL_EXPRESS_US", 500) * 1000ULL;
        xp_undo.reserve(64 * (RegChunkDwords + 1));
        LOG_INFO(Render,
                 "SOTC_CULL_EXPRESS=1: segment <= {} ops, skip <= {} ops, decision <= {} us, "
                 "signals {}, marks {}",
                 xp_max_seg, xp_max_skip, xp_budget_ns / 1000, xp_signals, xp_marks);
    }
    if (back_threaded) {
        back_ring.resize(BackRingSize);
        dirty_list.reserve(NumRegChunks);
        back_thread = std::jthread{std::bind_front(&Liverpool::BackLoop, this)};
        back_id = back_thread.get_id();
        // sotc: SOTC_FLIP_SERVICE=1 (off by default), see Vulkan::Presenter::wait_service.
        static Liverpool* service_liverpool = nullptr;
        const char* flip_service = std::getenv("SOTC_FLIP_SERVICE");
        if (flip_service && flip_service[0] == '1') {
            service_liverpool = this;
            Vulkan::Presenter::wait_service = [] { service_liverpool->ServiceCommandsOnBackThread(); };
        }
    }
    // sotc: SOTC_PARALLEL_RECORD=1 (default off): GpuPrep threads (liverpool.h, draw_prep.h).
    // Started before the command processor, which reads prep_workers in BackAlloc.
    const char* parallel_record = std::getenv("SOTC_PARALLEL_RECORD");
    if (back_threaded && parallel_record && parallel_record[0] == '1') {
        const unsigned available = BbThreads::Available();
        u32 workers = std::clamp<u32>(available / 4, 1, MaxPrepWorkers);
        if (const char* w = std::getenv("SOTC_PARALLEL_RECORD_WORKERS")) {
            workers = std::clamp<u32>(static_cast<u32>(std::strtoul(w, nullptr, 0)), 1,
                                      MaxPrepWorkers);
        }
        if (available <= 6) {
            LOG_WARNING(Render, "SOTC_PARALLEL_RECORD: off, {} hardware threads (needs 7 or more)",
                        available);
        } else {
            prep_workers = workers;
            prep_ring = std::make_unique<Vulkan::PreparedDraw[]>(BackRingSize);
            for (u32 k = 0; k < prep_workers; ++k) {
                prep_regs.push_back(std::make_unique<Regs>());
            }
            for (u32 k = 0; k < prep_workers; ++k) {
                prep_threads.emplace_back(std::bind_front(&Liverpool::PrepLoop, this), k);
            }
            LOG_INFO(Render, "SOTC_PARALLEL_RECORD: {} prep thread(s)", prep_workers);
        }
    }
    process_thread = std::jthread{std::bind_front(&Liverpool::Process, this)};
}

Liverpool::~Liverpool() {
    g_rbx_self.store(nullptr, std::memory_order_release); // sotc: SOTC_RB_EXACT
    Vulkan::Presenter::wait_service = nullptr; // sotc: SOTC_FLIP_SERVICE
    process_thread.request_stop();
    process_thread.join();
    if (back_thread.joinable()) {
        back_thread.request_stop();
        if (back_nospin) {
            // sotc: BackLoop blocks on back_wake, which only changes when something bumps it
            WakeBackAtomic();
        }
        back_thread.join();
    }
    for (auto& thread : prep_threads) { // sotc: SOTC_PARALLEL_RECORD
        thread.request_stop();
    }
    if (!prep_threads.empty()) {
        prep_wake.fetch_add(1, std::memory_order_seq_cst);
        prep_wake.notify_all();
    }
    prep_threads.clear(); // joins
#ifdef _WIN32
    if (cp_wake_event) { // sotc: SOTC_CP_IDLE_US
        ::CloseHandle(cp_wake_event);
    }
#endif
}

static inline void CpuRelax() {
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#endif
}

void Liverpool::BackLoop(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuDraw");
    VideoCore::BufferCache::SetDrawThread(std::this_thread::get_id()); // sotc: SOTC_RB_EXACT
    SotcStats::RegisterThreadRole(SotcStats::RoleDraw); // sotc: SOTC_FPS thread accounting
    SotcRaiseGpuThread();
    BbThreads::PlaceThread(BbThreads::Role::Draw);
    Shader::g_info_slot = 1;
#ifdef __linux__
    back_tid = gettid();
#endif
    u64 tail = back_tail.load(std::memory_order_relaxed);
    while (!stoken.stop_requested()) {
        if (num_commands.load(std::memory_order_acquire)) {
            ProcessCommands();
        }
        // sotc: SOTC_EOP_TICK: labels the GPU has finished are written here, also while no new
        // work arrives (EopService returns at once when none is outstanding).
        if (eop_poll && eop_outstanding.load(std::memory_order_acquire) != 0) {
            EopService();
        }
        if (back_head.load(std::memory_order_acquire) == tail) {
            if (eop_poll && eop_outstanding.load(std::memory_order_acquire) != 0) {
                // sotc: SOTC_EOP_TICK: a label is waiting for the GPU, so poll instead of
                // blocking on back_cv or back_wake, which nothing signals when the GPU finishes.
                // sotc: SOTC_RB_EXACT: a spin-waited label should not wait 200 us for the poll.
                std::this_thread::sleep_for(std::chrono::microseconds{rbx ? 30 : 200});
                continue;
            }
            const u64 idle_start = SotcStats::NowNs();
            struct IdleTime {
                u64 start;
                ~IdleTime() {
                    SotcStats::draw_idle_ns.fetch_add(SotcStats::NowNs() - start,
                                                      std::memory_order_relaxed);
                }
            } idle_time{idle_start};
            if (back_nospin) {
                // sotc: spin ~20 us, then block until back_wake changes. The snapshot is taken
                // before the checks, and every producer changes the checked state before bumping.
                const auto spin_end = std::chrono::steady_clock::now() + std::chrono::microseconds{20};
                bool found = false;
                for (u32 spin = 1;; ++spin) {
                    if (back_head.load(std::memory_order_acquire) != tail ||
                        num_commands.load(std::memory_order_acquire)) {
                        found = true;
                        break;
                    }
                    if ((spin & 63) == 0 && std::chrono::steady_clock::now() >= spin_end) {
                        break;
                    }
                    CpuRelax();
                }
                if (!found) {
                    back_sleeping.store(true, std::memory_order_seq_cst);
                    const u32 seen = back_wake.load(std::memory_order_seq_cst);
                    if (back_head.load(std::memory_order_seq_cst) == tail &&
                        !num_commands.load(std::memory_order_seq_cst) &&
                        !stoken.stop_requested()) {
                        back_wake.wait(seen, std::memory_order_seq_cst);
                    }
                    back_sleeping.store(false, std::memory_order_relaxed);
                }
                continue;
            }
            bool ready = false;
            for (u32 spin = 0; spin < 4096; ++spin) {
                if (back_head.load(std::memory_order_acquire) != tail ||
                    num_commands.load(std::memory_order_acquire)) {
                    ready = true;
                    break;
                }
                CpuRelax();
            }
            if (!ready) {
                std::unique_lock lk{back_mutex};
                back_sleeping.store(true, std::memory_order_seq_cst);
                back_cv.wait_for(lk, std::chrono::milliseconds(1), [&] {
                    return back_head.load(std::memory_order_seq_cst) != tail ||
                           num_commands.load(std::memory_order_seq_cst) ||
                           stoken.stop_requested();
                });
                back_sleeping.store(false, std::memory_order_relaxed);
            }
            continue;
        }
        // sotc: SOTC_CULL_EXPRESS: a published express segment may run before the older ops.
        if (xp_on &&
            xp_head.load(std::memory_order_acquire) != xp_tail.load(std::memory_order_relaxed)) {
            XpPoll(tail);
        }
        BackOp& op = back_ring[tail % BackRingSize];
        back_exec_seq = tail;
        const u64 xp_t0 = xp_on ? SotcStats::NowNs() : 0;
        if (prep_workers) { // sotc: SOTC_PARALLEL_RECORD: the next prepared draw's first lines
            const char* next = reinterpret_cast<const char*>(&prep_ring[(tail + 1) % BackRingSize]);
            for (u32 line = 0; line < 4; ++line) {
                _mm_prefetch(next + line * 64, _MM_HINT_T0);
            }
        }
        const bool func_op = op.kind == BackOp::Kind::Func || op.wd.num_segs != 0;
        if (op.xp_done) {
            XpReplayState(op); // sotc: SOTC_CULL_EXPRESS: ran ahead, only its state is replayed
        } else if (SotcStats::kFpsLog && op.kind == BackOp::Kind::Func && op.func) {
            const u64 func_start = SotcStats::NowNs();
            ExecuteBackOp(op);
            SotcStats::draw_func_ns[op.func_kind & 3].fetch_add(SotcStats::NowNs() - func_start,
                                                                std::memory_order_relaxed);
        } else {
            ExecuteBackOp(op);
        }
        op.func.Reset();
        ++tail;
        back_tail.store(tail, std::memory_order_seq_cst);
        if (xp_on) {
            if (!op.xp_done) { // time per FIFO op (outliers clipped), for the saved-time estimate
                const double dt =
                    static_cast<double>(std::min<u64>(SotcStats::NowNs() - xp_t0, 1'000'000));
                xp_op_ns += (dt - xp_op_ns) / 64.0;
            }
            if (xp_watch_end && tail >= xp_watch_end) { // a FIFO segment's label has run
                SotcStats::xp_lat_fifo_ns.fetch_add(SotcStats::NowNs() - xp_watch_ns,
                                                    std::memory_order_relaxed);
                SotcStats::xp_lat_fifo_n.fetch_add(1, std::memory_order_relaxed);
                xp_watch_end = 0;
            }
        }
        // sotc: SOTC_CP_IDLE_US: the queued functions write the labels, fences and data the
        // command processor's WaitRegMem loops test (EOP/EOS, WRITE_DATA, RELEASE_MEM, DMA), and
        // the label wait kind depends on back_tail; the ring running empty also wakes it.
        if (func_op || back_head.load(std::memory_order_relaxed) == tail) {
            NotifyCpWait();
        }
        if (drain_waiting.load(std::memory_order_seq_cst)) {
            std::scoped_lock lk{back_mutex};
            drain_cv.notify_all();
        }
    }
}

void Liverpool::DrainBack() {
    if (!back_threaded || std::this_thread::get_id() == back_id) {
        return;
    }
    // sotc: SOTC_WD_FUSE: what the draw thread must have run includes the held-back writes.
    if (std::this_thread::get_id() == gpu_id && wd_pending.num_segs) {
        FlushWd();
    }
    WaitBackTail(back_head.load(std::memory_order_acquire));
}

void Liverpool::WaitBackTail(u64 head) {
    SotcStats::cp_waits.fetch_add(1, std::memory_order_relaxed);
    // sotc: SOTC_FPS: command processor time blocked on the draw thread.
    const SotcStats::RoleTimer blocked{SotcStats::RoleCp, SotcStats::cp_blocked_ns};
    for (u32 spin = 0; back_tail.load(std::memory_order_acquire) < head; ++spin) {
        if (spin < 4096) {
            CpuRelax();
            continue;
        }
        std::unique_lock lk{back_mutex};
        drain_waiting.store(true, std::memory_order_seq_cst);
        drain_cv.wait_for(lk, std::chrono::milliseconds(1), [&] {
            return back_tail.load(std::memory_order_seq_cst) >= head;
        });
        drain_waiting.store(false, std::memory_order_relaxed);
    }
}

Liverpool::BackOp& Liverpool::BackAlloc() {
    const u64 head = back_head.load(std::memory_order_relaxed);
    // sotc: SOTC_PARALLEL_RECORD: a slot is reused only once the prep threads replayed it too
    // (MinRingTail is back_tail alone without them).
    if (prep_workers && SotcStats::kFpsLog) {
        const u32 lag = static_cast<u32>(head - MinRingTail(std::memory_order_acquire));
        u32 seen = SotcStats::pr_lag_max.load(std::memory_order_relaxed);
        while (lag > seen &&
               !SotcStats::pr_lag_max.compare_exchange_weak(seen, lag, std::memory_order_relaxed)) {
        }
    }
    if (head - MinRingTail(std::memory_order_acquire) >= BackRingSize) {
        // sotc: SOTC_FPS: the ring is full, the command processor is blocked on the draw thread.
        const SotcStats::RoleTimer blocked{SotcStats::RoleCp, SotcStats::cp_blocked_ns};
        // sotc: SOTC_PARALLEL_RECORD: the wait counts as the prep threads' when the draw thread
        // alone would have left room.
        const bool prep_throttle = prep_workers && SotcStats::kFpsLog &&
                                   head - back_tail.load(std::memory_order_acquire) < BackRingSize;
        const u64 throttle_start = prep_throttle ? SotcStats::NowNs() : 0;
        for (u32 spin = 0; head - MinRingTail(std::memory_order_acquire) >= BackRingSize;
             ++spin) {
            if (spin < 4096) {
                CpuRelax();
                continue;
            }
            std::unique_lock lk{back_mutex};
            drain_waiting.store(true, std::memory_order_seq_cst);
            drain_cv.wait_for(lk, std::chrono::milliseconds(1), [&] {
                return head - MinRingTail(std::memory_order_seq_cst) < BackRingSize;
            });
            drain_waiting.store(false, std::memory_order_relaxed);
        }
        if (prep_throttle) {
            SotcStats::pr_throttle_ns.fetch_add(SotcStats::NowNs() - throttle_start,
                                                std::memory_order_relaxed);
        }
    }
    BackOp& op = back_ring[head % BackRingSize];
    op.pipeline = nullptr;
    op.num_stages = 0;
    op.wd.num_segs = 0;
    op.func_kind = 3;
    op.xp_cls = XpUnknown; // sotc: SOTC_CULL_EXPRESS
    op.xp_done = false;
    op.xp_rsize = op.xp_wsize = 0;
    op.qid = curr_qid < 0 ? u8{255} : static_cast<u8>(curr_qid);
    // The registers as of this point of the command stream: the chunks changed since the last op.
    op.reg_chunks.clear();
    for (const u16 chunk : dirty_list) {
        const u32* src = &regs.reg_array[u32{chunk} * RegChunkDwords];
        op.reg_chunks.push_back(chunk);
        op.reg_chunks.insert(op.reg_chunks.end(), src, src + RegChunkDwords);
        dirty_chunks[chunk / 64] &= ~(u64{1} << (chunk % 64));
    }
    dirty_list.clear();
    op.cb_extent = last_cb_extent;
    op.db_extent = last_db_extent;
    return op;
}

void Liverpool::BackPublish(bool writes_memory) {
    const u64 head = back_head.load(std::memory_order_relaxed) + 1;
    back_head.store(head, std::memory_order_seq_cst);
    if (writes_memory) {
        back_write_seq = head;
    }
    // sotc: SOTC_PARALLEL_RECORD: a prep thread blocks on prep_wake after it saw back_head
    // unchanged (seq_cst on both sides: either it sees the new head or this sees it asleep).
    if (prep_workers && prep_sleepers.load(std::memory_order_seq_cst) != 0) {
        prep_wake.fetch_add(1, std::memory_order_seq_cst);
        prep_wake.notify_all();
    }
    if (back_sleeping.load(std::memory_order_seq_cst)) {
        if (back_nospin) {
            WakeBackAtomic();
        } else {
            std::scoped_lock lk{back_mutex};
            back_cv.notify_one();
        }
    }
}

void Liverpool::QueueBackFunc(Common::InlineFunction&& func, bool writes_memory) {
    // sotc: SOTC_WD_FUSE: whatever the command processor queues goes after the writes held back
    // before it (RunOnBackThread during a pipeline lookup, ordered commands, ...).
    if (wd_pending.num_segs && std::this_thread::get_id() == gpu_id) {
        FlushWd();
    }
    BackOp& op = BackAlloc();
    op.kind = BackOp::Kind::Func;
    op.func = std::move(func);
    op.func_kind = func_kind_hint;
    if (xp_hint.cls != XpUnknown) { // sotc: SOTC_CULL_EXPRESS (the parser classified this op)
        op.xp_cls = xp_hint.cls;
        op.xp_raddr = xp_hint.raddr;
        op.xp_rsize = xp_hint.rsize;
        op.xp_waddr = xp_hint.waddr;
        op.xp_wsize = xp_hint.wsize;
    }
    xp_hint = XpHint{};
    if (SotcStats::kFpsLog) {
        SotcStats::cp_func_ops[func_kind_hint & 3].fetch_add(1, std::memory_order_relaxed);
    }
    BackPublish(writes_memory);
    if (SotcStats::kFpsLog && func_kind_hint == 0) {
        wd_last_seq = back_head.load(std::memory_order_relaxed);
    }
}

void Liverpool::WaitBackWritesSlow() {
    // sotc: SOTC_FPS: WaitBackWrites calls that had to wait for the draw thread, and for how long.
    if (!SotcStats::kFpsLog) {
        WaitBackTail(back_write_seq);
        return;
    }
    const u64 start = SotcStats::NowNs();
    WaitBackTail(back_write_seq);
    SotcStats::cp_bw_waits.fetch_add(1, std::memory_order_relaxed);
    SotcStats::cp_bw_wait_ns.fetch_add(SotcStats::NowNs() - start, std::memory_order_relaxed);
}

bool Liverpool::FuseWriteData(VAddr address, const u32* data, u32 data_size) {
    if (!wd_fuse || data_size == 0 || data_size > WdMaxBytes || (data_size & 3)) {
        return false;
    }
    BackWd& wd = wd_pending;
    if (wd.num_segs >= WdMaxSegs || wd.used + data_size > WdMaxBytes) {
        // Full: the held-back writes go first, in order, then this one starts a new batch.
        FlushWd();
    }
    if (wd.num_segs == 0) {
        wd.used = 0;
    }
    wd.segs[wd.num_segs++] = {address, wd.used, static_cast<u8>(data_size)};
    std::memcpy(wd.data.data() + wd.used, data, data_size);
    wd.used = static_cast<u8>(wd.used + data_size);
    return true;
}

void Liverpool::FlushWd() {
    // One op carrying the writes and no closure: the draw thread applies them in order, as the
    // separate Func ops did (nothing was queued in between, see FuseTransparent).
    if (wd_pending.num_segs == 0) {
        return;
    }
    BackOp& op = BackAlloc();
    op.kind = BackOp::Kind::Func;
    op.func_kind = 0;
    op.wd.num_segs = wd_pending.num_segs;
    op.wd.used = wd_pending.used;
    op.wd.segs = wd_pending.segs;
    std::memcpy(op.wd.data.data(), wd_pending.data.data(), wd_pending.used);
    if (SotcStats::kFpsLog) {
        SotcStats::cp_func_ops[0].fetch_add(wd_pending.num_segs, std::memory_order_relaxed);
    }
    wd_pending.num_segs = 0;
    wd_pending.used = 0;
    BackPublish(true);
    if (SotcStats::kFpsLog) {
        wd_last_seq = back_head.load(std::memory_order_relaxed);
    }
}

void Liverpool::NotePairs() {
    if (SotcStats::kFpsLog &&
        (wd_fuse ? wd_pending.num_segs != 0
                 : wd_last_seq != 0 && wd_last_seq == back_head.load(std::memory_order_relaxed))) {
        SotcStats::cp_wd_pairs.fetch_add(1, std::memory_order_relaxed);
    }
}

void Liverpool::PrepareWdForIndirect(VAddr begin, u64 span, bool allowed) {
    if (wd_pending.num_segs == 0) {
        return;
    }
    // All or nothing: every held-back write must lie inside the arguments this packet reads
    // (count buffers are read by the command processor's own paths: not allowed).
    bool inside = allowed;
    for (u32 i = 0; inside && i < wd_pending.num_segs; ++i) {
        const BackWd::Seg& seg = wd_pending.segs[i];
        inside = seg.address >= begin && seg.address + seg.length <= begin + span;
    }
    if (!inside) {
        FlushWd();
    }
}

void Liverpool::AttachWd(BackOp& op) {
    if (wd_pending.num_segs == 0) {
        return; // flushed meanwhile (QueueBackFunc), or none held
    }
    op.wd.num_segs = wd_pending.num_segs;
    op.wd.used = wd_pending.used;
    op.wd.segs = wd_pending.segs;
    std::memcpy(op.wd.data.data(), wd_pending.data.data(), wd_pending.used);
    if (SotcStats::kFpsLog) {
        SotcStats::cp_wd_fused.fetch_add(wd_pending.num_segs, std::memory_order_relaxed);
    }
    wd_pending.num_segs = 0;
    wd_pending.used = 0;
}

void Liverpool::ApplyBackWd(const BackWd& wd) {
    // Draw thread. Exactly what the WRITE_DATA Func did, once per packet.
    const u64 start = SotcStats::kFpsLog ? SotcStats::NowNs() : 0;
    // sotc: SOTC_RB_EXACT rule 7: a CPU-visible write, written now. Owed data announced by it is
    // settled first.
    u64 rbx_snap = 0;
    if (rbx && rasterizer && wd.num_segs) {
        rbx_snap = rasterizer->GetBufferCache().HotWriteSeq();
        rasterizer->GetBufferCache().SettleOwedUpTo(rbx_snap);
    }
    for (u32 i = 0; i < wd.num_segs; ++i) {
        const BackWd::Seg& seg = wd.segs[i];
        if (rasterizer) {
            rasterizer->OnFence();
        }
        std::memcpy(reinterpret_cast<void*>(seg.address), wd.data.data() + seg.offset, seg.length);
        Core::MemoryManager::NoteHostWrite(seg.address, seg.length, 4);
    }
    if (rbx && rasterizer && wd.num_segs) {
        RbxNoteWritten(rbx_snap);
    }
    if (SotcStats::kFpsLog) {
        SotcStats::draw_func_ns[0].fetch_add(SotcStats::NowNs() - start,
                                             std::memory_order_relaxed);
    }
}

void Liverpool::CaptureBackOp(BackOp& op, const Vulkan::Pipeline* pipeline) {
    op.pipeline = pipeline;
    op.num_stages = 0;
    if (!pipeline) {
        return;
    }
    for (const Shader::Info* info : pipeline->GetStages()) {
        if (!info) {
            continue;
        }
        BackStage& stage = op.stages[op.num_stages++];
        const Shader::InfoRuntime& rt = info->runtime[0];
        stage.info = const_cast<Shader::Info*>(info);
        if (rt.user_data.data() == nullptr) {
            stage.num_user_data = ~0u;
        } else {
            stage.num_user_data = static_cast<u32>(std::min<size_t>(rt.user_data.size(), 16));
            std::memcpy(stage.user_data.data(), rt.user_data.data(),
                        stage.num_user_data * sizeof(u32));
        }
        stage.pgm_base = rt.pgm_base;
        stage.flat.assign(rt.flattened_ud_buf.begin(), rt.flattened_ud_buf.end());
    }
}

/// A draw or dispatch with this pipeline may write guest memory (storage buffers or images).
static bool PipelineWrites(const Vulkan::Pipeline* pipeline) {
    if (!pipeline) {
        return false;
    }
    if (pipeline->IsCompute()) {
        return true;
    }
    for (const Shader::Info* info : pipeline->GetStages()) {
        if (!info) {
            continue;
        }
        if (info->uses_dma || info->has_storage_images) {
            return true;
        }
        for (const auto& buffer : info->buffers) {
            if (buffer.is_written) {
                return true;
            }
        }
        for (const auto& image : info->images) {
            if (image.is_written) {
                return true;
            }
        }
    }
    return false;
}

/// sotc: SOTC_WD_FUSE. True for the packets that may come between a held-back WRITE_DATA and the
/// indirect draw taking it: they only update the command processor's register state (which is
/// replayed on the draw thread with the next op), read no guest memory, and queue no op.
/// Everything else (any draw, dispatch, fence, DMA, wait, indirect buffer, COND_EXEC, a flip NOP,
/// ...) is not: the held-back writes are queued before it.
static bool FuseTransparent(const PM4Header* header, PM4ItOpcode opcode) {
    switch (opcode) {
    case PM4ItOpcode::Nop: {
        const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
        return nop->header.count.Value() == 0 ||
               nop->data_block[0] != PM4CmdNop::PayloadType::PatchedFlip;
    }
    case PM4ItOpcode::ContextControl:
    case PM4ItOpcode::ClearState:
    case PM4ItOpcode::SetConfigReg:
    case PM4ItOpcode::SetContextReg:
    case PM4ItOpcode::SetShReg:
    case PM4ItOpcode::SetUconfigReg:
    case PM4ItOpcode::SetPredication:
    case PM4ItOpcode::IndexType:
    case PM4ItOpcode::NumInstances:
    case PM4ItOpcode::IndexBase:
    case PM4ItOpcode::IndexBufferSize:
    case PM4ItOpcode::SetBase:
    case PM4ItOpcode::WriteData:
    // The packets that take them (Liverpool::PrepareWdForIndirect decides).
    case PM4ItOpcode::DrawIndirect:
    case PM4ItOpcode::DrawIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirect:
    case PM4ItOpcode::DrawIndexIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirectCountMulti:
    case PM4ItOpcode::DispatchIndirect:
        return true;
    default:
        return false;
    }
}

void Liverpool::ExecuteBackOp(BackOp& op) {
    // sotc: SOTC_WD_FUSE: the WRITE_DATA packets that preceded this op in the command stream.
    if (op.wd.num_segs) {
        ApplyBackWd(op.wd);
    }
    for (size_t i = 0; i < op.reg_chunks.size(); i += RegChunkDwords + 1) {
        std::memcpy(&back_regs.reg_array[op.reg_chunks[i] * RegChunkDwords], &op.reg_chunks[i + 1],
                    RegChunkDwords * sizeof(u32));
    }
    back_cb_extent = op.cb_extent;
    back_db_extent = op.db_extent;
    for (u32 i = 0; i < op.num_stages; ++i) {
        BackStage& stage = op.stages[i];
        Shader::InfoRuntime& rt = stage.info->runtime[1];
        rt.user_data = stage.num_user_data == ~0u
                           ? std::span<const u32>{}
                           : std::span<const u32>{stage.user_data.data(), stage.num_user_data};
        rt.flattened_ud_buf.assign(stage.flat.begin(), stage.flat.end());
        rt.pgm_base = stage.pgm_base;
    }
    switch (op.kind) {
    case BackOp::Kind::Func:
        if (op.func) { // sotc: empty for a flushed batch of WRITE_DATA (FlushWd)
            op.func();
        }
        break;
    case BackOp::Kind::Draw:
        rasterizer->Draw(op.is_indexed, op.index_offset, op.pipeline, true,
                         prep_workers ? TakePrepared(back_exec_seq) : nullptr);
        break;
    case BackOp::Kind::DrawIndirect:
        rasterizer->DrawIndirect(op.is_indexed, op.address, op.offset, op.stride, op.max_count,
                                 op.count_address, op.vertex_sgpr_offset, op.instance_sgpr_offset,
                                 op.pipeline, true,
                                 prep_workers ? TakePrepared(back_exec_seq) : nullptr);
        break;
    case BackOp::Kind::Dispatch:
        back_cs = op.cs;
        rasterizer->DispatchDirect(op.pipeline, true);
        break;
    case BackOp::Kind::DispatchIndirect:
        back_cs = op.cs;
        rasterizer->DispatchIndirect(op.address, op.offset, op.size, op.pipeline, true);
        break;
    }
}

void Liverpool::PrepLoop(std::stop_token stoken, u32 index) {
    // sotc: SOTC_PARALLEL_RECORD (liverpool.h). Every op's registers are replayed, in order, so
    // prep_regs matches back_regs at each op; only this thread's share of the draws is computed,
    // and none the draw thread has already passed. The op stays valid until prep_tail moves past
    // it (BackAlloc), the slot of prep_ring until the draw thread ran that op.
    Common::SetCurrentThreadName(index == 0 ? "shadPS4:GpuPrep0" : "shadPS4:GpuPrep1");
    BbThreads::PlaceThread(BbThreads::Role::Prep);
    Shader::g_info_slot = 2 + index; // info.h: slots 2..5 are the draw workers'
    Regs& prep = *prep_regs[index];
    u64 seq = 0;
    while (!stoken.stop_requested()) {
        if (back_head.load(std::memory_order_acquire) == seq) {
            bool found = false;
            for (u32 spin = 0; spin < 2048; ++spin) {
                if (back_head.load(std::memory_order_acquire) != seq) {
                    found = true;
                    break;
                }
                CpuRelax();
            }
            if (!found) {
                prep_sleepers.fetch_add(1, std::memory_order_seq_cst);
                const u32 seen = prep_wake.load(std::memory_order_seq_cst);
                if (back_head.load(std::memory_order_seq_cst) == seq &&
                    !stoken.stop_requested()) {
                    prep_wake.wait(seen, std::memory_order_seq_cst);
                }
                prep_sleepers.fetch_sub(1, std::memory_order_seq_cst);
            }
            continue;
        }
        const BackOp& op = back_ring[seq % BackRingSize];
        for (size_t i = 0; i < op.reg_chunks.size(); i += RegChunkDwords + 1) {
            std::memcpy(&prep.reg_array[op.reg_chunks[i] * RegChunkDwords], &op.reg_chunks[i + 1],
                        RegChunkDwords * sizeof(u32));
        }
        const bool is_draw = op.kind == BackOp::Kind::Draw || op.kind == BackOp::Kind::DrawIndirect;
        const bool is_disp = op.kind == BackOp::Kind::Dispatch ||
                             op.kind == BackOp::Kind::DispatchIndirect;
        const auto setup_stages = [&] {
            for (u32 i = 0; i < op.num_stages; ++i) {
                const BackStage& stage = op.stages[i];
                Shader::InfoRuntime& rt = stage.info->runtime[Shader::g_info_slot];
                rt.user_data = stage.num_user_data == ~0u
                                   ? std::span<const u32>{}
                                   : std::span<const u32>{stage.user_data.data(),
                                                          stage.num_user_data};
                rt.flattened_ud_buf.assign(stage.flat.begin(), stage.flat.end());
                rt.pgm_base = stage.pgm_base;
            }
        };
        if (is_draw && seq % prep_workers == index &&
            seq >= back_tail.load(std::memory_order_acquire)) {
            Vulkan::PreparedDraw& slot = prep_ring[seq % BackRingSize];
            slot.map_epoch = rasterizer->MapEpoch();
            setup_stages();
            if (xp_on) {
                slot.resources_ready = false; // sotc: SOTC_CULL_EXPRESS
                slot.mrt_mask = 0xFF;
            }
            rasterizer->PrefetchImagesShared(op.pipeline); // SOTC_ASYNC_IMAGE_CREATE
            rasterizer->PrepareDraw(prep, op.pipeline, op.is_indexed,
                                    op.kind == BackOp::Kind::Draw, slot);
            if (xp_on && !slot.resources_ready) {
                XpFillResources(slot, op.pipeline);
            }
            slot.ready_seq.store(seq, std::memory_order_release);
        } else if (xp_on && is_disp && seq % prep_workers == index &&
                   seq >= back_tail.load(std::memory_order_acquire)) {
            // sotc: SOTC_CULL_EXPRESS: dispatches get a slot too, with the resource footprint only.
            Vulkan::PreparedDraw& slot = prep_ring[seq % BackRingSize];
            slot.resources_ready = false;
            slot.mrt_mask = 0xFF;
            setup_stages();
            XpFillResources(slot, op.pipeline);
            slot.ready_seq.store(seq, std::memory_order_release);
        }
        ++seq;
        prep_tail[index].store(seq, std::memory_order_seq_cst);
        if (drain_waiting.load(std::memory_order_seq_cst)) {
            std::scoped_lock lk{back_mutex};
            drain_cv.notify_all();
        }
    }
}

const Vulkan::PreparedDraw* Liverpool::TakePrepared(u64 seq) {
    // sotc: SOTC_PARALLEL_RECORD: published for this very op (the slot is reused every
    // BackRingSize ops) and no mapping changed since it was computed.
    const Vulkan::PreparedDraw& slot = prep_ring[seq % BackRingSize];
    const auto count = [](std::atomic<uint64_t>& counter) {
        if (SotcStats::kFpsLog) {
            counter.fetch_add(1, std::memory_order_relaxed);
        }
    };
    if (slot.ready_seq.load(std::memory_order_acquire) != seq) {
        count(SotcStats::pr_not_ready);
        return nullptr;
    }
    if (!slot.valid) {
        count(SotcStats::pr_ineligible);
        return nullptr;
    }
    if (slot.map_epoch != rasterizer->MapEpoch()) {
        count(SotcStats::pr_epoch);
        return nullptr;
    }
    count(SotcStats::pr_prepared);
    return &slot;
}

void Liverpool::SubmitDraw(bool is_indexed, u32 index_offset) {
    if (!back_threaded) {
        rasterizer->Draw(is_indexed, index_offset);
        return;
    }
    WaitBackWrites();
    // sotc: no pipeline for a draw FilterDraw drops on the draw thread (review of #99).
    const auto* pipeline =
        rasterizer->IsDrawFilteredOut() ? nullptr : rasterizer->LookupGraphicsPipeline(0, 0);
    if (!prep_workers) { // sotc: SOTC_PARALLEL_RECORD: the prep threads do it (PrepLoop)
        rasterizer->PrefetchImages(pipeline); // sotc: SOTC_ASYNC_IMAGE_CREATE
    }
    BackOp& op = BackAlloc();
    op.kind = BackOp::Kind::Draw;
    op.is_indexed = is_indexed;
    op.index_offset = index_offset;
    CaptureBackOp(op, pipeline);
    BackPublish(back_strict && PipelineWrites(pipeline));
}

void Liverpool::SubmitDrawIndirect(bool is_indexed, VAddr address, u32 offset, u32 stride,
                                   u32 max_count, VAddr count_address, u16 vertex_sgpr_offset,
                                   u16 instance_sgpr_offset) {
    if (!back_threaded) {
        rasterizer->DrawIndirect(is_indexed, address, offset, stride, max_count, count_address,
                                 vertex_sgpr_offset, instance_sgpr_offset);
        return;
    }
    // sotc: SOTC_WD_FUSE: WRITE_DATA packets held back for this draw's arguments (or queued).
    NotePairs();
    PrepareWdForIndirect(address + offset, u64{stride} * max_count, count_address == 0);
    WaitBackWrites();
    const auto* pipeline = rasterizer->IsDrawFilteredOut()
                               ? nullptr
                               : rasterizer->LookupGraphicsPipeline(vertex_sgpr_offset,
                                                                    instance_sgpr_offset);
    if (!prep_workers) { // sotc: SOTC_PARALLEL_RECORD: the prep threads do it (PrepLoop)
        rasterizer->PrefetchImages(pipeline); // sotc: SOTC_ASYNC_IMAGE_CREATE
    }
    BackOp& op = BackAlloc();
    AttachWd(op);
    op.kind = BackOp::Kind::DrawIndirect;
    op.is_indexed = is_indexed;
    op.address = address;
    op.offset = offset;
    op.stride = stride;
    op.max_count = max_count;
    op.count_address = count_address;
    op.vertex_sgpr_offset = vertex_sgpr_offset;
    op.instance_sgpr_offset = instance_sgpr_offset;
    CaptureBackOp(op, pipeline);
    BackPublish(back_strict && PipelineWrites(pipeline));
}

void Liverpool::PrefetchComputeAhead(std::span<const u32> rest) {
    auto& cache = rasterizer->GetPipelineCache();
    if (!cache.ComputePrefetchEnabled()) {
        return;
    }
    const auto& cs = GetCsRegs();
    // Usual case: the shader is known (one cached binary info lookup, as the dispatch does).
    if (cache.IsProgramKnown(GetParams(cs).hash)) {
        return;
    }
    SotcStats::compute_prefetch_scans.fetch_add(1, std::memory_order_relaxed);
    // The scan replays on a copy the only packets that change the compute registers (the
    // SET_SH_REG writes of ProcessGraphics/ProcessCompute and the dispatch fields), so each
    // dispatch it meets sees the registers it will run with. Indirect buffers are not followed.
    // The current shader is translated by the dispatch itself, meanwhile.
    ComputeProgram copy = cs;
    constexpr size_t MaxDwords = 64_KB;
    constexpr u32 MaxDispatches = 256;
    u32 dispatches = 0;
    size_t pos = 0;
    const size_t end = std::min(rest.size(), MaxDwords);
    while (pos < end && dispatches < MaxDispatches) {
        const auto* header = reinterpret_cast<const PM4Header*>(&rest[pos]);
        if (header->type == 2) {
            ++pos;
            continue;
        }
        if (header->type != 3) {
            break;
        }
        const u32 count = header->type3.NumWords();
        if (pos + count + 1 > end) {
            break;
        }
        switch (header->type3.opcode) {
        case PM4ItOpcode::SetShReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            const u32 reg = set_data->reg_offset;
            const size_t offset = (reg - 0x200) * sizeof(u32);
            const size_t set_size = (count - 1) * sizeof(u32);
            if (count >= 1 && reg >= 0x200 && reg <= 0x200 + sizeof(ComputeProgram) / 4 &&
                offset + set_size <= sizeof(ComputeProgram)) {
                std::memcpy(reinterpret_cast<u8*>(&copy) + offset,
                            reinterpret_cast<const u32*>(header) + 2, set_size);
            }
            break;
        }
        case PM4ItOpcode::DispatchDirect: {
            const auto* dispatch = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
            copy.dim_x = dispatch->dim_x;
            copy.dim_y = dispatch->dim_y;
            copy.dim_z = dispatch->dim_z;
            copy.dispatch_initiator = dispatch->dispatch_initiator;
            [[fallthrough]];
        }
        case PM4ItOpcode::DispatchIndirect:
            if (copy.dispatch_initiator & 1) {
                ++dispatches;
                cache.PrefetchComputeProgram(copy);
            }
            break;
        default:
            break;
        }
        pos += count + 1;
    }
}

void Liverpool::SubmitDispatch(bool indirect, VAddr address, u32 offset, u32 size) {
    if (!back_threaded) {
        if (indirect) {
            rasterizer->DispatchIndirect(address, offset, size);
        } else {
            rasterizer->DispatchDirect();
        }
        return;
    }
    if (indirect) { // sotc: SOTC_WD_FUSE
        NotePairs();
        PrepareWdForIndirect(address + offset, size, true);
    } else {
        FlushWd();
    }
    WaitBackWrites();
    const auto* pipeline = rasterizer->LookupComputePipeline();
    // sotc: SOTC_ASYNC_IMAGE_CREATE; with SOTC_PARALLEL_RECORD the prep threads prefetch too, so
    // the dispatches take their lock (PrefetchImagesShared).
    if (prep_workers) {
        rasterizer->PrefetchImagesShared(pipeline);
    } else {
        rasterizer->PrefetchImages(pipeline);
    }
    BackOp& op = BackAlloc();
    if (indirect) {
        AttachWd(op);
    }
    op.kind = indirect ? BackOp::Kind::DispatchIndirect : BackOp::Kind::Dispatch;
    op.address = address;
    op.offset = offset;
    op.size = size;
    op.cs = GetCsRegs();
    CaptureBackOp(op, pipeline);
    BackPublish(back_strict);
    if (xp_on && curr_qid == static_cast<s32>(GfxQueueId)) { // sotc: SOTC_CULL_EXPRESS
        xp_last_dispatch = back_head.load(std::memory_order_relaxed);
    }
}

// sotc: SOTC_CULL_EXPRESS=1 (default off): the express lane.
//
// Problem: with SOTC_CP_AHEAD=2 the XPR occlusion submit of frame N (occluder depth draws, HiZ
// dispatches, the main cull dispatch, then an EOP label the game spins on) can sit in the back
// ring behind hundreds of frame N-1 ops the draw thread has not recorded yet. The PS4 GPU front
// end would have reached it long before.
//
// Mechanism. The command processor tags every op with the queue that parsed it and, for Func ops,
// with what they do to guest memory (XpHint). It publishes a segment [start, end) when it parses
// an EOP that matches the XPR pattern (CS_DONE/bottom-of-pipe, data 1, a dispatch since the
// segment start); a segment starts at the beginning of the top-level gfx submit or after the
// previous label. At an op boundary the draw thread (XpPoll) may execute the gfx ops of that
// segment (the "express" ops) BEFORE the ops [back_tail, start) (plus any foreign-queue op parsed
// inside the segment): both sets are the "skipped" ops. It first submits the open command buffer
// (it holds only older, already recorded work), records the express ops, submits them at once,
// and goes on in FIFO order; the lane-run ops are flagged xp_done and only replay their register
// state when FIFO reaches them. GPU order is queue order, so on the host the express work now sits
// between the recorded older work and the not yet recorded skipped work. Everything uncertain
// keeps the FIFO order and is counted (SotcStats::XpReason).
//
// Why that is exact (each rule is tested before the segment runs ahead):
//  1. Known footprints. Every skipped and every express draw/dispatch needs a footprint from
//     op_footprint.h (page-rounded read/write ranges of buffers, images with all their mips,
//     render targets, depth, vertex/index/indirect data), computed on the GpuPrep threads. Func
//     ops are admitted only when the parser classified them: labels (a write of the label
//     address), WRITE_DATA and mem-to-mem/fill DMA (their ranges), flips and frame marks (no
//     memory), and WRITE_DATA fused into an op (its segments are writes). Anything else (DMA from or to GDS,
//     lod stats, ordered commands, EopService, an op without footprint) blocks.
//  2. No conflict. Conflicts() on the union of all express footprints against each skipped
//     footprint: write/read, read/write and write/write overlaps all refuse the reorder, so for
//     every pair of ops that could exchange data, the order is the stream's. Disjoint ops commute
//     on the GPU too (the host orders by queue, barriers are per resource). The host-side
//     caches (buffer, texture, image layouts) are touched only by the draw thread in record order,
//     which is also the GPU submission order, so their state is consistent by construction.
//  3. Progress signals. A skipped label/flip/frame mark may be seen by the game after the express
//     label instead of before. Labels write different addresses (the same 8-byte key as an
//     express label blocks: per-address FIFO), the express label announces only work recorded
//     before it, and the game never infers "frame N-1 finished" from the XPR label of frame N
//     (the RB_EXACT design already accepts cross-address label reordering). Each skipped label
//     still lands after the work it guards, because it stays behind it. SOTC_CULL_EXPRESS_SIGNALS=0
//     / _MARKS=0 make them block. A skipped frame mark (OnSubmit + Flush, the frames_recorded
//     ticket) runs later than before relative to the express ops: its garbage collection ages
//     by scheduler tick, and the ticket only gates guest submits that were not ahead of it.
//  4. Record-time guest reads. A skipped op reads guest memory when it is recorded, which is
//     now later by the express duration. That is legal: the PS4 GPU reads at execution time,
//     later than parse time; the game keeps memory stable until the label that follows the op,
//     and that label is itself skipped, so it is written after the op was recorded. The express
//     ops read earlier than in FIFO but after the game submitted them, which already
//     guarantees their inputs; inputs written by older recorded ops are earlier in GPU order, and
//     inputs written by skipped ops are refused by rule 2.
//  5. Bounds: at most SOTC_CULL_EXPRESS_SKIP skipped ops (default 480), SOTC_CULL_EXPRESS_SEG
//     express ops (256) and SOTC_CULL_EXPRESS_US (500 us) for the whole decision.
// Registers: every op carries the register chunks that changed since the previous op of the
// stream, so the lane advances back_regs through [tail, end) in stream order (undo log), and
// restores the state at tail afterwards; an op that already ran replays only its chunks.
// Not done: renaming the HiZ image to avoid its write-after-read conflict (see the report).

namespace {
void XpAddRange(boost::container::small_vector<AmdGpu::OpRange, 16>& v, VAddr addr, u64 size) {
    if (size == 0) {
        return;
    }
    const VAddr lo = addr & ~VAddr{0xFFF};
    const VAddr hi = (addr + size + 0xFFF) & ~VAddr{0xFFF};
    v.push_back({lo, hi - lo});
}
void XpAddRange(boost::container::small_vector<AmdGpu::OpRange, 8>& v, VAddr addr, u64 size) {
    if (size == 0) {
        return;
    }
    const VAddr lo = addr & ~VAddr{0xFFF};
    const VAddr hi = (addr + size + 0xFFF) & ~VAddr{0xFFF};
    v.push_back({lo, hi - lo});
}
} // namespace

void Liverpool::XpFillResources(Vulkan::PreparedDraw& slot, const Vulkan::Pipeline* pipeline) {
    if (!pipeline) {
        return; // a dropped draw: XpEvaluate treats a null pipeline as an empty footprint
    }
    const auto stages = pipeline->GetStages();
    const Shader::Gcn::FetchShaderData* fetch = nullptr;
    if (!pipeline->IsCompute()) {
        const auto* graphics = static_cast<const Vulkan::GraphicsPipeline*>(pipeline);
        fetch = &graphics->GetFetchShader();
        slot.mrt_mask = static_cast<u8>(graphics->GetGraphicsKey().mrt_mask);
    }
    AmdGpu::CollectResourceFootprint(std::span<const Shader::Info* const>{stages}, fetch,
                                     slot.resources);
    slot.resources_ready = true;
}

void Liverpool::XpPublishSeg(const XpSeg& seg) {
    SotcStats::xp_segments.fetch_add(1, std::memory_order_relaxed);
    if (!prep_workers) {
        SotcStats::xp_reasons[SotcStats::XpNoPrep].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u64 head = xp_head.load(std::memory_order_relaxed);
    if (head - xp_tail.load(std::memory_order_acquire) >= XpRing) {
        return; // the draw thread is far behind on segments: FIFO
    }
    xp_ring[head % XpRing] = seg;
    xp_head.store(head + 1, std::memory_order_release);
    if (back_sleeping.load(std::memory_order_seq_cst)) {
        WakeBack();
    }
}

void Liverpool::XpApplyState(const BackOp& op) {
    for (size_t i = 0; i < op.reg_chunks.size(); i += RegChunkDwords + 1) {
        const u32 chunk = op.reg_chunks[i];
        const u64 bit = u64{1} << (chunk % 64);
        if (!(xp_undo_mask[chunk / 64] & bit)) {
            xp_undo_mask[chunk / 64] |= bit;
            xp_undo_list.push_back(static_cast<u16>(chunk));
            xp_undo.push_back(chunk);
            const u32* src = &back_regs.reg_array[chunk * RegChunkDwords];
            xp_undo.insert(xp_undo.end(), src, src + RegChunkDwords);
        }
        std::memcpy(&back_regs.reg_array[chunk * RegChunkDwords], &op.reg_chunks[i + 1],
                    RegChunkDwords * sizeof(u32));
    }
    back_cb_extent = op.cb_extent;
    back_db_extent = op.db_extent;
    if (op.kind == BackOp::Kind::Dispatch || op.kind == BackOp::Kind::DispatchIndirect) {
        back_cs = op.cs;
    }
}

void Liverpool::XpUndoState() {
    for (size_t i = 0; i < xp_undo.size(); i += RegChunkDwords + 1) {
        std::memcpy(&back_regs.reg_array[xp_undo[i] * RegChunkDwords], &xp_undo[i + 1],
                    RegChunkDwords * sizeof(u32));
    }
    for (const u16 chunk : xp_undo_list) {
        xp_undo_mask[chunk / 64] &= ~(u64{1} << (chunk % 64));
    }
    xp_undo.clear();
    xp_undo_list.clear();
    back_cb_extent = xp_saved_cb;
    back_db_extent = xp_saved_db;
    back_cs = xp_saved_cs;
}

void Liverpool::XpReplayState(const BackOp& op) {
    for (size_t i = 0; i < op.reg_chunks.size(); i += RegChunkDwords + 1) {
        std::memcpy(&back_regs.reg_array[op.reg_chunks[i] * RegChunkDwords], &op.reg_chunks[i + 1],
                    RegChunkDwords * sizeof(u32));
    }
    back_cb_extent = op.cb_extent;
    back_db_extent = op.db_extent;
    if (op.kind == BackOp::Kind::Dispatch || op.kind == BackOp::Kind::DispatchIndirect) {
        back_cs = op.cs;
    }
}

void Liverpool::XpPoll(u64 tail) {
    while (xp_head.load(std::memory_order_acquire) != xp_tail.load(std::memory_order_relaxed)) {
        const XpSeg seg = xp_ring[xp_tail.load(std::memory_order_relaxed) % XpRing];
        if (!XpEvaluate(seg, tail)) {
            return; // retry at the next op boundary
        }
        xp_retries = 0;
        xp_tail.store(xp_tail.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }
}

bool Liverpool::XpEvaluate(const XpSeg& seg, u64 tail) {
    using namespace SotcStats;
    const auto done = [&](XpReason r) {
        xp_reasons[r].fetch_add(1, std::memory_order_relaxed);
        return true;
    };
    const auto fifo = [&](XpReason r) {
        if (!xp_watch_end) { // measure when its label really runs
            xp_watch_end = seg.end;
            xp_watch_ns = seg.queued_ns;
        }
        return done(r);
    };
    if (seg.start <= tail) {
        return done(XpNoBacklog);
    }
    if (!rasterizer || !prep_workers) {
        return fifo(XpNoPrep);
    }
    if (seg.end - seg.start > xp_max_seg) {
        return fifo(XpSegTooLong);
    }
    if (seg.start - tail > xp_max_skip) {
        return fifo(XpSkipTooMany);
    }

    struct Skipped {
        OpFootprint fp;
        u8 kind; // 0 draw, 1 dispatch, 2 func
        bool has_ranges;
    };
    thread_local std::vector<Skipped> skipped;
    thread_local std::vector<VAddr> skipped_labels;
    skipped.clear();
    skipped_labels.clear();
    OpFootprint ex;
    ex.known = true;
    std::array<VAddr, 4> ex_labels{};
    u32 num_ex_labels = 0;
    u32 num_express = 0, num_skipped = 0;
    u32 sk_labels = 0, sk_flips = 0, sk_marks = 0;

    const u64 t0 = NowNs();
    xp_saved_cb = back_cb_extent;
    xp_saved_db = back_db_extent;
    xp_saved_cs = back_cs;

    XpReason fail = XpOk;
    bool retry = false;
    for (u64 seq = tail; seq < seg.end && fail == XpOk; ++seq) {
        BackOp& op = back_ring[seq % BackRingSize];
        XpApplyState(op); // back_regs now holds the registers this op runs with
        const bool express = seq >= seg.start && op.qid == GfxQueueId;
        if (op.xp_done && !express) {
            continue; // ran ahead with an earlier segment: not part of this reorder
        }
        OpFootprint fp;
        fp.known = true;
        bool bad_func = false, not_ready = false, unknown_fp = false, signal_blocked = false;
        bool is_label = false;
        VAddr label_key = 0;
        u8 kind = 2;
        for (u32 i = 0; i < op.wd.num_segs; ++i) { // WRITE_DATA fused into the op: CPU-side writes
            XpAddRange(fp.writes, op.wd.segs[i].address, op.wd.segs[i].length);
        }
        switch (op.kind) {
        case BackOp::Kind::Func:
            if (!op.func) {
                break; // a flushed batch of WRITE_DATA: only the fused segments above
            }
            switch (op.xp_cls) {
            case XpLabel:
                XpAddRange(fp.writes, op.xp_waddr, op.xp_wsize);
                is_label = true;
                label_key = op.xp_waddr & ~VAddr{7};
                signal_blocked = !express && !xp_signals;
                break;
            case XpWriteData:
                XpAddRange(fp.writes, op.xp_waddr, op.xp_wsize);
                break;
            case XpDma:
                XpAddRange(fp.reads, op.xp_raddr, op.xp_rsize);
                XpAddRange(fp.writes, op.xp_waddr, op.xp_wsize);
                break;
            case XpMark:
                bad_func = express;
                signal_blocked = !express && !xp_marks;
                break;
            case XpFlip:
                bad_func = express;
                signal_blocked = !express && !xp_signals;
                break;
            default:
                bad_func = true;
                break;
            }
            break;
        case BackOp::Kind::Draw:
        case BackOp::Kind::DrawIndirect:
        case BackOp::Kind::Dispatch:
        case BackOp::Kind::DispatchIndirect: {
            const bool dispatch = op.kind == BackOp::Kind::Dispatch ||
                                  op.kind == BackOp::Kind::DispatchIndirect;
            kind = dispatch ? 1 : 0;
            if (!op.pipeline) {
                break; // dropped by the draw filter: runs as nothing
            }
            const Vulkan::PreparedDraw& slot = prep_ring[seq % BackRingSize];
            if (slot.ready_seq.load(std::memory_order_acquire) != seq || !slot.resources_ready) {
                not_ready = true;
                break;
            }
            OpArgs args;
            args.indirect = op.kind == BackOp::Kind::DrawIndirect ||
                            op.kind == BackOp::Kind::DispatchIndirect;
            args.is_indexed = op.is_indexed;
            args.index_offset = op.index_offset;
            args.address = op.address;
            args.offset = op.offset;
            args.stride = op.stride;
            args.max_count = op.max_count;
            args.count_address = op.count_address;
            args.size = op.size;
            OpFootprint part;
            const bool ok = dispatch ? ComputeDispatchFootprint(slot, op.cs, part, &args)
                                     : ComputeDrawFootprint(slot, back_regs, part, &args);
            if (!ok || !part.known) {
                unknown_fp = true;
                break;
            }
            fp.reads.insert(fp.reads.end(), part.reads.begin(), part.reads.end());
            fp.writes.insert(fp.writes.end(), part.writes.begin(), part.writes.end());
            break;
        }
        }
        if (bad_func) {
            fail = express ? XpExpressUnknown : XpSkipUnknownFunc;
        } else if (signal_blocked) {
            fail = XpSkipSignals;
        } else if (not_ready) {
            fail = express ? XpExpressNotReady : XpSkipNotReady;
            retry = true;
        } else if (unknown_fp) {
            fail = express ? XpExpressUnknownFp : XpSkipUnknownFp;
        }
        if (fail != XpOk) {
            break;
        }
        if (express) {
            ++num_express;
            ex.reads.insert(ex.reads.end(), fp.reads.begin(), fp.reads.end());
            ex.writes.insert(ex.writes.end(), fp.writes.begin(), fp.writes.end());
            if (is_label && num_ex_labels < ex_labels.size()) {
                ex_labels[num_ex_labels++] = label_key;
            }
        } else {
            ++num_skipped;
            if (is_label) {
                skipped_labels.push_back(label_key);
                ++sk_labels;
            } else if (op.kind == BackOp::Kind::Func && op.xp_cls == XpFlip && op.func) {
                ++sk_flips;
            } else if (op.kind == BackOp::Kind::Func && op.xp_cls == XpMark && op.func) {
                ++sk_marks;
            }
            const bool has = !fp.reads.empty() || !fp.writes.empty();
            if (has) {
                skipped.push_back({std::move(fp), kind, true});
            }
        }
        if (((seq - tail) & 31) == 31 && NowNs() - t0 > xp_budget_ns) {
            fail = XpBudget;
        }
    }
    XpUndoState();
    const auto account = [&] {
        const u64 ns = NowNs() - t0;
        xp_decide_ns.fetch_add(ns, std::memory_order_relaxed);
        GateStoreMax(xp_decide_max_ns, ns);
    };
    if (fail != XpOk) {
        account();
        if (retry && ++xp_retries < 8) {
            return false; // a prep thread is behind: look again after the next op
        }
        return fifo(fail);
    }
    if (num_ex_labels == 0) {
        account();
        return fifo(XpExpressUnknown); // the segment must end in its label (parser invariant)
    }
    for (const VAddr key : skipped_labels) {
        for (u32 i = 0; i < num_ex_labels; ++i) {
            if (key == ex_labels[i]) {
                account();
                return fifo(XpLabelSame);
            }
        }
    }
    // Rule 2: the union of the express footprints against every skipped footprint.
    OpFootprint ew, er;
    ew.known = er.known = true;
    ew.writes = ex.writes;
    er.reads = ex.reads;
    for (const Skipped& sk : skipped) {
        if (!Conflicts(ex, sk.fp)) {
            continue;
        }
        OpFootprint sr, sw;
        sr.known = sw.known = true;
        sr.reads = sk.fp.reads;
        sw.writes = sk.fp.writes;
        if (Conflicts(ew, sr)) {
            xp_conf_war.fetch_add(1, std::memory_order_relaxed);
        } else if (Conflicts(er, sw)) {
            xp_conf_raw.fetch_add(1, std::memory_order_relaxed);
        } else {
            xp_conf_waw.fetch_add(1, std::memory_order_relaxed);
        }
        (sk.kind == 0 ? xp_conf_draw : sk.kind == 1 ? xp_conf_dispatch : xp_conf_func)
            .fetch_add(1, std::memory_order_relaxed);
        account();
        return fifo(XpConflict);
    }
    if (NowNs() - t0 > xp_budget_ns) {
        account();
        return fifo(XpBudget);
    }
    account();

    // All rules hold: run the express ops ahead.
    rasterizer->Flush(); // the open buffer holds only older recorded work
    const u64 run_start = NowNs();
    xp_saved_cb = back_cb_extent;
    xp_saved_db = back_db_extent;
    xp_saved_cs = back_cs;
    u64 label_exec_ns = 0;
    for (u64 seq = tail; seq < seg.end; ++seq) {
        BackOp& op = back_ring[seq % BackRingSize];
        XpApplyState(op);
        if (seq >= seg.start && op.qid == GfxQueueId && !op.xp_done) {
            back_exec_seq = seq;
            ExecuteBackOp(op);
            op.xp_done = true;
            if (seq + 1 == seg.end) {
                label_exec_ns = NowNs();
            }
        }
    }
    XpUndoState();
    back_exec_seq = tail;
    rasterizer->Flush(); // the express work goes to the GPU before anything older that is unrecorded
    NotifyCpWait();
    xp_ahead.fetch_add(1, std::memory_order_relaxed);
    xp_skipped_ops.fetch_add(num_skipped, std::memory_order_relaxed);
    xp_express_ops.fetch_add(num_express, std::memory_order_relaxed);
    xp_sk_labels.fetch_add(sk_labels, std::memory_order_relaxed);
    xp_sk_flips.fetch_add(sk_flips, std::memory_order_relaxed);
    xp_sk_marks.fetch_add(sk_marks, std::memory_order_relaxed);
    xp_lat_ahead_ns.fetch_add((label_exec_ns ? label_exec_ns : NowNs()) - seg.queued_ns,
                              std::memory_order_relaxed);
    xp_saved_est_ns.fetch_add(static_cast<u64>(num_skipped * xp_op_ns), std::memory_order_relaxed);
    xp_run_ns.fetch_add(NowNs() - run_start, std::memory_order_relaxed);
    return done(XpOk);
}

// sotc: SOTC_EOP_TICK=1. A label whose work is still unsubmitted waits for the flush of the next
// submission, so the guest's wait on it would stall until then. After this long the open command
// buffer is submitted (EopService), the same as the previous code did at every label.
static const u64 EopMaxWaitNs = [] {
    // sotc: SOTC_EOP_TICK_US overrides the 2 ms (diagnostics).
    const char* v = std::getenv("SOTC_EOP_TICK_US");
    return v ? std::strtoull(v, nullptr, 10) * 1000ULL : 2'000'000ULL;
}();
// sotc: SOTC_RB_EXACT: a deferred label deferred only by queue order is not flushed when queued
// (its tick may be empty), so the open buffer is submitted after this wait (SOTC_RB_WAIT_US).
static const u64 RbxMaxWaitNs = [] {
    const char* v = std::getenv("SOTC_RB_WAIT_US");
    return v ? std::strtoull(v, nullptr, 10) * 1000ULL : 300'000ULL;
}();
// sotc: SOTC_EOP_TICK statistics, printed under SOTC_FPS (driver.cpp).
std::atomic<u64> g_eop_written{0}, g_eop_latency_ns{0}, g_eop_max_latency_ns{0}, g_eop_flushes{0};
// sotc: SOTC_RB_EXACT label statistics (liverpool.h).
std::atomic<u64> g_rbx_labels_deferred{0}, g_rbx_labels_total{0}, g_rbx_defer_latency_ns{0},
    g_rbx_defer_max_latency_ns{0}, g_rbx_offthread{0};
// Adaptive policy (layer 2). Addresses in deferred mode is a gauge; the EWMAs are global averages
// over all addresses (ns), a = landing latency, b = readback wait charged to early labels.
std::atomic<u64> g_rbx_addr_deferred{0}, g_rbx_mode_switches{0}, g_rbx_probes{0},
    g_rbx_labels_early_hot{0}, g_rbx_waits_charged{0}, g_rbx_waits_unattributed{0},
    g_rbx_ewma_a_ns{1'500'000}, g_rbx_ewma_b_ns{0};

// sotc: SOTC_RB_EXACT=1, label side (rules 5-7 of the design).
//
// PS4 semantics: a GPU write becomes meaningful to the CPU when the GPU has finished it, and the
// game learns that through labels (EOP / ReleaseMem fences) and other CPU-visible GPU writes. The
// port used to write every label as soon as the command processor / draw thread executed the
// packet, long before the host GPU ran the work, so every guest readback of a window the GPU is
// still writing had to wait for that work. With the switch on, the buffer cache
// (BufferCache::RbExactEnabled) can release pages whose GPU-written ranges are still pending and
// not yet "announced"; what announces a range is a label written after it in the command stream.
// This code is the label half of that contract:
//  - Rule 5. At the label's position in the stream (draw thread, same order as the stream) the hot
//    write sequence (BufferCache::HotWriteSeq: GPU writes into windows the guest reads back) is
//    snapshotted. A label of a queue (gfx / compute) is deferred iff the sequence changed since
//    the previous label of that queue, or a deferred label to the same 8-byte aligned address is
//    still outstanding (per-address FIFO: the scheduler's queue is FIFO, and an undeferred label
//    never overtakes a deferred one to its address). Deferred labels use the SOTC_EOP_TICK
//    machinery: scheduler.DeferOperation keyed on the open tick, polled by BackLoop through
//    EopService (so also while no new work arrives), the open command buffer submitted by
//    EopService once a label has waited 2 ms (SOTC_EOP_TICK_US), the CP's own waits satisfied by
//    eop_shadow, and EopWaitGuard making the draw thread submit when a CP wait is blocked on a
//    label queued behind unsubmitted work. The interrupt follows its label (same callback).
//    Labels with no hot write since the previous one keep the old early write, so only labels
//    that guard hot data pay for the GPU round trip.
//    The game's cull pass (eboot 0xc6f2f -> 0x925c0 -> 0x86eb60) emits an EVENT_WRITE_EOP
//    (PM4 header 0xC0044700, opcode 0x47, event CS_DONE, data 1) on the gfx DCB after its
//    dispatch and then spins on the word, so a deferred label first records the downloads of the
//    hot data written before it (BufferCache::RecordLabelDownloads) and the open command buffer
//    is submitted at once (rasterizer->Flush), so the latency is the GPU execution only.
//  - Rule 6. Before a deferred label is written, BufferCache::ApplyOwedBeforeLabel applies the
//    owed downloads that are complete, so the game never sees a label before the data it
//    announces. The deferred operation runs inside Scheduler::PopPendingOperations, called by the
//    draw thread (Draw, Dispatch, Flush, EopService, buffer cache waits); a call from another
//    thread only sets the job ready and is counted in g_rbx_offthread (RbxDrain lands it later).
//  - Rule 7. Every label written to guest memory, early or deferred, then reports the snapshot
//    taken at its stream position (BufferCache::NoteLabelWritten): at landing for a deferred
//    one, right after the write for the others; a plain WRITE_DATA to memory reports the same
//    way. DMA_DATA and GDS copies are GPU writes, not CPU-visible announcements, and do not.
// SOTC_EOP_DEFER (the diagnostic priority-thread label) is ignored while this is on: its
// callback runs on a thread that must not call into the buffer cache.
// With the switch off none of this runs (rbx false) and every path is as before.
// A label is deferred when it follows a hot GPU write (the hot sequence changed since the previous
// label of its queue) AND the adaptive policy of its address says deferral pays (RbxPolicyDefer),
// or when a deferred label to the same address is still outstanding (per-address FIFO).
// There is no cross-address cascade: a later label of the queue is written early even while an
// earlier one is deferred. That is exact because the only hazard of an early label is announcing
// owed bytes before they are applied, and SettleOwedUpTo (called before every early write) closes
// it; bytes still GPU-modified stay protected, so a guest read of them takes the exact wait.
// The order of labels to different addresses can therefore differ from the PS4's.
// sotc: SOTC_RB_EXACT=publish: a deferred label to this address (8-byte key) is still outstanding.
bool Liverpool::RbxAddrOutstanding(VAddr label) {
    std::scoped_lock lk{rbx_mutex};
    return rbx_out.contains(label & ~VAddr{7});
}

bool Liverpool::RbxDecide(u32 queue, VAddr label, u64& snap, bool* hot) {
    snap = rasterizer->GetBufferCache().HotWriteSeq();
    const bool changed = snap != rbx_last_seq[queue];
    rbx_last_seq[queue] = snap;
    if (hot) {
        *hot = changed;
    }
    g_rbx_labels_total.fetch_add(1, std::memory_order_relaxed);
    const VAddr key = label & ~VAddr{7};
    {
        std::scoped_lock lk{rbx_mutex};
        if (rbx_out.contains(key)) {
            return true;
        }
    }
    if (!changed) {
        return false;
    }
    if (RbxPolicyDefer(key)) {
        return true;
    }
    RbxRecordEarly(snap, key); // an early label that announces hot data: its waits are charged
    return false;
}

// ---- SOTC_RB_EXACT adaptive policy (layer 2) ----
// Per label address (8-byte aligned, at most RbxMaxStats, least recently used evicted) two EWMAs:
// a = landing latency of the address's deferred labels (what deferring costs the game when it
// waits on the label), b = guest readback wait time charged to its early labels (what writing
// early costs). An address starts early; it is deferred once b > 1.25 a and goes back once
// a > 1.25 b. In deferred mode one hot label in 32 is written early to keep b fresh.
// Attribution: BufferCache calls RbxWaitObserver(writer_seq, wait_ns) for every guest readback
// that waited for the GPU on hot data (any guest thread). The wait is charged to the first
// recent early hot label whose snapshot >= writer_seq (the label that announced that write). A
// ring entry's accumulated cost becomes one b sample when the entry leaves the ring.

void Liverpool::RbxUpdateMode(RbxAddrStat& st) {
    if (!st.deferred && st.b_samples != 0 && st.b > st.a * 1.25) {
        st.deferred = true;
        g_rbx_addr_deferred.fetch_add(1, std::memory_order_relaxed);
        g_rbx_mode_switches.fetch_add(1, std::memory_order_relaxed);
    } else if (st.deferred && st.b_samples != 0 && st.a > st.b * 1.25) {
        st.deferred = false;
        g_rbx_addr_deferred.fetch_sub(1, std::memory_order_relaxed);
        g_rbx_mode_switches.fetch_add(1, std::memory_order_relaxed);
    }
}

Liverpool::RbxAddrStat& Liverpool::RbxStat(VAddr key) { // under rbx_stat_mutex
    auto it = rbx_stats.find(key);
    if (it == rbx_stats.end()) {
        if (rbx_stats.size() >= RbxMaxStats) {
            auto victim = rbx_stats.begin();
            for (auto i = rbx_stats.begin(); i != rbx_stats.end(); ++i) {
                if (i->second.last_use < victim->second.last_use) {
                    victim = i;
                }
            }
            if (victim->second.deferred) {
                g_rbx_addr_deferred.fetch_sub(1, std::memory_order_relaxed);
            }
            rbx_stats.erase(victim);
        }
        RbxAddrStat st;
        st.a = double(g_rbx_ewma_a_ns.load(std::memory_order_relaxed));
        it = rbx_stats.emplace(key, st).first;
    }
    it->second.last_use = ++rbx_use_clock;
    return it->second;
}

bool Liverpool::RbxPolicyDefer(VAddr key) {
    std::scoped_lock lk{rbx_stat_mutex};
    RbxAddrStat& st = RbxStat(key);
    ++st.hot_count;
    if (!st.deferred) {
        return false;
    }
    if ((st.hot_count & 31) == 0) {
        g_rbx_probes.fetch_add(1, std::memory_order_relaxed);
        return false; // re-probe early mode to keep b fresh
    }
    return true;
}

void Liverpool::RbxRecordEarly(u64 snap, VAddr key) {
    g_rbx_labels_early_hot.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lk{rbx_stat_mutex};
    if (rbx_early_count == rbx_early_ring.size()) {
        const RbxEarly& old = rbx_early_ring[rbx_early_head];
        RbxAddrStat& st = RbxStat(old.key);
        st.b = st.b_samples ? st.b + 0.125 * (old.cost - st.b) : old.cost;
        ++st.b_samples;
        const double g = double(g_rbx_ewma_b_ns.load(std::memory_order_relaxed));
        g_rbx_ewma_b_ns.store(u64(g + 0.05 * (old.cost - g)), std::memory_order_relaxed);
        RbxUpdateMode(st);
    } else {
        ++rbx_early_count;
    }
    rbx_early_ring[rbx_early_head] = RbxEarly{snap, key, 0.0};
    rbx_early_head = (rbx_early_head + 1) % u32(rbx_early_ring.size());
}

void Liverpool::RbxFoldLanding(VAddr key, u64 latency_ns) {
    std::scoped_lock lk{rbx_stat_mutex};
    RbxAddrStat& st = RbxStat(key);
    st.a = st.a_samples ? st.a + 0.125 * (double(latency_ns) - st.a) : double(latency_ns);
    ++st.a_samples;
    const double g = double(g_rbx_ewma_a_ns.load(std::memory_order_relaxed));
    g_rbx_ewma_a_ns.store(u64(g + 0.05 * (double(latency_ns) - g)), std::memory_order_relaxed);
    RbxUpdateMode(st);
}

// Any thread. Cheap: one short critical section over a ring of 128.
void Liverpool::RbxWaitObserver(u64 writer_seq, u64 wait_ns) {
    Liverpool* self = g_rbx_self.load(std::memory_order_acquire);
    if (!self) {
        return;
    }
    std::scoped_lock lk{self->rbx_stat_mutex};
    const u32 n = self->rbx_early_count;
    const u32 size = u32(self->rbx_early_ring.size());
    // Oldest entry first: the ring is in snapshot order.
    for (u32 i = 0; i < n; ++i) {
        RbxEarly& e = self->rbx_early_ring[(self->rbx_early_head + size - n + i) % size];
        if (e.snap >= writer_seq) {
            e.cost += double(wait_ns);
            g_rbx_waits_charged.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    g_rbx_waits_unattributed.fetch_add(1, std::memory_order_relaxed);
}

// Draw thread. EOS is a fence like a label: it is deferred under the same rules. Otherwise false
// is returned with snap set, and the caller settles (SettleOwedUpTo), writes now and reports.
template <typename F>
bool Liverpool::RbxTryDefer(u32 queue, VAddr addr, bool shadow32, u32 value, u64& snap, F&& make) {
    bool hot = false;
    if (!RbxDecide(queue, addr, snap, &hot)) {
        return false;
    }
    if (hot) {
        rasterizer->GetBufferCache().RecordLabelDownloads();
    }
    QueueDeferredLabel(queue, addr, shadow32, false, value, 0, snap,
                       Common::UniqueFunction<void>{make()});
    if (hot) {
        rasterizer->Flush();
    }
    return true;
}

void Liverpool::RbxNoteWritten(u64 snap) {
    rasterizer->GetBufferCache().NoteLabelWritten(snap);
}

std::shared_ptr<Liverpool::RbxJob> Liverpool::QueueDeferredLabel(
    u32 queue, VAddr label, bool shadow32, bool shadow64, u32 data_lo, u32 data_hi, u64 snap,
    Common::UniqueFunction<void>&& write, u64 t_cp, u64 t_draw) {
    auto& scheduler = rasterizer->GetScheduler();
    eop_last_tick.store(scheduler.CurrentTick(), std::memory_order_relaxed);
    if (eop_outstanding.fetch_add(1, std::memory_order_acq_rel) == 0) {
        eop_oldest_ns.store(SotcStats::NowNs(), std::memory_order_relaxed);
    }
    // The CP's waits on this label are satisfied by its pending value (see eop_shadow); only the
    // guest sees the real write, after the GPU.
    if (shadow32 || shadow64) {
        std::scoped_lock lk{eop_shadow_mutex};
        eop_shadow[label] = data_lo;
        if (shadow64) {
            eop_shadow[label + 4] = data_hi;
        }
        eop_shadow_count.store(u32(eop_shadow.size()), std::memory_order_release);
    }
    auto job = std::make_shared<RbxJob>();
    job->label = label;
    job->key = label & ~VAddr{7};
    job->shadow32 = shadow32;
    job->shadow64 = shadow64;
    job->lo = data_lo;
    job->hi = data_hi;
    job->snap = snap;
    job->queue = queue;
    job->queued_ns = SotcStats::NowNs();
    job->tick = scheduler.CurrentTick();
    job->write = std::move(write);
    // sotc: SOTC_RB_EXACT=publish: latency stamps of a CS_DONE label (t_cp != 0).
    job->stat = t_cp != 0;
    job->t_cp = t_cp;
    job->t_draw = t_draw;
    if (rbx) {
        std::scoped_lock lk{rbx_mutex};
        ++rbx_out[job->key];
        ++rbx_q_out[queue];
        rbx_jobs.push_back(job);
    }
    scheduler.DeferOperation([this, job] {
        if (rbx) {
            // The label's tick is done. Landing is in queue order and needs the draw thread and
            // the owed downloads applied; RbxDrain retries from EopService until it can.
            job->ready.store(true, std::memory_order_release);
            RbxDrain();
        } else {
            LandLabel(*job);
        }
    });
    return job;
}

// Queue time (ns) of the oldest deferred label whose tick is still the open one or later, that
// is not submitted to the GPU yet; 0 when there is none. Jobs are queued in tick order.
u64 Liverpool::RbxOldestUnsubmittedNs() {
    const u64 cur = rasterizer->GetScheduler().CurrentTick();
    std::scoped_lock lk{rbx_mutex};
    for (const auto& job : rbx_jobs) {
        if (job->tick >= cur) {
            return job->queued_ns;
        }
    }
    return 0;
}

// sotc: SOTC_RB_EXACT. Called only from top-level draw thread points that are not inside any
// buffer or texture cache operation: BackLoop's EopService, and ServiceCommandsOnBackThread
// (SOTC_FLIP_SERVICE's wait for a free frame, between two operations). It lands the labels whose
// tick the GPU has finished and runs no other scheduler operation. It deliberately does not call
// PopPendingOperations: that would run the fault manager and image reclaim operations in the
// middle of a caller that holds references across the wait. Semaphore::Wait and the presenter's
// frame waits are not hooked either, for the same reason (and the presenter callers could not be
// shown to be outside cache calls). ApplyOwedBeforeLabel returns false when it is not safe, so
// the job just stays queued.
void Liverpool::RbxWaitService() {
    if (!rbx || !rasterizer || std::this_thread::get_id() != back_id ||
        eop_outstanding.load(std::memory_order_acquire) == 0) {
        return;
    }
    RbxDrain();
}

// Writes the label, its interrupt and the bookkeeping (draw thread when rbx).
void Liverpool::LandLabel(RbxJob& job) {
    const u64 latency = SotcStats::NowNs() - job.queued_ns;
    if (eop_tick) {
        g_eop_written.fetch_add(1, std::memory_order_relaxed);
        g_eop_latency_ns.fetch_add(latency, std::memory_order_relaxed);
        u64 prev = g_eop_max_latency_ns.load(std::memory_order_relaxed);
        while (latency > prev && !g_eop_max_latency_ns.compare_exchange_weak(
                                     prev, latency, std::memory_order_relaxed)) {
        }
    }
    if (rbx) {
        SotcStats::RbExLabelDeferral(latency);
        RbxFoldLanding(job.key, latency);
        g_rbx_labels_deferred.fetch_add(1, std::memory_order_relaxed);
        g_rbx_defer_latency_ns.fetch_add(latency, std::memory_order_relaxed);
        u64 prev = g_rbx_defer_max_latency_ns.load(std::memory_order_relaxed);
        while (latency > prev && !g_rbx_defer_max_latency_ns.compare_exchange_weak(
                                     prev, latency, std::memory_order_relaxed)) {
        }
    }
    job.write();
    if (job.stat) {
        // sotc: SOTC_RB_EXACT=publish latency sums. Differences clamp at 0: a stamp can be taken
        // out of order when the draw thread lands a label during Flush.
        const auto diff = [](u64 a, u64 b) { return a > b ? a - b : u64{0}; };
        const u64 t_land = SotcStats::NowNs();
        const u64 t_sub = job.t_sub ? job.t_sub : job.t_draw;
        const u64 t_gpu = job.t_gpu ? job.t_gpu : t_sub;
        SotcStats::rbp_labels_deferred.fetch_add(1, std::memory_order_relaxed);
        SotcStats::rbp_lat_cp_ns.fetch_add(diff(job.t_draw, job.t_cp), std::memory_order_relaxed);
        SotcStats::rbp_lat_submit_ns.fetch_add(diff(t_sub, job.t_draw),
                                               std::memory_order_relaxed);
        SotcStats::rbp_lat_gpu_ns.fetch_add(diff(t_gpu, t_sub), std::memory_order_relaxed);
        SotcStats::rbp_lat_land_ns.fetch_add(diff(t_land, t_gpu), std::memory_order_relaxed);
        SotcStats::rbp_lat_samples.fetch_add(1, std::memory_order_relaxed);
    }
    if (job.shadow32 || job.shadow64) {
        // Written for real now: drop the shadow unless a later label of the same address
        // already replaced it.
        std::scoped_lock lk{eop_shadow_mutex};
        const auto drop = [&](VAddr a, u32 v) {
            if (const auto it = eop_shadow.find(a); it != eop_shadow.end() && it->second == v) {
                eop_shadow.erase(it);
            }
        };
        drop(job.label, job.lo);
        if (job.shadow64) {
            drop(job.label + 4, job.hi);
        }
        eop_shadow_count.store(u32(eop_shadow.size()), std::memory_order_release);
    }
    if (rbx) {
        RbxNoteWritten(job.snap); // rule 7, at landing
        std::scoped_lock lk{rbx_mutex};
        if (const auto it = rbx_out.find(job.key); it != rbx_out.end() && --it->second == 0) {
            rbx_out.erase(it);
        }
        --rbx_q_out[job.queue];
    }
    if (eop_outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        eop_oldest_ns.store(0, std::memory_order_relaxed);
    }
    NotifyCpWait(); // sotc: SOTC_CP_IDLE_US
}

// SOTC_RB_EXACT: lands the deferred labels in queue order. A label lands when its tick is done
// (ready), on the draw thread only, and when ApplyOwedBeforeLabel says the data it announces is in
// guest memory (false: stay queued, EopService retries). An op that ran on another thread only
// sets ready; the next draw thread poll lands it. Order is kept by our own FIFO, not by the
// scheduler's queue, so a retry never lets a later label overtake an earlier one.
void Liverpool::RbxDrain() {
    if (std::this_thread::get_id() != back_id) {
        g_rbx_offthread.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (;;) {
        std::shared_ptr<RbxJob> job;
        {
            std::scoped_lock lk{rbx_mutex};
            if (rbx_jobs.empty()) {
                return;
            }
            job = rbx_jobs.front();
        }
        // Ready when the scheduler op ran (an optimisation) or the GPU has finished the job's
        // tick: the drain does not depend on any scheduler operation having run.
        if (!job->ready.load(std::memory_order_acquire)) {
            auto* work = rasterizer->GetScheduler().GetWorkSemaphore();
            if (!work->IsFree(job->tick)) {
                work->Refresh();
            }
            if (!work->IsFree(job->tick)) {
                return;
            }
            job->ready.store(true, std::memory_order_release);
        }
        if (job->stat && job->t_gpu == 0) {
            job->t_gpu = SotcStats::NowNs(); // sotc: publish: the tick is seen done (first look)
        }
        // Rule 6: the data the label announces must be in guest memory first.
        if (!rasterizer->GetBufferCache().ApplyOwedBeforeLabel()) {
            return;
        }
        {
            std::scoped_lock lk{rbx_mutex};
            rbx_jobs.pop_front();
        }
        LandLabel(*job);
    }
}

void Liverpool::EopService() {
    eop_service_requested.store(false, std::memory_order_relaxed);
    if (!rasterizer || eop_outstanding.load(std::memory_order_acquire) == 0) {
        return;
    }
    auto& scheduler = rasterizer->GetScheduler();
    // Runs the queued labels whose tick the GPU has reached. The queue is FIFO, so labels to the
    // same address are written in submission order.
    scheduler.PopPendingOperations();
    if (rbx) {
        RbxDrain(); // sotc: SOTC_RB_EXACT: retry labels held by ApplyOwedBeforeLabel
    }
    if (eop_outstanding.load(std::memory_order_acquire) == 0) {
        return;
    }
    if (rbx) {
        // The guard runs on the oldest label that is not submitted yet, not on the 0 -> 1
        // timestamp, which goes stale while labels keep streaming. A flush moves every queued
        // label to a submitted tick, so it happens at most once per tick.
        const u64 queued = RbxOldestUnsubmittedNs();
        const u64 tick = scheduler.CurrentTick();
        if (queued != 0 && SotcStats::NowNs() - queued > std::min(EopMaxWaitNs, RbxMaxWaitNs) &&
            rbx_flush_tick != tick) {
            rbx_flush_tick = tick;
            g_eop_flushes.fetch_add(1, std::memory_order_relaxed);
            rasterizer->Flush();
        }
        return;
    }
    const u64 waited = SotcStats::NowNs() - eop_oldest_ns.load(std::memory_order_relaxed);
    if (waited > EopMaxWaitNs &&
        eop_last_tick.load(std::memory_order_relaxed) >= scheduler.CurrentTick()) {
        g_eop_flushes.fetch_add(1, std::memory_order_relaxed);
        rasterizer->Flush();
    }
}

bool Liverpool::EopShadowSatisfies(const PM4CmdWaitRegMem& wait) {
    if (wait.mem_space.Value() != PM4CmdWaitRegMem::MemSpace::Memory ||
        eop_shadow_count.load(std::memory_order_acquire) == 0) {
        return false;
    }
    std::scoped_lock lk{eop_shadow_mutex};
    const auto it = eop_shadow.find(wait.Address<VAddr>());
    return it != eop_shadow.end() && wait.TestValue(it->second);
}

void Liverpool::EopWaitGuard() {
    if (eop_service_requested.load(std::memory_order_relaxed) ||
        eop_outstanding.load(std::memory_order_acquire) == 0) {
        return;
    }
    if (rbx) {
        const u64 queued = RbxOldestUnsubmittedNs();
        if (queued == 0 || SotcStats::NowNs() - queued <= std::min(EopMaxWaitNs, RbxMaxWaitNs)) {
            return;
        }
    } else {
        const u64 waited = SotcStats::NowNs() - eop_oldest_ns.load(std::memory_order_relaxed);
        if (waited <= EopMaxWaitNs) {
            return;
        }
    }
    eop_service_requested.store(true, std::memory_order_relaxed);
    QueueBackFunc(Common::UniqueFunction<void>{[this] { EopService(); }}, false);
}

u8 Liverpool::MemWaitKind(const PM4CmdWaitRegMem& wait, bool vo_label) const {
    if (wait.mem_space.Value() != PM4CmdWaitRegMem::MemSpace::Memory) {
        return CpWaitOther;
    }
    if (vo_label) {
        return CpWaitFlip;
    }
    // A label still to come from the draw thread (EOP/EOS, WRITE_DATA, RELEASE_MEM run there) or
    // from the GPU (SOTC_EOP_TICK). Approximate: any queued op counts.
    if ((back_threaded &&
         back_tail.load(std::memory_order_relaxed) != back_head.load(std::memory_order_relaxed)) ||
        eop_outstanding.load(std::memory_order_relaxed) != 0) {
        return CpWaitLabel;
    }
    return CpWaitRegMem;
}

// sotc: SOTC_CP_IDLE_US. Lost wake-ups: NotifyCpWait bumps cp_wake_epoch and then reads
// cp_sleeping; this sets cp_sleeping and then reads the epoch (all seq_cst), so either the
// notifier sees the sleeper and wakes it, or the sleeper sees the new epoch and does not block.
// The caller read seen_epoch before it last re-tested every waiting task, so a change made
// before that re-test was seen by it, and one made after it moves the epoch. Conditions only the
// guest CPU changes (game-written labels, Rewind, semaphores) cannot notify: the timeout bounds
// how late those are seen. On Windows the condition variable and WaitOnAddress timeouts are whole
// milliseconds, so the block is an auto-reset event plus a high-resolution waitable timer.
bool Liverpool::CpIdleSleep(u64 seen_epoch) {
    const u64 start = SotcStats::kFpsLog ? SotcStats::NowNs() : 0;
    cp_sleeping.store(true, std::memory_order_seq_cst);
    bool notified = cp_wake_epoch.load(std::memory_order_seq_cst) != seen_epoch;
    if (notified) {
        cp_sleeping.store(false, std::memory_order_relaxed);
        return true; // changed during the round: not counted as a sleep
    }
#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    struct Timer {
        HANDLE handle = nullptr;
        ~Timer() {
            if (handle) {
                ::CloseHandle(handle);
            }
        }
    };
    thread_local Timer timer;
    if (!timer.handle) {
        timer.handle = ::CreateWaitableTimerExW(nullptr, nullptr,
                                                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                TIMER_ALL_ACCESS);
        if (!timer.handle) {
            timer.handle = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
    }
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(cp_idle_timeout_ns / 100); // relative, 100 ns units
    const HANDLE handles[2] = {static_cast<HANDLE>(cp_wake_event), timer.handle};
    DWORD result = WAIT_FAILED;
    if (timer.handle && ::SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE)) {
        // A set event with the epoch unchanged is stale (a WakeCp that raced the end of the
        // previous sleep): wait on, the timer keeps the deadline.
        do {
            result = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        } while (result == WAIT_OBJECT_0 &&
                 cp_wake_epoch.load(std::memory_order_seq_cst) == seen_epoch);
    } else {
        result = ::WaitForSingleObject(handles[0],
                                       static_cast<DWORD>((cp_idle_timeout_ns + 999'999) / 1'000'000));
    }
    notified = result == WAIT_OBJECT_0;
    if (notified && timer.handle) {
        ::CancelWaitableTimer(timer.handle);
    }
#else
    {
        std::unique_lock lk{cp_idle_mutex};
        notified = cp_idle_cv.wait_for(lk, std::chrono::nanoseconds{cp_idle_timeout_ns}, [&] {
            return cp_wake_epoch.load(std::memory_order_seq_cst) != seen_epoch;
        });
    }
#endif
    cp_sleeping.store(false, std::memory_order_relaxed);
    if (SotcStats::kFpsLog) {
        const u64 now = SotcStats::NowNs();
        SotcStats::CpAdd(SotcStats::cp_sleeps, 1);
        SotcStats::CpAdd(SotcStats::cp_sleep_ns, now - start);
        if (notified) {
            const u64 notify = cp_notify_ns.load(std::memory_order_relaxed);
            SotcStats::CpAdd(SotcStats::cp_wakes, 1);
            SotcStats::CpAdd(SotcStats::cp_wake_latency_ns,
                             notify >= start && now > notify ? now - notify : 0);
        } else {
            SotcStats::CpAdd(SotcStats::cp_sleep_timeouts, 1);
        }
    }
    return notified;
}

void Liverpool::WakeCp() noexcept {
    if (SotcStats::kFpsLog) {
        cp_notify_ns.store(SotcStats::NowNs(), std::memory_order_relaxed);
    }
#ifdef _WIN32
    ::SetEvent(static_cast<HANDLE>(cp_wake_event));
#else
    { std::scoped_lock lk{cp_idle_mutex}; }
    cp_idle_cv.notify_one();
#endif
}

void Liverpool::WriteFenceValue(void* address, u64 data, u32 num_bytes) {
    // The value is written to the backing memory directly (the GPU may target pages the guest
    // mapped read-only), which the memory tracker cannot see. Invalidate first so a copy of the
    // page already uploaded to the GPU is not reused with the old value, and so GPU-written data
    // sharing the page is read back before the new value lands on top of it.
    if (rasterizer) {
        rasterizer->InvalidateMemory(std::bit_cast<VAddr>(address), num_bytes, true);
    }
    auto* memory = Core::Memory::Instance();
    ASSERT(memory->TryWriteBacking(address, &data, num_bytes, 2));
}

void Liverpool::ProcessCommands() {
    // sotc: with the draw thread, the commands run there between two queued operations (guest
    // threads waiting for a readback do not wait for the queue), and the ordered ones are queued
    // here, at this point of the command stream.
    if (back_threaded) {
        if (std::this_thread::get_id() == back_id) {
            // sotc: SOTC_RB_BATCH: the readbacks these commands start share one submit.
            const VideoCore::ReadbackBatchScope readback_batch;
            while (num_commands) {
                Common::UniqueFunction<void> callback{};
                {
                    std::scoped_lock lk{submit_mutex};
                    callback = std::move(command_queue.front());
                    command_queue.pop();
                    --num_commands;
                }
                callback();
            }
            NotifyCpWait(); // sotc: SOTC_CP_IDLE_US: a game thread's command may write memory
            return;
        }
        while (num_ordered) {
            Common::UniqueFunction<void> callback{};
            {
                std::scoped_lock lk{submit_mutex};
                callback = std::move(ordered_queue.front());
                ordered_queue.pop();
                --num_ordered;
            }
            QueueBackFunc(std::move(callback), back_strict);
        }
        return;
    }
    // Process incoming commands with high priority
    const VideoCore::ReadbackBatchScope readback_batch; // sotc: SOTC_RB_BATCH
    while (num_commands) {
        Common::UniqueFunction<void> callback{};
        {
            std::scoped_lock lk{submit_mutex};
            callback = std::move(command_queue.front());
            command_queue.pop();
            --num_commands;
        }
        callback();
    }
}

void Liverpool::Process(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuCommandProcessor");
    // sotc: SOTC_FPS thread accounting. Without the draw thread the back ops run here, so its
    // waits count as the draw thread's (DrawWait) while its CPU time stays the command processor's.
    SotcStats::RegisterThreadRole(SotcStats::RoleCp);
    if (!back_threaded) {
        SotcStats::tl_role = SotcStats::RoleDraw;
    }
    SotcRaiseGpuThread();
    BbThreads::PlaceThread(BbThreads::Role::CommandProcessor);
    gpu_id = std::this_thread::get_id();
#ifdef __linux__
    gpu_tid = gettid();
#endif

    while (!stoken.stop_requested()) {
        {
            const SotcStats::FrameTimer idle_timer{SotcStats::FrameIdle};
            std::unique_lock lk{submit_mutex};
            Common::CondvarWait(submit_cv, lk, stoken,
                                [this] {
                                    return (num_commands && !back_threaded) || num_ordered ||
                                           num_submits || submit_done;
                                });
        }
        if (stoken.stop_requested()) {
            break;
        }

        VideoCore::StartCapture();

        curr_qid = -1;
        // sotc: SOTC_CP_YIELD (default 1). Every YIELD sits in a wait loop (WaitRegMem,
        // MemSemaphore, Rewind, the CE/DE counters, or a child buffer's wait), so a resumed task
        // that is not done is waiting. When a whole round over the mapped queues finishes no task
        // and runs no command, the thread gives up its time slice instead of spinning: with few
        // cores the spin took a core from the game's threads and the draw thread. With a core to
        // spare, yield returns at once.
        static const bool cp_yield = [] {
            const char* v = std::getenv("SOTC_CP_YIELD");
            return !v || std::atoi(v) != 0;
        }();
        u32 since_progress = 0;

        // sotc: no-progress tracking, for the SOTC_FPS spin accounting and SOTC_CP_IDLE_US.
        // A resume made progress when it finished its task, left a wait loop (cp_wait_exits) or
        // was the first of its task; any other resume only re-tested the wait it yielded in. A
        // streak is the time from the first resume without progress to the next progress. The
        // clock is read once per progress resume (parse time) and every 32 resumes of a streak
        // (spin time, idle deadline); the resumes of a streak are not timed one by one.
        const bool track = SotcStats::kFpsLog || cp_idle_ns != 0;
        std::array<void*, NumTotalQueues> last_resumed{}; // the task each queue resumed last
        u32 idle_iters = 0;      // iterations of the round in progress, 0: a new round starts
        u32 round_kinds = 0;     // CpWaitKind bits seen in the round in progress
        u64 round_epoch = 0;     // cp_wake_epoch when the round in progress started
        bool spinning = false;   // in a streak
        bool idle_due = false;   // the streak passed SOTC_CP_IDLE_US: sleep at the round's end
        u32 streak_resumes = 0;
        u64 mark_ns = 0;         // last clock reading (resume start, or the streak's last sample)
        u64 streak_start_ns = 0; // start of the streak, or of the spin window after a wake
        u64 pending_rounds = 0;  // SOTC_FPS rounds not yet added to the counters
        std::array<u64, SotcStats::CpSpinKinds> pending_kinds{};
        const auto publish_rounds = [&] {
            if (pending_rounds) {
                SotcStats::CpAdd(SotcStats::cp_spin_rounds, pending_rounds);
                for (int k = 0; k < SotcStats::CpSpinKinds; ++k) {
                    if (pending_kinds[k]) {
                        SotcStats::CpAdd(SotcStats::cp_spin_kind_rounds[k], pending_kinds[k]);
                        pending_kinds[k] = 0;
                    }
                }
                pending_rounds = 0;
            }
        };
        const auto end_streak = [&] {
            if (spinning && SotcStats::kFpsLog) {
                publish_rounds();
            }
            spinning = false;
            idle_due = false;
            idle_iters = 0;
        };

        while (num_submits || (num_commands && !back_threaded) || num_ordered) {
            if (num_commands && !back_threaded) {
                since_progress = 0;
            }
            if (track && ((num_commands && !back_threaded) || num_ordered)) {
                end_streak(); // the commands are progress (and may be what a task waits for)
            }
            ProcessCommands();
            // sotc: SOTC_CP_AHEAD=2: a mark pushed while every task waits (one may wait on a
            // guest write that comes after the guest's own frame-lag wait) is served here.
            // Two compares unless a mark arrived or a task finished.
            if (frame_tickets) {
                ServiceFrameMarks();
            }

            if (track) {
                if (idle_iters >= num_mapped_queues) {
                    // A whole round over the mapped queues without progress.
                    if (SotcStats::kFpsLog && round_kinds) {
                        ++pending_rounds;
                        for (int k = 0; k < SotcStats::CpSpinKinds; ++k) {
                            pending_kinds[k] += (round_kinds >> k) & 1;
                        }
                    }
                    if (idle_due) {
                        // sotc: SOTC_CP_IDLE_US. Every queue was resumed after round_epoch was
                        // read, so a notification since then makes CpIdleSleep return at once.
                        if (SotcStats::kFpsLog) {
                            const u64 now = SotcStats::NowNs();
                            SotcStats::CpAdd(SotcStats::cp_spin_ns, now - mark_ns);
                            publish_rounds();
                        }
                        const bool notified = CpIdleSleep(round_epoch);
                        mark_ns = SotcStats::NowNs();
                        streak_resumes = 0;
                        since_progress = 0;
                        // After a notification the full spin window runs again (the condition
                        // probably just changed); after a timeout one round re-tests the waits
                        // the guest CPU writes, then the thread sleeps again.
                        idle_due = !notified;
                        if (notified) {
                            streak_start_ns = mark_ns;
                        }
                    }
                    idle_iters = 0;
                }
                if (idle_iters == 0) {
                    round_kinds = 0;
                    if (cp_idle_ns) {
                        round_epoch = cp_wake_epoch.load(std::memory_order_seq_cst);
                    }
                }
                ++idle_iters;
            }

            curr_qid = (curr_qid + 1) % num_mapped_queues;
            ++since_progress;

            auto& queue = mapped_queues[curr_qid];

            // sotc: skip the lock entirely for a queue that almost certainly has nothing queued
            // (see GpuQueue::has_work) instead of taking m_access on every queue every pass.
            if (!queue.has_work.load(std::memory_order_acquire)) {
                continue;
            }

            Task::Handle task{};
            {
                std::scoped_lock lock{queue.m_access};
                if (queue.submits.empty()) {
                    continue;
                }
                task = queue.submits.front();
            }
            // sotc: SOTC_FPS parse time: outside a streak the resume is timed from its start;
            // the first progress after a streak is timed from the streak's last sample (at most
            // 31 re-tests earlier), so cp_parse_ns no longer contains the spin.
            if (SotcStats::kFpsLog && !spinning) {
                mark_ns = SotcStats::NowNs();
            }
            const u64 exits_before = cp_wait_exits;
            task.resume();
            if (track) {
                void* const resumed = task.done() ? nullptr : task.address();
                const bool progress = !resumed || cp_wait_exits != exits_before ||
                                      resumed != last_resumed[curr_qid];
                last_resumed[curr_qid] = resumed;
                if (progress) {
                    if (SotcStats::kFpsLog) {
                        const u64 parse_ns = SotcStats::NowNs() - mark_ns;
                        SotcStats::cp_parse_ns.fetch_add(parse_ns, std::memory_order_relaxed);
                        SotcStats::cp_parse_all_ns.fetch_add(parse_ns, std::memory_order_relaxed);
                    }
                    end_streak();
                } else {
                    round_kinds |= 1u << (cp_wait_kind < SotcStats::CpSpinKinds
                                              ? cp_wait_kind
                                              : SotcStats::CpSpinKinds - 1);
                    if (!spinning) {
                        spinning = true;
                        streak_resumes = 0;
                        if (!SotcStats::kFpsLog) {
                            mark_ns = SotcStats::NowNs();
                        }
                        streak_start_ns = mark_ns; // this resume's start (or now)
                    } else if ((++streak_resumes & 31) == 0) {
                        const u64 now = SotcStats::NowNs();
                        if (SotcStats::kFpsLog) {
                            SotcStats::CpAdd(SotcStats::cp_spin_ns, now - mark_ns);
                            publish_rounds();
                        }
                        mark_ns = now;
                        if (cp_idle_ns && now - streak_start_ns >= cp_idle_ns) {
                            idle_due = true;
                        }
                    }
                }
            }

            if (!task.done()) {
                if (cp_yield && since_progress >= num_mapped_queues) {
                    since_progress = 0;
                    std::this_thread::yield();
                }
            } else {
                since_progress = 0;
                task.destroy();

                {
                    std::scoped_lock lock{queue.m_access};
                    queue.submits.pop();
                    // sotc: SOTC_ASC_RPTR=2: the ring copy the task ran from.
                    if (!queue.owned.empty()) {
                        queue.owned.pop();
                    }
                    queue.has_work.store(!queue.submits.empty(), std::memory_order_release);

                    --num_submits;
                    std::scoped_lock lock2{submit_mutex};
                    submit_cv.notify_all();
                }
                // sotc: SOTC_CP_AHEAD=2: a finished task may complete a frame's submits.
                if (frame_tickets) {
                    ++q_completed[curr_qid];
                    ++q_done_epoch;
                    ServiceFrameMarks();
                }
            }
        }
        if (track) {
            end_streak();
        }

        // sotc: SOTC_CP_AHEAD=2: the frame ends (OnSubmit, Flush, ticket) are queued by
        // ServiceFrameMarks when their submits are parsed, not here once the CP is idle: the guest
        // may already have submitted the next frame, and the CP may not become idle between two
        // frames. submit_done is cleared before the marks are read, so a SubmitDone arriving
        // after the read sets it again and the CP comes back here (no lost frame end).
        if (frame_tickets) {
            if (submit_done.exchange(false)) {
                VideoCore::EndCapture();
            }
            ServiceFrameMarks();
        } else if (submit_done) {
            VideoCore::EndCapture();
            if (rasterizer) {
                BackOrRun(
                    [this] {
                        rasterizer->OnSubmit();
                        rasterizer->Flush();
                    },
                    false);
            }
            submit_done = false;
        }

        BackOrRun([] { Platform::IrqC::Instance()->Signal(Platform::InterruptId::GpuIdle); },
                  false);
    }
}

// sotc: the frames of finished nested command buffer tasks (an indirect buffer each: ~700 a
// second in the compute queues) were never freed, ~60 MB a minute (PR #219). Freed at once, as
// that PR did, the game showed speckled, rainbow-coloured textures on the Shrine's floor and
// walls and then crashed in its heap (0x49500), with that change alone on (user, 22 Oct): the
// draw thread still ran work queued from them. A frame is freed once the draw thread has run
// everything queued until it finished (back_tail >= the back_head then); the user still saw a
// huge black polygon over the Shrine with that (vertex garbage, the GTX 1080 streamer's "vertex
// explosion"): something else keeps using freed memory, and the leak hid it (frames were never
// reused). Off until that is found, as before #219: SOTC_CP_FREE_TASKS=1 frees them.
static bool FreeNestedTasks() {
    static const bool free_tasks = [] {
        const char* v = std::getenv("SOTC_CP_FREE_TASKS");
        return v && v[0] == '1';
    }();
    return free_tasks;
}

static bool TaskGuard() {
    static const bool guard = [] {
        const char* v = std::getenv("SOTC_TASK_GUARD");
        const bool on = v && v[0] == '1';
        if (on) {
            std::fprintf(stderr, "SOTC TASK GUARD: freed task frames become inaccessible\n");
        }
        return on;
    }();
    return guard;
}

void* Liverpool::Task::promise_type::operator new(std::size_t size) {
#ifdef _WIN32
    if (TaskGuard()) {
        void* frame = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!frame) {
            throw std::bad_alloc{};
        }
        return frame;
    }
#endif
    return ::operator new(size);
}

void Liverpool::Task::promise_type::operator delete(void* frame, std::size_t size) {
#ifdef _WIN32
    if (TaskGuard()) {
        // The pages are decommitted (inaccessible, memory given back) but the address range stays
        // reserved for good: no later frame, nor anything else, ever gets the same address
        // (telling a stale read from a cache keyed by a reused address).
        VirtualFree(frame, size, MEM_DECOMMIT);
        return;
    }
#endif
    ::operator delete(frame);
}

void Liverpool::FreeTaskFrame(std::coroutine_handle<> frame) {
    if (!FreeNestedTasks()) {
        return;
    }
    // SOTC_CP_FREE_NOW=1: at once, as PR #219 did (reproducing its garbage for tests)
    static const bool free_now = std::getenv("SOTC_CP_FREE_NOW") != nullptr;
    if (!back_threaded || free_now) {
        frame.destroy();
        return;
    }
    finished_frames.emplace_back(frame, back_head.load(std::memory_order_acquire));
    const u64 tail = back_tail.load(std::memory_order_acquire);
    size_t done = 0;
    while (done < finished_frames.size() && finished_frames[done].second <= tail) {
        finished_frames[done++].first.destroy();
    }
    finished_frames.erase(finished_frames.begin(), finished_frames.begin() + done);
}

Liverpool::Task Liverpool::ProcessCeUpdate(std::span<const u32> ccb) {
    FIBER_ENTER(ccb_task_name);

    while (!ccb.empty()) {
        ProcessCommands();

        const auto* header = reinterpret_cast<const PM4Header*>(ccb.data());
        const u32 type = header->type;
        if (type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", type);
        }

        const PM4ItOpcode opcode = header->type3.opcode;
        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            // const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::WriteConstRam: {
            const auto* write_const = reinterpret_cast<const PM4WriteConstRam*>(header);
            memcpy(cblock.constants_heap.data() + write_const->Offset(), &write_const->data,
                   write_const->Size());
            break;
        }
        case PM4ItOpcode::DumpConstRam: {
            const auto* dump_const = reinterpret_cast<const PM4DumpConstRam*>(header);
            DrainBack(); // sotc: queued draws may still read the old constants
            memcpy(dump_const->Address<void*>(),
                   cblock.constants_heap.data() + dump_const->Offset(), dump_const->Size());
            Core::MemoryManager::NoteHostWrite(dump_const->Address<VAddr>(), dump_const->Size(), 5);
            break;
        }
        case PM4ItOpcode::IncrementCeCounter: {
            ++cblock.ce_count;
            break;
        }
        case PM4ItOpcode::WaitOnDeCounterDiff: {
            const auto diff = it_body[0];
            while ((cblock.de_count - cblock.ce_count) >= diff) {
                cp_wait_kind = CpWaitDeCounter; // sotc: SOTC_FPS CP spin accounting
                YIELD_CE();
            }
            ++cp_wait_exits; // sotc: progress marker (Process)
            break;
        }
        case PM4ItOpcode::IndirectBufferConst: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task =
                ProcessCeUpdate({indirect_buffer->Address<const u32>(), indirect_buffer->ib_size});
            RESUME_CE(task);

            while (!task.handle.done()) {
                YIELD_CE();
                RESUME_CE(task);
            }
            // sotc: the nested task's frame is not freed by Task (FreeTaskFrame).
            FreeTaskFrame(task.handle);
            break;
        }
        default:
            const u32 count = header->type3.NumWords();
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), count);
        }
        ccb = NextPacket(ccb, header->type3.NumWords() + 1);
    }

    FIBER_EXIT;
}

Liverpool::Task Liverpool::ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb) {
    FIBER_ENTER(dcb_task_name);

    cblock.Reset();

    // sotc: SOTC_CULL_EXPRESS: the first op of a top-level gfx submit starts an express segment
    // (indirect buffers re-enter this function: only the outermost call counts).
    if (xp_on && xp_gfx_depth++ == 0) {
        xp_seg_start = back_head.load(std::memory_order_relaxed);
    }

    // TODO: potentially, ASCs also can depend on CE and in this case the
    // CE task should be moved into more global scope
    Task ce_task{};

    if (!ccb.empty()) {
        // In case of CCB provided kick off CE asap to have the constant heap ready to use
        ce_task = ProcessCeUpdate(ccb);
        RESUME_GFX(ce_task);
    }

    const auto base_addr = reinterpret_cast<uintptr_t>(dcb.data());
    while (!dcb.empty()) {
        func_kind_hint = 3; // sotc: SOTC_FPS accounting (ordered commands queued below)
        ProcessCommands();

        const auto* header = reinterpret_cast<const PM4Header*>(dcb.data());
        const u32 type = header->type;

        switch (type) {
        default:
            UNREACHABLE_MSG("Wrong PM4 type {}", type);
            break;
        case 0:
            UNREACHABLE_MSG("Unimplemented PM4 type 0, base reg: {}, size: {}",
                            header->type0.base.Value(), header->type0.NumWords());
            break;
        case 2:
            // Type-2 packet are used for padding purposes
            dcb = NextPacket(dcb, 1);
            continue;
        case 3:
            const u32 count = header->type3.NumWords();
            const PM4ItOpcode opcode = header->type3.opcode;
            // sotc: SOTC_FPS accounting kind of the ops this packet queues, and SOTC_WD_FUSE:
            // held-back WRITE_DATA packets go out before any packet that is not transparent.
            func_kind_hint = opcode == PM4ItOpcode::DmaData        ? 2
                             : opcode == PM4ItOpcode::WriteData    ? 0
                             : (opcode == PM4ItOpcode::EventWriteEop ||
                                opcode == PM4ItOpcode::EventWriteEos)
                                 ? 1
                                 : 3;
            if (wd_pending.num_segs && !FuseTransparent(header, opcode)) {
                FlushWd();
            }
            switch (opcode) {
            case PM4ItOpcode::Nop: {
                const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
                if (nop->header.count.Value() == 0) {
                    break;
                }

                switch (nop->data_block[0]) {
                case PM4CmdNop::PayloadType::PatchedFlip: {
                    // There is no evidence that GPU CP drives flip events by parsing
                    // special NOP packets. For convenience lets assume that it does.
                    // sotc: SOTC_CP_FLIP_PACE: the wait for a free presentation frame happens
                    // here, before the flip is queued, instead of on the draw thread.
                    if (auto* pacer = Vulkan::Presenter::cp_pacer; pacer && back_threaded) {
                        pacer->PaceFlip();
                        XpSetHint(XpFlip, 0, 0);
                        BackOrRun(
                            [pacer] {
                                pacer->FlipReached();
                                Platform::IrqC::Instance()->Signal(
                                    Platform::InterruptId::GfxFlip);
                            },
                            false);
                        break;
                    }
                    XpSetHint(XpFlip, 0, 0);
                    BackOrRun(
                        [] { Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip); },
                        false);
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPush: {
                    if (guest_markers_enabled && !back_threaded) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        rasterizer->ScopeMarkerBegin(label, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugColorMarkerPush: {
                    if (guest_markers_enabled && !back_threaded) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        const u32 color = *reinterpret_cast<const u32*>(
                            reinterpret_cast<const u8*>(&nop->data_block[1]) + marker_sz);
                        rasterizer->ScopedMarkerInsertColor(label, color, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPop: {
                    if (guest_markers_enabled && !back_threaded) {
                        rasterizer->ScopeMarkerEnd(true);
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::ContextControl: {
                break;
            }
            case PM4ItOpcode::ClearState: {
                regs.SetDefaults();
                MarkRegs(0, Regs::NumRegs);
                break;
            }
            case PM4ItOpcode::SetConfigReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ConfigRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);
                std::memcpy(&regs.reg_array[reg_addr], payload, (count - 1) * sizeof(u32));
                MarkRegs(reg_addr, count - 1);
                break;
            }
            case PM4ItOpcode::SetContextReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ContextRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);

                std::memcpy(&regs.reg_array[reg_addr], payload, (count - 1) * sizeof(u32));
                MarkRegs(reg_addr, count - 1);

                // In the case of HW, render target memory has alignment as color block operates on
                // tiles. There is no information of actual resource extents stored in CB context
                // regs, so any deduction of it from slices/pitch will lead to a larger surface
                // created. The same applies to the depth targets. Fortunately, the guest always
                // sends a trailing NOP packet right after the context regs setup, so we can use the
                // heuristic below and extract the hint to determine actual resource dims.

                switch (reg_addr) {
                case ContextRegs::CbColor0Base:
                case ContextRegs::CbColor1Base:
                case ContextRegs::CbColor2Base:
                case ContextRegs::CbColor3Base:
                case ContextRegs::CbColor4Base:
                case ContextRegs::CbColor5Base:
                case ContextRegs::CbColor6Base:
                case ContextRegs::CbColor7Base: {
                    const auto col_buf_id = (reg_addr - ContextRegs::CbColor0Base) /
                                            (ContextRegs::CbColor1Base - ContextRegs::CbColor0Base);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x0e || nop_offset == 0x0d || nop_offset == 0x0b) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    } else {
                        last_cb_extent[col_buf_id].raw = 0;
                    }
                    break;
                }
                case ContextRegs::CbColor0Cmask:
                case ContextRegs::CbColor1Cmask:
                case ContextRegs::CbColor2Cmask:
                case ContextRegs::CbColor3Cmask:
                case ContextRegs::CbColor4Cmask:
                case ContextRegs::CbColor5Cmask:
                case ContextRegs::CbColor6Cmask:
                case ContextRegs::CbColor7Cmask: {
                    const auto col_buf_id =
                        (reg_addr - ContextRegs::CbColor0Cmask) /
                        (ContextRegs::CbColor1Cmask - ContextRegs::CbColor0Cmask);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x04) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    }
                    break;
                }
                case ContextRegs::DbZInfo: {
                    if (header->type3.count == 8) {
                        ASSERT_MSG(payload[20] == 0xc0001000,
                                   "NOP hint is missing in DB setup sequence");
                        last_db_extent.raw = payload[21];
                    } else {
                        last_db_extent.raw = 0;
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::SetShReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto set_size = (count - 1) * sizeof(u32);

                if (set_data->reg_offset >= 0x200 &&
                    set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                    ASSERT(set_size <= sizeof(ComputeProgram));
                    auto* addr = reinterpret_cast<u32*>(&mapped_queues[GfxQueueId].cs_state) +
                                 (set_data->reg_offset - 0x200);
                    std::memcpy(addr, header + 2, set_size);
                } else {
                    std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset],
                                header + 2, set_size);
                    MarkRegs(Regs::ShRegWordOffset + set_data->reg_offset,
                             static_cast<u32>(set_size / 4));
                }
                break;
            }
            case PM4ItOpcode::SetUconfigReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                std::memcpy(&regs.reg_array[Regs::UconfigRegWordOffset + set_data->reg_offset],
                            header + 2, (count - 1) * sizeof(u32));
                MarkRegs(Regs::UconfigRegWordOffset + set_data->reg_offset, count - 1);
                break;
            }
            case PM4ItOpcode::SetPredication: {
                // sotc: decoded for diagnostics only (SOTC_CMD_STATS); the predicate is not applied.
                const auto* pred = reinterpret_cast<const PM4CmdSetPredication*>(header);
                if (SotcStats::kCmdStats) {
                    static std::atomic<u32> pred_logged{0};
                    const auto op = static_cast<u32>(pred->control.op.Value());
                    SotcStats::pred_ops[op < 4 ? op : 4].fetch_add(1, std::memory_order_relaxed);
                    const u32 index = pred_logged.fetch_add(1, std::memory_order_relaxed);
                    if (index < 4) {
                        LOG_INFO(Render,
                                 "SET_PREDICATION #{}: op {}, address {:#x}, hint {}, visible {}, "
                                 "continue {}, raw dwords {:#010x} {:#010x}",
                                 index + 1, op, pred->Address(), pred->control.hint.Value(),
                                 pred->control.draw_visible.Value(),
                                 pred->control.continue_bit.Value(), pred->start_address_lo,
                                 pred->control.raw);
                    }
                }
                // sotc: once; the game sends it every frame.
                static bool reported = false;
                if (!std::exchange(reported, true)) {
                    LOG_WARNING(Render, "Unimplemented IT_SET_PREDICATION (reported once)");
                }
                break;
            }
            case PM4ItOpcode::IndexType: {
                const auto* index_type = reinterpret_cast<const PM4CmdDrawIndexType*>(header);
                regs.index_buffer_type.raw = index_type->raw;
                MarkRegs(&regs.index_buffer_type, sizeof(regs.index_buffer_type));
                break;
            }
            case PM4ItOpcode::DrawIndex2: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndex2*>(header);
                regs.max_index_size = draw_index->max_size;
                regs.index_base_address.base_addr_lo = draw_index->index_base_lo;
                regs.index_base_address.base_addr_hi = draw_index->index_base_hi;
                regs.num_indices = draw_index->index_count;
                regs.draw_initiator = draw_index->draw_initiator;
                MarkRegs(&regs.max_index_size, sizeof(regs.max_index_size));
                MarkRegs(&regs.index_base_address, sizeof(regs.index_base_address));
                MarkRegs(&regs.num_indices, sizeof(regs.num_indices));
                MarkRegs(&regs.draw_initiator, sizeof(regs.draw_initiator));
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker("gfx:{}:DrawIndex2", fmt::make_format_args(cmd_address),
                                        [&] { SubmitDraw(true, 0); });
                break;
            }
            case PM4ItOpcode::DrawIndexOffset2: {
                const auto* draw_index_off =
                    reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
                regs.max_index_size = draw_index_off->max_size;
                regs.num_indices = draw_index_off->index_count;
                regs.draw_initiator = draw_index_off->draw_initiator;
                MarkRegs(&regs.max_index_size, sizeof(regs.max_index_size));
                MarkRegs(&regs.num_indices, sizeof(regs.num_indices));
                MarkRegs(&regs.draw_initiator, sizeof(regs.draw_initiator));
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexOffset2", fmt::make_format_args(cmd_address),
                    [&] { SubmitDraw(true, draw_index_off->index_offset); });
                break;
            }
            case PM4ItOpcode::DrawIndexAuto: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
                regs.num_indices = draw_index->index_count;
                regs.draw_initiator = draw_index->draw_initiator;
                MarkRegs(&regs.num_indices, sizeof(regs.num_indices));
                MarkRegs(&regs.draw_initiator, sizeof(regs.draw_initiator));
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker("gfx:{}:DrawIndexAuto", fmt::make_format_args(cmd_address),
                                        [&] { SubmitDraw(false, 0); });
                break;
            }
            case PM4ItOpcode::DrawIndirect: {
                const auto* draw_indirect = reinterpret_cast<const PM4CmdDrawIndirect*>(header);
                const auto offset = draw_indirect->data_offset;
                const auto stride = sizeof(DrawIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndirect", fmt::make_format_args(cmd_address), [&] {
                        SubmitDrawIndirect(false, indirect_args_addr, offset, stride, 1, 0,
                                           draw_indirect->base_vtx_loc,
                                           draw_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndirectMulti: {
                const auto* draw_indirect =
                    reinterpret_cast<const PM4CmdDrawIndirectMulti*>(header);
                const auto offset = draw_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndirectMulti", fmt::make_format_args(cmd_address), [&] {
                        SubmitDrawIndirect(false, indirect_args_addr, offset,
                                           draw_indirect->stride, draw_indirect->count, 0,
                                           draw_indirect->base_vtx_loc,
                                           draw_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirect: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirect*>(header);
                const auto offset = draw_index_indirect->data_offset;
                const auto stride = sizeof(DrawIndexedIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirect", fmt::make_format_args(cmd_address), [&] {
                        SubmitDrawIndirect(true, indirect_args_addr, offset, stride, 1, 0,
                                           draw_index_indirect->base_vtx_loc,
                                           draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirectMulti", fmt::make_format_args(cmd_address), [&] {
                        SubmitDrawIndirect(
                            true, indirect_args_addr, offset, draw_index_indirect->stride,
                            draw_index_indirect->count, 0, draw_index_indirect->base_vtx_loc,
                            draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectCountMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectCountMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirectCountMulti", fmt::make_format_args(cmd_address), [&] {
                        SubmitDrawIndirect(
                            true, indirect_args_addr, offset, draw_index_indirect->stride,
                            draw_index_indirect->count,
                            draw_index_indirect->count_indirect_enable.Value()
                                ? draw_index_indirect->count_addr
                                : 0,
                            draw_index_indirect->base_vtx_loc, draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DispatchDirect: {
                const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
                auto& cs_program = GetCsRegs();
                cs_program.dim_x = dispatch_direct->dim_x;
                cs_program.dim_y = dispatch_direct->dim_y;
                cs_program.dim_z = dispatch_direct->dim_z;
                cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                    break;
                }
                PrefetchComputeAhead(count + 1 <= dcb.size() ? dcb.subspan(count + 1)
                                                             : std::span<const u32>{});
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker("gfx:{}:DispatchDirect", fmt::make_format_args(cmd_address),
                                        [&] { SubmitDispatch(false, 0, 0, 0); });
                break;
            }
            case PM4ItOpcode::DispatchIndirect: {
                const auto* dispatch_indirect =
                    reinterpret_cast<const PM4CmdDispatchIndirect*>(header);
                auto& cs_program = GetCsRegs();
                const auto offset = dispatch_indirect->data_offset;
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                    break;
                }
                PrefetchComputeAhead(count + 1 <= dcb.size() ? dcb.subspan(count + 1)
                                                             : std::span<const u32>{});
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DispatchIndirect", fmt::make_format_args(cmd_address),
                    [&] { SubmitDispatch(true, indirect_args_addr, offset, size); });
                break;
            }
            case PM4ItOpcode::NumInstances: {
                const auto* num_instances = reinterpret_cast<const PM4CmdDrawNumInstances*>(header);
                regs.num_instances.num_instances = num_instances->num_instances;
                MarkRegs(&regs.num_instances, sizeof(regs.num_instances));
                break;
            }
            case PM4ItOpcode::IndexBase: {
                const auto* index_base = reinterpret_cast<const PM4CmdDrawIndexBase*>(header);
                regs.index_base_address.base_addr_lo = index_base->addr_lo;
                regs.index_base_address.base_addr_hi = index_base->addr_hi;
                MarkRegs(&regs.index_base_address, sizeof(regs.index_base_address));
                break;
            }
            case PM4ItOpcode::IndexBufferSize: {
                const auto* index_size = reinterpret_cast<const PM4CmdDrawIndexBufferSize*>(header);
                regs.num_indices = index_size->num_indices;
                MarkRegs(&regs.num_indices, sizeof(regs.num_indices));
                break;
            }
            case PM4ItOpcode::SetBase: {
                const auto* set_base = reinterpret_cast<const PM4CmdSetBase*>(header);
                ASSERT(set_base->base_index == PM4CmdSetBase::BaseIndex::DrawIndexIndirPatchTable);
                indirect_args_addr = set_base->Address<u64>();
                break;
            }
            case PM4ItOpcode::EventWrite: {
                const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
                LOG_DEBUG(Render, "Encountered EventWrite: event_type = {}, event_index = {}",
                          magic_enum::enum_name(event->event_type.Value()),
                          magic_enum::enum_name(event->event_index.Value()));
                if (event->event_type.Value() == EventType::SoVgtStreamoutFlush) {
                    // TODO: handle proper synchronization, for now signal that update is done
                    // immediately
                    regs.cp_strmout_cntl.offset_update_done = 1;
                    MarkRegs(&regs.cp_strmout_cntl, sizeof(regs.cp_strmout_cntl));
                } else if (event->event_index.Value() == EventIndex::ZpassDone) {
                    if (event->event_type.Value() == EventType::PixelPipeStatDump) {
                        static constexpr u64 OcclusionCounterValidMask = 0x8000000000000000ULL;
                        // SOTC_OCCLUSION_STEP overrides the fake per-dump increment (A/B tests:
                        // 0 = nothing passes the depth test, i.e. every query reads "occluded").
                        static const u64 OcclusionCounterStep = [] {
                            const char* v = std::getenv("SOTC_OCCLUSION_STEP");
                            return v ? std::strtoull(v, nullptr, 0) : 0x2FFFFFFULL;
                        }();
                        u64* results = event->Address<u64*>();
                        g_sotc_zpass_dumps.fetch_add(1, std::memory_order_relaxed);
                        for (s32 i = 0; i < num_counter_pairs; ++i, results += 2) {
                            *results = pixel_counter | OcclusionCounterValidMask;
                        }
                        pixel_counter += OcclusionCounterStep;
                    }
                }
                break;
            }
            case PM4ItOpcode::EventWriteEos: {
                const PM4CmdEventWriteEos eos = *reinterpret_cast<const PM4CmdEventWriteEos*>(header);
                if (eos.command != PM4CmdEventWriteEos::Command::GdsStore) {
                    XpSetHint(XpLabel, reinterpret_cast<VAddr>(eos.Address()), 8);
                }
                BackOrRun([this, eos] {
                    if (rasterizer) {
                        rasterizer->OnFence();
                    }
                    // sotc: SOTC_RB_EXACT rules 5-7 for EOS (RbxTryDefer).
                    u64 rbx_snap = 0;
                    // sotc: publish mode defers an EOS only to keep the per-address FIFO (a
                    // deferred label to the same address is outstanding: writing this one now
                    // would be overwritten by the older value when that one lands); otherwise
                    // rbx_ok false takes the snapshot below.
                    const bool rbx_ok =
                        rbx && rasterizer &&
                        eos.command == PM4CmdEventWriteEos::Command::SignalFence &&
                        (!VideoCore::BufferCache::RbExactPublish() ||
                         RbxAddrOutstanding(reinterpret_cast<VAddr>(eos.Address())));
                    if (rbx_ok &&
                        RbxTryDefer(0, reinterpret_cast<VAddr>(eos.Address()), true,
                                    eos.DataDWord(), rbx_snap, [this, eos] {
                                        return [this, eos] {
                                            eos.SignalFence([this](void* a, u64 d, u32 n) {
                                                WriteFenceValue(a, d, n);
                                            });
                                        };
                                    })) {
                        return;
                    }
                    if (rbx && rasterizer && !rbx_ok) {
                        rbx_snap = rasterizer->GetBufferCache().HotWriteSeq();
                    }
                    if (rbx && rasterizer) {
                        rasterizer->GetBufferCache().SettleOwedUpTo(rbx_snap);
                    }
                    eos.SignalFence([this](void* address, u64 data, u32 num_bytes) {
                        WriteFenceValue(address, data, num_bytes);
                    });
                    if (rbx && rasterizer) {
                        RbxNoteWritten(rbx_snap); // rule 7: early write
                    }
                    if (eos.command == PM4CmdEventWriteEos::Command::GdsStore) {
                        ASSERT(eos.size == 1);
                        if (rasterizer) {
                            rasterizer->Finish();
                            const u32 value = rasterizer->ReadDataFromGds(eos.gds_index);
                            *eos.Address() = value;
                        }
                    }
                }, back_strict);
                if (xp_on) {
                    xp_seg_start = back_head.load(std::memory_order_relaxed); // a label ends a segment
                }
                break;
            }
            case PM4ItOpcode::EventWriteEop: {
                const PM4CmdEventWriteEop eop_packet =
                    *reinterpret_cast<const PM4CmdEventWriteEop*>(header);
                XpSetHint(XpLabel, reinterpret_cast<VAddr>(eop_packet.Address<void>()),
                          eop_packet.data_sel.Value() == DataSelect::None ? 0 : 8);
                // sotc: SOTC_RB_EXACT=publish: the CP's stamp of this label (latency sums).
                const u64 t_cp = (rbx && VideoCore::BufferCache::RbExactPublish())
                                     ? SotcStats::NowNs()
                                     : 0;
                BackOrRun([this, eop_packet, t_cp] {
                const auto* event_eop = &eop_packet;
                if (rasterizer) {
                    rasterizer->OnFence();
                }
                // sotc: SOTC_RB_EXACT=publish: only a CS_DONE label with 32-bit data after a hot
                // GPU write is deferred; no adaptive policy, the per-address FIFO still applies.
                const bool rbp = rbx && rasterizer && VideoCore::BufferCache::RbExactPublish();
                const u64 t_draw = rbp ? SotcStats::NowNs() : 0;
                bool rbp_cand = false;
                // sotc: SOTC_EOP_TICK=1. The label is a scheduler deferred operation keyed on the
                // tick open now: it runs once the GPU has finished the work recorded up to here.
                // Nothing is flushed per label, so the command buffer keeps its batching. The
                // queue is FIFO, so labels to one address are written in submission order. The
                // draw thread polls the queue (BackLoop, EopService), and EopWaitGuard submits the
                // open buffer when a wait has been blocked on a label for too long.
                // sotc: SOTC_RB_EXACT: only a label that follows a hot GPU write is deferred
                // (QueueDeferredLabel has the details); the others are written below, early.
                u64 rbx_snap = 0;
                bool rbx_defer = false;
                bool rbx_hot = false;
                if (rbp && rasterizer) {
                    // sotc: publish: the decision of RbxDecide without the adaptive policy and
                    // the early-cost ring. rbx_last_seq is updated for every EOP label.
                    rbx_snap = rasterizer->GetBufferCache().HotWriteSeq();
                    rbx_hot = rbx_snap != rbx_last_seq[0];
                    rbx_last_seq[0] = rbx_snap;
                    // sotc: the game's XPR completion label (eboot 0x925c0 -> 0x86eb60): event
                    // 0x28 = BOTTOM_OF_PIPE_TS (not CS_DONE, 0x2f: the first run filtered on that
                    // and deferred nothing), 32-bit data, value 1.
                    rbp_cand = rbx_hot &&
                               event_eop->event_type.Value() ==
                                   static_cast<u32>(EventType::BottomOfPipeTs) &&
                               event_eop->data_sel.Value() == DataSelect::Data32Low &&
                               event_eop->data_lo == 1;
                    const VAddr key = reinterpret_cast<VAddr>(event_eop->Address<void>()) & ~VAddr{7};
                    bool fifo = false;
                    {
                        std::scoped_lock lk{rbx_mutex};
                        fifo = rbx_out.contains(key);
                    }
                    rbx_defer = rbp_cand || fifo;
                } else if (rbx && rasterizer) {
                    rbx_defer = RbxDecide(0, reinterpret_cast<VAddr>(event_eop->Address<void>()),
                                          rbx_snap, &rbx_hot);
                }
                if ((eop_tick || rbx_defer) && rasterizer) {
                    const PM4CmdEventWriteEop eop = *event_eop;
                    const VAddr label = reinterpret_cast<VAddr>(eop.Address<void>());
                    // As in the SOTC_EOP_DEFER path: the GPU copy of the label's page must not
                    // be reused with the old value. Done now, while the page is still the same.
                    if (eop.data_sel.Value() != DataSelect::None) {
                        rasterizer->InvalidateMemory(label, 8, true);
                    }
                    if (rbx_defer && rbx_hot) {
                        rasterizer->GetBufferCache().RecordLabelDownloads();
                    }
                    const auto job = QueueDeferredLabel(
                        0, label, eop.data_sel.Value() == DataSelect::Data32Low,
                        eop.data_sel.Value() == DataSelect::Data64, eop.data_lo, eop.data_hi,
                        rbx_snap, Common::UniqueFunction<void>{[eop] {
                            eop.SignalFence(
                                [](void* address, u64 data, u32 num_bytes) {
                                    ASSERT(Core::Memory::Instance()->TryWriteBacking(
                                        address, &data, num_bytes, 2));
                                },
                                [] {
                                    Platform::IrqC::Instance()->Signal(
                                        Platform::InterruptId::GfxEop);
                                });
                        }},
                        rbp_cand ? t_cp : 0, t_draw);
                    if (rbx_defer && rbx_hot) {
                        // The game spins on this label: submit now, so its latency is the GPU
                        // execution only (not the next periodic flush). A label deferred only by
                        // queue order is submitted by EopService after RbxMaxWaitNs.
                        rasterizer->Flush();
                        if (rbp_cand) {
                            job->t_sub = SotcStats::NowNs(); // sotc: publish: after Flush()
                        }
                    }
                    return;
                }
                // sotc: an end-of-pipe label means "the GPU finished everything before it". It
                // was written as soon as the command processor read it, while the work was still
                // queued for the host GPU: the game then reused or freed memory the GPU was still
                // reading or about to write (heap and descriptor corruption once the texture
                // streaming moves memory around). The label and its interrupt now wait for the
                // host GPU to finish the work recorded so far (the scheduler's priority thread).
                // SOTC_EOP_DEFER=1 only: it halves the frame rate (the game waits for the GPU at
                // every label) and did not stop the streaming corruption (2026-10-12 tests).
                static const bool defer_eop = [] {
                    const char* v = std::getenv("SOTC_EOP_DEFER");
                    return v && v[0] == '1';
                }();
                if (defer_eop && !rbx && rasterizer) {
                    const PM4CmdEventWriteEop eop = *event_eop;
                    const VAddr label = reinterpret_cast<VAddr>(eop.Address<void>());
                    // A GPU copy of the label's page must not be reused with the old value.
                    if (eop.data_sel.Value() != DataSelect::None) {
                        rasterizer->InvalidateMemory(label, 8, true);
                    }
                    rasterizer->GetScheduler().DeferPriorityOperation([this, eop] {
                        eop.SignalFence(
                            [](void* address, u64 data, u32 num_bytes) {
                                ASSERT(Core::Memory::Instance()->TryWriteBacking(address, &data,
                                                                                 num_bytes, 2));
                            },
                            [] {
                                Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop);
                            });
                        NotifyCpWait(); // sotc: SOTC_CP_IDLE_US
                    });
                    // Submit that work now: the game may wait for the label before it sends
                    // anything else, and nothing else would submit it.
                    rasterizer->Flush();
                    return;
                }
                if (rbx && rasterizer) {
                    rasterizer->GetBufferCache().SettleOwedUpTo(rbx_snap);
                }
                event_eop->SignalFence(
                    [this](void* address, u64 data, u32 num_bytes) {
                        WriteFenceValue(address, data, num_bytes);
                    },
                    [] { Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop); });
                if (rbx && rasterizer) {
                    RbxNoteWritten(rbx_snap); // rule 7: early label
                }
                }, back_strict);
                if (xp_on) {
                    // sotc: SOTC_CULL_EXPRESS: a label closes the segment being parsed; the XPR
                    // pattern (a CS_DONE-like EOP with data 1 after a dispatch of this segment)
                    // publishes it.
                    const u64 head = back_head.load(std::memory_order_relaxed);
                    const VAddr label = reinterpret_cast<VAddr>(eop_packet.Address<void>());
                    const auto ev = eop_packet.event_type.Value();
                    const bool pattern =
                        xp_pattern && (ev == static_cast<u32>(EventType::CsDone) ||
                                       ev == static_cast<u32>(EventType::BottomOfPipeTs)) &&
                        eop_packet.data_sel.Value() == DataSelect::Data32Low &&
                        eop_packet.data_lo == 1 && xp_last_dispatch > xp_seg_start;
                    const bool forced = xp_forced_addr != 0 && label == xp_forced_addr;
                    if ((pattern || forced) && head > xp_seg_start) {
                        XpSeg seg;
                        seg.start = xp_seg_start;
                        seg.end = head;
                        seg.label_key = label & ~VAddr{7};
                        seg.queued_ns = SotcStats::NowNs();
                        XpPublishSeg(seg);
                    }
                    xp_seg_start = head;
                }
                break;
            }
            case PM4ItOpcode::DmaData: {
                const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
                if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                    break;
                }
                ASSERT(dma_data->command.das == 0);
                const PM4DmaData dma = *dma_data;
                if (dma.src_sel == DmaDataSrc::Data && dma.dst_sel == DmaDataDst::Gds) {
                    BackOrRun([this, dma] {
                        rasterizer->FillBuffer(dma.dst_addr_lo, dma.NumBytes(), dma.data, true);
                    });
                } else if ((dma.src_sel == DmaDataSrc::Memory ||
                            dma.src_sel == DmaDataSrc::MemoryUsingL2) &&
                           dma.dst_sel == DmaDataDst::Gds) {
                    BackOrRun([this, dma] {
                        rasterizer->CopyBuffer(dma.dst_addr_lo, dma.SrcAddress<VAddr>(),
                                               dma.NumBytes(), true, false);
                    });
                } else if (dma.src_sel == DmaDataSrc::Data &&
                           (dma.dst_sel == DmaDataDst::Memory ||
                            dma.dst_sel == DmaDataDst::MemoryUsingL2)) {
                    XpSetHint(XpDma, dma.DstAddress<VAddr>(), dma.NumBytes());
                    BackOrRun([this, dma] {
                        rasterizer->FillBuffer(dma.DstAddress<VAddr>(), dma.NumBytes(), dma.data,
                                               false);
                    });
                } else if (dma.src_sel == DmaDataSrc::Gds &&
                           (dma.dst_sel == DmaDataDst::Memory ||
                            dma.dst_sel == DmaDataDst::MemoryUsingL2)) {
                    BackOrRun([this, dma] {
                        rasterizer->CopyBuffer(dma.DstAddress<VAddr>(), dma.src_addr_lo,
                                               dma.NumBytes(), false, true);
                    });
                } else if ((dma.src_sel == DmaDataSrc::Memory ||
                            dma.src_sel == DmaDataSrc::MemoryUsingL2) &&
                           (dma.dst_sel == DmaDataDst::Memory ||
                            dma.dst_sel == DmaDataDst::MemoryUsingL2)) {
                    XpSetHint(XpDma, dma.DstAddress<VAddr>(), dma.NumBytes(),
                              dma.SrcAddress<VAddr>(), dma.NumBytes());
                    BackOrRun([this, dma] {
                        rasterizer->CopyBuffer(dma.DstAddress<VAddr>(), dma.SrcAddress<VAddr>(),
                                               dma.NumBytes(), false, false);
                    });
                } else {
                    UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}", u32(dma_data->src_sel),
                                    u32(dma_data->dst_sel));
                }
                break;
            }
            case PM4ItOpcode::WriteData: {
                const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
                ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
                const u32 data_size = (header->type3.count.Value() - 2) * 4;
                u64* address = write_data->Address<u64*>();
                if (!write_data->wr_one_addr.Value()) {
                    // sotc: SOTC_WD_FUSE: held back for the next indirect draw (false: queued).
                    if (back_threaded && FuseWriteData(std::bit_cast<VAddr>(address),
                                                       write_data->data, data_size)) {
                        break;
                    }
                    std::vector<u32> data(write_data->data, write_data->data + data_size / 4);
                    XpSetHint(XpWriteData, std::bit_cast<VAddr>(address), data_size);
                    BackOrRun([this, address, data = std::move(data), data_size] {
                        if (rasterizer) {
                            rasterizer->OnFence();
                        }
                        // sotc: SOTC_RB_EXACT: written now, never deferred (a later draw may read these
                        // bytes through the buffer cache); owed data it announces is settled first.
                        u64 rbx_snap = 0;
                        if (rbx && rasterizer) {
                            rbx_snap = rasterizer->GetBufferCache().HotWriteSeq();
                            rasterizer->GetBufferCache().SettleOwedUpTo(rbx_snap);
                        }
                        std::memcpy(address, data.data(), data_size);
                        Core::MemoryManager::NoteHostWrite(std::bit_cast<VAddr>(address),
                                                           data_size, 4);
                        if (rbx && rasterizer) { // sotc: SOTC_RB_EXACT rule 7
                            RbxNoteWritten(rbx_snap);
                        }
                    });
                } else {
                    UNREACHABLE();
                }
                break;
            }
            case PM4ItOpcode::CopyData: {
                const auto* copy_data = reinterpret_cast<const PM4CmdCopyData*>(header);
                LOG_WARNING(Render,
                            "unhandled IT_COPY_DATA src_sel = {}, dst_sel = {}, "
                            "count_sel = {}, wr_confirm = {}, engine_sel = {}",
                            u32(copy_data->src_sel.Value()), u32(copy_data->dst_sel.Value()),
                            copy_data->count_sel.Value(), copy_data->wr_confirm.Value(),
                            u32(copy_data->engine_sel.Value()));
                break;
            }
            case PM4ItOpcode::MemSemaphore: {
                const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
                if (mem_semaphore->IsSignaling()) {
                    mem_semaphore->Signal();
                } else {
                    while (!mem_semaphore->Signaled()) {
                        cp_wait_kind = CpWaitOther; // sotc: SOTC_FPS CP spin accounting
                        YIELD_GFX();
                    }
                    ++cp_wait_exits; // sotc: progress marker (Process)
                    mem_semaphore->Decrement();
                }
                break;
            }
            case PM4ItOpcode::AcquireMem: {
                // const auto* acquire_mem = reinterpret_cast<PM4CmdAcquireMem*>(header);
                break;
            }
            case PM4ItOpcode::Rewind: {
                if (!rasterizer) {
                    break;
                }
                const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
                while (!rewind->Valid()) {
                    cp_wait_kind = CpWaitOther; // sotc: SOTC_FPS CP spin accounting
                    YIELD_GFX();
                }
                ++cp_wait_exits; // sotc: progress marker (Process)
                break;
            }
            case PM4ItOpcode::WaitRegMem: {
                const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
                // ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
                // Optimization: VO label waits are special because the emulator
                // will write to the label when presentation is finished. So if
                // there are no other submits to yield to we can sleep the thread
                // instead and allow other tasks to run.
                const u64* wait_addr = wait_reg_mem->Address<u64*>();
                if (vo_port->IsVoLabel(wait_addr) &&
                    num_submits == mapped_queues[GfxQueueId].submits.size()) {
                    vo_port->WaitVoLabel([&] { return wait_reg_mem->Test(regs.reg_array); });
                    ++cp_wait_exits; // sotc: progress marker (Process)
                    break;
                }
                while (!wait_reg_mem->Test(regs.reg_array)) {
                    // sotc: SOTC_EOP_TICK: a label queued for the GPU satisfies the stream's own
                    // wait at once (the host queue keeps the order).
                    if (eop_poll && EopShadowSatisfies(*wait_reg_mem)) {
                        break;
                    }
                    // sotc: SOTC_EOP_TICK: the waited-for label may be queued behind work that
                    // is not submitted yet; EopWaitGuard submits it after 2 ms.
                    if (eop_poll) {
                        EopWaitGuard();
                    }
                    if (SotcStats::kFpsLog) { // sotc: SOTC_FPS CP spin accounting
                        cp_wait_kind = MemWaitKind(*wait_reg_mem, vo_port->IsVoLabel(wait_addr));
                    }
                    YIELD_GFX();
                }
                ++cp_wait_exits; // sotc: progress marker (Process)
                break;
            }
            case PM4ItOpcode::IndirectBuffer: {
                const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
                auto task = ProcessGraphics(
                    {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, {});
                RESUME_GFX(task);

                while (!task.handle.done()) {
                    YIELD_GFX();
                    RESUME_GFX(task);
                }
                // sotc: free the nested task's frame (Task does not; it was leaked per IB).
                FreeTaskFrame(task.handle);
                break;
            }
            case PM4ItOpcode::IncrementDeCounter: {
                ++cblock.de_count;
                break;
            }
            case PM4ItOpcode::WaitOnCeCounter: {
                while (cblock.ce_count <= cblock.de_count && !ce_task.handle.done()) {
                    RESUME_GFX(ce_task);
                }
                break;
            }
            case PM4ItOpcode::PfpSyncMe: {
                break;
            }
            case PM4ItOpcode::StrmoutBufferUpdate: {
                const auto* strmout = reinterpret_cast<const PM4CmdStrmoutBufferUpdate*>(header);
                LOG_WARNING(Render_Vulkan,
                            "Unimplemented IT_STRMOUT_BUFFER_UPDATE, update_memory = {}, "
                            "source_select = {}, buffer_select = {}",
                            strmout->update_memory.Value(),
                            magic_enum::enum_name(strmout->source_select.Value()),
                            strmout->buffer_select.Value());
                break;
            }
            case PM4ItOpcode::GetLodStats: {
                // sotc: the texture LOD statistics Shadow of the Colossus streams its fine mips
                // from (sotc_lod_stats.h). Packet (from the game's Gnm getLodStats, 0x86a880):
                // size - 32, address >> 6, address bits 38-39 | 0xbfc00. The game reads 256 u64
                // bank entries at +0x40 and again at +0x840 (it alternates between the two).
                // On again (2026-10-13): the guest memory corruption with streaming came from the
                // GPU readback on unmap (Rasterizer::UnmapMemory), not from these statistics.
                // SOTC_LOD_STATS=0 leaves the buffer untouched (SOTC_LOD_LEVEL picks the mip).
                static const bool lod_stats = [] {
                    const char* v = std::getenv("SOTC_LOD_STATS");
                    return !v || v[0] != '0';
                }();
                const u32* d = reinterpret_cast<const u32*>(header) + 1;
                if (lod_stats && count >= 3) {
                    const VAddr base = (VAddr{d[1]} << 6) | (VAddr{d[2] & 3u} << 38);
                    const u32 size = d[0] + 0x20;
                    BackOrRun([base, size] {
                    u64 entries[256];
                    SotcLod::Collect(entries);
                    for (u32 half = 0; 0x40 + (half + 1) * sizeof(entries) <= size; ++half) {
                        runtime_memory_write_backing(base + 0x40 + half * sizeof(entries), entries,
                                                     sizeof(entries));
                    }
                    // SOTC_LOD_TRACE=<packets> (any other value: 600) between trace lines.
                    static const u64 lod_trace = [] {
                        const char* v = std::getenv("SOTC_LOD_TRACE");
                        const u64 n = v ? std::strtoull(v, nullptr, 0) : 0;
                        return v ? (n > 1 ? n : 600) : 0;
                    }();
                    static u64 packets = 0;
                    if (lod_trace && packets++ % lod_trace == 0) {
                        u32 used = 0;
                        for (const u64 e : entries) {
                            used += (e >> 56) != 0xf;
                        }
                        LOG_WARNING(Render_Vulkan,
                                    "sotc GET_LOD_STATS {:#x}+{:#x}: {} banks sampled, page pool "
                                    "{}% free",
                                    base, size, used, SotcLod::PoolFreePercent());
                    }
                    }, false);
                }
                break;
            }
            case PM4ItOpcode::CondExec: {
                const auto* cond_exec = reinterpret_cast<const PM4CmdCondExec*>(header);
                if (cond_exec->command.Value() != 0) {
                    LOG_WARNING(Render, "IT_COND_EXEC used a reserved command");
                }
                // sotc: the predicate may be written by a queued fence (EOP/EOS/ReleaseMem write
                // from the CPU side, not counted by WaitBackWrites) or by a dispatch not recorded
                // yet (its output not protected yet): the draw thread must have run them. Rare.
                DrainBack();
                const auto skip = *cond_exec->Address() == false;
                if (skip) {
                    SotcStats::Drop(4, reinterpret_cast<uintptr_t>(cond_exec->Address()),
                                    cond_exec->exec_count.Value());
                    dcb = NextPacket(dcb,
                                     header->type3.NumWords() + 1 + cond_exec->exec_count.Value());
                    continue;
                }
                break;
            }
            default:
                UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                                static_cast<u32>(opcode), count);
            }
            dcb = NextPacket(dcb, header->type3.NumWords() + 1);
            break;
        }
    }

    FlushWd(); // sotc: SOTC_WD_FUSE: nothing stays held back past the command buffer

    if (ce_task.handle) {
        while (!ce_task.handle.done()) {
            RESUME_GFX(ce_task);
        }
        ce_task.handle.destroy();
    }

    if (xp_on) {
        --xp_gfx_depth; // sotc: SOTC_CULL_EXPRESS
    }

    FIBER_EXIT;
}

template <bool is_indirect>
Liverpool::Task Liverpool::ProcessCompute(std::span<const u32> acb, u32 vqid) {
    FIBER_ENTER(acb_task_name[vqid]);
    auto& queue = asc_queues[{vqid}];

    struct IndirectPatch {
        const PM4Header* header;
        VAddr indirect_addr;
    };
    boost::container::small_vector<IndirectPatch, 4> indirect_patches;

    auto base_addr = reinterpret_cast<VAddr>(acb.data());
    size_t acb_size = acb.size_bytes();
    while (!acb.empty()) {
        func_kind_hint = 3; // sotc: SOTC_FPS accounting
        ProcessCommands();

        auto* header = reinterpret_cast<const PM4Header*>(acb.data());
        // Type-2 padding is a single dword; its count bits are meaningless. Sizing it as type-3
        // makes padding at the end of a submission look split, which corrupts the next packet.
        u32 next_dw_off = header->type == 2 ? 1 : header->type3.NumWords() + 1;

        // If we have a buffered packet, use it.
        if (queue.tmp_dwords > 0) [[unlikely]] {
            header = reinterpret_cast<const PM4Header*>(queue.tmp_packet.data());
            next_dw_off = header->type3.NumWords() + 1 - queue.tmp_dwords;
            std::memcpy(queue.tmp_packet.data() + queue.tmp_dwords, acb.data(),
                        next_dw_off * sizeof(u32));
            queue.tmp_dwords = 0;
        }

        // If the packet is split across ring boundary, buffer until next submission
        if (next_dw_off > acb.size()) [[unlikely]] {
            std::memcpy(queue.tmp_packet.data(), acb.data(), acb.size_bytes());
            queue.tmp_dwords = acb.size();
            if constexpr (!is_indirect) {
                if (asc_rptr_mode != 2) { // sotc: SOTC_ASC_RPTR=2 moved it at the doorbell
                    *queue.read_addr += acb.size();
                    *queue.read_addr %= queue.ring_size_dw;
                    NoteAscRptr(vqid, queue);
                }
            }
            break;
        }

        if (header->type == 2) {
            // Type-2 packet are used for padding purposes
            next_dw_off = 1;
            acb = NextPacket(acb, next_dw_off);
            if constexpr (!is_indirect) {
                if (asc_rptr_mode != 2) { // sotc: SOTC_ASC_RPTR=2 moved it at the doorbell
                    *queue.read_addr += next_dw_off;
                    *queue.read_addr %= queue.ring_size_dw;
                    NoteAscRptr(vqid, queue);
                }
            }
            continue;
        }
        // sotc: SOTC_ASC_RPTR=1: the read pointer already moved past this packet (an indirect
        // buffer, at its fetch).
        [[maybe_unused]] bool rptr_moved = false;

        if (header->type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", header->type.Value());
        }

        const PM4ItOpcode opcode = header->type3.opcode;

        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::IndirectBuffer: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task = ProcessCompute<true>(
                {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, vqid);
            // sotc: SOTC_ASC_RPTR=1. The hardware read pointer counts fetched ring dwords: an
            // indirect buffer packet is consumed when the CP jumps into the buffer, not when the
            // buffer ends. The packet is fully decoded into the nested task above and its ring
            // dwords are not read again (the packets after it lie between the read and the
            // write pointer, which the guest does not overwrite), so the slot can be handed back
            // now. Without it the slot stays taken until the whole buffer is parsed, including
            // its waits.
            if constexpr (!is_indirect) {
                if (asc_rptr_mode == 1) {
                    *queue.read_addr += next_dw_off;
                    *queue.read_addr %= queue.ring_size_dw;
                    rptr_moved = true;
                    NoteAscRptr(vqid, queue);
                }
            }
            RESUME_ASC(task, vqid);

            while (!task.handle.done()) {
                YIELD_ASC(vqid);
                RESUME_ASC(task, vqid);
            }
            // sotc: free the nested task's frame. Task does not, and SotC's compute queues run
            // an indirect buffer per dispatch batch: ~1 KiB leaked hundreds of times a second.
            FreeTaskFrame(task.handle);
            break;
        }
        case PM4ItOpcode::DmaData: {
            const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
            if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                break;
            }
            ASSERT(dma_data->command.das == 0);
            const PM4DmaData dma = *dma_data;
            if (dma.src_sel == DmaDataSrc::Data && dma.dst_sel == DmaDataDst::Gds) {
                BackOrRun([this, dma] {
                    rasterizer->FillBuffer(dma.dst_addr_lo, dma.NumBytes(), dma.data, true);
                });
            } else if ((dma.src_sel == DmaDataSrc::Memory ||
                        dma.src_sel == DmaDataSrc::MemoryUsingL2) &&
                       dma.dst_sel == DmaDataDst::Gds) {
                BackOrRun([this, dma] {
                    rasterizer->CopyBuffer(dma.dst_addr_lo, dma.SrcAddress<VAddr>(),
                                           dma.NumBytes(), true, false);
                });
            } else if (dma.src_sel == DmaDataSrc::Data &&
                       (dma.dst_sel == DmaDataDst::Memory ||
                        dma.dst_sel == DmaDataDst::MemoryUsingL2)) {
                BackOrRun([this, dma] {
                    rasterizer->FillBuffer(dma.DstAddress<VAddr>(), dma.NumBytes(), dma.data,
                                           false);
                });
            } else if (dma.src_sel == DmaDataSrc::Gds &&
                       (dma.dst_sel == DmaDataDst::Memory ||
                        dma.dst_sel == DmaDataDst::MemoryUsingL2)) {
                BackOrRun([this, dma] {
                    rasterizer->CopyBuffer(dma.DstAddress<VAddr>(), dma.src_addr_lo,
                                           dma.NumBytes(), false, true);
                });
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                const u32 num_bytes = dma_data->NumBytes();
                const VAddr src_addr = dma_data->SrcAddress<VAddr>();
                const VAddr dst_addr = dma_data->DstAddress<VAddr>();
                const PM4Header* header =
                    reinterpret_cast<const PM4Header*>(dst_addr - sizeof(PM4Header));
                if (dst_addr >= base_addr && dst_addr < base_addr + acb_size &&
                    num_bytes == sizeof(PM4CmdDispatchIndirect::GroupDimensions) &&
                    header->type == 3 && header->type3.opcode == PM4ItOpcode::DispatchDirect) {
                    indirect_patches.emplace_back(header, src_addr);
                } else {
                    BackOrRun([this, dst_addr, src_addr, num_bytes] {
                        rasterizer->CopyBuffer(dst_addr, src_addr, num_bytes, false, false);
                    });
                }
            } else {
                UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}", u32(dma_data->src_sel),
                                u32(dma_data->dst_sel));
            }
            break;
        }
        case PM4ItOpcode::AcquireMem: {
            break;
        }
        case PM4ItOpcode::Rewind: {
            if (!rasterizer) {
                break;
            }
            const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
            while (!rewind->Valid()) {
                cp_wait_kind = CpWaitOther; // sotc: SOTC_FPS CP spin accounting
                YIELD_ASC(vqid);
            }
            ++cp_wait_exits; // sotc: progress marker (Process)
            break;
        }
        case PM4ItOpcode::SetShReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            const auto set_size = (header->type3.NumWords() - 1) * sizeof(u32);

            if (set_data->reg_offset >= 0x200 &&
                set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                ASSERT(set_size <= sizeof(ComputeProgram));
                auto* addr = reinterpret_cast<u32*>(&mapped_queues[vqid + 1].cs_state) +
                             (set_data->reg_offset - 0x200);
                std::memcpy(addr, header + 2, set_size);
            } else {
                std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset],
                            header + 2, set_size);
                MarkRegs(Regs::ShRegWordOffset + set_data->reg_offset,
                         static_cast<u32>(set_size / 4));
            }
            break;
        }
        case PM4ItOpcode::SetQueueReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetQueueReg*>(header);
            LOG_WARNING(Render, "Encountered compute SetQueueReg: vqid = {}, reg_offset = {:#x}",
                        set_data->vqid.Value(), set_data->reg_offset.Value());
            break;
        }
        case PM4ItOpcode::DispatchDirect: {
            const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
            if (auto it = std::ranges::find(indirect_patches, header, &IndirectPatch::header);
                it != indirect_patches.end()) {
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                SubmitDispatch(true, it->indirect_addr, 0, size);
                break;
            }
            auto& cs_program = GetCsRegs();
            cs_program.dim_x = dispatch_direct->dim_x;
            cs_program.dim_y = dispatch_direct->dim_y;
            cs_program.dim_z = dispatch_direct->dim_z;
            cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                break;
            }
            PrefetchComputeAhead(next_dw_off <= acb.size() ? acb.subspan(next_dw_off)
                                                           : std::span<const u32>{});
            const auto cmd_address = reinterpret_cast<const void*>(header);
            rasterizer->ScopeMarker("asc[{}]:{}:DispatchDirect",
                                    fmt::make_format_args(vqid, cmd_address),
                                    [&] { SubmitDispatch(false, 0, 0, 0); });
            break;
        }
        case PM4ItOpcode::DispatchIndirect: {
            const auto* dispatch_indirect =
                reinterpret_cast<const PM4CmdDispatchIndirectMec*>(header);
            auto& cs_program = GetCsRegs();
            const auto ib_address = dispatch_indirect->Address<VAddr>();
            const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                break;
            }
            PrefetchComputeAhead(next_dw_off <= acb.size() ? acb.subspan(next_dw_off)
                                                           : std::span<const u32>{});
            const auto cmd_address = reinterpret_cast<const void*>(header);
            rasterizer->ScopeMarker("asc[{}]:{}:DispatchIndirect",
                                    fmt::make_format_args(vqid, cmd_address),
                                    [&] { SubmitDispatch(true, ib_address, 0, size); });
            break;
        }
        case PM4ItOpcode::WriteData: {
            const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
            ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
            const u32 data_size = (header->type3.count.Value() - 2) * 4;
            if (!write_data->wr_one_addr.Value()) {
                void* address = write_data->Address<void*>();
                std::vector<u32> data(write_data->data, write_data->data + data_size / 4);
                BackOrRun([this, address, data = std::move(data), data_size] {
                    if (rasterizer) {
                        rasterizer->OnFence();
                    }
                    // sotc: SOTC_RB_EXACT: as the graphics WRITE_DATA, never deferred.
                    u64 rbx_snap = 0;
                    if (rbx && rasterizer) {
                        rbx_snap = rasterizer->GetBufferCache().HotWriteSeq();
                        rasterizer->GetBufferCache().SettleOwedUpTo(rbx_snap);
                    }
                    std::memcpy(address, data.data(), data_size);
                    Core::MemoryManager::NoteHostWrite(reinterpret_cast<VAddr>(address), data_size,
                                                       4);
                    if (rbx && rasterizer) { // sotc: SOTC_RB_EXACT rule 7
                        RbxNoteWritten(rbx_snap);
                    }
                });
            } else {
                UNREACHABLE();
            }
            break;
        }
        case PM4ItOpcode::MemSemaphore: {
            const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
            if (mem_semaphore->IsSignaling()) {
                mem_semaphore->Signal();
            } else {
                while (!mem_semaphore->Signaled()) {
                    cp_wait_kind = CpWaitOther; // sotc: SOTC_FPS CP spin accounting
                    YIELD_ASC(vqid);
                }
                ++cp_wait_exits; // sotc: progress marker (Process)
                mem_semaphore->Decrement();
            }
            break;
        }
        case PM4ItOpcode::WaitRegMem: {
            const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
            ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
            while (!wait_reg_mem->Test(regs.reg_array)) {
                // sotc: SOTC_RB_EXACT: as in the graphics wait, a deferred label's pending value
                // satisfies the stream's own wait, and a wait blocked on a label queued behind
                // unsubmitted work makes the draw thread submit it (EopWaitGuard).
                if (eop_poll) {
                    if (EopShadowSatisfies(*wait_reg_mem)) {
                        break;
                    }
                    EopWaitGuard();
                }
                if (SotcStats::kFpsLog) { // sotc: SOTC_FPS CP spin accounting
                    cp_wait_kind = MemWaitKind(
                        *wait_reg_mem, vo_port && vo_port->IsVoLabel(wait_reg_mem->Address<u64*>()));
                }
                YIELD_ASC(vqid);
            }
            ++cp_wait_exits; // sotc: progress marker (Process)
            break;
        }
        case PM4ItOpcode::ReleaseMem: {
            const PM4CmdReleaseMem release = *reinterpret_cast<const PM4CmdReleaseMem*>(header);
            if (release.data_sel.Value() != DataSelect::GdsMemStore) {
                XpSetHint(XpLabel, release.Address<VAddr>(), 8);
            }
            BackOrRun([this, release, pipe_id = queue.pipe_id] {
                if (rasterizer) {
                    rasterizer->OnFence();
                }
                // sotc: SOTC_RB_EXACT rules 5-7 for the compute fence (QueueDeferredLabel).
                // A GDS store is a GPU copy, not a label.
                u64 rbx_snap = 0;
                bool rbx_defer = false;
                bool rbx_hot = false;
                const bool rbx_label = rbx && rasterizer &&
                                       release.data_sel.Value() != DataSelect::GdsMemStore;
                if (rbx_label && VideoCore::BufferCache::RbExactPublish() &&
                    !RbxAddrOutstanding(release.Address<VAddr>())) {
                    // sotc: publish mode: a compute fence is not deferred (snapshot only), unless
                    // a deferred label to the same address is outstanding (per-address FIFO, via
                    // RbxDecide below).
                    rbx_snap = rasterizer->GetBufferCache().HotWriteSeq();
                } else if (rbx_label) {
                    rbx_defer = RbxDecide(1, release.Address<VAddr>(), rbx_snap, &rbx_hot);
                }
                if (rbx_defer) {
                    const auto ds = release.data_sel.Value();
                    if (rbx_hot) {
                        rasterizer->GetBufferCache().RecordLabelDownloads();
                    }
                    // Same page invalidation the early write path never needed (the write goes
                    // straight to the guest mapping), so none here either.
                    QueueDeferredLabel(
                        1, release.Address<VAddr>(), ds == DataSelect::Data32Low,
                        ds == DataSelect::Data64, release.data_lo, release.data_hi, rbx_snap,
                        Common::UniqueFunction<void>{[release, pipe_id] {
                            release.SignalFence(
                                [pipe_id] {
                                    Platform::IrqC::Instance()->Signal(
                                        static_cast<Platform::InterruptId>(pipe_id));
                                },
                                [](VAddr, u16, u16) {});
                        }});
                    if (rbx_hot) {
                        rasterizer->Flush(); // the guest may spin on this label (see EOP)
                    }
                    return;
                }
                if (rbx_label) {
                    rasterizer->GetBufferCache().SettleOwedUpTo(rbx_snap);
                }
                release.SignalFence(
                    [pipe_id] {
                        Platform::IrqC::Instance()->Signal(
                            static_cast<Platform::InterruptId>(pipe_id));
                    },
                    [this](VAddr dst, u16 gds_index, u16 num_dwords) {
                        rasterizer->CopyBuffer(dst, gds_index, num_dwords * sizeof(u32), false,
                                               true);
                    });
                if (rbx_label) {
                    RbxNoteWritten(rbx_snap); // rule 7: early label
                }
            }, back_strict);
            break;
        }
        case PM4ItOpcode::EventWrite: {
            // const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
            break;
        }
        default:
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), header->type3.NumWords());
        }

        acb = NextPacket(acb, next_dw_off);

        if constexpr (!is_indirect) {
            if (!rptr_moved && asc_rptr_mode != 2) { // sotc: SOTC_ASC_RPTR
                *queue.read_addr += next_dw_off;
                *queue.read_addr %= queue.ring_size_dw;
                NoteAscRptr(vqid, queue);
            }
        }
    }

    FIBER_EXIT;
}

Liverpool::CmdBuffer Liverpool::CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];
    ASSERT_MSG(queue.dcb_buffer.capacity() >= queue.dcb_buffer_offset + dcb.size(),
               "dcb copy buffer out of reserved space");
    ASSERT_MSG(queue.ccb_buffer.capacity() >= queue.ccb_buffer_offset + ccb.size(),
               "ccb copy buffer out of reserved space");

    queue.dcb_buffer.resize(
        std::max(queue.dcb_buffer.size(), queue.dcb_buffer_offset + dcb.size()));
    queue.ccb_buffer.resize(
        std::max(queue.ccb_buffer.size(), queue.ccb_buffer_offset + ccb.size()));

    const u32 prev_dcb_buffer_offset = queue.dcb_buffer_offset;
    const u32 prev_ccb_buffer_offset = queue.ccb_buffer_offset;
    if (!dcb.empty()) {
        std::memcpy(queue.dcb_buffer.data() + queue.dcb_buffer_offset, dcb.data(),
                    dcb.size_bytes());
        queue.dcb_buffer_offset += dcb.size();
        dcb = std::span<const u32>{queue.dcb_buffer.begin() + prev_dcb_buffer_offset,
                                   queue.dcb_buffer.begin() + queue.dcb_buffer_offset};
    }

    if (!ccb.empty()) {
        std::memcpy(queue.ccb_buffer.data() + queue.ccb_buffer_offset, ccb.data(),
                    ccb.size_bytes());
        queue.ccb_buffer_offset += ccb.size();
        ccb = std::span<const u32>{queue.ccb_buffer.begin() + prev_ccb_buffer_offset,
                                   queue.ccb_buffer.begin() + queue.ccb_buffer_offset};
    }

    return std::make_pair(dcb, ccb);
}

void Liverpool::SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];

    if (EmulatorSettings.IsCopyGpuBuffers()) {
        std::tie(dcb, ccb) = CopyCmdBuffers(dcb, ccb);
    }

    auto task = ProcessGraphics(dcb, ccb);
    {
        std::scoped_lock lock{queue.m_access};
        queue.submits.emplace(task.handle);
        queue.has_work.store(true, std::memory_order_release);
    }

    {
        std::scoped_lock lk{submit_mutex};
        ++num_submits;
        ++q_enqueued[GfxQueueId]; // sotc: SOTC_CP_AHEAD=2 frame marks
        submit_cv.notify_one();
    }
    NotifyCpWait(); // sotc: SOTC_CP_IDLE_US: a new task to run while the others wait
}

void Liverpool::SubmitAsc(u32 gnm_vqid, std::span<const u32> acb) {
    ASSERT_MSG(gnm_vqid > 0 && gnm_vqid < NumTotalQueues, "Invalid virtual ASC queue index");
    auto& queue = mapped_queues[gnm_vqid];

    const auto vqid = gnm_vqid - 1;
    // sotc: SOTC_ASC_RPTR=2: the task runs from a copy of the ring dwords, so sceGnmDingDong can
    // hand the ring space back to the guest at once (the copy lives until the task finishes).
    std::vector<u32> ring_copy;
    if (asc_rptr_mode == 2) {
        ring_copy.assign(acb.begin(), acb.end());
        acb = std::span<const u32>{ring_copy.data(), ring_copy.size()};
    }
    const auto& task = ProcessCompute(acb, vqid);
    {
        std::scoped_lock lock{queue.m_access};
        queue.submits.emplace(task.handle);
        if (asc_rptr_mode == 2) {
            queue.owned.push(std::move(ring_copy)); // moving keeps the dwords where acb points
        }
        queue.has_work.store(true, std::memory_order_release);
    }

    {
        std::scoped_lock lk{submit_mutex};
        num_mapped_queues = std::max(num_mapped_queues, gnm_vqid + 1);
        ++num_submits;
        ++q_enqueued[gnm_vqid]; // sotc: SOTC_CP_AHEAD=2 frame marks
        submit_cv.notify_one();
    }
    NotifyCpWait(); // sotc: SOTC_CP_IDLE_US
}

// sotc: SOTC_CP_AHEAD. Read once; gnmdriver.cpp reads the same value through this.
int Liverpool::CpAheadMode() noexcept {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_CP_AHEAD");
        const int n = v ? std::atoi(v) : 0;
        return n == 0 ? 0 : (n == 2 ? 2 : 1);
    }();
    return mode;
}

// sotc: SOTC_ASC_RPTR (0, 1 or 2; anything else is 0).
int Liverpool::AscRptrMode() noexcept {
    static const int mode = [] {
        const char* v = std::getenv("SOTC_ASC_RPTR");
        const int n = v ? std::atoi(v) : 0;
        return (n == 1 || n == 2) ? n : 0;
    }();
    return mode;
}

// sotc: SOTC_CP_AHEAD=2. Called by SubmitDone under submit_mutex: every submit of the frame
// went through SubmitGfx / SubmitAsc before (gnmdriver serializes them with m_submit_lock).
void Liverpool::PushFrameMark() {
    CpFrameMark mark;
    mark.frame = frames_marked.load(std::memory_order_relaxed) + 1;
    mark.enqueued = q_enqueued;
    frame_marks.push_back(mark);
    frames_marked.store(mark.frame, std::memory_order_release);
    frame_mark_epoch.fetch_add(1, std::memory_order_release);
}

// sotc: SOTC_CP_AHEAD=2, command processor thread. A frame is parsed once each queue finished as
// many tasks as it had enqueued at the frame's SubmitDone: all of its draw-thread work is queued
// then. Its end goes after that work, in the order the old idle path used: the per-frame
// housekeeping and flush (OnSubmit, Flush), then the ticket, which the draw thread publishes when
// it reaches it, that is after it ran (and read the guest memory of) every op of the frame.
void Liverpool::ServiceFrameMarks() {
    const u64 epoch = frame_mark_epoch.load(std::memory_order_acquire);
    if (epoch == fm_seen_epoch && q_done_epoch == fm_seen_done) {
        return;
    }
    fm_seen_epoch = epoch;
    fm_seen_done = q_done_epoch;
    for (;;) {
        u64 frame = 0;
        {
            std::scoped_lock lk{submit_mutex};
            if (frame_marks.empty()) {
                return;
            }
            const CpFrameMark& mark = frame_marks.front();
            for (u32 q = 0; q < NumTotalQueues; ++q) {
                if (q_completed[q] < mark.enqueued[q]) {
                    return;
                }
            }
            frame = mark.frame;
            frame_marks.pop_front();
        }
        if (rasterizer) {
            XpSetHint(XpMark, 0, 0);
            BackOrRun(
                [this] {
                    rasterizer->OnSubmit();
                    rasterizer->Flush();
                },
                false);
        }
        XpSetHint(XpMark, 0, 0);
        BackOrRun(
            [this, frame] {
                {
                    std::scoped_lock lk{frames_mutex};
                    frames_recorded.store(frame, std::memory_order_release);
                }
                frames_cv.notify_all();
            },
            false);
    }
}

// sotc: SOTC_CP_AHEAD=2, guest threads (the submit gate).
void Liverpool::WaitFramesRecorded(u64 target) noexcept {
    if (frames_recorded.load(std::memory_order_acquire) >= target) {
        return;
    }
    std::unique_lock lk{frames_mutex};
    frames_cv.wait(lk, [&] { return frames_recorded.load(std::memory_order_acquire) >= target; });
}

// sotc: SOTC_FPS compute ring accounting. Doorbell side (guest thread, sceGnmDingDong).
void Liverpool::NoteAscDoorbell(u32 vqid, u32 next_offs_dw, u32 submitted_dw) noexcept {
    if (!SotcStats::kFpsLog || vqid >= NumComputeRings) {
        return;
    }
    const auto& queue = asc_queues[{vqid}];
    auto& st = asc_ring_stats[vqid];
    const u64 now = SotcStats::NowNs();
    SotcStats::asc_doorbells.fetch_add(1, std::memory_order_relaxed);
    // A full window that the command processor ended just before this doorbell: the game was
    // most likely spinning on the read pointer until then.
    if (const u64 freed = st.freed_ns.exchange(0, std::memory_order_relaxed);
        freed && now - freed < 1'000'000) {
        SotcStats::asc_full_spun.fetch_add(1, std::memory_order_relaxed);
        SotcStats::asc_full_spun_ns.fetch_add(now - st.window_ns.load(std::memory_order_relaxed),
                                              std::memory_order_relaxed);
    }
    const u32 size = queue.ring_size_dw;
    const u32 wptr = next_offs_dw % size;
    st.wptr_dw.store(wptr, std::memory_order_relaxed);
    st.need_dw.store(submitted_dw, std::memory_order_relaxed);
    // The game's own test (eboot 0x85d3d0): free = (rptr - wptr - 1) mod size.
    const u32 rptr = *queue.read_addr % size;
    const u32 free_dw = (rptr + size - wptr - 1) % size;
    if (free_dw < submitted_dw) {
        u64 expected = 0;
        st.full_ns.compare_exchange_strong(expected, now, std::memory_order_relaxed);
    }
}

// sotc: SOTC_FPS compute ring accounting. Read pointer side (command processor, ProcessCompute).
void Liverpool::NoteAscRptr(u32 vqid, const AscQueueInfo& queue) noexcept {
    if (!SotcStats::kFpsLog || vqid >= NumComputeRings) {
        return;
    }
    auto& st = asc_ring_stats[vqid];
    const u64 full = st.full_ns.load(std::memory_order_relaxed);
    if (!full) {
        return;
    }
    const u32 size = queue.ring_size_dw;
    const u32 rptr = *queue.read_addr % size;
    const u32 wptr = st.wptr_dw.load(std::memory_order_relaxed);
    const u32 free_dw = (rptr + size - wptr - 1) % size;
    if (free_dw < st.need_dw.load(std::memory_order_relaxed)) {
        return;
    }
    u64 expected = full;
    if (!st.full_ns.compare_exchange_strong(expected, 0, std::memory_order_relaxed)) {
        return;
    }
    const u64 now = SotcStats::NowNs();
    SotcStats::asc_full_windows.fetch_add(1, std::memory_order_relaxed);
    SotcStats::asc_full_ns.fetch_add(now - full, std::memory_order_relaxed);
    SotcStats::GateStoreMax(SotcStats::asc_full_max_ns, now - full);
    st.window_ns.store(full, std::memory_order_relaxed);
    st.freed_ns.store(now, std::memory_order_relaxed);
}

} // namespace AmdGpu
