// bbport: SDL3 window for the Vulkan swapchain (X11 or Wayland).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_overlay.h"
#include "video_core/renderdoc.h"

namespace Frontend {

// sotc: the mouse as a PC camera. A click in the window captures it (relative mode, hidden
// cursor); Esc or losing focus releases it. runtime_pad.c takes the motion as the right stick
// and the buttons through its bindings (mouse.<input>=). SOTC_MOUSE=0 leaves the mouse alone.
namespace {
std::atomic<float> g_mouse_dx{0}, g_mouse_dy{0};
std::atomic<u32> g_mouse_buttons{0};
std::atomic<bool> g_mouse_captured{false};
bool MouseEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_MOUSE");
        return !v || v[0] != '0';
    }();
    return enabled;
}
void AddMotion(std::atomic<float>& total, float delta) {
    float old = total.load(std::memory_order_relaxed);
    while (!total.compare_exchange_weak(old, old + delta, std::memory_order_relaxed)) {
    }
}
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
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());

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

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
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

} // namespace Frontend

// sotc: motion since the last call, the held buttons (bit = SDL button number); 0 while the
// mouse is not captured.
extern "C" int bbgpu_mouse_take(float* dx, float* dy, unsigned* buttons) {
    *dx = Frontend::g_mouse_dx.exchange(0, std::memory_order_relaxed);
    *dy = Frontend::g_mouse_dy.exchange(0, std::memory_order_relaxed);
    *buttons = Frontend::g_mouse_buttons.load(std::memory_order_relaxed);
    return Frontend::g_mouse_captured.load(std::memory_order_relaxed) ? 1 : 0;
}
