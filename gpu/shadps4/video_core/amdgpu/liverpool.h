// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <exception>
#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <semaphore>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <queue>

#include "common/assert.h"
#include "common/slot_vector.h"
#include "common/types.h"
#include "common/unique_function.h"
#include "common/inline_function.h" // sotc: SOTC_INLINE_FUNC
#include "video_core/amdgpu/cb_db_extent.h"
#include "video_core/amdgpu/op_footprint.h" // sotc: SOTC_OP_OVERLAP
#include "video_core/amdgpu/regs.h"

namespace Vulkan {
class Rasterizer;
class Pipeline;
struct PreparedDraw; // sotc: SOTC_PARALLEL_RECORD (draw_prep.h)
}

namespace Shader {
struct Info;
}

namespace Libraries::VideoOut {
struct VideoOutPort;
}

namespace AmdGpu {

struct PM4CmdWaitRegMem; // sotc: pm4_cmds.h (EopShadowSatisfies)

// sotc: SOTC_CP_LABEL_SKIP report line (liverpool.cpp), appended every 5 s while the switch is on:
// waits that did not pass at once, skippable ones, skipped ones (mode 2 counts only).
void ReportCpLabelSkip(std::string& out, double seconds);

// sotc: SOTC_EOP_TICK statistics (liverpool.cpp), printed under SOTC_FPS.
extern std::atomic<u64> g_eop_written, g_eop_latency_ns, g_eop_max_latency_ns, g_eop_flushes;
// sotc: SOTC_RB_EXACT label side (liverpool.cpp). Labels deferred behind a hot GPU write, the sum
// and the maximum of their deferral latency (ns, queue to landing), labels written in total, and
// deferred labels that landed on a thread other than the draw thread.
extern std::atomic<u64> g_rbx_labels_deferred, g_rbx_labels_total, g_rbx_defer_latency_ns,
    g_rbx_defer_max_latency_ns, g_rbx_offthread;
// sotc: SOTC_RB_EXACT adaptive label policy: addresses currently deferred (gauge), mode switches,
// early re-probes of deferred addresses, hot labels written early, waits charged / not charged to
// an early label, global EWMAs (ns) of deferred landing latency (a) and charged wait (b).
extern std::atomic<u64> g_rbx_addr_deferred, g_rbx_mode_switches, g_rbx_probes,
    g_rbx_labels_early_hot, g_rbx_waits_charged, g_rbx_waits_unattributed, g_rbx_ewma_a_ns,
    g_rbx_ewma_b_ns;

struct Liverpool {
    static constexpr u32 GfxQueueId = 0u;
    static constexpr u32 NumGfxRings = 1u;     // actually 2, but HP is reserved by system software
    static constexpr u32 NumComputePipes = 7u; // actually 8, but #7 is reserved by system software
    static constexpr u32 NumQueuesPerPipe = 8u;
    static constexpr u32 NumComputeRings = NumComputePipes * NumQueuesPerPipe;
    static constexpr u32 NumTotalQueues = NumGfxRings + NumComputeRings;
    static_assert(NumTotalQueues < 64u); // need to fit into u64 bitmap for ffs

    enum ContextRegs : u32 {
        DbZInfo = 0xA010,
        CbColor0Base = 0xA318,
        CbColor1Base = 0xA327,
        CbColor2Base = 0xA336,
        CbColor3Base = 0xA345,
        CbColor4Base = 0xA354,
        CbColor5Base = 0xA363,
        CbColor6Base = 0xA372,
        CbColor7Base = 0xA381,
        CbColor0Cmask = 0xA31F,
        CbColor1Cmask = 0xA32E,
        CbColor2Cmask = 0xA33D,
        CbColor3Cmask = 0xA34C,
        CbColor4Cmask = 0xA35B,
        CbColor5Cmask = 0xA36A,
        CbColor6Cmask = 0xA379,
        CbColor7Cmask = 0xA388,
    };

    Regs regs{};
    std::array<CbDbExtent, NUM_COLOR_BUFFERS> last_cb_extent{};
    CbDbExtent last_db_extent{};

public:
    explicit Liverpool();
    ~Liverpool();

    void SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb);
    void SubmitAsc(u32 gnm_vqid, std::span<const u32> acb);

    void SubmitDone() noexcept {
        std::scoped_lock lk{submit_mutex};
        mapped_queues[GfxQueueId].ccb_buffer_offset = 0;
        mapped_queues[GfxQueueId].dcb_buffer_offset = 0;
        if (frame_tickets) {
            PushFrameMark(); // sotc: SOTC_CP_AHEAD=2
        }
        submit_done = true;
        submit_cv.notify_one();
        NotifyCpWait();
    }

    /// sotc: SOTC_CP_IDLE_US. Called after port code changed something a command buffer task of
    /// the command processor may be waiting on (a label or fence written, a flip label reset, a
    /// new submit or command): wakes the command processor if it sleeps in CpIdleSleep. Nothing
    /// (not even the atomic) when the switch is off. Any thread.
    void NotifyCpWait() noexcept {
        if (!cp_idle_ns) {
            return;
        }
        // seq_cst on both sides (CpIdleSleep): either this sees cp_sleeping or the command
        // processor sees the new epoch before it blocks.
        cp_wake_epoch.fetch_add(1, std::memory_order_seq_cst);
        if (cp_sleeping.load(std::memory_order_seq_cst)) [[unlikely]] {
            WakeCp();
        }
    }

    void WaitGpuIdle() noexcept {
        {
            std::unique_lock lk{submit_mutex};
            submit_cv.wait(lk, [this] { return num_submits == 0; });
        }
        // sotc: and until the draw thread ran what the command processor queued.
        while (back_tail.load(std::memory_order_acquire) != back_head.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    /// sotc: waits only until the command processor parsed every submitted command buffer; the
    /// draw thread may still run what it queued (SOTC_CP_AHEAD, see gnmdriver.cpp WaitSubmitGate).
    void WaitCommandsConsumed() noexcept {
        std::unique_lock lk{submit_mutex};
        submit_cv.wait(lk, [this] { return num_submits == 0; });
    }

    /// sotc: the SOTC_CP_AHEAD value (0 off, the default; 1 parse gate; 2 frame tickets; other
    /// non-zero values mean 1, as before mode 2 existed). See gnmdriver.cpp WaitSubmitGate.
    static int CpAheadMode() noexcept;
    /// sotc: SOTC_OPSTAT: sizeof(BackOp) (the type is private).
    static std::size_t BackOpSize() noexcept;
    /// sotc: SOTC_CP_AHEAD=2: frames closed by SubmitDone so far (any thread).
    u64 FramesMarked() const noexcept {
        return frames_marked.load(std::memory_order_acquire);
    }
    /// sotc: SOTC_CP_AHEAD=2: frames whose every operation the draw thread has run (any thread).
    u64 FramesRecorded() const noexcept {
        return frames_recorded.load(std::memory_order_acquire);
    }
    /// sotc: SOTC_CP_AHEAD=2: blocks until at least `target` frames are recorded (guest threads).
    void WaitFramesRecorded(u64 target) noexcept;
    /// sotc: SOTC_ASC_RPTR (0, the default: the compute ring read pointer moves past a packet when
    /// the command processor has finished it, an indirect buffer included; 1: past an indirect
    /// buffer packet when the command processor fetches it, before the buffer runs; 2: the
    /// doorbell copies the submitted ring dwords and moves the read pointer to the write pointer
    /// at once). See ProcessCompute and gnmdriver.cpp sceGnmDingDong.
    static int AscRptrMode() noexcept;
    /// sotc: SOTC_FPS compute ring accounting, from sceGnmDingDong (guest thread, under the
    /// submit lock): the guest wrote the ring up to next_offs_dw with a doorbell of
    /// submitted_dw dwords.
    void NoteAscDoorbell(u32 vqid, u32 next_offs_dw, u32 submitted_dw) noexcept;

    bool IsGpuIdle() const {
        return num_submits == 0 &&
               back_tail.load(std::memory_order_acquire) == back_head.load(std::memory_order_acquire);
    }

    void SetVoPort(Libraries::VideoOut::VideoOutPort* port) {
        vo_port = port;
    }

    void BindRasterizer(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
    }

    template <bool wait_done = false>
    void SendCommand(auto&& func) {
        // sotc: from the command processor itself (it forwards the queue to the draw thread).
        if (back_threaded && std::this_thread::get_id() == gpu_id) {
            QueueBackFunc(Common::UniqueFunction<void>{std::move(func)}, true);
            if constexpr (wait_done) {
                DrainBack();
            }
            return;
        }
        if constexpr (wait_done) {
            std::binary_semaphore sem{0};
            {
                std::scoped_lock lk{submit_mutex};
                command_queue.emplace([&sem, &func] {
                    func();
                    sem.release();
                });
                ++num_commands;
                submit_cv.notify_one();
            }
            WakeBack();
            NotifyCpWait(); // sotc: SOTC_CP_IDLE_US (without the draw thread the CP runs it)
            sem.acquire();
        } else {
            {
                std::scoped_lock lk{submit_mutex};
                command_queue.emplace(std::move(func));
                ++num_commands;
                submit_cv.notify_one();
            }
            WakeBack();
            NotifyCpWait(); // sotc: SOTC_CP_IDLE_US
        }
    }

    /// sotc: a command that must run at the command processor's point of the command stream,
    /// after the work it queued so far (SendCommand runs before that work with the draw thread:
    /// readbacks for guest threads do not wait for the queue).
    void SendOrderedCommand(Common::UniqueFunction<void>&& func) {
        if (!back_threaded) {
            SendCommand(std::move(func));
            return;
        }
        {
            std::scoped_lock lk{submit_mutex};
            ordered_queue.emplace(std::move(func));
            ++num_ordered;
            submit_cv.notify_one();
        }
        NotifyCpWait(); // sotc: SOTC_CP_IDLE_US
    }

    void ReserveCopyBufferSpace() {
        GpuQueue& gfx_queue = mapped_queues[GfxQueueId];
        std::scoped_lock lk(gfx_queue.m_access);
        constexpr size_t GfxReservedSize = 2_MB >> 2;
        gfx_queue.ccb_buffer.reserve(GfxReservedSize);
        gfx_queue.dcb_buffer.reserve(GfxReservedSize);
    }

    inline ComputeProgram& GetCsRegs() {
        return mapped_queues[curr_qid].cs_state;
    }

    struct AscQueueInfo {
        static constexpr size_t Pm4BufferSize = 1024;
        VAddr map_addr;
        u32* read_addr;
        u32 ring_size_dw;
        u32 pipe_id;
        std::array<u32, Pm4BufferSize> tmp_packet;
        u32 tmp_dwords;
    };
    Common::SlotVector<AscQueueInfo> asc_queues{};

    std::thread::id GetGpuCommandProcessorThread() {
        return gpu_id;
    }

    /// sotc: the draw thread (SOTC_CP_THREADS, on by default). The command processor parses the
    /// command buffers and resolves the pipeline of every draw and dispatch, and the draw thread
    /// binds and records them, in the same order: the pipeline lookup (~20% of the command
    /// processor time) runs while the previous draws are bound. Every other operation that
    /// touches the GPU state or writes guest memory for the GPU is queued to the draw thread as
    /// well, so it keeps the command stream order. The draw thread owns copies of the registers
    /// (updated from the chunks the command processor changed), of the compute registers and of
    /// the per-draw shader data (Shader::InfoRuntime, slot 1).
    bool IsBackThreaded() const noexcept {
        return back_threaded;
    }
    bool OnBackThread() const noexcept {
        return back_threaded && std::this_thread::get_id() == back_id;
    }
    /// sotc: SOTC_FLIP_SERVICE: runs the queued commands of the game's threads, when called on
    /// the draw thread between two operations' worth of state (Vulkan::Presenter::wait_service).
    void ServiceCommandsOnBackThread() {
        if (OnBackThread() && num_commands.load(std::memory_order_acquire)) {
            ProcessCommands();
        }
        RbxWaitService(); // sotc: SOTC_RB_EXACT (no-op when off)
    }
    /// The command processor or the draw thread: both work on the GPU state.
    bool IsGpuSideThread() const noexcept {
        const auto id = std::this_thread::get_id();
        return id == gpu_id || (back_threaded && id == back_id);
    }
    /// A fault on the command processor thread: the draw thread must be idle before the fault
    /// handler touches the GPU state (it may also produce what the faulting read wants).
    void PrepareGpuSideFault() {
        if (back_threaded && std::this_thread::get_id() == gpu_id) {
            DrainBack();
        }
    }
    /// Registers of the draw or dispatch being bound (the draw thread's copy on that thread).
    const Regs& DrawRegs() const noexcept {
        return OnBackThread() ? back_regs : regs;
    }
    const ComputeProgram& DrawCsRegs() noexcept {
        return OnBackThread() ? back_cs : GetCsRegs();
    }
    const std::array<CbDbExtent, NUM_COLOR_BUFFERS>& DrawCbExtent() const noexcept {
        return OnBackThread() ? back_cb_extent : last_cb_extent;
    }
    const CbDbExtent& DrawDbExtent() const noexcept {
        return OnBackThread() ? back_db_extent : last_db_extent;
    }
    /// Command processor: waits until the draw thread ran everything queued.
    void DrainBack();
    /// sotc: frees a finished nested command buffer task's frame once the draw thread has run
    /// the work queued until now (PR #219 freed it at once: garbage textures, heap corruption).
    void FreeTaskFrame(std::coroutine_handle<> frame);
    /// True on the command processor thread while the draw thread does the GPU work.
    bool RunsGpuWorkElsewhere() const noexcept {
        return back_threaded && std::this_thread::get_id() == gpu_id;
    }
    /// Command processor: runs func on the draw thread after what is queued, and waits for it.
    void RunOnBackThread(Common::UniqueFunction<void>&& func) {
        QueueBackFunc(std::move(func), true);
        DrainBack();
    }

#ifdef __linux__
    u32 GetGpuCommandProcessorThreadId() {
        return gpu_tid;
    }
    /// sotc: IsGpuSideThread by Linux thread id (the userfaultfd handler thread asks for the
    /// faulting thread): a draw-thread write fault handled as a guest one would wait for the
    /// draw thread itself.
    bool IsGpuSideThreadId(u32 tid) const noexcept {
        return tid == gpu_tid || (back_threaded && tid == back_tid);
    }
#endif

    /// sotc: SOTC_DROP_FAST guard. True when [addr, addr + size) overlaps a command buffer the
    /// command processor has not finished reading (CmdRanges below). Any thread.
    bool CmdRangeInUse(VAddr addr, u64 size) const {
        // A submit whose walk hit a cap has incomplete ranges: fail closed while one is in flight.
        return cmd_overflow_live.load(std::memory_order_acquire) != 0 ||
               cmd_ranges.Intersects(addr, size);
    }

private:
    /// sotc: SOTC_DROP_FAST guard (CmdRangeInUse). The guest ranges the command processor still
    /// reads: each submitted command buffer until its task finishes (SubmitGfx/SubmitAsc to the
    /// pop in Process), and each nested indirect buffer while it is parsed (CmdRangeGuard). An
    /// unmap that skips the wait for the command processor must not free one of them: the parser
    /// would read unmapped (zero) memory and stop on "Unimplemented PM4 type 0". A handful of
    /// entries at a time; one small mutex, taken after GpuQueue::m_access when both are held.
    class CmdRanges {
    public:
        /// Only SOTC_DROP_FAST reads the ranges; with it unset nothing is recorded.
        static bool Enabled() {
            static const bool on = [] {
                const char* v = std::getenv("SOTC_DROP_FAST"); // sotc: on unless =0
                return !v || v[0] != '0';
            }();
            return on;
        }
        void Add(const void* data, u64 bytes) {
            if (!Enabled() || data == nullptr || bytes == 0) {
                return;
            }
            std::scoped_lock lock{mutex};
            ranges.emplace_back(reinterpret_cast<VAddr>(data), bytes);
        }
        void Remove(const void* data, u64 bytes) {
            if (!Enabled() || data == nullptr || bytes == 0) {
                return;
            }
            const VAddr addr = reinterpret_cast<VAddr>(data);
            std::scoped_lock lock{mutex};
            for (auto it = ranges.begin(); it != ranges.end(); ++it) {
                if (it->first == addr && it->second == bytes) {
                    *it = ranges.back();
                    ranges.pop_back();
                    return;
                }
            }
        }
        bool Intersects(VAddr addr, u64 size) const {
            // Ends saturate at the top of the address space, so a wrapping range overlaps
            // everything above its start instead of nothing.
            const auto end_of = [](VAddr at, u64 n) -> VAddr {
                return n > ~VAddr{0} - at ? ~VAddr{0} : at + n;
            };
            const VAddr end = end_of(addr, size);
            std::scoped_lock lock{mutex};
            for (const auto& [lo, bytes] : ranges) {
                if (addr < end_of(lo, bytes) && lo < end) {
                    return true;
                }
            }
            return false;
        }

    private:
        mutable std::mutex mutex;
        std::vector<std::pair<VAddr, u64>> ranges;
    };
    /// sotc: holds a nested indirect buffer in CmdRanges while its task is parsed (a local of the
    /// parent coroutine, so it lives across the parent's yields).
    struct CmdRangeGuard {
        CmdRangeGuard(CmdRanges& ranges_, const void* data_, u64 bytes_)
            : ranges{ranges_}, data{data_}, bytes{bytes_} {
            ranges.Add(data, bytes);
        }
        ~CmdRangeGuard() {
            ranges.Remove(data, bytes);
        }
        CmdRangeGuard(const CmdRangeGuard&) = delete;
        CmdRangeGuard& operator=(const CmdRangeGuard&) = delete;
        CmdRanges& ranges;
        const void* data;
        u64 bytes;
    };
    CmdRanges cmd_ranges;
    /// sotc: submits in flight whose walk hit a cap (SubmitRanges::overflowed). CmdRangeInUse
    /// answers true while this is not zero.
    std::atomic<u32> cmd_overflow_live{0};
    /// The CmdRanges entries of one submit: the DCB and CCB (or the ACB slice), plus every nested
    /// indirect buffer they call, found at submit time (CollectNestedIbs). overflowed: a cap of
    /// the walk was hit, so the list may miss ranges.
    struct SubmitRanges {
        std::vector<std::pair<const void*, u64>> list;
        bool overflowed{};
    };
    /// sotc: SOTC_DROP_FAST guard. Walks a command buffer's packets at submit time and appends
    /// the ranges of the indirect buffers it calls (recursively, capped), so they are guarded from
    /// the submit on and not only once the parser reaches them (reports2/01-cmdbuf-lifetime.md).
    /// Nested buffers whose memory is not mapped are skipped (counted); a repeated one is walked
    /// once per submit.
    static void CollectNestedIbs(std::span<const u32> cb, SubmitRanges& out, u32 depth);

    struct Task {
        struct promise_type {
            auto get_return_object() {
                Task task{};
                task.handle = std::coroutine_handle<promise_type>::from_promise(*this);
                return task;
            }
            static constexpr std::suspend_always initial_suspend() noexcept {
                // We want the task to be suspended at start
                return {};
            }
            static constexpr std::suspend_always final_suspend() noexcept {
                return {};
            }
            void unhandled_exception() {
                try {
                    std::rethrow_exception(std::current_exception());
                } catch (const std::exception& e) {
                    UNREACHABLE_MSG("Unhandled exception: {}", e.what());
                }
            }
            void return_void() {}
            /// sotc: SOTC_TASK_GUARD=1 gives every task frame pages of its own that become
            /// inaccessible when it is freed (and stay so for a while): a later read of a freed
            /// frame faults where it happens (hunting the garbage textures of PR #219).
            static void* operator new(std::size_t size);
            static void operator delete(void* frame, std::size_t size);
            struct empty {};
            std::suspend_always yield_value(empty&&) {
                return {};
            }
        };

        using Handle = std::coroutine_handle<promise_type>;
        Handle handle;
    };

    using CmdBuffer = std::pair<std::span<const u32>, std::span<const u32>>;
    CmdBuffer CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb);
    Task ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb);
    Task ProcessCeUpdate(std::span<const u32> ccb);
    template <bool is_indirect = false>
    Task ProcessCompute(std::span<const u32> acb, u32 vqid);

    void ProcessCommands();
    /// Writes an EOP/EOS fence value, keeping GPU caches of that memory coherent.
    void WriteFenceValue(void* address, u64 data, u32 num_bytes);
    void Process(std::stop_token stoken);

    struct GpuQueue {
        std::mutex m_access{};
        std::atomic<u32> dcb_buffer_offset;
        std::atomic<u32> ccb_buffer_offset;
        std::vector<u32> dcb_buffer;
        std::vector<u32> ccb_buffer;
        std::queue<Task::Handle> submits{};
        std::queue<SubmitRanges> submit_ranges{}; ///< sotc: CmdRanges entries, in step with submits
        ComputeProgram cs_state{};
        // sotc: lock-free hint for Process()'s round-robin so it can skip m_access entirely for
        // queues with nothing queued, instead of locking every queue every pass just to find
        // most of them empty. Set true under the lock on push, re-synced under the lock on pop;
        // read without the lock as a hint only (a stale false just defers that queue to the next
        // pass - num_submits keeps the outer loop going - a stale true costs one wasted lock).
        //
        // A single shared bitmask + bit-scan (std::countr_zero) to jump straight to the next
        // queue with work, instead of this per-queue flag, measured *worse* here: it drains each
        // batch faster, so submit_cv parks and wakes this thread more often for the same work,
        // and each wake is a costlier kernel round-trip on Windows than the iterations it saves
        // (own-code share 23.3%->16%, but ntdll wait share 37%->45%, reproduced over 2 runs).
        // Keep the simpler per-queue hint; see git history for the bitmask version and numbers.
        std::atomic<bool> has_work{false};
        // sotc: SOTC_ASC_RPTR=2: the ring dwords each queued compute submit runs from, one per
        // entry of `submits` (same order, under m_access); freed when its task finishes.
        std::queue<std::vector<u32>> owned{};
    };
    /// sotc: SOTC_ASC_RPTR mode (constructor) and its read pointer update, ProcessCompute.
    int asc_rptr_mode{};
    void NoteAscRptr(u32 vqid, const AscQueueInfo& queue) noexcept;
    /// sotc: SOTC_FPS compute ring accounting per vqid. A ring is "full" once the free space
    /// after a doorbell is smaller than that doorbell's size, so a like-sized next submit would
    /// spin in the game's retry loop (eboot 0x863a50).
    struct AscRingStat {
        std::atomic<u32> wptr_dw{0};   ///< the guest's write offset after its last doorbell
        std::atomic<u32> need_dw{0};   ///< dwords the last doorbell submitted
        std::atomic<u64> full_ns{0};   ///< when the ring became full (0: not full)
        std::atomic<u64> freed_ns{0};  ///< when the command processor last freed a full ring
        std::atomic<u64> window_ns{0}; ///< full_ns of that window
    };
    std::array<AscRingStat, NumComputeRings> asc_ring_stats{};

    // sotc: SOTC_CP_AHEAD=2 frame tickets (see gnmdriver.cpp WaitSubmitGate). A mark holds, per
    // queue, how many submits were enqueued when the guest called SubmitDone; the frame is
    // parsed once every queue completed that many tasks (tasks of one queue finish in order).
    struct CpFrameMark {
        u64 frame{};
        std::array<u64, NumTotalQueues> enqueued{};
    };
    bool frame_tickets{};
    std::array<u64, NumTotalQueues> q_enqueued{};  ///< under submit_mutex
    std::array<u64, NumTotalQueues> q_completed{}; ///< command processor thread only
    u64 q_done_epoch{};                            ///< command processor: tasks finished
    std::deque<CpFrameMark> frame_marks;           ///< under submit_mutex
    std::atomic<u64> frame_mark_epoch{0};          ///< bumped by PushFrameMark
    u64 fm_seen_epoch{}, fm_seen_done{};           ///< command processor: last check
    std::atomic<u64> frames_marked{0};
    std::atomic<u64> frames_recorded{0};
    std::mutex frames_mutex;
    std::condition_variable frames_cv;
    /// Under submit_mutex (SubmitDone).
    void PushFrameMark();
    /// Command processor: queues the frame end of every mark whose submits are all parsed.
    void ServiceFrameMarks();
    std::array<GpuQueue, NumTotalQueues> mapped_queues{};
    u32 num_mapped_queues{1u}; // GFX is always available

    VAddr indirect_args_addr{};
    u32 num_counter_pairs{};
    u64 pixel_counter{};

    struct ConstantEngine {
        void Reset() {
            ce_count = 0;
            de_count = 0;
            ce_compare_count = 0;
        }

        [[nodiscard]] u32 Diff() const {
            ASSERT_MSG(ce_count >= de_count, "DE counter is ahead of CE");
            return ce_count - de_count;
        }

        u32 ce_compare_count{};
        u32 ce_count{};
        u32 de_count{};
        static std::array<u8, 48_KB> constants_heap;
    } cblock{};

    Vulkan::Rasterizer* rasterizer{};
    Libraries::VideoOut::VideoOutPort* vo_port{};
    const bool guest_markers_enabled;
    std::jthread process_thread{};
    std::atomic<u32> num_submits{};
    std::atomic<u32> num_commands{};
    std::atomic<bool> submit_done{};
    std::mutex submit_mutex;
    std::condition_variable_any submit_cv;
    std::queue<Common::UniqueFunction<void>> command_queue{};
    std::queue<Common::UniqueFunction<void>> ordered_queue{}; // sotc: SendOrderedCommand
    std::atomic<u32> num_ordered{};
    std::thread::id gpu_id;
#ifdef __linux__
    u32 gpu_tid;
    u32 back_tid{}; // sotc: the draw thread's (IsGpuSideThreadId)
#endif
    s32 curr_qid{-1};

    // sotc: the draw thread (see IsBackThreaded).
    struct BackStage {
        Shader::Info* info;
        std::array<u32, 16> user_data; // ShaderParams::NumShaderUserData
        u32 num_user_data; // ~0u: no user data bound
        VAddr pgm_base;
        std::vector<u32> flat;
    };
    /// sotc: SOTC_WD_FUSE. Small WRITE_DATA writes (<= WdMaxBytes in all, <= WdMaxSegs packets)
    /// stored inline in an op instead of a Func closure: applied by the draw thread, in order,
    /// right before the op's own work (see Liverpool::FuseWriteData).
    static constexpr u32 WdMaxBytes = 64;
    static constexpr u32 WdMaxSegs = 4;
    struct BackWd {
        struct Seg {
            VAddr address;
            u8 offset;
            u8 length;
        };
        u8 num_segs{};
        u8 used{};
        std::array<Seg, WdMaxSegs> segs;
        alignas(4) std::array<u8, WdMaxBytes> data;
    };
    struct BackOp {
        enum class Kind : u8 { Func, Draw, DrawIndirect, Dispatch, DispatchIndirect };
        Kind kind{};
        u8 func_kind{3}; ///< sotc: SOTC_FPS accounting: 0 write data, 1 EOP/EOS/release, 2 DMA, 3 other
        BackWd wd; ///< sotc: SOTC_WD_FUSE: applied before the op (num_segs == 0: none)
        bool is_indexed{};
        u16 vertex_sgpr_offset{};
        u16 instance_sgpr_offset{};
        u32 index_offset{};
        u32 offset{};
        u32 stride{};
        u32 max_count{};
        u32 size{};
        VAddr address{};
        VAddr count_address{};
        const Vulkan::Pipeline* pipeline{};
        std::vector<u32> reg_chunks; // chunk index followed by its RegChunkDwords dwords
        std::array<CbDbExtent, NUM_COLOR_BUFFERS> cb_extent{};
        CbDbExtent db_extent{};
        ComputeProgram cs{};
        std::array<BackStage, 5> stages{};
        u32 num_stages{};
        Common::InlineFunction func; // sotc: SOTC_INLINE_FUNC (heap unless on)
        // sotc: SOTC_CULL_EXPRESS (Liverpool::XpPoll). qid: the queue whose task parsed the op
        // (curr_qid). xp_cls/xp_*: what a Func op does to guest memory, set by the parser right
        // before it queues the closure (XpHint); XpUnknown blocks any reorder across the op.
        // xp_done: the express lane already ran the op; BackLoop then only replays its state.
        u8 xp_cls{};
        u8 qid{};
        bool xp_done{};
        VAddr xp_raddr{}, xp_waddr{};
        u64 xp_rsize{}, xp_wsize{};
    };
    static constexpr u32 RegChunkDwords = 64;
    static constexpr u32 NumRegChunks = Regs::NumRegs / RegChunkDwords;
    static constexpr u64 BackRingSize = 512;

    template <typename F>
    void BackOrRun(F&& func, bool writes_memory = true) {
        if (!back_threaded) {
            func();
            return;
        }
        QueueBackFunc(Common::InlineFunction{std::forward<F>(func)}, writes_memory);
    }
    void QueueBackFunc(Common::InlineFunction&& func, bool writes_memory);

    // sotc: SOTC_WD_FUSE (command processor thread only). WRITE_DATA packets wait here instead of
    // being queued while only packets that neither read nor write guest memory, nor queue an op,
    // follow (FuseTransparent). An indirect draw or dispatch whose argument range contains all of
    // them takes them along (AttachWd); everything else queues them first as one Func-less op
    // (FlushWd), which is what separate Func ops did.
    bool wd_fuse{};
    BackWd wd_pending{};
    u8 func_kind_hint{3}; ///< kind of the packet being parsed (BackOp::func_kind)
    u64 wd_last_seq{};    ///< SOTC_FPS: back_head after the last WRITE_DATA op (pair detection)
    /// Queues a WRITE_DATA, or holds it back for the next indirect draw. False: the caller
    /// queues it as a Func.
    bool FuseWriteData(VAddr address, const u32* data, u32 data_size);
    void FlushWd();
    /// Before an indirect draw or dispatch reading args [begin, begin + span): keeps the pending
    /// writes when they all lie inside, flushes them otherwise.
    void PrepareWdForIndirect(VAddr begin, u64 span, bool allowed);
    void AttachWd(BackOp& op);
    /// SOTC_FPS: counts an indirect draw/dispatch right after a WRITE_DATA (held back or queued).
    void NotePairs();
    void ApplyBackWd(const BackWd& wd);
    void WaitBackWritesSlow();
    void SubmitDraw(bool is_indexed, u32 index_offset);
    void SubmitDrawIndirect(bool is_indexed, VAddr address, u32 offset, u32 stride, u32 max_count,
                            VAddr count_address, u16 vertex_sgpr_offset, u16 instance_sgpr_offset);
    void SubmitDispatch(bool indirect, VAddr address, u32 offset, u32 size);
    /// sotc: SOTC_ASYNC_COMPUTE_XLATE: when the current dispatch's compute shader is new,
    /// queues the new compute shaders of the next dispatches in rest (the packets after it)
    /// for the pipeline workers (see Vulkan::PipelineCache::ComputePrefetchEnabled).
    void PrefetchComputeAhead(std::span<const u32> rest);
    BackOp& BackAlloc();
    /// gpu_write: the op writes guest memory on the GPU even when writes_memory is false (a
    /// pipeline that may write, with SOTC_CP_STRICT off). Feeds back_gpu_write_seq only.
    void BackPublish(bool writes_memory, bool gpu_write = false);
    void CaptureBackOp(BackOp& op, const Vulkan::Pipeline* pipeline);
    /// Before reading guest memory the queued work may write (pipeline lookups read the SRTs):
    /// waits for the last queued op that writes guest memory from the CPU (WRITE_DATA, DMA), or
    /// with SOTC_CP_STRICT=1 also for GPU writes (dispatches, storage writes, fences). GPU-written
    /// memory the command processor reads is read-protected once the draw thread recorded the
    /// writer, and its faults run on the draw thread after that; the SRTs and sharp tables are
    /// written by the CPU.
    void WaitBackWrites() {
        if (back_threaded && back_tail.load(std::memory_order_acquire) < back_write_seq) {
            WaitBackWritesSlow(); // sotc: SOTC_FPS accounting around WaitBackTail
        }
    }
    void WaitBackTail(u64 target);
    void MarkRegs(u32 first_dword, u32 num_dwords) {
        if (!back_threaded || num_dwords == 0) {
            return;
        }
        const u32 last = std::min(first_dword + num_dwords - 1, Regs::NumRegs - 1);
        for (u32 chunk = first_dword / RegChunkDwords; chunk <= last / RegChunkDwords; ++chunk) {
            const u64 bit = u64{1} << (chunk % 64);
            if (!(dirty_chunks[chunk / 64] & bit)) {
                dirty_chunks[chunk / 64] |= bit;
                dirty_list.push_back(static_cast<u16>(chunk));
            }
        }
    }
    void MarkRegs(const void* field, size_t bytes) {
        const auto offset = static_cast<u32>(reinterpret_cast<const u32*>(field) -
                                             regs.reg_array.data());
        MarkRegs(offset, static_cast<u32>((bytes + 3) / 4));
    }
    /// sotc: SOTC_REG_SAME (liverpool.cpp). Copies dwords from src to regs at dst and marks the
    /// chunks dirty; with SOTC_REG_SAME=1 nothing is copied or marked when the words are unchanged.
    void WriteRegsMarked(u32 dst, const void* src, u32 dwords);
    void ExecuteBackOp(BackOp& op);
    void BackLoop(std::stop_token stoken);
    /// sotc: SOTC_EOP_TICK=1. Runs the end-of-pipe labels the GPU has finished, and flushes the
    /// open command buffer when a label has waited too long for it. Draw thread only.
    void EopService();
    /// sotc: SOTC_EOP_TICK=1. Called by the command processor while it waits on memory: queues
    /// EopService on the draw thread once the oldest label has waited too long.
    void EopWaitGuard();
    void WakeBack() {
        if (back_threaded && back_nospin) {
            // sotc: SOTC_WAIT_NOSPIN: BackLoop blocks on back_wake; bump it only when it sleeps.
            if (back_sleeping.load(std::memory_order_seq_cst)) {
                WakeBackAtomic();
            }
            return;
        }
        if (back_threaded) {
            std::scoped_lock lk{back_mutex};
            back_cv.notify_one();
        }
    }
    /// sotc: changes back_wake and wakes BackLoop. Called after the state BackLoop waits on changed.
    void WakeBackAtomic() {
        back_wake.fetch_add(1, std::memory_order_seq_cst);
        back_wake.notify_one();
    }

    bool back_threaded{};
    bool back_strict{};
    /// sotc: SOTC_WAIT_NOSPIN=1: BackLoop spins ~20 us, then blocks on back_wake (not back_cv).
    bool back_nospin{};
    // sotc: switches read in the constructor (liverpool.cpp), all OFF by default:
    // SOTC_BACK_HEAD_CACHE (draw thread caches back_head), SOTC_BACK_PF (prefetch of the next
    // op's register chunks), SOTC_REG_SAME (register writes that change nothing are not marked,
    // WriteRegsMarked), SOTC_OPSTAT (counters printed every 5 s).
    bool back_head_cache{};
    bool back_pf{};
    bool reg_same{};
    bool opstat{};
    /// sotc: doorbell BackLoop blocks on under back_nospin; see WakeBackAtomic.
    std::atomic<u32> back_wake{0};
    std::thread::id back_id{};
    std::vector<BackOp> back_ring;
    // sotc: the command processor writes back_head and the draw thread writes back_tail on every
    // op. Each gets its own cache line (layout only, no behaviour change), so the two per-op stores
    // do not bounce one line between the cores. back_sleeping is read per op by the command
    // processor (BackPublish) and stays with back_head; drain_waiting is read per op by the draw
    // thread and stays with back_tail.
    alignas(64) std::atomic<u64> back_head{0};
    std::atomic<bool> back_sleeping{false};
    /// sotc: finished task frames and the back_head they wait for (FreeTaskFrame)
    std::vector<std::pair<std::coroutine_handle<>, u64>> finished_frames;
    alignas(64) std::atomic<u64> back_tail{0};
    std::atomic<bool> drain_waiting{false};
    /// sotc: SOTC_BACK_HEAD_CACHE: the last back_head the draw thread loaded (draw thread only).
    u64 draw_known_head{};
    std::mutex back_mutex;
    std::condition_variable back_cv;
    std::condition_variable drain_cv;
    u64 back_write_seq{0};
    /// sotc: SOTC_CP_LABEL_SKIP. back_head of the newest publish that may write guest memory on
    /// the GPU or the CPU (every writes_memory publish, every pipeline that may write, and every
    /// non-recorded label). Recorded labels do not count: a skipped wait is for one of them.
    /// Always >= back_write_seq.
    u64 back_gpu_write_seq{0};
    Regs back_regs{};
    ComputeProgram back_cs{};
    std::array<CbDbExtent, NUM_COLOR_BUFFERS> back_cb_extent{};
    CbDbExtent back_db_extent{};
    std::array<u64, (NumRegChunks + 63) / 64> dirty_chunks{};
    std::vector<u16> dirty_list;
    std::jthread back_thread{};
    u64 back_exec_seq{}; ///< sotc: the op ExecuteBackOp runs (draw thread)

    /// sotc: SOTC_PARALLEL_RECORD=1 (default off; needs the draw thread and more than 6
    /// hardware threads). prep_workers GpuPrep threads (SOTC_PARALLEL_RECORD_WORKERS, default
    /// clamp(threads / 4, 1, 2)) follow the back ring: each replays every op's registers into its
    /// own prep_regs and computes the register-only inputs (draw_prep.h) of the draws with
    /// seq % prep_workers == its index into prep_ring[seq % BackRingSize]. BackAlloc reuses a
    /// slot only when the draw thread and every prep thread are past it (MinRingTail).
    static constexpr u32 MaxPrepWorkers = 2;
    u32 prep_workers{};
    std::array<std::atomic<u64>, MaxPrepWorkers> prep_tail{}; ///< next op each will look at
    std::atomic<u32> prep_wake{0};     ///< doorbell of sleeping prep threads (BackPublish)
    std::atomic<u32> prep_sleepers{0}; ///< prep threads blocked on prep_wake
    std::unique_ptr<Vulkan::PreparedDraw[]> prep_ring;
    std::vector<std::unique_ptr<Regs>> prep_regs;
    std::vector<std::jthread> prep_threads;
    void PrepLoop(std::stop_token stoken, u32 index);
    /// The oldest op still in use: by the draw thread (back_tail) or a prep thread.
    u64 MinRingTail(std::memory_order order) const {
        u64 tail = back_tail.load(order);
        for (u32 k = 0; k < prep_workers; ++k) {
            tail = std::min(tail, prep_tail[k].load(order));
        }
        return tail;
    }
    /// Draw thread: the prepared inputs of op seq when they are usable, else null (counted).
    const Vulkan::PreparedDraw* TakePrepared(u64 seq);

    /// sotc: SOTC_EOP_TICK=1 (needs back_threaded). End-of-pipe labels wait for the GPU through
    /// the scheduler's deferred operations instead of being written when the command processor
    /// reads them. Off by default.
    bool eop_tick{};
    /// Labels queued and not yet written. Written on the draw thread, read by the CP guard.
    std::atomic<u32> eop_outstanding{0};
    /// Steady clock (ns) at which eop_outstanding went from 0 to 1; 0 when none is outstanding.
    std::atomic<u64> eop_oldest_ns{0};
    /// Scheduler tick of the newest queued label. Equal to CurrentTick() while that label's work
    /// is still in the command buffer being recorded, that is not yet submitted.
    std::atomic<u64> eop_last_tick{0};
    /// sotc: SOTC_EOP_TICK: values queued labels will receive (u32 address -> value), so a
    /// WaitRegMem of the command stream on such a label does not wait for the host GPU: the host
    /// queue runs the stream in order anyway. Guest CPU threads still see the label only once
    /// the GPU has finished (the protection the switch is for).
    std::mutex eop_shadow_mutex;
    std::unordered_map<VAddr, u32> eop_shadow;
    std::atomic<u32> eop_shadow_count{0};
    /// sotc: true when the pending value of a queued label satisfies this wait.
    bool EopShadowSatisfies(const PM4CmdWaitRegMem& wait);

    // sotc: SOTC_CP_LABEL_SKIP=1 (skip) or 2 (count only), off by default. A WAIT_REG_MEM on a
    // label this command processor queued earlier is treated as satisfied while the draw thread
    // has not run that label yet, when the label's value satisfies the wait. Only labels of the
    // same queue count, and only while no other queued op came after them (label_barrier). The
    // CP-side records below are touched by the command processor thread only.
    static constexpr u32 LabelQueues = 8;   ///< 0 graphics, 1 + vqid for compute (vqid < 7)
    static constexpr u32 LabelRecs = 8;     ///< newest queued labels kept per queue
    struct LabelRec {
        VAddr addr{};
        u32 value{};
        u64 seq{}; ///< back_head after the label was published (0: empty slot)
    };
    u8 label_skip{};                                   ///< 0 off, 1 skip, 2 count only
    /// back_head of the newest publish that is not a recorded label of queue q.
    std::array<u64, LabelQueues> label_barrier{};
    std::array<std::array<LabelRec, LabelRecs>, LabelQueues> label_recs{};
    std::array<u32, LabelQueues> label_next{};
    static u32 LabelQueueOf(u32 vqid) {
        return vqid + 1 < LabelQueues ? vqid + 1 : LabelQueues;
    }
    /// Records a label the queue just published (prev_barrier: label_barrier[q] before it).
    /// recordable false: the publish stays a barrier for every label of q.
    void NoteQueuedLabel(u32 q, u64 prev_barrier, bool recordable, VAddr addr, bool dword64,
                         u32 lo, u32 hi);
    /// label_barrier[q] as it is after the FlushWd that QueueBackFunc may run before the label
    /// op (that flush is a publish too). Command processor thread, before BackOrRun.
    u64 LabelPrevBarrier(u32 q);
    /// True when the wait on a not-yet-satisfied label may be treated as satisfied: the queued
    /// ops that may write guest memory (back_gpu_write_seq) are waited for first. Mode 2: counts,
    /// returns false.
    bool LabelWaitSkip(u32 q, const PM4CmdWaitRegMem& wait, bool vo_label);
    /// A EopService call is queued on the draw thread and has not run yet.
    std::atomic<bool> eop_service_requested{false};

    // sotc: SOTC_RB_EXACT=1, label side (rules 5-7 of the design; liverpool.cpp has the long
    // comment). Needs the draw thread. rbx: the switch is on. eop_poll: the deferred label queue
    // is in use (SOTC_EOP_TICK or rbx), so BackLoop polls it and the CP waits use eop_shadow and
    // EopWaitGuard.
    bool rbx{};
    bool eop_poll{};
    /// Hot-write sequence seen at the previous label of each queue (0 gfx, 1 compute); draw
    /// thread only.
    std::array<u64, 2> rbx_last_seq{};
    /// Deferred labels outstanding per 8-byte aligned address (per-address FIFO).
    std::mutex rbx_mutex;
    std::unordered_map<VAddr, u32> rbx_out;
    bool RbxAddrOutstanding(VAddr label); ///< sotc: SOTC_RB_EXACT=publish per-address FIFO
    /// Draw thread, at the label's position in the stream: snapshots HotWriteSeq into snap and
    /// returns true when the label must wait for the GPU.
    bool RbxDecide(u32 queue, VAddr label, u64& snap, bool* hot = nullptr);
    /// Reports a label (or other CPU-visible GPU write) that has just reached guest memory.
    void RbxNoteWritten(u64 snap);
    template <typename F>
    bool RbxTryDefer(u32 queue, VAddr addr, bool shadow32, u32 value, u64& snap, F&& make);
    /// A deferred label: lands (RbxDrain / LandLabel) in queue order once its tick is done.
    struct RbxJob {
        VAddr label{}, key{};
        bool shadow32{}, shadow64{};
        u32 lo{}, hi{}, queue{};
        u64 snap{}, queued_ns{}, tick{};
        std::atomic<bool> ready{false};
        Common::UniqueFunction<void> write;
        // sotc: SOTC_RB_EXACT=publish latency stamps (stat: a CS_DONE label, see EventWriteEop).
        bool stat{};
        u64 t_cp{}, t_draw{}, t_sub{}, t_gpu{};
    };
    /// Queues a label write (write is the whole SignalFence: value and interrupt) as a scheduler
    /// deferred operation. shadow32/shadow64 publish its value for the CP's own waits. t_cp /
    /// t_draw (nonzero t_cp = stat job) are the publish-mode latency stamps.
    std::shared_ptr<RbxJob> QueueDeferredLabel(u32 queue, VAddr label, bool shadow32,
                                               bool shadow64, u32 data_lo, u32 data_hi, u64 snap,
                                               Common::UniqueFunction<void>&& write,
                                               u64 t_cp = 0, u64 t_draw = 0);
    std::deque<std::shared_ptr<RbxJob>> rbx_jobs; ///< FIFO, under rbx_mutex
    std::array<u32, 2> rbx_q_out{};               ///< deferred labels outstanding per queue
    void LandLabel(RbxJob& job);
    void RbxDrain();
    u64 RbxOldestUnsubmittedNs();
    // Adaptive policy per label address (liverpool.cpp, RbxDecide).
    struct RbxAddrStat {
        double a{}, b{}; ///< EWMAs (ns): landing latency, readback wait charged to early labels
        u32 a_samples{}, b_samples{}, hot_count{};
        bool deferred{};
        u64 last_use{};
    };
    struct RbxEarly {
        u64 snap{};
        VAddr key{};
        double cost{};
    };
    static constexpr size_t RbxMaxStats = 1024;
    std::mutex rbx_stat_mutex; ///< policy table and early ring (draw thread and guest threads)
    std::unordered_map<VAddr, RbxAddrStat> rbx_stats;
    u64 rbx_use_clock{};
    std::array<RbxEarly, 128> rbx_early_ring{};
    u32 rbx_early_head{}, rbx_early_count{};
    RbxAddrStat& RbxStat(VAddr key);
    void RbxUpdateMode(RbxAddrStat& st);
    bool RbxPolicyDefer(VAddr key);
    void RbxRecordEarly(u64 snap, VAddr key);
    void RbxFoldLanding(VAddr key, u64 latency_ns);
    static void RbxWaitObserver(u64 writer_seq, u64 wait_ns);
    u64 rbx_flush_tick{}; ///< tick of the last EopService flush (draw thread)

    // sotc: SOTC_CULL_EXPRESS=1 (default off). The "express lane" (liverpool.cpp has the long
    // comment with the exactness argument). The command processor recognises the XPR occlusion
    // submit (a gfx segment that ends in a dispatch and a CS_DONE end-of-pipe label) and publishes
    // it as an express segment [start, end) of back ring sequence numbers. The draw thread, at an
    // op boundary, may run that segment BEFORE the older ops [back_tail, start) when none of them
    // can observe it (XpPoll); anything uncertain keeps the FIFO order and is counted.
    /// What a Func op is, for the lane. Everything not listed is XpUnknown and blocks.
    enum XpCls : u8 {
        XpUnknown,
        XpLabel,     ///< EOP/EOS/ReleaseMem fence: writes waddr/wsize (CPU-visible progress signal)
        XpWriteData, ///< WRITE_DATA to memory: writes waddr/wsize
        XpDma,       ///< DMA_DATA between guest memory ranges: reads raddr/rsize, writes waddr/wsize
        XpMark,      ///< frame end (OnSubmit + Flush, or the frames_recorded ticket)
        XpFlip       ///< flip signal
    };
    struct XpHint {
        u8 cls{};
        VAddr raddr{}, waddr{};
        u64 rsize{}, wsize{};
    };
    /// A published express segment: ring sequence numbers [start, end), the last op is the label.
    struct XpSeg {
        u64 start{}, end{}, queued_ns{};
        VAddr label_key{}; ///< the label's 8-byte aligned address
    };
    bool xp_on{};
    bool xp_signals{true};      ///< SOTC_CULL_EXPRESS_SIGNALS=0: skipped labels/flips block
    bool xp_marks{true};        ///< SOTC_CULL_EXPRESS_MARKS=0: skipped frame marks block
    bool xp_pattern{true};      ///< SOTC_CULL_EXPRESS_PATTERN=0: only SOTC_CULL_EXPRESS_ADDR
    VAddr xp_forced_addr{};     ///< SOTC_CULL_EXPRESS_ADDR=<hex>: this label address qualifies
    u32 xp_max_seg{256};        ///< SOTC_CULL_EXPRESS_SEG
    u32 xp_max_skip{480};       ///< SOTC_CULL_EXPRESS_SKIP (rule 5, <= BackRingSize)
    u64 xp_budget_ns{500'000};  ///< SOTC_CULL_EXPRESS_US (rule 5)
    XpHint xp_hint{};           ///< command processor: consumed by QueueBackFunc
    u64 xp_seg_start{};         ///< command processor: first op of the gfx segment being parsed
    u64 xp_last_dispatch{};     ///< command processor: back_head after the last gfx dispatch
    static constexpr u64 XpRing = 8;
    std::array<XpSeg, XpRing> xp_ring{};
    std::atomic<u64> xp_head{0}; ///< published by the command processor (release)
    std::atomic<u64> xp_tail{0}; ///< taken by the draw thread
    u32 xp_gfx_depth{};          ///< command processor: nesting of ProcessGraphics (indirect buffers)
    u32 xp_retries{};            ///< draw thread: re-evaluations of the oldest segment
    double xp_op_ns{2000.0};     ///< draw thread: EWMA of the time per FIFO op
    u64 xp_watch_end{}, xp_watch_ns{}; ///< draw thread: a FIFO segment whose latency is measured
    // Register undo log of the lane (draw thread): the chunks the lane advanced back_regs through.
    std::vector<u32> xp_undo;
    std::array<u64, (NumRegChunks + 63) / 64> xp_undo_mask{};
    std::vector<u16> xp_undo_list;
    std::array<CbDbExtent, NUM_COLOR_BUFFERS> xp_saved_cb{};
    CbDbExtent xp_saved_db{};
    ComputeProgram xp_saved_cs{};
    void XpPublishSeg(const XpSeg& seg);
    /// Prep thread: the shader-bound footprint of an op into its prepared slot.
    void XpFillResources(Vulkan::PreparedDraw& slot, const Vulkan::Pipeline* pipeline);
    /// sotc: SOTC_OP_OVERLAP (7c.0, SotcStats::kOpOverlap). ovl_on is set before the prep threads
    /// start; ovl_prev* are draw-thread only.
    bool ovl_on{};
    bool ovl_prev_known{};
    AmdGpu::OpFootprint ovl_prev;
    /// Draw thread, before the op runs: compare its footprint with the previous op's.
    void NoteOpOverlap(const BackOp& op);
    void XpPoll(u64 tail);
    /// Draw thread: returns true when the segment was consumed (ran ahead or dropped).
    bool XpEvaluate(const XpSeg& seg, u64 tail);
    void XpApplyState(const BackOp& op);
    void XpUndoState();
    void XpReplayState(const BackOp& op); ///< a lane-run op reached in FIFO order
    void XpSetHint(u8 cls, VAddr waddr, u64 wsize, VAddr raddr = 0, u64 rsize = 0) {
        xp_hint = XpHint{cls, raddr, waddr, rsize, wsize};
    }

public:
    /// sotc: SOTC_RB_EXACT: lands the labels that are ready from a long draw thread wait.
    void RbxWaitService();

private:

    /// sotc: what the command buffer task that last yielded waits on (CP thread only, set by the
    /// wait loops right before they yield; SOTC_FPS "SOTC CP spin" line). Label: a memory wait
    /// while ops that write guest memory are still queued for the draw thread (or SOTC_EOP_TICK
    /// labels are outstanding); flip: a VideoOut flip label; reg mem: other guest memory (written
    /// by the game's CPU threads); other: MemSemaphore, Rewind, register waits.
    enum CpWaitKind : u8 { CpWaitRegMem, CpWaitLabel, CpWaitFlip, CpWaitDeCounter, CpWaitOther };
    u8 cp_wait_kind{CpWaitOther};
    u8 MemWaitKind(const PM4CmdWaitRegMem& wait, bool vo_label) const;
    /// sotc: wait loops left (CP thread only). A resume that left one, finished its task or
    /// was the first of its task made progress; any other resume only re-tested its wait.
    u64 cp_wait_exits{0};
    /// sotc: SOTC_CP_IDLE_US=<us> (0/unset: off). After that long without progress the command
    /// processor blocks in CpIdleSleep for at most SOTC_CP_IDLE_TIMEOUT_US (default 200) instead
    /// of yielding; NotifyCpWait wakes it.
    u64 cp_idle_ns{0};
    u64 cp_idle_timeout_ns{200'000};
    std::atomic<u64> cp_wake_epoch{0};
    std::atomic<bool> cp_sleeping{false};
    std::atomic<u64> cp_notify_ns{0}; ///< SOTC_FPS: when WakeCp last woke it (wake latency)
#ifdef _WIN32
    void* cp_wake_event{}; ///< auto-reset event (HANDLE)
#else
    std::mutex cp_idle_mutex;
    std::condition_variable cp_idle_cv;
#endif
    /// Blocks until NotifyCpWait changes cp_wake_epoch from seen_epoch or the timeout passes.
    /// Returns true when woken by a notification (or the epoch had already moved).
    bool CpIdleSleep(u64 seen_epoch);
    void WakeCp() noexcept;
};

} // namespace AmdGpu
