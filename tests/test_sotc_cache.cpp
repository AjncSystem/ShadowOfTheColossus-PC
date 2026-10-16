// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <vulkan/vulkan.h>
#include "common/cache_file.h"
#include "common/sotc_cache_path.h"
#include "video_core/renderer_vulkan/vk_driver_cache.h"

static std::vector<unsigned char> Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: sotc-cache-test <shader.spv> <disposable output directory>\n";
        return 2;
    }
    const auto directory = std::filesystem::path(argv[2]);
    std::filesystem::create_directories(directory);
    const auto path = directory / "driver.vkcache";
    using Common::FS::SotcCachePath;
    assert(SotcCachePath("release-a/user", "", false, "local") ==
           SotcCachePath("release-b/user", "", false, "local"));
    assert(SotcCachePath("release/user", "", true, "local") ==
           std::filesystem::path("release/user/cache"));
    assert(SotcCachePath("release/user", "chosen", true, "local") ==
           std::filesystem::path("chosen"));

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    VkInstance instance{};
    assert(vkCreateInstance(&instance_info, nullptr, &instance) == VK_SUCCESS);
    uint32_t count{};
    assert(vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS && count);
    std::vector<VkPhysicalDevice> devices(count);
    assert(vkEnumeratePhysicalDevices(instance, &count, devices.data()) == VK_SUCCESS);
    VkPhysicalDevice physical = devices[0];
    for (const auto device : devices) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(device, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) physical = device;
    }
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    Vulkan::DriverCache::Identity identity{props.vendorID, props.deviceID, {}};
    std::memcpy(identity.uuid.data(), props.pipelineCacheUUID, 16);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family{};
    while (family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    assert(family < count);
    const float priority = 1;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue;
    VkDevice device{};
    assert(vkCreateDevice(physical, &device_info, nullptr, &device) == VK_SUCCESS);
    VkPipelineCacheCreateInfo cache_info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache cache{};
    assert(vkCreatePipelineCache(device, &cache_info, nullptr, &cache) == VK_SUCCESS);
    const auto shader_bytes = Read(argv[1]);
    assert(!shader_bytes.empty() && shader_bytes.size() % 4 == 0);
    std::vector<uint32_t> code(shader_bytes.size() / 4);
    std::memcpy(code.data(), shader_bytes.data(), shader_bytes.size());
    VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = shader_bytes.size();
    module_info.pCode = code.data();
    VkShaderModule module{};
    assert(vkCreateShaderModule(device, &module_info, nullptr, &module) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout{};
    assert(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout) == VK_SUCCESS);
    VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.layout = layout;
    pipeline_info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = module;
    pipeline_info.stage.pName = "main";
    VkPipeline pipeline{};
    assert(vkCreateComputePipelines(device, cache, 1, &pipeline_info, nullptr, &pipeline) == VK_SUCCESS);
    size_t size{};
    assert(vkGetPipelineCacheData(device, cache, &size, nullptr) == VK_SUCCESS);
    std::vector<unsigned char> bytes(size);
    assert(vkGetPipelineCacheData(device, cache, &size, bytes.data()) == VK_SUCCESS);
    bytes.resize(size);
    assert(Vulkan::DriverCache::Matches(bytes, identity));
    const auto encoded = Vulkan::DriverCache::Encode(bytes);
    assert(Common::FS::WriteCacheFile(path, encoded));
    assert(Read(path) == encoded);
    const auto decoded = Vulkan::DriverCache::Decode(Read(path), identity);
    assert(decoded == bytes);
    auto corrupt = encoded;
    corrupt.back() ^= 0x80;
    assert(Vulkan::DriverCache::Decode(corrupt, identity).empty());
    auto bad_header = bytes;
    bad_header[0] = 33;
    assert(Vulkan::DriverCache::Decode(Vulkan::DriverCache::Encode(bad_header), identity).empty());
    assert(Vulkan::DriverCache::Decode(std::span(encoded).first(encoded.size() - 1), identity).empty());
    auto other = identity;
    other.vendor ^= 1;
    assert(Vulkan::DriverCache::Decode(encoded, other).empty());
    other = identity;
    other.uuid[0] ^= 1;
    assert(Vulkan::DriverCache::Decode(encoded, other).empty());
    // A second publication must replace an existing file on Windows.
    assert(Common::FS::WriteCacheFile(path, std::span(encoded).first(8)));
    assert(Read(path).size() == 8);
    assert(Common::FS::WriteCacheFile(path, encoded));
    cache_info.initialDataSize = decoded.size();
    cache_info.pInitialData = decoded.data();
    VkPipelineCache restored{};
    assert(vkCreatePipelineCache(device, &cache_info, nullptr, &restored) == VK_SUCCESS);
    VkPipeline second{};
    assert(vkCreateComputePipelines(device, restored, 1, &pipeline_info, nullptr, &second) == VK_SUCCESS);
    vkDestroyPipeline(device, second, nullptr);
    vkDestroyPipelineCache(device, restored, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyShaderModule(device, module, nullptr);
    vkDestroyPipelineCache(device, cache, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    std::cout << "Vulkan cache roundtrip, corruption/device rejection, replacement and paths passed: "
              << props.deviceName << ", " << bytes.size() << " bytes\n";
}
