// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: noclip free camera for the SotC Lab (freecam.h, docs/sotc/LAB.md), built on the engine's own
// debug camera and console commands (engine_cmd.h, docs/re-stutters/engine-commands.md):
//   PrintCameraPos / printPlayerPos  read the start pose ("SetCameraPos x y z a b c" output lines)
//   toggleDebugCam 1|0               detaches the camera from the game camera
//   SetCameraPos x y z pitch yaw roll   (degrees) places it, one line per frame
//   SetPlayerPos x y z 0 yaw 0       carries Wander under the camera, so area streaming follows
// Every line runs on the update thread inside the console tick (EngineFrameLine, called by
// engine_cmd.cpp when its own queue has nothing due), so nothing here writes guest memory.
//
// Angles: the engine reports and takes (pitch, yaw, roll) in degrees; a roll of +-180 is the same
// orientation as (pitch + 180, 180 - yaw, 0). The camera looks along
// -(sin yaw cos pitch, -sin pitch, cos yaw cos pitch) (measured: the game camera looking at Wander).
// Our Pose keeps yaw and pitch in radians in that convention.

#include "freecam.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <SDL3/SDL.h>

#include "autopilot.h"
#include "engine_cmd.h"
#include "probe_cam.h"
#include "../shim/bbport_overlay.h"

namespace SotcFreecam {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = 180.0f / kPi;
constexpr float kDefaultSpeed = 15.0f;       // metres per second, before modifiers
constexpr float kDefaultMaxSpeed = 120.0f;   // SOTC_FREECAM_MAXSPEED default
constexpr float kFallbackDt = 1.0f / 30.0f;
constexpr float kMaxDt = 0.1f;               // a hitch does not fling the camera
constexpr float kLookSensitivity = 0.0025f;  // radians per mouse pixel
constexpr float kKeyLook = 1.6f;             // radians per second with the arrow keys
constexpr float kPadLook = 2.5f;             // radians per second at full right stick
constexpr float kStickDead = 0.15f;
constexpr int kStartTimeoutFrames = 120;     // waiting for the start pose lines

enum class Phase {
    Off,
    AskCamera,  // send PrintCameraPos
    AskPlayer,  // send printPlayerPos
    ReadPose,   // parse both, then toggleDebugCam 1
    Flying,
    Leaving,    // toggleDebugCam 0
    Restore,    // put Wander back where the flight started
};

struct Input {
    float mouse_dx = 0, mouse_dy = 0, wheel = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0, l2 = 0, r2 = 0;
    uint32_t buttons = 0;
};

std::mutex g_mutex; // everything below
State g_state;
Input g_input;
Phase g_phase = Phase::Off;
int g_phase_frames = 0;
bool g_goto_pending = false;
Pose g_goto;
bool g_keep_player = false;  // leave Wander at the camera when free-cam ends (G key)
Vec3 g_player_start;
float g_player_start_yaw = 0.0f;
bool g_carry = true;          // SOTC_FREECAM_CARRY (default 1): Wander follows the camera
float g_drop = 8.0f;          // SOTC_FREECAM_DROP: Wander this far under the camera (m)
uint64_t g_frame = 0;
int g_start_tries = 0;        // start pose requests in this activation

float EnvFloat(const char* name, float def) {
    const char* v = std::getenv(name);
    if (!v || !*v) {
        return def;
    }
    char* end = nullptr;
    const float f = std::strtof(v, &end);
    return end != v && std::isfinite(f) ? f : def;
}

bool EnvFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

float Wrap(float a) { // radians into (-pi, pi]
    while (a > kPi) a -= 2 * kPi;
    while (a <= -kPi) a += 2 * kPi;
    return a;
}

// Engine (pitch, yaw, roll) degrees -> our pose angles.
void FromEngine(float a, float b, float c, Pose* p) {
    if (std::fabs(c) > 90.0f) {
        a += 180.0f;
        b = 180.0f - b;
    }
    p->pitch = std::clamp(Wrap(a / kDeg), -1.55f, 1.55f);
    p->yaw = Wrap(b / kDeg);
}

// Parses the newest "<key> x y z a b c" output line (e.g. key "SetCameraPos").
bool NewestPose(const char* key, float v[6]) {
    const std::vector<std::string> out = SotcEngineCmd::RecentOutput();
    for (auto it = out.rbegin(); it != out.rend(); ++it) {
        const size_t at = it->find(key);
        if (at == std::string::npos || it->compare(0, 2, "> ") == 0) {
            continue; // our own echoed command lines start with "> "
        }
        if (std::sscanf(it->c_str() + at + std::strlen(key), "%f %f %f %f %f %f", &v[0], &v[1], &v[2],
                        &v[3], &v[4], &v[5]) == 6) {
            return true;
        }
    }
    return false;
}

bool MenuHasKeyboard() {
    return BbOverlay::CapturesInput();
}

// Speed multiplier from the modifiers held this frame.
float Boost(const bool* k) {
    float m = 1.0f;
    if (k && (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT])) m *= 4.0f;
    if (k && (k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL])) m *= 0.25f;
    return m;
}

// Advances g_state.cam by the input of this frame. Called with g_mutex held.
void Integrate(float dt) {
    const bool* k = SDL_WasInit(SDL_INIT_VIDEO) && !MenuHasKeyboard() ? SDL_GetKeyboardState(nullptr)
                                                                         : nullptr;
    Pose& p = g_state.cam;
    Input in = g_input;
    g_input.mouse_dx = g_input.mouse_dy = g_input.wheel = 0.0f;

    if (in.wheel != 0.0f) {
        g_state.speed = std::clamp(g_state.speed * std::pow(1.25f, in.wheel), 0.5f, MaxSpeed());
    }
    // Look: mouse right turns right (yaw down), mouse down looks down.
    p.yaw = Wrap(p.yaw - in.mouse_dx * kLookSensitivity);
    p.pitch -= in.mouse_dy * kLookSensitivity;
    const auto dead = [](float v) { return std::fabs(v) < kStickDead ? 0.0f : v; };
    p.yaw = Wrap(p.yaw - dead(in.rx) * kPadLook * dt);
    p.pitch -= dead(in.ry) * kPadLook * dt;
    if (k) {
        if (k[SDL_SCANCODE_LEFT]) p.yaw = Wrap(p.yaw + kKeyLook * dt);
        if (k[SDL_SCANCODE_RIGHT]) p.yaw = Wrap(p.yaw - kKeyLook * dt);
        if (k[SDL_SCANCODE_UP]) p.pitch += kKeyLook * dt;
        if (k[SDL_SCANCODE_DOWN]) p.pitch -= kKeyLook * dt;
    }
    p.pitch = std::clamp(p.pitch, -1.55f, 1.55f);

    // Move: forward along the view, right on the horizontal plane, up along world Y.
    float fwd = -dead(in.ly), side = dead(in.lx), up = in.r2 - in.l2;
    if (k) {
        fwd += (k[SDL_SCANCODE_W] ? 1.0f : 0.0f) - (k[SDL_SCANCODE_S] ? 1.0f : 0.0f);
        side += (k[SDL_SCANCODE_D] ? 1.0f : 0.0f) - (k[SDL_SCANCODE_A] ? 1.0f : 0.0f);
        up += (k[SDL_SCANCODE_E] ? 1.0f : 0.0f) - (k[SDL_SCANCODE_Q] ? 1.0f : 0.0f);
    }
    const float speed = std::min(g_state.speed * Boost(k), MaxSpeed());
    const float cp = std::cos(p.pitch), sp = std::sin(p.pitch);
    const float sy = std::sin(p.yaw), cy = std::cos(p.yaw);
    const Vec3 f{-sy * cp, sp, -cy * cp};
    const Vec3 r{-cy, 0.0f, sy};
    p.pos.x += (f.x * fwd + r.x * side) * speed * dt;
    p.pos.y += (f.y * fwd + up) * speed * dt;
    p.pos.z += (f.z * fwd + r.z * side) * speed * dt;
}

std::string CameraLine(const Pose& p) {
    char line[160];
    std::snprintf(line, sizeof line, "SetCameraPos %.3f %.3f %.3f %.3f %.3f 0", p.pos.x, p.pos.y,
                  p.pos.z, p.pitch * kDeg, p.yaw * kDeg);
    return line;
}

std::string PlayerLine(const Vec3& v, float yaw_deg) {
    char line[160];
    std::snprintf(line, sizeof line, "SetPlayerPos %.3f %.3f %.3f 0 %.3f 0", v.x, v.y, v.z, yaw_deg);
    return line;
}

} // namespace

void Register() {
    // The free camera needs no hook of its own (engine commands); the observe-only camera probe
    // stays available (SOTC_CAM_PROBE).
    SotcProbes::RegisterCamProbe();
    std::lock_guard lock(g_mutex);
    g_state.speed = std::clamp(EnvFloat("SOTC_FREECAM_SPEED", kDefaultSpeed), 0.5f, 1000.0f);
    g_carry = EnvFloat("SOTC_FREECAM_CARRY", 1.0f) != 0.0f;
    g_drop = EnvFloat("SOTC_FREECAM_DROP", 8.0f);
}

bool Enabled() {
    static const bool on = EnvFlag("SOTC_FREECAM") || EnvFlag("SOTC_LAB");
    return on && SotcEngineCmd::Available();
}

bool Active() {
    std::lock_guard lock(g_mutex);
    return g_phase != Phase::Off;
}

bool SetActive(bool on) {
    if (on && !Enabled()) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    if (on) {
        if (g_phase == Phase::Off || g_phase == Phase::Leaving || g_phase == Phase::Restore) {
            g_phase = Phase::AskCamera;
            g_phase_frames = 0;
            g_keep_player = false;
            g_start_tries = 0;
        }
    } else if (g_phase != Phase::Off && g_phase != Phase::Leaving && g_phase != Phase::Restore) {
        g_phase = g_phase == Phase::Flying ? Phase::Leaving : Phase::Off;
        g_phase_frames = 0;
    }
    return true;
}

void LeaveHere() {
    std::lock_guard lock(g_mutex);
    g_keep_player = true;
    if (g_phase == Phase::Flying) {
        g_phase = Phase::Leaving;
        g_phase_frames = 0;
    }
}

State Get() {
    std::lock_guard lock(g_mutex);
    State s = g_state;
    s.enabled = Enabled();
    s.active = g_phase != Phase::Off;
    s.autopilot = SotcAutopilot::GetProgress().playing;
    return s;
}

bool Goto(const Pose& pose) {
    if (!Enabled()) {
        return false;
    }
    {
        std::lock_guard lock(g_mutex);
        g_goto = pose;
        g_goto_pending = true;
    }
    return SetActive(true);
}

void SetSpeed(float metres_per_second) {
    std::lock_guard lock(g_mutex);
    g_state.speed = std::clamp(metres_per_second, 0.5f, MaxSpeed());
}

void FeedMouse(float dx, float dy, float wheel) {
    std::lock_guard lock(g_mutex);
    g_input.mouse_dx += dx;
    g_input.mouse_dy += dy;
    g_input.wheel += wheel;
}

void FeedPad(float lx, float ly, float rx, float ry, float l2, float r2, uint32_t buttons) {
    std::lock_guard lock(g_mutex);
    g_input.lx = lx;
    g_input.ly = ly;
    g_input.rx = rx;
    g_input.ry = ry;
    g_input.l2 = l2;
    g_input.r2 = r2;
    g_input.buttons = buttons;
}

float MaxSpeed() {
    static const float max = std::max(1.0f, EnvFloat("SOTC_FREECAM_MAXSPEED", kDefaultMaxSpeed));
    return max;
}

bool EngineFrameLine(float dt, std::string* line) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) {
        dt = kFallbackDt;
    }
    dt = std::min(dt, kMaxDt);
    std::lock_guard lock(g_mutex);
    ++g_frame;
    ++g_phase_frames;
    switch (g_phase) {
    case Phase::Off:
        return false;
    case Phase::AskCamera:
        *line = "PrintCameraPos";
        g_phase = Phase::AskPlayer;
        g_phase_frames = 0;
        return true;
    case Phase::AskPlayer:
        *line = "printPlayerPos";
        g_phase = Phase::ReadPose;
        g_phase_frames = 0;
        return true;
    case Phase::ReadPose: {
        float cam[6], player[6];
        const bool have_cam = NewestPose("Camera Position: SetCameraPos", cam);
        const bool have_player = NewestPose("Player Position: SetPlayerPos", player);
        if (!have_cam || !have_player) {
            if (g_phase_frames > kStartTimeoutFrames) {
                // The game camera or Wander may not exist yet (loading): ask again, a few times.
                if (++g_start_tries >= 4) {
                    std::fprintf(stderr, "SOTC FREECAM: no start pose from the engine, staying off\n");
                    g_phase = Phase::Off;
                } else {
                    g_phase = Phase::AskCamera;
                }
                g_phase_frames = 0;
            }
            return false;
        }
        g_state.cam.pos = {cam[0], cam[1], cam[2]};
        FromEngine(cam[3], cam[4], cam[5], &g_state.cam);
        g_player_start = {player[0], player[1], player[2]};
        g_player_start_yaw = player[4];
        g_state.player = g_player_start;
        g_state.player_known = true;
        g_input = {};
        *line = "toggleDebugCam 1";
        g_phase = Phase::Flying;
        g_phase_frames = 0;
        std::fprintf(stderr, "SOTC FREECAM on at %.2f %.2f %.2f\n", cam[0], cam[1], cam[2]);
        return true;
    }
    case Phase::Flying: {
        if (g_goto_pending) {
            g_state.cam = g_goto;
            g_goto_pending = false;
        }
        SotcFreecam::Pose ap;
        if (SotcAutopilot::Step(dt, &ap)) {
            g_state.cam = ap;
        } else {
            Integrate(dt);
        }
        ++g_state.frames;
        // Camera on even frames; Wander under it on odd ones (one console line per frame).
        if (g_carry && (g_frame & 1)) {
            g_state.player = {g_state.cam.pos.x, g_state.cam.pos.y - g_drop, g_state.cam.pos.z};
            *line = PlayerLine(g_state.player, g_state.cam.yaw * kDeg);
        } else {
            *line = CameraLine(g_state.cam);
        }
        return true;
    }
    case Phase::Leaving:
        *line = "toggleDebugCam 0";
        g_phase = Phase::Restore;
        g_phase_frames = 0;
        return true;
    case Phase::Restore:
        g_phase = Phase::Off;
        if (g_carry) {
            const Vec3 at = g_keep_player ? g_state.cam.pos : g_player_start;
            *line = PlayerLine(at, g_player_start_yaw);
            g_state.player = at;
            std::fprintf(stderr, "SOTC FREECAM off, Wander at %.2f %.2f %.2f\n", at.x, at.y, at.z);
            return true;
        }
        return false;
    }
    return false;
}

} // namespace SotcFreecam

extern "C" int bbgpu_freecam_captures_input(void) {
    return SotcFreecam::Active() ? 1 : 0;
}

extern "C" void bbgpu_freecam_feed_pad(float lx, float ly, float rx, float ry, float l2, float r2,
                                      uint32_t buttons) {
    SotcFreecam::FeedPad(lx, ly, rx, ry, l2, r2, buttons);
}
