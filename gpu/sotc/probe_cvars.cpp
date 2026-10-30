// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: cvar probe (plan step 0c, docs/HOOK-DESIGN.md). One range hook (scripts/hooks.d/cvars.py),
// switch SOTC_HOOK_CVAR_PROBE: CVAR_MAX_DELTA resolves sub_262a80's two cvar registrations and
// derives the image offsets of the gameMaxDeltaTime and gameDeltaTimeCorrectionTolorance objects
// (fields "storage" and "tolerance"). A range hook installs no detour, so there is no handler; the
// probe only reads the cvar objects when it reports.
//
// For each cvar object (image offset + ImageBase()): the current value is the float at
// *(float*)(*(uint64*)(obj + 0x60)), the default is the float at obj + 0xd0. Expected defaults are
// 0.1 and 0.5. Observe only.
//
// The cvar objects live in the eboot image, which the loader maps directly (the game uses 31-bit
// pointers, so a stored runtime pointer into the image is a valid host pointer). They are read as
// host memory at ImageBase() + offset, NOT through the GPU backing view: that view covers only
// GPU-tracked guest allocations, not the image, so it returns null here.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "hooks.h"
#include "probes.h"

namespace SotcProbes {
namespace {

constexpr uint64_t kStoragePtrOffset = 0x60; // cvar object: pointer to the engine's live float
constexpr uint64_t kDefaultOffset = 0xd0;    // cvar object: the default float
constexpr uint64_t kImageSpan = 256ull * 1024 * 1024; // guard for a pointer into the mapped image

// A host address that lies inside the mapped eboot image.
bool InImage(uintptr_t addr) {
    const uintptr_t base = SotcHooks::ImageBase();
    return base != 0 && addr >= base && addr < base + kImageSpan;
}

// Resolves one cvar object (by its derived field name) and reads its default and current value
// directly from the mapped image.
bool ReadCvar(const char* field, float* def, float* cur) {
    uint64_t image_off = 0;
    if (!SotcHooks::Derived("CVAR_MAX_DELTA", field, &image_off) || SotcHooks::ImageBase() == 0) {
        return false;
    }
    const uintptr_t obj = SotcHooks::ImageBase() + static_cast<uintptr_t>(image_off);
    if (!InImage(obj + kDefaultOffset) || !InImage(obj + kStoragePtrOffset)) {
        return false;
    }
    std::memcpy(def, reinterpret_cast<const void*>(obj + kDefaultOffset), sizeof *def);
    uint64_t storage = 0;
    std::memcpy(&storage, reinterpret_cast<const void*>(obj + kStoragePtrOffset), sizeof storage);
    if (!InImage(static_cast<uintptr_t>(storage))) {
        *cur = *def; // the live pointer is not in the image; fall back to the default slot
        return true;
    }
    std::memcpy(cur, reinterpret_cast<const void*>(static_cast<uintptr_t>(storage)), sizeof *cur);
    return true;
}

// Last values printed, so the line is emitted once and then only when a value moves.
bool g_have_last = false;
float g_last_max_cur = 0.0f;
float g_last_tol_cur = 0.0f;

bool SameFloat(float a, float b) {
    // Bitwise equality so a NaN compares equal to itself and a change is never missed by rounding.
    return std::memcmp(&a, &b, sizeof a) == 0;
}

} // namespace

void RegisterCvars() {
    // Nothing to register: CVAR_MAX_DELTA is a range hook with no detour, so it has no handler. The
    // switch state and the derived offsets are only known after InstallHooks, so the cvar objects are
    // read lazily in ReportCvars; Derived() returns false until (and unless) the hook resolves.
}

void ReportCvars(std::string& out, double /*seconds*/) {
    float max_def = 0.0f, max_cur = 0.0f;
    float tol_def = 0.0f, tol_cur = 0.0f;
    const bool have_max = ReadCvar("storage", &max_def, &max_cur);
    const bool have_tol = ReadCvar("tolerance", &tol_def, &tol_cur);
    if (!have_max && !have_tol) {
        return; // hook not resolved, or the objects are not readable yet
    }

    const bool changed = !g_have_last || !SameFloat(max_cur, g_last_max_cur) ||
                         !SameFloat(tol_cur, g_last_tol_cur);
    if (!changed) {
        return; // print once, then only when a current value moves
    }
    g_have_last = true;
    g_last_max_cur = max_cur;
    g_last_tol_cur = tol_cur;

    char line[256];
    std::snprintf(line, sizeof line,
                  "SOTC CVAR probe: gameMaxDeltaTime default=%.4f current=%.4f; "
                  "gameDeltaTimeCorrectionTolorance default=%.4f current=%.4f\n",
                  have_max ? max_def : 0.0, have_max ? max_cur : 0.0,
                  have_tol ? tol_def : 0.0, have_tol ? tol_cur : 0.0);
    out += line;
}

} // namespace SotcProbes
