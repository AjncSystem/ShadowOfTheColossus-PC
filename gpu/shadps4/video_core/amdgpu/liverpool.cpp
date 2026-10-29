// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <deque>
#include <mutex>
#include <new>
#ifdef _WIN32
#include <windows.h>
#endif
#include <boost/preprocessor/stringize.hpp>
#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

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
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/sotc_lod_stats.h"
#include "video_core/sotc_opstats.h"
#include "video_core/sotc_stats.h"
#include <optional>
#include "bbport_toggles.h"
#include "bbport_threads.h"

// sotc: writes guest memory through the backing view (runtime_memory.c).
extern "C" int runtime_memory_write_backing(unsigned long long address, const void* data,
                                            unsigned long long size);
// sotc: marks this thread as the draw thread for the split of the guest memory lock waits
// (runtime_memory.c / sotc_stats.h).
extern "C" void runtime_memory_mark_draw_thread(void);

// Counted for the SOTC_PROBE diagnostic log (emulator.cpp).
std::atomic<u64> g_sotc_zpass_dumps{0};

namespace AmdGpu {

static const char* dcb_task_name{"DCB_TASK"};
static const char* ccb_task_name{"CCB_TASK"};

#define MAX_NAMES 56
static_assert(Liverpool::NumComputeRings <= MAX_NAMES);
static_assert(BbToggle::MicroCostSweep == (1u << 23), "Liverpool::OnBackThread hard-codes the bit");
static_assert(BbToggle::CpFastPaths == (1u << 20), "Liverpool::PollCommands hard-codes the bit");

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

// sotc: yield of a packet wait loop (WaitRegMem, MemSemaphore, Rewind). `first` is a local bool,
// true until the loop yielded once: the first yield of a wait means this resume reached it, i.e.
// made progress (cp_progress, read by Process for the CpPollBackoff stall detection); the later
// ones only re-checked a condition that is still false.
#define YIELD_WAIT(name, first)                                                                    \
    if (first) {                                                                                   \
        cp_progress = true;                                                                        \
        first = false;                                                                             \
    }                                                                                              \
    YIELD(name)
#define YIELD_WAIT_GFX(first) YIELD_WAIT(dcb_task_name, first)
#define YIELD_WAIT_ASC(id, first) YIELD_WAIT(acb_task_name[id], first)

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

/// sotc: SOTC_HASH_STATS clock: the TSC (a few ns per read, against ~25 ns for the steady clock on
/// Windows), calibrated against the steady clock at report time (HashStatsReport). Off x86-64 it
/// is the steady clock itself (ratio 1).
static inline u64 HashTicks() {
#if defined(__x86_64__) || defined(_M_X64)
    return __rdtsc();
#else
    return SotcStats::NowNs();
#endif
}

Liverpool::Liverpool() : guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    num_counter_pairs = Libraries::Kernel::sceKernelIsNeoMode() ? 16 : 8;
    // sotc: SOTC_CP_THREADS=0 binds and records the draws on the command processor thread.
    const char* threads = std::getenv("SOTC_CP_THREADS");
    back_threaded = !threads || threads[0] != '0';
    const char* strict = std::getenv("SOTC_CP_STRICT");
    back_strict = strict && strict[0] == '1';
    // sotc: A/B knobs of the draw thread ring; the defaults keep the old behaviour (512 entries,
    // 4096 spin iterations). The capacity is rounded down to a power of two so every wrap-around
    // stays a mask.
    if (const char* ring = std::getenv("SOTC_BACK_RING")) {
        u64 capacity = std::strtoull(ring, nullptr, 0);
        capacity = std::max(BackRingMin, std::min(capacity, BackRingMax));
        while (capacity & (capacity - 1)) {
            capacity &= capacity - 1;
        }
        back_ring_capacity = capacity;
        back_ring_mask = capacity - 1;
    }
    if (const char* spin = std::getenv("SOTC_BACK_SPIN")) {
        const u64 iterations = std::strtoull(spin, nullptr, 0);
        back_spin = static_cast<u32>(std::min<u64>(iterations, BackSpinMax));
    }
    if (back_threaded && SotcStats::kHashStats) {
        // sotc: SOTC_HASH_STATS=1 table (value-initialised: counters and `used` start at 0); the
        // last slot is the overflow bucket for keys that do not fit.
        hash_slots = std::make_unique<HashSlot[]>(HashSlots + 1);
        HashSlot& overflow = hash_slots[HashSlots];
        overflow.kind = 0xff;
        overflow.used.store(1, std::memory_order_release);
        hash_tsc0 = HashTicks();
        hash_ns0 = SotcStats::NowNs();
    }
    if (back_threaded) {
        back_shadow.assign(Regs::NumRegs, 0);
        back_ring.resize(back_ring_capacity);
        dirty_list.reserve(NumRegChunks);
        back_thread = std::jthread{std::bind_front(&Liverpool::BackLoop, this)};
        back_id = back_thread.get_id();
    }
    process_thread = std::jthread{std::bind_front(&Liverpool::Process, this)};
}

Liverpool::~Liverpool() {
    process_thread.request_stop();
    process_thread.join();
    if (back_thread.joinable()) {
        back_thread.request_stop();
        back_thread.join();
    }
}

static inline void CpuRelax() {
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#endif
}

// sotc: SOTC_FPS=1, time a command processor wait takes (added to `counter` when it ends).
namespace {
struct CpWaitClock {
    std::atomic<u64>& counter;
    u64 start;
    explicit CpWaitClock(std::atomic<u64>& c) : counter{c}, start{SotcStats::kFpsLog ? SotcStats::NowNs() : 0} {}
    ~CpWaitClock() {
        if (start) {
            counter.fetch_add(SotcStats::NowNs() - start, std::memory_order_relaxed);
        }
    }
};
} // namespace

void Liverpool::BackLoop(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuDraw");
    SotcStats::RegisterThread(0);
    // sotc: MicroCostSweep, see Liverpool::OnBackThread.
    t_on_back_thread = true;
    runtime_memory_mark_draw_thread();
    Shader::g_info_slot = 1;
#ifdef __linux__
    back_tid = gettid();
#endif
    u64 tail = back_tail.load(std::memory_order_relaxed);
    while (!stoken.stop_requested()) {
        if (num_commands.load(std::memory_order_acquire)) {
            // sotc: SOTC_FPS=1, time of the commands guest threads queued for this thread
            // (readbacks, invalidations): part of "draw busy" but of no op in SOTC_HASH_STATS.
            const CpWaitClock cmd_clock{SotcStats::draw_cmds_ns};
            ProcessCommands();
        }
        if (back_head.load(std::memory_order_acquire) == tail) {
            // sotc: draw_idle_ns is only printed with SOTC_FPS=1: without the flag the two clock
            // reads (13-16% of RtlQueryPerformanceCounter in the profiler) and the fetch_add are
            // skipped.
            // sotc: with SOTC_HASH_STATS=1 the same time also goes to hash_idle_ns (own counter,
            // the SOTC_FPS printer resets draw_idle_ns).
            const bool time_idle = SotcStats::kFpsLog || SotcStats::kHashStats;
            const u64 idle_start = time_idle ? SotcStats::NowNs() : 0;
            struct IdleTime {
                u64 start;
                bool active;
                ~IdleTime() {
                    if (active) {
                        const u64 ns = SotcStats::NowNs() - start;
                        if (SotcStats::kFpsLog) {
                            SotcStats::draw_idle_ns.fetch_add(ns, std::memory_order_relaxed);
                        }
                        if (SotcStats::kHashStats) {
                            SotcStats::hash_idle_ns.fetch_add(ns, std::memory_order_relaxed);
                        }
                    }
                }
            } idle_time{idle_start, time_idle};
            bool ready = false;
            for (u32 spin = 0; spin < back_spin; ++spin) {
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
        BackOp& op = back_ring[tail & back_ring_mask];
        if (SotcStats::kHashStats) [[unlikely]] {
            ExecuteBackOpTimed(op); // sotc: SOTC_HASH_STATS=1
        } else {
            ExecuteBackOp(op);
        }
        op.func = {};
        ++tail;
        back_tail.store(tail, std::memory_order_seq_cst);
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
    WaitBackTail(back_head.load(std::memory_order_acquire));
}

void Liverpool::WaitBackTail(u64 head) {
    SotcStats::cp_waits.fetch_add(1, std::memory_order_relaxed);
    const CpWaitClock wait_clock{SotcStats::cp_drain_wait_ns};
    for (u32 spin = 0; back_tail.load(std::memory_order_acquire) < head; ++spin) {
        if (spin < back_spin) {
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
    // sotc: SOTC_FPS=1, only a wait that happens is timed (the clock starts at the first spin).
    std::optional<CpWaitClock> ring_clock;
    for (u32 spin = 0; head - back_tail.load(std::memory_order_acquire) >= back_ring_capacity;
         ++spin) {
        if (!spin) {
            ring_clock.emplace(SotcStats::cp_ring_wait_ns);
        }
        if (spin < back_spin) {
            CpuRelax();
            continue;
        }
        std::unique_lock lk{back_mutex};
        drain_waiting.store(true, std::memory_order_seq_cst);
        drain_cv.wait_for(lk, std::chrono::milliseconds(1), [&] {
            return head - back_tail.load(std::memory_order_seq_cst) < back_ring_capacity;
        });
        drain_waiting.store(false, std::memory_order_relaxed);
    }
    BackOp& op = back_ring[head & back_ring_mask];
    op.pipeline = nullptr;
    op.num_stages = 0;
    // The registers as of this point of the command stream: the chunks changed since the last op.
    // sotc: BbToggle::DrawFixedCostOpt. Per chunk: its index, a 64-bit mask of the dwords that
    // differ from what the draw thread already holds (back_shadow, kept equal to its back_regs:
    // both only ever take the dwords an op carries, in order), then those dwords. A chunk the
    // command stream wrote with the same values is not sent at all. With the toggle set (old
    // path) every dirty chunk goes whole (mask all ones); the shadow follows either way.
    op.reg_chunks.clear();
    op.verify_chunks.clear();
    // sotc: BbToggle::DepthFastPath also needs the changed dwords (the draw thread tells render
    // register changes from per-draw ones by them); the old path needs both bits set.
    const bool whole = BbToggle::Disabled(BbToggle::DrawFixedCostOpt) &&
                       BbToggle::Disabled(BbToggle::DepthFastPath);
    static const bool verify = std::getenv("SOTC_DRAW_OPT_VERIFY") != nullptr;
    for (const u16 chunk : dirty_list) {
        const u32* src = &regs.reg_array[u32{chunk} * RegChunkDwords];
        if (verify) {
            op.verify_chunks.push_back(chunk);
            op.verify_chunks.insert(op.verify_chunks.end(), src, src + RegChunkDwords);
        }
        u32* shadow = &back_shadow[u32{chunk} * RegChunkDwords];
        dirty_chunks[chunk / 64] &= ~(u64{1} << (chunk % 64));
        u64 mask = ~u64{0};
        if (!whole) {
            mask = 0;
            for (u32 i = 0; i < RegChunkDwords; i += 4) {
                const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
                const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(shadow + i));
                const u32 equal =
                    static_cast<u32>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(a, b))));
                mask |= u64{(~equal) & 0xFu} << i;
            }
            if (mask == 0) {
                continue;
            }
        }
        op.reg_chunks.push_back(chunk);
        op.reg_chunks.push_back(static_cast<u32>(mask));
        op.reg_chunks.push_back(static_cast<u32>(mask >> 32));
        if (mask == ~u64{0}) {
            op.reg_chunks.insert(op.reg_chunks.end(), src, src + RegChunkDwords);
        } else {
            for (u64 m = mask; m != 0; m &= m - 1) {
                op.reg_chunks.push_back(src[std::countr_zero(m)]);
            }
        }
        std::memcpy(shadow, src, RegChunkDwords * sizeof(u32));
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
    if (back_sleeping.load(std::memory_order_seq_cst)) {
        std::scoped_lock lk{back_mutex};
        back_cv.notify_one();
    }
}

void Liverpool::QueueBackFunc(Common::UniqueFunction<void>&& func, bool writes_memory) {
    BackOp& op = BackAlloc();
    op.kind = BackOp::Kind::Func;
    op.func = std::move(func);
    BackPublish(writes_memory);
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
        // sotc: BbToggle::DrawReplay: did this op's user data or flat buffer change since the last
        // op captured with this shader? The draw thread keeps a generation by it (ExecuteBackOp).
        // Computed whatever the bit says, so the answer is relative to the previous capture
        // when the bit is flipped in a run.
        Shader::InfoRuntime& wrt = stage.info->runtime[0];
        bool changed = stage.num_user_data != wrt.cap_ud_n;
        if (stage.num_user_data != ~0u) {
            const size_t bytes = stage.num_user_data * sizeof(u32);
            changed |= std::memcmp(wrt.cap_ud.data(), stage.user_data.data(), bytes) != 0;
            std::memcpy(wrt.cap_ud.data(), stage.user_data.data(), bytes);
        }
        wrt.cap_ud_n = stage.num_user_data;
        if (wrt.cap_flat.size() != rt.flattened_ud_buf.size() ||
            std::memcmp(wrt.cap_flat.data(), rt.flattened_ud_buf.data(),
                        rt.flattened_ud_buf.size() * sizeof(u32)) != 0) {
            wrt.cap_flat.assign(rt.flattened_ud_buf.begin(), rt.flattened_ud_buf.end());
            changed = true;
        }
        stage.changed = changed;
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

void Liverpool::ExecuteBackOp(BackOp& op) {
    if (SotcOps::kOn) [[unlikely]] { // sotc: SOTC_OP_STATS=1, op composition
        SotcOps::NoteBackOp(static_cast<u32>(op.kind));
    }
    const u64 phase_t0 = Vulkan::DrawPhase::kOn ? Vulkan::DrawPhase::Now() : 0;
    // sotc: back_chunk_serial: the chunks an op carries changed in value (BackAlloc sends only
    // those, or every dirty one with DrawFixedCostOpt set); see DrawChunksChangedSince.
    const u32 serial = ++back_serial;
    u32 changed_dwords = 0, changed_chunks = 0;
    bool rel_render = !back_rel_ready, rel_dynamic = !back_rel_ready, rel_color = !back_rel_ready;
    u32 partial_old[3][2];
    for (u32 k = 0; k < 3; ++k) {
        for (u32 i = 0; i < rel_partial_n[k]; ++i) {
            partial_old[k][i] = back_regs.reg_array[rel_partial[k][i].dword];
        }
    }
    for (size_t i = 0; i < op.reg_chunks.size();) {
        ++changed_chunks;
        const u32 chunk = op.reg_chunks[i];
        const u64 mask = u64{op.reg_chunks[i + 1]} | (u64{op.reg_chunks[i + 2]} << 32);
        rel_render |= (rel_mask[RelRender][chunk] & mask) != 0; // sotc: DrawRelSerial
        rel_dynamic |= (rel_mask[RelDynamic][chunk] & mask) != 0;
        rel_color |= (rel_mask[RelColor][chunk] & mask) != 0;
        const u32* values = &op.reg_chunks[i + 3];
        u32* dst = &back_regs.reg_array[chunk * RegChunkDwords];
        if (mask == ~u64{0}) {
            std::memcpy(dst, values, RegChunkDwords * sizeof(u32));
            i += 3 + RegChunkDwords;
            changed_dwords += RegChunkDwords;
        } else {
            u32 n = 0;
            for (u64 m = mask; m != 0; m &= m - 1) {
                dst[std::countr_zero(m)] = values[n++];
            }
            i += 3 + n;
            changed_dwords += n;
        }
        back_chunk_serial[chunk] = serial;
    }
    if (!op.verify_chunks.empty()) {
        // SOTC_DRAW_OPT_VERIFY: the draw thread's registers must now hold what the command
        // processor had in every chunk it marked dirty, sent or not.
        static std::atomic<u64> checked{0}, bad{0};
        for (size_t i = 0; i < op.verify_chunks.size(); i += RegChunkDwords + 1) {
            const u32 chunk = op.verify_chunks[i];
            const bool same =
                std::memcmp(&back_regs.reg_array[chunk * RegChunkDwords], &op.verify_chunks[i + 1],
                            RegChunkDwords * sizeof(u32)) == 0;
            const u64 n = checked.fetch_add(1, std::memory_order_relaxed) + 1;
            if (!same && bad.fetch_add(1, std::memory_order_relaxed) < 20) {
                std::fprintf(stderr,
                             "SOTC DRAW OPT VERIFY: register chunk %u differs on the draw thread\n",
                             chunk);
            }
            if ((n & 0xFFFFF) == 0) {
                std::fprintf(stderr, "SOTC DRAW OPT VERIFY: register chunks checked %llu, bad %llu\n",
                             static_cast<unsigned long long>(n),
                             static_cast<unsigned long long>(bad.load()));
            }
        }
    }
    for (u32 i = 0; i < rel_partial_n[RelRender]; ++i) {
        const auto& part = rel_partial[RelRender][i];
        rel_render |= ((partial_old[RelRender][i] ^ back_regs.reg_array[part.dword]) & part.bits) != 0;
    }
    for (u32 i = 0; i < rel_partial_n[RelDynamic]; ++i) {
        const auto& part = rel_partial[RelDynamic][i];
        rel_dynamic |=
            ((partial_old[RelDynamic][i] ^ back_regs.reg_array[part.dword]) & part.bits) != 0;
    }
    for (u32 i = 0; i < rel_partial_n[RelColor]; ++i) {
        const auto& part = rel_partial[RelColor][i];
        rel_color |= ((partial_old[RelColor][i] ^ back_regs.reg_array[part.dword]) & part.bits) != 0;
    }
    rel_render |= back_db_extent.raw != op.db_extent.raw; // sotc: DrawRelSerial
    rel_color |= std::memcmp(back_cb_extent.data(), op.cb_extent.data(),
                             sizeof(back_cb_extent)) != 0;
    back_rel_serial[RelRender] += rel_render;
    back_rel_serial[RelDynamic] += rel_dynamic;
    back_rel_serial[RelColor] += rel_color;
    back_cb_extent = op.cb_extent;
    back_db_extent = op.db_extent;
    const bool replay_prefix = !BbToggle::Disabled(BbToggle::DrawReplay);
    for (u32 i = 0; i < op.num_stages; ++i) {
        BackStage& stage = op.stages[i];
        Shader::InfoRuntime& rt = stage.info->runtime[1];
        rt.user_data = stage.num_user_data == ~0u
                           ? std::span<const u32>{}
                           : std::span<const u32>{stage.user_data.data(), stage.num_user_data};
        if (replay_prefix) {
            // sotc: BbToggle::DrawReplay: the op's flat buffer becomes this copy's (a swap: no
            // bytes are read or written here; the op's vector takes the old ones and the command
            // processor assigns it again before reusing the op). The command processor compared
            // the op's user data and flat buffer with those of the previous op of this shader
            // (CaptureBackOp): ud_gen moves exactly when they differ (see InfoRuntime).
            rt.flattened_ud_buf.swap(stage.flat);
            rt.ud_gen += stage.changed;
            if (stage.changed) {
                // The bytes were written by the command processor's core: ask for the first KiB
                // now, the binds read them a few hundred ns later (a hit never reads them).
                const char* p = reinterpret_cast<const char*>(rt.flattened_ud_buf.data());
                const size_t n = std::min<size_t>(rt.flattened_ud_buf.size() * sizeof(u32), 1024);
                for (size_t off = 0; off < n; off += 64) {
                    __builtin_prefetch(p + off);
                }
            }
        } else {
            rt.flattened_ud_buf.assign(stage.flat.begin(), stage.flat.end());
            ++rt.ud_gen; // the generation is only meaningful with the bit clear: always "changed"
        }
        rt.pgm_base = stage.pgm_base;
    }
    if (Vulkan::DrawPhase::kOn && (op.kind == BackOp::Kind::Draw || op.kind == BackOp::Kind::DrawIndirect)) {
        Vulkan::DrawPhase::AddOpPrefix(Vulkan::DrawPhase::Now() - phase_t0, changed_chunks,
                                       changed_dwords, op.kind == BackOp::Kind::Draw ? 0 : 1);
    }
    switch (op.kind) {
    case BackOp::Kind::Func:
        op.func();
        break;
    case BackOp::Kind::Draw:
        rasterizer->Draw(op.is_indexed, op.index_offset, op.pipeline, true);
        break;
    case BackOp::Kind::DrawIndirect:
        rasterizer->DrawIndirect(op.is_indexed, op.address, op.offset, op.stride, op.max_count,
                                 op.count_address, op.vertex_sgpr_offset, op.instance_sgpr_offset,
                                 op.pipeline, true);
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

Liverpool::HashSlot& Liverpool::HashFind(u32 kind, u64 h1, u64 h2) {
    constexpr u32 MaxProbes = 32;
    u64 mix = (h1 ^ ((h2 << 29) | (h2 >> 35)) ^ (u64{kind} << 56)) * 0x9E3779B97F4A7C15ull;
    mix ^= mix >> 31;
    u32 index = static_cast<u32>((mix * 0xC2B2AE3D27D4EB4Full) >> (64 - HashSlotBits));
    for (u32 probe = 0; probe < MaxProbes; ++probe, index = (index + 1) & (HashSlots - 1)) {
        HashSlot& slot = hash_slots[index];
        if (slot.used.load(std::memory_order_relaxed) == 0) {
            if (hash_used >= HashSlots / 4 * 3) {
                break;
            }
            slot.kind = kind;
            slot.h1 = h1;
            slot.h2 = h2;
            ++hash_used;
            slot.used.store(1, std::memory_order_release); // publishes the key to the reporter
            return slot;
        }
        if (slot.kind == kind && slot.h1 == h1 && slot.h2 == h2) {
            return slot;
        }
    }
    return hash_slots[HashSlots];
}

void Liverpool::ExecuteBackOpTimed(BackOp& op) {
    // Key: kind plus the hashes of the VS (or CS) and PS; other stages are folded into the first.
    u64 h1 = 0;
    u64 h2 = 0;
    for (u32 i = 0; i < op.num_stages; ++i) {
        const Shader::Info& info = *op.stages[i].info;
        switch (info.sw_stage) {
        case Shader::SwStage::Fragment:
            h2 = info.pgm_hash;
            break;
        case Shader::SwStage::Vertex:
        case Shader::SwStage::Compute:
            h1 ^= info.pgm_hash;
            break;
        default:
            h1 = h1 * 31 + info.pgm_hash;
            break;
        }
    }
    HashSlot& slot = HashFind(static_cast<u32>(op.kind), h1, h2);
    // sotc: the draw thread's own counts (SotcStats::hash_tl): the global counters are bumped by
    // the other threads too and reset by the printers.
    const SotcStats::HashThreadCounts before = SotcStats::hash_tl;
    const u64 t0 = HashTicks();
    ExecuteBackOp(op);
    const u64 t1 = HashTicks();
    const SotcStats::HashThreadCounts& after = SotcStats::hash_tl;
    slot.ticks.fetch_add(t1 - t0, std::memory_order_relaxed);
    slot.ops.fetch_add(1, std::memory_order_relaxed);
    slot.protects.fetch_add(after.protects - before.protects, std::memory_order_relaxed);
    slot.readbacks.fetch_add(after.readbacks - before.readbacks, std::memory_order_relaxed);
    slot.finishes.fetch_add(after.finishes - before.finishes, std::memory_order_relaxed);
    slot.upload.fetch_add(after.upload - before.upload, std::memory_order_relaxed);
}

void Liverpool::HashStatsReport(std::string& out, double seconds) {
    char line[256];
    if (!hash_slots) {
        out += "SOTC hash: no table (SOTC_CP_THREADS=0: the draw thread is off)\n";
        return;
    }
    // Ticks to ns over the whole run so far (no sleep needed to calibrate).
    const u64 elapsed_ns = SotcStats::NowNs() - hash_ns0;
    const u64 elapsed_ticks = HashTicks() - hash_tsc0;
    const double ns_per_tick =
        elapsed_ns > 50'000'000 && elapsed_ticks > 0 ? double(elapsed_ns) / double(elapsed_ticks)
                                                     : 1.0;
    struct Entry {
        const HashSlot* slot;
        u64 ticks, ops, protects, readbacks, finishes, upload;
    };
    std::vector<Entry> entries;
    u64 total_ticks = 0;
    u64 total_ops = 0;
    u64 kind_ticks[6]{}, kind_ops[6]{};
    for (u32 i = 0; i <= HashSlots; ++i) {
        HashSlot& slot = hash_slots[i];
        if (slot.used.load(std::memory_order_acquire) == 0) {
            continue;
        }
        Entry e{&slot,
                slot.ticks.exchange(0, std::memory_order_relaxed),
                slot.ops.exchange(0, std::memory_order_relaxed),
                slot.protects.exchange(0, std::memory_order_relaxed),
                slot.readbacks.exchange(0, std::memory_order_relaxed),
                slot.finishes.exchange(0, std::memory_order_relaxed),
                slot.upload.exchange(0, std::memory_order_relaxed)};
        if (e.ops == 0) {
            continue;
        }
        total_ticks += e.ticks;
        total_ops += e.ops;
        {
            // sotc: per op kind totals (printed after the table; 5 = overflow bucket)
            const u32 k = std::min<u32>(slot.kind, 5);
            kind_ticks[k] += e.ticks;
            kind_ops[k] += e.ops;
        }
        entries.push_back(e);
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.ticks > b.ticks; });
    static constexpr const char* kKindNames[] = {"func", "draw", "draw-indirect", "dispatch",
                                                 "dispatch-indirect"};
    const double per_s = 1.0 / std::max(seconds, 1e-3);
    const auto ms = [&](u64 ticks) { return double(ticks) * ns_per_tick / 1e6; };
    for (u32 k = 0; k < 6; ++k) {
        if (kind_ops[k] == 0) {
            continue;
        }
        std::snprintf(line, sizeof(line),
                      "SOTC hash kind total %s: %.1f ms/s, %.0f ops/s (%.2f us/op)\n",
                      k < std::size(kKindNames) ? kKindNames[k] : "overflow",
                      ms(kind_ticks[k]) * per_s, double(kind_ops[k]) * per_s,
                      ms(kind_ticks[k]) * 1000.0 / double(kind_ops[k]));
        out += line;
    }
    const size_t shown = std::min<size_t>(entries.size(), 15);
    u64 shown_ticks = 0;
    for (size_t i = 0; i < shown; ++i) {
        const Entry& e = entries[i];
        shown_ticks += e.ticks;
        const char* kind = e.slot->kind < std::size(kKindNames) ? kKindNames[e.slot->kind]
                                                                : "overflow";
        char hash[48];
        if (e.slot->h1 == 0 && e.slot->h2 == 0) {
            std::snprintf(hash, sizeof(hash), "-");
        } else if (e.slot->h2 == 0) {
            std::snprintf(hash, sizeof(hash), "%016llx",
                          static_cast<unsigned long long>(e.slot->h1));
        } else {
            std::snprintf(hash, sizeof(hash), "%016llx/%016llx",
                          static_cast<unsigned long long>(e.slot->h1),
                          static_cast<unsigned long long>(e.slot->h2));
        }
        std::snprintf(line, sizeof(line),
                      "SOTC hash %s %s : %.2f ms/s, %.0f ops/s, %.1f protects/s, %.1f readbacks/s "
                      "(%.1f finishing), %.2f MB/s uploaded\n",
                      kind, hash, ms(e.ticks) * per_s, double(e.ops) * per_s,
                      double(e.protects) * per_s, double(e.readbacks) * per_s,
                      double(e.finishes) * per_s, double(e.upload) / 1048576.0 * per_s);
        out += line;
    }
    const double idle_ms = double(SotcStats::hash_idle_ns.exchange(0)) / 1e6;
    const double busy_ms = std::max(seconds * 1000.0 - idle_ms, 0.0);
    const double table_ms = ms(total_ticks);
    std::snprintf(line, sizeof(line),
                  "SOTC hash coverage: ops %.0f ms/s of %.0f ms/s draw-thread busy (%.0f%%), top "
                  "%zu of %zu keys %.0f%% of the ops, %.0f ops/s\n",
                  table_ms * per_s, busy_ms * per_s, busy_ms > 0.0 ? 100.0 * table_ms / busy_ms : 0.0,
                  shown, entries.size(),
                  total_ticks ? 100.0 * double(shown_ticks) / double(total_ticks) : 0.0,
                  double(total_ops) * per_s);
    out += line;
}

void Liverpool::SubmitDraw(bool is_indexed, u32 index_offset) {
    if (!back_threaded) {
        rasterizer->Draw(is_indexed, index_offset);
        return;
    }
    WaitBackWrites();
    // sotc: no pipeline for a draw FilterDraw drops on the draw thread (review of #99).
    const u64 pt0 = Vulkan::DrawPhase::kOn ? Vulkan::DrawPhase::Now() : 0;
    const auto* pipeline =
        rasterizer->IsDrawFilteredOut() ? nullptr : rasterizer->LookupGraphicsPipeline(0, 0);
    const u64 pt1 = Vulkan::DrawPhase::kOn ? Vulkan::DrawPhase::Now() : 0;
    BackOp& op = BackAlloc();
    const u64 pt2 = Vulkan::DrawPhase::kOn ? Vulkan::DrawPhase::Now() : 0;
    op.kind = BackOp::Kind::Draw;
    op.is_indexed = is_indexed;
    op.index_offset = index_offset;
    CaptureBackOp(op, pipeline);
    if (Vulkan::DrawPhase::kOn) {
        const u64 pt3 = Vulkan::DrawPhase::Now();
        Vulkan::DrawPhase::AddCp(0, pt1 - pt0);
        Vulkan::DrawPhase::AddCp(1, pt2 - pt1);
        Vulkan::DrawPhase::AddCp(2, pt3 - pt2);
    }
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
    WaitBackWrites();
    const u64 pt0 = Vulkan::DrawPhase::kOn ? Vulkan::DrawPhase::Now() : 0;
    const auto* pipeline = rasterizer->IsDrawFilteredOut()
                               ? nullptr
                               : rasterizer->LookupGraphicsPipeline(vertex_sgpr_offset,
                                                                    instance_sgpr_offset);
    const u64 pt1 = Vulkan::DrawPhase::kOn ? Vulkan::DrawPhase::Now() : 0;
    BackOp& op = BackAlloc();
    const u64 pt2 = Vulkan::DrawPhase::kOn ? Vulkan::DrawPhase::Now() : 0;
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
    if (Vulkan::DrawPhase::kOn) {
        const u64 pt3 = Vulkan::DrawPhase::Now();
        Vulkan::DrawPhase::AddCp(0, pt1 - pt0);
        Vulkan::DrawPhase::AddCp(1, pt2 - pt1);
        Vulkan::DrawPhase::AddCp(2, pt3 - pt2);
    }
    BackPublish(back_strict && PipelineWrites(pipeline));
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
    WaitBackWrites();
    const auto* pipeline = rasterizer->LookupComputePipeline();
    BackOp& op = BackAlloc();
    op.kind = indirect ? BackOp::Kind::DispatchIndirect : BackOp::Kind::Dispatch;
    op.address = address;
    op.offset = offset;
    op.size = size;
    op.cs = GetCsRegs();
    CaptureBackOp(op, pipeline);
    BackPublish(back_strict);
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
    SotcStats::RegisterThread(1);
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
        // sotc: SOTC_CP_YIELD (default 1, upstream 0.60.6-plus): with few cores (<= 6 logical) the
        // spin of a waiting queue took a core from the game's threads and the draw thread, so the
        // backoff below gives up its time slice at once instead of pausing first. With a core to
        // spare yield returns at once anyway.
        static const bool cp_yield_few_cores = [] {
            const char* v = std::getenv("SOTC_CP_YIELD");
            return (!v || std::atoi(v) != 0) && BbThreads::Available() <= 6;
        }();

        // sotc: BbToggle::CpPollBackoff. A packet that waits (WaitRegMem, MemSemaphore, Rewind)
        // yields and is polled again on the next visit, so a queue that waits used to spin this
        // loop at full speed, taking queue.m_access every time. Count the iterations in a row
        // that made no progress (a command ran, a task finished or reached a new wait); once a
        // whole pass over the mapped queues made none, back off once per pass: first a few
        // pauses, then a thread yield, and after 500 us of stall a short wait on submit_cv
        // (woken by a new submit or command; the condition itself is changed by other threads
        // without a notification, so the wait is capped at 100 us, which Windows rounds up to its
        // timer resolution). Packets are polled exactly as before, only less often.
        u32 stalled = 0;
        u64 stall_since_ns = 0;
        const auto progress = [&] {
            stalled = 0;
            stall_since_ns = 0;
        };
        const auto no_progress = [&] {
            if (BbToggle::Disabled(BbToggle::CpPollBackoff)) {
                return;
            }
            const u32 queues = std::max(num_mapped_queues, 1u);
            if (++stalled < queues || stalled % queues != 0) {
                return;
            }
            if (stalled / queues <= 32) {
                if (cp_yield_few_cores) {
                    std::this_thread::yield();
                    return;
                }
                for (u32 i = 0; i < 16; ++i) {
                    CpuRelax();
                }
                return;
            }
            const u64 now_ns = SotcStats::NowNs();
            if (stall_since_ns == 0) {
                stall_since_ns = now_ns;
            }
            if (now_ns - stall_since_ns < 500'000) {
                std::this_thread::yield();
                return;
            }
            const u32 submits = num_submits.load(std::memory_order_relaxed);
            std::unique_lock lk{submit_mutex};
            submit_cv.wait_for(lk, std::chrono::microseconds(100), [&] {
                return num_ordered || (num_commands && !back_threaded) || num_submits != submits;
            });
        };

        while (num_submits || (num_commands && !back_threaded) || num_ordered) {
            const bool had_commands = num_ordered || (num_commands && !back_threaded);
            ProcessCommands();

            curr_qid = (curr_qid + 1) % num_mapped_queues;
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
                    if (had_commands) {
                        progress();
                    } else {
                        no_progress();
                    }
                    continue;
                }
                task = queue.submits.front();
            }
            // sotc: cp_parse_ns is only printed with SOTC_FPS=1: no clock reads nor fetch_add
            // otherwise (the profiler showed them on every task.resume()).
            const u64 parse_start = SotcStats::kFpsLog ? SotcStats::NowNs() : 0;
            cp_progress = false;
            task.resume();
            if (SotcStats::kFpsLog) {
                SotcStats::cp_parse_ns.fetch_add(SotcStats::NowNs() - parse_start,
                                                 std::memory_order_relaxed);
            }

            if (had_commands || cp_progress || task.done()) {
                progress();
            } else {
                no_progress();
            }
            if (task.done()) {
                task.destroy();

                std::scoped_lock lock{queue.m_access};
                queue.submits.pop();
                queue.has_work.store(!queue.submits.empty(), std::memory_order_release);

                --num_submits;
                std::scoped_lock lock2{submit_mutex};
                submit_cv.notify_all();
            }
        }

        if (submit_done) {
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
// second in the compute queues) were never freed: ~45 MB a minute, 2.6 GB an hour at the Shrine
// (PR #219). Freed at once, as that PR did, the game showed speckled, rainbow-coloured textures
// on the Shrine's floor and walls, a huge black polygon and a crash in its heap (0x49500) in the
// 0.60.5 builds. That was blamed on the frames (nothing was reused while they leaked), but the
// causes found since were the speculative hot readbacks (SOTC_RB_HOT, T31: late GPU copies landing
// on guest memory the game had reused) and the bind memo of #232; no code keeps a pointer into a
// frame (they hold spans of guest memory, values and by-value lambda captures; the draw thread
// never sees them). With SOTC_TASK_GUARD=1 SOTC_CP_FREE_NOW=1 (frames on pages of their own that
// are decommitted at once, so any later access faults) the Shrine idle (240 s), the temple route
// and the title demo (10 min) ran with no fault and clean frames. A frame is still freed only
// once the draw thread has run everything queued until it finished (back_tail >= the back_head
// then), which costs nothing. On by default; SOTC_CP_FREE_TASKS=0 keeps the frames (the leak).
static bool FreeNestedTasks() {
    static const bool free_tasks = [] {
        const char* v = std::getenv("SOTC_CP_FREE_TASKS");
        return !v || v[0] != '0';
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
                YIELD_CE();
            }
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

// sotc: BbToggle::CpFastPaths. A type 3 Nop packet that the Nop case of ProcessGraphics passes
// without doing anything: no payload, or a payload that is neither the patched flip nor (when
// markers_matter) a debug marker. Keep in step with that case.
static inline bool IsInertNop(const PM4Header* header, bool markers_matter) {
    if (header->type != 3 || header->type3.opcode != PM4ItOpcode::Nop) {
        return false;
    }
    const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
    if (nop->header.count.Value() == 0) {
        return true;
    }
    switch (nop->data_block[0]) {
    case PM4CmdNop::PayloadType::PatchedFlip:
        return false;
    case PM4CmdNop::PayloadType::DebugMarkerPush:
    case PM4CmdNop::PayloadType::DebugColorMarkerPush:
    case PM4CmdNop::PayloadType::DebugMarkerPop:
        return !markers_matter;
    default:
        return true;
    }
}

// sotc: SOTC_PREFETCH_COMPUTE=0 disables. Scans a command buffer before it runs and queues the translation of
// the compute programs it dispatches on the pipeline workers, so the burst of new shaders that
// follows entering an area translates in parallel instead of one after another on the command
// processor. The dispatch itself still looks its program up as before (and waits for the queued
// job). A program the scan got wrong only adds a permutation; nothing here changes guest state.
void Liverpool::PrefetchComputePrograms(std::span<const u32> cmds, s32 qid,
                                        ComputeProgram* shadow_in, int depth) {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_PREFETCH_COMPUTE");
        return !v || v[0] != '0';
    }();
    if (!enabled || !rasterizer) {
        return;
    }
    // Nested buffers run as their own tasks and enter here again: a range the scan of the
    // submission already walked is skipped. Indirect buffers are followed (SotC dispatches
    // from one per batch), so the whole submission is seen from its first command.
    const auto lo = reinterpret_cast<uintptr_t>(cmds.data());
    const auto hi = lo + cmds.size_bytes();
    if (!shadow_in) {
        for (const auto& [a, b] : prefetch_ranges) {
            if (lo >= a && lo < b) {
                return;
            }
        }
        prefetch_ranges.clear();
    }
    prefetch_ranges.emplace_back(lo, hi);
    ComputeProgram local_shadow;
    if (!shadow_in) {
        local_shadow = mapped_queues[qid].cs_state;
    }
    auto& shadow = shadow_in ? *shadow_in : local_shadow;
    u64 last_addr = ~0ull;
    size_t scanned = 0;
    while (!cmds.empty() && scanned < (1u << 16)) {
        const auto* header = reinterpret_cast<const PM4Header*>(cmds.data());
        if (header->type == 2) {
            cmds = cmds.subspan(1);
            ++scanned;
            continue;
        }
        if (header->type != 3) {
            return;
        }
        const size_t size = header->type3.NumWords() + 1;
        if (size > cmds.size()) {
            return;
        }
        switch (header->type3.opcode) {
        case PM4ItOpcode::SetShReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            const auto set_size = (header->type3.NumWords() - 1) * sizeof(u32);
            if (set_data->reg_offset >= 0x200 &&
                set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4) &&
                set_size <= sizeof(ComputeProgram) -
                                (set_data->reg_offset - 0x200) * sizeof(u32)) {
                std::memcpy(reinterpret_cast<u32*>(&shadow) + (set_data->reg_offset - 0x200),
                            header + 2, set_size);
            }
            break;
        }
        case PM4ItOpcode::IndirectBuffer: {
            const auto* ib = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            const auto* addr = ib->Address<const u32>();
            if (depth < 2 && ib->ib_size != 0 &&
                rasterizer->IsMapped(reinterpret_cast<VAddr>(addr), ib->ib_size * sizeof(u32))) {
                PrefetchComputePrograms({addr, ib->ib_size}, qid, &shadow, depth + 1);
            }
            break;
        }
        case PM4ItOpcode::DispatchDirect:
        case PM4ItOpcode::DispatchIndirect: {
            const u64 code = shadow.address;
            if (code == 0 || code == last_addr || !rasterizer->IsMapped(code << 8, 256)) {
                break;
            }
            last_addr = code;
            auto& live = mapped_queues[qid].cs_state;
            const auto saved = live;
            const auto saved_qid = curr_qid;
            live = shadow;
            curr_qid = qid;
            rasterizer->PrefetchComputeProgram();
            live = saved;
            curr_qid = saved_qid;
            break;
        }
        default:
            break;
        }
        cmds = cmds.subspan(size);
        scanned += size;
    }
}

Liverpool::Task Liverpool::ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb) {
    FIBER_ENTER(dcb_task_name);
    PrefetchComputePrograms(dcb, GfxQueueId);

    cblock.Reset();

    // TODO: potentially, ASCs also can depend on CE and in this case the
    // CE task should be moved into more global scope
    Task ce_task{};

    if (!ccb.empty()) {
        // In case of CCB provided kick off CE asap to have the constant heap ready to use
        ce_task = ProcessCeUpdate(ccb);
        RESUME_GFX(ce_task);
    }

    const auto base_addr = reinterpret_cast<uintptr_t>(dcb.data());
    // sotc: BbToggle::CpFastPaths, read once per command buffer. A Nop that does nothing here (see
    // IsInertNop) goes past ProcessCommands and the opcode switch: the commands it would have let
    // through are taken at the next packet that does something, with no operation in between.
    const bool nop_fast = !BbToggle::Disabled(BbToggle::CpFastPaths) && !SotcOps::kOn;
    // Markers only matter with the guest markers on and no draw thread (the Nop case below).
    const bool markers_matter = guest_markers_enabled && !back_threaded;
    while (!dcb.empty()) {
        if (nop_fast) {
            while (!dcb.empty() && IsInertNop(reinterpret_cast<const PM4Header*>(dcb.data()), markers_matter)) {
                dcb = NextPacket(
                    dcb, reinterpret_cast<const PM4Header*>(dcb.data())->type3.NumWords() + 1);
            }
            if (dcb.empty()) {
                break;
            }
        }
        PollCommands(); // sotc: BbToggle::CpFastPaths

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
            if (SotcOps::kOn) [[unlikely]] { // sotc: SOTC_OP_STATS=1
                SotcOps::CpPacket(static_cast<u32>(opcode), count + 1, false);
            }
            switch (opcode) {
            case PM4ItOpcode::Nop: {
                const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
                if (SotcOps::kOn) [[unlikely]] { // sotc: SOTC_OP_STATS=1
                    SotcOps::CpNop(count + 1, nop->header.count.Value() == 0
                                                  ? 0xFFFFFFFFu
                                                  : static_cast<u32>(nop->data_block[0]));
                }
                if (nop->header.count.Value() == 0) {
                    break;
                }

                switch (nop->data_block[0]) {
                case PM4CmdNop::PayloadType::PatchedFlip: {
                    // There is no evidence that GPU CP drives flip events by parsing
                    // special NOP packets. For convenience lets assume that it does.
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
                MarkParamRegs(&regs.index_buffer_type, sizeof(regs.index_buffer_type));
                break;
            }
            case PM4ItOpcode::DrawIndex2: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndex2*>(header);
                regs.max_index_size = draw_index->max_size;
                regs.index_base_address.base_addr_lo = draw_index->index_base_lo;
                regs.index_base_address.base_addr_hi = draw_index->index_base_hi;
                regs.num_indices = draw_index->index_count;
                regs.draw_initiator = draw_index->draw_initiator;
                MarkParamRegs(&regs.max_index_size, sizeof(regs.max_index_size));
                MarkParamRegs(&regs.index_base_address, sizeof(regs.index_base_address));
                MarkParamRegs(&regs.num_indices, sizeof(regs.num_indices));
                MarkParamRegs(&regs.draw_initiator, sizeof(regs.draw_initiator));
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker("gfx:{}:DrawIndex2", fmt::make_format_args(cmd_address),
                                        [&] {
                                            SubmitDraw(true, 0);
                                        });
                break;
            }
            case PM4ItOpcode::DrawIndexOffset2: {
                const auto* draw_index_off =
                    reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
                regs.max_index_size = draw_index_off->max_size;
                regs.num_indices = draw_index_off->index_count;
                regs.draw_initiator = draw_index_off->draw_initiator;
                MarkParamRegs(&regs.max_index_size, sizeof(regs.max_index_size));
                MarkParamRegs(&regs.num_indices, sizeof(regs.num_indices));
                MarkParamRegs(&regs.draw_initiator, sizeof(regs.draw_initiator));
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexOffset2", fmt::make_format_args(cmd_address),
                    [&] {
                        SubmitDraw(true, draw_index_off->index_offset);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexAuto: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
                regs.num_indices = draw_index->index_count;
                regs.draw_initiator = draw_index->draw_initiator;
                MarkParamRegs(&regs.num_indices, sizeof(regs.num_indices));
                MarkParamRegs(&regs.draw_initiator, sizeof(regs.draw_initiator));
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker("gfx:{}:DrawIndexAuto", fmt::make_format_args(cmd_address),
                                        [&] {
                                            SubmitDraw(false, 0);
                                        });
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
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker("gfx:{}:DispatchDirect", fmt::make_format_args(cmd_address),
                                        [&] {
                                            SubmitDispatch(false, 0, 0, 0);
                                        });
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
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DispatchIndirect", fmt::make_format_args(cmd_address),
                    [&] {
                        SubmitDispatch(true, indirect_args_addr, offset, size);
                    });
                break;
            }
            case PM4ItOpcode::NumInstances: {
                const auto* num_instances = reinterpret_cast<const PM4CmdDrawNumInstances*>(header);
                regs.num_instances.num_instances = num_instances->num_instances;
                MarkParamRegs(&regs.num_instances, sizeof(regs.num_instances));
                break;
            }
            case PM4ItOpcode::IndexBase: {
                const auto* index_base = reinterpret_cast<const PM4CmdDrawIndexBase*>(header);
                regs.index_base_address.base_addr_lo = index_base->addr_lo;
                regs.index_base_address.base_addr_hi = index_base->addr_hi;
                MarkParamRegs(&regs.index_base_address, sizeof(regs.index_base_address));
                break;
            }
            case PM4ItOpcode::IndexBufferSize: {
                const auto* index_size = reinterpret_cast<const PM4CmdDrawIndexBufferSize*>(header);
                regs.num_indices = index_size->num_indices;
                MarkParamRegs(&regs.num_indices, sizeof(regs.num_indices));
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
                BackOrRun([this, eos] {
                    if (rasterizer) {
                        rasterizer->OnFence();
                    }
                    eos.SignalFence([this](void* address, u64 data, u32 num_bytes) {
                        WriteFenceValue(address, data, num_bytes);
                    });
                    if (eos.command == PM4CmdEventWriteEos::Command::GdsStore) {
                        ASSERT(eos.size == 1);
                        if (rasterizer) {
                            rasterizer->Finish();
                            const u32 value = rasterizer->ReadDataFromGds(eos.gds_index);
                            *eos.Address() = value;
                        }
                    }
                }, back_strict);
                break;
            }
            case PM4ItOpcode::EventWriteEop: {
                const PM4CmdEventWriteEop eop_packet =
                    *reinterpret_cast<const PM4CmdEventWriteEop*>(header);
                BackOrRun([this, eop_packet] {
                const auto* event_eop = &eop_packet;
                if (rasterizer) {
                    rasterizer->OnFence();
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
                if (defer_eop && rasterizer) {
                    const PM4CmdEventWriteEop eop = *event_eop;
                    const VAddr label = reinterpret_cast<VAddr>(eop.Address<void>());
                    // A GPU copy of the label's page must not be reused with the old value.
                    if (eop.data_sel.Value() != DataSelect::None) {
                        rasterizer->InvalidateMemory(label, 8, true);
                    }
                    rasterizer->GetScheduler().DeferPriorityOperation([eop] {
                        eop.SignalFence(
                            [](void* address, u64 data, u32 num_bytes) {
                                ASSERT(Core::Memory::Instance()->TryWriteBacking(address, &data,
                                                                                 num_bytes, 2));
                            },
                            [] {
                                Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop);
                            });
                    });
                    // Submit that work now: the game may wait for the label before it sends
                    // anything else, and nothing else would submit it.
                    rasterizer->Flush();
                    return;
                }
                event_eop->SignalFence(
                    [this](void* address, u64 data, u32 num_bytes) {
                        WriteFenceValue(address, data, num_bytes);
                    },
                    [] { Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop); });
                }, back_strict);
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
                } else if ((dma.src_sel == DmaDataSrc::Memory ||
                            dma.src_sel == DmaDataSrc::MemoryUsingL2) &&
                           (dma.dst_sel == DmaDataDst::Memory ||
                            dma.dst_sel == DmaDataDst::MemoryUsingL2)) {
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
                    std::vector<u32> data(write_data->data, write_data->data + data_size / 4);
                    BackOrRun([this, address, data = std::move(data), data_size] {
                        if (rasterizer) {
                            rasterizer->OnFence();
                        }
                        std::memcpy(address, data.data(), data_size);
                        Core::MemoryManager::NoteHostWrite(std::bit_cast<VAddr>(address),
                                                           data_size, 4);
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
                    bool first_poll = true;
                    while (!mem_semaphore->Signaled()) {
                        YIELD_WAIT_GFX(first_poll);
                    }
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
                bool first_poll = true;
                while (!rewind->Valid()) {
                    YIELD_WAIT_GFX(first_poll);
                }
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
                    const CpWaitClock label_clock{SotcStats::cp_label_wait_ns};
                    vo_port->WaitVoLabel([&] { return wait_reg_mem->Test(regs.reg_array); });
                    break;
                }
                bool first_poll = true;
                while (!wait_reg_mem->Test(regs.reg_array)) {
                    YIELD_WAIT_GFX(first_poll);
                }
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

    if (ce_task.handle) {
        while (!ce_task.handle.done()) {
            RESUME_GFX(ce_task);
        }
        ce_task.handle.destroy();
    }

    FIBER_EXIT;
}

template <bool is_indirect>
Liverpool::Task Liverpool::ProcessCompute(std::span<const u32> acb, u32 vqid) {
    FIBER_ENTER(acb_task_name[vqid]);
    PrefetchComputePrograms(acb, vqid + 1);
    auto& queue = asc_queues[{vqid}];

    struct IndirectPatch {
        const PM4Header* header;
        VAddr indirect_addr;
    };
    boost::container::small_vector<IndirectPatch, 4> indirect_patches;

    auto base_addr = reinterpret_cast<VAddr>(acb.data());
    size_t acb_size = acb.size_bytes();
    while (!acb.empty()) {
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
                *queue.read_addr += acb.size();
                *queue.read_addr %= queue.ring_size_dw;
            }
            break;
        }

        if (header->type == 2) {
            // Type-2 packet are used for padding purposes
            next_dw_off = 1;
            acb = NextPacket(acb, next_dw_off);
            if constexpr (!is_indirect) {
                *queue.read_addr += next_dw_off;
                *queue.read_addr %= queue.ring_size_dw;
            }
            continue;
        }

        if (header->type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", header->type.Value());
        }

        const PM4ItOpcode opcode = header->type3.opcode;
        if (SotcOps::kOn) [[unlikely]] { // sotc: SOTC_OP_STATS=1
            SotcOps::CpPacket(static_cast<u32>(opcode), header->type3.NumWords() + 1, true);
        }

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
            bool first_poll = true;
            while (!rewind->Valid()) {
                YIELD_WAIT_ASC(vqid, first_poll);
            }
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
                    std::memcpy(address, data.data(), data_size);
                    Core::MemoryManager::NoteHostWrite(reinterpret_cast<VAddr>(address), data_size,
                                                       4);
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
                bool first_poll = true;
                while (!mem_semaphore->Signaled()) {
                    YIELD_WAIT_ASC(vqid, first_poll);
                }
                mem_semaphore->Decrement();
            }
            break;
        }
        case PM4ItOpcode::WaitRegMem: {
            const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
            ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
            bool first_poll = true;
            while (!wait_reg_mem->Test(regs.reg_array)) {
                YIELD_WAIT_ASC(vqid, first_poll);
            }
            break;
        }
        case PM4ItOpcode::ReleaseMem: {
            const PM4CmdReleaseMem release = *reinterpret_cast<const PM4CmdReleaseMem*>(header);
            BackOrRun([this, release, pipe_id = queue.pipe_id] {
                if (rasterizer) {
                    rasterizer->OnFence();
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
            *queue.read_addr += next_dw_off;
            *queue.read_addr %= queue.ring_size_dw;
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

    std::scoped_lock lk{submit_mutex};
    ++num_submits;
    submit_cv.notify_one();
}

void Liverpool::SubmitAsc(u32 gnm_vqid, std::span<const u32> acb) {
    ASSERT_MSG(gnm_vqid > 0 && gnm_vqid < NumTotalQueues, "Invalid virtual ASC queue index");
    auto& queue = mapped_queues[gnm_vqid];

    const auto vqid = gnm_vqid - 1;
    const auto& task = ProcessCompute(acb, vqid);
    {
        std::scoped_lock lock{queue.m_access};
        queue.submits.emplace(task.handle);
        queue.has_work.store(true, std::memory_order_release);
    }

    std::scoped_lock lk{submit_mutex};
    num_mapped_queues = std::max(num_mapped_queues, gnm_vqid + 1);
    ++num_submits;
    submit_cv.notify_one();
}

} // namespace AmdGpu
