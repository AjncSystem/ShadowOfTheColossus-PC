//  SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/host_passes/pp_pass.h"

#include <cstddef>

#include "common/assert.h"
#include "core/emulator_settings.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/post_process_frag.h"
#include "video_core/host_shaders/pp_key_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#include <algorithm>
#include <cmath>

#include <boost/container/static_vector.hpp>

namespace Vulkan::HostPasses {

// sotc: Settings as post_process.frag reads them. 128 bytes is the push constant size every
// Vulkan device must take, so the switches share one word (bits 0-1 hdr, 2 srgb_input, 3-4 ui,
// 5 drt, 6 dither at 8 bits, 7 dither at 10 bits, 8-15 the grain cell in 1/16 pixels).
struct GpuSettings {
    float gamma;
    u32 flags;
    float grain;
    u32 seed;
    std::array<float, 4> vignette;
    float ui_sharpen, sky_sharpen, hdr_white, hdr_curve, source_peak, highlight_gain, hdr_peak,
        ui_white;
    float exposure, contrast, shadows, highlights, saturation, blowout, flare, gamma_correction,
        highlight_saturation;
    float hue_b, hue_c, hue_d, hue_e, hue_correction;
    float gamut_expansion;
    float hue_a;
};
static_assert(sizeof(GpuSettings) == 128);
static_assert(offsetof(GpuSettings, vignette) == 16);

static GpuSettings Pack(const PostProcessingPass::Settings& s, u32 output_height) {
    u32 flags = (s.hdr & 3u) | ((s.srgb_input ? 1u : 0u) << 2) | ((s.ui & 3u) << 3) |
                ((s.drt ? 1u : 0u) << 5);
    // The grain cell: a pixel at 1080p, 2 at 2160p (white noise per pixel was half as coarse at
    // 4K as at 1080p), times the size setting.
    const float cell = std::max(1.0f, float(output_height) / 1080.0f * s.grain_size);
    flags |= u32(std::clamp(cell * 16.0f + 0.5f, 16.0f, 255.0f)) << 8;
    if (s.dither_bits == 8) {
        flags |= 1u << 6;
    } else if (s.dither_bits == 10) {
        flags |= 1u << 7;
    }
    return GpuSettings{
        .gamma = s.gamma,
        .flags = flags,
        .grain = s.grain,
        .seed = s.seed,
        .vignette = s.vignette,
        .ui_sharpen = s.ui_sharpen,
        .sky_sharpen = s.sky_sharpen,
        // hdr 2 (SDR game on HDR10) has its SDR white in the paper white's place.
        .hdr_white = s.hdr == 2 ? s.paper_white : s.hdr_white,
        .hdr_curve = s.hdr_curve,
        .source_peak = s.source_peak,
        .highlight_gain = s.highlight_gain,
        .hdr_peak = s.hdr_peak,
        .ui_white = s.ui_white,
        .exposure = s.exposure,
        .contrast = s.contrast,
        .shadows = s.shadows,
        .highlights = s.highlights,
        .saturation = s.saturation,
        .blowout = s.blowout,
        .flare = s.flare,
        .gamma_correction = s.gamma_correction,
        .highlight_saturation = s.highlight_saturation,
        .hue_b = s.hue_b,
        .hue_c = s.hue_c,
        .hue_d = s.hue_d,
        .hue_e = s.hue_e,
        .hue_correction = s.hue_correction,
        .gamut_expansion = s.gamut_expansion,
        .hue_a = s.hue_a,
    };
}

void PostProcessingPass::Create(vk::Device device, const vk::Format surface_format) {
    boost::container::static_vector<vk::DescriptorSetLayoutBinding, 4> bindings{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
        {
            .binding = 2,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
        {
            // sotc: the scene key (pp_key.comp)
            .binding = 3,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
    };

    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci{
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };

    desc_set_layout = Check<"create pp descriptor set layout">(
        device.createDescriptorSetLayoutUnique(desc_layout_ci));

    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(GpuSettings),
    };

    const auto& vs_module = CompileSPV(FS_TRI_VERT, device);
    ASSERT(vs_module);
    SetObjectName(device, vs_module, "fs_tri.vert");

    const auto& fs_module = CompileSPV(POST_PROCESS_FRAG, device);
    ASSERT(fs_module);
    SetObjectName(device, fs_module, "post_process.frag");

    const std::array shaders_ci{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = vs_module,
            .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = fs_module,
            .pName = "main",
        },
    };

    const vk::PipelineLayoutCreateInfo layout_info{
        .setLayoutCount = 1U,
        .pSetLayouts = &*desc_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    };

    pipeline_layout =
        Check<"create pp pipeline layout">(device.createPipelineLayoutUnique(layout_info));

    const std::array pp_color_formats{
        surface_format,
    };
    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci{
        .colorAttachmentCount = pp_color_formats.size(),
        .pColorAttachmentFormats = pp_color_formats.data(),
    };

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info{
        .vertexBindingDescriptionCount = 0u,
        .vertexAttributeDescriptionCount = 0u,
    };

    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };

    const vk::Viewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = 1.0f,
        .height = 1.0f,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    const vk::Rect2D scissor = {
        .offset = {0, 0},
        .extent = {1, 1},
    };

    const vk::PipelineViewportStateCreateInfo viewport_info{
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };

    const vk::PipelineRasterizationStateCreateInfo raster_state{
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eBack,
        .frontFace = vk::FrontFace::eClockwise,
        .depthBiasEnable = false,
        .lineWidth = 1.0f,
    };

    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };

    const std::array attachments{
        vk::PipelineColorBlendAttachmentState{
            .blendEnable = false,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
        },
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending{
        .logicOpEnable = false,
        .logicOp = vk::LogicOp::eCopy,
        .attachmentCount = attachments.size(),
        .pAttachments = attachments.data(),
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };

    const std::array dynamic_states{
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };

    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = dynamic_states.size(),
        .pDynamicStates = dynamic_states.data(),
    };

    const vk::GraphicsPipelineCreateInfo pipeline_info{
        .pNext = &pipeline_rendering_ci,
        .stageCount = shaders_ci.size(),
        .pStages = shaders_ci.data(),
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *pipeline_layout,
    };

    pipeline = Check<"create post process pipeline">(device.createGraphicsPipelineUnique(
        /*pipeline_cache*/ {}, pipeline_info));

    // Once pipeline is compiled, we don't need the shader module anymore
    device.destroyShaderModule(vs_module);
    device.destroyShaderModule(fs_module);

    // Create sampler resource
    const vk::SamplerCreateInfo sampler_ci{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
    };
    sampler = Check<"create pp sampler">(device.createSamplerUnique(sampler_ci));
}

void PostProcessingPass::Render(vk::CommandBuffer cmdbuf, vk::ImageView input,
                                vk::Extent2D input_size, Frame& frame, Settings settings,
                                vk::ImageView ui, vk::ImageView sky) {
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Host/Post processing",
        });
    }

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const std::array<vk::RenderingAttachmentInfo, 1> attachments{{
        {
            .imageView = frame.image_view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
        },
    }};
    const vk::RenderingInfo rendering_info{
        .renderArea{
            .extent{
                .width = frame.width,
                .height = frame.height,
            },
        },
        .layerCount = 1,
        .colorAttachmentCount = attachments.size(),
        .pColorAttachments = attachments.data(),
    };

    vk::DescriptorImageInfo image_info{
        .sampler = *sampler,
        .imageView = input,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    vk::DescriptorImageInfo ui_info{
        .sampler = *sampler,
        .imageView = ui ? ui : input,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    vk::DescriptorImageInfo sky_info{
        .sampler = *sampler,
        .imageView = sky ? sky : input,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    if (!sky) {
        settings.sky_sharpen = 0.0f;
    }

    ASSERT_MSG(key_buffer, "pp pass: CreateKey not called");
    const vk::DescriptorBufferInfo key_info{
        .buffer = key_buffer->Handle(),
        .offset = 0,
        .range = 16,
    };

    const std::array set_writes{
        vk::WriteDescriptorSet{
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &image_info,
        },
        vk::WriteDescriptorSet{
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &ui_info,
        },
        vk::WriteDescriptorSet{
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &sky_info,
        },
        vk::WriteDescriptorSet{
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 3,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &key_info,
        },
    };

    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);

    const std::array viewports = {
        vk::Viewport{
            .width = static_cast<float>(frame.width),
            .height = static_cast<float>(frame.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        },
    };

    cmdbuf.setViewport(0, viewports);
    cmdbuf.setScissor(0, vk::Rect2D{
                             .extent{
                                 .width = frame.width,
                                 .height = frame.height,
                             },
                         });

    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0, set_writes);
    const GpuSettings gpu = Pack(settings, frame.height);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(gpu),
                         &gpu);

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.draw(3, 1, 0, 0);
    cmdbuf.endRendering();

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame.image,
        .subresourceRange = simple_subresource,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }
}

// sotc: the scene key for the HDR picture controls (pp_key.comp, post_process.frag grade()).
void PostProcessingPass::CreateKey(const Instance& instance) {
    const vk::Device device = instance.GetDevice();
    key_buffer = std::make_unique<VideoCore::Buffer>(instance, 0, 16,
                                                     VideoCore::MemoryType::DeviceLocal,
                                                     "pp scene key");
    const std::array bindings{
        vk::DescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        vk::DescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    };
    key_set_layout = Check<"create pp key descriptor set layout">(
        device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo{
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
    const vk::PushConstantRange range{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = 16,
    };
    key_layout = Check<"create pp key pipeline layout">(
        device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo{
            .setLayoutCount = 1,
            .pSetLayouts = &*key_set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
    const auto module = CompileSPV(PP_KEY_COMP, device);
    ASSERT(module);
    SetObjectName(device, module, "pp_key.comp");
    key_pipeline = Check<"create pp key pipeline">(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage =
                    {
                        .stage = vk::ShaderStageFlagBits::eCompute,
                        .module = module,
                        .pName = "main",
                    },
                .layout = *key_layout,
            }));
    device.destroyShaderModule(module);
}

void PostProcessingPass::MeasureKey(vk::CommandBuffer cmdbuf, vk::ImageView input,
                                    KeyInput mode, float curve) {
    if (!key_buffer || !key_pipeline) {
        return;
    }
    // Smoothed like an eye adapting: about half a second to follow a change of scene, so the
    // pivot does not jump with a flash or a camera cut.
    const auto now = std::chrono::steady_clock::now();
    float alpha = 1.0f;
    if (key_measured) {
        const float dt = std::chrono::duration<float>(now - key_last).count();
        alpha = 1.0f - std::exp(-std::clamp(dt, 0.0f, 1.0f) / 0.5f);
    }
    key_last = now;
    key_measured = true;

    // The input was written by the game, DLSS or FSR; the key was read by the last frame's pass.
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead |
                         vk::AccessFlagBits2::eShaderStorageRead |
                         vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &before,
    });

    const vk::DescriptorImageInfo image_info{
        .sampler = *sampler,
        .imageView = input,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::DescriptorBufferInfo key_info{
        .buffer = key_buffer->Handle(),
        .offset = 0,
        .range = 16,
    };
    const std::array writes{
        vk::WriteDescriptorSet{
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &image_info,
        },
        vk::WriteDescriptorSet{
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &key_info,
        },
    };
    struct {
        u32 mode;
        float curve;
        float alpha;
        float pad;
    } params{static_cast<u32>(mode), curve, alpha, 0.0f};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *key_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *key_layout, 0, writes);
    cmdbuf.pushConstants(*key_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params),
                         &params);
    cmdbuf.dispatch(1, 1, 1);

    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &after,
    });
}

} // namespace Vulkan::HostPasses
