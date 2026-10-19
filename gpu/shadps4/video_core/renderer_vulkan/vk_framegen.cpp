// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the frame generation mode, see vk_framegen.h.

#include "video_core/renderer_vulkan/vk_framegen.h"
#include "video_core/sotc_ui_lang.h"

#ifdef _WIN32
#include <windows.h>
#include <dxgi.h>
#else
#include <strings.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Vulkan::FrameGen {

namespace {

enum class Asked { Off, Auto, Dlss, Fsr };

bool SameIgnoringCase(const char* a, const char* b) {
#ifdef _WIN32
    return _stricmp(a, b) == 0;
#else
    return strcasecmp(a, b) == 0;
#endif
}

Asked AskedMode() {
    static const Asked asked = [] {
        if (const char* v = std::getenv("SOTC_FRAMEGEN"); v && v[0]) {
            if (SameIgnoringCase(v, "auto")) {
                return Asked::Auto;
            }
            if (SameIgnoringCase(v, "dlss") || SameIgnoringCase(v, "dlssg")) {
                return Asked::Dlss;
            }
            if (SameIgnoringCase(v, "fsr") || SameIgnoringCase(v, "fsr3")) {
                return Asked::Fsr;
            }
            if (!SameIgnoringCase(v, "off") && v[0] != '0') {
                std::fprintf(stderr, "SOTC FRAMEGEN: unknown SOTC_FRAMEGEN=%s, off\n", v);
            }
            return Asked::Off;
        }
        const char* legacy = std::getenv("SOTC_DLSSG");
        return legacy && legacy[0] == '1' ? Asked::Dlss : Asked::Off;
    }();
    return asked;
}

#ifdef _WIN32
/// An NVIDIA GPU in the system (DXGI, before Vulkan): Streamline is for those only.
bool HasNvidiaGpu() {
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) {
        return false;
    }
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    const auto create = reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    bool found = false;
    IDXGIFactory1* factory = nullptr;
    if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; !found && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                found = desc.VendorId == 0x10DE;
            }
            adapter->Release();
        }
        factory->Release();
    }
    return found;
}
#endif

std::atomic<int> current{static_cast<int>(Mode::Off)};
std::atomic<bool> have_dlss{false}, have_fsr{false};

} // namespace

bool WantStreamline() {
    // With fsr too, so that F8 can still switch to DLSS-G.
    if (AskedMode() == Asked::Off) {
        return false;
    }
#ifndef _WIN32
    std::fprintf(stderr, "SOTC FRAMEGEN: DLSS-G is Windows only (Streamline), FSR 3 possible\n");
    return false;
#else
    if (!HasNvidiaGpu()) {
        std::fprintf(stderr, "SOTC FRAMEGEN: no NVIDIA GPU, DLSS-G unavailable (FSR 3 possible)\n");
        return false;
    }
    return true;
#endif
}

bool HasNvidia() {
    return HasNvidiaGpu();
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
