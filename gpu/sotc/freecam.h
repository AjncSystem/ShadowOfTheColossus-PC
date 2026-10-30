// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: noclip free camera for the SotC Lab (docs/sotc/LAB.md). Switch SOTC_FREECAM (set by
// SOTC_LAB=1). While active the engine's debug camera flies freely and carries Wander along, so area
// streaming follows the camera as in real play. Driven entirely by engine console commands
// (engine_cmd.h): toggleDebugCam, SetCameraPos, SetPlayerPos; see freecam.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace SotcFreecam {

struct Vec3 {
    float x{}, y{}, z{};
};

struct Pose {
    Vec3 pos;      ///< world (engine) coordinates
    float yaw{};   ///< radians, 0 = engine +Z, positive turns left
    float pitch{}; ///< radians, positive looks up
};

struct State {
    bool enabled{};       ///< switch on and every required hook installed
    bool active{};        ///< free-cam currently flying
    bool player_known{};  ///< the player position was read this session
    bool autopilot{};     ///< SotcAutopilot drives the pose
    Pose cam;             ///< the free-cam pose (or the game camera's when inactive)
    Vec3 player;          ///< last read player position
    float speed{};        ///< base speed, metres per second
    uint64_t frames{};    ///< update-thread frames seen by the hooks
};

/// Registers the handlers of FC_CAM, FC_WANDER (and the observe-only CAM_PROBE_* hooks).
/// Called from RegisterAllHandlers().
void Register();

bool Enabled();
bool Active();
/// Turns free-cam on/off. On: starts at the current camera pose. Any thread. False when not
/// Enabled() or while a loading screen is up.
bool SetActive(bool on);
State Get();

/// Moves the free camera (and Wander) to a pose; turns free-cam on. Any thread; applied on the
/// next update-thread frame.
bool Goto(const Pose& pose);
void SetSpeed(float metres_per_second);

// --- Input (fed by the integration code; any thread) -------------------------------------------
/// Mouse motion (pixels) and wheel steps from SDL events while active.
void FeedMouse(float dx, float dy, float wheel);
/// Gamepad sticks (-1..1), triggers (0..1) and the PS4 button bits of runtime_pad.c, sampled by
/// the pad code while free-cam captures input.
void FeedPad(float lx, float ly, float rx, float ry, float l2, float r2, uint32_t buttons);

/// Speed above which area streaming is known not to keep up (SOTC_FREECAM_MAXSPEED, m/s).
float MaxSpeed();

/// Ends free-cam leaving Wander at the camera position (he falls to the ground), instead of putting
/// him back where the flight started. Any thread.
void LeaveHere();

/// Update thread, from the engine console tick (engine_cmd.cpp) when no queued command is due:
/// advances the free camera by dt seconds and returns the one console line to run this frame.
bool EngineFrameLine(float dt, std::string* line);

} // namespace SotcFreecam

extern "C" {
/// Non-zero while free-cam is active: runtime_pad.c then gives the game a neutral pad.
int bbgpu_freecam_captures_input(void);
/// Called by runtime_pad.c with the host pad state while free-cam captures input.
void bbgpu_freecam_feed_pad(float lx, float ly, float rx, float ry, float l2, float r2,
                            uint32_t buttons);
}
