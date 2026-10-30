// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SotC Lab window (F2): free camera, autopilot routes, boss jumps, SOTC_* switches and the
// stat tests. Drawn from the overlay's ImGui frame (bbport_overlay.cpp). Every action goes through
// SotcLab::Execute or the module APIs in gpu/sotc/*.h. Text follows the launcher's language
// (SotcUi::T, SOTC_UI_LANG). Persistent files: user/lab_bookmarks.tsv, user/routes/, the switch
// list from SOTC_TUNING_TSV (default out/tuning.tsv).
#include "sotc_lab_menu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h" // ImGui::GetActiveID (the overlay includes it too)
#include "video_core/sotc_trace.h" // sotc: SOTC_TRACE dump button
#include "video_core/sotc_ui_lang.h"
#include "../sotc/autopilot.h"
#include "../sotc/engine_cmd.h"
#include "../sotc/freecam.h"
#include "../sotc/lab_commands.h"
#include "../sotc/sotc_live.h"

namespace SotcLabMenu {
namespace {

std::atomic<bool> g_visible{false};

// Language helpers. SotcUi::Pt() is true when the launcher runs in Portuguese.
const char* T(const char* pt, const char* en) {
    return SotcUi::T(pt, en);
}

// Returns the text in the active language, falling back to the other one when it is empty.
std::string LangText(const std::string& pt, const std::string& en) {
    const std::string& first = SotcUi::Pt() ? pt : en;
    const std::string& second = SotcUi::Pt() ? en : pt;
    return first.empty() ? second : first;
}

std::string Lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// --- TSV helpers -------------------------------------------------------------------------------
// The switch list and the bookmarks use the same escaping: backslash, tab and newline.

std::string Unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[i + 1];
            if (n == 't') {
                out += '\t';
                ++i;
                continue;
            }
            if (n == 'n') {
                out += '\n';
                ++i;
                continue;
            }
            if (n == '\\') {
                out += '\\';
                ++i;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

std::string Escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

// Splits on real tabs only (escaped tabs are the two characters backslash and t).
std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            out.push_back(line.substr(start));
            break;
        }
        out.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return out;
}

std::string FloatText(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", static_cast<double>(v));
    return b;
}

// Reads one TSV file line by line, skipping the UTF-8 BOM and CR line endings.
template <class Fn>
bool ReadLines(const std::string& path, Fn&& fn) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (first && line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) {
            line.erase(0, 3);
        }
        first = false;
        fn(line);
    }
    return true;
}

// --- Switch list (out/tuning.tsv) --------------------------------------------------------------

struct Option {
    std::string value, label_pt, label_en;
};

struct Switch {
    std::string name, kind, group, label_pt, label_en, hint_pt, hint_en, def, current;
    std::vector<Option> options;
};

struct Group {
    std::string id, label_pt, label_en;
};

// "value=label_pt|label_en" entries joined by ";;".
std::vector<Option> ParseOptions(const std::string& raw) {
    std::vector<Option> out;
    if (raw.empty()) {
        return out;
    }
    size_t start = 0;
    for (;;) {
        const size_t sep = raw.find(";;", start);
        const std::string item =
            raw.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
        Option o;
        const size_t eq = item.find('=');
        o.value = eq == std::string::npos ? std::string() : item.substr(0, eq);
        const std::string lab = eq == std::string::npos ? item : item.substr(eq + 1);
        const size_t bar = lab.find('|');
        if (bar == std::string::npos) {
            o.label_pt = lab;
            o.label_en = lab;
        } else {
            o.label_pt = lab.substr(0, bar);
            o.label_en = lab.substr(bar + 1);
        }
        out.push_back(o);
        if (sep == std::string::npos) {
            break;
        }
        start = sep + 2;
    }
    return out;
}

// --- Lab state ---------------------------------------------------------------------------------

struct Bookmark {
    std::string name;
    SotcFreecam::Pose pose;
};

using TextBuf = std::array<char, 256>;
using LiveMap = std::map<std::string, int>;
using PendingMap = std::map<std::string, std::string>;

struct Lab {
    bool init_done{};

    // Camera
    std::vector<Bookmark> bookmarks;
    char bm_name[64]{};
    float goto_v[5]{}; // x y z yaw pitch
    float cam_speed{5.0f};
    bool cam_speed_set{};

    // Routes
    std::vector<std::string> routes;
    int route_sel{-1};
    float route_speed{20.0f};
    bool route_speed_set{};
    char rec_name[64]{};
    char seg_name[64]{};

    // Colossi
    int colossus_sel{-1};
    int mode{0}; // 0 play, 1 kill, 2 outro
    bool horse{true};
    int relaunch_boss{0};

    // SOTC_*
    bool tuning_loaded{};
    std::string tuning_path;
    std::string tuning_err;
    std::vector<Group> groups;
    std::vector<Switch> switches;
    char filter[128]{};
    std::map<std::string, TextBuf> value_edit;

    // Tests
    char matrix_path[256] = "tools/matrices/example.json";
    char mark_name[64]{};

    // Last reply or message, shown under the tabs.
    std::string msg;
};

Lab ui;

// --- Bookmarks (user/lab_bookmarks.tsv) --------------------------------------------------------

constexpr const char* kBookmarksPath = "user/lab_bookmarks.tsv";

void LoadBookmarks() {
    ui.bookmarks.clear();
    ReadLines(kBookmarksPath, [](const std::string& line) {
        if (line.empty()) {
            return;
        }
        const auto f = SplitTabs(line);
        if (f.size() < 6) {
            return;
        }
        Bookmark b;
        b.name = Unescape(f[0]);
        b.pose.pos.x = std::strtof(f[1].c_str(), nullptr);
        b.pose.pos.y = std::strtof(f[2].c_str(), nullptr);
        b.pose.pos.z = std::strtof(f[3].c_str(), nullptr);
        b.pose.yaw = std::strtof(f[4].c_str(), nullptr);
        b.pose.pitch = std::strtof(f[5].c_str(), nullptr);
        ui.bookmarks.push_back(b);
    });
}

void SaveBookmarks() {
    std::error_code ec;
    std::filesystem::create_directories("user", ec);
    std::ofstream out(kBookmarksPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        ui.msg = T("não foi possível gravar user/lab_bookmarks.tsv",
                   "could not write user/lab_bookmarks.tsv");
        return;
    }
    for (const Bookmark& b : ui.bookmarks) {
        out << Escape(b.name) << '\t' << FloatText(b.pose.pos.x) << '\t'
            << FloatText(b.pose.pos.y) << '\t' << FloatText(b.pose.pos.z) << '\t'
            << FloatText(b.pose.yaw) << '\t' << FloatText(b.pose.pitch) << '\n';
    }
}

// --- Switch list loading -----------------------------------------------------------------------

void LoadTuning() {
    ui.tuning_loaded = true;
    ui.groups.clear();
    ui.switches.clear();
    ui.tuning_err.clear();
    const char* env = std::getenv("SOTC_TUNING_TSV");
    ui.tuning_path = (env && *env) ? env : "out/tuning.tsv";

    const bool ok = ReadLines(ui.tuning_path, [](const std::string& line) {
        if (line.empty()) {
            return;
        }
        if (line.rfind("#group\t", 0) == 0) {
            const auto f = SplitTabs(line);
            if (f.size() >= 4) {
                ui.groups.push_back({Unescape(f[1]), Unescape(f[2]), Unescape(f[3])});
            }
            return;
        }
        if (line[0] == '#') {
            return;
        }
        auto f = SplitTabs(line);
        if (f.size() < 8) {
            return;
        }
        f.resize(11);
        Switch s;
        s.name = Unescape(f[0]);
        s.kind = Unescape(f[1]);
        s.group = Unescape(f[2]);
        s.label_pt = Unescape(f[3]);
        s.label_en = Unescape(f[4]);
        s.hint_pt = Unescape(f[5]);
        s.hint_en = Unescape(f[6]);
        s.def = Unescape(f[7]);
        // f[8] is the live flag written by the exporter; the registry (SotcLive::IsLive) is
        // what the window trusts at draw time.
        s.current = Unescape(f[9]);
        s.options = ParseOptions(Unescape(f[10]));
        if (!s.name.empty()) {
            ui.switches.push_back(std::move(s));
        }
    });
    if (!ok) {
        ui.tuning_err = std::string(T("não encontrado (rode scripts/tuning_export.py): ",
                                      "not found (run scripts/tuning_export.py): ")) +
                        ui.tuning_path;
        return;
    }

    // Groups referenced by switches but not declared go into an "Other" section.
    std::set<std::string> known;
    for (const Group& g : ui.groups) {
        known.insert(g.id);
    }
    for (const Switch& sw : ui.switches) {
        if (known.insert(sw.group).second) {
            ui.groups.push_back({sw.group, "Outros", "Other"});
        }
    }
}

// --- Common widgets ----------------------------------------------------------------------------

void HoverTip(const std::string& tip) {
    if (!tip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tip.c_str());
    }
}

void Badge(bool live) {
    ImGui::SameLine();
    if (live) {
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.35f, 1.0f), "%s", T("ao vivo", "live"));
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", T("reinício", "restart"));
    }
}

// --- SOTC_* tab --------------------------------------------------------------------------------

bool IsOn(const std::string& kind, const std::string& eff) {
    if (eff.empty()) {
        return kind == "flag_on"; // empty = default
    }
    return std::atoi(eff.c_str()) != 0;
}

// Index of the option that matches the effective value, or -1.
int FindOption(const Switch& sw, const std::string& eff) {
    for (size_t i = 0; i < sw.options.size(); ++i) {
        if (sw.options[i].value == eff) {
            return static_cast<int>(i);
        }
    }
    if (eff.empty() || eff == sw.def) {
        for (size_t i = 0; i < sw.options.size(); ++i) {
            if (sw.options[i].value.empty()) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

// Live switches apply at once ("set"); the others are queued for the next launch ("pending").
void Apply(const Switch& sw, bool is_live, const std::string& value) {
    ui.msg = SotcLab::Execute(std::string(is_live ? "set " : "pending ") + sw.name + "=" + value);
}

void DrawSwitch(const Switch& sw, const LiveMap& live, const PendingMap& pending) {
    ImGui::PushID(sw.name.c_str());
    const bool is_live = SotcLive::IsLive(sw.name);

    // Effective value: the live value, else the queued one, else the value from the export.
    std::string eff;
    const auto lit = live.find(sw.name);
    const auto pit = pending.find(sw.name);
    if (is_live && lit != live.end()) {
        eff = std::to_string(lit->second);
    } else if (pit != pending.end()) {
        eff = pit->second;
    } else {
        eff = sw.current;
    }

    const std::string label = LangText(sw.label_pt, sw.label_en).empty()
                                  ? sw.name
                                  : LangText(sw.label_pt, sw.label_en);
    const std::string hint = LangText(sw.hint_pt, sw.hint_en);
    const std::string tip = hint.empty() ? sw.name : hint + "\n" + sw.name;

    ImGui::TextUnformatted(label.c_str());
    HoverTip(tip);
    Badge(is_live);

    if (sw.kind == "flag" || sw.kind == "flag_on") {
        bool on = IsOn(sw.kind, eff);
        if (ImGui::Checkbox("##v", &on)) {
            Apply(sw, is_live, on ? "1" : "0");
        }
        HoverTip(tip);
    } else if (sw.kind == "choice") {
        const int sel = FindOption(sw, eff);
        std::string shown = eff;
        if (sel >= 0) {
            const Option& o = sw.options[static_cast<size_t>(sel)];
            shown = LangText(o.label_pt, o.label_en);
        }
        if (ImGui::BeginCombo("##v", shown.c_str())) {
            for (size_t i = 0; i < sw.options.size(); ++i) {
                const Option& o = sw.options[i];
                const std::string olabel = LangText(o.label_pt, o.label_en);
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(olabel.c_str(), static_cast<int>(i) == sel)) {
                    Apply(sw, is_live, o.value);
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        HoverTip(tip);
    } else {
        // "value": free text, committed with Enter. Live values must be integers.
        TextBuf& buf = ui.value_edit[sw.name];
        if (ImGui::GetActiveID() != ImGui::GetID("##v")) {
            std::snprintf(buf.data(), buf.size(), "%s", eff.c_str());
        }
        const std::string vhint =
            std::string(T("padrão: ", "default: ")) +
            std::string(sw.def.empty() ? T("(vazio)", "(empty)") : sw.def.c_str());
        if (ImGui::InputTextWithHint("##v", vhint.c_str(), buf.data(), buf.size(),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
            Apply(sw, is_live, buf.data());
        }
        HoverTip(tip);
    }
    ImGui::Spacing();
    ImGui::PopID();
}

void DrawPending(const PendingMap& pending) {
    ImGui::Separator();
    ImGui::Text("%s (%d)", T("Pendentes (aplicados na próxima abertura)",
                             "Pending (applied on the next launch)"),
                static_cast<int>(pending.size()));
    if (pending.empty()) {
        ImGui::TextDisabled("%s", T("nenhum", "none"));
        return;
    }
    for (const auto& [name, value] : pending) {
        ImGui::BulletText("%s = %s", name.c_str(), value.empty() ? "(default)" : value.c_str());
    }
    if (ImGui::Button(T("Salvar para a próxima abertura", "Save for the next launch"))) {
        std::string err;
        ui.msg = SotcLab::WritePendingEnv(0, &err)
                     ? std::string(T("salvo em user/lab_pending_env.json",
                                     "saved to user/lab_pending_env.json"))
                     : err;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Reabrir agora", "Reopen now"))) {
        if (SotcLab::OrchestratorAttached()) {
            ui.msg = SotcLab::Execute("relaunch");
        } else {
            std::string err;
            ui.msg = SotcLab::WritePendingEnv(0, &err)
                         ? std::string(T("Feche e abra o jogo pelo launcher: ele aplicará as mudanças",
                                         "Close and reopen the game from the launcher: it will apply the changes"))
                         : err;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Descartar", "Discard"))) {
        SotcLab::ClearPending();
        ui.msg = T("pendentes descartados", "pending changes discarded");
    }
}

void DrawSwitchesTab() {
    if (!ui.tuning_loaded) {
        LoadTuning();
    }
    const std::vector<SotcLive::Entry> snap = SotcLive::Snapshot();
    LiveMap live;
    for (const auto& e : snap) {
        live[e.name] = e.value;
    }
    const PendingMap pending = SotcLab::Pending();

    if (ImGui::Button(T("Recarregar", "Reload"))) {
        LoadTuning();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Restaurar padrões", "Restore defaults"))) {
        for (const auto& e : snap) {
            ui.msg = SotcLab::Execute("set " + e.name + "=");
        }
        SotcLab::ClearPending();
        ui.value_edit.clear();
        ui.msg = T("padrões restaurados", "defaults restored");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", ui.tuning_path.c_str());
    if (!ui.tuning_err.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", ui.tuning_err.c_str());
    }
    ImGui::InputTextWithHint("##filter", T("buscar...", "search..."), ui.filter, sizeof(ui.filter));

    DrawPending(pending);
    ImGui::Separator();

    const std::string flt = Lower(ui.filter);
    auto matches = [&flt](const Switch& sw) {
        if (flt.empty()) {
            return true;
        }
        const std::string hay =
            Lower(sw.name + " " + sw.label_pt + " " + sw.label_en + " " + sw.hint_pt + " " + sw.hint_en);
        return hay.find(flt) != std::string::npos;
    };

    for (const Group& gr : ui.groups) {
        std::vector<const Switch*> items;
        for (const Switch& sw : ui.switches) {
            if (sw.group == gr.id && matches(sw)) {
                items.push_back(&sw);
            }
        }
        if (items.empty() && !flt.empty()) {
            continue;
        }
        const std::string header = LangText(gr.label_pt, gr.label_en) + "###grp_" + gr.id;
        if (!flt.empty()) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }
        if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const Switch* sw : items) {
                DrawSwitch(*sw, live, pending);
            }
        }
    }
}

// --- Camera tab --------------------------------------------------------------------------------

void DrawCamera() {
    const SotcFreecam::State st = SotcFreecam::Get();
    if (!ui.cam_speed_set && st.speed > 0.0f) {
        ui.cam_speed = st.speed;
        ui.cam_speed_set = true;
    }
    float hi = SotcFreecam::MaxSpeed();
    if (hi < 1.0f) {
        hi = 1.0f;
    }

    bool on = st.active;
    if (!st.enabled) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Checkbox(T("Câmera livre", "Free camera"), &on)) {
        if (!SotcFreecam::SetActive(on)) {
            ui.msg = T("não foi possível mudar a câmera livre agora (carregamento em curso?)",
                       "could not change the free camera now (loading in progress?)");
        }
    }
    if (!st.enabled) {
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s", T("câmera livre indisponível: hooks não encontrados",
                                    "free camera unavailable: hooks not found"));
    }

    ImGui::Text("pos  %.2f  %.2f  %.2f", st.cam.pos.x, st.cam.pos.y, st.cam.pos.z);
    ImGui::Text("yaw  %.3f rad   pitch  %.3f rad", st.cam.yaw, st.cam.pitch);
    if (st.player_known) {
        ImGui::TextDisabled("%s  %.2f  %.2f  %.2f", T("jogador:", "player:"), st.player.x,
                            st.player.y, st.player.z);
    }
    ImGui::Text("%s: %s", T("Autopiloto", "Autopilot"),
                st.autopilot ? T("sim", "yes") : T("não", "no"));

    if (ImGui::SliderFloat(T("Velocidade (m/s)", "Speed (m/s)"), &ui.cam_speed, 0.5f, hi, "%.1f")) {
        SotcFreecam::SetSpeed(ui.cam_speed);
    }

    ImGui::Separator();
    ImGui::TextUnformatted(T("Ir para pose", "Go to pose"));
    ImGui::InputFloat3("x y z", ui.goto_v);
    ImGui::InputFloat2(T("yaw pitch (rad)", "yaw pitch (rad)"), ui.goto_v + 3);
    if (ImGui::Button(T("Usar pose atual", "Use current pose"))) {
        ui.goto_v[0] = st.cam.pos.x;
        ui.goto_v[1] = st.cam.pos.y;
        ui.goto_v[2] = st.cam.pos.z;
        ui.goto_v[3] = st.cam.yaw;
        ui.goto_v[4] = st.cam.pitch;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Ir", "Go"))) {
        SotcFreecam::Pose p;
        p.pos = {ui.goto_v[0], ui.goto_v[1], ui.goto_v[2]};
        p.yaw = ui.goto_v[3];
        p.pitch = ui.goto_v[4];
        if (!SotcFreecam::Goto(p)) {
            ui.msg = T("ir para a pose falhou", "goto failed");
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted(T("Marcadores", "Bookmarks"));
    ImGui::InputTextWithHint("##bmname", T("nome da pose", "pose name"), ui.bm_name, sizeof(ui.bm_name));
    ImGui::SameLine();
    if (ImGui::Button(T("Salvar pose atual", "Save current pose"))) {
        Bookmark b;
        b.name = ui.bm_name[0] != '\0'
                     ? std::string(ui.bm_name)
                     : std::string(T("marca ", "mark ")) + std::to_string(ui.bookmarks.size() + 1);
        b.pose = st.cam;
        ui.bookmarks.push_back(b);
        SaveBookmarks();
        ui.bm_name[0] = '\0';
    }

    int del = -1;
    for (size_t i = 0; i < ui.bookmarks.size(); ++i) {
        const Bookmark& b = ui.bookmarks[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextUnformatted(b.name.c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 150.0f * ImGui::GetFontSize() / 18.0f);
        if (ImGui::Button(T("Ir", "Go"))) {
            SotcFreecam::Goto(b.pose);
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Apagar", "Delete"))) {
            del = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (del >= 0) {
        ui.bookmarks.erase(ui.bookmarks.begin() + del);
        SaveBookmarks();
    }
}

// --- Routes tab --------------------------------------------------------------------------------

void RefreshRoutes() {
    ui.routes = SotcAutopilot::ListRoutes();
    if (ui.route_sel >= static_cast<int>(ui.routes.size())) {
        ui.route_sel = -1;
    }
}

void DrawRoutes() {
    if (ImGui::Button(T("Atualizar lista", "Refresh list"))) {
        RefreshRoutes();
    }
    ImGui::BeginChild("##routes", ImVec2(0.0f, 120.0f * ImGui::GetFontSize() / 18.0f));
    for (size_t i = 0; i < ui.routes.size(); ++i) {
        if (ImGui::Selectable(ui.routes[i].c_str(), ui.route_sel == static_cast<int>(i))) {
            ui.route_sel = static_cast<int>(i);
        }
    }
    ImGui::EndChild();
    if (ImGui::Button(T("Carregar", "Load")) && ui.route_sel >= 0) {
        std::string err;
        const std::string& path = ui.routes[static_cast<size_t>(ui.route_sel)];
        if (SotcAutopilot::Load(path, &err)) {
            ui.route_speed_set = false;
            ui.msg = std::string(T("rota carregada: ", "route loaded: ")) + path;
        } else {
            ui.msg = err;
        }
    }

    const SotcAutopilot::Progress pr = SotcAutopilot::GetProgress();
    const SotcAutopilot::Route route = SotcAutopilot::Current();
    if (!route.name.empty() || pr.loaded) {
        ImGui::Text("%s: %s (%zu %s)", T("Rota", "Route"), route.name.c_str(), route.points.size(),
                    T("pontos", "points"));
    }
    if (!route.points.empty() && !ui.route_speed_set) {
        ui.route_speed = route.speed;
        ui.route_speed_set = true;
    }

    if (!pr.loaded) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(T("Play", "Play"))) {
        SotcAutopilot::Play();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Pausa", "Pause"))) {
        SotcAutopilot::Pause();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Parar", "Stop"))) {
        SotcAutopilot::Stop();
    }
    bool loop = pr.loop;
    ImGui::SameLine();
    if (ImGui::Checkbox(T("Repetir", "Loop"), &loop)) {
        SotcAutopilot::SetLoop(loop);
    }
    float hi = SotcFreecam::MaxSpeed();
    if (hi < 1.0f) {
        hi = 1.0f;
    }
    if (ImGui::SliderFloat(T("Velocidade (m/s)", "Speed (m/s)"), &ui.route_speed, 0.5f, hi, "%.1f")) {
        SotcAutopilot::SetSpeed(ui.route_speed);
    }
    if (!pr.loaded) {
        ImGui::EndDisabled();
    }

    const float frac = pr.distance_total > 0.0f ? pr.distance_done / pr.distance_total : 0.0f;
    char overlay[96];
    std::snprintf(overlay, sizeof overlay, "%.0f / %.0f m", pr.distance_done, pr.distance_total);
    ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), overlay);
    ImGui::Text("%s: %s   %s: %d   %s: %zu / %zu", T("Segmento", "Segment"),
                pr.seg.empty() ? "-" : pr.seg.c_str(), T("Volta", "Lap"), pr.lap,
                T("Ponto", "Point"), pr.count ? pr.point + 1 : size_t{0}, pr.count);

    ImGui::Separator();
    ImGui::TextUnformatted(T("Gravação", "Recording"));
    ImGui::InputTextWithHint("##recname", T("nome da rota", "route name"), ui.rec_name, sizeof(ui.rec_name));
    ImGui::SameLine();
    if (ImGui::Button(T("Iniciar gravação", "Start recording"))) {
        SotcAutopilot::StartRecording(ui.rec_name);
    }
    ImGui::InputTextWithHint("##segname", T("segmento (opcional)", "segment (optional)"), ui.seg_name,
                             sizeof(ui.seg_name));
    if (!pr.recording) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(T("Adicionar ponto (F4)", "Add point (F4)"))) {
        SotcAutopilot::AddWaypoint(SotcFreecam::Get().cam, std::string(ui.seg_name));
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Parar gravação", "Stop recording"))) {
        SotcAutopilot::StopRecording();
    }
    if (!pr.recording) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Salvar rota", "Save route"))) {
        if (ui.rec_name[0] == '\0') {
            ui.msg = T("dê um nome à rota antes de salvar", "give the route a name before saving");
        } else {
            const std::string path = std::string("user/routes/") + ui.rec_name + ".json";
            std::error_code ec;
            std::filesystem::create_directories("user/routes", ec);
            std::string err;
            ui.msg = SotcAutopilot::Save(path, &err)
                         ? std::string(T("rota salva em ", "route saved to ")) + path
                         : err;
            RefreshRoutes();
        }
    }
}

// --- Colossi tab -------------------------------------------------------------------------------

void DrawColossi() {
    const SotcEngineCmd::Colossus* cols = SotcEngineCmd::Colossi();
    const bool busy = SotcEngineCmd::BossJumpPending();
    const bool water = ui.colossus_sel >= 0 && cols[ui.colossus_sel].water_arena;

    ImGui::TextUnformatted(T("Modo:", "Mode:"));
    ImGui::SameLine();
    ImGui::RadioButton(T("Jogar", "Play"), &ui.mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton(T("Matar", "Kill"), &ui.mode, 1);
    ImGui::SameLine();
    ImGui::RadioButton(T("Pós-luta", "Aftermath"), &ui.mode, 2);

    if (water) {
        ImGui::BeginDisabled();
    }
    ImGui::Checkbox(T("Cavalo (warp)", "Horse warp"), &ui.horse);
    if (water) {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T("indisponível na arena da água", "not available in water arenas"));
    }
    if (ui.mode != 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s",
                           T("Atenção: Matar e Pós-luta alteram o save. O save é copiado antes para "
                             "user/save-backups.",
                             "Warning: Kill and Aftermath change the save. It is copied first to "
                             "user/save-backups."));
    }
    if (busy) {
        static const char kSpin[] = {'|', '/', '-', '\\'};
        const int frame = static_cast<int>(ImGui::GetTime() * 8.0) & 3;
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s %c",
                           T("Carregando colosso...", "Loading colossus..."), kSpin[frame]);
    }

    static const char* kModeWord[] = {"play", "kill", "outro"};
    const float table_h = 300.0f * ImGui::GetFontSize() / 18.0f;
    if (ImGui::BeginTable("##colossi", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, table_h))) {
        ImGui::TableSetupColumn("N");
        ImGui::TableSetupColumn(T("Colosso", "Colossus"));
        ImGui::TableSetupColumn(T("Água", "Water"));
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (int i = 0; i < SotcEngineCmd::kColossusCount; ++i) {
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(std::to_string(i + 1).c_str(), ui.colossus_sel == i,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                ui.colossus_sel = i;
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(cols[i].name);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(cols[i].water_arena ? T("sim", "yes") : "-");
            ImGui::TableSetColumnIndex(3);
            if (busy) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button(T("Lutar", "Fight"))) {
                const bool horse_ok = ui.horse && !cols[i].water_arena;
                std::string cmd = "boss " + std::to_string(i + 1) + " " +
                                  kModeWord[ui.mode < 0 || ui.mode > 2 ? 0 : ui.mode];
                if (!horse_ok) {
                    cmd += " nohorse";
                }
                ui.msg = SotcLab::Execute(cmd);
                const bool unavailable = ui.msg.rfind("err live-unavailable", 0) == 0;
                ui.relaunch_boss = unavailable ? i + 1 : 0;
            }
            if (busy) {
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (ui.relaunch_boss > 0) {
        const int n = ui.relaunch_boss;
        const std::string label = std::string(T("Reabrir no colosso ", "Reopen at colossus ")) +
                                  std::to_string(n);
        if (ImGui::Button(label.c_str())) {
            ui.msg = SotcLab::Execute("relaunch boss " + std::to_string(n));
            if (!SotcLab::OrchestratorAttached()) {
                std::string err;
                if (SotcLab::WritePendingEnv(n, &err)) {
                    ui.msg = std::string(T("Feche e abra o jogo pelo launcher: ele abrirá no colosso ",
                                           "Close and reopen the game from the launcher: it will open at colossus ")) +
                             std::to_string(n);
                } else {
                    ui.msg = err;
                }
            }
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted(T("Saída do motor", "Engine output"));
    ImGui::BeginChild("##engout", ImVec2(0.0f, 140.0f * ImGui::GetFontSize() / 18.0f));
    for (const std::string& line : SotcEngineCmd::RecentOutput()) {
        ImGui::TextUnformatted(line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

// --- Tests tab ---------------------------------------------------------------------------------

void DrawTests() {
    ImGui::InputText(T("Matriz", "Matrix"), ui.matrix_path, sizeof(ui.matrix_path));
    if (ImGui::Button(T("Iniciar", "Start"))) {
        std::string err;
        ui.msg = SotcLab::StartMatrix(ui.matrix_path, &err)
                     ? std::string(T("matriz iniciada", "matrix started"))
                     : err;
    }
    ImGui::SameLine();
    if (SotcLab::OrchestratorAttached()) {
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.35f, 1.0f), "%s",
                           T("orquestrador conectado", "orchestrator attached"));
    } else {
        ImGui::TextDisabled("%s", T("orquestrador não conectado", "orchestrator not attached"));
    }

    const SotcLab::Window w = SotcLab::CurrentWindow();
    ImGui::Separator();
    ImGui::Text("%s: %s", T("Segmento", "Segment"), w.segment.empty() ? "-" : w.segment.c_str());
    ImGui::Text("%s: %llu", T("Quadros", "Frames"), static_cast<unsigned long long>(w.frames));
    ImGui::Text("p50 %.1f ms   p99 %.1f ms   max %.1f ms", w.p50, w.p99, w.max);
    ImGui::Text("%s >33: %llu   >50: %llu   >100: %llu", T("Engasgos", "Hitches"),
                static_cast<unsigned long long>(w.hitch33),
                static_cast<unsigned long long>(w.hitch50),
                static_cast<unsigned long long>(w.hitch100));
    const std::string area = SotcLab::Area();
    ImGui::Text("%s: %s", T("Área", "Area"), area.empty() ? "-" : area.c_str());

    ImGui::Separator();
    ImGui::InputTextWithHint("##markname", T("nome do segmento", "segment name"), ui.mark_name,
                             sizeof(ui.mark_name));
    ImGui::SameLine();
    if (ImGui::Button(T("Marcar segmento", "Mark segment"))) {
        ui.msg = SotcLab::Execute(std::string("mark ") + ui.mark_name);
    }

    // sotc: SOTC_TRACE dump on demand (docs/sotc/LAB.md, "Trace"); disabled when the recorder is off.
    ImGui::Separator();
    if (!SotcTrace::kOn) {
        ImGui::TextDisabled("%s", T("SOTC_TRACE desligado", "SOTC_TRACE is off"));
    }
    ImGui::BeginDisabled(!SotcTrace::kOn);
    if (ImGui::Button(T("Gravar trace agora", "Record trace now"))) {
        ui.msg = SotcLab::Execute("trace dump menu");
    }
    ImGui::EndDisabled();
    const SotcTrace::DumpInfo dump = SotcTrace::LastDump();
    ImGui::Text("%s: %s", T("Último trace", "Last trace"), dump.path.empty() ? "-" : dump.path.c_str());
}

} // namespace

bool Available() {
    return SotcLab::Enabled();
}

bool Visible() {
    return g_visible.load(std::memory_order_relaxed);
}

void Toggle() {
    g_visible.store(!g_visible.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void Draw() {
    if (!Visible() || !Available()) {
        return;
    }
    if (!ui.init_done) {
        ui.init_done = true;
        LoadBookmarks();
        LoadTuning();
        RefreshRoutes();
    }

    const float s = ImGui::GetFontSize() / 18.0f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 40.0f * s, vp->WorkPos.y + 40.0f * s),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(660.0f * s, 620.0f * s), ImGuiCond_FirstUseEver);

    bool open = true;
    const bool expanded = ImGui::Begin("SotC Lab###sotclab", &open, ImGuiWindowFlags_NoCollapse);
    if (!open) {
        g_visible.store(false, std::memory_order_relaxed);
    }
    if (expanded) {
        if (ImGui::BeginTabBar("##labtabs")) {
            if (ImGui::BeginTabItem(T("Câmera", "Camera"))) {
                DrawCamera();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(T("Rotas", "Routes"))) {
                DrawRoutes();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(T("Colossos", "Colossi"))) {
                DrawColossi();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("SOTC_*")) {
                DrawSwitchesTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(T("Testes", "Tests"))) {
                DrawTests();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (!ui.msg.empty()) {
            ImGui::Separator();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(ui.msg.c_str());
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::End();
}

} // namespace SotcLabMenu
