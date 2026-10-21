/* Windows implementations of compat_win.h. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <intrin.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "compat_win.h"

/* bb_jmp_buf: rbx rbp rdi rsi r12-r15 (0-56), rsp after return (64), return address (72),
 * xmm6-15 (80-239), mxcsr (240), x87 control word (244). */
__asm__(".text\n"
        ".globl bb_setjmp\n"
        ".def bb_setjmp; .scl 2; .type 32; .endef\n"
        "bb_setjmp:\n"
        "    mov %rbx,0(%rcx)\n    mov %rbp,8(%rcx)\n    mov %rdi,16(%rcx)\n    mov %rsi,24(%rcx)\n"
        "    mov %r12,32(%rcx)\n    mov %r13,40(%rcx)\n    mov %r14,48(%rcx)\n    mov %r15,56(%rcx)\n"
        "    lea 8(%rsp),%rdx\n    mov %rdx,64(%rcx)\n    mov (%rsp),%rdx\n    mov %rdx,72(%rcx)\n"
        "    movups %xmm6,80(%rcx)\n    movups %xmm7,96(%rcx)\n    movups %xmm8,112(%rcx)\n"
        "    movups %xmm9,128(%rcx)\n    movups %xmm10,144(%rcx)\n    movups %xmm11,160(%rcx)\n"
        "    movups %xmm12,176(%rcx)\n    movups %xmm13,192(%rcx)\n    movups %xmm14,208(%rcx)\n"
        "    movups %xmm15,224(%rcx)\n    stmxcsr 240(%rcx)\n    fnstcw 244(%rcx)\n"
        "    xor %eax,%eax\n    ret\n"
        ".globl bb_longjmp\n"
        ".def bb_longjmp; .scl 2; .type 32; .endef\n"
        "bb_longjmp:\n"
        "    mov %edx,%eax\n    test %eax,%eax\n    jnz 1f\n    mov $1,%eax\n"
        "1:  mov 0(%rcx),%rbx\n    mov 8(%rcx),%rbp\n    mov 16(%rcx),%rdi\n    mov 24(%rcx),%rsi\n"
        "    mov 32(%rcx),%r12\n    mov 40(%rcx),%r13\n    mov 48(%rcx),%r14\n    mov 56(%rcx),%r15\n"
        "    movups 80(%rcx),%xmm6\n    movups 96(%rcx),%xmm7\n    movups 112(%rcx),%xmm8\n"
        "    movups 128(%rcx),%xmm9\n    movups 144(%rcx),%xmm10\n    movups 160(%rcx),%xmm11\n"
        "    movups 176(%rcx),%xmm12\n    movups 192(%rcx),%xmm13\n    movups 208(%rcx),%xmm14\n"
        "    movups 224(%rcx),%xmm15\n    ldmxcsr 240(%rcx)\n    fldcw 244(%rcx)\n"
        "    mov 64(%rcx),%rsp\n    jmp *72(%rcx)\n");

int compat_errno_from_win32(unsigned long error) {
    switch (error) {
    case ERROR_SUCCESS: return 0;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH: case ERROR_BAD_NET_NAME: case ERROR_INVALID_NAME: return ENOENT;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT: return EACCES;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return EEXIST;
    case ERROR_INVALID_HANDLE: return EBADF;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: case ERROR_COMMITMENT_LIMIT: return ENOMEM;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return ENOSPC;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    case ERROR_DIRECTORY: return ENOTDIR;
    case ERROR_FILENAME_EXCED_RANGE: case ERROR_BUFFER_OVERFLOW: return ENAMETOOLONG;
    case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
    case ERROR_NOACCESS: return EFAULT;
    case ERROR_HANDLE_EOF: return 0;
    case ERROR_NEGATIVE_SEEK: case ERROR_INVALID_PARAMETER: return EINVAL;
    default: return EIO;
    }
}

/* Positioned I/O on a synchronous handle moves the file pointer: it is restored afterwards.
 * A concurrent read() on the same descriptor would race, as with a shared offset anyway. */
static ssize_t positioned(int fd, void *buffer, size_t size, int64_t offset, int write) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (offset < 0) { errno = EINVAL; return -1; }
    LARGE_INTEGER zero = {0}, saved;
    if (!SetFilePointerEx(h, zero, &saved, FILE_CURRENT)) { errno = compat_errno_from_win32(GetLastError()); return -1; }
    size_t done = 0;
    while (done < size) {
        DWORD chunk = size - done > 0x40000000u ? 0x40000000u : (DWORD)(size - done), n = 0;
        OVERLAPPED at = {0};
        uint64_t position = (uint64_t)offset + done;
        at.Offset = (DWORD)position; at.OffsetHigh = (DWORD)(position >> 32);
        BOOL ok = write ? WriteFile(h, (const char *)buffer + done, chunk, &n, &at)
                        : ReadFile(h, (char *)buffer + done, chunk, &n, &at);
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF) break;
            SetFilePointerEx(h, saved, NULL, FILE_BEGIN);
            if (done) return (ssize_t)done;
            errno = compat_errno_from_win32(e);
            return -1;
        }
        done += n;
        if (n < chunk) break;
    }
    SetFilePointerEx(h, saved, NULL, FILE_BEGIN);
    return (ssize_t)done;
}
ssize_t compat_pread(int fd, void *buffer, size_t size, int64_t offset) { return positioned(fd, buffer, size, offset, 0); }
ssize_t compat_pwrite(int fd, const void *buffer, size_t size, int64_t offset) { return positioned(fd, (void *)buffer, size, offset, 1); }

long compat_utc_offset(time_t t) {
    struct tm local, utc;
    if (localtime_s(&local, &t) || gmtime_s(&utc, &t)) return 0;
    /* mktime interprets utc as local time: the difference is the zone offset (with DST). */
    utc.tm_isdst = local.tm_isdst;
    time_t as_local = mktime(&utc);
    return as_local == (time_t)-1 ? 0 : (long)(t - as_local);
}

int compat_random(void *buffer, size_t size) {
    while (size) {
        ULONG chunk = size > 0x10000000u ? 0x10000000u : (ULONG)size;
        if (BCryptGenRandom(NULL, buffer, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) return -1;
        buffer = (char *)buffer + chunk; size -= chunk;
    }
    return 0;
}

/* One high-resolution timer per thread (Windows 10 1803+); a plain waitable timer before. */
static __thread HANDLE sleep_timer;
void compat_sleep_ns(uint64_t ns) {
    if (!ns) { SwitchToThread(); return; }
    if (!sleep_timer) {
        sleep_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!sleep_timer) sleep_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)((ns + 99) / 100); /* relative, 100 ns units */
    if (sleep_timer && SetWaitableTimer(sleep_timer, &due, 0, NULL, NULL, FALSE))
        WaitForSingleObject(sleep_timer, INFINITE);
    else
        Sleep((DWORD)((ns + 999999) / 1000000));
}

int compat_remove_tree(const char *path) {
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return 0;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesA(path, attributes & ~FILE_ATTRIBUTE_READONLY);
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) return RemoveDirectoryA(path) ? 0 : -1;
        return DeleteFileA(path) ? 0 : -1;
    }
    char pattern[MAX_PATH * 4];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA entry;
    HANDLE find = FindFirstFileA(pattern, &entry);
    int result = 0;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(entry.cFileName, ".") || !strcmp(entry.cFileName, "..")) continue;
            char child[MAX_PATH * 4];
            snprintf(child, sizeof(child), "%s\\%s", path, entry.cFileName);
            if (compat_remove_tree(child)) result = -1;
        } while (FindNextFileA(find, &entry));
        FindClose(find);
    }
    if (!RemoveDirectoryA(path)) result = -1;
    return result;
}

int compat_cpu_times(int who, int64_t *user_us, int64_t *kernel_us) {
    FILETIME created, exited, kernel, user;
    BOOL ok = who ? GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)
                  : GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    if (!ok) return -1;
    *user_us = (int64_t)(((uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime) / 10);
    *kernel_us = (int64_t)(((uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) / 10);
    return 0;
}

/* Cross-core TSC check. A helper thread on the first core and this thread on each other core
 * take turns publishing rdtsc: a value read after the other core published a larger one means
 * the TSC ran backwards between the two cores. QPC is not used for the comparison, since on
 * many systems it is the TSC itself. */
static volatile LONG64 skew_stamp;
static volatile LONG skew_state, skew_quit;
static int64_t skew_back;
static int pin_to(unsigned core) {
    if (!SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << core)) return 0;
    for (int i = 0; i < 1000 && GetCurrentProcessorNumber() != core; ++i) SwitchToThread();
    return GetCurrentProcessorNumber() == core;
}
static int skew_wait(LONG state) {
    for (uint64_t spins = 0; skew_state != state; ++spins) {
        if (spins > (UINT64_C(1) << 28)) return 0;
        _mm_pause();
    }
    return 1;
}
static DWORD WINAPI skew_helper(void *core) {
    pin_to((unsigned)(uintptr_t)core);
    for (;;) {
        LONG state;
        while ((state = skew_state) != 1 && state != 3) {
            if (skew_quit) return 0;
            _mm_pause();
        }
        if (state == 1) {
            skew_stamp = (LONG64)__rdtsc();
            skew_state = 2;
        } else {
            _mm_lfence();
            const int64_t d = (int64_t)(__rdtsc() - (uint64_t)skew_stamp);
            if (d < skew_back) skew_back = d;
            skew_state = 0;
        }
    }
}

int compat_tsc_skew(unsigned *worst_core, double *worst_us) {
    *worst_core = 0; *worst_us = 0;
    HANDLE thread = GetCurrentThread();
    DWORD_PTR process, system;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system) || !process) return -1;
    unsigned cores[64]; int n = 0;
    for (unsigned i = 0; i < 64; ++i) if (process >> i & 1) cores[n++] = i;
    if (n < 2) return 0;
    DWORD_PTR saved = SetThreadAffinityMask(thread, (DWORD_PTR)1 << cores[0]);
    if (!saved) return -1;
    /* TSC frequency on one core, to report the offsets in microseconds. */
    LARGE_INTEGER frequency, q0, q1;
    QueryPerformanceFrequency(&frequency);
    int result = -1;
    if (!pin_to(cores[0])) goto done;
    QueryPerformanceCounter(&q0); uint64_t t0 = __rdtsc();
    compat_sleep_ns(10000000);
    QueryPerformanceCounter(&q1); uint64_t t1 = __rdtsc();
    if (q1.QuadPart <= q0.QuadPart || t1 <= t0) goto done;
    const double ticks_per_us = (double)(t1 - t0) * (double)frequency.QuadPart /
                                (double)(q1.QuadPart - q0.QuadPart) / 1e6;
    skew_state = 0; skew_quit = 0;
    HANDLE helper = CreateThread(NULL, 0, skew_helper, (void *)(uintptr_t)cores[0], 0, NULL);
    if (!helper) goto done;
    result = 0;
    for (int c = 1; c < n && result >= 0; ++c) {
        if (!pin_to(cores[c])) continue;
        int64_t forward = INT64_MAX;
        skew_back = INT64_MAX;
        for (int round = 0; round < 64; ++round) {
            skew_state = 1;
            if (!skew_wait(2)) { result = -1; break; }
            _mm_lfence();
            const int64_t d = (int64_t)(__rdtsc() - (uint64_t)skew_stamp);
            if (d < forward) forward = d;
            skew_stamp = (LONG64)__rdtsc();
            skew_state = 3;
            if (!skew_wait(0)) { result = -1; break; }
        }
        if (result < 0) break;
        /* forward = latency + (offset_c - offset_0), skew_back = latency + (offset_0 - offset_c). */
        if (forward < -(int64_t)ticks_per_us || skew_back < -(int64_t)ticks_per_us) result = 1;
        const double offset_us = ((double)forward - (double)skew_back) / 2 / ticks_per_us;
        if (fabs(offset_us) > fabs(*worst_us)) { *worst_us = offset_us; *worst_core = cores[c]; }
    }
    skew_quit = 1;
    WaitForSingleObject(helper, INFINITE);
    CloseHandle(helper);
done:
    SetThreadAffinityMask(thread, saved);
    return result;
}

/* QPC scaled to about 1 GHz: the same on every core. */
static uint64_t qpc_scale(void) {
    static uint64_t scale;
    if (!scale) {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        scale = f.QuadPart > 0 && f.QuadPart < 1000000000 ? 1000000000 / (uint64_t)f.QuadPart : 1;
    }
    return scale;
}
uint64_t compat_qpc_tsc(void) {
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    return (uint64_t)q.QuadPart * qpc_scale();
}
uint64_t compat_qpc_tsc_frequency(void) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    return (uint64_t)f.QuadPart * qpc_scale();
}
#endif
