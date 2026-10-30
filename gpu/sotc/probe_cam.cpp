// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: camera and Wander probe (docs/re-stutters/freecam-re.md). Observe only.
// PLAYER_XFORM: entry of sub_0xb0, the player transform reader used by the printPlayerPos and
// setPlayerPos commands. It logs the object and what the type switch of sub_660fc0 resolves to, so
// the layout of the Wander object can be confirmed at run time. Every handler returns Continue.
// Logged as "SOTC CAMPROBE ..." once per call (command time only, so no rate limit is needed).

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "guest_mem.h"
#include "hooks.h"
#include "probe_cam.h"

namespace SotcProbes {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr int64_t kTypeOffset = 0xc;        // object: u32, low byte = type (sub_660fc0 switch)
constexpr int64_t kTypeLeftA = 0x120;        // types 9, 13, 22: the transform is obj + 0x24 * 8
constexpr int64_t kTypeLeftB = 0x40;         // type 10: obj + 8 * 8
constexpr int64_t kXformPosPtr = 0x150;      // transform: pointer to float[3] position
constexpr int64_t kXformRotPtr = 0x160;      // transform: pointer to float[4] quaternion

// The transform of an object as sub_660fc0 resolves it, for the types it can resolve without a
// guest call. 0 for the other types (logged as "type not resolved here").
uint64_t ResolveTransform(uint64_t obj, uint32_t* type_out) {
    uint32_t flags = 0;
    if (!SotcGuest::Read(obj + kTypeOffset, &flags)) {
        return 0;
    }
    *type_out = flags & 0xffu;
    switch (*type_out) {
    case 9:
    case 13:
    case 22:
        return obj + kTypeLeftA;
    case 10:
        return obj + kTypeLeftB;
    default:
        return 0;
    }
}

HookAction PlayerXformPre(GuestRegs& regs, void*) {
    const uint64_t obj = regs.rdi;
    uint32_t type = 0xffffffffu;
    const uint64_t xform = ResolveTransform(obj, &type);
    uint64_t pos_ptr = 0;
    float pos[3] = {0.0f, 0.0f, 0.0f};
    bool pos_ok = false;
    if (xform != 0 && SotcGuest::Read(xform + kXformPosPtr, &pos_ptr) && pos_ptr != 0) {
        pos_ok = SotcGuest::Read(pos_ptr, pos, sizeof pos);
    }
    if (xform == 0) {
        std::fprintf(stderr, "SOTC CAMPROBE player obj=0x%llx type=%u (type not resolved here)\n",
                     static_cast<unsigned long long>(obj), static_cast<unsigned>(type));
    } else {
        std::fprintf(stderr,
                     "SOTC CAMPROBE player obj=0x%llx type=%u xform=0x%llx pos_ptr=0x%llx "
                     "pos=%s%.3f %.3f %.3f\n",
                     static_cast<unsigned long long>(obj), static_cast<unsigned>(type),
                     static_cast<unsigned long long>(xform),
                     static_cast<unsigned long long>(pos_ptr), pos_ok ? "" : "unreadable ",
                     pos[0], pos[1], pos[2]);
    }
    return HookAction::Continue;
}

} // namespace

void RegisterCamProbe() {
    SotcHooks::RegisterHandler("PLAYER_XFORM", SotcHooks::Handler{PlayerXformPre, nullptr, nullptr});
}

} // namespace SotcProbes
