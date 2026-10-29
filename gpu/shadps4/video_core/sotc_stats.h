// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: counters printed with the frame rate (SOTC_FPS=1, videoout/driver.cpp).
#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "video_core/sotc_trace.h" // sotc: SOTC_TRACE

namespace SotcStats {
/// sotc: the switches of the stats printers in videoout/driver.cpp (SOTC_FPS line; SOTC_CMD_STATS
/// lines inside it), read once at static initialisation, before any guest thread runs (no
/// function-local static guard and no getenv inside the vectored fault handler; same pattern as
/// BbStats::enabled). Counters that only one printer reads are bumped only when that printer is
/// on: commands (SOTC_FPS) and cmd_kinds, render_breaks, upload_bytes, fault_writes,
/// fault_walker, fault_gpu_reads, fault_granule_pages (SOTC_CMD_STATS). By default the per-draw lock xadds and the
/// guest-thread fault counter (which shared cache lines with CP counters) are gone; with the
/// switches on, the printed numbers are unchanged (the printer reads and resets each counter with
/// exchange(0) only under the same switch). The variables are never set at run time.
// sotc: SOTC_TRACE=1 (sotc_trace.h) needs the same timers.
inline const bool kFpsLog = std::getenv("SOTC_FPS") != nullptr || [] {
    const char* v = std::getenv("SOTC_TRACE");
    return v && v[0] == '1';
}();
inline const bool kCmdStats = std::getenv("SOTC_CMD_STATS") != nullptr;
/// sotc: SOTC_STALL_DETAIL=1 (off by default): the SOTCSTALL line also shows the flip pacing of the
/// frame, the guest threads' CPU and the user/kernel CPU of the upload threads (vk_presenter.cpp).
inline const bool kStallDetail = [] {
    const char* v = std::getenv("SOTC_STALL_DETAIL");
    return v && v[0] == '1';
}();
/// sotc: SOTC_FPS_LEAN=1 (off by default): SOTC_FPS keeps its per-frame summary and counters
/// but drops the per-op work that only feeds detailed reports: the per-op/per-wait clock reads
/// (kFpsTimers) and the per-binding write-extent marks (kWriteMarks, the "SOTC GPU writes by
/// shader" lines). Unset: both follow kFpsLog exactly as before. kFpsTimers/kWriteMarks read
/// kFpsLog and kFpsLean: inline variables defined earlier in this same header, which every
/// translation unit sees in this order, so they are initialized first (partial ordering).
inline const bool kFpsLean = [] {
    const char* v = std::getenv("SOTC_FPS_LEAN");
    return v && v[0] == '1';
}();
inline const bool kFpsTimers = kFpsLog && !kFpsLean;
/// sotc: SOTC_CEILING=<sync,tex> (off by default): MEASUREMENT ONLY, the picture is wrong. Skips
/// a whole part of the draw thread's work to show how much faster it would be if that part cost
/// nothing: "sync" skips the buffer memory synchronization (uploads of CPU-written bytes and the
/// re-protection of their pages; residency, labels and command order are kept), "tex" skips the
/// texture uploads. Never play with it: the GPU reads stale data and readbacks return stale bytes.
enum CeilingPart : unsigned { CeilingSync = 1, CeilingTex = 2 };
inline const unsigned kCeiling = [] {
    const char* v = std::getenv("SOTC_CEILING");
    unsigned parts = 0;
    if (v) {
        const auto has = [v](const char* word) {
            const char* p = std::strstr(v, word);
            return p != nullptr;
        };
        parts |= has("sync") ? CeilingSync : 0u;
        parts |= has("tex") ? CeilingTex : 0u;
        if (parts) {
            std::fprintf(stderr, "SOTC_CEILING=%s: measurement only, the picture is wrong%c", v,
                         10);
        }
    }
    return parts;
}();
inline const bool kWriteMarks = kFpsLog && !kFpsLean;
/// sotc: skip BindResources on consecutive draws whose pipeline, push data and resource
/// handles did not change. On by default (SOTC_FAST_BIND=0 turns it off). In 0.60.5 it left black
/// dots in a grid on sandy ground (new constants at the same address); with the fix (mem_epoch
/// below) the sand under the title demo's bridge is clean (attract shots, 23 Oct). Shrine idle
/// 33.5 -> 35.3 FPS, title demo 50.6 -> 52.2.
inline const bool kFastBind = [] {
    const char* v = std::getenv("SOTC_FAST_BIND");
    return !v || v[0] != '0';
}();
/// sotc: SOTC_FASTBIND_WIDE=1 (default off): a reused bind is also taken in a later GPU submit
/// (the tick must differ). The lookups are skipped; push constants and descriptors are issued
/// again into the current command buffer (Rasterizer::BindResources, vk_rasterizer.cpp).
inline const bool kFastBindWide = [] {
    const char* v = std::getenv("SOTC_FASTBIND_WIDE");
    return v && v[0] == '1';
}();
/// sotc: SOTC_BIND_CACHE=1 (default off): a multi-entry cache of full binds of read-only graphics
/// draws, so a draw that repeats the state of an earlier draw in the same submit (not only the
/// one right before it, as SOTC_FAST_BIND) skips the buffer/texture lookups
/// (Rasterizer::TryBindCache, vk_rasterizer.cpp). Unset or 0: the bind path is unchanged.
inline const bool kBindCache = [] {
    const char* v = std::getenv("SOTC_BIND_CACHE");
    return v && v[0] == '1';
}();
/// sotc: SOTC_BIND_CACHE counters (SOTC_FPS only): eligible draws looked up (fast-bind hits are
/// not), hits, matches refused at validation (an image dirty, re-bound or a target), full binds
/// not stored (something a hit could not repeat), and flushes of a non-empty cache by reason.
inline std::atomic<uint64_t> bind_cache_lookups{0}, bind_cache_hits{0}, bind_cache_refused{0},
    bind_cache_uncacheable{0};
enum BindCacheFlushReason : uint32_t {
    BindCacheTick, BindCacheMemory, BindCacheResources, BindCacheTargets, BindCacheUnsafe,
    BindCacheReasons
};
inline constexpr const char* kBindCacheFlushNames[BindCacheReasons] = {
    "tick", "memory", "resources", "targets", "unsafe"};
inline std::atomic<uint64_t> bind_cache_flushes[BindCacheReasons]{};
/// sotc: SOTC_BIND_CACHE_RANGES=1 (default off; only with SOTC_BIND_CACHE=1): a dispatch, draw or
/// DMA that writes GPU memory drops only the cached binds that read what it writes
/// (Rasterizer::BindCacheInvalidate), and guest memory changes (mem_epoch) no longer flush the
/// cache: each hit already refuses CPU-modified buffer ranges and dirty images. Unset or 0: the
/// SOTC_BIND_CACHE flush rules are unchanged.
inline const bool kBindCacheRanges = [] {
    const char* cache = std::getenv("SOTC_BIND_CACHE");
    const char* v = std::getenv("SOTC_BIND_CACHE_RANGES");
    return cache && cache[0] == '1' && v && v[0] == '1';
}();
/// sotc: SOTC_BIND_CACHE_RANGES counters (SOTC_FPS only): entries dropped by written ranges, and
/// the writes checked against a non-empty cache.
inline std::atomic<uint64_t> bind_cache_range_entries{0}, bind_cache_range_writes{0};
/// sotc: SOTC_BIND_CACHE_XTICK=1 (default off; needs SOTC_BIND_CACHE=1 and SOTC_BIND_CACHE_RANGES=1):
/// cached binds survive the submit tick (the next hit of an entry in a new tick replays the
/// per-tick side effects a full bind has: image and sampler stamps, arena block stamps), the
/// buffer side of the validity checks follows BufferCache::BindVersion (removals and handle
/// changes only, not additions), and a CPU-modified buffer range is synchronized instead of
/// refusing the hit (Rasterizer::TryBindCache). Unset or 0: unchanged.
inline const bool kBindCacheXtick = [] {
    // sotc: reads every env it depends on itself instead of kBindCacheRanges: the dynamic
    // initialization order of inline variables across translation units is not reliable here,
    // and this switch came out false in every run that set it (no "xtick hits" in the logs).
    const auto on = [](const char* name) {
        const char* v = std::getenv(name);
        return v && v[0] == '1';
    };
    return on("SOTC_BIND_CACHE") && on("SOTC_BIND_CACHE_RANGES") && on("SOTC_BIND_CACHE_XTICK");
}();
/// sotc: SOTC_BIND_CACHE_XTICK counters (SOTC_FPS only): hits of entries made in an earlier tick,
/// hits that synchronized a CPU-modified range, first hits of an entry in a new tick (replays),
/// and lookups whose key was also seen in the previous game frame.
inline std::atomic<uint64_t> bind_cache_xtick_hits{0}, bind_cache_xtick_repaired{0},
    bind_cache_xtick_replays{0}, bind_cache_seen_prev_frame{0};
/// sotc: SOTC_BIND_CACHE_FRAME=1 (default off; needs SOTC_BIND_CACHE_XTICK=1, so also _RANGES and
/// SOTC_BIND_CACHE): the bind cache survives the frame. A table of 4096 entries keyed by pipeline,
/// FastBindHash and the render targets (not flushed when they change); an entry is checked at its
/// hit against what it depends on instead of being wiped by a global generation: the images
/// registered or unregistered over the ranges it looked up (TextureCache::ImageChangedSince), the
/// GPU writes to what it reads (RangeStamps), BufferCache::BindVersion and, with flat buffers, the
/// stream ring. SOTC_FPS counters: misses by reason (a lookup that found no usable entry), entries
/// dropped by reason, stores, hits that crossed a game frame, and the buffer binding memo.
/// Unset or 0: the SOTC_BIND_CACHE path is unchanged.
inline const bool kBindCacheFrame = [] {
    const auto on = [](const char* name) { // sotc: see kBindCacheXtick
        const char* v = std::getenv(name);
        return v && v[0] == '1';
    };
    return on("SOTC_BIND_CACHE") && on("SOTC_BIND_CACHE_RANGES") && on("SOTC_BIND_CACHE_XTICK") &&
           on("SOTC_BIND_CACHE_FRAME");
}();
/// sotc: SOTC_BUFFER_MEMO=1 (off by default): only the buffer binding memo of SOTC_BIND_CACHE_FRAME
/// (vk_rasterizer.cpp BufferMemo: a read-only V# of a read-only graphics bind reuses the arena and
/// offset ObtainBuffer gave for the same address, size and mapping while BindVersion is unchanged;
/// CPU-written ranges are still synchronized and the use stamps made). The rest of the frame-wide
/// bind cache (which measured worse) stays off. Needs SOTC_BIND_CACHE (the read-only eligibility).
inline const bool kBufferMemo = [] {
    const char* v = std::getenv("SOTC_BUFFER_MEMO");
    return v && v[0] == '1';
}();
enum BindFrameReason : uint32_t {
    BfNoEntry,   ///< no entry under the key
    BfFlush,     ///< made before a cache-wide flush (sampler destroyed, metadata clear)
    BfBuffers,   ///< BufferCache::BindVersion moved (an unbind, eviction or arena replacement)
    BfStream,    ///< the stream ring wrapped under an entry that holds a flat buffer copy
    BfWrites,    ///< a GPU write touched a range or image the entry reads
    BfImages,    ///< an image was registered/unregistered over a looked-up range
    BfMapping,   ///< a guest map/unmap with an entry that held a rejected (unmapped) T#
    BfRefused,   ///< the hit-time checks (dirty / re-bound image, mapped extent, sampler)
    BfReasons
};
inline constexpr const char* kBindFrameNames[BfReasons] = {"none", "flush",  "buffers", "stream",
                                                           "writes", "images", "mapping", "refused"};
inline std::atomic<uint64_t> bf_miss[BfReasons]{}, bf_drop[BfReasons]{};
inline std::atomic<uint64_t> bf_stored{0}, bf_replaced{0}, bf_not_stored_moved{0},
    bf_cross_frame_hits{0}, bf_narrowed{0};
/// sotc: SOTC_BIND_CACHE_FRAME buffer binding memo (BindBuffers): read-only V#s that skipped
/// ObtainBuffer, those that ran it, and the memo's repairs of CPU-modified ranges.
inline std::atomic<uint64_t> bf_memo_hits{0}, bf_memo_misses{0}, bf_memo_repaired{0};
/// sotc: SOTC_BIND_CACHE_FRAME resource identity key: flat buffer copies a hit made / found still
/// current, distinct keys looked up and game frames seen (distinct keys per frame: does the table
/// hold a frame's worth?), and the entries live in the table.
inline std::atomic<uint64_t> bf_flat_copied{0}, bf_flat_reused{0}, bf_distinct_keys{0},
    bf_frames{0}, bf_live{0};
/// sotc: SOTC_RS_MEMO=1 (default off): a draw into the same render targets as the draw before it,
/// in the render pass that draw began, takes that draw's render state instead of looking its
/// targets up again (Rasterizer::RsMemoPrepare / RsMemoBegin, vk_rasterizer.cpp). Unset or 0:
/// PrepareRenderState and BeginRendering are unchanged.
inline const bool kRsMemo = [] {
    const char* v = std::getenv("SOTC_RS_MEMO");
    return v && v[0] == '1';
}();
/// sotc: SOTC_RS_MEMO counters (SOTC_FPS only): draws looked up, hits, misses by reason, and full
/// passes not stored (a clear, a feedback loop).
enum RsMemoMissReason : uint32_t {
    RsMissNone, RsMissTick, RsMissResources, RsMissRegs, RsMissPass, RsMissImage, RsMissMeta,
    RsMissReasons
};
inline constexpr const char* kRsMemoMissNames[RsMissReasons] = {
    "empty", "tick", "resources", "regs", "pass", "image", "meta"};
inline std::atomic<uint64_t> rs_memo_lookups{0}, rs_memo_hits{0}, rs_memo_unstored{0};
inline std::atomic<uint64_t> rs_memo_misses[RsMissReasons]{};
/// sotc: SOTC_PARALLEL_RECORD=1 (default off): GpuPrep threads compute the register-only inputs
/// of the draws ahead of the draw thread (Liverpool::PrepLoop, amdgpu/draw_prep.h). Counters
/// (SOTC_FPS only): draws that took a prepared slot, draws computed inline because the slot was
/// not published yet, a mapping changed since, or the prep thread refused the draw; the largest
/// distance (ops) between the newest queued op and the slowest prep thread; the time the command
/// processor waited for a full ring because of the prep threads; SOTC_PR_VERIFY differences.
inline std::atomic<uint64_t> pr_prepared{0}, pr_not_ready{0}, pr_epoch{0}, pr_ineligible{0},
    pr_throttle_ns{0}, pr_verify_mismatches{0};
inline std::atomic<uint32_t> pr_lag_max{0};
/// sotc: SOTC_HOST_IMAGE_COPY=1 (default off): linear textures the GPU did not write are re-uploaded
/// by the host (vkCopyMemoryToImageEXT, VK_EXT_host_image_copy) instead of a staging copy in a
/// command buffer (TextureCache::RefreshImage, Runtime::TryHostImageCopy). Off as well when the
/// texture dump or replacement hook is set (they change the bytes after the upload) and with
/// SOTC_FASTBIND_WIDE=1 (a bind reused in a later submit is not seen by the image's tick).
inline const bool kHostImageCopy = [] {
    const char* v = std::getenv("SOTC_HOST_IMAGE_COPY");
    return v && v[0] == '1' && !std::getenv("SOTC_TEX_DUMP") && !std::getenv("SOTC_TEX_REPLACE") &&
           !(std::getenv("SOTC_FASTBIND_WIDE") && std::getenv("SOTC_FASTBIND_WIDE")[0] == '1');
    // sotc: reads SOTC_FASTBIND_WIDE itself (see kBindCacheXtick on initialization order).
}();
/// sotc: host uploads done, and candidates refused because the image was not idle on the GPU.
inline std::atomic<uint64_t> host_image_copies{0};
inline std::atomic<uint64_t> host_image_copy_busy{0};
inline std::atomic<uint64_t> fast_bind_eligible{0};
inline std::atomic<uint64_t> fast_bind_hits{0};
inline std::atomic<uint64_t> fast_bind_wide_hits{0}; ///< SOTC_FASTBIND_WIDE: reused in a later submit
/// sotc: eligible draws that rebuilt their bindings (SOTC_FPS only, BindResources): all misses,
/// and the misses where only the submit tick differed (the state was otherwise reusable).
inline std::atomic<uint64_t> fast_bind_misses{0};
inline std::atomic<uint64_t> fast_bind_tick_misses{0};
/// sotc: bumped whenever guest memory changes outside the GPU's own draws: a write by the command
/// processor (constant RAM dumps, WRITE_DATA, DMA: MemoryManager::NoteHostWrite) or a CPU write
/// the page tracking sees (MemoryTracker). A reused bind (SOTC_FAST_BIND) needs it unchanged: the
/// next draw may have the same descriptors over new constants at the same address, and skipping
/// BindResources skipped their upload (black dots in a grid on the sand, user, 0.60.5).
inline std::atomic<uint64_t> mem_epoch{0};
/// sotc: SOTC_SYNC_FAST=1 (default off). BufferCache::SynchronizeMemory runs for every buffer of
/// every draw and dispatch and scans the page bits of the range even when nothing changed since
/// that range was last synchronized. With it on, each RegionManager keeps a write generation per
/// 2 MiB (RegionManager::sync_gen, bumped after any CPU bit rises or GPU bit falls), and a range
/// found clean after a sync is skipped in O(1) while the generations of its granules have not
/// moved (BufferCache::SynchronizeMemory). Unset or 0: the generations are never bumped and the
/// sync path is unchanged. Read at static initialisation (the bump runs in the fault handler).
inline const bool kSyncFast = [] {
    const char* v = std::getenv("SOTC_SYNC_FAST");
    return v && v[0] == '1';
}();
/// sotc: SOTC_SYNC_FAST counters (SOTC_FPS only): syncs looked up, syncs skipped.
inline std::atomic<uint64_t> sync_fast_lookups{0}, sync_fast_skipped{0};
/// sotc: SOTC_PROTECT_BATCH=1 (default off). The re-protects of one buffer upload
/// (BufferCache::SynchronizeMemoryFull: CPU bits cleared, pages write-protected again) are
/// collected per thread and applied together before the upload reads guest memory: adjacent runs,
/// also across 16 MiB tracking regions, become one host protect, and pages whose tracker state
/// changed meanwhile are skipped (PageManager::ProtectBatch). Unset or 0: every update protects
/// at once, as before.
inline const bool kProtectBatch = [] {
    const char* v = std::getenv("SOTC_PROTECT_BATCH");
    return v && v[0] == '1';
}();
/// sotc: SOTC_PROTECT_BATCH counters (SOTC_FPS only): host protect calls the flushes made and the
/// pages they covered, protect requests collected (one per call made without batching), pages
/// skipped because their state had moved on.
inline std::atomic<uint64_t> protect_batch_calls{0}, protect_batch_pages{0},
    protect_batch_requests{0}, protect_batch_redundant{0};
/// sotc: SOTC_PROTECT_COALESCE=1 (default off). Two exact reductions of host protect calls (page_manager.cpp):
///  1. A guest write fault collects the unprotects its tracker updates make (the faulting page, the
///     fault granule, the write-ahead, an image's pages) and applies them as one call per run when
///     the handler is done, instead of one call per update.
///  2. The flush of a ProtectBatch or of a fault bridges holes of up to SOTC_PROTECT_GAP pages
///     (default 8) between runs of the same protection when the pages in the hole are in that
///     protection already (checked under their page locks).
/// See PageManager::FaultProtectScope for why neither can hide a write from the GPU.
inline const bool kProtectCoalesce = [] {
    const char* v = std::getenv("SOTC_PROTECT_COALESCE");
    return v && v[0] == '1';
}();
/// sotc: SOTC_PROTECT_COALESCE counters (SOTC_FPS only): unprotect runs collected by faults, host
/// calls and pages the fault flushes made, pages skipped because their state had moved on, and
/// faults that found their own page not covered by their runs (another thread's flush pending).
inline std::atomic<uint64_t> coalesce_requests{0}, coalesce_calls{0}, coalesce_pages{0},
    coalesce_skipped{0}, coalesce_self_fixes{0};
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
inline std::atomic<uint64_t> readback_dirty{0}; ///< sotc: async readbacks redone synchronously (stale)
/// sotc: in-flight readbacks made inexact, by reason (BufferCache::StaleReason: unmap, another
/// readback applied, a later GPU write, a command processor write), and the ones applied anyway
/// with the later GPU write's parts kept GPU-modified (SOTC_RB_KEEP_LATER).
inline std::atomic<uint64_t> readback_stale[4]{};
inline std::atomic<uint64_t> readback_kept_later{0};
/// sotc: SOTC_FPS only: guest threads' wall-clock time in asynchronous readbacks that waited
/// (BufferCache::ReadMemory, from the Begin request to the Finish return), since the last report:
/// count, total, longest, and the parts waiting for the submit (recording thread) and the GPU.
/// rb_guest_all_* are the same totals, never reset (FrameAccount, SOTCSTALL lines).
inline std::atomic<uint64_t> rb_guest_count{0}, rb_guest_ns{0}, rb_guest_max_ns{0};
inline std::atomic<uint64_t> rb_guest_submit_ns{0}, rb_guest_gpu_ns{0};
inline std::atomic<uint64_t> rb_guest_all_ns{0}, rb_guest_all_count{0};
/// sotc: async readbacks whose copies read nothing the tick being recorded writes, with no
/// pending sparse binds (could take SOTC_RB_ASYNC_AHEAD); those that did (SOTC_RB_ASYNC_AHEAD=1),
/// their guest waits (counted when the guest finishes, with rb_guest_*), and the eligible ones
/// left on the usual path because no slot was free. Since the last report.
inline std::atomic<uint64_t> rb_ahead_eligible{0};
inline std::atomic<uint64_t> rb_ahead_async{0}, rb_ahead_async_ns{0}, rb_ahead_async_busy{0};
/// sotc: guest readbacks by the state of the tick that wrote their bytes last (write ring of
/// BufferCache): the GPU finished it / submitted but not finished / still being recorded; the sum of
/// their age in ticks (current tick - writer tick) and their count. Since the last report.
inline std::atomic<uint64_t> rb_writer_done{0}, rb_writer_sub{0}, rb_writer_unsub{0};
inline std::atomic<uint64_t> rb_writer_age_sum{0}, rb_writer_count{0};
/// sotc: SOTC_RB_TICK: copies done on the transfer queue, the guest waits for them (count, ns), and
/// the readbacks that took the usual path: all, no transfer queue, writer unknown, binds pending.
inline std::atomic<uint64_t> rb_tick_xfer{0}, rb_tick_waits{0}, rb_tick_wait_ns{0};
inline std::atomic<uint64_t> rb_tick_fb{0}, rb_tick_fb_noxfer{0}, rb_tick_fb_unknown{0},
    rb_tick_fb_binds{0};
/// sotc: SOTC_FPS only: the pattern of the guest readback requests (BufferCache::ReadMemory, async
/// path; "SOTC RB pattern" line), since the last report. Per guest thread, a request is "adjacent"
/// when its faulting address lies outside the same thread's previous readback window but within
/// 64 KiB after its end or before its start (_1ms: and it came at most 1 ms after the previous
/// one returned), "inside" when it lies in that window (pages the previous readback left
/// protected: owned by another readback, kept GPU-modified, or written by the GPU again).
/// "repeat": its base granule (the SOTC_RB_NARROW window, 512 KiB without it) was read back in
/// the previous game frame (flip_count) too. waited: requests that waited for the GPU.
/// asked / window / downloaded: bytes the fault asked for (<= 8, the fault handler does not
/// decode the access size), of the windows, and of GPU-modified data actually copied.
/// rb_pat_adjacent_all: adjacent requests, never reset (FrameAccount, SOTCSTALL "rb adj").
inline std::atomic<uint64_t> rb_pat_count{0}, rb_pat_waited{0}, rb_pat_adjacent{0},
    rb_pat_adjacent_1ms{0}, rb_pat_inside{0}, rb_pat_repeat{0};
inline std::atomic<uint64_t> rb_pat_asked_bytes{0}, rb_pat_window_bytes{0},
    rb_pat_download_bytes{0};
inline std::atomic<uint64_t> rb_pat_adjacent_all{0};
/// sotc: SOTC_RB_GROW=1 (BufferCache::ReadMemory / BeginAsyncReadback), since the last report:
/// requests with a grown window; of those, the ones whose copies read bytes the tick being
/// recorded writes (current_writes: not eligible for SOTC_RB_ASYNC_AHEAD) and the same for base
/// windows; other base granules of the grown windows that held GPU-modified data (readbacks
/// saved, upper bound: each would have been a fault of its own if the guest reads it) and those
/// bytes. Counted with SOTC_FPS only.
inline std::atomic<uint64_t> rb_grow_grown{0}, rb_grow_cur_writes{0}, rb_base_cur_writes{0},
    rb_grow_extra_granules{0}, rb_grow_extra_bytes{0};
/// sotc: formats the distinct base granules and the top 5 granules by request count of the
/// current report period into `out` (at most out_size bytes, NUL-terminated) and starts a new
/// period. Defined in buffer_cache.cpp (the pattern table lives there).
void RbPatternTake(char* out, std::size_t out_size);
/// sotc: shader binaries of an older build replaced in the cache, and permutations left out of it
/// because their slot holds another current one (RegisterShaderBinary).
inline std::atomic<uint64_t> shader_cache_replaced{0};
inline std::atomic<uint64_t> shader_cache_left_out{0};
/// sotc: SOTC_RB_OWNER: readbacks that found part of their range downloaded by others, and the
/// bytes they left to them. SOTC_RB_BATCH: batched submits, and the submits they saved.
inline std::atomic<uint64_t> rb_owner_shared{0};
inline std::atomic<uint64_t> rb_owner_skipped_bytes{0};
/// sotc: SOTC_RB_PREFETCH ("SOTC RB prefetch" line), since the last report: prefetches recorded;
/// applied at a guest fault with their copies done (no GPU wait); still on the GPU at the fault
/// (the guest waited for their tick); freed unasked after SOTC_RB_PREFETCH_TICKS; freed because a
/// later write or unmap made them stale; windows not prefetched because of the count or staging
/// caps. rb_pf_consumed_all: never reset (FrameAccount, SOTCSTALL "rb pf").
inline std::atomic<uint64_t> rb_pf_issued{0}, rb_pf_consumed{0}, rb_pf_waited{0},
    rb_pf_expired{0}, rb_pf_stale{0}, rb_pf_capped{0};
inline std::atomic<uint64_t> rb_pf_consumed_all{0};
/// sotc: SOTC_RB_MATERIALIZE: prefetches applied without a guest fault, their bytes, and the ones
/// issued with ranges not all written by the GPU in this frame or the previous one (left to faults).
inline std::atomic<uint64_t> rb_mat_applied{0}, rb_mat_bytes{0}, rb_mat_not_fresh{0};
/// sotc: SOTC_RB_PREV: guest read faults served from the last applied version (and bytes
/// released), refused because the window is not rewritten every frame, refused because the
/// last applied version is too old. rb_prev_served_all: never reset (SOTCSTALL "prev").
inline std::atomic<uint64_t> rb_prev_served{0}, rb_prev_bytes{0}, rb_prev_not_producer{0},
    rb_prev_too_old{0};
inline std::atomic<uint64_t> rb_prev_served_all{0};
/// sotc: SOTC_RB_PREV: owed downloads recorded, and serves refused because a guest readback in
/// flight already downloads the pages.
inline std::atomic<uint64_t> rb_prev_owed{0}, rb_prev_busy{0};
/// sotc: SOTC_RB_EXACT ("SOTC RB exact" line), since the last report. served: guest read faults
/// answered without waiting for GPU work that was not done (served_dl: with a download of done
/// ranges only, the rest of served: nothing to copy). pages_owed: pages released with owed ranges;
/// owed_objs/owed_applied/owed_dropped/owed_bytes: owed downloads recorded, applied to guest
/// memory, dropped (nothing left to apply) and the bytes applied; owed_trimmed: bytes a later
/// write, unmap or guest write took out of an owed download. fb_*: faults that took the usual
/// readback, by reason (announced: a pending range already announced by a label; unknown: writer
/// not known precisely; cpu: page with the CPU bit; inflight: pages owned by a guest readback in
/// flight; other: write fault, no GPU range, caps). label_*: labels deferred by liverpool.cpp and
/// their deferral latency (RbExLabelDeferral). unsafe_announce / apply_wrongthread are
/// diagnostics that must stay 0.
inline std::atomic<uint64_t> rb_ex_served{0}, rb_ex_served_dl{0}, rb_ex_pages_owed{0},
    rb_ex_owed_objs{0}, rb_ex_owed_applied{0}, rb_ex_owed_dropped{0}, rb_ex_owed_bytes{0},
    rb_ex_owed_trimmed{0};
inline std::atomic<uint64_t> rb_ex_fb_announced{0}, rb_ex_fb_unknown{0}, rb_ex_fb_cpu{0},
    rb_ex_fb_inflight{0}, rb_ex_fb_other{0};
inline std::atomic<uint64_t> rb_ex_label_deferred{0}, rb_ex_label_defer_ns{0},
    rb_ex_label_defer_max_ns{0};
inline std::atomic<uint64_t> rb_ex_unsafe_announce{0}, rb_ex_apply_wrongthread{0};
inline std::atomic<uint64_t> rb_ex_fb_floor{0};
/// sotc: SOTC_WRITE_FAULT_NARROW: write faults narrowed to their pages, window bytes not read.
inline std::atomic<uint64_t> rb_wf_narrow{0}, rb_wf_saved{0};
inline std::atomic<uint64_t> rb_ex_apply_reentered{0}, rb_ex_settle_waits{0}, rb_ex_settle_ns{0};
/// sotc: SOTC_RB_EXACT label downloads: recorded, applied before a label, dropped (dirty or
/// unused; the pages stay protected) and the bytes applied.
inline std::atomic<uint64_t> rb_ex_ld_recorded{0}, rb_ex_ld_applied{0}, rb_ex_ld_dropped{0},
    rb_ex_ld_bytes{0};
/// sotc: SOTC_RB_EXACT: one deferred label was written ns after it was processed.
inline void RbExLabelDeferral(uint64_t ns) {
    rb_ex_label_deferred.fetch_add(1, std::memory_order_relaxed);
    rb_ex_label_defer_ns.fetch_add(ns, std::memory_order_relaxed);
    uint64_t seen = rb_ex_label_defer_max_ns.load(std::memory_order_relaxed);
    while (seen < ns && !rb_ex_label_defer_max_ns.compare_exchange_weak(
                            seen, ns, std::memory_order_relaxed)) {
    }
}
/// sotc: SOTC_HRR (dry run, buffer_cache/hrr.h): would-be promoted ranges and their readbacks.
/// hrr_faults_all is never reset (SOTCSTALL "hrr"); the others are reset by the SOTC HRR line.
inline std::atomic<uint64_t> hrr_ranges{0}, hrr_kib{0}, hrr_faults{0}, hrr_nowait{0},
    hrr_waited{0}, hrr_rb_ns{0}, hrr_rb_count{0}, hrr_demotions{0}, hrr_import_fail{0},
    hrr_faults_all{0};
inline std::atomic<uint64_t> rb_batch_submits{0};
inline std::atomic<uint64_t> rb_batch_saved{0};
inline std::atomic<uint64_t> readback_finish{0};  ///< downloads that flushed the current work
inline std::atomic<uint64_t> readback_finish_asked{0}; ///< ... for bytes the guest itself asked
inline std::atomic<uint64_t> heap_guard_builds{0}; ///< readbacks into the BPE heap
inline std::atomic<uint64_t> readback_copies{0};          ///< ranges downloaded
inline std::atomic<uint64_t> readback_copies_changed{0};  ///< ... whose bytes differed from memory
inline std::atomic<uint64_t> readback_bytes{0};
inline std::atomic<uint64_t> readback_bytes_changed{0};
/// sotc: BufferCache::ReleaseMemory's GPU sync (Finish, or Flush with SOTC_UNBIND_GPU_WAIT=1):
/// calls and time (SOTC_FPS only).
inline std::atomic<uint64_t> release_finish_count{0}, release_finish_ns{0};
/// sotc: time the guest thread waits in BufferCache::DropGpuModified (SOTC_DROP_ASYNC=0; 0 in
/// the default async mode). SOTC_FPS only.
inline std::atomic<uint64_t> drop_gpu_wait_ns{0};
/// sotc: SOTC_DROP_FAST: unmaps dropped on the guest thread, and those that still waited
/// (GPU-written pages in the range). Totals.
inline std::atomic<uint64_t> drop_fast{0};
/// sotc: SOTC_ASYNC_UPLOAD, totals: uploads copied by the copy threads and their bytes, the time
/// submits waited for them, the guest writes / unmaps / frees that waited for a job (the write
/// fence) and that time, and the large uploads kept on the draw thread (hot pages, no backing).
inline std::atomic<uint64_t> async_upload_jobs{0};
inline std::atomic<uint64_t> async_upload_bytes{0};
inline std::atomic<uint64_t> async_upload_submit_ns{0};
inline std::atomic<uint64_t> async_upload_fence_waits{0};
inline std::atomic<uint64_t> async_upload_fence_ns{0};
inline std::atomic<uint64_t> async_upload_kept{0};
/// sotc: SOTC_FENCE_PIECES (reset by each SOTC_FPS report): fence waits for single pieces and
/// their time, pieces the waiting thread copied itself, and guest read faults let through
/// without waiting while copies were pending.
inline std::atomic<uint64_t> fence_piece_waits{0}, fence_piece_ns{0};
inline std::atomic<uint64_t> fence_pieces_helped{0}, fence_read_skips{0};
/// sotc: SOTC_HOT_PAGES, totals: pages made sticky (writable for their region's lifetime) and the
/// ones refused at the cap.
inline std::atomic<uint64_t> hot_pages_marks{0};
inline std::atomic<uint64_t> hot_pages_cap_hits{0};
/// sotc: SOTC_HOT_PAGES adaptive cap (region_manager.h HotPages): times the cap grew / shrank.
inline std::atomic<uint64_t> hot_pages_cap_grows{0}, hot_pages_cap_shrinks{0};
inline std::atomic<uint64_t> async_texture_uploads{0}; ///< SOTC_ASYNC_TEXTURE_UPLOAD, total
inline std::atomic<uint64_t> image_pool_allocs{0}; ///< SOTC_IMAGE_POOL_MB: images from the pool
/// sotc: SOTC_CP_FLIP_PACE: command processor time spent waiting for a free presentation frame
/// before queueing a flip (since the last SOTC_FPS report).
inline std::atomic<uint64_t> cp_flip_pace_ns{0};
/// sotc: SOTC_STALL_DETAIL: the same pacing time, never reset (the report exchanges cp_flip_pace_ns).
inline std::atomic<uint64_t> cp_flip_pace_all_ns{0};
/// sotc: SOTC_HOST_IMPORT, totals: uploads copied by the GPU from imported guest memory, their
/// bytes and wait time, uploads that went back to the CPU path, chunks the driver refused, and
/// the chunks imported now (64 MiB each).
inline std::atomic<uint64_t> host_import_uploads{0};
inline std::atomic<uint64_t> host_import_bytes{0};
inline std::atomic<uint64_t> host_import_ns{0};
inline std::atomic<uint64_t> host_import_fallbacks{0};
inline std::atomic<uint64_t> host_import_refused{0};
inline std::atomic<uint64_t> host_import_live_chunks{0};
inline std::atomic<uint64_t> drop_slow{0};
/// SOTC_RB_NARROW: readback window (KiB, 0 = off), GPU-modified bytes of the 512 KiB window left
/// outside the narrow one (not downloaded nor unprotected), live hot readbacks (gauge).
inline std::atomic<uint64_t> rb_narrow_kib{0}, rb_narrow_kept_bytes{0}, rb_hot_entries{0};
inline std::atomic<uint64_t> uploads{0};        ///< SynchronizeMemory copies (CPU data to GPU)
/// SOTC_XFER_QUEUE: uploads copied on the transfer queue instead of the graphics queue.
inline std::atomic<uint64_t> xfer_copies{0};
/// DMA sweeps (SynchronizeDmaBuffers): count, resident ranges visited, sweeps that uploaded.
inline std::atomic<uint64_t> dma_sweeps{0}, dma_sweep_ranges{0}, dma_sweep_uploading{0};
/// ... merged runs visited, and time (ns) in all sweeps and in the ones that uploaded nothing.
inline std::atomic<uint64_t> dma_sweep_runs{0}, dma_sweep_ns{0}, dma_sweep_idle_ns{0};
inline std::atomic<uint64_t> upload_bytes{0};
/// sotc: SOTC_HASH_SMALL=<bytes> (default 0 = off, read once at static initialisation). A read-only
/// bind of at most that many bytes whose pages the tracker reports CPU-dirty is hashed (XXH3 of
/// the guest bytes) against the bytes last uploaded for the same bind range: equal bytes skip the
/// upload (BufferCache::SyncSmallByHash). Pages stay write-protected: a guest write still faults.
inline const uint64_t kHashSmall = [] {
    const char* v = std::getenv("SOTC_HASH_SMALL");
    return v ? std::strtoull(v, nullptr, 0) : uint64_t{0};
}();
/// sotc: hashed binds whose bytes were unchanged (upload skipped), hashed binds uploaded, and the
/// bytes of the skipped uploads. Printed with the SOTC_FPS stats (videoout/driver.cpp).
inline std::atomic<uint64_t> hash_small_hits{0};
inline std::atomic<uint64_t> hash_small_uploads{0};
inline std::atomic<uint64_t> hash_small_bytes{0};
inline std::atomic<uint64_t> render_breaks{0};  ///< EndRendering that closed a render pass
/// sotc: SOTC_PASS_SEC passes recorded on a secondary command buffer, and passes over the draw
/// threshold that stayed on the primary (a block the secondary cannot hold, or no pass format)
inline std::atomic<uint64_t> passes_secondary{0};
inline std::atomic<uint64_t> passes_fallback{0};
inline std::atomic<uint64_t> fault_writes{0};       ///< guest write faults on tracked pages
inline std::atomic<uint64_t> fault_walker{0};       ///< SRT walker loads emulated after a fault
inline std::atomic<uint64_t> fault_gpu_reads{0};    ///< other read faults on the command processor
/// sotc: SOTC_FAULT_GRANULE=<bytes> (16384 or 65536; unset or anything else = 4096, the page). A
/// guest write fault also invalidates the other pages of its aligned granule, so writes that
/// follow in the same granule do not fault again. Read once at static initialisation.
inline const uint64_t kFaultGranule = [] {
    const char* v = std::getenv("SOTC_FAULT_GRANULE");
    const uint64_t g = v ? std::strtoull(v, nullptr, 10) : 4096;
    return (g == 16384 || g == 65536) ? g : uint64_t{4096};
}();
/// sotc: pages invalidated beyond the faulting one by kFaultGranule (SOTC_CMD_STATS).
inline std::atomic<uint64_t> fault_granule_pages{0};
/// sotc: SOTC_FAULT_GRANULE pages not uploaded because the guest left them unchanged.
inline std::atomic<uint64_t> fault_granule_skipped{0};
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
/// loop around them, which spins while a queue waits). sotc: only resumes that made progress are
/// timed (one clock read each); resumes that only re-test a wait are cp_spin_ns.
inline std::atomic<uint64_t> draw_idle_ns{0};
inline std::atomic<uint64_t> cp_parse_ns{0};
inline uint64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
} ///< draws and dispatches recorded
/// sotc: SOTC_WRITE_AHEAD_ADAPT=1 (off by default; unset = the fixed write-ahead of
/// SOTC_WRITE_AHEAD_PAGES, 15 pages). A guest thread that keeps taking write faults right after
/// the extent its previous fault unprotected (a sequential writer: memcpy of a streamed block)
/// doubles its write-ahead window on every such fault, from 16 pages up to
/// SOTC_WRITE_AHEAD_MAX_PAGES (default 512 = 2 MiB), in its direction of travel; a fault that is
/// not adjacent, or later than SOTC_WRITE_AHEAD_ADAPT_US (default 1000) after the previous one
/// finished, resets it. The pages it marks are snapshot-hashed first (like the fault granule), so
/// those the guest never writes are not uploaded (BufferCache::DropUnchangedGranulePages).
inline const bool kWriteAheadAdapt = [] {
    const char* v = std::getenv("SOTC_WRITE_AHEAD_ADAPT");
    return v && v[0] == '1';
}();
inline const uint64_t kWriteAheadAdaptUs = [] {
    const char* v = std::getenv("SOTC_WRITE_AHEAD_ADAPT_US");
    const uint64_t us = v ? std::strtoull(v, nullptr, 10) : 1000;
    return us < 10 ? uint64_t{10} : (us > 1000000 ? uint64_t{1000000} : us);
}();
inline const uint32_t kWriteAheadMaxPages = [] {
    const char* v = std::getenv("SOTC_WRITE_AHEAD_MAX_PAGES");
    const uint64_t p = v ? std::strtoull(v, nullptr, 10) : 512;
    return static_cast<uint32_t>(p < 16 ? 16 : (p > 4096 ? 4096 : p));
}();
/// sotc: guest write fault accounting (SOTC_FPS; the "SOTC FAULTS" line in videoout/driver.cpp and
/// "faults" in the SOTCSTALL lines). A fault is timed from the entry of bbgpu_handle_fault to its
/// exit; inside it the time waiting for a region mutex, for the async upload fence and in the host
/// protect call is kept apart (the texture cache's own lock is not split out). Sequential means: the
/// same thread, right after the extent its previous fault unprotected (see FaultObserve).
namespace FaultStat {
inline std::atomic<uint64_t> count{0}, seq{0}, total_ns{0}, mutex_ns{0}, fence_ns{0}, protect_ns{0};
inline std::atomic<uint64_t> flush_ns{0}; ///< sotc: time in the readback of MemoryTracker::InvalidateRegion
inline std::atomic<uint64_t> ahead_pages{0};     ///< pages the write-ahead marked
inline std::atomic<uint64_t> ahead_unchanged{0}; ///< of those snapshotted, left unchanged at upload
inline std::atomic<uint64_t> all_count{0}, all_ns{0}; ///< never reset (FrameAccount)
inline constexpr unsigned kBuckets = 256;
inline std::atomic<uint64_t> hist[kBuckets]{}; ///< 4 buckets per power of two of the latency in ns
inline unsigned Bucket(uint64_t ns) {
    if (ns < 4) {
        return static_cast<unsigned>(ns);
    }
    const unsigned msb = 63 - static_cast<unsigned>(__builtin_clzll(ns));
    return (msb - 1) * 4 + static_cast<unsigned>((ns >> (msb - 2)) & 3);
}
/// Upper edge (ns) of a bucket.
inline uint64_t BucketUpper(unsigned idx) {
    if (idx < 4) {
        return idx + 1;
    }
    const unsigned msb = idx / 4 + 1;
    return static_cast<uint64_t>(5 + idx % 4) << (msb - 2);
}
struct Tl {
    bool active = false;   ///< inside bbgpu_handle_fault with SOTC_FPS on
    bool counted = false;  ///< the fault was a handled guest write fault
    bool sequential = false;
    uint64_t t0 = 0, mutex_ns = 0, fence_ns = 0, protect_ns = 0, flush_ns = 0;
    uint64_t cur_page = 0, last_page = 0, last_lo = 0, last_hi = 0, last_end_ns = 0;
    uint32_t window = 16; ///< pages, kWriteAheadAdapt
    int dir = 0;          ///< +1 forward, -1 backward, 0 none
};
inline thread_local Tl tl;
/// sotc: where a handled guest write fault spends its time, in stages that do not overlap (the
/// "SOTC FAULT SPLIT" line). Each stage keeps its own time: the mutex and host-protect time
/// measured inside it (tl.mutex_ns, tl.protect_ns) and the time of stages nested in it are taken
/// out, and those two are reported on their own.
///  entry    bbgpu_handle_fault entry up to the page manager's write-fault handler (dispatch,
///           without the async-upload fence wait, which is tl.fence_ns)
///  inval    Rasterizer::InvalidateMemoryOnGuestFault of the faulting page: the texture cache and
///           the buffer cache (readback not split out, see tl.flush_ns)
///  tc       the texture cache part of inval, when the rasterizer marks it (StageScope kStTc)
///  granule  SOTC_FAULT_GRANULE: the neighbouring pages' checks and invalidation
///  granhash ... of which snapshotting their bytes (BufferCache::NoteGranulePage)
///  ahead    write-ahead (fixed or SOTC_WRITE_AHEAD_ADAPT)
///  flush    SOTC_PROTECT_COALESCE: applying the collected unprotects
///  repair   RepairStuckWriteFault
enum Stage : unsigned { kStEntry, kStInval, kStTc, kStGranule, kStGranHash, kStAhead, kStFlush,
                        kStRepair, kStCount };
inline constexpr const char* kStageNames[kStCount] = {"entry",   "inval", "tc",    "granule",
                                                      "granhash", "ahead", "flush", "repair"};
inline std::atomic<uint64_t> stage_total_ns[kStCount]{};
struct StageTl {
    uint64_t ns[kStCount]{};
    uint64_t sum = 0; ///< all stage time kept so far (the nested stages of a scope are its delta)
};
inline thread_local StageTl stl;
/// Times a stage of the fault being handled on this thread (see above).
struct StageScope {
    unsigned stage;
    uint64_t start = 0, mutex0 = 0, protect0 = 0, sum0 = 0;
    bool on;
    explicit StageScope(unsigned s) : stage{s}, on{kFpsTimers && tl.active} { // sotc: SOTC_FPS_LEAN
        if (on) {
            start = NowNs();
            mutex0 = tl.mutex_ns;
            protect0 = tl.protect_ns;
            sum0 = stl.sum;
        }
    }
    ~StageScope() {
        if (!on) {
            return;
        }
        const uint64_t total = NowNs() - start;
        const uint64_t inner = (tl.mutex_ns - mutex0) + (tl.protect_ns - protect0) + (stl.sum - sum0);
        const uint64_t own = total > inner ? total - inner : 0;
        stl.ns[stage] += own;
        stl.sum += own;
    }
    StageScope(const StageScope&) = delete;
    StageScope& operator=(const StageScope&) = delete;
};
/// The write-fault handler of the page manager is reached: everything since Begin that was not the
/// fence wait was dispatch ("entry").
inline void NoteHandlerEntry() {
    if (kFpsTimers && tl.active) { // sotc: SOTC_FPS_LEAN: clock reads
        const uint64_t spent = NowNs() - tl.t0;
        const uint64_t own = spent > tl.fence_ns ? spent - tl.fence_ns : 0;
        stl.ns[kStEntry] = own;
        stl.sum += own;
    }
}
/// sotc: SOTC_FAULT_OSCOST=1 (SOTC_FPS): what the operating system adds to a write fault around the
/// handler, measured once on a private page (page_manager.cpp MeasureFaultOsCost): the time from the
/// faulting store to the entry of bbgpu_handle_fault, and from the handler's exit to the store
/// completing. 0 = not measured.
inline std::atomic<uint64_t> os_fault_in_ns{0}, os_fault_out_ns{0};
/// Adds the scope's duration to a field of tl while a fault is being timed.
struct Scope {
    uint64_t Tl::*field;
    uint64_t start = 0;
    bool on;
    explicit Scope(uint64_t Tl::*f) : field{f}, on{kFpsTimers && tl.active} { // sotc: SOTC_FPS_LEAN
        if (on) {
            start = NowNs();
        }
    }
    ~Scope() {
        if (on) {
            tl.*field += NowNs() - start;
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
inline void Begin() {
    tl.active = true;
    tl.counted = false;
    tl.mutex_ns = tl.fence_ns = tl.protect_ns = tl.flush_ns = 0;
    for (unsigned i = 0; i < kStCount; ++i) {
        stl.ns[i] = 0;
    }
    stl.sum = 0;
    // sotc: SOTC_FPS_LEAN: the fault still counts without kFpsTimers, only the clock is skipped.
    tl.t0 = kFpsTimers ? NowNs() : 0;
}
inline void End() {
    if (!tl.active) {
        return;
    }
    tl.active = false;
    if (!tl.counted) {
        return;
    }
    count.fetch_add(1, std::memory_order_relaxed);
    all_count.fetch_add(1, std::memory_order_relaxed);
    if (tl.sequential) {
        seq.fetch_add(1, std::memory_order_relaxed);
    }
    if (!kFpsTimers) { // sotc: SOTC_FPS_LEAN: the time sums and the histogram need the clock
        return;
    }
    const uint64_t ns = NowNs() - tl.t0;
    total_ns.fetch_add(ns, std::memory_order_relaxed);
    all_ns.fetch_add(ns, std::memory_order_relaxed);
    mutex_ns.fetch_add(tl.mutex_ns, std::memory_order_relaxed);
    fence_ns.fetch_add(tl.fence_ns, std::memory_order_relaxed);
    protect_ns.fetch_add(tl.protect_ns, std::memory_order_relaxed);
    flush_ns.fetch_add(tl.flush_ns, std::memory_order_relaxed);
    for (unsigned i = 0; i < kStCount; ++i) {
        stage_total_ns[i].fetch_add(stl.ns[i], std::memory_order_relaxed);
    }
    hist[Bucket(ns)].fetch_add(1, std::memory_order_relaxed);
}
/// What the write-ahead does for a guest write fault on the page at `page` (4 KiB aligned).
struct Plan {
    bool sequential = false;
    int dir = 0;
    uint32_t window = 16; ///< pages
};
/// Classifies the fault against this thread's previous one: sequential when it comes within
/// kWriteAheadAdaptUs of the previous fault's end, in the same direction as the run so far, at
/// most 4 pages beyond the extent the previous fault unprotected (or on a page of that extent that
/// was protected again). Only called with SOTC_FPS or SOTC_WRITE_AHEAD_ADAPT on.
inline Plan Observe(uint64_t page) {
    constexpr uint64_t kPage = 4096, kSlack = 4 * kPage;
    Tl& t = tl;
    Plan plan;
    t.cur_page = page;
    if (t.last_hi != 0 && NowNs() - t.last_end_ns <= kWriteAheadAdaptUs * 1000) {
        int dir = 0;
        if (page > t.last_page && page <= t.last_hi + kSlack) {
            dir = 1;
        } else if (page < t.last_page && page + kSlack >= t.last_lo) {
            dir = -1;
        }
        if (dir != 0 && (t.dir == 0 || t.dir == dir)) {
            plan.sequential = true;
            plan.dir = dir;
            const uint64_t doubled = uint64_t{t.window} * 2;
            plan.window = static_cast<uint32_t>(doubled < kWriteAheadMaxPages
                                                    ? doubled
                                                    : kWriteAheadMaxPages);
        }
    }
    t.sequential = plan.sequential;
    t.dir = plan.dir;
    t.window = plan.window;
    return plan;
}
/// Records the extent [lo, hi) the fault unprotected (granule, write-ahead) and counts it.
inline void Done(uint64_t lo, uint64_t hi) {
    Tl& t = tl;
    t.last_page = t.cur_page;
    t.last_lo = lo < t.cur_page ? lo : t.cur_page;
    t.last_hi = hi > t.cur_page + 4096 ? hi : t.cur_page + 4096;
    t.last_end_ns = NowNs();
    t.counted = true;
}
} // namespace FaultStat
/// SOTC_CMD_STATS=1: draws, indirect draws (arguments GPU-written / CPU-side / CPU-side and
/// empty), dispatches, indirect dispatches.
inline std::atomic<uint64_t> cmd_kinds[7]{}; // [6]: direct draws with 0 indices or 0 instances
/// sotc: IT_SET_PREDICATION packets by predication op (0 clear, 1 zpass, 2 primcount, 3 bool64,
/// other), SOTC_CMD_STATS only; printed as the "SOTC predication" line (videoout/driver.cpp).
inline std::atomic<uint64_t> pred_ops[5]{};
/// SOTC_CMD_STATS: command blocks the recording thread ran, per CommandRecorder::BlockCategory
/// (vk_recorder.cpp counts them, the SOTC_FPS printer takes them per window).
inline std::atomic<uint64_t> recorder_blocks[16]{};
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
inline std::atomic<uint64_t> arena_tick_waits{0}; ///< SOTC_ARENA_TICK_FREE: waits for freed arena runs
/// sotc: SOTC_HRR=1 (buffer_cache/hrr.h): arena bytes and chunks in host-visible memory, now.
inline std::atomic<uint64_t> hrr_host_bytes{0}, hrr_host_allocs{0};
/// sotc: SOTC_HRR=1 (BufferCache::MigrateHostResident): granules moved from device to host
/// memory, and their bytes.
/// hrr_migrate_failed: migrations a check kept in device memory (retried, see MigrateHostResident).
inline std::atomic<uint64_t> hrr_migrated{0}, hrr_migrated_bytes{0}, hrr_migrate_failed{0};
/// sotc: SOTC_HRR=1 direct readbacks (BufferCache::FinishAsyncReadback): readbacks that read their
/// bytes from host-visible arena chunks instead of a staging copy, and those bytes.
inline std::atomic<uint64_t> hrr_direct_readbacks{0}, hrr_direct_bytes{0};
/// sotc: SOTC_ADAPTIVE_LOAD (sotc_adaptive.h), per report (SOTC_FPS only): flips seen and those
/// while relieved, engagements, far cascades moved instead of drawn because of the governor,
/// periodic submits left out (approximate: base-interval submits a longer window replaced),
/// texture collector passes and cold arena checks that waited.
inline std::atomic<uint64_t> adaptive_flips{0}, adaptive_relieved_flips{0}, adaptive_engaged{0};
inline std::atomic<uint64_t> adaptive_amortised{0}, adaptive_flushes_saved{0};
inline std::atomic<uint64_t> adaptive_gc_deferred{0}, adaptive_evict_deferred{0};
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
// sotc: SOTC_ASYNC_COMPUTE_XLATE, totals: compute shaders queued ahead of their dispatch, the
// dispatches that waited for one (and the wait), and those taken back before a worker started.
inline std::atomic<uint64_t> compute_prefetch_queued{0};
inline std::atomic<uint64_t> compute_prefetch_scans{0};
inline std::atomic<uint64_t> compute_prefetch_waits{0};
inline std::atomic<uint64_t> compute_prefetch_wait_ns{0};
inline std::atomic<uint64_t> compute_prefetch_taken_back{0};
inline std::atomic<uint64_t> compute_prefetch_timeouts{0};
// sotc: SOTC_ASYNC_IMAGE_CREATE, totals (VideoCore::ImagePrefetcher): creations the command
// processor queued and dropped (cap, budget, limits), images the workers made (and their time)
// or could not make within the budget, images Image::Image took ready, took after waiting for a
// worker (and the wait), took back still queued and created itself, created itself without any
// match, and made images nobody took (destroyed).
inline std::atomic<uint64_t> async_image_requested{0}, async_image_skipped{0};
inline std::atomic<uint64_t> async_image_built{0}, async_image_build_ns{0}, async_image_failed{0};
inline std::atomic<uint64_t> async_image_taken{0}, async_image_waits{0}, async_image_wait_ns{0};
inline std::atomic<uint64_t> async_image_taken_back{0}, async_image_sync{0};
inline std::atomic<uint64_t> async_image_expired{0};
/// sotc: SRT walker loads that faulted (null or unmapped table pointer) and read zero, total.
inline std::atomic<uint64_t> srt_walker_faults{0};
/// sotc: Phi operands of SRT walker offsets: resolved to their one common value, and fallbacks
/// (operands differ, or a cycle through Phis). Counted when the shader is translated.
inline std::atomic<uint64_t> phi_resolved{0}, phi_fallback{0};
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
/// sotc: read once at static initialisation (as kCmdStats), so a check costs no guard: it ran per
/// buffer bind as a function-local static (~1.9% of the draw thread).
inline const bool kOptOff = [] {
    const char* v = std::getenv("SOTC_OPT_OFF");
    return v && v[0] == '1';
}();
inline bool OptOff() {
    return kOptOff;
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
    bool timed = false; ///< sotc: the time is read only with kFpsTimers (SOTC_FPS_LEAN drops it)
    std::chrono::steady_clock::time_point start{};
    AllocTrace(AllocKind k, uint64_t bytes) : kind{k} {
        if (kFpsLog) {
            alloc_count[k].fetch_add(1, std::memory_order_relaxed);
            alloc_bytes[k].fetch_add(bytes, std::memory_order_relaxed);
            if (kFpsTimers) {
                timed = true;
                start = std::chrono::steady_clock::now();
            }
        }
    }
    ~AllocTrace() {
        if (timed) {
            alloc_ns[kind].fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - start)
                                         .count(),
                                     std::memory_order_relaxed);
        }
    }
};

/// Adds the scope's duration to frame_ns[cause]; reads no clock unless kFpsTimers (SOTC_FPS, not
/// SOTC_FPS_LEAN). A timer
/// nested in one of the same cause on the same thread adds nothing (counted once).
struct FrameTimer {
    FrameCause cause;
    bool outer = false;
    std::chrono::steady_clock::time_point start{};
    static inline thread_local bool active[FrameCauses]{};
    explicit FrameTimer(FrameCause c) : cause{c} {
        if (kFpsTimers && !active[c]) {
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

/// sotc: per-thread accounting of the GPU threads (SOTC_FPS only; "SOTC cpu/frame", "SOTC draw
/// waits/frame", "SOTC cp/frame", "SOTC recorder/frame" lines in videoout/driver.cpp, and the
/// per-frame figures of the SOTCSTALL lines in vk_presenter.cpp). Every counter below is
/// monotonic (never reset), so each printer keeps its own previous snapshot (FrameAccount).
///
/// Role of the calling thread, set at the start of each GPU thread (cheap thread_local; read
/// only behind kFpsLog). The CPU-time roles are also the BbThreadCpu::Register roles.
enum CpuRole : uint8_t {
    RoleOther, RoleCp, RoleDraw, RoleRecorder, RolePassSecondary, RoleUpload,
    RoleGuest, // sotc: SOTC_STALL_DETAIL, game guest threads (bbgpu_register_guest_thread);
               // their tl_role stays RoleOther
    CpuRoles
};
inline thread_local uint8_t tl_role = RoleOther;
/// Records the calling thread under `role` for the CPU-time sums (bbport_thread_cpu.cpp, which
/// is only told when SOTC_FPS is on) and sets tl_role.
void RegisterThreadRole(CpuRole role);

/// Waits of the draw thread (the thread that runs the back ops; the command processor itself
/// when the draw thread is off), by what it waits for:
/// gpu: GPU work to finish (semaphore and fence waits: Finish, FinishHostRead, readbacks,
///      SubmitAheadAndWait, command pool and descriptor reuse, eviction);
/// recorder: the recording thread (WaitSubmitted, Sync, chunk and fake handle backpressure);
/// upload: async upload copies and transfer-queue host copies;
/// tc: the texture cache lock held by another thread.
enum WaitKind : uint8_t { WaitGpu, WaitRecorder, WaitUpload, WaitTcLock, WaitKinds };
inline std::atomic<uint64_t> draw_wait_ns[WaitKinds]{}, draw_wait_count[WaitKinds]{};
/// Command processor time blocked on the draw thread (inside cp_parse_ns): BackAlloc while the
/// op ring is full, WaitBackTail/DrainBack. cp_parse_all_ns is cp_parse_ns, never reset.
inline std::atomic<uint64_t> cp_blocked_ns{0}, cp_parse_all_ns{0};
/// sotc: SOTC_FPS "SOTC CP ops" lines (Liverpool, SOTC_WD_FUSE). Func ops queued by kind (0 write
/// data, 1 EOP/EOS/release, 2 DMA, 3 other) and their run time on the draw thread; WRITE_DATA
/// packets fused into a draw; WRITE_DATA -> indirect draw/dispatch pairs; WaitBackWrites calls
/// that waited and their time.
inline std::atomic<uint64_t> cp_func_ops[4]{}, draw_func_ns[4]{};
inline std::atomic<uint64_t> cp_wd_fused{0}, cp_wd_pairs{0}, cp_bw_waits{0}, cp_bw_wait_ns{0};
/// sotc: command processor no-progress spin (Liverpool::Process; "SOTC CP spin" line, SOTCSTALL
/// "spin"). Written by the CP thread only (monotonic, store of load + n). spin: wall time of
/// streaks in which every resumed task only re-tested its wait (outside cp_parse_ns; sampled
/// every 32 resumes). rounds: whole rounds over the mapped queues without progress, and per
/// wait kind (Liverpool::CpWaitKind) the rounds in which a task waited on it. sleeps:
/// SOTC_CP_IDLE_US blocks, of which timeouts ended by the timeout; wakes/wake latency: blocks
/// ended by NotifyCpWait, and the time from its notification to the CP running again.
inline constexpr int CpSpinKinds = 5;
inline std::atomic<uint64_t> cp_spin_ns{0}, cp_spin_rounds{0}, cp_spin_kind_rounds[CpSpinKinds]{};
inline std::atomic<uint64_t> cp_sleeps{0}, cp_sleep_timeouts{0}, cp_sleep_ns{0}, cp_wakes{0},
    cp_wake_latency_ns{0};
inline void CpAdd(std::atomic<uint64_t>& counter, uint64_t n) {
    counter.store(counter.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}
/// Recording thread: time running blocks (not waiting for work), and inside it the waits for
/// async uploads before a submit (AsyncUpload::WaitUpTo) and vkQueueSubmit.
inline std::atomic<uint64_t> rec_busy_ns{0}, rec_upload_wait_ns{0}, rec_submit_ns{0};

/// Times a draw-thread wait into draw_wait_*[kind]. Exclusive: a wait nested in another (on the
/// same thread) is taken out of the outer one, so each nanosecond lands in one bucket. The wait
/// count needs only SOTC_FPS; the clock reads need kFpsTimers (or SOTC_TRACE) and the draw thread.
struct DrawWait {
    static inline thread_local uint64_t nested_ns = 0;
    uint64_t start = 0;
    uint64_t outer_nested = 0;
    WaitKind kind;
    bool on = false;    ///< counted: SOTC_FPS, on the draw thread
    bool timed = false; ///< clock read: kFpsTimers, or SOTC_TRACE (its span needs the times)
    explicit DrawWait(WaitKind k) : kind{k} {
        if (kFpsLog && tl_role == RoleDraw) {
            on = true;
            if (kFpsTimers || SotcTrace::kOn) { // sotc: SOTC_FPS_LEAN drops the clock reads
                timed = true;
                outer_nested = nested_ns;
                nested_ns = 0;
                start = NowNs();
            }
        }
    }
    ~DrawWait() {
        if (timed) {
            const uint64_t now = NowNs();
            const uint64_t elapsed = now - start;
            if (SotcTrace::kOn) { // sotc: SOTC_TRACE
                static constexpr const char* kNames[WaitKinds] = {"wait gpu", "wait recorder",
                                                                  "wait upload", "wait tc lock"};
                SotcTrace::Span(kNames[kind], start, now);
            }
            const uint64_t inner = nested_ns < elapsed ? nested_ns : elapsed;
            draw_wait_ns[kind].fetch_add(elapsed - inner, std::memory_order_relaxed);
            nested_ns = outer_nested + elapsed;
        }
        if (on) {
            draw_wait_count[kind].fetch_add(1, std::memory_order_relaxed);
        }
    }
    DrawWait(const DrawWait&) = delete;
    DrawWait& operator=(const DrawWait&) = delete;
};

/// Adds the scope's duration to `total` when SOTC_FPS is on and the caller has `role`.
/// sotc: trace_name (a string literal) also records the scope as a SOTC_TRACE span.
struct RoleTimer {
    std::atomic<uint64_t>& total;
    uint64_t start = 0;
    bool on = false;
    const char* trace_name = nullptr;
    RoleTimer(CpuRole role, std::atomic<uint64_t>& t, const char* trace = nullptr)
        : total{t}, trace_name{trace} {
        if ((kFpsTimers || SotcTrace::kOn) && tl_role == role) { // sotc: SOTC_FPS_LEAN
            on = true;
            start = NowNs();
        }
    }
    ~RoleTimer() {
        if (on) {
            const uint64_t now = NowNs();
            total.fetch_add(now - start, std::memory_order_relaxed);
            if (trace_name && SotcTrace::kOn) {
                SotcTrace::Span(trace_name, start, now);
            }
        }
    }
    RoleTimer(const RoleTimer&) = delete;
    RoleTimer& operator=(const RoleTimer&) = delete;
};

/// One reading of all the accounting counters; the difference of two is what happened between
/// them (Diff). Taken at every game flip by the SOTCSTALL code and per report by the printer.
struct FrameAccount {
    uint64_t cpu_ns[CpuRoles]{};
    uint64_t wait_ns[WaitKinds]{}, wait_count[WaitKinds]{};
    uint64_t parse_ns = 0, blocked_ns = 0;
    uint64_t spin_ns = 0; ///< sotc: command processor no-progress spin (cp_spin_ns)
    uint64_t rec_busy = 0, rec_upload = 0, rec_submit = 0;
    uint64_t fence = 0; ///< SOTC_ASYNC_UPLOAD write fence waits (async_upload_fence_ns)
    uint64_t rb_wait_ns = 0, rb_wait_count = 0; ///< sotc: guest readback waits (rb_guest_all_*)
    uint64_t rb_adj = 0; ///< sotc: adjacent guest readback requests (rb_pat_adjacent_all)
    uint64_t rb_pf = 0;  ///< sotc: SOTC_RB_PREFETCH prefetches consumed (rb_pf_consumed_all)
    uint64_t fault_count = 0, fault_ns = 0; ///< sotc: guest write faults (FaultStat::all_*)
    uint64_t flip_pace_ns = 0; ///< sotc: SOTC_STALL_DETAIL, cp_flip_pace_all_ns
    uint64_t upload_user_ns = 0, upload_kernel_ns = 0; ///< sotc: SOTC_STALL_DETAIL, RoleUpload threads
};
FrameAccount TakeAccount();
/// `now - before`, per field (saturating at zero).
FrameAccount DiffAccount(const FrameAccount& now, const FrameAccount& before);

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
/// sotc: SOTC_RB_GROUP ("SOTC RB group" line, buffer_cache.cpp RbgAtFault), since the last report.
/// delivered: group downloads recorded (first_hit: at the first fault of a bind whose key was read
/// in pieces before; second_hit: at the second fault of a bind), with the ranges of their groups,
/// the page runs and bytes they copied (bytes_max: the largest). applied/applied_bytes: group
/// downloads written to guest memory. faults: guest read faults that landed on a known group; late:
/// ... on one already delivered (they raced the apply or hit a page left out). learned: keys found
/// hot. fb_*: groups not delivered, by reason (cold: read in one window so far; old: past the
/// freshness bound; size: too many ranges, or cut at the caps; cap: too many downloads in flight
/// or staging; empty: nothing left to copy; dirty: every page trimmed by later writes; expired:
/// never complete in time). skip_*: pages left out (readback window, owned by a readback in
/// flight, CPU bit, owed bytes, bytes not written by a fresh group). trimmed: bytes taken out by
/// later writes, unmaps or applies.
inline std::atomic<uint64_t> rbg_delivered{0}, rbg_first_hit{0}, rbg_second_hit{0},
    rbg_ranges{0}, rbg_runs{0}, rbg_bytes{0}, rbg_bytes_max{0}, rbg_applied{0},
    rbg_applied_bytes{0}, rbg_faults{0}, rbg_late{0}, rbg_learned{0}, rbg_trimmed{0};
inline std::atomic<uint64_t> rbg_fb_cold{0}, rbg_fb_old{0}, rbg_fb_size{0}, rbg_fb_cap{0},
    rbg_fb_empty{0}, rbg_fb_dirty{0}, rbg_expired{0};
inline std::atomic<uint64_t> rbg_skip_window{0}, rbg_skip_owned{0}, rbg_skip_cpu{0},
    rbg_skip_owed{0}, rbg_skip_stale{0};
/// sotc: SOTC_RB_GROUP: formats the "SOTC RB group" report line into out and resets the counters;
/// false (nothing written) when nothing happened (defined in buffer_cache.cpp).
bool RbGroupTake(char* out, std::size_t out_size);

/// sotc: SOTC_FPS "SOTC submit gate" line (gnmdriver.cpp GateWait): the game's time in the
/// submit gate, per site (0 command buffer submit, 1 compute doorbell, 2 SubmitDone), since the
/// last report. A call that took 5 us or more counts as a wait.
inline constexpr int GateSites = 3;
inline std::atomic<uint64_t> gate_calls[GateSites]{}, gate_waits[GateSites]{},
    gate_ns[GateSites]{}, gate_max_ns[GateSites]{};
/// SOTC_CP_AHEAD=2: frames closed by the guest but not yet recorded, sampled at SubmitDone.
inline std::atomic<uint64_t> gate_lag_sum{0}, gate_lag_samples{0}, gate_lag_max{0};
/// sotc: compute rings too full for a submit like the last one (Liverpool::NoteAscDoorbell):
/// windows that ended, their time and the longest; windows followed within 1 ms by a doorbell
/// on that ring (the game was most likely spinning in its retry loop, eboot 0x863a50) and the
/// time from the start of those windows to that doorbell; doorbells in all.
inline std::atomic<uint64_t> asc_full_windows{0}, asc_full_ns{0}, asc_full_max_ns{0},
    asc_full_spun{0}, asc_full_spun_ns{0}, asc_doorbells{0};
/// sotc: raises `max` to `value` (SOTC_FPS maxima above).
inline void GateStoreMax(std::atomic<uint64_t>& max, uint64_t value) {
    uint64_t seen = max.load(std::memory_order_relaxed);
    while (value > seen && !max.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}
} // namespace SotcStats

// sotc: SOTC_HLE_SHADOW_CULL=1 (default off): the shadow-scene frustum cull compute shader is
// computed on the CPU at its place in the command stream (video_core/sotc_shadow_cull.cpp).
// Counters printed with SOTC_FPS (videoout/driver.cpp), bumped only while the switch is on.
namespace SotcStats {
inline const bool kShadowCullHle = [] {
    const char* v = std::getenv("SOTC_HLE_SHADOW_CULL");
    return v && v[0] == '1';
}();
/// Why a dispatch of that shader ran on the GPU instead.
enum ShadowCullFallback : int {
    ShadowCullLayout,   ///< resources, strides, dispatch shape or aliasing not the known ones
    ShadowCullInputGpu, ///< an input range is GPU-written (guest memory not authoritative)
    ShadowCullBounds,   ///< an access outside its buffer or an unmapped range
    ShadowCullReplace,  ///< the buffer cache refused the CPU write of an output
    ShadowCullRbPrev,   ///< SOTC_RB_PREV on: guest memory may be older than the GPU copy
    ShadowCullFallbacks
};
inline std::atomic<uint64_t> shadow_cull_dispatches{0}; ///< dispatches computed on the CPU
inline std::atomic<uint64_t> shadow_cull_sections{0};   ///< sections evaluated by them
inline std::atomic<uint64_t> shadow_cull_kept{0};       ///< set bits the GPU might have cleared
inline std::atomic<uint64_t> shadow_cull_ns{0};         ///< CPU time, fallbacks included
inline std::atomic<uint64_t> shadow_cull_fallbacks[ShadowCullFallbacks]{};
} // namespace SotcStats

// sotc: SOTC_CULL_EXPRESS=1 (default off): the XPR occlusion pass (a submit ending in a dispatch and
// a CS_DONE end-of-pipe label) is recorded and submitted AHEAD of the older ops still waiting in
// the back ring when that is provably unobservable (Liverpool::XpPoll, liverpool.cpp). Counters
// printed with SOTC_FPS (videoout/driver.cpp), bumped only while the switch is on.
namespace SotcStats {
inline const bool kCullExpress = [] {
    const char* v = std::getenv("SOTC_CULL_EXPRESS");
    return v && v[0] == '1';
}();
/// Why a published express segment ran in FIFO order (or at all).
enum XpReason : int {
    XpNoBacklog,      ///< the draw thread was already at (or inside) the segment
    XpSegTooLong,     ///< the segment has more ops than SOTC_CULL_EXPRESS_SEG
    XpSkipTooMany,    ///< more skipped ops than SOTC_CULL_EXPRESS_SKIP
    XpBudget,         ///< the decision took longer than SOTC_CULL_EXPRESS_US
    XpSkipUnknownFunc,///< a skipped Func op the lane cannot classify (flip, DMA to GDS, ...)
    XpSkipSignals,    ///< a skipped label/flip/mark and SOTC_CULL_EXPRESS_SIGNALS=0 / _MARKS=0
    XpSkipNotReady,   ///< a skipped draw/dispatch has no footprint yet (prep thread behind)
    XpSkipUnknownFp,  ///< a skipped draw/dispatch whose footprint is unknown
    XpExpressUnknown, ///< an express op that is not a draw, dispatch, label or known write
    XpExpressNotReady,///< an express draw/dispatch has no footprint yet
    XpExpressUnknownFp,///< an express draw/dispatch whose footprint is unknown
    XpConflict,       ///< a skipped op's footprint conflicts with the express ops'
    XpLabelSame,      ///< a skipped label writes the address of an express label (per-address FIFO)
    XpNoPrep,         ///< no GpuPrep threads (SOTC_PARALLEL_RECORD=1 needed)
    XpOk,             ///< ran ahead
    XpReasons
};
inline std::atomic<uint64_t> xp_segments{0};      ///< express segments published by the parser
inline std::atomic<uint64_t> xp_ahead{0};         ///< ... that ran ahead of older ops
inline std::atomic<uint64_t> xp_skipped_ops{0};   ///< older ops they jumped over
inline std::atomic<uint64_t> xp_express_ops{0};   ///< ops run ahead
inline std::atomic<uint64_t> xp_reasons[XpReasons]{};
/// Skipped ops that were labels / flips / frame marks (the progress signals of rule 3).
inline std::atomic<uint64_t> xp_sk_labels{0}, xp_sk_flips{0}, xp_sk_marks{0};
/// Queue-to-execution latency of the express EOP: ahead (sum, count) and in FIFO order.
inline std::atomic<uint64_t> xp_lat_ahead_ns{0}, xp_lat_fifo_ns{0}, xp_lat_fifo_n{0};
/// Estimated time saved: skipped ops times the draw thread's EWMA time per op.
inline std::atomic<uint64_t> xp_saved_est_ns{0};
/// Time of the decisions (footprints and conflict checks) and of the express runs.
inline std::atomic<uint64_t> xp_decide_ns{0}, xp_decide_max_ns{0}, xp_run_ns{0};
/// XpConflict split. By direction: an express write hits a skipped read (WAR, e.g. a frame N-1
/// dispatch still reading the HiZ the express pass overwrites), a skipped write hits an express
/// read (RAW), both write (WAW). By the kind of the first conflicting skipped op.
inline std::atomic<uint64_t> xp_conf_war{0}, xp_conf_raw{0}, xp_conf_waw{0};
inline std::atomic<uint64_t> xp_conf_draw{0}, xp_conf_dispatch{0}, xp_conf_func{0};
} // namespace SotcStats

// sotc: SOTC_RB_EXACT=publish counters (lean XPR publish mode). Written by BufferCache (label
// downloads, readbacks) and Liverpool (deferred CS_DONE labels, latency), printed with SOTC_FPS
// by the "SOTC RB publish" line of videoout/driver.cpp.
namespace SotcStats {
inline std::atomic<uint64_t> rbp_labels_deferred{0};   ///< deferred CS_DONE labels that landed
inline std::atomic<uint64_t> rbp_bytes_published{0};   ///< bytes applied by label downloads
inline std::atomic<uint64_t> rbp_dropped_dirty{0};     ///< label downloads dropped: a later write made them dirty
inline std::atomic<uint64_t> rbp_cpu_pages_skipped{0}; ///< pages skipped at apply because the CPU bit was set
inline std::atomic<uint64_t> rbp_lat_cp_ns{0};         ///< sum: CP parsed the EOP -> draw thread processed it
inline std::atomic<uint64_t> rbp_lat_submit_ns{0};     ///< sum: draw thread processed -> Flush() returned
inline std::atomic<uint64_t> rbp_lat_gpu_ns{0};        ///< sum: Flush() returned -> RbxDrain saw the tick done
inline std::atomic<uint64_t> rbp_lat_land_ns{0};       ///< sum: tick done -> label written
inline std::atomic<uint64_t> rbp_lat_samples{0};       ///< number of labels in the sums above
inline std::atomic<uint64_t> rbp_reads_before{0};      ///< guest readbacks on bytes of a label download not yet applied
inline std::atomic<uint64_t> rbp_reads_after{0};       ///< guest readbacks that still faulted on a page a label download applied earlier
} // namespace SotcStats
