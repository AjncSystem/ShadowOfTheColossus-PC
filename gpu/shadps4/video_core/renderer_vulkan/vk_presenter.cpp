// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <utility>
#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/system/systemservice.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_dlssg.h"
#include "video_core/renderer_vulkan/vk_framegen.h"
#include "video_core/renderer_vulkan/vk_fsrfg.h"
#include "video_core/sotc_fps_cap.h"
#include "video_core/sotc_stats.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"
#include "bbport_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <vector>
#include <imgui.h>
#include <stb_image_write.h>
#include <vk_mem_alloc.h>

namespace Vulkan {

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

enum class ScreenshotKind : u8 {
    GameOnly,
    WithOverlays,
};

struct ScreenshotReadback {
    ScreenshotKind kind{};
    std::vector<std::filesystem::path> paths{};
    VideoCore::Buffer buffer;
    u32 width{};
    u32 height{};
    vk::Format format{};
    bool hdr_encoded{};

    ScreenshotReadback(const Instance& instance, ScreenshotKind kind_,
                       std::vector<std::filesystem::path> paths_, const u32 width_,
                       const u32 height_, const vk::Format format_, const bool hdr_encoded_)
        : kind{kind_}, paths{std::move(paths_)},
          buffer{instance, 0, static_cast<u64>(width_) * static_cast<u64>(height_) * 4,
                 VideoCore::MemoryType::HostCached},
          width{width_}, height{height_}, format{format_}, hdr_encoded{hdr_encoded_} {}
};

static std::string SanitizeFilenameComponent(std::string value) {
    for (char& c : value) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-') {
            c = '_';
        }
    }
    if (value.empty()) {
        return "UNKNOWN";
    }
    return value;
}

static std::vector<std::filesystem::path> BuildScreenshotPaths(const ScreenshotKind kind,
                                                               const u32 count) {
    static std::atomic<u64> screenshot_sequence{0};
    std::vector<std::filesystem::path> paths{};
    if (count == 0) {
        return paths;
    }

    const auto& screenshots_dir = Common::FS::GetUserPath(Common::FS::PathType::ScreenshotsDir);
    std::filesystem::create_directories(screenshots_dir);

    const auto game_id =
        SanitizeFilenameComponent(std::string(Common::ElfInfo::Instance().GameSerial()));
    const auto now = std::chrono::system_clock::now();
    const auto now_time = std::chrono::system_clock::to_time_t(now);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000;

    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &now_time);
#else
    localtime_r(&now_time, &local_tm);
#endif

    std::ostringstream stamp;
    stamp << std::put_time(&local_tm, "%Y%m%d_%H%M%S") << '_' << std::setw(3) << std::setfill('0')
          << ms;

    const char* suffix = kind == ScreenshotKind::GameOnly ? "game" : "hud";
    const auto first_sequence = screenshot_sequence.fetch_add(count, std::memory_order_relaxed);

    paths.reserve(count);
    const auto stamp_str = stamp.str();
    for (u32 i = 0; i < count; ++i) {
        paths.emplace_back(screenshots_dir / fmt::format("{}_{}_{}_{:06}.png", game_id, stamp_str,
                                                         suffix, first_sequence + i));
    }

    return paths;
}

static float PqToNits(const float encoded) {
    // ST.2084 inverse EOTF
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;

    const float v = std::clamp(encoded, 0.0f, 1.0f);
    const float vp = std::pow(v, 1.0f / m2);
    const float num = std::max(vp - c1, 0.0f);
    const float den = std::max(c2 - c3 * vp, 1e-6f);
    return 10000.0f * std::pow(num / den, 1.0f / m1);
}

static float ToneMapToSdrLinear(const float nits) {
    // Map absolute HDR luminance into SDR [0,1], preserving 100-nit white.
    constexpr float sdr_white_nits = 100.0f;
    const float x = std::max(nits, 0.0f) / sdr_white_nits;
    const float mapped = (2.0f * x) / (1.0f + x);
    return std::clamp(mapped, 0.0f, 1.0f);
}

static float LinearToSrgb(const float linear) {
    const float x = std::clamp(linear, 0.0f, 1.0f);
    if (x <= 0.0031308f) {
        return 12.92f * x;
    }
    return 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

static const std::array<float, 1024>& GetPqDecodeNitsLut() {
    static const std::array<float, 1024> lut = [] {
        std::array<float, 1024> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = PqToNits(static_cast<float>(i) / 1023.0f);
        }
        return values;
    }();
    return lut;
}

static const std::array<u8, 1024>& GetUnorm10ToU8Lut() {
    static const std::array<u8, 1024> lut = [] {
        std::array<u8, 1024> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = static_cast<u8>((i * 255u + 511u) / 1023u);
        }
        return values;
    }();
    return lut;
}

static void CopyImageToReadback(const vk::CommandBuffer& cmdbuf, const vk::Image image,
                                const vk::ImageLayout layout, ScreenshotReadback& readback) {
    const vk::BufferImageCopy copy_region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {readback.width, readback.height, 1},
    };
    cmdbuf.copyImageToBuffer(image, layout, readback.buffer.Handle(), copy_region);
}

static bool ConvertReadbackToRgba8(const ScreenshotReadback& readback, std::vector<u8>& out_rgba) {
    const u64 pixel_count = static_cast<u64>(readback.width) * static_cast<u64>(readback.height);
    const u64 byte_size = pixel_count * 4;
    if (readback.buffer.mapped_data.size() < byte_size) {
        LOG_ERROR(Render_Vulkan, "Screenshot readback buffer size mismatch (have {}, need {})",
                  readback.buffer.mapped_data.size(), byte_size);
        return false;
    }

    const auto src =
        std::span<const u8>{readback.buffer.mapped_data.data(), static_cast<size_t>(byte_size)};
    out_rgba.resize(static_cast<size_t>(byte_size));

    switch (readback.format) {
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
        std::memcpy(out_rgba.data(), src.data(), out_rgba.size());
        for (u64 i = 0; i < pixel_count; ++i) {
            out_rgba[static_cast<size_t>(i) * 4 + 3] = 255;
        }
        return true;
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            out_rgba[o + 0] = src[o + 2];
            out_rgba[o + 1] = src[o + 1];
            out_rgba[o + 2] = src[o + 0];
            out_rgba[o + 3] = 255;
        }
        return true;
    case vk::Format::eA2R10G10B10UnormPack32: {
        const auto& pq_decode_lut = GetPqDecodeNitsLut();
        const auto& unorm10_to_u8 = GetUnorm10ToU8Lut();

        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            const u32 packed = static_cast<u32>(src[o + 0]) | (static_cast<u32>(src[o + 1]) << 8) |
                               (static_cast<u32>(src[o + 2]) << 16) |
                               (static_cast<u32>(src[o + 3]) << 24);
            const u32 b = (packed >> 0) & 0x3FF;
            const u32 g = (packed >> 10) & 0x3FF;
            const u32 r = (packed >> 20) & 0x3FF;

            if (readback.hdr_encoded) {
                // Rec.2020 + PQ. Convert to SDR Rec.709 for PNG output.
                const float r2020 = pq_decode_lut[r];
                const float g2020 = pq_decode_lut[g];
                const float b2020 = pq_decode_lut[b];

                const float r709_nits = 1.6605f * r2020 - 0.5876f * g2020 - 0.0728f * b2020;
                const float g709_nits = -0.1246f * r2020 + 1.1329f * g2020 - 0.0083f * b2020;
                const float b709_nits = -0.0182f * r2020 - 0.1006f * g2020 + 1.1187f * b2020;

                const float r_srgb = LinearToSrgb(ToneMapToSdrLinear(r709_nits));
                const float g_srgb = LinearToSrgb(ToneMapToSdrLinear(g709_nits));
                const float b_srgb = LinearToSrgb(ToneMapToSdrLinear(b709_nits));

                out_rgba[o + 0] = static_cast<u8>(std::clamp(r_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 1] = static_cast<u8>(std::clamp(g_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 2] = static_cast<u8>(std::clamp(b_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
            } else {
                out_rgba[o + 0] = unorm10_to_u8[r];
                out_rgba[o + 1] = unorm10_to_u8[g];
                out_rgba[o + 2] = unorm10_to_u8[b];
            }
            out_rgba[o + 3] = 255;
        }
        return true;
    }
    case vk::Format::eA2B10G10R10UnormPack32: {
        const auto& pq_decode_lut = GetPqDecodeNitsLut();
        const auto& unorm10_to_u8 = GetUnorm10ToU8Lut();

        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            const u32 packed = static_cast<u32>(src[o + 0]) | (static_cast<u32>(src[o + 1]) << 8) |
                               (static_cast<u32>(src[o + 2]) << 16) |
                               (static_cast<u32>(src[o + 3]) << 24);
            const u32 r = (packed >> 0) & 0x3FF;
            const u32 g = (packed >> 10) & 0x3FF;
            const u32 b = (packed >> 20) & 0x3FF;

            if (readback.hdr_encoded) {
                // HDR swapchain path is Rec.2020 + PQ. Convert to SDR Rec.709 for PNG output.
                const float r2020 = pq_decode_lut[r];
                const float g2020 = pq_decode_lut[g];
                const float b2020 = pq_decode_lut[b];

                const float r709_nits = 1.6605f * r2020 - 0.5876f * g2020 - 0.0728f * b2020;
                const float g709_nits = -0.1246f * r2020 + 1.1329f * g2020 - 0.0083f * b2020;
                const float b709_nits = -0.0182f * r2020 - 0.1006f * g2020 + 1.1187f * b2020;

                const float r_srgb = LinearToSrgb(ToneMapToSdrLinear(r709_nits));
                const float g_srgb = LinearToSrgb(ToneMapToSdrLinear(g709_nits));
                const float b_srgb = LinearToSrgb(ToneMapToSdrLinear(b709_nits));

                out_rgba[o + 0] = static_cast<u8>(std::clamp(r_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 1] = static_cast<u8>(std::clamp(g_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 2] = static_cast<u8>(std::clamp(b_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
            } else {
                out_rgba[o + 0] = unorm10_to_u8[r];
                out_rgba[o + 1] = unorm10_to_u8[g];
                out_rgba[o + 2] = unorm10_to_u8[b];
            }
            out_rgba[o + 3] = 255;
        }
        return true;
    }
    default:
        LOG_WARNING(Render_Vulkan, "Unsupported screenshot format: {}",
                    vk::to_string(readback.format));
        return false;
    }
}

static bool WritePng(const std::filesystem::path& path, const std::span<const u8> rgba,
                     const u32 width, const u32 height) {
    Common::FS::IOFile file(path, Common::FS::FileAccessMode::Create);
    if (!file.IsOpen()) {
        return false;
    }

    auto callback = [](void* context, void* data, int size) {
        const auto* f = static_cast<Common::FS::IOFile*>(context);
        f->WriteRaw<u8>(data, size);
    };
    return stbi_write_png_to_func(callback, &file, width, height, 4, rgba.data(), 0);
}

static void SavePendingScreenshot(const ScreenshotReadback& readback) {
    if (readback.paths.empty()) {
        return;
    }

    std::vector<u8> rgba;
    if (!ConvertReadbackToRgba8(readback, rgba)) {
        return;
    }

    const auto& primary_path = readback.paths.front();
    if (!WritePng(primary_path, rgba, readback.width, readback.height)) {
        LOG_ERROR(Render_Vulkan, "Failed saving screenshot to {}", primary_path.string());
        return;
    }

    LOG_INFO(Render_Vulkan, "Saved screenshot: {}", primary_path.string());

    std::ifstream file(primary_path, std::ios::binary);
    std::vector<u8> imgdata;
    if (file) {
        imgdata =
            std::vector<u8>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    (void)imgdata; // sotc port: no notification layer

    for (size_t i = 1; i < readback.paths.size(); ++i) {
        const auto& path = readback.paths[i];
        std::error_code ec{};
        std::filesystem::copy_file(primary_path, path, std::filesystem::copy_options::none, ec);
        if (ec) {
            // Fallback for platforms/filesystems where copy_file can fail for transient
            // reasons.
            if (!WritePng(path, rgba, readback.width, readback.height)) {
                LOG_ERROR(Render_Vulkan, "Failed saving screenshot to {}", path.string());
                continue;
            }
        }

        LOG_INFO(Render_Vulkan, "Saved screenshot: {}", path.string());
        std::ifstream file(path, std::ios::binary);
        std::vector<u8> imgdata;
        if (file) {
            imgdata = std::vector<u8>(std::istreambuf_iterator<char>(file),
                                      std::istreambuf_iterator<char>());
        }
        (void)imgdata; // sotc port: no notification layer
    }
}

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, [] {
          // sotc: Vulkan commands of the command processor recorded on a thread of their own
          // (cp_record_thread of Pink-shadPS4, luizgustavs): canyon of the attract demo
          // 24 -> 28 FPS. SOTC_RECORD_THREAD=0 records them on the command processor.
          const char* v = std::getenv("SOTC_RECORD_THREAD");
          return !v || v[0] != '0';
      }()},
      present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window}, runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames.
    // sotc: with DLSS Frame Generation Streamline keeps 2 swapchain images, FSR 3 holds a frame
    // for half a frame: 4 frames in flight so that the command processor does not wait for the
    // presenter.
    const u32 num_frames =
        Dlssg::Enabled() || FrameGen::AskedAtStart() ? std::max(num_images, 4u) : num_images;
    present_frames.resize(num_frames);
    hudless_frames.resize(num_frames);
    for (u32 i = 0; i < num_frames; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        hudless_frames[i].id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation =
        static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    pp_pass.Create(device, swapchain.GetSurfaceFormat().format);

    // sotc: the pipeline cache is read here, once frames can be presented, and blank frames with
    // the loading screen (moving, with the shader bar) are shown meanwhile. It used to be read in
    // the PipelineCache constructor with nothing on screen: a still window for seconds, minutes
    // after a driver update.
    std::atomic<bool> warming{true};
    std::thread painter([this, &warming] {
        while (warming.load(std::memory_order_relaxed)) {
            Present(PrepareBlankFrame(true), false, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    });
    rasterizer->GetPipelineCache().WarmUp();
    warming.store(false, std::memory_order_relaxed);
    painter.join();
}

Presenter::~Presenter() {
    // This thread records the last commands of the rasterizer scheduler.
    draw_scheduler.ClaimRecorder();
    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
    for (auto& frame : hudless_frames) {
        if (frame.image) {
            vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
            device.destroyImageView(frame.image_view);
        }
    }
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (false) { // sotc port: no ImGui layer
    }
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->is_hdr = swapchain.GetHDR();
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    Dlssg::NoInputs(frame->id); // sotc: a repeated frame, no frame generation
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
    const auto image_id = texture_cache.FindImage(desc);
    texture_cache.UpdateImage(image_id);

    Frame* frame = GetRenderFrame();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    auto& image = texture_cache.GetImage(image_id);
    auto image_view = *image.FindView(view_info).image_view;
    const vk::Extent2D image_size = {image.info.size.width, image.info.size.height};
    // sotc: this frame's depth, motion vectors and camera for DLSS Frame Generation.
    SotcStats::cp_frame.fetch_add(1, std::memory_order_relaxed);
    Dlssg::PrepareInputs(frame->id, cmdbuf, texture_cache, runtime, image_size.width,
                         image_size.height);
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);
    // sotc: SOTC_OUTPUT_RES=WxH (1280x720 ... 3840x2160): the presented frame has this size, the
    // game's 1080p image is scaled to it (FSR 1 + RCAS with BB_FSR1=1, else bilinear).
    static const std::pair<s32, s32> output_size = [] {
        const char* v = std::getenv("SOTC_OUTPUT_RES");
        s32 w = 0, h = 0;
        if (v && std::sscanf(v, "%dx%d", &w, &h) == 2 && w >= 640 && h >= 360 && w <= 7680 && h <= 4320) {
            return std::pair{w, h};
        }
        return std::pair<s32, s32>{0, 0};
    }();
    if (output_size.first) {
        SetExpectedGameSize(output_size.first, output_size.second);
        static bool reported = false;
        if (!std::exchange(reported, true)) {
            std::fprintf(stderr, "SOTC output %ux%u from a %ux%u game image, FSR 1 %s\n",
                         expected_frame_width, expected_frame_height, image_size.width,
                         image_size.height, fsr_settings.enable ? "on" : "off");
        }
    }

    const u32 capture_game_only_count = VideoCore::ConsumeGameOnlyScreenshotRequests();
    std::optional<ScreenshotReadback> pending_screenshot;

    // Capture the guest output before any host-side scaling (FSR/PP) is applied.
    if (capture_game_only_count > 0) {
        const bool hdr_encoded =
            attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq;
        auto& readback = pending_screenshot.emplace(
            instance, ScreenshotKind::GameOnly,
            BuildScreenshotPaths(ScreenshotKind::GameOnly, capture_game_only_count),
            image_size.width, image_size.height, view_info.format, hdr_encoded);
        const vk::BufferImageCopy copy_region = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {readback.width, readback.height, 1},
        };
        runtime.DownloadImage(&image, &readback.buffer, std::span{&copy_region, 1});
    }

    // Continue with host-side passes that draw the displayed (scaled) frame.

    // sotc: FSR 1 reads the frame in compute shaders (EASU), not in the fragment shader of the
    // post process pass: a barrier for the fragment stage alone let it read the frame before the
    // game finished writing it, and the game's next frame write over it while it still read
    // (torn, blocky image with FSR 1 on AMD cards, fine with bilinear, #209).
    runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eFragmentShader |
                        vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();

    image_view = fsr_pass.Render(cmdbuf, image_view, image_size, {frame->width, frame->height},
                                 fsr_settings, frame->is_hdr);

    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;
    pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings);

    // sotc: the game frame without the HUD, through the same pass (frame generation keeps the
    // HUD of the real frame instead of interpolating it). Not with FSR 1 scaling, whose images
    // are a ring meant for one frame each.
    if (const vk::ImageView hudless_view = rasterizer->TakeHudless(cpu_address, view_info.format);
        hudless_view && !DebugState.is_using_fsr) {
        Frame& hudless = hudless_frames[frame->id];
        if (hudless.width != frame->width || hudless.height != frame->height ||
            hudless.is_hdr != frame->is_hdr || !hudless.image) {
            RecreateFrame(&hudless, frame->width, frame->height);
        }
        auto hudless_barrier = pre_barrier;
        hudless_barrier.image = hudless.image;
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &hudless_barrier,
        });
        pp_pass.Render(cmdbuf, hudless_view, image_size, hudless, pp_settings);
        frame->hudless = &hudless;
        Dlssg::NoteHudless(frame->id, hudless.image, hudless.image_view,
                           swapchain.GetSurfaceFormat().format, hudless.width, hudless.height);
    }


    if (pending_screenshot) {
        draw_scheduler.DeferPriorityOperation(
            [deferred_screenshot = std::move(pending_screenshot)]() {
                SavePendingScreenshot(deferred_screenshot.value());
            });
    }

    // Flush frame creation commands.
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);
    Dlssg::FrameSubmitted(frame->id);
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    Dlssg::NoInputs(frame->id); // sotc: no game image, no frame generation
    return frame;
}

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };

    // Recreate the swapchain if the window was resized.
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        Dlssg::Suspend(); // sotc: frame generation off while the swapchain changes
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

    // sotc: FSR 3 frame generation: the frame between the last game frame and this one first,
    // this one half a frame later.
    if (is_game_frame && !is_reusing_frame) {
        FrameGen::TestTick();
        SotcFpsCap::TestTick();
        FsrFg::NoteGameFrame();
        if (FrameGen::Current() == FrameGen::Mode::Fsr && PresentGenerated(frame)) {
            // SOTC_FSRFG_ONLY_GENERATED=1: the game frames are not shown (checking the others).
            static const bool only_generated = [] {
                const char* v = std::getenv("SOTC_FSRFG_ONLY_GENERATED");
                return v && v[0] == '1';
            }();
            if (only_generated) {
                Check(instance.GetDevice().resetFences(frame->present_done));
                SubmitInfo info{};
                info.AddWait(frame->ready_semaphore, frame->ready_tick);
                info.AddSignal(frame->present_done);
                present_scheduler.Flush(info);
                free_frame();
                return;
            }
            FsrFg::WaitForGameFrame();
        }
    }

    if (!swapchain.AcquireNextImage()) {
        Dlssg::Suspend();
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            free_frame();
            return;
        }
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));


    const vk::Image swapchain_image = swapchain.Image();
    const vk::ImageView swapchain_image_view = swapchain.ImageView();

    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();
    const u32 capture_with_overlays_count = VideoCore::ConsumeWithOverlaysScreenshotRequests();
    std::optional<ScreenshotReadback> pending_screenshot;

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        auto* profiler_ctx = instance.GetProfilerContext();
        TracyVkNamedZoneC(profiler_ctx, renderer_gpu_zone, cmdbuf, "Host frame",
                          MarkersPalette::GpuMarkerColor, profiler_ctx != nullptr);

        const vk::Extent2D extent = swapchain.GetExtent();
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };

        bool swapchain_copied_for_screenshot = false;

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                               vk::PipelineStageFlagBits::eColorAttachmentOutput,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barriers);

        { // sotc port: the game image is blitted to the swapchain (no ImGui layer).
            const auto color_range = vk::ImageSubresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            };
            const std::array to_transfer{
                vk::ImageMemoryBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                    .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                    .newLayout = vk::ImageLayout::eTransferDstOptimal,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = swapchain_image,
                    .subresourceRange = color_range,
                },
                vk::ImageMemoryBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eColorAttachmentRead,
                    .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                    .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                    .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = frame->image,
                    .subresourceRange = color_range,
                },
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                   vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_transfer);
            cmdbuf.clearColorImage(swapchain_image, vk::ImageLayout::eTransferDstOptimal,
                                   vk::ClearColorValue{std::array<float, 4>{0.f, 0.f, 0.f, 1.f}},
                                   color_range);
            // sotc: the clear and the blit both write the swapchain image; without a barrier
            // between them AMD let the clear land after the blit on part of the image: black
            // tiles over the lower left of the screen, UI included (issue #43).
            const vk::ImageMemoryBarrier clear_to_blit{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                   vk::PipelineStageFlagBits::eTransfer, {}, {}, {},
                                   clear_to_blit);
            const auto rect = FitImage(frame->width, frame->height, extent.width, extent.height);
            const vk::ImageBlit blit{
                .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                         vk::Offset3D{static_cast<s32>(frame->width),
                                                      static_cast<s32>(frame->height), 1}},
                .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .dstOffsets = std::array{
                    vk::Offset3D{rect.offset.x, rect.offset.y, 0},
                    vk::Offset3D{rect.offset.x + static_cast<s32>(rect.extent.width),
                                 rect.offset.y + static_cast<s32>(rect.extent.height), 1}},
            };
            cmdbuf.blitImage(frame->image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                             vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
            const std::array back{
                vk::ImageMemoryBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                    .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                    .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = swapchain_image,
                    .subresourceRange = color_range,
                },
                vk::ImageMemoryBarrier{
                    .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                    .dstAccessMask = vk::AccessFlagBits::eShaderRead,
                    .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                    .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = frame->image,
                    .subresourceRange = color_range,
                },
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                   vk::PipelineStageFlagBits::eAllCommands,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, back);
            // sotc port: the in-game overlay on top (the save/load slot list of
            // sceSaveDataDialog); draws nothing while it has nothing to show.
            static bool overlay_ready = false;
            if (!std::exchange(overlay_ready, true)) {
                // Up to 2 overlay draws per game frame with FSR 3: a deeper buffer ring.
                BbOverlay::Init(instance, swapchain.GetSurfaceFormat().format,
                                std::max(swapchain.GetImageCount(), 8u));
            }
            BbOverlay::Render(cmdbuf, swapchain_image_view, extent);
        }

        if (capture_with_overlays_count > 0) {
            auto& readback = pending_screenshot.emplace(
                instance, ScreenshotKind::WithOverlays,
                BuildScreenshotPaths(ScreenshotKind::WithOverlays, capture_with_overlays_count),
                extent.width, extent.height,
                swapchain.GetHDR() ? vk::Format::eA2B10G10R10UnormPack32
                                   : swapchain.GetSurfaceFormat().format,
                swapchain.GetHDR());

            const vk::ImageMemoryBarrier to_transfer{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_transfer);
            CopyImageToReadback(cmdbuf, swapchain_image, vk::ImageLayout::eTransferSrcOptimal,
                                readback);
            swapchain_copied_for_screenshot = true;
        }

        const vk::AccessFlags post_src_access_mask =
            swapchain_copied_for_screenshot ? vk::AccessFlagBits::eTransferRead
                                            : vk::AccessFlagBits::eColorAttachmentWrite;
        const vk::ImageLayout post_old_layout = swapchain_copied_for_screenshot
                                                    ? vk::ImageLayout::eTransferSrcOptimal
                                                    : vk::ImageLayout::eColorAttachmentOptimal;
        const vk::ImageMemoryBarrier post_barrier{
            .srcAccessMask = post_src_access_mask,
            .dstAccessMask = vk::AccessFlagBits::eNone,
            .oldLayout = post_old_layout,
            .newLayout = vk::ImageLayout::ePresentSrcKHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);

        if (profiler_ctx) {
            TracyVkCollect(profiler_ctx, cmdbuf);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.
    if (pending_screenshot) {
        scheduler.DeferPriorityOperation([deferred_screenshot = std::move(pending_screenshot)]() {
            SavePendingScreenshot(deferred_screenshot.value());
        });
    }

    SubmitInfo info{};
    // sotc: with the command recorder the frame's submit may not be on the queue yet; waiting
    // for it on the same queue before it is there would block the queue.
    if (frame->ready_semaphore == draw_scheduler.GetWorkSemaphore()->Handle()) {
        draw_scheduler.WaitSubmitted(frame->ready_tick, false);
    }
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
    scheduler.Flush(info);

    // Present to swapchain.
    {
        // sotc: DLSS Frame Generation inputs of this frame, and Reflex present markers.
        const vk::Extent2D extent = swapchain.GetExtent();
        Dlssg::BeforePresent(frame->id,
                             FitImage(frame->width, frame->height, extent.width, extent.height),
                             extent);
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        const bool presented = swapchain.Present();
        Dlssg::AfterPresent(frame->id);
        if (FrameGen::Current() == FrameGen::Mode::Fsr) {
            FsrFg::NotePresent();
        }
        if (!presented) {
            Dlssg::Suspend();
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
    }

    free_frame();
    if (!is_reusing_frame && is_game_frame) {
        DebugState.IncFlipFrameNum();
        // SOTC_FRAME_LOG=<n> prints the frame count and time to stderr every n frames, for
        // measuring the frame rate of scripted runs.
        static const u64 every = [] {
            const char* v = std::getenv("SOTC_FRAME_LOG");
            return v ? std::strtoull(v, nullptr, 10) : 0ULL;
        }();
        if (every != 0) {
            static u64 frames = 0;
            static const auto t0 = std::chrono::steady_clock::now();
            if (++frames % every == 0) {
                const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count();
                // us: the same time in microseconds, for frame time percentiles.
                std::fprintf(stderr, "SOTCFRAME %llu t=%lldms us=%lld\n",
                             static_cast<unsigned long long>(frames),
                             static_cast<long long>(us / 1000), static_cast<long long>(us));
                std::fflush(stderr);
            }
            // SOTC_STALL_MS (default 70): for slower frames, what happened since the last one.
            static const long long stall_us = [] {
                const char* v = std::getenv("SOTC_STALL_MS");
                return (v ? std::atoll(v) : 70LL) * 1000;
            }();
            static long long last_us = -1;
            static u64 last[11]{};
            const long long now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - t0)
                                         .count();
            const u64 cur[11] = {SotcStats::pipelines_new.load(), SotcStats::pipeline_ns.load(),
                                SotcStats::shaders_compiled.load(), SotcStats::shader_ns.load(),
                                SotcStats::images_new.load(), SotcStats::image_ns.load(),
                                SotcStats::readbacks.load(), SotcStats::readback_ns.load(),
                                SotcStats::image_query_ns.load(), SotcStats::image_alloc_ns.load(),
                                SotcStats::image_dedicated.load()};
            if (last_us >= 0 && now_us - last_us > stall_us) {
                std::fprintf(stderr,
                             "SOTCSTALL frame %llu %.0f ms: pipelines %llu (%.0f ms), shaders %llu "
                             "(%.0f ms), images %llu (%.0f ms: query %.0f, alloc %.0f, %llu dedicated), "
                             "readbacks %llu (%.0f ms)\n",
                             static_cast<unsigned long long>(frames), (now_us - last_us) / 1e3,
                             static_cast<unsigned long long>(cur[0] - last[0]), (cur[1] - last[1]) / 1e6,
                             static_cast<unsigned long long>(cur[2] - last[2]), (cur[3] - last[3]) / 1e6,
                             static_cast<unsigned long long>(cur[4] - last[4]), (cur[5] - last[5]) / 1e6,
                             (cur[8] - last[8]) / 1e6, (cur[9] - last[9]) / 1e6,
                             static_cast<unsigned long long>(cur[10] - last[10]),
                             static_cast<unsigned long long>(cur[6] - last[6]), (cur[7] - last[7]) / 1e6);
            }
            last_us = now_us;
            std::copy(std::begin(cur), std::end(cur), std::begin(last));
            if (false) {
            }
        }
    }
}

bool Presenter::PresentGenerated(Frame* frame) {
    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::Image generated =
        FsrFg::Generate(cmdbuf, frame->id, frame->image, frame->width, frame->height,
                        frame->hudless ? frame->hudless->image : vk::Image{});

    // FSR 3 reads the game frame and its inputs: after the frame's own commands.
    if (frame->ready_semaphore == draw_scheduler.GetWorkSemaphore()->Handle()) {
        draw_scheduler.WaitSubmitted(frame->ready_tick, false);
    }
    SubmitInfo info{};
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    const auto submit = [&] {
        const u64 tick = scheduler.CurrentTick();
        scheduler.Flush(info);
        FsrFg::Submitted(scheduler, tick);
    };
    if (!generated) {
        submit(); // FSR 3 history only (first frame, after a cut)
        return false;
    }
    if (!swapchain.AcquireNextImage()) {
        submit();
        Dlssg::Suspend();
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        return false;
    }

    const vk::Image swapchain_image = swapchain.Image();
    const vk::Extent2D extent = swapchain.GetExtent();
    const auto color_range = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    const auto swapchain_barrier = [&](vk::AccessFlags src, vk::AccessFlags dst,
                                       vk::ImageLayout from, vk::ImageLayout to) {
        return vk::ImageMemoryBarrier{
            .srcAccessMask = src,
            .dstAccessMask = dst,
            .oldLayout = from,
            .newLayout = to,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange = color_range,
        };
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                           vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                           {}, {},
                           swapchain_barrier(vk::AccessFlagBits::eNone,
                                             vk::AccessFlagBits::eTransferWrite,
                                             vk::ImageLayout::eUndefined,
                                             vk::ImageLayout::eTransferDstOptimal));
    cmdbuf.clearColorImage(swapchain_image, vk::ImageLayout::eTransferDstOptimal,
                           vk::ClearColorValue{std::array<float, 4>{0.f, 0.f, 0.f, 1.f}},
                           color_range);
    // sotc: the same clear-to-blit barrier as the game frames (issue #43): without it AMD RX 7000
    // showed the black tiles on the generated frames (KingJhoss).
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                           vk::PipelineStageFlagBits::eTransfer, {}, {}, {},
                           swapchain_barrier(vk::AccessFlagBits::eTransferWrite,
                                             vk::AccessFlagBits::eTransferWrite,
                                             vk::ImageLayout::eTransferDstOptimal,
                                             vk::ImageLayout::eTransferDstOptimal));
    const auto rect = FitImage(frame->width, frame->height, extent.width, extent.height);
    const vk::ImageBlit blit{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                 vk::Offset3D{static_cast<s32>(frame->width),
                                              static_cast<s32>(frame->height), 1}},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstOffsets =
            std::array{vk::Offset3D{rect.offset.x, rect.offset.y, 0},
                       vk::Offset3D{rect.offset.x + static_cast<s32>(rect.extent.width),
                                    rect.offset.y + static_cast<s32>(rect.extent.height), 1}},
    };
    cmdbuf.blitImage(generated, vk::ImageLayout::eGeneral, swapchain_image,
                     vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
    // sotc: SOTC_SHOT_GENERATED=<file>: when the file appears it is removed and the next generated
    // frame is saved as a screenshot ("hud" file name, without the overlay): checking frame
    // generation (BB_FRAME_DUMP_TRIGGER saves the game frames).
    static const char* shot_generated = std::getenv("SOTC_SHOT_GENERATED");
    std::optional<ScreenshotReadback> pending_screenshot;
    if (shot_generated && std::remove(shot_generated) == 0) {
        auto& readback = pending_screenshot.emplace(
            instance, ScreenshotKind::WithOverlays,
            BuildScreenshotPaths(ScreenshotKind::WithOverlays, 1), extent.width, extent.height,
            swapchain.GetHDR() ? vk::Format::eA2B10G10R10UnormPack32
                               : swapchain.GetSurfaceFormat().format,
            swapchain.GetHDR());
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {},
                               swapchain_barrier(vk::AccessFlagBits::eTransferWrite,
                                                 vk::AccessFlagBits::eTransferRead,
                                                 vk::ImageLayout::eTransferDstOptimal,
                                                 vk::ImageLayout::eTransferSrcOptimal));
        CopyImageToReadback(cmdbuf, swapchain_image, vk::ImageLayout::eTransferSrcOptimal,
                            readback);
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {},
                               swapchain_barrier(vk::AccessFlagBits::eTransferRead,
                                                 vk::AccessFlagBits::eTransferWrite,
                                                 vk::ImageLayout::eTransferSrcOptimal,
                                                 vk::ImageLayout::eTransferDstOptimal));
        scheduler.DeferPriorityOperation([deferred_screenshot = std::move(pending_screenshot)]() {
            SavePendingScreenshot(deferred_screenshot.value());
        });
    }
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                           vk::PipelineStageFlagBits::eAllCommands,
                           vk::DependencyFlagBits::eByRegion, {}, {},
                           swapchain_barrier(vk::AccessFlagBits::eTransferWrite,
                                             vk::AccessFlagBits::eColorAttachmentWrite,
                                             vk::ImageLayout::eTransferDstOptimal,
                                             vk::ImageLayout::eColorAttachmentOptimal));
    BbOverlay::Render(cmdbuf, swapchain.ImageView(), extent, false);
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                           vk::PipelineStageFlagBits::eAllCommands,
                           vk::DependencyFlagBits::eByRegion, {}, {},
                           swapchain_barrier(vk::AccessFlagBits::eColorAttachmentWrite,
                                             vk::AccessFlagBits::eNone,
                                             vk::ImageLayout::eColorAttachmentOptimal,
                                             vk::ImageLayout::ePresentSrcKHR));
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    submit();
    std::scoped_lock submit_lock{Scheduler::submit_mutex};
    if (!swapchain.Present()) {
        Dlssg::Suspend();
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }
    FsrFg::NotePresent();
    return true;
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }
    frame->hudless = nullptr;

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(std::lround(height * expected_ratio));
    } else {
        expected_frame_height = static_cast<s32>(std::lround(width / expected_ratio));
    }
}

} // namespace Vulkan
