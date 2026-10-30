// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_TRACE_SAMPLE=<Hz>, the continuous stack sampler (trace v2, layer L3a; sotc_sampler.cpp).
// Needs SOTC_TRACE=1 or 2 (SotcTrace::kOn). Off by default; off means no thread is started.
#pragma once

#include <cstdint>
#include <string>

namespace SotcSampler {

/// The game image and its exception directory, as sotc_patch_eboot receives them.
struct Info {
    uint64_t image_base = 0;
    uint64_t image_size = 0;
    uint64_t eh_frame_hdr = 0; ///< absolute address of .eh_frame_hdr
    uint64_t eh_frame_hdr_size = 0;
};

/// Starts the sampler thread ("sotc:Sampler") when SOTC_TRACE_SAMPLE is set and the recorder is on.
/// Call once, after the image is mapped and before guest threads run (sotc_patch_eboot). Also
/// registers the STACKS and MODULES chunk providers. Returns false when nothing was started.
bool Start(const Info& info);
/// Asks the sampler thread to stop after its current step (it is not joined).
void Stop();
/// SOTC_TRACE_SAMPLE as Hz (0: off), clamped to 4000.
uint32_t ConfiguredHz();
/// Self-stats as one JSON object: rates, rounds, samples, idle skips, truncated walks, copies that
/// were not made, suspended time (total and worst per sample), stacks and dropped stacks.
std::string StatsJson();
/// STACKS chunk (TRACE-FORMAT.md type 7): the stacks interned since the last call, as a JSON
/// object, or empty when there are none. Registered with SotcTrace::RegisterChunkProvider.
std::string StacksChunk();
/// MODULES chunk (TRACE-FORMAT.md type 9): the host modules as a cumulative JSON array (name, base,
/// size, preferred_base; decimal), or empty when there are none. Registered with
/// SotcTrace::RegisterChunkProvider.
std::string ModulesChunk();

} // namespace SotcSampler
