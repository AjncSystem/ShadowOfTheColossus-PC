/*
 * Copyright (c) 2026 Q2RTX FSR Vulkan contributors
 * SPDX-License-Identifier: MIT
 *
 * Pipeline callbacks for the public SDK 2.3 FSR3.1.5 host scheduler.  The
 * scheduler owns FfxPipelineState; this bridge owns the corresponding Vulkan
 * objects and reflects the actual SPIR-V bindings back into that state so the
 * host's resource-name patch-up remains authoritative.
 *
 * sotc: local copy of gpu/third_party/fsr-vulkan/src/ffx_vk_fsr3_3_1_5_bridge.cpp (c64f093),
 * swapped into the 3.1.6 frame generation target by gpu/CMakeLists.txt. Main change: staged
 * constant buffers are persistently mapped and recycled after RetireFrame instead of one
 * vkCreateBuffer + vkAllocateMemory + map per constant buffer (12 per frame), which would be
 * hundreds of WDDM allocations per second next to the GPU threads. Also: compute pipelines can
 * be pinned to a 32-wide subgroup (ffxVkSotcSetRequiredSubgroupSize, for AMD). Edits are
 * marked "sotc:".
 */

#include "ffx_vk_fsr3_3_1_5_bridge.h"
#include "ffx_vk_fsr3_3_1_5_bundle.h"
#include "ffx_vk_fsr3_3_1_5_descriptor.h"
#include "ffx_vk_fsr3_3_1_5_pipeline.h"

#include "ffx_interface.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <iterator>
#include <vector>

// sotc: required compute subgroup size for every pipeline this bridge creates (0 = driver
// default). Set by the host before the frame generation context is created, from
// Vulkan::Instance::FrameGenSubgroupSize().
static uint32_t g_sotcRequiredSubgroupSize = 0u;
extern "C" void ffxVkSotcSetRequiredSubgroupSize(uint32_t size)
{
    g_sotcRequiredSubgroupSize = size;
}

// sotc: live constant buffers (one VkDeviceMemory each, free list included) and descriptor sets
// (one VkDescriptorPool each), over every bridge. tools/fg_bench.cpp checks they stay flat.
static std::atomic<uint32_t> g_sotcLiveConstantBuffers{0u};
static std::atomic<uint32_t> g_sotcLiveDescriptorSets{0u};
extern "C" void ffxVkSotcBridgeLiveObjects(uint32_t* constantBuffers, uint32_t* descriptorSets)
{
    *constantBuffers = g_sotcLiveConstantBuffers.load();
    *descriptorSets = g_sotcLiveDescriptorSets.load();
}

namespace {

struct BridgeResource {
    FfxApiResourceDescription description{};
    VkImage image = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    /* Exact allocation size returned by Vulkan.  This makes the SDK's memory
     * query describe the bridge's actual independent allocations instead of
     * estimating texel footprints. */
    VkDeviceSize allocationSize = 0u;
    VkImageView srvView = VK_NULL_HANDLE;
    std::vector<VkImageView> uavViews;
    VkMemoryPropertyFlags memoryProperties = 0u;
    FfxApiResourceState currentState = FFX_API_RESOURCE_STATE_COMMON;
    /* Vulkan tracks layouts independently from the SDK's abstract state.
     * Newly-created optimal images start undefined even when the host's first
     * requested state is COMPUTE_READ or UNORDERED_ACCESS. */
    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout restoreLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    FfxApiResourceState restoreState = FFX_API_RESOURCE_STATE_COMMON;
    bool owned = true;
    bool imported = false;
};

struct BridgeConstantBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0u;
    uint64_t frameId = 0u;
    // sotc: recycling state; the buffer holds `capacity` bytes and stays mapped.
    VkDeviceSize capacity = 0u;
    void* mapped = nullptr;
    bool coherent = true;
};

struct BridgeDescriptorSet {
    FfxVkFsr3_3_1_5DescriptorSet descriptorSet{};
    uint64_t frameId = 0u;
};

} // namespace

struct FfxVkFsr3_3_1_5Bridge {
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    const VkAllocationCallbacks* allocationCallbacks;
    std::mutex mutex;
    std::vector<std::unique_ptr<BridgeResource>> resources;
    std::vector<std::unique_ptr<BridgeConstantBuffer>> constantBuffers;
    // sotc: retired constant buffers, reused by later StageConstantBufferData calls.
    std::vector<std::unique_ptr<BridgeConstantBuffer>> freeConstantBuffers;
    /* Retain descriptors and staged constants until the embedding application
     * proves the command buffer complete.  The next public lifecycle layer
     * will recycle these against a caller fence; retaining is deliberately
     * conservative and makes this first native graph recorder GPU-safe. */
    std::vector<std::unique_ptr<BridgeDescriptorSet>> descriptorSets;
    std::vector<FfxGpuJobDescription> jobs;
    std::vector<int32_t> registeredImportedResources;
    uint64_t activeFrameId = 0u;

    FfxVkFsr3_3_1_5Bridge(VkPhysicalDevice inPhysicalDevice,
                           VkDevice inDevice,
                           const VkAllocationCallbacks* inAllocationCallbacks)
        : physicalDevice(inPhysicalDevice),
          device(inDevice),
          allocationCallbacks(inAllocationCallbacks) {}
};

extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeDestroyResource(
    FfxInterface* backend, FfxResourceInternal resource, FfxUInt32 effectContextId);

namespace {

struct BridgePipeline {
    FfxVkFsr3_3_1_5Pipeline pipeline{};
};

static VkFormat to_vk_format(uint32_t format)
{
    switch (format) {
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
    case FFX_API_SURFACE_FORMAT_R16G16B16A16_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R32G32_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R32G32_UINT: return VK_FORMAT_R32G32_UINT;
    case FFX_API_SURFACE_FORMAT_R16G16_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R16G16_UINT: return VK_FORMAT_R16G16_UINT;
    case FFX_API_SURFACE_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
    case FFX_API_SURFACE_FORMAT_R16_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R16_UINT: return VK_FORMAT_R16_UINT;
    case FFX_API_SURFACE_FORMAT_R16_UNORM: return VK_FORMAT_R16_UNORM;
    case FFX_API_SURFACE_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
    case FFX_API_SURFACE_FORMAT_R8_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
    case FFX_API_SURFACE_FORMAT_R8_SNORM: return VK_FORMAT_R8_SNORM;
    case FFX_API_SURFACE_FORMAT_R8_UINT: return VK_FORMAT_R8_UINT;
    case FFX_API_SURFACE_FORMAT_R8G8_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
    case FFX_API_SURFACE_FORMAT_R8G8_UINT: return VK_FORMAT_R8G8_UINT;
    case FFX_API_SURFACE_FORMAT_R32_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM:
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static FfxApiSurfaceFormat from_vk_format(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R32G32B32A32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT;
    case VK_FORMAT_R32G32B32A32_UINT: return FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R32G32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32G32_FLOAT;
    case VK_FORMAT_R32G32_UINT: return FFX_API_SURFACE_FORMAT_R32G32_UINT;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R16G16_UINT: return FFX_API_SURFACE_FORMAT_R16G16_UINT;
    case VK_FORMAT_R16G16_SINT: return FFX_API_SURFACE_FORMAT_R16G16_SINT;
    case VK_FORMAT_R16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16_FLOAT;
    case VK_FORMAT_R16_UINT: return FFX_API_SURFACE_FORMAT_R16_UINT;
    case VK_FORMAT_R16_UNORM: return FFX_API_SURFACE_FORMAT_R16_UNORM;
    case VK_FORMAT_R16_SNORM: return FFX_API_SURFACE_FORMAT_R16_SNORM;
    case VK_FORMAT_R8_UNORM: return FFX_API_SURFACE_FORMAT_R8_UNORM;
    case VK_FORMAT_R8_SNORM: return FFX_API_SURFACE_FORMAT_R8_SNORM;
    case VK_FORMAT_R8_UINT: return FFX_API_SURFACE_FORMAT_R8_UINT;
    case VK_FORMAT_R8G8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8_UNORM;
    case VK_FORMAT_R8G8_UINT: return FFX_API_SURFACE_FORMAT_R8G8_UINT;
    case VK_FORMAT_R32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case VK_FORMAT_R32_UINT: return FFX_API_SURFACE_FORMAT_R32_UINT;
    case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

extern "C" FfxVersionNumber ffxVkFsr3_3_1_5BridgeGetSDKVersion(FfxInterface*)
{
    return FFX_SDK_MAKE_VERSION(2, 3, 0);
}

static uint32_t full_mip_count(uint32_t width, uint32_t height)
{
    uint32_t result = 1u;
    while (width > 1u || height > 1u) {
        width = width > 1u ? width >> 1u : 1u;
        height = height > 1u ? height >> 1u : 1u;
        ++result;
    }
    return result;
}

static uint32_t find_memory_type(VkPhysicalDevice physicalDevice, uint32_t typeBits,
                                 VkMemoryPropertyFlags required)
{
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((typeBits & (1u << index)) &&
            (properties.memoryTypes[index].propertyFlags & required) == required)
            return index;
    }
    return UINT32_MAX;
}

static void destroy_resource(FfxVkFsr3_3_1_5Bridge* bridge, BridgeResource* resource)
{
    if (!bridge || !resource)
        return;
    for (VkImageView view : resource->uavViews) {
        if (view != VK_NULL_HANDLE)
            vkDestroyImageView(bridge->device, view, bridge->allocationCallbacks);
    }
    if (resource->srvView != VK_NULL_HANDLE)
        vkDestroyImageView(bridge->device, resource->srvView, bridge->allocationCallbacks);
    if (resource->owned) {
        if (resource->image != VK_NULL_HANDLE)
            vkDestroyImage(bridge->device, resource->image, bridge->allocationCallbacks);
        if (resource->buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(bridge->device, resource->buffer, bridge->allocationCallbacks);
        if (resource->memory != VK_NULL_HANDLE)
            vkFreeMemory(bridge->device, resource->memory, bridge->allocationCallbacks);
    }
}

static bool create_image_views(FfxVkFsr3_3_1_5Bridge* bridge, BridgeResource* resource)
{
    if (!bridge || !resource || resource->image == VK_NULL_HANDLE)
        return false;
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = resource->image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = to_vk_format(resource->description.format);
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = resource->description.mipCount;
    viewInfo.subresourceRange.layerCount = resource->description.depth ? resource->description.depth : 1u;
    if (viewInfo.format == VK_FORMAT_UNDEFINED ||
        vkCreateImageView(bridge->device, &viewInfo, bridge->allocationCallbacks,
                          &resource->srvView) != VK_SUCCESS)
        return false;
    if (resource->description.usage & FFX_API_RESOURCE_USAGE_UAV) {
        resource->uavViews.resize(resource->description.mipCount, VK_NULL_HANDLE);
        for (uint32_t mip = 0; mip < resource->description.mipCount; ++mip) {
            viewInfo.subresourceRange.baseMipLevel = mip;
            viewInfo.subresourceRange.levelCount = 1u;
            if (vkCreateImageView(bridge->device, &viewInfo, bridge->allocationCallbacks,
                                  &resource->uavViews[mip]) != VK_SUCCESS)
                return false;
        }
    }
    return true;
}

static void destroy_constant_buffer(FfxVkFsr3_3_1_5Bridge* bridge,
                                    BridgeConstantBuffer* constantBuffer)
{
    if (!bridge || !constantBuffer)
        return;
    if (constantBuffer->buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(bridge->device, constantBuffer->buffer, bridge->allocationCallbacks);
    if (constantBuffer->memory != VK_NULL_HANDLE)
        vkFreeMemory(bridge->device, constantBuffer->memory, bridge->allocationCallbacks);
    if (constantBuffer->capacity) // sotc: counted once fully created
        --g_sotcLiveConstantBuffers;
}

static void destroy_descriptor_set(BridgeDescriptorSet* descriptorSet)
{
    if (descriptorSet) {
        ffxVkFsr3_3_1_5DestroyDescriptorSet(&descriptorSet->descriptorSet);
        --g_sotcLiveDescriptorSets; // sotc
    }
}

static int32_t allocate_resource_slot(FfxVkFsr3_3_1_5Bridge* bridge,
                                      std::unique_ptr<BridgeResource> resource)
{
    std::lock_guard<std::mutex> lock(bridge->mutex);
    for (size_t index = 0; index < bridge->resources.size(); ++index) {
        if (!bridge->resources[index]) {
            bridge->resources[index] = std::move(resource);
            return static_cast<int32_t>(index + 1u);
        }
    }
    bridge->resources.emplace_back(std::move(resource));
    return static_cast<int32_t>(bridge->resources.size());
}

static BridgeResource* lookup_resource(FfxVkFsr3_3_1_5Bridge* bridge, int32_t index)
{
    if (!bridge || index <= 0)
        return nullptr;
    const size_t slot = static_cast<size_t>(index - 1);
    std::lock_guard<std::mutex> lock(bridge->mutex);
    return slot < bridge->resources.size() ? bridge->resources[slot].get() : nullptr;
}

static int32_t lookup_resource_index(FfxVkFsr3_3_1_5Bridge* bridge, const void* resource)
{
    if (!bridge || !resource)
        return 0;
    std::lock_guard<std::mutex> lock(bridge->mutex);
    for (size_t index = 0; index < bridge->resources.size(); ++index) {
        if (bridge->resources[index].get() == resource)
            return static_cast<int32_t>(index + 1u);
    }
    return 0;
}

static BridgeConstantBuffer* lookup_constant_buffer(FfxVkFsr3_3_1_5Bridge* bridge,
                                                     const uint32_t* token)
{
    if (!bridge || !token)
        return nullptr;
    std::lock_guard<std::mutex> lock(bridge->mutex);
    for (const std::unique_ptr<BridgeConstantBuffer>& buffer : bridge->constantBuffers) {
        if (reinterpret_cast<const uint32_t*>(buffer.get()) == token)
            return buffer.get();
    }
    return nullptr;
}

static VkImageLayout image_layout(FfxApiResourceState state)
{
    if (state & FFX_API_RESOURCE_STATE_UNORDERED_ACCESS)
        return VK_IMAGE_LAYOUT_GENERAL;
    if (state & FFX_API_RESOURCE_STATE_COPY_DEST)
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (state & FFX_API_RESOURCE_STATE_COPY_SRC)
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

static void transition_image(VkCommandBuffer commandBuffer, BridgeResource* resource,
                             VkImageLayout oldLayout, VkImageLayout newLayout,
                             VkPipelineStageFlags sourceStage,
                             VkPipelineStageFlags destinationStage,
                             VkAccessFlags sourceAccess,
                             VkAccessFlags destinationAccess)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.image = resource->image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = resource->description.mipCount;
    barrier.subresourceRange.layerCount = resource->description.depth ? resource->description.depth : 1u;
    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0u,
                         0u, nullptr, 0u, nullptr, 1u, &barrier);
}

static bool ensure_image_layout(VkCommandBuffer commandBuffer, BridgeResource* resource,
                                VkImageLayout desiredLayout)
{
    if (!resource || resource->image == VK_NULL_HANDLE)
        return false;
    if (resource->imageLayout == desiredLayout)
        return true;
    const VkPipelineStageFlags sourceStage = resource->imageLayout == VK_IMAGE_LAYOUT_UNDEFINED ?
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT :
        (resource->imageLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
         resource->imageLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ?
             VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    const VkAccessFlags sourceAccess = resource->imageLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0u :
        (resource->imageLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_TRANSFER_WRITE_BIT :
         resource->imageLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ? VK_ACCESS_TRANSFER_READ_BIT :
         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    const VkAccessFlags destinationAccess = desiredLayout == VK_IMAGE_LAYOUT_GENERAL ?
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT :
        desiredLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_TRANSFER_WRITE_BIT :
        desiredLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ? VK_ACCESS_TRANSFER_READ_BIT :
        VK_ACCESS_SHADER_READ_BIT;
    const VkPipelineStageFlags destinationStage =
        desiredLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
        desiredLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ? VK_PIPELINE_STAGE_TRANSFER_BIT :
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    transition_image(commandBuffer, resource, resource->imageLayout, desiredLayout,
                     sourceStage, destinationStage, sourceAccess, destinationAccess);
    resource->imageLayout = desiredLayout;
    return true;
}

static bool record_compute_job(FfxVkFsr3_3_1_5Bridge* bridge,
                               VkCommandBuffer commandBuffer,
                               const FfxComputeJobDescription& job)
{
    if (!bridge || !job.pipeline || !job.pipeline->pipeline)
        return false;
    BridgePipeline* pipeline = static_cast<BridgePipeline*>(job.pipeline->pipeline);
    if (!pipeline || pipeline->pipeline.pipeline == VK_NULL_HANDLE)
        return false;

    FfxVkFsr3_3_1_5DescriptorResource descriptorResources[
        FFX_VK_FSR3_3_1_5_MAX_PIPELINE_BINDINGS]{};
    uint32_t descriptorCount = 0u;
    uint32_t srvIndex = 0u;
    uint32_t uavIndex = 0u;
    uint32_t srvBufferIndex = 0u;
    uint32_t uavBufferIndex = 0u;
    uint32_t cbIndex = 0u;
    for (uint32_t index = 0; index < pipeline->pipeline.bindingCount; ++index) {
        const FfxVkFsr3_3_1_5DescriptorBinding& binding = pipeline->pipeline.bindings[index];
        FfxVkFsr3_3_1_5DescriptorResource& descriptor = descriptorResources[descriptorCount];
        descriptor.name = binding.name;
        switch (binding.descriptorClass) {
        case FFX_VK_FSR3_3_1_5_DESCRIPTOR_SRV: {
            if (srvIndex >= job.pipeline->srvTextureCount)
                return false;
            BridgeResource* resource = lookup_resource(bridge,
                job.srvTextures[srvIndex++].resource.internalIndex);
            if (!resource || !ensure_image_layout(commandBuffer, resource,
                                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
                return false;
            descriptor.imageView = resource->srvView;
            descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ++descriptorCount;
            break;
        }
        case FFX_VK_FSR3_3_1_5_DESCRIPTOR_UAV: {
            if (uavIndex >= job.pipeline->uavTextureCount)
                return false;
            const FfxTextureUAV& uav = job.uavTextures[uavIndex++];
            BridgeResource* resource = lookup_resource(bridge, uav.resource.internalIndex);
            if (!resource || resource->uavViews.empty() ||
                !ensure_image_layout(commandBuffer, resource, VK_IMAGE_LAYOUT_GENERAL))
                return false;
            /* FI shaders statically declare its full 13-level SPD pyramid,
             * while a smaller display allocation has fewer legal Vulkan mips.
             * The SDK's dispatch constants prevent accesses to those inactive
             * tail bindings. Bind the final real mip for them so the complete
             * descriptor ABI remains valid without creating illegal views. */
            descriptor.imageView = resource->uavViews[std::min<size_t>(
                uav.mip, resource->uavViews.size() - 1u)];
            descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            ++descriptorCount;
            break;
        }
        case FFX_VK_FSR3_3_1_5_DESCRIPTOR_BUFFER_SRV: {
            if (srvBufferIndex >= job.pipeline->srvBufferCount)
                return false;
            const FfxBufferSRV& srv = job.srvBuffers[srvBufferIndex++];
            BridgeResource* resource = lookup_resource(bridge, srv.resource.internalIndex);
            if (!resource || resource->buffer == VK_NULL_HANDLE ||
                srv.offset >= resource->description.width)
                return false;
            descriptor.buffer = resource->buffer;
            descriptor.bufferOffset = srv.offset;
            /* SDK buffer views use zero to mean the remaining buffer.  The
             * FI counters binding relies on that convention; rejecting it
             * aborted the dispatch before it could write a generated frame. */
            descriptor.bufferRange = srv.size ? srv.size :
                resource->description.width - srv.offset;
            if (descriptor.bufferRange == 0u)
                return false;
            ++descriptorCount;
            break;
        }
        case FFX_VK_FSR3_3_1_5_DESCRIPTOR_BUFFER_UAV: {
            if (uavBufferIndex >= job.pipeline->uavBufferCount)
                return false;
            const FfxBufferUAV& uav = job.uavBuffers[uavBufferIndex++];
            BridgeResource* resource = lookup_resource(bridge, uav.resource.internalIndex);
            if (!resource || resource->buffer == VK_NULL_HANDLE ||
                uav.offset >= resource->description.width)
                return false;
            descriptor.buffer = resource->buffer;
            descriptor.bufferOffset = uav.offset;
            descriptor.bufferRange = uav.size ? uav.size :
                resource->description.width - uav.offset;
            if (descriptor.bufferRange == 0u)
                return false;
            ++descriptorCount;
            break;
        }
        case FFX_VK_FSR3_3_1_5_DESCRIPTOR_CONSTANT_BUFFER: {
            if (cbIndex >= job.pipeline->constCount)
                return false;
            BridgeConstantBuffer* constantBuffer = lookup_constant_buffer(
                bridge, job.cbs[cbIndex++].data);
            if (!constantBuffer || constantBuffer->buffer == VK_NULL_HANDLE ||
                constantBuffer->size == 0u)
                return false;
            descriptor.buffer = constantBuffer->buffer;
            descriptor.bufferRange = constantBuffer->size;
            ++descriptorCount;
            break;
        }
        case FFX_VK_FSR3_3_1_5_DESCRIPTOR_SAMPLER:
            break; /* Immutable in the reflected layout. */
        default:
            return false;
        }
    }
    if (srvIndex != job.pipeline->srvTextureCount ||
        uavIndex != job.pipeline->uavTextureCount ||
        srvBufferIndex != job.pipeline->srvBufferCount ||
        uavBufferIndex != job.pipeline->uavBufferCount ||
        cbIndex != job.pipeline->constCount)
        return false;

    std::unique_ptr<BridgeDescriptorSet> descriptorSet(new (std::nothrow) BridgeDescriptorSet);
    if (!descriptorSet || ffxVkFsr3_3_1_5CreateDescriptorSet(
            &pipeline->pipeline, descriptorResources, descriptorCount,
            &descriptorSet->descriptorSet) != FFX_VK_PORTABLE_OK)
        return false;
    ++g_sotcLiveDescriptorSets; // sotc
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline.pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline->pipeline.pipelineLayout, 0u, 1u,
                            &descriptorSet->descriptorSet.descriptorSet, 0u, nullptr);
    vkCmdDispatch(commandBuffer, job.dimensions[0], job.dimensions[1], job.dimensions[2]);
    {
        std::lock_guard<std::mutex> lock(bridge->mutex);
        descriptorSet->frameId = bridge->activeFrameId;
        bridge->descriptorSets.emplace_back(std::move(descriptorSet));
    }
    return true;
}

static FfxVkFsr3_3_1_5Bridge* bridge_from(FfxInterface* backend)
{
    return backend ? static_cast<FfxVkFsr3_3_1_5Bridge*>(backend->device) : nullptr;
}

static FfxErrorCode from_portable(FfxVkPortableResult result)
{
    switch (result) {
    case FFX_VK_PORTABLE_OK:
        return FFX_OK;
    case FFX_VK_PORTABLE_ERROR_INVALID_POINTER:
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_POINTER);
    case FFX_VK_PORTABLE_ERROR_INVALID_STRUCT_SIZE:
    case FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT:
    case FFX_VK_PORTABLE_ERROR_UNSUPPORTED:
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT);
    case FFX_VK_PORTABLE_ERROR_OUT_OF_MEMORY:
        return static_cast<FfxErrorCode>(FFX_ERROR_OUT_OF_MEMORY);
    default:
        return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
    }
}

static bool pass_from_name(const wchar_t* name, uint32_t* outPass)
{
    struct NamePass { const wchar_t* name; uint32_t pass; };
    static const NamePass kPasses[] = {
        {L"FSR3-PREPARE-INPUTS", 0u},
        {L"FSR3-LUMA-PYRAMID", 1u},
        {L"FSR3-SHADING-CHANGE-PYRAMID", 2u},
        {L"FSR3-SHADING-CHANGE", 3u},
        {L"FSR3-PREPARE-REACTIVITY", 4u},
        {L"FSR3-LUMA-INSTABILITY", 5u},
        {L"FSR3-ACCUMULATE", 6u},
        {L"FSR3-ACCUM_SHARP", 7u},
        {L"FSR3-RCAS", 8u},
        {L"FSR3-DEBUG-VIEW", 9u},
        {L"FSR3-GEN_REACTIVE", 10u},
    };
    if (!name || !outPass)
        return false;
    for (const NamePass& item : kPasses) {
        if (std::wcscmp(name, item.name) == 0) {
            *outPass = item.pass;
            return true;
        }
    }
    return false;
}

static bool copy_name(wchar_t* destination, size_t destinationCount, const char* source)
{
    if (!destination || !source || destinationCount == 0u)
        return false;
    size_t index = 0;
    for (; source[index] != '\0' && index + 1u < destinationCount; ++index) {
        const unsigned char character = static_cast<unsigned char>(source[index]);
        if (character > 0x7fu)
            return false;
        destination[index] = static_cast<wchar_t>(character);
    }
    if (source[index] != '\0')
        return false;
    destination[index] = L'\0';
    return true;
}

static FfxErrorCode append_binding(FfxResourceBinding* bindings, uint32_t* count,
                                   uint32_t capacity,
                                   const FfxVkFsr3_3_1_5DescriptorBinding& reflected)
{
    if (!bindings || !count || *count >= capacity ||
        !copy_name(bindings[*count].name, FFX_RESOURCE_NAME_SIZE, reflected.name))
        return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
    bindings[*count].slotIndex = reflected.binding;
    bindings[*count].arrayIndex = 0u;
    bindings[*count].resourceIdentifier = 0u;
    ++*count;
    return FFX_OK;
}

} // namespace

extern "C" FfxVkFsr3_3_1_5Bridge* ffxVkFsr3_3_1_5CreateBridge(
    VkDevice device, const VkAllocationCallbacks* allocationCallbacks)
{
    return ffxVkFsr3_3_1_5CreateBridgeWithPhysicalDevice(
        VK_NULL_HANDLE, device, allocationCallbacks);
}

extern "C" FfxVkFsr3_3_1_5Bridge* ffxVkFsr3_3_1_5CreateBridgeWithPhysicalDevice(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const VkAllocationCallbacks* allocationCallbacks)
{
    if (device == VK_NULL_HANDLE)
        return nullptr;
    return new (std::nothrow) FfxVkFsr3_3_1_5Bridge(
        physicalDevice, device, allocationCallbacks);
}

extern "C" void ffxVkFsr3_3_1_5DestroyBridge(FfxVkFsr3_3_1_5Bridge* bridge)
{
    if (!bridge)
        return;
    for (const std::unique_ptr<BridgeResource>& resource : bridge->resources)
        destroy_resource(bridge, resource.get());
    for (const std::unique_ptr<BridgeConstantBuffer>& constantBuffer : bridge->constantBuffers)
        destroy_constant_buffer(bridge, constantBuffer.get());
    for (const std::unique_ptr<BridgeConstantBuffer>& constantBuffer : bridge->freeConstantBuffers)
        destroy_constant_buffer(bridge, constantBuffer.get()); // sotc
    for (const std::unique_ptr<BridgeDescriptorSet>& descriptorSet : bridge->descriptorSets)
        destroy_descriptor_set(descriptorSet.get());
    delete bridge;
}

extern "C" VkImage ffxVkFsr3_3_1_5BridgeGetNativeImage(
    FfxVkFsr3_3_1_5Bridge* bridge, const void* resourceToken)
{
    const int32_t index = lookup_resource_index(bridge, resourceToken);
    BridgeResource* resource = lookup_resource(bridge, index);
    return resource ? resource->image : VK_NULL_HANDLE;
}

extern "C" FfxVkFsr3_3_1_5Resource ffxVkFsr3_3_1_5BridgeImportImage(
    FfxVkFsr3_3_1_5Bridge* bridge,
    const FfxVkFsr3_3_1_5ImportedImageDescription* description)
{
    FfxVkFsr3_3_1_5Resource result{};
    if (!bridge || !description || description->image == VK_NULL_HANDLE ||
        description->width == 0u || description->height == 0u ||
        description->mipCount == 0u || description->arrayLayers == 0u ||
        description->layout == VK_IMAGE_LAYOUT_UNDEFINED ||
        (description->usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0u)
        return result;
    const FfxApiSurfaceFormat format = from_vk_format(description->format);
    if (format == FFX_API_SURFACE_FORMAT_UNKNOWN)
        return result;
    std::unique_ptr<BridgeResource> resource(new (std::nothrow) BridgeResource);
    if (!resource)
        return result;
    resource->description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    resource->description.format = format;
    resource->description.width = description->width;
    resource->description.height = description->height;
    resource->description.depth = description->arrayLayers;
    resource->description.mipCount = description->mipCount;
    resource->description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    resource->description.usage =
        (description->usage & VK_IMAGE_USAGE_STORAGE_BIT) ? FFX_API_RESOURCE_USAGE_UAV :
                                                            FFX_API_RESOURCE_USAGE_READ_ONLY;
    resource->image = description->image;
    resource->imageLayout = description->layout;
    resource->restoreLayout = description->layout;
    resource->currentState = static_cast<FfxApiResourceState>(description->state);
    resource->restoreState = resource->currentState;
    resource->owned = false;
    resource->imported = true;
    if (!create_image_views(bridge, resource.get())) {
        destroy_resource(bridge, resource.get());
        return result;
    }
    const int32_t index = allocate_resource_slot(bridge, std::move(resource));
    if (index <= 0)
        return result;
    BridgeResource* stored = lookup_resource(bridge, index);
    if (!stored) {
        FfxResourceInternal failed{index};
        FfxInterface backend{};
        backend.device = bridge;
        ffxVkFsr3_3_1_5BridgeDestroyResource(&backend, failed, 0u);
        return result;
    }
    result.resource = stored;
    return result;
}

extern "C" void ffxVkFsr3_3_1_5BridgeReleaseImportedImage(
    FfxVkFsr3_3_1_5Bridge* bridge, FfxVkFsr3_3_1_5Resource resource)
{
    const int32_t index = lookup_resource_index(bridge, resource.resource);
    if (index == 0)
        return;
    FfxResourceInternal internal{index};
    FfxInterface backend{};
    backend.device = bridge;
    ffxVkFsr3_3_1_5BridgeDestroyResource(&backend, internal, 0u);
}

/* Internal SDK-facing conversion.  Keep it out of the portable header: the
 * public ABI deliberately exposes only an opaque resource token. */
extern "C" FfxApiResource ffxVkFsr3_3_1_5BridgeResolveResource(
    FfxVkFsr3_3_1_5Bridge* bridge, FfxVkFsr3_3_1_5Resource resourceToken)
{
    FfxApiResource result{};
    BridgeResource* resource = lookup_resource(
        bridge, lookup_resource_index(bridge, resourceToken.resource));
    if (!resource)
        return result;
    result.resource = resource;
    result.description = resource->description;
    result.state = resource->currentState;
    return result;
}

/* The public 2.3 effect asks this backend-independent helper only when an
 * application queries memory usage.  It must exist in the linked renderer
 * even though Q2RTX does not currently expose that diagnostic.  Return a
 * conservative byte count for the formats the embedded upscaler profile
 * creates; Vulkan allocation itself still uses vkGet*MemoryRequirements. */
FfxErrorCode GetResourceSizeFromDescription(
    FfxDevice, const FfxCreateResourceDescription* description,
    uint64_t* sizeInBytes, uint64_t* alignment)
{
    if (!description || !sizeInBytes)
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_POINTER);
    if (description->resourceDescription.type == FFX_API_RESOURCE_TYPE_BUFFER) {
        *sizeInBytes = description->resourceDescription.size;
        if (alignment)
            *alignment = 1u;
        return FFX_OK;
    }
    uint32_t bytesPerPixel = 0u;
    switch (description->resourceDescription.format) {
    case FFX_API_SURFACE_FORMAT_R8_UNORM:
    case FFX_API_SURFACE_FORMAT_R8_UINT:
    case FFX_API_SURFACE_FORMAT_R8_SNORM:
        bytesPerPixel = 1u;
        break;
    case FFX_API_SURFACE_FORMAT_R16_FLOAT:
    case FFX_API_SURFACE_FORMAT_R16_UINT:
    case FFX_API_SURFACE_FORMAT_R16_UNORM:
    case FFX_API_SURFACE_FORMAT_R16_SNORM:
    case FFX_API_SURFACE_FORMAT_R8G8_UNORM:
    case FFX_API_SURFACE_FORMAT_R8G8_UINT:
        bytesPerPixel = 2u;
        break;
    case FFX_API_SURFACE_FORMAT_R32_FLOAT:
    case FFX_API_SURFACE_FORMAT_R32_UINT:
    case FFX_API_SURFACE_FORMAT_R16G16_FLOAT:
    case FFX_API_SURFACE_FORMAT_R16G16_UINT:
    case FFX_API_SURFACE_FORMAT_R16G16_SINT:
        bytesPerPixel = 4u;
        break;
    case FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT:
    case FFX_API_SURFACE_FORMAT_R32G32_FLOAT:
    case FFX_API_SURFACE_FORMAT_R32G32_UINT:
        bytesPerPixel = 8u;
        break;
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT:
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT:
        bytesPerPixel = 16u;
        break;
    default:
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT);
    }
    uint64_t total = 0u;
    uint64_t width = description->resourceDescription.width;
    uint64_t height = description->resourceDescription.height;
    uint64_t depth = std::max(1u, description->resourceDescription.depth);
    uint32_t mipCount = description->resourceDescription.mipCount;
    if (mipCount == 0u) {
        mipCount = 1u;
        uint64_t mipWidth = width;
        uint64_t mipHeight = height;
        while (mipWidth > 1u || mipHeight > 1u) {
            mipWidth = std::max<uint64_t>(1u, mipWidth >> 1u);
            mipHeight = std::max<uint64_t>(1u, mipHeight >> 1u);
            ++mipCount;
        }
    }
    for (uint32_t mip = 0u; mip < mipCount; ++mip) {
        total += width * height * depth * bytesPerPixel;
        width = std::max<uint64_t>(1u, width >> 1u);
        height = std::max<uint64_t>(1u, height >> 1u);
        depth = std::max<uint64_t>(1u, depth >> 1u);
    }
    *sizeInBytes = total;
    if (alignment)
        *alignment = 1u;
    return FFX_OK;
}

extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeCreateBackendContext(
    FfxInterface* backend, FfxEffect, FfxEffectBindlessConfig*, FfxUInt32* outContextId)
{
    if (!bridge_from(backend) || !outContextId)
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_POINTER);
    *outContextId = 1u;
    return FFX_OK;
}

extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeDestroyBackendContext(
    FfxInterface* backend, FfxUInt32)
{
    if (!bridge_from(backend))
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_POINTER);
    return FFX_OK;
}

extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeGetDeviceCapabilities(
    FfxInterface* backend, FfxDeviceCapabilities* outCapabilities)
{
    if (!bridge_from(backend) || !outCapabilities)
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_POINTER);
    std::memset(outCapabilities, 0, sizeof(*outCapabilities));
    outCapabilities->maximumSupportedShaderModel = FFX_SHADER_MODEL_6_2;
    outCapabilities->waveLaneCountMin = 32u;
    outCapabilities->waveLaneCountMax = 32u;
    return FFX_OK;
}

extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeStageConstantBufferData(
    FfxInterface* backend, void* data, FfxUInt32 size, FfxConstantBuffer* outConstantBuffer)
{
    FfxVkFsr3_3_1_5Bridge* bridge = bridge_from(backend);
    std::unique_ptr<BridgeConstantBuffer> constantBuffer;
    if (!bridge || !data || !outConstantBuffer)
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_POINTER);
    if (bridge->physicalDevice == VK_NULL_HANDLE || size == 0u || size % sizeof(uint32_t) != 0u)
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT);
    // sotc: reuse a retired buffer that is large enough (each pass stages the same sizes every
    // frame, so after the first frames no allocation happens).
    {
        std::lock_guard<std::mutex> lock(bridge->mutex);
        auto& pool = bridge->freeConstantBuffers;
        auto it = std::find_if(pool.begin(), pool.end(),
            [size](const std::unique_ptr<BridgeConstantBuffer>& item) { return item->capacity >= size; });
        if (it != pool.end()) {
            constantBuffer = std::move(*it);
            *it = std::move(pool.back());
            pool.pop_back();
        }
    }
    if (!constantBuffer) {
        constantBuffer.reset(new (std::nothrow) BridgeConstantBuffer);
        if (!constantBuffer)
            return static_cast<FfxErrorCode>(FFX_ERROR_OUT_OF_MEMORY);
        VkBufferCreateInfo bufferInfo{};
        VkMemoryRequirements requirements{};
        VkMemoryAllocateInfo allocationInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (vkCreateBuffer(bridge->device, &bufferInfo, bridge->allocationCallbacks,
                           &constantBuffer->buffer) != VK_SUCCESS)
            return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
        vkGetBufferMemoryRequirements(bridge->device, constantBuffer->buffer, &requirements);
        allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocationInfo.allocationSize = requirements.size;
        allocationInfo.memoryTypeIndex = find_memory_type(
            bridge->physicalDevice, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        if (allocationInfo.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(bridge->device, &allocationInfo, bridge->allocationCallbacks,
                             &constantBuffer->memory) != VK_SUCCESS ||
            vkBindBufferMemory(bridge->device, constantBuffer->buffer, constantBuffer->memory, 0u) != VK_SUCCESS) {
            destroy_constant_buffer(bridge, constantBuffer.get());
            return static_cast<FfxErrorCode>(FFX_ERROR_OUT_OF_MEMORY);
        }
        if (vkMapMemory(bridge->device, constantBuffer->memory, 0u, VK_WHOLE_SIZE, 0u,
                        &constantBuffer->mapped) != VK_SUCCESS) {
            destroy_constant_buffer(bridge, constantBuffer.get());
            return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
        }
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(bridge->physicalDevice, &memoryProperties);
        constantBuffer->coherent =
            (memoryProperties.memoryTypes[allocationInfo.memoryTypeIndex].propertyFlags &
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u;
        constantBuffer->capacity = size;
        ++g_sotcLiveConstantBuffers; // sotc
    }
    std::memcpy(constantBuffer->mapped, data, size);
    if (!constantBuffer->coherent) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = constantBuffer->memory;
        range.size = VK_WHOLE_SIZE;
        if (vkFlushMappedMemoryRanges(bridge->device, 1u, &range) != VK_SUCCESS) {
            destroy_constant_buffer(bridge, constantBuffer.get());
            return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
        }
    }
    constantBuffer->size = size;
    constantBuffer->frameId = bridge->activeFrameId;
    outConstantBuffer->num32BitEntries = size / sizeof(uint32_t);
    outConstantBuffer->data = reinterpret_cast<uint32_t*>(constantBuffer.get());
    {
        std::lock_guard<std::mutex> lock(bridge->mutex);
        bridge->constantBuffers.emplace_back(std::move(constantBuffer));
    }
    return FFX_OK;
}

extern "C" void ffxVkFsr3_3_1_5BridgeBeginFrame(FfxVkFsr3_3_1_5Bridge* bridge,
                                                   uint64_t frameId)
{
    if (!bridge)
        return;
    std::lock_guard<std::mutex> lock(bridge->mutex);
    bridge->activeFrameId = frameId;
}

extern "C" void ffxVkFsr3_3_1_5BridgeRetireFrame(FfxVkFsr3_3_1_5Bridge* bridge,
                                                    uint64_t completedFrameId)
{
    if (!bridge || !completedFrameId)
        return;
    std::vector<std::unique_ptr<BridgeDescriptorSet>> descriptorSets;
    {
        std::lock_guard<std::mutex> lock(bridge->mutex);
        auto descriptorEnd = std::stable_partition(bridge->descriptorSets.begin(),
            bridge->descriptorSets.end(), [completedFrameId](const std::unique_ptr<BridgeDescriptorSet>& item) {
                return !item || item->frameId == 0u || item->frameId > completedFrameId;
            });
        descriptorSets.assign(std::make_move_iterator(descriptorEnd),
                              std::make_move_iterator(bridge->descriptorSets.end()));
        bridge->descriptorSets.erase(descriptorEnd, bridge->descriptorSets.end());
        auto constantEnd = std::stable_partition(bridge->constantBuffers.begin(),
            bridge->constantBuffers.end(), [completedFrameId](const std::unique_ptr<BridgeConstantBuffer>& item) {
                return !item || item->frameId == 0u || item->frameId > completedFrameId;
            });
        // sotc: completed constant buffers go back to the free list instead of being destroyed.
        for (auto it = constantEnd; it != bridge->constantBuffers.end(); ++it) {
            if (*it)
                bridge->freeConstantBuffers.emplace_back(std::move(*it));
        }
        bridge->constantBuffers.erase(constantEnd, bridge->constantBuffers.end());
    }
    for (const std::unique_ptr<BridgeDescriptorSet>& item : descriptorSets)
        destroy_descriptor_set(item.get());
}

extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeCreateResource(
    FfxInterface* backend,
    const FfxCreateResourceDescription* description,
    FfxUInt32,
    FfxResourceInternal* outResource)
{
    FfxVkFsr3_3_1_5Bridge* bridge = bridge_from(backend);
    if (!bridge || !description || !outResource)
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_POINTER);
    if (bridge->physicalDevice == VK_NULL_HANDLE ||
        description->initData.type == FFX_RESOURCE_INIT_DATA_TYPE_INVALID)
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT);

    std::unique_ptr<BridgeResource> resource(new (std::nothrow) BridgeResource);
    if (!resource)
        return static_cast<FfxErrorCode>(FFX_ERROR_OUT_OF_MEMORY);
    resource->description = description->resourceDescription;
    resource->currentState = static_cast<FfxApiResourceState>(description->initialState);
    if (resource->description.width == 0u ||
        (resource->description.type != FFX_API_RESOURCE_TYPE_BUFFER &&
         resource->description.height == 0u))
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT);

    VkMemoryRequirements requirements{};
    VkMemoryAllocateInfo allocationInfo{};
    allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    VkMemoryPropertyFlags memoryProperties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (description->heapInfo.heapType == FFX_HEAP_TYPE_UPLOAD ||
        description->heapInfo.heapType == FFX_HEAP_TYPE_READBACK)
        memoryProperties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

    if (resource->description.type == FFX_API_RESOURCE_TYPE_BUFFER) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = resource->description.width;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (vkCreateBuffer(bridge->device, &bufferInfo, bridge->allocationCallbacks,
                           &resource->buffer) != VK_SUCCESS)
            return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
        vkGetBufferMemoryRequirements(bridge->device, resource->buffer, &requirements);
    } else if (resource->description.type == FFX_API_RESOURCE_TYPE_TEXTURE2D) {
        const VkFormat format = to_vk_format(resource->description.format);
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        if (format == VK_FORMAT_UNDEFINED)
            return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT);
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {resource->description.width, resource->description.height, 1u};
        imageInfo.mipLevels = resource->description.mipCount ? resource->description.mipCount :
                              full_mip_count(resource->description.width, resource->description.height);
        resource->description.mipCount = imageInfo.mipLevels;
        imageInfo.arrayLayers = resource->description.depth ? resource->description.depth : 1u;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (resource->description.usage & FFX_API_RESOURCE_USAGE_UAV)
            imageInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(bridge->device, &imageInfo, bridge->allocationCallbacks,
                          &resource->image) != VK_SUCCESS)
            return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
        vkGetImageMemoryRequirements(bridge->device, resource->image, &requirements);
    } else {
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT);
    }

    allocationInfo.allocationSize = requirements.size;
    allocationInfo.memoryTypeIndex = find_memory_type(bridge->physicalDevice,
                                                       requirements.memoryTypeBits,
                                                       memoryProperties);
    if (allocationInfo.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(bridge->device, &allocationInfo, bridge->allocationCallbacks,
                         &resource->memory) != VK_SUCCESS) {
        destroy_resource(bridge, resource.get());
        return static_cast<FfxErrorCode>(FFX_ERROR_OUT_OF_MEMORY);
    }
    {
        VkPhysicalDeviceMemoryProperties physicalMemoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(bridge->physicalDevice, &physicalMemoryProperties);
        resource->memoryProperties =
            physicalMemoryProperties.memoryTypes[allocationInfo.memoryTypeIndex].propertyFlags;
    }
    const VkResult bindResult = resource->buffer != VK_NULL_HANDLE ?
        vkBindBufferMemory(bridge->device, resource->buffer, resource->memory, 0u) :
        vkBindImageMemory(bridge->device, resource->image, resource->memory, 0u);
    if (bindResult != VK_SUCCESS) {
        destroy_resource(bridge, resource.get());
        return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
    }
    resource->allocationSize = requirements.size;

    if (resource->image != VK_NULL_HANDLE && !create_image_views(bridge, resource.get())) {
        destroy_resource(bridge, resource.get());
        return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
    }

    /* Upload resources are fully initialized before the host queues a copy.
     * Optimal-tiled images are initialized only by that queued copy path. */
    if (resource->buffer != VK_NULL_HANDLE &&
        description->initData.type != FFX_RESOURCE_INIT_DATA_TYPE_UNINITIALIZED) {
        void* mapped = nullptr;
        if (vkMapMemory(bridge->device, resource->memory, 0u, description->initData.size,
                        0u, &mapped) != VK_SUCCESS) {
            destroy_resource(bridge, resource.get());
            return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
        }
        if (description->initData.type == FFX_RESOURCE_INIT_DATA_TYPE_BUFFER)
            std::memcpy(mapped, description->initData.buffer, description->initData.size);
        else if (description->initData.type == FFX_RESOURCE_INIT_DATA_TYPE_VALUE)
            std::memset(mapped, description->initData.value, description->initData.size);
        if ((resource->memoryProperties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0u) {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = resource->memory;
            range.size = description->initData.size;
            if (vkFlushMappedMemoryRanges(bridge->device, 1u, &range) != VK_SUCCESS) {
                vkUnmapMemory(bridge->device, resource->memory);
                destroy_resource(bridge, resource.get());
                return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
            }
        }
        vkUnmapMemory(bridge->device, resource->memory);
    }

    outResource->internalIndex = allocate_resource_slot(bridge, std::move(resource));
    if (description->resourceDescription.type != FFX_API_RESOURCE_TYPE_BUFFER &&
        description->initData.type != FFX_RESOURCE_INIT_DATA_TYPE_UNINITIALIZED) {
        FfxCreateResourceDescription uploadDescription = *description;
        FfxResourceInternal uploadResource{};
        FfxGpuJobDescription uploadJob{};
        uploadDescription.heapInfo = FfxResourceHeapPlacementInfo::InitUpload();
        uploadDescription.resourceDescription.type = FFX_API_RESOURCE_TYPE_BUFFER;
        uploadDescription.resourceDescription.width = static_cast<uint32_t>(description->initData.size);
        uploadDescription.resourceDescription.height = 1u;
        uploadDescription.resourceDescription.depth = 1u;
        uploadDescription.resourceDescription.mipCount = 1u;
        uploadDescription.resourceDescription.usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
        uploadDescription.initialState = FFX_API_RESOURCE_STATE_GENERIC_READ;
        if (!backend->fpCreateResource || !backend->fpScheduleGpuJob ||
            backend->fpCreateResource(backend, &uploadDescription, 0u, &uploadResource) != FFX_OK) {
            ffxVkFsr3_3_1_5BridgeDestroyResource(backend, *outResource, 0u);
            outResource->internalIndex = 0;
            return static_cast<FfxErrorCode>(FFX_ERROR_INCOMPLETE_INTERFACE);
        }
        uploadJob.jobType = FFX_GPU_JOB_COPY;
        uploadJob.copyJobDescriptor.src = uploadResource;
        uploadJob.copyJobDescriptor.dst = *outResource;
        uploadJob.copyJobDescriptor.size = static_cast<uint32_t>(description->initData.size);
        if (backend->fpScheduleGpuJob(backend, &uploadJob) != FFX_OK) {
            ffxVkFsr3_3_1_5BridgeDestroyResource(backend, uploadResource, 0u);
            ffxVkFsr3_3_1_5BridgeDestroyResource(backend, *outResource, 0u);
            outResource->internalIndex = 0;
            return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
        }
    }
    return FFX_OK;
}

extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeGetEffectGpuMemoryUsage(
    FfxInterface* backend, FfxUInt32, FfxApiEffectMemoryUsage* outUsage)
{
    FfxVkFsr3_3_1_5Bridge* bridge = bridge_from(backend);
    if (!bridge || !outUsage)
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_POINTER);

    FfxApiEffectMemoryUsage usage{};
    {
        std::lock_guard<std::mutex> lock(bridge->mutex);
        for (const std::unique_ptr<BridgeResource>& resource : bridge->resources) {
            if (resource && resource->owned)
                usage.totalUsageInBytes += resource->allocationSize;
        }
    }
    /* The bridge deliberately gives every SDK-owned resource a distinct
     * VkDeviceMemory allocation, so none is aliasable.  Imported images are
     * caller-owned and intentionally absent. */
    *outUsage = usage;
    return FFX_OK;
}

extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeDestroyResource(
    FfxInterface* backend, FfxResourceInternal resource, FfxUInt32)
{
    FfxVkFsr3_3_1_5Bridge* bridge = bridge_from(backend);
    if (!bridge)
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_POINTER);
    if (resource.internalIndex == 0)
        return FFX_OK;
    const size_t index = resource.internalIndex > 0 ?
        static_cast<size_t>(resource.internalIndex - 1) : SIZE_MAX;
    std::unique_ptr<BridgeResource> owned;
    {
        std::lock_guard<std::mutex> lock(bridge->mutex);
        if (index >= bridge->resources.size() || !bridge->resources[index])
            return FFX_OK; // The SDK intentionally makes duplicate releases idempotent.
        owned = std::move(bridge->resources[index]);
    }
    destroy_resource(bridge, owned.get());
    return FFX_OK;
}

extern "C" FfxApiResourceDescription ffxVkFsr3_3_1_5BridgeGetResourceDescription(
    FfxInterface* backend, FfxResourceInternal resource)
{
    BridgeResource* bridgeResource = lookup_resource(bridge_from(backend), resource.internalIndex);
    return bridgeResource ? bridgeResource->description : FfxApiResourceDescription{};
}

extern "C" FfxApiResource ffxVkFsr3_3_1_5BridgeGetResource(
    FfxInterface* backend, FfxResourceInternal resource)
{
    BridgeResource* bridgeResource = lookup_resource(bridge_from(backend), resource.internalIndex);
    FfxApiResource result{};
    if (bridgeResource) {
        result.resource = bridgeResource;
        result.description = bridgeResource->description;
        result.state = bridgeResource->currentState;
    }
    return result;
}

extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeRegisterResource(
    FfxInterface* backend, const FfxApiResource* resource, FfxUInt32,
    FfxResourceInternal* outResource)
{
    if (!bridge_from(backend) || !resource || !outResource)
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_POINTER);
    const int32_t index = lookup_resource_index(bridge_from(backend), resource->resource);
    if (index == 0)
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT);
    BridgeResource* bridgeResource = lookup_resource(bridge_from(backend), index);
    if (!bridgeResource)
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT);
    if (bridgeResource->imported) {
        bridgeResource->restoreState = static_cast<FfxApiResourceState>(resource->state);
        std::lock_guard<std::mutex> lock(bridge_from(backend)->mutex);
        const auto& registered = bridge_from(backend)->registeredImportedResources;
        if (std::find(registered.begin(), registered.end(), index) == registered.end())
            bridge_from(backend)->registeredImportedResources.push_back(index);
    }
    outResource->internalIndex = index;
    return FFX_OK;
}

extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeUnregisterResources(
    FfxInterface* backend, FfxCommandList commandList, FfxUInt32)
{
    FfxVkFsr3_3_1_5Bridge* bridge = bridge_from(backend);
    VkCommandBuffer commandBuffer = reinterpret_cast<VkCommandBuffer>(commandList);
    if (!bridge || commandBuffer == VK_NULL_HANDLE)
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_POINTER);
    std::vector<int32_t> registered;
    {
        std::lock_guard<std::mutex> lock(bridge->mutex);
        registered.swap(bridge->registeredImportedResources);
    }
    for (const int32_t index : registered) {
        BridgeResource* resource = lookup_resource(bridge, index);
        if (!resource || !resource->imported ||
            !ensure_image_layout(commandBuffer, resource, resource->restoreLayout))
            return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
        resource->currentState = resource->restoreState;
    }
    return FFX_OK;
}

extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeScheduleGpuJob(
    FfxInterface* backend, const FfxGpuJobDescription* job)
{
    FfxVkFsr3_3_1_5Bridge* bridge = bridge_from(backend);
    if (!bridge || !job)
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_POINTER);
    /* Copy is the first executable job type.  Keep the queue generic so the
     * subsequent clear/barrier/compute layer preserves the SDK order. */
    if (job->jobType > FFX_GPU_JOB_DISCARD)
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT);
    std::lock_guard<std::mutex> lock(bridge->mutex);
    bridge->jobs.push_back(*job);
    return FFX_OK;
}

extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeExecuteGpuJobs(
    FfxInterface* backend, FfxCommandList commandList, FfxUInt32)
{
    FfxVkFsr3_3_1_5Bridge* bridge = bridge_from(backend);
    VkCommandBuffer commandBuffer = reinterpret_cast<VkCommandBuffer>(commandList);
    std::vector<FfxGpuJobDescription> jobs;
    if (!bridge || commandBuffer == VK_NULL_HANDLE)
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_POINTER);
    {
        std::lock_guard<std::mutex> lock(bridge->mutex);
        jobs.swap(bridge->jobs);
    }
    for (const FfxGpuJobDescription& job : jobs) {
        switch (job.jobType) {
        case FFX_GPU_JOB_COPY: {
            BridgeResource* source = lookup_resource(bridge, job.copyJobDescriptor.src.internalIndex);
            BridgeResource* destination = lookup_resource(bridge, job.copyJobDescriptor.dst.internalIndex);
            if (!source || !destination)
                return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT);
            if (source->buffer != VK_NULL_HANDLE && destination->image != VK_NULL_HANDLE &&
                job.copyJobDescriptor.size != 0u) {
                VkBufferMemoryBarrier sourceBarrier{};
                sourceBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                sourceBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
                sourceBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                sourceBarrier.buffer = source->buffer;
                sourceBarrier.offset = job.copyJobDescriptor.srcOffset;
                sourceBarrier.size = job.copyJobDescriptor.size;
                vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_HOST_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr,
                                     1u, &sourceBarrier, 0u, nullptr);
                if (!ensure_image_layout(commandBuffer, destination,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL))
                    return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT);
                VkBufferImageCopy copy{};
                copy.bufferOffset = job.copyJobDescriptor.srcOffset;
                copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                copy.imageSubresource.layerCount = destination->description.depth ?
                    destination->description.depth : 1u;
                copy.imageExtent = {destination->description.width, destination->description.height, 1u};
                vkCmdCopyBufferToImage(commandBuffer, source->buffer, destination->image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
                if (!ensure_image_layout(commandBuffer, destination,
                                         image_layout(destination->currentState)))
                    return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
                break;
            }

            /* FSR frame interpolation retains the current interpolation source
             * as its next-frame history with an image-to-image copy.  This is
             * distinct from the buffer upload above: treating it as an upload
             * silently dropped the history job and produced black generated
             * frames on RADV. */
            if (source->image == VK_NULL_HANDLE || destination->image == VK_NULL_HANDLE ||
                !ensure_image_layout(commandBuffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) ||
                !ensure_image_layout(commandBuffer, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL))
                return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT);
            VkImageCopy copy{};
            copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.srcSubresource.layerCount = std::min(source->description.depth ? source->description.depth : 1u,
                                                       destination->description.depth ? destination->description.depth : 1u);
            copy.dstSubresource = copy.srcSubresource;
            copy.extent = {std::min(source->description.width, destination->description.width),
                           std::min(source->description.height, destination->description.height), 1u};
            vkCmdCopyImage(commandBuffer, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           destination->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
            if (!ensure_image_layout(commandBuffer, source, image_layout(source->currentState)) ||
                !ensure_image_layout(commandBuffer, destination, image_layout(destination->currentState)))
                return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
            break;
        }
        case FFX_GPU_JOB_CLEAR_FLOAT: {
            BridgeResource* target = lookup_resource(bridge, job.clearJobDescriptor.target.internalIndex);
            if (!target || target->image == VK_NULL_HANDLE ||
                !ensure_image_layout(commandBuffer, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL))
                return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT);
            VkClearColorValue clear{};
            clear.float32[0] = job.clearJobDescriptor.color[0];
            clear.float32[1] = job.clearJobDescriptor.color[1];
            clear.float32[2] = job.clearJobDescriptor.color[2];
            clear.float32[3] = job.clearJobDescriptor.color[3];
            VkImageSubresourceRange range{};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.levelCount = target->description.mipCount;
            range.layerCount = target->description.depth ? target->description.depth : 1u;
            vkCmdClearColorImage(commandBuffer, target->image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1u, &range);
            target->currentState = FFX_API_RESOURCE_STATE_COPY_DEST;
            break;
        }
        case FFX_GPU_JOB_BARRIER: {
            const FfxBarrierDescription& barrier = job.barrierDescriptor;
            BridgeResource* resource = lookup_resource(bridge, barrier.resource.internalIndex);
            if (!resource)
                return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT);
            if (barrier.barrierType == FFX_BARRIER_TYPE_TRANSITION) {
                if (resource->image != VK_NULL_HANDLE &&
                    !ensure_image_layout(commandBuffer, resource, image_layout(barrier.newState)))
                    return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
                /* A same-layout transition (most often GENERAL -> GENERAL)
                 * is still a real memory dependency.  The previous bridge
                 * treated it as a no-op because ensure_image_layout only
                 * emits a barrier for a layout change.  That leaves a later
                 * FI/OF compute pass free to read stale UAV writes, which on
                 * RADV manifests as a completely black generated frame. */
                if (resource->image != VK_NULL_HANDLE) {
                    VkImageMemoryBarrier memoryBarrier{};
                    memoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                        VK_ACCESS_SHADER_WRITE_BIT;
                    memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                        VK_ACCESS_SHADER_WRITE_BIT;
                    memoryBarrier.oldLayout = resource->imageLayout;
                    memoryBarrier.newLayout = resource->imageLayout;
                    memoryBarrier.image = resource->image;
                    memoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    memoryBarrier.subresourceRange.levelCount =
                        resource->description.mipCount;
                    memoryBarrier.subresourceRange.layerCount =
                        resource->description.depth ? resource->description.depth : 1u;
                    vkCmdPipelineBarrier(commandBuffer,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr,
                        0u, nullptr, 1u, &memoryBarrier);
                }
                resource->currentState = barrier.newState;
            } else {
                VkMemoryBarrier memoryBarrier{};
                memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 1u,
                                     &memoryBarrier, 0u, nullptr, 0u, nullptr);
            }
            break;
        }
        case FFX_GPU_JOB_DISCARD:
            /* Vulkan has no matching discard operation.  The next clear or
             * write establishes contents; preserve layout/state tracking. */
            break;
        case FFX_GPU_JOB_COMPUTE:
            if (!record_compute_job(bridge, commandBuffer, job.computeJobDescriptor))
                return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
            break;
        default:
            return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT);
        }
    }
    return FFX_OK;
}

extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeCreatePipeline(
    FfxInterface* backend,
    FfxShaderBlob* shaderBlob,
    const FfxPipelineDescription* description,
    FfxUInt32,
    FfxPipelineState* outPipeline)
{
    FfxVkFsr3_3_1_5Bridge* bridge = bridge_from(backend);
    uint32_t pass = 0;
    uint32_t permutation = 7u; // Lanczos + HDR + low-resolution MVs.
    const uint32_t* words = nullptr;
    size_t wordCount = 0;
    BridgePipeline* owned = nullptr;

    if (!bridge || !description || !outPipeline)
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_POINTER);
    /* SDK 2.3's FI/OF Vulkan blob accessor returns a checked SPIR-V module
     * directly.  Prefer it whenever present so this resource/job bridge stays
     * reusable across public effects.  The historical FSR3.1.5 host still
     * supplies DX12-style/empty blobs, so preserve its explicit catalogue as
     * a strict fallback rather than guessing from arbitrary data. */
    if (shaderBlob && shaderBlob->data && shaderBlob->size >= sizeof(uint32_t) &&
        shaderBlob->size % sizeof(uint32_t) == 0u) {
        uint32_t magic = 0u;
        std::memcpy(&magic, shaderBlob->data, sizeof(magic));
        if (magic == 0x07230203u) {
            words = reinterpret_cast<const uint32_t*>(shaderBlob->data);
            wordCount = shaderBlob->size / sizeof(uint32_t);
        }
    }
    if (!words) {
        if (!pass_from_name(description->name, &pass))
            return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT);
        if (pass == 7u)
            permutation |= 32u; // The host's ACCUM_SHARP pipeline is distinct.
        if (ffxVkFsr3_3_1_5GetEmbeddedModule(pass, permutation, &words, &wordCount) !=
            FFX_VK_PORTABLE_OK)
            return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_ARGUMENT);
    }

    owned = new (std::nothrow) BridgePipeline;
    if (!owned)
        return static_cast<FfxErrorCode>(FFX_ERROR_OUT_OF_MEMORY);
    FfxVkFsr3_3_1_5PipelineCreateInfo createInfo{};
    createInfo.structSize = sizeof(createInfo);
    createInfo.device = bridge->device;
    createInfo.allocationCallbacks = bridge->allocationCallbacks;
    createInfo.spirvWords = words;
    createInfo.spirvWordCount = wordCount;
    const FfxErrorCode result = from_portable(
        ffxVkFsr3_3_1_5CreatePipeline(&createInfo, &owned->pipeline));
    if (result != FFX_OK) {
        delete owned;
        return result;
    }
    // sotc: the embedded SPIR-V is the wave32 permutation (GetDeviceCapabilities reports 32
    // lanes), so on devices that may run compute wider (AMD) the pipeline is rebuilt with a
    // required subgroup size of 32. NVIDIA (fixed 32) never takes this path.
    if (g_sotcRequiredSubgroupSize != 0u) {
        char entryPoint[64];
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroupInfo{};
        subgroupInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO;
        subgroupInfo.requiredSubgroupSize = g_sotcRequiredSubgroupSize;
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.pNext = &subgroupInfo;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = owned->pipeline.shaderModule;
        pipelineInfo.stage.pName = entryPoint;
        pipelineInfo.layout = owned->pipeline.pipelineLayout;
        VkPipeline pinned = VK_NULL_HANDLE;
        if (ffxVkFsr3_3_1_5ReflectComputeEntryPoint(words, wordCount, entryPoint) != FFX_VK_PORTABLE_OK ||
            vkCreateComputePipelines(bridge->device, VK_NULL_HANDLE, 1u, &pipelineInfo,
                                     bridge->allocationCallbacks, &pinned) != VK_SUCCESS) {
            ffxVkFsr3_3_1_5DestroyPipeline(&owned->pipeline);
            delete owned;
            return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
        }
        vkDestroyPipeline(bridge->device, owned->pipeline.pipeline, bridge->allocationCallbacks);
        owned->pipeline.pipeline = pinned;
    }


    std::memset(outPipeline, 0, sizeof(*outPipeline));
    for (uint32_t index = 0; index < owned->pipeline.bindingCount; ++index) {
        const FfxVkFsr3_3_1_5DescriptorBinding& binding = owned->pipeline.bindings[index];
        FfxErrorCode appendResult = FFX_OK;
        switch (binding.descriptorClass) {
        case FFX_VK_FSR3_3_1_5_DESCRIPTOR_SRV:
            appendResult = append_binding(outPipeline->srvTextureBindings,
                                          &outPipeline->srvTextureCount,
                                          FFX_MAX_NUM_SRVS, binding);
            break;
        case FFX_VK_FSR3_3_1_5_DESCRIPTOR_UAV:
            appendResult = append_binding(outPipeline->uavTextureBindings,
                                          &outPipeline->uavTextureCount,
                                          FFX_MAX_NUM_UAVS, binding);
            break;
        case FFX_VK_FSR3_3_1_5_DESCRIPTOR_BUFFER_SRV:
            appendResult = append_binding(outPipeline->srvBufferBindings,
                                          &outPipeline->srvBufferCount,
                                          FFX_MAX_NUM_SRVS, binding);
            break;
        case FFX_VK_FSR3_3_1_5_DESCRIPTOR_BUFFER_UAV:
            appendResult = append_binding(outPipeline->uavBufferBindings,
                                          &outPipeline->uavBufferCount,
                                          FFX_MAX_NUM_UAVS, binding);
            break;
        case FFX_VK_FSR3_3_1_5_DESCRIPTOR_CONSTANT_BUFFER:
            appendResult = append_binding(outPipeline->constantBufferBindings,
                                          &outPipeline->constCount,
                                          FFX_MAX_NUM_CONST_BUFFERS, binding);
            break;
        case FFX_VK_FSR3_3_1_5_DESCRIPTOR_SAMPLER:
            break; // Immutable sampler in the reflected Vulkan layout.
        default:
            appendResult = static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
            break;
        }
        if (appendResult != FFX_OK) {
            ffxVkFsr3_3_1_5DestroyPipeline(&owned->pipeline);
            delete owned;
            std::memset(outPipeline, 0, sizeof(*outPipeline));
            return appendResult;
        }
    }
    if (!copy_name(outPipeline->name, FFX_RESOURCE_NAME_SIZE, "FSR3.1.5 Vulkan")) {
        ffxVkFsr3_3_1_5DestroyPipeline(&owned->pipeline);
        delete owned;
        std::memset(outPipeline, 0, sizeof(*outPipeline));
        return static_cast<FfxErrorCode>(FFX_ERROR_BACKEND_API_ERROR);
    }
    outPipeline->pipeline = owned;
    return FFX_OK;
}

extern "C" FfxErrorCode ffxVkFsr3_3_1_5BridgeDestroyPipeline(
    FfxInterface*, FfxPipelineState* pipeline, FfxUInt32)
{
    if (!pipeline)
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_POINTER);
    BridgePipeline* owned = static_cast<BridgePipeline*>(pipeline->pipeline);
    if (owned) {
        ffxVkFsr3_3_1_5DestroyPipeline(&owned->pipeline);
        delete owned;
    }
    std::memset(pipeline, 0, sizeof(*pipeline));
    return FFX_OK;
}
