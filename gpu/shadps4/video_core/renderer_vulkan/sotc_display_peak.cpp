// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the HDR display's peak brightness as Windows reports it (IDXGIOutput6::GetDesc1). dxgi.dll
// is loaded here at run time, so nothing new is linked.
#include "video_core/renderer_vulkan/sotc_display_peak.h"

#ifdef _WIN32
#include <windows.h>
#include <dxgi1_6.h>
#endif

namespace Vulkan {

float SotcDisplayPeakNits() {
#ifdef _WIN32
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) {
        return 0.0f;
    }
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    const auto create =
        reinterpret_cast<CreateFactory>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
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
                if (SUCCEEDED(output6->GetDesc1(&desc)) &&
                    desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 &&
                    desc.MaxLuminance > peak) {
                    peak = desc.MaxLuminance;
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
#else
    return 0.0f;
#endif
}

} // namespace Vulkan
