/*
 * Copyright (c) 2026 Q2RTX FSR Vulkan contributors
 * SPDX-License-Identifier: MIT
 *
 * sotc: local copy of gpu/third_party/fsr-vulkan/src/ffx_vk_fsr3_3_1_6_framegen.cpp (c64f093),
 * swapped into the 3.1.6 frame generation target by gpu/CMakeLists.txt. Upstream never drives
 * the bridge's frame bookkeeping on this path (only the 3.1.5 upscaler calls
 * ffxVkFsr3_3_1_5BridgeBeginFrame/RetireFrame), so every staged constant buffer and per-job
 * descriptor pool stayed tagged frame 0 and lived until context destroy: about 12
 * vkAllocateMemory plus one VkDescriptorPool per job on every interpolation. RecordPrepare now
 * opens the bridge frame and ContextRetireFrame retires it. Edits are marked "sotc:".
 */

#include "ffx_vk_fsr3_3_1_5_bridge.h"

#include "ffx_interface.h"
#include "ffx_frameinterpolation.h"
#include "ffx_opticalflow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeCreatePipeline(
    FfxInterface*, FfxShaderBlob*, const FfxPipelineDescription*, FfxUInt32, FfxPipelineState*);
extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeDestroyPipeline(
    FfxInterface*, FfxPipelineState*, FfxUInt32);
extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeCreateResource(
    FfxInterface*, const FfxCreateResourceDescription*, FfxUInt32, FfxResourceInternal*);
extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeDestroyResource(
    FfxInterface*, FfxResourceInternal, FfxUInt32);
extern "C" FfxApiResource ffxVkFsr3_3_1_5BridgeGetResource(
    FfxInterface*, FfxResourceInternal);
extern "C" FfxApiResourceDescription ffxVkFsr3_3_1_5BridgeGetResourceDescription(
    FfxInterface*, FfxResourceInternal);
extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeRegisterResource(
    FfxInterface*, const FfxApiResource*, FfxUInt32, FfxResourceInternal*);
extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeUnregisterResources(
    FfxInterface*, FfxCommandList, FfxUInt32);
extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeScheduleGpuJob(
    FfxInterface*, const FfxGpuJobDescription*);
extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeExecuteGpuJobs(
    FfxInterface*, FfxCommandList, FfxUInt32);
extern "C" FfxVersionNumber ffxVkFsr3_3_1_5BridgeGetSDKVersion(FfxInterface*);
extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeCreateBackendContext(
    FfxInterface*, FfxEffect, FfxEffectBindlessConfig*, FfxUInt32*);
extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeDestroyBackendContext(FfxInterface*, FfxUInt32);
extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeGetDeviceCapabilities(
    FfxInterface*, FfxDeviceCapabilities*);
extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeGetEffectGpuMemoryUsage(
    FfxInterface*, FfxUInt32, FfxApiEffectMemoryUsage*);
extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeStageConstantBufferData(
    FfxInterface*, void*, FfxUInt32, FfxConstantBuffer*);
extern "C" FfxApiResource ffxVkFsr3_3_1_5BridgeResolveResource(
    FfxVkFsr3_3_1_5Bridge*, FfxVkFsr3_3_1_5Resource);

namespace {

struct OwnedSharedImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize allocationSize = 0u;
    FfxVkFsr3_3_1_5Resource imported{};
    FfxApiResource resource{};
};

static FfxVkFsr3_3_1_6FrameGenerationResult result_from_ffx(FfxErrorCode result)
{
    if (result == FFX_OK)
        return FFX_VK_FSR3_3_1_6_FRAMEGEN_OK;
    if (result == static_cast<FfxErrorCode>(FFX_ERROR_OUT_OF_MEMORY))
        return FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    if (result == static_cast<FfxErrorCode>(FFX_ERROR_INVALID_POINTER) ||
        result == static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT))
        return FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    return FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_BACKEND;
}

static VkFormat to_vk_format(FfxApiSurfaceFormat format)
{
    switch (format) {
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case FFX_API_SURFACE_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
    case FFX_API_SURFACE_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
    case FFX_API_SURFACE_FORMAT_R8_UINT: return VK_FORMAT_R8_UINT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static FfxApiSurfaceFormat to_ffx_format(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

static uint32_t find_memory_type(VkPhysicalDevice physicalDevice, uint32_t bits,
                                 VkMemoryPropertyFlags required)
{
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
    for (uint32_t index = 0u; index < properties.memoryTypeCount; ++index) {
        if ((bits & (1u << index)) != 0u &&
            (properties.memoryTypes[index].propertyFlags & required) == required)
            return index;
    }
    return UINT32_MAX;
}

static bool make_shared_image(VkPhysicalDevice physicalDevice, VkDevice device,
                              FfxVkFsr3_3_1_5Bridge* bridge,
                              const FfxApiResourceDescription& description,
                              OwnedSharedImage* outImage)
{
    if (!outImage || !bridge || description.type != FFX_API_RESOURCE_TYPE_TEXTURE2D) {
        std::fprintf(stderr, "FSR3.1.6 shared resource is not a 2D image: type=%u format=%u extent=%ux%u\n",
                     description.type, description.format, description.width, description.height);
        return false;
    }
    const VkFormat format = to_vk_format(static_cast<FfxApiSurfaceFormat>(description.format));
    if (format == VK_FORMAT_UNDEFINED || description.width == 0u || description.height == 0u) {
        std::fprintf(stderr, "FSR3.1.6 shared image unsupported: format=%u extent=%ux%u mip=%u\n",
                     description.format, description.width, description.height, description.mipCount);
        return false;
    }
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {description.width, description.height, 1u};
    imageInfo.mipLevels = description.mipCount ? description.mipCount : 1u;
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(device, &imageInfo, nullptr, &outImage->image) != VK_SUCCESS) {
        std::fprintf(stderr, "FSR3.1.6 shared image vkCreateImage failed: format=%d\n", (int)format);
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, outImage->image, &requirements);
    VkMemoryAllocateInfo allocationInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocationInfo.allocationSize = requirements.size;
    allocationInfo.memoryTypeIndex = find_memory_type(
        physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocationInfo.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(device, &allocationInfo, nullptr, &outImage->memory) != VK_SUCCESS ||
        vkBindImageMemory(device, outImage->image, outImage->memory, 0u) != VK_SUCCESS) {
        std::fprintf(stderr, "FSR3.1.6 shared image allocation/bind failed: format=%d bits=%x\n",
                     (int)format, requirements.memoryTypeBits);
        return false;
    }
    outImage->allocationSize = requirements.size;
    FfxVkFsr3_3_1_5ImportedImageDescription imported{};
    imported.image = outImage->image;
    imported.format = format;
    imported.width = description.width;
    imported.height = description.height;
    imported.mipCount = imageInfo.mipLevels;
    imported.arrayLayers = 1u;
    imported.layout = VK_IMAGE_LAYOUT_GENERAL;
    imported.state = FFX_VK_FSR3_3_1_5_RESOURCE_STATE_UNORDERED_ACCESS;
    imported.usage = imageInfo.usage;
    outImage->imported = ffxVkFsr3_3_1_5BridgeImportImage(bridge, &imported);
    outImage->resource = ffxVkFsr3_3_1_5BridgeResolveResource(bridge, outImage->imported);
    if (!outImage->imported.resource || !outImage->resource.resource) {
        std::fprintf(stderr, "FSR3.1.6 shared image bridge import failed: format=%d extent=%ux%u\n",
                     (int)format, description.width, description.height);
        return false;
    }
    return true;
}

static void destroy_shared_image(VkDevice device, FfxVkFsr3_3_1_5Bridge* bridge,
                                 OwnedSharedImage* image)
{
    if (!image)
        return;
    if (bridge && image->imported.resource)
        ffxVkFsr3_3_1_5BridgeReleaseImportedImage(bridge, image->imported);
    if (image->memory)
        vkFreeMemory(device, image->memory, nullptr);
    if (image->image)
        vkDestroyImage(device, image->image, nullptr);
    *image = {};
}

static void initialize_backend(FfxInterface* backend, FfxVkFsr3_3_1_5Bridge* bridge)
{
    backend->device = bridge;
    backend->fpGetSDKVersion = ffxVkFsr3_3_1_5BridgeGetSDKVersion;
    backend->fpCreateBackendContext = ffxVkFsr3_3_1_5BridgeCreateBackendContext;
    backend->fpDestroyBackendContext = ffxVkFsr3_3_1_5BridgeDestroyBackendContext;
    backend->fpGetDeviceCapabilities = ffxVkFsr3_3_1_5BridgeGetDeviceCapabilities;
    backend->fpGetEffectGpuMemoryUsage = ffxVkFsr3_3_1_5BridgeGetEffectGpuMemoryUsage;
    backend->fpStageConstantBufferDataFunc = ffxVkFsr3_3_1_5BridgeStageConstantBufferData;
    backend->fpCreateResource = ffxVkFsr3_3_1_5BridgeCreateResource;
    backend->fpDestroyResource = ffxVkFsr3_3_1_5BridgeDestroyResource;
    backend->fpGetResource = ffxVkFsr3_3_1_5BridgeGetResource;
    backend->fpGetResourceDescription = ffxVkFsr3_3_1_5BridgeGetResourceDescription;
    backend->fpRegisterResource = ffxVkFsr3_3_1_5BridgeRegisterResource;
    backend->fpUnregisterResources = ffxVkFsr3_3_1_5BridgeUnregisterResources;
    backend->fpScheduleGpuJob = ffxVkFsr3_3_1_5BridgeScheduleGpuJob;
    backend->fpExecuteGpuJobs = ffxVkFsr3_3_1_5BridgeExecuteGpuJobs;
    backend->fpCreatePipeline = ffxVkFsr3_3_1_5BridgeCreatePipeline;
    backend->fpDestroyPipeline = ffxVkFsr3_3_1_5BridgeDestroyPipeline;
}

static bool valid_image(const FfxVkFsr3_3_1_6FrameGenerationImage& image,
                        VkFormat format, uint32_t width, uint32_t height, bool writable)
{
    const VkImageUsageFlags required = writable ?
        (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT) : VK_IMAGE_USAGE_SAMPLED_BIT;
    return image.image != VK_NULL_HANDLE && image.format == format &&
           image.width >= width && image.height >= height &&
           (image.usage & required) == required;
}

static bool valid_motion_image(const FfxVkFsr3_3_1_6FrameGenerationImage& image,
                               uint32_t width, uint32_t height)
{
    /* The SDK contract consumes XY motion. Q2RTX stores the same normalized
     * float XY pair in FLAT_MOTION.RG while retaining depth derivatives in BA;
     * sampled SPIR-V has no storage-image format restriction on this input. */
    return (image.format == VK_FORMAT_R16G16_SFLOAT ||
            image.format == VK_FORMAT_R16G16B16A16_SFLOAT) &&
           valid_image(image, image.format, width, height, false);
}

static bool finite_value(float value)
{
    return std::isfinite(value);
}

static bool finite_positive_value(float value)
{
    return finite_value(value) && value > 0.0f;
}

static bool finite_nonnegative_value(float value)
{
    return finite_value(value) && value >= 0.0f;
}

static bool finite_vector3(const float value[3])
{
    return finite_value(value[0]) && finite_value(value[1]) && finite_value(value[2]);
}

static FfxVkFsr3_3_1_5Resource import_frame_image(
    FfxVkFsr3_3_1_5Bridge* bridge,
    const FfxVkFsr3_3_1_6FrameGenerationImage& image, bool writable)
{
    FfxVkFsr3_3_1_5ImportedImageDescription description{};
    description.image = image.image;
    description.format = image.format;
    description.width = image.width;
    description.height = image.height;
    description.mipCount = 1u;
    description.arrayLayers = 1u;
    description.layout = image.layout;
    description.state = writable ? FFX_VK_FSR3_3_1_5_RESOURCE_STATE_UNORDERED_ACCESS :
                                   FFX_VK_FSR3_3_1_5_RESOURCE_STATE_COMPUTE_READ;
    description.usage = image.usage;
    return ffxVkFsr3_3_1_5BridgeImportImage(bridge, &description);
}

static void initialize_shared_image_layouts(
    VkCommandBuffer commandBuffer, const OwnedSharedImage* images, uint32_t count)
{
    VkImageMemoryBarrier barriers[5]{};
    for (uint32_t index = 0u; index < count; ++index) {
        barriers[index].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[index].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barriers[index].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[index].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[index].image = images[index].image;
        barriers[index].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[index].subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        barriers[index].subresourceRange.layerCount = 1u;
    }
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr,
                         0u, nullptr, count, barriers);
}

} // namespace

struct FfxVkFsr3_3_1_6FrameGenerationContext {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t maxRenderWidth = 0u;
    uint32_t maxRenderHeight = 0u;
    uint32_t displayWidth = 0u;
    uint32_t displayHeight = 0u;
    FfxVkFsr3_3_1_5Bridge* bridge = nullptr;
    FfxInterface backend{};
    FfxOpticalflowContext opticalFlow{};
    FfxFrameInterpolationContext interpolation{};
    bool opticalFlowCreated = false;
    bool interpolationCreated = false;
    bool initialized = false;
    bool prepared = false;
    bool dispatched = false;
    uint64_t frameId = 0u;
    uint32_t renderWidth = 0u;
    uint32_t renderHeight = 0u;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    VkImage preparedColorImage = VK_NULL_HANDLE;
    VkImage preparedDepthImage = VK_NULL_HANDLE;
    VkImage preparedMotionVectorImage = VK_NULL_HANDLE;
    FfxApiResource color{};
    OwnedSharedImage opticalFlowVector{};
    OwnedSharedImage opticalFlowScd{};
    OwnedSharedImage dilatedDepth{};
    OwnedSharedImage dilatedMotionVectors{};
    OwnedSharedImage reconstructedPreviousDepth{};
    std::vector<FfxVkFsr3_3_1_5Resource> pendingImports;
    struct RetainedFrame {
        uint64_t frameId = 0u;
        std::vector<FfxVkFsr3_3_1_5Resource> imports;
    };
    std::vector<RetainedFrame> retainedFrames;
};

extern "C" FfxVkFsr3_3_1_6FrameGenerationResult
ffxVkFsr3_3_1_6FrameGenerationContextCreate(
    const FfxVkFsr3_3_1_6FrameGenerationCreateInfo* createInfo,
    FfxVkFsr3_3_1_6FrameGenerationContext** outContext)
{
    if (!createInfo || !outContext || createInfo->physicalDevice == VK_NULL_HANDLE ||
        createInfo->device == VK_NULL_HANDLE || createInfo->maxRenderWidth == 0u ||
        createInfo->maxRenderHeight == 0u || createInfo->displayWidth == 0u ||
        createInfo->displayHeight == 0u ||
        to_ffx_format(createInfo->colorFormat) == FFX_API_SURFACE_FORMAT_UNKNOWN)
        return FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    *outContext = nullptr;
    FfxVkFsr3_3_1_6FrameGenerationContext* context =
        new (std::nothrow) FfxVkFsr3_3_1_6FrameGenerationContext;
    if (!context)
        return FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    context->physicalDevice = createInfo->physicalDevice;
    context->device = createInfo->device;
    context->maxRenderWidth = createInfo->maxRenderWidth;
    context->maxRenderHeight = createInfo->maxRenderHeight;
    context->displayWidth = createInfo->displayWidth;
    context->displayHeight = createInfo->displayHeight;
    context->colorFormat = createInfo->colorFormat;
    context->bridge = ffxVkFsr3_3_1_5CreateBridgeWithPhysicalDevice(
        context->physicalDevice, context->device, nullptr);
    if (!context->bridge) {
        delete context;
        return FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    }
    initialize_backend(&context->backend, context->bridge);
    FfxOpticalflowContextDescription opticalFlowDescription{};
    opticalFlowDescription.backendInterface = context->backend;
    opticalFlowDescription.resolution = {createInfo->displayWidth, createInfo->displayHeight};
    FfxErrorCode result = ffxOpticalflowContextCreate(&context->opticalFlow, &opticalFlowDescription);
    if (result == FFX_OK)
        context->opticalFlowCreated = true;
    FfxFrameInterpolationContextDescription interpolationDescription{};
    interpolationDescription.backendInterface = context->backend;
    interpolationDescription.maxRenderSize = {createInfo->maxRenderWidth, createInfo->maxRenderHeight};
    interpolationDescription.displaySize = {createInfo->displayWidth, createInfo->displayHeight};
    interpolationDescription.backBufferFormat = to_ffx_format(context->colorFormat);
    interpolationDescription.previousInterpolationSourceFormat = to_ffx_format(context->colorFormat);
    if (result == FFX_OK)
        result = ffxFrameInterpolationContextCreate(&context->interpolation, &interpolationDescription);
    if (result != FFX_OK)
        std::fprintf(stderr, "FSR3.1.6 public FI/OF scheduler context create failed (FFX %u)\n",
                     static_cast<unsigned>(result));
    if (result == FFX_OK)
        context->interpolationCreated = true;
    FfxOpticalflowSharedResourceDescriptions opticalFlowShared{};
    FfxFrameInterpolationSharedResourceDescriptions interpolationShared{};
    const char* failureStage = "create optical-flow context";
    if (result == FFX_OK)
        result = ffxOpticalflowGetSharedResourceDescriptions(&context->opticalFlow, &opticalFlowShared);
    failureStage = "allocate optical-flow vector";
    if (result == FFX_OK && !make_shared_image(context->physicalDevice, context->device, context->bridge,
                                                opticalFlowShared.opticalFlowVector.resourceDescription,
                                                &context->opticalFlowVector))
        result = static_cast<FfxErrorCode>(FFX_ERROR_OUT_OF_MEMORY);
    if (result == FFX_OK && !make_shared_image(context->physicalDevice, context->device, context->bridge,
                                                opticalFlowShared.opticalFlowSCD.resourceDescription,
                                                &context->opticalFlowScd))
        result = static_cast<FfxErrorCode>(FFX_ERROR_OUT_OF_MEMORY);
    failureStage = "query interpolation shared resources";
    if (result == FFX_OK)
        result = ffxFrameInterpolationGetSharedResourceDescriptions(&context->interpolation,
                                                                     &interpolationShared);
    if (result == FFX_OK && !make_shared_image(context->physicalDevice, context->device, context->bridge,
                                                interpolationShared.dilatedDepth.resourceDescription,
                                                &context->dilatedDepth))
        result = static_cast<FfxErrorCode>(FFX_ERROR_OUT_OF_MEMORY);
    failureStage = "allocate dilated motion vectors";
    if (result == FFX_OK && !make_shared_image(context->physicalDevice, context->device, context->bridge,
                                                interpolationShared.dilatedMotionVectors.resourceDescription,
                                                &context->dilatedMotionVectors))
        result = static_cast<FfxErrorCode>(FFX_ERROR_OUT_OF_MEMORY);
    if (result == FFX_OK && !make_shared_image(context->physicalDevice, context->device, context->bridge,
                                                interpolationShared.reconstructedPrevNearestDepth.resourceDescription,
                                                &context->reconstructedPreviousDepth))
        result = static_cast<FfxErrorCode>(FFX_ERROR_OUT_OF_MEMORY);
    if (result != FFX_OK) {
        std::fprintf(stderr, "FSR3.1.6 FI/OF public context failed at %s (FFX %u)\n",
                     failureStage, static_cast<unsigned>(result));
        ffxVkFsr3_3_1_6FrameGenerationContextDestroy(context);
        return result_from_ffx(result);
    }
    *outContext = context;
    return FFX_VK_FSR3_3_1_6_FRAMEGEN_OK;
}

extern "C" FfxVkFsr3_3_1_6FrameGenerationResult
ffxVkFsr3_3_1_6FrameGenerationContextGetMemoryUsage(
    FfxVkFsr3_3_1_6FrameGenerationContext* context,
    FfxVkFsr3_3_1_6FrameGenerationMemoryUsage* outUsage)
{
    if (!context || !outUsage || !context->opticalFlowCreated ||
        !context->interpolationCreated || !context->backend.fpGetEffectGpuMemoryUsage)
        return FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    FfxApiEffectMemoryUsage bridgeUsage{};
    const FfxErrorCode result = context->backend.fpGetEffectGpuMemoryUsage(
        &context->backend, 0u, &bridgeUsage);
    if (result != FFX_OK)
        return result_from_ffx(result);

    const uint64_t sharedUsage = context->opticalFlowVector.allocationSize +
        context->opticalFlowScd.allocationSize + context->dilatedDepth.allocationSize +
        context->dilatedMotionVectors.allocationSize +
        context->reconstructedPreviousDepth.allocationSize;
    outUsage->totalUsageInBytes = bridgeUsage.totalUsageInBytes + sharedUsage;
    outUsage->aliasableUsageInBytes = bridgeUsage.aliasableUsageInBytes;
    return FFX_VK_FSR3_3_1_6_FRAMEGEN_OK;
}

extern "C" FfxVkFsr3_3_1_6FrameGenerationResult
ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(
    FfxVkFsr3_3_1_6FrameGenerationContext* context,
    const FfxVkFsr3_3_1_6FrameGenerationPrepareInfo* info)
{
    if (!context || !info || context->prepared ||
        info->commandBuffer == VK_NULL_HANDLE || info->renderWidth == 0u || info->renderHeight == 0u ||
        info->renderWidth > context->maxRenderWidth ||
        info->renderHeight > context->maxRenderHeight ||
        !finite_value(info->jitterOffsetX) || !finite_value(info->jitterOffsetY) ||
        !finite_value(info->motionVectorScaleX) || !finite_value(info->motionVectorScaleY) ||
        !finite_positive_value(info->frameTimeMilliseconds) ||
        !finite_positive_value(info->cameraNear) ||
        !finite_positive_value(info->cameraFar) || info->cameraFar <= info->cameraNear ||
        !finite_positive_value(info->viewSpaceToMeters) ||
        !finite_positive_value(info->cameraVerticalFovRadians) ||
        !finite_nonnegative_value(info->minLuminance) ||
        !finite_value(info->maxLuminance) || info->maxLuminance < info->minLuminance ||
        !finite_vector3(info->cameraPosition) || !finite_vector3(info->cameraUp) ||
        !finite_vector3(info->cameraRight) || !finite_vector3(info->cameraForward) ||
        info->transferFunction < FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB ||
        info->transferFunction > FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SCRGB ||
        !valid_image(info->color, context->colorFormat,
                     context->displayWidth, context->displayHeight, false) ||
        !valid_image(info->depth, VK_FORMAT_R32_SFLOAT, info->renderWidth, info->renderHeight, false) ||
        !valid_motion_image(info->motionVectors, info->renderWidth, info->renderHeight))
        return FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    // sotc: tag this frame's constant buffers and descriptor sets (the first-use layout jobs
    // below included) with its id so ContextRetireFrame can recycle/free them; frame id 0 is
    // never retired by the bridge.
    if (info->frameId == 0u)
        return FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    ffxVkFsr3_3_1_5BridgeBeginFrame(context->bridge, info->frameId);
    if (!context->initialized) {
        const OwnedSharedImage shared[] = {
            context->opticalFlowVector, context->opticalFlowScd, context->dilatedDepth,
            context->dilatedMotionVectors, context->reconstructedPreviousDepth};
        initialize_shared_image_layouts(info->commandBuffer, shared, 5u);
        const FfxErrorCode initResult = ffxVkFsr3_3_1_5BridgeExecuteGpuJobs(
            &context->backend, reinterpret_cast<FfxCommandList>(info->commandBuffer), 1u);
        if (initResult != FFX_OK)
            return result_from_ffx(initResult);
        context->initialized = true;
    }
    const FfxVkFsr3_3_1_5Resource color = import_frame_image(context->bridge, info->color, false);
    const FfxVkFsr3_3_1_5Resource depth = import_frame_image(context->bridge, info->depth, false);
    const FfxVkFsr3_3_1_5Resource motion = import_frame_image(context->bridge, info->motionVectors, false);
    context->color = ffxVkFsr3_3_1_5BridgeResolveResource(context->bridge, color);
    const FfxApiResource depthResource = ffxVkFsr3_3_1_5BridgeResolveResource(context->bridge, depth);
    const FfxApiResource motionResource = ffxVkFsr3_3_1_5BridgeResolveResource(context->bridge, motion);
    if (!color.resource || !depthResource.resource || !motionResource.resource) {
        ffxVkFsr3_3_1_5BridgeReleaseImportedImage(context->bridge, color);
        ffxVkFsr3_3_1_5BridgeReleaseImportedImage(context->bridge, depth);
        ffxVkFsr3_3_1_5BridgeReleaseImportedImage(context->bridge, motion);
        return FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    }
    context->pendingImports = {color, depth, motion};
    /* From this point a scheduler error may follow partially recorded Vulkan
     * commands.  Retain every imported view until the caller retires the
     * submitted/abandoned command buffer after its fence, rather than freeing
     * a descriptor target underneath those commands. */
    context->prepared = true;
    context->frameId = info->frameId;
    context->renderWidth = info->renderWidth;
    context->renderHeight = info->renderHeight;
    context->preparedColorImage = info->color.image;
    context->preparedDepthImage = info->depth.image;
    context->preparedMotionVectorImage = info->motionVectors.image;
    FfxOpticalflowDispatchDescription flow{};
    flow.commandList = reinterpret_cast<FfxCommandList>(info->commandBuffer);
    flow.color = context->color;
    flow.opticalFlowVector = context->opticalFlowVector.resource;
    flow.opticalFlowSCD = context->opticalFlowScd.resource;
    flow.reset = info->reset == VK_TRUE;
    flow.backbufferTransferFunction = static_cast<int>(info->transferFunction);
    flow.minMaxLuminance = {info->minLuminance, info->maxLuminance};
    FfxErrorCode result = ffxOpticalflowContextDispatch(&context->opticalFlow, &flow);
    FfxFrameInterpolationPrepareDescription prepare{};
    if (result == FFX_OK) {
        prepare.commandList = reinterpret_cast<FfxCommandList>(info->commandBuffer);
        prepare.renderSize = {info->renderWidth, info->renderHeight};
        prepare.depth = depthResource;
        prepare.motionVectors = motionResource;
        prepare.jitterOffset = {info->jitterOffsetX, info->jitterOffsetY};
        prepare.motionVectorScale = {info->motionVectorScaleX, info->motionVectorScaleY};
        prepare.frameTimeDelta = info->frameTimeMilliseconds;
        prepare.cameraNear = info->cameraNear;
        prepare.cameraFar = info->cameraFar;
        prepare.viewSpaceToMetersFactor = info->viewSpaceToMeters;
        prepare.cameraFovAngleVertical = info->cameraVerticalFovRadians;
        prepare.frameID = info->frameId;
        prepare.dilatedDepth = context->dilatedDepth.resource;
        prepare.dilatedMotionVectors = context->dilatedMotionVectors.resource;
        prepare.reconstructedPrevDepth = context->reconstructedPreviousDepth.resource;
        std::memcpy(prepare.cameraPosition, info->cameraPosition, sizeof(prepare.cameraPosition));
        std::memcpy(prepare.cameraUp, info->cameraUp, sizeof(prepare.cameraUp));
        std::memcpy(prepare.cameraRight, info->cameraRight, sizeof(prepare.cameraRight));
        std::memcpy(prepare.cameraForward, info->cameraForward, sizeof(prepare.cameraForward));
        result = ffxFrameInterpolationPrepare(&context->interpolation, &prepare);
    }
    if (result != FFX_OK)
        return result_from_ffx(result);
    return FFX_VK_FSR3_3_1_6_FRAMEGEN_OK;
}

extern "C" FfxVkFsr3_3_1_6FrameGenerationResult
ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(
    FfxVkFsr3_3_1_6FrameGenerationContext* context,
    const FfxVkFsr3_3_1_6FrameGenerationDispatchInfo* info)
{
    if (!context || !info || !context->prepared || context->dispatched ||
        info->commandBuffer == VK_NULL_HANDLE || info->frameId != context->frameId ||
        info->displayWidth != context->displayWidth ||
        info->displayHeight != context->displayHeight ||
        !finite_positive_value(info->frameTimeMilliseconds) ||
        !finite_positive_value(info->cameraNear) ||
        !finite_positive_value(info->cameraFar) || info->cameraFar <= info->cameraNear ||
        !finite_positive_value(info->viewSpaceToMeters) ||
        !finite_positive_value(info->cameraVerticalFovRadians) ||
        !finite_nonnegative_value(info->minLuminance) ||
        !finite_value(info->maxLuminance) || info->maxLuminance < info->minLuminance ||
        info->transferFunction < FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB ||
        info->transferFunction > FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SCRGB ||
        !valid_image(info->color, context->colorFormat, info->displayWidth, info->displayHeight, false) ||
        !valid_image(info->output, context->colorFormat, info->displayWidth, info->displayHeight, true) ||
        (info->distortionField.image != VK_NULL_HANDLE &&
         !valid_image(info->distortionField, VK_FORMAT_R16G16_SFLOAT, 1u, 1u, false)))
        return FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    const bool defaultInterpolationRect =
        info->interpolationX == 0u && info->interpolationY == 0u &&
        info->interpolationWidth == 0u && info->interpolationHeight == 0u;
    if (!defaultInterpolationRect &&
        (info->interpolationWidth == 0u || info->interpolationHeight == 0u ||
         info->interpolationX >= info->displayWidth ||
         info->interpolationY >= info->displayHeight ||
         info->interpolationWidth > info->displayWidth - info->interpolationX ||
         info->interpolationHeight > info->displayHeight - info->interpolationY))
        return FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    if (info->color.image != ffxVkFsr3_3_1_5BridgeGetNativeImage(context->bridge, context->color.resource))
        return FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    /* FI reads the current scene, depth, and motion while it writes the
     * generated scene. A host must supply a distinct output allocation: image
     * metadata alone cannot make a read/write alias safe, and doing so can
     * overwrite another temporal provider's history. */
    if (info->output.image == context->preparedColorImage ||
        info->output.image == context->preparedDepthImage ||
        info->output.image == context->preparedMotionVectorImage ||
        (info->distortionField.image != VK_NULL_HANDLE &&
         info->output.image == info->distortionField.image))
        return FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    const FfxVkFsr3_3_1_5Resource output = import_frame_image(context->bridge, info->output, true);
    const FfxApiResource outputResource = ffxVkFsr3_3_1_5BridgeResolveResource(context->bridge, output);
    if (!outputResource.resource) {
        ffxVkFsr3_3_1_5BridgeReleaseImportedImage(context->bridge, output);
        return FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    }
    FfxVkFsr3_3_1_5Resource distortion{};
    FfxApiResource distortionResource{};
    if (info->distortionField.image != VK_NULL_HANDLE) {
        distortion = import_frame_image(context->bridge, info->distortionField, false);
        distortionResource = ffxVkFsr3_3_1_5BridgeResolveResource(context->bridge, distortion);
        if (!distortionResource.resource) {
            ffxVkFsr3_3_1_5BridgeReleaseImportedImage(context->bridge, distortion);
            ffxVkFsr3_3_1_5BridgeReleaseImportedImage(context->bridge, output);
            return FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_OUT_OF_MEMORY;
        }
    }
    context->pendingImports.push_back(output);
    if (distortion.resource)
        context->pendingImports.push_back(distortion);
    FfxFrameInterpolationDispatchDescription dispatch{};
    dispatch.commandList = reinterpret_cast<FfxCommandList>(info->commandBuffer);
    dispatch.displaySize = {info->displayWidth, info->displayHeight};
    dispatch.renderSize = {context->renderWidth, context->renderHeight};
    dispatch.currentBackBuffer = context->color;
    dispatch.output = outputResource;
    dispatch.interpolationRect = {static_cast<int32_t>(info->interpolationX),
                                  static_cast<int32_t>(info->interpolationY),
                                  static_cast<int32_t>(info->interpolationWidth),
                                  static_cast<int32_t>(info->interpolationHeight)};
    if (defaultInterpolationRect) {
        dispatch.interpolationRect.width = static_cast<int32_t>(info->displayWidth);
        dispatch.interpolationRect.height = static_cast<int32_t>(info->displayHeight);
    }
    dispatch.opticalFlowVector = context->opticalFlowVector.resource;
    dispatch.opticalFlowSceneChangeDetection = context->opticalFlowScd.resource;
    dispatch.opticalFlowBufferSize = {context->opticalFlowVector.resource.description.width,
                                      context->opticalFlowVector.resource.description.height};
    /* The public provider normalizes the optical-flow field by the display
     * dimensions.  The FI shaders recover the field extent from this scale;
     * passing 1.0 here collapses that calculation and produces black frames. */
    dispatch.opticalFlowScale = {1.0f / static_cast<float>(info->displayWidth),
                                 1.0f / static_cast<float>(info->displayHeight)};
    dispatch.opticalFlowBlockSize = 8u;
    dispatch.frameTimeDelta = info->frameTimeMilliseconds;
    dispatch.reset = info->reset == VK_TRUE;
    dispatch.cameraNear = info->cameraNear;
    dispatch.cameraFar = info->cameraFar;
    dispatch.cameraFovAngleVertical = info->cameraVerticalFovRadians;
    dispatch.viewSpaceToMetersFactor = info->viewSpaceToMeters;
    dispatch.backBufferTransferFunction = static_cast<FfxApiBackbufferTransferFunction>(info->transferFunction);
    dispatch.minMaxLuminance[0] = info->minLuminance;
    dispatch.minMaxLuminance[1] = info->maxLuminance;
    dispatch.frameID = info->frameId;
    dispatch.dilatedDepth = context->dilatedDepth.resource;
    dispatch.dilatedMotionVectors = context->dilatedMotionVectors.resource;
    dispatch.reconstructedPrevDepth = context->reconstructedPreviousDepth.resource;
    dispatch.distortionField = distortionResource;
    const FfxErrorCode result = ffxFrameInterpolationDispatch(&context->interpolation, &dispatch);
    if (result != FFX_OK)
        return result_from_ffx(result);
    context->retainedFrames.push_back({context->frameId, std::move(context->pendingImports)});
    context->color = {};
    context->prepared = false;
    context->dispatched = false;
    context->renderWidth = 0u;
    context->renderHeight = 0u;
    return FFX_VK_FSR3_3_1_6_FRAMEGEN_OK;
}

extern "C" FfxVkFsr3_3_1_6FrameGenerationResult
ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(
    FfxVkFsr3_3_1_6FrameGenerationContext* context, uint64_t completedFrameId)
{
    if (!context)
        return FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    if (context->prepared && context->frameId <= completedFrameId) {
        for (const FfxVkFsr3_3_1_5Resource resource : context->pendingImports)
            ffxVkFsr3_3_1_5BridgeReleaseImportedImage(context->bridge, resource);
        context->pendingImports.clear();
        context->color = {};
        context->prepared = false;
        context->dispatched = false;
        context->renderWidth = 0u;
        context->renderHeight = 0u;
    }
    auto retained = context->retainedFrames.begin();
    while (retained != context->retainedFrames.end()) {
        if (retained->frameId <= completedFrameId) {
            for (const FfxVkFsr3_3_1_5Resource resource : retained->imports)
                ffxVkFsr3_3_1_5BridgeReleaseImportedImage(context->bridge, resource);
            retained = context->retainedFrames.erase(retained);
        } else {
            ++retained;
        }
    }
    // sotc: completed constant buffers go back to the bridge free list, descriptor pools die.
    ffxVkFsr3_3_1_5BridgeRetireFrame(context->bridge, completedFrameId);
    return FFX_VK_FSR3_3_1_6_FRAMEGEN_OK;
}

extern "C" void ffxVkFsr3_3_1_6FrameGenerationContextDestroy(
    FfxVkFsr3_3_1_6FrameGenerationContext* context)
{
    if (!context)
        return;
    for (const FfxVkFsr3_3_1_5Resource resource : context->pendingImports)
        ffxVkFsr3_3_1_5BridgeReleaseImportedImage(context->bridge, resource);
    for (const FfxVkFsr3_3_1_6FrameGenerationContext::RetainedFrame& frame :
         context->retainedFrames) {
        for (const FfxVkFsr3_3_1_5Resource resource : frame.imports)
            ffxVkFsr3_3_1_5BridgeReleaseImportedImage(context->bridge, resource);
    }
    if (context->interpolationCreated)
        ffxFrameInterpolationContextDestroy(&context->interpolation);
    if (context->opticalFlowCreated)
        ffxOpticalflowContextDestroy(&context->opticalFlow);
    destroy_shared_image(context->device, context->bridge, &context->opticalFlowVector);
    destroy_shared_image(context->device, context->bridge, &context->opticalFlowScd);
    destroy_shared_image(context->device, context->bridge, &context->dilatedDepth);
    destroy_shared_image(context->device, context->bridge, &context->dilatedMotionVectors);
    destroy_shared_image(context->device, context->bridge, &context->reconstructedPreviousDepth);
    ffxVkFsr3_3_1_5DestroyBridge(context->bridge);
    delete context;
}
