// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: area prewarm probe (plan step 0c, docs/re-stutters/reports/05-streaming.md, 07-first-use.md).
// Three entry hooks (scripts/hooks.d/area.py), switch SOTC_AREA_PREWARM:
//   AREA_ZONE_ACTIVATE entry of ZoneActivate (eboot 0x34fb30). rdi = the zone manager (mgr), esi = the
//                      zone id. The zone table (mgr+0xa0 begin, mgr+0xa8 end, 0x80-byte records sorted by
//                      a uint key) gives the area name. This is the area source: once it has fired, the
//                      loader hook below no longer drives the area.
//   AREA_LOADER_STATE  entry of the AreaLoaderState setter (sub_343890). rdi = the AreaLoaderState
//                      object; rdx = int* of the requested state (the value is *rdx). Counters only once
//                      the zone hook has fired (its name chain is not verified).
//   AREA_PRECACHE      entry of AreaLoad_PrecacheLists (0x533100). rdi = the loader object; the area
//                      is not in the entry registers, so a precache is attributed to the area the last
//                      area change recorded.
// The probe keeps the current area and counts state transitions, zone changes and precache events per
// report window. Observe only: every handler returns HookAction::Continue; all guest reads go through
// the backing view (never the guest mapping) with a null/unmapped guard.

#include <array>
#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "area_prewarm.h"
#include "hooks.h"
#include "lab_commands.h" // sotc: SotC Lab "get state" area
#include "probes.h"

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address, and the mapped region of one.
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
}

namespace SotcProbes {
namespace {

using SotcHooks::GuestRegs;
using SotcHooks::HookAction;

constexpr uint64_t kNameChain0 = 0x18; // rdi+0x18 -> X
constexpr uint64_t kNameChain1 = 0x68; // X+0x68   -> Y
constexpr uint64_t kNameChain2 = 0x08; // Y+0x08   -> char*
constexpr uint64_t kGuidBase = 0x30;   // rdi+0x30 -> base; GUID 16 bytes at base+8
constexpr uint64_t kGuidOffset = 0x08;
constexpr size_t kGuidLen = 16;
constexpr size_t kNameMax = 96;

// Zone table of the manager (ZoneActivate, 0x34fb30): begin/end pointers, 0x80-byte records sorted by a
// uint key at +0x00. The name is a char* at +0x10 with its length at +0x18; the length is checked and
// the name must be printable ASCII, else the zone is "unnamed" and gets the key "zone_<id>".
constexpr uint64_t kZoneBegin = 0xa0;
constexpr uint64_t kZoneEnd = 0xa8;
constexpr uint64_t kZoneRecord = 0x80;
constexpr uint64_t kZoneMaxRecords = 4096;
constexpr uint64_t kZoneNamePtr = 0x10;
constexpr uint64_t kZoneNameLen = 0x18;
constexpr size_t kZoneNameCap = 64; // names up to 63 characters

const char* StateName(int32_t s) {
    switch (s) {
    case 0:
        return "Auto";
    case 1:
        return "Unavailable";
    case 2:
        return "Unloaded";
    case 3:
        return "Loading";
    case 4:
        return "Loaded";
    case 5:
        return "Creating";
    case 6:
        return "Hidden";
    case 7:
        return "FadingIn";
    case 8:
        return "Created";
    case 9:
        return "FadingOut";
    default:
        return "?";
    }
}

// Reads n bytes of guest memory through the backing view, bounded to the mapped region. False when
// unmapped or partly outside the region.
bool ReadGuest(uintptr_t address, void* out, size_t n) {
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!runtime_memory_region(address, &start, &end, &mapped) || !mapped) {
        return false;
    }
    if (address < start || address + n > end) {
        return false;
    }
    const unsigned char* p = runtime_memory_backing_pointer(address);
    if (!p) {
        return false;
    }
    std::memcpy(out, p, n);
    return true;
}

bool ReadU64(uintptr_t address, uint64_t* out) {
    return ReadGuest(address, out, sizeof *out);
}

// Copies a NUL-terminated guest C string into out (always terminated), bounded to the mapped region.
// False when the pointer is null or unmapped.
bool ReadGuestString(uintptr_t address, char* out, size_t cap) {
    out[0] = '\0';
    if (!address || cap == 0) {
        return false;
    }
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!runtime_memory_region(address, &start, &end, &mapped) || !mapped || address < start) {
        return false;
    }
    const unsigned char* p = runtime_memory_backing_pointer(address);
    if (!p) {
        return false;
    }
    size_t avail = static_cast<size_t>(end - address);
    size_t limit = std::min(cap - 1, avail);
    size_t i = 0;
    for (; i < limit && p[i] != 0; ++i) {
        out[i] = static_cast<char>(p[i]);
    }
    out[i] = '\0';
    return true;
}

// The current area, guarded by g_mutex.
std::mutex g_mutex;
char g_area_name[kNameMax] = {0};
uint8_t g_area_guid[kGuidLen] = {0};
bool g_area_valid = false;
int32_t g_last_state = -1;
// sotc: SOTC_AREA_PREWARM: the area changes sent to SotcAreaPrewarm::OnAreaChange, in order (the
// call is made after the lock is released, so the sequence number keeps them in order).
uint64_t g_change_seq = 0;
// Set by the first real zone change (zone id != 0). From then on the loader hook keeps counters only.
std::atomic<bool> g_zone_seen{false};

// Window counters (reset by ReportArea).
std::atomic<uint64_t> g_state_sets{0};   // AREA_LOADER_STATE calls
std::atomic<uint64_t> g_transitions{0};  // calls whose requested state differs from the previous
std::atomic<uint64_t> g_precache{0};     // AREA_PRECACHE calls
std::atomic<uint64_t> g_unknown{0};      // calls where the name/GUID chain could not be read
std::atomic<uint64_t> g_zone_changes{0}; // AREA_ZONE_ACTIVATE calls with a zone id
std::atomic<uint64_t> g_zone_unnamed{0}; // of those, zones whose name could not be read

// Reads the area name and GUID of an AreaLoaderState object through the backing view. False when any
// link in the chain is null/unmapped (the caller then treats the area as unknown).
bool ReadAreaIdentity(uintptr_t obj, char* name, size_t name_cap, uint8_t guid[kGuidLen]) {
    name[0] = '\0';
    std::memset(guid, 0, kGuidLen);
    uint64_t x = 0, y = 0, strp = 0;
    if (!ReadU64(obj + kNameChain0, &x) || !x) {
        return false;
    }
    if (!ReadU64(static_cast<uintptr_t>(x) + kNameChain1, &y) || !y) {
        return false;
    }
    if (!ReadU64(static_cast<uintptr_t>(y) + kNameChain2, &strp) || !strp) {
        return false;
    }
    if (!ReadGuestString(static_cast<uintptr_t>(strp), name, name_cap)) {
        return false;
    }
    uint64_t guid_base = 0;
    if (!ReadU64(obj + kGuidBase, &guid_base) || !guid_base) {
        return false;
    }
    if (!ReadGuest(static_cast<uintptr_t>(guid_base) + kGuidOffset, guid, kGuidLen)) {
        return false;
    }
    return true;
}

// Finds the zone record with the given key (lower bound on the sorted table). False when the table
// pointers are unmapped or inconsistent, or the key is absent.
bool FindZoneRecord(uintptr_t mgr, uint32_t zone, uintptr_t* record) {
    uint64_t begin = 0, end = 0;
    if (!ReadU64(mgr + kZoneBegin, &begin) || !ReadU64(mgr + kZoneEnd, &end)) {
        return false;
    }
    if (begin == 0 || end <= begin) {
        return false;
    }
    const uint64_t bytes = end - begin;
    if (bytes % kZoneRecord != 0 || bytes / kZoneRecord > kZoneMaxRecords) {
        return false;
    }
    const uint64_t count = bytes / kZoneRecord;
    uint64_t lo = 0, hi = count;
    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo) / 2;
        uint32_t key = 0;
        if (!ReadGuest(static_cast<uintptr_t>(begin + mid * kZoneRecord), &key, sizeof key)) {
            return false;
        }
        if (key < zone) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo >= count) {
        return false;
    }
    uint32_t key = 0;
    if (!ReadGuest(static_cast<uintptr_t>(begin + lo * kZoneRecord), &key, sizeof key) || key != zone) {
        return false;
    }
    *record = static_cast<uintptr_t>(begin + lo * kZoneRecord);
    return true;
}

// The name of a zone: printable ASCII, 1 to 63 characters, read with the length field at +0x18.
bool ReadZoneName(uintptr_t mgr, uint32_t zone, char* out, size_t cap) {
    out[0] = '\0';
    uintptr_t rec = 0;
    if (!FindZoneRecord(mgr, zone, &rec)) {
        return false;
    }
    uint64_t ptr = 0, len = 0;
    if (!ReadU64(rec + kZoneNamePtr, &ptr) || !ReadU64(rec + kZoneNameLen, &len)) {
        return false;
    }
    if (ptr == 0 || len == 0 || len >= kZoneNameCap || len + 1 > cap) {
        return false;
    }
    char buf[kZoneNameCap] = {0};
    if (!ReadGuest(static_cast<uintptr_t>(ptr), buf, static_cast<size_t>(len))) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] < 0x20 || buf[i] > 0x7e) {
            return false;
        }
    }
    std::memcpy(out, buf, static_cast<size_t>(len));
    out[len] = '\0';
    return true;
}

HookAction OnLoaderState(GuestRegs& regs, void*) {
    g_state_sets.fetch_add(1, std::memory_order_relaxed);

    int32_t requested = -1;
    ReadGuest(static_cast<uintptr_t>(regs.rdx), &requested, sizeof requested); // *rdx

    char name[kNameMax] = {0};
    uint8_t guid[kGuidLen] = {0};
    const bool ok = ReadAreaIdentity(static_cast<uintptr_t>(regs.rdi), name, kNameMax, guid);
    if (!ok) {
        g_unknown.fetch_add(1, std::memory_order_relaxed);
    }

    // sotc: SOTC_AREA_PREWARM: once the zone hook has fired it owns the area; this hook only counts.
    // An unknown identity is never sent as "" (that would reset the area).
    bool notify = false;
    char notify_name[kNameMax] = {0};
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (requested != g_last_state) {
            g_transitions.fetch_add(1, std::memory_order_relaxed);
            g_last_state = requested;
        }
        if (!g_zone_seen.load(std::memory_order_acquire)) {
            if (ok) {
                const bool area_changed =
                    name[0] && (!g_area_valid || std::strcmp(g_area_name, name) != 0);
                if (area_changed) {
                    notify = true;
                    std::snprintf(notify_name, kNameMax, "%s", name);
                }
                std::snprintf(g_area_name, kNameMax, "%s", name[0] ? name : "?");
                std::memcpy(g_area_guid, guid, kGuidLen);
                g_area_valid = true;
            } else {
                std::snprintf(g_area_name, kNameMax, "unknown");
                std::memset(g_area_guid, 0, kGuidLen);
                g_area_valid = false;
            }
        }
        if (notify) {
            seq = ++g_change_seq;
        }
    }
    // sotc: SOTC_AREA_PREWARM: outside g_mutex (the prewarm module does its own file I/O).
    if (notify) {
        std::fprintf(stderr, "SOTC AREA: entered %s (loader state)\n", notify_name);
        SotcAreaPrewarm::OnAreaChange(notify_name, seq);
        SotcLab::SetArea(notify_name);
    }
    return HookAction::Continue;
}

// sotc: SOTC_AREA_PREWARM: ZoneActivate. Zone 0 is the init/reset call and is skipped.
HookAction OnZoneActivate(GuestRegs& regs, void*) {
    const auto mgr = static_cast<uintptr_t>(regs.rdi);
    const auto zone = static_cast<uint32_t>(regs.rsi);
    if (zone == 0) {
        return HookAction::Continue;
    }
    g_zone_changes.fetch_add(1, std::memory_order_relaxed);

    char zone_name[kZoneNameCap] = {0};
    const bool named = ReadZoneName(mgr, zone, zone_name, sizeof zone_name);
    if (!named) {
        g_zone_unnamed.fetch_add(1, std::memory_order_relaxed);
    }

    char key[kNameMax] = {0};
    if (named) {
        for (size_t i = 0; zone_name[i] != '\0' && i + 1 < kNameMax; ++i) {
            const char c = zone_name[i];
            key[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }
    } else {
        std::snprintf(key, kNameMax, "zone_%u", static_cast<unsigned>(zone));
    }

    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_zone_seen.store(true, std::memory_order_release);
        std::snprintf(g_area_name, kNameMax, "%s", key);
        std::memset(g_area_guid, 0, kGuidLen);
        g_area_valid = false;
        seq = ++g_change_seq;
    }
    std::fprintf(stderr, "SOTC AREA: entered %s (zone %u)\n", key, static_cast<unsigned>(zone));
    SotcAreaPrewarm::OnAreaChange(key, seq);
    SotcLab::SetArea(key);
    return HookAction::Continue;
}

HookAction OnPrecache(GuestRegs&, void*) {
    g_precache.fetch_add(1, std::memory_order_relaxed);
    return HookAction::Continue;
}

} // namespace

void RegisterArea() {
    // sotc: SOTC_AREA_PREWARM: the records of the boot area start now (no-op when the switch is off).
    SotcAreaPrewarm::OnAreaChange("boot", 0);
    SotcHooks::Handler zone;
    zone.pre = &OnZoneActivate;
    SotcHooks::RegisterHandler("AREA_ZONE_ACTIVATE", zone);

    SotcHooks::Handler state;
    state.pre = &OnLoaderState;
    SotcHooks::RegisterHandler("AREA_LOADER_STATE", state);

    SotcHooks::Handler precache;
    precache.pre = &OnPrecache;
    SotcHooks::RegisterHandler("AREA_PRECACHE", precache);
}

void ReportArea(std::string& out, double seconds) {
    if (seconds <= 0.0) {
        seconds = 1.0;
    }
    const uint64_t sets = g_state_sets.exchange(0, std::memory_order_relaxed);
    const uint64_t transitions = g_transitions.exchange(0, std::memory_order_relaxed);
    const uint64_t precache = g_precache.exchange(0, std::memory_order_relaxed);
    const uint64_t unknown = g_unknown.exchange(0, std::memory_order_relaxed);
    const uint64_t zones = g_zone_changes.exchange(0, std::memory_order_relaxed);
    const uint64_t unnamed = g_zone_unnamed.exchange(0, std::memory_order_relaxed);
    if (sets == 0 && precache == 0 && zones == 0) {
        return;
    }

    char name[kNameMax] = {0};
    uint8_t guid[kGuidLen] = {0};
    bool valid = false;
    int32_t last_state = -1;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::memcpy(name, g_area_name, kNameMax);
        std::memcpy(guid, g_area_guid, kGuidLen);
        valid = g_area_valid;
        last_state = g_last_state;
    }

    char guid_hex[kGuidLen * 2 + 1] = {0};
    if (valid) {
        for (size_t i = 0; i < kGuidLen; ++i) {
            std::snprintf(guid_hex + i * 2, 3, "%02x", guid[i]);
        }
    }

    char line[400];
    std::snprintf(line, sizeof line,
                  "SOTC AREA probe: current %s%s%s state %s; %llu state sets (%llu transitions), "
                  "%llu precache, %llu unknown, %llu zone changes (%llu unnamed) in %.0f s\n",
                  name[0] ? name : "?", valid ? " guid " : "", valid ? guid_hex : "",
                  StateName(last_state), static_cast<unsigned long long>(sets),
                  static_cast<unsigned long long>(transitions),
                  static_cast<unsigned long long>(precache),
                  static_cast<unsigned long long>(unknown),
                  static_cast<unsigned long long>(zones), static_cast<unsigned long long>(unnamed),
                  seconds);
    out += line;
}

} // namespace SotcProbes
