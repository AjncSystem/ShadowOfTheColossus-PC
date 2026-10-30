// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: GPU write extents of shader-written storage buffers.
//
// BindBuffers (vk_rasterizer.cpp) marks the whole V# range of every written storage binding as
// GPU-modified (BufferCache::ObtainBuffer is_written), so a CPU read of any byte of it needs a GPU
// round trip even when the shader never wrote that byte. Two parts, both header-only:
//
// 1. Counter (SOTC_FPS, no behaviour change). Every marked range is remembered in a bounded ring
//    together with the shader hash and the binding index; every guest readback request
//    (BufferCache::ReadMemory) is attributed to the newest ring entry that contains its address.
//    Printed every 5 s by the SOTC_FPS report ("SOTC GPU writes by shader" line, driver.cpp):
//    per (program hash, binding index) the bytes marked, the number of marks, the readbacks, the
//    offsets (from the V# base) the readbacks asked for, and the size of the binding and the
//    thread count of the last dispatch. The binding index is the position in Info::buffers.
//
// 2. SOTC_WRITE_EXTENT=1 (off by default): for the listed (hash, binding) pairs of DIRECT compute
//    dispatches only, the range marked as GPU-modified is the window that the rule proves the
//    shader can write, instead of the whole V# range. Rules come from user/write_extent.txt
//    (SOTC_WRITE_EXTENT_FILE overrides the path), one per line:
//
//        <hash> <binding> [offset [fixed [per_thread [per_group]]]]     # comment
//
//    hash: program hash as printed by the counter (0x...). binding: index in Info::buffers.
//    All numbers are bytes (decimal or 0x...). The window is
//        [offset, offset + fixed + per_thread * threads + per_group * groups)
//    from the V# base, clamped to the V# size; threads = groups x local size of the dispatch.
//    PROOF OBLIGATION of the person who lists a shader: for every dispatch of that shader (all
//    its permutations, every dispatch dimension the game uses) no store/atomic of that binding
//    falls outside the window. Typical case: thread i writes element i of an array of 4-byte
//    elements: per_thread = 4. A thread writing one bit of a mask needs less (per_thread = 4 is
//    then a safe upper bound). A larger window is always safe; a smaller one makes CPU reads of
//    the bytes outside it stale.
//    Never listed: graphics stages, indirect dispatches, formatted (texel) buffers, special
//    buffers, anything with a non-zero dispatch start; those keep the full range.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace SotcWriteExtent {

/// SOTC_WRITE_EXTENT=1: narrow the marked range of the listed shaders (part 2).
inline const bool kEnabled = [] {
    const char* v = std::getenv("SOTC_WRITE_EXTENT");
    return v && v[0] == '1';
}();

struct Rule {
    uint64_t hash;
    uint32_t binding;
    uint64_t offset, fixed, per_thread, per_group;
};

inline const std::vector<Rule>& Rules() {
    static const std::vector<Rule> rules = [] {
        std::vector<Rule> out;
        if (!kEnabled) {
            return out;
        }
        const char* path = std::getenv("SOTC_WRITE_EXTENT_FILE");
        if (!path || !path[0]) {
            path = "user/write_extent.txt";
        }
        std::FILE* f = std::fopen(path, "r");
        if (!f) {
            std::fprintf(stderr, "SOTC write extent: no rule file %s, nothing narrowed\n", path);
            return out;
        }
        char line[512];
        while (std::fgets(line, sizeof(line), f)) {
            if (char* hash = std::strchr(line, '#')) {
                *hash = 0;
            }
            unsigned long long v[6]{};
            int n = 0;
            for (char* p = line; n < 6;) {
                char* end = nullptr;
                const unsigned long long x = std::strtoull(p, &end, 0);
                if (end == p) {
                    break;
                }
                v[n++] = x;
                p = end;
            }
            if (n < 2) {
                continue;
            }
            out.push_back({v[0], static_cast<uint32_t>(v[1]), v[2], v[3], v[4], v[5]});
        }
        std::fclose(f);
        std::fprintf(stderr, "SOTC write extent: %zu rule(s) from %s\n", out.size(), path);
        return out;
    }();
    return rules;
}

/// The window [off, off + len) of the V# (size bytes) that may be marked GPU-written by this
/// dispatch of the shader's binding, or false when the full range has to be kept (no rule, or the
/// rule's window covers the whole binding anyway).
inline bool Narrow(uint64_t hash, uint32_t binding, uint64_t groups, uint64_t threads,
                   uint64_t size, uint64_t& off, uint64_t& len) {
    for (const Rule& r : Rules()) {
        if (r.hash != hash || r.binding != binding) {
            continue;
        }
        // Saturating arithmetic: an overflow means "everything".
        const auto mul = [](uint64_t a, uint64_t b) {
            unsigned __int128 p = static_cast<unsigned __int128>(a) * b;
            return p > ~uint64_t{0} ? ~uint64_t{0} : static_cast<uint64_t>(p);
        };
        const auto add = [](uint64_t a, uint64_t b) {
            return a > ~uint64_t{0} - b ? ~uint64_t{0} : a + b;
        };
        const uint64_t extent = add(add(r.fixed, mul(r.per_thread, threads)), mul(r.per_group, groups));
        if (extent == 0 || r.offset >= size) {
            return false;
        }
        off = r.offset;
        len = std::min(extent, size - off);
        return len < size; // nothing gained when the window is the whole binding
    }
    return false;
}

// ---- counter (SOTC_FPS) -------------------------------------------------------------------

struct Entry {
    uint64_t hash{};
    uint32_t binding{};
    uint64_t marked_bytes{}, marks{}, readbacks{};
    uint64_t range_bytes{}, threads{};
    uint64_t min_off = ~uint64_t{0}, max_off{};
    uint64_t narrowed_marks{};
};

struct RingItem {
    uintptr_t addr; ///< start of the marked range
    uint64_t size;
    uintptr_t base; ///< V# base address (read offsets are reported from it)
    uint32_t entry;
};

struct State {
    static constexpr size_t MaxEntries = 512;
    static constexpr size_t RingSize = 4096;
    std::mutex mutex;
    std::vector<Entry> entries;
    std::vector<RingItem> ring;
    size_t ring_next = 0;
    uint64_t unattributed = 0;
    uint64_t total_marked = 0;
    State() {
        entries.reserve(MaxEntries);
        ring.reserve(RingSize);
    }
};

inline State& St() {
    static State state;
    return state;
}

/// BindBuffers, a written binding was marked: [mark_addr, mark_addr + mark_size) of the V#
/// base .. base + range_bytes. Cheap and bounded; call only with SOTC_FPS.
inline void NoteMark(uint64_t hash, uint32_t binding, uintptr_t base, uint64_t range_bytes,
                     uintptr_t mark_addr, uint64_t mark_size, uint64_t threads) {
    State& s = St();
    std::scoped_lock lock{s.mutex};
    size_t index = s.entries.size();
    for (size_t i = 0; i < s.entries.size(); ++i) {
        if (s.entries[i].hash == hash && s.entries[i].binding == binding) {
            index = i;
            break;
        }
    }
    if (index == s.entries.size()) {
        if (index >= State::MaxEntries) {
            return;
        }
        s.entries.push_back({.hash = hash, .binding = binding});
    }
    Entry& e = s.entries[index];
    e.marked_bytes += mark_size;
    ++e.marks;
    e.narrowed_marks += mark_size < range_bytes;
    e.range_bytes = range_bytes;
    e.threads = threads;
    s.total_marked += mark_size;
    const RingItem item{mark_addr, mark_size, base, static_cast<uint32_t>(index)};
    if (s.ring.size() < State::RingSize) {
        s.ring.push_back(item);
    } else {
        s.ring[s.ring_next] = item;
    }
    s.ring_next = (s.ring_next + 1) % State::RingSize;
}

/// BufferCache::ReadMemory: a guest read of [addr, addr + size) needs a readback. Attributed to
/// the newest marked range that contains addr.
inline void NoteReadback(uintptr_t addr) {
    State& s = St();
    std::scoped_lock lock{s.mutex};
    const size_t n = s.ring.size();
    for (size_t k = 0; k < n; ++k) {
        const size_t i = (s.ring_next + n - 1 - k) % n; // newest first
        const RingItem& item = s.ring[i];
        if (addr >= item.addr && addr < item.addr + item.size) {
            Entry& e = s.entries[item.entry];
            ++e.readbacks;
            const uint64_t off = addr - item.base;
            e.min_off = std::min(e.min_off, off);
            e.max_off = std::max(e.max_off, off);
            return;
        }
    }
    ++s.unattributed;
}

/// The report text (top 8 by readbacks) of the period; starts a new period. Empty when nothing
/// was read back or marked.
inline void Take(char* out, size_t out_size) {
    out[0] = 0;
    State& s = St();
    std::scoped_lock lock{s.mutex};
    std::vector<size_t> order;
    for (size_t i = 0; i < s.entries.size(); ++i) {
        if (s.entries[i].readbacks != 0) {
            order.push_back(i);
        }
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return s.entries[a].readbacks > s.entries[b].readbacks;
    });
    int used = 0;
    const auto put = [&](const char* fmt, auto... args) {
        if (used >= 0 && static_cast<size_t>(used) < out_size) {
            used += std::snprintf(out + used, out_size - used, fmt, args...);
        }
    };
    for (size_t k = 0; k < order.size() && k < 8; ++k) {
        const Entry& e = s.entries[order[k]];
        put("%s%#llx/b%u marked %.2f MB (%llu marks%s, binding %.1f KiB, %llu threads), "
            "readbacks %llu at +%llu..+%llu",
            k ? "; " : "", static_cast<unsigned long long>(e.hash), e.binding,
            e.marked_bytes / 1048576.0, static_cast<unsigned long long>(e.marks),
            e.narrowed_marks ? ", narrowed" : "", e.range_bytes / 1024.0,
            static_cast<unsigned long long>(e.threads),
            static_cast<unsigned long long>(e.readbacks),
            static_cast<unsigned long long>(e.min_off), static_cast<unsigned long long>(e.max_off));
    }
    if (!order.empty() || s.unattributed != 0) {
        put("%s total marked %.1f MB, readbacks outside any mark %llu", order.empty() ? "" : "; ",
            s.total_marked / 1048576.0, static_cast<unsigned long long>(s.unattributed));
    }
    for (Entry& e : s.entries) {
        e.marked_bytes = e.marks = e.readbacks = e.narrowed_marks = 0;
        e.min_off = ~uint64_t{0};
        e.max_off = 0;
    }
    s.unattributed = 0;
    s.total_marked = 0;
}

} // namespace SotcWriteExtent
