// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "bbport_copy.h"
#include "core/memory.h"
#include "shader_recompiler/info.h"
#include "video_core/sotc_stats.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/renderer_vulkan/sotc_dispphase.h" // sotc: SOTC_DISP_PHASES

extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Vulkan {

static constexpr u64 COPY_SHADER_HASH = 0xfefebf9f;

static bool ExecuteCopyShaderHLE(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                                 Rasterizer& rasterizer) {
    auto& runtime = rasterizer.GetRuntime();
    auto& buffer_cache = rasterizer.GetBufferCache();

    // Copy shader defines three formatted buffers as inputs: control, source, and destination.
    const auto ctl_buf_sharp = info.buffers[0].GetSharp(info);
    const auto src_buf_sharp = info.buffers[1].GetSharp(info);
    const auto dst_buf_sharp = info.buffers[2].GetSharp(info);
    const auto buf_stride = src_buf_sharp.GetStride();
    ASSERT(buf_stride == dst_buf_sharp.GetStride());

    struct CopyShaderControl {
        u32 dst_idx;
        u32 src_idx;
        u32 end;
    };
    static_assert(sizeof(CopyShaderControl) == 12);
    ASSERT(ctl_buf_sharp.GetStride() == sizeof(CopyShaderControl));
    const auto ctl_buf = reinterpret_cast<const CopyShaderControl*>(ctl_buf_sharp.base_address);

    static std::vector<vk::BufferCopy> copies;
    copies.clear();
    copies.reserve(cs_program.dim_x);

    for (u32 i = 0; i < cs_program.dim_x; i++) {
        const auto& [dst_idx, src_idx, end] = ctl_buf[i];
        const u32 local_dst_offset = dst_idx * buf_stride;
        const u32 local_src_offset = src_idx * buf_stride;
        const u32 local_size = (end + 1) * buf_stride;
        copies.emplace_back(local_src_offset, local_dst_offset, local_size);
    }

    static constexpr vk::DeviceSize MaxDistanceForMerge = 64_MB;
    u32 batch_start = 0;
    u32 batch_end = 0;

    while (batch_end < copies.size()) {
        // Place first copy into the current batch
        const auto& copy = copies[batch_start];
        auto src_offset_min = copy.srcOffset;
        auto src_offset_max = copy.srcOffset + copy.size;
        auto dst_offset_min = copy.dstOffset;
        auto dst_offset_max = copy.dstOffset + copy.size;

        for (++batch_end; batch_end < copies.size(); batch_end++) {
            // Compute new src and dst bounds if we were to batch this copy
            const auto& [src_offset, dst_offset, size] = copies[batch_end];
            auto new_src_offset_min = std::min(src_offset_min, src_offset);
            auto new_src_offset_max = std::max(src_offset_max, src_offset + size);
            if (new_src_offset_max - new_src_offset_min > MaxDistanceForMerge) {
                break;
            }

            auto new_dst_offset_min = std::min(dst_offset_min, dst_offset);
            auto new_dst_offset_max = std::max(dst_offset_max, dst_offset + size);
            if (new_dst_offset_max - new_dst_offset_min > MaxDistanceForMerge) {
                break;
            }

            // We can batch this copy
            src_offset_min = new_src_offset_min;
            src_offset_max = new_src_offset_max;
            dst_offset_min = new_dst_offset_min;
            dst_offset_max = new_dst_offset_max;
        }

        // Obtain buffers for the total source and destination ranges.
        const auto [src_buf, src_buf_offset] = buffer_cache.ObtainBuffer(
            src_buf_sharp.base_address + src_offset_min, src_offset_max - src_offset_min, false);
        // sotc: the merged range is obtained for reading only and just the copied pieces are
        // marked GPU-written. Marking the whole range made readbacks bring back the gaps between
        // the copies from the GPU's stale copy, over what the CPU wrote there since (the game
        // moves streamed textures with this shader; its allocator keeps free-list nodes in the
        // gaps: heap corruption). SOTC_COPY_HLE_WHOLE=1 marks the whole range (as before).
        static const bool mark_whole = std::getenv("SOTC_COPY_HLE_WHOLE") != nullptr;
        const auto [dst_buf, dst_buf_offset] = buffer_cache.ObtainBuffer(
            dst_buf_sharp.base_address + dst_offset_min, dst_offset_max - dst_offset_min,
            mark_whole);
        if (!mark_whole) {
            for (u32 i = batch_start; i < batch_end; ++i) {
                buffer_cache.ObtainBuffer(dst_buf_sharp.base_address + copies[i].dstOffset,
                                          static_cast<u32>(copies[i].size), true);
            }
        }

        // Apply found buffer base.
        const auto vk_copies = std::span{copies}.subspan(batch_start, batch_end - batch_start);
        for (auto& copy : vk_copies) {
            copy.srcOffset = copy.srcOffset - src_offset_min + src_buf_offset;
            copy.dstOffset = copy.dstOffset - dst_offset_min + dst_buf_offset;
        }

        // Execute buffer copies.
        LOG_TRACE(Render_Vulkan, "HLE buffer copy: src_size = {}, dst_size = {}",
                  src_offset_max - src_offset_min, dst_offset_max - dst_offset_min);
        runtime.CopyBuffer(src_buf, dst_buf, vk_copies);
        batch_start = batch_end;
    }

    return true;
}

// sotc: Shadow of the Colossus fills small tables every frame with a compute shader:
// dst[i] = src[i % m] (32-bit elements) for i < min(n, user SGPR6, the dispatched threads),
// n and m read from the constant buffer. The command processor then reads those tables (SRT
// walker), and with the shader on the GPU every read waited for the GPU (readback): about a
// third of all readbacks in the open world. With a CPU-written source the fill is done here,
// in stream order, so the tables stay CPU-side. SOTC_FILL_HLE=0 runs the shader on the GPU.
static constexpr u64 REPLICATE_SHADER_HASH = 0x50784801;

// sotc: SOTC_FILL_HLE_PAGES (default on, 0 replaces whole tables): a changed table usually
// differs in a few of its 4 KiB pages (at the Shrine 1.5 of the 8.4 MB of changed tables per
// frame). Only the pages that differ are replaced, so only they are invalidated, marked
// CPU-written and uploaded again. The caller checked that no byte of the table is GPU-written,
// so `current` (the backing) is what the guest sees, and the unchanged pages already hold the
// result: the same state as a table that did not change at all. If a replace is refused (other
// GPU-written bytes on a page), false runs the shader on the GPU, which writes the whole table.
// The bytes a fill writes: `direct` (contiguous), or `pattern` repeated with `period` (the buffer
// holds period + 4 KiB bytes, so At(o) is valid for the rest of o's 4 KiB page).
struct TableData {
    const u8* direct = nullptr;
    const u8* pattern = nullptr;
    u64 period = 0;
    const u8* At(u64 offset) const {
        return direct ? direct + offset : pattern + offset % period;
    }
    const u8* Run(u64 offset, u64 size) const { // contiguous bytes of [offset, offset + size)
        if (direct) {
            return direct + offset;
        }
        static std::vector<u8> run;
        run.resize(size);
        for (u64 done = 0; done < size;) {
            const u64 chunk = std::min<u64>(size - done, 4096 - (offset + done) % 4096);
            std::memcpy(run.data() + done, At(offset + done), chunk);
            done += chunk;
        }
        return run.data();
    }
};

// sotc: off by default (SOTC_FILL_HLE_PARALLEL=1 turns it on): the tables are compared on the
// draw thread. The parallel compare saved ~2 us per op (~0.1 ms/frame) but its copy threads
// read guest pages the GPU tracker may protect; a fault there waits for the draw thread, which
// waits for the copy threads: boot deadlock in 5 of 32 launches. The pre-touch below closes it
// as far as known, so the gain does not justify the remaining race.
static bool ParallelCompareEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_FILL_HLE_PARALLEL");
        return v && v[0] == '1';
    }();
    return enabled;
}

static bool ReplaceChangedPages(Rasterizer& rasterizer, VAddr address, const u8* current,
                                const TableData& data, u64 size) {
    auto& buffer_cache = rasterizer.GetBufferCache();
    VAddr run_start = 0;
    VAddr run_end = 0; // the pending run of changed pages, empty while run_end == run_start
    const auto flush = [&] {
        if (run_end == run_start) {
            return true;
        }
        const u64 flush_t0 = DispPhase::kOn ? DispPhase::Now() : 0;
        const u64 offset = run_start - address;
        const u64 run_size = run_end - run_start;
        run_start = run_end;
        if (!buffer_cache.TryWriteThroughFromCpu(address + offset, data.Run(offset, run_size),
                                                 run_size)) {
            return false;
        }
        if (SotcStats::kCmdStats) {
            SotcStats::inv_hle.fetch_add(run_size, std::memory_order_relaxed);
        }
        rasterizer.GetTextureCache().InvalidateMemory(address + offset, run_size);
        if (DispPhase::kOn) {
            auto& u = DispPhase::Subs();
            u.hle_flush += DispPhase::Now() - flush_t0;
            u.hle_repl_bytes += run_size;
        }
        return true;
    };
    const VAddr end = address + size;
    // sotc: a table of 1 MiB and more (the game copies a 4 MiB and an 8 MiB one every frame,
    // ~0.4 ms of memcmp of the draw thread, memory bound) is compared on the copy threads, one
    // flag per page; the scan below then reads the flags instead of comparing. Same result.
    // SOTC_FILL_HLE_PARALLEL=0 compares on this thread.
    const bool parallel = ParallelCompareEnabled();
    static std::vector<u8> differs;
    const VAddr first_page = address & ~VAddr{4095};
    const bool flagged = parallel && size >= 1_MB && BbCopy::Enabled();
    if (flagged) {
        constexpr u64 PagesPerTask = 64; // 256 KiB
        const u64 pages = (end - first_page + 4095) / 4096;
        differs.resize(pages);
        BbCopy::ParallelFor((pages + PagesPerTask - 1) / PagesPerTask, [&](std::size_t task) {
            const u64 last = std::min(pages, (task + 1) * PagesPerTask);
            for (u64 page = task * PagesPerTask; page < last; ++page) {
                const VAddr lo = std::max<VAddr>(first_page + page * 4096, address);
                const VAddr hi = std::min<VAddr>(first_page + page * 4096 + 4096, end);
                differs[page] =
                    std::memcmp(current + (lo - address), data.At(lo - address), hi - lo) != 0;
            }
        });
    }
    for (VAddr lo = address; lo < end;) {
        const VAddr hi = std::min<VAddr>((lo & ~VAddr{4095}) + 4096, end);
        const u64 cmp_t0 = DispPhase::kOn ? DispPhase::Now() : 0;
        const bool page_differs =
            flagged ? differs[(lo - first_page) / 4096] != 0
                    : std::memcmp(current + (lo - address), data.At(lo - address), hi - lo) != 0;
        if (DispPhase::kOn) {
            auto& u = DispPhase::Subs();
            u.hle_cmp += DispPhase::Now() - cmp_t0;
            u.hle_cmp_bytes += hi - lo;
        }
        if (page_differs) {
            if (run_end != lo && !flush()) {
                return false;
            }
            if (run_end == run_start) {
                run_start = lo;
            }
            run_end = hi;
        }
        lo = hi;
    }
    return flush();
}

// The whole table: the pattern of src_count elements, repeated (no division per element).
static std::vector<u32> data;
static void BuildTable(const u32* source, u64 src_count, u64 count) {
    if (data.size() < count) {
        data.resize(count); // grow only: data[0, count) is all that is read
    }
    std::memcpy(data.data(), source, src_count * 4);
    for (u64 done = src_count; done < count;) {
        const u64 chunk = std::min(done, count - done);
        std::memcpy(data.data() + done, data.data(), chunk * 4);
        done += chunk;
    }
}

namespace {
// sotc: SOTC_DISP_PHASES: total time and table size of one replicator HLE call.
struct HleScope {
    u64 t0 = DispPhase::kOn ? DispPhase::Now() : 0;
    u64 table_bytes = 0;
    ~HleScope() {
        if (!DispPhase::kOn) {
            return;
        }
        auto& u = DispPhase::Subs();
        const u64 dt = DispPhase::Now() - t0;
        ++u.hle_n;
        u.hle_total += dt;
        u.hle_table_bytes += table_bytes;
        if (table_bytes >= 1_MB) {
            ++u.hle_big_n;
            u.hle_big_ticks += dt;
        }
    }
};
} // namespace

static bool ExecuteReplicateShaderHLE(const Shader::Info& info,
                                      const AmdGpu::ComputeProgram& cs_program,
                                      Rasterizer& rasterizer) {
    HleScope hle_scope;
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_FILL_HLE");
        return !v || v[0] != '0';
    }();
    if (!enabled || info.buffers.size() < 3 || cs_program.dim_y > 1 || cs_program.dim_z > 1) {
        return false;
    }
    auto& buffer_cache = rasterizer.GetBufferCache();
    const auto ctl = info.buffers[0].GetSharp(info);
    const auto src = info.buffers[1].GetSharp(info);
    const auto dst = info.buffers[2].GetSharp(info);
    const u32 src_stride = src.GetStride() ? src.GetStride() : 4;
    const u32 dst_stride = dst.GetStride() ? dst.GetStride() : 4;
    if (src_stride != 4 || dst_stride != 4 || ctl.GetSize() < 8) {
        return false;
    }
    if (buffer_cache.IsRegionGpuModified(ctl.base_address, 8)) {
        return false;
    }
    const u32* control = std::bit_cast<const u32*>(ctl.base_address);
    const u32 n = control[0];
    const u32 m = control[1];
    const u64 threads = u64(cs_program.dim_x) * 64;
    const u64 count = std::min<u64>({n, cs_program.user_data[6], threads, dst.GetSize() / 4});
    if (count == 0) {
        return true;
    }
    const u64 src_count = m ? std::min<u64>(m, count) : count;
    if (src_count * 4 > src.GetSize() ||
        buffer_cache.IsRegionGpuModified(src.base_address, src_count * 4)) {
        return false;
    }
    const u32* source = std::bit_cast<const u32*>(src.base_address);
    const VAddr address = dst.base_address;
    hle_scope.table_bytes = count * 4;
    // sotc: the tables are mostly the same every frame. When no byte of the destination is GPU
    // written (so its pages are not read-protected and the backing is authoritative) and it
    // already holds this data, the result of the replace below is already in place: skip its
    // tracking updates, unprotects and invalidations (~9% of the GPU command thread).
    // Tables larger than one clean page (or straddling one) are compared through the backing
    // view of their mapping run, under the same condition (no byte GPU-written: the backing is
    // what the guest sees). SOTC_FILL_HLE_FAST=0 keeps the one-page check only.
    static const bool fast = [] {
        const char* v = std::getenv("SOTC_FILL_HLE_FAST");
        return !v || v[0] != '0';
    }();
    static const bool page_replace = [] {
        const char* v = std::getenv("SOTC_FILL_HLE_PAGES");
        return !v || v[0] != '0';
    }();
    if (!SotcStats::OptOff() && !buffer_cache.IsRegionGpuModified(address, count * 4)) {
        bool same = false;
        VAddr run_start = 0;
        u64 run_size = 0;
        const u8* run = fast ? Core::Memory::Instance()->GetBackingRun(address, &run_start, &run_size)
                             : nullptr;
        const bool in_run =
            run && run_size && address >= run_start && address + count * 4 <= run_start + run_size;
        // sotc: with page replaces, the table is compared page by page against its source (a
        // copy: the source itself; a repeated pattern of up to 64 KiB: one page past one
        // period of it), without building it first: ~15 MB of copying per frame at the Shrine.
        const u64 period = src_count * 4;
        const bool overlap = src.base_address < address + count * 4 &&
                             address < src.base_address + period;
        if (in_run && page_replace && !overlap && (src_count == count || period <= 64_KB)) {
            if (DispPhase::kOn) {
                auto& u = DispPhase::Subs();
                ++(src_count == count ? u.hle_direct : u.hle_pattern);
                u.hle_pre += DispPhase::Now() - hle_scope.t0;
            }
            TableData table{};
            if (src_count == count) {
                table.direct = reinterpret_cast<const u8*>(source);
                // sotc: the parallel compare reads the source (guest memory) on the copy threads.
                // A page of it that is GPU-tracked (the exact-range check above does not cover a
                // page shared with GPU-written bytes) faults there, and a copy thread's fault
                // waits for this thread (SendCommand<true> in BufferCache::ReadMemory) while this
                // thread waits for the copy threads in BbCopy::ParallelFor: a deadlock that hung
                // ~25% of boots at the loading screen. Touch one byte per page here first: the
                // fault, if any, is handled inline by this (draw) thread and the page is
                // accessible when the workers read it.
                if (count * 4 >= 1_MB && ParallelCompareEnabled() && BbCopy::Enabled()) {
                    const volatile u8* bytes = table.direct;
                    u8 sink = 0;
                    for (u64 offset = 0; offset < count * 4; offset += 4096) {
                        sink ^= bytes[offset];
                    }
                    sink ^= bytes[count * 4 - 1];
                    asm volatile("" : : "r"(sink));
                }
            } else {
                const u64 pb_t0 = DispPhase::kOn ? DispPhase::Now() : 0;
                static std::vector<u8> pattern;
                pattern.resize(period + 4096);
                std::memcpy(pattern.data(), source, period);
                for (u64 done = period; done < pattern.size();) {
                    const u64 chunk = std::min<u64>(done, pattern.size() - done);
                    std::memcpy(pattern.data() + done, pattern.data(), chunk);
                    done += chunk;
                }
                table.pattern = pattern.data();
                table.period = period;
                if (DispPhase::kOn) {
                    DispPhase::Subs().hle_pattern_build += DispPhase::Now() - pb_t0;
                }
            }
            return ReplaceChangedPages(rasterizer, address, run + (address - run_start), table,
                                       count * 4);
        }
        if (DispPhase::kOn) {
            ++DispPhase::Subs().hle_other;
        }
        BuildTable(source, src_count, count);
        if (in_run) {
            const u8* current = run + (address - run_start);
            same = std::memcmp(current, data.data(), count * 4) == 0;
            if (!same && page_replace) {
                return ReplaceChangedPages(rasterizer, address, current,
                                           TableData{reinterpret_cast<const u8*>(data.data())},
                                           count * 4);
            }
        } else {
            static std::vector<u32> current;
            if (current.size() < count) {
                current.resize(count);
            }
            same = buffer_cache.ReadClean(address, current.data(), static_cast<u32>(count * 4)) &&
                   std::memcmp(current.data(), data.data(), count * 4) == 0;
        }
        if (same) {
            return true;
        }
    } else {
        BuildTable(source, src_count, count);
    }
    // sotc: SOTC_FILL_HLE_GPU (default on, =0 off; HiddenUserHere, upstream PR #339). A destination
    // resident in one arena gets the table from the GPU (a fill, or a seed doubled by copies)
    // instead of being marked CPU-written, which the next DMA sweep or bind copied through staging
    // again (one 8 MiB table, ~33 times per area load). The backing is still written.
    static const bool fill_gpu = [] {
        const char* v = std::getenv("SOTC_FILL_HLE_GPU");
        return !(v && v[0] == '0');
    }();
    if (fill_gpu) {
        if (!buffer_cache.TryFillThroughFromCpu(address, data.data(), count * 4, src_count * 4)) {
            return false;
        }
        if (SotcStats::kCmdStats) SotcStats::inv_hle.fetch_add(count * 4, std::memory_order_relaxed);
        // The guest bytes changed: the reused-bind epoch moves as with InvalidateMemory, and
        // textures over the range are invalidated; the arena already holds the table.
        SotcStats::mem_epoch.fetch_add(1, std::memory_order_relaxed);
        rasterizer.GetTextureCache().InvalidateMemory(address, count * 4);
        return true;
    }
    if (!buffer_cache.TryReplaceFromCpu(address, data.data(), count * 4)) {
        return false;
    }
    if (SotcStats::kCmdStats) SotcStats::inv_hle.fetch_add(count * 4, std::memory_order_relaxed);
    rasterizer.InvalidateMemory(address, count * 4);
    return true;
}

// sotc: the lighting compute shaders of the open world read a light array through a descriptor
// with num_records near 4 GiB ("unbounded", which BindBuffers limits to a 64 MiB window) and
// through raw 64-bit pointers (uses_dma: the whole DMA sweep runs before them). Both only touch
// a few KiB, but the window and the sweep collected, re-protected and copied to the GPU every
// page the game had written in all resident memory since the last frame: 2-9 MB and ~25 page
// protects per frame, 0.5-0.9 ms of the draw thread (0x57bd9f27 paid it, as the first user;
// 0xa0adab44 paid its own window; the other 8 MB per frame were read by nobody: with the sweep
// off the frame uploads dropped from 11 MB to 2-4 MB). The extents come from the shader code:
//  - 0x57bd9f27 (tiled light culling, 120x68 groups): the light array at the pointer in user
//    data dwords 2-3 (the V# of buffer 0 points to the same place), its first dword n (the
//    shader reads it as flattened slot 0x34), then lights of 28 dwords read at dwords
//    28 * i + {32, 36, 40} (buffer 0) and at bytes 112 * i + {80 .. 180} (DMA) for i < n.
//  - 0xfb974c0e (clustered lights): same pointer, n at flattened slot 0x9d, lights of 56 dwords
//    (224 bytes), buffer 0 at dwords 56 * i + {7 .. 39} and DMA at bytes 224 * i + {32 .. 252}
//    for i <= n.
//  - 0xa0adab44 (deferred lighting, 240x135 groups): the index i of every load is < 4 (guard
//    4 > i); buffer 0 (stride 64) is read at dwords 16 * i + {428 .. 459}, buffer 1 (stride 16)
//    at dwords 4 * i + {510, 511}. 0x91c6c95e (20x12x64 groups) reads buffer 1 (stride 64, the
//    same table) at dwords 16 * i + {428 .. 439}, i < 4; 0x79704cba (volumetric lighting) reads
//    buffer 3 (stride 64) at dwords 16 * i + {348, 352, 356} (u32x4), i < 4.
//  - 0xa8403cb5 (GPU particle spawner, dispatch-indirect, 27-40 per second in the open world):
//    its bound descriptors are all small or already resident (the unbounded one, buffer 3, is
//    clamped to the ~5 MB the game mapped there and uploads nothing after the first dispatch).
//    The cost was the DMA sweep it triggered: 8-9 MB and ~65 page protects per dispatch,
//    since the shader reads through a raw pointer. Every DMA load of the shader is a scalar load
//    through the 64-bit pointer in user data dwords 16-17 at byte 288 + 16 * k + {0, 4}, k in
//    {-1, 0, 1, 2} (k is a curve segment index chosen by float compares: the values are 0 or 1,
//    minus 1, or the bit pattern 2, never anything else; the second load is 16 bytes further),
//    that is bytes 272 .. 344 of the pointer (user data 16-17 = the base of its buffer 3, a table of
//    16-byte curve keys). Its sibling 0x755ad38f (the next uses_dma dispatch-indirect of the
//    frame, which paid the same sweep once the first one stopped: 74 ms/s, 577 MB/s) has the same
//    curve code, DMA loads and user data slots. So does 0xcc4d91f3 (third variant, 29 ops/s, found
//    paying the sweep next, 355 MB/s) with the table at byte 128 + 16 * k + {0, 4, 16, 20}: bytes
//    112 .. 184 (not measured after the change). The first 512 bytes are synchronised.
// A hint is only given when the descriptor has the stride the shader was analysed with and the
// count is plausible. SOTC_HLE_<HASH>=0 disables a shader's hints (57BD9F27, FB974C0E,
// A0ADAB44, the last one for 0xa0adab44, 0x91c6c95e and 0x79704cba; A8403CB5, for 0xa8403cb5,
// 0x755ad38f and 0xcc4d91f3).
static bool HintEnabled(u64 hash) {
    const auto read = [](const char* name) {
        const char* v = std::getenv(name);
        return !v || v[0] != '0';
    };
    static const bool lights_cull = read("SOTC_HLE_57BD9F27");
    static const bool lights_cluster = read("SOTC_HLE_FB974C0E");
    static const bool lighting = read("SOTC_HLE_A0ADAB44");
    static const bool particles = read("SOTC_HLE_A8403CB5");
    switch (hash) {
    case 0x57bd9f27:
        return lights_cull;
    case 0xfb974c0e:
        return lights_cluster;
    case 0xa0adab44:
    case 0x91c6c95e:
    case 0x79704cba:
        return lighting;
    case 0xa8403cb5:
    case 0x755ad38f:
    case 0xcc4d91f3:
        return particles;
    default:
        return false;
    }
}

static constexpr u32 MaxLights = 8192;

u64 BufferExtentHint(const Shader::Info& info, u32 index, u32 stride) {
    if (!HintEnabled(info.pgm_hash)) {
        return 0;
    }
    const auto& flat = info.FlatBuf();
    switch (info.pgm_hash) {
    case 0x57bd9f27: {
        static constexpr u32 CountSlot = 0x34;
        if (index != 0 || stride != 112 || flat.size() <= CountSlot || flat[CountSlot] > MaxLights) {
            return 0;
        }
        return u64{flat[CountSlot]} * 112 + 64 + 256; // 256 bytes of slack
    }
    case 0xfb974c0e: {
        static constexpr u32 CountSlot = 0x9d;
        if (index != 0 || stride != 224 || flat.size() <= CountSlot || flat[CountSlot] > MaxLights) {
            return 0;
        }
        return u64{flat[CountSlot]} * 224 + 256;
    }
    case 0xa0adab44:
        if (index == 0 && stride == 64) {
            return 2032 + 256;
        }
        if (index == 1 && stride == 16) {
            return 2096 + 256;
        }
        return 0;
    case 0x91c6c95e:
        // Same four-entry table: buffer 1 (stride 64) at dwords 16 * i + {428 .. 439}, i < 4.
        return index == 1 && stride == 64 ? 1952 + 256 : 0;
    case 0x79704cba:
        // The same table, buffer 3 (stride 64) at dwords 16 * i + {348, 352, 356} (u32x4), i < 4.
        return index == 3 && stride == 64 ? 1632 + 256 : 0;
    default:
        return 0;
    }
}

bool KnownDmaRange(const Shader::Info& info, VAddr& address, u64& size) {
    if (!HintEnabled(info.pgm_hash)) {
        return false;
    }
    const auto& flat = info.FlatBuf();
    if (info.pgm_hash == 0xa8403cb5 || info.pgm_hash == 0x755ad38f ||
        info.pgm_hash == 0xcc4d91f3) {
        // The curve table pointer of the particle spawner (see above): bytes 272 .. 344.
        if (flat.size() <= 17) {
            return false;
        }
        address = (u64{flat[17]} << 32 | flat[16]) & 0xFFFFFFFFFFFFULL;
        size = 512;
        static std::atomic<u32> shown{0};
        if (address != 0 && shown.load(std::memory_order_relaxed) < 2 &&
            shown.fetch_add(1, std::memory_order_relaxed) < 2) {
            std::fprintf(stderr, "SOTC hle a8403cb5/755ad38f dma range %#llx + %llu\n",
                         static_cast<unsigned long long>(address),
                         static_cast<unsigned long long>(size));
        }
        return address != 0;
    }
    u32 count_slot;
    u64 element;
    u64 tail; // bytes read past the last element's start
    switch (info.pgm_hash) {
    case 0x57bd9f27:
        count_slot = 0x34, element = 112, tail = 184;
        break;
    case 0xfb974c0e:
        count_slot = 0x9d, element = 224, tail = 256 + 224; // i <= n
        break;
    default:
        return false;
    }
    if (flat.size() <= std::max<u32>(count_slot, 0x13) || flat[count_slot] > MaxLights) {
        return false;
    }
    address = (u64{flat[0x13]} << 32 | flat[0x12]) & 0xFFFFFFFFFFFFULL;
    size = u64{flat[count_slot]} * element + tail + 256; // 256 bytes of slack
    return address != 0;
}

bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer) {
    switch (info.pgm_hash) {
    case COPY_SHADER_HASH:
        return ExecuteCopyShaderHLE(info, cs_program, rasterizer);
    case REPLICATE_SHADER_HASH:
        return ExecuteReplicateShaderHLE(info, cs_program, rasterizer);
    default:
        return false;
    }
}

} // namespace Vulkan
