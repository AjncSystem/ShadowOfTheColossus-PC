// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the frame generation mode, see vk_framegen.h.

#include "video_core/renderer_vulkan/vk_framegen.h"
#include "video_core/sotc_ui_lang.h"

#include <windows.h>
#include <dxgi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Vulkan::FrameGen {

namespace {

enum class Asked { Off, Auto, Dlss, Fsr };

Asked AskedMode() {
    static const Asked asked = [] {
        if (const char* v = std::getenv("SOTC_FRAMEGEN"); v && v[0]) {
            if (_stricmp(v, "auto") == 0) {
                return Asked::Auto;
            }
            if (_stricmp(v, "dlss") == 0 || _stricmp(v, "dlssg") == 0) {
                return Asked::Dlss;
            }
            if (_stricmp(v, "fsr") == 0 || _stricmp(v, "fsr3") == 0) {
                return Asked::Fsr;
            }
            if (_stricmp(v, "off") != 0 && v[0] != '0') {
                std::fprintf(stderr, "SOTC FRAMEGEN: unknown SOTC_FRAMEGEN=%s, off\n", v);
            }
            return Asked::Off;
        }
        const char* legacy = std::getenv("SOTC_DLSSG");
        return legacy && legacy[0] == '1' ? Asked::Dlss : Asked::Off;
    }();
    return asked;
}

/// Whether the GPU the port will run on is NVIDIA (DXGI, before Vulkan): Streamline is for those
/// only. Any NVIDIA adapter was enough before, and on a PC with an RX 9070 next to an NVIDIA card
/// Streamline wrapped the AMD device, whose creation then failed (VK_NV_low_latency2, #194). This
/// follows Instance's choice: SOTC_GPU_VENDOR, else the most dedicated memory (discrete first).
bool HasNvidiaGpu() {
    if (const char* vendor = std::getenv("SOTC_GPU_VENDOR")) {
        return std::strtoul(vendor, nullptr, 16) == 0x10DE;
    }
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) {
        return false;
    }
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    const auto create = reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    bool nvidia = false;
    SIZE_T best = 0;
    IDXGIFactory1* factory = nullptr;
    if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                desc.DedicatedVideoMemory > best) {
                best = desc.DedicatedVideoMemory;
                nvidia = desc.VendorId == 0x10DE;
            }
            adapter->Release();
        }
        factory->Release();
    }
    return nvidia;
}

std::atomic<int> current{static_cast<int>(Mode::Off)};
std::atomic<bool> have_dlss{false}, have_fsr{false};

} // namespace

bool WantStreamline() {
    // With fsr too, so that F8 can still switch to DLSS-G.
    if (AskedMode() == Asked::Off) {
        return false;
    }
    if (!HasNvidiaGpu()) {
        std::fprintf(stderr, "SOTC FRAMEGEN: no NVIDIA GPU, DLSS-G unavailable (FSR 3 possible)\n");
        return false;
    }
    return true;
}

void SetAvailable(bool dlss, bool fsr) {
    have_dlss = dlss;
    have_fsr = fsr;
    Mode mode = Mode::Off;
    switch (AskedMode()) {
    case Asked::Off:
        break;
    case Asked::Auto:
        mode = dlss ? Mode::Dlss : fsr ? Mode::Fsr : Mode::Off;
        break;
    case Asked::Dlss:
        mode = dlss ? Mode::Dlss : Mode::Off;
        break;
    case Asked::Fsr:
        mode = fsr ? Mode::Fsr : Mode::Off;
        break;
    }
    current = static_cast<int>(mode);
    std::fprintf(stderr, "SOTC FRAMEGEN: %s (DLSS-G %s, FSR 3 %s; F8 switches)\n", Name(mode),
                 dlss ? "available" : "unavailable", fsr ? "available" : "unavailable");
}

Mode Current() {
    return static_cast<Mode>(current.load(std::memory_order_relaxed));
}

bool AskedAtStart() {
    return AskedMode() != Asked::Off;
}

bool Available(Mode mode) {
    switch (mode) {
    case Mode::Off:
        return true;
    case Mode::Dlss:
        return have_dlss;
    case Mode::Fsr:
        return have_fsr;
    }
    return false;
}

bool Set(Mode mode) {
    if (!Available(mode)) {
        return false;
    }
    current = static_cast<int>(mode);
    std::fprintf(stderr, "SOTC FRAMEGEN: switched to %s\n", Name(mode));
    return true;
}

Mode Cycle() {
    Mode mode = Current();
    for (int i = 0; i < 3; ++i) {
        mode = static_cast<Mode>((static_cast<int>(mode) + 1) % 3);
        if (Available(mode)) {
            break;
        }
    }
    current = static_cast<int>(mode);
    std::fprintf(stderr, "SOTC FRAMEGEN: switched to %s\n", Name(mode));
    return mode;
}

void TestTick() {
    static const double every = [] {
        const char* v = std::getenv("SOTC_FRAMEGEN_TEST_CYCLE");
        return v ? std::atof(v) : 0.0;
    }();
    if (every <= 0.0) {
        return;
    }
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - last).count() >= every) {
        last = now;
        Cycle();
    }
}

const char* Name(Mode mode) {
    switch (mode) {
    case Mode::Off:
        return SotcUi::T("desligada", "off");
    case Mode::Dlss:
        return "DLSS-G";
    case Mode::Fsr:
        return "FSR 3";
    }
    return "?";
}

} // namespace Vulkan::FrameGen
