// sotc: offline test of the SOTC_TRACE recorder (gpu/shim/sotc_trace.cpp), no game needed.
// Build (CLANG64): clang++ -std=c++20 -O1 -Igpu/shadps4 tools/sotc_trace_test.cpp
//   gpu/shim/sotc_trace.cpp -o out/sotc_trace_test.exe
// Run: SOTC_TRACE=1 SOTC_TRACE_DIR=out/trace_test out/sotc_trace_test.exe
// Several threads record spans while "frames" end every 33 ms; frame 10 takes 120 ms. One file
// must appear, holding the slow frame, every thread's spans of its window and none outside it.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#include "video_core/sotc_stats.h"
#include "video_core/sotc_trace.h"

int main() {
    if (!SotcTrace::kOn) {
        std::fprintf(stderr, "set SOTC_TRACE=1\n");
        return 2;
    }
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            if (t == 1) {
                SotcStats::tl_role = SotcStats::RoleDraw;
            }
            SotcTrace::OpBatch batch;
            uint64_t n = 0;
            while (!stop.load()) {
                const uint64_t t0 = SotcTrace::Now();
                if (t == 1) {
                    while (SotcTrace::Now() - t0 < (n % 100 == 0 ? 80000u : 3000u)) {
                    }
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                }
                const uint64_t t1 = SotcTrace::Now();
                if (t == 1) {
                    batch.Note("draw", t0, t1, n);
                } else {
                    SotcTrace::Span("evf wait", t0, t1, n, 0x1000 + 0x101ae4);
                }
                ++n;
            }
            batch.Flush();
        });
    }
    SotcTrace::SetImage(0x1000, 0x2000000);
    const uint32_t q = SotcTrace::RegisterQueue("GPU: test queue");
    uint64_t prev = SotcTrace::Now();
    for (uint64_t frame = 1; frame <= 20; ++frame) {
        SotcTrace::Submitted(q, frame, SotcTrace::Now());
        std::this_thread::sleep_for(std::chrono::milliseconds(frame == 10 ? 120 : 33));
        const uint64_t now = SotcTrace::Now();
        SotcTrace::Completed(q, frame, frame, now);
        SotcTrace::FrameEnd(frame, prev, now);
        prev = now;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop = true;
    for (auto& th : threads) {
        th.join();
    }
    return 0;
}
