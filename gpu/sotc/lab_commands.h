// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SotC Lab command layer, shared by the control pipe (ctl_server.cpp), the F2 menu
// (gpu/shim/sotc_lab_menu.cpp) and SOTC_SET_TRIGGER. One line in, one line out.
//
// Commands (case-sensitive, space separated; replies start with "ok", "err" or "{" for JSON):
//   ping                                  -> ok pong
//   freecam on|off                        -> ok | err <why>
//   goto x y z [yaw pitch]                -> ok
//   speed <m/s>                           -> ok
//   path load <file> | play | pause | stop | seek <point> | loop on|off | save <file>
//        | record <name> | waypoint [seg] | stoprecord
//   boss <1..16> [play|kill|outro] [nohorse]  -> ok | err <why> (err live-unavailable: relaunch)
//   engine <raw engine console line>      -> ok | err <why>
//   set NAME=value                        -> ok | err needs-restart | err bad-value
//   pending NAME=value                    -> ok (queued for the next launch, see below)
//   relaunch [boss N]                     -> ok (asks an attached orchestrator to relaunch)
//   mark <segment>                        -> ok (resets the stats window, names it)
//   get state                             -> {"pos":[..],"cam":[..],"freecam":bool,"autopilot":{..},
//                                             "area":"..","boss_pending":bool,"segment":"..",
//                                             "window":{"frames":n,"p50":ms,"p99":ms,"max":ms,
//                                             "hitch33":n,"hitch50":n,"hitch100":n},
//                                             "live":{"SOTC_X":v,..},"pending":{..},
//                                             "relaunch":{"requested":bool,"boss":n}}
//   get live                              -> {"SOTC_X":v,..}
//   quit                                  -> ok (closes the pipe connection only)
//
// Restart-only switches: "pending" collects them; WritePendingEnv() stores them in
// user/lab_pending_env.json ({"env": {"SOTC_X": "v"}, "boss": n}), which sotc.py and the launcher
// merge into the next launch once, then delete.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace SotcLab {

/// SOTC_LAB=1 (or any SotC Lab switch) is set.
bool Enabled();

/// Runs one command line and returns the reply line (no trailing newline). Any thread.
std::string Execute(std::string_view line);

/// Called once per presented frame (driver.cpp flip) with the frame time in ms.
void OnFrame(double frame_ms);

struct Window {
    std::string segment;
    uint64_t frames{};
    double p50{}, p99{}, max{};
    uint64_t hitch33{}, hitch50{}, hitch100{};
};
/// Current stats window (since the last mark).
Window CurrentWindow();
void Mark(std::string_view segment);

/// Name of the area last reported by the area probe (empty when unknown). Set by probe_area.cpp.
void SetArea(std::string_view area);
std::string Area();

// --- Restart-only switches ---------------------------------------------------------------------
void SetPending(std::string_view name, std::string_view value);
std::map<std::string, std::string> Pending();
void ClearPending();
/// Writes user/lab_pending_env.json; boss 0 = none. False with *err on failure.
bool WritePendingEnv(int boss, std::string* err = nullptr);

/// Marks a relaunch request (seen by an attached orchestrator through "get state").
void RequestRelaunch(int boss);
bool OrchestratorAttached(); ///< a pipe client said "attach" within the last 10 s

/// Starts tools/sotc_matrix.py --attach --matrix <path> as a child process (command from
/// SOTC_LAB_ORCH_CMD, default "python tools/sotc_matrix.py"). False with *err on failure.
bool StartMatrix(const std::string& matrix_path, std::string* err = nullptr);

} // namespace SotcLab
