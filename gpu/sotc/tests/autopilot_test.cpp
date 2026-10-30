// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: autopilot-test. Standalone check of the SotC Lab autopilot (gpu/sotc/autopilot.cpp) and the
// route JSON reader/writer (gpu/sotc/mini_json.h). Not part of the CMake build: compile it with
// autopilot.cpp only, e.g. clang++ -std=c++17 -I gpu/sotc gpu/sotc/tests/autopilot_test.cpp
// gpu/sotc/autopilot.cpp -o autopilot_test, and run it from a scratch directory (it writes
// autopilot_test_out/). Prints PASS or FAIL; the exit code is 0 on PASS.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "../autopilot.h"
#include "../mini_json.h"

// Stub: the real limit lives in freecam.cpp, which this test does not link.
namespace SotcFreecam {
float MaxSpeed() { return 100.0f; }
}

namespace {

int g_failures = 0;

#define EXPECT(cond)                                              \
    do {                                                          \
        if (!(cond)) {                                            \
            ++g_failures;                                         \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);   \
        }                                                         \
    } while (0)

bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

bool NearPos(const SotcFreecam::Vec3& a, const SotcFreecam::Vec3& b, float tol) {
    return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.z, b.z, tol);
}

SotcFreecam::Pose MakePose(float x, float y, float z, float yaw = 0.0f, float pitch = 0.0f) {
    SotcFreecam::Pose p;
    p.pos.x = x;
    p.pos.y = y;
    p.pos.z = z;
    p.yaw = yaw;
    p.pitch = pitch;
    return p;
}

SotcAutopilot::Waypoint MakeWaypoint(const SotcFreecam::Pose& pose, const std::string& seg = {},
                                     uint32_t dwell_ms = 0) {
    SotcAutopilot::Waypoint w;
    w.pose = pose;
    w.seg = seg;
    w.dwell_ms = dwell_ms;
    return w;
}

void TestSplineHitsPoints() {
    SotcAutopilot::Route r;
    r.name = "spline";
    r.speed = 10.0f;
    r.points = {MakeWaypoint(MakePose(0, 0, 0)), MakeWaypoint(MakePose(0, 0, 10)),
                MakeWaypoint(MakePose(10, 0, 20)), MakeWaypoint(MakePose(20, 5, 20))};
    SotcAutopilot::SetRoute(r);
    for (size_t i = 0; i + 1 < r.points.size(); ++i) {
        SotcAutopilot::Seek(i);
        SotcAutopilot::Play();
        SotcFreecam::Pose pose;
        EXPECT(SotcAutopilot::Step(0.0f, &pose));
        EXPECT(NearPos(pose.pos, r.points[i].pose.pos, 1e-3f));
        SotcAutopilot::Pause();
    }
    // The last point is reached by the spline too (end of the last segment).
    SotcAutopilot::Seek(2);
    SotcAutopilot::Play();
    SotcFreecam::Pose pose;
    EXPECT(SotcAutopilot::Step(100.0f, &pose));  // far past the end: non-loop route stops there
    EXPECT(NearPos(pose.pos, r.points[3].pose.pos, 1e-3f));
    EXPECT(!SotcAutopilot::GetProgress().playing);
}

void TestConstantSpeed() {
    SotcAutopilot::Route r;
    r.speed = 10.0f;
    r.points = {MakeWaypoint(MakePose(0, 0, 0)), MakeWaypoint(MakePose(0, 0, 10), "a"),
                MakeWaypoint(MakePose(10, 0, 20)), MakeWaypoint(MakePose(20, 0, 20))};
    SotcAutopilot::SetRoute(r);
    SotcAutopilot::Play();
    float before = SotcAutopilot::GetProgress().distance_done;
    for (int i = 0; i < 10; ++i) {
        SotcFreecam::Pose pose;
        EXPECT(SotcAutopilot::Step(0.1f, &pose));
        const float after = SotcAutopilot::GetProgress().distance_done;
        EXPECT(Near(after - before, 1.0f, 1e-3f));  // 10 m/s for 0.1 s
        before = after;
    }
    SotcAutopilot::Stop();
}

void TestDwellThenFinish() {
    SotcAutopilot::Route r;
    r.speed = 10.0f;
    r.points = {MakeWaypoint(MakePose(0, 0, 0)), MakeWaypoint(MakePose(0, 0, 10), {}, 500)};
    SotcAutopilot::SetRoute(r);
    SotcAutopilot::SetLoop(false);
    SotcAutopilot::Play();
    SotcFreecam::Pose pose;
    EXPECT(SotcAutopilot::Step(100.0f, &pose));  // arrives at point 1 and holds
    EXPECT(SotcAutopilot::GetProgress().playing);
    EXPECT(SotcAutopilot::GetProgress().point == 1);
    EXPECT(SotcAutopilot::Step(0.2f, &pose));
    EXPECT(NearPos(pose.pos, r.points[1].pose.pos, 1e-3f));
    EXPECT(SotcAutopilot::GetProgress().playing);
    EXPECT(SotcAutopilot::Step(0.4f, &pose));  // dwell over: the route ends
    EXPECT(!SotcAutopilot::GetProgress().playing);
    EXPECT(!SotcAutopilot::Step(0.1f, &pose));
}

void TestLoopLaps() {
    SotcAutopilot::Route r;
    r.speed = 10.0f;
    r.points = {MakeWaypoint(MakePose(0, 0, 0)), MakeWaypoint(MakePose(0, 0, 10))};
    SotcAutopilot::SetRoute(r);
    SotcAutopilot::SetLoop(true);
    SotcAutopilot::Play();
    SotcFreecam::Pose pose;
    EXPECT(SotcAutopilot::Step(5.0f, &pose));  // 50 m on a 20 m loop
    const SotcAutopilot::Progress p = SotcAutopilot::GetProgress();
    EXPECT(p.playing);
    EXPECT(p.lap >= 2);
    SotcAutopilot::SetLoop(false);
    SotcAutopilot::Stop();
}

void TestSegmentNames() {
    SotcAutopilot::Route r;
    r.points = {MakeWaypoint(MakePose(0, 0, 0), "bridge"), MakeWaypoint(MakePose(0, 0, 10)),
                MakeWaypoint(MakePose(0, 0, 20), "courtyard"), MakeWaypoint(MakePose(0, 0, 30))};
    SotcAutopilot::SetRoute(r);
    SotcAutopilot::Seek(1);
    EXPECT(SotcAutopilot::GetProgress().seg == "bridge");  // inherited from point 0
    SotcAutopilot::Seek(2);
    EXPECT(SotcAutopilot::GetProgress().seg == "courtyard");
    SotcAutopilot::Stop();
}

void TestSpeedClamp() {
    SotcAutopilot::Route r;
    r.points = {MakeWaypoint(MakePose(0, 0, 0)), MakeWaypoint(MakePose(0, 0, 10))};
    SotcAutopilot::SetRoute(r);
    SotcAutopilot::SetSpeed(1000.0f);
    EXPECT(Near(SotcAutopilot::Current().speed, 100.0f, 1e-4f));
    SotcAutopilot::SetSpeed(0.0f);
    EXPECT(Near(SotcAutopilot::Current().speed, 0.1f, 1e-4f));
    SotcAutopilot::SetSpeed(12.5f);
    EXPECT(Near(SotcAutopilot::Current().speed, 12.5f, 1e-4f));
}

void TestTooFewPoints() {
    SotcAutopilot::Route r;
    SotcAutopilot::SetRoute(r);  // zero points
    SotcAutopilot::Play();
    SotcFreecam::Pose pose;
    EXPECT(!SotcAutopilot::Step(0.1f, &pose));
    r.points = {MakeWaypoint(MakePose(1, 2, 3))};
    SotcAutopilot::SetRoute(r);  // one point
    SotcAutopilot::Play();
    EXPECT(!SotcAutopilot::Step(0.1f, &pose));
    SotcAutopilot::Stop();
}

void TestRecording() {
    SotcAutopilot::StartRecording("rec");
    SotcAutopilot::AddWaypoint(MakePose(0, 0, 0), "one");
    SotcAutopilot::AddWaypoint(MakePose(0, 0, 10));
    SotcAutopilot::StopRecording();
    const SotcAutopilot::Route r = SotcAutopilot::Current();
    EXPECT(r.name == "rec");
    EXPECT(r.points.size() == 2);
    EXPECT(r.points[0].seg == "one");
}

void TestJsonParse() {
    SotcJson::Value v;
    std::string err;
    const std::string text =
        " {\"s\": \"a\\\"b\\\\c\\n\\u00e9\\ud83d\\ude00\", \"n\": [-1.5e2, 0, true, false, null], "
        "\"o\": {}, \"e\": []} ";
    EXPECT(SotcJson::Parse(text, &v, &err));
    const SotcJson::Value* s = v.Get("s");
    EXPECT(s != nullptr && s->IsString());
    EXPECT(s != nullptr && s->str == "a\"b\\c\n\xC3\xA9\xF0\x9F\x98\x80");
    const SotcJson::Value* n = v.Get("n");
    EXPECT(n != nullptr && n->IsArray() && n->items.size() == 5);
    if (n != nullptr && n->items.size() == 5) {
        EXPECT(n->items[0].IsNumber() && n->items[0].number == -150.0);
        EXPECT(n->items[1].IsNumber() && n->items[1].number == 0.0);
        EXPECT(n->items[2].IsBool() && n->items[2].boolean);
        EXPECT(n->items[3].IsBool() && !n->items[3].boolean);
        EXPECT(n->items[4].type == SotcJson::Value::Type::Null);
    }
    EXPECT(v.Get("missing") == nullptr);

    // Malformed input is rejected.
    EXPECT(!SotcJson::Parse("[1,]", &v, &err));
    EXPECT(!SotcJson::Parse("{\"a\":1,}", &v, &err));
    EXPECT(!SotcJson::Parse("{\"a\":1} x", &v, &err));
    EXPECT(!SotcJson::Parse("\"abc", &v, &err));
    EXPECT(!SotcJson::Parse("\"a\x01b\"", &v, &err));
    EXPECT(!SotcJson::Parse("\"\\ud83d\"", &v, &err));
    EXPECT(!SotcJson::Parse("[1 2]", &v, &err));
    EXPECT(!err.empty());
}

void TestJsonRoundTrip() {
    SotcJson::Value root = SotcJson::Value::MakeObject();
    root.Set("text", SotcJson::Value::MakeString("line\nquote\" tab\t\x01 caf\xC3\xA9"));
    root.Set("pi", SotcJson::Value::MakeNumber(0.1f));
    root.Set("big", SotcJson::Value::MakeNumber(-12345.5));
    SotcJson::Value arr = SotcJson::Value::MakeArray();
    arr.Push(SotcJson::Value::MakeBool(true));
    arr.Push(SotcJson::Value::MakeObject());
    root.Set("arr", arr);

    const std::string text = SotcJson::Dump(root);
    SotcJson::Value back;
    std::string err;
    EXPECT(SotcJson::Parse(text, &back, &err));
    const SotcJson::Value* t = back.Get("text");
    EXPECT(t != nullptr && t->str == "line\nquote\" tab\t\x01 caf\xC3\xA9");
    const SotcJson::Value* pi = back.Get("pi");
    EXPECT(pi != nullptr && Near(float(pi->number), 0.1f, 0.0f));
    const SotcJson::Value* big = back.Get("big");
    EXPECT(big != nullptr && big->number == -12345.5);
    EXPECT(SotcJson::Dump(back) == text);  // dumping the parsed value again gives the same text
}

void TestRouteFileRoundTrip() {
    const std::string path = "autopilot_test_out/nested/route.json";
    SotcAutopilot::Route r;
    r.name = "roundtrip";
    r.speed = 17.5f;
    r.start_boss = 3;
    r.has_start_goto = true;
    r.start_goto = MakePose(1.5f, -2.0f, 3.25f, 0.5f, -0.25f);
    r.points = {MakeWaypoint(MakePose(0, 0, 0, 0.1f, 0.0f), "bridge", 250),
                MakeWaypoint(MakePose(4.5f, 1.0f, 9.0f, -1.0f, 0.3f)),
                MakeWaypoint(MakePose(10.0f, 2.0f, 12.0f, 3.0f, -0.1f), "courtyard")};
    SotcAutopilot::SetRoute(r);
    std::string err;
    EXPECT(SotcAutopilot::Save(path, &err));
    EXPECT(std::filesystem::exists(path));

    EXPECT(SotcAutopilot::Load(path, &err));
    const SotcAutopilot::Route b = SotcAutopilot::Current();
    EXPECT(b.name == "roundtrip");
    EXPECT(Near(b.speed, 17.5f, 1e-4f));
    EXPECT(b.start_boss == 3);
    EXPECT(b.has_start_goto);
    EXPECT(NearPos(b.start_goto.pos, r.start_goto.pos, 1e-5f));
    EXPECT(Near(b.start_goto.yaw, 0.5f, 1e-5f) && Near(b.start_goto.pitch, -0.25f, 1e-5f));
    EXPECT(b.points.size() == 3);
    if (b.points.size() == 3) {
        EXPECT(b.points[0].seg == "bridge");
        EXPECT(b.points[0].dwell_ms == 250);
        EXPECT(b.points[1].seg.empty());
        EXPECT(b.points[1].dwell_ms == 0);
        EXPECT(b.points[2].seg == "courtyard");
        EXPECT(NearPos(b.points[1].pose.pos, r.points[1].pose.pos, 1e-5f));
        EXPECT(Near(b.points[2].pose.yaw, 3.0f, 1e-5f));
        EXPECT(Near(b.points[2].pose.pitch, -0.1f, 1e-5f));
    }

    // A file that is not a route is refused with a message.
    EXPECT(!SotcAutopilot::Load("autopilot_test_out/missing.json", &err));
    EXPECT(!err.empty());
    std::filesystem::remove_all("autopilot_test_out");
}

} // namespace

int main() {
    TestSplineHitsPoints();
    TestConstantSpeed();
    TestDwellThenFinish();
    TestLoopLaps();
    TestSegmentNames();
    TestSpeedClamp();
    TestTooFewPoints();
    TestRecording();
    TestJsonParse();
    TestJsonRoundTrip();
    TestRouteFileRoundTrip();
    if (g_failures == 0) {
        std::printf("PASS\n");
        return 0;
    }
    std::printf("FAIL: %d check(s)\n", g_failures);
    return 1;
}
