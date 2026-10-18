// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: frame interpolation of SOTC_FG (docs/sotc/FRAMEGEN.md): FSR 3.1.6 optical flow + frame
// interpolation (third_party/fsr-vulkan) between two real frames. Run by the frame pacer thread
// on the present scheduler's real command buffers, never on the draw thread (whose recorder
// replays fake handles). Phase 3 inputs are neutral: zero motion vectors and constant depth, so
// the interpolation follows the optical flow of the color alone.

#pragma once

#include <array>
#include <utility>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

struct FfxVkFsr3_3_1_6FrameGenerationContext;

namespace Vulkan::HostPasses {

class FgPass {
public:
    FgPass(const Instance& instance, Scheduler& scheduler, Scheduler& draw_scheduler);
    ~FgPass();

    /// Interpolates between the frame given to the previous call and `frame` (R8G8B8A8, General
    /// layout, ready at its tick) and submits the work. Returns the generated frame, to present
    /// before `frame`, or null when there is none (new history after Reset, size change or HDR;
    /// error). The generated frame stays valid until the second call after this one.
    Frame* Generate(const Frame& frame, double frame_time_ms);

    /// The next Generate starts a new history instead of interpolating from the frame before.
    void Reset() {
        reset = true;
    }

    /// An interpolation call failed: no more frames are generated this session.
    bool Failed() const {
        return failed;
    }

    /// GPU time of each interpolation that completed since the last call, in ms.
    std::vector<float> TakeGpuTimes() {
        return std::exchange(gpu_ms, {});
    }

private:
    struct Image {
        vk::Image image;
        VmaAllocation allocation{};
    };
    struct Slot {
        Frame frame{}; // output image + present fence, presented like a real frame
        VmaAllocation allocation{};
        u64 tick{};     // present scheduler tick of the interpolation submit
        u64 frame_id{}; // its FFX frame id, retired once the tick is done
        bool pending{};
    };

    bool Recreate(u32 width, u32 height);
    void Destroy();
    /// Retires the interpolations whose submit completed (reads their timestamps).
    void Retire();

    const Instance& instance;
    Scheduler& scheduler;
    Scheduler& draw_scheduler;
    FfxVkFsr3_3_1_6FrameGenerationContext* context{};
    Image depth{}, motion{};
    std::array<Slot, 2> slots{};
    u32 next_slot{};
    u32 render_width{}, render_height{};
    u64 frame_id{};
    bool reset{true};
    bool failed{};
    vk::QueryPool queries{};
    double timestamp_period_ns{};
    std::vector<float> gpu_ms;
};

} // namespace Vulkan::HostPasses
