// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SotC Lab command layer (grammar and replies in lab_commands.h). Every command is parsed
// here and forwarded to the subsystem that owns the state: freecam, autopilot, engine commands,
// live switches. Each piece of state below has its own mutex; no lock is held while another
// subsystem is called, so Execute can run on any thread (control pipe, F2 menu, SOTC_SET_TRIGGER).

#include "lab_commands.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "autopilot.h"
#include "engine_cmd.h"
#include "freecam.h"
#include "sotc_live.h"
#include "video_core/sotc_trace.h" // sotc: SOTC_TRACE (trace commands, segment marks)

#include <SDL3/SDL.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace SotcLab {
namespace {

constexpr size_t kMaxFrameSamples = 1000000;
constexpr auto kAttachTimeout = std::chrono::seconds(10);
const char* const kPendingPath = "user/lab_pending_env.json";

// --- Frame statistics (OnFrame, Mark, CurrentWindow) -------------------------------------------

struct FrameStats {
    std::mutex mutex;
    std::string segment;
    uint64_t frames{};
    uint64_t hitch33{}, hitch50{}, hitch100{};
    double max{};
    std::vector<double> samples;
};

FrameStats& Stats() {
    static FrameStats stats;
    return stats;
}

double Percentile(std::vector<double>& values, double q) {
    if (values.empty()) {
        return 0.0;
    }
    const size_t k = static_cast<size_t>(std::llround(q * static_cast<double>(values.size() - 1)));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
    return values[k];
}

// --- Shared state not covered by the frame statistics -------------------------------------------

struct Shared {
    std::mutex mutex;
    std::string area;
    std::map<std::string, std::string> pending;
    bool relaunch_requested{};
    int relaunch_boss{};
};

Shared& State() {
    static Shared shared;
    return shared;
}

std::atomic<int64_t> g_attach_ticks{0}; // steady_clock ticks of the last "attach", 0 = never

// --- JSON output -------------------------------------------------------------------------------

std::string JsonStr(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
    return out;
}

std::string JsonNum(double v) {
    if (!std::isfinite(v)) {
        return "null";
    }
    char buf[400]; // %.3f of the largest finite double fits
    std::snprintf(buf, sizeof buf, "%.3f", v);
    return buf;
}

std::string JsonBool(bool b) {
    return b ? "true" : "false";
}

std::string JsonMapObject(const std::map<std::string, std::string>& m) {
    std::string out = "{";
    bool first = true;
    for (const auto& [key, value] : m) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += JsonStr(key) + ":" + JsonStr(value);
    }
    return out + "}";
}

// --- Parsing -----------------------------------------------------------------------------------

std::string_view Trim(std::string_view s) {
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) {
        return {};
    }
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// A command line split into space-separated tokens. Rest(i) is the text from token i to the end
// (used for free-form arguments such as file names, engine lines and segment names).
struct Args {
    std::string_view line;
    std::vector<std::string_view> tok;
    std::vector<size_t> at;

    size_t size() const { return tok.size(); }
    std::string_view operator[](size_t i) const { return tok[i]; }
    std::string_view Rest(size_t i) const {
        return i < tok.size() ? line.substr(at[i]) : std::string_view{};
    }
};

Args Split(std::string_view raw) {
    Args a;
    a.line = Trim(raw);
    size_t i = 0;
    while (i < a.line.size()) {
        while (i < a.line.size() && (a.line[i] == ' ' || a.line[i] == '\t')) {
            ++i;
        }
        if (i >= a.line.size()) {
            break;
        }
        const size_t b = i;
        while (i < a.line.size() && a.line[i] != ' ' && a.line[i] != '\t') {
            ++i;
        }
        a.tok.push_back(a.line.substr(b, i - b));
        a.at.push_back(b);
    }
    return a;
}

bool ParseFloat(std::string_view s, double* out) {
    const std::string text(s);
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

bool ParseInt(std::string_view s, long long* out) {
    const std::string text(s);
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(text.c_str(), &end, 10);
    if (end != text.c_str() + text.size() || errno == ERANGE) {
        return false;
    }
    *out = v;
    return true;
}

bool ParseColossus(std::string_view s, int* out) {
    long long v = 0;
    if (!ParseInt(s, &v) || v < 1 || v > SotcEngineCmd::kColossusCount) {
        return false;
    }
    *out = static_cast<int>(v);
    return true;
}

// Splits "NAME=value" (value may be empty: back to the default).
bool SplitAssignment(std::string_view text, std::string* name, std::string* value) {
    const size_t eq = text.find('=');
    if (eq == std::string_view::npos || eq == 0) {
        return false;
    }
    *name = std::string(text.substr(0, eq));
    *value = std::string(text.substr(eq + 1));
    return name->find_first_of(" \t") == std::string::npos;
}

std::string ErrWith(const std::string& why, const char* fallback) {
    return "err " + (why.empty() ? std::string(fallback) : why);
}

// --- Commands ----------------------------------------------------------------------------------

std::string CmdFreecam(const Args& a) {
    if (a.size() != 2 || (a[1] != "on" && a[1] != "off")) {
        return "err usage: freecam on|off";
    }
    if (!SotcFreecam::SetActive(a[1] == "on")) {
        return "err freecam-unavailable";
    }
    return "ok";
}

std::string CmdGoto(const Args& a) {
    if (a.size() != 4 && a.size() != 6) {
        return "err usage: goto x y z [yaw pitch]";
    }
    double v[5] = {};
    const size_t n = a.size() - 1;
    for (size_t i = 0; i < n; ++i) {
        if (!ParseFloat(a[i + 1], &v[i])) {
            return "err bad-number";
        }
    }
    // Position is given; yaw and pitch keep the current free-cam pose unless given.
    SotcFreecam::Pose pose = SotcFreecam::Get().cam;
    pose.pos = {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
    if (n == 5) {
        pose.yaw = static_cast<float>(v[3]);
        pose.pitch = static_cast<float>(v[4]);
    }
    if (!SotcFreecam::Goto(pose)) {
        return "err goto-failed";
    }
    return "ok";
}

std::string CmdSpeed(const Args& a) {
    double v = 0.0;
    if (a.size() != 2 || !ParseFloat(a[1], &v) || v <= 0.0) {
        return "err usage: speed <m/s> (positive number)";
    }
    SotcFreecam::SetSpeed(static_cast<float>(v));
    return "ok";
}

std::string CmdPath(const Args& a) {
    if (a.size() < 2) {
        return "err usage: path load|play|pause|stop|seek|loop|save|record|waypoint|stoprecord";
    }
    const std::string_view sub = a[1];
    std::string err;
    if (sub == "load") {
        if (a.size() < 3) {
            return "err usage: path load <file>";
        }
        if (!SotcAutopilot::Load(std::string(a.Rest(2)), &err)) {
            return ErrWith(err, "load-failed");
        }
        return "ok";
    }
    if (sub == "save") {
        if (a.size() < 3) {
            return "err usage: path save <file>";
        }
        if (!SotcAutopilot::Save(std::string(a.Rest(2)), &err)) {
            return ErrWith(err, "save-failed");
        }
        return "ok";
    }
    if (sub == "play") {
        SotcAutopilot::Play();
        return "ok";
    }
    if (sub == "pause") {
        SotcAutopilot::Pause();
        return "ok";
    }
    if (sub == "stop") {
        SotcAutopilot::Stop();
        return "ok";
    }
    if (sub == "seek") {
        long long point = 0;
        if (a.size() != 3 || !ParseInt(a[2], &point) || point < 0) {
            return "err usage: path seek <point>";
        }
        SotcAutopilot::Seek(static_cast<size_t>(point));
        return "ok";
    }
    if (sub == "loop") {
        if (a.size() != 3 || (a[2] != "on" && a[2] != "off")) {
            return "err usage: path loop on|off";
        }
        SotcAutopilot::SetLoop(a[2] == "on");
        return "ok";
    }
    if (sub == "record") {
        if (a.size() < 3) {
            return "err usage: path record <name>";
        }
        SotcAutopilot::StartRecording(std::string(a.Rest(2)));
        return "ok";
    }
    if (sub == "waypoint") {
        if (!SotcAutopilot::GetProgress().recording) {
            return "err not-recording";
        }
        const std::string seg = a.size() >= 3 ? std::string(a.Rest(2)) : std::string();
        SotcAutopilot::AddWaypoint(SotcFreecam::Get().cam, seg);
        return "ok";
    }
    if (sub == "stoprecord") {
        SotcAutopilot::StopRecording();
        return "ok";
    }
    return "err unknown-path-command";
}

std::string CmdBoss(const Args& a) {
    int n = 0;
    if (a.size() < 2 || !ParseColossus(a[1], &n)) {
        return "err usage: boss <1..16> [play|kill|outro] [nohorse]";
    }
    SotcEngineCmd::BossMode mode = SotcEngineCmd::BossMode::Play;
    bool horse = true;
    for (size_t i = 2; i < a.size(); ++i) {
        if (a[i] == "play") {
            mode = SotcEngineCmd::BossMode::Play;
        } else if (a[i] == "kill") {
            mode = SotcEngineCmd::BossMode::Kill;
        } else if (a[i] == "outro") {
            mode = SotcEngineCmd::BossMode::Outro;
        } else if (a[i] == "nohorse") {
            horse = false;
        } else if (a[i] == "horse") {
            horse = true;
        } else {
            return "err bad-argument";
        }
    }
    std::string why;
    if (!SotcEngineCmd::RequestBoss(n, mode, horse, &why)) {
        return why.empty() ? "err live-unavailable" : "err live-unavailable " + why;
    }
    return "ok";
}

std::string CmdEngine(const Args& a) {
    const std::string_view raw = a.Rest(1);
    if (a.size() < 2) {
        return "err usage: engine <raw engine console line>";
    }
    std::string err;
    if (!SotcEngineCmd::Queue(std::string(raw), &err)) {
        return ErrWith(err, "queue-failed");
    }
    return "ok";
}

std::string CmdSet(const Args& a) {
    std::string name, value;
    if (a.size() < 2 || !SplitAssignment(a.Rest(1), &name, &value)) {
        return "err usage: set NAME=value";
    }
    switch (SotcLive::Set(name, value)) {
    case SotcLive::SetResult::Ok:
        return "ok";
    case SotcLive::SetResult::NotLive:
        SetPending(name, value); // the next launch picks it up (WritePendingEnv)
        return "err needs-restart";
    case SotcLive::SetResult::BadValue:
        break;
    }
    return "err bad-value";
}

std::string CmdPending(const Args& a) {
    std::string name, value;
    if (a.size() < 2 || !SplitAssignment(a.Rest(1), &name, &value)) {
        return "err usage: pending NAME=value";
    }
    SetPending(name, value);
    return "ok";
}

std::string CmdRelaunch(const Args& a) {
    int boss = 0;
    if (a.size() == 3 && a[1] == "boss") {
        if (!ParseColossus(a[2], &boss)) {
            return "err bad-argument";
        }
    } else if (a.size() != 1) {
        return "err usage: relaunch [boss N]";
    }
    std::string err;
    if (!WritePendingEnv(boss, &err)) {
        return ErrWith(err, "pending-env-failed");
    }
    RequestRelaunch(boss);
    return "ok";
}

std::string CmdMark(const Args& a) {
    if (a.size() < 2) {
        return "err usage: mark <segment>";
    }
    Mark(a.Rest(1));
    if (SotcTrace::kOn) { // sotc: the segment names the trace frames that follow
        const std::string seg(a.Rest(1));
        SotcTrace::Mark(seg.c_str());
        SotcTrace::SetSegment(seg.c_str(), 0);
    }
    return "ok";
}

// trace dump [reason] | mark <text> | stats | threshold <ms> (SOTC_TRACE, see sotc_trace.h)
std::string CmdTrace(const Args& a) {
    if (a.size() < 2) {
        return "err usage: trace dump|mark|stats|threshold";
    }
    const std::string_view sub = a[1];
    if (sub == "stats") {
        return SotcTrace::kOn ? SotcTrace::StatsJson() : std::string("{\"mode\":0}");
    }
    if (sub == "dump") {
        if (!SotcTrace::kOn) {
            return "err off";
        }
        const std::string reason = a.size() >= 3 ? std::string(a.Rest(2)) : std::string("lab");
        return SotcTrace::RequestDump(reason.c_str()) ? "ok queued" : "err limit";
    }
    if (sub == "mark") {
        if (a.size() < 3) {
            return "err usage: trace mark <text>";
        }
        if (!SotcTrace::kOn) {
            return "err off";
        }
        const std::string text(a.Rest(2));
        SotcTrace::Mark(text.c_str());
        return "ok";
    }
    if (sub == "threshold") {
        if (a.size() != 3) {
            return "err usage: trace threshold <ms>";
        }
        if (!SotcTrace::kOn) {
            return "err off";
        }
        const std::string text(a[2]);
        if (text.empty() || text[0] < '0' || text[0] > '9') {
            return "err bad-argument";
        }
        char* end = nullptr;
        const unsigned long long ms = std::strtoull(text.c_str(), &end, 10);
        if (end != text.c_str() + text.size()) {
            return "err bad-argument";
        }
        SotcTrace::SetThresholdMs(ms);
        return "ok";
    }
    return "err usage: trace dump|mark|stats|threshold";
}

std::string JsonWindow(const Window& w) {
    return "{\"frames\":" + std::to_string(w.frames) + ",\"p50\":" + JsonNum(w.p50) +
           ",\"p99\":" + JsonNum(w.p99) + ",\"max\":" + JsonNum(w.max) +
           ",\"hitch33\":" + std::to_string(w.hitch33) +
           ",\"hitch50\":" + std::to_string(w.hitch50) +
           ",\"hitch100\":" + std::to_string(w.hitch100) + "}";
}

std::string JsonLive(const std::vector<SotcLive::Entry>& entries) {
    std::string out = "{";
    bool first = true;
    for (const auto& e : entries) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += JsonStr(e.name) + ":" + std::to_string(e.value);
    }
    return out + "}";
}

std::string JsonState() {
    const SotcFreecam::State fc = SotcFreecam::Get();
    const SotcAutopilot::Progress ap = SotcAutopilot::GetProgress();
    const Window w = CurrentWindow();
    const std::string area = Area();
    const bool boss_pending = SotcEngineCmd::BossJumpPending();
    const auto live = SotcLive::Snapshot();
    const auto pending = Pending();
    bool relaunch_requested = false;
    int relaunch_boss = 0;
    {
        std::lock_guard<std::mutex> lock(State().mutex);
        relaunch_requested = State().relaunch_requested;
        relaunch_boss = State().relaunch_boss;
    }

    std::string out = "{";
    out += "\"pos\":[" + JsonNum(fc.player.x) + "," + JsonNum(fc.player.y) + "," +
           JsonNum(fc.player.z) + "]";
    // cam: x, y, z, yaw (radians), pitch (radians)
    out += ",\"cam\":[" + JsonNum(fc.cam.pos.x) + "," + JsonNum(fc.cam.pos.y) + "," +
           JsonNum(fc.cam.pos.z) + "," + JsonNum(fc.cam.yaw) + "," + JsonNum(fc.cam.pitch) + "]";
    out += ",\"freecam\":" + JsonBool(fc.active);
    out += ",\"autopilot\":{\"loaded\":" + JsonBool(ap.loaded) +
           ",\"playing\":" + JsonBool(ap.playing) +
           ",\"recording\":" + JsonBool(ap.recording) + ",\"loop\":" + JsonBool(ap.loop) +
           ",\"point\":" + std::to_string(ap.point) + ",\"count\":" + std::to_string(ap.count) +
           ",\"seg\":" + JsonStr(ap.seg) + ",\"lap\":" + std::to_string(ap.lap) +
           ",\"distance_done\":" + JsonNum(ap.distance_done) +
           ",\"distance_total\":" + JsonNum(ap.distance_total) + "}";
    out += ",\"area\":" + JsonStr(area);
    out += ",\"boss_pending\":" + JsonBool(boss_pending);
    out += ",\"segment\":" + JsonStr(w.segment);
    out += ",\"window\":" + JsonWindow(w);
    out += ",\"live\":" + JsonLive(live);
    out += ",\"pending\":" + JsonMapObject(pending);
    out += ",\"relaunch\":{\"requested\":" + JsonBool(relaunch_requested) +
           ",\"boss\":" + std::to_string(relaunch_boss) + "}";
    const SotcTrace::DumpInfo dump = SotcTrace::LastDump();
    out += ",\"trace\":{\"mode\":" + std::to_string(SotcTrace::kMode) +
           ",\"last_dump\":" + JsonStr(dump.path) + ",\"dumps\":" + std::to_string(dump.serial) +
           "}";
    return out + "}";
}

std::string CmdGet(const Args& a) {
    if (a.size() == 2 && a[1] == "state") {
        return JsonState();
    }
    if (a.size() == 2 && a[1] == "live") {
        return JsonLive(SotcLive::Snapshot());
    }
    return "err usage: get state|live";
}

// --- Dispatch ----------------------------------------------------------------------------------

} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SOTC_LAB");
        return v && v[0] != '\0' && std::strcmp(v, "0") != 0;
    }();
    return enabled;
}

std::string Execute(std::string_view line) {
    try {
        const Args a = Split(line);
        if (a.size() == 0) {
            return "err empty-line";
        }
        const std::string_view cmd = a[0];
        if (cmd == "ping") {
            return "ok pong";
        }
        if (cmd == "freecam") {
            return CmdFreecam(a);
        }
        if (cmd == "goto") {
            return CmdGoto(a);
        }
        if (cmd == "speed") {
            return CmdSpeed(a);
        }
        if (cmd == "path") {
            return CmdPath(a);
        }
        if (cmd == "boss") {
            return CmdBoss(a);
        }
        if (cmd == "engine") {
            return CmdEngine(a);
        }
        if (cmd == "set") {
            return CmdSet(a);
        }
        if (cmd == "pending") {
            return CmdPending(a);
        }
        if (cmd == "relaunch") {
            return CmdRelaunch(a);
        }
        if (cmd == "mark") {
            return CmdMark(a);
        }
        if (cmd == "trace") {
            return CmdTrace(a);
        }
        if (cmd == "get") {
            return CmdGet(a);
        }
        if (cmd == "attach") {
            g_attach_ticks.store(std::chrono::steady_clock::now().time_since_epoch().count());
            return "ok";
        }
        if (cmd == "quit") {
            return "ok"; // the server closes the connection after this reply
        }
        return "err unknown-command";
    } catch (...) {
        return "err internal";
    }
}

namespace {

// SOTC_LAB_ROUTE=<route file>: plays that route by itself once gameplay begins, then (after
// SOTC_LAB_ROUTE_EXIT seconds, default 5; negative keeps the game open) closes the game.
// SOTC_LAB_ROUTE_DELAY (seconds, default 8) is the wait after the last "starting game" line, so the
// area has loaded before the free camera takes over. Runs from OnFrame (the flip thread) and only
// queues work for the update thread through the module APIs.
enum class AutoPhase { Off, WaitGame, StartCam, WaitCam, Playing, Done, Closed };

struct AutoRoute {
    AutoPhase phase = AutoPhase::Off;
    std::string path;
    double delay_s = 8.0;
    double exit_s = 5.0;
    uint64_t starts_seen = 0;
    int64_t phase_ms = 0;
    bool was_playing = false;
};

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

double EnvSeconds(const char* name, double def) {
    const char* v = std::getenv(name);
    if (!v || !*v) {
        return def;
    }
    char* end = nullptr;
    const double d = std::strtod(v, &end);
    return end != v && std::isfinite(d) ? d : def;
}

void AutoRouteTick() {
    static AutoRoute a = [] {
        AutoRoute r;
        const char* v = std::getenv("SOTC_LAB_ROUTE");
        if (v && *v && Enabled()) {
            r.path = v;
            r.delay_s = EnvSeconds("SOTC_LAB_ROUTE_DELAY", 8.0);
            r.exit_s = EnvSeconds("SOTC_LAB_ROUTE_EXIT", 5.0);
            std::string err;
            if (SotcAutopilot::Load(r.path, &err)) {
                r.phase = AutoPhase::WaitGame;
                std::fprintf(stderr, "SOTC LAB ROUTE %s: waiting for gameplay\n", r.path.c_str());
            } else {
                std::fprintf(stderr, "SOTC LAB ROUTE %s: %s\n", r.path.c_str(), err.c_str());
            }
        }
        return r;
    }();
    if (a.phase == AutoPhase::Off || a.phase == AutoPhase::Closed) {
        return;
    }
    const int64_t now = NowMs();
    switch (a.phase) {
    case AutoPhase::WaitGame: {
        const uint64_t starts = SotcEngineCmd::GameStarts();
        const int64_t last = SotcEngineCmd::LastGameStartMs();
        if (starts > 0 && now - last >= static_cast<int64_t>(a.delay_s * 1000.0)) {
            a.phase = AutoPhase::StartCam;
        }
        break;
    }
    case AutoPhase::StartCam:
        if (SotcFreecam::SetActive(true)) {
            a.phase = AutoPhase::WaitCam;
            a.phase_ms = now;
        }
        break;
    case AutoPhase::WaitCam: {
        const SotcFreecam::State st = SotcFreecam::Get();
        if (st.active && st.frames > 0) {
            SotcAutopilot::Stop();
            SotcAutopilot::Play();
            Mark(SotcAutopilot::Current().name.empty() ? "route" : SotcAutopilot::Current().name);
            a.phase = AutoPhase::Playing;
            a.phase_ms = now;
            a.was_playing = false;
            std::fprintf(stderr, "SOTC LAB ROUTE start\n");
        } else if (!st.active && now - a.phase_ms > 3000) {
            a.phase = AutoPhase::StartCam; // the start pose was not ready yet: try again
        }
        break;
    }
    case AutoPhase::Playing: {
        const bool playing = SotcAutopilot::GetProgress().playing;
        if (playing) {
            a.was_playing = true;
        } else if (a.was_playing || now - a.phase_ms > 5000) {
            const Window w = CurrentWindow();
            std::fprintf(stderr,
                         "SOTC LAB ROUTE done: %llu frames, p50 %.1f ms, p99 %.1f ms, max %.1f ms, "
                         "hitches >33 ms %llu, >50 ms %llu, >100 ms %llu\n",
                         static_cast<unsigned long long>(w.frames), w.p50, w.p99, w.max,
                         static_cast<unsigned long long>(w.hitch33),
                         static_cast<unsigned long long>(w.hitch50),
                         static_cast<unsigned long long>(w.hitch100));
            a.phase = AutoPhase::Done;
            a.phase_ms = now;
        }
        break;
    }
    case AutoPhase::Done:
        if (a.exit_s < 0.0) {
            a.phase = AutoPhase::Closed;
        } else if (now - a.phase_ms >= static_cast<int64_t>(a.exit_s * 1000.0)) {
            std::fprintf(stderr, "SOTC LAB ROUTE exit\n");
            std::fflush(stderr);
            a.phase = AutoPhase::Closed;
            // The same path as closing the window.
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
        break;
    default:
        break;
    }
}

} // namespace

void OnFrame(double frame_ms) {
    AutoRouteTick();
    if (!std::isfinite(frame_ms)) {
        return;
    }
    FrameStats& s = Stats();
    std::lock_guard<std::mutex> lock(s.mutex);
    ++s.frames;
    s.max = std::max(s.max, frame_ms);
    if (frame_ms > 33.0) {
        ++s.hitch33;
    }
    if (frame_ms > 50.0) {
        ++s.hitch50;
    }
    if (frame_ms > 100.0) {
        ++s.hitch100;
    }
    if (s.samples.size() < kMaxFrameSamples) {
        s.samples.push_back(frame_ms);
    }
}

Window CurrentWindow() {
    FrameStats& s = Stats();
    std::vector<double> samples;
    Window w;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        w.segment = s.segment;
        w.frames = s.frames;
        w.max = s.max;
        w.hitch33 = s.hitch33;
        w.hitch50 = s.hitch50;
        w.hitch100 = s.hitch100;
        samples = s.samples;
    }
    w.p50 = Percentile(samples, 0.50);
    w.p99 = Percentile(samples, 0.99);
    return w;
}

void Mark(std::string_view segment) {
    FrameStats& s = Stats();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.segment = std::string(segment);
    s.frames = 0;
    s.hitch33 = s.hitch50 = s.hitch100 = 0;
    s.max = 0.0;
    s.samples.clear();
}

void SetArea(std::string_view area) {
    std::lock_guard<std::mutex> lock(State().mutex);
    State().area = std::string(area);
}

std::string Area() {
    std::lock_guard<std::mutex> lock(State().mutex);
    return State().area;
}

void SetPending(std::string_view name, std::string_view value) {
    std::lock_guard<std::mutex> lock(State().mutex);
    State().pending[std::string(name)] = std::string(value);
}

std::map<std::string, std::string> Pending() {
    std::lock_guard<std::mutex> lock(State().mutex);
    return State().pending;
}

void ClearPending() {
    std::lock_guard<std::mutex> lock(State().mutex);
    State().pending.clear();
}

bool WritePendingEnv(int boss, std::string* err) {
    const std::string body = "{\"env\":" + JsonMapObject(Pending()) +
                             ",\"boss\":" + std::to_string(boss) + "}\n";
    std::error_code ec;
    std::filesystem::create_directories("user", ec);
    if (ec) {
        if (err) {
            *err = "cannot create user/";
        }
        return false;
    }
    std::ofstream out(kPendingPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (err) {
            *err = std::string("cannot write ") + kPendingPath;
        }
        return false;
    }
    out << body;
    if (!out) {
        if (err) {
            *err = std::string("cannot write ") + kPendingPath;
        }
        return false;
    }
    return true;
}

void RequestRelaunch(int boss) {
    std::lock_guard<std::mutex> lock(State().mutex);
    State().relaunch_requested = true;
    State().relaunch_boss = boss;
}

bool OrchestratorAttached() {
    const int64_t ticks = g_attach_ticks.load();
    if (ticks == 0) {
        return false;
    }
    const auto attached = std::chrono::steady_clock::duration(ticks);
    return std::chrono::steady_clock::now().time_since_epoch() - attached <= kAttachTimeout;
}

bool StartMatrix(const std::string& matrix_path, std::string* err) {
#ifdef _WIN32
    const char* override_cmd = std::getenv("SOTC_LAB_ORCH_CMD");
    std::string cmd = (override_cmd && *override_cmd) ? override_cmd : "python tools/sotc_matrix.py";
    cmd += " --attach --matrix \"" + matrix_path + "\"";
    std::vector<char> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr,
                                   &si, &pi);
    if (!ok) {
        if (err) {
            *err = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
        }
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    (void)matrix_path;
    if (err) {
        *err = "the orchestrator can only be started on Windows";
    }
    return false;
#endif
}

} // namespace SotcLab
