// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <memory>
#include <deque>
#include <mutex>
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

// sotc: BbToggle::MicroCostSweep (gpu/shim/bbport_toggles.h) is read by OnBackThread below; the
// header is not included here because the shader tests include this one without the runtime.
extern "C" std::uint64_t runtime_disabled_optimizations;

namespace AmdGpu {

/// sotc: true on the draw thread only (Liverpool::BackLoop sets it when the thread starts, the
/// thread never changes). Replaces the std::this_thread::get_id() comparison of OnBackThread.
inline thread_local bool t_on_back_thread = false;

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
    /// sotc: a register chunk was written since the last captured operation, so the GPU state is
    /// no longer the one the last graphics pipeline was built from (PipelineKeyMemo). Always
    /// false without the draw thread: MarkRegs does not track chunks then.
    /// sotc: BbToggle::CpFastPaths. A register the pipeline key can read was written since the key
    /// was last built (every MarkRegs except the draw-parameter ones, MarkParamRegs). Unlike the
    /// dirty chunks it is not cleared by the next captured operation, only by a key build.
    bool KeyDirty() const noexcept {
        return key_dirty;
    }
    void ClearKeyDirty() noexcept {
        key_dirty = false;
    }
    bool HasDirtyRegChunks() const noexcept {
        for (const u64 word : dirty_chunks) {
            if (word != 0) {
                return true;
            }
        }
        return false;
    }
    bool OnBackThread() const noexcept {
        // sotc: 1u << 23 is BbToggle::MicroCostSweep (static_assert in liverpool.cpp). Clear: one
        // thread_local read, the same value as the thread id comparison (set: the old path).
        if (!(__atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_RELAXED) & (1u << 23)))
            [[likely]] {
#ifdef SOTC_VERIFY_ON_BACK_THREAD
            ASSERT_MSG(t_on_back_thread == (back_threaded && std::this_thread::get_id() == back_id),
                       "OnBackThread flag differs from the thread id");
#endif
            return back_threaded && t_on_back_thread;
        }
        return back_threaded && std::this_thread::get_id() == back_id;
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
    /// sotc: draw thread. The number of ops it has executed, and whether any of the register chunks
    /// listed changed in value after the op with serial `since` (a chunk the command stream wrote
    /// with the same value does not count). Always true off the draw thread, so a memo built on it
    /// then recomputes every time.
    [[nodiscard]] u32 DrawRegSerial() const noexcept {
        return back_serial;
    }
    /// sotc: BbToggle::DepthFastPath. Draw thread. Three serials, each moving (by one per op) whenever
    /// an op changed in value a register dword the rasterizer marked relevant to it
    /// (SetRenderRelevance): RelRender for the depth side of the render target block (depth
    /// registers, depth extent), RelColor for its colour side (colour target registers and
    /// extents), RelDynamic for the dynamic state derivation. Ops that changed only other dwords
    /// (index base, counts, user data...) leave them. Until the masks are set every op moves all.
    enum RelKind : u32 { RelRender = 0, RelDynamic = 1, RelColor = 2, NumRelKinds };
    [[nodiscard]] u32 DrawRelSerial(RelKind kind) const noexcept {
        return back_rel_serial[kind];
    }
    /// sotc: the dwords of Regs (bit per dword, RegChunkDwords per entry) one block reads
    /// (Rasterizer::DepthFastEnsureRelevance). Called on the draw thread, before the first draw
    /// that uses DrawRelSerial; the masks count as set after the last kind.
    /// sotc: a dword of which only the bits in `bits` matter to `kind` (leave it out of the mask
    /// given to SetRenderRelevance for that kind): the serial then moves only when one of those
    /// bits changed. DB_DEPTH_CONTROL is one for the render target block: of its depth function,
    /// stencil functions... only the enable bits reach the targets.
    void SetRelevancePartial(RelKind kind, u32 dword, u32 bits) {
        if (rel_partial_n[kind] < rel_partial[kind].size()) {
            rel_partial[kind][rel_partial_n[kind]++] = {dword, bits};
        }
    }
    void SetRenderRelevance(RelKind kind, const u64* mask, size_t chunks) {
        for (size_t i = 0; i < chunks && i < rel_mask[kind].size(); ++i) {
            rel_mask[kind][i] = mask[i];
        }
        back_rel_ready = kind == RelColor;
    }
    [[nodiscard]] bool DrawChunksChangedSince(const u16* chunks, size_t count,
                                              u32 since) const noexcept {
        if (!OnBackThread()) {
            return true;
        }
        for (size_t i = 0; i < count; ++i) {
            if (back_chunk_serial[chunks[i]] > since) {
                return true;
            }
        }
        return false;
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

    /// sotc: SOTC_HASH_STATS=1. Appends to `out` the top 15 (op kind, shader hash) keys of the draw
    /// thread's time since the last call and a coverage line, then clears the table counters.
    /// Called by the report thread (videoout/driver.cpp); `seconds` is the window length.
    void HashStatsReport(std::string& out, double seconds);

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
    /// sotc: BbToggle::CpFastPaths (1u << 20, static_assert in liverpool.cpp). ProcessCommands() for
    /// the per-packet call sites of the command processor: with the draw thread, this thread only
    /// ever takes the ordered commands (see ProcessCommands), so with none queued the call does
    /// nothing and is not made. The draw thread and the old path always call it.
    void PollCommands() {
        if (back_threaded && num_ordered.load(std::memory_order_relaxed) == 0 &&
            !(__atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_RELAXED) & (1u << 20)) &&
            !OnBackThread()) {
            return;
        }
        ProcessCommands();
    }
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
    /// sotc: BbToggle::CpPollBackoff. Command processor thread only. Set by a packet wait loop
    /// the first time a resume reaches it (the resume got that far, so it made progress); a resume
    /// that only re-checks a wait that is still false leaves it clear, and Process then counts the
    /// pass over the queues as stalled.
    bool cp_progress{};

    // sotc: the draw thread (see IsBackThreaded).
    struct BackStage {
        Shader::Info* info;
        std::array<u32, 16> user_data; // ShaderParams::NumShaderUserData
        u32 num_user_data; // ~0u: no user data bound
        VAddr pgm_base;
        std::vector<u32> flat;
        bool changed; ///< sotc: DrawReplay, see CaptureBackOp
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
        /// per changed chunk: its index, a 64-bit mask of the dwords that differ (two u32), then
        /// those dwords (BackAlloc / ExecuteBackOp)
        std::vector<u32> reg_chunks;
        /// SOTC_DRAW_OPT_VERIFY: the whole content of every dirty chunk, as the command processor
        /// had it (chunk index, RegChunkDwords dwords), to compare with the draw thread's copy.
        std::vector<u32> verify_chunks;
        std::array<CbDbExtent, NUM_COLOR_BUFFERS> cb_extent{};
        CbDbExtent db_extent{};
        ComputeProgram cs{};
        std::array<BackStage, 5> stages{};
        u32 num_stages{};
        Common::UniqueFunction<void> func;
    };
    static constexpr u32 RegChunkDwords = 64;
    static constexpr u32 NumRegChunks = Regs::NumRegs / RegChunkDwords;
    // sotc: A/B knobs of the draw thread ring, read once in the constructor. SOTC_BACK_RING is
    // the ring capacity (power of two, clamped to [512, 8192]), SOTC_BACK_SPIN the CpuRelax
    // iterations before blocking (clamped to [0, 65536]). The defaults (512, 4096) are the
    // behaviour from before the knobs; the backing array is sized for the maximum capacity.
    static constexpr u64 BackRingMin = 512;
    static constexpr u64 BackRingMax = 8192;
    static constexpr u32 BackSpinDefault = 4096;
    static constexpr u32 BackSpinMax = 65536;

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
        key_dirty = true;
        MarkChunks(first_dword, num_dwords);
    }
    /// sotc: the draw parameters a draw packet writes (index type, counts, base, draw initiator,
    /// instance count): the chunks travel to the draw thread as always, but the pipeline key does
    /// not read them (RefreshGraphicsKey, BuildRuntimeInfo), so they do not make the key dirty.
    void MarkParamRegs(const void* field, size_t bytes) {
        if (!back_threaded) {
            return;
        }
        const auto offset = static_cast<u32>(reinterpret_cast<const u32*>(field) -
                                             regs.reg_array.data());
        MarkChunks(offset, static_cast<u32>((bytes + 3) / 4));
    }
    void MarkChunks(u32 first_dword, u32 num_dwords) {
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
    /// sotc: ExecuteBackOp with the SOTC_HASH_STATS accounting around it.
    void ExecuteBackOpTimed(BackOp& op);
    /// sotc: SOTC_HASH_STATS table, one slot per (op kind, VS/CS hash, PS hash). Written only by
    /// the draw thread: the key fields before `used` is published (release) and never changed
    /// afterwards, the counters with relaxed fetch_add. The report thread reads a slot only after
    /// an acquire load of `used` and takes the counters with exchange(0), so there is no lock and
    /// no data race (a slot's counters may be read between two of its updates: harmless).
    struct HashSlot {
        std::atomic<u32> used{0};
        u32 kind{};
        u64 h1{}; ///< VS (draw) or CS (dispatch) pgm_hash
        u64 h2{}; ///< PS pgm_hash
        std::atomic<u64> ticks{0}, ops{0}, protects{0}, readbacks{0}, finishes{0}, upload{0};
    };
    static constexpr u32 HashSlotBits = 13;
    static constexpr u32 HashSlots = 1u << HashSlotBits; ///< the extra slot at [HashSlots] is the overflow bucket
    HashSlot& HashFind(u32 kind, u64 h1, u64 h2);
    std::unique_ptr<HashSlot[]> hash_slots; ///< HashSlots + 1 entries, allocated only with SOTC_HASH_STATS
    u32 hash_used{};                        ///< draw thread only: slots in use
    u64 hash_tsc0{};                        ///< TSC and steady clock (ns) when the table was created,
    u64 hash_ns0{};                         ///< to calibrate ticks to ns at report time
    void BackLoop(std::stop_token stoken);
    void WakeBack() {
        if (back_threaded) {
            std::scoped_lock lk{back_mutex};
            back_cv.notify_one();
        }
    }

    bool back_threaded{};
    bool back_strict{};
    std::thread::id back_id{};
    u64 back_ring_capacity{BackRingMin}; ///< sotc: SOTC_BACK_RING (power of two)
    u64 back_ring_mask{BackRingMin - 1}; ///< sotc: back_ring_capacity - 1, the wrap-around mask
    u32 back_spin{BackSpinDefault};      ///< sotc: SOTC_BACK_SPIN, iterations before blocking
    std::vector<BackOp> back_ring;       ///< sotc: sized to the capacity read at startup
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
    std::array<u32, NumRegChunks> back_chunk_serial{}; ///< per chunk: serial of its last change
    u32 back_serial{};                                  ///< ops executed (draw thread only)
    std::array<std::array<u64, NumRegChunks>, 3> rel_mask{}; ///< sotc: see SetRenderRelevance
    std::array<u32, 3> back_rel_serial{};                    ///< sotc: see DrawRelSerial
    struct RelPartial {
        u32 dword;
        u32 bits;
    };
    std::array<std::array<RelPartial, 2>, 3> rel_partial{};
    std::array<u32, 3> rel_partial_n{};
    bool back_rel_ready{};
    std::vector<u32> back_shadow; ///< CP side: the registers as the draw thread will hold them (BackAlloc)
    ComputeProgram back_cs{};
    std::array<CbDbExtent, NUM_COLOR_BUFFERS> back_cb_extent{};
    CbDbExtent back_db_extent{};
    std::array<u64, (NumRegChunks + 63) / 64> dirty_chunks{};
    std::vector<u16> dirty_list;
    bool key_dirty{true}; ///< sotc: see KeyDirty (command processor thread only)
    std::jthread back_thread{};
};

} // namespace AmdGpu
