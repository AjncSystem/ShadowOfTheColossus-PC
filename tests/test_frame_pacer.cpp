// sotc: checks of the frame pacer's display refresh estimate and guest vblank lock
// (gpu/shadps4/video_core/renderer_vulkan/vk_frame_pacer.h). ninja -C out/gpu frame-pacer-test
#include <cassert>
#include <cmath>
#include <cstdio>
#include "video_core/renderer_vulkan/vk_frame_pacer.h"

using namespace Vulkan::FramePacing;

static bool Near(double a, double b, double eps) {
    return std::abs(a - b) < eps;
}

int main() {
    // A 143.9 Hz display seeded as 144 Hz: shown intervals of 2, 3, 4 and 5 refreshes (30 -> 60
    // passthrough, real-only at 30) converge to the real period.
    const double real = 1000.0 / 143.9;
    double p = 1000.0 / 144.0;
    const int ks[] = {2, 3, 4, 5};
    for (int i = 0; i < 400; ++i) {
        p = RefineRefresh(p, ks[i % 4] * real);
        assert(p > 0.0);
    }
    assert(Near(p, real, 1e-4));

    // Intervals that are not whole refreshes, or are too long, are rejected.
    assert(RefineRefresh(1000.0 / 144.0, 1000.0 / 60.0) == 0.0); // 2.4 refreshes
    assert(RefineRefresh(1000.0 / 60.0, 200.0) == 0.0);          // 12 refreshes (loading)

    // 60 Hz guest: locks to 59.94 and 119.88 Hz displays, not to 144 or 121 Hz.
    const double guest = 1000.0 / 60.0;
    assert(Near(LockedVblankMs(guest, 1000.0 / 59.94), 1000.0 / 59.94, 1e-9));
    assert(Near(LockedVblankMs(guest, 1000.0 / 119.88), 2000.0 / 119.88, 1e-9));
    assert(LockedVblankMs(guest, 1000.0 / 144.0) == 0.0);
    assert(LockedVblankMs(guest, 1000.0 / 121.0) == 0.0);
    // 480 Hz uncapped-preset guest on a 144 Hz display: no whole multiple.
    assert(LockedVblankMs(1000.0 / 480.0, 1000.0 / 144.0) == 0.0);

    std::puts("frame pacer checks passed");
    return 0;
}
