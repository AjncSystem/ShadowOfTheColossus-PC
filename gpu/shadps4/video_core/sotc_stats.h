// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: counters printed with the frame rate (SOTC_FPS=1, videoout/driver.cpp).
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace SotcStats {
/// sotc: the switches of the stats printers in videoout/driver.cpp (SOTC_FPS line; SOTC_CMD_STATS
/// lines inside it), read once at static initialisation, before any guest thread runs (no
/// function-local static guard and no getenv inside the vectored fault handler; same pattern as
/// BbStats::enabled). Counters that only one printer reads are bumped only when that printer is
/// on: commands (SOTC_FPS) and cmd_kinds, render_breaks, upload_bytes, fault_writes,
/// fault_walker, fault_gpu_reads (SOTC_CMD_STATS). By default the per-draw lock xadds and the
/// guest-thread fault counter (which shared cache lines with CP counters) are gone; with the
/// switches on, the printed numbers are unchanged (the printer reads and resets each counter with
/// exchange(0) only under the same switch). The variables are never set at run time.
inline const bool kFpsLog = std::getenv("SOTC_FPS") != nullptr;
inline const bool kCmdStats = std::getenv("SOTC_CMD_STATS") != nullptr;
/// sotc: skip BindResources on consecutive draws whose pipeline, push data and resource
/// handles did not change. Off by default since 0.60.6 (SOTC_FAST_BIND=1 turns it on): in 0.60.5
/// it left black dots in a grid on sandy ground (new constants at the same address); the fix
/// (mem_epoch below) waits for a check on that ground.
inline const bool kFastBind = [] {
    const char* v = std::getenv("SOTC_FAST_BIND");
    return v && v[0] == '1';
}();
inline std::atomic<uint64_t> fast_bind_eligible{0};
inline std::atomic<uint64_t> fast_bind_hits{0};
/// sotc: bumped whenever guest memory changes outside the GPU's own draws: a write by the command
/// processor (constant RAM dumps, WRITE_DATA, DMA: MemoryManager::NoteHostWrite) or a CPU write
/// the page tracking sees (MemoryTracker). A reused bind (SOTC_FAST_BIND) needs it unchanged: the
/// next draw may have the same descriptors over new constants at the same address, and skipping
/// BindResources skipped their upload (black dots in a grid on the sand, user, 0.60.5).
inline std::atomic<uint64_t> mem_epoch{0};
/// sotc: the game started loading a save or a new area ("st_gameflow() starting game",
/// runtime_file.c): the texture cache drops every image not in use (TextureCache::
/// RunGarbageCollector), so stale images over reused texture memory go away (green blocks after
/// long sessions and many loads, user, 22 Oct). SOTC_LOAD_FLUSH=0 turns it off.
inline std::atomic<bool> load_flush{false};
/// sotc: bytes of hot readbacks not applied because the guest may have written them since.
inline std::atomic<uint64_t> rb_hot_cpu_skipped{0};
inline std::atomic<uint64_t> readbacks{0};    ///< BufferCache::DownloadMemory calls
inline std::atomic<uint64_t> readback_ns{0};  ///< time spent in them (copy and wait)
inline std::atomic<uint64_t> watch_addr{0};     ///< SOTC_CS_WATCH: GPU writes to report
inline std::atomic<uint64_t> current_program{0}; ///< hash of the dispatch being recorded (1 = a draw)
inline std::atomic<uint64_t> readback_ahead{0};   ///< downloads of already submitted work
inline std::atomic<uint64_t> readback_finish{0};  ///< downloads that flushed the current work
inline std::atomic<uint64_t> readback_finish_asked{0}; ///< ... for bytes the guest itself asked
inline std::atomic<uint64_t> heap_guard_builds{0}; ///< readbacks into the BPE heap
inline std::atomic<uint64_t> readback_copies{0};          ///< ranges downloaded
inline std::atomic<uint64_t> readback_copies_changed{0};  ///< ... whose bytes differed from memory
inline std::atomic<uint64_t> readback_bytes{0};
inline std::atomic<uint64_t> readback_bytes_changed{0};
/// SOTC_RB_NARROW: readback window (KiB, 0 = off), GPU-modified bytes of the 512 KiB window left
/// outside the narrow one (not downloaded nor unprotected), live hot readbacks (gauge).
inline std::atomic<uint64_t> rb_narrow_kib{0}, rb_narrow_kept_bytes{0}, rb_hot_entries{0};
inline std::atomic<uint64_t> uploads{0};        ///< SynchronizeMemory copies (CPU data to GPU)
/// DMA sweeps (SynchronizeDmaBuffers): count, resident ranges visited, sweeps that uploaded.
inline std::atomic<uint64_t> dma_sweeps{0}, dma_sweep_ranges{0}, dma_sweep_uploading{0};
/// ... merged runs visited, and time (ns) in all sweeps and in the ones that uploaded nothing.
inline std::atomic<uint64_t> dma_sweep_runs{0}, dma_sweep_ns{0}, dma_sweep_idle_ns{0};
inline std::atomic<uint64_t> upload_bytes{0};
inline std::atomic<uint64_t> render_breaks{0};  ///< EndRendering that closed a render pass
inline std::atomic<uint64_t> fault_writes{0};       ///< guest write faults on tracked pages
inline std::atomic<uint64_t> fault_walker{0};       ///< SRT walker loads emulated after a fault
inline std::atomic<uint64_t> fault_gpu_reads{0};    ///< other read faults on the command processor
inline std::atomic<uint64_t> commands{0}; ///< draws and dispatches recorded (monotonic, CP only)
/// Change of a monotonic single-writer counter since the last call (`seen` keeps the last value).
inline uint64_t TakeDelta(const std::atomic<uint64_t>& counter, uint64_t& seen) {
    const uint64_t now = counter.load(std::memory_order_relaxed);
    const uint64_t delta = now - seen;
    seen = now;
    return delta;
}
/// Command processor waits for the draw thread (Liverpool::IsBackThreaded).
inline std::atomic<uint64_t> cp_waits{0};
/// Busy time of the two command processor threads (SOTC_FPS): the draw thread minus the time it
/// waits for work, the parser as the time inside the command buffer coroutines (not the polling
/// loop around them, which spins while a queue waits).
inline std::atomic<uint64_t> draw_idle_ns{0};
inline std::atomic<uint64_t> cp_parse_ns{0};
inline uint64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
} ///< draws and dispatches recorded
/// SOTC_CMD_STATS=1: draws, indirect draws (arguments GPU-written / CPU-side / CPU-side and
/// empty), dispatches, indirect dispatches.
inline std::atomic<uint64_t> cmd_kinds[6]{};
inline std::atomic<uint64_t> early_flushes{0}; ///< submits right after a write to a read-back page
/// Suspects of long frames, printed for each frame over SOTC_STALL_MS (SOTC_FRAME_LOG runs):
/// pipelines created, shaders recompiled and images created during the frame, and their time.
inline std::atomic<uint64_t> pipelines_new{0}, pipeline_ns{0};
inline std::atomic<uint64_t> shaders_compiled{0}, shader_ns{0};
inline std::atomic<uint64_t> images_new{0}, image_ns{0};
/// Parts of an image creation: format query, memory allocation (and how many got memory of
/// their own instead of a block shared with others).
inline std::atomic<uint64_t> image_query_ns{0}, image_alloc_ns{0}, image_dedicated{0};
/// Video memory: usage the driver reports (texture cache GC), budget the GC works with, and the
/// sparse buffer arena memory (never released) as allocations and bytes.
inline std::atomic<uint64_t> vram_used{0}, vram_budget{0}, arena_allocs{0}, arena_bytes{0};
/// Texture cache collector: ticks (one per submit with a frame's work), images freed and those
/// written back to memory first, and the images in the cache with their (guest) size.
inline std::atomic<uint64_t> gc_ticks{0}, gc_freed{0}, gc_downloads{0};
inline std::atomic<uint64_t> cached_images{0}, cached_image_bytes{0};
inline std::atomic<uint64_t> vram_emergency{0}; ///< emergency texture collections (allocation failed)
inline std::atomic<uint64_t> write_fault_gpu_pending{0}; ///< write-fault readbacks left protected
inline std::atomic<uint64_t> write_fault_stale_gpu{0};   ///< ... that cleared a stale GPU page bit
inline std::atomic<uint64_t> arena_evicted{0};  ///< arena bytes given back by MaybeEvictCold
inline std::atomic<uint64_t> arena_rebound{0};  ///< ... bound again afterwards
inline std::atomic<uint64_t> arena_pinned{0};   ///< ... faulted back in by shaders (kept since)
inline std::atomic<uint64_t> arena_evict_mismatch{0}; ///< read back unequal to guest memory: kept
inline std::atomic<uint64_t> arena_evict_runs{0};
inline std::atomic<uint64_t> arena_evict_ns{0}; ///< time in MaybeEvictCold (scan, GPU wait, unbind)
// sotc: compute pipelines built by pipeline workers (SOTC_ASYNC_COMPUTE), totals.
inline std::atomic<uint64_t> async_compute_built{0};
inline std::atomic<uint64_t> async_compute_build_ns{0}; ///< worker time building them
inline std::atomic<uint64_t> async_compute_waits{0};    ///< binds the recording thread waited for
inline std::atomic<uint64_t> async_compute_wait_ns{0};
inline std::atomic<uint64_t> async_compute_self_built{0}; ///< built by the recording thread itself
inline std::atomic<uint64_t> async_translate_stale{0}; ///< permutations whose tables changed
// sotc: graphics shader stages translated by pipeline workers (SOTC_ASYNC_TRANSLATE), totals.
inline std::atomic<uint64_t> async_translate_built{0};
inline std::atomic<uint64_t> async_translate_ns{0};    ///< worker time translating them
inline std::atomic<uint64_t> async_translate_skips{0}; ///< draws skipped while waiting for one
/// sotc: SRT walker loads that faulted (null or unmapped table pointer) and read zero, total.
inline std::atomic<uint64_t> srt_walker_faults{0};
inline std::atomic<uint64_t> image_bytes{0};    ///< live texture cache and host pass images
inline std::atomic<uint64_t> image_count{0};
inline std::atomic<uint64_t> gc_kept_tiled{0};  ///< GC candidates kept: GPU-written tiled images
inline std::atomic<uint64_t> vma_block_bytes{0};      ///< VMA device-local blocks
inline std::atomic<uint64_t> vma_allocation_bytes{0}; ///< ... and the allocations inside them
/// Image census (TextureCache::Census), bytes of host image memory by class.
inline std::atomic<uint64_t> census_gpu_tiled{0};    ///< GPU-written tiled (the GC keeps them)
inline std::atomic<uint64_t> census_gpu_linear{0};   ///< GPU-written linear
inline std::atomic<uint64_t> census_streamed{0};     ///< sampled, T# with LOD streaming counters
inline std::atomic<uint64_t> census_other{0};        ///< other sampled images
inline std::atomic<uint64_t> census_below_base{0};   ///< mips below the T# base_level
inline std::atomic<uint64_t> census_below_minlod{0}; ///< mips below max(base_level, min_lod)
inline std::atomic<uint64_t> census_live{0};         ///< images used in the last few ticks
inline std::atomic<uint64_t> census_count{0};
inline std::atomic<uint64_t> desync_repairs{0}; ///< stuck write faults repaired (page_manager)
inline std::atomic<uint64_t> desync_legit{0}; ///< ... repeats left alone (tracker wants protection)
inline std::atomic<uint64_t> stack_reg_spins{0}; ///< stack registry writer spins (page_manager)
inline std::atomic<uint64_t> fault_grace_marks{0};    ///< pages made hot (repeat-fault grace)
inline std::atomic<uint64_t> fault_grace_cap_hits{0}; ///< ... refused by the budget
inline std::atomic<uint64_t> fault_grace_hot_upload_bytes{0}; ///< hot pages uploaded, kept writable
/// SOTC_CMD_STATS: system calls behind the command processor's time (SOTC sys line): page
/// protection changes (calls, bytes; made writable), texture cache lock (acquired, contended),
/// device memory allocations and frees (VMA callbacks).
inline std::atomic<uint64_t> sys_protects{0}, sys_protect_bytes{0}, sys_unprotects{0};
inline std::atomic<uint64_t> sys_tc_locks{0}, sys_tc_contended{0}, sys_tc_wait_ns{0};
inline std::atomic<uint64_t> sys_vk_allocs{0}, sys_vk_frees{0};
/// ... protect calls from image page watchers (UpdatePageWatchers) and buffer regions.
inline std::atomic<uint64_t> sys_protect_src[2]{};
/// ... bytes invalidated: map hook, protect hook (the game's mprotect), HLE fills, all callers
/// of BufferCache::InvalidateMemory.
inline std::atomic<uint64_t> inv_map{0}, inv_protect{0}, inv_hle{0}, inv_buffer{0};
/// Mip-tail images (SOTC_MIP_TAIL, TextureCache::DecideRebase). Census gauges, bytes:
inline std::atomic<uint64_t> census_rebased{0};         ///< host memory of mip-tail images
inline std::atomic<uint64_t> census_rebased_count{0};
inline std::atomic<uint64_t> census_dropped{0};         ///< guest bytes of the mips they leave out
inline std::atomic<uint64_t> census_projected{0};       ///< what full-chain candidates would save
inline std::atomic<uint64_t> census_projected_hwcnt{0}; ///< ... of it, T#s with LOD counters
/// ... and counters (per stats line): tails created, refused (a full chain memoized, a GPU-written
/// overlap, a full chain in use, a layout RebaseToMip cannot reproduce), tails freed (a writer
/// overlapped them, an older tail of the same chain), tails re-uploaded instead of hashed
/// (MaybeCpuDirty), and broken never-written rules (cumulative).
inline std::atomic<uint64_t> rebase_created{0}, rebase_reuse_full{0}, rebase_skip_gpu{0},
    rebase_skip_live_full{0}, rebase_skip_layout{0}, rebase_writer_freed{0},
    rebase_dedup_freed{0}, rebase_maybe_dirty{0}, rebase_violations{0};

/// SOTC_OPT_OFF=1: the 2026-10-14 command-processor optimizations are switched off (replicate
/// HLE no-change skip, single texel-buffer image sync, rate-limited VRAM queries), for A/B runs
/// on one binary.
inline bool OptOff() {
    static const bool off = [] {
        const char* v = std::getenv("SOTC_OPT_OFF");
        return v && v[0] == '1';
    }();
    return off;
}

struct ScopedTimer {
    std::atomic<uint64_t>& total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    explicit ScopedTimer(std::atomic<uint64_t>& t) : total{t} {}
    ~ScopedTimer() {
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count(),
                        std::memory_order_relaxed);
    }
};

/// sotc: where the time of slow frames goes (SOTC_FPS, "SOTC FRAMES" line in videoout/driver.cpp).
/// Command-processor time per cause since the last flip; the flip reads and resets them.
/// shader: shader translation (CompileModule); pipeline: pipeline creation; upload: texture uploads (RefreshImage); gc:
/// emergency texture collections; readback: GPU-to-CPU downloads; image: new host images; buffer:
/// CPU-to-GPU buffer uploads of bound ranges; dma: DMA sweeps; alloc: arena and staging memory
/// allocation; hash: texture content hashes (RefreshImage); idle: waiting for the game to submit
/// work. A readback inside an upload counts in
/// both.
enum FrameCause {
    FrameCompile, FramePipeline, FrameUpload, FrameGc, FrameReadback, FrameImage, FrameBuffer,
    FrameDma, FrameAlloc, FrameHash, FrameIdle, FrameCauses
};
inline constexpr const char* kFrameCauseNames[FrameCauses] = {
    "shader", "pipeline", "upload", "gc",   "readback", "image",
    "buffer", "dma",      "alloc",  "hash", "idle"};
/// Bytes copied from guest memory to the GPU since the last flip (SynchronizeMemory).
inline std::atomic<uint64_t> frame_copy_bytes{0};
inline std::atomic<uint64_t> frame_ns[FrameCauses]{};
inline std::atomic<uint64_t> frame_compiles{0}; ///< shader modules and pipelines created
/// sotc: shader cache preload at boot (loading screen bar): pipelines to load and loaded.
inline std::atomic<uint32_t> warmup_total{0}, warmup_done{0};
/// sotc: the boot's shader cache preload is over (the archive read-ahead waits for it: both
/// on one hard disk made the loading screen last 2 minutes).
inline std::atomic<bool> warmup_finished{false};
inline std::atomic<uint64_t> flip_count{0};     ///< game flips (videoout/driver.cpp Flip)
/// Game frames prepared by the command processor (vk_presenter.cpp PrepareFrame): unlike
/// flip_count, in step with the draws the command processor is processing.
inline std::atomic<uint64_t> cp_frame{0};
/// sotc: what the in-game performance overlay shows (bbport_overlay.cpp, show_fps / F10),
/// published by the SOTC_FPS printer at the end of each five-second window (videoout/driver.cpp).
inline std::atomic<float> ov_cmds_per_frame{0.0f};
inline std::atomic<float> ov_readbacks_per_s{0.0f};
inline std::atomic<float> ov_readback_ms_per_s{0.0f};
inline std::atomic<uint32_t> ov_compiles_window{0};
inline std::atomic<uint64_t> ov_compiles_total{0};
inline std::atomic<int> ov_worst_cause{-1}; ///< FrameCause that took most of the worst frame
inline std::atomic<float> ov_worst_cause_ms{0.0f};
/// DLSS Frame Generation (vk_dlssg.cpp): -1 not loaded, 0 off, 1 on, 2 error; frames shown per
/// game frame; sl::DLSSGStatus bits.
inline std::atomic<int> ov_fg_state{-1};
inline std::atomic<uint32_t> ov_fg_frames{0};
inline std::atomic<uint32_t> ov_fg_status{0};
/// FSR 3 frame generation (vk_fsrfg.cpp): 0 waiting for a 3D scene, 1 generating, 2 error.
inline std::atomic<int> ov_fsr_state{0};
/// Phases of PipelineCache::CompileModule (SOTC_FPS, "SOTC COMPILE" line): translate, emit SPIR-V,
/// shader patch lookup, vkCreateShaderModule, cache registration.
inline std::atomic<uint64_t> compile_phase_ns[5]{};
/// Memory allocations on the frame path per kind (SOTC_FPS, "SOTC ALLOC" line every 5 s):
/// count, bytes and time.
enum AllocKind { AllocArena, AllocStagingBlock, AllocStagingLarge, AllocKinds };
inline constexpr const char* kAllocKindNames[AllocKinds] = {"arena", "staging-block",
                                                            "staging-large"};
inline std::atomic<uint64_t> alloc_count[AllocKinds]{}, alloc_bytes[AllocKinds]{},
    alloc_ns[AllocKinds]{};
struct AllocTrace {
    AllocKind kind;
    std::chrono::steady_clock::time_point start{};
    AllocTrace(AllocKind k, uint64_t bytes) : kind{k} {
        if (kFpsLog) {
            alloc_count[k].fetch_add(1, std::memory_order_relaxed);
            alloc_bytes[k].fetch_add(bytes, std::memory_order_relaxed);
            start = std::chrono::steady_clock::now();
        }
    }
    ~AllocTrace() {
        if (kFpsLog) {
            alloc_ns[kind].fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - start)
                                         .count(),
                                     std::memory_order_relaxed);
        }
    }
};

/// Adds the scope's duration to frame_ns[cause]; reads no clock unless SOTC_FPS is set. A timer
/// nested in one of the same cause on the same thread adds nothing (counted once).
struct FrameTimer {
    FrameCause cause;
    bool outer = false;
    std::chrono::steady_clock::time_point start{};
    static inline thread_local bool active[FrameCauses]{};
    explicit FrameTimer(FrameCause c) : cause{c} {
        if (kFpsLog && !active[c]) {
            outer = active[c] = true;
            start = std::chrono::steady_clock::now();
        }
    }
    ~FrameTimer() {
        if (outer) {
            active[cause] = false;
            frame_ns[cause].fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count(),
                                      std::memory_order_relaxed);
        }
    }
};

/// SOTC_DROP_TRACE=1: logs the first 80 draws/dispatches of each kind that are dropped.
inline void Drop(int kind, unsigned long long a, unsigned long long b) {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_DROP_TRACE");
        return v && v[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static std::atomic<int> shown[8]{};
    static const char* names[8] = {"draw-nopipeline", "draw-nobind", "dispatch-nopipeline",
                                   "dispatch-nobind", "condexec-skip", "dispatch-hle", "?", "?"};
    if (shown[kind & 7].fetch_add(1) < 80) {
        std::fprintf(stderr, "SOTC drop %s %#llx %#llx\n", names[kind & 7], a, b);
    }
}
} // namespace SotcStats
