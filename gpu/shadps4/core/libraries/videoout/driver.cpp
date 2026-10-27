// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>
#include <cstring>
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
#include "video_core/renderer_vulkan/vk_framegen.h"
#include "bbport_overlay.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/sotc_fps_cap.h"
#include "video_core/sotc_event.h"
#include "video_core/sotc_stats.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// sotc: K32GetProcessMemoryInfo (the RAM line below) is exported by kernel32, no psapi.lib; the
// SDK declares it for PSAPI 2 only.
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <psapi.h>
#endif
#include "video_core/sotc_ui_lang.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

// sotc: src/runtime_memory.c. Bytes of the pagefile-backed guest section (BB_DMEM_MB); it is
// shared, so it counts towards the system commit but not towards this process's private commit.
extern "C" uint64_t runtime_memory_guest_section_bytes(void);
extern "C" size_t runtime_memory_ram_census(char* out, size_t cap);
extern "C" void runtime_memory_region_stats(uint64_t* slow, uint64_t* slow_draw,
                                                  uint64_t* snap_builds, uint64_t* generation,
                                                  uint64_t* verified, uint64_t* mismatch,
                                                  uint64_t* threads, uint64_t* bytes);

extern "C" void runtime_memory_protect_stats(uint64_t out[4]);

namespace Libraries::VideoOut {

// sotc: printf into the SOTC_FPS report, which is written to stderr in one go.
[[gnu::format(printf, 2, 3)]] static void ReportPrintf(std::string& out, const char* format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    const int size = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    if (size > 0) {
        const size_t at = out.size();
        out.resize(at + size + 1);
        std::vsnprintf(out.data() + at, size + 1, format, args);
        out.resize(at + size);
    }
    va_end(args);
}

// sotc: see SubmitFlipInternal. SOTC_EARLY_VO_LABEL=0 restores the upstream behaviour.
static bool EarlyVoLabel() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_EARLY_VO_LABEL");
        return !v || std::strcmp(v, "0") != 0;
    }();
    return on;
}

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

VideoOutDriver::~VideoOutDriver() {
    // sotc: the vblank thread queues present jobs; stop it before the presenter thread.
    present_thread = {};
    present_job_thread = {};
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
    main_port.submit_prev_index = -1;

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

// sotc: SOTC_ASYNC_PRESENT=1. Measured with DLSS-G: Streamline's present returns in 0.1 ms
// (its pacer presents on a thread of its own), so the vblank thread keeps presenting by default.
// FSR 3 frame generation presents the game frame half a frame after the generated one: from
// the first FSR 3 frame on, the presenter has a thread of its own (vblank thread only).
bool VideoOutDriver::AsyncPresent() {
    static const bool async = [] {
        const char* v = std::getenv("SOTC_ASYNC_PRESENT");
        return v && v[0] == '1';
    }();
    static bool fsr = false;
    if (!fsr && Vulkan::FrameGen::Current() == Vulkan::FrameGen::Mode::Fsr) {
        fsr = true;
        std::fprintf(stderr, "SOTC FRAMEGEN: FSR 3, presenting on the presenter thread\n");
    }
    return async || fsr;
}

void VideoOutDriver::RunPresentJob(std::function<void()> job, bool droppable) {
    if (!AsyncPresent()) {
        job();
        return;
    }
    {
        std::scoped_lock lock{present_job_mutex};
        if (droppable && !present_jobs.empty()) {
            return; // a blank or repeated frame while the presenter is still busy
        }
        if (!present_job_thread.joinable()) {
            present_job_thread =
                std::jthread([this](std::stop_token token) { PresentJobThread(token); });
        }
        present_jobs.push(std::move(job));
    }
    present_job_cv.notify_one();
}

void VideoOutDriver::PresentJobThread(std::stop_token token) {
    Common::SetCurrentThreadName("shadPS4:Presenter");
    while (!token.stop_requested()) {
        std::function<void()> job;
        {
            std::unique_lock lock{present_job_mutex};
            if (!present_job_cv.wait(lock, token, [this] { return !present_jobs.empty(); })) {
                return;
            }
            job = std::move(present_jobs.front());
            present_jobs.pop();
        }
        job();
    }
}

void VideoOutDriver::Flip(const Request& req) {
    // Update HDR status before presenting, and present the frame.
    RunPresentJob([frame = req.frame, hdr = req.port->is_hdr] {
        presenter->SetHDR(hdr);
        presenter->Present(frame);
    });
    SotcStats::flip_count.fetch_add(1, std::memory_order_relaxed);
    {
        // sotc: a performance line in the event log every minute (average FPS, frames slower
        // than 50 ms, the slowest, video memory in use).
        using Clock = std::chrono::steady_clock;
        static Clock::time_point minute_start = Clock::now(), last_flip = minute_start;
        static u32 minute_flips = 0, slow = 0;
        static double worst_ms = 0.0;
        const Clock::time_point now = Clock::now();
        const double gap_ms = std::chrono::duration<double, std::milli>(now - last_flip).count();
        last_flip = now;
        ++minute_flips;
        worst_ms = std::max(worst_ms, gap_ms);
        slow += gap_ms > 50.0;
        const double seconds = std::chrono::duration<double>(now - minute_start).count();
        if (seconds >= 60.0) {
            SotcEvent::Log("PERF",
                           SotcUi::T("Último minuto: %.1f FPS médio, %u quadros acima de 50 ms, pior %.0f ms, memória de vídeo %llu MB",
                                     "Last minute: %.1f FPS average, %u frames over 50 ms, worst %.0f ms, video memory %llu MB"),
                           minute_flips / seconds, slow, worst_ms,
                           static_cast<unsigned long long>(SotcStats::vram_used.load(std::memory_order_relaxed) >> 20));
            const u64 skipped_cpu = SotcStats::rb_hot_cpu_skipped.exchange(0, std::memory_order_relaxed);
            const u64 skipped_late = SotcStats::rb_hot_late_skipped.exchange(0, std::memory_order_relaxed);
            if (skipped_cpu || skipped_late) {
                std::fprintf(stderr, "SOTC RBHOT skipped: %llu KB at gather, %llu KB at apply\n",
                             static_cast<unsigned long long>(skipped_cpu >> 10),
                             static_cast<unsigned long long>(skipped_late >> 10));
            }
            if (const u64 skipped = skipped_cpu + skipped_late) {
                SotcEvent::Log("MEMORY",
                               SotcUi::T("Leituras da GPU não aplicadas sobre memória que o jogo reescreveu: %llu KB",
                                         "GPU readbacks not applied over memory the game rewrote: %llu KB"),
                               static_cast<unsigned long long>(skipped >> 10));
            }
            minute_start = now;
            minute_flips = 0;
            slow = 0;
            worst_ms = 0.0;
        }
    }

    // sotc: SOTC_STALL_LOG[=<ms>]: for each frame longer than the threshold, the frame's biggest
    // events (buffer uploads, allocations, texture uploads, fence waits, readbacks; sotc_stats.h
    // StallNote) and the busy counters of the threads. Read before the SOTC_FPS printer below
    // resets the cause counters.
    if (SotcStats::kStallLog) {
        static auto last_flip = std::chrono::steady_clock::now();
        static u64 flip_number = 0;
        struct Seen {
            u64 commands, idle, parse, recorder, lock_draw, lock_all, readbacks, readback_ns;
        };
        static Seen seen{};
        const auto now = std::chrono::steady_clock::now();
        const double interval_ms = std::chrono::duration<double, std::milli>(now - last_flip).count();
        last_flip = now;
        ++flip_number;
        const Seen cur{SotcStats::commands.load(),
                       SotcStats::draw_idle_ns.load(),
                       SotcStats::cp_parse_ns.load(),
                       SotcStats::recorder_busy_ns.load(),
                       SotcStats::lock_slow_ns_draw.load(),
                       SotcStats::lock_slow_ns.load(),
                       SotcStats::readbacks.load(),
                       SotcStats::readback_ns.load()};
        const auto delta = [](u64 now_value, u64 before) {
            return now_value >= before ? now_value - before : now_value; // reset by a printer
        };
        std::vector<SotcStats::StallAggRow> agg;
        auto events = SotcStats::StallTake(&agg);
        if (interval_ms >= SotcStats::kStallLogMs) {
            std::sort(events.begin(), events.end(),
                      [](const auto& a, const auto& b) { return a.ms > b.ms; });
            double listed = 0.0;
            for (const auto& event : events) {
                listed += event.ms;
            }
            std::string causes;
            for (int c = 0; c < SotcStats::FrameCauses; ++c) {
                const double ms = SotcStats::frame_ns[c].load(std::memory_order_relaxed) / 1e6;
                if (ms >= 0.5) {
                    char item[40];
                    std::snprintf(item, sizeof(item), " %s %.1f", SotcStats::kFrameCauseNames[c], ms);
                    causes += item;
                }
            }
            static bool anchor_shown = false;
            if (!std::exchange(anchor_shown, true)) {
                std::fprintf(stderr, "SOTC STALL anchor %p (call sites are offsets from it)\n",
                             reinterpret_cast<const void*>(&SotcStats::StallAnchor));
            }
            std::string text;
            char head[512];
            std::snprintf(
                head, sizeof(head),
                "SOTC STALL flip %llu: %.1f ms | cmds %llu, draw-thread idle %.1f ms, parse %.1f ms, "
                "recorder %.1f ms, guest-lock waits %.1f ms (draw %.1f), readbacks %llu (%.1f ms), "
                "copy %.1f MB | causes:%s | %zu events, %.1f ms listed\n",
                static_cast<unsigned long long>(flip_number), interval_ms,
                static_cast<unsigned long long>(delta(cur.commands, seen.commands)),
                delta(cur.idle, seen.idle) / 1e6, delta(cur.parse, seen.parse) / 1e6,
                delta(cur.recorder, seen.recorder) / 1e6, delta(cur.lock_all, seen.lock_all) / 1e6,
                delta(cur.lock_draw, seen.lock_draw) / 1e6,
                static_cast<unsigned long long>(delta(cur.readbacks, seen.readbacks)),
                delta(cur.readback_ns, seen.readback_ns) / 1e6,
                SotcStats::frame_copy_bytes.load(std::memory_order_relaxed) / 1048576.0,
                causes.c_str(), events.size(), listed);
            text = head;
            // Totals per kind, then the biggest (kind, program or image) rows.
            for (int k = 0; k < SotcStats::StallKinds; ++k) {
                SotcStats::StallAggRow total{k, 0, 0, 0.0, 0.0, 0.0, 0.0};
                for (const auto& row : agg) {
                    if (row.kind == k) {
                        total.calls += row.calls;
                        total.ms += row.ms;
                        total.mb += row.mb;
                        total.a += row.a;
                        total.b += row.b;
                    }
                }
                if (total.calls) {
                    char line[256];
                    std::snprintf(line, sizeof(line),
                                  "    total %s: %llu calls, %.1f ms (phase a %.1f, b %.1f), %.1f MB\n",
                                  SotcStats::kStallKindNames[k],
                                  static_cast<unsigned long long>(total.calls), total.ms, total.a,
                                  total.b, total.mb);
                    text += line;
                }
            }
            std::sort(agg.begin(), agg.end(),
                      [](const auto& a, const auto& b) { return a.ms > b.ms; });
            for (size_t i = 0; i < agg.size() && i < 8; ++i) {
                char line[256];
                std::snprintf(line, sizeof(line),
                              "    by key %s %#llx: %llu calls, %.1f ms (a %.1f, b %.1f), %.2f MB\n",
                              SotcStats::kStallKindNames[agg[i].kind], agg[i].key,
                              static_cast<unsigned long long>(agg[i].calls), agg[i].ms, agg[i].a,
                              agg[i].b, agg[i].mb);
                text += line;
            }
            for (size_t i = 0; i < events.size() && i < 14; ++i) {
                text += "    ";
                text += events[i].text;
                text += '\n';
            }
            std::fputs(text.c_str(), stderr);
            std::fflush(stderr);
        }
        seen = cur;
    }

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
            std::string report; // sotc: written at once off this thread (see ReportPrintf)
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
                const uint64_t compiles = SotcStats::frame_compiles.exchange(0);
                // sotc: frames later than the window's usual pace (median + 10%), whatever the
                // vblank rate: a missed 30 FPS frame shows at 50 ms at 60 Hz, 41.7 ms at 120 Hz.
                const double late_ms = pct(0.5) * 1.1;
                const auto late = std::count_if(frame_ms.begin(), frame_ms.end(),
                                                [&](float ms) { return ms > late_ms; });
                ReportPrintf(report,
                             "SOTC FRAMES p50 %.1f ms, p99 %.1f ms (1%% low %.1f FPS), worst %.1f ms "
                             "[%s]; >50 ms: %u [%s]; compiled %llu; late %lld of %zu\n",
                             pct(0.5), pct(0.99), 1000.0 / pct(0.99), worst_ms,
                             columns(worst_ns).c_str(), slow_frames, columns(slow_ns).c_str(),
                             static_cast<unsigned long long>(compiles),
                             static_cast<long long>(late), frame_ms.size());
                // sotc: the overlay's copy of this window (bbport_overlay.cpp).
                SotcStats::ov_compiles_window.store(static_cast<uint32_t>(compiles));
                SotcStats::ov_compiles_total.fetch_add(compiles);
                int cause = -1;
                for (int c = 0; c < SotcStats::FrameCauses; ++c) {
                    if (worst_ns[c] > 2000000 && (cause < 0 || worst_ns[c] > worst_ns[cause])) {
                        cause = c;
                    }
                }
                SotcStats::ov_worst_cause.store(cause);
                SotcStats::ov_worst_cause_ms.store(cause < 0 ? 0.0f : float(worst_ns[cause] / 1e6));
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
                    ReportPrintf(report, "SOTC ALLOC %s\n", allocs.c_str());
                }
                if (const uint64_t translate = SotcStats::compile_phase_ns[0].exchange(0)) {
                    const auto take = [](int i) {
                        return SotcStats::compile_phase_ns[i].exchange(0) / 1e6;
                    };
                    const double emit = take(1), patch = take(2), module = take(3), reg = take(4);
                    ReportPrintf(report,
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
            const double cmds_per_frame =
                flips ? double(SotcStats::TakeDelta(SotcStats::commands, commands_seen)) / flips : 0.0;
            SotcStats::ov_cmds_per_frame.store(float(cmds_per_frame));
            SotcStats::ov_readbacks_per_s.store(float(readbacks / seconds));
            SotcStats::ov_readback_ms_per_s.store(float(readback_ns / 1e6 / seconds));
            ReportPrintf(report,
                         "SOTC FPS %.1f at %.0fs; %.0f cmds/frame; readbacks %.0f/s (ahead %llu, finish %llu, heap %llu, early %llu), %.0f ms/s; "
                         "changed copies %llu/%llu, bytes %llu/%llu; vram %llu/%llu MB, "
                         "arenas %llu (%llu MB)\n",
                         flips / seconds, std::chrono::duration<double>(now - run_start).count(),
                         cmds_per_frame, readbacks / seconds,
                         static_cast<unsigned long long>(SotcStats::readback_ahead.exchange(0)),
                         static_cast<unsigned long long>(SotcStats::readback_finish.exchange(0)),
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
            // sotc: zero is printed too, so every report accounts for the guard's interventions.
            ReportPrintf(report, "SOTC BPE guard: clipped writes %llu, bytes %llu\n",
                         static_cast<unsigned long long>(
                             SotcStats::heap_guard_clipped_writes.exchange(0)),
                         static_cast<unsigned long long>(
                             SotcStats::heap_guard_clipped_bytes.exchange(0)));
            ReportPrintf(report,
                         "SOTC BPE guard cost: batches %llu, heaps checked %llu, unproven %llu, "
                         "tree nodes %llu; select %.1f, build %.1f, write %.1f ms/s\n",
                         static_cast<unsigned long long>(SotcStats::heap_guard_batches.exchange(0)),
                         static_cast<unsigned long long>(SotcStats::heap_guard_checked.exchange(0)),
                         static_cast<unsigned long long>(SotcStats::heap_guard_fallbacks.exchange(0)),
                         static_cast<unsigned long long>(SotcStats::heap_guard_nodes.exchange(0)),
                         SotcStats::heap_guard_ns_select.exchange(0) / 1e6 / seconds,
                         SotcStats::heap_guard_ns_build.exchange(0) / 1e6 / seconds,
                         SotcStats::heap_guard_ns_write.exchange(0) / 1e6 / seconds);
            ReportPrintf(report, "SOTC readback finish: %llu for bytes the guest asked\n",
                         static_cast<unsigned long long>(
                             SotcStats::readback_finish_asked.exchange(0)));
            // sotc: Lote 3. Diff-first of the BPE guard (copies already equal to guest memory are
            // dropped), candidate 1 telemetry (dirty asynchronous readbacks are downloaded twice).
            // Zero is printed too.
            {
                const auto u = [](std::atomic<uint64_t>& v) {
                    return static_cast<unsigned long long>(v.exchange(0));
                };
                const unsigned long long diff_copies = u(SotcStats::bpe_diff_copies);
                const unsigned long long diff_skipped = u(SotcStats::bpe_diff_skipped);
                const unsigned long long diff_bytes = u(SotcStats::bpe_diff_bytes);
                const unsigned long long diff_skipped_bytes = u(SotcStats::bpe_diff_skipped_bytes);
                ReportPrintf(report,
                             "SOTC BPE diff: batches %llu (all copies equal %llu), copies %llu, dropped %llu "
                             "(%.1f%%), kept for overlap %llu, KiB %llu/%llu dropped; compare %.2f ms/s\n",
                             u(SotcStats::bpe_diff_batches), u(SotcStats::bpe_diff_empty_batches),
                             diff_copies, diff_skipped,
                             diff_copies ? 100.0 * diff_skipped / diff_copies : 0.0,
                             u(SotcStats::bpe_diff_overlapped), diff_skipped_bytes / 1024,
                             diff_bytes / 1024, SotcStats::bpe_diff_ns.exchange(0) / 1e6 / seconds);
                const unsigned long long fin = u(SotcStats::rbf_finishes);
                const unsigned long long dirty = u(SotcStats::rbf_dirty);
                const unsigned long long clean_bytes = u(SotcStats::rbf_clean_bytes);
                const unsigned long long dirty_copy = u(SotcStats::rbf_dirty_copy_bytes);
                const unsigned long long dirty_hit = u(SotcStats::rbf_dirty_hit_bytes);
                ReportPrintf(report,
                             "SOTC RB dirty: async finishes %llu, dirty %llu (%.1f%%), guest range hit %llu, "
                             "only part dirty %llu; copied KiB clean %llu, dirty %llu (hit %llu, %.1f%%), "
                             "windows %llu KiB; redownload %llu calls, %llu KiB, %.2f ms/s\n",
                             fin, dirty, fin ? 100.0 * dirty / fin : 0.0,
                             u(SotcStats::rbf_dirty_asked), u(SotcStats::rbf_dirty_partial),
                             clean_bytes / 1024, dirty_copy / 1024, dirty_hit / 1024,
                             dirty_copy ? 100.0 * dirty_hit / dirty_copy : 0.0,
                             u(SotcStats::rbf_dirty_window_bytes) / 1024,
                             u(SotcStats::rbf_redownload_calls),
                             u(SotcStats::rbf_redownload_bytes) / 1024,
                             SotcStats::rbf_redownload_ns.exchange(0) / 1e6 / seconds);
                // sotc: READBACK-PLAN step 0.
                const unsigned long long samples = u(SotcStats::rbs_inflight_samples);
                const unsigned long long isum = u(SotcStats::rbs_inflight_sum);
                const unsigned long long begin_bytes = u(SotcStats::rbs_begin_bytes);
                const unsigned long long overlap_bytes = u(SotcStats::rbs_overlap_bytes);
                ReportPrintf(report,
                             "SOTC RB cause: first GpuWrite %llu Applied %llu Drop %llu CpuReplace %llu; "
                             "dirty with GpuWrite %llu (asked pages %llu), without %llu; stale KiB in copies "
                             "GpuWrite %llu Applied %llu Drop %llu CpuReplace %llu; in flight avg %.2f max %llu, "
                             "%.1f%% of begin bytes also in another in-flight readback\n",
                             u(SotcStats::rbs_first[0]), u(SotcStats::rbs_first[1]),
                             u(SotcStats::rbs_first[2]), u(SotcStats::rbs_first[3]),
                             u(SotcStats::rbs_any_gpuwrite), u(SotcStats::rbs_gpuwrite_asked),
                             u(SotcStats::rbs_no_gpuwrite), u(SotcStats::rbs_cause_bytes[0]) / 1024,
                             u(SotcStats::rbs_cause_bytes[1]) / 1024,
                             u(SotcStats::rbs_cause_bytes[2]) / 1024,
                             u(SotcStats::rbs_cause_bytes[3]) / 1024,
                             samples ? double(isum) / samples : 0.0, u(SotcStats::rbs_inflight_max),
                             begin_bytes ? 100.0 * overlap_bytes / begin_bytes : 0.0);
                // sotc: READBACK-PLAN fine path.
                const unsigned long long fine_finishes = u(SotcStats::rbs_fine_finishes);
                const unsigned long long r1 = u(SotcStats::rbs_rounds[0]);
                const unsigned long long r2 = u(SotcStats::rbs_rounds[1]);
                const unsigned long long r3 = u(SotcStats::rbs_rounds[2]);
                const unsigned long long r4 = u(SotcStats::rbs_rounds[3]);
                const unsigned long long fine_bytes = u(SotcStats::rbs_fine_applied_bytes);
                const double fine_ms = SotcStats::rbs_fine_ns.exchange(0) / 1e6 / seconds;
                if (fine_finishes || r1 || r2 || r3 || r4) {
                    ReportPrintf(report,
                                 "SOTC RB fine: finishes %llu (applied %llu KiB, %.2f ms/s draw thread); "
                                 "guest reads finished in round 1 %llu, 2 %llu, 3 %llu, synchronous "
                                 "fallback %llu\n",
                                 fine_finishes, fine_bytes / 1024, fine_ms, r1, r2, r3, r4);
                }
                if (SotcStats::rbv_calls.load() != 0) {
                    ReportPrintf(report,
                                 "SOTC RB verify: %llu readbacks, applied %llu B (MISMATCH_APPLIED %llu), "
                                 "skipped %llu B (MISMATCH_SKIPPED %llu; differing bytes explained: in "
                                 "GPU set %llu, guest memory equals arena %llu, page CPU-modified %llu), "
                                 "GpuWrite stale %llu B (%llu changed), MISMATCH_SET %llu, "
                                 "MISMATCH_PAGE %llu\n",
                                 static_cast<unsigned long long>(SotcStats::rbv_calls.load()),
                                 static_cast<unsigned long long>(SotcStats::rbv_applied.load()),
                                 static_cast<unsigned long long>(SotcStats::rbv_applied_bad.load()),
                                 static_cast<unsigned long long>(SotcStats::rbv_skipped.load()),
                                 static_cast<unsigned long long>(SotcStats::rbv_skipped_bad.load()),
                                 static_cast<unsigned long long>(SotcStats::rbv_skipped_in_set.load()),
                                 static_cast<unsigned long long>(SotcStats::rbv_skipped_backing.load()),
                                 static_cast<unsigned long long>(SotcStats::rbv_skipped_cpu.load()),
                                 static_cast<unsigned long long>(SotcStats::rbv_gpuwrite.load()),
                                 static_cast<unsigned long long>(SotcStats::rbv_gpuwrite_changed.load()),
                                 static_cast<unsigned long long>(SotcStats::rbv_set_bad.load()),
                                 static_cast<unsigned long long>(SotcStats::rbv_page_bad.load()));
                }
                const unsigned long long cmp_calls = u(SotcStats::rbs_cmp_calls);
                if (cmp_calls) {
                    ReportPrintf(report,
                                 "SOTC RB stale cmp (%llu redownloads): bytes also in snapshot, changed: "
                                 "not stale %llu/%llu, stale non-GpuWrite %llu/%llu, stale GpuWrite %llu/%llu; "
                                 "fresh-only bytes %llu\n",
                                 cmp_calls, u(SotcStats::rbs_cmp_changed[0]),
                                 u(SotcStats::rbs_cmp_bytes[0]), u(SotcStats::rbs_cmp_changed[1]),
                                 u(SotcStats::rbs_cmp_bytes[1]), u(SotcStats::rbs_cmp_changed[2]),
                                 u(SotcStats::rbs_cmp_bytes[2]), u(SotcStats::rbs_cmp_fresh_only));
                }
            }
            static uint64_t last_images_new = 0;
            const uint64_t images_new = SotcStats::images_new.load();
            // sotc: one exchange feeds both the GC line and the VRAM line below (same window);
            // reading it twice left the VRAM "GC freed" always at 0.
            const uint64_t gc_freed = SotcStats::gc_freed.exchange(0);
            ReportPrintf(report,
                         "SOTC GC: %.1f ticks/frame, freed %.0f/s (%llu written back), images new "
                         "%.0f/s, cached %llu (%llu MB)\n",
                         flips ? double(SotcStats::gc_ticks.exchange(0)) / flips : 0.0,
                         gc_freed / seconds,
                         static_cast<unsigned long long>(SotcStats::gc_downloads.exchange(0)),
                         (images_new - last_images_new) / seconds,
                         static_cast<unsigned long long>(SotcStats::cached_images.load()),
                         static_cast<unsigned long long>(SotcStats::cached_image_bytes.load() >> 20));
            last_images_new = images_new;
            // sotc: warnings dropped by SotcStats::LogThrottled in the last window (M5/P5), so the
            // line rate that reaches the launcher pipe is visible.
            const uint64_t log_lines = SotcStats::log_lines.exchange(0);
            const uint64_t log_suppressed = SotcStats::log_suppressed.exchange(0);
            if (log_lines || log_suppressed) {
                ReportPrintf(report, "SOTC log: %.0f lines/s, %llu suppressed\n",
                             log_lines / seconds,
                             static_cast<unsigned long long>(log_suppressed));
            }
            // sotc: SOTC_RB_NARROW: GPU-written pages that the wide readback window would have
            // downloaded and unprotected (and the GPU protected again), left protected instead.
            if (const uint64_t kib = SotcStats::rb_narrow_kib.load(std::memory_order_relaxed)) {
                ReportPrintf(report,
                             "SOTC RB narrow %llu KiB windows: %.0f KiB/s of GPU-written pages "
                             "left protected, %llu hot readbacks\n",
                             static_cast<unsigned long long>(kib),
                             SotcStats::rb_narrow_kept_bytes.exchange(0) / 1024.0 / seconds,
                             static_cast<unsigned long long>(
                                 SotcStats::rb_hot_entries.load(std::memory_order_relaxed)));
            }
            if (SotcStats::kFastBind) {
                const uint64_t eligible = SotcStats::fast_bind_eligible.exchange(0);
                const uint64_t hits = SotcStats::fast_bind_hits.exchange(0);
                std::fprintf(stderr, "SOTC FASTBIND hits %llu/%llu (%.1f%%)\n",
                             static_cast<unsigned long long>(hits),
                             static_cast<unsigned long long>(eligible),
                             100.0 * double(hits) / double(std::max<u64>(eligible, 1)));
            }
            if (const uint64_t sweeps = SotcStats::dma_sweeps.exchange(0)) {
                const uint64_t ranges = SotcStats::dma_sweep_ranges.exchange(0);
                const uint64_t uploading = SotcStats::dma_sweep_uploading.exchange(0);
                const uint64_t runs = SotcStats::dma_sweep_runs.exchange(0);
                const uint64_t sweep_ns = SotcStats::dma_sweep_ns.exchange(0);
                const uint64_t idle_ns = SotcStats::dma_sweep_idle_ns.exchange(0);
                ReportPrintf(report,
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
                ReportPrintf(report,
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
                ReportPrintf(
                    report,
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
                ReportPrintf(report,
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
                ReportPrintf(report,
                             "SOTC page tracking desync repairs %llu (repeated faults left alone "
                             "%llu)\n",
                             static_cast<unsigned long long>(repairs),
                             static_cast<unsigned long long>(legit));
            }
            // sotc: process memory (M4). The working set is what the process has resident, the
            // private commit (PagefileUsage) what it has committed, and the faults are cumulative.
            // sotc: PagefileUsage leaves out the guest section (~BB_DMEM_MB, pagefile-backed and
            // shared), which shows up in the system commit instead, so both are printed (task 6).
            // "system commit 0 / 0" means GetPerformanceInfo failed.
#ifdef _WIN32
            {
                PROCESS_MEMORY_COUNTERS pmc{};
                if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
                    PERFORMANCE_INFORMATION pi{};
                    const bool have_commit = GetPerformanceInfo(&pi, sizeof(pi)) != 0;
                    const uint64_t commit_total =
                        have_commit ? (pi.CommitTotal * pi.PageSize) >> 20 : 0;
                    const uint64_t commit_limit =
                        have_commit ? (pi.CommitLimit * pi.PageSize) >> 20 : 0;
                    ReportPrintf(report,
                                 "SOTC RAM: working set %llu MB (peak %llu MB), private commit "
                                 "%llu MB (guest section excluded), guest section %llu MB, system "
                                 "commit %llu / %llu MB, page faults %llu\n",
                                 static_cast<unsigned long long>(pmc.WorkingSetSize >> 20),
                                 static_cast<unsigned long long>(pmc.PeakWorkingSetSize >> 20),
                                 static_cast<unsigned long long>(pmc.PagefileUsage >> 20),
                                 static_cast<unsigned long long>(
                                     runtime_memory_guest_section_bytes() >> 20),
                                 static_cast<unsigned long long>(commit_total),
                                 static_cast<unsigned long long>(commit_limit),
                                 static_cast<unsigned long long>(pmc.PageFaultCount));
                }
                // sotc: SOTC_RAM_CENSUS=1: where the RAM goes (src/runtime_memory.c). Computed on
                // a background thread there; this only copies the last finished text (empty when
                // disabled, and for the first interval).
                char census[4096];
                if (runtime_memory_ram_census(census, sizeof(census)) > 0) {
                    report += census;
                }
            }
#endif
            // sotc: stalls of the guest RAM lock, slow path only (Task 3 in
            // docs/sotc/LOTE-1-CORRECCIONES.md); src/runtime_memory.c counts them. Silently
            // skipped when the interval had no slow wait. The times are the total waited in the
            // window, so they read as ms per second; "draw" is the draw/CP thread only.
            {
                const uint64_t lock_waits = SotcStats::lock_slow_waits.exchange(0);
                const uint64_t lock_ns = SotcStats::lock_slow_ns.exchange(0);
                const uint64_t lock_waits_draw = SotcStats::lock_slow_waits_draw.exchange(0);
                const uint64_t lock_ns_draw = SotcStats::lock_slow_ns_draw.exchange(0);
                if (lock_waits || lock_waits_draw) {
                    ReportPrintf(report,
                                 "SOTC memory lock: %.0f slow waits/s, %.1f ms/s (draw: %.0f/s, "
                                 "%.1f ms/s)\n",
                                 lock_waits / seconds, lock_ns / 1e6 / seconds,
                                 lock_waits_draw / seconds, lock_ns_draw / 1e6 / seconds);
                }
            }
            // sotc: host protect calls of the GPU side (src/runtime_memory.c), QPC ticks of 100 ns.
            {
                uint64_t ps[4];
                runtime_memory_protect_stats(ps);
                if (ps[0]) {
                    ReportPrintf(report,
                                 "SOTC host protect: %.0f calls/s, %.1f ms/s (draw thread: %.0f/s, "
                                 "%.1f ms/s)\n",
                                 ps[0] / seconds, ps[1] / 1e4 / seconds, ps[2] / seconds,
                                 ps[3] / 1e4 / seconds);
                }
            }
            // sotc: guest region lookups that took the table lock (src/runtime_memory.c, C2 in
            // docs/sotc/PERFIL-2.md), per-thread snapshot rebuilds and VMA table generation steps
            // (two per map/unmap/protect of the guest), per second of the interval.
            {
                static uint64_t prev_slow = 0, prev_draw = 0, prev_builds = 0, prev_gen = 0;
                uint64_t slow = 0, draw = 0, builds = 0, gen = 0, verified = 0, mismatch = 0,
                         threads = 0, bytes = 0;
                runtime_memory_region_stats(&slow, &draw, &builds, &gen, &verified, &mismatch,
                                            &threads, &bytes);
                ReportPrintf(report,
                             "SOTC region cache: %.0f slow lookups/s (draw thread %.0f), %.1f "
                             "snapshot builds/s, %.1f table generations/s; %llu threads hold a VMA copy "
                             "(%.1f KiB)\n",
                             (slow - prev_slow) / seconds, (draw - prev_draw) / seconds,
                             (builds - prev_builds) / seconds,
                             (gen - prev_gen) / seconds, static_cast<unsigned long long>(threads),
                             bytes / 1024.0);
                if (verified) {
                    ReportPrintf(report, "SOTC region verify: %llu answers checked, %llu mismatches\n",
                                 static_cast<unsigned long long>(verified),
                                 static_cast<unsigned long long>(mismatch));
                }
                prev_slow = slow;
                prev_draw = draw;
                prev_builds = builds;
                prev_gen = gen;
            }
            // sotc: video memory against the budget the texture cache collects for; emergency
            // collections mean an allocation already failed (6 GB cards in large areas).
            if (const uint64_t budget = SotcStats::vram_budget.load(std::memory_order_relaxed)) {
                ReportPrintf(report,
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
                             static_cast<unsigned long long>(gc_freed),
                             static_cast<unsigned long long>(SotcStats::gc_kept_tiled.exchange(0)),
                             static_cast<unsigned long long>(
                                 SotcStats::vram_emergency.exchange(0)));
            }
            // sotc (MEMORIA-PLAN.md): arena memory by bind class, images with several backings,
            // idle-age histogram of cached images, cold sweep and image-source release totals.
            if (SotcStats::vram_budget.load(std::memory_order_relaxed) != 0) {
                const auto mib = [](const std::atomic<uint64_t>& v) {
                    return static_cast<unsigned long long>(v.load(std::memory_order_relaxed) >> 20);
                };
                const auto num = [](const std::atomic<uint64_t>& v) {
                    return static_cast<unsigned long long>(v.load(std::memory_order_relaxed));
                };
                const uint64_t used = SotcStats::arena_used.load(std::memory_order_relaxed);
                const uint64_t classified = SotcStats::arena_img_only.load() +
                                            SotcStats::arena_other_only.load() +
                                            SotcStats::arena_mixed.load();
                ReportPrintf(
                    report,
                    "SOTC MEM arena used %llu MiB (image-source only %llu, of which idle 30 s %llu; "
                    "other only %llu of which idle 30 s %llu; both %llu; unclassified %llu), "
                    "image-source released %llu MiB "
                    "in %llu runs; multi-backing images %llu (%llu MiB in non-current backings)\n",
                    static_cast<unsigned long long>(used >> 20), mib(SotcStats::arena_img_only),
                    mib(SotcStats::arena_img_idle), mib(SotcStats::arena_other_only),
                    mib(SotcStats::arena_other_idle), mib(SotcStats::arena_mixed),
                    static_cast<unsigned long long>((used > classified ? used - classified : 0) >> 20),
                    mib(SotcStats::arena_img_released), num(SotcStats::arena_img_runs),
                    num(SotcStats::img_multi_count), mib(SotcStats::img_multi_bytes));
                char cpu[256], gpu[256];
                int cpu_len = 0, gpu_len = 0;
                for (int i = 0; i < SotcStats::kIdleBuckets; ++i) {
                    cpu_len += std::snprintf(cpu + cpu_len, sizeof(cpu) - cpu_len, "%s%llu/%llu",
                                             i ? " " : "", num(SotcStats::img_idle_cpu_n[i]),
                                             mib(SotcStats::img_idle_cpu_b[i]));
                    gpu_len += std::snprintf(gpu + gpu_len, sizeof(gpu) - gpu_len, "%s%llu/%llu",
                                             i ? " " : "", num(SotcStats::img_idle_gpu_n[i]),
                                             mib(SotcStats::img_idle_gpu_b[i]));
                }
                ReportPrintf(
                    report,
                    "SOTC MEM images idle <10s <30s <60s <120s <300s >=300s (count/MiB): "
                    "cpu-origin %s | gpu-written or target %s; cold sweep freed %llu (%llu MiB), "
                    "recreated %llu (%llu within 10 s, %llu MiB), %.2f ms in %llu passes\n",
                    cpu, gpu, num(SotcStats::sweep_freed), mib(SotcStats::sweep_bytes),
                    num(SotcStats::sweep_recreated), num(SotcStats::sweep_recreated_fast),
                    mib(SotcStats::sweep_recreated_bytes),
                    SotcStats::sweep_ns.load(std::memory_order_relaxed) / 1e6,
                    num(SotcStats::sweep_passes));
            }
            // sotc: write-fault readbacks whose pages were still GPU-modified (totals).
            if (const uint64_t pending = SotcStats::write_fault_gpu_pending.load(),
                stale = SotcStats::write_fault_stale_gpu.load(),
                stale_reads = SotcStats::read_fault_stale_gpu.load();
                pending || stale || stale_reads) {
                std::fprintf(stderr,
                             "SOTC write faults over GPU data: left protected %llu, stale page "
                             "bits cleared %llu; read faults that cleared one %llu\n",
                             static_cast<unsigned long long>(pending),
                             static_cast<unsigned long long>(stale),
                             static_cast<unsigned long long>(stale_reads));
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
            if (const uint64_t waits = SotcStats::cp_waits.exchange(0)) {
                ReportPrintf(report, "SOTC CP waits for the draw thread %.0f/s\n", waits / seconds);
            }
            if (const uint64_t hits = SotcStats::pipe_key_memo_hits.exchange(0)) {
                const uint64_t misses = SotcStats::pipe_key_memo_misses.exchange(0);
                ReportPrintf(report,
                             "SOTC pipeline key memo: %.0f%% of %.0f draws (%.0f%% of the draws "
                             "only with the fine rule)\n",
                             100.0 * double(hits) / double(hits + misses), double(hits + misses),
                             100.0 * double(SotcStats::pipe_key_memo_fine_hits.exchange(0)) /
                                 double(hits + misses));
            } else {
                SotcStats::pipe_key_memo_misses.exchange(0);
            }
            if (const uint64_t hits = SotcStats::bind_memo_hits.exchange(0)) {
                const uint64_t misses = SotcStats::bind_memo_misses.exchange(0);
                ReportPrintf(report, "SOTC draw bind memo: %.0f%% of %.0f stage bindings\n",
                             100.0 * double(hits) / double(hits + misses), double(hits + misses));
            } else {
                SotcStats::bind_memo_misses.exchange(0);
            }
            if (const uint64_t hits = SotcStats::buffer_memo_hits.exchange(0)) {
                const uint64_t misses = SotcStats::buffer_memo_misses.exchange(0);
                ReportPrintf(report, "SOTC buffer bind memo: %.0f%% of %.0f read-only bindings\n",
                             100.0 * double(hits) / double(hits + misses), double(hits + misses));
            } else {
                SotcStats::buffer_memo_misses.exchange(0);
            }
            if (const uint64_t hits = SotcStats::rt_memo_hits.exchange(0)) {
                const uint64_t misses = SotcStats::rt_memo_misses.exchange(0);
                ReportPrintf(report, "SOTC draw target memo: %.0f%% of %.0f draws\n",
                             100.0 * double(hits) / double(hits + misses), double(hits + misses));
            } else {
                SotcStats::rt_memo_misses.exchange(0);
            }
            // sotc: Phase 1 counters (per report): why the texture bind memo missed, and how many
            // read-only formatted buffer bindings find an image behind them.
            {
                const uint64_t tmk = SotcStats::tbm_miss_key.exchange(0),
                               tme = SotcStats::tbm_miss_epoch.exchange(0),
                               tmg = SotcStats::tbm_miss_gen.exchange(0),
                               tmr = SotcStats::tbm_miss_replay.exchange(0),
                               tnp = SotcStats::tbm_not_plain.exchange(0);
                if (tmk + tme + tmg + tmr + tnp) {
                    ReportPrintf(report,
                                 "SOTC tex memo misses: key %llu, epoch %llu, generation %llu, "
                                 "replay %llu, uncovered stages %llu\n",
                                 (unsigned long long)tmk, (unsigned long long)tme,
                                 (unsigned long long)tmg, (unsigned long long)tmr,
                                 (unsigned long long)tnp);
                }
                const uint64_t fc = SotcStats::fmt_sync_calls.exchange(0),
                               fm = SotcStats::fmt_sync_meta.exchange(0),
                               fi = SotcStats::fmt_sync_image.exchange(0);
                if (fc) {
                    ReportPrintf(report,
                                 "SOTC formatted buffers: %llu, on metadata %llu, with an image %llu "
                                 "(%.1f%%)\n",
                                 (unsigned long long)fc, (unsigned long long)fm,
                                 (unsigned long long)fi, 100.0 * double(fi) / double(fc));
                }
            }
            if (const uint64_t same = SotcStats::pipe_same_key_hits.exchange(0)) {
                const uint64_t rest = SotcStats::pipe_same_key_misses.exchange(0);
                ReportPrintf(report, "SOTC pipeline same key: %.0f%% of %.0f draws\n",
                             100.0 * double(same) / double(same + rest), double(same + rest));
            } else {
                SotcStats::pipe_same_key_misses.exchange(0);
            }
            if (const uint64_t draws = SotcStats::pipe_key_draws.exchange(0)) {
                ReportPrintf(report, "SOTC pipeline key build: %.2f us of %.0f draws\n",
                             SotcStats::pipe_key_ns.exchange(0) / 1000.0 / double(draws),
                             double(draws));
            } else {
                SotcStats::pipe_key_ns.exchange(0);
            }
            // sotc: the two counters are only accumulated with SOTC_FPS=1 (liverpool.cpp skips
            // the clock reads otherwise), so do not print a line that would read 0.00 ms.
            if (SotcStats::kFpsLog && flips) {
                const double idle_ms = SotcStats::draw_idle_ns.exchange(0) / 1e6;
                const double parse_ms = SotcStats::cp_parse_ns.exchange(0) / 1e6;
                // sotc: recorder thread busy time (vk_recorder.cpp ConsumerLoop: time replaying
                // commands, not waiting for them); 0 when the scheduler has no recorder thread.
                const double recorder_ms = SotcStats::recorder_busy_ns.exchange(0) / 1e6;
                std::fprintf(stderr,
                             "SOTC threads per frame: draw busy %.2f ms, command parse %.2f ms, "
                             "recorder busy %.2f ms\n",
                             (seconds * 1000.0 - idle_ms) / flips, parse_ms / flips,
                             recorder_ms / flips);
                // sotc: how much of the command parse time is the parser itself.
                const double ring_ms = SotcStats::cp_ring_wait_ns.exchange(0) / 1e6;
                const double drain_ms = SotcStats::cp_drain_wait_ns.exchange(0) / 1e6;
                const double label_ms = SotcStats::cp_label_wait_ns.exchange(0) / 1e6;
                std::fprintf(stderr,
                             "SOTC CP own work %.2f ms per frame (parse minus waits: ring %.2f, "
                             "drain %.2f, label %.2f)\n",
                             (parse_ms - ring_ms - drain_ms - label_ms) / flips, ring_ms / flips,
                             drain_ms / flips, label_ms / flips);
                {
                    // sotc: CPU time against wall time: a draw thread whose CPU time is well below
                    // its busy time was preempted or stalled by the scheduler.
                    static double prev_draw = 0, prev_cp = 0, prev_proc = 0;
                    const double draw_cpu = SotcStats::ThreadCpuMs(0),
                                 cp_cpu = SotcStats::ThreadCpuMs(1),
                                 proc_cpu = SotcStats::ProcessCpuMs();
                    std::fprintf(stderr,
                                 "SOTC CPU time per frame: draw thread %.2f ms, command processor "
                                 "%.2f ms, process %.1f ms (%.1f cores)\n",
                                 (draw_cpu - prev_draw) / flips, (cp_cpu - prev_cp) / flips,
                                 (proc_cpu - prev_proc) / flips,
                                 (proc_cpu - prev_proc) / (seconds * 1000.0));
                    prev_draw = draw_cpu;
                    prev_cp = cp_cpu;
                    prev_proc = proc_cpu;
                }
                std::fprintf(stderr, "SOTC draw thread guest commands %.2f ms per frame\n",
                             SotcStats::draw_cmds_ns.exchange(0) / 1e6 / flips);
            }
            Vulkan::DrawPhase::ReplayReport(); // sotc: SOTC_DRAW_REPLAY_PROBE (no-op otherwise)
            // sotc: SOTC_HASH_STATS=1, top shader hashes of the draw thread (liverpool.cpp).
            if (SotcStats::kHashStats && liverpool) {
                liverpool->HashStatsReport(report, seconds);
            }
            if (const uint64_t faults =
                    SotcStats::srt_walker_faults.load(std::memory_order_relaxed)) {
                std::fprintf(stderr, "SOTC SRT walker loads that read zero: %llu\n",
                             static_cast<unsigned long long>(faults));
            }
            // sotc: compute pipelines built by workers (SOTC_ASYNC_COMPUTE), totals.
            if (const uint64_t built =
                    SotcStats::async_compute_built.load(std::memory_order_relaxed)) {
                std::fprintf(
                    stderr,
                    "SOTC ASYNC COMPUTE built %llu (%.0f ms on workers), recorder waited %llu "
                    "times (%.0f ms), %llu made at prefetch install\n",
                    static_cast<unsigned long long>(built),
                    SotcStats::async_compute_build_ns.load(std::memory_order_relaxed) / 1e6,
                    static_cast<unsigned long long>(
                        SotcStats::async_compute_waits.load(std::memory_order_relaxed)),
                    SotcStats::async_compute_wait_ns.load(std::memory_order_relaxed) / 1e6,
                    static_cast<unsigned long long>(
                        SotcStats::compute_speculated.load(std::memory_order_relaxed)));
            }
            // sotc: graphics shaders translated by workers (SOTC_ASYNC_TRANSLATE), totals.
            if (const uint64_t built =
                    SotcStats::async_translate_built.load(std::memory_order_relaxed)) {
                std::fprintf(stderr,
                             "SOTC ASYNC TRANSLATE %llu stages (%.0f ms on workers), %llu draws "
                             "skipped, %llu stale; compute built by the recorder %llu\n",
                             static_cast<unsigned long long>(built),
                             SotcStats::async_translate_ns.load(std::memory_order_relaxed) / 1e6,
                             static_cast<unsigned long long>(
                                 SotcStats::async_translate_skips.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(
                                 SotcStats::async_translate_stale.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(SotcStats::async_compute_self_built.load(
                                 std::memory_order_relaxed)));
            }
            const bool cmd_stats = SotcStats::kCmdStats; // sotc: as the counters (sotc_stats.h)
            if (cmd_stats && flips) {
                double k[6];
                for (int i = 0; i < 6; ++i) {
                    k[i] = double(SotcStats::cmd_kinds[i].exchange(0)) / flips;
                }
                ReportPrintf(report,
                             "SOTC cmds/frame: draw %.0f, indirect %.0f gpu-args + %.0f cpu-args "
                             "(%.0f empty), dispatch %.0f, dispatch-indirect %.0f\n",
                             k[0], k[1], k[2], k[3], k[4], k[5]);
                ReportPrintf(report,
                             "SOTC faults/s: guest writes %.0f, walker loads %.0f, CP reads %.0f\n",
                             SotcStats::fault_writes.exchange(0) / seconds,
                             SotcStats::fault_walker.exchange(0) / seconds,
                             SotcStats::fault_gpu_reads.exchange(0) / seconds);
                ReportPrintf(report,
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
            // The flip thread does not wait for the log: one stderr write took 7-10 ms (bench log
            // file, Radeon RX 6650 XT session), ~15 of them a frame of 80-100 ms every 5 s.
            std::thread([text = std::move(report)] {
                std::fwrite(text.data(), 1, text.size(), stderr);
            }).detach();
            window_start = now;
        }
    } else if (SotcStats::kHashStats && liverpool) {
        // sotc: SOTC_HASH_STATS=1 without SOTC_FPS=1: only the hash table, in its own 5 s window.
        static auto hash_window_start = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - hash_window_start >= std::chrono::seconds(5)) {
            const double seconds = std::chrono::duration<double>(now - hash_window_start).count();
            std::string report;
            liverpool->HashStatsReport(report, seconds);
            hash_window_start = now;
            std::thread([text = std::move(report)] {
                std::fwrite(text.data(), 1, text.size(), stderr);
            }).detach();
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
    if (!EarlyVoLabel() && port->prev_index != -1) {
        port->buffer_labels[port->prev_index] = 0;
        port->SignalVoLabel();
    }
    // save to prev buf index
    port->prev_index = req.index;
}

void VideoOutDriver::DrawBlankFrame() {
    RunPresentJob(
        [] {
            const auto empty_frame = presenter->PrepareBlankFrame(true);
            presenter->Present(empty_frame, false, false);
        },
        true);
}

void VideoOutDriver::DrawLastFrame() {
    RunPresentJob(
        [] {
            const auto frame = presenter->PrepareLastFrame();
            if (frame != nullptr) {
                presenter->Present(frame, true);
            }
        },
        true);
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
        liverpool->SendOrderedCommand(
            [=, this]() { SubmitFlipInternal(port, index, flip_arg, is_eop); });
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

    // sotc: the buffer shown before this flip is free once the copy of this one is recorded:
    // PrepareFrame read it on the GPU timeline, ahead of anything the game renders into the old
    // buffer next. Upstream frees it only when the present thread takes this flip on a vblank
    // tick, and the command processor waits for that in the VO label wait at the start of the
    // next frame.
    if (EarlyVoLabel()) {
        if (port->submit_prev_index != -1) {
            port->buffer_labels[port->submit_prev_index] = 0;
            port->SignalVoLabel();
        }
        port->submit_prev_index = index;
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

    // sotc: the flip rate is the minimum interval between two flips, counted from the last flip,
    // as on the console: a 30 FPS frame that misses its vblank is shown one vblank later (50 ms).
    // Taking requests only on vblanks where count % (rate + 1) == 0 made it wait for the next
    // even vblank instead: 66.7 ms, the game dropping from 30 to 15 FPS at every late frame. The
    // interval is in 60 Hz vblanks, scaled to BB_VBLANK_HZ, so a faster vblank only shortens the
    // wait of a late frame. Rate 0 (60 FPS, the fps60 mode) still flips at every vblank.
    // SOTC_FLIP_PACING=0 restores the parity rule.
    static const bool flip_pacing = [] {
        const char* v = std::getenv("SOTC_FLIP_PACING");
        return !v || v[0] != '0';
    }();
    const u64 vblanks_per_60hz = std::max<u64>(1, EmulatorSettings.GetVblankFrequency() / 60);
    // sotc: SOTC_FPS_LIMIT=<fps> (the launcher's 60 FPS mode) limits the uncapped game to at least
    // vblank_hz / fps vblanks between flips; a late frame still goes out at the next vblank (at
    // 120 Hz a 20 ms frame shows at 25 ms, not at 33 ms as with 60 Hz vblanks).
    // The in-game menu changes it while playing (SotcFpsCap::limit_fps: 60 or unlimited).
    const auto vblanks_for = [](long fps) {
        return fps > 0 ? std::max<u64>(1, EmulatorSettings.GetVblankFrequency() / u64(fps)) : u64{0};
    };
    const u64 start_limit_vblanks = [&] {
        const char* v = std::getenv("SOTC_FPS_LIMIT");
        return vblanks_for(v ? std::strtol(v, nullptr, 10) : 0);
    }();
    u64 last_flip_vblank = 0;
    bool flipped = false;

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
        // sotc: the game's rate, or none when uncapped (F7, sotc_fps_cap.h).
        const int flip_rate = SotcFpsCap::EffectiveRate();
        bool take_request;
        if (!flip_pacing) {
            take_request = vblank_status.count % (flip_rate + 1) == 0;
        } else if (!flipped) {
            take_request = true;
        } else if (flip_rate <= 0) {
            const int live_limit = SotcFpsCap::limit_fps.load(std::memory_order_relaxed);
            const u64 limit_vblanks = live_limit >= 0 ? vblanks_for(live_limit) : start_limit_vblanks;
            // sotc: with 480 Hz vblanks (the launcher's 60 FPS mode) a game the in-game menu sets to
            // unlimited must not pass 120 FPS (its movement timing breaks above that): never fewer
            // than vblank_hz / 120 vblanks between flips. Unchanged at BB_VBLANK_HZ <= 120.
            static const bool explicit_fast_vblank = [] {
                const char* v = std::getenv("BB_VBLANK_HZ");
                return v && std::atoi(v) > 120; // BB_VBLANK_HZ=0 (uncapped presets) stays unlimited
            }();
            const u64 floor_vblanks =
                explicit_fast_vblank ? std::max<u64>(1, EmulatorSettings.GetVblankFrequency() / 120) : 1;
            take_request = vblank_status.count - last_flip_vblank >= std::max(limit_vblanks, floor_vblanks);
        } else {
            const u64 interval = u64(flip_rate + 1) * vblanks_per_60hz;
            take_request = vblank_status.count - last_flip_vblank >= interval;
        }
        if (take_request) {
            const auto request = receive_request();
            if (request) {
                last_flip_count = vblank_status.count;
            }
            if (!request) {
                if (timer.GetTotalWait().count() < 0) { // Dont draw too fast
                    if (!main_port.is_open) {
                        DrawBlankFrame();
                    } else if (ImGui::Core::MustKeepDrawing() || BbOverlay::LoadingShown()) {
                        // sotc: also the port's loading screen while the game shows nothing.
                        DrawLastFrame();
                    }
                }
            } else {
                Flip(request);
                last_flip_vblank = vblank_status.count;
                flipped = true;
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
