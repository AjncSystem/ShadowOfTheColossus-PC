// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: CPU patches for Shadow of the Colossus, applied to the mapped eboot before it runs.
//  - SSE4a (EXTRQ/INSERTQ/MOVNTSS/MOVNTSD) on CPUs without it (Intel): the 4-byte forms are
//    relocated with the instructions after them, the longer forms jump to a trampoline.
//  - Selective red-zone protection for three functions that keep live data below rsp while
//    touching memory the GPU page tracker may fault on (Windows exception dispatch writes
//    below rsp).
//  - rdtsc redirected to a host clock when the cores disagree on the TSC (src/compat_win.c).
// The first two come from the shadPS4 SotC fork (cpu_patches.cpp, module.cpp).
#include <vector>
#include "common/logging/log.h"
#include "core/signals.h"
#include "cpu_patches.h"
#include "dwarf.h"
#include "hook_layout.h"
#include "hooks.h"
#include "sotc_patches.h"
#include "../shim/sotc_sampler.h" // sotc: trace v2 continuous sampler (SOTC_TRACE_SAMPLE)

extern "C" int sotc_patch_eboot(const SotcPatchRequest* request, SotcPatchResult* result) {
    *result = {};
    result->needs_sse4a = Core::NeedsSse4aEmulation() ? 1 : 0;
    Core::RegisterPatchModule(request->image, request->module_size, request->trampoline,
                              request->trampoline_size);
    const auto base = reinterpret_cast<uintptr_t>(request->image);

    std::vector<uintptr_t> function_starts;
    if (request->eh_frame_hdr_size) {
        const uintptr_t hdr_start = base + request->eh_frame_hdr;
        const uintptr_t hdr_end = hdr_start + request->eh_frame_hdr_size;
        Dwarf::EHHeaderInfo info;
        if (!Dwarf::DecodeEHHdr(hdr_start, hdr_end, info) ||
            !Dwarf::DecodeEHHdrTable(info, hdr_end, function_starts)) {
            return -1;
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
        if (result->needs_sse4a) {
            result->short_sse4a += Core::PatchShortSse4aInstructions(address, size, function_starts);
        }
        if (!selected.empty()) {
            const auto red = Core::PatchRedZoneMemoryInstructions(address, size, function_starts, selected);
            result->red_zone_functions += red.function_count;
            result->red_zone_instructions += red.patched_memory_instruction_count;
        }
        if (request->tsc_clock) {
            u64 unpatched = 0;
            result->rdtsc += Core::PatchRdtscInstructions(address, size, function_starts,
                                                          request->tsc_clock, &unpatched);
            result->rdtsc_unpatched += unpatched;
        }
        // Long forms last: the linear sweep sees the jumps written above as plain instructions.
        Core::PrePatchInstructions(address, size);
    }

    // sotc: engine hooks last, so their pattern checks see the final bytes (docs/HOOK-DESIGN.md).
    // The code layout gives the hook scan its code segments and function starts.
    if (request->hooks_path) {
        Core::RegisterFunctionStarts(function_starts); // the detours need the function table
        SotcHooks::RegisterAllHandlers();
        std::vector<SotcHooks::CodeRange> code;
        for (uint32_t i = 0; i < request->segment_count; ++i) {
            const uint64_t begin = request->segments[i].offset;
            code.push_back({begin, begin + request->segments[i].size});
        }
        std::vector<uint64_t> starts;
        for (const uintptr_t start : function_starts) {
            if (start >= base) {
                starts.push_back(start - base);
            }
        }
        SotcHooks::SetCodeLayout(code, starts);
        SotcHooks::InstallStats hook_stats{};
        SotcHooks::InstallHooks(request->hooks_path, request->image, request->module_size,
                                &hook_stats);
        result->hooks_installed = hook_stats.installed;
        result->hooks_off = hook_stats.off;
    }
    result->trampoline_used = Core::TrampolineBytesUsed();
    // sotc: trace v2 continuous sampler (SOTC_TRACE_SAMPLE). A no-op unless the recorder and the
    // sampler switch are on. Runs before any guest thread starts.
    SotcSampler::Info sampler{base, request->module_size, base + request->eh_frame_hdr,
                              request->eh_frame_hdr_size};
    SotcSampler::Start(sampler);
    return 0;
}

// Fallback for a site the patchers could not rewrite: the shadPS4 interpreter executes it
// from the exception (the red zone is not safe there, but the game keeps running).
extern "C" int sotc_handle_illegal(void* context) {
    return Core::Signals::Instance()->DispatchIllegalInstruction(context) ? 1 : 0;
}
