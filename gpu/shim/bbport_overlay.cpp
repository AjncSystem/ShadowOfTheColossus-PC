// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <string>

#include <SDL3/SDL.h>
#include "bbport_settings.h"
#include "sdl_window.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "stb_image.h"
#include <unordered_map>
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_framegen.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_fps_cap.h"
#include "video_core/sotc_event.h"
#include "video_core/sotc_live.h"
#include "video_core/sotc_ui_lang.h"
#include "video_core/sotc_stats.h"

extern Frontend::WindowSDL* g_window;
static const Vulkan::Instance* s_instance = nullptr;
// sotc: the volume of the runtime's audio output (src/runtime_audio.c); weak, so builds without the
// runtime (tests) still link.
extern "C" __attribute__((weak)) void runtime_audio_set_volume_percent(int pct);
extern "C" __attribute__((weak)) int runtime_audio_volume_percent(void);

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
bool dirty = false; // settings changed while open: saved on close
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
// The game's save/load slot list (OpenSaveList), guarded by imgui_mutex.
bool list_active = false;
std::string list_title;
std::vector<std::string> list_items;
// sotc: each save's picture (icon0.png of its sce_sys), shown to the left of its text as in the
// PS4 and shadPS4 lists. Loaded once per file, kept for the session.
std::vector<std::string> list_icons, pending_icons;
std::unordered_map<std::string, ImTextureData*> icon_textures;

ImTextureData* IconTexture(const std::string& path) {
    if (path.empty()) {
        return nullptr;
    }
    if (const auto it = icon_textures.find(path); it != icon_textures.end()) {
        return it->second;
    }
    ImTextureData* tex = nullptr;
    int w = 0, h = 0, n = 0;
    if (unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &n, 4)) {
        tex = IM_NEW(ImTextureData)();
        tex->Create(ImTextureFormat_RGBA32, w, h);
        std::memcpy(tex->GetPixels(), pixels, size_t(w) * h * 4);
        stbi_image_free(pixels);
        ImGui::RegisterUserTexture(tex);
    }
    icon_textures[path] = tex;
    return tex;
}
int list_selected = 0, list_result = -1;
int list_confirm = -1;  // item waiting for the delete confirmation, or -1
int list_first_deletable = 0;
bool list_scroll = false, list_stick_down = false, list_stick_up = false;
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

// sotc: the settings menu is Bloodborne's; the SotC port shows only the game's dialogs
// (BB_OVERLAY_MENU=1 opens the menu anyway).
bool MenuAllowed() {
    const char* v = std::getenv("BB_OVERLAY_MENU");
    if (v && v[0] == '0') return false;
    return true;
}

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    SotcEvent::Log("KEY", "%s", value ? SotcUi::T("Menu do jogo aberto", "In-game menu opened")
                                      : SotcUi::T("Menu do jogo fechado", "In-game menu closed"));
    ImGui::GetIO().MouseDrawCursor = value;
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

// sotc: a short message at the top of the screen (F8: the frame generation mode): its text
// (under imgui_mutex) and until when, in steady clock milliseconds.
std::string toast_text;
std::atomic<long long> toast_until_ms{0};

// sotc: when the last game flip came (steady clock milliseconds), kept by LoadingShown.
std::atomic<long long> last_flip_ms{0};

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool ToastShown() {
    return NowMs() < toast_until_ms.load(std::memory_order_relaxed);
}

// sotc: the performance overlay (show_fps=1 in sotc.ini, or SOTC_FPS / SOTC_OVERLAY=1; F10
// toggles it). History of present intervals (one per game flip), present thread only.
constexpr int kHistory = 240;
float frame_hist[kHistory]{};
int hist_pos = 0, hist_count = 0;
uint64_t session_frames = 0, session_late = 0, session_stutters = 0;
std::atomic<bool> overlay_on{[] {
    if (const char* v = std::getenv("SOTC_OVERLAY")) {
        return v[0] != '0';
    }
    return std::getenv("SOTC_FPS") != nullptr;
}()};

// Frame time the game aims for: 33.3 ms (30 FPS) or 16.7 ms (60 FPS), from the median.
float TargetMs() {
    if (hist_count < 8) {
        return 33.3f;
    }
    float sorted[kHistory];
    std::copy(frame_hist, frame_hist + hist_count, sorted);
    std::nth_element(sorted, sorted + hist_count / 2, sorted + hist_count);
    return sorted[hist_count / 2] < 25.0f ? 16.7f : 33.3f;
}

bool OverlayOn() {
    return BbSettings::Get().show_fps || overlay_on.load(std::memory_order_relaxed);
}

void RecordFrame(float ms) {
    frame_hist[hist_pos] = ms;
    hist_pos = (hist_pos + 1) % kHistory;
    hist_count = std::min(hist_count + 1, kHistory);
    ++session_frames;
    if (ms > TargetMs() * 1.1f) {
        ++session_late;
    }
    if (ms > 50.0f) {
        ++session_stutters;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    Store(value, v, ImGui::Checkbox(label, &v));
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    Store(value, v, ImGui::SliderFloat(label, &v, lo, hi, "%.2f"));
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// sotc: the launcher's settings file (SOTC_LAUNCHER_SETTINGS, user/launcher_settings.json), so a
// quick setting changed in the menu is also the launcher's choice next time. Only keys already in
// the file are replaced.
void SaveLauncherSetting(const char* key, const std::string& json_value) {
#ifdef _WIN32
    const wchar_t* wpath = _wgetenv(L"SOTC_LAUNCHER_SETTINGS");
    if (!wpath || !wpath[0]) {
        return;
    }
    const std::filesystem::path path{wpath};
#else
    const char* cpath = std::getenv("SOTC_LAUNCHER_SETTINGS");
    if (!cpath || !cpath[0]) {
        return;
    }
    const std::filesystem::path path{cpath};
#endif
    std::string text;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return;
        }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::regex re("(\"" + std::string(key) +
                        "\"\\s*:\\s*)(\"[^\"]*\"|true|false|-?[0-9.]+)");
    std::smatch m;
    if (!std::regex_search(text, m, re)) {
        return;
    }
    text = m.prefix().str() + m[1].str() + json_value + m.suffix().str();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// sotc: SOTC_LOD_POOL_LOW at start (8 when unset): the page pool switch of texture level "High".
int EnvPoolLow() {
    static const int value = [] {
        const char* v = std::getenv("SOTC_LOD_POOL_LOW");
        return v ? std::atoi(v) : 8;
    }();
    return value;
}

// sotc: texture level (#235): 0 off; 1 high, with the page pool switch (coarser mips while the
// game's page pool is low, sotc_lod_stats.h); 2 maximum, without it. Applies at once
// (sotc_live.h) and is kept as the launcher's choice.
void SetTextureLevel(int level) {
    SotcLive::lod_linger.store(level > 0 ? 1 : 0, std::memory_order_relaxed);
    SotcLive::lod_pool_low.store(level == 2 ? 0 : (EnvPoolLow() > 0 ? EnvPoolLow() : 8),
                                 std::memory_order_relaxed);
    const char* keys[] = {"\"off\"", "\"high\"", "\"max\""};
    SaveLauncherSetting("textures", keys[level]);
    const char* names[] = {SotcUi::T("Desligada", "Off"), SotcUi::T("Alta", "High"),
                           SotcUi::T("Máxima", "Maximum")};
    SotcEvent::Log("MENU", SotcUi::T("Qualidade das texturas: %s", "Texture quality: %s"), names[level]);
}

// sotc: FPS mode 0 = 30 (the game's cap), 1 = 60, 2 = unlimited, as the launcher's.
void SetFpsMode(int mode) {
    if (mode > 0) {
        SotcFpsCap::limit_fps.store(mode == 1 ? 60 : 0, std::memory_order_relaxed);
    }
    if ((mode > 0) != SotcFpsCap::uncapped.load(std::memory_order_relaxed)) {
        SotcFpsCap::Toggle();
    }
    const char* keys[] = {"\"30\"", "\"60\"", "\"unlimited\""};
    SaveLauncherSetting("fps_mode", keys[mode]);
    const char* names[] = {"30", "60", SotcUi::T("ilimitado", "unlimited")};
    SotcEvent::Log("MENU", "FPS: %s", names[mode]);
}

// sotc: upscaler 0 = FSR 1, 1 = bilinear; applied at the next frame (vk_presenter.cpp).
void SetUpscaler(int mode) {
    SotcLive::fsr1.store(mode == 0 ? 1 : 0, std::memory_order_relaxed);
    SaveLauncherSetting("upscaler", mode == 0 ? "\"fsr1\"" : "\"bilinear\"");
    SotcEvent::Log("MENU", "Upscaler: %s", mode == 0 ? "FSR 1" : "Bilinear");
}

// sotc: RCAS sharpening 0 high, 1 medium, 2 low, 3 off (the launcher's SHARPNESS values).
constexpr int RcasValues[] = {0, 250, 600, -2};
void SetRcas(int mode) {
    SotcLive::rcas.store(RcasValues[mode], std::memory_order_relaxed);
    const char* keys[] = {"\"0\"", "\"250\"", "\"600\"", "\"off\""};
    SaveLauncherSetting("sharpness", keys[mode]);
    const char* names[] = {SotcUi::T("Alta", "High"), SotcUi::T("Média", "Medium"),
                           SotcUi::T("Baixa", "Low"), SotcUi::T("Desligada", "Off")};
    SotcEvent::Log("MENU", SotcUi::T("Nitidez (RCAS): %s", "Sharpening (RCAS): %s"), names[mode]);
}

// sotc: "Quick settings": the launcher's quick settings that can change while playing.
void QuickSettings() {
    using SotcUi::T;
    ImGui::SeparatorText(T("Ajuste rápido (vale na hora)", "Quick settings (apply at once)"));

    // sotc: FPS as the launcher's: 30 (the game's cap), 60 or unlimited. F7 switches the cap.
    static const int env_limit = [] {
        const char* v = std::getenv("SOTC_FPS_LIMIT");
        return v ? std::atoi(v) : 0;
    }();
    const bool uncapped = SotcFpsCap::uncapped.load(std::memory_order_relaxed);
    const int live_limit = SotcFpsCap::limit_fps.load(std::memory_order_relaxed);
    const int fps_mode = !uncapped ? 0 : (live_limit >= 0 ? live_limit : env_limit) > 0 ? 1 : 2;
    const char* fps_names[] = {T("30 FPS (como no PS4)", "30 FPS (as on PS4)"), "60 FPS",
                               T("Ilimitado", "Unlimited")};
    if (ImGui::BeginCombo("FPS  [F7]", fps_names[fps_mode])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(fps_names[i], i == fps_mode) && i != fps_mode) {
                SetFpsMode(i);
            }
        }
        ImGui::EndCombo();
    }
    // Started at 30 FPS the game is not in its own 60 FPS mode (fps60game, chosen at start):
    // uncapped it runs faster, but some physics may be off (the 2nd colossus' wall).
    if (uncapped && !SotcFpsCap::target_slot.load(std::memory_order_relaxed)) {
        Hint(T("Aberto em 30 FPS: o modo 60 do próprio jogo (física certa) só entra abrindo o jogo "
               "com 60 FPS no lançador.",
               "Started at 30 FPS: the game's own 60 FPS mode (right physics) only applies when the "
               "game starts with 60 FPS in the launcher."));
    }

    // sotc: upscaler and sharpening, applied at the next frame (vk_presenter.cpp, sotc_live.h).
    static const int env_fsr1 = [] {
        const char* v = std::getenv("BB_FSR1");
        return v && v[0] != '0' ? 1 : 0;
    }();
    const int live_fsr1 = SotcLive::fsr1.load(std::memory_order_relaxed);
    const int up_mode = (live_fsr1 >= 0 ? live_fsr1 : env_fsr1) ? 0 : 1;
    const char* up_names[] = {T("FSR 1 (AMD, qualquer placa)", "FSR 1 (AMD, any GPU)"),
                              T("Bilinear (simples)", "Bilinear (plain)")};
    if (ImGui::BeginCombo("Upscaler", up_names[up_mode])) {
        for (int i = 0; i < 2; ++i) {
            if (ImGui::Selectable(up_names[i], i == up_mode) && i != up_mode) {
                SetUpscaler(i);
            }
        }
        ImGui::EndCombo();
    }
    static const int env_rcas = [] {
        const char* on = std::getenv("BB_RCAS");
        if (on && on[0] == '0') {
            return -2;
        }
        const char* v = std::getenv("BB_RCAS_ATTENUATION");
        return v ? std::atoi(v) : 250;
    }();
    const int live_rcas = SotcLive::rcas.load(std::memory_order_relaxed);
    const int rcas_now = live_rcas != -1 ? live_rcas : env_rcas;
    const char* rcas_names[] = {T("Alta", "High"), T("Média (padrão)", "Medium (default)"),
                                T("Baixa", "Low"), T("Desligada", "Off")};
    int rcas_mode = 1;
    for (int i = 0; i < 4; ++i) {
        if (RcasValues[i] == rcas_now) {
            rcas_mode = i;
        }
    }
    if (ImGui::BeginCombo(T("Nitidez (RCAS)", "Sharpening (RCAS)"), rcas_names[rcas_mode])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(rcas_names[i], i == rcas_mode) && i != rcas_mode) {
                SetRcas(i);
            }
        }
        ImGui::EndCombo();
    }
    if (!(live_fsr1 >= 0 ? live_fsr1 : env_fsr1)) {
        Hint(T("A nitidez vale com o FSR 1.", "Sharpening applies with FSR 1."));
    }

    using Vulkan::FrameGen::Mode;
    const Mode fg = Vulkan::FrameGen::Current();
    const char* fg_names[] = {T("Desligada", "Off"), "NVIDIA DLSS-G", "AMD FSR 3"};
    const char* fg_keys[] = {"\"off\"", "\"dlss\"", "\"fsr\""};
    if (ImGui::BeginCombo(T("Geração de quadros  [F8]", "Frame generation  [F8]"),
                          fg_names[static_cast<int>(fg)])) {
        for (int i = 0; i < 3; ++i) {
            const Mode mode = static_cast<Mode>(i);
            const bool available = Vulkan::FrameGen::Available(mode);
            if (!available) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Selectable(fg_names[i], mode == fg) && available &&
                Vulkan::FrameGen::Set(mode)) {
                SaveLauncherSetting("framegen", fg_keys[i]);
                SotcEvent::Log("MENU", SotcUi::T("Geração de quadros: %s", "Frame generation: %s"),
                               fg_names[i]);
            }
            if (!available) {
                ImGui::EndDisabled();
            }
        }
        ImGui::EndCombo();
    }

    static const int env_linger = [] {
        const char* v = std::getenv("SOTC_LOD_LINGER");
        return v ? std::atoi(v) : 0;
    }();
    const int live_linger = SotcLive::lod_linger.load(std::memory_order_relaxed);
    const int live_low = SotcLive::lod_pool_low.load(std::memory_order_relaxed);
    const bool hq = (live_linger >= 0 ? live_linger : env_linger) > 0;
    const int tex_level = !hq ? 0 : (live_low >= 0 ? live_low : EnvPoolLow()) > 0 ? 1 : 2;
    const char* tex_names[] = {T("Desligada", "Off"), T("Alta (estável)", "High (stable)"),
                               T("Máxima (nítida)", "Maximum (sharp)")};
    // sotc: no "Maximum" in PS4 Pro mode: there it exhausted the game's page pool within minutes
    // at the Shrine (0x1a371, user, 22 Oct).
    static const bool neo = [] {
        const char* v = std::getenv("SOTC_NEO");
        return v && v[0] == '1';
    }();
    if (ImGui::BeginCombo(T("Qualidade das texturas", "Texture quality"), tex_names[tex_level])) {
        for (int i = 0; i < 3; ++i) {
            const bool blocked = neo && i == 2;
            if (blocked) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Selectable(tex_names[i], i == tex_level) && i != tex_level && !blocked) {
                SetTextureLevel(i);
            }
            if (blocked) {
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("%s", T("— não no modo PS4 Pro", "— not in PS4 Pro mode"));
            }
        }
        ImGui::EndCombo();
    }
    Hint(T("Alta: folhas e pedras nítidas, um pouco menos em vistas amplas. Máxima: sempre "
           "nítida, mas pode fechar em vistas muito amplas (fora do modo PS4 Pro, onde fechava "
           "em minutos). Alta e Máxima valem na hora (o jogo leva alguns segundos para carregar "
           "as texturas); Desligada só aparece ao abrir o jogo de novo (o jogo não descarta o "
           "que já carregou).",
           "High: sharp leaves and rocks, a little less in wide views. Maximum: always sharp, "
           "but it may close in very wide views (not in PS4 Pro mode, where it closed within "
           "minutes). High and Maximum apply at once (the game takes a few seconds to load the "
           "textures); Off only shows when the game starts again (the game keeps what it already "
           "loaded)."));

    static const int env_reuse = [] {
        const char* v = std::getenv("SOTC_SHADOW_REUSE");
        if (!v) {
            return 0;
        }
        if (v[0] == 'a') {
            return s_instance && s_instance->GetVendorID() == 0x10DE ? 1 : 0;
        }
        return v[0] == '0' ? 0 : 1;
    }();
    const int live_reuse = SotcLive::shadow_reuse.load(std::memory_order_relaxed);
    bool reuse = (live_reuse >= 0 ? live_reuse : env_reuse) > 0;
    if (ImGui::Checkbox(T("Reaproveitar sombras distantes (podem piscar)",
                          "Reuse far shadows (they may blink)"),
                        &reuse)) {
        SotcLive::shadow_reuse.store(reuse ? 1 : 0, std::memory_order_relaxed);
        SaveLauncherSetting("shadow_reuse", reuse ? "\"1\"" : "\"0\"");
        SotcEvent::Log("MENU", SotcUi::T("Reaproveitar sombras distantes: %s", "Reuse far shadows: %s"),
                       reuse ? SotcUi::T("ligado", "on") : SotcUi::T("desligado", "off"));
    }

    if (runtime_audio_volume_percent && runtime_audio_set_volume_percent) {
        static const int volumes[] = {100, 150, 200, 300};
        const int now = runtime_audio_volume_percent();
        char label[16];
        std::snprintf(label, sizeof(label), "%d%%", now);
        if (ImGui::BeginCombo(T("Volume", "Volume"), label)) {
            for (const int v : volumes) {
                char item[16];
                std::snprintf(item, sizeof(item), "%d%%", v);
                if (ImGui::Selectable(item, v == now)) {
                    runtime_audio_set_volume_percent(v);
                    SaveLauncherSetting("volume", "\"" + std::to_string(v) + "\"");
                    SotcEvent::Log("MENU", "Volume: %d%%", v);
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", T("Resolução, efeitos (névoa, vinheta, SSAO), AF 16x, modo PS4 Pro e "
                                "memória de vídeo: no lançador (valem ao abrir o jogo de novo).",
                                "Resolution, effects (fog, vignette, SSAO), 16x AF, PS4 Pro mode and "
                                "video memory use: in the launcher (they apply when the game starts "
                                "again)."));
    ImGui::PopStyleColor();
}

void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 40.0f * base_scale,
                                   viewport->WorkPos.y + 40.0f * base_scale),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(660.0f * base_scale, 0.0f), ImGuiCond_Appearing);
    bool keep_open = true;
    const std::string title = std::string("Shadow of the Colossus — ") +
                              SotcUi::T("Configurações", "Settings") + "  (Insert / F1 / L3+R3)###menu";
    if (!ImGui::Begin(title.c_str(), &keep_open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // --- Header: FPS, GPU & VRAM info ---
    ImGui::Text("%.0f FPS  (%.1f ms)", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                frame_ms_avg);
    if (s_instance) {
        const u64 budget_mb = s_instance->GetTotalMemoryBudget() / (1024 * 1024);
        const u64 used_mb = s_instance->GetDeviceMemoryUsage() / (1024 * 1024);
        ImGui::SameLine();
        ImGui::TextDisabled(" |  %s (%lu / %lu MB VRAM)",
                            std::string(s_instance->GetModelName()).c_str(),
                            static_cast<unsigned long>(used_mb),
                            static_cast<unsigned long>(budget_mb));
    }

    QuickSettings();

    // sotc: only what the SotC port reads. Bloodborne's output resolution, temporal upscalers
    // (FSR 3.1/4, DLSS, TAA, presets), RCAS strength and engine effects were shown here but never
    // applied: those are the launcher's (environment and engine cvars at start).
    ImGui::SeparatorText(SotcUi::T("Tela", "Display"));
    bool fs = s.fullscreen.load();
    if (ImGui::Checkbox(SotcUi::T("Tela cheia  [F11]", "Fullscreen  [F11]"), &fs)) {
        Store(s.fullscreen, fs, true);
        if (g_window) {
            g_window->SetFullscreen(fs);
        }
        SotcEvent::Log("MENU", SotcUi::T("Tela cheia: %s", "Fullscreen: %s"),
                       fs ? SotcUi::T("ligada", "on") : SotcUi::T("desligada", "off"));
    }
    ImGui::SameLine();
    Checkbox(SotcUi::T("Mostrar FPS  [F10]", "Show FPS  [F10]"), s.show_fps);

    ImGui::SeparatorText(SotcUi::T("Câmera e mouse", "Camera & mouse"));
    if (const bool was = s.mouse_enabled.load(); Checkbox(SotcUi::T("Câmera pelo mouse", "Mouse camera"), s.mouse_enabled), was != s.mouse_enabled.load()) {
        SotcEvent::Log("MENU", SotcUi::T("Câmera pelo mouse: %s", "Mouse camera: %s"),
                       s.mouse_enabled.load() ? SotcUi::T("ligada", "on") : SotcUi::T("desligada", "off"));
    }
    ImGui::BeginDisabled(!s.mouse_enabled);
    Slider(SotcUi::T("Sensibilidade do mouse", "Mouse sensitivity"), s.mouse_sens, 0.1f, 3.0f);
    if (const bool was = s.mouse_invert.load(); Checkbox(SotcUi::T("Inverter eixo vertical (Y)", "Invert vertical axis (Y)"), s.mouse_invert), was != s.mouse_invert.load()) {
        SotcEvent::Log("MENU", SotcUi::T("Inverter eixo Y do mouse: %s", "Invert mouse Y axis: %s"),
                       s.mouse_invert.load() ? SotcUi::T("ligado", "on") : SotcUi::T("desligado", "off"));
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button(SotcUi::T("Fechar", "Close"))) {
        keep_open = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", SotcUi::T("Esc / F1 fecha; tudo fica salvo",
                                        "Esc / F1 closes; everything is kept"));

    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("Controller: Cross (A) = OK, Circle (B) = cancel");
    ImGui::End();
}

// The game's save/load slot list: one entry per save, the selected one highlighted.
void SaveListBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.46f, viewport->WorkSize.y * 0.62f),
                             ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::Begin("##save_list", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(1.25f);
    ImGui::PushTextWrapPos(0.0f); // sotc: save dialog messages are whole sentences
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", list_title.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    const float footer = ImGui::GetTextLineHeightWithSpacing() * 2.2f;
    ImGui::BeginChild("##items", ImVec2(0.0f, -footer));
    // sotc: rows with the save's picture on the left (16:9 of 228x128) and its text on the right.
    const bool any_icon = std::any_of(list_icons.begin(), list_icons.end(),
                                      [](const std::string& p) { return !p.empty(); });
    const float row_h = any_icon ? ImGui::GetTextLineHeightWithSpacing() * 4.2f : 0.0f;
    for (int i = 0; i < static_cast<int>(list_items.size()); ++i) {
        ImGui::PushID(i);
        const bool selected = i == list_selected;
        ImTextureData* icon = i < static_cast<int>(list_icons.size()) ? IconTexture(list_icons[i]) : nullptr;
        const ImVec2 start = ImGui::GetCursorPos();
        const bool clicked =
            any_icon ? ImGui::Selectable("##row", selected,
                                         ImGuiSelectableFlags_AllowDoubleClick |
                                             ImGuiSelectableFlags_AllowOverlap,
                                         ImVec2(0.0f, row_h))
                     : ImGui::Selectable(list_items[i].c_str(), selected,
                                         ImGuiSelectableFlags_AllowDoubleClick);
        if (clicked) {
            list_selected = i;
            if (ImGui::IsMouseDoubleClicked(0)) {
                list_result = i;
            }
        }
        if (any_icon) {
            const ImVec2 end = ImGui::GetCursorPos();
            const float pad = row_h * 0.06f;
            const float img_h = row_h - pad * 2.0f;
            const float img_w = icon ? img_h * float(icon->Width) / float(std::max(icon->Height, 1))
                                     : img_h * 228.0f / 128.0f;
            ImGui::SetCursorPos(ImVec2(start.x + pad, start.y + pad));
            if (icon) {
                ImGui::Image(icon->GetTexRef(), ImVec2(img_w, img_h));
            } else {
                ImGui::Dummy(ImVec2(img_w, img_h));
            }
            const float text_x = start.x + pad * 3.0f + img_w;
            const float text_h = ImGui::CalcTextSize(list_items[i].c_str()).y;
            ImGui::SetCursorPos(ImVec2(text_x, start.y + std::max(pad, (row_h - text_h) * 0.5f)));
            ImGui::TextUnformatted(list_items[i].c_str());
            ImGui::SetCursorPos(end);
        }
        if (selected && list_scroll) {
            ImGui::SetScrollHereY(0.5f);
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    list_scroll = false;
    ImGui::EndChild();
    ImGui::Separator();
    if (list_confirm >= 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                           "Apagar este save?   Enter / X (A): sim    Esc / O (B): nao");
    } else if (list_first_deletable >= static_cast<int>(list_items.size())) { // sotc: YES/NO
        ImGui::TextDisabled("Setas / direcional: escolher    Enter / X (A): confirmar    "
                            "Esc / O (B): nao");
    } else {
        ImGui::TextDisabled("Arrows / D-pad: Select    Enter / Cross (A): Confirm    "
                            "Esc / Circle (B): Back    Del / Triangle (Y): Delete");
    }
    ImGui::End();
}

// sotc: the port's loading screen while the game shows nothing (boot, shader cache, long loads):
// the title, a turning arc and "Loading…" in the launcher's language, over a dark backdrop.
void LoadingScreen() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    // sotc: a hitch of a second or two in play only dims the picture; after 3 s without a game
    // frame (boot, shader cache, long loads) the backdrop turns opaque within half a second, so the
    // game's last frame does not show through looking frozen.
    const long long stall = NowMs() - last_flip_ms.load(std::memory_order_relaxed);
    ImGui::SetNextWindowBgAlpha(std::clamp(0.55f + float(stall - 3000) / 500.0f * 0.45f, 0.55f, 1.0f));
    ImGui::Begin("##loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 size = viewport->WorkSize;
    const ImVec2 origin = viewport->WorkPos;
    const float unit = size.y / 1080.0f;
    const ImU32 dim = IM_COL32(150, 150, 150, 255);
    const float t = float(NowMs() % 1000000) / 1000.0f;
    // The title breathes slowly (gold, 65-100%).
    const char* title = "SHADOW OF THE COLOSSUS";
    const float title_px = 46.0f * unit;
    const ImVec2 title_size = ImGui::GetFont()->CalcTextSizeA(title_px, FLT_MAX, 0.0f, title);
    const int glow = int(255.0f * (0.825f + 0.175f * std::sin(t * 2.0f)));
    const ImU32 gold = IM_COL32(214, 184, 120, glow);
    const float title_y = origin.y + size.y * 0.40f;
    draw->AddText(ImGui::GetFont(), title_px,
                  ImVec2(origin.x + (size.x - title_size.x) * 0.5f, title_y), gold, title);
    // A turning arc under the title, with a faint full ring behind it.
    const ImVec2 centre(origin.x + size.x * 0.5f, title_y + title_px + 70.0f * unit);
    const float radius = 22.0f * unit;
    draw->AddCircle(centre, radius, IM_COL32(214, 184, 120, 50), 48, 3.0f * unit);
    draw->PathArcTo(centre, radius, t * 5.0f, t * 5.0f + 1.6f, 32);
    draw->PathStroke(IM_COL32(214, 184, 120, 255), 0, 3.5f * unit);
    // "Loading" with moving dots.
    char text[64];
    const int dots = int(t * 2.5f) % 4;
    std::snprintf(text, sizeof(text), "%s%.*s", SotcUi::T("Carregando", "Loading"), dots, "...");
    const float text_px = 24.0f * unit;
    const ImVec2 full = ImGui::GetFont()->CalcTextSizeA(
        text_px, FLT_MAX, 0.0f, SotcUi::T("Carregando...", "Loading..."));
    draw->AddText(ImGui::GetFont(), text_px,
                  ImVec2(centre.x - full.x * 0.5f, centre.y + radius + 22.0f * unit), dim, text);
    // While the shader cache is read at boot: a bar with the percentage.
    const u32 total = SotcStats::warmup_total.load(std::memory_order_relaxed);
    if (total > 0) {
        const u32 done = std::min(SotcStats::warmup_done.load(std::memory_order_relaxed), total);
        const float frac = float(done) / float(total);
        const float bar_w = size.x * 0.4f, bar_h = 10.0f * unit;
        const ImVec2 bar0(origin.x + (size.x - bar_w) * 0.5f, centre.y + radius + 70.0f * unit);
        draw->AddRectFilled(bar0, ImVec2(bar0.x + bar_w, bar0.y + bar_h), IM_COL32(60, 60, 64, 255), bar_h * 0.5f);
        draw->AddRectFilled(bar0, ImVec2(bar0.x + bar_w * frac, bar0.y + bar_h), gold, bar_h * 0.5f);
        char line[128];
        std::snprintf(line, sizeof(line), "%s %d%%  (%u / %u)", SotcUi::T("Carregando shaders…", "Loading shaders…"),
                      int(frac * 100.0f + 0.5f), done, total);
        const float line_px = 22.0f * unit;
        const ImVec2 line_size = ImGui::GetFont()->CalcTextSizeA(line_px, FLT_MAX, 0.0f, line);
        draw->AddText(ImGui::GetFont(), line_px,
                      ImVec2(origin.x + (size.x - line_size.x) * 0.5f, bar0.y + bar_h + 14.0f * unit), dim, line);
    }
    ImGui::End();
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    // Latest frames in order, oldest first.
    float frames[kHistory];
    for (int i = 0; i < hist_count; ++i) {
        frames[i] = frame_hist[(hist_pos - hist_count + i + kHistory) % kHistory];
    }
    const float target = TargetMs();
    float sum = 0.0f, worst = 0.0f;
    int late = 0;
    for (int i = 0; i < hist_count; ++i) {
        sum += frames[i];
        worst = std::max(worst, frames[i]);
        late += frames[i] > target * 1.1f;
    }
    float sorted[kHistory];
    std::copy(frames, frames + hist_count, sorted);
    std::sort(sorted, sorted + hist_count);
    const float p99 = hist_count ? sorted[std::min(hist_count - 1, hist_count * 99 / 100)] : 0.0f;
    const float avg_fps = sum > 0.0f ? 1000.0f * hist_count / sum : 0.0f;
    const float seconds = sum / 1000.0f;

    const ImVec4 green(0.45f, 0.9f, 0.45f, 1.0f), yellow(1.0f, 0.82f, 0.3f, 1.0f),
        red(1.0f, 0.4f, 0.35f, 1.0f), grey(0.7f, 0.7f, 0.7f, 1.0f);
    const auto rate_color = [&](float ms) {
        return ms <= target * 1.1f ? green : ms <= target * 1.6f ? yellow : red;
    };
    const float now_ms = frame_ms_avg;
    ImGui::TextColored(rate_color(now_ms), "%.0f FPS", now_ms > 0.0f ? 1000.0f / now_ms : 0.0f);
    ImGui::SameLine();
    ImGui::TextColored(grey, SotcUi::T("%.1f ms   alvo %.0f FPS", "%.1f ms   target %.0f FPS"), now_ms, 1000.0f / target);

    // Frame time graph, 0 to 2.2x the target, with the target line.
    const float graph_max = target * 2.2f;
    const ImVec2 graph_size(260.0f * base_scale, 54.0f * base_scale);
    ImGui::PushStyleColor(ImGuiCol_PlotLines, late ? yellow : green);
    ImGui::PlotLines("##frametimes", frames, hist_count, 0, nullptr, 0.0f, graph_max, graph_size);
    ImGui::PopStyleColor();
    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    const float y = hi.y - (hi.y - lo.y) * (target / graph_max);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(lo.x, y), ImVec2(hi.x, y),
                                        IM_COL32(120, 200, 255, 160), 1.0f);

    ImGui::Text(SotcUi::T("%.0f s: média %.1f | 1%% low %.1f | pior %.0f ms", "%.0f s: avg %.1f | 1%% low %.1f | worst %.0f ms"), seconds, avg_fps,
                p99 > 0.0f ? 1000.0f / p99 : 0.0f, worst);
    ImGui::TextColored(late ? (late > 3 ? red : yellow) : green, SotcUi::T("Atrasados: %d", "Late: %d"), late);
    ImGui::SameLine();
    ImGui::TextColored(grey, SotcUi::T("| sessão %llu (%.1f%%), travadas >50 ms %llu", "| session %llu (%.1f%%), stutters >50 ms %llu"),
                       static_cast<unsigned long long>(session_late),
                       session_frames ? 100.0 * session_late / session_frames : 0.0,
                       static_cast<unsigned long long>(session_stutters));
    if (SotcStats::kFpsLog) {
        ImGui::TextColored(grey, "CPU %.0f cmds/image | relectures GPU %.0f/s (%.0f ms/s)",
                           SotcStats::ov_cmds_per_frame.load(), SotcStats::ov_readbacks_per_s.load(),
                           SotcStats::ov_readback_ms_per_s.load());
        const int cause = SotcStats::ov_worst_cause.load();
        ImGui::TextColored(grey, "Shaders compiles %u (5 s), %llu session",
                           SotcStats::ov_compiles_window.load(),
                           static_cast<unsigned long long>(SotcStats::ov_compiles_total.load()));
        if (cause >= 0) {
            ImGui::SameLine();
            ImGui::TextColored(grey, SotcUi::T("| pior quadro: %s %.0f ms", "| worst frame: %s %.0f ms"),
                               SotcStats::kFrameCauseNames[cause],
                               SotcStats::ov_worst_cause_ms.load());
        }
    }
    // Frame generation (vk_framegen.h): DLSS-G (vk_dlssg.cpp) or FSR 3 (vk_fsrfg.cpp).
    switch (Vulkan::FrameGen::Current()) {
    case Vulkan::FrameGen::Mode::Off:
        ImGui::TextColored(grey, SotcUi::T("Geração de quadros: desligada", "Frame generation: off"));
        break;
    case Vulkan::FrameGen::Mode::Dlss:
        if (const int fg = SotcStats::ov_fg_state.load(std::memory_order_relaxed); fg == 1) {
            const uint32_t shown = SotcStats::ov_fg_frames.load(std::memory_order_relaxed);
            const uint32_t n = shown ? shown : 2;
            ImGui::TextColored(green, SotcUi::T("DLSS-G x%u: ~%.0f FPS na tela", "DLSS-G x%u: ~%.0f FPS shown"), n, avg_fps * n);
        } else if (fg == 2) {
            ImGui::TextColored(red, "DLSS-G : erreur (statut %#x)",
                               SotcStats::ov_fg_status.load(std::memory_order_relaxed));
        } else {
            ImGui::TextColored(grey, SotcUi::T("DLSS-G: aguardando (sem cena 3D)", "DLSS-G: waiting (no 3D scene)"));
        }
        break;
    case Vulkan::FrameGen::Mode::Fsr:
        if (const int fg = SotcStats::ov_fsr_state.load(std::memory_order_relaxed); fg == 1) {
            ImGui::TextColored(green, SotcUi::T("FSR 3 x2: ~%.0f FPS na tela", "FSR 3 x2: ~%.0f FPS shown"), avg_fps * 2);
        } else if (fg == 2) {
            ImGui::TextColored(red, "FSR 3 : erreur (voir le journal)");
        } else {
            ImGui::TextColored(grey, SotcUi::T("FSR 3: aguardando (sem cena 3D)", "FSR 3: waiting (no 3D scene)"));
        }
        break;
    }
    if (const uint64_t vram = SotcStats::vram_used.load(std::memory_order_relaxed)) {
        ImGui::TextColored(grey, "VRAM %.1f / %.1f Go", vram / 1073741824.0,
                           SotcStats::vram_budget.load(std::memory_order_relaxed) / 1073741824.0);
        ImGui::SameLine();
    }
    ImGui::TextDisabled(SotcUi::T("F7: %s   F8: geração de quadros   F10: esconder", "F7: %s   F8: frame generation   F10: hide"),
                        SotcFpsCap::uncapped ? SotcUi::T("FPS livres", "FPS uncapped") : SotcUi::T("travado em 30", "capped at 30"));
    ImGui::End();
}

void Toast() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + 24.0f * base_scale),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.7f);
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextUnformatted(toast_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::End();
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    s_instance = &instance;
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    // sotc: F7 caps the game at 30 FPS or uncaps it (sotc_fps_cap.h).
    // The new state is shown at the next frame (Render).
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F7 && !event.key.repeat) {
        const bool uncapped = SotcFpsCap::Toggle();
        SotcEvent::Log("KEY", "%s", uncapped ? SotcUi::T("F7: limite de 30 FPS desligado", "F7: 30 FPS cap off")
                                             : SotcUi::T("F7: limite de 30 FPS ligado", "F7: 30 FPS cap on"));
        return true;
    }
    // sotc: F8 switches the frame generation mode (off, DLSS-G, FSR 3; vk_framegen.h).
    // The new mode is shown at the next frame (Render).
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F8 && !event.key.repeat) {
        const Vulkan::FrameGen::Mode mode = Vulkan::FrameGen::Cycle();
        const char* fg_event_names[] = {SotcUi::T("desligada", "off"), "NVIDIA DLSS-G", "AMD FSR 3"};
        SotcEvent::Log("KEY", SotcUi::T("F8: geração de quadros %s", "F8: frame generation %s"),
                       fg_event_names[static_cast<int>(mode)]);
        if (mode == Vulkan::FrameGen::Mode::Off &&
            !Vulkan::FrameGen::Available(Vulkan::FrameGen::Mode::Fsr) &&
            !Vulkan::FrameGen::Available(Vulkan::FrameGen::Mode::Dlss)) {
            toast_text = SotcUi::T("Geração de quadros: indisponível nesta placa", "Frame generation: not available on this GPU");
            toast_until_ms.store(NowMs() + 2500, std::memory_order_relaxed);
        }
        return true;
    }
    // sotc: F10 shows or hides the performance overlay.
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F10 && !event.key.repeat) {
        const bool on = !OverlayOn();
        BbSettings::Get().show_fps = false; // the ini setting only gives the initial state
        overlay_on.store(on, std::memory_order_relaxed);
        SotcEvent::Log("KEY", "%s", on ? SotcUi::T("F10: painel de desempenho mostrado", "F10: performance overlay shown")
                                       : SotcUi::T("F10: painel de desempenho escondido", "F10: performance overlay hidden"));
        return true;
    }
    if (list_active && !menu_open && list_result == -1) {
        const int count = static_cast<int>(list_items.size());
        const auto move = [&](int delta) {
            list_confirm = -1;
            if (count) {
                list_selected = (list_selected + delta + count) % count;
                list_scroll = true;
            }
        };
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
            switch (event.key.key) {
            case SDLK_UP:
            case SDLK_W:
                move(-1);
                break;
            case SDLK_DOWN:
            case SDLK_S:
                move(1);
                break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_SPACE:
                if (!event.key.repeat && list_confirm >= 0) {
                    list_result = -100 - list_confirm; // delete it (PollSaveList)
                    list_confirm = -1;
                } else if (!event.key.repeat && count) {
                    list_result = list_selected;
                }
                break;
            case SDLK_ESCAPE:
            case SDLK_BACKSPACE:
                if (!event.key.repeat) {
                    if (list_confirm >= 0) {
                        list_confirm = -1;
                    } else {
                        list_result = -2;
                    }
                }
                break;
            case SDLK_DELETE:
                if (!event.key.repeat && list_selected >= list_first_deletable &&
                    list_selected < count) {
                    list_confirm = list_selected;
                }
                break;
            default:
                break;
            }
            return true;
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
            return true;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            switch (event.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP:
                move(-1);
                break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                move(1);
                break;
            case SDL_GAMEPAD_BUTTON_SOUTH:
                if (list_confirm >= 0) {
                    list_result = -100 - list_confirm;
                    list_confirm = -1;
                } else if (count) {
                    list_result = list_selected;
                }
                break;
            case SDL_GAMEPAD_BUTTON_EAST:
                if (list_confirm >= 0) {
                    list_confirm = -1;
                } else {
                    list_result = -2;
                }
                break;
            case SDL_GAMEPAD_BUTTON_NORTH:
                if (list_selected >= list_first_deletable && list_selected < count) {
                    list_confirm = list_selected;
                }
                break;
            default:
                break;
            }
            return true;
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            return true;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
                const bool down = event.gaxis.value > 20000, up = event.gaxis.value < -20000;
                if (down && !list_stick_down) {
                    move(1);
                }
                if (up && !list_stick_up) {
                    move(-1);
                }
                list_stick_down = down;
                list_stick_up = up;
            }
            return true;
        case SDL_EVENT_MOUSE_MOTION: {
            const float density = PixelDensity(event.motion.windowID);
            io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
            return true;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_LEFT) {
                io.AddMouseButtonEvent(0, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            }
            return true;
        case SDL_EVENT_MOUSE_WHEEL:
            io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
            return true;
        default:
            break;
        }
    }
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        const bool toggle_key = (event.key.key == SDLK_INSERT || event.key.key == SDLK_F1);
        if (down && !event.key.repeat && MenuAllowed() &&
            (toggle_key || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(toggle_key ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down && MenuAllowed()) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

bool LoadingShown() {
    // sotc: a second without a game flip. Read from the presenter and the video out threads: the
    // races only move the moment by a frame.
    static std::atomic<u64> seen{~u64{0}};
    const u64 flips = SotcStats::flip_count.load(std::memory_order_relaxed);
    const long long now = NowMs();
    if (seen.exchange(flips, std::memory_order_relaxed) != flips) {
        last_flip_ms.store(now, std::memory_order_relaxed);
        return false;
    }
    return now - last_flip_ms.load(std::memory_order_relaxed) >= 1000;
}

// sotc: a SOTC_MENU_TRIGGER / SOTC_SET_TRIGGER file waits (scripted tests): Render handles it.
bool TriggerPending() {
    static const char* menu = std::getenv("SOTC_MENU_TRIGGER");
    static const char* set = std::getenv("SOTC_SET_TRIGGER");
    std::error_code ec;
    return (menu && std::filesystem::exists(menu, ec)) || (set && std::filesystem::exists(set, ec));
}

bool Visible() {
    return initialized && (menu_open || text_entry_active || list_active || OverlayOn() ||
                           ToastShown() || LoadingShown() || TriggerPending());
}

bool CapturesInput() {
    return menu_open || text_entry_active || list_active;
}

bool OpenSaveList(const std::string& title, const std::vector<std::string>& items, int focus,
                  int first_deletable) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false; // nothing could show it
    }
    list_title = title;
    list_items = items;
    list_icons = std::move(pending_icons); // sotc: the pictures sent just before (or none)
    pending_icons.clear();
    list_selected = items.empty() ? 0 : std::clamp(focus, 0, static_cast<int>(items.size()) - 1);
    list_result = -1;
    list_confirm = -1;
    list_first_deletable = first_deletable < 0 ? static_cast<int>(items.size()) : first_deletable;
    list_scroll = true;
    list_active = true;
    ImGui::GetIO().MouseDrawCursor = true;
    return true;
}

void SetSaveListIcons(std::vector<std::string> paths) {
    std::scoped_lock lock{imgui_mutex};
    pending_icons = std::move(paths);
}

int PollSaveList() {
    std::scoped_lock lock{imgui_mutex};
    if (!list_active || list_result == -1) {
        return -1;
    }
    list_active = false;
    if (initialized) {
        ImGui::GetIO().MouseDrawCursor = menu_open.load();
    }
    return list_result;
}

void CloseSaveList() {
    std::scoped_lock lock{imgui_mutex};
    list_active = false;
    if (initialized) {
        ImGui::GetIO().MouseDrawCursor = menu_open.load();
    }
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent,
            bool game_frame) {
    // Present interval for the FPS readout (measured also while nothing is drawn), game frames
    // only: a generated frame (FSR 3) is drawn half a frame later.
    const auto now = std::chrono::steady_clock::now();
    static auto last_draw = now;
    const float draw_ms = std::chrono::duration<float, std::milli>(now - last_draw).count();
    last_draw = now;
    if (game_frame) {
        const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
        last_present = now;
        if (ms > 0.0f && ms < 1000.0f) {
            frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
        }
        if (ms > 0.0f && ms < 5000.0f) {
            RecordFrame(ms);
        }
    }
    const float ms = draw_ms;
    // The frame generation mode changed (F8): shown for 2.5 s.
    static Vulkan::FrameGen::Mode shown_mode = Vulkan::FrameGen::Current();
    if (const Vulkan::FrameGen::Mode mode = Vulkan::FrameGen::Current(); mode != shown_mode) {
        shown_mode = mode;
        std::scoped_lock lock{imgui_mutex};
        toast_text = std::string(SotcUi::T("Geração de quadros: ", "Frame generation: ")) + Vulkan::FrameGen::Name(mode);
        toast_until_ms.store(NowMs() + 2500, std::memory_order_relaxed);
    }
    // The 30 FPS cap was switched (F7).
    static bool shown_uncapped = SotcFpsCap::uncapped;
    if (const bool uncapped = SotcFpsCap::uncapped; uncapped != shown_uncapped) {
        shown_uncapped = uncapped;
        std::scoped_lock lock{imgui_mutex};
        toast_text = uncapped ? SotcUi::T("FPS livres", "FPS uncapped") : SotcUi::T("FPS travados em 30", "FPS capped at 30");
        toast_until_ms.store(NowMs() + 2500, std::memory_order_relaxed);
    }
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    const bool loading = LoadingShown() && !list_active && !text_entry_active;
    static bool was_loading = false;
    static long long loading_since = 0;
    if (loading != was_loading) { // sotc: one log line when it appears and when it goes away
        was_loading = loading;
        if (loading) {
            loading_since = NowMs();
            std::fprintf(stderr, "SOTC loading screen: shown\n");
            SotcEvent::Log("GAME", "%s", SotcUi::T("Tela de carregamento (o jogo parou de mostrar quadros)",
                                                   "Loading screen (the game stopped showing frames)"));
        } else {
            std::fprintf(stderr, "SOTC loading screen: hidden after %.1f s\n",
                         double(NowMs() - loading_since) / 1000.0);
            SotcEvent::Log("GAME", SotcUi::T("Fim da tela de carregamento (%.1f s)", "Loading screen over (%.1f s)"),
                           double(NowMs() - loading_since) / 1000.0);
        }
    }
    if (loading) {
        LoadingScreen();
    }
    // sotc: SOTC_MENU_TRIGGER=<file>: when the file appears it is removed and the menu opens or
    // closes (scripted pictures of the menu: the "menu" event of tools/sotc_play.py).
    static const char* menu_trigger = std::getenv("SOTC_MENU_TRIGGER");
    if (menu_trigger && MenuAllowed() && std::remove(menu_trigger) == 0) {
        SetOpen(!menu_open);
    }
    // sotc: SOTC_SET_TRIGGER=<file>: "tex=off|high|max", "fps=30|60|unlimited",
    // "up=fsr1|bilinear" or "rcas=high|medium|low|off" in the file sets that quick setting as the
    // menu does, then the file is removed (scripted tests: tools/sotc_play.py).
    static const char* set_trigger = std::getenv("SOTC_SET_TRIGGER");
    if (set_trigger) {
        if (std::FILE* f = std::fopen(set_trigger, "rb")) {
            char line[32] = {};
            std::fread(line, 1, sizeof(line) - 1, f);
            std::fclose(f);
            std::remove(set_trigger);
            const std::string text{line};
            const std::string key = text.substr(0, text.find('='));
            const char v = text.size() > key.size() + 1 ? text[key.size() + 1] : 0;
            if (key == "tex") {
                SetTextureLevel(v == 'o' ? 0 : v == 'm' ? 2 : 1);
            } else if (key == "fps") {
                SetFpsMode(v == '3' ? 0 : v == '6' ? 1 : 2);
            } else if (key == "up") {
                SetUpscaler(v == 'f' ? 0 : 1);
            } else if (key == "rcas") {
                SetRcas(v == 'h' ? 0 : v == 'm' ? 1 : v == 'l' ? 2 : 3);
            }
            std::fprintf(stderr, "SOTC SET: %s\n", line);
        }
    }
    if (menu_open) {
        Menu();
    }
    if (OverlayOn() && !menu_open) {
        FpsCounter();
    }
    if (ToastShown()) {
        Toast();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (list_active && !menu_open) {
        SaveListBox();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay
