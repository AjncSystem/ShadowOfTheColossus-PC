// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SotC Lab autopilot (gpu/sotc/autopilot.h). Route state, a centripetal Catmull-Rom spline
// with an arc-length table per segment, constant-speed Step(). One mutex guards all state: Step runs
// on the game update thread, the rest on UI and pipe threads. Step never calls into freecam.
// Route JSON uses mini_json.h; angles in the route file are radians, like SotcFreecam::Pose.

#include "autopilot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "freecam.h"
#include "mini_json.h"
#include "video_core/sotc_trace.h" // sotc: SOTC_TRACE segment and lap marks

namespace SotcAutopilot {
namespace {

using SotcFreecam::Pose;
using SotcFreecam::Vec3;
using SotcJson::Value;

constexpr size_t kSamples = 32;      // arc-length samples per segment
constexpr double kMinKnot = 1e-4;    // floor of a centripetal knot step (coincident points)
constexpr double kMinSpeed = 0.1;    // metres per second
constexpr double kPi = 3.14159265358979323846;

struct P3 {
    double x{}, y{}, z{};
};

struct Segment {
    double length{};
    std::array<double, kSamples + 1> cum{};  // arc length at parameter k / kSamples
};

std::mutex g_mutex;
Route g_route;
std::vector<Segment> g_segs;  // size n-1 when n >= 2
double g_total{};             // total arc length of the route
bool g_playing{};
bool g_recording{};
bool g_loop{};
bool g_announced{};           // the current segment name was announced since the last reset
size_t g_seg{};              // segment index; n-1 only when parked on the last point
double g_dist{};             // metres into segment g_seg
size_t g_at{};               // last point reached
double g_dwell_left{};       // seconds still to hold at g_at
int g_lap{};
std::string g_seg_name;      // name of the current segment ("" before any named point)

P3 ToP3(const Vec3& v) { return P3{v.x, v.y, v.z}; }

Vec3 ToVec3(const P3& p) {
    Vec3 v;
    v.x = float(p.x);
    v.y = float(p.y);
    v.z = float(p.z);
    return v;
}

P3 Mix(const P3& a, const P3& b, double wa, double wb) {
    return P3{wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z};
}

double Dist(const P3& a, const P3& b) {
    const double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Centripetal knot step: |b - a|^alpha with alpha = 0.5, floored so coincident points stay finite.
double Knot(const P3& a, const P3& b) { return std::max(std::sqrt(Dist(a, b)), kMinKnot); }

// Barry-Goldman evaluation of the centripetal Catmull-Rom segment p1 -> p2, s in [0, 1].
P3 CatmullRom(const P3& p0, const P3& p1, const P3& p2, const P3& p3, double s) {
    const double t0 = 0.0;
    const double t1 = t0 + Knot(p0, p1);
    const double t2 = t1 + Knot(p1, p2);
    const double t3 = t2 + Knot(p2, p3);
    const double t = t1 + s * (t2 - t1);
    const P3 a1 = Mix(p0, p1, (t1 - t) / (t1 - t0), (t - t0) / (t1 - t0));
    const P3 a2 = Mix(p1, p2, (t2 - t) / (t2 - t1), (t - t1) / (t2 - t1));
    const P3 a3 = Mix(p2, p3, (t3 - t) / (t3 - t2), (t - t2) / (t3 - t2));
    const P3 b1 = Mix(a1, a2, (t2 - t) / (t2 - t0), (t - t0) / (t2 - t0));
    const P3 b2 = Mix(a2, a3, (t3 - t) / (t3 - t1), (t - t1) / (t3 - t1));
    return Mix(b1, b2, (t2 - t) / (t2 - t1), (t - t1) / (t2 - t1));
}

// Position on segment i (points i -> i+1) at spline parameter s. The first and last segments use
// their end point twice as the missing neighbour.
P3 SegmentPos(const Route& r, size_t i, double s) {
    const size_t n = r.points.size();
    const size_t i0 = i > 0 ? i - 1 : i;
    const size_t i3 = i + 2 < n ? i + 2 : i + 1;
    return CatmullRom(ToP3(r.points[i0].pose.pos), ToP3(r.points[i].pose.pos),
                      ToP3(r.points[i + 1].pose.pos), ToP3(r.points[i3].pose.pos), s);
}

// Shortest-way interpolation between two angles in radians.
double AngleLerp(double a, double b, double f) { return a + std::remainder(b - a, 2.0 * kPi) * f; }

Pose SegmentPose(const Route& r, size_t i, double s) {
    const Waypoint& a = r.points[i];
    const Waypoint& b = r.points[i + 1];
    Pose out;
    out.pos = ToVec3(SegmentPos(r, i, s));
    out.yaw = float(AngleLerp(a.pose.yaw, b.pose.yaw, s));
    out.pitch = float(a.pose.pitch + (b.pose.pitch - a.pose.pitch) * s);
    return out;
}

// Spline parameter for a distance along one segment (inverse of the arc-length table).
double ParamAt(const Segment& seg, double d) {
    if (seg.length <= 0.0 || d <= 0.0) return 0.0;
    if (d >= seg.length) return 1.0;
    const auto it = std::upper_bound(seg.cum.begin(), seg.cum.end(), d);
    const size_t k = size_t(it - seg.cum.begin());  // cum[k-1] <= d < cum[k], 1 <= k <= kSamples
    const double lo = seg.cum[k - 1];
    const double hi = seg.cum[k];
    const double f = hi > lo ? (d - lo) / (hi - lo) : 0.0;
    return (double(k - 1) + f) / double(kSamples);
}

void RebuildLocked() {
    g_segs.clear();
    g_total = 0.0;
    const size_t n = g_route.points.size();
    if (n < 2) return;
    g_segs.resize(n - 1);
    for (size_t i = 0; i + 1 < n; ++i) {
        Segment& seg = g_segs[i];
        seg.cum[0] = 0.0;
        P3 prev = SegmentPos(g_route, i, 0.0);
        for (size_t k = 1; k <= kSamples; ++k) {
            const P3 cur = SegmentPos(g_route, i, double(k) / double(kSamples));
            seg.cum[k] = seg.cum[k - 1] + Dist(prev, cur);
            prev = cur;
        }
        seg.length = seg.cum[kSamples];
        g_total += seg.length;
    }
}

// Name in effect at point p: its own seg, else the last named point before it.
std::string EffectiveSegLocked(size_t p) {
    const auto& pts = g_route.points;
    if (pts.empty()) return {};
    if (p >= pts.size()) p = pts.size() - 1;
    for (size_t i = p + 1; i-- > 0;) {
        if (!pts[i].seg.empty()) return pts[i].seg;
    }
    return {};
}

// Parks the player on point p without dwell and without announcements.
void ResetLocked(size_t p) {
    g_at = p;
    g_seg = p;
    g_dist = 0.0;
    g_dwell_left = 0.0;
    g_seg_name = EffectiveSegLocked(p);
    g_announced = false;
}

Pose PoseLocked() {
    const size_t n = g_route.points.size();
    if (n == 0) return Pose{};
    if (g_seg + 1 >= n) return g_route.points[n - 1].pose;
    return SegmentPose(g_route, g_seg, ParamAt(g_segs[g_seg], g_dist));
}

void AnnounceLocked(const char* what, const std::string& name) {
    std::fprintf(stderr, "SOTC SEG %s %s\n", name.c_str(), what);
    std::fflush(stderr);
    if (SotcTrace::kOn) { // sotc: the segment is stored with every following frame
        const bool begin = std::string_view(what) == "begin";
        const std::string text = "SEG " + name + " " + what;
        SotcTrace::Mark(text.c_str());
        SotcTrace::SetSegment(begin ? name.c_str() : "", static_cast<uint32_t>(g_lap));
    }
}

// Passing point p: a named point starts a new segment when its name differs from the current one.
void PassPointLocked(size_t p) {
    const std::string& name = g_route.points[p].seg;
    if (name.empty() || name == g_seg_name) return;
    if (!g_seg_name.empty()) AnnounceLocked("end", g_seg_name);
    AnnounceLocked("begin", name);
    g_seg_name = name;
}

void EndSegmentLocked() {
    if (!g_seg_name.empty()) AnnounceLocked("end", g_seg_name);
    g_seg_name.clear();
}

// Reaches point p. True when the player holds there for the point's dwell.
bool ArriveLocked(size_t p) {
    g_at = p;
    PassPointLocked(p);
    const double hold = double(g_route.points[p].dwell_ms) / 1000.0;
    if (hold > 0.0) {
        g_dwell_left = hold;
        g_seg = p;
        g_dist = 0.0;
        return true;
    }
    return false;
}

// Leaves point g_at (reached, not holding): onward along the route, a loop wrap, or the route end.
void DepartLocked() {
    const size_t n = g_route.points.size();
    if (g_at + 1 < n) {
        g_seg = g_at;
        g_dist = 0.0;
        return;
    }
    if (g_loop) {
        EndSegmentLocked();
        ++g_lap;
        std::fprintf(stderr, "SOTC LAP %d\n", g_lap);
        std::fflush(stderr);
        if (SotcTrace::kOn) { // sotc: lap mark; EndSegmentLocked above already cleared the segment
            const std::string text = "LAP " + std::to_string(g_lap);
            SotcTrace::Mark(text.c_str());
            SotcTrace::SetSegment("", static_cast<uint32_t>(g_lap));
        }
        g_at = 0;
        g_seg = 0;
        g_dist = 0.0;
        if (!ArriveLocked(0)) DepartLocked();
        return;
    }
    EndSegmentLocked();
    g_playing = false;
    g_seg = n - 1;
    g_dist = 0.0;
}

bool SetError(std::string* err, const std::string& message) {
    if (err) *err = message;
    return false;
}

// Reads an optional finite number member. 0: absent, 1: read, -1: present but not a finite number.
int ReadNumber(const Value& obj, const char* key, double* out) {
    const Value* v = obj.Get(key);
    if (!v) return 0;
    if (!v->IsNumber() || !std::isfinite(v->number)) return -1;
    *out = v->number;
    return 1;
}

// Reads an array of exactly `count` finite numbers.
bool ReadNumbers(const Value& arr, size_t count, double* out) {
    if (!arr.IsArray() || arr.items.size() != count) return false;
    for (size_t i = 0; i < count; ++i) {
        const Value& v = arr.items[i];
        if (!v.IsNumber() || !std::isfinite(v.number)) return false;
        out[i] = v.number;
    }
    return true;
}

Value Numbers(std::initializer_list<double> values) {
    Value arr = Value::MakeArray();
    for (const double v : values) arr.Push(Value::MakeNumber(v));
    return arr;
}

bool RouteFromJson(const Value& root, const std::string& default_name, Route* out,
                   std::string* err) {
    if (!root.IsObject()) return SetError(err, "the route must be a JSON object");
    Route r;
    r.name = default_name;
    if (const Value* v = root.Get("name")) {
        if (!v->IsString()) return SetError(err, "name must be a string");
        r.name = v->str;
    }
    double number = 0.0;
    int got = ReadNumber(root, "speed", &number);
    if (got < 0 || (got > 0 && number <= 0.0)) return SetError(err, "speed must be a positive number");
    if (got > 0) r.speed = float(number);

    if (const Value* start = root.Get("start")) {
        if (!start->IsObject()) return SetError(err, "start must be an object");
        got = ReadNumber(*start, "boss", &number);
        if (got < 0 || (got > 0 && (number < 0.0 || number > 16.0))) {
            return SetError(err, "start.boss must be 1..16");
        }
        if (got > 0) r.start_boss = int(number);
        if (const Value* g = start->Get("goto")) {
            double v[5] = {};
            if (!ReadNumbers(*g, 5, v)) return SetError(err, "start.goto must be [x, y, z, yaw, pitch]");
            r.has_start_goto = true;
            r.start_goto.pos.x = float(v[0]);
            r.start_goto.pos.y = float(v[1]);
            r.start_goto.pos.z = float(v[2]);
            r.start_goto.yaw = float(v[3]);
            r.start_goto.pitch = float(v[4]);
        }
    }

    const Value* points = root.Get("points");
    if (!points || !points->IsArray()) return SetError(err, "points must be an array");
    for (size_t i = 0; i < points->items.size(); ++i) {
        const Value& pv = points->items[i];
        const std::string where = "points[" + std::to_string(i) + "]";
        if (!pv.IsObject()) return SetError(err, where + " must be an object");
        const Value* p = pv.Get("p");
        double xyz[3] = {};
        if (!p || !ReadNumbers(*p, 3, xyz)) return SetError(err, where + ".p must be [x, y, z]");
        Waypoint w;
        w.pose.pos.x = float(xyz[0]);
        w.pose.pos.y = float(xyz[1]);
        w.pose.pos.z = float(xyz[2]);
        double angle = 0.0;
        if (ReadNumber(pv, "yaw", &angle) < 0) return SetError(err, where + ".yaw must be a number");
        w.pose.yaw = float(angle);
        angle = 0.0;
        if (ReadNumber(pv, "pitch", &angle) < 0) return SetError(err, where + ".pitch must be a number");
        w.pose.pitch = float(angle);
        if (const Value* sv = pv.Get("seg")) {
            if (!sv->IsString()) return SetError(err, where + ".seg must be a string");
            w.seg = sv->str;
        }
        got = ReadNumber(pv, "dwell_ms", &number);
        if (got < 0 || (got > 0 && number < 0.0)) return SetError(err, where + ".dwell_ms must be >= 0");
        if (got > 0) w.dwell_ms = uint32_t(number);
        r.points.push_back(w);
    }
    *out = r;
    return true;
}

} // namespace

bool Load(const std::string& path, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return SetError(err, "cannot open " + path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    Value root;
    std::string json_err;
    if (!SotcJson::Parse(buffer.str(), &root, &json_err)) return SetError(err, path + ": " + json_err);
    Route route;
    const std::string stem = std::filesystem::path(path).stem().string();
    if (!RouteFromJson(root, stem, &route, &json_err)) return SetError(err, path + ": " + json_err);
    SetRoute(route);
    return true;
}

bool Save(const std::string& path, std::string* err) {
    const Route r = Current();
    Value root = Value::MakeObject();
    root.Set("name", Value::MakeString(r.name));
    root.Set("speed", Value::MakeNumber(r.speed));

    Value start = Value::MakeObject();
    if (r.start_boss > 0) start.Set("boss", Value::MakeNumber(r.start_boss));
    if (r.has_start_goto) {
        const Pose& g = r.start_goto;
        start.Set("goto", Numbers({g.pos.x, g.pos.y, g.pos.z, g.yaw, g.pitch}));
    }
    root.Set("start", start);

    Value points = Value::MakeArray();
    for (const Waypoint& w : r.points) {
        Value o = Value::MakeObject();
        o.Set("p", Numbers({w.pose.pos.x, w.pose.pos.y, w.pose.pos.z}));
        o.Set("yaw", Value::MakeNumber(w.pose.yaw));
        o.Set("pitch", Value::MakeNumber(w.pose.pitch));
        if (!w.seg.empty()) o.Set("seg", Value::MakeString(w.seg));
        if (w.dwell_ms > 0) o.Set("dwell_ms", Value::MakeNumber(double(w.dwell_ms)));
        points.Push(o);
    }
    root.Set("points", points);

    const std::filesystem::path file(path);
    if (file.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        if (ec) return SetError(err, "cannot create the directory of " + path);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return SetError(err, "cannot write " + path);
    const std::string text = SotcJson::Dump(root) + "\n";
    out.write(text.data(), std::streamsize(text.size()));
    out.flush();
    if (!out) return SetError(err, "cannot write " + path);
    return true;
}

Route Current() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_route;
}

void SetRoute(const Route& route) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_route = route;
    RebuildLocked();
    g_playing = false;
    g_recording = false;
    g_lap = 0;
    ResetLocked(0);
}

void StartRecording(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const float speed = g_route.speed;
    g_route = Route{};
    g_route.name = name;
    g_route.speed = speed;
    RebuildLocked();
    g_recording = true;
    g_playing = false;
    g_lap = 0;
    ResetLocked(0);
}

void AddWaypoint(const Pose& pose, const std::string& seg) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const size_t before = g_route.points.size();
    Waypoint w;
    w.pose = pose;
    w.seg = seg;
    g_route.points.push_back(w);
    RebuildLocked();
    // Appending to a route that was parked at its end starts it over from the first point.
    if (g_seg + 1 >= before) ResetLocked(0);
}

void StopRecording() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_recording = false;
}

void Play() {
    std::lock_guard<std::mutex> lock(g_mutex);
    const size_t n = g_route.points.size();
    if (n < 2) {
        g_playing = false;
        return;
    }
    // Parked on the last point (route finished or seeked there): play again from the first point.
    if (g_seg + 1 >= n) ResetLocked(0);
    g_playing = true;
    if (!g_announced && !g_seg_name.empty()) {
        AnnounceLocked("begin", g_seg_name);
        g_announced = true;
    }
}

void Pause() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_playing = false;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_playing = false;
    g_lap = 0;
    ResetLocked(0);
}

void Seek(size_t point) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const size_t n = g_route.points.size();
    if (n == 0) return;
    ResetLocked(std::min(point, n - 1));
}

void SetLoop(bool loop) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_loop = loop;
}

void SetSpeed(float metres_per_second) {
    // Read the limit before taking our lock: freecam must never wait on the autopilot lock.
    const double limit = double(SotcFreecam::MaxSpeed());
    if (!std::isfinite(double(metres_per_second))) return;
    const double v = std::max(kMinSpeed, std::min(double(metres_per_second), limit));
    std::lock_guard<std::mutex> lock(g_mutex);
    g_route.speed = float(v);
}

Progress GetProgress() {
    std::lock_guard<std::mutex> lock(g_mutex);
    Progress p;
    const size_t n = g_route.points.size();
    p.loaded = n > 0;
    p.playing = g_playing;
    p.recording = g_recording;
    p.loop = g_loop;
    p.point = g_at;
    p.count = n;
    p.seg = g_seg_name;
    p.lap = g_lap;
    double done = 0.0;
    for (size_t i = 0; i < g_segs.size() && i < g_seg; ++i) done += g_segs[i].length;
    if (g_seg < g_segs.size()) done += g_dist;
    p.distance_done = float(done);
    p.distance_total = float(g_total);
    return p;
}

bool Step(float dt, Pose* pose) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const size_t n = g_route.points.size();
    if (!g_playing || n < 2 || pose == nullptr) return false;
    double left = dt > 0.0f ? double(dt) : 0.0;

    if (g_dwell_left > 0.0) {
        if (left < g_dwell_left) {
            g_dwell_left -= left;
            *pose = PoseLocked();
            return true;
        }
        left -= g_dwell_left;
        g_dwell_left = 0.0;
        DepartLocked();
    } else if (g_seg + 1 >= n) {
        DepartLocked();  // parked on the last point (Seek): leave it
    }
    if (!g_playing || g_dwell_left > 0.0) {
        *pose = PoseLocked();
        return true;
    }
    if (g_total <= 1e-9) {  // every point coincides: nothing to fly
        *pose = PoseLocked();
        return true;
    }

    double rem = left * double(g_route.speed);
    for (;;) {
        const double seg_len = g_segs[g_seg].length;
        const double avail = seg_len - g_dist;
        if (rem < avail) {
            g_dist += rem;
            break;
        }
        rem -= avail;
        g_dist = seg_len;
        if (ArriveLocked(g_seg + 1)) break;  // hold at the point for its dwell
        DepartLocked();
        if (!g_playing || g_dwell_left > 0.0) break;  // route ended, or a loop wrap holds at point 0
    }
    *pose = PoseLocked();
    return true;
}

std::vector<std::string> ListRoutes() {
    std::vector<std::string> out;
    const char* dirs[] = {"tools/routes", "user/routes"};
    for (const char* dir : dirs) {
        std::error_code ec;
        std::filesystem::directory_iterator it{std::filesystem::path(dir), ec};
        if (ec) continue;
        const std::filesystem::directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            const std::filesystem::path& p = it->path();
            if (p.extension().string() != ".json") continue;
            std::error_code file_ec;
            if (!it->is_regular_file(file_ec)) continue;
            out.push_back(std::string(dir) + "/" + p.filename().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace SotcAutopilot
