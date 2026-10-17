// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "core/memory.h"
#include "shader_recompiler/info.h"
#include "video_core/sotc_stats.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"

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

static bool ReplaceChangedPages(Rasterizer& rasterizer, VAddr address, const u8* current,
                                const TableData& data, u64 size) {
    auto& buffer_cache = rasterizer.GetBufferCache();
    VAddr run_start = 0;
    VAddr run_end = 0; // the pending run of changed pages, empty while run_end == run_start
    const auto flush = [&] {
        if (run_end == run_start) {
            return true;
        }
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
        return true;
    };
    const VAddr end = address + size;
    for (VAddr lo = address; lo < end;) {
        const VAddr hi = std::min<VAddr>((lo & ~VAddr{4095}) + 4096, end);
        if (std::memcmp(current + (lo - address), data.At(lo - address), hi - lo) != 0) {
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

static bool ExecuteReplicateShaderHLE(const Shader::Info& info,
                                      const AmdGpu::ComputeProgram& cs_program,
                                      Rasterizer& rasterizer) {
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
            TableData table{};
            if (src_count == count) {
                table.direct = reinterpret_cast<const u8*>(source);
            } else {
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
            }
            return ReplaceChangedPages(rasterizer, address, run + (address - run_start), table,
                                       count * 4);
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
    if (!buffer_cache.TryReplaceFromCpu(address, data.data(), count * 4)) {
        return false;
    }
    if (SotcStats::kCmdStats) SotcStats::inv_hle.fetch_add(count * 4, std::memory_order_relaxed);
    rasterizer.InvalidateMemory(address, count * 4);
    return true;
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
