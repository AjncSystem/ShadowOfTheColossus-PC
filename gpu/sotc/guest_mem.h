// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: safe guest memory access for the SotC Lab modules (free-cam, engine commands, probes).
// Reads and writes go through the loader's unprotected backing view (runtime_memory.c), so they
// neither fault on GPU-tracked pages nor mark them dirty. Every access checks that the whole range
// lies in one mapped region; a failed check returns false and nothing is guessed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
// runtime_memory.c: the unprotected backing view of a guest address, and the mapped region of one.
const unsigned char* runtime_memory_backing_pointer(uintptr_t address);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
}

namespace SotcGuest {

/// The writable backing pointer of [address, address + n), or nullptr when the range is not
/// inside one mapped region.
inline unsigned char* Backing(uint64_t address, size_t n) {
    if (address == 0 || n == 0) {
        return nullptr;
    }
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    if (!runtime_memory_region(static_cast<uintptr_t>(address), &start, &end, &mapped) || !mapped ||
        address < start || address + n > end) {
        return nullptr;
    }
    return const_cast<unsigned char*>(
        runtime_memory_backing_pointer(static_cast<uintptr_t>(address)));
}

inline bool Read(uint64_t address, void* out, size_t n) {
    const unsigned char* p = Backing(address, n);
    if (!p) {
        return false;
    }
    std::memcpy(out, p, n);
    return true;
}

inline bool Write(uint64_t address, const void* in, size_t n) {
    unsigned char* p = Backing(address, n);
    if (!p) {
        return false;
    }
    std::memcpy(p, in, n);
    return true;
}

template <typename T>
inline bool Read(uint64_t address, T* out) {
    return Read(address, out, sizeof(T));
}

template <typename T>
inline bool Write(uint64_t address, const T& value) {
    return Write(address, &value, sizeof(T));
}

/// Follows a pointer chain: base, then *(base + offs[0]), ... ; the last offset is added, not
/// dereferenced. Guest pointers are 64-bit slots holding addresses below 2 GiB. Returns 0 on a
/// failed read or a null link.
inline uint64_t Chase(uint64_t base, const int64_t* offs, size_t count) {
    uint64_t p = base;
    for (size_t i = 0; i + 1 < count; ++i) {
        uint64_t next = 0;
        if (!Read(p + offs[i], &next) || next == 0) {
            return 0;
        }
        p = next;
    }
    return count ? p + offs[count - 1] : p;
}

} // namespace SotcGuest
