// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_fsr4.h"

// bbport: AMD ships the SDK's upscaler as Windows DLLs only; elsewhere Init fails and FSR 1
// upscales instead.
#ifdef _WIN32
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

#else

// sotc: native FSR 4 outside Windows — see vk_fsr4.h. AMD ships FSR 4.1 only as Windows PE DLLs,
// so this drives the vendored portable "source-v07" provider (third_party/fsr-vulkan, MIT): the
// same FSR 4 INT8/DOT4 network, replayed as native Vulkan SPIR-V from an asset bundle on disk.
//
// The provider's per-frame contract is strict: re-register the four external image states (its
// cbUnregisterResources drops those records at the end of every dispatch), BeginFrame(id),
// Dispatch (which records and executes the 14 jobs itself), then RetireFrame(id) once the GPU has
// finished that work. At most three frames may be unretired, so retirement follows the scheduler
// tick Dispatch is given in Params and never frame age.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include <ffx_vk_fsr4_v07.h>
#include <ffx_vk_fsr4_v07_assets.h>

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan::Fsr4 {

namespace {

using Clock = std::chrono::steady_clock;
using u8 = std::uint8_t;

const Instance* g_instance{};
std::string asset_dir;
bool available = false;
std::string name = "FSR 4 v07";
std::atomic<const char*> name_ptr{"FSR 4 v07"};

// Backend and provider contexts. Both are bound to one asset tier (chosen by the output extent)
// and one model preset (chosen by the upscale ratio), so a size change recreates them.
FfxInterface backend{};
bool backend_ready = false;
std::vector<u8> scratch; ///< the backend's bookkeeping memory; must outlive the backend
ffxContext ctx{};
u32 ctx_render_w{}, ctx_render_h{}, ctx_out_w{}, ctx_out_h{};
bool ctx_failed = false;
bool features_warned = false;

u64 next_id = 1; ///< provider frame ids are host-defined and must strictly increase
std::deque<std::pair<u64, u64>> in_flight; ///< frame id, scheduler tick
Scheduler* submit_scheduler{};
// Dispatch queues and retires from the recording thread while teardown drains from whichever
// thread destroys the renderer, so the lifetime bookkeeping is guarded. The backend itself is only
// ever touched from Dispatch and from teardown, which needs no lock.
std::mutex lifetime_mutex;
Clock::time_point last_dispatch{};

// ── asset bundle ───────────────────────────────────────────────────────────

std::string ExeDir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return {};
    }
    buf[n] = '\0';
    const std::string path(buf, static_cast<size_t>(n));
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

/// SOTC_FSR4_ASSETS, then next to the executable, then the per-user data dir. The bundle is 44 MB
/// of generated model payload that is deliberately not part of the repository, so it is looked up
/// at run time rather than compiled in.
std::string FindAssetDir() {
    if (const char* env = std::getenv("SOTC_FSR4_ASSETS"); env && *env) {
        return env;
    }
    const char* home = std::getenv("HOME");
    const std::string candidates[] = {
        ExeDir() + "/fsr4_shaders",
        // Packaged layouts put the executable one level below the assets: the AppImage installs
        // bin/bb-probe with the bundle beside bin/, so neither the sibling nor the per-user
        // directory (a different data dir there) would find it and FSR 4 silently became FSR 1.
        ExeDir() + "/../fsr4_shaders",
        home && *home ? std::string(home) + "/.local/share/sotc-pc/fsr4_shaders" : std::string(),
    };
    for (const std::string& dir : candidates) {
        // rcas.spv is tier- and preset-independent, so it proves the directory is a bundle.
        if (!dir.empty() && ::access((dir + "/rcas.spv").c_str(), R_OK) == 0) {
            return dir;
        }
    }
    return {};
}

bool ReadFile(const std::string& path, std::vector<u8>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(len));
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (got != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

/// The bundle names the 12 model passes after themselves ("fsr4_model_v07_i8_pass7") while pre,
/// post, rcas and spd_auto_exposure are all "main". Asking RADV for an entry point a module does
/// not have segfaults the driver instead of returning an error, so read the name from the SPIR-V.
bool SpirvEntryPoint(const std::vector<u8>& spv, std::string& out) {
    if (spv.size() < 5 * sizeof(u32)) {
        return false;
    }
    const u32* words = reinterpret_cast<const u32*>(spv.data());
    const size_t nwords = spv.size() / sizeof(u32);
    if (words[0] != 0x07230203u) {
        return false;
    }
    for (size_t i = 5; i < nwords;) {
        const size_t word_count = words[i] >> 16;
        const u32 opcode = words[i] & 0xFFFFu;
        if (word_count < 1 || i + word_count > nwords) {
            return false;
        }
        if (opcode == 15 /* OpEntryPoint */ && word_count >= 4) {
            out.assign(reinterpret_cast<const char*>(&words[i + 3]));
            return !out.empty();
        }
        i += word_count;
    }
    return false;
}

// ── formats, aspects and resource descriptors ──────────────────────────────

VkImageAspectFlags AspectFor(vk::Format format) {
    switch (format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eX8D24UnormPack32:
    case vk::Format::eD32Sfloat:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case vk::Format::eS8Uint:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    case vk::Format::eD16UnormS8Uint:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32SfloatS8Uint:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

/// The v07 FfxSurfaceFormat enum has no depth or stencil members and cbRegisterResource rejects a
/// format the backend cannot map, while the backend never reads the stored format back (the real
/// format travels in the view). Depth and stencil are therefore described as R32_FLOAT. UNKNOWN
/// means "the backend would reject this": the caller fails the frame instead of lying about it.
u32 SurfaceFormat(vk::Format format) {
    switch (format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eX8D24UnormPack32:
    case vk::Format::eD32Sfloat:
    case vk::Format::eD16UnormS8Uint:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32SfloatS8Uint:
    case vk::Format::eR32Sfloat:          return FFX_SURFACE_FORMAT_R32_FLOAT;
    case vk::Format::eR32G32B32A32Sfloat: return FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT;
    case vk::Format::eR16G16B16A16Sfloat: return FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case vk::Format::eR16G16Sfloat:       return FFX_SURFACE_FORMAT_R16G16_FLOAT;
    case vk::Format::eR16Sfloat:          return FFX_SURFACE_FORMAT_R16_FLOAT;
    case vk::Format::eR8G8B8A8Unorm:      return FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case vk::Format::eB10G11R11UfloatPack32: return FFX_SURFACE_FORMAT_R11G11B10_FLOAT;
    default:                              return FFX_SURFACE_FORMAT_UNKNOWN;
    }
}

FfxApiResource Resource(const Image& img, u32 format, bool output) {
    FfxApiResource r{};
    r.resource = static_cast<VkImageView>(img.view);
    r.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = format;
    r.description.width = img.width;
    r.description.height = img.height;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.state = output ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS
                     : FFX_API_RESOURCE_STATE_COMPUTE_READ;
    return r;
}

/// The state the port leaves an image in at the hand-off, and the state it expects back: the
/// inputs in ShaderReadOnlyOptimal and the output in General (UpscaleFsr barriers it there first
/// and to ShaderReadOnlyOptimal afterwards). The stages are deliberately every-stage, as
/// vk_fsrfg.cpp does for the same hand-off, because the producers are copies and compute.
FfxFsr4VkExternalImageState ExternalState(const Image& img, bool output) {
    FfxFsr4VkExternalImageState s{};
    s.structSize = sizeof(s);
    s.image = static_cast<VkImage>(img.image);
    s.view = static_cast<VkImageView>(img.view);
    s.layout = output ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    s.stageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    s.accessMask = output ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
    s.restoreLayout = s.layout;
    s.restoreStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    s.restoreAccessMask = s.accessMask;
    // The backend's barriers used to name VK_IMAGE_ASPECT_COLOR_BIT unconditionally, which is
    // invalid for a depth import and leaves the real aspects in their old layout while the
    // descriptor says GENERAL; the record therefore carries the aspects to transition.
    const VkImageAspectFlags aspects = AspectFor(img.format);
    // FSR 4 samples depth only (t2, the low-res depth) and the port's view names the depth aspect
    // alone, so that is all the transition covers. It is also what keeps the barrier valid with
    // separateDepthStencilLayouts on, where a combined DEPTH|STENCIL barrier would demand that
    // both aspects already share one layout.
    s.aspectMask =
        (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0 ? VK_IMAGE_ASPECT_DEPTH_BIT : aspects;
    return s;
}

// ── frame lifetime ─────────────────────────────────────────────────────────

/// Retires the frames whose commands the GPU has finished, without blocking.
void RetireCompleted() {
    std::lock_guard lk(lifetime_mutex);
    while (!in_flight.empty() && submit_scheduler) {
        const auto [id, tick] = in_flight.front();
        if (!submit_scheduler->IsFree(tick)) {
            break;
        }
        ffxFsr4VkRetireFrame(&backend, id);
        in_flight.pop_front();
    }
}

/// Drains every outstanding frame, waiting for the GPU. The wait runs with the lock released so a
/// concurrent Dispatch on another thread cannot deadlock against it.
///
/// This waits on the timeline semaphore directly rather than through Scheduler::Wait: that one
/// calls Flush() (and WaitSubmitted, which kicks the recorder) when the tick is not current yet,
/// and Dispatch runs *inside* a Scheduler::RecordDeferred — flushing there would submit the
/// command buffer that is still being recorded. Only frames queued by an earlier Dispatch are ever
/// waited on (the current one is queued after BeginFrame succeeds), and the flush carrying those
/// has already happened, so a plain semaphore wait is both sufficient and safe.
void RetireAll() {
    for (;;) {
        u64 id = 0, tick = 0;
        Semaphore* semaphore = nullptr;
        {
            std::lock_guard lk(lifetime_mutex);
            if (in_flight.empty() || !submit_scheduler) {
                return;
            }
            id = in_flight.front().first;
            tick = in_flight.front().second;
            semaphore = submit_scheduler->GetWorkSemaphore();
        }
        // Bounded rather than Semaphore::Wait: this runs on the recording thread, and a tick whose
        // submission is still queued behind the work being recorded would never signal, so an
        // unbounded wait could wedge the recorder. Give up and leave the frame unretired; the next
        // Dispatch retries, and BeginFrame declining a fourth frame only skips one upscale.
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        while (!semaphore->IsFree(tick)) {
            if (Clock::now() >= deadline) {
                std::fprintf(stderr, "SOTC FSR: frame %llu not finished after 2 s, not retired\n",
                             static_cast<unsigned long long>(id));
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        {
            std::lock_guard lk(lifetime_mutex);
            if (in_flight.empty() || in_flight.front().second != tick) {
                return;
            }
            ffxFsr4VkRetireFrame(&backend, id);
            in_flight.pop_front();
        }
    }
}

void DestroyContext() {
    RetireAll();
    if (ctx) {
        ffxFsr4V07DestroyContext(&ctx, nullptr);
        ctx = nullptr;
    }
    if (backend_ready && backend.device) {
        ffxFsr4VkDestroyContext(static_cast<FfxFsr4VkContext*>(backend.device));
    }
    backend_ready = false;
    std::memset(&backend, 0, sizeof(backend));
    scratch.clear();
    scratch.shrink_to_fit();
}

bool CreateContext(const Params& p) {
    // The provider picks the preset from the upscale ratio and the tier from the output extent.
    const FfxFsr4ModelPreset preset =
        ffxFsr4SelectModelPreset(p.render_width, p.out_width, false);
    FfxFsr4V07AssetSet assets{};
    if (!ffxFsr4V07BuildAssetSet(preset, p.out_width, p.out_height, &assets)) {
        std::fprintf(stderr, "SOTC FSR: no v07 asset set for %ux%u -> %ux%u (tier limit %ux%u)\n",
                     p.render_width, p.render_height, p.out_width, p.out_height,
                     FFX_FSR4_DOT4_MAX_OUTPUT_WIDTH, FFX_FSR4_DOT4_MAX_OUTPUT_HEIGHT);
        return false;
    }
    // sotc: above this output size the dispatch hangs the GPU inside the game (the semaphore stops
    // advancing and the picture freezes at 0 FPS) even though the same 1600x900 -> 3840x2160 run is
    // correct for 400 frames in the standalone harness, and FSR 1 handles 4K in the game fine. Not
    // root-caused; a hung GPU is unrecoverable without killing the process, so decline the frame
    // and let the caller fall back to FSR 1 instead. Verified good: 1920x1080 and 2560x1440.
    // SOTC_FSR4_MAX_PIXELS overrides the limit (0 removes it) for testing a larger output.
    static const u64 max_pixels = [] {
        const char* v = std::getenv("SOTC_FSR4_MAX_PIXELS");
        if (!v || !*v) {
            return u64{2560} * 1440;
        }
        const unsigned long long parsed = std::strtoull(v, nullptr, 10);
        return parsed == 0 ? ~u64{0} : static_cast<u64>(parsed);
    }();
    if (static_cast<u64>(p.out_width) * p.out_height > max_pixels) {
        std::fprintf(stderr,
                     "SOTC FSR: %ux%u output is above the %llu pixels verified for the v07 "
                     "provider in game (it hangs the GPU there), FSR 1 instead\n",
                     p.out_width, p.out_height, static_cast<unsigned long long>(max_pixels));
        return false;
    }
    const char* names[FFX_FSR4_VK_PASS_COUNT];
    names[0] = assets.pre;
    for (u32 i = 0; i < FFX_FSR4_MODEL_PASS_COUNT; ++i) {
        names[1 + i] = assets.model[i];
    }
    names[13] = assets.post;
    names[14] = assets.rcas;
    names[15] = assets.spdAutoExposure;

    std::vector<std::vector<u8>> spv(FFX_FSR4_VK_PASS_COUNT);
    std::vector<std::string> entry(FFX_FSR4_VK_PASS_COUNT);
    FfxFsr4VkShaderBlob blobs[FFX_FSR4_VK_PASS_COUNT]{};
    for (u32 i = 0; i < FFX_FSR4_VK_PASS_COUNT; ++i) {
        const std::string path = asset_dir + "/" + names[i];
        if (!ReadFile(path, spv[i]) || !SpirvEntryPoint(spv[i], entry[i])) {
            std::fprintf(stderr, "SOTC FSR: cannot load %s (looked in SOTC_FSR4_ASSETS, "
                                 "<exe dir>/fsr4_shaders, ~/.local/share/sotc-pc/fsr4_shaders), "
                                 "FSR 1 instead\n",
                         path.c_str());
            return false;
        }
        blobs[i].spirv = reinterpret_cast<const u32*>(spv[i].data());
        blobs[i].sizeBytes = spv[i].size();
        blobs[i].entryPoint = entry[i].c_str();
    }
    std::vector<u8> init, weights;
    if (!ReadFile(asset_dir + "/" + assets.initializer, init) ||
        !ReadFile(asset_dir + "/" + assets.prePassWeights, weights) ||
        init.size() != FFX_FSR4_V07_INITIALIZER_BYTES ||
        weights.size() != FFX_FSR4_V07_PRE_PASS_WEIGHTS_BYTES) {
        std::fprintf(stderr, "SOTC FSR: %s model data is missing or the wrong size in %s\n",
                     ffxFsr4ModelPresetName(preset), asset_dir.c_str());
        return false;
    }

    // The backend deep-copies the SPIR-V and both model blobs, so they may go when this returns.
    scratch.assign(ffxFsr4VkGetScratchMemorySize(), 0);
    FfxFsr4VkCreateInfo bci{};
    bci.device = static_cast<VkDevice>(g_instance->GetDevice());
    bci.physicalDevice = static_cast<VkPhysicalDevice>(g_instance->GetPhysicalDevice());
    bci.allocator = nullptr;
    bci.modelInitializer = init.data();
    bci.modelInitializerSize = init.size();
    bci.prePassWeights = weights.data();
    bci.prePassWeightsSize = weights.size();
    bci.scratchBuffer = scratch.data();
    bci.scratchBufferSize = scratch.size();
    std::memcpy(bci.shaders, blobs, sizeof(blobs));
    if (const VkResult r = ffxFsr4VkCreateContext(&bci, &backend); r != VK_SUCCESS) {
        if (!features_warned) {
            features_warned = true;
            std::fprintf(stderr,
                         "SOTC FSR: the v07 backend cannot run on this GPU (%d) — it needs "
                         "shaderInt8, shaderFloat16, 16-bit storage, shaderIntegerDotProduct and "
                         "linear sampling of RGBA16F/RGBA8/R32F. FSR 1 instead\n",
                         static_cast<int>(r));
        }
        scratch.clear();
        return false;
    }
    backend_ready = true;

    ffxFsr4V07SetBackendInterface(&backend);
    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    // The scene is tonemapped (0..1, as DLSS gets it): no HDR, no auto exposure.
    desc.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    if (p.depth_inverted) {
        desc.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    }
    desc.maxRenderSize.width = p.render_width;
    desc.maxRenderSize.height = p.render_height;
    desc.maxUpscaleSize.width = p.out_width;
    desc.maxUpscaleSize.height = p.out_height;
    if (const ffxReturnCode_t created = ffxFsr4V07CreateContext(&ctx, &desc.header, nullptr);
        created != FFX_API_RETURN_OK || !ctx) {
        std::fprintf(stderr, "SOTC FSR: v07 context %ux%u -> %ux%u failed (%u)\n", p.render_width,
                     p.render_height, p.out_width, p.out_height, created);
        DestroyContext();
        return false;
    }
    std::fprintf(stderr, "SOTC FSR: %s upscaling %ux%u -> %ux%u (%s preset, %s tier, depth %s)\n",
                 name.c_str(), p.render_width, p.render_height, p.out_width, p.out_height,
                 ffxFsr4ModelPresetName(preset), assets.tier,
                 p.depth_inverted ? "inverted" : "normal");
    return true;
}

bool EnsureContext(const Params& p) {
    const bool same = ctx && ctx_render_w == p.render_width && ctx_render_h == p.render_height &&
                      ctx_out_w == p.out_width && ctx_out_h == p.out_height;
    if (same) {
        return true;
    }
    // As the Windows path: a failure is remembered for this size only, so a resolution change
    // (e.g. 4K, which is declined, back down to 1440p) gets another attempt instead of leaving
    // FSR 4 off for the rest of the session.
    const bool size_changed =
        ctx_render_w != p.render_width || ctx_render_h != p.render_height ||
        ctx_out_w != p.out_width || ctx_out_h != p.out_height;
    if (ctx_failed && !size_changed) {
        return false;
    }
    if (ctx || backend_ready) {
        // A rare event (a resolution or upscale-ratio change): the old context's descriptor pools
        // and constant-buffer ring may still be read by submitted frames.
        (void)g_instance->GetDevice().waitIdle();
        DestroyContext();
    }
    ctx_render_w = p.render_width;
    ctx_render_h = p.render_height;
    ctx_out_w = p.out_width;
    ctx_out_h = p.out_height;
    ctx_failed = !CreateContext(p);
    return !ctx_failed;
}

} // namespace

bool Init(const Instance& instance) {
    g_instance = &instance;
    if (!instance.IsComputeDerivativeGroupLinearEnabled()) {
        std::fprintf(stderr, "SOTC FSR: computeDerivativeGroupLinear is not enabled, FSR 1 instead\n");
        return false;
    }
    asset_dir = FindAssetDir();
    if (asset_dir.empty()) {
        std::fprintf(stderr, "SOTC FSR: no FSR 4 asset bundle found (set SOTC_FSR4_ASSETS, or put "
                             "fsr4_shaders next to the executable or in "
                             "~/.local/share/sotc-pc), FSR 1 instead\n");
        return false;
    }
    std::fprintf(stderr, "SOTC FSR: v07 asset bundle %s\n", asset_dir.c_str());
    available = true;
    name_ptr.store(name.c_str(), std::memory_order_relaxed);
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
    RetireCompleted();
    if (!p.color.view || !p.depth.view || !p.mvec.view || !p.output.view) {
        std::fprintf(stderr, "SOTC FSR: an input has no image view (the v07 ABI binds views)\n");
        return false;
    }
    const u32 formats[4] = {
        SurfaceFormat(p.color.format),
        SurfaceFormat(p.depth.format),
        SurfaceFormat(p.mvec.format),
        SurfaceFormat(p.output.format),
    };
    static const char* const slot_names[4] = {"colour", "depth", "motion vector", "output"};
    for (int i = 0; i < 4; ++i) {
        if (formats[i] == FFX_SURFACE_FORMAT_UNKNOWN) {
            std::fprintf(stderr, "SOTC FSR: unsupported %s format %d for the v07 provider\n",
                         slot_names[i], static_cast<int>(i == 0 ? p.color.format
                                                         : i == 1 ? p.depth.format
                                                         : i == 2 ? p.mvec.format
                                                                  : p.output.format));
            return false;
        }
    }
    if (!EnsureContext(p)) {
        return false;
    }
    // cbUnregisterResources (inside Dispatch) drops the import records, so they are re-registered
    // for every frame.
    const Image* const images[4] = {&p.color, &p.depth, &p.mvec, &p.output};
    for (int i = 0; i < 4; ++i) {
        const FfxFsr4VkExternalImageState state = ExternalState(*images[i], i == 3);
        if (const VkResult r = ffxFsr4VkSetExternalImageState(&backend, &state); r != VK_SUCCESS) {
            std::fprintf(stderr, "SOTC FSR: cannot register the %s image %p / view %p (%d)\n",
                         slot_names[i], static_cast<void*>(state.image),
                         static_cast<void*>(state.view), static_cast<int>(r));
            return false;
        }
    }

    const u64 id = next_id++;
    VkResult begun = ffxFsr4VkBeginFrame(&backend, id);
    if (begun == VK_NOT_READY) {
        // All three frame slots are still in flight. Waiting for the oldest is a small stall and
        // keeps FSR 4 running; retiring by frame age instead would be unsafe.
        RetireAll();
        begun = ffxFsr4VkBeginFrame(&backend, id);
    }
    if (begun != VK_SUCCESS) {
        std::fprintf(stderr, "SOTC FSR: no free frame slot (%d)\n", static_cast<int>(begun));
        return false;
    }

    const auto now = Clock::now();
    float frame_ms = 16.7f;
    if (last_dispatch.time_since_epoch().count() != 0) {
        frame_ms = std::clamp(
            std::chrono::duration<float, std::milli>(now - last_dispatch).count(), 1.0f, 100.0f);
    }
    last_dispatch = now;

    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cmdbuf;
    d.color = Resource(p.color, formats[0], false);
    d.depth = Resource(p.depth, formats[1], false);
    d.motionVectors = Resource(p.mvec, formats[2], false);
    d.output = Resource(p.output, formats[3], true);
    d.jitterOffset.x = p.jitter_x;
    d.jitterOffset.y = p.jitter_y;
    d.motionVectorScale.x = p.mvec_scale_x;
    d.motionVectorScale.y = p.mvec_scale_y;
    d.renderSize.width = p.render_width;
    d.renderSize.height = p.render_height;
    d.upscaleSize.width = p.out_width;
    d.upscaleSize.height = p.out_height;
    d.enableSharpening = false; // the presenter's RCAS sharpens (SOTC_DLSS_SHARPEN)
    d.enableAutoExposure = false;
    d.sharpness = 0.0f;
    d.frameTimeDelta = frame_ms;
    d.preExposure = 1.0f;
    d.reset = p.reset;
    d.cameraNear = p.z_near;
    d.cameraFar = std::min(p.z_far, 1.0e6f);
    d.cameraFovAngleVertical = p.fov_y;
    d.viewSpaceToMetersFactor = 1.0f;

    // The frame slot is open now, so it retires even if the dispatch fails: RetireFrame needs the
    // tick of the submission that carries the partially recorded work. Queued here rather than
    // handed to a hook on the submitting thread, because Dispatch runs inside a RecordDeferred and
    // may not have run yet when that thread moves on — a handoff between the two could see the
    // "recorded" flag before it was set, drop the frame id and leak its backend slot.
    {
        std::lock_guard lk(lifetime_mutex);
        submit_scheduler = p.scheduler;
        in_flight.emplace_back(id, p.tick);
    }
    if (const ffxReturnCode_t r = ffxFsr4V07Dispatch(&ctx, &d.header); r != FFX_API_RETURN_OK) {
        static u32 errors = 0;
        if (errors++ < 5) {
            std::fprintf(stderr, "SOTC FSR: v07 dispatch failed (%u)\n", r);
        }
        return false;
    }
    return true;
}

} // namespace Vulkan::Fsr4

#endif
