// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: counters printed with the frame rate (SOTC_FPS=1, videoout/driver.cpp).
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace SotcStats {
inline std::atomic<uint64_t> flips{0};          ///< guest frames presented (async pipeline ages)
inline std::atomic<uint64_t> async_compiles{0}; ///< graphics pipelines created on workers
inline std::atomic<uint64_t> async_skips{0};    ///< draws skipped while their pipeline compiled
inline std::atomic<uint64_t> async_waits{0};    ///< draws that had to wait for a pending pipeline
inline std::atomic<uint64_t> compute_swaps{0};  ///< unoptimized compute pipelines replaced
/// Texture cache image creations by site: 0 depth/color overlap recreation, 1 expand,
/// 2 new image in FindImage, 3 stencil companion; [4] deletions (DeleteImage).
inline std::atomic<uint64_t> image_churn[5]{};
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
inline std::atomic<uint64_t> uploads{0};        ///< SynchronizeMemory copies (CPU data to GPU)
inline std::atomic<uint64_t> upload_bytes{0};
inline std::atomic<uint64_t> render_breaks{0};  ///< EndRendering that closed a render pass
/// Barriers that closed a render pass, by reason (Rasterizer::BarrierReason bits: indirect args,
/// vertex, index, GDS, buffer read, buffer write, texture, color target, depth target).
inline std::atomic<uint64_t> barrier_breaks[9]{};
/// The game's synchronization in its command streams (Liverpool::NoteSync): PS, CS and VS partial
/// flushes, cache flushes, end-of-shader and end-of-pipe events, ACQUIRE_MEM, WAIT_REG_MEM. The
/// epoch counts them all.
inline std::atomic<uint64_t> sync_events[8]{};
inline std::atomic<uint64_t> sync_epoch{0};
inline std::atomic<uint64_t> fault_writes{0};       ///< guest write faults on tracked pages
inline std::atomic<uint64_t> fault_walker{0};       ///< SRT walker loads emulated after a fault
inline std::atomic<uint64_t> fault_gpu_reads{0};    ///< other read faults on the command processor
inline std::atomic<uint64_t> commands{0}; ///< draws and dispatches recorded
/// SOTC_CMD_STATS=1: draws, indirect draws (arguments GPU-written / CPU-side / CPU-side and
/// empty), dispatches, indirect dispatches.
inline std::atomic<uint64_t> cmd_kinds[6]{};
inline std::atomic<uint64_t> early_flushes{0}; ///< submits right after a write to a read-back page
inline std::atomic<uint64_t> periodic_flushes{0}; ///< FlushPeriodic submits on its time limit
inline std::atomic<uint64_t> idle_flushes{0};     ///< ... earlier because the GPU ran out of work
// Track R stage 1 (SOTC rb/frame line, with SOTC_CMD_STATS=1).
inline std::atomic<uint64_t> readback_noop{0};           ///< DownloadMemory calls with nothing to copy
inline std::atomic<uint64_t> readback_wait_ns{0};        ///< inside FinishHostRead / SubmitAheadAndWait
inline std::atomic<uint64_t> readback_finish_extra{0};   ///< finish only because of a batched hot window
inline std::atomic<uint64_t> readback_extra_deferred{0}; ///< hot windows left out of a batch (HOT_SPLIT)
inline std::atomic<uint64_t> readback_rewritten{0};      ///< finish: writer submitted, window written again
inline std::atomic<uint64_t> readback_dedup{0};          ///< guest read faults that waited for another's
inline std::atomic<uint64_t> readback_dedup_miss{0};     ///< SOTC_RB_DEDUP=2: the retried load faults again
inline std::atomic<uint64_t> readback_joined{0};         ///< async readbacks served by one in flight
inline std::atomic<uint64_t> readback_write_refresh{0};  ///< async write faults whose bytes the GPU wrote again
inline std::atomic<uint64_t> fence_flushes{0};           ///< label submits (SOTC_RB_FENCE_FLUSH)
inline std::atomic<uint64_t> readback_gap_us{0};         ///< SOTC_RB_TRACE: last hot write -> request
inline std::atomic<uint64_t> readback_gap_cmds{0};
inline std::atomic<uint64_t> readback_traced{0};
inline std::atomic<uint64_t> recorder_wait_ns{0};        ///< WaitSubmitted: the recorder thread lagged
// SOTC_RB_EAGER: read-back windows copied right behind their writer.
inline std::atomic<uint64_t> eager_recorded{0};  ///< windows copied to staging behind their writer
inline std::atomic<uint64_t> eager_hits{0};      ///< readbacks served from such a copy
inline std::atomic<uint64_t> eager_dirty{0};     ///< ... written again after the copy (usual path)
inline std::atomic<uint64_t> eager_changed{0};   ///< ... GPU-modified ranges changed (usual path)
inline std::atomic<uint64_t> eager_unused{0};    ///< copies dropped without a readback
inline std::atomic<uint64_t> eager_wait_ns{0};   ///< waiting for the copy's submission
inline std::atomic<uint64_t> eager_verify_bad{0}; ///< SOTC_RB_EAGER=2: copy differs from a fresh one
inline std::atomic<uint64_t> gpu_busy_ns{0};  ///< SOTC_GPU_TIME: GPU time of the submitted command buffers
inline std::atomic<uint64_t> gpu_gap_ns{0};   ///< ... GPU idle between them (waiting for a submit)
inline std::atomic<uint64_t> gpu_timed{0};    ///< ... command buffers timed
inline std::atomic<uint64_t> dma_syncs{0};      ///< SynchronizeDmaBuffers calls
inline std::atomic<uint64_t> dma_sync_skips{0}; ///< ... skipped (nothing CPU-modified since)
inline std::atomic<uint64_t> dma_hot_pages{0};      ///< SOTC_DMA_HOT: pages kept writable (gauge)
inline std::atomic<uint64_t> dma_hot_promotions{0};
inline std::atomic<uint64_t> dma_hot_demotions{0};
inline std::atomic<uint64_t> dma_sync_bad{0};   ///< SOTC_DMA_SYNC_MEMO=2: a skip would have missed uploads
// DrawScan stage 0 (what a command-stream scanner would face), SOTC_CMD_STATS=1.
inline std::atomic<uint64_t> gfx_submits{0};       ///< top-level graphics command buffers
inline std::atomic<uint64_t> gfx_submit_words{0};
inline std::atomic<uint64_t> draws_nested{0};      ///< graphics draws inside an IndirectBuffer
inline std::atomic<uint64_t> draws_top{0};         ///< ... in the top-level command buffer
inline std::atomic<uint64_t> asc_gfx_sh_writes{0}; ///< graphics SH registers written by ASC queues
inline std::atomic<uint64_t> condexec_skips{0};
inline std::atomic<uint64_t> reg_words{0};         ///< register words written (all register packets)
inline std::atomic<uint64_t> scan_agree{0};    ///< SOTC_DRAW_SCAN: scanner history equals the CP's
inline std::atomic<uint64_t> scan_mismatch{0}; ///< ... differs (or the draw was not found)
inline std::atomic<uint64_t> scan_late{0};     ///< ... buffer not scanned yet at the draw
inline std::atomic<uint64_t> scan_lost{0};     ///< ... buffer no longer tracked
inline std::atomic<uint64_t> scan_regs_checked{0}; ///< SOTC_DRAW_SCAN=2: register files compared
inline std::atomic<uint64_t> scan_regs_differ{0};  ///< ... and found different
// SOTC_DRAW_PREP (vk_draw_prep.h)
inline std::atomic<uint64_t> prep_results[8]{};   ///< helper: proposals by PreparedDraw::Result
inline std::atomic<uint64_t> prep_none{0};        ///< draws without a record (history differs)
inline std::atomic<uint64_t> prep_unready[8]{};   ///< records without a proposal, by result
inline std::atomic<uint64_t> prep_reject[8]{};    ///< proposals the command processor refused
inline std::atomic<uint64_t> prep_accept{0};      ///< proposals used
inline std::atomic<uint64_t> prep_mismatch{0};    ///< SOTC_DRAW_PREP=2: differs from full selection
inline std::atomic<uint64_t> prep_diag_later{0};  ///< SOTC_PREP_DIAG: flat data changed after the scan
inline std::atomic<uint64_t> prep_diag_read{0};   ///< ... the helper's read differs from the CP's
inline std::atomic<uint64_t> prep_diag_other{0};  ///< ... neither
inline std::atomic<uint64_t> sel_draws{0};      ///< graphics pipeline selections
inline std::atomic<uint64_t> sel_regs_same{0};  ///< ... with no relevant register write since the last
inline std::atomic<uint64_t> sel_fast_hit{0};   ///< SOTC_SEL_FAST: selections reused
/// ... not reused: 0 none, 1 registers, 2 indirect params, 3 shader code, 4 bindings,
/// 5 flattened user data, 6 fetch shader, 7 vertex V#
inline std::atomic<uint64_t> sel_fast_miss[8]{};
inline std::atomic<uint64_t> sel_fast_fallback{0}; ///< reused, but the pipeline was not built yet
inline std::atomic<uint64_t> sel_fast_mismatch{0}; ///< SOTC_SEL_FAST=2: differs from a full selection
inline std::atomic<uint64_t> sel_regs_unnoticed{0}; ///< ... register file changed without a bump
inline std::atomic<uint64_t> cp_idle_ns{0};    ///< command processor waiting for submissions
inline std::atomic<uint64_t> cp_vo_wait_ns{0}; ///< ... waiting for a display buffer (flip label)
inline std::atomic<uint64_t> eager_ready{0};      ///< hits whose submission had already completed
inline std::atomic<uint64_t> eager_age_us{0};     ///< copy recorded -> readback asked, summed over hits
/// Suspects of long frames, printed for each frame over SOTC_STALL_MS (SOTC_FRAME_LOG runs):
/// pipelines created, shaders recompiled and images created during the frame, and their time.
inline std::atomic<uint64_t> pipelines_new{0}, pipeline_ns{0};
inline std::atomic<uint64_t> shaders_compiled{0}, shader_ns{0};
inline std::atomic<uint64_t> images_new{0}, image_ns{0};
/// A counter written by one thread only (the command processor): a plain increment instead of
/// a locked one (counted on every binding of every draw). The reader's exchange(0) may lose an
/// increment racing with it.
inline void Bump(std::atomic<uint64_t>& counter, uint64_t amount = 1) {
    counter.store(counter.load(std::memory_order_relaxed) + amount, std::memory_order_relaxed);
}
/// Texture bindings answered from Rasterizer::BindTextures' memo (no description, no FindImage).
inline std::atomic<uint64_t> tex_bind_memo_hits{0}, tex_bind_memo_mismatch{0};
/// Texture blocks of a stage (Rasterizer::TryTextureBlock): answered by the memo, bound the usual
/// way, not answerable (storage images, descriptor arrays, too many), stored, SOTC_TEX_BLOCK_MEMO=2
/// mismatches.
inline std::atomic<uint64_t> tex_block[5]{};
/// Texture blocks bound the usual way and not stored, by reason: a binding of another kind (null
/// by the memory map, rebound, feedback loop, storage), the image set changed meanwhile, too many
/// descriptors or images, an image holding a write.
inline std::atomic<uint64_t> tex_block_reject[4]{};
/// Texture blocks the memo did not answer, by reason: another key in the slot, the image set
/// changed, an image changed (state, backing, link or binding flags), an image was uploaded.
inline std::atomic<uint64_t> tex_block_miss[4]{};
/// Image state transitions that changed (or may have changed) the state, of images tracked as one
/// block and of images tracked per subresource (TransitAspect).
inline std::atomic<uint64_t> image_state_changes[2]{};
/// Where the command processor waits for recorded work (Scheduler: Finish, FinishHostRead,
/// SubmitAheadAndWait, Wait), and its own memory invalidations (all, over image memory): the sync
/// points a two-stage command processor would have (SOTC split line).
inline std::atomic<uint64_t> cp_drains[4]{};
inline std::atomic<uint64_t> cp_invalidations[2]{};
/// Render/depth targets answered from Rasterizer::PrepareRenderState's memo.
inline std::atomic<uint64_t> rt_memo_hits{0};
/// Buffer access ranges added to the barrier lists, and ones skipped as recorded already in the
/// same window (Runtime::AccessBuffer).
inline std::atomic<uint64_t> access_adds{0}, access_adds_skipped{0};
/// Draws that reused the render state of an earlier one (Rasterizer::BeginRendering), and
/// SOTC_RENDER_STATE_MEMO=2 mismatches.
inline std::atomic<uint64_t> render_state_reused{0}, render_state_mismatch{0};
/// Texel buffer reads synchronized from images (BufferCache::SynchronizeMemoryFromImage): calls,
/// metadata ranges, ranges an image was re-tiled into.
inline std::atomic<uint64_t> texel_syncs[3]{};

/// SOTC_PHASE_TIME=1: TSC cycles the command processor spends in each phase of the draws and
/// dispatches (Rasterizer::PhaseClock): 0 pipeline selection, 1 render targets
/// (PrepareRenderState), 2 resource bindings, 3 BeginRendering, 4 recording (vertex/index
/// buffers, barriers, descriptors, dynamic state, the draw), 5 after the draw (ResetBindings,
/// FlushPeriodic), 6 dispatch pipeline and HLE, 7 dispatch bindings, 8 dispatch rest. Inside
/// those: 9 BindBuffers, 10 BindTextures; BeginRendering: 11 color image update, 12
/// FindRenderTarget, 13 color metadata, 14 color transit, 15 FindDepthTarget, 16 depth metadata,
/// 17 depth transits and the rest; recording of the indirect draws with CPU-side arguments: 18
/// vertex/index buffers, 19 arguments and barriers, 20 descriptors, 21 dynamic state, 22
/// render pass, pipeline and draw. BindTextures: 23 first pass (lookups), 24 second pass (views,
/// layouts, descriptors), 25 samplers. BindBuffers: 26 range checks, 27 ObtainBuffer, 28 written
/// ranges invalidated, 29 hazard check and descriptor, 30 special buffers. 31 BindResources'
/// checks for compute dispatches done another way (image copy, metadata clear, image clear).
/// PipelineCache::TryPreparedSelection: 32 program parameters, 33 walker, 34 specialization
/// inputs compared, 35 fetch shader, 36 the rest. 37 texture blocks keyed and looked up in the
/// memo (Rasterizer::BindTextures).
inline constexpr int NumPhases = 38;
inline std::atomic<uint64_t> phase_cycles[NumPhases]{};
/// SOTC_PHASE_TIME=1: time the recording thread spends running queued Vulkan commands
/// (CommandRecorder::ConsumerLoop), not spinning or sleeping between them.
inline std::atomic<uint64_t> recorder_busy_ns{0};
inline bool PhaseTime() {
    static const bool enabled = std::getenv("SOTC_PHASE_TIME") != nullptr;
    return enabled;
}
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
