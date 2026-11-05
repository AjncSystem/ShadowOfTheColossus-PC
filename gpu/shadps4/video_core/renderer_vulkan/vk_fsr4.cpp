// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_fsr4.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../../../third_party/amd-fsr-sdk/api/include/vk/ffx_api_vk.h"
#include "../../../../third_party/amd-fsr-sdk/upscalers/include/ffx_upscale.h"

#include "video_core/renderer_vulkan/vk_instance.h"

namespace Vulkan::Fsr4 {

namespace {

struct Api {
    HMODULE dll{};
    PfnFfxCreateContext create{};
    PfnFfxDestroyContext destroy{};
    PfnFfxQuery query{};
    PfnFfxDispatch dispatch{};
} api;

VkDeviceContext device_context{};
bool available = false;
u64 version_id = 0; ///< SOTC_FSR_VERSION's pick; 0 = the SDK's default (the newest for this GPU)
std::string name = "FSR";
std::atomic<const char*> name_ptr{"FSR"};

ffxContext context{};
u32 ctx_render_w = 0, ctx_render_h = 0, ctx_out_w = 0, ctx_out_h = 0;
bool ctx_inverted = false;
bool ctx_failed = false; ///< the last create failed for this size: not retried every frame
std::chrono::steady_clock::time_point last_dispatch{};

void OnMessage(uint32_t type, const wchar_t* message) {
    static u32 count = 0;
    if (count++ < 50) {
        std::fprintf(stderr, "SOTC FSR: %s: %ls\n",
                     type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", message);
    }
}

/// "AMD FSR 4.1.2 ..." -> "FSR 4.1.2" (the label shown in the overlay).
std::string ShortName(const char* version_name) {
    std::string s = version_name ? version_name : "";
    const size_t digit = s.find_first_of("0123456789");
    if (digit == std::string::npos) {
        return s.empty() ? "FSR" : s;
    }
    size_t end = digit;
    while (end < s.size() && (std::isdigit(static_cast<unsigned char>(s[end])) || s[end] == '.')) {
        ++end;
    }
    return "FSR " + s.substr(digit, end - digit);
}

FfxApiResource Resource(const Image& image, bool depth, bool output) {
    FfxApiResource r{};
    r.resource = static_cast<VkImage>(image.image);
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffxApiGetSurfaceFormatVK(static_cast<VkFormat>(image.format));
    r.description.width = image.width;
    r.description.height = image.height;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = output  ? FFX_API_RESOURCE_USAGE_UAV
                          : depth ? FFX_API_RESOURCE_USAGE_DEPTHTARGET
                                  : FFX_API_RESOURCE_USAGE_READ_ONLY;
    r.state = output ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS
                     : FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    return r;
}

void DestroyContext() {
    if (context) {
        api.destroy(&context, nullptr);
        context = nullptr;
    }
}

bool CreateContext(const Params& p) {
    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    // The scene is tonemapped (0..1, as DLSS gets it): no HDR, no auto exposure.
    desc.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    if (p.depth_inverted) {
        desc.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    }
    if (const char* v = std::getenv("SOTC_FSR_DEBUG"); v && v[0] == '1') {
        desc.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    }
    desc.maxRenderSize = {p.render_width, p.render_height};
    desc.maxUpscaleSize = {p.out_width, p.out_height};
    desc.fpMessage = OnMessage;

    ffxCreateContextDescUpscaleVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.version = FFX_UPSCALER_VERSION;
    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext = device_context;
    ffxOverrideVersion override_version{};
    override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    override_version.versionId = version_id;
    desc.header.pNext = &version.header;
    version.header.pNext = &backend.header;
    if (version_id) {
        backend.header.pNext = &override_version.header;
    }
    const ffxReturnCode_t r = api.create(&context, &desc.header, nullptr);
    if (r != FFX_API_RETURN_OK) {
        context = nullptr;
        std::fprintf(stderr, "SOTC FSR: context %ux%u -> %ux%u failed (%u)\n", p.render_width,
                     p.render_height, p.out_width, p.out_height, r);
        return false;
    }
    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (api.query(&context, &provider.header) == FFX_API_RETURN_OK && provider.versionName) {
        name = ShortName(provider.versionName);
        name_ptr.store(name.c_str(), std::memory_order_relaxed);
    }
    std::fprintf(stderr, "SOTC FSR: %s upscaling %ux%u -> %ux%u (depth %s)\n", name.c_str(),
                 p.render_width, p.render_height, p.out_width, p.out_height,
                 p.depth_inverted ? "inverted" : "normal");
    return true;
}

} // namespace

bool Init(const Instance& instance) {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? std::wstring(L".") : dir.substr(0, slash);
    const std::wstring loader = dir + L"\\amd_fidelityfx_loader_vk.dll";
    api.dll = LoadLibraryExW(loader.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!api.dll) {
        std::fprintf(stderr, "SOTC FSR: amd_fidelityfx_loader_vk.dll not found next to "
                             "bb-probe.exe (error %lu), FSR 1 instead\n",
                     GetLastError());
        return false;
    }
    api.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(api.dll, "ffxCreateContext"));
    api.destroy =
        reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(api.dll, "ffxDestroyContext"));
    api.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(api.dll, "ffxQuery"));
    api.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(api.dll, "ffxDispatch"));
    if (!api.create || !api.destroy || !api.query || !api.dispatch) {
        std::fprintf(stderr, "SOTC FSR: amd_fidelityfx_loader_vk.dll has no FSR API\n");
        return false;
    }
    device_context.vkDevice = static_cast<VkDevice>(instance.GetDevice());
    device_context.vkPhysicalDevice = static_cast<VkPhysicalDevice>(instance.GetPhysicalDevice());
    device_context.vkDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

    // The versions this GPU runs, newest first; SOTC_FSR_VERSION=3 (or 4, 3.1.5...) picks the
    // first whose name has it (comparisons on an RDNA card).
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versions.device = &device_context;
    u64 count = 0;
    versions.outputCount = &count;
    if (api.query(nullptr, &versions.header) != FFX_API_RETURN_OK || count == 0) {
        std::fprintf(stderr, "SOTC FSR: no FSR upscaler for this GPU, FSR 1 instead\n");
        return false;
    }
    std::vector<u64> ids(count);
    std::vector<const char*> names(count);
    versions.versionIds = ids.data();
    versions.versionNames = names.data();
    if (api.query(nullptr, &versions.header) != FFX_API_RETURN_OK) {
        return false;
    }
    const char* wanted = std::getenv("SOTC_FSR_VERSION");
    std::string list;
    for (u64 i = 0; i < count; ++i) {
        const char* version_name = names[i] ? names[i] : "?";
        list += (i ? ", " : "") + std::string(version_name);
        if (wanted && *wanted && !version_id && std::strstr(version_name, wanted)) {
            version_id = ids[i];
            name = ShortName(version_name);
        }
    }
    if (!version_id) {
        name = ShortName(names[0]);
    }
    name_ptr.store(name.c_str(), std::memory_order_relaxed);
    std::fprintf(stderr, "SOTC FSR: AMD FSR SDK loaded, versions for this GPU: %s; using %s\n",
                 list.c_str(), name.c_str());
    available = true;
    return true;
}

bool Available() {
    return available;
}

const char* Name() {
    return name_ptr.load(std::memory_order_relaxed);
}

bool Dispatch(VkCommandBuffer cmdbuf, const Params& p) {
    if (!available) {
        return false;
    }
    const bool same = context && ctx_render_w == p.render_width &&
                      ctx_render_h == p.render_height && ctx_out_w == p.out_width &&
                      ctx_out_h == p.out_height && ctx_inverted == p.depth_inverted;
    if (!same) {
        const bool size_changed = ctx_render_w != p.render_width ||
                                  ctx_render_h != p.render_height || ctx_out_w != p.out_width ||
                                  ctx_out_h != p.out_height || ctx_inverted != p.depth_inverted;
        if (ctx_failed && !size_changed) {
            return false;
        }
        if (context) {
            // The old context may still be in use by submitted frames (a rare event: a size or
            // depth layout change).
            static_cast<void>(VULKAN_HPP_DEFAULT_DISPATCHER.vkDeviceWaitIdle(device_context.vkDevice));
            DestroyContext();
        }
        ctx_render_w = p.render_width;
        ctx_render_h = p.render_height;
        ctx_out_w = p.out_width;
        ctx_out_h = p.out_height;
        ctx_inverted = p.depth_inverted;
        ctx_failed = !CreateContext(p);
        if (ctx_failed) {
            return false;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    float frame_ms = 16.7f;
    if (last_dispatch.time_since_epoch().count() != 0) {
        frame_ms = std::clamp(
            std::chrono::duration<float, std::milli>(now - last_dispatch).count(), 1.0f, 100.0f);
    }
    last_dispatch = now;

    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cmdbuf;
    d.color = Resource(p.color, false, false);
    d.depth = Resource(p.depth, true, false);
    d.motionVectors = Resource(p.mvec, false, false);
    d.output = Resource(p.output, false, true);
    d.jitterOffset = {p.jitter_x, p.jitter_y};
    d.motionVectorScale = {p.mvec_scale_x, p.mvec_scale_y};
    d.renderSize = {p.render_width, p.render_height};
    d.upscaleSize = {p.out_width, p.out_height};
    d.enableSharpening = false; // the presenter's RCAS sharpens (SOTC_DLSS_SHARPEN)
    d.sharpness = 0.0f;
    d.frameTimeDelta = frame_ms;
    d.preExposure = 1.0f;
    d.reset = p.reset;
    d.cameraNear = p.z_near;
    d.cameraFar = std::min(p.z_far, 1.0e6f);
    d.cameraFovAngleVertical = p.fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    const ffxReturnCode_t r = api.dispatch(&context, &d.header);
    if (r != FFX_API_RETURN_OK) {
        static u32 errors = 0;
        if (errors++ < 5) {
            std::fprintf(stderr, "SOTC FSR: dispatch failed (%u)\n", r);
        }
        return false;
    }
    return true;
}

} // namespace Vulkan::Fsr4
