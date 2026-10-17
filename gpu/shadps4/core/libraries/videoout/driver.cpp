// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <vector>
#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/videoout/videoout_error.h"
#include "imgui/renderer/imgui_core.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/sotc_stats.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Libraries::VideoOut {

constexpr static bool Is32BppPixelFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::A8R8G8B8Srgb:
    case PixelFormat::A8B8G8R8Srgb:
    case PixelFormat::A2R10G10B10:
    case PixelFormat::A2R10G10B10Srgb:
    case PixelFormat::A2R10G10B10Bt2020Pq:
        return true;
    default:
        return false;
    }
}

constexpr u32 PixelFormatBpp(PixelFormat pixel_format) {
    switch (pixel_format) {
    case PixelFormat::A16R16G16B16Float:
        return 8;
    default:
        return 4;
    }
}

VideoOutDriver::VideoOutDriver(u32 width, u32 height) {
    main_port.resolution.full_width = width;
    main_port.resolution.full_height = height;
    main_port.resolution.pane_width = width;
    main_port.resolution.pane_height = height;
    present_thread = std::jthread([&](std::stop_token token) { PresentThread(token); });
}

VideoOutDriver::~VideoOutDriver() = default;

int VideoOutDriver::Open(const ServiceThreadParams* params) {
    if (main_port.is_open) {
        return ORBIS_VIDEO_OUT_ERROR_RESOURCE_BUSY;
    }
    main_port.is_open = true;
    liverpool->SetVoPort(&main_port);
    return 1;
}

void VideoOutDriver::Close(s32 handle) {
    std::scoped_lock lock{mutex};

    // Mark as closed
    main_port.is_open = false;
    main_port.flip_rate = 0;
    main_port.prev_index = -1;

    // Clear port information
    std::memset(main_port.buffer_labels.data(), 0, sizeof(main_port.buffer_labels));
    std::memset(main_port.groups.data(), 0, sizeof(main_port.groups));
    std::memset(&main_port.vblank_status, 0, sizeof(main_port.vblank_status));
    main_port.flip_status = FlipStatus{};

    // Re-initialize buffers
    std::memset(main_port.buffer_slots.data(), 0, sizeof(main_port.buffer_slots));
    for (auto& buffer : main_port.buffer_slots) {
        buffer.group_index = -1;
    }

    // Clear events
    for (auto event : main_port.flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    main_port.flip_events.clear();
    for (auto event : main_port.vblank_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    main_port.vblank_events.clear();
}

VideoOutPort* VideoOutDriver::GetPort(int handle) {
    if (handle != 1) [[unlikely]] {
        return nullptr;
    }
    return &main_port;
}

int VideoOutDriver::RegisterBuffers(VideoOutPort* port, s32 startIndex, void* const* addresses,
                                    s32 bufferNum, const BufferAttribute* attribute) {
    const s32 group_index = port->FindFreeGroup();
    if (group_index >= MaxDisplayBufferGroups) {
        return ORBIS_VIDEO_OUT_ERROR_NO_EMPTY_SLOT;
    }

    if (startIndex + bufferNum > MaxDisplayBuffers || startIndex > MaxDisplayBuffers ||
        bufferNum > MaxDisplayBuffers) {
        LOG_ERROR(Lib_VideoOut,
                  "Attempted to register too many buffers startIndex = {}, bufferNum = {}",
                  startIndex, bufferNum);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    const s32 end_index = startIndex + bufferNum;
    if (bufferNum > 0 &&
        std::any_of(port->buffer_slots.begin() + startIndex, port->buffer_slots.begin() + end_index,
                    [](auto& buffer) { return buffer.group_index != -1; })) {
        return ORBIS_VIDEO_OUT_ERROR_SLOT_OCCUPIED;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "startIndex = {}, bufferNum = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             startIndex, bufferNum, GetPixelFormatString(attribute->pixel_format),
             attribute->aspect_ratio, static_cast<u32>(attribute->tiling_mode), attribute->width,
             attribute->height, attribute->pitch_in_pixel, attribute->option);

    auto& group = port->groups[group_index];
    std::memcpy(&group.attrib, attribute, sizeof(BufferAttribute));
    group.is_occupied = true;

    for (u32 i = 0; i < bufferNum; i++) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(addresses[i]);
        port->buffer_slots[startIndex + i] = VideoOutBuffer{
            .group_index = group_index,
            .address_left = address,
            .address_right = 0,
        };

        // Reset flip label also when registering buffer
        port->buffer_labels[startIndex + i] = 0;
        port->SignalVoLabel();

        presenter->RegisterVideoOutSurface(group, address);
        LOG_INFO(Lib_VideoOut, "buffers[{}] = {:#x}", i + startIndex, address);
    }

    return group_index;
}

int VideoOutDriver::UnregisterBuffers(VideoOutPort* port, s32 attributeIndex) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    auto& group = port->groups[attributeIndex];
    group.is_occupied = false;

    for (auto& buffer : port->buffer_slots) {
        if (buffer.group_index != attributeIndex) {
            continue;
        }
        buffer.group_index = -1;
    }

    return ORBIS_OK;
}

int VideoOutDriver::ChangeBufferAttribute(VideoOutPort* port, s32 attributeIndex,
                                          const BufferAttribute* attribute) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "attributeIndex = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             attributeIndex, GetPixelFormatString(attribute->pixel_format), attribute->aspect_ratio,
             static_cast<u32>(attribute->tiling_mode), attribute->width, attribute->height,
             attribute->pitch_in_pixel, attribute->option);

    std::unique_lock lock{port->port_mutex};
    std::memcpy(&port->groups[attributeIndex].attrib, attribute, sizeof(BufferAttribute));
    return 0;
}

// sotc: src/runtime_memory.c, live view splits since the last call.
extern "C" uint64_t runtime_memory_carve_splits(void);
// sotc: src/runtime_file.c, guest file read time since the last call.
extern "C" uint64_t runtime_file_read_ns_take(void);

void VideoOutDriver::Flip(const Request& req) {
    // Update HDR status before presenting.
    presenter->SetHDR(req.port->is_hdr);

    // Present the frame.
    presenter->Present(req.frame);

    // sotc: SOTC_FPS=1 prints the flip rate every five seconds.
    const bool fps_log = SotcStats::kFpsLog; // sotc: the switch the counters use (sotc_stats.h)
    if (fps_log) {
        static auto window_start = std::chrono::steady_clock::now();
        static const auto run_start = window_start;
        static u32 flips = 0;
        static uint64_t commands_seen = 0; // sotc: SotcStats::commands at the last line
        ++flips;
        const auto now = std::chrono::steady_clock::now();
        // sotc: frame times of this window, and where the command processor spent the time of
        // slow frames (SotcStats::FrameTimer sites) -> the "SOTC FRAMES" line.
        // Column FrameCauses ("io") is the time guest threads spent in file reads (all threads, so
        // it can exceed the frame); the last one counts bytes copied to the GPU ("copyMB").
        constexpr int kColumns = SotcStats::FrameCauses + 2;
        static auto last_flip = now;
        static std::vector<float> frame_ms;
        static double worst_ms = 0.0;
        static uint64_t worst_ns[kColumns]{}, slow_ns[kColumns]{};
        static u32 slow_frames = 0;
        const double interval_ms = std::chrono::duration<double, std::milli>(now - last_flip).count();
        last_flip = now;
        uint64_t cause_ns[kColumns];
        for (int c = 0; c < SotcStats::FrameCauses; ++c) {
            cause_ns[c] = SotcStats::frame_ns[c].exchange(0, std::memory_order_relaxed);
        }
        cause_ns[SotcStats::FrameCauses] = runtime_file_read_ns_take();
        cause_ns[SotcStats::FrameCauses + 1] = SotcStats::frame_copy_bytes.exchange(0);
        frame_ms.push_back(static_cast<float>(interval_ms));
        if (interval_ms > worst_ms) {
            worst_ms = interval_ms;
            std::copy(std::begin(cause_ns), std::end(cause_ns), std::begin(worst_ns));
        }
        if (interval_ms > 50.0) {
            ++slow_frames;
            for (int c = 0; c < kColumns; ++c) {
                slow_ns[c] += cause_ns[c];
            }
        }
        if (now - window_start >= std::chrono::seconds(5)) {
            const double seconds = std::chrono::duration<double>(now - window_start).count();
            {
                std::sort(frame_ms.begin(), frame_ms.end());
                const auto pct = [&](double p) {
                    return frame_ms[std::min(frame_ms.size() - 1,
                                             static_cast<size_t>(p * frame_ms.size()))];
                };
                const auto columns = [](const uint64_t* ns) {
                    std::string text;
                    char item[48];
                    for (int c = 0; c < kColumns; ++c) {
                        std::snprintf(item, sizeof(item), "%s%s %.1f", c ? ", " : "",
                                      c < SotcStats::FrameCauses ? SotcStats::kFrameCauseNames[c]
                                      : c == SotcStats::FrameCauses ? "io"
                                                                    : "copyMB",
                                      ns[c] / 1e6);
                        text += item;
                    }
                    return text;
                };
                std::fprintf(stderr,
                             "SOTC FRAMES p50 %.1f ms, p99 %.1f ms (1%% low %.1f FPS), worst %.1f ms "
                             "[%s]; >50 ms: %u [%s]; compiled %llu\n",
                             pct(0.5), pct(0.99), 1000.0 / pct(0.99), worst_ms,
                             columns(worst_ns).c_str(), slow_frames, columns(slow_ns).c_str(),
                             static_cast<unsigned long long>(SotcStats::frame_compiles.exchange(0)));
                std::string allocs;
                for (int k = 0; k < SotcStats::AllocKinds; ++k) {
                    const uint64_t count = SotcStats::alloc_count[k].exchange(0);
                    const uint64_t bytes = SotcStats::alloc_bytes[k].exchange(0);
                    const uint64_t ns = SotcStats::alloc_ns[k].exchange(0);
                    if (count) {
                        char item[96];
                        std::snprintf(item, sizeof(item), "%s%s %llu (%.1f MiB, %.1f ms)",
                                      allocs.empty() ? "" : ", ", SotcStats::kAllocKindNames[k],
                                      static_cast<unsigned long long>(count), bytes / 1048576.0,
                                      ns / 1e6);
                        allocs += item;
                    }
                }
                if (!allocs.empty()) {
                    std::fprintf(stderr, "SOTC ALLOC %s\n", allocs.c_str());
                }
                if (const uint64_t translate = SotcStats::compile_phase_ns[0].exchange(0)) {
                    const auto take = [](int i) {
                        return SotcStats::compile_phase_ns[i].exchange(0) / 1e6;
                    };
                    const double emit = take(1), patch = take(2), module = take(3), reg = take(4);
                    std::fprintf(stderr,
                                 "SOTC COMPILE translate %.1f, emit %.1f, patch lookup %.1f, "
                                 "module %.1f, register %.1f ms\n",
                                 translate / 1e6, emit, patch, module, reg);
                }
                frame_ms.clear();
                worst_ms = 0.0;
                std::fill(std::begin(worst_ns), std::end(worst_ns), 0);
                std::fill(std::begin(slow_ns), std::end(slow_ns), 0);
                slow_frames = 0;
            }
            const uint64_t readbacks = SotcStats::readbacks.exchange(0, std::memory_order_relaxed);
            const uint64_t readback_ns =
                SotcStats::readback_ns.exchange(0, std::memory_order_relaxed);
            const uint64_t copies = SotcStats::readback_copies.exchange(0);
            const uint64_t copies_changed = SotcStats::readback_copies_changed.exchange(0);
            const uint64_t bytes = SotcStats::readback_bytes.exchange(0);
            const uint64_t bytes_changed = SotcStats::readback_bytes_changed.exchange(0);
            std::fprintf(stderr,
                         "SOTC FPS %.1f at %.0fs; %.0f cmds/frame; readbacks %.0f/s (ahead %llu, finish %llu, heap %llu, early %llu), %.0f ms/s; "
                         "changed copies %llu/%llu, bytes %llu/%llu\n",
                         flips / seconds, std::chrono::duration<double>(now - run_start).count(),
                         flips ? double(SotcStats::TakeDelta(SotcStats::commands, commands_seen)) / flips : 0.0,
                         readbacks / seconds,
                         static_cast<unsigned long long>(SotcStats::readback_ahead.exchange(0)),
                         static_cast<unsigned long long>(SotcStats::readback_finish.exchange(0)),
                         static_cast<unsigned long long>(SotcStats::heap_guard_builds.exchange(0)),
                         static_cast<unsigned long long>(SotcStats::early_flushes.exchange(0)),
                         readback_ns / 1e6 / seconds,
                         static_cast<unsigned long long>(copies_changed),
                         static_cast<unsigned long long>(copies),
                         static_cast<unsigned long long>(bytes_changed),
                         static_cast<unsigned long long>(bytes));
            if (const uint64_t sweeps = SotcStats::dma_sweeps.exchange(0)) {
                const uint64_t ranges = SotcStats::dma_sweep_ranges.exchange(0);
                const uint64_t uploading = SotcStats::dma_sweep_uploading.exchange(0);
                const uint64_t runs = SotcStats::dma_sweep_runs.exchange(0);
                const uint64_t sweep_ns = SotcStats::dma_sweep_ns.exchange(0);
                const uint64_t idle_ns = SotcStats::dma_sweep_idle_ns.exchange(0);
                std::fprintf(stderr,
                             "SOTC DMA sweeps %.0f/s, %.1f ranges each (%.1f runs), %.0f/s "
                             "uploading; %.1f ms/s (%.1f in sweeps that uploaded nothing)\n",
                             sweeps / seconds, double(ranges) / sweeps, double(runs) / sweeps,
                             uploading / seconds, sweep_ns / 1e6 / seconds,
                             idle_ns / 1e6 / seconds);
            }
            if (const uint64_t images = SotcStats::census_count.load(std::memory_order_relaxed)) {
                const auto mib = [](const std::atomic<uint64_t>& v) {
                    return static_cast<unsigned long long>(v.load(std::memory_order_relaxed) >> 20);
                };
                std::fprintf(stderr,
                             "SOTC IMAGES %llu: gpu-written tiled %llu MiB, linear %llu MiB; "
                             "sampled streamed %llu MiB, other %llu MiB; below base mip %llu MiB, "
                             "below min_lod %llu MiB; live %llu MiB\n",
                             static_cast<unsigned long long>(images), mib(SotcStats::census_gpu_tiled),
                             mib(SotcStats::census_gpu_linear), mib(SotcStats::census_streamed),
                             mib(SotcStats::census_other), mib(SotcStats::census_below_base),
                             mib(SotcStats::census_below_minlod), mib(SotcStats::census_live));
                // sotc: mip-tail images (SOTC_MIP_TAIL): what they hold and leave out, what the
                // full-chain candidates would save, and the decisions since the last line.
                const auto take = [](std::atomic<uint64_t>& v) {
                    return static_cast<unsigned long long>(v.exchange(0));
                };
                std::fprintf(
                    stderr,
                    "SOTC MIPTAIL rebased %llu MiB in %llu (dropped %llu MiB guest), projected "
                    "%llu MiB (hwcnt %llu); created %llu, reuse-full %llu, skip gpu %llu "
                    "live-full %llu layout %llu, writer-freed %llu, dedup-freed %llu, "
                    "maybe-dirty %llu, VIOLATIONS %llu\n",
                    mib(SotcStats::census_rebased),
                    static_cast<unsigned long long>(
                        SotcStats::census_rebased_count.load(std::memory_order_relaxed)),
                    mib(SotcStats::census_dropped), mib(SotcStats::census_projected),
                    mib(SotcStats::census_projected_hwcnt), take(SotcStats::rebase_created),
                    take(SotcStats::rebase_reuse_full), take(SotcStats::rebase_skip_gpu),
                    take(SotcStats::rebase_skip_live_full), take(SotcStats::rebase_skip_layout),
                    take(SotcStats::rebase_writer_freed), take(SotcStats::rebase_dedup_freed),
                    take(SotcStats::rebase_maybe_dirty),
                    static_cast<unsigned long long>(
                        SotcStats::rebase_violations.load(std::memory_order_relaxed)));
            }
            const uint64_t grace_marks = SotcStats::fault_grace_marks.exchange(0);
            const uint64_t grace_caps = SotcStats::fault_grace_cap_hits.exchange(0);
            const uint64_t grace_bytes = SotcStats::fault_grace_hot_upload_bytes.exchange(0);
            const uint64_t reg_spins = SotcStats::stack_reg_spins.exchange(0);
            const uint64_t carves = runtime_memory_carve_splits();
            if (grace_marks || grace_caps || grace_bytes || reg_spins || carves) {
                std::fprintf(stderr,
                             "SOTC fault grace: marks %llu, cap hits %llu, hot uploads %llu KiB; "
                             "stack registry spins %llu; live view splits %llu\n",
                             static_cast<unsigned long long>(grace_marks),
                             static_cast<unsigned long long>(grace_caps),
                             static_cast<unsigned long long>(grace_bytes >> 10),
                             static_cast<unsigned long long>(reg_spins),
                             static_cast<unsigned long long>(carves));
            }
            const uint64_t repairs = SotcStats::desync_repairs.exchange(0);
            const uint64_t legit = SotcStats::desync_legit.exchange(0);
            if (repairs || legit) {
                std::fprintf(stderr,
                             "SOTC page tracking desync repairs %llu (repeated faults left alone "
                             "%llu)\n",
                             static_cast<unsigned long long>(repairs),
                             static_cast<unsigned long long>(legit));
            }
            // sotc: video memory against the budget the texture cache collects for; emergency
            // collections mean an allocation already failed (6 GB cards in large areas).
            if (const uint64_t budget = SotcStats::vram_budget.load(std::memory_order_relaxed)) {
                std::fprintf(stderr,
                             "SOTC VRAM %llu/%llu MiB (VMA blocks %llu MiB holding %llu MiB, "
                             "buffer arenas %llu MiB, images %llu MiB in "
                             "%llu; GC freed %llu, kept tiled %llu), emergency collections %llu\n",
                             static_cast<unsigned long long>(
                                 SotcStats::vram_used.load(std::memory_order_relaxed) >> 20),
                             static_cast<unsigned long long>(budget >> 20),
                             static_cast<unsigned long long>(
                                 SotcStats::vma_block_bytes.load(std::memory_order_relaxed) >> 20),
                             static_cast<unsigned long long>(
                                 SotcStats::vma_allocation_bytes.load(std::memory_order_relaxed) >>
                                 20),
                             static_cast<unsigned long long>(
                                 SotcStats::arena_bytes.load(std::memory_order_relaxed) >> 20),
                             static_cast<unsigned long long>(
                                 SotcStats::image_bytes.load(std::memory_order_relaxed) >> 20),
                             static_cast<unsigned long long>(
                                 SotcStats::image_count.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(SotcStats::gc_freed.exchange(0)),
                             static_cast<unsigned long long>(SotcStats::gc_kept_tiled.exchange(0)),
                             static_cast<unsigned long long>(
                                 SotcStats::vram_emergency.exchange(0)));
            }
            // sotc: write-fault readbacks whose pages were still GPU-modified (totals).
            if (const uint64_t pending = SotcStats::write_fault_gpu_pending.load(),
                stale = SotcStats::write_fault_stale_gpu.load();
                pending || stale) {
                std::fprintf(stderr,
                             "SOTC write faults over GPU data: left protected %llu, stale page "
                             "bits cleared %llu\n",
                             static_cast<unsigned long long>(pending),
                             static_cast<unsigned long long>(stale));
            }
            // sotc: cold buffer arena memory given back (BufferCache::MaybeEvictCold), totals.
            if (const uint64_t runs = SotcStats::arena_evict_runs.load(std::memory_order_relaxed)) {
                std::fprintf(
                    stderr,
                    "SOTC ARENA evicted %llu MiB in %llu runs (%.1f ms), rebound %llu MiB, "
                    "faulted back %llu MiB, kept unequal %llu MiB\n",
                    static_cast<unsigned long long>(
                        SotcStats::arena_evicted.load(std::memory_order_relaxed) >> 20),
                    static_cast<unsigned long long>(runs),
                    SotcStats::arena_evict_ns.load(std::memory_order_relaxed) / 1e6,
                    static_cast<unsigned long long>(
                        SotcStats::arena_rebound.load(std::memory_order_relaxed) >> 20),
                    static_cast<unsigned long long>(
                        SotcStats::arena_pinned.load(std::memory_order_relaxed) >> 20),
                    static_cast<unsigned long long>(
                        SotcStats::arena_evict_mismatch.load(std::memory_order_relaxed) >> 20));
            }
            const bool cmd_stats = SotcStats::kCmdStats; // sotc: as the counters (sotc_stats.h)
            if (cmd_stats && flips) {
                double k[6];
                for (int i = 0; i < 6; ++i) {
                    k[i] = double(SotcStats::cmd_kinds[i].exchange(0)) / flips;
                }
                std::fprintf(stderr,
                             "SOTC cmds/frame: draw %.0f, indirect %.0f gpu-args + %.0f cpu-args "
                             "(%.0f empty), dispatch %.0f, dispatch-indirect %.0f\n",
                             k[0], k[1], k[2], k[3], k[4], k[5]);
                std::fprintf(stderr,
                             "SOTC faults/s: guest writes %.0f, walker loads %.0f, CP reads %.0f\n",
                             SotcStats::fault_writes.exchange(0) / seconds,
                             SotcStats::fault_walker.exchange(0) / seconds,
                             SotcStats::fault_gpu_reads.exchange(0) / seconds);
                std::fprintf(stderr,
                             "SOTC per frame: uploads %.0f (%.0f KB), render pass breaks %.0f\n",
                             double(SotcStats::uploads.exchange(0)) / flips,
                             double(SotcStats::upload_bytes.exchange(0)) / 1024.0 / flips,
                             double(SotcStats::render_breaks.exchange(0)) / flips);
                std::fprintf(stderr,
                             "SOTC sys/frame: protects %.0f (%.0f writable, %.0f KB), "
                             "texture cache locks %.0f (%.0f contended, %.3f ms waiting), vk allocs %.1f, frees %.1f; protects from images %.0f, "
                             "buffers %.0f; invalidated KB: map %.0f, mprotect %.0f, hle %.0f, "
                             "all %.0f\n",
                             double(SotcStats::sys_protects.exchange(0)) / flips,
                             double(SotcStats::sys_unprotects.exchange(0)) / flips,
                             double(SotcStats::sys_protect_bytes.exchange(0)) / 1024.0 / flips,
                             double(SotcStats::sys_tc_locks.exchange(0)) / flips,
                             double(SotcStats::sys_tc_contended.exchange(0)) / flips,
                             SotcStats::sys_tc_wait_ns.exchange(0) / 1e6 / flips,
                             double(SotcStats::sys_vk_allocs.exchange(0)) / flips,
                             double(SotcStats::sys_vk_frees.exchange(0)) / flips,
                             double(SotcStats::sys_protect_src[0].exchange(0)) / flips,
                             double(SotcStats::sys_protect_src[1].exchange(0)) / flips,
                             SotcStats::inv_map.exchange(0) / 1024.0 / flips,
                             SotcStats::inv_protect.exchange(0) / 1024.0 / flips,
                             SotcStats::inv_hle.exchange(0) / 1024.0 / flips,
                             SotcStats::inv_buffer.exchange(0) / 1024.0 / flips);
            }
            flips = 0;
            window_start = now;
        }
    }

    // Update flip status.
    auto* port = req.port;
    {
        std::unique_lock lock{port->port_mutex};
        auto& flip_status = port->flip_status;
        flip_status.count++;
        flip_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
        flip_status.tsc = Libraries::Kernel::sceKernelReadTsc();
        flip_status.flip_arg = req.flip_arg;
        flip_status.current_buffer = req.index;
        if (req.eop) {
            --flip_status.gc_queue_num;
        }
        --flip_status.flip_pending_num;
    }

    // Trigger flip events for the port.
    for (auto event : port->flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->TriggerEvent(
                static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                Kernel::OrbisKernelEvent::Filter::VideoOut,
                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Flip) |
                                        (req.flip_arg << 16)));
        }
    }

    // Reset prev flip label
    if (port->prev_index != -1) {
        port->buffer_labels[port->prev_index] = 0;
        port->SignalVoLabel();
    }
    // save to prev buf index
    port->prev_index = req.index;
}

void VideoOutDriver::DrawBlankFrame() {
    const auto empty_frame = presenter->PrepareBlankFrame(true);
    presenter->Present(empty_frame, false, false);
}

void VideoOutDriver::DrawLastFrame() {
    const auto frame = presenter->PrepareLastFrame();
    if (frame != nullptr) {
        presenter->Present(frame, true);
    }
}

bool VideoOutDriver::SubmitFlip(VideoOutPort* port, s32 index, s64 flip_arg,
                                bool is_eop /*= false*/) {
    {
        std::unique_lock lock{port->port_mutex};
        if (index != -1 && port->flip_status.flip_pending_num > 16) {
            LOG_ERROR(Lib_VideoOut, "Flip queue is full");
            return false;
        }

        if (is_eop) {
            ++port->flip_status.gc_queue_num;
        }
        ++port->flip_status.flip_pending_num; // integral GPU and CPU pending flips counter
        port->flip_status.submit_tsc = Libraries::Kernel::sceKernelReadTsc();
    }

    if (!is_eop) {
        // Non EOP flips can arrive from any thread so ask GPU thread to perform them
        liverpool->SendCommand([=, this]() { SubmitFlipInternal(port, index, flip_arg, is_eop); });
    } else {
        SubmitFlipInternal(port, index, flip_arg, is_eop);
    }

    return true;
}

void VideoOutDriver::SubmitFlipInternal(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop) {
    Vulkan::Frame* frame;
    if (index == -1) {
        frame = presenter->PrepareBlankFrame(false);
    } else {
        const auto& buffer = port->buffer_slots[index];
        ASSERT_MSG(buffer.group_index >= 0, "Trying to flip an unregistered buffer!");
        const auto& group = port->groups[buffer.group_index];
        frame = presenter->PrepareFrame(group, buffer.address_left);
    }

    std::scoped_lock lock{mutex};
    requests.push({
        .frame = frame,
        .port = port,
        .flip_arg = flip_arg,
        .index = index,
        .eop = is_eop,
    });
}

void VideoOutDriver::PresentThread(std::stop_token token) {
    const std::chrono::nanoseconds vblank_period(1000000000 /
                                                 EmulatorSettings.GetVblankFrequency());

    Common::SetCurrentThreadName("shadPS4:PresentThread");
    Common::SetCurrentThreadRealtime(vblank_period);

    Common::AccurateTimer timer{vblank_period};

    const auto receive_request = [this] -> Request {
        std::scoped_lock lk{mutex};
        if (!requests.empty()) {
            const auto request = requests.front();
            requests.pop();
            return request;
        }
        return {};
    };

    while (!token.stop_requested()) {
        timer.Start();

        if (DebugState.IsGuestThreadsPaused()) {
            DrawLastFrame();
            timer.End();
            continue;
        }

        // Check if it's time to take a request.
        auto& vblank_status = main_port.vblank_status;
        if (vblank_status.count % (main_port.flip_rate + 1) == 0) {
            const auto request = receive_request();
            if (!request) {
                if (timer.GetTotalWait().count() < 0) { // Dont draw too fast
                    if (!main_port.is_open) {
                        DrawBlankFrame();
                    } else if (ImGui::Core::MustKeepDrawing()) {
                        DrawLastFrame();
                    }
                }
            } else {
                Flip(request);
                FRAME_END;
            }
        }

        {
            // Needs lock here as can be concurrently read by `sceVideoOutGetVblankStatus`
            std::scoped_lock lock{main_port.vo_mutex};

            // Trigger flip events for the port
            for (auto event : main_port.vblank_events) {
                auto equeue = Kernel::GetEqueue(event);
                if (equeue != nullptr) {
                    equeue->TriggerEvent(
                        static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                        Kernel::OrbisKernelEvent::Filter::VideoOut,
                        reinterpret_cast<void*>(
                            static_cast<u64>(OrbisVideoOutInternalEventId::Vblank) |
                            (vblank_status.count << 16)));
                }
            }

            // Update vblank status
            vblank_status.count++;
            vblank_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
            vblank_status.tsc = Libraries::Kernel::sceKernelReadTsc();
            main_port.vblank_cv.notify_all();
        }

        timer.End();
    }
}

} // namespace Libraries::VideoOut
