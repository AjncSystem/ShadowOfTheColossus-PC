// sotc: GPU cost of one FSR 3.1.6 frame generation step (RecordPrepare + RecordDispatch) at the
// port's sizes: render (depth/motion) 1920x1080, display (color/output) 1080p, 1440p and 2160p.
// Standalone: own Vulkan device, no window, no game. Build: ninja -C out/gpu fg-bench.
// Inputs are cleared images (the passes have fixed dispatch sizes, so content barely changes
// the cost); the numbers are GPU timestamps around both calls, median and p95 over 120 frames.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>
#include "ffx_vk_fsr3_3_1_5_bridge.h"

namespace {

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
};

VkPhysicalDevice g_physical;
VkDevice g_device;

bool Check(bool ok, const char* what) {
    if (!ok) std::fprintf(stderr, "fg-bench: %s failed\n", what);
    return ok;
}

bool CreateImage(uint32_t w, uint32_t h, VkFormat format, Image* out) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {w, h, 1u};
    info.mipLevels = info.arrayLayers = 1u;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(g_device, &info, nullptr, &out->image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g_device, out->image, &req);
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(g_physical, &mem);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount && alloc.memoryTypeIndex == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            alloc.memoryTypeIndex = i;
    out->format = format;
    out->width = w;
    out->height = h;
    return alloc.memoryTypeIndex != UINT32_MAX &&
           vkAllocateMemory(g_device, &alloc, nullptr, &out->memory) == VK_SUCCESS &&
           vkBindImageMemory(g_device, out->image, out->memory, 0) == VK_SUCCESS;
}

void DestroyImage(Image& image) {
    vkDestroyImage(g_device, image.image, nullptr);
    vkFreeMemory(g_device, image.memory, nullptr);
    image = {};
}

FfxVkFsr3_3_1_6FrameGenerationImage Desc(const Image& image, VkImageLayout layout) {
    return {image.image, image.format, image.width, image.height, layout,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
}

void Barrier(VkCommandBuffer cmd, const Image& image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Clears an image to a value and leaves it in `layout` for the FFX calls.
void Fill(VkCommandBuffer cmd, const Image& image, float v, VkImageLayout from,
          VkImageLayout layout) {
    Barrier(cmd, image, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue color{{v, 1.0f - v, 0.5f * v, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    Barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout);
}

} // namespace

int main() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance;
    if (!Check(vkCreateInstance(&ici, nullptr, &instance) == VK_SUCCESS, "vkCreateInstance"))
        return 1;
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    if (!Check(count > 0, "physical device")) return 1;
    g_physical = devices[0];
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { g_physical = d; break; }
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_physical, &props);
    std::printf("device: %s\n", props.deviceName);

    uint32_t family = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(g_physical, &count, families.data());
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR derivatives{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
    derivatives.computeDerivativeGroupLinear = VK_TRUE;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &derivatives;
    features.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    features.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    const char* extensions[] = {VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &features;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = extensions;
    if (!Check(vkCreateDevice(g_physical, &dci, nullptr, &g_device) == VK_SUCCESS,
               "vkCreateDevice (needs VK_KHR_compute_shader_derivatives, *WithoutFormat)"))
        return 1;
    VkQueue queue;
    vkGetDeviceQueue(g_device, family, 0, &queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    VkCommandPool pool;
    vkCreateCommandPool(g_device, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(g_device, &cai, &cmd);
    VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpi.queryCount = 2;
    VkQueryPool queries;
    vkCreateQueryPool(g_device, &qpi, nullptr, &queries);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vkCreateFence(g_device, &fci, nullptr, &fence);

    const uint32_t rw = 1920, rh = 1080;
    const uint32_t displays[][2] = {{1920, 1080}, {2560, 1440}, {3840, 2160}};
    const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    int status = 0;
    for (const auto& d : displays) {
        Image color[2], depth, motion, output;
        bool ok = CreateImage(d[0], d[1], colorFormat, &color[0]) &&
                  CreateImage(d[0], d[1], colorFormat, &color[1]) &&
                  CreateImage(rw, rh, VK_FORMAT_R32_SFLOAT, &depth) &&
                  CreateImage(rw, rh, VK_FORMAT_R16G16_SFLOAT, &motion) &&
                  CreateImage(d[0], d[1], colorFormat, &output);
        FfxVkFsr3_3_1_6FrameGenerationContext* context = nullptr;
        FfxVkFsr3_3_1_6FrameGenerationCreateInfo create{g_physical, g_device, rw, rh,
                                                         d[0], d[1], colorFormat};
        ok = Check(ok, "images") &&
             Check(ffxVkFsr3_3_1_6FrameGenerationContextCreate(&create, &context) ==
                       FFX_VK_FSR3_3_1_6_FRAMEGEN_OK, "context create");
        FfxVkFsr3_3_1_6FrameGenerationMemoryUsage usage{};
        if (ok) ffxVkFsr3_3_1_6FrameGenerationContextGetMemoryUsage(context, &usage);
        std::vector<double> gpu, cpu;
        const VkImageLayout read = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        for (uint64_t frame = 1; ok && frame <= 130; ++frame) {
            const Image& src = color[frame & 1];
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            vkBeginCommandBuffer(cmd, &begin);
            const VkImageLayout from = frame <= 2 ? VK_IMAGE_LAYOUT_UNDEFINED : read;
            Fill(cmd, src, (frame % 7) / 7.0f, from, read);
            if (frame == 1) {
                Fill(cmd, depth, 0.5f, VK_IMAGE_LAYOUT_UNDEFINED, read);
                Fill(cmd, motion, 0.0f, VK_IMAGE_LAYOUT_UNDEFINED, read);
                Barrier(cmd, output, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            }
            vkCmdResetQueryPool(cmd, queries, 0, 2);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queries, 0);
            FfxVkFsr3_3_1_6FrameGenerationPrepareInfo prep{};
            prep.commandBuffer = cmd;
            prep.color = Desc(src, read);
            prep.depth = Desc(depth, read);
            prep.motionVectors = Desc(motion, read);
            prep.renderWidth = rw;
            prep.renderHeight = rh;
            prep.motionVectorScaleX = float(rw);
            prep.motionVectorScaleY = float(rh);
            prep.frameTimeMilliseconds = 33.3f;
            prep.maxLuminance = 1.0f;
            prep.cameraNear = 0.1f;
            prep.cameraFar = 10000.0f;
            prep.viewSpaceToMeters = 1.0f;
            prep.cameraVerticalFovRadians = 1.0f;
            prep.cameraUp[1] = prep.cameraRight[0] = 1.0f;
            prep.cameraForward[2] = -1.0f;
            prep.frameId = frame;
            prep.reset = frame == 1;
            FfxVkFsr3_3_1_6FrameGenerationDispatchInfo disp{};
            disp.commandBuffer = cmd;
            disp.color = prep.color;
            disp.output = Desc(output, VK_IMAGE_LAYOUT_GENERAL);
            disp.displayWidth = d[0];
            disp.displayHeight = d[1];
            disp.frameTimeMilliseconds = prep.frameTimeMilliseconds;
            disp.cameraNear = prep.cameraNear;
            disp.cameraFar = prep.cameraFar;
            disp.viewSpaceToMeters = 1.0f;
            disp.cameraVerticalFovRadians = 1.0f;
            disp.maxLuminance = 1.0f;
            disp.frameId = frame;
            disp.reset = prep.reset;
            const auto t0 = std::chrono::steady_clock::now();
            ok = Check(ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(context, &prep) ==
                           FFX_VK_FSR3_3_1_6_FRAMEGEN_OK, "RecordPrepare") &&
                 Check(ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(context, &disp) ==
                           FFX_VK_FSR3_3_1_6_FRAMEGEN_OK, "RecordDispatch");
            const auto t1 = std::chrono::steady_clock::now();
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queries, 1);
            vkEndCommandBuffer(cmd);
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            ok = ok && Check(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS, "submit") &&
                 Check(vkWaitForFences(g_device, 1, &fence, VK_TRUE, 5'000'000'000ull) ==
                           VK_SUCCESS, "fence (device lost?)");
            vkResetFences(g_device, 1, &fence);
            uint64_t ts[2]{};
            vkGetQueryPoolResults(g_device, queries, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            ok = ok && Check(ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(context, frame) ==
                                 FFX_VK_FSR3_3_1_6_FRAMEGEN_OK, "RetireFrame");
            if (frame > 10) { // warm-up: pipeline creation, first-use allocations
                gpu.push_back(double(ts[1] - ts[0]) * props.limits.timestampPeriod * 1e-6);
                cpu.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
        }
        if (ok) {
            std::sort(gpu.begin(), gpu.end());
            std::sort(cpu.begin(), cpu.end());
            const size_t n = gpu.size();
            std::printf("display %ux%u (render %ux%u): gpu p50 %.3f ms p95 %.3f ms max %.3f ms | "
                        "cpu record p50 %.3f ms p95 %.3f ms | fg memory %.1f MiB\n",
                        d[0], d[1], rw, rh, gpu[n / 2], gpu[n * 95 / 100], gpu[n - 1],
                        cpu[n / 2], cpu[n * 95 / 100], usage.totalUsageInBytes / 1048576.0);
        } else {
            status = 1;
        }
        vkDeviceWaitIdle(g_device);
        ffxVkFsr3_3_1_6FrameGenerationContextDestroy(context);
        for (Image* image : {&color[0], &color[1], &depth, &motion, &output})
            if (image->image) DestroyImage(*image);
        if (!ok) break;
    }
    vkDestroyFence(g_device, fence, nullptr);
    vkDestroyQueryPool(g_device, queries, nullptr);
    vkDestroyCommandPool(g_device, pool, nullptr);
    vkDestroyDevice(g_device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return status;
}
