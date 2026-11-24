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
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <regex>
#include <string>
#include <string_view>
#include <vector>


#include <SDL3/SDL.h>
#include "bbport_settings.h"
#include "sdl_window.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "stb_image.h"
#include <unordered_map>
#include "imgui_impl_vulkan.h"
#include "video_core/host_shaders/overlay_hdr_frag.h"
#include "video_core/renderer_vulkan/vk_framegen.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/sotc_fps_cap.h"
#include "video_core/sotc_event.h"
#include "video_core/sotc_live.h"
#include "video_core/sotc_tonemap.h"
#include "video_core/sotc_ui_lang.h"
#include "video_core/sotc_stats.h"

extern Frontend::WindowSDL* g_window;
static const Vulkan::Instance* s_instance = nullptr;
// sotc: the volume of the runtime's audio output (src/runtime_audio.c); weak, so builds without the
// runtime (tests) still link.
extern "C" __attribute__((weak)) void runtime_audio_set_volume_percent(int pct);
extern "C" __attribute__((weak)) int runtime_audio_volume_percent(void);
extern "C" __attribute__((weak)) int runtime_rumble_enabled(void);
extern "C" __attribute__((weak)) void runtime_rumble_set(int on);

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

// sotc: the launcher's settings file (SOTC_LAUNCHER_SETTINGS, user/launcher_settings.json), so what
// is changed in the menu is also the launcher's choice next time. Values are handled as raw JSON
// ("\"60\"", true); a key the file does not have yet is added at its end.
std::filesystem::path LauncherSettingsPath() {
#ifdef _WIN32
    const wchar_t* wpath = _wgetenv(L"SOTC_LAUNCHER_SETTINGS");
    return wpath ? std::filesystem::path{wpath} : std::filesystem::path{};
#else
    const char* cpath = std::getenv("SOTC_LAUNCHER_SETTINGS");
    return cpath ? std::filesystem::path{cpath} : std::filesystem::path{};
#endif
}

std::string ReadLauncherSettings() {
    const auto path = LauncherSettingsPath();
    if (path.empty()) {
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::regex LauncherSettingRegex(const char* key) {
    return std::regex("(\"" + std::string(key) + "\"\\s*:\\s*)(\"[^\"]*\"|true|false|-?[0-9.]+)");
}

std::mutex settings_mutex;                                  // the cache, the snapshot and the file
std::unordered_map<std::string, std::string> setting_cache; // raw values, as last read or written
std::string start_settings;                                 // the file as the game started
bool start_read = false;

std::string RawFrom(const std::string& text, const char* key) {
    std::smatch m;
    return std::regex_search(text, m, LauncherSettingRegex(key)) ? m[2].str() : std::string{};
}

void SnapshotStartLocked() {
    if (!start_read) {
        start_settings = ReadLauncherSettings();
        start_read = true;
    }
}

// The raw JSON value of a key now, or "" when the file does not have it.
std::string SettingRaw(const char* key) {
    std::scoped_lock lock{settings_mutex};
    if (const auto it = setting_cache.find(key); it != setting_cache.end()) {
        return it->second;
    }
    return setting_cache[key] = RawFrom(ReadLauncherSettings(), key);
}

// The raw JSON value the game started with: a ◆ setting differs from it until the game restarts.
std::string StartRaw(const char* key) {
    std::scoped_lock lock{settings_mutex};
    SnapshotStartLocked();
    return RawFrom(start_settings, key);
}

std::string Unquote(const std::string& v) {
    return v.size() >= 2 && v.front() == '"' ? v.substr(1, v.size() - 2) : v;
}

// The value of a key (a string without its quotes), or "" when it is not in the file.
std::string LoadLauncherSetting(const char* key) {
    return Unquote(SettingRaw(key));
}

// Settings the launcher's play modes set: changing one by hand makes the mode Custom, as in the
// launcher (else the mode would put its own value back).
bool PlayModeKey(std::string_view key) {
    static constexpr std::string_view keys[] = {
        "neo",           "fps_mode",       "textures",     "af16",          "upscaler",
        "sharpness",     "vram_use",       "dlss_mode",    "grass_shadows", "grass_distance",
        "grass_height",  "weed_distance",  "weed_height",  "grass_fade",    "grass_detail"};
    return std::find(std::begin(keys), std::end(keys), key) != std::end(keys);
}

void WriteSetting(std::string& text, const std::string& key, const std::string& json_value) {
    std::smatch m;
    if (std::regex_search(text, m, LauncherSettingRegex(key.c_str()))) {
        text = m.prefix().str() + m[1].str() + json_value + m.suffix().str();
        return;
    }
    const size_t end = text.rfind('}');
    if (end == std::string::npos || end == 0) {
        return;
    }
    const size_t last = text.find_last_not_of(" \t\r\n", end - 1);
    if (last == std::string::npos) {
        return;
    }
    const bool empty = text[last] == '{';
    text.insert(last + 1, std::string(empty ? "" : ",") + "\n  \"" + key + "\": " + json_value);
}

void SaveLauncherSetting(const char* key, const std::string& json_value) {
    std::scoped_lock lock{settings_mutex};
    SnapshotStartLocked();
    setting_cache[key] = json_value;
    std::string text = ReadLauncherSettings();
    if (text.empty()) {
        return;
    }
    WriteSetting(text, key, json_value);
    if (PlayModeKey(key)) {
        const std::string mode = RawFrom(text, "play_mode");
        if (!mode.empty() && mode != "\"custom\"") {
            WriteSetting(text, "play_mode", "\"custom\"");
            setting_cache["play_mode"] = "\"custom\"";
        }
    }
    std::ofstream out(LauncherSettingsPath(), std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    // sotc (#467): a read-only settings file used to fail here silently; say so once.
    static bool warned = false;
    if (!out && !warned) {
        warned = true;
        SotcEvent::Log("MENU",
                       SotcUi::T("Não foi possível salvar a configuração em %s (pasta somente leitura?)",
                                 "Could not save the setting to %s (read-only folder?)"),
                       LauncherSettingsPath().string().c_str());
    }
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

// sotc: the FPS mode now: 0 = 30 (the game's cap), 1 = 60, 2 = unlimited.
int FpsMode() {
    static const int env_limit = [] {
        const char* v = std::getenv("SOTC_FPS_LIMIT");
        return v ? std::atoi(v) : 0;
    }();
    if (!SotcFpsCap::uncapped.load(std::memory_order_relaxed)) {
        return 0;
    }
    const int live_limit = SotcFpsCap::limit_fps.load(std::memory_order_relaxed);
    return (live_limit >= 0 ? live_limit : env_limit) > 0 ? 1 : 2;
}

const char* FpsModeName(int mode) {
    const char* names[] = {SotcUi::T("FPS travados em 30", "FPS capped at 30"),
                           SotcUi::T("FPS travados em 60", "FPS capped at 60"),
                           SotcUi::T("FPS ilimitados", "FPS unlimited")};
    return names[mode];
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

// ---- the settings menu (F1 / Insert / L3+R3) ------------------------------------------------------
// sotc: laid out like ZooT-PC's menu: tabs at the top (Q/E, L1/R1), rows with their value between
// arrows (up/down choose, left/right change), help for the chosen row on the right and the keys at
// the bottom, in the game's colours (weathered stone, cloudy sky, sand, aged gold). What can change
// while playing applies at once; what the game reads at start (marked ◆) is saved in the launcher's
// settings and a band asks for a restart. SOTC_MENU_CLASSIC is not needed: every setting of the old
// menu is here.

// The menu's language: the launcher's (SOTC_UI_LANG), switchable in the Game tab.
enum MenuLang { kLangPt, kLangEn, kLangEs, kLangZh, kLangKo, kLangRu, kLangCount };
constexpr const char* kLangCodes[kLangCount] = {"pt", "en", "es", "zh", "ko", "ru"};
std::atomic<int> menu_lang{-1};

int LangFromCode(std::string_view code) {
    for (int i = 0; i < kLangCount; ++i) {
        if (code.substr(0, 2) == kLangCodes[i]) {
            return i;
        }
    }
    return kLangEn;
}

int MenuLanguage() {
    int lang = menu_lang.load(std::memory_order_relaxed);
    if (lang < 0) {
        const char* v = std::getenv("SOTC_UI_LANG");
        lang = LangFromCode(v ? v : "en");
        menu_lang.store(lang, std::memory_order_relaxed);
    }
    return lang;
}

// Spanish, Chinese, Korean and Russian texts, looked up by the Portuguese one (as the launcher's
// sotc_i18n.py); generated by tools/gen_menu_text.py, a missing one falls back to English.
struct MenuText {
    const char* pt;
    const char* es;
    const char* zh;
    const char* ko;
    const char* ru;
};
#include "bbport_menu_text.inc"

const char* L(const char* pt, const char* en) {
    const int lang = MenuLanguage();
    if (lang == kLangPt) {
        return pt;
    }
    if (lang == kLangEn) {
        return en;
    }
    static const std::unordered_map<std::string_view, const MenuText*> table = [] {
        std::unordered_map<std::string_view, const MenuText*> map;
        for (const MenuText& t : kMenuText) {
            map.emplace(t.pt, &t);
        }
        return map;
    }();
    const auto it = table.find(pt);
    if (it == table.end()) {
        return en;
    }
    const char* text = lang == kLangEs   ? it->second->es
                       : lang == kLangZh ? it->second->zh
                       : lang == kLangKo ? it->second->ko
                                         : it->second->ru;
    return text && *text ? text : en;
}

// Chinese and Korean need glyphs DejaVu Sans does not have: Windows' own fonts are merged into the
// menu font when one of these languages is chosen (ImGui 1.92 loads glyphs as they are drawn).
bool cjk_font_loaded = false;
void EnsureCjkFont() {
    const int lang = MenuLanguage();
    if (cjk_font_loaded || (lang != kLangZh && lang != kLangKo)) {
        return;
    }
    cjk_font_loaded = true;
#ifdef _WIN32
    const char* windir = std::getenv("WINDIR");
    const std::string fonts = std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\";
    for (const char* file : {"msyh.ttc", "malgun.ttf", "simsun.ttc"}) {
        std::error_code ec;
        if (std::filesystem::exists(fonts + file, ec)) {
            ImFontConfig config;
            config.MergeMode = true;
            ImGui::GetIO().Fonts->AddFontFromFileTTF((fonts + file).c_str(), 18.0f, &config);
        }
    }
#endif
}

enum MenuTab { kTabDisplay, kTabPerf, kTabGrass, kTabSound, kTabControls, kTabGame, kTabDebug, kTabCount };

struct MenuChoice {
    std::string label;
    std::string value; // raw JSON for the launcher's settings, or an index for live ones
};

struct MenuItem {
    int tab = kTabDisplay;
    std::string label, help, hotkey;
    std::vector<MenuChoice> choices;
    std::function<int()> get;              // the current choice (-1: none of them)
    std::function<void(int)> set;          // pick a choice
    std::function<bool()> restart_pending; // a change waits for the game to restart
    std::function<bool(int)> choice_ok;    // a choice this PC cannot take (skipped, grey)
    std::function<bool()> shown;           // rows that only matter with another setting
    std::function<bool()> enabled;
    bool restart = false;                  // ◆: read when the game starts
    bool action = false;                   // a button (one choice that runs something)
    bool slider = false;                   // a range: a bar, no wrap-around, click or drag to set
};

std::vector<MenuItem> items;
int items_lang = -1;
std::atomic<int> pending_fullscreen{-1}; // the menu's choice, for the window thread (UpdateTextInput)
int slider_drag = -1;               // the slider row (index in items) the mouse is dragging
int menu_tab = kTabDisplay;
std::array<int, kTabCount> menu_row{};
std::array<int, kTabCount> menu_first{};
bool menu_pad = false;              // the last menu input came from a controller
bool stick_x[2]{}, stick_y[2]{};    // the left stick's held directions (-, +)
enum MenuInput { kInUp, kInDown, kInLeft, kInRight, kInActivate, kInTabPrev, kInTabNext };
std::vector<int> menu_inputs;       // from HandleEvent, applied by DrawMenu (render thread)
// A controller's left / right held down (d-pad or stick): on a slider row it repeats, as a held key
// does, after a pause. -1 left, +1 right, 0 none.
int pad_hold = 0;
std::chrono::steady_clock::time_point pad_hold_next{};

void PadHold(int direction) {
    pad_hold = direction;
    pad_hold_next = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
}

int IndexOf(const std::vector<MenuChoice>& choices, const std::string& value) {
    for (size_t i = 0; i < choices.size(); ++i) {
        if (choices[i].value == value) {
            return int(i);
        }
    }
    return -1;
}

std::vector<MenuChoice> OnOff() {
    return {{L("Desligado", "Off"), "false"}, {L("Ligado", "On"), "true"}};
}

std::vector<MenuChoice> Quoted(std::initializer_list<std::pair<const char*, std::string>> list) {
    std::vector<MenuChoice> out;
    for (const auto& [value, label] : list) {
        out.push_back({label, "\"" + std::string(value) + "\""});
    }
    return out;
}

// A setting the game reads at start: saved in the launcher's settings, applied at the restart.
MenuItem& AddRestart(int tab, const char* key, const char* label, const char* help,
                     std::vector<MenuChoice> choices, const char* fallback) {
    MenuItem item;
    item.tab = tab;
    item.label = label;
    item.help = help;
    item.restart = true;
    item.choices = std::move(choices);
    const std::string k = key, fb = fallback;
    std::vector<std::string> values;
    for (const auto& c : item.choices) {
        values.push_back(c.value);
    }
    item.get = [k, fb, values] {
        std::string v = SettingRaw(k.c_str());
        for (size_t i = 0; i < values.size(); ++i) {
            if (values[i] == (v.empty() ? fb : v)) {
                return int(i);
            }
        }
        return -1;
    };
    item.set = [k, values](int i) { SaveLauncherSetting(k.c_str(), values[size_t(i)]); };
    item.restart_pending = [k, fb] {
        std::string now = SettingRaw(k.c_str()), start = StartRaw(k.c_str());
        return (now.empty() ? fb : now) != (start.empty() ? fb : start);
    };
    items.push_back(std::move(item));
    return items.back();
}

// A setting that applies at once.
MenuItem& AddLive(int tab, const char* label, const char* help, std::vector<MenuChoice> choices,
                  std::function<int()> get, std::function<void(int)> set) {
    MenuItem item;
    item.tab = tab;
    item.label = label;
    item.help = help;
    item.choices = std::move(choices);
    item.get = std::move(get);
    item.set = std::move(set);
    items.push_back(std::move(item));
    return items.back();
}

std::vector<MenuChoice> Indexed(std::initializer_list<const char*> labels) {
    std::vector<MenuChoice> out;
    for (const char* l : labels) {
        out.push_back({l, std::to_string(out.size())});
    }
    return out;
}

bool UpscalerAtStartIsTemporal() {
    const std::string v = Unquote(StartRaw("upscaler"));
    if (!v.empty()) {
        return v == "dlss" || v == "fsr4";
    }
    const char* env = std::getenv("SOTC_UPSCALER");
    return env && (std::strcmp(env, "dlss") == 0 || std::strcmp(env, "fsr4") == 0);
}

bool UpscalerIsTemporal() {
    const std::string v = LoadLauncherSetting("upscaler");
    return v.empty() ? UpscalerAtStartIsTemporal() : v == "dlss" || v == "fsr4";
}

void BuildItems() {
    items.clear();
    items_lang = MenuLanguage();
    auto& s = BbSettings::Get();

    // ---- Display
    AddLive(kTabDisplay, L("Tela cheia", "Fullscreen"),
            L("Alterna entre janela e tela cheia.", "Switches between a window and fullscreen."),
            {{L("Janela", "Window"), "0"}, {L("Tela cheia", "Fullscreen"), "1"}},
            [] {
                const int pending = pending_fullscreen.load(std::memory_order_relaxed);
                if (pending >= 0) {
                    return pending;
                }
                return (g_window ? g_window->IsFullscreen() : BbSettings::Get().fullscreen.load()) ? 1 : 0;
            },
            [](int v) {
                BbSettings::Get().fullscreen = v != 0;
                dirty = true;
                // sotc: the window thread switches it (UpdateTextInput): SDL's switch waits for
                // that thread, which may be waiting for imgui_mutex held here (a hang, 16 Nov).
                pending_fullscreen.store(v, std::memory_order_relaxed);
                SaveLauncherSetting("fullscreen", v ? "true" : "false");
            })
        .hotkey = "F11";
    {
        std::vector<MenuChoice> outputs;
        for (const char* size : {"1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160",
                                 "1920x1200", "2560x1600", "2560x1080", "3440x1440", "3840x1600",
                                 "3840x1080", "5120x1440"}) {
            std::string label = size;
            label.replace(label.find('x'), 1, " × ");
            const std::string sz = size;
            const char* aspect = sz == "1920x1200" || sz == "2560x1600"   ? "16:10"
                                 : sz == "2560x1080" || sz == "3440x1440" ||
                                         sz == "3840x1600"                 ? "21:9"
                                 : sz == "3840x1080" || sz == "5120x1440" ? "32:9"
                                                                          : "16:9";
            outputs.push_back({label + "  (" + aspect + ")", "\"" + sz + "\""});
        }
        AddRestart(kTabDisplay, "output_res", L("Resolução de saída", "Output resolution"),
                   L("O tamanho da imagem final. Com DLSS ou FSR 4 o jogo desenha menor e amplia até ela.",
                     "The size of the final picture. With DLSS or FSR 4 the game renders smaller and scales up to it."),
                   std::move(outputs), "\"1920x1080\"");
    }
    {
        MenuItem& up = AddRestart(
            kTabDisplay, "upscaler", L("Upscaler", "Upscaler"),
            L("Como a imagem do jogo é ampliada para a tela. FSR 1 e Bilinear trocam na hora; DLSS e FSR 4 valem ao reiniciar o jogo.",
              "How the game's picture is scaled to the screen. FSR 1 and Bilinear switch at once; DLSS and FSR 4 apply when the game restarts."),
            Quoted({{"fsr1", L("FSR 1 (AMD, qualquer placa)", "FSR 1 (AMD, any GPU)")},
                    {"dlss", "NVIDIA DLSS"},
                    {"fsr4", "AMD FSR 4.1"},
                    {"bilinear", L("Bilinear (simples)", "Bilinear (plain)")}}),
            "\"fsr1\"");
        // FSR 1 <-> Bilinear apply at the next frame when the game did not start with DLSS / FSR 4.
        up.set = [](int i) {
            const char* keys[] = {"fsr1", "dlss", "fsr4", "bilinear"};
            if ((i == 0 || i == 3) && !UpscalerAtStartIsTemporal()) {
                SetUpscaler(i == 0 ? 0 : 1);
            } else {
                SaveLauncherSetting("upscaler", "\"" + std::string(keys[i]) + "\"");
            }
        };
        up.restart_pending = [] {
            const std::string now = LoadLauncherSetting("upscaler");
            return !now.empty() && (now == "dlss" || now == "fsr4" || UpscalerAtStartIsTemporal()) &&
                   now != Unquote(StartRaw("upscaler"));
        };
    }
    AddRestart(kTabDisplay, "dlss_mode", L("Modo do DLSS / FSR 4", "DLSS / FSR 4 mode"),
               L("Quanto menor a imagem desenhada, mais FPS e menos nitidez.",
                 "The smaller the rendered picture, the more FPS and the less sharpness."),
               Quoted({{"dlaa", L("DLAA (nativo)", "DLAA (native)")},
                       {"quality", L("Qualidade (1,5×)", "Quality (1.5×)")},
                       {"balanced", L("Equilibrado (1,72×)", "Balanced (1.72×)")},
                       {"performance", L("Desempenho (2×)", "Performance (2×)")},
                       {"ultra", L("Ultra Desempenho (3×)", "Ultra Performance (3×)")}}),
               "\"quality\"")
        .shown = UpscalerIsTemporal;
    {
        static const int env_rcas = [] {
            const char* on = std::getenv("BB_RCAS");
            if (on && on[0] == '0') {
                return -2;
            }
            const char* v = std::getenv("BB_RCAS_ATTENUATION");
            return v ? std::atoi(v) : 250;
        }();
        AddLive(kTabDisplay, L("Nitidez (RCAS)", "Sharpening (RCAS)"),
                L("O realce de nitidez do FSR 1. Vale na hora.", "FSR 1's sharpening. Applies at once."),
                Indexed({L("Alta", "High"), L("Média (padrão)", "Medium (default)"), L("Baixa", "Low"),
                         L("Desligada", "Off")}),
                [] {
                    const int live = SotcLive::rcas.load(std::memory_order_relaxed);
                    const int now = live != -1 ? live : env_rcas;
                    for (int i = 0; i < 4; ++i) {
                        if (RcasValues[i] == now) {
                            return i;
                        }
                    }
                    return 1;
                },
                [](int i) { SetRcas(i); });
    }
    {
        // Live (presenter reads SotcLive::dlss_sharpen each frame): RCAS attenuation in stops.
        static const char* kSharpenKeys[] = {"off", "2", "1.5", "1", "0.5", "0"};
        static const float kSharpenValues[] = {SotcLive::kSharpenOff, 2.0f, 1.5f, 1.0f, 0.5f, 0.0f};
        MenuItem& sharpen = AddLive(
            kTabDisplay, L("Nitidez do DLSS / FSR 4", "DLSS / FSR 4 sharpening"),
            L("O realce de nitidez depois do DLSS ou do FSR 4, ao vivo. Máxima é o padrão.",
              "The sharpening after DLSS or FSR 4, live. Highest is the default."),
            {{L("Desligada", "Off"), "0"}, {L("Mínima", "Lowest"), "1"}, {L("Baixa", "Low"), "2"},
             {L("Média", "Medium"), "3"}, {L("Alta", "High"), "4"},
             {L("Máxima (padrão)", "Highest (default)"), "5"}},
            [] {
                float now = SotcLive::dlss_sharpen.load(std::memory_order_relaxed);
                if (now <= SotcLive::kSharpenUnset) {
                    std::string v = LoadLauncherSetting("dlss_sharpen");
                    if (v.empty()) {
                        const char* e = std::getenv("SOTC_DLSS_SHARPEN");
                        v = e ? e : "0";
                    }
                    now = v.empty() || v[0] == 'o' ? SotcLive::kSharpenOff
                                                   : static_cast<float>(std::atof(v.c_str()));
                }
                int best = 0;
                for (int i = 1; i < 6; ++i) {
                    if (std::fabs(kSharpenValues[i] - now) <
                        std::fabs(kSharpenValues[best] - now)) {
                        best = i;
                    }
                }
                return best;
            },
            [](int i) {
                SotcLive::dlss_sharpen.store(kSharpenValues[i], std::memory_order_relaxed);
                SaveLauncherSetting("dlss_sharpen", std::string("\"") + kSharpenKeys[i] + "\"");
            });
        sharpen.slider = true;
        sharpen.shown = UpscalerIsTemporal;
    }
    AddRestart(kTabDisplay, "neo", L("Modo PS4 Pro", "PS4 Pro mode"),
               L("Os gráficos do PS4 Pro: mais detalhe e sombras melhores. Em telas mais largas que 16:9 ele fica desligado.",
                 "The PS4 Pro graphics: more detail and better shadows. On screens wider than 16:9 it stays off."),
               OnOff(), "false");
    AddRestart(kTabDisplay, "hdr", L("HDR do jogo", "Game HDR"),
               L("A renderização HDR do próprio jogo (pede um monitor HDR ligado no Windows).",
                 "The game's own HDR rendering (needs an HDR monitor turned on in Windows)."),
               OnOff(), "false");
    {
        // sotc: game HDR levels (vk_presenter.cpp RefreshHdrLevels), live and kept in the
        // launcher's settings. Shown when the game started with its HDR on.
        const auto hdr_on = [] {
            const char* v = std::getenv("BB_HDR");
            const char* sdr_to_hdr = std::getenv("SOTC_HDR"); // the SDR expansion, not the game's
            return v && v[0] == '1' && !(sdr_to_hdr && sdr_to_hdr[0] == '1');
        };
        // Picture controls that work in SDR too: shown unless the SDR-to-HDR expansion runs.
        const auto picture_on = [] {
            const char* sdr_to_hdr = std::getenv("SOTC_HDR");
            return !(sdr_to_hdr && sdr_to_hdr[0] == '1');
        };
        // A level as the launcher keeps it: whole numbers as before ("200"), else one decimal.
        const auto LevelText = [](float v) {
            char buf[32];
            if (v == std::floor(v)) {
                std::snprintf(buf, sizeof(buf), "%d", int(v));
            } else {
                std::snprintf(buf, sizeof(buf), "%.1f", v);
            }
            return std::string(buf);
        };
        // Choices in nits; `extra` is a first choice with its own label (auto / same / off).
        // The live value below `unset_below` means "the one read at start".
        const auto add_level = [&](const char* label, const char* help, const char* key,
                                   std::atomic<float>& live, const char* env, float fallback,
                                   std::vector<std::pair<float, std::string>> choices,
                                   float unset_below = 0.0f) -> MenuItem& {
            std::vector<MenuChoice> labels;
            std::vector<float> values;
            for (auto& [v, name] : choices) {
                labels.push_back({name, std::to_string(labels.size())});
                values.push_back(v);
            }
            const std::string k = key, e = env;
            MenuItem& level = AddLive(kTabDisplay, label, help, std::move(labels),
                    [&live, values, k, e, fallback, unset_below] {
                        float now = live.load(std::memory_order_relaxed);
                        if (now < unset_below) {
                            std::string v = LoadLauncherSetting(k.c_str());
                            if (v.empty()) {
                                const char* ev = std::getenv(e.c_str());
                                v = ev ? ev : "";
                            }
                            now = v.empty() ? fallback
                                  : v == "off" ? 10000.0f
                                  : v == "auto" ? 0.0f
                                                : static_cast<float>(std::atof(v.c_str()));
                        }
                        // The nearest choice (a hand-written value between steps).
                        int best = 0;
                        for (size_t i = 1; i < values.size(); ++i) {
                            if (std::fabs(values[i] - now) < std::fabs(values[size_t(best)] - now)) {
                                best = int(i);
                            }
                        }
                        return best;
                    },
                    [&live, values, k, LevelText](int i) { // PickChoice logs the change
                        const float v = values[size_t(i)];
                        live.store(v, std::memory_order_relaxed);
                        SaveLauncherSetting(k.c_str(), LevelText(v));
                    });
            level.shown = hdr_on;
            return level;
        };
        const char* nits = L("nits", "nits");
        std::vector<std::pair<float, std::string>> whites;
        for (int v = 80; v <= 400; v += 10) {
            whites.push_back({float(v), std::to_string(v) + " " + nits});
        }
        add_level(L("HDR: brilho do jogo", "HDR: game brightness"),
                  L("O brilho do mundo: o branco em nits. 200 é o padrão; o jogo puro tem ~100.",
                    "The world's brightness: its white in nits. 200 is the default; the game itself has ~100."),
                  "hdr_white", SotcLive::hdr_white, "SOTC_HDR_WHITE", 200.0f, whites)
            .slider = true;
        std::vector<std::pair<float, std::string>> peaks{
            {0.0f, L("Automático (do Windows)", "Auto (from Windows)")}};
        for (int v = 400; v <= 2000; v += 50) {
            peaks.push_back({float(v), std::to_string(v) + " " + nits});
        }
        peaks.push_back({10000.0f, L("Desligado (cortar)", "Off (clip)")});
        add_level(L("HDR: pico da tela", "HDR: display peak"),
                  L("O brilho máximo da tela. Os realces (sol, céu, água) descem suavemente até ele em vez de estourar.",
                    "Your display's peak brightness. Highlights (sun, sky, water) roll off smoothly into it instead of clipping."),
                  "hdr_peak", SotcLive::hdr_peak, "SOTC_HDR_PEAK", 0.0f, peaks)
            .slider = true;
        std::vector<std::pair<float, std::string>> uis{
            {0.0f, L("O do jogo (3x o brilho do jogo)", "The game's (3x game brightness)")}};
        for (int v = 50; v <= 1000; v += 10) {
            uis.push_back({float(v), std::to_string(v) + " " + nits});
        }
        // The old hdr_ui / SOTC_HDR_UI scaled the game's UI (white at 3x it): 3x as nits.
        const float legacy_ui = [] {
            std::string v = LoadLauncherSetting("hdr_ui");
            if (v.empty()) {
                const char* e = std::getenv("SOTC_HDR_UI");
                v = e ? e : "";
            }
            return v.empty() ? 0.0f : 3.0f * std::max(0.0f, static_cast<float>(std::atof(v.c_str())));
        }();
        add_level(L("HDR: brilho da interface", "HDR: UI brightness"),
                  L("O branco de menus, legendas e HUD em nits, separado do mundo. Arraste a barra ou use ◀ ▶.",
                    "The white of menus, subtitles and HUD in nits, apart from the world. Drag the bar or use ◀ ▶."),
                  "hdr_ui_nits", SotcLive::hdr_ui, "SOTC_HDR_UI_NITS", legacy_ui, uis)
            .slider = true;

        // Picture controls (sotc_live.h hdr_grade): neutral by default, on the world only.
        constexpr float kUnsetBelow = SotcLive::kGradeUnset + 1.0f;
        std::vector<std::pair<float, std::string>> evs;
        for (int t = -30; t <= 30; ++t) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%+.1f EV", t / 10.0f);
            evs.push_back({t / 10.0f, t == 0 ? std::string("0 EV") : std::string(buf)});
        }
        const auto percents = [] {
            std::vector<std::pair<float, std::string>> v;
            for (int p = 0; p <= 100; p += 2) {
                v.push_back({float(p), std::to_string(p)});
            }
            return v;
        };
        // Presets (sotc_live.h kPicturePresets): the tone mapper and every picture control at
        // once. The choice shown is the preset the current values match, else Custom.
        {
            static const char* kPresetTonemapKeys[] = {"\"vanilla\"", "\"vanilla_plus\"",
                                                       "\"none\"", "\"vanilla_plus_hue\"",
                                                       "\"renodrt\""};
            constexpr int kPresets = int(std::size(SotcLive::kPicturePresets));
            MenuItem& preset = AddLive(
                kTabDisplay, L("Imagem: predefinição", "Picture: preset"),
                L("Ajusta a curva de tons e todos os controles de imagem de uma vez. Original: o jogo. RenoDX: como os padrões dos mods RenoDX (RenoDRT, gama 2.2). Vívido: cores mais ricas, rumo ao P3. Filme: mais contraste, pretos um pouco mais fundos e granulação. O brilho, o pico e a interface ficam. Mexer num controle torna Personalizado.",
                  "Sets the tone mapper and every picture control at once. Original: the game. RenoDX: like the RenoDX mods' defaults (RenoDRT, gamma 2.2). Vivid: richer colour, towards P3. Film: more contrast, slightly deeper blacks and grain. Brightness, peak and UI stay. Changing a control makes it Custom."),
                {{L("Original", "Original"), "0"}, {"RenoDX", "1"}, {L("Vívido", "Vivid"), "2"},
                 {L("Filme", "Film"), "3"}, {L("Personalizado", "Custom"), "4"}},
                [kUnsetBelow] {
                    float now[SotcLive::kGradeCount];
                    for (int i = 0; i < SotcLive::kGradeCount; ++i) {
                        now[i] = SotcLive::hdr_grade[i].load(std::memory_order_relaxed);
                        if (now[i] < kUnsetBelow) {
                            const std::string v = LoadLauncherSetting(SotcLive::kGradeKeys[i]);
                            now[i] = v.empty() ? SotcLive::kPicturePresets[0].grade[i]
                                               : static_cast<float>(std::atof(v.c_str()));
                        }
                    }
                    int tm = SotcLive::hdr_tonemap.load(std::memory_order_relaxed);
                    if (tm < 0) {
                        const std::string v = LoadLauncherSetting("hdr_tonemap");
                        tm = v == "vanilla_plus" || v == "vanilla+" ? 1
                             : v == "none"                          ? 2
                             : v == "vanilla_plus_hue"              ? 3
                             : v == "renodrt"                       ? 4
                                                                    : 0;
                    }
                    for (int p = 0; p < kPresets; ++p) {
                        const SotcLive::PicturePreset& pr = SotcLive::kPicturePresets[p];
                        bool same = pr.tonemap == tm;
                        for (int i = 0; same && i < SotcLive::kGradeCount; ++i) {
                            same = std::fabs(now[i] - pr.grade[i]) < 0.5f;
                        }
                        if (same) {
                            return p;
                        }
                    }
                    return kPresets; // Custom
                },
                [kPresets](int p) {
                    if (p < 0 || p >= kPresets) {
                        return; // Custom: the values stay as they are
                    }
                    const SotcLive::PicturePreset& pr = SotcLive::kPicturePresets[p];
                    for (int i = 0; i < SotcLive::kGradeCount; ++i) {
                        SotcLive::hdr_grade[i].store(pr.grade[i], std::memory_order_relaxed);
                        SaveLauncherSetting(SotcLive::kGradeKeys[i],
                                            std::to_string(int(pr.grade[i])));
                    }
                    SotcLive::hdr_tonemap.store(pr.tonemap, std::memory_order_relaxed);
                    SaveLauncherSetting("hdr_tonemap", kPresetTonemapKeys[pr.tonemap]);
                    SotcLive::hdr_grading.store(100.0f, std::memory_order_relaxed);
                    SaveLauncherSetting("hdr_grading", "100");
                });
            preset.shown = picture_on;
        }
        {
            MenuItem& item = add_level(L("Imagem: exposição", "Picture: exposure"),
                  L("Clareia ou escurece o mundo inteiro, em pontos de exposição. 0 é o original.",
                    "Brightens or darkens the whole world, in stops. 0 is the original."),
                  "hdr_exposure", SotcLive::hdr_grade[SotcLive::kExposure], "SOTC_HDR_EXPOSURE",
                  0.0f, evs, kUnsetBelow);
            item.slider = true;
            item.shown = picture_on;
        }
        {
            MenuItem& item = add_level(L("Imagem: realces", "Picture: highlights"),
                  L("O brilho das partes claras (céu, sol, água). 50 é o original.",
                    "The brightness of the bright parts (sky, sun, water). 50 is the original."),
                  "hdr_highlights", SotcLive::hdr_grade[SotcLive::kHighlights],
                  "SOTC_HDR_HIGHLIGHTS", 50.0f, percents(), kUnsetBelow);
            item.slider = true;
            item.shown = picture_on;
        }
        {
            MenuItem& item = add_level(L("Imagem: sombras", "Picture: shadows"),
                  L("Clareia (acima de 50) ou aprofunda (abaixo) as partes escuras. 50 é o original.",
                    "Lifts (above 50) or deepens (below) the dark parts. 50 is the original."),
                  "hdr_shadows", SotcLive::hdr_grade[SotcLive::kShadows], "SOTC_HDR_SHADOWS",
                  50.0f, percents(), kUnsetBelow);
            item.slider = true;
            item.shown = picture_on;
        }
        {
            MenuItem& item = add_level(L("Imagem: contraste", "Picture: contrast"),
                  L("A diferença entre claro e escuro, em volta do cinza médio. 50 é o original.",
                    "The difference between light and dark, around mid grey. 50 is the original."),
                  "hdr_contrast", SotcLive::hdr_grade[SotcLive::kContrast], "SOTC_HDR_CONTRAST",
                  50.0f, percents(), kUnsetBelow);
            item.slider = true;
            item.shown = picture_on;
        }
        {
            MenuItem& item = add_level(L("Imagem: saturação", "Picture: saturation"),
                  L("A força das cores. 50 é o original; 0 é preto e branco.",
                    "Colour strength. 50 is the original; 0 is black and white."),
                  "hdr_saturation", SotcLive::hdr_grade[SotcLive::kSaturation],
                  "SOTC_HDR_SATURATION", 50.0f, percents(), kUnsetBelow);
            item.slider = true;
            item.shown = picture_on;
        }
        {
            MenuItem& item = add_level(L("Imagem: desbotar realces", "Picture: highlight desaturation"),
                  L("Os realces mais fortes perdem a cor e vão para o branco perto do pico da tela. 0 é o original.",
                    "The strongest highlights lose their colour and go to white near the display's peak. 0 is the original."),
                  "hdr_blowout", SotcLive::hdr_grade[SotcLive::kBlowout], "SOTC_HDR_BLOWOUT",
                  0.0f, percents(), kUnsetBelow);
            item.slider = true;
            item.shown = picture_on;
        }
        {
            MenuItem& item = add_level(L("Imagem: saturação dos realces", "Picture: highlight saturation"),
                  L("A força das cores só nas partes claras (céu, sol, fogo), mais quanto mais claras. 50 é o original.",
                    "Colour strength in the bright parts only (sky, sun, fire), more the brighter. 50 is the original."),
                  "hdr_highlight_saturation", SotcLive::hdr_grade[SotcLive::kHighlightSaturation],
                  "SOTC_HDR_HIGHLIGHT_SATURATION", 50.0f, percents(), kUnsetBelow);
            item.slider = true;
            item.shown = picture_on;
        }
        {
            MenuItem& item = add_level(L("Imagem: compensar véu (flare)", "Picture: flare compensation"),
                  L("Tira o véu acinzentado das partes quase pretas, deixando o preto mais fundo. 0 é o original.",
                    "Takes the grey veil out of the near-black parts for deeper blacks. 0 is the original."),
                  "hdr_flare", SotcLive::hdr_grade[SotcLive::kFlare], "SOTC_HDR_FLARE", 0.0f,
                  percents(), kUnsetBelow);
            item.slider = true;
            item.shown = picture_on;
        }
        add_level(L("HDR: correção de gama", "HDR: gamma correction"),
                  L("Mostra a parte escura como uma tela SDR mostraria em vez da curva sRGB: 2.2 como um monitor comum (sombras um pouco mais fundas, como o jogo em SDR), 2.4 como uma TV calibrada (BT.1886, ainda mais fundas). Desligado é o original.",
                    "Shows the dark range as an SDR display would instead of the sRGB curve: 2.2 like a typical monitor (slightly deeper shadows, as the game looks in SDR), 2.4 like a calibrated TV (BT.1886, deeper still). Off is the original."),
                  "hdr_gamma_correction", SotcLive::hdr_grade[SotcLive::kGammaCorrection],
                  "SOTC_HDR_GAMMA_CORRECTION", 0.0f,
                  {{0.0f, L("Desligado", "Off")}, {100.0f, "2.2"}, {200.0f, "2.4 (BT.1886)"}},
                  kUnsetBelow);
        add_level(L("HDR: expansão de gama de cores", "HDR: gamut expansion"),
                  L("Leva as cores já saturadas (céu, grama, fogo) além do sRGB, rumo ao P3 que as telas HDR mostram; cinzas e pele quase não mudam. 0 é o original.",
                    "Takes already saturated colours (sky, grass, fire) beyond sRGB, towards the P3 that HDR displays show; greys and skin barely move. 0 is the original."),
                  "hdr_gamut_expansion", SotcLive::hdr_grade[SotcLive::kGamutExpansion],
                  "SOTC_HDR_GAMUT_EXPANSION", 0.0f, percents(), kUnsetBelow)
            .slider = true;
        {
            MenuItem& grain = add_level(
                L("Imagem: granulação de filme", "Picture: film grain"),
                L("Uma granulação fina e em movimento, como em filme, mais nos tons médios. Também esconde faixas no céu e na névoa. 0 é o original (o jogo não tem).",
                  "A fine, moving grain like film, strongest in the mid tones. It also hides banding in the sky and fog. 0 is the original (the game has none)."),
                "hdr_grain", SotcLive::hdr_grade[SotcLive::kGrain], "SOTC_HDR_GRAIN", 0.0f,
                percents(), kUnsetBelow);
            grain.slider = true;
            grain.shown = picture_on;
        }
        {
            std::vector<std::pair<float, std::string>> sizes;
            for (int p = 50; p <= 200; p += 10) {
                sizes.push_back({float(p), std::to_string(p) + "%"});
            }
            MenuItem& size = add_level(
                L("Imagem: tamanho da granulação", "Picture: grain size"),
                L("O tamanho dos grãos. 100% é um pixel em 1080p e cresce com a resolução (dois pixels em 4K), então a granulação parece a mesma em qualquer tela.",
                  "The size of the grain. 100% is one pixel at 1080p and grows with the resolution (two pixels at 4K), so the grain looks the same on any screen."),
                "hdr_grain_size", SotcLive::hdr_grade[SotcLive::kGrainSize], "SOTC_HDR_GRAIN_SIZE",
                100.0f, sizes, kUnsetBelow);
            size.slider = true;
            size.shown = picture_on;
        }
        MenuItem& reset = AddLive(
            kTabDisplay, L("Imagem: restaurar", "Picture: reset"),
            L("Volta todos os controles de imagem (granulação e expansão de gama inclusive), a curva de tons, a correção de cor do jogo e a força do bloom, da vinheta, da névoa e do borrão de movimento ao original. O brilho e o pico ficam.",
              "Puts every picture control (grain and gamut expansion included), the tone mapper, the game's colour grading and the bloom, vignette, fog and motion blur strengths back to the original. Brightness and peak stay."),
            {{L("Executar", "Run"), "0"}}, [] { return -1; },
            [](int) {
                const SotcLive::PicturePreset& neutral = SotcLive::kPicturePresets[0];
                for (int i = 0; i < SotcLive::kGradeCount; ++i) {
                    SotcLive::hdr_grade[i].store(neutral.grade[i], std::memory_order_relaxed);
                    SaveLauncherSetting(SotcLive::kGradeKeys[i],
                                        std::to_string(int(neutral.grade[i])));
                }
                SotcLive::hdr_grading.store(100.0f, std::memory_order_relaxed);
                SaveLauncherSetting("hdr_grading", "100");
                SotcLive::hdr_tonemap.store(SotcTonemap::kVanilla, std::memory_order_relaxed);
                SaveLauncherSetting("hdr_tonemap", "\"vanilla\"");
                for (auto* fx : {&SotcLive::fx_bloom, &SotcLive::fx_vignette, &SotcLive::fx_fog,
                                 &SotcLive::fx_motion_blur}) {
                    fx->store(100.0f, std::memory_order_relaxed);
                }
                SaveLauncherSetting("fx_bloom_strength", "100");
                SaveLauncherSetting("fx_vignette_strength", "100");
                SaveLauncherSetting("fx_fog_strength", "100");
                SaveLauncherSetting("fx_motion_blur_strength", "100");
                toast_text = L("Imagem restaurada", "Picture reset");
                toast_until_ms.store(NowMs() + 2500, std::memory_order_relaxed);
            });
        reset.action = true;
        reset.shown = picture_on;

        // Tone mapper (sotc_tonemap.h): the game's curve, extended, or none, as the curve of the
        // game's own tonemap pass (any upscaler).
        static const int kTonemapModes[] = {SotcTonemap::kVanilla, SotcTonemap::kVanillaPlus,
                                            SotcTonemap::kVanillaPlusHue, SotcTonemap::kRenoDrt,
                                            SotcTonemap::kNone};
        static const char* kTonemapKeys[] = {"\"vanilla\"", "\"vanilla_plus\"",
                                             "\"vanilla_plus_hue\"", "\"renodrt\"",
                                             "\"none\""};
        MenuItem& tonemap = AddLive(
            kTabDisplay, L("HDR: curva de tons", "HDR: tone mapper"),
            L("Original: a curva do jogo. Original+: a mesma curva, mas os realces continuam subindo até o pico da tela em vez de achatar. Original+ (cor): a mesma, no brilho e não em cada cor, então as cores claras não puxam para o amarelo e o branco. RenoDRT: sem a curva do jogo, o brilho desce suavemente até o pico da tela mantendo cor e saturação (como os mods RenoDX). Nenhuma: sem curva, só a descida suave no pico.",
              "Vanilla: the game's curve. Vanilla+: the same curve, but highlights keep rising to your display's peak instead of flattening. Vanilla+ (hue): the same on brightness rather than per colour channel, so bright colours do not drift to yellow and white. RenoDRT: without the game's curve, brightness rolls off smoothly into your display's peak keeping hue and saturation (as RenoDX mods do). None: no curve, only the smooth roll-off at the peak."),
            {{L("Original", "Vanilla"), "0"}, {L("Original+", "Vanilla+"), "1"},
             {L("Original+ (cor)", "Vanilla+ (hue)"), "2"}, {"RenoDRT", "3"},
             {L("Nenhuma", "None"), "4"}},
            [] {
                int mode = SotcLive::hdr_tonemap.load(std::memory_order_relaxed);
                if (mode < 0) {
                    std::string v = LoadLauncherSetting("hdr_tonemap");
                    if (v.empty()) {
                        const char* e = std::getenv("SOTC_HDR_TONEMAP");
                        v = e ? e : "";
                    }
                    mode = v == "vanilla_plus" || v == "vanilla+" ? SotcTonemap::kVanillaPlus
                           : v == "vanilla_plus_hue"              ? SotcTonemap::kVanillaPlusHue
                           : v == "renodrt"                       ? SotcTonemap::kRenoDrt
                           : v == "none"                          ? SotcTonemap::kNone
                                                                  : SotcTonemap::kVanilla;
                }
                for (int i = 0; i < 5; ++i) {
                    if (kTonemapModes[i] == mode) {
                        return i;
                    }
                }
                return 0;
            },
            [](int i) {
                SotcLive::hdr_tonemap.store(kTonemapModes[i], std::memory_order_relaxed);
                SaveLauncherSetting("hdr_tonemap", kTonemapKeys[i]);
            });
        tonemap.shown = hdr_on;
        {
            MenuItem& item = add_level(L("HDR: correção de matiz", "HDR: hue correction"),
                  L("Só com Original+ (cor) e RenoDRT: quanto a cor dos realces volta à do jogo (o fogo amarelando, por exemplo). 100 é a do jogo; 0 mantém a cor de cada coisa ao clarear.",
                    "Vanilla+ (hue) and RenoDRT only: how far the hue of highlights goes back to the game's (fire turning yellow, for one). 100 is the game's; 0 keeps each colour's hue as it brightens."),
                  "hdr_hue_correction", SotcLive::hdr_grade[SotcLive::kHueCorrection],
                  "SOTC_HDR_HUE_CORRECTION", 100.0f, percents(), kUnsetBelow);
            item.slider = true;
            item.shown = picture_on;
        }
        std::vector<std::pair<float, std::string>> gradings;
        for (int p = 0; p <= 100; p += 5) {
            gradings.push_back({float(p), std::to_string(p) + "%"});
        }
        add_level(L("Imagem: correção de cor do jogo", "Picture: game colour grading"),
                  L("A força das cores que o jogo dá a cada lugar (tom, saturação, contraste). 100% é o original; 0% tira o tom de cor, mantendo o brilho.",
                    "How strongly the game's per-place colour look applies (tint, saturation, contrast). 100% is the original; 0% removes the colour cast and keeps the brightness."),
                  "hdr_grading", SotcLive::hdr_grading, "SOTC_HDR_GRADING", 100.0f, gradings)
            .slider = true;

        // Effect strengths (sotc_tonemap.h), live, SDR and HDR: 0..200% of the game's.
        std::vector<std::pair<float, std::string>> strengths;
        for (int p = 0; p <= 200; p += 10) {
            strengths.push_back({float(p), std::to_string(p) + "%"});
        }
        MenuItem& bloom = add_level(
            L("Brilho difuso (bloom)", "Bloom"),
            L("A força do brilho que se espalha em volta das luzes e do céu. 100% é o original; 0% tira.",
              "How strong the glow around lights and the sky is. 100% is the original; 0% removes it."),
            "fx_bloom_strength", SotcLive::fx_bloom, "SOTC_FX_BLOOM", 100.0f, strengths);
        bloom.slider = true;
        bloom.shown = [] { return true; }; // SDR too
        MenuItem& vignette = add_level(
            L("Força da vinheta", "Vignette strength"),
            L("Quanto os cantos da imagem escurecem. 100% é o original; 0% tira. Com a vinheta desligada acima, não faz nada.",
              "How much the corners of the picture darken. 100% is the original; 0% removes it. Does nothing with the vignette switched off above."),
            "fx_vignette_strength", SotcLive::fx_vignette, "SOTC_FX_VIGNETTE", 100.0f, strengths);
        vignette.slider = true;
        vignette.shown = [] { return true; };
        MenuItem& fog = add_level(
            L("Força da névoa", "Fog strength"),
            L("Quanto a névoa atmosférica junta com a distância. 100% é o original; 0% tira; 200% dobra. Com a névoa desligada acima, não faz nada.",
              "How much the atmospheric fog builds up with distance. 100% is the original; 0% removes it; 200% doubles it. Does nothing with the fog switched off above."),
            "fx_fog_strength", SotcLive::fx_fog, "SOTC_FX_FOG", 100.0f, strengths);
        fog.slider = true;
        fog.shown = [] { return true; };
        MenuItem& motion = add_level(
            L("Força do borrão de movimento", "Motion blur strength"),
            L("O comprimento do borrão quando a câmera ou algo se move. 100% é o original; 200% dobra. Só com o borrão de movimento ligado.",
              "How long the blur is when the camera or something moves. 100% is the original; 200% doubles it. Only with motion blur switched on."),
            "fx_motion_blur_strength", SotcLive::fx_motion_blur, "SOTC_FX_MOTION_BLUR", 100.0f,
            strengths);
        motion.slider = true;
        motion.shown = [] { return true; };
    }
    AddRestart(kTabDisplay, "af16", L("Filtro de texturas 16x", "16x texture filtering"),
               L("Texturas nítidas no chão visto de lado (filtro anisotrópico 16x).",
                 "Sharp textures on the ground seen at an angle (16x anisotropic filtering)."),
               OnOff(), "false");
    AddRestart(kTabDisplay, "present_mode", L("Sincronização (VSync)", "Sync (VSync)"),
               L("FIFO segue o monitor, sem cortes. Mailbox tem menos atraso. Immediate pode cortar a imagem.",
                 "FIFO follows the monitor, no tearing. Mailbox has less lag. Immediate may tear."),
               Quoted({{"Fifo", "FIFO (VSync)"}, {"Mailbox", "Mailbox"}, {"Immediate", "Immediate"}}),
               "\"Mailbox\"");
    const char* fx_help = L("Um efeito do motor do jogo, lido quando o jogo abre.",
                            "An effect of the game's engine, read when the game starts.");
    AddRestart(kTabDisplay, "fx_fog", L("Névoa atmosférica", "Atmospheric fog"), fx_help, OnOff(), "true");
    AddRestart(kTabDisplay, "fx_vignette", L("Vinheta (cantos escurecidos)", "Vignette (darkened corners)"),
               fx_help, OnOff(), "true");
    AddRestart(kTabDisplay, "fx_ssao", L("Oclusão ambiente (SSAO)", "Ambient occlusion (SSAO)"), fx_help,
               OnOff(), "true");
    AddRestart(kTabDisplay, "fx_motionblur", L("Borrão de movimento", "Motion blur"), fx_help, OnOff(),
               "true");

    // ---- Performance
    {
        MenuItem& fps = AddLive(
            kTabPerf, "FPS",
            L("O limite de quadros. Vale na hora; o modo 60 do próprio jogo (física certa) só entra ao reiniciar quando o jogo abriu em 30.",
              "The frame rate cap. Applies at once; the game's own 60 FPS mode (right physics) only comes in at a restart when the game started at 30."),
            Indexed({L("30 FPS (como no PS4)", "30 FPS (as on PS4)"), "60 FPS", L("Ilimitado", "Unlimited")}),
            [] { return FpsMode(); }, [](int i) { SetFpsMode(i); });
        fps.hotkey = "F7";
        fps.restart_pending = [] {
            return !SotcFpsCap::target_slot.load(std::memory_order_relaxed) && FpsMode() > 0;
        };
    }
    AddRestart(kTabPerf, "fps_dynamic", L("FPS dinâmico", "Dynamic FPS"),
               L("Em 60 FPS, quando o PC não segura 60 o jogo desce para um degrau estável (48 ou 40) em vez de oscilar.",
                 "At 60 FPS, when the PC cannot hold 60 the game steps down to a steady rate (48 or 40) instead of swinging."),
               OnOff(), "true");
    AddLive(kTabPerf, L("Ritmo dos quadros", "Frame pacing"),
            L("Em 60 FPS e ilimitado: solta cada quadro no ritmo do jogo em vez de na grade do vblank.",
              "At 60 FPS and unlimited: releases each frame at the game's rate instead of on the vblank grid."),
            Indexed({L("Grade do vblank (original)", "Vblank grid (original)"),
                     L("Suave (ritmo do jogo)", "Smooth (game's rate)")}),
            [] { return SotcFpsCap::flip_pacer.load(std::memory_order_relaxed) ? 1 : 0; },
            [](int i) {
                if ((i == 1) != SotcFpsCap::flip_pacer.load(std::memory_order_relaxed)) {
                    SotcFpsCap::TogglePacer();
                }
            })
        .hotkey = "F6";
    {
        MenuItem& fg = AddLive(
            kTabPerf, L("Geração de quadros", "Frame generation"),
            L("Cria quadros extras entre os do jogo. DLSS-G pede uma RTX 40 ou 50; FSR 3 roda em qualquer placa.",
              "Adds frames between the game's. DLSS-G needs an RTX 40 or 50; FSR 3 runs on any GPU."),
            Indexed({L("Desligada", "Off"), "NVIDIA DLSS-G", "AMD FSR 3"}),
            [] { return int(Vulkan::FrameGen::Current()); },
            [](int i) {
                const char* keys[] = {"\"off\"", "\"dlss\"", "\"fsr\""};
                if (Vulkan::FrameGen::Set(static_cast<Vulkan::FrameGen::Mode>(i))) {
                    SaveLauncherSetting("framegen", keys[i]);
                }
            });
        fg.hotkey = "F8";
        fg.choice_ok = [](int i) {
            return Vulkan::FrameGen::Available(static_cast<Vulkan::FrameGen::Mode>(i));
        };
    }
    AddRestart(kTabPerf, "fg_mult", L("Multiplicador do DLSS-G", "DLSS-G multiplier"),
               L("Quantos quadros o DLSS-G cria. Acima de 2x só nas RTX 50.",
                 "How many frames DLSS-G makes. Above 2x on RTX 50 cards only."),
               Quoted({{"2", L("2x (padrão)", "2x (default)")}, {"3", "3x (RTX 50)"}, {"4", "4x (RTX 50)"},
                       {"6", "6x (RTX 50)"}, {"dynamic", L("Dinâmico", "Dynamic")}}),
               "\"2\"");
    AddRestart(kTabPerf, "fg_hudless", L("HUD firme na geração", "Steady HUD with frame generation"),
               L("O HUD e as letras não tremem com a geração de quadros.",
                 "The HUD and the letters do not shake with frame generation."),
               OnOff(), "false");
    {
        static const int env_linger = [] {
            const char* v = std::getenv("SOTC_LOD_LINGER");
            return v ? std::atoi(v) : 0;
        }();
        // sotc: "Maximum" in PS4 Pro mode exhausted the game's page pool until PR #288;
        // SOTC_MAX_IN_PRO=0 blocks it again.
        static const bool neo_blocks_max = [] {
            const char* v = std::getenv("SOTC_NEO");
            const char* allow = std::getenv("SOTC_MAX_IN_PRO");
            return v && v[0] == '1' && allow && allow[0] == '0';
        }();
        MenuItem& tex = AddLive(
            kTabPerf, L("Qualidade das texturas", "Texture quality"),
            L("Alta: folhas e pedras nítidas, estável. Máxima: sempre nítida, pede mais memória de vídeo. Desligada só aparece ao reiniciar.",
              "High: sharp leaves and rocks, stable. Maximum: always sharp, needs more video memory. Off only shows after a restart."),
            Indexed({L("Desligada", "Off"), L("Alta (estável)", "High (stable)"),
                     L("Máxima (nítida)", "Maximum (sharp)")}),
            [] {
                const int linger = SotcLive::lod_linger.load(std::memory_order_relaxed);
                const int low = SotcLive::lod_pool_low.load(std::memory_order_relaxed);
                if ((linger >= 0 ? linger : env_linger) <= 0) {
                    return 0;
                }
                return (low >= 0 ? low : EnvPoolLow()) > 0 ? 1 : 2;
            },
            [](int i) { SetTextureLevel(i); });
        tex.choice_ok = [](int i) { return !(neo_blocks_max && i == 2); };
        tex.restart_pending = [] {
            return LoadLauncherSetting("textures") == "off" && Unquote(StartRaw("textures")) != "off";
        };
    }
    {
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
        AddLive(kTabPerf, L("Reaproveitar sombras distantes", "Reuse far shadows"),
                L("Desenha as sombras distantes em quadros alternados: mais FPS, mas elas podem piscar.",
                  "Draws far shadows on alternate frames: more FPS, but they may blink."),
                Indexed({L("Desligado", "Off"), L("Ligado", "On")}),
                [] {
                    const int live = SotcLive::shadow_reuse.load(std::memory_order_relaxed);
                    return (live >= 0 ? live : env_reuse) > 0 ? 1 : 0;
                },
                [](int i) {
                    SotcLive::shadow_reuse.store(i, std::memory_order_relaxed);
                    SaveLauncherSetting("shadow_reuse", i ? "\"1\"" : "\"0\"");
                });
    }
    AddRestart(kTabPerf, "vram_use", L("Memória de vídeo", "Video memory"),
               L("Quanto da memória da placa o jogo usa antes de liberar texturas.",
                 "How much of the card's memory the game uses before it frees textures."),
               Quoted({{"low", L("Econômico (placas de 4–6 GB)", "Economy (4–6 GB cards)")},
                       {"normal", L("Padrão", "Default")},
                       {"high", L("Alto (placas de 8–12 GB)", "High (8–12 GB cards)")},
                       {"max", L("Máximo (placas de 16 GB ou mais)", "Maximum (16 GB cards or more)")}}),
               "\"normal\"");
    AddLive(kTabPerf, L("Painel de desempenho", "Performance overlay"),
            L("FPS, tempo de quadro e memória de vídeo no canto da tela.",
              "FPS, frame time and video memory in the corner of the screen."),
            Indexed({L("Escondido", "Hidden"), L("Mostrado", "Shown")}),
            [] { return OverlayOn() ? 1 : 0; },
            [](int i) {
                BbSettings::Get().show_fps = false; // the ini setting only gives the initial state
                overlay_on.store(i != 0, std::memory_order_relaxed);
                SaveLauncherSetting("show_fps", i ? "true" : "false");
            })
        .hotkey = "F10";

    // ---- Grass: distance, height and detail are constants of the grass compute shaders (at once);
    // shadows and fade are engine settings read at start.
    {
        static const std::array<float, 5> env = [] {
            std::array<float, 5> g{1.f, 1.f, 1.f, 1.f, 1.f};
            if (const char* v = std::getenv("SOTC_GRASS")) {
                std::sscanf(v, "%f %f %f %f %f", &g[0], &g[1], &g[2], &g[3], &g[4]);
            }
            return g;
        }();
        const auto grass = [](const char* key, int slot, const char* label, const char* help,
                              std::vector<MenuChoice> choices) {
            std::vector<float> values;
            for (const auto& c : choices) {
                values.push_back(std::strtof(Unquote(c.value).c_str(), nullptr));
            }
            const std::string k = key;
            AddLive(kTabGrass, label, help, std::move(choices),
                    [slot, values] {
                        const float v = SotcLive::grass[slot].load(std::memory_order_relaxed);
                        const float now = v >= 0.f ? v : env[size_t(slot)];
                        for (size_t i = 0; i < values.size(); ++i) {
                            if (values[i] == now) {
                                return int(i);
                            }
                        }
                        return -1;
                    },
                    [slot, values, k](int i) {
                        SotcLive::grass[slot].store(values[size_t(i)], std::memory_order_relaxed);
                        char text[16];
                        std::snprintf(text, sizeof(text), "\"%g\"", values[size_t(i)]);
                        SaveLauncherSetting(k.c_str(), text);
                    });
        };
        const auto as_ps4 = L("Como no PS4", "As on PS4");
        const auto farther = [&](bool weeds) {
            auto c = Quoted({{"1", as_ps4},
                             {"2", L("2x mais longe", "2x farther")},
                             {"3", L("3x mais longe", "3x farther")},
                             {"5", L("5x mais longe", "5x farther")}});
            if (weeds) {
                c.push_back({L("10x mais longe (campo denso)", "10x farther (dense field)"), "\"10\""});
            }
            return c;
        };
        const auto heights = [&] {
            return Quoted({{"0.75", L("Mais baixa (75%)", "Lower (75%)")},
                           {"1", as_ps4},
                           {"1.5", L("Mais alta (150%)", "Taller (150%)")},
                           {"2", L("Bem mais alta (200%)", "Much taller (200%)")}});
        };
        grass("grass_detail", 4, L("Detalhe da grama", "Grass detail"),
              L("Lâminas completas mais longe: a grama e a sombra perto de você não surgem tarde.",
                "Full blades farther out: grass and its shadow near you no longer pop in late."),
              Quoted({{"1", as_ps4}, {"0.25", L("Alto", "High")}, {"0.15", L("Mais alto", "Higher")},
                      {"0.1", L("Muito alto", "Very high")}}));
        grass("grass_distance", 0, L("Distância da grama", "Grass distance"),
              L("Até onde a grama aparece. Mais longe pesa mais na placa.",
                "How far out the grass shows. Farther costs the GPU more."),
              farther(false));
        grass("grass_height", 1, L("Altura da grama", "Grass height"),
              L("Recomendado: como no PS4, ou 150% se gosta de mais grama; 200% vira arbusto.",
                "Recommended: as on PS4, or 150% if you like more grass; 200% turns it into bushes."),
              heights());
        grass("weed_distance", 2, L("Distância do mato", "Weed distance"),
              L("Até onde o mato aparece; 10x deixa o campo denso.",
                "How far out the weeds show; 10x makes a dense field."),
              farther(true));
        grass("weed_height", 3, L("Altura do mato", "Weed height"),
              L("A altura do mato.", "The height of the weeds."), heights());
        AddRestart(kTabGrass, "grass_shadows", L("Sombras da grama", "Grass shadows"),
                   L("Até onde a grama projeta sombra.", "How far out the grass casts shadows."),
                   Quoted({{"1", L("Perto (~3 m, mais leve)", "Near (~3 m, lighter)")},
                           {"2", L("Como no PS4 (~8 m)", "As on PS4 (~8 m)")},
                           {"3", L("Longe (~24 m, mais pesado)", "Far (~24 m, heavier)")}}),
                   "\"2\"");
        AddRestart(kTabGrass, "grass_fade", L("Esmaecimento da grama", "Grass fade"),
                   L("Abaixo de 1 a grama fica rala e a sombra dela também some; acima de 1, menos grama.",
                     "Below 1 the grass thins out and so does its shadow; above 1, less grass."),
                   Quoted({{"0.5", "0.5"}, {"0.75", "0.75"}, {"1", as_ps4}, {"2", "2"}, {"3", "3"}}),
                   "\"1\"");
    }

    // ---- Sound
    if (runtime_audio_volume_percent && runtime_audio_set_volume_percent) {
        static const int volumes[] = {100, 150, 200, 300, 400, 500};
        std::vector<MenuChoice> choices;
        for (const int v : volumes) {
            choices.push_back({std::to_string(v) + "%" + (v == 100 ? std::string("  ") + L("(original)", "(original)") : ""),
                               std::to_string(v)});
        }
        AddLive(kTabSound, L("Volume", "Volume"),
                L("O volume do jogo; 100% é o original.", "The game's volume; 100% is the original."),
                std::move(choices),
                [] {
                    const int now = runtime_audio_volume_percent();
                    for (int i = 0; i < 6; ++i) {
                        if (volumes[i] == now) {
                            return i;
                        }
                    }
                    return -1;
                },
                [](int i) {
                    runtime_audio_set_volume_percent(volumes[i]);
                    SaveLauncherSetting("volume", "\"" + std::to_string(volumes[i]) + "\"");
                });
    }
    AddRestart(kTabSound, "audio_channels", L("Canais de áudio", "Audio channels"),
               L("Automático deixa o Windows ajustar o 7.1 do jogo ao seu aparelho.",
                 "Automatic lets Windows fit the game's 7.1 to your device."),
               Quoted({{"auto", L("Automático (o do Windows)", "Automatic (Windows')")},
                       {"2", L("Estéreo 2.0", "Stereo 2.0")},
                       {"6", "Surround 5.1"},
                       {"8", "Surround 7.1"}}),
               "\"auto\"");
    AddRestart(kTabSound, "audio_lfe", L("Subwoofer (LFE)", "Subwoofer (LFE)"),
               L("Manda o canal de graves para o subwoofer.", "Sends the bass channel to the subwoofer."),
               OnOff(), "true");
    AddRestart(kTabSound, "sound_firmware", L("Módulos de som do console", "Console sound modules"),
               L("Usa os módulos de som do seu PS4 em vez do mixer embutido do port.",
                 "Uses your PS4's sound modules instead of the port's built-in mixer."),
               OnOff(), "false");

    // ---- Controls
    if (runtime_rumble_enabled && runtime_rumble_set) {
        AddLive(kTabControls, L("Vibração do controle", "Controller vibration"),
                L("Liga ou desliga a vibração do controle.", "Turns the controller's vibration on or off."),
                Indexed({L("Desligada", "Off"), L("Ligada", "On")}),
                [] { return runtime_rumble_enabled() ? 1 : 0; },
                [](int i) {
                    runtime_rumble_set(i);
                    SaveLauncherSetting("rumble", i ? "true" : "false");
                });
    }
    AddLive(kTabControls, L("Câmera pelo mouse", "Mouse camera"),
            L("Move a câmera com o mouse.", "Moves the camera with the mouse."),
            Indexed({L("Desligada", "Off"), L("Ligada", "On")}),
            [] { return BbSettings::Get().mouse_enabled.load() ? 1 : 0; },
            [](int i) {
                BbSettings::Get().mouse_enabled = i != 0;
                dirty = true;
                SaveLauncherSetting("mouse", i ? "true" : "false");
            });
    {
        static const float sens[] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
        static const char* keys[] = {"\"0.5\"", "\"0.75\"", "\"1.0\"", "\"1.5\"", "\"2.0\""};
        AddLive(kTabControls, L("Sensibilidade do mouse", "Mouse sensitivity"),
                L("A velocidade da câmera pelo mouse.", "How fast the mouse turns the camera."),
                Indexed({L("0,5 (lenta)", "0.5 (slow)"), L("0,75", "0.75"), L("1,0 (padrão)", "1.0 (default)"),
                         L("1,5", "1.5"), L("2,0 (rápida)", "2.0 (fast)")}),
                [] {
                    const float now = BbSettings::Get().mouse_sens.load();
                    int best = 0;
                    for (int i = 1; i < 5; ++i) {
                        if (std::abs(sens[i] - now) < std::abs(sens[best] - now)) {
                            best = i;
                        }
                    }
                    return best;
                },
                [](int i) {
                    BbSettings::Get().mouse_sens = sens[i];
                    dirty = true;
                    SaveLauncherSetting("mouse_sens", keys[i]);
                })
            .enabled = [] { return BbSettings::Get().mouse_enabled.load(); };
    }
    AddLive(kTabControls, L("Inverter eixo vertical (Y)", "Invert vertical axis (Y)"),
            L("Inverte a câmera pelo mouse para cima e para baixo.", "Inverts the mouse camera up and down."),
            Indexed({L("Desligado", "Off"), L("Ligado", "On")}),
            [] { return BbSettings::Get().mouse_invert.load() ? 1 : 0; },
            [](int i) {
                BbSettings::Get().mouse_invert = i != 0;
                dirty = true;
                SaveLauncherSetting("mouse_invert", i ? "true" : "false");
            })
        .enabled = [] { return BbSettings::Get().mouse_enabled.load(); };
    AddRestart(kTabControls, "button_icons", L("Ícones dos botões", "Button icons"),
               L("Os botões mostrados no jogo: os do PlayStation ou os do Xbox.",
                 "The buttons the game shows: PlayStation's or Xbox's."),
               Quoted({{"ps", L("PlayStation (do jogo)", "PlayStation (the game's)")},
                       {"xbox", "Xbox (A B X Y)"}}),
               "\"ps\"");

    // ---- Game
    AddLive(kTabGame, L("Idioma do menu", "Menu language"),
            L("O idioma deste menu. Os avisos do jogo (F7, F8) mudam ao reiniciar.",
              "This menu's language. The game's notices (F7, F8) change at the restart."),
            Indexed({"Português", "English", "Español", "简体中文", "한국어", "Русский"}),
            [] { return MenuLanguage(); },
            [](int i) {
                menu_lang.store(i, std::memory_order_relaxed);
                SaveLauncherSetting("ui_language", "\"" + std::string(kLangCodes[i]) + "\"");
            });
    AddRestart(kTabGame, "language", L("Idioma do jogo", "Game language"),
               L("O idioma dos textos e das legendas do jogo.", "The language of the game's texts and subtitles."),
               Quoted({{"", L("Automático (o do jogo)", "Automatic (the game's)")},
                       {"7", L("Português (Portugal)", "Portuguese (Portugal)")},
                       {"15", L("Português (Brasil)", "Portuguese (Brazil)")},
                       {"1", L("Inglês", "English")},
                       {"3", L("Espanhol", "Spanish")},
                       {"2", L("Francês", "French")},
                       {"4", L("Alemão", "German")},
                       {"5", L("Italiano", "Italian")},
                       {"0", L("Japonês", "Japanese")}}),
               "\"\"");
    // ---- Debug: the launcher's Debug page (test tools read at start) and the F5 / F9 actions.
    AddRestart(kTabDebug, "debug_menu", L("Menu de debug do jogo", "The game's debug menu"),
               L("O menu de testes da Bluepoint (F6 ou Start+Select no jogo): carregador de áreas, câmera livre, estatísticas. O jogo pode ficar menos estável: desligue para jogar normalmente.",
                 "Bluepoint's test menu (F6 or Start+Select in game): area loader, free camera, statistics. The game can be less stable: turn it off to play normally."),
               OnOff(), "false");
    AddRestart(kTabDebug, "skip_logos", L("Pular logos de abertura", "Skip opening logos"),
               L("Sem os logos e o aviso de salvamento ao abrir.", "No logos and autosave notice at start."),
               OnOff(), "false");
    AddRestart(kTabDebug, "skip_film", L("Pular filme e tela de título", "Skip intro film and title screen"),
               L("O jogo entra direto, sem o filme e a tela de título.", "The game goes straight in, without the film and the title screen."), OnOff(),
               "false");
    AddRestart(kTabDebug, "debug_watch", L("Vigia da memória de texturas", "Texture memory watch"),
               L("Anota em user/events.log quando o jogo reaproveita memória de texturas ainda em uso. Útil para relatar texturas com lixo colorido.",
                 "Writes to user/events.log when the game reuses memory of textures still in use. Useful when reporting colour garbage on textures."),
               OnOff(), "false");
    AddRestart(kTabDebug, "dlss_compare", L("Comparar DLSS com FSR 1 (F9)", "Compare DLSS with FSR 1 (F9)"),
               L("Com DLSS ou FSR 4, a tecla F9 alterna entre ele e o FSR 1 para comparar a imagem.",
                 "With DLSS or FSR 4, the F9 key switches between it and FSR 1 to compare the picture."),
               OnOff(), "false");
    {
        MenuItem& cap = AddLive(kTabDebug, L("Capturar texturas", "Capture textures"),
                                L("Salva as texturas do quadro atual em user/log/texcap_N.",
                                  "Saves the textures of the current frame to user/log/texcap_N."),
                                {{L("Executar", "Run"), "0"}}, [] { return -1; },
                                [](int) {
                                    SotcStats::tex_capture.store(true, std::memory_order_relaxed);
                                    toast_text = L("Texturas capturadas", "Textures captured");
                                    toast_until_ms.store(NowMs() + 2500, std::memory_order_relaxed);
                                });
        cap.hotkey = "F5";
        cap.action = true;
        MenuItem& reload = AddLive(kTabDebug, L("Recarregar texturas", "Reload textures"),
                                   L("Envia de novo todas as texturas da memória do jogo para a placa (corrige texturas erradas na hora).",
                                     "Uploads every texture again from game memory to the GPU (fixes wrong textures on the spot)."),
                                   {{L("Executar", "Run"), "0"}}, [] { return -1; },
                                   [](int) {
                                       SotcStats::reload_all.store(true, std::memory_order_relaxed);
                                       toast_text = L("Texturas recarregadas", "Textures reloaded");
                                       toast_until_ms.store(NowMs() + 2500, std::memory_order_relaxed);
                                   });
        reload.hotkey = "F9";
        reload.action = true;
    }
    (void)s;
}

std::vector<int> TabRows(int tab) {
    std::vector<int> rows;
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].tab == tab && (!items[i].shown || items[i].shown())) {
            rows.push_back(int(i));
        }
    }
    return rows;
}

bool ItemEnabled(const MenuItem& item) {
    return !item.enabled || item.enabled();
}

void PickChoice(MenuItem& item, int choice) {
    if (!ItemEnabled(item) || choice < 0 || choice >= int(item.choices.size()) ||
        (item.choice_ok && !item.choice_ok(choice)) || choice == item.get()) {
        return;
    }
    item.set(choice);
    SotcEvent::Log("MENU", "%s: %s", item.label.c_str(), item.choices[size_t(choice)].label.c_str());
}

void StepItem(MenuItem& item, int direction) {
    const int n = int(item.choices.size());
    if (n == 0 || !ItemEnabled(item)) {
        return;
    }
    if (item.action) { // a button: any change runs it
        item.set(0);
        SotcEvent::Log("MENU", "%s", item.label.c_str());
        return;
    }
    if (item.slider) { // stops at the ends: a brightness must not jump from its top to its bottom
        PickChoice(item, std::clamp(item.get() + direction, 0, n - 1));
        return;
    }
    int at = std::max(item.get(), direction > 0 ? -1 : 0);
    for (int tries = 0; tries < n; ++tries) {
        at = (at + direction + n) % n;
        if (!item.choice_ok || item.choice_ok(at)) {
            PickChoice(item, at);
            return;
        }
    }
}

void ApplyMenuInput(int input) {
    if (input == kInTabPrev || input == kInTabNext) {
        menu_tab = (menu_tab + (input == kInTabNext ? 1 : kTabCount - 1)) % kTabCount;
        return;
    }
    const auto rows = TabRows(menu_tab);
    if (rows.empty()) {
        return;
    }
    int& row = menu_row[size_t(menu_tab)];
    row = std::clamp(row, 0, int(rows.size()) - 1);
    const int n = int(rows.size());
    switch (input) {
    case kInUp:
        row = (row + n - 1) % n;
        break;
    case kInDown:
        row = (row + 1) % n;
        break;
    case kInLeft:
        StepItem(items[size_t(rows[size_t(row)])], -1);
        break;
    case kInRight:
    case kInActivate:
        StepItem(items[size_t(rows[size_t(row)])], +1);
        break;
    default:
        break;
    }
}

// The game's palette: weathered stone, cloudy sky, sand and aged gold.
namespace Pal {
constexpr ImU32 SkyTop = IM_COL32(58, 66, 76, 236);
constexpr ImU32 SkyLow = IM_COL32(16, 18, 22, 246);
constexpr ImU32 Haze = IM_COL32(120, 130, 140, 34);
constexpr ImU32 StoneHi = IM_COL32(62, 66, 70, 245);
constexpr ImU32 StoneLo = IM_COL32(40, 43, 47, 245);
constexpr ImU32 Panel = IM_COL32(24, 27, 31, 220);
constexpr ImU32 PanelSel = IM_COL32(48, 50, 51, 238);
constexpr ImU32 ValueBox = IM_COL32(15, 17, 20, 232);
constexpr ImU32 Gold = IM_COL32(200, 164, 98, 255);
constexpr ImU32 GoldBright = IM_COL32(238, 210, 148, 255);
constexpr ImU32 GoldDeep = IM_COL32(150, 122, 74, 255);
constexpr ImU32 GoldFill = IM_COL32(190, 155, 94, 255);
constexpr ImU32 Ink = IM_COL32(28, 24, 18, 255);
constexpr ImU32 Sand = IM_COL32(214, 204, 184, 255);
constexpr ImU32 SandDim = IM_COL32(150, 142, 126, 255);
constexpr ImU32 Grey = IM_COL32(100, 100, 100, 255);
constexpr ImU32 Amber = IM_COL32(218, 150, 72, 255);
constexpr ImU32 AmberBand = IM_COL32(84, 56, 24, 238);
constexpr ImU32 Moss = IM_COL32(150, 176, 132, 255);
constexpr ImU32 Cap = IM_COL32(206, 194, 168, 255);
} // namespace Pal

ImVec2 MeasureText(float px, const char* text, float wrap = 0.0f) {
    return ImGui::GetFont()->CalcTextSizeA(px, FLT_MAX, wrap, text);
}

void PutText(ImDrawList* d, float px, ImVec2 at, ImU32 color, const char* text, float wrap = 0.0f) {
    d->AddText(ImGui::GetFont(), px, at, color, text, nullptr, wrap);
}

// A text cut to a width, with "…".
std::string Fit(float px, const std::string& text, float width) {
    if (MeasureText(px, text.c_str()).x <= width) {
        return text;
    }
    std::string out = text;
    while (!out.empty() && MeasureText(px, (out + "…").c_str()).x > width) {
        out.pop_back();
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) {
            out.pop_back(); // whole UTF-8 characters
        }
        if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xC0) {
            out.pop_back();
        }
    }
    return out + "…";
}

void DrawMenu() {
    if (items_lang != MenuLanguage()) {
        BuildItems();
    }
    for (const int input : menu_inputs) {
        ApplyMenuInput(input);
        if (items_lang != MenuLanguage()) {
            BuildItems(); // the language changed: the texts are built again
        }
    }
    menu_inputs.clear();
    if (pad_hold != 0) {
        // A held controller direction repeats on a slider row (every 60 ms after 400 ms).
        const auto now = std::chrono::steady_clock::now();
        const auto rows = TabRows(menu_tab);
        const int r = menu_row[size_t(menu_tab)];
        const bool slider =
            r >= 0 && r < int(rows.size()) && items[size_t(rows[size_t(r)])].slider;
        if (slider && now >= pad_hold_next) {
            ApplyMenuInput(pad_hold < 0 ? kInLeft : kInRight);
            pad_hold_next = now + std::chrono::milliseconds(60);
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 o = vp->WorkPos;
    const float W = vp->WorkSize.x, H = vp->WorkSize.y;
    const float u = std::max(0.5f, std::min(W / 1920.0f, H / 1080.0f));
    ImGui::SetNextWindowPos(o);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##sotc_menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* d = ImGui::GetWindowDrawList();
    const bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const ImVec2 mouse = io.MousePos;
    const auto inside = [&](ImVec2 a, ImVec2 b) {
        return mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y;
    };
    const auto clicked = [&](ImVec2 a, ImVec2 b) { return click && inside(a, b); };
    bool close = false;

    // Backdrop: a cloudy sky fading into the dark, the game faint behind it.
    d->AddRectFilledMultiColor(o, ImVec2(o.x + W, o.y + H), Pal::SkyTop, Pal::SkyTop, Pal::SkyLow,
                               Pal::SkyLow);
    d->AddRectFilledMultiColor(o, ImVec2(o.x + W, o.y + H * 0.30f), Pal::Haze, Pal::Haze,
                               IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));

    // The content column: at most 1720 units wide, centred (21:9 and 32:9 screens).
    const float cw = std::min(W - 80.0f * u, 1720.0f * u);
    const float x0 = o.x + (W - cw) * 0.5f, x1 = x0 + cw;

    // Header: the title, the port's version and the PC's numbers.
    {
        const float ty = o.y + 30.0f * u;
        PutText(d, 30.0f * u, ImVec2(x0, ty), Pal::GoldBright, "SHADOW OF THE COLOSSUS");
        static const std::string version = [] {
            const char* v = std::getenv("SOTC_PORT_VERSION");
            return v ? std::string("  ·  PC ") + v : std::string();
        }();
        const std::string sub = std::string(L("Configurações", "Settings")) + version;
        PutText(d, 17.0f * u, ImVec2(x0 + 2.0f * u, ty + 38.0f * u), Pal::SandDim, sub.c_str());
        char info[192];
        int len = std::snprintf(info, sizeof(info), "%.0f FPS  ·  %.1f ms",
                                frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg);
        if (s_instance && len > 0 && len < int(sizeof(info))) {
            std::snprintf(info + len, sizeof(info) - size_t(len), "  ·  %s  ·  %llu / %llu MB",
                          std::string(s_instance->GetModelName()).c_str(),
                          static_cast<unsigned long long>(s_instance->GetDeviceMemoryUsage() >> 20),
                          static_cast<unsigned long long>(s_instance->GetTotalMemoryBudget() >> 20));
        }
        const ImVec2 isz = MeasureText(16.0f * u, info);
        PutText(d, 16.0f * u, ImVec2(x1 - isz.x, ty + 40.0f * u), Pal::SandDim, info);
    }

    // Tab bar on a band of stone, between the Q / E (L1 / R1) keys.
    static const std::string button_icons = Unquote(SettingRaw("button_icons"));
    const bool xbox = button_icons == "xbox";
    const float bar_y = o.y + 112.0f * u, bar_h = 54.0f * u;
    d->AddRectFilledMultiColor(ImVec2(o.x, bar_y), ImVec2(o.x + W, bar_y + bar_h), Pal::StoneHi,
                               Pal::StoneHi, Pal::StoneLo, Pal::StoneLo);
    d->AddLine(ImVec2(o.x, bar_y), ImVec2(o.x + W, bar_y), IM_COL32(90, 92, 94, 200), 1.0f * u);
    d->AddLine(ImVec2(o.x, bar_y + bar_h), ImVec2(o.x + W, bar_y + bar_h), Pal::GoldDeep, 1.5f * u);
    const auto key_cap = [&](float x, float y, const char* key, float h) {
        const float px = h * 0.62f;
        const ImVec2 ts = MeasureText(px, key);
        const float w = std::max(h, ts.x + h * 0.5f);
        d->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), Pal::Cap, 4.0f * u);
        PutText(d, px, ImVec2(x + (w - ts.x) * 0.5f, y + (h - ts.y) * 0.5f), Pal::Ink, key);
        return w;
    };
    {
        const char* tab_names[kTabCount] = {L("Imagem", "Display"),   L("Desempenho", "Performance"),
                                            L("Grama", "Grass"),       L("Som", "Sound"),
                                            L("Controles", "Controls"), L("Jogo", "Game"),
                                            "Debug"};
        const float cap_h = 28.0f * u;
        const char* prev_key = menu_pad ? (xbox ? "LB" : "L1") : "Q";
        const char* next_key = menu_pad ? (xbox ? "RB" : "R1") : "E";
        const float tab_px = 21.0f * u, pad = 30.0f * u;
        float total = 0.0f;
        for (const char* name : tab_names) {
            total += MeasureText(tab_px, name).x + pad * 2.0f;
        }
        float x = x0 + (cw - total) * 0.5f;
        const float cap_y = bar_y + (bar_h - cap_h) * 0.5f;
        const float pw = key_cap(x - 54.0f * u, cap_y, prev_key, cap_h);
        if (clicked(ImVec2(x - 54.0f * u, cap_y), ImVec2(x - 54.0f * u + pw, cap_y + cap_h))) {
            ApplyMenuInput(kInTabPrev);
        }
        for (int i = 0; i < kTabCount; ++i) {
            const ImVec2 ts = MeasureText(tab_px, tab_names[i]);
            const float w = ts.x + pad * 2.0f;
            const ImVec2 a(x, bar_y), b(x + w, bar_y + bar_h);
            const bool on = i == menu_tab;
            if (on) {
                d->AddRectFilledMultiColor(a, b, IM_COL32(84, 80, 70, 250), IM_COL32(84, 80, 70, 250),
                                           IM_COL32(60, 57, 50, 250), IM_COL32(60, 57, 50, 250));
                d->AddRectFilled(ImVec2(a.x, b.y - 3.0f * u), b, Pal::Gold);
            } else if (inside(a, b)) {
                d->AddRectFilled(a, b, IM_COL32(255, 255, 255, 14));
            }
            PutText(d, tab_px, ImVec2(x + pad, bar_y + (bar_h - ts.y) * 0.5f),
                    on ? Pal::GoldBright : Pal::SandDim, tab_names[i]);
            if (clicked(a, b)) {
                menu_tab = i;
            }
            x += w;
        }
        const float nw = key_cap(x + 26.0f * u, cap_y, next_key, cap_h);
        if (clicked(ImVec2(x + 26.0f * u, cap_y), ImVec2(x + 26.0f * u + nw, cap_y + cap_h))) {
            ApplyMenuInput(kInTabNext);
        }
    }

    // Restart band and footer heights.
    std::vector<std::string> waiting;
    for (const MenuItem& item : items) {
        if (item.restart_pending && item.restart_pending()) {
            waiting.push_back(item.label);
        }
    }
    const float footer_h = 64.0f * u;
    const float band_h = waiting.empty() ? 0.0f : 46.0f * u;
    const float area_top = bar_y + bar_h + 26.0f * u;
    const float area_bottom = o.y + H - footer_h - band_h - (band_h > 0 ? 16.0f * u : 0.0f) - 10.0f * u;

    // Rows of the tab.
    const auto rows = TabRows(menu_tab);
    int& row = menu_row[size_t(menu_tab)];
    row = rows.empty() ? 0 : std::clamp(row, 0, int(rows.size()) - 1);
    const float list_w = cw * 0.63f;
    const float label_w = list_w * 0.52f, value_w = list_w - label_w - 10.0f * u;
    const float row_h = 48.0f * u, gap = 6.0f * u;
    const int fit = std::max(1, int((area_bottom - area_top + gap) / (row_h + gap)));
    int& first = menu_first[size_t(menu_tab)];
    if (row < first) {
        first = row;
    } else if (row >= first + fit) {
        first = row - fit + 1;
    }
    first = std::clamp(first, 0, std::max(0, int(rows.size()) - fit));
    if (io.MouseWheel != 0.0f && inside(ImVec2(x0, area_top), ImVec2(x0 + list_w, area_bottom))) {
        first = std::clamp(first - int(io.MouseWheel), 0, std::max(0, int(rows.size()) - fit));
        row = std::clamp(row, first, first + fit - 1);
    }
    float y = area_top;
    const float label_px = 20.0f * u, value_px = 18.5f * u;
    for (int n = first; n < int(rows.size()) && n < first + fit; ++n) {
        MenuItem& item = items[size_t(rows[size_t(n)])];
        const bool on = n == row;
        const bool enabled = ItemEnabled(item);
        const ImVec2 ra(x0, y), rb(x0 + label_w, y + row_h);
        d->AddRectFilled(ra, rb, on ? Pal::PanelSel : Pal::Panel, 3.0f * u);
        if (on) {
            d->AddRectFilled(ra, ImVec2(ra.x + 4.0f * u, rb.y), Pal::Gold, 2.0f * u);
        }
        if (clicked(ra, rb)) {
            row = n;
        }
        // Label, its key and the ◆ of a setting read at start.
        const ImU32 label_col = !enabled ? Pal::Grey : on ? Pal::GoldBright : Pal::Sand;
        float lx = ra.x + 20.0f * u;
        const float marks_w = (item.hotkey.empty() ? 0.0f : 52.0f * u) + (item.restart ? 24.0f * u : 0.0f);
        const std::string label = Fit(label_px, item.label, label_w - 28.0f * u - marks_w);
        const ImVec2 ls = MeasureText(label_px, label.c_str());
        PutText(d, label_px, ImVec2(lx, y + (row_h - ls.y) * 0.5f), label_col, label.c_str());
        lx += ls.x + 10.0f * u;
        if (item.restart) {
            const ImVec2 ds = MeasureText(13.0f * u, "◆");
            PutText(d, 13.0f * u, ImVec2(lx, y + (row_h - ds.y) * 0.5f), Pal::Amber, "◆");
            lx += ds.x + 10.0f * u;
        }
        if (!item.hotkey.empty()) {
            const ImVec2 hs = MeasureText(14.0f * u, item.hotkey.c_str());
            PutText(d, 14.0f * u, ImVec2(lx, y + (row_h - hs.y) * 0.5f), Pal::SandDim, item.hotkey.c_str());
        }

        // The value: two choices side by side, more between ◀ ▶.
        const float vx = x0 + label_w + 10.0f * u;
        const int now = item.get();
        if (item.choices.size() == 2) {
            const float half = value_w * 0.5f;
            for (int c = 0; c < 2; ++c) {
                const bool lit = c == now;
                const bool ok = !item.choice_ok || item.choice_ok(c);
                const ImVec2 a(vx + half * float(c) + (c ? 2.0f * u : 0.0f), y),
                    b(vx + half * float(c + 1) - (c ? 0.0f : 2.0f * u), y + row_h);
                const ImU32 fill = lit ? (on && enabled ? Pal::GoldFill : IM_COL32(86, 72, 48, 240)) : Pal::ValueBox;
                d->AddRectFilled(a, b, fill, 3.0f * u);
                const ImU32 ink = !enabled || !ok ? Pal::Grey
                                  : lit ? (on ? Pal::Ink : Pal::GoldBright)
                                        : Pal::SandDim;
                const std::string text = Fit(value_px, item.choices[size_t(c)].label, half - 16.0f * u);
                const ImVec2 ts = MeasureText(value_px, text.c_str());
                PutText(d, value_px, ImVec2(a.x + (b.x - a.x - ts.x) * 0.5f, y + (row_h - ts.y) * 0.5f), ink,
                        text.c_str());
                if (clicked(a, b)) {
                    row = n;
                    PickChoice(item, c);
                }
            }
            if (on) {
                d->AddRect(ImVec2(vx - 2.0f * u, y - 2.0f * u), ImVec2(vx + value_w + 2.0f * u, y + row_h + 2.0f * u),
                           Pal::GoldBright, 4.0f * u, 0, 1.5f * u);
            }
        } else {
            const ImVec2 a(vx, y), b(vx + value_w, y + row_h);
            d->AddRectFilled(a, b, on && enabled ? Pal::GoldFill : Pal::ValueBox, 3.0f * u);
            if (on) {
                d->AddRect(ImVec2(a.x - 2.0f * u, a.y - 2.0f * u), ImVec2(b.x + 2.0f * u, b.y + 2.0f * u),
                           Pal::GoldBright, 4.0f * u, 0, 1.5f * u);
            }
            const ImU32 ink = !enabled ? Pal::Grey : on ? Pal::Ink : Pal::Sand;
            const ImU32 arrow = !enabled ? Pal::Grey : on ? Pal::Ink : Pal::GoldDeep;
            const std::string value =
                item.action ? item.choices[0].label + "  ↵"
                : now >= 0  ? item.choices[size_t(now)].label
                            : std::string("—");
            if (item.action && !on) {
                d->AddRect(a, b, Pal::GoldDeep, 3.0f * u, 0, 1.0f * u); // a button
            }
            const std::string text = Fit(value_px, value, value_w - 80.0f * u);
            const ImVec2 ts = MeasureText(value_px, text.c_str());
            const float lift = item.slider ? 4.0f * u : 0.0f; // room for the bar below
            PutText(d, value_px, ImVec2(a.x + (value_w - ts.x) * 0.5f, y + (row_h - ts.y) * 0.5f - lift),
                    ink, text.c_str());
            const float apx = 14.0f * u;
            const ImVec2 as = MeasureText(apx, "◀");
            if (!item.action) {
                const int last = int(item.choices.size()) - 1;
                // A slider's arrow at its end is dimmed: it goes no further.
                const ImU32 left = item.slider && now <= 0 ? Pal::Grey : arrow;
                const ImU32 right = item.slider && now >= last ? Pal::Grey : arrow;
                PutText(d, apx, ImVec2(a.x + 14.0f * u, y + (row_h - as.y) * 0.5f), left, "◀");
                PutText(d, apx, ImVec2(b.x - 14.0f * u - as.x, y + (row_h - as.y) * 0.5f), right, "▶");
            }
            if (item.slider) {
                // The bar along the bottom of the box, filled up to the value, with a handle.
                const int last = std::max(int(item.choices.size()) - 1, 1);
                const float bx0 = a.x + 40.0f * u, bx1 = b.x - 40.0f * u;
                const float by = b.y - 7.0f * u;
                const float t = float(std::clamp(now, 0, last)) / float(last);
                const float hx = bx0 + (bx1 - bx0) * t;
                d->AddRectFilled(ImVec2(bx0, by - 1.5f * u), ImVec2(bx1, by + 1.5f * u),
                                 on && enabled ? IM_COL32(60, 48, 30, 200) : IM_COL32(40, 36, 30, 200),
                                 1.5f * u);
                d->AddRectFilled(ImVec2(bx0, by - 1.5f * u), ImVec2(hx, by + 1.5f * u),
                                 !enabled ? Pal::Grey : on ? Pal::Ink : Pal::Gold, 1.5f * u);
                d->AddCircleFilled(ImVec2(hx, by), 4.5f * u,
                                   !enabled ? Pal::Grey : on ? Pal::Ink : Pal::GoldBright);
                // Click on the box and drag: the value follows the pointer (between the arrows).
                const int index = rows[size_t(n)];
                if (clicked(ImVec2(bx0 - 12.0f * u, a.y), ImVec2(bx1 + 12.0f * u, b.y)) && enabled) {
                    slider_drag = index;
                    row = n;
                }
                if (slider_drag == index) {
                    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        slider_drag = -1;
                    } else {
                        const float f = std::clamp((mouse.x - bx0) / (bx1 - bx0), 0.0f, 1.0f);
                        PickChoice(item, int(std::lround(f * float(last))));
                    }
                }
                if (slider_drag != index) {
                    if (clicked(a, ImVec2(bx0 - 12.0f * u, b.y))) {
                        row = n;
                        StepItem(item, -1);
                    } else if (clicked(ImVec2(bx1 + 12.0f * u, a.y), b)) {
                        row = n;
                        StepItem(item, +1);
                    }
                }
            } else if (clicked(a, ImVec2(a.x + value_w * 0.5f, b.y))) {
                row = n;
                StepItem(item, -1);
            } else if (clicked(ImVec2(a.x + value_w * 0.5f, a.y), b)) {
                row = n;
                StepItem(item, +1);
            }
        }
        y += row_h + gap;
    }
    // More rows above or below.
    if (first > 0) {
        PutText(d, 14.0f * u, ImVec2(x0 + label_w * 0.5f, area_top - 20.0f * u), Pal::SandDim, "▲");
    }
    if (first + fit < int(rows.size())) {
        PutText(d, 14.0f * u, ImVec2(x0 + label_w * 0.5f, y - 2.0f * u), Pal::SandDim, "▼");
    }

    // Help for the chosen row, on the right.
    if (!rows.empty()) {
        const MenuItem& item = items[size_t(rows[size_t(row)])];
        const float hx = x0 + list_w + 40.0f * u, hw = x1 - hx;
        const float pad = 22.0f * u;
        const float title_px = 23.0f * u, body_px = 18.0f * u, note_px = 16.5f * u;
        const float wrap = hw - pad * 2.0f;
        const ImVec2 title = MeasureText(title_px, item.label.c_str(), wrap);
        const ImVec2 body = MeasureText(body_px, item.help.c_str(), wrap);
        const char* note = item.restart ? L("◆  Vale ao reiniciar o jogo", "◆  Applies when the game restarts")
                                        : L("●  Vale na hora", "●  Applies at once");
        const ImVec2 ns = MeasureText(note_px, note, wrap);
        const float hh = pad + title.y + 14.0f * u + body.y + 24.0f * u + ns.y + pad;
        const ImVec2 a(hx, area_top), b(x1, area_top + hh);
        d->AddRectFilled(a, b, Pal::Panel, 4.0f * u);
        d->AddRect(a, b, IM_COL32(110, 100, 80, 120), 4.0f * u, 0, 1.0f * u);
        d->AddRectFilled(a, ImVec2(b.x, a.y + 3.0f * u), Pal::GoldDeep, 4.0f * u, ImDrawFlags_RoundCornersTop);
        float hy = a.y + pad;
        PutText(d, title_px, ImVec2(hx + pad, hy), Pal::GoldBright, item.label.c_str(), wrap);
        hy += title.y + 14.0f * u;
        PutText(d, body_px, ImVec2(hx + pad, hy), Pal::Sand, item.help.c_str(), wrap);
        hy += body.y + 12.0f * u;
        d->AddLine(ImVec2(hx + pad, hy), ImVec2(b.x - pad, hy), IM_COL32(110, 100, 80, 120), 1.0f * u);
        hy += 12.0f * u;
        PutText(d, note_px, ImVec2(hx + pad, hy), item.restart ? Pal::Amber : Pal::Moss, note, wrap);
    }

    // The restart band: what waits for the game to start again (kept in the launcher's settings).
    if (!waiting.empty()) {
        const float by = o.y + H - footer_h - band_h - 16.0f * u;
        const ImVec2 a(x0, by), b(x1, by + band_h);
        d->AddRectFilled(a, b, Pal::AmberBand, 4.0f * u);
        d->AddRect(a, b, Pal::Amber, 4.0f * u, 0, 1.5f * u);
        std::string list;
        for (const auto& w : waiting) {
            list += (list.empty() ? "" : ", ") + w;
        }
        const std::string head = std::string("⚠  ") + L("Reinicie o jogo para aplicar", "Restart the game to apply") + ":  ";
        const float px = 18.5f * u;
        const ImVec2 hs = MeasureText(px, head.c_str());
        PutText(d, px, ImVec2(a.x + 18.0f * u, by + (band_h - hs.y) * 0.5f), Pal::GoldBright, head.c_str());
        const std::string rest = Fit(px, list, cw - hs.x - 36.0f * u);
        PutText(d, px, ImVec2(a.x + 18.0f * u + hs.x, by + (band_h - hs.y) * 0.5f), Pal::Sand, rest.c_str());
    }

    // Footer: "kept automatically" on the left, the keys on the right (keyboard or controller,
    // whichever was used last).
    {
        const float fy = o.y + H - footer_h + (footer_h - 30.0f * u) * 0.5f;
        d->AddLine(ImVec2(x0, o.y + H - footer_h), ImVec2(x1, o.y + H - footer_h), IM_COL32(110, 100, 80, 90),
                   1.0f * u);
        PutText(d, 16.0f * u, ImVec2(x0, fy + 5.0f * u), Pal::SandDim,
                L("Tudo fica salvo automaticamente", "Everything is saved automatically"));
        const float cap_h = 28.0f * u;
        float kx = x1;
        const auto group = [&](const char* label, std::initializer_list<const char*> keys, bool closes) {
            const float right = kx;
            const ImVec2 ls = MeasureText(17.0f * u, label);
            kx -= ls.x;
            PutText(d, 17.0f * u, ImVec2(kx, fy + (cap_h - ls.y) * 0.5f), Pal::Sand, label);
            kx -= 10.0f * u;
            for (auto it = std::rbegin(keys); it != std::rend(keys); ++it) {
                const float w = std::max(cap_h, MeasureText(cap_h * 0.62f, *it).x + cap_h * 0.5f);
                kx -= w;
                key_cap(kx, fy, *it, cap_h);
                kx -= 5.0f * u;
            }
            if (closes && clicked(ImVec2(kx, fy), ImVec2(right, fy + cap_h))) {
                close = true;
            }
            kx -= 30.0f * u;
        };
        if (menu_pad) {
            group(L("Fechar", "Close"), {xbox ? "B" : "○"}, true);
            group(L("Abas", "Tabs"), {xbox ? "LB" : "L1", xbox ? "RB" : "R1"}, false);
            group(L("Mudar", "Change"), {"◀", "▶"}, false);
            group(L("Escolher", "Choose"), {"▲", "▼"}, false);
        } else {
            group(L("Fechar", "Close"), {"Esc"}, true);
            group(L("Abas", "Tabs"), {"Q", "E"}, false);
            group(L("Mudar", "Change"), {"◀", "▶"}, false);
            group(L("Escolher", "Choose"), {"▲", "▼"}, false);
        }
    }
    ImGui::End();
    if (close) {
        SetOpen(false);
    }
}

// Menu input from the window thread (under imgui_mutex): keys, buttons and the left stick become
// menu moves, applied at the next frame. Returns true when the event was the menu's.
bool MenuEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN: {
        menu_pad = false;
        const bool repeat = event.key.repeat;
        switch (event.key.key) {
        case SDLK_UP:
        case SDLK_W:
            menu_inputs.push_back(kInUp);
            break;
        case SDLK_DOWN:
        case SDLK_S:
            menu_inputs.push_back(kInDown);
            break;
        case SDLK_LEFT:
        case SDLK_A:
            menu_inputs.push_back(kInLeft);
            break;
        case SDLK_RIGHT:
        case SDLK_D:
            menu_inputs.push_back(kInRight);
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            if (!repeat) {
                menu_inputs.push_back(kInActivate);
            }
            break;
        case SDLK_Q:
        case SDLK_PAGEUP:
            if (!repeat) {
                menu_inputs.push_back(kInTabPrev);
            }
            break;
        case SDLK_E:
        case SDLK_PAGEDOWN:
            if (!repeat) {
                menu_inputs.push_back(kInTabNext);
            }
            break;
        case SDLK_TAB:
            if (!repeat) {
                menu_inputs.push_back((event.key.mod & SDL_KMOD_SHIFT) ? kInTabPrev : kInTabNext);
            }
            break;
        default:
            break;
        }
        return true;
    }
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        menu_pad = true;
        switch (event.gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
            menu_inputs.push_back(kInUp);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            menu_inputs.push_back(kInDown);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
            menu_inputs.push_back(kInLeft);
            PadHold(-1);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            menu_inputs.push_back(kInRight);
            PadHold(+1);
            break;
        case SDL_GAMEPAD_BUTTON_SOUTH:
            menu_inputs.push_back(kInActivate);
            break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
            menu_inputs.push_back(kInTabPrev);
            break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
            menu_inputs.push_back(kInTabNext);
            break;
        case SDL_GAMEPAD_BUTTON_EAST:
        case SDL_GAMEPAD_BUTTON_START:
            SetOpen(false);
            break;
        default:
            break;
        }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if ((event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT && pad_hold < 0) ||
            (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT && pad_hold > 0)) {
            pad_hold = 0;
        }
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const bool lx = event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX;
        const bool ly = event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY;
        if (!lx && !ly) {
            return true;
        }
        bool* held = lx ? stick_x : stick_y;
        const bool neg = event.gaxis.value < -20000, pos = event.gaxis.value > 20000;
        if (neg && !held[0]) {
            menu_pad = true;
            menu_inputs.push_back(lx ? kInLeft : kInUp);
            if (lx) {
                PadHold(-1);
            }
        }
        if (pos && !held[1]) {
            menu_pad = true;
            menu_inputs.push_back(lx ? kInRight : kInDown);
            if (lx) {
                PadHold(+1);
            }
        }
        if (lx && !neg && !pos) {
            pad_hold = 0;
        }
        held[0] = neg;
        held[1] = pos;
        return true;
    }
    default:
        return false;
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

std::atomic<const char*> upscaler_label{nullptr};

// sotc: the upscaler shown while comparing DLSS with FSR1 (F9, vk_dlssg.cpp), top right,
// below the FPS counter when it is shown (fps_bottom: its lower edge this frame, else 0).
void UpscalerLabel(const char* label, float fps_bottom) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    const float top = fps_bottom > 0.0f ? fps_bottom + pad * 0.5f : viewport->WorkPos.y + pad;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, top),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##upscaler", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::Text("%s  (F9)", label);
    ImGui::End();
}

float FpsCounter() {
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
                        FpsModeName(FpsMode()));
    const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
    ImGui::End();
    return bottom;
}

// sotc: while F3 shows the game's original picture, a label at the top left says so.
void CompareLabel() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad, viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##compare", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted("ORIGINAL (F3)");
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

static u32 s_image_count = 2;
static vk::Format s_format = vk::Format::eUndefined;
static bool InitBackend(vk::Format format);

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

    s_image_count = image_count;
    if (!InitBackend(format)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    (void)StartRaw("fps_mode"); // sotc: the launcher's settings as the game starts (restart band)
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
}

// sotc: the ImGui Vulkan backend for the swapchain's `format` (its pipeline and, on the HDR10
// swapchain, overlay_hdr.frag).
static bool InitBackend(vk::Format format) {
    const Vulkan::Instance& instance = *s_instance;
    const u32 image_count = s_image_count;
    static VkFormat color_format;
    color_format = static_cast<VkFormat>(format);
    s_format = format;
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = instance.GetInstance();
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
    // sotc: on the HDR10 swapchain (PQ) the menu's sRGB colours are encoded at 203 nits
    // (overlay_hdr.frag) instead of landing at up to 10000 nits.
    if (format == vk::Format::eA2B10G10R10UnormPack32) {
        info.CustomShaderFragCreateInfo = VkShaderModuleCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(OVERLAY_HDR_FRAG),
            .pCode = OVERLAY_HDR_FRAG,
        };
    }
    return ImGui_ImplVulkan_Init(&info);
}

void OnSurfaceFormatChange(vk::Format format) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized || format == s_format) {
        return;
    }
    // The caller waited for the device to be idle; ImGui recreates its textures on the next
    // frame (the backend destroyed them).
    ImGui_ImplVulkan_Shutdown();
    if (!InitBackend(format)) {
        std::printf("Overlay: ImGui Vulkan backend re-init failed\n");
        initialized = false;
        return;
    }
    std::printf("Overlay: swapchain format %d, menu %s\n", int(format),
                format == vk::Format::eA2B10G10R10UnormPack32 ? "in HDR10 (203 nits)" : "in SDR");
}

void UpdateTextInput(SDL_Window* window) {
    // sotc: the menu's fullscreen choice, switched here on the window thread without the lock.
    if (const int fullscreen = pending_fullscreen.exchange(-1, std::memory_order_relaxed);
        fullscreen >= 0 && g_window) {
        g_window->SetFullscreen(fullscreen != 0);
    }
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
    // sotc: F7 switches 30 -> 60 -> unlimited FPS, as the menu's FPS choice (kept for the launcher).
    // The new state is shown at the next frame (Render).
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F7 && !event.key.repeat) {
        SetFpsMode((FpsMode() + 1) % 3);
        return true;
    }
    // sotc: F6 switches uncapped flip pacing: the game's own rate or the vblank grid
    // (sotc_fps_cap.h, videoout driver.cpp).
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F6 && !event.key.repeat) {
        const bool paced = SotcFpsCap::TogglePacer();
        SotcEvent::Log("KEY", "%s",
                       paced ? SotcUi::T("F6: ritmo dos quadros suave", "F6: smooth frame pacing")
                             : SotcUi::T("F6: quadros na grade do vblank",
                                         "F6: flips on the vblank grid"));
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
    // sotc: F3 switches between the user's picture and the game's own (tone mapper, grading,
    // picture controls, effect strengths), brightness and peak kept; a label shows while on.
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F3 && !event.key.repeat) {
        const bool on = !SotcLive::compare_vanilla.load(std::memory_order_relaxed);
        SotcLive::compare_vanilla.store(on, std::memory_order_relaxed);
        const char* what = on ? SotcUi::T("F3: imagem original do jogo", "F3: the game's original picture")
                              : SotcUi::T("F3: sua imagem", "F3: your picture");
        SotcEvent::Log("KEY", "%s", what);
        toast_text = what;
        toast_until_ms.store(NowMs() + 2000, std::memory_order_relaxed);
        return true;
    }
    // sotc: F5 (diagnostic) saves the textures of the next frame (sotc_stats.h).
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F5 && !event.key.repeat) {
        SotcStats::tex_capture.store(true, std::memory_order_relaxed);
        SotcEvent::Log("KEY", "%s", SotcUi::T("F5: captura de texturas", "F5: texture capture"));
        toast_text = SotcUi::T("F5: texturas capturadas", "F5: textures captured");
        toast_until_ms.store(NowMs() + 2500, std::memory_order_relaxed);
        return true;
    }
    // sotc: F9 (diagnostic) uploads every cached texture again from guest memory (sotc_stats.h).
    // With the launcher's DLSS comparison on (SOTC_DLSS_COMPARE=1), F9 switches DLSS / FSR 1
    // instead (window.cpp).
    static const bool dlss_compare = [] {
        const char* v = std::getenv("SOTC_DLSS_COMPARE");
        return v && v[0] == '1';
    }();
    if (!dlss_compare && event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F9 &&
        !event.key.repeat) {
        SotcStats::reload_all.store(true, std::memory_order_relaxed);
        SotcEvent::Log("KEY", "%s", SotcUi::T("F9: recarregar texturas", "F9: reload textures"));
        toast_text = SotcUi::T("F9: texturas recarregadas", "F9: textures reloaded");
        toast_until_ms.store(NowMs() + 2500, std::memory_order_relaxed);
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
        return MenuEvent(event); // sotc: the menu's own navigation (DrawMenu)
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
        return MenuEvent(event);
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return is_open && MenuEvent(event);
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
                           ToastShown() || LoadingShown() || TriggerPending() ||
                           upscaler_label.load(std::memory_order_relaxed) ||
                           SotcLive::compare_vanilla.load(std::memory_order_relaxed));
}

void SetUpscalerLabel(const char* label) {
    upscaler_label.store(label, std::memory_order_relaxed);
}

std::string LauncherSetting(const char* key) {
    return LoadLauncherSetting(key);
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
        // sotc: the on-screen FPS counter in the log: each second, the lowest and the last value
        // it showed ("SOTC SCREEN FPS"), so the log has what the player sees (the 5 s averages of
        // "SOTC FPS" hide a 1 s dip to 32).
        if (frame_ms_avg > 0.0f) {
            static const auto start = now;
            static auto second_start = now;
            static float lowest = 1e9f;
            const float shown = 1000.0f / frame_ms_avg;
            lowest = std::min(lowest, shown);
            if (now - second_start >= std::chrono::seconds(1)) {
                std::fprintf(stderr, "SOTC SCREEN FPS %.0fs: lowest %.0f, now %.0f\n",
                             std::chrono::duration<float>(now - start).count(), lowest, shown);
                second_start = now;
                lowest = 1e9f;
            }
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
    // The flip pacing was switched (F6).
    static bool shown_paced = SotcFpsCap::flip_pacer;
    if (const bool paced = SotcFpsCap::flip_pacer; paced != shown_paced) {
        shown_paced = paced;
        std::scoped_lock lock{imgui_mutex};
        toast_text = paced ? SotcUi::T("Ritmo dos quadros: suave (ritmo do jogo)",
                                       "Frame pacing: smooth (game's rate)")
                           : SotcUi::T("Ritmo dos quadros: grade do vblank (original)",
                                       "Frame pacing: vblank grid (original)");
        toast_until_ms.store(NowMs() + 2500, std::memory_order_relaxed);
    }
    // The FPS mode was switched (F7 or the menu).
    static int shown_fps_mode = FpsMode();
    if (const int fps_mode = FpsMode(); fps_mode != shown_fps_mode) {
        shown_fps_mode = fps_mode;
        std::scoped_lock lock{imgui_mutex};
        toast_text = FpsModeName(fps_mode);
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

    EnsureCjkFont(); // sotc: Chinese / Korean glyphs, before the frame (fonts change between frames)
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
            } else if (key == "tab" || key == "row" || key == "step" || key == "lang") {
                // sotc: the menu's tab, row, a step of the chosen row (1 / -1) or its language
                const std::string value = text.substr(key.size() + 1);
                if (key == "tab") {
                    menu_tab = std::clamp(std::atoi(value.c_str()), 0, kTabCount - 1);
                } else if (key == "row") {
                    menu_row[size_t(menu_tab)] = std::max(0, std::atoi(value.c_str()));
                } else if (key == "step") {
                    menu_inputs.push_back(std::atoi(value.c_str()) < 0 ? kInLeft : kInRight);
                } else {
                    menu_lang.store(LangFromCode(value), std::memory_order_relaxed);
                }
            }
            std::fprintf(stderr, "SOTC SET: %s\n", line);
        }
    }
    if (menu_open) {
        DrawMenu();
    }
    float fps_bottom = 0.0f;
    if (OverlayOn() && !menu_open) {
        fps_bottom = FpsCounter();
    }
    if (ToastShown()) {
        Toast();
    }
    if (SotcLive::compare_vanilla.load(std::memory_order_relaxed)) {
        CompareLabel();
    }
    if (const char* label = upscaler_label.load(std::memory_order_relaxed)) {
        UpscalerLabel(label, fps_bottom);
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
