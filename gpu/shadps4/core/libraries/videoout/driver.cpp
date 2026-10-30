// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>
#include <cstring>
// sotc: SOTC_UNTRACK_LOG reads guest memory after a VirtualQuery check (UntrackRead).
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
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
#include "bbport_threads.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/sotc_fps_cap.h"
#include "video_core/sotc_adaptive.h"
#include "video_core/sotc_event.h"
#include "video_core/buffer_cache/hrr.h"
#include "video_core/buffer_cache/region_manager.h"
#include "video_core/sotc_stats.h"
#include "video_core/sotc_write_extent.h"
#include "video_core/sotc_ui_lang.h"
#include "../../../../sotc/hooks.h"      // sotc: SotcHookStats::Report
#include "../../../../sotc/write_decl.h" // sotc: SotcWriteDecl::Report
#include "../../../../sotc/probes.h"     // sotc: engine probe report lines
#include "../../../../sotc/mem_classes.h" // sotc: SOTC_CLASS_TRACK report (plan step 6)
#include "../../../../sotc/lab_commands.h" // sotc: SotC Lab frame-time window
#include "../../../../sotc/area_prewarm.h" // sotc: SOTC_AREA_PREWARM report (plan step 5d)
#include "../../../../sotc/probe_buildsync.h" // sotc: Wave D report lines
#include "../../../../sotc/probe_dt.h"
#include "../../../../sotc/probe_drawtag.h"
#include "../../../../sotc/particle_prev.h"
#include "../../../../sotc/decommit_budget.h"
#include "../../../../sotc/mip_budget.h"
#include "video_core/buffer_cache/buffer_cache.h"     // sotc: VideoCore::ReportReadbackRips (B6)
#include "video_core/texture_cache/texture_cache.h"   // sotc: ReportImageFrees / ReportUnmapPark (B4/C5)

// sotc: SOTC_TINY_ASYNC (vk_pipeline_cache.cpp).
namespace Vulkan {
void SotcTinyAsyncNoteDisplayBuffer(u64 address, u64 size);
void SotcTinyAsyncReport(std::string& out, double seconds);
void ReportPipeMemo(std::string& out, double seconds); // sotc: SOTC_PIPE_MEMO
} // namespace Vulkan
// sotc: src/runtime_kernel.c, SOTC USLEEP line (usleep histogram, SOTC_USLEEP_YIELD).
extern "C" void runtime_usleep_stats(char* out, size_t size, double seconds);
// sotc: src/runtime_mutex.c, SOTC_MUTEX_SPIN_US stats (Wave D C1).
extern "C" void runtime_mutex_spin_stats(char* out, size_t size, double seconds);
// sotc: Wave D B5/C6, declared in vk_presenter.h; forward-declared here in case the header lacks it.
namespace Vulkan {
void ReportFlipWait(std::string& out, double seconds);
void ReportPresent(std::string& out, double seconds);
} // namespace Vulkan

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

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

// sotc: SOTC_IMAGE_BASE=<hex host address of the eboot image> (the port does not pass it to the
// GPU library, so it is set by hand; the log prints it back). Read at the SOTC FPS report only.
static u64 UntrackImageBase() {
    // sotc: the loader reports the image to the GPU library (bbgpu_trace_image, kept by
    // SotcTrace for every session); SOTC_IMAGE_BASE=<hex> overrides it.
    static const u64 env_base = [] {
        const char* v = std::getenv("SOTC_IMAGE_BASE");
        return v ? static_cast<u64>(std::strtoull(v, nullptr, 16)) : 0ULL;
    }();
    return env_base ? env_base : SotcTrace::ImageBase();
}

// sotc: reads size bytes of guest memory at addr, or returns false without touching it. The
// address must be a 31-bit host address (the image and the game's heaps are below 2 GiB, the
// game stores 31-bit pointers), and the whole read must be committed, readable and inside one
// VirtualQuery region. A concurrent unmap could still race the copy; the engine does not unmap
// these heaps, and the log is a diagnostic only (SOTC_UNTRACK_LOG). Not Windows: always false.
// sotc: end of the user address space (x64 Windows). The engine's objects and heaps are not all
// below 2 GiB: the game keeps 31-bit pointers for some data, but its allocators' chunks and their
// list nodes live in the direct/flexible memory far above it.
constexpr u64 kUntrackUserEnd = 0x0000800000000000ULL;

static bool UntrackRead(u64 addr, void* out, size_t size) {
#ifdef _WIN32
    if (addr < 0x10000ULL || addr + size > kUntrackUserEnd || addr + size < addr) {
        return false;
    }
    const auto* ptr = reinterpret_cast<const void*>(static_cast<uintptr_t>(addr));
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(ptr, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (addr + size > region_end) {
        return false;
    }
    std::memcpy(out, ptr, size);
    return true;
#else
    (void)addr;
    (void)out;
    (void)size;
    return false;
#endif
}

// sotc: SOTC_UNTRACK_LOG. The chunk list of the RenderSingleFrameGPU parent allocator, as an eboot
// offset walk: id (u32) at 0x2ed1f54 picks a heap object from the table at 0x2dfaab0; its parent
// (+0x80) must equal the pointer at 0x2dfb2b8. The chunk list head is parent+0x48, with the
// sentinel at parent+0x38. Each node: base at +0x60, size at +0x58, next at +8 (measured on the
// game: +8 of the last node is the sentinel, +0 holds the node itself; the first nodes print both).
// One line is written only when the list changes, so the report stays short.
static void UntrackLogChunks(u64 image, std::string& report) {
    static std::string last;
    static bool printed_links = false;
    u32 id = 0;
    u64 heap = 0;
    u64 parent = 0;
    u64 expected = 0;
    if (!UntrackRead(image + 0x2ed1f54, &id, sizeof(id)) || id >= 4096 ||
        !UntrackRead(image + 0x2dfaab0 + 8ULL * id, &heap, sizeof(heap)) ||
        !UntrackRead(heap + 0x80, &parent, sizeof(parent)) ||
        !UntrackRead(image + 0x2dfb2b8, &expected, sizeof(expected))) {
        ReportPrintf(report, "SOTC untrack: heap table not readable (id %u, base %llx)\n", id,
                     static_cast<unsigned long long>(image));
        return;
    }
    if (parent != expected) {
        ReportPrintf(report, "SOTC untrack: parent %llx is not the expected %llx (id %u)\n",
                     static_cast<unsigned long long>(parent),
                     static_cast<unsigned long long>(expected), id);
        return;
    }
    const u64 head = parent + 0x38;
    u64 node = 0;
    if (!UntrackRead(parent + 0x48, &node, sizeof(node))) {
        ReportPrintf(report, "SOTC untrack: chunk list head not readable\n");
        return;
    }
    std::string chunks;
    int count = 0;
    while (node != head && count < 64) {
        u64 base = 0;
        u64 size = 0;
        if (!UntrackRead(node + 0x60, &base, sizeof(base)) ||
            !UntrackRead(node + 0x58, &size, sizeof(size))) {
            break;
        }
        u64 next0 = 0;
        u64 next8 = 0;
        UntrackRead(node, &next0, sizeof(next0));
        UntrackRead(node + 8, &next8, sizeof(next8));
        if (!printed_links && count < 4) {
            ReportPrintf(report, "SOTC untrack link probe: node %llx next+0 %llx next+8 %llx\n",
                         static_cast<unsigned long long>(node),
                         static_cast<unsigned long long>(next0),
                         static_cast<unsigned long long>(next8));
        }
        char item[48];
        std::snprintf(item, sizeof(item), " %llx:%llx", static_cast<unsigned long long>(base),
                      static_cast<unsigned long long>(size));
        chunks += item;
        node = next8; // sotc: the link is at +8 (measured: +8 of the last node is the sentinel)
        ++count;
    }
    if (count > 0) {
        printed_links = true;
    }
    char summary[96];
    std::snprintf(summary, sizeof(summary), "SOTC untrack chunks: %d nodes%s:", count,
                  node == head ? "" : " (walk cut)");
    const std::string line = std::string(summary) + chunks + "\n";
    if (line != last) {
        last = line;
        ReportPrintf(report, "%s", line.c_str());
    }
}

// sotc: SOTC_UNTRACK_AUTO. The chunk list of UntrackLogChunks as ranges. False when a read fails,
// the list does not end at the sentinel within 64 nodes, or a node is not a sane chunk (64 KiB
// aligned, in user space, at most one 32 MiB chunk). The link is at +8 (see UntrackLogChunks); a
// wrong link makes the walk fail, which publishes nothing.
static bool UntrackAutoOneWalk(u64 parent, std::vector<VideoCore::SotcUntrack::Range>& out) {
    const u64 head = parent + 0x38;
    out.clear();
    u64 node = 0;
    if (!UntrackRead(parent + 0x48, &node, sizeof(node))) {
        return false;
    }
    for (int n = 0; node != head && n < 64; ++n) {
        u64 base = 0;
        u64 size = 0;
        u64 next = 0;
        if (!UntrackRead(node + 0x60, &base, sizeof(base)) ||
            !UntrackRead(node + 0x58, &size, sizeof(size)) ||
            !UntrackRead(node + 8, &next, sizeof(next))) {
            return false; // sotc: the link is at +8 (measured; +0 holds the node itself)
        }
        // sotc: chunk bases are guest addresses (the game's direct memory lives far above 2 GiB,
        // e.g. 0x10xxxxxxxx): any user-space address, 64 KiB aligned, up to 32 MiB.
        if (base < 0x10000ULL || base >= kUntrackUserEnd || (base & 0xFFFFULL) != 0 || size == 0 ||
            size > (32ULL << 20) || base + size > kUntrackUserEnd) {
            return false;
        }
        out.push_back(VideoCore::SotcUntrack::Range{base, size});
        node = next;
    }
    return node == head;
}

// sotc: SOTC_UNTRACK_AUTO. The live chunks of the RenderSingleFrameGPU heap, read twice; false
// unless both walks succeed and agree (a list changing during the walk is dropped).
static bool UntrackAutoWalk(u64 image, std::vector<VideoCore::SotcUntrack::Range>& out) {
    u32 id = 0;
    u64 heap = 0;
    u64 parent = 0;
    u64 expected = 0;
    if (!UntrackRead(image + 0x2ed1f54, &id, sizeof(id)) || id >= 4096 ||
        !UntrackRead(image + 0x2dfaab0 + 8ULL * id, &heap, sizeof(heap)) ||
        !UntrackRead(heap + 0x80, &parent, sizeof(parent)) ||
        !UntrackRead(image + 0x2dfb2b8, &expected, sizeof(expected)) || parent != expected) {
        return false;
    }
    std::vector<VideoCore::SotcUntrack::Range> second;
    return UntrackAutoOneWalk(parent, out) && UntrackAutoOneWalk(parent, second) && out == second;
}

// sotc: SOTC_UNTRACK_AUTO. Every 30 flips the flip thread publishes the chunk list (only when
// SOTC_CP_AHEAD is unset, see SotcUntrack::AutoOn). It touches no region: the draw thread applies it.
static void UntrackAutoTick() {
    static u32 flips = 0;
    if (!VideoCore::SotcUntrack::AutoOn() || ++flips % 30 != 0) {
        return;
    }
    const u64 image = UntrackImageBase();
    std::vector<VideoCore::SotcUntrack::Range> live;
    if (image != 0 && UntrackAutoWalk(image, live)) {
        VideoCore::SotcUntrack::Publish(std::move(live));
    }
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
        liverpool->NotifyCpWait(); // sotc: SOTC_CP_IDLE_US (VO label waits)

        presenter->RegisterVideoOutSurface(group, address);
        // sotc: SOTC_TINY_ASYNC keeps the pipelines of tiny draws into display buffers inline.
        Vulkan::SotcTinyAsyncNoteDisplayBuffer(
            address, static_cast<u64>(attribute->pitch_in_pixel) * attribute->height * 8);
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
// sotc: src/runtime_memory.c, region lock sleeps since the last call.
extern "C" uint64_t runtime_memory_lock_sleeps(void);
// sotc: src/runtime_memory.c, time other guest threads were stopped by live view splits.
extern "C" uint64_t runtime_memory_carve_freeze_ns(void);
// sotc: src/runtime_memory.c, splits done without stopping the guest threads (SOTC_CARVE_NOSTOP).
extern "C" uint64_t runtime_memory_carve_nostop(void);
extern "C" void runtime_memory_lock_stats(uint64_t* writes, double* wait_ms, double* hold_ms,
                                          double* worst_ms);
extern "C" void runtime_memory_lock_stats2(double* worst_hold_ms, uint64_t* read_waits,
                                           double* read_wait_ms);
extern "C" void runtime_memory_carve_stats(uint64_t* mib, uint64_t* runs, double* split_ms,
                                           double* worst_ms, double* other_ms);
extern "C" void runtime_memory_lock_sites(char* readers, char* writers, size_t capacity);
extern "C" void runtime_memory_cache_stats(uint64_t* misses, uint64_t* stale, uint64_t* fast);
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
    UntrackAutoTick(); // sotc: SOTC_UNTRACK_AUTO (every 30 flips)
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
        SotcLab::OnFrame(gap_ms); // sotc: SotC Lab stats window (lab_commands.h)
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
        // sotc: SOTC_DRAW_SPLIT: this frame's share of the split counters (sotc_stats.h) and of the
        // CPU time of the draw, command processor and recorder threads, summed for the window, kept
        // for its worst frame and summed over its frames above 40 ms. The last column is the
        // deepest draw thread queue (a maximum, not a sum). All in TSC ticks until printed.
        constexpr int kSplitColumns = int{SotcStats::SplitParts} + int{SotcStats::SplitThreads} + 1;
        constexpr int kSplitQueue = kSplitColumns - 1;
        static uint64_t split_seen[kSplitColumns]{}, split_sum[kSplitColumns]{},
            split_worst[kSplitColumns]{}, split_slow[kSplitColumns]{};
        static double split_worst_ms = 0.0;
        static u32 split_frames = 0, split_slow_frames = 0;
        static uint64_t split_tick0 = 0, split_ns0 = 0;
        if (SotcStats::kDrawSplit) {
            uint64_t current[kSplitColumns]{};
            for (int c = 0; c < SotcStats::SplitParts; ++c) {
                current[c] = SotcStats::split_ticks[c].load(std::memory_order_relaxed);
            }
            for (int t = 0; t < SotcStats::SplitThreads; ++t) {
                current[SotcStats::SplitParts + t] =
                    SotcStats::SplitThreadCycles(static_cast<SotcStats::SplitThread>(t));
            }
            uint64_t frame[kSplitColumns]{};
            for (int c = 0; c < kSplitQueue; ++c) {
                // A thread registered since the last flip starts from its first reading.
                frame[c] = split_seen[c] || c < SotcStats::SplitParts ? current[c] - split_seen[c] : 0;
                split_seen[c] = current[c];
            }
            frame[kSplitQueue] = SotcStats::split_queue_max.exchange(0, std::memory_order_relaxed);
            if (split_tick0 == 0) { // first flip: only the starting point
                split_tick0 = SotcStats::Ticks();
                split_ns0 = SotcStats::NowNs();
            } else {
                const auto add = [&](uint64_t* to) {
                    for (int c = 0; c < kSplitQueue; ++c) {
                        to[c] += frame[c];
                    }
                    to[kSplitQueue] = std::max(to[kSplitQueue], frame[kSplitQueue]);
                };
                ++split_frames;
                add(split_sum);
                if (interval_ms > split_worst_ms) {
                    split_worst_ms = interval_ms;
                    std::copy(std::begin(frame), std::end(frame), std::begin(split_worst));
                }
                if (interval_ms > 40.0) {
                    ++split_slow_frames;
                    add(split_slow);
                }
            }
        }
        if (now - window_start >= std::chrono::seconds(5)) {
            const double seconds = std::chrono::duration<double>(now - window_start).count();
            std::string report; // sotc: written at once off this thread (see ReportPrintf)
            // sotc: SOTC_AB_PR (7c.0): label the window that just ended, then flip the phase for the
            // next one. Printed first, so every line of this report belongs to that window.
            if (SotcStats::kAbPr) {
                const uint32_t w = SotcStats::ab_window.load(std::memory_order_relaxed);
                ReportPrintf(report, "SOTC AB PR window %u: %s (%llu prepared draws ignored)\n", w,
                             (w & 1) ? "off" : "on",
                             static_cast<unsigned long long>(SotcStats::ab_ignored.exchange(0)));
                SotcStats::ab_window.store(w + 1, std::memory_order_relaxed);
            }
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
            ReportPrintf(report, "SOTC readback finish: %llu for bytes the guest asked\n",
                         static_cast<unsigned long long>(
                             SotcStats::readback_finish_asked.exchange(0)));
            if (SotcStats::kShadowCullHle) // sotc: SOTC_HLE_SHADOW_CULL (sotc_shadow_cull.cpp)
                ReportPrintf(report, "SOTC HLE shadow cull: %llu dispatches, %llu sections, %llu bits kept conservatively, %.2f ms/s; GPU fallbacks: layout %llu, input GPU-written %llu, bounds %llu, write refused %llu, SOTC_RB_PREV %llu\n",
                             static_cast<unsigned long long>(SotcStats::shadow_cull_dispatches.exchange(0)), static_cast<unsigned long long>(SotcStats::shadow_cull_sections.exchange(0)), static_cast<unsigned long long>(SotcStats::shadow_cull_kept.exchange(0)), SotcStats::shadow_cull_ns.exchange(0) / 1e6 / seconds,
                             static_cast<unsigned long long>(SotcStats::shadow_cull_fallbacks[0].exchange(0)), static_cast<unsigned long long>(SotcStats::shadow_cull_fallbacks[1].exchange(0)), static_cast<unsigned long long>(SotcStats::shadow_cull_fallbacks[2].exchange(0)), static_cast<unsigned long long>(SotcStats::shadow_cull_fallbacks[3].exchange(0)), static_cast<unsigned long long>(SotcStats::shadow_cull_fallbacks[4].exchange(0)));
            if (SotcStats::kCullExpress) { // sotc: SOTC_CULL_EXPRESS (Liverpool::XpEvaluate)
                auto xp = [](std::atomic<uint64_t>& c) { return static_cast<unsigned long long>(c.exchange(0)); };
                const uint64_t ahead_n = SotcStats::xp_ahead.load();
                const uint64_t lat_a = SotcStats::xp_lat_ahead_ns.exchange(0);
                const uint64_t lat_f = SotcStats::xp_lat_fifo_ns.exchange(0);
                const uint64_t lat_fn = SotcStats::xp_lat_fifo_n.exchange(0);
                const uint64_t seg_n = SotcStats::xp_segments.load();
                ReportPrintf(report,
                             "SOTC cull express: segments %llu, ahead %llu (skipped %llu ops, express %llu ops; skipped labels %llu, flips %llu, marks %llu), label latency ahead %.2f ms avg / FIFO %.2f ms avg (%llu), est. saved %.2f ms, decide %.0f us avg %.0f us max, run %.2f ms; FIFO because: no backlog %llu, no prep %llu, segment long %llu, skip many %llu, budget %llu, skipped unknown func %llu, signals %llu, skipped not ready %llu, skipped unknown fp %llu, express unknown %llu, express not ready %llu, express unknown fp %llu, label same %llu, CONFLICT %llu (express write vs skipped read/HiZ-like %llu, skipped write vs express read %llu, both write %llu; by skipped draw %llu, dispatch %llu, func %llu)\n",
                             static_cast<unsigned long long>(seg_n), static_cast<unsigned long long>(ahead_n),
                             xp(SotcStats::xp_skipped_ops), xp(SotcStats::xp_express_ops), xp(SotcStats::xp_sk_labels),
                             xp(SotcStats::xp_sk_flips), xp(SotcStats::xp_sk_marks),
                             ahead_n ? lat_a / 1e6 / ahead_n : 0.0, lat_fn ? lat_f / 1e6 / lat_fn : 0.0,
                             static_cast<unsigned long long>(lat_fn), SotcStats::xp_saved_est_ns.exchange(0) / 1e6,
                             SotcStats::xp_decide_ns.exchange(0) / 1e3 / (seg_n ? seg_n : 1),
                             SotcStats::xp_decide_max_ns.exchange(0) / 1e3, SotcStats::xp_run_ns.exchange(0) / 1e6,
                             xp(SotcStats::xp_reasons[SotcStats::XpNoBacklog]), xp(SotcStats::xp_reasons[SotcStats::XpNoPrep]),
                             xp(SotcStats::xp_reasons[SotcStats::XpSegTooLong]), xp(SotcStats::xp_reasons[SotcStats::XpSkipTooMany]),
                             xp(SotcStats::xp_reasons[SotcStats::XpBudget]), xp(SotcStats::xp_reasons[SotcStats::XpSkipUnknownFunc]),
                             xp(SotcStats::xp_reasons[SotcStats::XpSkipSignals]), xp(SotcStats::xp_reasons[SotcStats::XpSkipNotReady]),
                             xp(SotcStats::xp_reasons[SotcStats::XpSkipUnknownFp]), xp(SotcStats::xp_reasons[SotcStats::XpExpressUnknown]),
                             xp(SotcStats::xp_reasons[SotcStats::XpExpressNotReady]), xp(SotcStats::xp_reasons[SotcStats::XpExpressUnknownFp]),
                             xp(SotcStats::xp_reasons[SotcStats::XpLabelSame]), xp(SotcStats::xp_reasons[SotcStats::XpConflict]),
                             xp(SotcStats::xp_conf_war), xp(SotcStats::xp_conf_raw), xp(SotcStats::xp_conf_waw),
                             xp(SotcStats::xp_conf_draw), xp(SotcStats::xp_conf_dispatch), xp(SotcStats::xp_conf_func));
                SotcStats::xp_segments.store(0);
                SotcStats::xp_ahead.store(0);
            }
            {
                // sotc: guest threads' wait in asynchronous readbacks (BufferCache::ReadMemory),
                // and the ones that could go / went ahead (SOTC_RB_ASYNC_AHEAD).
                const uint64_t n = SotcStats::rb_guest_count.exchange(0);
                const uint64_t ns = SotcStats::rb_guest_ns.exchange(0);
                const uint64_t ahead = SotcStats::rb_ahead_async.exchange(0);
                const uint64_t ahead_ns = SotcStats::rb_ahead_async_ns.exchange(0);
                ReportPrintf(report,
                             "SOTC RB guest wait: %llu readbacks, %.1f ms total, avg %.0f us, max "
                             "%.1f ms (submit %.1f ms, gpu %.1f ms)\n",
                             static_cast<unsigned long long>(n), ns / 1e6,
                             n ? ns / 1e3 / n : 0.0, SotcStats::rb_guest_max_ns.exchange(0) / 1e6,
                             SotcStats::rb_guest_submit_ns.exchange(0) / 1e6,
                             SotcStats::rb_guest_gpu_ns.exchange(0) / 1e6);
                const uint64_t normal = n > ahead ? n - ahead : 0;
                const uint64_t normal_ns = ns > ahead_ns ? ns - ahead_ns : 0;
                ReportPrintf(report,
                             "SOTC RB ahead-async: %llu eligible, %llu ahead (avg %.0f us), %llu "
                             "usual (avg %.0f us), %llu without a free slot\n",
                             static_cast<unsigned long long>(
                                 SotcStats::rb_ahead_eligible.exchange(0)),
                             static_cast<unsigned long long>(ahead),
                             ahead ? ahead_ns / 1e3 / ahead : 0.0,
                             static_cast<unsigned long long>(normal),
                             normal ? normal_ns / 1e3 / normal : 0.0,
                             static_cast<unsigned long long>(
                                 SotcStats::rb_ahead_async_busy.exchange(0)));
                {
                    // sotc: who wrote the bytes of the guest readbacks, and SOTC_RB_TICK's copies.
                    const uint64_t wn = SotcStats::rb_writer_count.exchange(0);
                    const auto wpct = [wn](uint64_t v) { return wn ? 100.0 * v / wn : 0.0; };
                    ReportPrintf(report,
                                 "SOTC RB writer: done %.0f%%, submitted %.0f%%, unsubmitted "
                                 "%.0f%%, avg age %.1f ticks\n",
                                 wpct(SotcStats::rb_writer_done.exchange(0)),
                                 wpct(SotcStats::rb_writer_sub.exchange(0)),
                                 wpct(SotcStats::rb_writer_unsub.exchange(0)),
                                 wn ? static_cast<double>(
                                          SotcStats::rb_writer_age_sum.exchange(0)) / wn
                                    : 0.0);
                    const uint64_t tx = SotcStats::rb_tick_xfer.exchange(0);
                    const uint64_t tw = SotcStats::rb_tick_waits.exchange(0);
                    const uint64_t twns = SotcStats::rb_tick_wait_ns.exchange(0);
                    const uint64_t tfb = SotcStats::rb_tick_fb.exchange(0);
                    const uint64_t tfb_noxfer = SotcStats::rb_tick_fb_noxfer.exchange(0);
                    const uint64_t tfb_unknown = SotcStats::rb_tick_fb_unknown.exchange(0);
                    const uint64_t tfb_binds = SotcStats::rb_tick_fb_binds.exchange(0);
                    if (tx != 0 || tfb != 0) {
                        ReportPrintf(report,
                                     "SOTC RB tick: xfer copies %llu (avg wait %.0f us), fallback "
                                     "%llu (no xfer %llu, unknown %llu, binds %llu)\n",
                                     static_cast<unsigned long long>(tx),
                                     tw ? twns / 1e3 / tw : 0.0,
                                     static_cast<unsigned long long>(tfb),
                                     static_cast<unsigned long long>(tfb_noxfer),
                                     static_cast<unsigned long long>(tfb_unknown),
                                     static_cast<unsigned long long>(tfb_binds));
                    }
                }
                // sotc: the pattern of the guest readback requests (sotc_stats.h rb_pat_*) and
                // SOTC_RB_GROW's grown windows (rb_grow_*).
                const uint64_t pat = SotcStats::rb_pat_count.exchange(0);
                const auto pct = [pat](uint64_t v) { return pat ? 100.0 * v / pat : 0.0; };
                const uint64_t adjacent = SotcStats::rb_pat_adjacent.exchange(0);
                const uint64_t adjacent_1ms = SotcStats::rb_pat_adjacent_1ms.exchange(0);
                const uint64_t inside = SotcStats::rb_pat_inside.exchange(0);
                const uint64_t repeat = SotcStats::rb_pat_repeat.exchange(0);
                const uint64_t waited = SotcStats::rb_pat_waited.exchange(0);
                const uint64_t asked = SotcStats::rb_pat_asked_bytes.exchange(0);
                const uint64_t window = SotcStats::rb_pat_window_bytes.exchange(0);
                const uint64_t downloaded = SotcStats::rb_pat_download_bytes.exchange(0);
                char top[256];
                SotcStats::RbPatternTake(top, sizeof(top));
                ReportPrintf(report,
                             "SOTC RB pattern: %llu readbacks (%llu waited); adjacent to the same "
                             "thread's previous window %.0f%% (within 1 ms %.0f%%), inside it "
                             "%.0f%%, same window as last frame %.0f%%; avg asked %.1f B, window "
                             "%.1f KiB, downloaded %.1f KiB; %s\n",
                             static_cast<unsigned long long>(pat),
                             static_cast<unsigned long long>(waited), pct(adjacent),
                             pct(adjacent_1ms), pct(inside), pct(repeat),
                             pat ? double(asked) / pat : 0.0,
                             pat ? window / 1024.0 / pat : 0.0,
                             pat ? downloaded / 1024.0 / pat : 0.0, top);
                {
                    // sotc: written storage bindings by shader (sotc_write_extent.h).
                    char writes[1536];
                    SotcWriteExtent::Take(writes, sizeof(writes));
                    if (writes[0]) {
                        ReportPrintf(report, "SOTC GPU writes by shader: %s\n", writes);
                    }
                }
                // sotc: SOTC_HRR (dry run, buffer_cache/hrr.h): ranges that would be promoted
                // and the readbacks that hit them. "avg rb" is what those readbacks cost the
                // guest now, the most a promotion could save per fault.
                if (const char* hrr = std::getenv("SOTC_HRR"); hrr && hrr[0] && hrr[0] != '0') {
                    const uint64_t hf = SotcStats::hrr_faults.exchange(0);
                    const uint64_t hrb = SotcStats::hrr_rb_count.exchange(0);
                    const uint64_t hns = SotcStats::hrr_rb_ns.exchange(0);
                    ReportPrintf(report,
                                 "SOTC HRR: ranges %llu (KiB %llu), faults %llu (no wait %llu, "
                                 "waited %llu, avg rb now %.0f us), demotions %llu, import "
                                 "failures %llu (%s)\n",
                                 static_cast<unsigned long long>(
                                     SotcStats::hrr_ranges.load(std::memory_order_relaxed)),
                                 static_cast<unsigned long long>(
                                     SotcStats::hrr_kib.load(std::memory_order_relaxed)),
                                 static_cast<unsigned long long>(hf),
                                 static_cast<unsigned long long>(
                                     SotcStats::hrr_nowait.exchange(0)),
                                 static_cast<unsigned long long>(
                                     SotcStats::hrr_waited.exchange(0)),
                                 hrb ? hns / 1e3 / hrb : 0.0,
                                 static_cast<unsigned long long>(
                                     SotcStats::hrr_demotions.exchange(0)),
                                 static_cast<unsigned long long>(
                                     SotcStats::hrr_import_fail.exchange(0)),
                                 VideoCore::HostResidentRanges::Real()
                                     ? "promotion on"
                                     : "dry run: nothing is promoted");
                    // sotc: SOTC_HRR=1 (buffer_cache/hrr.h): host-visible arena memory now, granules
                    // migrated to it, and readbacks served from it without a copy. Only with the
                    // real promotion (Real()), not for the dry run or a device without the memory.
                    if (VideoCore::HostResidentRanges::Real()) {
                        ReportPrintf(
                            report,
                            "SOTC HRR host: host MiB %.1f, migrated %llu (MiB %.1f, failed %llu), "
                            "direct readbacks %llu (KiB %llu)\n",
                            SotcStats::hrr_host_bytes.load(std::memory_order_relaxed) / 1048576.0,
                            static_cast<unsigned long long>(
                                SotcStats::hrr_migrated.load(std::memory_order_relaxed)),
                            SotcStats::hrr_migrated_bytes.load(std::memory_order_relaxed) /
                                1048576.0,
                            static_cast<unsigned long long>(
                                SotcStats::hrr_migrate_failed.load(std::memory_order_relaxed)),
                            static_cast<unsigned long long>(
                                SotcStats::hrr_direct_readbacks.load(std::memory_order_relaxed)),
                            static_cast<unsigned long long>(
                                SotcStats::hrr_direct_bytes.load(std::memory_order_relaxed) / 1024));
                    }
                }
                // sotc: SOTC_UNTRACK_LOG=1 (off by default): the RenderSingleFrameGPU chunk list
                // (UntrackLogChunks) and the pages SOTC_UNTRACK_RANGES kept. Only with SOTC_FPS.
                {
                    static const bool untrack_log = [] {
                        const char* v = std::getenv("SOTC_UNTRACK_LOG");
                        return v && v[0] == '1';
                    }();
                    if (untrack_log) {
                        static bool base_shown = false;
                        const u64 image = UntrackImageBase();
                        if (!image) {
                            if (!base_shown) {
                                ReportPrintf(report, "SOTC untrack: image base unknown (SOTC_IMAGE_BASE=<hex> sets it) for the chunk log\n");
                            }
                        } else {
                            if (!base_shown) {
                                ReportPrintf(report, "SOTC untrack: image base %llx\n",
                                             static_cast<unsigned long long>(image));
                            }
                            UntrackLogChunks(image, report);
                        }
                        base_shown = true;
                        if (!VideoCore::SotcUntrack::Ranges().empty()) {
                            ReportPrintf(report, "SOTC untrack: %llu pages kept (uploads that kept the CPU bit)\n",
                                         static_cast<unsigned long long>(VideoCore::SotcUntrack::PagesKept()));
                        }
                    }
                }
                // sotc: SOTC_UNTRACK_AUTO (off by default): the published chunk list, the pages it
                // kept per second (the SOTC_UNTRACK_LOG counter) and the self-check faults.
                if (VideoCore::SotcUntrack::AutoOn()) {
                    static u64 kept_seen = 0;
                    const u64 kept = VideoCore::SotcUntrack::PagesKept();
                    ReportPrintf(report,
                                 "SOTC untrack auto: %zu ranges, %.1f MiB untracked, %.0f pages "
                                 "kept/s, self-check faults %llu%s\n",
                                 VideoCore::SotcUntrack::AutoRanges(),
                                 VideoCore::SotcUntrack::AutoBytes() / 1048576.0,
                                 (kept - kept_seen) / seconds,
                                 static_cast<unsigned long long>(
                                     VideoCore::SotcUntrack::FaultsUntracked()),
                                 VideoCore::SotcUntrack::Tripped()
                                     ? " (tripped: untracked pages released)"
                                     : "");
                    kept_seen = kept;
                }
                // sotc: SOTC_RB_PREFETCH (BufferCache::PrefetchIssue). The wait saved is an
                // estimate: consumed prefetches times this period's average guest readback wait.
                {
                    const uint64_t issued = SotcStats::rb_pf_issued.exchange(0);
                    const uint64_t consumed = SotcStats::rb_pf_consumed.exchange(0);
                    const uint64_t pf_waited = SotcStats::rb_pf_waited.exchange(0);
                    const uint64_t expired = SotcStats::rb_pf_expired.exchange(0);
                    const uint64_t stale = SotcStats::rb_pf_stale.exchange(0);
                    const uint64_t capped = SotcStats::rb_pf_capped.exchange(0);
                    if (issued | consumed | pf_waited | expired | stale | capped) {
                        ReportPrintf(report,
                                     "SOTC RB prefetch: issued %llu, consumed %llu (guest wait "
                                     "saved ~%.1f ms), expired %llu, stale %llu, skipped by caps "
                                     "%llu, waited at the fault %llu\n",
                                     static_cast<unsigned long long>(issued),
                                     static_cast<unsigned long long>(consumed),
                                     n ? consumed * (double(ns) / n) / 1e6 : 0.0,
                                     static_cast<unsigned long long>(expired),
                                     static_cast<unsigned long long>(stale),
                                     static_cast<unsigned long long>(capped),
                                     static_cast<unsigned long long>(pf_waited));
                    }
                    // sotc: SOTC_RB_PREV
                    const uint64_t prev = SotcStats::rb_prev_served.exchange(0);
                    const uint64_t prev_bytes = SotcStats::rb_prev_bytes.exchange(0);
                    const uint64_t prev_np = SotcStats::rb_prev_not_producer.exchange(0);
                    const uint64_t prev_old = SotcStats::rb_prev_too_old.exchange(0);
                    const uint64_t prev_owed = SotcStats::rb_prev_owed.exchange(0);
                    const uint64_t prev_busy = SotcStats::rb_prev_busy.exchange(0);
                    if (prev | prev_np | prev_old | prev_busy) {
                        ReportPrintf(report,
                                     "SOTC RB previous: served %llu (%.1f MiB released, %llu newer "
                                     "versions delivered later) without a GPU wait, refused: not "
                                     "rewritten every frame %llu, too old %llu, readback in "
                                     "flight %llu\n",
                                     static_cast<unsigned long long>(prev), prev_bytes / 1048576.0,
                                     static_cast<unsigned long long>(prev_owed),
                                     static_cast<unsigned long long>(prev_np),
                                     static_cast<unsigned long long>(prev_old),
                                     static_cast<unsigned long long>(prev_busy));
                    }
                    // sotc: SOTC_RB_MATERIALIZE
                    const uint64_t mat = SotcStats::rb_mat_applied.exchange(0);
                    const uint64_t mat_bytes = SotcStats::rb_mat_bytes.exchange(0);
                    const uint64_t not_fresh = SotcStats::rb_mat_not_fresh.exchange(0);
                    if (mat | not_fresh) {
                        ReportPrintf(report,
                                     "SOTC RB materialize: applied %llu (%.1f MiB) without a "
                                     "fault, not fresh %llu\n",
                                     static_cast<unsigned long long>(mat), mat_bytes / 1048576.0,
                                     static_cast<unsigned long long>(not_fresh));
                    }
                    {
                        const uint64_t wf_n = SotcStats::rb_wf_narrow.exchange(0);
                        const uint64_t wf_b = SotcStats::rb_wf_saved.exchange(0);
                        if (wf_n) {
                            ReportPrintf(report,
                                         "SOTC write faults narrowed to their pages: %llu "
                                         "(%.1f KiB of window not read)\n",
                                         (unsigned long long)wf_n, wf_b / 1024.0);
                        }
                    }
                    // sotc: SOTC_RB_EXACT
                    {
                        const uint64_t ex_served = SotcStats::rb_ex_served.exchange(0);
                        const uint64_t ex_dl = SotcStats::rb_ex_served_dl.exchange(0);
                        const uint64_t ex_pages = SotcStats::rb_ex_pages_owed.exchange(0);
                        const uint64_t ex_objs = SotcStats::rb_ex_owed_objs.exchange(0);
                        const uint64_t ex_applied = SotcStats::rb_ex_owed_applied.exchange(0);
                        const uint64_t ex_dropped = SotcStats::rb_ex_owed_dropped.exchange(0);
                        const uint64_t ex_bytes = SotcStats::rb_ex_owed_bytes.exchange(0);
                        const uint64_t ex_trim = SotcStats::rb_ex_owed_trimmed.exchange(0);
                        const uint64_t fb_ann = SotcStats::rb_ex_fb_announced.exchange(0);
                        const uint64_t fb_unk = SotcStats::rb_ex_fb_unknown.exchange(0);
                        const uint64_t fb_floor = SotcStats::rb_ex_fb_floor.exchange(0);
                        const uint64_t fb_cpu = SotcStats::rb_ex_fb_cpu.exchange(0);
                        const uint64_t fb_inf = SotcStats::rb_ex_fb_inflight.exchange(0);
                        const uint64_t fb_oth = SotcStats::rb_ex_fb_other.exchange(0);
                        const uint64_t lab_n = SotcStats::rb_ex_label_deferred.exchange(0);
                        const uint64_t lab_ns = SotcStats::rb_ex_label_defer_ns.exchange(0);
                        const uint64_t lab_max = SotcStats::rb_ex_label_defer_max_ns.exchange(0);
                        const uint64_t ld_rec = SotcStats::rb_ex_ld_recorded.exchange(0);
                        const uint64_t ld_app = SotcStats::rb_ex_ld_applied.exchange(0);
                        const uint64_t ld_drop = SotcStats::rb_ex_ld_dropped.exchange(0);
                        const uint64_t ld_bytes = SotcStats::rb_ex_ld_bytes.exchange(0);
                        const uint64_t set_n = SotcStats::rb_ex_settle_waits.exchange(0);
                        const uint64_t set_ns = SotcStats::rb_ex_settle_ns.exchange(0);
                        const uint64_t reent = SotcStats::rb_ex_apply_reentered.exchange(0);
                        // B's adaptive label policy (liverpool.cpp): gauge and EWMAs as they
                        // are, the cumulative counters as deltas of this period.
                        const uint64_t ad_def = AmdGpu::g_rbx_addr_deferred.load();
                        const uint64_t ad_sw = AmdGpu::g_rbx_mode_switches.exchange(0);
                        const uint64_t ad_pr = AmdGpu::g_rbx_probes.exchange(0);
                        const uint64_t ad_hot = AmdGpu::g_rbx_labels_early_hot.exchange(0);
                        const uint64_t ad_ch = AmdGpu::g_rbx_waits_charged.exchange(0);
                        const uint64_t ad_un = AmdGpu::g_rbx_waits_unattributed.exchange(0);
                        const double ad_a = AmdGpu::g_rbx_ewma_a_ns.load() / 1e6;
                        const double ad_b = AmdGpu::g_rbx_ewma_b_ns.load() / 1e6;
                        const uint64_t unsafe = SotcStats::rb_ex_unsafe_announce.exchange(0);
                        const uint64_t wrong = SotcStats::rb_ex_apply_wrongthread.exchange(0);
                        if (ex_served | ex_dl | ex_pages | fb_ann | fb_unk | fb_floor | fb_cpu | fb_inf |
                            fb_oth | lab_n | ld_rec | set_n | reent | ad_sw | ad_pr | ad_hot | ad_ch | ad_un | unsafe | wrong) {
                            ReportPrintf(report,
                                         "SOTC RB exact: served without a GPU wait %llu (%llu "
                                         "with a done-only download), %llu pages released with "
                                         "owed ranges (%llu owed downloads: applied %llu, "
                                         "%.1f MiB; dropped %llu; %.1f KiB trimmed by later "
                                         "writes), usual readback: announced pending %llu, "
                                         "unknown writer %llu, older announced version undelivered %llu, CPU-bit page %llu, readback in "
                                         "flight %llu, other %llu; labels deferred %llu (avg "
                                         "%.2f ms, max %.2f ms); label downloads recorded %llu, applied %llu "
                                         "(%.1f MiB), dropped dirty %llu; settle waits %llu (%.2f ms), label ops requeued %llu; adaptive: %llu addresses deferred, %llu switches, %llu probes, %llu hot labels early, waits charged %llu / unattributed %llu, ewma landing %.2f ms, charged wait %.2f ms%s\n",
                                         static_cast<unsigned long long>(ex_served + ex_dl),
                                         static_cast<unsigned long long>(ex_dl),
                                         static_cast<unsigned long long>(ex_pages),
                                         static_cast<unsigned long long>(ex_objs),
                                         static_cast<unsigned long long>(ex_applied),
                                         ex_bytes / 1048576.0,
                                         static_cast<unsigned long long>(ex_dropped),
                                         ex_trim / 1024.0,
                                         static_cast<unsigned long long>(fb_ann),
                                         static_cast<unsigned long long>(fb_unk),
                                         static_cast<unsigned long long>(fb_floor),
                                         static_cast<unsigned long long>(fb_cpu),
                                         static_cast<unsigned long long>(fb_inf),
                                         static_cast<unsigned long long>(fb_oth),
                                         static_cast<unsigned long long>(lab_n),
                                         lab_n ? lab_ns / 1e6 / lab_n : 0.0, lab_max / 1e6,
                                         static_cast<unsigned long long>(ld_rec),
                                         static_cast<unsigned long long>(ld_app),
                                         ld_bytes / 1048576.0,
                                         static_cast<unsigned long long>(ld_drop),
                                         static_cast<unsigned long long>(set_n), set_ns / 1e6,
                                         static_cast<unsigned long long>(reent),
                                         static_cast<unsigned long long>(ad_def),
                                         static_cast<unsigned long long>(ad_sw),
                                         static_cast<unsigned long long>(ad_pr),
                                         static_cast<unsigned long long>(ad_hot),
                                         static_cast<unsigned long long>(ad_ch),
                                         static_cast<unsigned long long>(ad_un), ad_a, ad_b,
                                         (unsafe | wrong) ? " !! UNSAFE ANNOUNCE / WRONG THREAD" : "");
                        }
                    }
                    // sotc: SOTC_RB_EXACT=publish (lean XPR publish mode, sotc_stats.h counters).
                    if (const char* rbp_env = std::getenv("SOTC_RB_EXACT"); rbp_env && rbp_env[0] == 'p') {
                        const uint64_t lab = SotcStats::rbp_labels_deferred.exchange(0);
                        const uint64_t pub_bytes = SotcStats::rbp_bytes_published.exchange(0);
                        const uint64_t dropped = SotcStats::rbp_dropped_dirty.exchange(0);
                        const uint64_t cpu_skip = SotcStats::rbp_cpu_pages_skipped.exchange(0);
                        const uint64_t lat_cp = SotcStats::rbp_lat_cp_ns.exchange(0);
                        const uint64_t lat_sub = SotcStats::rbp_lat_submit_ns.exchange(0);
                        const uint64_t lat_gpu = SotcStats::rbp_lat_gpu_ns.exchange(0);
                        const uint64_t lat_land = SotcStats::rbp_lat_land_ns.exchange(0);
                        const uint64_t lat_n = SotcStats::rbp_lat_samples.exchange(0);
                        const uint64_t rd_before = SotcStats::rbp_reads_before.exchange(0);
                        const uint64_t rd_after = SotcStats::rbp_reads_after.exchange(0);
                        const double rbp_frames = static_cast<double>(std::max<size_t>(frame_ms.size(), 1));
                        const double n = static_cast<double>(lat_n);
                        const auto avg_ms = [n](uint64_t ns) { return n > 0 ? ns / 1e6 / n : 0.0; };
                        ReportPrintf(report,
                                     "SOTC RB publish: labels deferred %llu (%.1f/frame), published %llu KiB, "
                                     "dropped dirty %llu, CPU pages skipped %llu; latency avg ms: "
                                     "cp->draw %.2f, draw->submit %.2f, submit->gpu %.2f, gpu->label %.2f "
                                     "(%llu samples); guest readbacks on unapplied label data %llu, after "
                                     "apply %llu\n",
                                     static_cast<unsigned long long>(lab), lab / rbp_frames,
                                     static_cast<unsigned long long>(pub_bytes / 1024),
                                     static_cast<unsigned long long>(dropped),
                                     static_cast<unsigned long long>(cpu_skip),
                                     avg_ms(lat_cp), avg_ms(lat_sub), avg_ms(lat_gpu), avg_ms(lat_land),
                                     static_cast<unsigned long long>(lat_n),
                                     static_cast<unsigned long long>(rd_before),
                                     static_cast<unsigned long long>(rd_after));
                    }
                }
                if (char rbg[640]; SotcStats::RbGroupTake(rbg, sizeof(rbg))) ReportPrintf(report, "%s\n", rbg); // sotc: SOTC_RB_GROUP
                const uint64_t grown = SotcStats::rb_grow_grown.exchange(0);
                const uint64_t grow_cur = SotcStats::rb_grow_cur_writes.exchange(0);
                const uint64_t base_cur = SotcStats::rb_base_cur_writes.exchange(0);
                const uint64_t extra = SotcStats::rb_grow_extra_granules.exchange(0);
                const uint64_t extra_bytes = SotcStats::rb_grow_extra_bytes.exchange(0);
                if (grown != 0) {
                    ReportPrintf(report,
                                 "SOTC RB grow: %llu grown windows (%.0f%%), written by the "
                                 "current tick %llu grown / %llu base, readbacks saved <= %llu "
                                 "(%.0f KiB of other granules)\n",
                                 static_cast<unsigned long long>(grown), pct(grown),
                                 static_cast<unsigned long long>(grow_cur),
                                 static_cast<unsigned long long>(base_cur),
                                 static_cast<unsigned long long>(extra), extra_bytes / 1024.0);
                }
            }
            // sotc: read once: the VRAM line below reports it too (it printed 0 after this one).
            const uint64_t gc_freed_n = SotcStats::gc_freed.exchange(0);
            if (const u64 streamed = SotcStats::rb_stream_windows.exchange(0)) {
                ReportPrintf(report, "SOTC readback stream windows: %llu\n",
                             static_cast<unsigned long long>(streamed));
            }
            static uint64_t last_images_new = 0;
            const uint64_t images_new = SotcStats::images_new.load();
            ReportPrintf(report,
                         "SOTC GC: %.1f ticks/frame, freed %.0f/s (%llu written back), images new "
                         "%.0f/s, cached %llu (%llu MB), reused slots %llu\n",
                         flips ? double(SotcStats::gc_ticks.exchange(0)) / flips : 0.0,
                         gc_freed_n / seconds,
                         static_cast<unsigned long long>(SotcStats::gc_downloads.exchange(0)),
                         (images_new - last_images_new) / seconds,
                         static_cast<unsigned long long>(SotcStats::cached_images.load()),
                         static_cast<unsigned long long>(SotcStats::cached_image_bytes.load() >> 20),
                         static_cast<unsigned long long>(SotcStats::reuse_size_frees.exchange(0)));
            last_images_new = images_new;
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
            if (SotcStats::kHostImageCopy) {
                const uint64_t uploads = SotcStats::host_image_copies.exchange(0);
                const uint64_t busy = SotcStats::host_image_copy_busy.exchange(0);
                std::fprintf(stderr, "SOTC HOSTCOPY uploads %llu, not idle %llu\n",
                             static_cast<unsigned long long>(uploads),
                             static_cast<unsigned long long>(busy));
            }
            if (SotcStats::kFastBind) {
                const uint64_t eligible = SotcStats::fast_bind_eligible.exchange(0);
                const uint64_t hits = SotcStats::fast_bind_hits.exchange(0);
                std::fprintf(stderr, "SOTC FASTBIND hits %llu/%llu (%.1f%%)\n",
                             static_cast<unsigned long long>(hits),
                             static_cast<unsigned long long>(eligible),
                             100.0 * double(hits) / double(std::max<u64>(eligible, 1)));
                // sotc: misses (SOTC_FPS only, sotc_stats.h); tick-only = the submit tick changed.
                if (const uint64_t misses = SotcStats::fast_bind_misses.exchange(0)) {
                    const uint64_t tick_misses = SotcStats::fast_bind_tick_misses.exchange(0);
                    std::fprintf(stderr, "SOTC FASTBIND misses %llu (tick only %llu)\n",
                                 static_cast<unsigned long long>(misses),
                                 static_cast<unsigned long long>(tick_misses));
                }
                if (const uint64_t wide = SotcStats::fast_bind_wide_hits.exchange(0)) {
                    std::fprintf(stderr, "SOTC FASTBIND wide %llu (SOTC_FASTBIND_WIDE)\n",
                                 static_cast<unsigned long long>(wide));
                }
            }
            if (SotcStats::kBindCache) { // sotc: SOTC_BIND_CACHE (sotc_stats.h)
                const uint64_t lookups = SotcStats::bind_cache_lookups.exchange(0);
                const uint64_t hits = SotcStats::bind_cache_hits.exchange(0);
                uint64_t flushes[SotcStats::BindCacheReasons];
                for (uint32_t r = 0; r < SotcStats::BindCacheReasons; ++r) {
                    flushes[r] = SotcStats::bind_cache_flushes[r].exchange(0);
                }
                // sotc: SOTC_BIND_CACHE_RANGES: entries dropped by written ranges.
                char ranges[224] = "";
                if (SotcStats::kBindCacheRanges) {
                    std::snprintf(ranges, sizeof(ranges),
                                  "; invalidated by ranges %llu entries (writes %llu)",
                                  static_cast<unsigned long long>(
                                      SotcStats::bind_cache_range_entries.exchange(0)),
                                  static_cast<unsigned long long>(
                                      SotcStats::bind_cache_range_writes.exchange(0)));
                }
                if (SotcStats::kBindCacheXtick) { // sotc: SOTC_BIND_CACHE_XTICK
                    const size_t used = std::strlen(ranges);
                    std::snprintf(ranges + used, sizeof(ranges) - used,
                                  "; xtick hits %llu, repaired %llu, replays %llu, keys seen in "
                                  "the previous frame %llu",
                                  static_cast<unsigned long long>(
                                      SotcStats::bind_cache_xtick_hits.exchange(0)),
                                  static_cast<unsigned long long>(
                                      SotcStats::bind_cache_xtick_repaired.exchange(0)),
                                  static_cast<unsigned long long>(
                                      SotcStats::bind_cache_xtick_replays.exchange(0)),
                                  static_cast<unsigned long long>(
                                      SotcStats::bind_cache_seen_prev_frame.exchange(0)));
                }
                std::fprintf(stderr,
                             "SOTC BIND CACHE hits %llu/%llu (%.1f%%), refused %llu, not stored "
                             "%llu; flushes %s %llu %s %llu %s %llu %s %llu %s %llu%s\n",
                             static_cast<unsigned long long>(hits),
                             static_cast<unsigned long long>(lookups),
                             100.0 * double(hits) / double(std::max<u64>(lookups, 1)),
                             static_cast<unsigned long long>(
                                 SotcStats::bind_cache_refused.exchange(0)),
                             static_cast<unsigned long long>(
                                 SotcStats::bind_cache_uncacheable.exchange(0)),
                             SotcStats::kBindCacheFlushNames[0],
                             static_cast<unsigned long long>(flushes[0]),
                             SotcStats::kBindCacheFlushNames[1],
                             static_cast<unsigned long long>(flushes[1]),
                             SotcStats::kBindCacheFlushNames[2],
                             static_cast<unsigned long long>(flushes[2]),
                             SotcStats::kBindCacheFlushNames[3],
                             static_cast<unsigned long long>(flushes[3]),
                             SotcStats::kBindCacheFlushNames[4],
                             static_cast<unsigned long long>(flushes[4]), ranges);
            }
            // sotc: SOTC_BUFFER_MEMO alone: only the memo's hit rate (the line below needs _FRAME)
            if (SotcStats::kBufferMemo && !SotcStats::kBindCacheFrame) {
                const uint64_t memo_hits = SotcStats::bf_memo_hits.exchange(0);
                const uint64_t memo_misses = SotcStats::bf_memo_misses.exchange(0);
                const uint64_t memo_repaired = SotcStats::bf_memo_repaired.exchange(0);
                std::fprintf(stderr, "SOTC BUFFER MEMO hits %llu, misses %llu (%.1f%% hits), repaired %llu%c",
                             static_cast<unsigned long long>(memo_hits),
                             static_cast<unsigned long long>(memo_misses),
                             memo_hits + memo_misses
                                 ? 100.0 * double(memo_hits) / double(memo_hits + memo_misses)
                                 : 0.0,
                             static_cast<unsigned long long>(memo_repaired), 10);
            }
            if (SotcStats::kBindCacheFrame) { // sotc: SOTC_BIND_CACHE_FRAME (sotc_stats.h)
                char misses[160]{}, drops[160]{};
                size_t mi = 0, di = 0;
                for (uint32_t r = 0; r < SotcStats::BfReasons; ++r) {
                    mi += std::snprintf(misses + mi, sizeof(misses) - mi, "%s%s %llu",
                                        r ? ", " : "", SotcStats::kBindFrameNames[r],
                                        static_cast<unsigned long long>(
                                            SotcStats::bf_miss[r].exchange(0)));
                    di += std::snprintf(drops + di, sizeof(drops) - di, "%s%s %llu",
                                        r ? ", " : "", SotcStats::kBindFrameNames[r],
                                        static_cast<unsigned long long>(
                                            SotcStats::bf_drop[r].exchange(0)));
                }
                const uint64_t memo_hits = SotcStats::bf_memo_hits.exchange(0);
                const uint64_t memo_misses = SotcStats::bf_memo_misses.exchange(0);
                std::fprintf(stderr,
                             "SOTC BIND FRAME misses by reason [%s]; entries dropped [%s]; stored "
                             "%llu (replaced %llu, not stored: moved during the bind %llu); hits "
                             "across a game frame %llu; compute copies narrowed %llu; buffer memo "
                             "%llu/%llu (repaired %llu); distinct keys %.0f per frame, %llu "
                             "entries live of 4096; flat buffers re-copied %llu, still current "
                             "%llu\n",
                             misses, drops,
                             static_cast<unsigned long long>(SotcStats::bf_stored.exchange(0)),
                             static_cast<unsigned long long>(SotcStats::bf_replaced.exchange(0)),
                             static_cast<unsigned long long>(
                                 SotcStats::bf_not_stored_moved.exchange(0)),
                             static_cast<unsigned long long>(
                                 SotcStats::bf_cross_frame_hits.exchange(0)),
                             static_cast<unsigned long long>(SotcStats::bf_narrowed.exchange(0)),
                             static_cast<unsigned long long>(memo_hits),
                             static_cast<unsigned long long>(memo_hits + memo_misses),
                             static_cast<unsigned long long>(
                                 SotcStats::bf_memo_repaired.exchange(0)),
                             double(SotcStats::bf_distinct_keys.exchange(0)) /
                                 double(std::max<uint64_t>(SotcStats::bf_frames.exchange(0), 1)),
                             static_cast<unsigned long long>(
                                 SotcStats::bf_live.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(
                                 SotcStats::bf_flat_copied.exchange(0)),
                             static_cast<unsigned long long>(
                                 SotcStats::bf_flat_reused.exchange(0)));
            }
            if (SotcStats::kSyncFast) { // sotc: SOTC_SYNC_FAST (sotc_stats.h)
                const uint64_t lookups = SotcStats::sync_fast_lookups.exchange(0);
                const uint64_t skipped = SotcStats::sync_fast_skipped.exchange(0);
                ReportPrintf(report, "SOTC SYNC FAST skipped %llu/%llu (%.1f%%)\n",
                             static_cast<unsigned long long>(skipped),
                             static_cast<unsigned long long>(lookups),
                             100.0 * double(skipped) / double(std::max<u64>(lookups, 1)));
            }
            if (SotcStats::kProtectBatch) { // sotc: SOTC_PROTECT_BATCH (sotc_stats.h)
                ReportPrintf(report,
                             "SOTC PROTECT BATCH calls %llu (pages %llu), merged from %llu, "
                             "redundant skipped %llu\n",
                             static_cast<unsigned long long>(
                                 SotcStats::protect_batch_calls.exchange(0)),
                             static_cast<unsigned long long>(
                                 SotcStats::protect_batch_pages.exchange(0)),
                             static_cast<unsigned long long>(
                                 SotcStats::protect_batch_requests.exchange(0)),
                             static_cast<unsigned long long>(
                                 SotcStats::protect_batch_redundant.exchange(0)));
            }
            if (SotcStats::kRsMemo) { // sotc: SOTC_RS_MEMO (sotc_stats.h)
                const uint64_t lookups = SotcStats::rs_memo_lookups.exchange(0);
                const uint64_t hits = SotcStats::rs_memo_hits.exchange(0);
                const uint64_t cross = SotcStats::rs_memo_cross_hits.exchange(0);
                const uint64_t checks = SotcStats::rs_memo_verify_checks.exchange(0);
                const uint64_t mismatches = SotcStats::rs_memo_verify_mismatches.exchange(0);
                std::string reasons;
                for (uint32_t r = 0; r < SotcStats::RsMissReasons; ++r) {
                    char item[48];
                    std::snprintf(item, sizeof(item), "%s%s %llu", r ? ", " : "",
                                  SotcStats::kRsMemoMissNames[r],
                                  static_cast<unsigned long long>(
                                      SotcStats::rs_memo_misses[r].exchange(0)));
                    reasons += item;
                }
                // sotc: the mode 2 and mode 3 fields follow, only when that mode is on (mode 1's
                // line is unchanged).
                std::string extra;
                if (SotcStats::kRsMemoCross) {
                    char item[48];
                    std::snprintf(item, sizeof(item), ", cross-submit %llu",
                                  static_cast<unsigned long long>(cross));
                    extra += item;
                }
                if (SotcStats::kRsMemoVerify) {
                    char item[80];
                    std::snprintf(item, sizeof(item), ", verify %llu checked, %llu mismatches",
                                  static_cast<unsigned long long>(checks),
                                  static_cast<unsigned long long>(mismatches));
                    extra += item;
                }
                ReportPrintf(report,
                             "SOTC RS MEMO hits %llu/%llu (%.1f%%), misses by reason %s; not "
                             "stored %llu%s\n",
                             static_cast<unsigned long long>(hits),
                             static_cast<unsigned long long>(lookups),
                             100.0 * double(hits) / double(std::max<u64>(lookups, 1)),
                             reasons.c_str(),
                             static_cast<unsigned long long>(
                                 SotcStats::rs_memo_unstored.exchange(0)),
                             extra.c_str());
            }
            // sotc: SOTC_PARALLEL_RECORD (sotc_stats.h); silent while no prep thread runs.
            if (const uint64_t prepared = SotcStats::pr_prepared.exchange(0),
                not_ready = SotcStats::pr_not_ready.exchange(0),
                epoch = SotcStats::pr_epoch.exchange(0),
                ineligible = SotcStats::pr_ineligible.exchange(0);
                prepared + not_ready + epoch + ineligible != 0) {
                const double frames = static_cast<double>(std::max<size_t>(frame_ms.size(), 1));
                ReportPrintf(report,
                             "SOTC PR prep/frame: prepared %.0f, inline (not ready %.0f, epoch "
                             "%.0f, ineligible %.0f), prep lag max %u ops, CP throttled by prep "
                             "%.2f ms, verify mismatches %llu\n",
                             prepared / frames, not_ready / frames, epoch / frames,
                             ineligible / frames, SotcStats::pr_lag_max.exchange(0),
                             SotcStats::pr_throttle_ns.exchange(0) / 1e6 / frames,
                             static_cast<unsigned long long>(
                                 SotcStats::pr_verify_mismatches.exchange(0)));
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
            const uint64_t hs_hits = SotcStats::hash_small_hits.exchange(0);
            const uint64_t hs_uploads = SotcStats::hash_small_uploads.exchange(0);
            const uint64_t hs_bytes = SotcStats::hash_small_bytes.exchange(0);
            if (hs_hits || hs_uploads) { // sotc: SOTC_HASH_SMALL only
                ReportPrintf(report,
                             "SOTC hash small: hits %llu (%llu KiB upload skipped), uploads %llu\n",
                             static_cast<unsigned long long>(hs_hits),
                             static_cast<unsigned long long>(hs_bytes >> 10),
                             static_cast<unsigned long long>(hs_uploads));
            }
            const uint64_t reg_spins = SotcStats::stack_reg_spins.exchange(0);
            const uint64_t carves = runtime_memory_carve_splits();
            {
                // sotc: the guest memory table's writers (maps, unmaps, protection changes)
                uint64_t writes = 0;
                double wait_ms = 0, hold_ms = 0, worst_ms = 0;
                runtime_memory_lock_stats(&writes, &wait_ms, &hold_ms, &worst_ms);
                double worst_hold_ms = 0, read_wait_ms = 0, split_ms = 0, split_worst_ms = 0,
                       other_ms = 0;
                uint64_t read_waits = 0, split_mib = 0, split_runs = 0;
                runtime_memory_lock_stats2(&worst_hold_ms, &read_waits, &read_wait_ms);
                runtime_memory_carve_stats(&split_mib, &split_runs, &split_ms, &split_worst_ms,
                                           &other_ms);
                if (writes) {
                    ReportPrintf(report,
                                 "SOTC memory lock: %llu writes, writers waited %.1f ms (worst "
                                 "%.1f), held %.1f ms (worst %.1f); readers waited %llu times, "
                                 "%.1f ms; view splits remapped %llu MiB, %llu protection runs, "
                                 "%.1f ms (worst %.1f), other carves %.1f ms\n",
                                 static_cast<unsigned long long>(writes), wait_ms, worst_ms,
                                 hold_ms, worst_hold_ms,
                                 static_cast<unsigned long long>(read_waits), read_wait_ms,
                                 static_cast<unsigned long long>(split_mib),
                                 static_cast<unsigned long long>(split_runs), split_ms,
                                 split_worst_ms, other_ms);
                    char readers[512], writers[512];
                    runtime_memory_lock_sites(readers, writers, sizeof(readers));
                    uint64_t misses = 0, stale = 0;
                    uint64_t fast = 0;
                    runtime_memory_cache_stats(&misses, &stale, &fast);
                    ReportPrintf(report,
                                 "SOTC memory lock sites: readers waited in %s; held in %s; "
                                 "region cache misses %llu, stale %llu, read without the lock "
                                 "%llu\n",
                                 readers[0] ? readers : "-", writers[0] ? writers : "-",
                                 static_cast<unsigned long long>(misses),
                                 static_cast<unsigned long long>(stale),
                                 static_cast<unsigned long long>(fast));
                }
            }
            if (const uint64_t deferred = SotcStats::upload_deferred.exchange(0)) {
                // sotc: first texture uploads put off by the per-frame budget (texture_cache.cpp)
                ReportPrintf(report,
                             "SOTC upload budget: %llu first uploads put off (%llu MiB), %llu "
                             "images filled with zeros meanwhile\n",
                             static_cast<unsigned long long>(deferred),
                             static_cast<unsigned long long>(
                                 SotcStats::upload_deferred_bytes.exchange(0) >> 20),
                             static_cast<unsigned long long>(
                                 SotcStats::upload_zero_filled.exchange(0)));
            }
            if (const uint64_t guard_ns = SotcStats::heap_guard_ns.exchange(0)) {
                // sotc: the BPE heap guard's free-tree walks (bpe_heap_guard.cpp), this window
                ReportPrintf(report,
                             "SOTC heap guard walks: %.1f ms (slowest %.1f ms: %llu free blocks "
                             "from %llu visits); repeats skipped %llu; writes vouched by the "
                             "previous guard %llu\n",
                             guard_ns / 1e6, SotcStats::heap_guard_worst_ns.exchange(0) / 1e6,
                             static_cast<unsigned long long>(
                                 SotcStats::heap_guard_worst_blocks.exchange(0)),
                             static_cast<unsigned long long>(
                                 SotcStats::heap_guard_worst_adds.exchange(0)),
                             static_cast<unsigned long long>(
                                 SotcStats::heap_guard_repeats.exchange(0)),
                             static_cast<unsigned long long>(
                                 SotcStats::heap_guard_stale_hits.exchange(0)));
            }
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
            // sotc: unmaps (BufferCache::ReleaseMemory waits for the GPU, DropGpuModified waits for
            // the command processor unless SOTC_DROP_ASYNC is on) and region lock sleeps.
            const uint64_t release_count = SotcStats::release_finish_count.exchange(0, std::memory_order_relaxed);
            const uint64_t release_ns = SotcStats::release_finish_ns.exchange(0, std::memory_order_relaxed);
            const uint64_t drop_ns = SotcStats::drop_gpu_wait_ns.exchange(0, std::memory_order_relaxed);
            const uint64_t lock_sleeps = runtime_memory_lock_sleeps();
            const uint64_t freeze_ns = runtime_memory_carve_freeze_ns();
            if (const uint64_t nostop = runtime_memory_carve_nostop()) {
                ReportPrintf(report, "SOTC view splits without stopping the game: %llu\n",
                             static_cast<unsigned long long>(nostop));
            }
            if (release_count || release_ns || drop_ns || lock_sleeps || freeze_ns) {
                ReportPrintf(report,
                             "SOTC unmap: GPU waits %llu (%.2f ms); drop waits %.2f ms; "
                             "region lock sleeps %llu; view split freeze %.2f ms\n",
                             static_cast<unsigned long long>(release_count), release_ns / 1e6,
                             drop_ns / 1e6, static_cast<unsigned long long>(lock_sleeps),
                             freeze_ns / 1e6);
            }
            // sotc: async readbacks whose data went stale and were read back again synchronously.
            {
                const u64 replaced = SotcStats::shader_cache_replaced.exchange(0);
                const u64 left_out = SotcStats::shader_cache_left_out.exchange(0);
                if (replaced | left_out) {
                    ReportPrintf(report,
                                 "SOTC shader cache: %llu old binaries replaced, %llu permutations "
                                 "left out (slot taken)\n",
                                 static_cast<unsigned long long>(replaced),
                                 static_cast<unsigned long long>(left_out));
                }
            }
            if (const uint64_t dirty = SotcStats::readback_dirty.exchange(0)) {
                ReportPrintf(report, "SOTC readbacks redone synchronously (stale): %llu\n",
                             static_cast<unsigned long long>(dirty));
            }
            {
                u64 stale[4];
                for (int i = 0; i < 4; ++i) {
                    stale[i] = SotcStats::readback_stale[i].exchange(0);
                }
                const u64 kept = SotcStats::readback_kept_later.exchange(0);
                const u64 shared = SotcStats::rb_owner_shared.exchange(0);
                const u64 skipped = SotcStats::rb_owner_skipped_bytes.exchange(0);
                const u64 batches = SotcStats::rb_batch_submits.exchange(0);
                const u64 saved = SotcStats::rb_batch_saved.exchange(0);
                if (shared | batches) {
                    ReportPrintf(report,
                                 "SOTC readbacks sharing pages %llu (%llu KiB not copied again); "
                                 "batched submits %llu (%llu submits saved)\n",
                                 static_cast<unsigned long long>(shared),
                                 static_cast<unsigned long long>(skipped >> 10),
                                 static_cast<unsigned long long>(batches),
                                 static_cast<unsigned long long>(saved));
                }
                if (stale[0] | stale[1] | stale[2] | stale[3] | kept) {
                    ReportPrintf(report,
                                 "SOTC readbacks made stale by: unmap %llu, another readback %llu, "
                                 "later GPU write %llu, CP write %llu; applied keeping the later "
                                 "write %llu\n",
                                 static_cast<unsigned long long>(stale[0]),
                                 static_cast<unsigned long long>(stale[1]),
                                 static_cast<unsigned long long>(stale[2]),
                                 static_cast<unsigned long long>(stale[3]),
                                 static_cast<unsigned long long>(kept));
                }
            }
            // sotc: SOTC_CP_FLIP_PACE: command processor wait for free presentation frames.
            if (const uint64_t pace_ns = SotcStats::cp_flip_pace_ns.exchange(0); pace_ns && flips) {
                ReportPrintf(report, "SOTC CP flip pacing: %.2f ms per frame\n",
                             pace_ns / 1e6 / flips);
            }
            // sotc: SOTC_CP_AHEAD submit gate (gnmdriver.cpp GateWait) and compute rings too full
            // for the game's next submit (Liverpool::NoteAscDoorbell), since the last report.
            if (flips) {
                uint64_t calls[SotcStats::GateSites], waits[SotcStats::GateSites],
                    ns[SotcStats::GateSites], max_ns[SotcStats::GateSites];
                for (int k = 0; k < SotcStats::GateSites; ++k) {
                    calls[k] = SotcStats::gate_calls[k].exchange(0);
                    waits[k] = SotcStats::gate_waits[k].exchange(0);
                    ns[k] = SotcStats::gate_ns[k].exchange(0);
                    max_ns[k] = SotcStats::gate_max_ns[k].exchange(0);
                }
                const uint64_t lag_sum = SotcStats::gate_lag_sum.exchange(0);
                const uint64_t lag_n = SotcStats::gate_lag_samples.exchange(0);
                const uint64_t lag_max = SotcStats::gate_lag_max.exchange(0);
                const uint64_t windows = SotcStats::asc_full_windows.exchange(0);
                const uint64_t full_ns = SotcStats::asc_full_ns.exchange(0);
                const uint64_t full_max = SotcStats::asc_full_max_ns.exchange(0);
                const uint64_t spun = SotcStats::asc_full_spun.exchange(0);
                const uint64_t spun_ns = SotcStats::asc_full_spun_ns.exchange(0);
                const uint64_t doorbells = SotcStats::asc_doorbells.exchange(0);
                if (calls[0] | calls[1] | calls[2]) {
                    ReportPrintf(
                        report,
                        "SOTC submit gate (CP_AHEAD=%d): submit %.2f ms/frame (%.1f waits/frame, "
                        "max %.1f ms), doorbell %.2f ms/frame (%.1f, max %.1f), done %.2f "
                        "ms/frame (%.1f, max %.1f); frames in flight %.2f (max %llu); compute "
                        "ring full %llu (%.2f ms/frame, max %.1f ms), spun %llu (%.2f ms/frame), "
                        "%.1f doorbells/frame\n",
                        AmdGpu::Liverpool::CpAheadMode(), ns[0] / 1e6 / flips,
                        double(waits[0]) / flips, max_ns[0] / 1e6, ns[1] / 1e6 / flips,
                        double(waits[1]) / flips, max_ns[1] / 1e6, ns[2] / 1e6 / flips,
                        double(waits[2]) / flips, max_ns[2] / 1e6,
                        lag_n ? double(lag_sum) / lag_n : 0.0,
                        static_cast<unsigned long long>(lag_max),
                        static_cast<unsigned long long>(windows), full_ns / 1e6 / flips,
                        full_max / 1e6, static_cast<unsigned long long>(spun),
                        spun_ns / 1e6 / flips, double(doorbells) / flips);
                }
            }
            // sotc: SOTC_ASYNC_UPLOAD, totals.
            if (const uint64_t n = SotcStats::async_upload_jobs.load(std::memory_order_relaxed)) {
                ReportPrintf(
                    report,
                    "SOTC async upload: %llu uploads, %llu MiB by the copy threads; submits "
                    "waited %.1f ms, write fence %llu waits (%.1f ms), %llu kept on the draw "
                    "thread\n",
                    static_cast<unsigned long long>(n),
                    static_cast<unsigned long long>(
                        SotcStats::async_upload_bytes.load(std::memory_order_relaxed) >> 20),
                    SotcStats::async_upload_submit_ns.load(std::memory_order_relaxed) / 1e6,
                    static_cast<unsigned long long>(
                        SotcStats::async_upload_fence_waits.load(std::memory_order_relaxed)),
                    SotcStats::async_upload_fence_ns.load(std::memory_order_relaxed) / 1e6,
                    static_cast<unsigned long long>(
                        SotcStats::async_upload_kept.load(std::memory_order_relaxed)));
            }
            // sotc: SOTC_FENCE_PIECES, since the last report.
            {
                const uint64_t waits = SotcStats::fence_piece_waits.exchange(0);
                const uint64_t ns = SotcStats::fence_piece_ns.exchange(0);
                const uint64_t helped = SotcStats::fence_pieces_helped.exchange(0);
                const uint64_t reads = SotcStats::fence_read_skips.exchange(0);
                if (waits || helped || reads) {
                    ReportPrintf(report,
                                 "SOTC FENCE waits %llu (%.1f ms), pieces helped %llu, read "
                                 "faults not waited %llu\n",
                                 static_cast<unsigned long long>(waits), ns / 1e6,
                                 static_cast<unsigned long long>(helped),
                                 static_cast<unsigned long long>(reads));
                }
            }
            // sotc: SOTC_ASYNC_IMAGE_CREATE, totals (image_prefetch.h).
            if (const uint64_t n = SotcStats::async_image_requested.load(std::memory_order_relaxed)) {
                const auto get = [](const std::atomic<uint64_t>& c) {
                    return static_cast<unsigned long long>(c.load(std::memory_order_relaxed));
                };
                ReportPrintf(
                    report,
                    "SOTC async image create: %llu queued (%llu dropped), %llu made by workers "
                    "(%.1f ms, %llu refused), %llu taken ready, %llu waited (%.1f ms), %llu taken "
                    "back, %llu created on the spot, %llu never taken\n",
                    static_cast<unsigned long long>(n), get(SotcStats::async_image_skipped),
                    get(SotcStats::async_image_built),
                    SotcStats::async_image_build_ns.load(std::memory_order_relaxed) / 1e6,
                    get(SotcStats::async_image_failed), get(SotcStats::async_image_taken),
                    get(SotcStats::async_image_waits),
                    SotcStats::async_image_wait_ns.load(std::memory_order_relaxed) / 1e6,
                    get(SotcStats::async_image_taken_back), get(SotcStats::async_image_sync),
                    get(SotcStats::async_image_expired));
            }
            if (const uint64_t n = SotcStats::async_texture_uploads.load(std::memory_order_relaxed)) {
                ReportPrintf(report, "SOTC async texture uploads: %llu\n",
                             static_cast<unsigned long long>(n));
            }
            // sotc: SOTC_HOST_IMPORT, totals.
            if (const uint64_t n = SotcStats::host_import_uploads.load(std::memory_order_relaxed)) {
                ReportPrintf(
                    report,
                    "SOTC host import: %llu uploads, %llu MiB copied by the GPU (%.1f ms "
                    "waited), %llu back to the CPU, %llu chunks refused, %llu MiB imported\n",
                    static_cast<unsigned long long>(n),
                    static_cast<unsigned long long>(
                        SotcStats::host_import_bytes.load(std::memory_order_relaxed) >> 20),
                    SotcStats::host_import_ns.load(std::memory_order_relaxed) / 1e6,
                    static_cast<unsigned long long>(
                        SotcStats::host_import_fallbacks.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        SotcStats::host_import_refused.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        SotcStats::host_import_live_chunks.load(std::memory_order_relaxed) * 64));
            }
            // sotc: SOTC_HOT_PAGES: pages kept writable (sticky now, made sticky, refused at the cap).
            if (const uint64_t marks = SotcStats::hot_pages_marks.load(std::memory_order_relaxed)) {
                ReportPrintf(report,
                             "SOTC hot pages: %u sticky now (cap %u, grown %llu, shrunk %llu), %llu "
                             "made sticky, %llu at the cap\n",
                             VideoCore::HotPages::live.load(std::memory_order_relaxed),
                             VideoCore::HotPages::Max(),
                             static_cast<unsigned long long>(
                                 SotcStats::hot_pages_cap_grows.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(
                                 SotcStats::hot_pages_cap_shrinks.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(marks),
                             static_cast<unsigned long long>(
                                 SotcStats::hot_pages_cap_hits.load(std::memory_order_relaxed)));
            }
            // sotc: SOTC_DROP_FAST, totals: unmaps dropped on the guest thread, and waited.
            if (const uint64_t fast = SotcStats::drop_fast.load(std::memory_order_relaxed)) {
                ReportPrintf(report,
                             "SOTC drop fast %llu, waited %llu (%llu of them over a command "
                             "buffer in use); IB walk overflows %llu, unmapped IBs skipped %llu\n",
                             static_cast<unsigned long long>(fast),
                             static_cast<unsigned long long>(
                                 SotcStats::drop_slow.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(
                                 SotcStats::drop_fast_guarded.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(
                                 SotcStats::drop_fast_overflows.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(
                                 SotcStats::drop_fast_unmapped_ibs.load(std::memory_order_relaxed)));
            }
            {
                // sotc: SOTC_EOP_TICK labels: written, mean/max time from queue to write, flushes.
                const uint64_t n = AmdGpu::g_eop_written.exchange(0);
                const uint64_t lat = AmdGpu::g_eop_latency_ns.exchange(0);
                const uint64_t mx = AmdGpu::g_eop_max_latency_ns.exchange(0);
                const uint64_t fl = AmdGpu::g_eop_flushes.exchange(0);
                if (n || fl) {
                    ReportPrintf(report, "SOTC EOP tick: %llu labels, mean %.2f ms, max %.2f ms, flushes %llu\n",
                                 static_cast<unsigned long long>(n), n ? lat / 1e6 / n : 0.0,
                                 mx / 1e6, static_cast<unsigned long long>(fl));
                }
            }
            // sotc: SOTC_ARENA_TICK_FREE waits, and SRT walker Phis resolved vs fallen back.
            const uint64_t tick_waits = SotcStats::arena_tick_waits.exchange(0);
            const uint64_t phi_ok = SotcStats::phi_resolved.exchange(0);
            const uint64_t phi_fb = SotcStats::phi_fallback.exchange(0);
            if (tick_waits || phi_ok || phi_fb) {
                ReportPrintf(report, "SOTC arena tick waits %llu; SRT Phi resolved %llu, fallback %llu\n",
                             static_cast<unsigned long long>(tick_waits),
                             static_cast<unsigned long long>(phi_ok),
                             static_cast<unsigned long long>(phi_fb));
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
                             static_cast<unsigned long long>(gc_freed_n),
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
            if (const uint64_t waits = SotcStats::cp_waits.exchange(0)) {
                ReportPrintf(report, "SOTC CP waits for the draw thread %.0f/s\n", waits / seconds);
            }
            // sotc: CP waits on back-op writes (WaitBackWritesSlow), per 5 s window.
            if (const uint64_t bw_n = SotcStats::cp_bw_rep_waits.exchange(0)) {
                const uint64_t bw_ns = SotcStats::cp_bw_rep_ns.exchange(0);
                const uint64_t bw_max = SotcStats::cp_bw_rep_max_ns.exchange(0);
                ReportPrintf(report, "SOTC CP back-write waits: %.0f/s, %.2f ms/s, max %.2f ms\n",
                             bw_n / seconds, bw_ns / 1e6 / seconds, bw_max / 1e6);
            }
            // sotc: stutter-architecture counters (engine hooks, declared writes, tiny pipelines,
            // short sleeps); each prints only when its switch or SOTC_FPS gave it activity.
            SotcHookStats::Report(report, seconds);
            SotcWriteDecl::Report(report, seconds);
            Vulkan::SotcTinyAsyncReport(report, seconds);
            // sotc: engine probes (plan step 0c); each prints only when its hook is on and saw
            // activity in the window.
            SotcProbes::ReportXpr(report, seconds);
            SotcProbes::ReportLoader(report, seconds);
            SotcProbes::ReportAlloc(report, seconds);
            SotcProbes::ReportCvars(report, seconds);
            SotcProbes::ReportArea(report, seconds);
            SotcMemClasses::Report(report, seconds);
            // sotc: Wave C optimizations, each prints only while its switch is on.
            SotcAreaPrewarm::ReportAreaPrewarm(report, seconds); // SOTC_AREA_PREWARM >= 2
            Vulkan::ReportPipeMemo(report, seconds);              // SOTC_PIPE_MEMO
            AmdGpu::ReportCpLabelSkip(report, seconds);           // SOTC_CP_LABEL_SKIP
            // sotc: Wave D probes and optimizations (docs/re-stutters/reports2/00-PLAN.md); each
            // prints only while its switch is on.
            SotcBuildSync::ReportBuildSyncProbe(report, seconds);      // SOTC_BUILDSYNC_PROBE
            SotcProbes::ReportDtProbe(report, seconds);               // SOTC_DT_PROBE
            SotcProbes::ReportDrawTag(report, seconds);               // SOTC_DRAW_TAG
            SotcProbes::ReportParticlePrev(report, seconds);          // SOTC_PARTICLE_PREV
            SotcDecommitBudget::ReportDecommitBudget(report, seconds); // SOTC_DECOMMIT_BUDGET
            SotcMipBudget::ReportMipBudget(report, seconds);          // SOTC_MIP_BUDGET
            VideoCore::TextureCache::ReportImageFrees(report, seconds); // B4 (SOTC_FPS)
            VideoCore::TextureCache::ReportUnmapPark(report, seconds);  // SOTC_UNMAP_PARK
            Vulkan::ReportFlipWait(report, seconds);                  // B5 (SOTC_FPS)
            Vulkan::ReportPresent(report, seconds);                   // SOTC_PRESENT_FRAMES / SOTC_FPS
            VideoCore::ReportReadbackRips(report, seconds);           // B6 (SOTC_FPS)
            // sotc: SOTC_OP_OVERLAP (plan step 7c.0): footprint conflicts between consecutive ops.
            if (SotcStats::kOpOverlap) {
                const uint64_t ovl_ops = SotcStats::ovl_ops.exchange(0);
                const uint64_t ovl_nr = SotcStats::ovl_not_ready.exchange(0);
                const uint64_t ovl_unk = SotcStats::ovl_unknown.exchange(0);
                const uint64_t ovl_pairs = SotcStats::ovl_pairs.exchange(0);
                const uint64_t ovl_conf = SotcStats::ovl_conflicts.exchange(0);
                const uint64_t ovl_brk = SotcStats::ovl_breaks.exchange(0);
                if (ovl_ops || ovl_nr) {
                    ReportPrintf(report,
                                 "SOTC OP OVERLAP: %llu ops (%llu no footprint: %llu not ready, "
                                 "%llu unknown), %llu consecutive pairs, %llu conflicting "
                                 "(%.0f%%), %llu pairs cut by CP ops\n",
                                 static_cast<unsigned long long>(ovl_ops),
                                 static_cast<unsigned long long>(ovl_nr + ovl_unk),
                                 static_cast<unsigned long long>(ovl_nr),
                                 static_cast<unsigned long long>(ovl_unk),
                                 static_cast<unsigned long long>(ovl_pairs),
                                 static_cast<unsigned long long>(ovl_conf),
                                 ovl_pairs ? 100.0 * ovl_conf / ovl_pairs : 0.0,
                                 static_cast<unsigned long long>(ovl_brk));
                }
            }
            {
                char usleep_line[1024];
                usleep_line[0] = '\0';
                runtime_usleep_stats(usleep_line, sizeof(usleep_line), seconds);
                report += usleep_line;
            }
            { // sotc: SOTC_MUTEX_SPIN_US stats (Wave D C1)
                char spin_line[256];
                spin_line[0] = '\0';
                runtime_mutex_spin_stats(spin_line, sizeof(spin_line), seconds);
                if (spin_line[0]) {
                    report += spin_line;
                    report += '\n';
                }
            }
            // sotc: SOTC_WD_FUSE accounting (Liverpool).
            if (flips) {
                uint64_t ops[4], ns[4];
                uint64_t func_total = 0;
                for (int k = 0; k < 4; ++k) {
                    ops[k] = SotcStats::cp_func_ops[k].exchange(0);
                    ns[k] = SotcStats::draw_func_ns[k].exchange(0);
                    func_total += ops[k];
                }
                const double nf = static_cast<double>(flips);
                std::fprintf(stderr,
                             "SOTC CP ops: func %.0f/frame (write data %.0f, fused %.0f), back-write "
                             "waits %.1f (%.2f ms)\n",
                             func_total / nf, ops[0] / nf,
                             SotcStats::cp_wd_fused.exchange(0) / nf,
                             SotcStats::cp_bw_waits.exchange(0) / nf,
                             SotcStats::cp_bw_wait_ns.exchange(0) / 1e6 / nf);
                std::fprintf(stderr,
                             "SOTC CP ops draw thread ms/frame: write data %.2f, eop %.2f, dma "
                             "%.2f, other %.2f; write data -> indirect pairs %.0f/frame\n",
                             ns[0] / 1e6 / nf, ns[1] / 1e6 / nf, ns[2] / 1e6 / nf, ns[3] / 1e6 / nf,
                             SotcStats::cp_wd_pairs.exchange(0) / nf);
            }
            // sotc: SOTC_ADAPTIVE_LOAD governor (sotc_adaptive.h), this report's window.
            if (SotcAdaptive::kEnabled) {
                const uint64_t seen = SotcStats::adaptive_flips.exchange(0);
                const uint64_t relieved = SotcStats::adaptive_relieved_flips.exchange(0);
                ReportPrintf(report,
                             "SOTC ADAPTIVE relieved %.1f%% of frames, engaged %llu times, "
                             "cascades amortised %llu, flushes saved %llu, deferred: image GC "
                             "%llu, arena checks %llu\n",
                             seen ? 100.0 * static_cast<double>(relieved) / seen : 0.0,
                             static_cast<unsigned long long>(SotcStats::adaptive_engaged.exchange(0)),
                             static_cast<unsigned long long>(SotcStats::adaptive_amortised.exchange(0)),
                             static_cast<unsigned long long>(
                                 SotcStats::adaptive_flushes_saved.exchange(0)),
                             static_cast<unsigned long long>(
                                 SotcStats::adaptive_gc_deferred.exchange(0)),
                             static_cast<unsigned long long>(
                                 SotcStats::adaptive_evict_deferred.exchange(0)));
            }
            if (flips) {
                const double idle_ms = SotcStats::draw_idle_ns.exchange(0) / 1e6;
                const double parse_ms = SotcStats::cp_parse_ns.exchange(0) / 1e6;
                std::fprintf(stderr, "SOTC threads per frame: draw busy %.2f ms, command parse %.2f ms\n",
                             (seconds * 1000.0 - idle_ms) / flips, parse_ms / flips);
            }
            // sotc: on-CPU time of the GPU threads and what the draw thread, the command
            // processor and the recorder waited for (SotcStats::FrameAccount), per frame.
            {
                static SotcStats::FrameAccount report_last{};
                const SotcStats::FrameAccount now_acc = SotcStats::TakeAccount();
                const SotcStats::FrameAccount d = SotcStats::DiffAccount(now_acc, report_last);
                report_last = now_acc;
                if (flips) {
                    using namespace SotcStats;
                    const double per = 1e6 * static_cast<double>(flips); // ns -> ms per frame
                    const double nper = static_cast<double>(flips);
                    std::fprintf(stderr,
                                 "SOTC cpu/frame: cp %.2f draw %.2f rec %.2f passsec %.2f upload "
                                 "%.2f ms\n",
                                 d.cpu_ns[RoleCp] / per, d.cpu_ns[RoleDraw] / per,
                                 d.cpu_ns[RoleRecorder] / per, d.cpu_ns[RolePassSecondary] / per,
                                 d.cpu_ns[RoleUpload] / per);
                    std::fprintf(stderr,
                                 "SOTC draw waits/frame: gpu %.2f (%.1f), recorder %.2f (%.1f), "
                                 "upload %.2f, tc lock %.2f ms\n",
                                 d.wait_ns[WaitGpu] / per, d.wait_count[WaitGpu] / nper,
                                 d.wait_ns[WaitRecorder] / per, d.wait_count[WaitRecorder] / nper,
                                 d.wait_ns[WaitUpload] / per, d.wait_ns[WaitTcLock] / per);
                    std::fprintf(stderr,
                                 "SOTC cp/frame: parse %.2f, blocked on the draw thread %.2f, net "
                                 "parse %.2f ms\n",
                                 d.parse_ns / per, d.blocked_ns / per,
                                 (d.parse_ns > d.blocked_ns ? d.parse_ns - d.blocked_ns : 0) / per);
                    // sotc: command processor no-progress spin and SOTC_CP_IDLE_US sleeps
                    // (Liverpool::Process), per frame. Kinds: rounds in which a task waited on
                    // it (several kinds can share a round).
                    {
                        static uint64_t last[6 + CpSpinKinds]{};
                        const uint64_t cur[6 + CpSpinKinds] = {
                            cp_spin_rounds.load(), cp_sleeps.load(),       cp_sleep_timeouts.load(),
                            cp_sleep_ns.load(),    cp_wakes.load(),        cp_wake_latency_ns.load(),
                            cp_spin_kind_rounds[0].load(), cp_spin_kind_rounds[1].load(),
                            cp_spin_kind_rounds[2].load(), cp_spin_kind_rounds[3].load(),
                            cp_spin_kind_rounds[4].load()};
                        uint64_t v[6 + CpSpinKinds];
                        for (int i = 0; i < 6 + CpSpinKinds; ++i) {
                            v[i] = cur[i] - last[i];
                            last[i] = cur[i];
                        }
                        std::fprintf(stderr,
                                     "SOTC CP spin: %.2f ms/frame in %.0f rounds (wait reg mem "
                                     "%.0f, label %.0f, flip %.0f, de counter %.0f, other %.0f), "
                                     "sleeps %.1f (timeouts %.1f, %.2f ms), wake latency avg %.1f "
                                     "us\n",
                                     d.spin_ns / per, v[0] / nper, v[6] / nper, v[7] / nper,
                                     v[8] / nper, v[9] / nper, v[10] / nper, v[1] / nper,
                                     v[2] / nper, v[3] / per,
                                     v[4] ? v[5] / 1e3 / static_cast<double>(v[4]) : 0.0);
                    }
                    std::fprintf(stderr,
                                 "SOTC recorder/frame: busy %.2f, async upload wait %.2f, queue "
                                 "submit %.2f ms\n",
                                 d.rec_busy / per, d.rec_upload / per, d.rec_submit / per);
                }
            }
            if (SotcStats::kDrawSplit && split_frames && split_tick0) {
                const double ns = double(SotcStats::NowNs() - split_ns0);
                const double ticks_per_ms =
                    ns > 0 ? (SotcStats::Ticks() - split_tick0) / (ns / 1e6) : 1.0;
                const auto line = [&](const uint64_t* v, double frames) {
                    using namespace SotcStats;
                    const auto ms = [&](int c) { return v[c] / ticks_per_ms / frames; };
                    constexpr int threads = SplitParts; // the thread columns follow the parts
                    char text[1536];
                    std::snprintf(
                        text, sizeof(text),
                        "draw cpu %.1f ms (idle spin %.1f; idle %.1f), cp cpu %.1f, recorder cpu "
                        "%.1f; ops draw %.1f, indirect %.1f, dispatch %.1f, func %.1f, commands "
                        "%.1f; bind %.1f [buffers %.1f, textures %.1f, dma %.1f]; texture uploads "
                        "%.1f [hash %.1f, staging %.1f, copy %.1f (%.1f MB in %.0f copies, %.0f of "
                        "2 MB+, %.0f page faults), detile %.1f, record %.1f]; "
                        "draw waits gpu "
                        "%.1f, submit %.1f, recorder %.1f, frame %.1f; queue max %llu; guest "
                        "readback waits %.1f [queue %.1f, submit %.1f, gpu %.1f, finish %.1f]",
                        ms(threads + SplitThreadDraw), ms(SplitIdleSpin), ms(SplitIdle),
                        ms(threads + SplitThreadCp), ms(threads + SplitThreadRecorder),
                        ms(SplitDraw), ms(SplitIndirect), ms(SplitDispatch), ms(SplitFunc),
                        ms(SplitCommands), ms(SplitBind), ms(SplitBindBuffers),
                        ms(SplitBindTextures), ms(SplitBindDma), ms(SplitTexRefresh),
                        ms(SplitTexHash), ms(SplitTexStaging), ms(SplitTexCopy),
                        v[SplitTexBytes] / 1048576.0 / frames, v[SplitTexCopies] / frames,
                        v[SplitTexBig] / frames, v[SplitTexFaults] / frames,
                        ms(SplitTexDetile), ms(SplitTexRecord), ms(SplitWaitGpu),
                        ms(SplitWaitSubmit), ms(SplitWaitRecorder), ms(SplitWaitFrame),
                        static_cast<unsigned long long>(v[kSplitQueue]), v[SplitRbCount] / frames,
                        ms(SplitRbQueue), ms(SplitRbSubmit), ms(SplitRbGpu), ms(SplitRbFinish));
                    return std::string{text};
                };
                std::fprintf(stderr, "SOTC DRAW SPLIT per frame (%u frames): %s\n", split_frames,
                             line(split_sum, split_frames).c_str());
                std::fprintf(stderr, "SOTC DRAW SPLIT worst frame %.1f ms: %s\n", split_worst_ms,
                             line(split_worst, 1.0).c_str());
                if (split_slow_frames) {
                    std::fprintf(stderr, "SOTC DRAW SPLIT frames > 40 ms: %u, per frame: %s\n",
                                 split_slow_frames, line(split_slow, split_slow_frames).c_str());
                }
            }
            split_frames = 0;
            split_slow_frames = 0;
            split_worst_ms = 0.0;
            std::fill(std::begin(split_sum), std::end(split_sum), 0);
            std::fill(std::begin(split_worst), std::end(split_worst), 0);
            std::fill(std::begin(split_slow), std::end(split_slow), 0);
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
                    "times (%.0f ms)\n",
                    static_cast<unsigned long long>(built),
                    SotcStats::async_compute_build_ns.load(std::memory_order_relaxed) / 1e6,
                    static_cast<unsigned long long>(
                        SotcStats::async_compute_waits.load(std::memory_order_relaxed)),
                    SotcStats::async_compute_wait_ns.load(std::memory_order_relaxed) / 1e6);
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
            // sotc: compute shaders translated ahead of their dispatch (SOTC_ASYNC_COMPUTE_XLATE).
            if (const uint64_t queued =
                    SotcStats::compute_prefetch_queued.load(std::memory_order_relaxed)) {
                std::fprintf(
                    stderr,
                    "SOTC COMPUTE PREFETCH %llu shaders from %llu scans, %llu dispatches waited "
                    "(%.1f ms), %llu taken back, %llu timeouts\n",
                    static_cast<unsigned long long>(queued),
                    static_cast<unsigned long long>(
                        SotcStats::compute_prefetch_scans.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        SotcStats::compute_prefetch_waits.load(std::memory_order_relaxed)),
                    SotcStats::compute_prefetch_wait_ns.load(std::memory_order_relaxed) / 1e6,
                    static_cast<unsigned long long>(
                        SotcStats::compute_prefetch_taken_back.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        SotcStats::compute_prefetch_timeouts.load(std::memory_order_relaxed)));
            }
            {
                // sotc: guest write fault latency (SotcStats::FaultStat); p99 from a
                // histogram of 4 buckets per power of two, so it is the upper edge of one
                namespace FS = SotcStats::FaultStat;
                const double w = double(FS::count.exchange(0));
                const double sq = double(FS::seq.exchange(0));
                const double tot = double(FS::total_ns.exchange(0));
                const double mx = double(FS::mutex_ns.exchange(0));
                const double fe = double(FS::fence_ns.exchange(0));
                const double pr = double(FS::protect_ns.exchange(0));
                const double fl = double(FS::flush_ns.exchange(0));
                uint64_t buckets[FS::kBuckets];
                uint64_t total = 0;
                for (unsigned i = 0; i < FS::kBuckets; ++i) {
                    buckets[i] = FS::hist[i].exchange(0);
                    total += buckets[i];
                }
                uint64_t p99 = 0;
                uint64_t seen = 0;
                const uint64_t target = total - total / 100; // first bucket reaching 99%
                for (unsigned i = 0; i < FS::kBuckets && total != 0; ++i) {
                    seen += buckets[i];
                    if (seen >= target) {
                        p99 = FS::BucketUpper(i);
                        break;
                    }
                }
                const double per = w > 0 ? 1.0 / (w * 1000.0) : 0.0;
                ReportPrintf(report,
                             "SOTC FAULTS: %.0f writes (seq %.0f%%), avg %.1f us (mutex %.1f, "
                             "fence %.1f, protect %.1f, readback %.1f), p99 %.1f us; ahead pages %llu "
                             "(unchanged %llu); granule skipped %llu\n",
                             w, w > 0 ? 100.0 * sq / w : 0.0, tot * per, mx * per, fe * per,
                             pr * per, fl * per, p99 / 1000.0,
                             static_cast<unsigned long long>(FS::ahead_pages.exchange(0)),
                             static_cast<unsigned long long>(FS::ahead_unchanged.exchange(0)),
                             static_cast<unsigned long long>(
                                 SotcStats::fault_granule_skipped.exchange(0)));
                // sotc: where the time of those faults went (SotcStats::FaultStat::Stage), us per
                // fault: the stages are exclusive of the mutex and protect time inside them, which
                // are listed apart, and of each other (inval does not include tc, granule does not
                // include hash); inval includes the readback above; tc is 0 until the rasterizer
                // marks the texture cache part; os is the SOTC_FAULT_OSCOST measurement.
                double st[FS::kStCount];
                double st_sum = 0;
                for (unsigned i = 0; i < FS::kStCount; ++i) {
                    st[i] = double(FS::stage_total_ns[i].exchange(0));
                    st_sum += st[i];
                }
                double os_in = 0, os_out = 0;
                VideoCore::MeasureFaultOsCost(os_in, os_out);
                const double rest = tot - st_sum - mx - fe - pr;
                ReportPrintf(report,
                             "SOTC FAULT SPLIT (us/fault): entry %.1f, fence %.1f, inval %.1f "
                             "+ tc %.1f, granule %.1f + hash %.1f, ahead %.1f, flush %.1f, "
                             "repair %.1f, mutex %.1f, protect %.1f, rest %.1f; os in %.1f out "
                             "%.1f; coalesce %llu runs -> %llu calls, %llu pages, %llu skipped, "
                             "%llu self\n",
                             st[FS::kStEntry] * per, fe * per, st[FS::kStInval] * per,
                             st[FS::kStTc] * per, st[FS::kStGranule] * per,
                             st[FS::kStGranHash] * per, st[FS::kStAhead] * per,
                             st[FS::kStFlush] * per, st[FS::kStRepair] * per, mx * per, pr * per,
                             rest * per, os_in, os_out,
                             static_cast<unsigned long long>(
                                 SotcStats::coalesce_requests.exchange(0)),
                             static_cast<unsigned long long>(SotcStats::coalesce_calls.exchange(0)),
                             static_cast<unsigned long long>(SotcStats::coalesce_pages.exchange(0)),
                             static_cast<unsigned long long>(
                                 SotcStats::coalesce_skipped.exchange(0)),
                             static_cast<unsigned long long>(
                                 SotcStats::coalesce_self_fixes.exchange(0)));
            }
            const bool cmd_stats = SotcStats::kCmdStats; // sotc: as the counters (sotc_stats.h)
            if (cmd_stats && flips) {
                double k[7];
                for (int i = 0; i < 7; ++i) {
                    k[i] = double(SotcStats::cmd_kinds[i].exchange(0)) / flips;
                }
                ReportPrintf(report,
                             "SOTC cmds/frame: draw %.0f (%.0f empty), indirect %.0f gpu-args + "
                             "%.0f cpu-args (%.0f empty), dispatch %.0f, dispatch-indirect %.0f\n",
                             k[0], k[6], k[1], k[2], k[3], k[4], k[5]);
                // sotc: IT_SET_PREDICATION packets per frame (decoded only, not applied).
                double p[5];
                for (int i = 0; i < 5; ++i) {
                    p[i] = double(SotcStats::pred_ops[i].exchange(0)) / flips;
                }
                ReportPrintf(report,
                             "SOTC predication/frame: clear %.0f, zpass %.0f, primcount %.0f, "
                             "bool64 %.0f, other %.0f\n",
                             p[0], p[1], p[2], p[3], p[4]);
                unsigned long long r[13];
                for (int i = 0; i < 13; ++i) {
                    r[i] = SotcStats::recorder_blocks[i].exchange(0);
                }
                ReportPrintf(report,
                             "SOTC recorder blocks: draw %llu, dispatch %llu, begin rendering "
                             "%llu, end rendering %llu, barrier %llu, copy %llu, bind pipeline "
                             "%llu, descriptors %llu, push constants %llu, dynamic state %llu, "
                             "vertex/index %llu, other %llu, label %llu\n",
                             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10],
                             r[11], r[12]);
                // sotc: SOTC_PASS_SEC passes per frame: on a secondary command buffer, and kept
                // on the primary although over the draw threshold
                ReportPrintf(report, "SOTC passes/frame: secondary %.1f, fallback %.1f\n",
                             double(SotcStats::passes_secondary.exchange(0)) / flips,
                             double(SotcStats::passes_fallback.exchange(0)) / flips);
                ReportPrintf(report,
                             "SOTC faults/s: guest writes %.0f, walker loads %.0f, CP reads %.0f, "
                             "granule pages %.0f\n",
                             SotcStats::fault_writes.exchange(0) / seconds,
                             SotcStats::fault_walker.exchange(0) / seconds,
                             SotcStats::fault_gpu_reads.exchange(0) / seconds,
                             SotcStats::fault_granule_pages.exchange(0) / seconds);
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
        liverpool->NotifyCpWait(); // sotc: SOTC_CP_IDLE_US (VO label waits)
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
            liverpool->NotifyCpWait(); // sotc: SOTC_CP_IDLE_US (VO label waits)
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
    BbThreads::PlaceThread(BbThreads::Role::Present);
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

    // sotc: SOTC_PRESENT_WAIT=1 locks the flips to the display (vk_presenter.cpp tags each present
    // with an id, vk_swapchain.cpp forces FIFO). The timer above still sets the cadence; the
    // display gate only holds a flip until an earlier present is on screen:
    //   - after presenting id N, the next flip waits for id N - k + 1, where k is the number of
    //     presents allowed in flight, max(1, round(refresh / target fps));
    //   - the refresh is the median of the last 16 intervals between present completions, each
    //     taken as the host time a vkWaitForPresentKHR first saw it done; the target fps is
    //     60 / (flip rate + 1). Until 4 intervals are known, k is 1;
    //   - the gate waits at most 100 ms, then the flip goes out on the timer (the fallback);
    //   - each tick waits at most one vblank for it, so this thread keeps counting vblanks.
    // Off by default: nothing below runs. Async present (SOTC_ASYNC_PRESENT, FSR 3 frame
    // generation) has no gate, since its present id is taken after Flip returns.
    static const bool present_wait = [] {
        const char* v = std::getenv("SOTC_PRESENT_WAIT");
        return v && v[0] == '1';
    }();
    constexpr u64 present_wait_timeout_ns = 100'000'000;
    u64 gate_id = 0;       // the present the next flip waits for, 0 for none
    u64 gate_since_ns = 0; // host steady-clock time the gate was set
    const auto steady_ns = [] {
        return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count());
    };
    // Display refresh in Hz from the measured present completions: 0 until 4 intervals are known.
    const auto measured_refresh_hz = [&]() -> double {
        const u64 last = presenter->LastPresentId();
        std::array<u64, 16> intervals{};
        std::size_t n = 0;
        for (u64 i = 0; i < 16 && last >= i + 2; ++i) {
            const u64 id = last - i;
            const u64 prev = presenter->PresentDoneNs(id - 1);
            const u64 done = presenter->PresentDoneNs(id);
            if (prev != 0 && done > prev) {
                intervals[n++] = done - prev;
            }
        }
        if (n < 4) {
            return 0.0;
        }
        std::nth_element(intervals.begin(), intervals.begin() + n / 2, intervals.begin() + n);
        return 1e9 / double(intervals[n / 2]);
    };
    // True when the flip may go out now: no gate, the gate's present is on screen, or the gate
    // waited its 100 ms (the timer decides that flip).
    const auto gate_open = [&]() -> bool {
        if (gate_id == 0) {
            return true;
        }
        const u64 waited = steady_ns() - gate_since_ns;
        if (waited >= present_wait_timeout_ns) {
            gate_id = 0;
            return true;
        }
        // sotc: poll only (timeout 0). Blocking here for a vblank stalled this vblank thread, and
        // its timer then caught up with back-to-back vblanks: uneven flip pacing (review of
        // 2026-10-27). The next tick polls again.
        if (presenter->WaitPresented(gate_id, 0)) {
            gate_id = 0;
            return true;
        }
        return false;
    };

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
            take_request = limit_vblanks == 0 || vblank_status.count - last_flip_vblank >= limit_vblanks;
        } else {
            const u64 interval = u64(flip_rate + 1) * vblanks_per_60hz;
            take_request = vblank_status.count - last_flip_vblank >= interval;
        }
        // sotc: SOTC_PRESENT_WAIT display gate (see above). The poll of the last present, without
        // waiting, records when it was seen on screen for the refresh estimate.
        const bool display_pacing = present_wait && !AsyncPresent();
        if (!display_pacing) {
            gate_id = 0;
        } else if (flipped) {
            presenter->WaitPresented(presenter->LastPresentId(), 0);
            if (take_request) {
                take_request = gate_open();
            }
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
                // sotc: SOTC_LOW_LATENCY, start of the frame: NVIDIA sleep / AMD anti-lag / the
                // generic wait for the previous frame's GPU work. No-op when the switch is off.
                presenter->LowLatencyFrameStart();
                Flip(request);
                last_flip_vblank = vblank_status.count;
                flipped = true;
                // sotc: SOTC_ADAPTIVE_LOAD (sotc_adaptive.h): the flip interval against the
                // interval the live cap or frame limit aims at (0: uncapped without a limit).
                if (SotcAdaptive::kEnabled) {
                    u64 target_ns = 0;
                    if (flip_rate > 0) {
                        target_ns = u64(flip_rate + 1) * 1'000'000'000ULL / 60;
                    } else {
                        const int live_limit = SotcFpsCap::limit_fps.load(std::memory_order_relaxed);
                        const u64 limit_vblanks =
                            live_limit >= 0 ? vblanks_for(live_limit) : start_limit_vblanks;
                        target_ns = limit_vblanks * 1'000'000'000ULL /
                                    std::max<u64>(1, EmulatorSettings.GetVblankFrequency());
                    }
                    SotcAdaptive::OnFlip(steady_ns(), target_ns);
                }
                if (display_pacing) {
                    // The present Flip just made is the last id (Flip presents synchronously here).
                    const u64 last = presenter->LastPresentId();
                    const double target_fps = 60.0 / double(flip_rate + 1);
                    const double refresh_hz = measured_refresh_hz();
                    const u64 in_flight =
                        refresh_hz > 0.0
                            ? std::max<u64>(1, u64(std::llround(refresh_hz / target_fps)))
                            : 1;
                    gate_id = last >= in_flight ? last - in_flight + 1 : 0;
                    gate_since_ns = steady_ns();
                }
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
