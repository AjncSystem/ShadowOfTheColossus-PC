// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: DLSS Frame Generation through NVIDIA Streamline, see vk_dlssg.h.

#include "video_core/renderer_vulkan/vk_dlssg.h"
#include "video_core/renderer_vulkan/vk_framegen.h"
#include "video_core/renderer_vulkan/vk_fsrfg.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vk_mem_alloc.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include "core/memory.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/sotc_stats.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan::Dlssg {

namespace {

struct Api {
    HMODULE dll{};
    PFun_slInit* init{};
    PFun_slIsFeatureSupported* is_supported{};
    PFun_slGetFeatureRequirements* requirements{};
    PFun_slGetFeatureFunction* feature_function{};
    PFun_slGetNewFrameToken* frame_token{};
    PFun_slSetConstants* set_constants{};
    PFun_slSetTagForFrame* set_tag{};
    PFun_slDLSSGSetOptions* dlssg_set_options{};
    PFun_slDLSSGGetState* dlssg_get_state{};
    PFun_slReflexSetOptions* reflex_set_options{};
    PFun_slReflexSleep* reflex_sleep{};
    PFun_slPCLSetMarker* pcl_marker{};
} api;

/// The camera of a frame, from the guest's view constants (column vectors: clip = M * p).
struct Camera {
    double vp[4][4];      ///< world -> clip, no jitter
    double prev_vp[4][4]; ///< the previous frame's
    double jitter[2];     ///< NDC offset added to clip.xy / clip.w
    double pos[3];        ///< world position
};

/// What the first G-buffer draw of the frame being processed gave (command processor thread).
struct Pending {
    bool captured{};
    VideoCore::ImageId depth_id, mvec_id;
    u64 depth_uid{}, mvec_uid{};
    Camera cam{};
    float yscale{-1.0f}, zscale{1.0f}, zoffset{0.0f};
};

/// The inputs of one presentation frame (vk_presenter.h Frame, by id).
struct FrameSlot {
    VmaAllocation depth_alloc{}, mvec_alloc{};
    vk::Image depth, mvec;
    vk::ImageView depth_view, mvec_view;
    VkDeviceMemory depth_memory{}, mvec_memory{};
    vk::Format depth_format{}, mvec_format{};
    u32 width{}, height{};
    u32 index{};     ///< Streamline frame index, 0: no game frame
    bool valid{};    ///< depth, motion vectors and constants are set for this frame
    bool sl_valid{}; ///< ... and given to Streamline
    sl::Constants constants{};
    /// The presenter's HUD-less copy of this frame (NoteHudless), null when it has none.
    struct Hudless {
        vk::Image image;
        vk::ImageView view;
        vk::Format format{};
        u32 width{}, height{};
    } hudless;
    /// Streamline's timeline semaphore and value: DLSS-G is done reading this frame's inputs.
    VkSemaphore inputs_done{};
    u64 inputs_done_value{};
};

constexpr u32 kMaxFrames = 16;
constexpr u64 kMask48 = 0xFFFFFFFFFFFFull;

bool loaded = false;    // sl.interposer.dll loaded, slInit done
bool supported = false; // DLSS-G supported on the device
bool vsync_off = true;  // eVSyncOffRequired
int log_level = 0;      // SOTC_DLSSG_LOG
const Instance* g_instance{};
Pending pending;                     // command processor thread
std::array<FrameSlot, kMaxFrames> frames;
u32 next_index = 0;                  // command processor thread
bool have_prev = false;              // command processor thread
double prev_pos[3]{}, prev_fwd[3]{}; // command processor thread
u32 bad_cameras = 0;
// Present thread.
bool fg_on = false;
bool options_set = false;
u32 valid_streak = 0;
u64 presents = 0;
LARGE_INTEGER present_start{};
double present_ms_sum = 0.0, present_ms_max = 0.0;
u32 present_ms_count = 0;

void LogCallback(sl::LogType type, const char* msg) {
    if (type == sl::LogType::eInfo && log_level < 2) {
        return;
    }
    std::fprintf(stderr, "SOTC DLSSG [sl %s] %s", type == sl::LogType::eError ? "error" :
                 type == sl::LogType::eWarn ? "warn" : "info", msg);
    const size_t n = std::strlen(msg);
    if (n == 0 || msg[n - 1] != '\n') {
        std::fputc('\n', stderr);
    }
}

void OnApiError(const sl::APIError& e) {
    static std::atomic<u32> count{0};
    if (count.fetch_add(1) < 8) {
        std::fprintf(stderr, "SOTC DLSSG present/acquire error %d\n", static_cast<int>(e.vkRes));
    }
}

template <typename T>
bool Resolve(T*& out, const char* name) {
    out = reinterpret_cast<T*>(GetProcAddress(api.dll, name));
    if (!out) {
        std::fprintf(stderr, "SOTC DLSSG: %s missing in sl.interposer.dll\n", name);
    }
    return out != nullptr;
}

template <typename T>
bool ResolveFeature(T*& out, sl::Feature feature, const char* name) {
    void* fn{};
    if (api.feature_function(feature, name, fn) != sl::Result::eOk || !fn) {
        std::fprintf(stderr, "SOTC DLSSG: %s unavailable\n", name);
        return false;
    }
    out = reinterpret_cast<T*>(fn);
    return true;
}

sl::FrameToken* Token(u32 index) {
    sl::FrameToken* token{};
    if (api.frame_token(token, &index) != sl::Result::eOk) {
        return nullptr;
    }
    return token;
}

void Marker(sl::PCLMarker marker, u32 index) {
    if (!api.pcl_marker || index == 0) {
        return;
    }
    if (sl::FrameToken* token = Token(index)) {
        api.pcl_marker(marker, *token);
    }
}

// ---------------------------------------------------------------------------------------------
// Camera math. The game's matrices take column vectors (clip = M * p, M stored by rows).
// Streamline's take row vectors (clip = p * M): a Streamline matrix is the transpose.

using Mat = std::array<std::array<double, 4>, 4>;

Mat Mul(const Mat& a, const Mat& b) {
    Mat r{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) {
                s += a[i][k] * b[k][j];
            }
            r[i][j] = s;
        }
    }
    return r;
}

bool Invert(const Mat& m, Mat& out) {
    // Gauss-Jordan with partial pivoting.
    double a[4][8];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            a[i][j] = m[i][j];
            a[i][j + 4] = i == j ? 1.0 : 0.0;
        }
    }
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r) {
            if (std::abs(a[r][c]) > std::abs(a[p][c])) {
                p = r;
            }
        }
        if (std::abs(a[p][c]) < 1e-12) {
            return false;
        }
        if (p != c) {
            for (int j = 0; j < 8; ++j) {
                std::swap(a[p][j], a[c][j]);
            }
        }
        const double inv = 1.0 / a[c][c];
        for (int j = 0; j < 8; ++j) {
            a[c][j] *= inv;
        }
        for (int r = 0; r < 4; ++r) {
            if (r != c && a[r][c] != 0.0) {
                const double f = a[r][c];
                for (int j = 0; j < 8; ++j) {
                    a[r][j] -= f * a[c][j];
                }
            }
        }
    }
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            out[i][j] = a[i][j + 4];
        }
    }
    return true;
}

sl::float4x4 ToSl(const Mat& column_major_math) {
    // Transposed: row i of the Streamline matrix is column i of ours.
    sl::float4x4 r;
    for (int i = 0; i < 4; ++i) {
        r[i] = sl::float4(static_cast<float>(column_major_math[0][i]),
                          static_cast<float>(column_major_math[1][i]),
                          static_cast<float>(column_major_math[2][i]),
                          static_cast<float>(column_major_math[3][i]));
    }
    return r;
}

double Dot3(const double* a, const double* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

bool Finite(const double* v, int n) {
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(v[i])) {
            return false;
        }
    }
    return true;
}

/// Reads the view constants the G-buffer vertex shaders use (SotC: srt walker of
/// vs_0x79a4f6d5): view constant buffer = *(*(VS user data 0..1) + 8); its world -> clip matrix
/// rows at +0x80, the previous frame's at +0x100 (rows x, y and w only: 0x100, 0x110, 0x130),
/// the TAA jitter (NDC) at +0x140, the camera position at +0x230.
bool ReadCamera(const AmdGpu::Regs& regs, Camera& cam) {
    auto* memory = Core::Memory::Instance();
    const u64 ud = (u64(regs.vs_program.user_data[0]) | (u64(regs.vs_program.user_data[1]) << 32)) &
                   kMask48;
    if (ud == 0) {
        return false;
    }
    u64 cb = 0;
    if (!memory->TryReadBacking(ud + 8, &cb, sizeof(cb))) {
        return false;
    }
    cb &= kMask48;
    if (cb == 0) {
        return false;
    }
    float raw[0x48 / 4]; // 0x100 .. 0x148
    float cur[16], pos[3];
    if (!memory->TryReadBacking(cb + 0x80, cur, sizeof(cur)) ||
        !memory->TryReadBacking(cb + 0x100, raw, sizeof(raw)) ||
        !memory->TryReadBacking(cb + 0x230, pos, sizeof(pos))) {
        return false;
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            cam.vp[r][c] = cur[r * 4 + c];
        }
    }
    // Previous rows x (0x100), y (0x110), w (0x130); z is unused by the shaders and taken from
    // the current matrix (depth is not reprojected).
    for (int c = 0; c < 4; ++c) {
        cam.prev_vp[0][c] = raw[0 + c];
        cam.prev_vp[1][c] = raw[4 + c];
        cam.prev_vp[2][c] = cur[8 + c];
        cam.prev_vp[3][c] = raw[12 + c];
    }
    cam.jitter[0] = raw[16];
    cam.jitter[1] = raw[17];
    for (int i = 0; i < 3; ++i) {
        cam.pos[i] = pos[i];
    }
    if (!Finite(&cam.vp[0][0], 16) || !Finite(&cam.prev_vp[0][0], 16) || !Finite(cam.jitter, 2) ||
        !Finite(cam.pos, 3)) {
        return false;
    }
    // Sanity: the w row is the view direction (unit length up to a scale), and the camera
    // position is at w = 0.
    const double s = std::sqrt(Dot3(cam.vp[3], cam.vp[3]));
    if (s < 1e-3 || s > 1e3) {
        return false;
    }
    const double w_at_eye = Dot3(cam.vp[3], cam.pos) + cam.vp[3][3];
    const double scale = 1.0 + std::abs(cam.vp[3][3]);
    return std::abs(w_at_eye) < 1e-2 * scale;
}

/// Fills Streamline's constants from the camera. False when the matrix is not a perspective
/// projection we understand.
bool MakeConstants(const Pending& p, u32 width, u32 height, sl::Constants& out) {
    Mat m{}, mp{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            m[r][c] = p.cam.vp[r][c];
            mp[r][c] = p.cam.prev_vp[r][c];
        }
    }
    double jitter_y = p.cam.jitter[1];
    // Streamline's clip space has +y up (D3D). A positive viewport y scale maps NDC +y down:
    // flip y so that it does.
    if (p.yscale > 0.0f) {
        for (int c = 0; c < 4; ++c) {
            m[1][c] = -m[1][c];
            mp[1][c] = -mp[1][c];
        }
        jitter_y = -jitter_y;
    }
    // Depth as stored: zscale * z / w + zoffset.
    for (int c = 0; c < 4; ++c) {
        m[2][c] = p.zscale * m[2][c] + p.zoffset * m[3][c];
        mp[2][c] = p.zscale * mp[2][c] + p.zoffset * mp[3][c];
    }

    // Camera basis: forward from the w row, right and up from the x and y rows with their
    // forward part removed (off-centre terms).
    const double s = std::sqrt(Dot3(m[3].data(), m[3].data()));
    double fwd[3] = {m[3][0] / s, m[3][1] / s, m[3][2] / s};
    double right[3], up[3];
    const double cx = Dot3(m[0].data(), fwd), cy = Dot3(m[1].data(), fwd);
    for (int i = 0; i < 3; ++i) {
        right[i] = m[0][i] - cx * fwd[i];
        up[i] = m[1][i] - cy * fwd[i];
    }
    const double sx = std::sqrt(Dot3(right, right)), sy = std::sqrt(Dot3(up, up));
    if (sx < 1e-6 || sy < 1e-6) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        right[i] /= sx;
        up[i] /= sy;
    }
    // Depth: z row = k * w row + (0, 0, 0, b); depth(d) = k + b / (s * d) at view distance d.
    const double k = Dot3(m[2].data(), m[3].data()) / (s * s);
    const double b = (m[2][3] - k * m[3][3]) / s;
    const bool inverted = b > 0.0;
    double z_near, z_far;
    if (inverted) { // depth 1 at the near plane, 0 at the far one (k = 0: infinite)
        z_near = b / (1.0 - k);
        z_far = k < -1e-9 ? -b / k : 1e6;
    } else {
        z_near = std::abs(k) > 1e-9 ? -b / k : 0.1;
        z_far = b / (1.0 - k);
    }
    if (!(z_near > 0.0) || !(z_far > z_near)) {
        return false;
    }

    // View space (x right, y up, z forward) -> world: columns right, up, fwd, pos.
    Mat view_to_world{};
    for (int i = 0; i < 3; ++i) {
        view_to_world[i][0] = right[i];
        view_to_world[i][1] = up[i];
        view_to_world[i][2] = fwd[i];
        view_to_world[i][3] = p.cam.pos[i];
    }
    view_to_world[3][3] = 1.0;
    const Mat view_to_clip = Mul(m, view_to_world);
    Mat clip_to_view, inv_m, inv_mp;
    if (!Invert(view_to_clip, clip_to_view) || !Invert(m, inv_m) || !Invert(mp, inv_mp)) {
        return false;
    }
    const Mat clip_to_prev = Mul(mp, inv_m);
    const Mat prev_to_clip = Mul(m, inv_mp);

    out.cameraViewToClip = ToSl(view_to_clip);
    out.clipToCameraView = ToSl(clip_to_view);
    out.clipToLensClip = ToSl(Mat{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}});
    out.clipToPrevClip = ToSl(clip_to_prev);
    out.prevClipToClip = ToSl(prev_to_clip);
    // Pixel space jitter: NDC x right, y up -> pixels x right, y down.
    out.jitterOffset = sl::float2(static_cast<float>(p.cam.jitter[0] * width * 0.5),
                                  static_cast<float>(-jitter_y * height * 0.5));
    // MRT 4 holds curUV - prevUV (UV units, y down); Streamline wants prev - cur, normalised.
    out.mvecScale = sl::float2(-1.0f, -1.0f);
    out.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    out.cameraPos = sl::float3(static_cast<float>(p.cam.pos[0]), static_cast<float>(p.cam.pos[1]),
                               static_cast<float>(p.cam.pos[2]));
    out.cameraUp = sl::float3(static_cast<float>(up[0]), static_cast<float>(up[1]),
                              static_cast<float>(up[2]));
    out.cameraRight = sl::float3(static_cast<float>(right[0]), static_cast<float>(right[1]),
                                 static_cast<float>(right[2]));
    out.cameraFwd = sl::float3(static_cast<float>(fwd[0]), static_cast<float>(fwd[1]),
                               static_cast<float>(fwd[2]));
    out.cameraNear = static_cast<float>(z_near);
    out.cameraFar = static_cast<float>(z_far);
    out.cameraFOV = static_cast<float>(2.0 * std::atan(s / sy));
    out.cameraAspectRatio = static_cast<float>(sy / sx);
    out.motionVectorsInvalidValue = sl::INVALID_FLOAT;
    out.depthInverted = inverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    out.cameraMotionIncluded = sl::Boolean::eTrue;
    out.motionVectors3D = sl::Boolean::eFalse;
    out.orthographicProjection = sl::Boolean::eFalse;
    out.motionVectorsDilated = sl::Boolean::eFalse;
    out.motionVectorsJittered = sl::Boolean::eFalse;

    // A cut: the camera jumped or turned a lot since the last frame.
    bool reset = !have_prev;
    if (have_prev) {
        const double d[3] = {p.cam.pos[0] - prev_pos[0], p.cam.pos[1] - prev_pos[1],
                             p.cam.pos[2] - prev_pos[2]};
        reset = Dot3(d, d) > 50.0 * 50.0 || Dot3(fwd, prev_fwd) < 0.8;
    }
    out.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    // SOTC_CAM_LOG=1: the camera position every 10 frames with a millisecond clock, for scripted
    // runs (gameplay_bench --horse tells riding from walking by the speed).
    static const bool cam_log = [] {
        const char* v = std::getenv("SOTC_CAM_LOG");
        return v && v[0] == '1';
    }();
    if (cam_log && next_index % 10 == 0) {
        LARGE_INTEGER t, f;
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        std::fprintf(stderr, "SOTC CAM %lld %.2f %.2f %.2f\n",
                     static_cast<long long>(t.QuadPart * 1000 / f.QuadPart), p.cam.pos[0],
                     p.cam.pos[1], p.cam.pos[2]);
    }
    have_prev = true;
    std::memcpy(prev_pos, p.cam.pos, sizeof(prev_pos));
    std::memcpy(prev_fwd, fwd, sizeof(prev_fwd));

    if (log_level >= 1) {
        static u32 logged = 0;
        if (logged < 3 || (logged < 1000 && next_index % 600 == 0)) {
            ++logged;
            std::fprintf(stderr,
                         "SOTC DLSSG camera #%u pos %.2f %.2f %.2f fwd %.3f %.3f %.3f up %.3f %.3f "
                         "%.3f near %.3f far %.1f fov %.1f deg aspect %.3f inverted %d jitter "
                         "%.3f %.3f px reset %d (yscale %.0f zscale %.2f zoffset %.2f)\n",
                         next_index, p.cam.pos[0], p.cam.pos[1], p.cam.pos[2], fwd[0], fwd[1],
                         fwd[2], up[0], up[1], up[2], z_near, z_far,
                         out.cameraFOV * 57.29578, out.cameraAspectRatio, inverted ? 1 : 0,
                         out.jitterOffset.x, out.jitterOffset.y, reset ? 1 : 0, p.yscale, p.zscale,
                         p.zoffset);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------

void DestroyImages(FrameSlot& f) {
    const vk::Device device = g_instance->GetDevice();
    if (f.depth_view) {
        device.destroyImageView(f.depth_view);
    }
    if (f.mvec_view) {
        device.destroyImageView(f.mvec_view);
    }
    if (f.depth) {
        vmaDestroyImage(g_instance->GetAllocator(), f.depth, f.depth_alloc);
    }
    if (f.mvec) {
        vmaDestroyImage(g_instance->GetAllocator(), f.mvec, f.mvec_alloc);
    }
    f = FrameSlot{};
}

bool CreateImage(vk::Format format, u32 width, u32 height, bool depth, vk::Image& image,
                 VmaAllocation& alloc, VkDeviceMemory& memory, vk::ImageView& view) {
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferDst |
                                vk::ImageUsageFlagBits::eTransferSrc |
                                vk::ImageUsageFlagBits::eSampled;
    if (!depth) {
        usage |= vk::ImageUsageFlagBits::eStorage;
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
    const VmaAllocationCreateInfo alloc_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    VkImage raw{};
    VmaAllocationInfo out_info{};
    if (vmaCreateImage(g_instance->GetAllocator(), &info, &alloc_info, &raw, &alloc, &out_info) !=
        VK_SUCCESS) {
        return false;
    }
    image = vk::Image{raw};
    memory = out_info.deviceMemory;
    const vk::ImageViewCreateInfo view_info = {
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto [result, v] = g_instance->GetDevice().createImageView(view_info);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    view = v;
    return true;
}

bool EnsureImages(FrameSlot& f, vk::Format depth_format, vk::Format mvec_format, u32 width,
                  u32 height) {
    if (f.depth && f.depth_format == depth_format && f.mvec_format == mvec_format &&
        f.width == width && f.height == height) {
        return true;
    }
    const u32 index = f.index;
    DestroyImages(f);
    f.index = index;
    if (!CreateImage(depth_format, width, height, true, f.depth, f.depth_alloc, f.depth_memory,
                     f.depth_view) ||
        !CreateImage(mvec_format, width, height, false, f.mvec, f.mvec_alloc, f.mvec_memory,
                     f.mvec_view)) {
        std::fprintf(stderr, "SOTC DLSSG: cannot create the input images %ux%u\n", width, height);
        DestroyImages(f);
        f.index = index;
        return false;
    }
    f.depth_format = depth_format;
    f.mvec_format = mvec_format;
    f.width = width;
    f.height = height;
    return true;
}

bool Parallel() {
    static const bool parallel = [] {
        const char* v = std::getenv("SOTC_DLSSG_PARALLEL");
        return !v || v[0] != '0';
    }();
    return parallel;
}

/// Inputs are captured while DLSS-G or FSR 3 is the frame generation mode.
bool Capturing() {
    switch (FrameGen::Current()) {
    case FrameGen::Mode::Dlss:
        return supported;
    case FrameGen::Mode::Fsr:
        return g_instance != nullptr;
    default:
        return false;
    }
}

// SOTC_DLSSG_ASSUME_FOCUS=1 (background test runs only): DLSS-G pauses while the game window
// is not the foreground window. The scripted benchmarks keep their window behind the user's
// (it must never take the focus), so Streamline's GetForegroundWindow import is pointed at a
// function returning the game window.
HWND game_window{};

BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) {
        game_window = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND WINAPI AssumedForegroundWindow() {
    if (!game_window || !IsWindow(game_window)) {
        game_window = nullptr;
        EnumWindows(FindGameWindow, 0);
    }
    return game_window ? game_window : GetForegroundWindow();
}

void PatchForegroundWindow(const wchar_t* module_name) {
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(module_name));
    if (!base) {
        return;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) {
        return;
    }
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
         imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "USER32.dll") != 0) {
            continue;
        }
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(
            base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        for (; names->u1.AddressOfData; ++names, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            const auto* by_name =
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), "GetForegroundWindow") !=
                0) {
                continue;
            }
            DWORD old = 0;
            VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), PAGE_READWRITE, &old);
            thunk->u1.Function = reinterpret_cast<ULONG_PTR>(&AssumedForegroundWindow);
            VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), old, &old);
            std::fprintf(stderr, "SOTC DLSSG: test mode, game window assumed focused (%ls)\n",
                         module_name);
        }
    }
}

void SetOptions(bool on) {
    sl::DLSSGOptions options{};
    options.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    options.numFramesToGenerate = 1;
    options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    // SOTC_DLSSG_ONLY_GENERATED=1: only the generated frames are shown (checking them).
    static const bool only_generated = [] {
        const char* v = std::getenv("SOTC_DLSSG_ONLY_GENERATED");
        return v && v[0] == '1';
    }();
    if (only_generated) {
        options.flags = options.flags | sl::DLSSGFlags::eShowOnlyInterpolatedFrame;
    }
    options.onErrorCallback = OnApiError;
    // The game's queue is not held while DLSS-G runs (SOTC_DLSSG_PARALLEL=0: it is); a frame's
    // input images are rewritten only once Streamline has read them (PrepareInputs).
    if (Parallel()) {
        options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockNoClientQueues;
    }
    const sl::Result r = api.dlssg_set_options(sl::ViewportHandle{0}, options);
    if (r != sl::Result::eOk) {
        std::fprintf(stderr, "SOTC DLSSG: slDLSSGSetOptions(%s) failed (%d)\n", on ? "on" : "off",
                     static_cast<int>(r));
    } else if (log_level >= 1 || !options_set) {
        std::fprintf(stderr, "SOTC DLSSG: frame generation %s\n", on ? "on" : "off");
    }
    fg_on = on;
    options_set = true;
    SotcStats::ov_fg_state.store(on ? 1 : 0, std::memory_order_relaxed);
}

} // namespace

PFN_vkGetInstanceProcAddr LoadInterposer() {
    if (!FrameGen::WantStreamline()) {
        return nullptr;
    }
    if (const char* l = std::getenv("SOTC_DLSSG_LOG")) {
        log_level = std::atoi(l);
    }
    static std::wstring dir = [] {
        wchar_t path[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring p(path, n);
        const size_t slash = p.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring(L".") : p.substr(0, slash);
    }();
    const std::wstring interposer = dir + L"\\sl.interposer.dll";
    api.dll = LoadLibraryExW(interposer.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!api.dll) {
        std::fprintf(stderr, "SOTC DLSSG: sl.interposer.dll not found next to bb-probe.exe "
                             "(error %lu), frame generation off\n",
                     GetLastError());
        return nullptr;
    }
    if (!Resolve(api.init, "slInit") || !Resolve(api.is_supported, "slIsFeatureSupported") ||
        !Resolve(api.requirements, "slGetFeatureRequirements") ||
        !Resolve(api.feature_function, "slGetFeatureFunction") ||
        !Resolve(api.frame_token, "slGetNewFrameToken") ||
        !Resolve(api.set_constants, "slSetConstants") ||
        !Resolve(api.set_tag, "slSetTagForFrame")) {
        return nullptr;
    }
    const auto gipa =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(api.dll, "vkGetInstanceProcAddr"));
    if (!gipa) {
        std::fprintf(stderr, "SOTC DLSSG: vkGetInstanceProcAddr missing in sl.interposer.dll\n");
        return nullptr;
    }

    static const wchar_t* plugin_paths[] = {dir.c_str()};
    static const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex,
                                           sl::kFeaturePCL};
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = log_level >= 2 ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.pathsToPlugins = plugin_paths;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = log_level >= 1 ? dir.c_str() : nullptr;
    pref.logMessageCallback = LogCallback;
    // No over-the-air downloads: the plugins next to the executable only.
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = static_cast<u32>(std::size(features));
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0.0";
    pref.projectId = "7d6c0a52-3f1e-4b8a-9c2d-5e0f8a1b2c3d";
    pref.renderAPI = sl::RenderAPI::eVulkan;
    const sl::Result r = api.init(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) {
        std::fprintf(stderr, "SOTC DLSSG: slInit failed (%d), frame generation off\n",
                     static_cast<int>(r));
        return nullptr;
    }
    loaded = true;
    if (const char* f = std::getenv("SOTC_DLSSG_ASSUME_FOCUS"); f && f[0] == '1') {
        PatchForegroundWindow(L"sl.common.dll");
        PatchForegroundWindow(L"sl.dlss_g.dll");
    }
    std::fprintf(stderr, "SOTC DLSSG: Streamline %d.%d.%d loaded\n", SL_VERSION_MAJOR,
                 SL_VERSION_MINOR, SL_VERSION_PATCH);
    return gipa;
}

void OnDeviceCreated(const Instance& instance) {
    g_instance = &instance;
    FsrFg::Init(instance);
    struct ResolveMode {
        ~ResolveMode() {
            FrameGen::SetAvailable(supported, FsrFg::Supported());
        }
    } resolve_mode; // on every return
    if (!loaded) {
        return;
    }
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = static_cast<VkPhysicalDevice>(instance.GetPhysicalDevice());
    const sl::Result dlssg = api.is_supported(sl::kFeatureDLSS_G, adapter);
    const sl::Result reflex = api.is_supported(sl::kFeatureReflex, adapter);
    sl::FeatureRequirements req{};
    if (api.requirements(sl::kFeatureDLSS_G, req) == sl::Result::eOk) {
        vsync_off = req.flags & sl::FeatureRequirementFlags::eVSyncOffRequired;
        std::fprintf(stderr,
                     "SOTC DLSSG: requirements flags %#x, OS %u.%u.%u (needs %u.%u.%u), driver "
                     "%u.%u (needs %u.%u)\n",
                     static_cast<u32>(req.flags), req.osVersionDetected.major,
                     req.osVersionDetected.minor, req.osVersionDetected.build,
                     req.osVersionRequired.major, req.osVersionRequired.minor,
                     req.osVersionRequired.build, req.driverVersionDetected.major,
                     req.driverVersionDetected.minor, req.driverVersionRequired.major,
                     req.driverVersionRequired.minor);
    }
    if (dlssg != sl::Result::eOk) {
        std::fprintf(stderr, "SOTC DLSSG: DLSS frame generation not supported here (%d)\n",
                     static_cast<int>(dlssg));
        SotcStats::ov_fg_state.store(2, std::memory_order_relaxed);
        return;
    }
    if (!ResolveFeature(api.dlssg_set_options, sl::kFeatureDLSS_G, "slDLSSGSetOptions") ||
        !ResolveFeature(api.dlssg_get_state, sl::kFeatureDLSS_G, "slDLSSGGetState") ||
        !ResolveFeature(api.pcl_marker, sl::kFeaturePCL, "slPCLSetMarker")) {
        SotcStats::ov_fg_state.store(2, std::memory_order_relaxed);
        return;
    }
    if (reflex == sl::Result::eOk &&
        ResolveFeature(api.reflex_set_options, sl::kFeatureReflex, "slReflexSetOptions")) {
        ResolveFeature(api.reflex_sleep, sl::kFeatureReflex, "slReflexSleep");
        sl::ReflexOptions options{};
        options.mode = sl::ReflexMode::eLowLatency;
        api.reflex_set_options(options);
    }
    supported = true;
    SotcStats::ov_fg_state.store(0, std::memory_order_relaxed);
    std::fprintf(stderr, "SOTC DLSSG: DLSS frame generation available (Reflex %s)\n",
                 reflex == sl::Result::eOk ? "on" : "unavailable");
}

bool Enabled() {
    return supported;
}

bool CapturingInputs() {
    return Capturing();
}

// Streamline asks for vsync off with Vulkan, but DLSS-G generates and paces its frames in
// Mailbox mode too (measured: 2 frames shown per game frame), without tearing when G-SYNC is
// off. The requested mode is kept; SOTC_DLSSG_IMMEDIATE=1 switches to Immediate.
bool WantsImmediatePresent() {
    const char* v = std::getenv("SOTC_DLSSG_IMMEDIATE");
    return supported && vsync_off && v && v[0] == '1';
}

void NoteDraw(const AmdGpu::Regs& regs, u32 mrt_mask, VideoCore::ImageId depth_id,
              VideoCore::ImageId mvec_id, VideoCore::TextureCache& texture_cache) {
    if (pending.captured || !(mrt_mask & (1u << 4)) || !depth_id || !mvec_id || !Capturing()) {
        return;
    }
    const auto& cb4 = regs.color_buffers[4];
    if (!cb4 || cb4.GetDataFmt() != AmdGpu::DataFormat::Format16_16 ||
        cb4.GetNumberFmt() != AmdGpu::NumberFormat::Float || !regs.depth_buffer.DepthValid()) {
        return;
    }
    auto& depth = texture_cache.GetImage(depth_id);
    auto& mvec = texture_cache.GetImage(mvec_id);
    if (depth.info.size.width < 640 || mvec.info.size.width < 640 || depth.info.num_samples != 1 ||
        mvec.info.num_samples != 1 || !(depth.aspect_mask & vk::ImageAspectFlagBits::eDepth)) {
        return;
    }
    Pending p{};
    if (!ReadCamera(regs, p.cam)) {
        if (bad_cameras++ < 5) {
            std::fprintf(stderr, "SOTC DLSSG: G-buffer draw without a readable camera\n");
        }
        return;
    }
    p.captured = true;
    p.depth_id = depth_id;
    p.depth_uid = depth.image_uid;
    p.mvec_id = mvec_id;
    p.mvec_uid = mvec.image_uid;
    const auto& vp = regs.viewports[0];
    p.yscale = regs.viewport_control.yscale_enable ? vp.yscale : 1.0f;
    p.zscale = regs.viewport_control.zscale_enable ? vp.zscale : 1.0f;
    p.zoffset = regs.viewport_control.zoffset_enable ? vp.zoffset : 0.0f;
    pending = p;
}

void PrepareInputs(u32 frame_id, vk::CommandBuffer cmdbuf, VideoCore::TextureCache& texture_cache,
                   Runtime& runtime, u32 game_width, u32 game_height) {
    if (frame_id >= kMaxFrames) {
        return;
    }
    FrameSlot& f = frames[frame_id];
    f.valid = false;
    f.sl_valid = false;
    f.hudless = {};
    FsrFg::NoDepth(frame_id);
    const bool fsr = FrameGen::Current() == FrameGen::Mode::Fsr;
    if (!supported && !fsr) {
        pending = Pending{};
        return;
    }
    f.index = ++next_index;
    if (supported) {
        if (next_index == 1) {
            Marker(sl::PCLMarker::eSimulationStart, f.index);
        }
        Marker(sl::PCLMarker::eSimulationEnd, f.index);
        Marker(sl::PCLMarker::eRenderSubmitStart, f.index);
    }
    // Streamline may still be reading this frame's images from its last present (sotc: also
    // the presenter's HUD-less image, rewritten after this call even without a G-buffer pass).
    if (f.inputs_done && f.inputs_done_value) {
        const vk::Semaphore semaphore{f.inputs_done};
        const vk::SemaphoreWaitInfo wait{
            .semaphoreCount = 1, .pSemaphores = &semaphore, .pValues = &f.inputs_done_value};
        if (g_instance->GetDevice().waitSemaphores(wait, 100'000'000) != vk::Result::eSuccess) {
            static u32 timeouts = 0;
            if (timeouts++ < 5) {
                std::fprintf(stderr, "SOTC DLSSG: inputs still in use after 100 ms\n");
            }
        }
        f.inputs_done = {};
        f.inputs_done_value = 0;
    }

    const Pending p = std::exchange(pending, Pending{});
    if (!p.captured) {
        return;
    }
    VideoCore::Image* depth = texture_cache.TryGetImage(p.depth_id, p.depth_uid);
    VideoCore::Image* mvec = texture_cache.TryGetImage(p.mvec_id, p.mvec_uid);
    if (!depth || !mvec) {
        return;
    }
    const u32 width = std::min({game_width, depth->info.size.width, mvec->info.size.width});
    const u32 height = std::min({game_height, depth->info.size.height, mvec->info.size.height});
    if (width < 64 || height < 64) {
        return;
    }
    sl::Constants constants{};
    if (!MakeConstants(p, width, height, constants)) {
        if (bad_cameras++ < 5) {
            std::fprintf(stderr, "SOTC DLSSG: camera matrix not understood\n");
        }
        return;
    }
    if (!EnsureImages(f, depth->info.pixel_format, mvec->info.pixel_format, width, height)) {
        return;
    }

    // Game images -> transfer source (tracked by the texture cache), ours -> transfer dest.
    runtime.Transit(depth, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                    vk::AccessFlagBits2::eTransferRead);
    runtime.Transit(mvec, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                    vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    const auto range = [](vk::ImageAspectFlags aspect) {
        return vk::ImageSubresourceRange{
            .aspectMask = aspect, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0,
            .layerCount = 1};
    };
    const vk::ImageAspectFlags depth_aspect =
        depth->aspect_mask & (vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil);
    const std::array to_dst{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = f.depth,
            .subresourceRange = range(depth_aspect),
        },
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = f.mvec,
            .subresourceRange = range(vk::ImageAspectFlagBits::eColor),
        },
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(to_dst.size()),
        .pImageMemoryBarriers = to_dst.data(),
    });
    const auto copy = [&](vk::Image src, vk::Image dst, vk::ImageAspectFlags aspect) {
        const vk::ImageCopy region{
            .srcSubresource = {aspect, 0, 0, 1},
            .srcOffset = {0, 0, 0},
            .dstSubresource = {aspect, 0, 0, 1},
            .dstOffset = {0, 0, 0},
            .extent = {width, height, 1},
        };
        cmdbuf.copyImage(src, vk::ImageLayout::eTransferSrcOptimal, dst,
                         vk::ImageLayout::eTransferDstOptimal, region);
    };
    copy(depth->GetImage(), f.depth, vk::ImageAspectFlagBits::eDepth);
    copy(mvec->GetImage(), f.mvec, vk::ImageAspectFlagBits::eColor);
    const std::array to_read{
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = f.depth,
            .subresourceRange = range(depth_aspect),
        },
        vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = f.mvec,
            .subresourceRange = range(vk::ImageAspectFlagBits::eColor),
        },
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(to_read.size()),
        .pImageMemoryBarriers = to_read.data(),
    });

    f.constants = constants;
    f.valid = true;
    if (fsr) {
        // FSR 3 reads depth as R32F: from the copy, now ShaderReadOnlyOptimal.
        FsrFg::ConvertDepth(frame_id, cmdbuf, f.depth_view, width, height,
                            constants.depthInverted == sl::Boolean::eTrue);
    }
    if (!supported || FrameGen::Current() != FrameGen::Mode::Dlss) {
        return;
    }
    if (sl::FrameToken* token = Token(f.index)) {
        const sl::Result r = api.set_constants(constants, *token, sl::ViewportHandle{0});
        if (r != sl::Result::eOk) {
            static u32 errors = 0;
            if (errors++ < 5) {
                std::fprintf(stderr, "SOTC DLSSG: slSetConstants failed (%d)\n",
                             static_cast<int>(r));
            }
            return;
        }
        f.sl_valid = true;
    }
}

void NoteHudless(u32 frame_id, vk::Image image, vk::ImageView view, vk::Format format, u32 width,
                 u32 height) {
    if (frame_id < kMaxFrames) {
        frames[frame_id].hudless = {image, view, format, width, height};
    }
}

void FrameSubmitted(u32 frame_id) {
    if (!supported || frame_id >= kMaxFrames || frames[frame_id].index == 0) {
        return;
    }
    const u32 index = frames[frame_id].index;
    Marker(sl::PCLMarker::eRenderSubmitEnd, index);
    // The next frame starts here: Reflex may hold it back a little to keep the GPU queue short
    // (SOTC_DLSSG_SLEEP=0: no sleep).
    static const bool sleep = [] {
        const char* v = std::getenv("SOTC_DLSSG_SLEEP");
        return !v || v[0] != '0';
    }();
    if (sleep && api.reflex_sleep) {
        if (sl::FrameToken* token = Token(index + 1)) {
            api.reflex_sleep(*token);
        }
    }
    Marker(sl::PCLMarker::eSimulationStart, index + 1);
}

void NoInputs(u32 frame_id) {
    if (frame_id < kMaxFrames) {
        frames[frame_id].valid = false;
        frames[frame_id].sl_valid = false;
        frames[frame_id].index = 0;
    }
    FsrFg::NoDepth(frame_id);
}

void BeforePresent(u32 frame_id, vk::Rect2D rect, vk::Extent2D extent) {
    if (!supported || frame_id >= kMaxFrames) {
        return;
    }
    FrameSlot& f = frames[frame_id];
    valid_streak = f.sl_valid ? valid_streak + 1 : 0;
    const bool want = valid_streak >= 2;
    if (want != fg_on || !options_set) {
        SetOptions(want);
    }
    if (f.index == 0) {
        return;
    }
    sl::FrameToken* token = Token(f.index);
    if (!token) {
        return;
    }
    if (f.sl_valid) {
        sl::Resource depth{sl::ResourceType::eTex2d, static_cast<VkImage>(f.depth), f.depth_memory,
                           static_cast<VkImageView>(f.depth_view),
                           static_cast<u32>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)};
        depth.width = f.width;
        depth.height = f.height;
        depth.nativeFormat = static_cast<u32>(f.depth_format);
        depth.mipLevels = 1;
        depth.arrayLayers = 1;
        depth.flags = 0;
        depth.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
        sl::Resource mvec{sl::ResourceType::eTex2d, static_cast<VkImage>(f.mvec), f.mvec_memory,
                          static_cast<VkImageView>(f.mvec_view),
                          static_cast<u32>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)};
        mvec.width = f.width;
        mvec.height = f.height;
        mvec.nativeFormat = static_cast<u32>(f.mvec_format);
        mvec.mipLevels = 1;
        mvec.arrayLayers = 1;
        mvec.flags = 0;
        mvec.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        // sotc: the frame without its HUD (NoteHudless): DLSS-G then keeps the HUD still instead
        // of moving it with the scene. It must have the size of the game's area in the back
        // buffer (no scaling in the blit); otherwise, or when the frame has none, a null tag so
        // that an earlier frame's is not used.
        const FrameSlot::Hudless& h = f.hudless;
        const bool hudless_ok =
            h.image && h.width == rect.extent.width && h.height == rect.extent.height;
        sl::Resource hudless{sl::ResourceType::eTex2d, static_cast<VkImage>(h.image), nullptr,
                             static_cast<VkImageView>(h.view),
                             static_cast<u32>(VK_IMAGE_LAYOUT_GENERAL)};
        hudless.width = h.width;
        hudless.height = h.height;
        hudless.nativeFormat = static_cast<u32>(h.format);
        hudless.mipLevels = 1;
        hudless.arrayLayers = 1;
        hudless.flags = 0;
        hudless.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        const sl::Extent full{0, 0, f.width, f.height};
        const sl::Extent hudless_full{0, 0, h.width, h.height};
        sl::Extent back{static_cast<u32>(std::max(rect.offset.y, 0)),
                        static_cast<u32>(std::max(rect.offset.x, 0)), rect.extent.width,
                        rect.extent.height};
        const bool whole = back.left == 0 && back.top == 0 && back.width == extent.width &&
                           back.height == extent.height;
        std::array<sl::ResourceTag, 4> tags{
            sl::ResourceTag{&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent,
                            &full},
            sl::ResourceTag{&mvec, sl::kBufferTypeMotionVectors,
                            sl::ResourceLifecycle::eValidUntilPresent, &full},
            sl::ResourceTag{hudless_ok ? &hudless : nullptr, sl::kBufferTypeHUDLessColor,
                            sl::ResourceLifecycle::eValidUntilPresent, &hudless_full},
            sl::ResourceTag{nullptr, sl::kBufferTypeBackbuffer,
                            sl::ResourceLifecycle::eValidUntilPresent, &back},
        };
        const u32 count = whole ? 3 : 4;
        const sl::Result r = api.set_tag(*token, sl::ViewportHandle{0}, tags.data(), count, nullptr);
        if (r != sl::Result::eOk) {
            static u32 errors = 0;
            if (errors++ < 5) {
                std::fprintf(stderr, "SOTC DLSSG: slSetTagForFrame failed (%d)\n",
                             static_cast<int>(r));
            }
        }
    } else {
        std::array<sl::ResourceTag, 3> tags{
            sl::ResourceTag{nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent},
            sl::ResourceTag{nullptr, sl::kBufferTypeMotionVectors,
                            sl::ResourceLifecycle::eValidUntilPresent},
            sl::ResourceTag{nullptr, sl::kBufferTypeHUDLessColor,
                            sl::ResourceLifecycle::eValidUntilPresent},
        };
        api.set_tag(*token, sl::ViewportHandle{0}, tags.data(), 3, nullptr);
    }
    api.pcl_marker(sl::PCLMarker::ePresentStart, *token);
    QueryPerformanceCounter(&present_start);
}

void AfterPresent(u32 frame_id) {
    if (!supported || frame_id >= kMaxFrames) {
        return;
    }
    const u32 index = frames[frame_id].index;
    if (index != 0 && present_start.QuadPart) {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        const double ms = double(now.QuadPart - present_start.QuadPart) * 1000.0 / freq.QuadPart;
        present_ms_sum += ms;
        present_ms_max = std::max(present_ms_max, ms);
        ++present_ms_count;
        present_start.QuadPart = 0;
    }
    Marker(sl::PCLMarker::ePresentEnd, index);
    const bool stats = ++presents % 60 == 0;
    const bool track = Parallel() && fg_on && index != 0 && frames[frame_id].sl_valid;
    if (!stats && !track) {
        return;
    }
    sl::DLSSGState state{};
    if (api.dlssg_get_state(sl::ViewportHandle{0}, state, nullptr) != sl::Result::eOk) {
        return;
    }
    if (track) {
        frames[frame_id].inputs_done = static_cast<VkSemaphore>(state.inputsProcessingCompletionFence);
        frames[frame_id].inputs_done_value = state.lastPresentInputsProcessingCompletionFenceValue;
    }
    if (!stats) {
        return;
    }
    SotcStats::ov_fg_frames.store(state.numFramesActuallyPresented, std::memory_order_relaxed);
    SotcStats::ov_fg_status.store(static_cast<u32>(state.status), std::memory_order_relaxed);
    if (fg_on && state.status != sl::DLSSGStatus::eOk) {
        SotcStats::ov_fg_state.store(2, std::memory_order_relaxed);
    }
    static u64 last_log = 0;
    if (presents - last_log >= 600 || (log_level >= 1 && presents - last_log >= 120)) {
        last_log = presents;
        std::fprintf(stderr,
                     "SOTC DLSSG state: %s, status %#x, %u frames presented per frame, max "
                     "generated %u, min size %u; present %.2f ms avg, %.2f max\n",
                     fg_on ? "on" : "off", static_cast<u32>(state.status),
                     state.numFramesActuallyPresented, state.numFramesToGenerateMax,
                     state.minWidthOrHeight,
                     present_ms_count ? present_ms_sum / present_ms_count : 0.0, present_ms_max);
        present_ms_sum = present_ms_max = 0.0;
        present_ms_count = 0;
    }
}

void Suspend() {
    if (supported && fg_on) {
        SetOptions(false);
    }
    valid_streak = 0;
}

bool GetFrameInputs(u32 frame_id, FrameInputs& out) {
    if (frame_id >= kMaxFrames || !frames[frame_id].valid) {
        return false;
    }
    const FrameSlot& f = frames[frame_id];
    const sl::Constants& c = f.constants;
    out.mvec = f.mvec;
    out.mvec_usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc |
                     vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage;
    out.width = f.width;
    out.height = f.height;
    out.jitter[0] = c.jitterOffset.x;
    out.jitter[1] = c.jitterOffset.y;
    out.z_near = c.cameraNear;
    out.z_far = c.cameraFar;
    out.fov_y = c.cameraFOV;
    const sl::float3* vectors[] = {&c.cameraPos, &c.cameraUp, &c.cameraRight, &c.cameraFwd};
    float* outs[] = {out.pos, out.up, out.right, out.fwd};
    for (int i = 0; i < 4; ++i) {
        outs[i][0] = vectors[i]->x;
        outs[i][1] = vectors[i]->y;
        outs[i][2] = vectors[i]->z;
    }
    out.depth_inverted = c.depthInverted == sl::Boolean::eTrue;
    out.reset = c.reset == sl::Boolean::eTrue;
    return true;
}

} // namespace Vulkan::Dlssg
