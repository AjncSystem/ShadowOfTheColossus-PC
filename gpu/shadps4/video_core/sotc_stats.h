// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: counters printed with the frame rate (SOTC_FPS=1, videoout/driver.cpp).
#pragma once
#include <atomic>
#include <algorithm>
#include <vector>
#include <string>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <mutex>
#include <string>
#include <vector>

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
/// sotc: SOTC_HASH_STATS=1 (liverpool.cpp ExecuteBackOpTimed, driver.cpp report): time and system
/// call deltas of every draw-thread operation per (kind, shader hash). It needs the sys_* and
/// upload_bytes counters, so it also switches kCmdStats on (those counters are then bumped; the
/// SOTC_CMD_STATS lines inside the SOTC_FPS report print as well when SOTC_FPS is set).
inline const bool kHashStats = [] {
    const char* v = std::getenv("SOTC_HASH_STATS");
    return v && v[0] != '\0' && v[0] != '0';
}();
inline const bool kCmdStats = std::getenv("SOTC_CMD_STATS") != nullptr || kHashStats;
/// sotc: SOTC_HASH_STATS per-op deltas. The global counters below are bumped by every thread (the
/// command processor and the guest threads too) and reset by the printers, so a before/after
/// difference around one draw-thread op mixed in other threads' counts (or lost them over a
/// reset). These per-thread copies are bumped next to them, only with SOTC_HASH_STATS=1, and the
/// op wrapper (liverpool.cpp ExecuteBackOpTimed) reads the draw thread's own.
struct HashThreadCounts {
    uint64_t protects, readbacks, finishes, upload;
};
inline thread_local HashThreadCounts hash_tl{};
/// sotc: skip BindResources on consecutive draws whose pipeline, push data and resource
/// handles did not change. On by default (SOTC_FAST_BIND=0 turns it off). In 0.60.5 it left black
/// dots in a grid on sandy ground (new constants at the same address); with the fix (mem_epoch
/// below) the sand under the title demo's bridge is clean (attract shots, 23 Oct). Shrine idle
/// 33.5 -> 35.3 FPS, title demo 50.6 -> 52.2.
inline const bool kFastBind = [] {
    const char* v = std::getenv("SOTC_FAST_BIND");
    return !v || v[0] != '0';
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
/// sotc: F9 (diagnostic): every cached texture not drawn by the GPU is uploaded again from guest
/// memory. Garbage that stays was written there by the game; garbage that goes was a stale copy.
inline std::atomic<bool> reload_all{false};
/// sotc: F5 (diagnostic): the textures the next draws sample are saved to user/log/texcap_N
/// (raw mip 0 as the GPU holds it; Rasterizer::CaptureTextures).
inline std::atomic<bool> tex_capture{false};
/// sotc: FindMemo hits whose slot no longer held the memoized image (TextureCache::MemoLookup).
inline std::atomic<uint64_t> memo_stale{0};
/// sotc: readback bytes landing on pages the CPU wrote after the GPU (BufferCache, SOTC_RB_CPU_GUARD).
inline std::atomic<uint64_t> rb_over_cpu{0};
/// sotc: DMA copies (Rasterizer::CopyBuffer) done in host memory / on the GPU (SOTC_POOL_WATCH).
inline std::atomic<uint64_t> dma_cpu_copies{0}, dma_cpu_bytes{0}, dma_gpu_copies{0}, dma_gpu_bytes{0};
inline std::atomic<uint64_t> reuse_size_frees{0};
/// sotc: SOTC_POOL_WATCH: guest addresses of images whose pages the game freed while in use; the
/// rasterizer logs the T#s bound at these addresses afterwards.
inline std::atomic<uint64_t> pool_watch_addr[64]{};
inline std::atomic<uint32_t> pool_watch_count{0}; ///< ResolveOverlap: slot reused by another size
inline std::atomic<uint64_t> gc_downloads_skipped{0}; ///< SOTC_GC_DOWNLOAD=0
inline std::atomic<uint64_t> f9_gpu_copies{0}; ///< F9 uploads whose memory the GPU had written
/// sotc: bytes of hot readbacks not applied because the guest may have written them since.
inline std::atomic<uint64_t> rb_hot_cpu_skipped{0};
/// sotc: bytes of hot readbacks not applied because the guest wrote them during the GPU wait.
inline std::atomic<uint64_t> rb_hot_late_skipped{0};
inline std::atomic<uint64_t> readbacks{0};    ///< BufferCache::DownloadMemory calls
inline std::atomic<uint64_t> readback_ns{0};  ///< time spent in them (copy and wait)
inline std::atomic<uint64_t> watch_addr{0};     ///< SOTC_CS_WATCH: GPU writes to report
inline std::atomic<uint64_t> current_program{0}; ///< hash of the dispatch being recorded (1 = a draw)
inline std::atomic<uint64_t> readback_ahead{0};   ///< downloads of already submitted work
inline std::atomic<uint64_t> readback_finish{0};  ///< downloads that flushed the current work
/// sotc: guest readback waits by the age of the data (SOTC_RB_AGE=1, "SOTC RB AGE" line):
/// [0] its last GPU write had completed when asked, [1] it was still in flight; count and
/// wait for the GPU (ns).
inline std::atomic<uint64_t> rb_age_count[2]{}, rb_age_ns[2]{};
/// sotc (SOTC_RB_AGE): the host address of the guest instruction whose fault asked the
/// readback this thread is doing (page_manager.cpp HandleGuestFault), 0 none.
inline thread_local uint64_t fault_rip = 0;
/// sotc: guest loads completed from the backing without a readback (SOTC_CLEAN_GUEST_READS).
inline std::atomic<uint64_t> clean_guest_reads{0};
/// sotc: guest loads of GPU-written bytes answered with the last value read back
/// (SOTC_STALE_READS), and those that read back again because it was too old.
inline std::atomic<uint64_t> stale_guest_reads{0}, stale_refreshes{0};
inline const bool kRbAge = [] {
    const char* v = std::getenv("SOTC_RB_AGE");
    return v && v[0] == '1';
}();
inline std::atomic<uint64_t> readback_finish_asked{0}; ///< ... for bytes the guest itself asked
/// ... the ahead path was idle only because sparse binds were pending (data itself submitted)
inline std::atomic<uint64_t> readback_finish_binds{0};
/// ... only the widened window of the asked bytes was written by the command buffer being built
inline std::atomic<uint64_t> readback_finish_window{0};
/// ... only a hot region brought along was written by the command buffer being built
inline std::atomic<uint64_t> readback_finish_hot{0};
/// async readbacks whose window was re-written in flight and re-read synchronously
inline std::atomic<uint64_t> readback_dirty_retries{0};
/// stale hot bring-alongs dropped instead of forcing that synchronous re-read (SOTC_RB_PART)
inline std::atomic<uint64_t> rb_dirty_hot_dropped{0};
/// Dirtied readback windows re-read asynchronously (another round trip) instead of synchronously.
inline std::atomic<uint64_t> rb_requeues{0};
// sotc: metadata-clipped non-stack readback ranges and bytes, per SOTC_FPS report.
inline std::atomic<uint64_t> heap_guard_clipped_writes{0}, heap_guard_clipped_bytes{0};
inline std::atomic<uint64_t> heap_guard_builds{0}; ///< readbacks into the BPE heap
// sotc: BPE guard cost split per SOTC_FPS report (only counted with SOTC_FPS): batches, heaps whose
// extents were checked, tree nodes walked by builds, heaps whose extent list could not be proven (always
// built), and ns spent selecting heaps / building guards / filtering the writes.
inline std::atomic<uint64_t> heap_guard_batches{0}, heap_guard_checked{0}, heap_guard_nodes{0},
    heap_guard_fallbacks{0}, heap_guard_ns_select{0}, heap_guard_ns_build{0}, heap_guard_ns_write{0};
// sotc: Lote 3 diff-first (BbToggle::BpeGuardDiff), counted only with SOTC_FPS: batches, copies,
// copies dropped because their bytes already equalled guest memory (and bytes), copies kept because
// they overlap another copy, batches left with no copy to write, ns spent comparing.
inline std::atomic<uint64_t> bpe_diff_batches{0}, bpe_diff_copies{0}, bpe_diff_skipped{0},
    bpe_diff_overlapped{0}, bpe_diff_bytes{0}, bpe_diff_skipped_bytes{0}, bpe_diff_empty_batches{0},
    bpe_diff_ns{0};
// sotc: Lote 3, candidate 1 telemetry (no behaviour change; SOTC_FPS only). Asynchronous readbacks
// finished (FinishAsyncReadback, not dependent/already finished) and how many were dirty (a GPU write
// or applied readback hit their ranges while they were in flight, so the whole window is read again).
// Dirty ones: the dirty bytes inside their copies, the bytes their copies held, whether the range the
// guest asked for was hit, whether only part of the copied bytes was hit, the bytes and ns of the
// second download (DownloadMemory) and the bytes of the first one.
inline std::atomic<uint64_t> rbf_finishes{0}, rbf_dirty{0}, rbf_dirty_asked{0}, rbf_dirty_partial{0},
    rbf_clean_bytes{0}, rbf_dirty_copy_bytes{0}, rbf_dirty_hit_bytes{0}, rbf_redownload_bytes{0},
    rbf_redownload_ns{0}, rbf_redownload_calls{0}, rbf_dirty_window_bytes{0};
// sotc: READBACK-PLAN step 0 (SOTC_FPS only). Per dirty asynchronous readback: the cause of its first
// dirtying event (0 GpuWrite, 1 Applied, 2 Drop, 3 CpuReplace), the bytes of its copies hit per
// cause (union per cause), readbacks with a GpuWrite among their causes, readbacks whose causes are
// all non-GpuWrite, readbacks where a GpuWrite touched the pages the guest asked for. In flight at
// Begin: samples, sum and max of the count (including the new one), copy bytes already held by
// another in-flight readback and the bytes copied. SOTC_RB_STALE_CMP=1: bytes of the fresh download
// that a snapshot also held, by class (0 not stale, 1 stale by non-GpuWrite causes only, 2 stale by
// GpuWrite), and how many of them differ.
inline std::atomic<uint64_t> rbs_first[4]{}, rbs_cause_bytes[4]{}, rbs_any_gpuwrite{0},
    rbs_no_gpuwrite{0}, rbs_gpuwrite_asked{0}, rbs_inflight_samples{0}, rbs_inflight_sum{0},
    rbs_inflight_max{0}, rbs_overlap_bytes{0}, rbs_begin_bytes{0}, rbs_cmp_bytes[3]{},
    rbs_cmp_changed[3]{}, rbs_cmp_fresh_only{0}, rbs_cmp_calls{0};
// sotc: READBACK-PLAN fine path (BbToggle::ReadbackIntervals): finishes taken, bytes applied, ns of
// the draw thread inside it, ReadMemory rounds (finished in round 1, 2, 3, synchronous fallback).
// SOTC_RB_VERIFY=1: cumulative checks (never reset): bytes applied/skipped/GpuWrite-stale compared,
// mismatches (see VerifyFineBefore).
inline std::atomic<uint64_t> rbs_fine_finishes{0}, rbs_fine_applied_bytes{0}, rbs_fine_ns{0},
    rbs_rounds[4]{};
inline std::atomic<uint64_t> rbv_applied{0}, rbv_applied_bad{0}, rbv_skipped{0}, rbv_skipped_bad{0},
    rbv_gpuwrite{0}, rbv_gpuwrite_changed{0}, rbv_set_bad{0}, rbv_page_bad{0}, rbv_calls{0},
    rbv_skipped_in_set{0}, rbv_skipped_backing{0}, rbv_skipped_cpu{0};
inline std::atomic<uint64_t> heap_guard_stale_hits{0}; ///< writes the previous guard vouched for
inline std::atomic<uint64_t> upload_deferred{0};        ///< first uploads put off to a later frame
inline std::atomic<uint64_t> upload_deferred_bytes{0};
inline std::atomic<uint64_t> upload_zero_filled{0};     ///< ... images filled with zeros meanwhile
inline std::atomic<uint64_t> heap_guard_ns{0};        ///< time of the guard builds (free-tree walks)
inline std::atomic<uint64_t> heap_guard_worst_ns{0};
inline std::atomic<uint64_t> heap_guard_worst_blocks{0}; ///< free blocks of the slowest build
inline std::atomic<uint64_t> heap_guard_worst_adds{0};   ///< ... and the visits it made
inline std::atomic<uint64_t> heap_guard_repeats{0};
/// sotc: guard diagnostics: builds by path [0 world-heap refresh, 1 batch guard], Covers misses by why
/// [0..3], FindUsedBlock results after a build [0 found, 1..6 why]
inline std::atomic<uint64_t> hg_build_path[2]{}, hg_cover_why[4]{}, hg_find_why[7]{};
/// sotc: heap pieces [0 batches with a valid piece list, 1 without, 2 writes outside every piece,
/// 3 fence-tail writes that landed in a piece, 4 writes vouched by a piece chain]
inline std::atomic<uint64_t> hg_piece[5]{};      ///< nodes and ring members seen again, skipped
inline std::atomic<uint64_t> copy_guest_fallbacks{0}; ///< SOTC_COPY_GUEST_VIEW copies that faulted (backing view)
inline std::atomic<uint64_t> readback_copies{0};          ///< ranges downloaded
inline std::atomic<uint64_t> readback_copies_changed{0};  ///< ... whose bytes differed from memory
inline std::atomic<uint64_t> readback_bytes{0};
inline std::atomic<uint64_t> readback_bytes_changed{0};
/// SOTC_RB_NARROW: readback window (KiB, 0 = off), GPU-modified bytes of the 512 KiB window left
/// outside the narrow one (not downloaded nor unprotected), live hot readbacks (gauge).
inline std::atomic<uint64_t> rb_narrow_kib{0}, rb_narrow_kept_bytes{0}, rb_hot_entries{0};
/// sotc: readbacks with a streaming window (SOTC_RB_STREAM, BufferCache::ReadMemory)
inline std::atomic<uint64_t> rb_stream_windows{0};
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
/// sotc: guest memory lock (src/runtime_memory.c, lock_backoff): waits that took the slow branch
/// (>256 spins: WaitOnAddress, or Sleep(1) with bit 61 set) and the nanoseconds they waited, all
/// threads and the GPU draw thread alone (runtime_memory_mark_draw_thread). The _draw pair is a
/// subset of the total. lock_backoff counts through sotc_lock_slow_stat below.
inline std::atomic<uint64_t> lock_slow_waits{0}, lock_slow_ns{0};
inline std::atomic<uint64_t> lock_slow_waits_draw{0}, lock_slow_ns_draw{0};
/// Busy time of the two command processor threads (SOTC_FPS): the draw thread minus the time it
/// waits for work, the parser as the time inside the command buffer coroutines (not the polling
/// loop around them, which spins while a queue waits).
inline std::atomic<uint64_t> draw_idle_ns{0};
inline std::atomic<uint64_t> cp_parse_ns{0};
/// sotc: busy time of the recorder thread (vk_recorder.cpp ConsumerLoop: time replaying commands,
/// not waiting for them), accumulated with SOTC_FPS=1 only; the "threads per frame" line prints it.
inline std::atomic<uint64_t> recorder_busy_ns{0};
/// sotc: time the draw thread waited for work while SOTC_HASH_STATS=1 (own counter: draw_idle_ns
/// is reset by the SOTC_FPS printer); the denominator of the table coverage line.
inline std::atomic<uint64_t> hash_idle_ns{0};
inline uint64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
} ///< draws and dispatches recorded
/// sotc: throttle for warnings on hot paths (per draw or dispatch, vk_rasterizer.cpp). A line is
/// printed only if its site (the tag pointer, hashed to a slot) waited min_interval_ns and the
/// global budget of kLogLinesPerSecond has room in the current one-second window; otherwise the
/// caller drops it. Every printed line counts in log_lines, every dropped one in log_suppressed
/// (the SOTC_FPS window prints both; videoout/driver.cpp).
inline constexpr uint32_t kLogLinesPerSecond = 5;
inline constexpr uint32_t kLogSlots = 8;
inline std::atomic<uint64_t> log_lines{0};      ///< lines printed through LogThrottled
inline std::atomic<uint64_t> log_suppressed{0}; ///< lines LogThrottled dropped
inline bool LogThrottled(const char* tag, int64_t min_interval_ns = 200'000'000) {
    // One slot per tag (collisions only make both sites print less). Read-only on the hot path
    // until a line is actually due, so a suppressed line costs two relaxed loads and one add.
    static std::atomic<int64_t> slot_ns[kLogSlots]{};
    static std::atomic<int64_t> window_start{-1'000'000'000};
    static std::atomic<uint32_t> window_lines{0};
    const int64_t now = static_cast<int64_t>(NowNs());
    std::atomic<int64_t>& slot =
        slot_ns[((reinterpret_cast<std::uintptr_t>(tag) >> 4) ^
                 (reinterpret_cast<std::uintptr_t>(tag) >> 9)) &
                (kLogSlots - 1)];
    const int64_t last = slot.load(std::memory_order_relaxed);
    if (last != 0 && now - last < min_interval_ns) {
        log_suppressed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    int64_t start = window_start.load(std::memory_order_relaxed);
    if (now - start >= 1'000'000'000) {
        int64_t expected = start;
        if (window_start.compare_exchange_strong(expected, now, std::memory_order_relaxed)) {
            window_lines.store(0, std::memory_order_relaxed);
        }
    }
    if (window_lines.load(std::memory_order_relaxed) >= kLogLinesPerSecond) {
        log_suppressed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // The budget check is racy (a few lines may slip over per window), which is fine for a
    // diagnostic throttle and keeps a single atomic add here.
    window_lines.fetch_add(1, std::memory_order_relaxed);
    slot.store(now, std::memory_order_relaxed);
    log_lines.fetch_add(1, std::memory_order_relaxed);
    return true;
}

/// sotc: SOTC_DRAW_SPLIT=1 (with SOTC_FPS): where the draw thread's time goes, the "SOTC DRAW
/// SPLIT" lines (videoout/driver.cpp; step 0 of docs/sotc/PLANO-THREAD-DESENHO.md). "draw busy"
/// counts every wait inside an operation (flips, GPU waits, readbacks) as busy; these counters
/// separate the operations, the binds and the waits, in TSC ticks (one rdtsc per edge), and the
/// printer reads the CPU time of the draw, command processor and recorder threads. Counting only:
/// nothing else changes.
inline const bool kDrawSplit = std::getenv("SOTC_DRAW_SPLIT") != nullptr;
/// sotc: SOTC_VIS_DIRECT=1 (needs SOTC_PC_MEMORY=1; docs/sotc/MODELO-MEMORIA-PC.md steps 3 and 4):
/// the compute shaders that write the visibility bitmaps the game scans every frame (eboot
/// 0x1a6d60) write them straight into the game's memory (the imported backing view) instead of
/// the arena, so the game's reads no longer fault and wait for a readback; the next end-of-pipe
/// label after them is written when the GPU really got there. SOTC_VIS_PROGRAMS=hash,hash,...
/// replaces the default list.
inline const bool kVisDirect = [] {
    const char* v = std::getenv("SOTC_VIS_DIRECT");
    return v && v[0] == '1';
}();
inline bool VisDirectProgram(uint64_t hash) {
    static const std::vector<uint64_t> programs = [] {
        std::vector<uint64_t> out;
        const char* v = std::getenv("SOTC_VIS_PROGRAMS");
        std::string list = v ? v : "0x2e0f474b,0xf94d23d4,0x93aec24b,0x729c541e";
        size_t pos = 0;
        while (pos < list.size()) {
            const size_t comma = list.find(',', pos);
            const std::string item = list.substr(pos, comma == std::string::npos ? comma : comma - pos);
            if (const uint64_t h = std::strtoull(item.c_str(), nullptr, 0)) {
                out.push_back(h);
            }
            if (comma == std::string::npos) {
                break;
            }
            pos = comma + 1;
        }
        return out;
    }();
    return std::find(programs.begin(), programs.end(), hash) != programs.end();
}
/// sotc: SOTC_TEX_DIRECT=1 (needs SOTC_PC_MEMORY=1): textures uploaded from the game's memory
/// are copied by the GPU straight from the imported backing view, with no CPU copy into a staging
/// buffer (the first-entry hitch: 3-18 ms per draw of memcpy and page faults on the draw thread).
inline const bool kTexDirect = [] {
    const char* v = std::getenv("SOTC_TEX_DIRECT");
    return v && v[0] == '1';
}();
inline std::atomic<uint64_t> tex_direct_uploads{0}, tex_direct_bytes{0}, tex_direct_fallbacks{0};

/// A dispatch of one of those wrote the game's memory directly since the last label: the next
/// end-of-pipe label waits for the GPU (draw thread only).
inline bool vis_label_pending = false;
inline std::atomic<uint64_t> vis_direct_binds{0}, vis_honest_labels{0}, vis_fallbacks{0};

/// sotc: SOTC_SLOW_OPS=1 (with SOTC_DRAW_SPLIT): the draw thread's operations of each frame summed
/// by shader (kind and first stage's hash; Liverpool::BackLoop), and for each frame over 40 ms the
/// 8 costliest printed with the window ("SOTC SLOW OPS" lines, videoout/driver.cpp).
inline const bool kSlowOps = [] {
    const char* v = std::getenv("SOTC_SLOW_OPS");
    return v && v[0] == '1';
}();
struct SlowOp {
    uint64_t ticks;
    uint32_t count;
};
inline std::mutex slow_ops_mutex;
inline std::unordered_map<uint64_t, SlowOp> slow_ops; ///< key: kind << 60 | pgm hash (60 bits)
/// TSC ticks per millisecond, measured once against the steady clock (diagnostics only).
inline double TicksPerMs() {
    static const double value = [] {
        const uint64_t t0 = __builtin_ia32_rdtsc();
        const auto c0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - c0 < std::chrono::milliseconds(20)) {
        }
        const uint64_t t1 = __builtin_ia32_rdtsc();
        return double(t1 - t0) /
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0)
                   .count();
    }();
    return value;
}
/// SOTC_SLOW_OPS: inside the end-of-pipe label ops of the frame (ticks): OnFence, the label's
/// invalidate, the deferred label's flush; and labels deferred (a count).
enum EopPart { EopFence, EopInvalidate, EopFlush, EopDeferred, EopLabels, EopParts };
inline std::atomic<uint64_t> eop_ticks[EopParts]{};
struct EopTimer {
    EopPart part;
    uint64_t start;
    explicit EopTimer(EopPart p) : part{p}, start{kSlowOps ? __builtin_ia32_rdtsc() : 0} {}
    ~EopTimer() {
        if (kSlowOps) {
            eop_ticks[part].fetch_add(__builtin_ia32_rdtsc() - start, std::memory_order_relaxed);
        }
    }
};
inline void NoteSlowOp(uint64_t key, uint64_t ticks) {
    std::scoped_lock lock{slow_ops_mutex};
    SlowOp& op = slow_ops[key];
    op.ticks += ticks;
    ++op.count;
}
enum SplitPart {
    SplitDraw, SplitIndirect, SplitDispatch, SplitFunc, SplitCommands, // draw thread operations
    SplitBind, SplitBindBuffers, SplitBindTextures, SplitBindDma,      // inside BindResources
    SplitTexRefresh, SplitTexHash, SplitTexStaging, SplitTexCopy,      // TextureCache::RefreshImage
    SplitTexDetile, SplitTexRecord,
    SplitWaitGpu, SplitWaitSubmit, SplitWaitRecorder, SplitWaitFrame,  // draw thread blocked
    SplitIdle, SplitIdleSpin,                                          // draw thread out of work
    SplitRbQueue, SplitRbSubmit, SplitRbGpu, SplitRbFinish,            // guest readback waits
    SplitRbCount, ///< guest readback waits (a count, not ticks)
    /// Texture copies from guest memory on the draw thread (counts, not ticks): bytes, copies,
    /// copies of 2 MiB and more (split across the copy threads), process page faults meanwhile.
    SplitTexBytes, SplitTexCopies, SplitTexBig, SplitTexFaults,
    /// Inside the binds: buffer range clamp (memory table lookup), buffer cache lookup, texture
    /// descriptor decode (T# -> image/view info), image lookup, view lookup.
    SplitBufClamp, SplitBufObtain, SplitTexDesc, SplitTexFind, SplitTexView,
    /// Guest readbacks on the draw thread: begin (gather the ranges, record the copies), its
    /// submit, finish (apply to guest memory, heap guard included).
    SplitRbBegin, SplitRbBeginFlush, SplitRbApply,
    SplitRbDirty,   ///< inside the apply: a dirty readback read back again on the draw thread
    SplitRbRetries, ///< dirty readbacks handed back to their guest thread (a count)
    /// Inside the texture view lookup (FindTexture): the texture cache lock wait, page tracking
    /// of the image (TrackImage), the view itself (Image::FindView).
    SplitTexLock, SplitTexTrack, SplitTexMakeView,
    SplitParts
};
inline std::atomic<uint64_t> split_ticks[SplitParts]{};
/// Deepest draw thread queue (back_head - back_tail at a publish) since the last flip.
inline std::atomic<uint64_t> split_queue_max{0};
/// Set by Liverpool::BackLoop: the waits count only on the draw thread.
inline thread_local bool t_draw_thread = false;
inline uint64_t Ticks() {
    return __builtin_ia32_rdtsc();
}
struct SplitTimer {
    SplitPart part;
    bool on;
    uint64_t start;
    explicit SplitTimer(SplitPart p, bool draw_thread_only = true)
        : part{p}, on{kDrawSplit && (!draw_thread_only || t_draw_thread)}, start{on ? Ticks() : 0} {}
    ~SplitTimer() {
        if (on) {
            split_ticks[part].fetch_add(Ticks() - start, std::memory_order_relaxed);
        }
    }
};
enum SplitThread { SplitThreadDraw, SplitThreadCp, SplitThreadRecorder, SplitThreads };
/// The calling thread is that one (its CPU time is read by SplitThreadCycles); sotc_stats.cpp.
void RegisterSplitThread(SplitThread which);
/// CPU time of a registered thread in TSC ticks (QueryThreadCycleTime), 0 if not registered.
uint64_t SplitThreadCycles(SplitThread which);
/// Page faults of the whole process so far (GetProcessMemoryInfo), 0 where unknown.
uint64_t SplitProcessFaults();
/// Commit charge and free RAM, to stderr (session log); sotc_stats.cpp.
void LogSystemMemory();
/// A texture copy of `bytes` from guest memory: its wall time goes to SplitTexCopy and the counts
/// to SplitTexBytes..SplitTexFaults (draw thread only, SOTC_DRAW_SPLIT).
struct SplitTexCopyScope {
    bool on;
    uint64_t start, faults;
    explicit SplitTexCopyScope(uint64_t bytes) : on{kDrawSplit && t_draw_thread} {
        start = on ? Ticks() : 0;
        faults = on ? SplitProcessFaults() : 0;
        if (on) {
            split_ticks[SplitTexBytes].fetch_add(bytes, std::memory_order_relaxed);
            split_ticks[SplitTexCopies].fetch_add(1, std::memory_order_relaxed);
            if (bytes >= (2u << 20)) {
                split_ticks[SplitTexBig].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    ~SplitTexCopyScope() {
        if (on) {
            split_ticks[SplitTexCopy].fetch_add(Ticks() - start, std::memory_order_relaxed);
            split_ticks[SplitTexFaults].fetch_add(SplitProcessFaults() - faults,
                                                  std::memory_order_relaxed);
        }
    }
};
/// SOTC_CMD_STATS=1: draws, indirect draws (arguments GPU-written / CPU-side / CPU-side and
/// empty), dispatches, indirect dispatches.
inline std::atomic<uint64_t> cmd_kinds[6]{};
inline std::atomic<uint64_t> early_flushes{0}; ///< submits right after a write to a read-back page
/// Suspects of long frames, printed for each frame over SOTC_STALL_MS (SOTC_FRAME_LOG runs):
/// pipelines created, shaders recompiled and images created during the frame, and their time.
inline std::atomic<uint64_t> pipelines_new{0}, pipeline_ns{0};
/// sotc: texture bind memo miss causes (SOTC_FPS): the slot holds another stage/T# set, the set
/// matched but the GPU-modified epoch moved, an image's address generation moved, the replay asked
/// for a re-bind, and stages the memo does not cover (arrays, mip fallback, too many images).
inline std::atomic<uint64_t> tbm_miss_key{0}, tbm_miss_epoch{0}, tbm_miss_gen{0}, tbm_miss_replay{0},
    tbm_not_plain{0};
/// sotc: read-only formatted buffer bindings that reached SynchronizeMemoryFromImage (SOTC_FPS), the
/// ones on a metadata surface, and the ones with an image behind them.
inline std::atomic<uint64_t> fmt_sync_calls{0}, fmt_sync_meta{0}, fmt_sync_image{0};
/// sotc: BbToggle::PipelineKeyMemo: draws that returned the previous pipeline without building
/// the key again, and draws that had to build it.
inline std::atomic<uint64_t> pipe_key_memo_hits{0}, pipe_key_memo_misses{0};
/// sotc: draws whose freshly built graphics key equaled the previous one (SOTC_PIPE_MEMO), the
/// upper bound of what a register-content memo could skip.
inline std::atomic<uint64_t> pipe_same_key_hits{0}, pipe_same_key_misses{0};
/// sotc: BbToggle::DrawBindMemo: stage texture bindings replayed from the memo, and plain stages
/// that had to be resolved (first sight, changed registry, re-bind).
inline std::atomic<uint64_t> bind_memo_hits{0}, bind_memo_misses{0};
/// sotc: BbToggle::BufferBindMemo: read-only buffer bindings answered from the memo, and those
/// that took the full path (SOTC_CMD_STATS).
inline std::atomic<uint64_t> buffer_memo_hits{0}, buffer_memo_misses{0};
/// ... and the render target resolution of PrepareRenderState replayed / done.
inline std::atomic<uint64_t> rt_memo_hits{0}, rt_memo_misses{0};
/// sotc: time (ns) and draws of RefreshGraphicsKey (the part PipelineKeyMemo removes).
inline std::atomic<uint64_t> pipe_key_ns{0}, pipe_key_draws{0};
inline std::atomic<uint64_t> shaders_compiled{0}, shader_ns{0};
inline std::atomic<uint64_t> images_new{0}, image_ns{0};
/// Parts of an image creation: format query, memory allocation (and how many got memory of
/// their own instead of a block shared with others).
inline std::atomic<uint64_t> image_query_ns{0}, image_alloc_ns{0}, image_dedicated{0};
/// Video memory: usage the driver reports (texture cache GC), budget the GC works with, and the
/// sparse buffer arena memory (never released) as allocations and bytes.
/// SOTC_RELEASE_DEFER: unmap releases queued, released after the GPU, released at once (a bind
/// used the blocks again), and the blocks still waiting (a gauge).
inline std::atomic<uint64_t> release_queued{0}, release_done{0}, release_forced{0},
    release_pending_blocks{0};
inline std::atomic<uint64_t> vram_used{0}, vram_budget{0}, arena_allocs{0}, arena_bytes{0};
/// sotc: host (not device-local) heaps: usage reported by the driver, and VMA's blocks in them.
inline std::atomic<uint64_t> host_heap_usage{0}, host_vma_block_bytes{0};
/// Texture cache collector: ticks (one per submit with a frame's work), images freed and those
/// written back to memory first, and the images in the cache with their (guest) size.
inline std::atomic<uint64_t> gc_ticks{0}, gc_freed{0}, gc_downloads{0};
inline std::atomic<uint64_t> cached_images{0}, cached_image_bytes{0};
inline std::atomic<uint64_t> vram_emergency{0}; ///< emergency texture collections (allocation failed)
inline std::atomic<uint64_t> write_fault_gpu_pending{0}; ///< write-fault readbacks left protected
inline std::atomic<uint64_t> write_fault_stale_gpu{0};   ///< ... that cleared a stale GPU page bit
inline std::atomic<uint64_t> read_fault_stale_gpu{0};    ///< sotc: the same for a read fault
inline std::atomic<uint64_t> read_fault_out_of_step{0};  ///< sotc: host protection without a watcher
inline std::atomic<uint64_t> arena_evicted{0};  ///< arena bytes given back by MaybeEvictCold
inline std::atomic<uint64_t> arena_rebound{0};  ///< ... bound again afterwards
inline std::atomic<uint64_t> arena_pinned{0};   ///< ... faulted back in by shaders (kept since)
inline std::atomic<uint64_t> arena_evict_mismatch{0}; ///< read back unequal to guest memory: kept
inline std::atomic<uint64_t> arena_evict_runs{0};
inline std::atomic<uint64_t> arena_evict_ns{0}; ///< time in MaybeEvictCold (scan, GPU wait, unbind)
// sotc (MEMORIA-PLAN.md): memory counters printed on the "SOTC MEM" line (SOTC_FPS=1). Telemetry only.
/// Arena bytes in use (ArenaUsedBytes) and by class of the blocks' binds: image sources only
/// (ObtainBufferForImage), other binds only, both, and "image only and idle for 30 s".
inline std::atomic<uint64_t> arena_used{0}, arena_img_only{0}, arena_other_only{0}, arena_mixed{0},
    arena_img_idle{0}, arena_other_idle{0}; ///< other-only blocks idle 30 s (not exact to give back)
inline std::atomic<uint64_t> arena_img_released{0}; ///< image-source arena bytes given back (ArenaImageRelease)
inline std::atomic<uint64_t> arena_img_runs{0};
/// Images with more than one backing VkImage (MSAA switch) and the bytes of the non-current ones.
inline std::atomic<uint64_t> img_multi_count{0}, img_multi_bytes{0};
/// Cached images by idle time (<10 s, <30 s, <60 s, <120 s, <300 s, >=300 s): CPU-origin ones that
/// the cold sweep may free, and the rest (GPU-written or render targets), count and host bytes.
inline constexpr int kIdleBuckets = 6;
inline std::atomic<uint64_t> img_idle_cpu_n[kIdleBuckets]{}, img_idle_cpu_b[kIdleBuckets]{};
inline std::atomic<uint64_t> img_idle_gpu_n[kIdleBuckets]{}, img_idle_gpu_b[kIdleBuckets]{};
/// Cold image sweep (ColdImageSweep): images and host bytes freed, images created again at the
/// address of a swept one (recreated: ever, within 10 s), time in the sweep passes.
inline std::atomic<uint64_t> sweep_freed{0}, sweep_bytes{0}, sweep_recreated{0},
    sweep_recreated_fast{0}, sweep_recreated_bytes{0}, sweep_ns{0}, sweep_passes{0};
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
/// sotc: how long flips waited between the submit and the present thread taking them (input delay).
inline std::atomic<uint64_t> flip_age_ns{0};
inline std::atomic<uint64_t> flip_age_max_ns{0};
inline std::atomic<uint64_t> flip_age_count{0};
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
enum AllocKind {
    AllocArena,
    AllocStagingBlock,
    AllocStagingLarge,
    AllocStagingBlockDevice, ///< ring blocks in video memory (the detiler's staging)
    AllocStagingKept,        ///< ring blocks of the kept reserve, made ahead (one per frame)
    AllocKinds
};
inline constexpr const char* kAllocKindNames[AllocKinds] = {
    "arena", "staging-block", "staging-large", "staging-block-vram", "staging-reserve"};
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

/// sotc: SOTC_TICKWATCH=1 -- a watcher thread on the draw scheduler samples its timeline
/// semaphore (vk_scheduler.cpp StartTickWatch); submits and GPU progress land in this ring and
/// the FLIPGAP detector (videoout/driver.cpp) prints what the timeline did during a gap.
/// Diagnostic: off by default, never on in benchmarks.
inline const bool kTickWatch = [] {
    const char* v = std::getenv("SOTC_TICKWATCH");
    return v && v[0] == '1';
}();
struct TickEvent {
    int64_t t_ns;  ///< steady_clock time_since_epoch (NowNs)
    uint64_t gpu;  ///< kind 0: known GPU tick; kind 1/2: the tick submitted
    uint64_t sub;  ///< kind 0: newest submitted tick; kind 1: (wait semas << 8) | cmd buffers;
                   ///< kind 2: (submit_mutex wait us << 32) | vkQueueSubmit us
    uint32_t kind; ///< 0 progress sample, 1 SubmitExecution, 2 put on the Vulkan queue
};
inline constexpr size_t kTickRing = 8192;
inline TickEvent tick_ring[kTickRing];
inline std::atomic<uint64_t> tick_ring_head{0};
inline void PushTick(uint32_t kind, uint64_t gpu, uint64_t sub) {
    const uint64_t i = tick_ring_head.fetch_add(1, std::memory_order_relaxed);
    tick_ring[i % kTickRing] = {static_cast<int64_t>(NowNs()), gpu, sub, kind};
}

/// sotc: SOTC_STALL_LOG[=<ms>] (default 40 ms): diagnostic of slow frames (driver.cpp Flip). The sites that
/// can stall the frame (buffer uploads, arena/staging allocations, texture uploads, fence waits,
/// readbacks) note an event with its details while the switch is on; at each flip the events of the
/// frame are taken and, if the frame took longer than the threshold, the biggest ones are printed
/// ("SOTC STALL" lines). Off: one predictable branch per site, no clock read.
inline const double kStallLogMs = [] {
    const char* v = std::getenv("SOTC_STALL_LOG");
    if (!v || v[0] == '\0' || v[0] == '0') {
        return 0.0;
    }
    const double ms = std::atof(v);
    return ms > 1.0 ? ms : 40.0;
}();
// (own initializer: dynamic initialization of inline variables of one header is not ordered
// between translation units, so this must not read kStallLogMs.)
inline const bool kStallLog = [] {
    const char* v = std::getenv("SOTC_STALL_LOG");
    return v && v[0] != 0 && !(v[0] == 48 && v[1] == 0);
}();
struct StallEvent {
    double ms;
    std::string text;
};
inline std::mutex stall_mutex;
inline std::vector<StallEvent> stall_events;
/// Call sites are logged as signed distances from this function (its address is printed on the
/// first slow frame), which is stable across runs and resolved with a symbol listing.
inline void StallAnchor() {}
inline thread_local const void* stall_caller{}; ///< SOTC_STALL_LOG: the caller of the last ObtainBuffer
inline long long StallOffset(const void* address) {
    return address ? static_cast<long long>(static_cast<const char*>(address) -
                                            reinterpret_cast<const char*>(&StallAnchor))
                   : 0LL;
}
inline std::atomic<uint64_t> fence_wait_ns{0}; ///< SOTC_STALL_LOG: time in blocking fence waits
inline void StallNote(double ms, const char* fmt, ...) {
    if (ms < 0.2) {
        return;
    }
    char text[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    std::scoped_lock lock{stall_mutex};
    if (stall_events.size() < 4096) {
        stall_events.push_back({ms, text});
    }
}
/// Per-frame totals of the stall sites (every call counts, even those too short for an event):
/// kind (what), key (program hash or image address), calls, time, megabytes, and two phase times.
enum StallKind { StallBufferUpload, StallTextureUpload, StallStagingAlloc, StallReadback,
                 StallGpuWait, StallGuestCopyWait, StallKinds };
inline constexpr const char* kStallKindNames[StallKinds] = {"buffer-upload", "texture-upload",
                                                            "staging-alloc", "readback", "gpu-wait", "guest-copy-wait"};
struct StallAggRow {
    int kind;
    unsigned long long key;
    uint64_t calls;
    double ms, mb, a, b;
};
inline std::vector<StallAggRow> stall_agg;
inline void StallAgg(int kind, unsigned long long key, double ms, double mb, double a = 0.0,
                     double b = 0.0) {
    std::scoped_lock lock{stall_mutex};
    for (auto& row : stall_agg) {
        if (row.kind == kind && row.key == key) {
            ++row.calls;
            row.ms += ms;
            row.mb += mb;
            row.a += a;
            row.b += b;
            return;
        }
    }
    stall_agg.push_back({kind, key, 1, ms, mb, a, b});
}
inline std::vector<StallEvent> StallTake(std::vector<StallAggRow>* agg = nullptr) {
    std::scoped_lock lock{stall_mutex};
    std::vector<StallEvent> taken;
    taken.swap(stall_events);
    if (agg) {
        agg->swap(stall_agg);
    }
    stall_agg.clear();
    return taken;
}
/// ms since `start` (steady clock); the callers read the clock only with kStallLog.
inline double StallMsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

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

/// sotc: one slow wait of the guest memory lock, called by lock_backoff (src/runtime_memory.c):
/// `draw` when the waiting thread is the GPU draw thread, `ns` what the wait lasted. A C-linkage
/// inline definition in this header keeps the counters here for the stats printers (and above any
/// test stub): `used` makes every including object emit it, and the runtime (C) links against it.
extern "C" __attribute__((used)) inline void sotc_lock_slow_stat(int draw, uint64_t ns) {
    SotcStats::lock_slow_waits.fetch_add(1, std::memory_order_relaxed);
    SotcStats::lock_slow_ns.fetch_add(ns, std::memory_order_relaxed);
    if (draw) {
        SotcStats::lock_slow_waits_draw.fetch_add(1, std::memory_order_relaxed);
        SotcStats::lock_slow_ns_draw.fetch_add(ns, std::memory_order_relaxed);
    }
}
