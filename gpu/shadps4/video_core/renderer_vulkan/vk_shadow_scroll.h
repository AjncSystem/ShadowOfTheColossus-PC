// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: reuse of sun shadow cascades moved by whole texels (see Rasterizer::ReuseShadowCascade).

#pragma once

#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

VK_DEFINE_HANDLE(VmaAllocation)

namespace VideoCore {
struct Image;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;

class ShadowScroll {
public:
    ShadowScroll(const Instance& instance, Scheduler& scheduler, Runtime& runtime);
    ~ShadowScroll();

    /// Rewrites layer `layer` of the depth image with its own content moved by (sx, sy) texels
    /// and every depth moved by dz; texels coming from outside hold `empty` (the clear depth).
    /// With `to_check` the result goes to a scratch image instead (verification). Command
    /// processor thread, no rendering active.
    bool Scroll(VideoCore::Image& image, u32 layer, s32 sx, s32 sy, float dz, float empty,
                bool to_check);

    /// Verification: saves the scratch result of the last Scroll(to_check) and the layer as it
    /// is now (drawn by the game) to `<prefix>_scrolled.raw` / `<prefix>_drawn.raw` (32-bit
    /// floats, width x height). Waits for the GPU.
    void SaveCheck(VideoCore::Image& image, u32 layer, const std::string& prefix);

private:
    struct Scratch {
        vk::Image image;
        VmaAllocation allocation{};
        vk::ImageView view;        ///< depth aspect: sampled, copied
        vk::ImageView attach_view; ///< all aspects: depth attachment (target scratch only)
        vk::Format format{};
        u32 width{}, height{};
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    };

    bool EnsureScratch(Scratch& scratch, vk::Format format, u32 width, u32 height, bool target);
    void DestroyScratch(Scratch& scratch);
    vk::Pipeline GetPipeline(vk::Format format);
    void TransitScratch(Scratch& scratch, vk::ImageLayout layout);
    void CopyLayer(VideoCore::Image& image, u32 layer, Scratch& scratch);

    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    vk::ShaderModule vertex{}, fragment{};
    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout pipeline_layout;
    std::vector<std::pair<vk::Format, vk::UniquePipeline>> pipelines;
    Scratch source;  ///< copy of the layer the shift reads
    Scratch checked; ///< verification: the shifted layer
    Scratch drawn;   ///< verification: the layer the game drew
};

} // namespace Vulkan
