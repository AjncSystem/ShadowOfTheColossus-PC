// bbport: shadPS4 settings used by the video core, read once from BB_* environment
// variables (defaults match shadPS4 except the pipeline cache, which is on).
#pragma once
#include <algorithm>
#include <cstdlib>
#include <string>
#include "common/types.h"

u32 BbDisplayRefreshHz(); // bbgpu.cpp: primary display refresh rate, 60 when unknown

enum GpuReadbacksMode : int { Disabled, Relaxed, Precise };

class EmulatorSettingsImpl {
public:
    static EmulatorSettingsImpl* GetInstance() { static EmulatorSettingsImpl s; return &s; }
    static bool Flag(const char* name, bool fallback) {
        const char* v = std::getenv(name);
        return v ? (v[0] == '1' || v[0] == 'y' || v[0] == 't') : fallback;
    }
    static long Number(const char* name, long fallback) {
        const char* v = std::getenv(name);
        return v ? std::strtol(v, nullptr, 10) : fallback;
    }
    s32 GetGpuId() { static const auto value = s32(Number("BB_GPU_ID", -1)); return value; }
    u32 GetInternalScreenWidth() { static const auto value = u32(Number("BB_INTERNAL_WIDTH", 1920)); return value; }
    u32 GetInternalScreenHeight() { static const auto value = u32(Number("BB_INTERNAL_HEIGHT", 1080)); return value; }
    std::string GetPresentMode() { const char* v = std::getenv("BB_PRESENT_MODE"); return v ? v : "Mailbox"; }
    int GetRcasAttenuation() { static const auto value = int(Number("BB_RCAS_ATTENUATION", 250)); return value; }
    // bbport: Relaxed by default: without readbacks FaceGen reads stale GPU-written vertices
    // (vertex explosions); in Hunter's Nightmare it costs no measurable frame rate.
    // sotc: Precise: Shadow of the Colossus reads GPU results the frame they are written (as in
    // the shadPS4 SotC fork); relaxed readbacks leave it with stale data.
    u32 GetReadbacksMode() { static const auto value = u32(Number("BB_READBACKS", GpuReadbacksMode::Precise)); return value; }
    // bbport: BB_VBLANK_HZ=0 (uncapped presets): vblank runs at 480 Hz so a finished frame is
    // shown within ~2 ms instead of waiting for the next display-rate vblank (below the display
    // rate frames alternated 10/20 ms at 100 Hz: judder), and GetFrameLimit() caps the rate at
    // the display refresh, at most 120 (above ~120 FPS movement timing breaks: running slows).
    // SotC: explicit vblank defaults to 30/60 FPS; BB_FPS_LIMIT=0 disables the host limit.
    u32 GetVblankFrequency() {
        static const u32 value = [] {
            const long hz = Number("BB_VBLANK_HZ", 60);
            return hz > 0 ? u32(hz) : 480u;
        }();
        return value;
    }
    /// Frames per second the present thread lets through; 0 = no limit. BB_FPS_LIMIT overrides.
    u32 GetFrameLimit() {
        static const u32 value = [] {
            const long limit = Number("BB_FPS_LIMIT", -1);
            if (limit >= 0) return u32(limit);
            const auto hz = Number("BB_VBLANK_HZ", 60);
            return hz > 0 ? (hz >= 120 ? 60u : 30u) : std::min<u32>(BbDisplayRefreshHz(), 120);
        }();
        return value;
    }
    bool IsCopyGpuBuffers() { static const auto value = Flag("BB_COPY_GPU_BUFFERS", false); return value; }
    // sotc: on, as in Pink-shadPS4's CUSA08809 config. Shader reads from a table at a runtime
    // index (the SSR/water probe records, 224 bytes each) cannot be flattened; without DMA every
    // lane read record 0 and distant water lost its ripples and reflections.
    // sotc: off by default (as in the Bloodborne and GoW3 ports): it syncs all resident memory
    // before each of the two probe dispatches per frame (~7% of the command processor, ~0.5 ms
    // of GPU). BB_DIRECT_MEMORY_ACCESS=1 (launcher: full water reflections) turns it on.
    bool IsDirectMemoryAccessEnabled() { static const auto value = Flag("BB_DIRECT_MEMORY_ACCESS", false); return value; }
    bool IsDumpShaders() { static const auto value = Flag("BB_DUMP_SHADERS", false); return value; }
    bool IsFsrEnabled() { static const auto value = Flag("BB_FSR1", false); return value; }
    bool IsHdrAllowed() { static const auto value = Flag("BB_HDR", false); return value; }
    bool IsNullGPU() { static const auto value = Flag("BB_NULL_GPU", false); return value; }
    bool IsPatchShaders() { return false; }
    bool IsInlineFetchShader() { return false; }
    bool IsPipelineCacheArchived() { return false; }
    bool IsPipelineCacheEnabled() { static const auto value = Flag("BB_PIPELINE_CACHE", true); return value; }
    bool IsRcasEnabled() { static const auto value = Flag("BB_RCAS", true); return value; }
    bool IsReadbackLinearImagesEnabled() { static const auto value = Flag("BB_READBACK_LINEAR", false); return value; }
    bool IsRenderdocEnabled() { return false; }
    bool IsShaderCollect() { return false; }
    bool IsUserfaultfdTracking() { return false; }
    bool IsVkCrashDiagnosticEnabled() { return false; }
    bool IsVkGuestMarkersEnabled() { static const auto value = Flag("BB_VK_MARKERS", false); return value; }
    bool IsVkHostMarkersEnabled() { static const auto value = Flag("BB_VK_MARKERS", false); return value; }
    bool IsVkValidationCoreEnabled() { return true; }
    bool IsVkValidationEnabled() { static const auto value = Flag("BB_VK_VALIDATION", false); return value; }
    bool IsVkValidationGpuEnabled() { return false; }
    bool IsVkValidationSyncEnabled() { static const auto value = Flag("BB_VK_VALIDATION_SYNC", false); return value; }
};
#define EmulatorSettings (*EmulatorSettingsImpl::GetInstance())
