// bbport: optimizations that can be switched off while the game runs (BB_TOGGLE_FILE,
// see runtime_memory.c), to find which one changes rendering without restarting.
#pragma once
#include <atomic>
#include <csetjmp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#ifdef _WIN32
// bbport (Windows): no signals. The loader's exception handler resumes the faulting thread
// in bb_longjmp (src/compat_win.c: no unwinding through the faulting frames).
typedef unsigned long long sigjmp_buf[32];
extern "C" __attribute__((returns_twice)) int bb_setjmp(sigjmp_buf buffer);
#define sigsetjmp(buffer, save) bb_setjmp(buffer)
#endif

extern "C" std::uint64_t runtime_disabled_optimizations;
/// Recovery point for speculative guest memory reads on this thread (runtime_memory.c).
extern "C" __thread sigjmp_buf* runtime_fault_recover;

namespace BbToggle {
enum : std::uint64_t {
    RegionCache = 1,
    FetchShaderCache = 2,
    PageTrackingEarlyExit = 4,
    PendingPollLimit = 8,
    ThreadedRecording = 16,
    ImageDescCache = 32,
    LockFreeUploadCheck = 64,
    FindImageCache = 128,
    DeferredUploads = 256,
    AccessMemo = 512,
    TextureBindingMemo = 1024,
    CoarseReadTracking = 2048,
    PreparedResources = 4096,
    DrawPreparation = 8192,
    DeferredStreamCopies = 16384,
    HotPages = 32768,
    FaultWindow = 65536,
    ParallelCopies = 131072,
    AsyncFences = 262144,
    PoolSmallCopies = 524288,
    RecordPrefetch = 1ull << 32,
    TextureViewMemo = 1ull << 33,
    TextureBindHelper = 1ull << 34,
    EarlyDrawInputs = 1ull << 35,
    ConstantRing = 1ull << 36,
    DrawPipeline = 1ull << 37,
    PipelinedTasks = 1ull << 38,
    PendingFenceWaits = 1ull << 39,
    // sotc (DISPATCH-FASTPATH.md): bit 40 was PipelinedDispatch (no reader left); reused.
    DispatchFastPath = 1ull << 40, ///< set: every written binding inserts its range into
                                   ///< current_writes (a tree search and merges) and the write-mark
                                   ///< memo is dropped by any readback that enters the list in
                                   ///< flight; clear: the ranges go to a vector that the two
                                   ///< Intersects queries scan too, and a readback that entered
                                   ///< after the memo was made drops it only when the binding
                                   ///< touches its region or GPU page runs; in-flight readbacks
                                   ///< keep a hull that skips them; ClampRangeSize is remembered per
                                   ///< VMA table generation; the resident-range, write-mark and
                                   ///< barrier-access memos use 1024/2048/256 slots instead of
                                   ///< 64/256/64. Same answers; honoured live, read per call. SET BY
                                   ///< DEFAULT: measured -0.3 +- 0.17 ms of draw thread, below the
                                   ///< 0.5 ms bar (SOTC_DISPFAST=1/0 pins it, SOTC_DISPFAST_VERIFY=1
                                   ///< checks; docs/sotc/DISPATCH-FASTPATH.md).
    // sotc (DRAW-REPLAY.md): bit 41 was RecorderFences (no reader left); reused.
    DrawReplay = 1ull << 41,      ///< set: ExecuteBackOp copies every stage's flat buffer into the
                                  ///< draw thread's InfoRuntime and a draw's reusable-bind key
                                  ///< (Rasterizer::BindResources) is the XXH3 of its pipeline, push
                                  ///< data, target addresses, user data and flat buffers; clear:
                                  ///< an unchanged flat buffer is not copied (a changed one is
                                  ///< swapped in), InfoRuntime::ud_gen counts the changes, and the
                                  ///< key is compared field by field with the one the last bind
                                  ///< stored (same equality, no hashing); the counters of the
                                  ///< FASTBIND line are plain stores, and only with SOTC_FPS.
                                  ///< CLEAR BY DEFAULT (accepted: -1.2 +- 0.33 ms of draw thread,
                                  ///< +2.7 +- 0.8 FPS in the heavy temple, 0 differences against
                                  ///< the hash: docs/sotc/DRAW-REPLAY.md); SOTC_DRAWREPLAY=0 pins
                                  ///< the old path, SOTC_DRAWREPLAY_VERIFY=1 compares the key with
                                  ///< the hash on every draw. Honoured live, read per op and per
                                  ///< draw.
    PipelinedMemoryWrites = 1ull << 42,
    MultiCopyShader = 1ull << 43,
    RenderStateMemo = 1ull << 44,
    TextureSetMemo = 1ull << 45,
    PipelinedIndirectDraws = 1ull << 46,
    SceneAttachmentsOnly = 1ull << 47,
    SampleSceneProxies = 1ull << 48,
    OrderedGuestWrites = 1ull << 49,
    SceneHalfRes = 1ull << 50, ///< live scaling also reduces the 960x540 post targets
    // sotc (Lote 3, 2026-10-25): set = the old path, clear (default) = the optimization. Bits 20
    // (flat copy swap) and 22 (image destroy worker) were measured without gain and removed.
    // sotc (SIGUIENTE-LIMITE.md): bit 20 is reused for the command processor fast paths.
    CpFastPaths = 1u << 20,        ///< set: the command processor walks every PM4 Nop packet through
                                   ///< ProcessCommands and the opcode switch, and a draw reuses the
                                   ///< previous pipeline only when no register chunk at all was
                                   ///< written since (PipelineKeyMemo); clear: a run of Nop packets
                                   ///< that does nothing (empty or unknown payload) is skipped
                                   ///< without them, and the pipeline memo ignores the registers a
                                   ///< draw packet itself writes (index type, counts, base, draw
                                   ///< initiator, instance count), which no pipeline key reads; the
                                   ///< per-packet ProcessCommands call is skipped when no ordered
                                   ///< command is queued. SET BY DEFAULT (off: it did not move the
                                   ///< frame time, see SIGUIENTE-LIMITE.md); SOTC_CP_FAST=1 or a
                                   ///< BB_TOGGLE_FILE mask without the bit turns it on. Honoured
                                   ///< live (Nops per command buffer, memo per draw).
    BpeGuardDiff = 1u << 21,       ///< set: ApplyDownloads builds BPE heap guards for every copy of
                                   ///< the batch and writes them all; clear: copies whose bytes
                                   ///< already equal guest memory are dropped first (nothing to
                                   ///< write, nothing to guard) and the guard objects and scratch
                                   ///< vectors are reused. Honoured live, read per batch.
    // sotc (READBACK-PLAN.md): set = the old path, clear (default) = the optimization.
    ReadbackIntervals = 1u << 22,  ///< set: a dirty asynchronous readback (any event touched the
                                   ///< region it copied while in flight) is thrown away and its
                                   ///< window downloaded again with a GPU wait (hot regions
                                   ///< included); clear: the bytes no event touched are applied,
                                   ///< the touched ones stay GPU-modified and protected, and the
                                   ///< guest repeats a narrow asynchronous round (ReadMemory)
                                   ///< when the pages it asked for still hold GPU bytes. Honoured
                                   ///< live, read per readback.
    // sotc (PERFIL-2.md C2): set = the old path, clear (default) = the optimization.
    MicroCostSweep = 1u << 23,     ///< set: Liverpool::OnBackThread compares std::this_thread::get_id
                                   ///< with the draw thread's id on every call, the guest region
                                   ///< lookup uses the 512-slot direct table + 8-entry ring and then
                                   ///< the table lock, and UpdatePageWatchersForRegion looks every
                                   ///< page up twice more for the unlock pass; clear: a thread_local
                                   ///< flag, a 2048-slot table backed by a per-thread snapshot of the
                                   ///< VMA table (valid for one table generation), and the locks
                                   ///< taken are remembered for the unlock pass. Results are
                                   ///< identical. Honoured live, read per call.
    // sotc (DESPACHOS, ESCENA-TEMPLO.md): set = the old path, clear (default) = the optimization.
    WriteMarkMemo = 1u << 24,      ///< set: every written buffer binding marks its range GPU-modified
                                   ///< (AddGpuModified: readbacks in flight, gpu_modified_ranges) and
                                   ///< records it in the writes of the command buffer being built
                                   ///< (RecordCurrentWrite) again; clear: a binding of the same
                                   ///< (address, size) whose two effects are already in force (no
                                   ///< range subtracted, no readback started or finished, same
                                   ///< command buffer, no hot read added) repeats nothing. Honoured
                                   ///< live, read per written binding.
    // sotc (DEPTH-FASTPATH.md): set = the old path, clear (default) = the optimization.
    DepthFastPath = 1u << 28,      ///< set: every draw derives its render target block (target
                                   ///< lookup, BeginRendering, dynamic state) from scratch and the
                                   ///< command processor sends whole register chunks when bit 60 is
                                   ///< set too; clear: a depth-only draw (no colour target) whose
                                   ///< render registers, extents, image registry, GPU-modified and
                                   ///< metadata epochs, depth image flags and layout state are those
                                   ///< of the last full derivation reuses its RenderState and the
                                   ///< dynamic state values (registers travel as changed dwords, so
                                   ///< "no render register changed" is known per op). Honoured live,
                                   ///< read per draw.
    // sotc (GBUFFER-FASTPATH.md): set = the old path, clear (default) = the optimization.
    GBufferFastPath = 1u << 29,    ///< set: a draw with colour targets (G-buffer) runs the full
                                   ///< BeginRendering every time; clear: when the render registers
                                   ///< (RelRender/RelColor series), image registry, GPU-modified and
                                   ///< metadata epochs, the pipeline's attachment mask and sample
                                   ///< counts and the state of every attachment image (flags,
                                   ///< backing, no-op transition, not sampled) are those of the last
                                   ///< full derivation, its RenderState is reused. Needs the
                                   ///< DepthFastPath transport (bit 28 clear). Honoured live, read
                                   ///< per draw.
    TexBindFastPath = 1u << 31,    ///< set: every texture of a replayed TexBindMemo goes through
                                   ///< BindResolvedImage (UpdateImage, FindView, Transit); clear: an
                                   ///< image that is still not Dirty and tracked whole, has the
                                   ///< backing the view was found in and needs no barrier for its
                                   ///< shader-read transition takes the remembered view and layout.
                                   ///< Honoured live, read per binding.
    // sotc (MEMORIA-PLAN.md): memory reductions, set = the old path, clear (default) = the change.
    ColdImageSweep = 1u << 25,     ///< set: cached images leave VRAM only when the GC budget
                                   ///< triggers; clear: CPU-origin images idle for
                                   ///< SOTC_COLD_IMAGE_S seconds (default 60) are freed a few per
                                   ///< pass whatever the VRAM use. Honoured live, read per pass.
    ArenaImageRelease = 1u << 26,  ///< set: arena copies of uploaded images stay until the
                                   ///< SOTC_ARENA_EVICT_MB threshold; clear: blocks only ever
                                   ///< bound as image sources go back after SOTC_ARENA_IMG_IDLE_S
                                   ///< idle seconds whatever the threshold. Honoured live.
    StagingScale = 1u << 27,       ///< set: host staging reserve and ring at their full size;
                                   ///< clear: smaller on PCs with 16 GB of RAM or less. Read once
                                   ///< at startup (the pool is sized when it is created).
    UpdateImageFastPath = 1u << 30,
    // TAA A/B in one run: optional techniques, off by default (no measured gain, 2026-10-02).
    TaaTonemapBlend = 1ull << 51,
    TaaClip = 1ull << 52,
    TaaVariance = 1ull << 53,
    TaaFilter = 1ull << 54,
    // On by default (bit set: off): history of a thin feature this jitter phase missed is kept
    // when nothing moves. Static-camera flicker of railings/window bars p99.9 -45% (2026-10-03).
    TaaKeepNearerHistory = 1ull << 55,
    BufferBindMemo = 1ull << 56,  ///< set: every read-only buffer binding goes through the arena
                                  ///< lookup, EnsureResident and the CPU-modified page look of
                                  ///< SynchronizeMemory (BufferCache::ObtainBuffer); clear: a
                                  ///< binding of the same (address, size) that found no
                                  ///< CPU-modified page is answered from a memo while the arena
                                  ///< table, the resident set, the use epoch and the CPU bits of
                                  ///< its 256 KiB words (RegionManager::CpuGenSum) are unchanged.
                                  ///< Written and formatted bindings never use it. Opt-in: the
                                  ///< memo only exists with SOTC_BUFFER_MEMO=1 (measured without
                                  ///< gain, see LOTE-2-RESULTADOS.md); with it, honoured live,
                                  ///< read per binding.
    SceneMipBias = 1ull << 57, ///< negative LOD bias of G-buffer samplers at reduced scene sizes
    PipelineKeyMemo = 1ull << 58, ///< set: rebuild the graphics pipeline key on every draw even
                                  ///< when no register chunk changed (see GetGraphicsPipeline)
    GcLruAdvance = 1ull << 59,    ///< set: do not move kept tiled images to the back of the LRU
                                  ///< during GC (the pass then keeps re-examining the same head).
                                  ///< On by default (V1 off) until V14 scales the GC thresholds
                                  ///< with the VRAM budget: on 12 GB its strong passes measured
                                  ///< up to +16% draw-thread time and -9% FPS.
    DrawFixedCostOpt = 1ull << 60, ///< set: every draw redoes all of its fixed work (state
                                   ///< derivation, push constants, descriptor push, attachment
                                   ///< setup); clear: work whose inputs did not change since the
                                   ///< previous draw is skipped (Rasterizer::DrawIndirect/Draw,
                                   ///< Pipeline::BindResources). Honoured live, read per draw.
    LowLatencyWait = 1ull << 61, ///< set: keep the 15.6 ms Sleep(1) lock backoff and the default
                                  ///< timer resolution; clear: WaitOnAddress and 1 ms. Honoured at
                                  ///< boot from BB_TOGGLE_FILE (SOTC_TIMER_RES=0/1 overrides) and
                                  ///< re-applied when the mask changes in a run.
    CpPollBackoff = 1ull << 62,   ///< set: the command processor polls waiting PM4 packets
                                  ///< (WaitRegMem, MemSemaphore, Rewind) at full speed as before;
                                  ///< clear: after a whole pass over the queues without progress
                                  ///< it pauses, yields and then waits briefly on submit_cv
                                  ///< (Liverpool::Process). Honoured live, read once per idle pass.
    DrawBindMemo = 1ull << 63,    ///< set: every draw resolves its render targets and texture
                                  ///< bindings from scratch (FindImage per T#, per attachment);
                                  ///< clear: the resolutions are remembered per stage and
                                  ///< exact T# set (Rasterizer::BindTextures) and replayed while
                                  ///< the image registry generation and the GPU-modified epoch
                                  ///< stay. Honoured live, read once per stage binding.
    // Startup-only settings are not toggles: the VMA block size is fixed when the allocator is
    // created (SOTC_VMA_BLOCK_MB), before any toggle file is read, so bit 60 (VmaBlock64) was
    // removed instead of pretending to work in a run.
    // Bits 20-29 are used as raw debug toggles by the camera/object motion and the upscaler.
};
inline bool Disabled(std::uint64_t bit) {
    return (__atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_RELAXED) & bit) != 0;
}
} // namespace BbToggle

namespace BbStats {
/// Guest writes caught by page protection, and pages currently left unprotected as hot.
inline std::atomic<std::uint64_t> tracker_faults{0};
inline std::atomic<std::int64_t> hot_pages{0};
/// Stall diagnostics (BB_FRAME_STATS): per-frame deltas printed for frames over 40 ms.
inline std::atomic<std::uint64_t> images_registered{0};
inline std::atomic<std::uint64_t> image_upload_bytes{0};
inline std::atomic<std::uint64_t> buffer_upload_bytes{0};
inline std::atomic<int> gpu_thread_clock{-1}; ///< clockid_t of the GPU command thread
inline std::atomic<std::uint64_t> draws{0}, dispatches{0}, submissions{0};
/// Frames the GPU command thread has started (display pass), for per-frame diagnostics.
inline std::atomic<std::uint64_t> gpu_frames{0};
/// Wall time spent in operations suspected of stalls (ns, all threads).
inline std::atomic<std::uint64_t> t_resident{0}, t_protect{0}, t_image_create{0}, t_refresh{0},
    t_staging{0}, t_host_wait{0}, t_copy{0}, copy_bytes{0}, t_read_faults{0}, read_faults{0},
    t_write_faults{0}, t_copy_cpu{0}, copy_sys_us{0}, copy_minflt{0};
/// Diagnostics are collected only with BB_FRAME_STATS=1.
inline const bool enabled = [] {
    const char* env = std::getenv("BB_FRAME_STATS");
    return env && env[0] == '1';
}();
struct Timer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start =
        enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ~Timer() {
        if (!enabled) {
            return;
        }
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count(),
                        std::memory_order_relaxed);
    }
};
/// Wall time the GPU command thread waited for guest submissions (ns).
inline std::atomic<std::uint64_t> gpu_idle_ns{0};
/// Draws recorded into the reduced scene targets, and draws after the scene started.
inline std::atomic<std::uint64_t> reduced_draws{0}, scene_draws{0};
/// Wall time spent blocked in the scheduler (ns): waiting for the recording thread to drain,
/// for host copies before a submission or fence, and for GPU ticks.
inline std::atomic<std::uint64_t> sync_recording_ns{0}, host_copies_wait_ns{0}, tick_wait_ns{0},
    copy_threads_wait_ns{0}, host_copy_waits{0};
struct WaitTimer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~WaitTimer() {
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start).count(),
                        std::memory_order_relaxed);
    }
};
/// GPU thread rusage, refreshed after each graphics submission.
inline std::atomic<std::uint64_t> gpu_sys_us{0}, gpu_user_us{0}, gpu_invol_switches{0},
    gpu_vol_switches{0}, gpu_minor_faults{0};
/// Protection faults (signals) taken by the GPU thread itself.
inline std::atomic<std::uint64_t> gpu_signal_faults{0};
// sotc: protect_calls/protect_pages/protect_revoke_* removed (written in AddressSpace::Protect,
// never read).
} // namespace BbStats
