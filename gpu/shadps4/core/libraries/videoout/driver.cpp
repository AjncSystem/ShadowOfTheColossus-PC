// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdio>
#include <cstdlib>
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

/// sotc: guest memory faults draw preparation helpers recovered from (src/probe.c), and their
/// assertions (gpu/shim/common/assert.cpp).
extern "C" volatile long long runtime_speculative_faults;
extern std::atomic<long long> g_bb_speculative_assert_count;

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
    if (EmulatorSettingsImpl::Flag("SOTC_PRESENT_THREAD", true)) {
        swap_queue = std::make_unique<Common::OrderedWorkQueue>([] {
            Common::SetCurrentThreadName("sotc:Present");
        });
    }
    present_thread = std::jthread([&](std::stop_token token) { PresentThread(token); });
}

VideoOutDriver::~VideoOutDriver() {
    present_thread.request_stop();
    request_cv.notify_all();
    if (present_thread.joinable()) present_thread.join();
    if (swap_queue) swap_queue->Shutdown();
}

void VideoOutDriver::RunPresenter(std::function<void()> work, bool only_if_idle) {
    if (swap_queue) {
        swap_queue->Submit(std::move(work), only_if_idle);
    } else {
        work();
    }
}

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

        // sotc: images are created on the command processor only (TextureCache).
        liverpool->SendCommand<true>(
            [&group, address] { presenter->RegisterVideoOutSurface(group, address); });
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

void VideoOutDriver::Flip(const Request& req) {
    // Update HDR status before presenting.
    RunPresenter([frame = req.frame, hdr = req.port->is_hdr] {
        presenter->SetHDR(hdr);
        presenter->Present(frame);
    });

    // sotc: SOTC_FPS=1 prints the flip rate every five seconds.
    static const bool fps_log = std::getenv("SOTC_FPS") != nullptr;
    if (fps_log) {
        static auto window_start = std::chrono::steady_clock::now();
        static const auto run_start = window_start;
        static u32 flips = 0;
        ++flips;
        const auto now = std::chrono::steady_clock::now();
        if (now - window_start >= std::chrono::seconds(5)) {
            const double seconds = std::chrono::duration<double>(now - window_start).count();
            const uint64_t readbacks = SotcStats::readbacks.exchange(0, std::memory_order_relaxed);
            const uint64_t readback_ns =
                SotcStats::readback_ns.exchange(0, std::memory_order_relaxed);
            const uint64_t copies = SotcStats::readback_copies.exchange(0);
            const uint64_t copies_changed = SotcStats::readback_copies_changed.exchange(0);
            const uint64_t bytes = SotcStats::readback_bytes.exchange(0);
            const uint64_t bytes_changed = SotcStats::readback_bytes_changed.exchange(0);
            const uint64_t rb_ahead = SotcStats::readback_ahead.exchange(0);
            const uint64_t rb_finish = SotcStats::readback_finish.exchange(0);
            std::fprintf(stderr,
                         "SOTC FPS %.1f at %.0fs; %.0f cmds/frame; readbacks %.0f/s (ahead %llu, finish %llu, heap %llu, early %llu), %.0f ms/s; "
                         "changed copies %llu/%llu, bytes %llu/%llu; vram %llu/%llu MB, "
                         "arenas %llu (%llu MB)\n",
                         flips / seconds, std::chrono::duration<double>(now - run_start).count(),
                         flips ? double(SotcStats::commands.exchange(0)) / flips : 0.0,
                         readbacks / seconds,
                         static_cast<unsigned long long>(rb_ahead),
                         static_cast<unsigned long long>(rb_finish),
                         static_cast<unsigned long long>(SotcStats::heap_guard_builds.exchange(0)),
                         static_cast<unsigned long long>(SotcStats::early_flushes.exchange(0)),
                         readback_ns / 1e6 / seconds,
                         static_cast<unsigned long long>(copies_changed),
                         static_cast<unsigned long long>(copies),
                         static_cast<unsigned long long>(bytes_changed),
                         static_cast<unsigned long long>(bytes),
                         static_cast<unsigned long long>(SotcStats::vram_used.load() >> 20),
                         static_cast<unsigned long long>(SotcStats::vram_budget.load() >> 20),
                         static_cast<unsigned long long>(SotcStats::arena_allocs.load()),
                         static_cast<unsigned long long>(SotcStats::arena_bytes.load() >> 20));
            std::fprintf(stderr, "SOTC readback finish: %llu for bytes the guest asked\n",
                         static_cast<unsigned long long>(
                             SotcStats::readback_finish_asked.exchange(0)));
            static uint64_t last_images_new = 0;
            const uint64_t images_new = SotcStats::images_new.load();
            std::fprintf(stderr,
                         "SOTC GC: %.1f ticks/frame, freed %.0f/s (%llu written back), images new "
                         "%.0f/s, cached %llu (%llu MB)\n",
                         flips ? double(SotcStats::gc_ticks.exchange(0)) / flips : 0.0,
                         SotcStats::gc_freed.exchange(0) / seconds,
                         static_cast<unsigned long long>(SotcStats::gc_downloads.exchange(0)),
                         (images_new - last_images_new) / seconds,
                         static_cast<unsigned long long>(SotcStats::cached_images.load()),
                         static_cast<unsigned long long>(SotcStats::cached_image_bytes.load() >> 20));
            last_images_new = images_new;
            const uint64_t async_built = SotcStats::async_compiles.exchange(0);
            const uint64_t async_skips = SotcStats::async_skips.exchange(0);
            const uint64_t async_waits = SotcStats::async_waits.exchange(0);
            const uint64_t compute_swaps = SotcStats::compute_swaps.exchange(0);
            uint64_t churn[5];
            for (int i = 0; i < 5; ++i) {
                churn[i] = SotcStats::image_churn[i].exchange(0);
            }
            if (churn[0] + churn[1] + churn[2] + churn[3] + churn[4]) {
                std::fprintf(stderr,
                             "SOTC images/s: overlap-recreate %.0f, expand/twin-reuse %.0f, new %.0f, "
                             "stencil %.0f, deleted %.0f\n",
                             churn[0] / seconds, churn[1] / seconds, churn[2] / seconds,
                             churn[3] / seconds, churn[4] / seconds);
            }
            if (async_built || async_skips || async_waits || compute_swaps) {
                std::fprintf(stderr,
                             "SOTC pipelines: %llu built on workers, %llu draws skipped, "
                             "%llu waits, %llu compute optimized\n",
                             static_cast<unsigned long long>(async_built),
                             static_cast<unsigned long long>(async_skips),
                             static_cast<unsigned long long>(async_waits),
                             static_cast<unsigned long long>(compute_swaps));
            }
            static const bool cmd_stats = std::getenv("SOTC_CMD_STATS") != nullptr;
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
                             "SOTC per frame: uploads %.0f (%.0f KB), render pass breaks %.0f, "
                             "texture binding memo hits %.0f (verify mismatches %llu), target "
                             "memo hits %.0f, buffer access adds %.0f (%.0f already recorded), "
                             "render states reused %.0f (verify mismatches %llu)\n",
                             double(SotcStats::uploads.exchange(0)) / flips,
                             double(SotcStats::upload_bytes.exchange(0)) / 1024.0 / flips,
                             double(SotcStats::render_breaks.exchange(0)) / flips,
                             double(SotcStats::tex_bind_memo_hits.exchange(0)) / flips,
                             static_cast<unsigned long long>(
                                 SotcStats::tex_bind_memo_mismatch.load()),
                             double(SotcStats::rt_memo_hits.exchange(0)) / flips,
                             double(SotcStats::access_adds.exchange(0)) / flips,
                             double(SotcStats::access_adds_skipped.exchange(0)) / flips,
                             double(SotcStats::render_state_reused.exchange(0)) / flips,
                             static_cast<unsigned long long>(
                                 SotcStats::render_state_mismatch.load()));
                std::fprintf(stderr,
                             "SOTC texel syncs/frame: %.0f (metadata %.0f, image re-tiled %.0f)\n",
                             double(SotcStats::texel_syncs[0].exchange(0)) / flips,
                             double(SotcStats::texel_syncs[1].exchange(0)) / flips,
                             double(SotcStats::texel_syncs[2].exchange(0)) / flips);
                if (SotcStats::PhaseTime()) {
                    // TSC ticks per microsecond over the interval since the previous line.
                    static u64 last_tsc = __builtin_ia32_rdtsc();
                    static auto last_time = std::chrono::steady_clock::now();
                    const u64 tsc = __builtin_ia32_rdtsc();
                    const auto now_time = std::chrono::steady_clock::now();
                    const double us =
                        std::chrono::duration<double, std::micro>(now_time - last_time).count();
                    const double per_us = us > 0 ? double(tsc - last_tsc) / us : 1.0;
                    last_tsc = tsc;
                    last_time = now_time;
                    double ms[SotcStats::NumPhases];
                    for (int i = 0; i < SotcStats::NumPhases; ++i) {
                        ms[i] = double(SotcStats::phase_cycles[i].exchange(0)) / per_us / 1000.0 /
                                flips;
                    }
                    std::fprintf(stderr,
                                 "SOTC phase ms/frame: draw pipeline %.2f, targets %.2f, "
                                 "bindings %.2f, begin rendering %.2f, record %.2f, after %.2f; "
                                 "dispatch pipeline+HLE %.2f, bindings %.2f, rest %.2f\n",
                                 ms[0], ms[1], ms[2], ms[3], ms[4], ms[5], ms[6], ms[7], ms[8]);
                    std::fprintf(stderr,
                                 "SOTC subphase ms/frame: buffers %.2f, textures %.2f; begin "
                                 "rendering: color update %.2f, FindRenderTarget %.2f, color "
                                 "meta %.2f, color transit %.2f, FindDepthTarget %.2f, depth "
                                 "meta %.2f, depth transits %.2f\n",
                                 ms[9], ms[10], ms[11], ms[12], ms[13], ms[14], ms[15], ms[16],
                                 ms[17]);
                    std::fprintf(stderr,
                                 "SOTC record ms/frame (indirect, CPU args): vertex/index %.2f, "
                                 "args+barriers %.2f, descriptors %.2f, dynamic state %.2f, "
                                 "pass+pipeline+draw %.2f\n",
                                 ms[18], ms[19], ms[20], ms[21], ms[22]);
                    std::fprintf(stderr,
                                 "SOTC bind ms/frame: textures lookup %.2f, views+layouts %.2f, "
                                 "samplers %.2f; buffers range %.2f, ObtainBuffer %.2f, written "
                                 "%.2f, hazard+descriptor %.2f, special %.2f; compute "
                                 "special checks %.2f\n",
                                 ms[23], ms[24], ms[25], ms[26], ms[27], ms[28], ms[29], ms[30],
                                 ms[31]);
                    std::fprintf(stderr,
                                 "SOTC prepared selection ms/frame: program params %.2f, walker "
                                 "%.2f, spec inputs %.2f, fetch shader %.2f, rest %.2f\n",
                                 ms[32], ms[33], ms[34], ms[35], ms[36]);
                    std::fprintf(stderr,
                                 "SOTC texture blocks ms/frame %.2f; recorder busy ms/frame %.2f\n",
                                 ms[37],
                                 double(SotcStats::recorder_busy_ns.exchange(0)) / 1e6 / flips);
                }
                std::fprintf(stderr,
                             "SOTC split/frame: finish %.2f, host-read finish %.2f, ahead waits "
                             "%.2f, tick waits %.2f; CP invalidations %.1f (over images %.1f)\n",
                             double(SotcStats::cp_drains[0].exchange(0)) / flips,
                             double(SotcStats::cp_drains[1].exchange(0)) / flips,
                             double(SotcStats::cp_drains[2].exchange(0)) / flips,
                             double(SotcStats::cp_drains[3].exchange(0)) / flips,
                             double(SotcStats::cp_invalidations[0].exchange(0)) / flips,
                             double(SotcStats::cp_invalidations[1].exchange(0)) / flips);
                std::fprintf(stderr,
                             "SOTC tex blocks/frame: reused %.0f, bound %.0f, not keyed %.0f, "
                             "stored %.0f (not stored: other binding %.0f, image set %.0f, size "
                             "%.0f, write %.0f; missed: key %.0f, image set %.0f, image %.0f, "
                             "upload %.0f), VERIFY MISMATCHES %llu; image state changes %.0f "
                             "whole, %.0f per subresource\n",
                             double(SotcStats::tex_block[0].exchange(0)) / flips,
                             double(SotcStats::tex_block[1].exchange(0)) / flips,
                             double(SotcStats::tex_block[2].exchange(0)) / flips,
                             double(SotcStats::tex_block[3].exchange(0)) / flips,
                             double(SotcStats::tex_block_reject[0].exchange(0)) / flips,
                             double(SotcStats::tex_block_reject[1].exchange(0)) / flips,
                             double(SotcStats::tex_block_reject[2].exchange(0)) / flips,
                             double(SotcStats::tex_block_reject[3].exchange(0)) / flips,
                             double(SotcStats::tex_block_miss[0].exchange(0)) / flips,
                             double(SotcStats::tex_block_miss[1].exchange(0)) / flips,
                             double(SotcStats::tex_block_miss[2].exchange(0)) / flips,
                             double(SotcStats::tex_block_miss[3].exchange(0)) / flips,
                             static_cast<unsigned long long>(SotcStats::tex_block[4].load()),
                             double(SotcStats::image_state_changes[0].exchange(0)) / flips,
                             double(SotcStats::image_state_changes[1].exchange(0)) / flips);
                {
                    double b[9];
                    for (int i = 0; i < 9; ++i) {
                        b[i] = double(SotcStats::barrier_breaks[i].exchange(0)) / flips;
                    }
                    std::fprintf(stderr,
                                 "SOTC barrier breaks/frame: indirect %.1f, vertex %.1f, index "
                                 "%.1f, gds %.1f, buffer read %.1f, buffer write %.1f, texture "
                                 "%.1f, color target %.1f, depth target %.1f\n",
                                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8]);
                    double s[8];
                    for (int i = 0; i < 8; ++i) {
                        s[i] = double(SotcStats::sync_events[i].exchange(0)) / flips;
                    }
                    std::fprintf(stderr,
                                 "SOTC sync/frame: ps flush %.1f, cs flush %.1f, vs flush %.1f, "
                                 "cache flush %.1f, eos %.1f, eop %.1f, acquire_mem %.1f, "
                                 "wait_reg_mem %.1f\n",
                                 s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
                }
                const double f = flips;
                const double readback_wait_ms = SotcStats::readback_wait_ns.exchange(0) / 1e6;
                const double readback_wait_frame = readback_wait_ms / f;
                const double traced = double(SotcStats::readback_traced.exchange(0));
                const double gap_us = double(SotcStats::readback_gap_us.exchange(0));
                const double gap_cmds = double(SotcStats::readback_gap_cmds.exchange(0));
                std::fprintf(stderr,
                             "SOTC rb/frame: calls %.2f, real %.2f (ahead %.2f, finish %.2f; "
                             "extra-forced %.2f, rewritten %.2f), noop %.2f, dedup %.2f (miss %.2f), "
                             "fence flushes %.2f, extras deferred %.2f; wait %.0f ms/s; "
                             "gap %.0f us %.0f cmds; recorder %.0f ms/s; async joined %.2f, "
                             "write refreshes %.2f\n",
                             readbacks / f, (rb_ahead + rb_finish) / f, rb_ahead / f,
                             rb_finish / f, SotcStats::readback_finish_extra.exchange(0) / f,
                             SotcStats::readback_rewritten.exchange(0) / f,
                             SotcStats::readback_noop.exchange(0) / f,
                             SotcStats::readback_dedup.exchange(0) / f,
                             SotcStats::readback_dedup_miss.exchange(0) / f,
                             SotcStats::fence_flushes.exchange(0) / f,
                             SotcStats::readback_extra_deferred.exchange(0) / f,
                             readback_wait_ms / seconds,
                             traced ? gap_us / traced : 0.0, traced ? gap_cmds / traced : 0.0,
                             SotcStats::recorder_wait_ns.exchange(0) / 1e6 / seconds,
                             SotcStats::readback_joined.exchange(0) / f,
                             SotcStats::readback_write_refresh.exchange(0) / f);
                // Command processor time per frame: what is left after its waits is the work
                // (unaffected by the vblank rounding of the frame rate).
                const double ms_frame = 1000.0 * seconds / f;
                const double idle_ms = SotcStats::cp_idle_ns.exchange(0) / 1e6 / f;
                const double vo_ms = SotcStats::cp_vo_wait_ns.exchange(0) / 1e6 / f;
                const double rb_ms = readback_wait_frame;
                std::fprintf(stderr,
                             "SOTC cp/frame: %.2f ms = work %.2f + readback wait %.2f + "
                             "flip wait %.2f + idle %.2f\n",
                             ms_frame, ms_frame - idle_ms - vo_ms - rb_ms, rb_ms, vo_ms, idle_ms);
                std::fprintf(stderr,
                             "SOTC scan/frame: gfx submits %.1f (%.0f KB), draws top %.0f nested %.0f, "
                             "ASC gfx-SH writes %.1f, CondExec skips %.1f, register words %.0f\n",
                             SotcStats::gfx_submits.exchange(0) / f,
                             SotcStats::gfx_submit_words.exchange(0) * 4.0 / 1024.0 / f,
                             SotcStats::draws_top.exchange(0) / f,
                             SotcStats::draws_nested.exchange(0) / f,
                             SotcStats::asc_gfx_sh_writes.exchange(0) / f,
                             SotcStats::condexec_skips.exchange(0) / f,
                             SotcStats::reg_words.exchange(0) / f);
                const double scan_total =
                    double(SotcStats::scan_agree.load() + SotcStats::scan_mismatch.load() +
                           SotcStats::scan_late.load() + SotcStats::scan_lost.load());
                if (scan_total > 0) {
                    std::fprintf(stderr,
                                 "SOTC drawscan: agree %.1f%%, mismatch %.1f%%, late %.1f%%, "
                                 "lost %.1f%%\n",
                                 100.0 * SotcStats::scan_agree.exchange(0) / scan_total,
                                 100.0 * SotcStats::scan_mismatch.exchange(0) / scan_total,
                                 100.0 * SotcStats::scan_late.exchange(0) / scan_total,
                                 100.0 * SotcStats::scan_lost.exchange(0) / scan_total);
                    if (const u64 checked = SotcStats::scan_regs_checked.exchange(0)) {
                        std::fprintf(stderr, "SOTC drawscan regs: %llu compared, %llu differ\n",
                                     static_cast<unsigned long long>(checked),
                                     static_cast<unsigned long long>(
                                         SotcStats::scan_regs_differ.exchange(0)));
                    }
                }
                {
                    // SOTC_DRAW_PREP (vk_draw_prep.h): per draw, as a share of all draws.
                    u64 results = 0, unready[8], reject[8], unready_sum = 0, reject_sum = 0;
                    for (int i = 0; i < 8; ++i) {
                        results += SotcStats::prep_results[i].exchange(0);
                        unready[i] = SotcStats::prep_unready[i].exchange(0);
                        reject[i] = SotcStats::prep_reject[i].exchange(0);
                        unready_sum += unready[i];
                        reject_sum += reject[i];
                    }
                    const u64 accept = SotcStats::prep_accept.exchange(0);
                    const u64 none = SotcStats::prep_none.exchange(0);
                    const double total = double(accept + unready_sum + reject_sum + none);
                    if (total > 0) {
                        const auto pct = [&](u64 n) { return 100.0 * double(n) / total; };
                        std::fprintf(
                            stderr,
                            "SOTC prep/frame: draws %.0f, accepted %.1f%%, no record %.1f%%, "
                            "no proposal %.1f%% (config %.1f bininfo %.1f program %.1f fault %.1f "
                            "fetch %.1f perm %.1f), rejected %.1f%% (params %.1f code %.1f bind "
                            "%.1f perm %.1f flat %.1f fetch %.1f vsharp %.1f), prepared %.0f, "
                            "verify mismatches %llu, helper faults %lld, helper asserts %lld\n",
                            total / f, pct(accept), pct(none), pct(unready_sum), pct(unready[1]),
                            pct(unready[2]), pct(unready[3]), pct(unready[4]), pct(unready[5]),
                            pct(unready[6]), pct(reject_sum), pct(reject[0]), pct(reject[1]),
                            pct(reject[2]), pct(reject[3]), pct(reject[4]), pct(reject[5]),
                            pct(reject[6]), double(results) / f,
                            static_cast<unsigned long long>(SotcStats::prep_mismatch.exchange(0)),
                            static_cast<long long>(runtime_speculative_faults),
                            static_cast<long long>(g_bb_speculative_assert_count.load()));
                        if (const u64 later = SotcStats::prep_diag_later.exchange(0),
                            read = SotcStats::prep_diag_read.exchange(0),
                            other = SotcStats::prep_diag_other.exchange(0);
                            later + read + other) {
                            std::fprintf(stderr,
                                         "SOTC prep diag: flat rejects changed later %llu, read "
                                         "differs %llu, other %llu\n",
                                         static_cast<unsigned long long>(later),
                                         static_cast<unsigned long long>(read),
                                         static_cast<unsigned long long>(other));
                        }
                    }
                }
                std::fprintf(stderr, "SOTC submits/frame: periodic %.1f, gpu-idle %.1f\n",
                             SotcStats::periodic_flushes.exchange(0) / f,
                             SotcStats::idle_flushes.exchange(0) / f);
                if (const uint64_t sel = SotcStats::sel_draws.exchange(0)) {
                    uint64_t miss[8];
                    for (int i = 0; i < 8; ++i) {
                        miss[i] = SotcStats::sel_fast_miss[i].exchange(0);
                    }
                    const double pct = 100.0 / sel;
                    std::fprintf(stderr,
                                 "SOTC sel/frame: selections %.0f, registers unchanged %.1f%%, "
                                 "reused %.1f%% (fallback %.1f%%); not reused: none %.1f%%, regs "
                                 "%.1f%%, params %.1f%%, code %.1f%%, bindings %.1f%%, flat %.1f%%, "
                                 "fetch %.1f%%, V# %.1f%%; verify mismatches %llu, unnoticed "
                                 "register writes %llu\n",
                                 sel / f, SotcStats::sel_regs_same.exchange(0) * pct,
                                 SotcStats::sel_fast_hit.exchange(0) * pct,
                                 SotcStats::sel_fast_fallback.exchange(0) * pct, miss[0] * pct,
                                 miss[1] * pct, miss[2] * pct, miss[3] * pct, miss[4] * pct,
                                 miss[5] * pct, miss[6] * pct, miss[7] * pct,
                                 static_cast<unsigned long long>(
                                     SotcStats::sel_fast_mismatch.exchange(0)),
                                 static_cast<unsigned long long>(
                                     SotcStats::sel_regs_unnoticed.exchange(0)));
                }
                if (const uint64_t dma = SotcStats::dma_syncs.exchange(0)) {
                    std::fprintf(stderr,
                                 "SOTC dma/frame: syncs %.1f, skipped %.1f, bad %llu; hot pages "
                                 "%llu, promoted %.1f, demoted %.1f\n",
                                 dma / f, SotcStats::dma_sync_skips.exchange(0) / f,
                                 static_cast<unsigned long long>(
                                     SotcStats::dma_sync_bad.exchange(0)),
                                 static_cast<unsigned long long>(SotcStats::dma_hot_pages.load()),
                                 SotcStats::dma_hot_promotions.exchange(0) / f,
                                 SotcStats::dma_hot_demotions.exchange(0) / f);
                }
                if (const uint64_t timed = SotcStats::gpu_timed.exchange(0)) {
                    std::fprintf(stderr,
                                 "SOTC gpu/frame: busy %.2f ms, idle between submits %.2f ms, "
                                 "%.1f command buffers\n",
                                 SotcStats::gpu_busy_ns.exchange(0) / 1e6 / f,
                                 SotcStats::gpu_gap_ns.exchange(0) / 1e6 / f, timed / f);
                }
                const uint64_t eager_recorded = SotcStats::eager_recorded.exchange(0);
                const double eager_hits_n = double(SotcStats::eager_hits.exchange(0));
                if (eager_recorded) {
                    std::fprintf(stderr,
                                 "SOTC eager/frame: recorded %.2f, hits %.2f, dirty %.2f, "
                                 "changed %.2f, unused %.2f, verify-bad %llu; wait %.0f ms/s; "
                                 "ready %.2f, age %.0f us\n",
                                 eager_recorded / f, eager_hits_n / f,
                                 SotcStats::eager_dirty.exchange(0) / f,
                                 SotcStats::eager_changed.exchange(0) / f,
                                 SotcStats::eager_unused.exchange(0) / f,
                                 static_cast<unsigned long long>(
                                     SotcStats::eager_verify_bad.exchange(0)),
                                 SotcStats::eager_wait_ns.exchange(0) / 1e6 / seconds,
                                 SotcStats::eager_ready.exchange(0) / f,
                                 eager_hits_n ? SotcStats::eager_age_us.exchange(0) / eager_hits_n
                                              : 0.0);
                }
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
    RunPresenter([] {
        const auto empty_frame = presenter->PrepareBlankFrame(true);
        presenter->Present(empty_frame, false, false);
    }, true);
}

void VideoOutDriver::DrawLastFrame() {
    RunPresenter([] {
        const auto frame = presenter->PrepareLastFrame();
        if (frame != nullptr) presenter->Present(frame, true);
    }, true);
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
    request_cv.notify_one();
}

void VideoOutDriver::PresentThread(std::stop_token token) {
    const std::chrono::nanoseconds vblank_period(1000000000 /
                                                 EmulatorSettings.GetVblankFrequency());

    Common::SetCurrentThreadName("shadPS4:PresentThread");
    Common::SetCurrentThreadRealtime(vblank_period);

    Common::AccurateTimer timer{vblank_period};
    const u32 frame_limit = EmulatorSettings.GetFrameLimit();
    const auto frame_period = frame_limit ? std::chrono::nanoseconds(1000000000 / frame_limit)
                                          : std::chrono::nanoseconds(0);
    const auto slack = frame_period / 16;
    auto next_flip = std::chrono::steady_clock::now();
    const auto next_slot = [&](std::chrono::steady_clock::time_point slot) {
        return std::max(slot + frame_period,
                        std::chrono::steady_clock::now() + frame_period - slack);
    };
    const bool immediate_flips = frame_limit != 0;
    std::printf("VideoOut: vblank %u Hz, frame limit %u FPS, separate present %d\n",
                EmulatorSettings.GetVblankFrequency(), frame_limit, swap_queue != nullptr);

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
        const auto tick_deadline = std::chrono::steady_clock::now() + vblank_period;

        if (DebugState.IsGuestThreadsPaused()) {
            DrawLastFrame();
            timer.End();
            continue;
        }

        // Check if it's time to take a request.
        // sotc: the flip rate counts from the last flip instead of fixed vblank phases: at 30 fps
        // (flip rate 1) a frame ready just after an even vblank went out two vblanks later
        // (67 ms) instead of at the next one (50 ms). On by default after a 16 min session on a
        // Radeon RX 6650 XT ("muito melhor"; heavy areas hold a steady 20 fps instead of
        // stuttering); SOTC_FLIP_LATE=0 restores the fixed phases.
        static const bool flip_late = [] {
            const char* v = std::getenv("SOTC_FLIP_LATE");
            return !v || v[0] != '0';
        }();
        static u64 last_flip_count = 0;
        auto& vblank_status = main_port.vblank_status;
        const bool flip_slot = !frame_limit || std::chrono::steady_clock::now() >= next_flip;
        const bool flip_due =
            flip_late ? vblank_status.count - last_flip_count >= u64(main_port.flip_rate) + 1
                      : vblank_status.count % (main_port.flip_rate + 1) == 0;
        if (flip_slot && flip_due) {
            const auto request = receive_request();
            if (request && frame_limit) next_flip = next_slot(next_flip);
            if (request) {
                last_flip_count = vblank_status.count;
            }
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

        if (!immediate_flips || main_port.flip_rate != 0) {
            timer.End();
            continue;
        }
        while (!token.stop_requested()) {
            {
                std::unique_lock lock{mutex};
                if (!request_cv.wait_until(lock, tick_deadline, [&] { return !requests.empty(); }))
                    break;
            }
            if (std::chrono::steady_clock::now() < next_flip) {
                if (next_flip >= tick_deadline) break;
                Common::PreciseSleepUntil(next_flip);
            }
            const auto request = receive_request();
            if (request) {
                next_flip = next_slot(next_flip);
                Flip(request);
                FRAME_END;
            }
        }
        Common::PreciseSleepUntil(tick_deadline);
    }
}

} // namespace Libraries::VideoOut
