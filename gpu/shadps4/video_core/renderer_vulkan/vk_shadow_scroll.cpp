// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: reuse of sun shadow cascades moved by whole texels, see vk_shadow_scroll.h.

#include "video_core/renderer_vulkan/vk_shadow_scroll.h"

#include <cstdio>
#include <vk_mem_alloc.h>

#include "common/assert.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"

#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/shadow_scroll_frag.h"

namespace Vulkan {

namespace {
bool HasStencil(vk::Format format) {
    return format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint ||
           format == vk::Format::eD16UnormS8Uint;
}

vk::ImageAspectFlags AllAspects(vk::Format format) {
    return HasStencil(format) ? vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil
                              : vk::ImageAspectFlags{vk::ImageAspectFlagBits::eDepth};
}

struct PushConstants {
    s32 shift_x;
    s32 shift_y;
    float dz;
    float empty;
};
} // namespace

ShadowScroll::ShadowScroll(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_} {
    const auto device = instance.GetDevice();
    vertex = CompileSPV(FS_TRI_VERT, device);
    fragment = CompileSPV(SHADOW_SCROLL_FRAG, device);
    const vk::DescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = vk::DescriptorType::eSampledImage,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };
    auto [set_result, set] = device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = 1,
        .pBindings = &binding,
    });
    ASSERT(set_result == vk::Result::eSuccess);
    set_layout = std::move(set);
    const vk::PushConstantRange push_range = {
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    const vk::DescriptorSetLayout layouts[] = {*set_layout};
    auto [layout_result, layout] = device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = layouts,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    });
    ASSERT(layout_result == vk::Result::eSuccess);
    pipeline_layout = std::move(layout);
}

ShadowScroll::~ShadowScroll() {
    const auto device = instance.GetDevice();
    DestroyScratch(source);
    DestroyScratch(checked);
    DestroyScratch(drawn);
    device.destroy(vertex);
    device.destroy(fragment);
}

void ShadowScroll::DestroyScratch(Scratch& scratch) {
    if (scratch.view) {
        instance.GetDevice().destroyImageView(scratch.view);
    }
    if (scratch.attach_view) {
        instance.GetDevice().destroyImageView(scratch.attach_view);
    }
    if (scratch.image) {
        vmaDestroyImage(instance.GetAllocator(), scratch.image, scratch.allocation);
    }
    scratch = Scratch{};
}

bool ShadowScroll::EnsureScratch(Scratch& scratch, vk::Format format, u32 width, u32 height,
                                 bool target) {
    if (scratch.image && scratch.format == format && scratch.width == width &&
        scratch.height == height) {
        return true;
    }
    if (scratch.image) {
        // Still possibly in use by recorded work: let the scheduler destroy it later.
        scheduler.DeferOperation([device = instance.GetDevice(), allocator = instance.GetAllocator(),
                                  old = scratch]() mutable {
            device.destroyImageView(old.view);
            if (old.attach_view) {
                device.destroyImageView(old.attach_view);
            }
            vmaDestroyImage(allocator, old.image, old.allocation);
        });
        scratch = Scratch{};
    }
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferDst |
                                vk::ImageUsageFlagBits::eTransferSrc |
                                vk::ImageUsageFlagBits::eSampled;
    if (target) {
        usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
    }
    const VkImageCreateInfo info = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = usage,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    const VmaAllocationCreateInfo alloc_info = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage image{};
    if (vmaCreateImage(instance.GetAllocator(), &info, &alloc_info, &image, &scratch.allocation,
                       nullptr) != VK_SUCCESS) {
        return false;
    }
    scratch.image = vk::Image{image};
    auto [view_result, view] = instance.GetDevice().createImageView({
        .image = scratch.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{.aspectMask = vk::ImageAspectFlagBits::eDepth,
                          .baseMipLevel = 0,
                          .levelCount = 1,
                          .baseArrayLayer = 0,
                          .layerCount = 1},
    });
    if (view_result != vk::Result::eSuccess) {
        DestroyScratch(scratch);
        return false;
    }
    scratch.view = view;
    if (target) {
        auto [attach_result, attach] = instance.GetDevice().createImageView({
            .image = scratch.image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange{.aspectMask = AllAspects(format),
                              .baseMipLevel = 0,
                              .levelCount = 1,
                              .baseArrayLayer = 0,
                              .layerCount = 1},
        });
        if (attach_result != vk::Result::eSuccess) {
            DestroyScratch(scratch);
            return false;
        }
        scratch.attach_view = attach;
    }
    scratch.format = format;
    scratch.width = width;
    scratch.height = height;
    scratch.layout = vk::ImageLayout::eUndefined;
    return true;
}

void ShadowScroll::TransitScratch(Scratch& scratch, vk::ImageLayout layout) {
    if (scratch.layout == layout && layout != vk::ImageLayout::eTransferDstOptimal) {
        return;
    }
    const vk::ImageMemoryBarrier2 barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .oldLayout = scratch.layout,
        .newLayout = layout,
        .image = scratch.image,
        .subresourceRange{.aspectMask = AllAspects(scratch.format),
                          .baseMipLevel = 0,
                          .levelCount = 1,
                          .baseArrayLayer = 0,
                          .layerCount = 1},
    };
    scheduler.CommandBuffer().pipelineBarrier2({
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    });
    scratch.layout = layout;
}

void ShadowScroll::CopyLayer(VideoCore::Image& image, u32 layer, Scratch& scratch) {
    const VideoCore::SubresourceRange range{.base = {.level = 0, .layer = layer},
                                            .extent = {.levels = 1, .layers = 1}};
    runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                    vk::AccessFlagBits2::eTransferRead, range);
    runtime.FlushBarriers();
    TransitScratch(scratch, vk::ImageLayout::eTransferDstOptimal);
    const vk::ImageCopy region = {
        .srcSubresource = {vk::ImageAspectFlagBits::eDepth, 0, layer, 1},
        .srcOffset = {0, 0, 0},
        .dstSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
        .dstOffset = {0, 0, 0},
        .extent = {scratch.width, scratch.height, 1},
    };
    scheduler.CommandBuffer().copyImage(image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                                        scratch.image, vk::ImageLayout::eTransferDstOptimal,
                                        region);
}

vk::Pipeline ShadowScroll::GetPipeline(vk::Format format) {
    for (const auto& [f, pipeline] : pipelines) {
        if (f == format) {
            return *pipeline;
        }
    }
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling = {
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_state = {
        .depthTestEnable = true,
        .depthWriteEnable = true,
        .depthCompareOp = vk::CompareOp::eAlways,
    };
    const std::array dynamic_states = {vk::DynamicState::eViewportWithCount,
                                       vk::DynamicState::eScissorWithCount};
    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    const std::array stages = {
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex, .module = vertex, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment, .module = fragment, .pName = "main"},
    };
    const vk::PipelineRenderingCreateInfo rendering = {
        .colorAttachmentCount = 0,
        .depthAttachmentFormat = format,
        .stencilAttachmentFormat = vk::Format::eUndefined,
    };
    const vk::PipelineColorBlendStateCreateInfo color_blending{};
    const vk::PipelineViewportStateCreateInfo viewport_info{};
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineRasterizationStateCreateInfo raster_state{.lineWidth = 1.f};
    auto [result, pipeline] = instance.GetDevice().createGraphicsPipelineUnique(
        VK_NULL_HANDLE, vk::GraphicsPipelineCreateInfo{
                            .pNext = &rendering,
                            .stageCount = static_cast<u32>(stages.size()),
                            .pStages = stages.data(),
                            .pVertexInputState = &vertex_input,
                            .pInputAssemblyState = &input_assembly,
                            .pViewportState = &viewport_info,
                            .pRasterizationState = &raster_state,
                            .pMultisampleState = &multisampling,
                            .pDepthStencilState = &depth_state,
                            .pColorBlendState = &color_blending,
                            .pDynamicState = &dynamic_info,
                            .layout = *pipeline_layout,
                        });
    if (result != vk::Result::eSuccess) {
        return {};
    }
    pipelines.emplace_back(format, std::move(pipeline));
    return *pipelines.back().second;
}

bool ShadowScroll::Scroll(VideoCore::Image& image, u32 layer, s32 sx, s32 sy, float dz,
                          float empty, bool to_check) {
    const vk::Format format = image.info.pixel_format;
    const u32 width = image.info.size.width, height = image.info.size.height;
    static u32 failures = 0;
    const auto fail = [&](const char* why) {
        if (failures++ < 5) {
            std::fprintf(stderr, "SOTC SHADOW SCROLL not possible: %s (format %d, %ux%u, %u layers, "
                                 "stencil %d, samples %u)\n",
                         why, static_cast<int>(format), width, height,
                         image.info.resources.layers, int(image.info.props.has_stencil),
                         image.info.num_samples);
        }
        return false;
    };
    if (image.info.num_samples != 1 || layer >= image.info.resources.layers ||
        (image.info.props.has_stencil && !HasStencil(format))) {
        return fail("image kind");
    }
    const vk::Pipeline pipeline = GetPipeline(format);
    if (!pipeline) {
        return fail("pipeline");
    }
    if (!EnsureScratch(source, format, width, height, false) ||
        (to_check && !EnsureScratch(checked, format, width, height, true))) {
        return fail("scratch image");
    }
    scheduler.EndRendering();

    // The layer as it is (last frame's cascade) -> source.
    CopyLayer(image, layer, source);
    TransitScratch(source, vk::ImageLayout::eShaderReadOnlyOptimal);

    // Target: the layer itself, or the verification scratch image.
    vk::ImageView target_view;
    // The stencil aspect (if any) is attached to nothing and keeps its texels.
    const vk::ImageLayout target_layout = HasStencil(format)
                                              ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                              : vk::ImageLayout::eDepthAttachmentOptimal;
    if (to_check) {
        TransitScratch(checked, target_layout);
        target_view = checked.attach_view;
    } else {
        const VideoCore::SubresourceRange range{.base = {.level = 0, .layer = layer},
                                                .extent = {.levels = 1, .layers = 1}};
        runtime.Transit(&image, target_layout,
                        vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                            vk::PipelineStageFlagBits2::eLateFragmentTests,
                        vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                            vk::AccessFlagBits2::eDepthStencilAttachmentRead,
                        range);
        runtime.FlushBarriers();
        auto [view_result, view] = instance.GetDevice().createImageView({
            .image = image.GetImage(),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange{.aspectMask = AllAspects(format),
                              .baseMipLevel = 0,
                              .levelCount = 1,
                              .baseArrayLayer = layer,
                              .layerCount = 1},
        });
        if (view_result != vk::Result::eSuccess) {
            return false;
        }
        scheduler.DeferOperation(
            [device = instance.GetDevice(), view] { device.destroyImageView(view); });
        target_view = view;
    }

    RenderState state{};
    state.width = static_cast<u16>(width);
    state.height = static_cast<u16>(height);
    state.num_layers = 1;
    state.depth_stencil_attachment.image_view = target_view;
    state.depth_stencil_attachment.image_layout = target_layout;
    state.depth_stencil_attachment.has_depth = true;
    scheduler.BeginRendering(state);

    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::DescriptorImageInfo image_info = {
        .imageView = source.view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet write = {
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eSampledImage,
        .pImageInfo = &image_info,
    };
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0, write);
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    const PushConstants push{sx, sy, dz, empty};
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(push),
                         &push);
    cmdbuf.setViewportWithCount(vk::Viewport{0.f, 0.f, float(width), float(height), 0.f, 1.f});
    cmdbuf.setScissorWithCount(vk::Rect2D{{0, 0}, {width, height}});
    cmdbuf.draw(3, 1, 0, 0);
    scheduler.EndRendering();
    scheduler.GetDynamicState().Invalidate();
    return true;
}

void ShadowScroll::SaveCheck(VideoCore::Image& image, u32 layer, const std::string& prefix) {
    if (!checked.image) {
        return;
    }
    const vk::Format format = image.info.pixel_format;
    const u32 width = checked.width, height = checked.height;
    if (!EnsureScratch(drawn, format, width, height, false)) {
        return;
    }
    scheduler.EndRendering();
    CopyLayer(image, layer, drawn);
    // Both to a host-visible buffer as 32-bit floats (D32) or 16-bit unorm (D16).
    const u32 texel = format == vk::Format::eD16Unorm ? 2 : 4;
    const u64 size = u64(width) * height * texel;
    const VkBufferCreateInfo buffer_info = static_cast<VkBufferCreateInfo>(vk::BufferCreateInfo{
        .size = size * 2,
        .usage = vk::BufferUsageFlagBits::eTransferDst,
    });
    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_info, &alloc_info, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        return;
    }
    TransitScratch(checked, vk::ImageLayout::eTransferSrcOptimal);
    TransitScratch(drawn, vk::ImageLayout::eTransferSrcOptimal);
    const auto cmdbuf = scheduler.CommandBuffer();
    for (int i = 0; i < 2; ++i) {
        const vk::BufferImageCopy copy = {
            .bufferOffset = size * i,
            .imageSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
            .imageExtent = {width, height, 1},
        };
        cmdbuf.copyImageToBuffer(i == 0 ? checked.image : drawn.image,
                                 vk::ImageLayout::eTransferSrcOptimal, vk::Buffer{buffer}, copy);
    }
    scheduler.Finish();
    vmaInvalidateAllocation(instance.GetAllocator(), allocation, 0, VK_WHOLE_SIZE);
    for (int i = 0; i < 2; ++i) {
        const std::string path = prefix + (i == 0 ? "_scrolled.raw" : "_drawn.raw");
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(static_cast<const u8*>(info.pMappedData) + size * i, 1, size, f);
            std::fclose(f);
        }
    }
    std::fprintf(stderr, "SOTC SHADOW CHECK saved %s_{scrolled,drawn}.raw (%ux%u, %u bytes/texel)\n",
                 prefix.c_str(), width, height, texel);
    vmaDestroyBuffer(instance.GetAllocator(), buffer, allocation);
}

} // namespace Vulkan
