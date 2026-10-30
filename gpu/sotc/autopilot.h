// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SotC Lab autopilot. Routes are waypoint lists flown at a constant speed along a centripetal
// Catmull-Rom spline (arc-length parametrised), advanced by the game's own dt so laps repeat.
// Pure host logic: free-cam (freecam.cpp) calls Step() on the update thread and applies the pose.
// Route file (tools/routes/<name>.json, user/routes/<name>.json):
//   { "name": "...", "speed": 20.0,
//     "start": { "boss": 3 } | { "goto": [x, y, z, yaw, pitch] } | {},
//     "points": [ { "p": [x, y, z], "yaw": 0.0, "pitch": 0.0, "seg": "bridge", "dwell_ms": 0 }, ... ] }
// "seg" names the segment that starts at that point (empty: same segment as before).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "freecam.h"

namespace SotcAutopilot {

struct Waypoint {
    SotcFreecam::Pose pose;
    std::string seg;
    uint32_t dwell_ms{};
};

struct Route {
    std::string name;
    float speed{20.0f};
    int start_boss{};           ///< 1..16, 0 none
    bool has_start_goto{};
    SotcFreecam::Pose start_goto;
    std::vector<Waypoint> points;
};

struct Progress {
    bool loaded{}, playing{}, recording{}, loop{};
    size_t point{}, count{};
    std::string seg;
    int lap{};
    float distance_done{}, distance_total{};
};

/// Loads a route file. Any thread (not while Step runs: guarded internally).
bool Load(const std::string& path, std::string* err = nullptr);
bool Save(const std::string& path, std::string* err = nullptr);
Route Current();
void SetRoute(const Route& route);

/// Recording: clears the route, then AddWaypoint appends the current free-cam pose.
void StartRecording(const std::string& name);
void AddWaypoint(const SotcFreecam::Pose& pose, const std::string& seg = {});
void StopRecording();

void Play();
void Pause();
void Stop();          ///< back to the first point
void Seek(size_t point);
void SetLoop(bool loop);
void SetSpeed(float metres_per_second);
Progress GetProgress();

/// Update thread, once per game frame: advances by dt seconds. Returns true and sets *pose while
/// playing. Prints "SOTC SEG <name> begin" / "end" and "SOTC LAP <n>" log lines at transitions.
bool Step(float dt, SotcFreecam::Pose* pose);

/// File names (without directory) of tools/routes/*.json and user/routes/*.json, with the
/// directory prefix "tools/routes/" or "user/routes/".
std::vector<std::string> ListRoutes();

} // namespace SotcAutopilot
