// SPDX-FileCopyrightText: Copyright 2026 SotC-PC
// SPDX-License-Identifier: GPL-2.0-or-later

// sotc: SOTC_GPU_TIMES (sotc_gpu_times.h), the part the recording sites need: what they recorded
// between two draws or dispatches, so the "between" GPU time can be split by cause.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/types.h"

namespace Vulkan::SotcGpuTimes {

inline const bool kOn = [] {
    const char* v = std::getenv("SOTC_GPU_TIMES");
    return v && v[0] != '0';
}();
/// SOTC_GPU_TIMES_DEPS=1: also the cull dependency and reader tracking (maps updated on every GPU
/// write and read: it weighs on the draw thread, so off unless asked).
inline const bool kDeps = kOn && [] {
    const char* v = std::getenv("SOTC_GPU_TIMES_DEPS");
    return v && v[0] == '1';
}();

/// What was recorded between two ops; the "between" time is also summed per combination.
enum Cause : u32 {
    CBarrier = 1 << 0,   ///< Runtime::FlushBarriers
    CUpload = 1 << 1,    ///< buffer upload from guest memory (SynchronizeMemory)
    CTile = 1 << 2,      ///< image re-tiled into a texel buffer (SynchronizeMemoryFromImage)
    CTexUpload = 1 << 3, ///< texture upload (Runtime::UploadImage)
    CImageCopy = 1 << 4, ///< image to image copy
    CReadback = 1 << 5,  ///< readback copy to the guest
    CFill = 1 << 6,      ///< buffer fill
    CPass = 1 << 7,      ///< a new render pass (Scheduler::BeginRendering)
    CBufCopy = 1 << 8,   ///< Runtime::CopyBuffer (uploads, readbacks and others)
    CCauseBits = 9,
};
inline const char* const kCauseNames[CCauseBits] = {"barrier", "upload", "tile", "texupload",
                                                    "imagecopy", "readback", "fill", "pass",
                                                    "bufcopy"};
inline std::atomic<u32> pending_causes{0};

/// A recording site: cheap when off.
inline void Note(Cause cause) {
    if (kOn) [[unlikely]] {
        pending_causes.fetch_or(cause, std::memory_order_relaxed);
    }
}

// sotc: dependencies of the visibility (cull) dispatches, to see whether a second queue could run
// them earlier: every GPU write by a draw or dispatch (written buffers, render and depth targets,
// storage images; not the uploads of CPU data, which a second queue would carry itself) marks
// its 64 KiB pages with the op's index; a cull dispatch takes the newest mark among the pages it
// reads. The GPU time from that op's end to the cull's start is how much earlier it could run.
inline u64 op_counter = 0; ///< index of the next draw or dispatch (draw thread)
inline std::mutex deps_mutex;
inline std::unordered_map<u64, u64> page_write_op; ///< 64 KiB page -> op index that wrote it
inline u64 pending_dep = ~0ull; ///< newest write the next op reads (cull programs only)

inline bool IsDepProgram(u64 hash);

/// Pages a cull program wrote (op index), and the later reads of them by other programs: whether
/// the GPU itself consumes the visibility results (a second queue would have to hand them over).
inline std::unordered_map<u64, u64> cull_out_page;
struct Consumer {
    u64 reads = 0;
    u64 min_distance = ~0ull; ///< ops between the cull write and the read
};
inline std::unordered_map<u64, Consumer> consumers; ///< reading program (1 = a draw)

inline void NoteGpuWrite(u64 address, u64 size, u64 program = 0) {
    if (!kDeps || size == 0) [[likely]] {
        return;
    }
    const u64 first = address >> 16, last = (address + size - 1) >> 16;
    if (last - first > 4096) {
        return; // a whole-memory range, not a resource
    }
    const bool cull = program > 1 && IsDepProgram(program);
    std::scoped_lock lock{deps_mutex};
    for (u64 page = first; page <= last; ++page) {
        page_write_op[page] = op_counter;
        if (cull) {
            cull_out_page[page] = op_counter;
        }
    }
}

/// A read by `program` (ObtainBuffer): counted when it reads what a cull program wrote.
inline void NoteGpuRead(u64 address, u64 size, u64 program) {
    if (!kDeps || size == 0 || cull_out_page.empty()) [[likely]] {
        return;
    }
    if (program > 1 && IsDepProgram(program)) {
        return;
    }
    const u64 first = address >> 16, last = (address + size - 1) >> 16;
    if (last - first > 4096) {
        return;
    }
    std::scoped_lock lock{deps_mutex};
    for (u64 page = first; page <= last; ++page) {
        if (const auto it = cull_out_page.find(page); it != cull_out_page.end()) {
            auto& c = consumers[program];
            ++c.reads;
            c.min_distance = std::min(c.min_distance, op_counter - it->second);
            break;
        }
    }
}

/// Newest op that wrote any page of [address, address + size); ~0 when none.
inline u64 LastGpuWrite(u64 address, u64 size) {
    if (size == 0) {
        return ~0ull;
    }
    const u64 first = address >> 16, last = (address + size - 1) >> 16;
    if (last - first > 4096) {
        return ~0ull;
    }
    u64 newest = ~0ull;
    std::scoped_lock lock{deps_mutex};
    for (u64 page = first; page <= last; ++page) {
        if (const auto it = page_write_op.find(page); it != page_write_op.end()) {
            newest = newest == ~0ull ? it->second : std::max(newest, it->second);
        }
    }
    return newest;
}

/// The programs whose dependencies are measured (SOTC_DEP_PROGRAMS, hex list).
inline bool IsDepProgram(u64 hash) {
    static const std::vector<u64> programs = [] {
        std::vector<u64> list;
        const char* v = std::getenv("SOTC_DEP_PROGRAMS");
        std::string s = v ? v
                          : "0x50784801,0x93aec24b,0x2e0f474b,0x4defcf7f,0xf94d23d4,0x729c541e,"
                            "0xc54081d1";
        for (size_t at = 0; at < s.size();) {
            size_t end = s.find(',', at);
            if (end == std::string::npos) {
                end = s.size();
            }
            list.push_back(std::strtoull(s.substr(at, end - at).c_str(), nullptr, 16));
            at = end + 1;
        }
        return list;
    }();
    return std::find(programs.begin(), programs.end(), hash) != programs.end();
}

} // namespace Vulkan::SotcGpuTimes
