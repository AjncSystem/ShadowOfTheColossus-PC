// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>

#include "core/libraries/videoout/buffer.h"
#include <imgui.h> // sotc port: ImTextureID only
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_low_latency.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Image image;
    vk::ImageView image_view;
    vk::Fence present_done;
    vk::Semaphore ready_semaphore;
    u64 ready_tick;
    bool is_hdr{false};
    u8 id{};
    /// sotc: this frame without the HUD (Rasterizer::TakeHudless) for frame generation, or null.
    Frame* hudless{};

    ImTextureID imgui_texture;
};

enum SchedulerType {
    Draw,
    Present,
    CpuFlip,
};

class Rasterizer;

class Presenter {
public:
    Presenter(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool);
    ~Presenter();

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }

    HostPasses::FsrPass::Settings& GetFsrSettingsRef() {
        return fsr_settings;
    }

    Frontend::WindowSDL& GetWindow() const {
        return window;
    }

    Rasterizer& GetRasterizer() const {
        return *rasterizer.get();
    }

    bool IsHDRSupported() const {
        return swapchain.HasHDR();
    }

    void SetHDR(bool enable) {
        if (!IsHDRSupported()) {
            return;
        }
        swapchain.SetHDR(enable);
        pp_settings.hdr = enable ? 1 : 0;
    }

    VideoCore::Image& RegisterVideoOutSurface(
        const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr cpu_address) {
        vo_buffers_addr.emplace_back(cpu_address);
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        const auto image_id = texture_cache.FindImage(desc);
        auto& image = texture_cache.GetImage(image_id);
        image.usage.vo_surface = 1u;
        return image;
    }

    bool IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const;

    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address);

    /// sotc: SOTC_FLIP_SERVICE=1 (off by default). Set by the command processor when the draw
    /// thread prepares the frames: PrepareFrame first waits for a free presentation frame,
    /// calling this every 200 us of waiting, so the commands of the game's threads (unmaps,
    /// readbacks) run during that wait instead of after it. The wait comes before anything of
    /// the flip is done, so they run as they would between two operations.
    inline static void (*wait_service)() = nullptr;

    /// sotc: SOTC_CP_FLIP_PACE=1 (off by default). The presenter the command processor paces its
    /// flips with, null when off. Before it queues a flip for the draw thread, the command
    /// processor waits (PaceFlip) until more presentation frames are free than flips it queued
    /// that the draw thread has not reached yet (FlipReached), so the draw thread finds a free
    /// frame instead of blocking in GetRenderFrame while the game's threads wait for it. The
    /// frame rate is unchanged: the same wait moves to the command processor.
    inline static Presenter* cp_pacer = nullptr;
    void PaceFlip();
    void FlipReached() {
        pending_flips.fetch_sub(1, std::memory_order_acq_rel);
    }

    Frame* PrepareBlankFrame(bool present_thread);

    void Present(Frame* frame, bool is_reusing_frame = false, bool is_game_frame = true);
    Frame* PrepareLastFrame();

    /// sotc: SOTC_LOW_LATENCY. Start of a game frame, called by the presenting thread before the
    /// flip is released (driver.cpp). Does nothing when the switch is off.
    void LowLatencyFrameStart() {
        low_latency.FrameStart(swapchain.GetHandle());
    }

    /// sotc: SOTC_PRESENT_WAIT. Id of the last present tagged for VK_KHR_present_id, 0 while none
    /// (the switch is off, or nothing presented yet). Written by the presenting thread.
    u64 LastPresentId() const {
        return last_present_id.load(std::memory_order_acquire);
    }

    /// sotc: SOTC_PRESENT_WAIT. Waits up to timeout_ns (vkWaitForPresentKHR) until present `id` is
    /// on screen. True when it is, or when there is nothing to wait for (id 0, a present from a
    /// replaced swapchain, or an error, which must not stall the caller); false on timeout, or
    /// when `id` is not presented yet. The host time the wait saw it done is kept, see
    /// PresentDoneNs.
    bool WaitPresented(u64 id, u64 timeout_ns);

    /// sotc: SOTC_PRESENT_WAIT. Host steady-clock time (ns) WaitPresented saw present `id` done,
    /// 0 when it has not (or is too old to be kept: the last 64 presents are).
    u64 PresentDoneNs(u64 id) const;

private:
    Frame* GetRenderFrame();
    /// sotc: SOTC_FLIP_SERVICE: waits until a presentation frame is free, calling wait_service.
    void WaitFreeFrame();

    /// sotc: SOTC_PRESENT_WAIT. A tagged present: its id, the swapchain it went to, and when
    /// WaitPresented saw it done (0 until then).
    struct PresentRecord {
        u64 id{};
        VkSwapchainKHR swapchain{};
        u64 done_ns{};
    };

    /// sotc: takes the id of the present about to be queued (call under Scheduler::submit_mutex).
    u64 TagPresent();

    void RecreateFrame(Frame* frame, u32 width, u32 height);

    /// sotc: FSR 3 frame generation: presents the frame between the last game frame and
    /// `frame`. False when there is none.
    bool PresentGenerated(Frame* frame);

    void SetExpectedGameSize(s32 width, s32 height);

private:
    float expected_ratio{1920.0 / 1080.0f};
    u32 expected_frame_width{1920};
    u32 expected_frame_height{1080};

    Frontend::WindowSDL& window;
    Instance instance;
    HostPasses::FsrPass fsr_pass;
    /// sotc: the same FSR 1 for the frame without the HUD (frame generation), with its own images:
    /// fsr_pass's ring holds the game frame's.
    HostPasses::FsrPass fsr_pass_hudless;
    HostPasses::FsrPass::Settings fsr_settings{};
    HostPasses::PostProcessingPass::Settings pp_settings{};
    HostPasses::PostProcessingPass pp_pass;
    AmdGpu::Liverpool* liverpool;
    Scheduler draw_scheduler;
    Scheduler present_scheduler;
    Scheduler flip_scheduler;
    Swapchain swapchain;
    LowLatency low_latency{instance}; ///< sotc: SOTC_LOW_LATENCY backend, see vk_low_latency.h
    Runtime runtime;
    std::unique_ptr<Rasterizer> rasterizer;
    VideoCore::TextureCache& texture_cache;
    vk::UniqueCommandPool command_pool;
    std::vector<Frame> present_frames;
    std::vector<Frame> hudless_frames; ///< sotc: Frame::hudless, one per present frame
    std::queue<Frame*> free_queue;
    Frame* last_submit_frame;
    std::mutex free_mutex;
    std::condition_variable free_cv;
    std::atomic<s32> pending_flips{0}; ///< sotc: SOTC_CP_FLIP_PACE, see cp_pacer
    std::condition_variable_any frame_cv;
    std::vector<VAddr> vo_buffers_addr;
    std::atomic<u64> last_present_id{0};         ///< sotc: SOTC_PRESENT_WAIT, see LastPresentId
    mutable std::mutex present_records_mutex;    ///< sotc: guards present_records
    std::array<PresentRecord, 64> present_records{}; ///< sotc: indexed by id % 64
};

} // namespace Vulkan
