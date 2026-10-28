// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <mutex>
#include <unordered_map>
#include <semaphore>
#include <span>
#include <thread>
#include <vector>
#include <queue>

#include "common/assert.h"
#include "common/slot_vector.h"
#include "common/types.h"
#include "common/unique_function.h"
#include "video_core/amdgpu/cb_db_extent.h"
#include "video_core/amdgpu/regs.h"

namespace Vulkan {
class Rasterizer;
class Pipeline;
}

namespace Shader {
struct Info;
}

namespace Libraries::VideoOut {
struct VideoOutPort;
}

namespace AmdGpu {

struct PM4CmdWaitRegMem; // sotc: pm4_cmds.h (EopShadowSatisfies)

// sotc: SOTC_EOP_TICK statistics (liverpool.cpp), printed under SOTC_FPS.
extern std::atomic<u64> g_eop_written, g_eop_latency_ns, g_eop_max_latency_ns, g_eop_flushes;

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
        submit_done = true;
        submit_cv.notify_one();
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
            sem.acquire();
        } else {
            {
                std::scoped_lock lk{submit_mutex};
                command_queue.emplace(std::move(func));
                ++num_commands;
                submit_cv.notify_one();
            }
            WakeBack();
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
        std::scoped_lock lk{submit_mutex};
        ordered_queue.emplace(std::move(func));
        ++num_ordered;
        submit_cv.notify_one();
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

private:
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
    };
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
    struct BackOp {
        enum class Kind : u8 { Func, Draw, DrawIndirect, Dispatch, DispatchIndirect };
        Kind kind{};
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
        Common::UniqueFunction<void> func;
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
        QueueBackFunc(Common::UniqueFunction<void>{std::forward<F>(func)}, writes_memory);
    }
    void QueueBackFunc(Common::UniqueFunction<void>&& func, bool writes_memory);
    void SubmitDraw(bool is_indexed, u32 index_offset);
    void SubmitDrawIndirect(bool is_indexed, VAddr address, u32 offset, u32 stride, u32 max_count,
                            VAddr count_address, u16 vertex_sgpr_offset, u16 instance_sgpr_offset);
    void SubmitDispatch(bool indirect, VAddr address, u32 offset, u32 size);
    /// sotc: SOTC_ASYNC_COMPUTE_XLATE: when the current dispatch's compute shader is new,
    /// queues the new compute shaders of the next dispatches in rest (the packets after it)
    /// for the pipeline workers (see Vulkan::PipelineCache::ComputePrefetchEnabled).
    void PrefetchComputeAhead(std::span<const u32> rest);
    BackOp& BackAlloc();
    void BackPublish(bool writes_memory);
    void CaptureBackOp(BackOp& op, const Vulkan::Pipeline* pipeline);
    /// Before reading guest memory the queued work may write (pipeline lookups read the SRTs):
    /// waits for the last queued op that writes guest memory from the CPU (WRITE_DATA, DMA), or
    /// with SOTC_CP_STRICT=1 also for GPU writes (dispatches, storage writes, fences). GPU-written
    /// memory the command processor reads is read-protected once the draw thread recorded the
    /// writer, and its faults run on the draw thread after that; the SRTs and sharp tables are
    /// written by the CPU.
    void WaitBackWrites() {
        if (back_threaded && back_tail.load(std::memory_order_acquire) < back_write_seq) {
            WaitBackTail(back_write_seq);
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
    /// sotc: doorbell BackLoop blocks on under back_nospin; see WakeBackAtomic.
    std::atomic<u32> back_wake{0};
    std::thread::id back_id{};
    std::vector<BackOp> back_ring;
    std::atomic<u64> back_head{0};
    std::atomic<u64> back_tail{0};
    /// sotc: finished task frames and the back_head they wait for (FreeTaskFrame)
    std::vector<std::pair<std::coroutine_handle<>, u64>> finished_frames;
    std::atomic<bool> back_sleeping{false};
    std::atomic<bool> drain_waiting{false};
    std::mutex back_mutex;
    std::condition_variable back_cv;
    std::condition_variable drain_cv;
    u64 back_write_seq{0};
    Regs back_regs{};
    ComputeProgram back_cs{};
    std::array<CbDbExtent, NUM_COLOR_BUFFERS> back_cb_extent{};
    CbDbExtent back_db_extent{};
    std::array<u64, (NumRegChunks + 63) / 64> dirty_chunks{};
    std::vector<u16> dirty_list;
    std::jthread back_thread{};

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
    /// A EopService call is queued on the draw thread and has not run yet.
    std::atomic<bool> eop_service_requested{false};
};

} // namespace AmdGpu
