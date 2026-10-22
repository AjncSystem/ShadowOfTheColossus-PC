// bbport: SDL3 window for the Vulkan swapchain (X11 or Wayland).
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "video_core/renderdoc.h"
#include "video_core/sotc_event.h"
#include "video_core/sotc_stats.h"
#include "video_core/sotc_ui_lang.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Frontend {

// sotc: the mouse as a PC camera. A click in the window captures it (relative mode, hidden
// cursor); Esc or losing focus releases it. runtime_pad.c takes the motion as the right stick
// and the buttons through its bindings (mouse.<input>=). SOTC_MOUSE=0 leaves the mouse alone.
namespace {
std::atomic<float> g_mouse_dx{0}, g_mouse_dy{0};
std::atomic<u32> g_mouse_buttons{0};
std::atomic<bool> g_mouse_captured{false};
void* g_behind_hwnd = nullptr; // sotc: SOTC_WINDOW_BEHIND window (Windows)
bool MouseEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_MOUSE");
        return !v || v[0] != '0';
    }();
    return enabled;
}
// sotc: the present thread flips on a 60 Hz timer (the PS4 vblank), so a 30 FPS frame should stay
// on screen for the same number of refreshes every time. Borderless fullscreen keeps the desktop
// rate: at 74 or 144 Hz the frames alternate between 2 and 3 refreshes and the motion judders.
// When the desktop rate is not a multiple of 60, fullscreen becomes exclusive at the desktop size
// and the highest multiple of 60 the display offers. BB_FULLSCREEN_HZ=<Hz> asks for that rate
// instead; BB_FULLSCREEN_HZ=0 keeps the desktop mode.
bool IsMultipleOf60(float hz) {
    const int n = static_cast<int>(hz / 60.0f + 0.5f);
    return n >= 1 && std::fabs(hz - 60.0f * n) < 0.5f;
}
void PickFullscreenMode(SDL_Window* window) {
    const char* wanted_env = std::getenv("BB_FULLSCREEN_HZ");
    const float wanted = wanted_env && wanted_env[0] ? static_cast<float>(std::atof(wanted_env)) : -1.0f;
    if (wanted == 0.0f) {
        return;
    }
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    if (!desktop || (wanted < 0.0f && IsMultipleOf60(desktop->refresh_rate))) {
        return;
    }
    int count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count);
    const SDL_DisplayMode* best = nullptr;
    for (int i = 0; modes && i < count; i++) {
        const SDL_DisplayMode* mode = modes[i];
        if (mode->w != desktop->w || mode->h != desktop->h) {
            continue;
        }
        if (wanted > 0.0f) {
            if (!best || std::fabs(mode->refresh_rate - wanted) < std::fabs(best->refresh_rate - wanted)) {
                best = mode;
            }
        } else if (IsMultipleOf60(mode->refresh_rate) && (!best || mode->refresh_rate > best->refresh_rate)) {
            best = mode;
        }
    }
    if (best && SDL_SetWindowFullscreenMode(window, best)) {
        SDL_SyncWindow(window);
        std::fprintf(stderr, "Display: exclusive fullscreen %dx%d @ %.2f Hz (desktop %.2f Hz; BB_FULLSCREEN_HZ=0 keeps it)\n",
                     best->w, best->h, best->refresh_rate, desktop->refresh_rate);
    } else {
        std::fprintf(stderr, "Display: desktop %dx%d @ %.2f Hz kept (no %s mode at that size)\n", desktop->w,
                     desktop->h, desktop->refresh_rate, wanted > 0.0f ? "matching" : "60 Hz multiple");
    }
    SDL_free(modes);
}
void AddMotion(std::atomic<float>& total, float delta) {
    float old = total.load(std::memory_order_relaxed);
    while (!total.compare_exchange_weak(old, old + delta, std::memory_order_relaxed)) {
    }
}

#if !defined(_WIN32) && SDL_VERSION_ATLEAST(3, 4, 0)
// The window icon from launcher/sotc.png (on Windows the executable's icon resource gives it).
void SetWindowIcon(SDL_Window* window) {
    const char* base = SDL_GetBasePath(); // out/ in a source tree
    const std::string paths[] = {std::string(base ? base : "") + "../launcher/sotc.png",
                                 "launcher/sotc.png"};
    for (const std::string& path : paths) {
        if (SDL_Surface* icon = SDL_LoadPNG(path.c_str())) {
            SDL_SetWindowIcon(window, icon);
            SDL_DestroySurface(icon);
            return;
        }
    }
}
#else
void SetWindowIcon(SDL_Window*) {}
#endif
} // namespace

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // sotc: the click that brings the window to the front also reaches the game (mouse capture).
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    const char* fullscreen = std::getenv("BB_FULLSCREEN");
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, fullscreen && fullscreen[0] == '1');
    // sotc: SOTC_WINDOW_BEHIND=1 (unattended benchmarks): the window opens without taking the
    // focus and goes behind the other windows, so a test run never interrupts the desktop.
    const char* behind_env = std::getenv("SOTC_WINDOW_BEHIND");
    const bool behind = behind_env && behind_env[0] == '1';
    if (behind) {
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "0");
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    }
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());
    SetWindowIcon(window); // Linux: launcher/sotc.png (PR #118); the launcher's icon below wins
    // sotc: the game's own icon (sce_sys/icon0.png of the player's dump, as a BMP the launcher
    // writes: SOTC_WINDOW_ICON) instead of the executable's.
    if (const char* icon_path = std::getenv("SOTC_WINDOW_ICON"); icon_path && *icon_path) {
        if (SDL_Surface* icon = SDL_LoadBMP(icon_path)) {
            SDL_SetWindowIcon(window, icon);
            SDL_DestroySurface(icon);
        }
    }
    if (const char* version = std::getenv("SOTC_GAME_VERSION"); version && *version) {
        title_suffix = std::string(" · v") + version;
        UpdateTextTitle();
    }
#ifdef _WIN32
    if (behind) {
        if (HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(
                SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))) {
            g_behind_hwnd = hwnd;
            // A WS_EX_NOACTIVATE window never becomes the foreground window, whoever shows it.
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                              GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
            SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
#endif

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef _WIN32
    if (driver && !std::strcmp(driver, "windows")) {
        window_info.type = WindowSystemType::Windows;
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else
#endif
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    if (fullscreen && fullscreen[0] == '1') {
        PickFullscreenMode(window);
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
}

WindowSDL::~WindowSDL() {
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

// sotc: the window title shows the game version and the frame rate, refreshed once a second.
void WindowSDL::UpdateTitleStats() {
    const u64 now = SDL_GetTicks();
    if (now - title_ticks < 1000) {
        return;
    }
    const u64 flips = SotcStats::flip_count.load(std::memory_order_relaxed);
    if (title_ticks != 0) {
        const double fps = double(flips - title_flips) * 1000.0 / double(now - title_ticks);
        const char* version = std::getenv("SOTC_GAME_VERSION");
        char suffix[64];
        std::snprintf(suffix, sizeof(suffix), "%s%s · %.0f FPS",
                      version && *version ? " · v" : "", version ? version : "", fps);
        title_suffix = suffix;
        UpdateTextTitle();
    }
    title_ticks = now;
    title_flips = flips;
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                                          : base_title + title_suffix;
    SDL_SetWindowTitle(window, title.c_str());
    BbOverlay::SetTextEntry(text_active, text_prompt, text);
}

bool WindowSDL::PollEvents() {
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    // sotc: while the in-game menu (or the save list, the text box) is open the mouse is free and
    // visible, so the menu can be clicked; it was left in the camera's relative mode, hidden and
    // held at the window centre. Closing the menu captures it again for the camera.
    {
        static bool released_for_menu = false;
        const bool menu = BbOverlay::CapturesInput();
        if (menu && g_mouse_captured) {
            SDL_SetWindowRelativeMouseMode(window, false);
            g_mouse_captured = false;
            g_mouse_buttons = 0;
            g_mouse_dx = 0;
            g_mouse_dy = 0;
            released_for_menu = true;
        } else if (!menu && released_for_menu) {
            released_for_menu = false;
            if (MouseEnabled() && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)) {
                SDL_SetWindowRelativeMouseMode(window, true);
                g_mouse_captured = true;
            }
        }
    }
    if (!text_active) {
        BbOverlay::UpdateTextInput(window);
        UpdateTitleStats();
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        // The controller finishes the text dialog too: Cross (A) accepts, Circle (B) cancels.
        if (text_active && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
            (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
             event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)) {
            std::scoped_lock lock{text_mutex};
            text_state = event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : 2;
            text_active = false;
            SDL_StopTextInput(window);
            UpdateTextTitle();
            continue;
        }
        if (BbOverlay::HandleEvent(event)) {
            continue;
        }
#ifdef _WIN32
        // sotc: SOTC_WINDOW_BEHIND: a test window that still gets the focus goes back behind.
        if (g_behind_hwnd && event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
            SetWindowPos(static_cast<HWND>(g_behind_hwnd), HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            continue;
        }
#endif
        if (std::getenv("SOTC_MOUSE_DEBUG") &&
            (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_MOTION ||
             event.type == SDL_EVENT_WINDOW_FOCUS_GAINED || event.type == SDL_EVENT_WINDOW_FOCUS_LOST)) {
            static int shown = 0;
            if (shown++ < 20) {
                std::fprintf(stderr, "SOTC mouse debug: event 0x%x\n", event.type);
            }
        }
        if (MouseEnabled()) {
            const auto capture = [this](bool on) {
                SDL_SetWindowRelativeMouseMode(window, on);
                g_mouse_captured = on;
                g_mouse_buttons = 0;
                g_mouse_dx = 0;
                g_mouse_dy = 0;
                SotcEvent::Log("INPUT", "%s", on ? SotcUi::T("Mouse preso na câmera (Esc solta)",
                                                             "Mouse captured for the camera (Esc releases it)")
                                                : SotcUi::T("Mouse solto", "Mouse released"));
                std::fprintf(stderr, "Runtime: mouse %s\n",
                             on ? "captured for the camera (Esc releases it)" : "released");
            };
            if (!g_mouse_captured && (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                                      event.type == SDL_EVENT_WINDOW_FOCUS_GAINED)) {
                capture(true); // a click (or coming back to the game) captures the mouse
                continue;
            }
            if (g_mouse_captured) {
                if (event.type == SDL_EVENT_MOUSE_MOTION) {
                    AddMotion(g_mouse_dx, event.motion.xrel);
                    AddMotion(g_mouse_dy, event.motion.yrel);
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    g_mouse_buttons |= 1u << event.button.button;
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    g_mouse_buttons &= ~(1u << event.button.button);
                } else if ((event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) ||
                           event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    capture(false);
                }
            }
        }
        // F11: fullscreen on/off (BB_FULLSCREEN only sets the state at start).
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F11 && !event.key.repeat) {
            const bool fullscreen = !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
            SDL_SetWindowFullscreen(window, fullscreen);
            SotcEvent::Log("KEY", "%s", fullscreen ? SotcUi::T("F11: tela cheia", "F11: fullscreen")
                                                   : SotcUi::T("F11: janela", "F11: windowed"));
        }
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F12 && !event.key.repeat) {
            VideoCore::TriggerCapture(); // only does something when running under RenderDoc
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    return is_open;
}

void WindowSDL::SetFullscreen(bool fs) {
    if (window) {
        SDL_SetWindowFullscreen(window, fs);
    }
}

bool WindowSDL::IsFullscreen() const {
    return window ? (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0 : false;
}

} // namespace Frontend

// sotc: motion since the last call, the held buttons (bit = SDL button number); 0 while the
// mouse is not captured.
extern "C" int bbgpu_mouse_take(float* dx, float* dy, unsigned* buttons) {
    const auto& s = BbSettings::Get();
    if (!s.mouse_enabled.load(std::memory_order_relaxed)) {
        *dx = 0;
        *dy = 0;
        *buttons = 0;
        return 0;
    }
    const float raw_dx = Frontend::g_mouse_dx.exchange(0, std::memory_order_relaxed);
    const float raw_dy = Frontend::g_mouse_dy.exchange(0, std::memory_order_relaxed);
    const float sens = s.mouse_sens.load(std::memory_order_relaxed);
    const bool invert = s.mouse_invert.load(std::memory_order_relaxed);
    *dx = raw_dx * sens;
    *dy = raw_dy * (invert ? -sens : sens);
    *buttons = Frontend::g_mouse_buttons.load(std::memory_order_relaxed);
    return Frontend::g_mouse_captured.load(std::memory_order_relaxed) ? 1 : 0;
}
