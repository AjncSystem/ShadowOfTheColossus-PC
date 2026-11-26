// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the HDR display's peak brightness as Windows reports it (IDXGIOutput6::GetDesc1). dxgi.dll
// is loaded here at run time, so nothing new is linked.
#include "video_core/renderer_vulkan/sotc_display_peak.h"

#include <atomic>

#include "common/logging/log.h"

#ifdef _WIN32
#include <windows.h>
#include <dxgi1_6.h>
#endif

namespace Vulkan {

#ifdef _WIN32
// The peak of the HDR output showing `monitor`, or with no monitor the highest of all HDR
// outputs (several displays: a 1000-nit TV and a 400-nit monitor each get their own).
static float QueryPeak(HMONITOR monitor) {
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) {
        return 0.0f;
    }
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    const auto create = reinterpret_cast<CreateFactory>(
        reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
    IDXGIFactory1* factory = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        FreeLibrary(dxgi);
        return 0.0f;
    }
    float peak = 0.0f;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            IDXGIOutput6* output6 = nullptr;
            if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6),
                                                 reinterpret_cast<void**>(&output6)))) {
                DXGI_OUTPUT_DESC1 desc{};
                if (SUCCEEDED(output6->GetDesc1(&desc)) && (!monitor || desc.Monitor == monitor)) {
                    const bool hdr =
                        desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                    if (monitor) {
                        peak = hdr ? desc.MaxLuminance : 0.0f;
                    } else if (hdr && desc.MaxLuminance > peak) {
                        peak = desc.MaxLuminance;
                    }
                }
                output6->Release();
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    FreeLibrary(dxgi);
    return peak;
}
#endif

// Set until the first query, then by SotcDisplayPeakChanged.
static std::atomic<bool> peak_stale{true};

void SotcDisplayPeakChanged() {
    peak_stale.store(true, std::memory_order_relaxed);
}

float SotcDisplayPeakNits(void* native_window) {
#ifdef _WIN32
    static bool queried = false;
    static float last_peak = 0.0f;
    if (!peak_stale.exchange(false, std::memory_order_relaxed)) {
        return last_peak;
    }
    const HWND hwnd = static_cast<HWND>(native_window);
    const HMONITOR monitor =
        hwnd && IsWindow(hwnd) ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    const float peak = QueryPeak(monitor);
    if (queried && peak != last_peak) {
        LOG_INFO(Render_Vulkan, "SOTC HDR: the game window's display now reports {} nits", peak);
    }
    queried = true;
    last_peak = peak;
    return peak;
#else
    (void)native_window;
    return 0.0f;
#endif
}

} // namespace Vulkan
