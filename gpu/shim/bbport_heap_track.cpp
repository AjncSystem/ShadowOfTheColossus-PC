// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: live-heap tracker for leak hunting, built only with -DBB_HEAP_TRACK=ON (Windows). The link
// wraps malloc/calloc/realloc/free and this file routes operator new/delete through them. One
// pointer in 8 (chosen by a hash of its address, so free knows without a lookup) is recorded with
// its call stack. With SOTC_HEAP_TRACK=<file> set, a thread appends the call stacks holding the
// most live sampled bytes to that file every SOTC_HEAP_TRACK_SECONDS (default 60). Addresses are
// printed as module + offset; tools/heap_track_report.py diffs two reports and symbolizes them.
// The shim glob also puts this file in bbgpu, where BB_HEAP_TRACK is not defined: empty there.
#if defined(_WIN32) && defined(BB_HEAP_TRACK)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" {
void* __real_malloc(size_t size);
void* __real_calloc(size_t count, size_t size);
void* __real_realloc(void* pointer, size_t size);
void __real_free(void* pointer);
}

namespace {

constexpr int Frames = 14;
constexpr uint32_t SampleShift = 3; // one pointer in 8
constexpr size_t TableSize = size_t{1} << 23;
constexpr uint32_t StackTableSize = 1u << 16;

struct Entry {
    uintptr_t pointer;
    uint32_t size;
    uint32_t stack;
};
struct Stack {
    uint32_t hash;
    uint32_t depth;
    void* frames[Frames];
};

std::atomic<bool> enabled{false};
SRWLOCK lock = SRWLOCK_INIT;
Entry* table;   // open addressing, linear probing, backward-shift deletion; pointer 0 = empty
Stack* stacks;  // id 0 is "no stack" (table full)
uint32_t stack_count = 1;
thread_local bool inside = false;

uint64_t Mix(uintptr_t pointer) {
    return (static_cast<uint64_t>(pointer) >> 4) * 0x9E3779B97F4A7C15ull;
}
bool Sampled(const void* pointer) {
    return (Mix(reinterpret_cast<uintptr_t>(pointer)) >> (64 - SampleShift)) == 0;
}
size_t Slot(uintptr_t pointer) {
    return (Mix(pointer) >> 20) & (TableSize - 1);
}

uint32_t InternStack(void* const* frames, uint32_t depth, uint32_t hash) {
    // Stacks are never removed; probe by hash in the same fixed table.
    for (uint32_t i = 0, s = hash & (StackTableSize - 1); i < StackTableSize;
         ++i, s = (s + 1) & (StackTableSize - 1)) {
        Stack& stack = stacks[s];
        if (stack.depth == 0) {
            if (stack_count >= StackTableSize / 2) {
                return 0;
            }
            ++stack_count;
            stack.hash = hash;
            stack.depth = depth;
            std::memcpy(stack.frames, frames, depth * sizeof(void*));
            return s;
        }
        if (stack.hash == hash && stack.depth == depth &&
            std::memcmp(stack.frames, frames, depth * sizeof(void*)) == 0) {
            return s;
        }
    }
    return 0;
}

void Remove(uintptr_t pointer) {
    size_t s = Slot(pointer);
    while (table[s].pointer != pointer) {
        if (table[s].pointer == 0) {
            return;
        }
        s = (s + 1) & (TableSize - 1);
    }
    // Backward-shift deletion keeps every probe chain unbroken.
    size_t hole = s;
    for (size_t next = (hole + 1) & (TableSize - 1); table[next].pointer != 0;
         next = (next + 1) & (TableSize - 1)) {
        const size_t home = Slot(table[next].pointer);
        if (((next - home) & (TableSize - 1)) >= ((next - hole) & (TableSize - 1))) {
            table[hole] = table[next];
            hole = next;
        }
    }
    table[hole] = {};
}

void Track(void* pointer, size_t size) {
    if (!enabled.load(std::memory_order_relaxed) || !Sampled(pointer) || inside) {
        return;
    }
    inside = true;
    void* frames[Frames];
    ULONG hash = 0;
    const uint32_t depth = RtlCaptureStackBackTrace(2, Frames, frames, &hash);
    AcquireSRWLockExclusive(&lock);
    const uintptr_t key = reinterpret_cast<uintptr_t>(pointer);
    Remove(key); // a pointer freed by a path this file does not see
    const uint32_t stack = depth ? InternStack(frames, depth, hash) : 0;
    size_t s = Slot(key);
    for (size_t i = 0; i < TableSize && table[s].pointer != 0; ++i) {
        s = (s + 1) & (TableSize - 1);
    }
    if (table[s].pointer == 0) {
        table[s] = {key, static_cast<uint32_t>(std::min<size_t>(size, UINT32_MAX)), stack};
    }
    ReleaseSRWLockExclusive(&lock);
    inside = false;
}

void Untrack(void* pointer) {
    if (!enabled.load(std::memory_order_relaxed) || !Sampled(pointer)) {
        return;
    }
    AcquireSRWLockExclusive(&lock);
    Remove(reinterpret_cast<uintptr_t>(pointer));
    ReleaseSRWLockExclusive(&lock);
}

void PrintFrame(FILE* out, void* frame) {
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(frame), &module) &&
        GetModuleFileNameA(module, name, sizeof(name))) {
        const char* base = std::max(std::strrchr(name, '\\'), std::strrchr(name, '/'));
        std::fprintf(out, " %s+0x%llx", base ? base + 1 : name,
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frame) -
                                                     reinterpret_cast<uintptr_t>(module)));
    } else {
        std::fprintf(out, " 0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frame)));
    }
}

DWORD WINAPI Reporter(void* parameter) {
    const char* path = static_cast<const char*>(parameter);
    const char* seconds_env = std::getenv("SOTC_HEAP_TRACK_SECONDS");
    const DWORD seconds = seconds_env ? std::max(5, std::atoi(seconds_env)) : 60;
    // Preallocated so the report allocates nothing while it holds the lock.
    auto* bytes = static_cast<uint64_t*>(VirtualAlloc(nullptr, StackTableSize * 16, MEM_COMMIT, PAGE_READWRITE));
    uint64_t* counts = bytes + StackTableSize;
    auto* order = static_cast<uint32_t*>(VirtualAlloc(nullptr, StackTableSize * 4, MEM_COMMIT, PAGE_READWRITE));
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        Sleep(seconds * 1000);
        std::memset(bytes, 0, StackTableSize * 16);
        uint64_t total = 0, number = 0;
        AcquireSRWLockShared(&lock);
        for (size_t i = 0; i < TableSize; ++i) {
            if (table[i].pointer) {
                bytes[table[i].stack] += table[i].size;
                ++counts[table[i].stack];
                total += table[i].size;
                ++number;
            }
        }
        ReleaseSRWLockShared(&lock);
        uint32_t used = 0;
        for (uint32_t s = 0; s < StackTableSize; ++s) {
            if (counts[s]) {
                order[used++] = s;
            }
        }
        const uint32_t top = std::min<uint32_t>(used, 60);
        std::partial_sort(order, order + top, order + used,
                          [&](uint32_t a, uint32_t b) { return bytes[a] > bytes[b]; });
        inside = true;
        if (FILE* out = std::fopen(path, "a")) {
            constexpr uint64_t Scale = 1u << SampleShift;
            std::fprintf(out, "=== t %llu s, live %llu bytes in %llu allocations (estimated), %u stacks\n",
                         (GetTickCount64() - start) / 1000, static_cast<unsigned long long>(total * Scale),
                         static_cast<unsigned long long>(number * Scale), stack_count);
            for (uint32_t i = 0; i < top; ++i) {
                const uint32_t s = order[i];
                std::fprintf(out, "%llu %llu |", static_cast<unsigned long long>(bytes[s] * Scale),
                             static_cast<unsigned long long>(counts[s] * Scale));
                for (uint32_t f = 0; s && f < stacks[s].depth; ++f) {
                    PrintFrame(out, stacks[s].frames[f]);
                }
                std::fputc('\n', out);
            }
            std::fclose(out);
        }
        inside = false;
    }
}

struct Init {
    Init() {
        const char* path = std::getenv("SOTC_HEAP_TRACK");
        if (!path || !*path) {
            return;
        }
        table = static_cast<Entry*>(VirtualAlloc(nullptr, TableSize * sizeof(Entry), MEM_COMMIT, PAGE_READWRITE));
        stacks = static_cast<Stack*>(VirtualAlloc(nullptr, StackTableSize * sizeof(Stack), MEM_COMMIT, PAGE_READWRITE));
        if (!table || !stacks) {
            return;
        }
        enabled.store(true);
        CreateThread(nullptr, 0, Reporter, const_cast<char*>(path), 0, nullptr);
    }
} init;

} // namespace

extern "C" {
void* __wrap_malloc(size_t size) {
    void* pointer = __real_malloc(size);
    if (pointer) {
        Track(pointer, size);
    }
    return pointer;
}
void* __wrap_calloc(size_t count, size_t size) {
    void* pointer = __real_calloc(count, size);
    if (pointer) {
        Track(pointer, count * size);
    }
    return pointer;
}
void* __wrap_realloc(void* old, size_t size) {
    if (old) {
        Untrack(old);
    }
    void* pointer = __real_realloc(old, size);
    if (pointer) {
        Track(pointer, size);
    }
    return pointer;
}
void __wrap_free(void* pointer) {
    if (pointer) {
        Untrack(pointer);
    }
    __real_free(pointer);
}
}

void* operator new(size_t size) {
    if (void* pointer = __wrap_malloc(size ? size : 1)) {
        return pointer;
    }
    throw std::bad_alloc();
}
void* operator new[](size_t size) {
    return operator new(size);
}
void* operator new(size_t size, const std::nothrow_t&) noexcept {
    return __wrap_malloc(size ? size : 1);
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    return __wrap_malloc(size ? size : 1);
}
void operator delete(void* pointer) noexcept {
    __wrap_free(pointer);
}
void operator delete[](void* pointer) noexcept {
    __wrap_free(pointer);
}
void operator delete(void* pointer, size_t) noexcept {
    __wrap_free(pointer);
}
void operator delete[](void* pointer, size_t) noexcept {
    __wrap_free(pointer);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    __wrap_free(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    __wrap_free(pointer);
}
#endif
