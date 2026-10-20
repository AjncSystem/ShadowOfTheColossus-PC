// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: CPU patches for Shadow of the Colossus, applied to the mapped eboot before it runs.
//  - SSE4a (EXTRQ/INSERTQ/MOVNTSS/MOVNTSD) on CPUs without it (Intel): the 4-byte forms are
//    relocated with the instructions after them, the longer forms jump to a trampoline.
//  - Selective red-zone protection for three functions that keep live data below rsp while
//    touching memory the GPU page tracker may fault on (Windows exception dispatch writes
//    below rsp).
// Both come from the shadPS4 SotC fork (cpu_patches.cpp, module.cpp).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif
#include "common/logging/log.h"
#include "core/signals.h"
#include "cpu_patches.h"
#include "dwarf.h"
#include "sotc_patches.h"

extern "C" int sotc_patch_eboot(const SotcPatchRequest* request, SotcPatchResult* result) {
    *result = {};
    result->needs_sse4a = Core::NeedsSse4aEmulation() ? 1 : 0;
    result->needs_bmi = Core::NeedsBmiEmulation() ? 1 : 0;
    Core::RegisterPatchModule(request->image, request->module_size, request->trampoline,
                              request->trampoline_size);
    const auto base = reinterpret_cast<uintptr_t>(request->image);

    std::vector<uintptr_t> function_starts;
    const auto add_table = [&](uint64_t offset, uint64_t size) {
        const uintptr_t hdr_start = base + offset;
        const uintptr_t hdr_end = hdr_start + size;
        Dwarf::EHHeaderInfo info;
        std::vector<uintptr_t> starts; // DecodeEHHdrTable replaces the vector's contents
        if (!Dwarf::DecodeEHHdr(hdr_start, hdr_end, info) ||
            !Dwarf::DecodeEHHdrTable(info, hdr_end, starts)) {
            return false;
        }
        function_starts.insert(function_starts.end(), starts.begin(), starts.end());
        return true;
    };
    if (request->eh_frame_hdr_size && !add_table(request->eh_frame_hdr, request->eh_frame_hdr_size)) {
        return -1;
    }
    // sotc: the linked modules' code (libc, Fios2, the user's audio modules) is only patched for
    // BMI1/LZCNT/MOVBE; a table that does not decode leaves that module to the linear sweep.
    for (uint32_t i = 0; i < request->module_table_count; ++i) {
        if (!add_table(request->module_tables[i].offset, request->module_tables[i].size)) {
            LOG_WARNING(Core, "Module function table at {:#x} does not decode",
                        request->module_tables[i].offset);
        }
    }
    result->function_starts = function_starts.size();

    std::vector<uintptr_t> selected;
    for (uint32_t i = 0; i < request->red_zone_count; ++i) {
        selected.push_back(base + request->red_zone_functions[i]);
    }
    for (uint32_t i = 0; i < request->segment_count; ++i) {
        const uint64_t address = base + request->segments[i].offset;
        const uint64_t size = request->segments[i].size;
        // sotc: module code only for BMI emulation; unchanged elsewhere (SSE4a: eboot only).
        if (request->segments[i].offset >= request->eboot_size && !result->needs_bmi) {
            continue;
        }
        if (result->needs_sse4a || result->needs_bmi) {
            result->short_sse4a += Core::PatchShortSse4aInstructions(address, size, function_starts);
        }
        if (!selected.empty()) {
            const auto red = Core::PatchRedZoneMemoryInstructions(address, size, function_starts, selected);
            result->red_zone_functions += red.function_count;
            result->red_zone_instructions += red.patched_memory_instruction_count;
        }
        // Long forms last: the linear sweep sees the jumps written above as plain instructions.
        // sotc: not over module code (only patched for BMI, at decoded boundaries above).
        if (request->segments[i].offset < request->eboot_size) {
            Core::PrePatchInstructions(address, size);
        }
    }
    return 0;
}

// Fallback for a site the patchers could not rewrite: the shadPS4 interpreter executes it
// from the exception (the red zone is not safe there, but the game keeps running).
extern "C" int sotc_handle_illegal(void* context) {
#if defined(_WIN32)
    // sotc: the first distinct illegal instructions are listed in the session log. A handler that
    // reports success without moving past the instruction expects it to run again patched; if the
    // same address faults again right after that, the game would spin there silently forever, so
    // it ends with a report instead (issue #49).
    const auto* pointers = static_cast<const EXCEPTION_POINTERS*>(context);
    const auto rip = static_cast<uintptr_t>(pointers->ContextRecord->Rip);
    static std::atomic<int> listed{0};
    static thread_local uintptr_t retry_rip = 0; // handled without moving past it
    static thread_local unsigned retries = 0;
    static thread_local uintptr_t last_listed = 0;
    const auto dump = [rip](const char* what) {
        const auto* b = reinterpret_cast<const unsigned char*>(rip);
        std::fprintf(stderr,
                     "SOTC illegal instruction %s at %#llx: %02x %02x %02x %02x %02x %02x %02x %02x "
                     "%02x %02x %02x %02x\n",
                     what, static_cast<unsigned long long>(rip), b[0], b[1], b[2], b[3], b[4], b[5],
                     b[6], b[7], b[8], b[9], b[10], b[11]);
        std::fflush(stderr);
    };
    if (rip == retry_rip) {
        if (++retries == 16) {
            dump("still faulting after being patched");
            return 0;
        }
    } else {
        retry_rip = 0;
        retries = 0;
    }
    if (rip != last_listed) {
        last_listed = rip;
        if (listed.fetch_add(1, std::memory_order_relaxed) < 32) {
            dump("handled");
        }
    }
    const bool handled = Core::Signals::Instance()->DispatchIllegalInstruction(context);
    if (handled && static_cast<uintptr_t>(pointers->ContextRecord->Rip) == rip) {
        retry_rip = rip;
    }
    return handled ? 1 : 0;
#else
    return Core::Signals::Instance()->DispatchIllegalInstruction(context) ? 1 : 0;
#endif
}
