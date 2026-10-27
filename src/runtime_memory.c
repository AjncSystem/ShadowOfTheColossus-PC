/* PS4 virtual memory: direct (physical) pool, flexible memory, reservations,
 * protection changes and queries. One sparse memfd backs the whole direct
 * pool, so any mapping of any physical range aliases the same storage.
 * Windows: a pagefile-backed section plays the memfd's part, and the PS4 user range is
 * reserved as one placeholder that views of the section replace (VirtualAlloc2/MapViewOfFile3,
 * as shadPS4 does); see the "host mappings" section below.
 * Guest addresses are placed below 1 TiB (PS4 user range): GPU descriptors
 * encode 40-bit addresses, so host-default 0x7f... addresses would not fit. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
/* sceKernelGetDirectMemorySize on retail PS4: 5056 MiB. BB_DMEM_MB raises it (the resolution
 * patches above 1080p need about 4 GiB more; run.sh sets it). A PS4 Pro (SOTC_NEO=1) gives games
 * 640 MiB more (shadPS4: 5888 vs 5248 MiB in total); with the base size the game's 64 KiB page
 * bitmap filled up when a save loaded in Pro mode and its allocator read past it (guest offset
 * 0x1a371, read of -1). */
int runtime_sotc_neo(void);
static uint64_t pool_size_mb;
static uint64_t pool_base_mb(void) { return runtime_sotc_neo() ? 5056 + 640 : 5056; }
static uint64_t pool_size_bytes(void) {
    if (!pool_size_mb) {
        const char *env = getenv("BB_DMEM_MB");
        const uint64_t base = pool_base_mb();
        uint64_t mb = env ? strtoull(env, NULL, 10) : 0;
        if (mb < base || mb > 16384) mb = base;
        pool_size_mb = mb;
    }
    return pool_size_mb * 1024 * 1024;
}
#define POOL_SIZE pool_size_bytes()
#define FLEXIBLE_SIZE (UINT64_C(448) * 1024 * 1024)
/* sotc: bytes of the pagefile-backed guest section, guest-pool part (POOL_SIZE after any
 * BB_DMEM_MB fallback): RAM telemetry of the stats printer. The section also carries the flexible
 * span (flex_span); 0 where the pool is a memfd (no page file there). */
uint64_t runtime_memory_guest_section_bytes(void) {
#ifdef _WIN32
    return POOL_SIZE;
#else
    return 0;
#endif
}
#define PAGE UINT64_C(16384)
#define USER_MIN UINT64_C(0x1000000000)
#define USER_MAX UINT64_C(0xfc00000000)
#define SYSTEM_LOW_MIN UINT64_C(0x0700000000)
#define SYSTEM_LOW_MAX UINT64_C(0x0800000000)
#define LIMIT 4096
#define INVALID ((int32_t)UINT32_C(0x80020016))
#define NO_MEMORY ((int32_t)UINT32_C(0x8002000c))
#define ACCESS ((int32_t)UINT32_C(0x8002000d))
#define NO_SPACE ((int32_t)UINT32_C(0x80020023))
#define MAP_FIXED_FLAG 0x10
#define MAP_NO_OVERWRITE 0x80
enum { KIND_RESERVED=1, KIND_DIRECT, KIND_FLEXIBLE };
typedef struct { uint64_t start, size; int type, used; } Block;
typedef struct { uintptr_t start, end; int kind, prot, type; uint64_t phys; } Vma;
typedef struct {
    uintptr_t start, end; uint64_t offset; int32_t protection, memory_type;
    uint32_t flags; char name[32];
} VirtualQueryInfo; /* flags: flexible, direct, stack, pooled, committed */
typedef struct { void *start; uint64_t offset, length; int8_t prot, type; int16_t reserved; int32_t operation; } BatchEntry;
_Static_assert(sizeof(VirtualQueryInfo)==72,"PS4 virtual query info layout");
_Static_assert(sizeof(BatchEntry)==32,"PS4 batch map entry layout");

/* sotc: waits taken by the lock's slow backoff (>256 spins) and the time they lasted, so a run
 * shows whether the region lock ever costs a draw thread a frame (T2/C4). The counters (and this
 * C-linkage updater) live in gpu/shadps4/video_core/sotc_stats.h: no data symbol crosses the
 * runtime/GPU library boundary, only a call. */
void sotc_lock_slow_stat(int draw, uint64_t ns);

/* Region table lock. Lookups (GPU per-draw queries, VirtualQuery, GPU page
 * protection) share it; map/unmap/protect take it exclusively. Both nest: code
 * under the lock may write guest memory, and a GPU-tracked page then faults into
 * runtime_memory_gpu_protect on the same thread. Readers nest because glibc's
 * default rwlock prefers readers; a thread holding it exclusively skips relocking. */
#ifdef _WIN32
/* sotc: winpthreads' rwlock queues new readers behind a waiting writer, so a nested read (a
 * reader faulting into runtime_memory_gpu_protect) deadlocked against a mapping thread
 * waiting to write: the GPU command processor, two job workers in the fault handler and
 * sceKernelBatchMap2 all stopped. Each read also took a mutex, thousands of times per frame.
 * This lock prefers readers like glibc's: a read is one atomic operation and never waits for
 * a waiting writer; a writer waits until no reader holds it (maps and unmaps are rare). */
extern uint64_t runtime_disabled_optimizations; /* defined below (BB_TOGGLE_FILE mask) */
static volatile int64_t lock_state; /* readers, or -1 while a writer holds it */
/* sotc: sleepers on the lock word (bit 61 keeps the old Sleep(1) backoff and the default timer
 * resolution). Sleep(1) is 15.6 ms without a raised timer resolution, so a draw thread that hit
 * a guest map/mprotect could lose a whole frame. WaitOnAddress wakes it as soon as the word
 * changes instead. */
static volatile long lock_waiters;
/* sotc: the GPU draw thread (liverpool.cpp BackLoop), for the _draw split of the slow waits. */
static volatile unsigned long lock_draw_thread;
void runtime_memory_mark_draw_thread(void) { lock_draw_thread=GetCurrentThreadId(); }
/* sotc: QPC ticks to ns; the frequency is constant, a racing first read stores the same value. */
static uint64_t lock_ticks_to_ns(LONGLONG ticks) {
    static LONGLONG frequency;
    if (!frequency) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); frequency=f.QuadPart ? f.QuadPart : 1; }
    return (uint64_t)ticks*UINT64_C(1000000000)/(uint64_t)frequency;
}
static void lock_backoff(unsigned *spins) {
    if (++*spins < 64) { YieldProcessor(); return; }
    if (*spins < 256) { SwitchToThread(); return; }
    /* sotc: T2/C4: only this slow branch can lose a frame, so only it is timed (the spin paths
     * stay as they were). */
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    if (runtime_disabled_optimizations&(UINT64_C(1)<<61)) {
        /* Sleep(0) only yields to equal or higher priority: a preempted lower-priority holder
         * could wait seconds. */
        Sleep(1);
    } else {
        int64_t expected=__atomic_load_n(&lock_state,__ATOMIC_RELAXED);
        __atomic_add_fetch(&lock_waiters,1,__ATOMIC_RELAXED);
        WaitOnAddress((volatile void *)&lock_state,&expected,sizeof(expected),1);
        __atomic_sub_fetch(&lock_waiters,1,__ATOMIC_RELAXED);
    }
    QueryPerformanceCounter(&end);
    sotc_lock_slow_stat(lock_draw_thread==GetCurrentThreadId(),
                        lock_ticks_to_ns(end.QuadPart-start.QuadPart));
}
static void lock_wake(void) {
    if (__atomic_load_n(&lock_waiters,__ATOMIC_RELAXED)) WakeByAddressAll((void *)&lock_state);
}
static void rw_rdlock(void) {
    for (unsigned spins=0;;lock_backoff(&spins)) {
        int64_t s=__atomic_load_n(&lock_state,__ATOMIC_RELAXED);
        if (s>=0 && __atomic_compare_exchange_n(&lock_state,&s,s+1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return;
    }
}
static void rw_rdunlock(void) { __atomic_sub_fetch(&lock_state,1,__ATOMIC_RELEASE); lock_wake(); }
static void rw_wrlock(void) {
    for (unsigned spins=0;;lock_backoff(&spins)) {
        int64_t s=0;
        if (__atomic_compare_exchange_n(&lock_state,&s,-1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return;
    }
}
static void rw_wrunlock(void) { __atomic_store_n(&lock_state,0,__ATOMIC_RELEASE); lock_wake(); }
#else
static pthread_rwlock_t lock=PTHREAD_RWLOCK_INITIALIZER;
void runtime_memory_mark_draw_thread(void) {} /* sotc: Windows-only lock instrumentation */
static void rw_rdlock(void) { pthread_rwlock_rdlock(&lock); }
static void rw_rdunlock(void) { pthread_rwlock_unlock(&lock); }
static void rw_wrlock(void) { pthread_rwlock_wrlock(&lock); }
static void rw_wrunlock(void) { pthread_rwlock_unlock(&lock); }
#endif
static __thread unsigned exclusive_depth;
/* Odd while a writer holds the lock; readers' region caches are valid for one even value. */
/* sotc: DISPATCH-FASTPATH.md: the draw thread reads this word for every region/backing/clamp lookup
 * (8,000 bindings a frame), and the lock word next to it is written by every guest thread that takes
 * the lock for reading: the load missed the cache each time another core had just written the line.
 * Alone on a 64-byte line it stays shared. Layout only, same values. */
static uint64_t table_generation_line[8] __attribute__((aligned(64)));
#define table_generation (table_generation_line[0])
#ifdef _WIN32
/* sotc: SOTC_STALL_LOG: how long a thread holds the lock exclusively (a map, unmap or protect of
 * the game) and who took it; a guest thread holding it stalls the draw thread's per-draw lookups
 * (the 92 ms frames with nothing else attributed). Prints held >= 2 ms: the caller's return
 * address (module offset from runtime_memory_tag) and the thread. */
void runtime_memory_tag(const void *p, size_t size, const char *name);
static __thread LARGE_INTEGER wrlock_t0; static __thread void *wrlock_ra;
static int stall_log_on(void) {
    static int on=-1;
    if (on<0) { const char *e=getenv("SOTC_STALL_LOG"); on=(e && e[0] && !(e[0]=='0' && !e[1])) ? 1 : 0; }
    return on;
}
__attribute__((noinline)) static void write_lock(void) {
    if (!exclusive_depth++) {
        rw_wrlock(); __atomic_add_fetch(&table_generation,1,__ATOMIC_ACQ_REL);
        if (stall_log_on()) { QueryPerformanceCounter(&wrlock_t0); wrlock_ra=__builtin_return_address(0); }
    }
}
static void write_unlock(void) {
    if (!--exclusive_depth) {
        if (stall_log_on() && wrlock_t0.QuadPart) {
            LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
            double ms=(double)lock_ticks_to_ns(t1.QuadPart-wrlock_t0.QuadPart)/1e6;
            if (ms>=2.0) fprintf(stderr,"SOTC STALL guest-lock held %.1f ms by thread %lu, caller %+lld from runtime_memory_tag\n",
                                 ms,(unsigned long)GetCurrentThreadId(),
                                 (long long)((char*)wrlock_ra-(char*)(void*)&runtime_memory_tag));
            wrlock_t0.QuadPart=0;
        }
        __atomic_add_fetch(&table_generation,1,__ATOMIC_RELEASE); rw_wrunlock();
    }
}
#else
static void write_lock(void) {
    if (!exclusive_depth++) { rw_wrlock(); __atomic_add_fetch(&table_generation,1,__ATOMIC_ACQ_REL); }
}
static void write_unlock(void) {
    if (!--exclusive_depth) { __atomic_add_fetch(&table_generation,1,__ATOMIC_RELEASE); rw_wrunlock(); }
}
#endif
static void read_lock(void) { if (!exclusive_depth) rw_rdlock(); }
static void read_unlock(void) { if (!exclusive_depth) rw_rdunlock(); }
static Block blocks[LIMIT];
static Vma *vmas; static size_t vma_count, vma_capacity;
#ifndef _WIN32
static int pool_fd=-1;
#endif
static void region_direct_init(void); /* sotc: SOTC_REGION_DIRECT, read before any fault */
static void runtime_toggle_init(void); /* sotc: BB_TOGGLE_FILE at startup (bit 61, timer) */
static unsigned char *backing_base; /* second view of the pool: host writes bypass guest/GPU page protection */
/* sotc: R5: the flexible span (section bytes after the direct pool) is 512 MiB by default for 448 MiB
 * of flexible memory (it was 1 GiB; the census measured a 406 MiB high-water mark). flex_alloc is
 * first-fit over contiguous section pages, so the 64 MiB of slack is what absorbs fragmentation;
 * SOTC_FLEX_SPAN_MB (448..1024) overrides it (the census prints the high-water mark). It only saves
 * commit charge: untouched section pages cost no RAM. The section is created with this size and
 * cannot grow afterwards, so an exhausted span fails the map (logged by map_flexible). */
#define FLEX_SPAN_MAX (UINT64_C(1024) * 1024 * 1024)
/* sotc: the default stays 1 GiB until a real save (open world, colossus fights, late areas) is
 * measured: 512 MiB was only checked on the attract demo, and a failed flexible map would crash
 * the game to save commit charge alone. SOTC_FLEX_SPAN_MB=512 tries it. */
#define FLEX_SPAN_DEFAULT (UINT64_C(1024) * 1024 * 1024)
static uint64_t flex_span=FLEX_SPAN_DEFAULT;
static uint64_t flex_bitmap[FLEX_SPAN_MAX/PAGE/64];
static uint64_t flex_high_water; /* sotc: highest flexible page end ever allocated, in bytes from the span start */
/* sotc: R3 counters: bytes zeroed (Windows) / punched (Linux) by host_discard, and calls. */
static uint64_t discard_bytes, discard_calls;
static void flex_span_init(void) {
    const char *e=getenv("SOTC_FLEX_SPAN_MB");
    uint64_t mb=e ? strtoull(e,NULL,10) : 0;
    if (mb>=FLEXIBLE_SIZE/(1024*1024) && mb<=FLEX_SPAN_MAX/(1024*1024)) flex_span=mb*1024*1024;
}
/* GPU hooks (bbgpu): notified outside the lock, in order, after each operation. */
typedef void (*GpuRange)(uintptr_t address, uint64_t size);
static GpuRange hook_map, hook_unmap, hook_invalidate;
enum { HOOK_MAP, HOOK_UNMAP, HOOK_INVALIDATE };
typedef struct { int kind; uintptr_t address; uint64_t size; } PendingHook;
/* sotc: grows on demand. With 64 fixed slots, a GPU hook slowed down by an emergency texture
 * collection (video memory full in the open world) let other threads queue past the end, and
 * the exit(21) that followed hung the game instead of closing it. */
static PendingHook *pending; static size_t pending_count, pending_capacity;
static size_t allocations, maps, flexible_maps, protects, queries;
static uint64_t live_bytes, flexible_bytes;

static int valid_alignment(uint64_t a) { return a >= PAGE && a <= POOL_SIZE && !(a & (a-1)); }
static uint64_t align_up(uint64_t n, uint64_t a) { return (n+a-1) & ~(a-1); }
static int host_prot(int prot) {
    /* GPU read/write bits (0x10/0x20) need host access for the future GPU backend. */
    return ((prot & 0x11) ? PROT_READ : 0) | ((prot & 0x22) ? PROT_WRITE|PROT_READ : 0) | ((prot & 4) ? PROT_EXEC|PROT_READ : 0);
}
#ifdef _WIN32
/* ---- host mappings (Windows) ----
 * [USER_MIN,USER_MAX) is covered by regions, each one placeholder or one view of the
 * section. Mapping a range carves it out as a single placeholder first: views overlapping
 * it are unmapped, placeholders coalesced and split, and the parts of views outside the
 * range mapped again with the page protections they had (GPU page tracking included). */
typedef struct { uintptr_t start, end; uint64_t phys; int mapped; } HostRegion;
typedef struct { uintptr_t start, end; DWORD protect; } ProtRun;
static HostRegion *regions; static size_t region_count, region_capacity;
static HANDLE section;
static DWORD win_prot(int prot) {
    if (prot & PROT_EXEC) return (prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : (prot & PROT_READ) ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (prot & PROT_WRITE) return PAGE_READWRITE;
    return (prot & PROT_READ) ? PAGE_READONLY : PAGE_NOACCESS;
}
static void carve_resume_on_fail(void);
static void win_fail(const char *what, uintptr_t address, uint64_t size) {
    carve_resume_on_fail(); /* sotc: no stdio while other guest threads are stopped */
    fprintf(stderr,"STOP: %s failed at %p (+0x%" PRIx64 "), error %lu\n",what,(void *)address,size,GetLastError());
    exit(21);
}
static size_t region_index(uintptr_t a) { /* first region with end > a */
    size_t lo=0, hi=region_count;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (regions[mid].end<=a) lo=mid+1; else hi=mid; }
    return lo;
}
static void region_reserve(size_t extra) {
    while (region_count+extra>region_capacity) {
        size_t capacity=region_capacity ? region_capacity*2 : 256;
        HostRegion *next=realloc(regions,capacity*sizeof(*regions));
        if (!next) { fputs("Out of memory (host regions)\n",stderr); exit(1); }
        regions=next; region_capacity=capacity;
    }
}
static void region_insert(size_t at, HostRegion r) {
    region_reserve(1);
    memmove(regions+at+1,regions+at,(region_count-at)*sizeof(*regions));
    regions[at]=r; ++region_count;
}
static void region_erase(size_t at, size_t n) {
    memmove(regions+at,regions+at+n,(region_count-at-n)*sizeof(*regions)); region_count-=n;
}
/* The guest range is reserved before anything else can be placed there (runtime_start). */
/* sotc: the process keeps the default 15.6 ms timer, so every Sleep(1), CV wait_for(1 ms) and
 * timed wait actually sleeps a whole frame. The resolution is process-wide (it also shortens the
 * recorder's sleep and the frame limiter, and raises idle power), so bit 61 decides it at boot
 * from BB_TOGGLE_FILE (runtime_toggle_init) and again when an in-run change flips the bit
 * (toggle_watcher). ntdll only, no winmm link dependency. */
static void runtime_set_timer_resolution(int fine) {
    static int applied=-1; /* -1: never applied */
    if (fine==applied) return;
    HMODULE ntdll=GetModuleHandleA("ntdll.dll");
    if (!ntdll) return;
    LONG (WINAPI *set_resolution)(ULONG,BOOLEAN,PULONG)=
        (LONG (WINAPI *)(ULONG,BOOLEAN,PULONG))(void *)GetProcAddress(ntdll,"NtSetTimerResolution");
    LONG (WINAPI *query_resolution)(PULONG,PULONG,PULONG)=
        (LONG (WINAPI *)(PULONG,PULONG,PULONG))(void *)GetProcAddress(ntdll,"NtQueryTimerResolution");
    if (!set_resolution) return;
    ULONG current=0;
    set_resolution(10000,fine ? TRUE : FALSE,&current); /* 100 ns units: ask for 1 ms */
    applied=fine;
    /* The query reports the system-wide resolution, which other programs (browsers, chat apps)
     * often hold at 1 ms: it is printed apart from what this process asked for. */
    ULONG minimum=0, maximum=0, actual=current;
    if (!query_resolution || query_resolution(&minimum,&maximum,&actual)!=0) actual=current;
    printf("Runtime: timer resolution %s (system now %.1f ms)\n",
           fine ? "1 ms requested" : "default (request released)",(double)actual/10000.0);
}
void runtime_memory_reserve(void) {
    /* sotc: before the timer resolution: bit 61 (and SOTC_TIMER_RES) must be honoured at boot. */
    runtime_toggle_init();
    if (region_count) return;
    if (!VirtualAlloc2(NULL,(void *)USER_MIN,USER_MAX-USER_MIN,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0))
        win_fail("reserving the PS4 address range (link with --disable-high-entropy-va)",USER_MIN,USER_MAX-USER_MIN);
    region_insert(0,(HostRegion){USER_MIN,USER_MAX,0,0});
    /* sotc: Shadow of the Colossus maps fixed ranges below the user area (0x7f0000000),
     * in the PS4 system-managed window. A second placeholder covers it; mappings never
     * span both reservations because the host's low maps lie between them. */
    if (!VirtualAlloc2(NULL,(void *)SYSTEM_LOW_MIN,SYSTEM_LOW_MAX-SYSTEM_LOW_MIN,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0))
        win_fail("reserving the PS4 system-managed window",SYSTEM_LOW_MIN,SYSTEM_LOW_MAX-SYSTEM_LOW_MIN);
    region_insert(0,(HostRegion){SYSTEM_LOW_MIN,SYSTEM_LOW_MAX,0,0});
}
static void view_map(const HostRegion *r, DWORD protect) {
    DWORD initial=protect==PAGE_NOACCESS ? PAGE_READWRITE : protect, old;
    if (!MapViewOfFile3(section,GetCurrentProcess(),(void *)r->start,r->phys,r->end-r->start,MEM_REPLACE_PLACEHOLDER,initial,NULL,0))
        win_fail("MapViewOfFile3",r->start,r->end-r->start);
    if (initial!=protect && !VirtualProtect((void *)r->start,r->end-r->start,protect,&old))
        win_fail("VirtualProtect",r->start,r->end-r->start);
}
static size_t save_protections(uintptr_t start, uintptr_t end, ProtRun **runs, size_t count, size_t *capacity) {
    for (uintptr_t at=start; at<end;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)at,&m,sizeof(m))) win_fail("VirtualQuery",at,0);
        uintptr_t next=(uintptr_t)m.BaseAddress+m.RegionSize;
        if (next>end) next=end;
        if (count==*capacity) { *capacity=*capacity ? *capacity*2 : 16; *runs=realloc(*runs,*capacity*sizeof(**runs)); }
        (*runs)[count++]=(ProtRun){at,next,m.Protect};
        at=next;
    }
    return count;
}
#ifdef _WIN32
size_t runtime_thread_suspend_others(void **out, size_t capacity); /* runtime_thread.c */
void runtime_thread_resume(void **handles, size_t n);
/* sotc: SOTC_CARVE_SUSPEND=0 keeps the other guest threads running while live views are split. */
static int carve_suspend_enabled(void) {
    static int v=-1;
    if (v<0) { const char *e=getenv("SOTC_CARVE_SUSPEND"); v=!(e && e[0]=='0'); }
    return v;
}
static uint64_t carve_splits;
uint64_t runtime_memory_carve_splits(void) { return __atomic_exchange_n(&carve_splits,0,__ATOMIC_RELAXED); }
/* The threads carve_host stopped, so win_fail can resume them before reporting. */
static void *carve_stopped[256];
static size_t carve_nstopped;
static int carve_stopping;
#endif
static void carve_resume_on_fail(void) {
#ifdef _WIN32
    if (carve_stopping) { carve_stopping=0; runtime_thread_resume(carve_stopped,carve_nstopped); }
#endif
}
/* Leaves [a,b) as exactly one unmapped placeholder; returns its region index. */
static size_t carve_host(uintptr_t a, uintptr_t b) {
    size_t first=region_index(a), last=first;
    if (first==region_count || regions[first].start>a) win_fail("guest range outside the reservation",a,b-a);
    while (last<region_count && regions[last].start<b) ++last;
    if (regions[last-1].end<b) win_fail("guest range outside the reservation",a,b-a);
    uintptr_t lo=regions[first].start, hi=regions[last-1].end;
    HostRegion left=regions[first], right=regions[last-1];
    int keep_left=left.mapped && lo<a, keep_right=right.mapped && b<hi;
    if (first==last-1 && !left.mapped && lo==a && hi==b) return first;
    /* sotc: SOTC_STALL_LOG: where the time of a split of a live view goes (it stops every other
     * guest thread, and the caller holds the guest lock): saving the page protections of what stays,
     * stopping the threads, unmapping and mapping again, restoring the protections. */
    LARGE_INTEGER cv0, cv1, cv2, cv3; int cv_log=stall_log_on();
    if (cv_log) QueryPerformanceCounter(&cv0);
    ProtRun *runs=NULL; size_t nruns=0, capacity=0, left_runs=0;
    if (keep_left) left_runs=nruns=save_protections(lo,a,&runs,nruns,&capacity);
    if (keep_right) nruns=save_protections(b,hi,&runs,nruns,&capacity);
    if (cv_log) QueryPerformanceCounter(&cv1);
    /* sotc: a live view is split: everything in it (fiber stacks included) disappears until it is
     * mapped again below. Other guest threads stop meanwhile (see runtime_thread_suspend_others).
     * The region table is grown first: no heap call while they are stopped (one may hold the heap). */
    if ((keep_left || keep_right) && carve_suspend_enabled()) {
        region_reserve(3);
        carve_nstopped=runtime_thread_suspend_others(carve_stopped,256);
        carve_stopping=1;
        __atomic_add_fetch(&carve_splits,1,__ATOMIC_RELAXED);
    }
    if (cv_log) QueryPerformanceCounter(&cv2);
    for (size_t i=first;i<last;++i) if (regions[i].mapped) {
        if (!UnmapViewOfFile2(GetCurrentProcess(),(void *)regions[i].start,MEM_PRESERVE_PLACEHOLDER))
            win_fail("UnmapViewOfFile2",regions[i].start,regions[i].end-regions[i].start);
    }
    if (last-first>1 && !VirtualFree((void *)lo,hi-lo,MEM_RELEASE|MEM_COALESCE_PLACEHOLDERS))
        win_fail("coalescing placeholders",lo,hi-lo);
    region_erase(first,last-first);
    region_insert(first,(HostRegion){lo,hi,0,0});
    size_t at=first;
    if (lo<a) {
        if (!VirtualFree((void *)lo,a-lo,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) win_fail("splitting a placeholder",lo,a-lo);
        regions[at].end=a;
        region_insert(at+1,(HostRegion){a,hi,0,0});
        if (keep_left) {
            regions[at].phys=left.phys; regions[at].mapped=1;
            view_map(&regions[at],PAGE_READWRITE);
        }
        ++at;
    }
    if (b<hi) {
        if (!VirtualFree((void *)a,b-a,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) win_fail("splitting a placeholder",a,b-a);
        regions[at].end=b;
        region_insert(at+1,(HostRegion){b,hi,0,0});
        if (keep_right) {
            regions[at+1].phys=right.phys+(b-right.start); regions[at+1].mapped=1;
            view_map(&regions[at+1],PAGE_READWRITE);
        }
    }
    for (size_t i=0;i<nruns;++i) {
        DWORD old;
        if (runs[i].protect!=PAGE_READWRITE && !VirtualProtect((void *)runs[i].start,runs[i].end-runs[i].start,runs[i].protect,&old))
            win_fail("restoring page protection",runs[i].start,runs[i].end-runs[i].start);
    }
    if (cv_log) QueryPerformanceCounter(&cv3);
    if (carve_stopping) { carve_stopping=0; runtime_thread_resume(carve_stopped,carve_nstopped); }
    (void)left_runs;
    if (cv_log && (keep_left || keep_right)) {
        LARGE_INTEGER cv4; QueryPerformanceCounter(&cv4);
        double total=(double)lock_ticks_to_ns(cv4.QuadPart-cv0.QuadPart)/1e6;
        if (total>=2.0) fprintf(stderr,"SOTC STALL carve: view of %.1f MB split for %.1f MB: %zu protection runs saved %.1f ms, "
                                "stop threads %.1f ms, unmap+map+restore %.1f ms, resume %.1f ms, total %.1f ms\n",
                                (double)(hi-lo)/1048576.0,(double)(b-a)/1048576.0,nruns,
                                (double)lock_ticks_to_ns(cv1.QuadPart-cv0.QuadPart)/1e6,
                                (double)lock_ticks_to_ns(cv2.QuadPart-cv1.QuadPart)/1e6,
                                (double)lock_ticks_to_ns(cv3.QuadPart-cv2.QuadPart)/1e6,
                                (double)lock_ticks_to_ns(cv4.QuadPart-cv3.QuadPart)/1e6,total);
    }
    free(runs);
    return at;
}
/* sotc: SOTC_VIEW_GRANULE_MB (default 4; 0 = off, 1..64): a mapping is made of views of at most this
 * many MiB instead of one view. Windows cannot unmap part of a view, so freeing a few MiB inside a
 * 100-250 MiB view (the game streams its heaps this way) saved the page protections of everything left,
 * stopped every other guest thread, unmapped the whole view, mapped the pieces again and restored the
 * protections: 4-66 ms with the guest lock held, which the draw thread's per-draw lookups waited for (the
 * 92-108 ms frames with nothing else in them). With small views the same split touches two pieces. */
static uint64_t view_granule(void) {
    static int64_t g=-1;
    if (g<0) {
        const char *e=getenv("SOTC_VIEW_GRANULE_MB");
        long mb=e ? atol(e) : 4;
        g=(mb>=1 && mb<=64) ? (int64_t)mb*1024*1024 : 0;
    }
    return (uint64_t)g;
}
static int host_map(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t phys) {
    size_t i=carve_host(address,address+size);
    if (reserved) return 0;
    const uint64_t g=view_granule();
    const DWORD protect=win_prot(prot);
    if (!g || size<=g+g/2) {
        regions[i].phys=phys; regions[i].mapped=1;
        view_map(&regions[i],protect);
        return 0;
    }
    /* regions[i] is the placeholder [address,address+size): split pieces of g off its front. */
    const uintptr_t end=address+size;
    uintptr_t at=address;
    while (end-at>g+g/2) {
        if (!VirtualFree((void *)at,g,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) win_fail("splitting a placeholder",at,g);
        regions[i].end=at+g;
        region_insert(i+1,(HostRegion){at+g,end,0,0});
        regions[i].phys=phys+(at-address); regions[i].mapped=1;
        view_map(&regions[i],protect);
        ++i; at+=g;
    }
    regions[i].phys=phys+(at-address); regions[i].mapped=1;
    view_map(&regions[i],protect);
    return 0;
}
static int host_unmap(uintptr_t address, uint64_t size) { carve_host(address,address+size); return 0; }
static int host_protect(uintptr_t address, uint64_t size, int prot) {
    DWORD protect=win_prot(prot), old;
    uintptr_t end=address+size;
    for (size_t i=region_index(address); i<region_count && regions[i].start<end; ++i) {
        if (!regions[i].mapped) continue;
        uintptr_t a=regions[i].start>address ? regions[i].start : address, b=regions[i].end<end ? regions[i].end : end;
        if (!VirtualProtect((void *)a,b-a,protect,&old)) return -1;
    }
    return 0;
}
/* Released pages read as zero when reused, like the memfd's punched holes. */
static void host_discard(uint64_t phys, uint64_t size) {
    __atomic_add_fetch(&discard_bytes,size,__ATOMIC_RELAXED); __atomic_add_fetch(&discard_calls,1,__ATOMIC_RELAXED);
    LARGE_INTEGER t0; if (stall_log_on()) QueryPerformanceCounter(&t0);
    memset(backing_base+phys,0,size);
    if (stall_log_on()) { /* sotc: SOTC_STALL_LOG, zeroing released pages happens under the exclusive lock */
        LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
        double ms=(double)lock_ticks_to_ns(t1.QuadPart-t0.QuadPart)/1e6;
        if (ms>=1.0) fprintf(stderr,"SOTC STALL host_discard %.1f MB took %.1f ms (under the guest lock)\n",size/1048576.0,ms);
    }
}
/* Direct memory occupies [0,POOL_SIZE) of the section, flexible memory [POOL_SIZE,+flex_span). */
static int pool(void) {
    if (backing_base) return 0;
    region_direct_init();
    flex_span_init();
    runtime_memory_reserve();
    uint64_t bytes=POOL_SIZE+flex_span;
    section=CreateFileMapping2(INVALID_HANDLE_VALUE,NULL,FILE_MAP_ALL_ACCESS,PAGE_EXECUTE_READWRITE,SEC_COMMIT,bytes,NULL,NULL,0);
    /* sotc: when Windows refuses the larger pool (error 1450: small page file, RAM in use), try
     * the sizes between the console's and the requested one first. Going straight to the base size
     * is what made saves in Pro mode read past the page bitmap (guest offset 0x1a371, #194), so the
     * base is the last choice. */
    static const uint64_t fallback_mb[] = {8192, 7168, 6144};
    for (size_t i=0; !section && i<sizeof fallback_mb/sizeof fallback_mb[0]; ++i) {
        if (fallback_mb[i] >= pool_size_mb || fallback_mb[i] <= pool_base_mb()) continue;
        fprintf(stderr,"Runtime: %" PRIu64 " MiB of guest memory refused (error %lu): trying %" PRIu64 " MiB\n",
                pool_size_mb,GetLastError(),fallback_mb[i]);
        pool_size_mb=fallback_mb[i];
        bytes=POOL_SIZE+flex_span;
        section=CreateFileMapping2(INVALID_HANDLE_VALUE,NULL,FILE_MAP_ALL_ACCESS,PAGE_EXECUTE_READWRITE,SEC_COMMIT,bytes,NULL,NULL,0);
    }
    if (!section && pool_size_mb>pool_base_mb()) {
        fprintf(stderr,"Runtime: %" PRIu64 " MiB of guest memory refused (error %lu): using %" PRIu64 " MiB; a larger page file allows more\n",
                bytes>>20,GetLastError(),pool_base_mb());
        pool_size_mb=pool_base_mb();
        bytes=POOL_SIZE+flex_span;
        section=CreateFileMapping2(INVALID_HANDLE_VALUE,NULL,FILE_MAP_ALL_ACCESS,PAGE_EXECUTE_READWRITE,SEC_COMMIT,bytes,NULL,NULL,0);
    }
    if (!section) { fprintf(stderr,"Runtime: cannot create %" PRIu64 " MiB of guest memory (error %lu); is the page file large enough?\n",bytes>>20,GetLastError()); return -1; }
    backing_base=MapViewOfFile3(section,GetCurrentProcess(),NULL,0,bytes,0,PAGE_READWRITE,NULL,0);
    return backing_base ? 0 : -1;
}
#else
/* sotc: the timer resolution is a Windows decision (ntdll); nothing to do elsewhere. */
static void runtime_set_timer_resolution(int fine) { (void)fine; }
/* Direct memory occupies [0,POOL_SIZE) of the memfd, flexible memory [POOL_SIZE,+flex_span). */
static int pool(void) {
    if (pool_fd>=0) return 0;
    region_direct_init();
    flex_span_init();
    pool_fd=memfd_create("bb-guest-memory", MFD_CLOEXEC);
    if (pool_fd<0 || ftruncate(pool_fd,(off_t)(POOL_SIZE+flex_span))) return -1;
    void *view=mmap(NULL,POOL_SIZE+flex_span,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_NORESERVE,pool_fd,0);
    if (view==MAP_FAILED) return -1;
    backing_base=view;
    return 0;
}
void runtime_memory_reserve(void) {}
static int host_map(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t phys) {
    void *mapped = !reserved
        ? mmap((void *)address,size,prot,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)phys)
        : mmap((void *)address,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED,-1,0);
    return mapped==MAP_FAILED ? -1 : 0;
}
static int host_unmap(uintptr_t address, uint64_t size) { return munmap((void *)address,size); }
static int host_protect(uintptr_t address, uint64_t size, int prot) { return mprotect((void *)address,size,prot); }
static void host_discard(uint64_t phys, uint64_t size) {
    __atomic_add_fetch(&discard_bytes,size,__ATOMIC_RELAXED); __atomic_add_fetch(&discard_calls,1,__ATOMIC_RELAXED);
    fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)phys,(off_t)size);
}
#endif
/* ---- sotc: allocation tags for the RAM census ----
 * Big host allocations of the GPU library register themselves (runtime_memory_tag) so the census can
 * name the private regions it lists. Append-only, lock-free: a slot is claimed with fetch_add, filled,
 * then published by a release store of its name; a full table drops new tags. */
#define MEM_TAGS 64
typedef struct { const void *p; size_t size; const char *volatile name; } MemTag;
static MemTag mem_tags[MEM_TAGS];
static unsigned mem_tag_count;
void runtime_memory_tag(const void *p, size_t size, const char *name) {
    if (!p || !name) return;
    unsigned n=__atomic_load_n(&mem_tag_count,__ATOMIC_ACQUIRE);
    if (n>MEM_TAGS) n=MEM_TAGS;
    for (unsigned i=0;i<n;++i) { /* same name inside the same 64 KiB granule: the same region */
        const char *have=__atomic_load_n(&mem_tags[i].name,__ATOMIC_ACQUIRE);
        if (have==name && ((uintptr_t)mem_tags[i].p>>16)==((uintptr_t)p>>16)) return;
    }
    unsigned slot=__atomic_fetch_add(&mem_tag_count,1,__ATOMIC_ACQ_REL);
    if (slot>=MEM_TAGS) return;
    mem_tags[slot].p=p; mem_tags[slot].size=size;
    __atomic_store_n(&mem_tags[slot].name,name,__ATOMIC_RELEASE);
}
/* Vulkan device memory by memory type (VMA allocate/free callbacks, only registered when the census
 * is on): live bytes and blocks per type, and which types are host-visible. */
#define VK_CENSUS_TYPES 32
static uint64_t vk_type_bytes[VK_CENSUS_TYPES];
static uint32_t vk_type_blocks[VK_CENSUS_TYPES], vk_host_visible_mask;
void runtime_memory_census_vk(unsigned type, int host_visible, int64_t delta_bytes) {
    if (type>=VK_CENSUS_TYPES) return;
    __atomic_add_fetch(&vk_type_bytes[type],(uint64_t)delta_bytes,__ATOMIC_RELAXED);
    if (delta_bytes>0) __atomic_add_fetch(&vk_type_blocks[type],1,__ATOMIC_RELAXED);
    else __atomic_sub_fetch(&vk_type_blocks[type],1,__ATOMIC_RELAXED);
    if (host_visible) __atomic_or_fetch(&vk_host_visible_mask,UINT32_C(1)<<type,__ATOMIC_RELAXED);
}
/* ---- sotc: RAM census (SOTC_RAM_CENSUS=1, diagnostic) ----
 * Where does the process RAM go? Walks the address space with VirtualQuery and counts resident
 * pages with K32QueryWorkingSetEx (looked up at run time: no psapi link change). It runs on its own
 * low-priority thread, because a full walk touches millions of page entries (the guest views and the
 * backing view are ~10 GiB of address space): the caller gets the text of the previous run, so the
 * first call returns nothing and each later one lags one interval. The region table is only copied
 * under the read lock (a few microseconds); nothing else is locked. */
#ifdef _WIN32
typedef struct { PVOID va; ULONG_PTR flags; } CensusWsEx; /* PSAPI_WORKING_SET_EX_INFORMATION: bit 0 valid, bit 15 shared */
typedef BOOL (WINAPI *CensusQwsFn)(HANDLE,PVOID,DWORD);
typedef LONG (NTAPI *CensusQitFn)(HANDLE,ULONG,PVOID,ULONG,PULONG);
typedef struct { LONG status; PVOID teb; ULONG_PTR pid, tid, affinity; LONG priority, base_priority; } CensusTbi; /* THREAD_BASIC_INFORMATION */
typedef struct { uintptr_t base, end; uint64_t commit, resident; } CensusTop;
#define CENSUS_BATCH 16384
static void census_top_add(CensusTop *top, uintptr_t base, uintptr_t end, uint64_t commit, uint64_t resident) {
    if (!commit) return;
    int worst=0;
    for (int i=1;i<8;++i) if (top[i].commit<top[worst].commit) worst=i;
    if (commit>top[worst].commit) top[worst]=(CensusTop){base,end,commit,resident};
}
/* Names of the tagged allocations (runtime_memory_tag) inside [base,end): "a=KiB+b=KiB" (at most 3
 * names, tagged sizes summed per name), or "?" when nothing is tagged there. */
static void census_label(char *out, size_t cap, uintptr_t base, uintptr_t end) {
    const char *names[3]={0}; uint64_t bytes[3]={0};
    int count=0;
    unsigned n=__atomic_load_n(&mem_tag_count,__ATOMIC_ACQUIRE);
    if (n>MEM_TAGS) n=MEM_TAGS;
    for (unsigned i=0;i<n;++i) {
        const char *name=__atomic_load_n(&mem_tags[i].name,__ATOMIC_ACQUIRE);
        uintptr_t p=(uintptr_t)mem_tags[i].p;
        if (!name || p<base || p>=end) continue;
        int k=0;
        while (k<count && names[k]!=name) ++k;
        if (k==3) continue;
        if (k==count) { names[k]=name; ++count; }
        bytes[k]+=mem_tags[i].size;
    }
    size_t at=0;
    if (!cap) return;
    out[0]=0;
    if (!count) { snprintf(out,cap,"?"); return; }
    for (int k=0;k<count && at<cap;++k) {
        int w=snprintf(out+at,cap-at,"%s%s=%lluK",k ? "+" : "",names[k],(unsigned long long)(bytes[k]>>10));
        if (w>0) at+=(size_t)w;
    }
}
typedef struct { DWORD cb; SIZE_T allocated, committed, reserved, max_reserve; } CensusHeapSummary; /* HEAP_SUMMARY */
typedef BOOL (WINAPI *CensusHeapSummaryFn)(HANDLE,DWORD,CensusHeapSummary *);
/* Sums HeapSummary over the process heaps (CRT/malloc growth); false when the API is missing. */
static int census_heaps(unsigned *heaps, uint64_t *allocated, uint64_t *committed, uint64_t *default_committed) {
    CensusHeapSummaryFn summary=(CensusHeapSummaryFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),"HeapSummary");
    HANDLE list[64];
    DWORD n=GetProcessHeaps(64,list);
    *heaps=0; *allocated=*committed=*default_committed=0;
    if (!summary || !n) return 0;
    if (n>64) n=64;
    HANDLE def=GetProcessHeap();
    for (DWORD i=0;i<n;++i) {
        CensusHeapSummary s; memset(&s,0,sizeof(s)); s.cb=sizeof(s);
        if (!summary(list[i],0,&s)) continue;
        ++*heaps; *allocated+=s.allocated; *committed+=s.committed;
        if (list[i]==def) *default_committed=s.committed;
    }
    return 1;
}
static void census_bit_set(uint64_t *bits, uint64_t index) { bits[index/64]|=UINT64_C(1)<<(index%64); }
/* Committed bytes of every thread's stack (TEB DeallocationStack..StackBase); *threads = thread count. */
static uint64_t census_stacks(unsigned *threads) {
    uint64_t total=0;
    *threads=0;
    HMODULE ntdll=GetModuleHandleA("ntdll.dll");
    CensusQitFn query=ntdll ? (CensusQitFn)(void *)GetProcAddress(ntdll,"NtQueryInformationThread") : NULL;
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if (snap==INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te; te.dwSize=sizeof(te);
    DWORD pid=GetCurrentProcessId();
    for (BOOL ok=Thread32First(snap,&te); ok; ok=Thread32Next(snap,&te)) {
        if (te.th32OwnerProcessID!=pid) continue;
        ++*threads;
        if (!query) continue;
        HANDLE t=OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,te.th32ThreadID);
        if (!t) continue;
        CensusTbi tbi; memset(&tbi,0,sizeof(tbi));
        if (query(t,0,&tbi,sizeof(tbi),NULL)==0 && tbi.teb) {
            /* x64 TEB: NT_TIB.StackBase +0x08, DeallocationStack +0x1478 */
            uintptr_t base=0, low=0; SIZE_T got=0;
            if (ReadProcessMemory(GetCurrentProcess(),(char *)tbi.teb+0x08,&base,sizeof(base),&got) && got==sizeof(base) &&
                ReadProcessMemory(GetCurrentProcess(),(char *)tbi.teb+0x1478,&low,sizeof(low),&got) && got==sizeof(low) &&
                low && base>low && base-low<=(UINT64_C(1)<<32)) {
                for (uintptr_t at=low; at<base;) {
                    MEMORY_BASIC_INFORMATION m;
                    if (!VirtualQuery((void *)at,&m,sizeof(m))) break;
                    uintptr_t next=(uintptr_t)m.BaseAddress+m.RegionSize;
                    if (next<=at) break;
                    if (m.State==MEM_COMMIT) total+=(next>base ? base : next)-at;
                    at=next;
                }
            }
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    return total;
}
static void census_run(char *text, size_t cap) {
    CensusQwsFn qws=(CensusQwsFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),"K32QueryWorkingSetEx");
    unsigned char *back=backing_base;
    uint64_t sbytes=back ? POOL_SIZE+flex_span : 0, nbits=sbytes>>12, nwords=(nbits+63)/64;
    HostRegion *snap=NULL; size_t sn=0;
    for (int tries=0; back && tries<4; ++tries) { /* copy of the region table: guest VA page -> section offset */
        read_lock(); size_t n=region_count; read_unlock();
        HostRegion *copy=malloc((n+32)*sizeof(*copy));
        if (!copy) break;
        read_lock();
        if (region_count<=n+32) { memcpy(copy,regions,region_count*sizeof(*copy)); sn=region_count; snap=copy; read_unlock(); break; }
        read_unlock(); free(copy);
    }
    uint64_t *gbits=nwords ? calloc(nwords,8) : NULL, *bbits=nwords ? calloc(nwords,8) : NULL;
    CensusWsEx *ws=malloc(CENSUS_BATCH*sizeof(*ws));
    if (!qws || !ws || (nwords && (!gbits || !bbits))) {
        snprintf(text,cap,"SOTC RAM census: unavailable (K32QueryWorkingSetEx or memory missing)\n");
        goto done;
    }
    {
        SYSTEM_INFO si; GetSystemInfo(&si);
        uintptr_t addr=0, limit=(uintptr_t)si.lpMaximumApplicationAddress, cur_alloc=0, cur_end=0;
        uint64_t commit[3]={0}, resident[3]={0}, shared[3]={0}, cur_commit=0, cur_resident=0;
        CensusTop top[8]; memset(top,0,sizeof(top));
        size_t gi=0;
        while (addr<limit) {
            MEMORY_BASIC_INFORMATION m;
            if (!VirtualQuery((void *)addr,&m,sizeof(m))) break;
            uintptr_t base=(uintptr_t)m.BaseAddress, end=base+m.RegionSize;
            if (end<=addr) break;
            addr=end;
            if (m.State!=MEM_COMMIT) continue;
            int t=m.Type==MEM_PRIVATE ? 0 : m.Type==MEM_MAPPED ? 1 : m.Type==MEM_IMAGE ? 2 : -1;
            if (t<0) continue;
            uint64_t region_resident=0;
            commit[t]+=m.RegionSize;
            for (uintptr_t p=base; p<end;) {
                size_t n=(end-p)/4096; if (n>CENSUS_BATCH) n=CENSUS_BATCH;
                for (size_t i=0;i<n;++i) { ws[i].va=(PVOID)(p+i*4096); ws[i].flags=0; }
                if (qws(GetCurrentProcess(),ws,(DWORD)(n*sizeof(*ws)))) {
                    for (size_t i=0;i<n;++i) {
                        if (!(ws[i].flags&1)) continue;
                        uintptr_t va=p+i*4096;
                        ++region_resident; ++resident[t];
                        if (ws[i].flags&(1u<<15)) ++shared[t];
                        if (back && va>=(uintptr_t)back && va<(uintptr_t)back+sbytes) census_bit_set(bbits,(va-(uintptr_t)back)>>12);
                        else if (sn) {
                            while (gi<sn && snap[gi].end<=va) ++gi;
                            if (gi<sn && snap[gi].mapped && snap[gi].start<=va) {
                                uint64_t off=snap[gi].phys+(va-snap[gi].start);
                                if (off<sbytes) census_bit_set(gbits,off>>12);
                            }
                        }
                    }
                }
                p+=n*4096;
            }
            if (t==0) {
                if ((uintptr_t)m.AllocationBase!=cur_alloc) { census_top_add(top,cur_alloc,cur_end,cur_commit,cur_resident); cur_commit=cur_resident=0; cur_alloc=(uintptr_t)m.AllocationBase; }
                cur_commit+=m.RegionSize; cur_resident+=region_resident*4096; cur_end=end;
            }
        }
        census_top_add(top,cur_alloc,cur_end,cur_commit,cur_resident);
        uint64_t g=0, b=0, both=0, any=0;
        for (uint64_t i=0;i<nwords;++i) {
            g+=(uint64_t)__builtin_popcountll(gbits[i]); b+=(uint64_t)__builtin_popcountll(bbits[i]);
            both+=(uint64_t)__builtin_popcountll(gbits[i]&bbits[i]); any+=(uint64_t)__builtin_popcountll(gbits[i]|bbits[i]);
        }
        unsigned threads; uint64_t stacks=census_stacks(&threads);
        const unsigned long long MB=1024*1024;
        size_t at=0;
#define CENSUS_PRINTF(...) do { if (at<cap) { int w_=snprintf(text+at,cap-at,__VA_ARGS__); if (w_>0) at+=(size_t)w_; } } while (0)
        CENSUS_PRINTF("SOTC RAM census: private commit %llu MB resident %llu MB (shared %llu); mapped commit %llu MB resident %llu MB (shared %llu); "
                      "image commit %llu MB resident %llu MB (shared %llu)\n",
                      commit[0]/MB,resident[0]*4096/MB,shared[0]*4096/MB, commit[1]/MB,resident[1]*4096/MB,shared[1]*4096/MB,
                      commit[2]/MB,resident[2]*4096/MB,shared[2]*4096/MB);
        CENSUS_PRINTF("SOTC RAM census: guest section resident %llu MB via guest views, %llu MB via backing view, %llu MB in both, %llu MB distinct "
                      "(of %llu MB); flex high-water %llu MB of %llu MB span; host_discard %llu MB in %llu calls\n",
                      g*4096/MB,b*4096/MB,both*4096/MB,any*4096/MB,(unsigned long long)sbytes/MB,
                      (unsigned long long)flex_high_water/MB,(unsigned long long)flex_span/MB,
                      __atomic_load_n(&discard_bytes,__ATOMIC_RELAXED)/MB,(unsigned long long)__atomic_load_n(&discard_calls,__ATOMIC_RELAXED));
        CENSUS_PRINTF("SOTC RAM census: %u threads, stacks committed %llu MB\n",threads,(unsigned long long)stacks/MB);
        {
            unsigned heaps; uint64_t heap_alloc, heap_commit, heap_default;
            if (census_heaps(&heaps,&heap_alloc,&heap_commit,&heap_default))
                CENSUS_PRINTF("SOTC RAM census: process heaps: %u heaps, allocated %llu MB, committed %llu MB (default heap %llu MB)\n",
                              heaps,(unsigned long long)heap_alloc/MB,(unsigned long long)heap_commit/MB,(unsigned long long)heap_default/MB);
            uint64_t hv_bytes=0, ty_bytes[VK_CENSUS_TYPES]; uint32_t hv_blocks=0, ty_blocks[VK_CENSUS_TYPES];
            uint32_t mask=__atomic_load_n(&vk_host_visible_mask,__ATOMIC_RELAXED);
            for (unsigned i=0;i<VK_CENSUS_TYPES;++i) {
                ty_bytes[i]=__atomic_load_n(&vk_type_bytes[i],__ATOMIC_RELAXED); ty_blocks[i]=__atomic_load_n(&vk_type_blocks[i],__ATOMIC_RELAXED);
                if (mask&(UINT32_C(1)<<i)) { hv_bytes+=ty_bytes[i]; hv_blocks+=ty_blocks[i]; }
            }
            CENSUS_PRINTF("SOTC RAM census: vulkan host-visible device memory %llu MB in %u blocks (types, h=host-visible, MB/blocks:",
                          (unsigned long long)hv_bytes/MB,hv_blocks);
            for (unsigned i=0;i<VK_CENSUS_TYPES;++i)
                if (ty_blocks[i]) CENSUS_PRINTF(" %u%s:%llu/%u",i,(mask&(UINT32_C(1)<<i)) ? "h" : "",(unsigned long long)ty_bytes[i]/MB,ty_blocks[i]);
            CENSUS_PRINTF(")\n");
        }
        CENSUS_PRINTF("SOTC RAM census: top private regions (base commit/resident MB name=tagged KiB):");
        for (int k=0;k<8;++k) { /* descending by commit */
            int best=-1;
            for (int i=0;i<8;++i) if (top[i].commit && (best<0 || top[i].commit>top[best].commit)) best=i;
            if (best<0) break;
            char label[96]; census_label(label,sizeof(label),top[best].base,top[best].end);
            CENSUS_PRINTF(" %p %llu/%llu %s",(void *)top[best].base,(unsigned long long)top[best].commit/MB,(unsigned long long)top[best].resident/MB,label);
            top[best].commit=0;
        }
        CENSUS_PRINTF("\n");
#undef CENSUS_PRINTF
    }
done:
    free(ws); free(gbits); free(bbits); free(snap);
}
static volatile LONG census_running;
static SRWLOCK census_lock=SRWLOCK_INIT;
static char census_text[4096];
static DWORD WINAPI census_thread(void *arg) {
    (void)arg;
    char text[4096]; text[0]=0;
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    census_run(text,sizeof(text));
    AcquireSRWLockExclusive(&census_lock);
    memcpy(census_text,text,sizeof(text));
    ReleaseSRWLockExclusive(&census_lock);
    InterlockedExchange(&census_running,0);
    return 0;
}
/* Copies the last finished census (several lines starting with "SOTC RAM census:") into out and
 * starts the next one in the background; empty when SOTC_RAM_CENSUS is not 1 or none has finished. */
size_t runtime_memory_ram_census(char *out, size_t cap) {
    static int enabled=-1;
    if (cap) out[0]=0;
    if (enabled<0) { const char *e=getenv("SOTC_RAM_CENSUS"); enabled=e && e[0]=='1'; }
    if (!enabled || !cap) return 0;
    AcquireSRWLockShared(&census_lock);
    snprintf(out,cap,"%s",census_text);
    ReleaseSRWLockShared(&census_lock);
    if (InterlockedCompareExchange(&census_running,1,0)==0) {
        HANDLE h=CreateThread(NULL,0,census_thread,NULL,0,NULL);
        if (h) CloseHandle(h); else InterlockedExchange(&census_running,0);
    }
    return strlen(out);
}
#else
size_t runtime_memory_ram_census(char *out, size_t cap) { if (cap) out[0]=0; return 0; } /* sotc: Windows-only diagnostic */
#endif
static void queue_hook(int kind, uintptr_t address, uint64_t size) {
    GpuRange hook = kind==HOOK_MAP ? hook_map : kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
    if (!hook) return;
    if (pending_count==pending_capacity) {
        size_t capacity = pending_capacity ? pending_capacity*2 : 64;
        PendingHook *grown = realloc(pending, capacity*sizeof(*pending));
        if (!grown) { fputs("STOP: GPU hook queue out of memory\n",stderr); fflush(stderr); _Exit(21); }
        pending=grown; pending_capacity=capacity;
        if (capacity>64) fprintf(stderr,"Runtime: GPU hook queue grew to %zu entries\n",capacity);
    }
    pending[pending_count++]=(PendingHook){kind,address,size};
}
/* Runs queued GPU notifications; callers invoke it after dropping the lock. */
static void flush_hooks(void) {
    for (;;) {
        write_lock();
        if (!pending_count) { write_unlock(); return; }
        PendingHook h=pending[0];
        memmove(pending,pending+1,--pending_count*sizeof(*pending));
        GpuRange hook = h.kind==HOOK_MAP ? hook_map : h.kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
        write_unlock();
        if (hook) hook(h.address,h.size);
    }
}
static int flex_test(uint64_t page) { return (int)((flex_bitmap[page/64]>>(page%64))&1); }
static void flex_set(uint64_t first, uint64_t count, int used) {
    for (uint64_t p=first;p<first+count;++p) {
        if (used) flex_bitmap[p/64]|=UINT64_C(1)<<(p%64); else flex_bitmap[p/64]&=~(UINT64_C(1)<<(p%64));
    }
}
/* sotc: set when flex_alloc finds no contiguous run (written under the write lock, printed by
 * map_flexible after it): pages asked for, largest free run and total free pages of the span. */
static __thread uint64_t flex_fail_need, flex_fail_run, flex_fail_free;
static uint64_t flex_alloc(uint64_t size) {
    uint64_t pages=size/PAGE, total=flex_span/PAGE, run=0, best=0, free_pages=0;
    for (uint64_t p=0;p<total;++p) {
        run = flex_test(p) ? 0 : run+1;
        if (run==pages) {
            flex_set(p+1-pages,pages,1);
            if ((p+1)*PAGE>flex_high_water) flex_high_water=(p+1)*PAGE;
            return POOL_SIZE+(p+1-pages)*PAGE;
        }
        if (run>best) best=run;
        if (!flex_test(p)) ++free_pages;
    }
    flex_fail_need=pages; flex_fail_run=best; flex_fail_free=free_pages;
    return UINT64_MAX;
}
static void flex_free(uint64_t phys, uint64_t size) {
    flex_set((phys-POOL_SIZE)/PAGE,size/PAGE,0);
    host_discard(phys,size);
}
static size_t vma_index(uintptr_t a) { /* first VMA with end > a */
    size_t lo=0, hi=vma_count;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (vmas[mid].end<=a) lo=mid+1; else hi=mid; }
    return lo;
}
static int vma_insert(size_t at, Vma v) {
    if (vma_count==vma_capacity) {
        size_t capacity=vma_capacity ? vma_capacity*2 : 256;
        Vma *next=realloc(vmas,capacity*sizeof(*vmas));
        if (!next) return -1;
        vmas=next; vma_capacity=capacity;
    }
    memmove(vmas+at+1,vmas+at,(vma_count-at)*sizeof(*vmas));
    vmas[at]=v; ++vma_count; return 0;
}
static void vma_erase(size_t at, size_t n) {
    memmove(vmas+at,vmas+at+n,(vma_count-at-n)*sizeof(*vmas)); vma_count-=n;
}
static int split_at(uintptr_t a) {
    size_t i=vma_index(a);
    if (i==vma_count || vmas[i].start>=a) return 0;
    Vma right=vmas[i];
    right.start=a;
    /* sotc: flexible memory is a contiguous run of the pool as well: the right piece must keep pointing
     * at its own pages, or a later drop_range frees (and zeroes) the left piece's live ones. */
    if (right.kind==KIND_DIRECT || right.kind==KIND_FLEXIBLE) right.phys+=a-vmas[i].start;
    vmas[i].end=a;
    return vma_insert(i+1,right);
}
/* Range [start,end) split on its edges; returns the first index inside it. */
static size_t carve(uintptr_t start, uintptr_t end, int *error) {
    *error = split_at(start) || split_at(end);
    return vma_index(start);
}
static int covered(uintptr_t start, uintptr_t end, int allow_reserved) {
    size_t i=vma_index(start); uintptr_t at=start;
    for (; at<end; ++i) {
        if (i==vma_count || vmas[i].start>at || (!allow_reserved && vmas[i].kind==KIND_RESERVED)) return 0;
        at=vmas[i].end;
    }
    return 1;
}
static int overlaps(uintptr_t start, uintptr_t end, int ignore_reserved) {
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (!ignore_reserved || vmas[i].kind!=KIND_RESERVED) return 1;
    return 0;
}
/* First-fit search in the PS4 user range, starting from the hint. */
static uintptr_t find_free(uintptr_t hint, uint64_t size, uint64_t alignment) {
    uintptr_t at=align_up(hint<USER_MIN ? USER_MIN : hint, alignment);
    for (size_t i=vma_index(at); at+size<=USER_MAX; ++i) {
        if (i==vma_count || vmas[i].start>=at+size) return at;
        at=align_up(vmas[i].end,alignment);
    }
    return 0;
}
static void drop_range(uintptr_t start, uintptr_t end) {
    int error; size_t i=carve(start,end,&error), n=0;
    while (i+n<vma_count && vmas[i+n].end<=end && vmas[i+n].start>=start) {
        if (vmas[i+n].kind==KIND_FLEXIBLE) {
            flexible_bytes-=vmas[i+n].end-vmas[i+n].start;
            flex_free(vmas[i+n].phys,vmas[i+n].end-vmas[i+n].start);
        }
        ++n;
    }
    vma_erase(i,n);
}
/* Places a host mapping; with MAP_FIXED it replaces what the guest had there. */
static int32_t place(void **inout, uint64_t size, int prot, int flags, uint64_t alignment,
                     int kind, int type, uint64_t phys) {
    uintptr_t address=(uintptr_t)*inout;
    if (flags & MAP_FIXED_FLAG) {
        if (!address || address%PAGE) return INVALID;
        if (address<USER_MIN && (address<SYSTEM_LOW_MIN || address+size>SYSTEM_LOW_MAX)) {
            fprintf(stderr,"Runtime: fixed mapping outside the guest windows: %p +0x%" PRIx64 "\n",(void *)address,size);
            return NO_MEMORY;
        }
        if ((flags & MAP_NO_OVERWRITE) && overlaps(address,address+size,1)) return NO_MEMORY;
    } else {
        address=find_free(address,size,alignment);
        if (!address) return NO_MEMORY;
    }
    if (host_map(address,size,host_prot(prot),kind==KIND_RESERVED,phys)) return NO_MEMORY;
    void *mapped=(void *)address;
    /* sotc: a fixed mapping over live memory replaces it; the GPU side drops what it tracks there
     * first (as unmap does), or it keeps buffers and images of the old contents. */
    if (flags & MAP_FIXED_FLAG)
        for (size_t i=vma_index(address); i<vma_count && vmas[i].start<address+size; ++i)
            if (vmas[i].kind!=KIND_RESERVED) {
                uintptr_t lo=vmas[i].start>address ? vmas[i].start : address;
                uintptr_t hi=vmas[i].end<address+size ? vmas[i].end : address+size;
                queue_hook(HOOK_UNMAP,lo,hi-lo);
            }
    drop_range(address,address+size);
    if (vma_insert(vma_index(address),(Vma){address,address+size,kind,prot,type,phys})) return NO_MEMORY;
    if (kind==KIND_FLEXIBLE) flexible_bytes+=size;
    if (kind!=KIND_RESERVED) queue_hook(HOOK_MAP,address,size);
    *inout=mapped;
    return 0;
}
static int block_type(uint64_t phys) {
    for (int i=0;i<LIMIT;++i)
        if (blocks[i].used && phys>=blocks[i].start && phys-blocks[i].start<blocks[i].size) return blocks[i].type;
    return -1;
}
static int direct_range_allocated(uint64_t phys, uint64_t size) {
    for (uint64_t at=phys; at<phys+size;) {
        int found=0;
        for (int i=0;i<LIMIT;++i) if (blocks[i].used && at>=blocks[i].start && at-blocks[i].start<blocks[i].size) {
            at=blocks[i].start+blocks[i].size; found=1; break;
        }
        if (!found) return 0;
    }
    return 1;
}

/* sotc: the pool is created before the size is given: when Windows refuses the larger pool, the
 * fallback shrinks it, and a game told 9152 MiB allocated near that top, got nothing and wrote
 * through a null pointer at boot (guest offset 0x18cae, #194). */
static ABI uint64_t direct_size(void) {
    write_lock();
    (void)pool();
    write_unlock();
    return POOL_SIZE;
}
static ABI int32_t direct_allocate(int64_t low, int64_t high, uint64_t size,
                                  uint64_t alignment, int type, int64_t *out) {
    if (!alignment) alignment = PAGE;
    if (!out || low < 0 || high < 0 || !size || size % PAGE || !valid_alignment(alignment) || type < 0 || type > 10)
        return INVALID;
    write_lock();
    if (pool()) { write_unlock(); return NO_MEMORY; }
    uint64_t end = (uint64_t)high < POOL_SIZE ? (uint64_t)high : POOL_SIZE;
    if ((uint64_t)low >= end || size > end - (uint64_t)low) { write_unlock(); return NO_SPACE; }
    uint64_t start = align_up((uint64_t)low, alignment);
    int slot = -1;
    for (int i=0; i<LIMIT; ++i) if (!blocks[i].used) { slot=i; break; }
    for (;;) {
        if (slot<0 || start > end || size > end-start) { write_unlock(); return NO_SPACE; }
        int overlap = 0;
        for (int i=0; i<LIMIT; ++i) {
            Block b = blocks[i];
            if (b.used && start < b.start+b.size && b.start < start+size) {
                start = align_up(b.start+b.size, alignment); overlap = 1; break;
            }
        }
        if (!overlap) break;
    }
    blocks[slot] = (Block){start, size, type, 1}; *out = (int64_t)start;
    ++allocations; live_bytes += size;
    write_unlock();
    printf("Runtime: direct allocation offset=0x%" PRIx64 ", size=%" PRIu64 " bytes, type=%d\n", start, size, type);
    return 0;
}
static int32_t map_direct_locked(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!out || physical < 0 || (uint64_t)physical % PAGE || !size || size % PAGE || !valid_alignment(alignment) ||
        (prot & ~0x37)) return INVALID;
    if (!direct_range_allocated((uint64_t)physical,size)) return INVALID;
    int32_t result=place(out,size,prot,flags,alignment,KIND_DIRECT,block_type((uint64_t)physical),(uint64_t)physical);
    if (!result) ++maps;
    return result;
}
static ABI int32_t direct_map(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    write_lock();
    int32_t result=map_direct_locked(out,size,prot,flags,physical,alignment);
    write_unlock();
    flush_hooks();
    if (!result) printf("Runtime: mapped direct memory %p, size=%" PRIu64 ", prot=0x%x\n", *out, size, prot);
    return result;
}
static ABI int32_t direct_map_named(void **out, uint64_t size, int prot, int flags, int64_t physical,
                                    uint64_t alignment, const char *name) {
    (void)name; return direct_map(out,size,prot,flags,physical,alignment);
}
static int32_t unmap_locked(uintptr_t start, uint64_t size) {
    if (start%PAGE || !size) return INVALID;
    uint64_t end=start+align_up(size,PAGE);
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (vmas[i].kind!=KIND_RESERVED) {
            uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
            queue_hook(HOOK_UNMAP,a,b-a);
        }
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i) {
        uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
        if (host_unmap(a,b-a)) return INVALID;
    }
    drop_range(start,end);
    return 0;
}
static ABI int32_t direct_unmap(void *address, uint64_t size) {
    /* The GPU forgets the range before it disappears (as shadPS4's early GPU unmap). */
    if (hook_unmap && !((uintptr_t)address%PAGE) && size) hook_unmap((uintptr_t)address,align_up(size,PAGE));
    write_lock();
    size_t before=pending_count;
    int32_t result=unmap_locked((uintptr_t)address,size);
    pending_count=before; /* already notified above */
    write_unlock();
    return result;
}
static ABI int32_t direct_release(uint64_t start, uint64_t size) {
    if (start % PAGE || size % PAGE) return INVALID;
    if (!size) return 0;
    write_lock();
    if (!direct_range_allocated(start,size)) { write_unlock(); return INVALID; }
    /* Mappings of released physical pages disappear with them. */
    for (size_t i=0; i<vma_count;) {
        Vma v=vmas[i];
        uint64_t length=v.end-v.start;
        if (v.kind==KIND_DIRECT && v.phys<start+size && start<v.phys+length) {
            uint64_t a=v.phys>start ? v.phys : start, b=v.phys+length<start+size ? v.phys+length : start+size;
            unmap_locked(v.start+(a-v.phys),b-a);
            i=vma_index(v.start);
            continue;
        }
        ++i;
    }
    for (int i=0; i<LIMIT; ++i) {
        Block *b=&blocks[i];
        if (!b->used || b->start>=start+size || start>=b->start+b->size) continue;
        uint64_t a=b->start>start ? b->start : start, e=b->start+b->size<start+size ? b->start+b->size : start+size;
        Block left=*b, right=*b;
        left.size=a-b->start; right.start=e; right.size=b->start+b->size-e;
        b->used=0;
        if (left.size) *b=left;
        if (right.size) for (int j=0;j<LIMIT;++j) if (!blocks[j].used) { blocks[j]=right; break; }
        live_bytes-=e-a;
    }
    host_discard(start,size); /* zero on reuse */
    write_unlock();
    flush_hooks();
    return 0;
}
static int32_t map_flexible_locked(void **inout, uint64_t size, int prot, int flags) {
    if (!inout || !size || size%PAGE || (prot & ~0x37)) return INVALID;
    if (flexible_bytes+size>FLEXIBLE_SIZE || pool()) return NO_MEMORY;
    flex_fail_need=0;
    uint64_t phys=flex_alloc(size);
    if (phys==UINT64_MAX) return NO_MEMORY;
    int32_t result=place(inout,size,prot,flags,PAGE,KIND_FLEXIBLE,0,phys);
    if (result) flex_free(phys,size); else ++flexible_maps;
    return result;
}
static ABI int32_t map_flexible(void **inout, uint64_t size, int prot, int flags) {
    write_lock();
    int32_t result=map_flexible_locked(inout,size,prot,flags);
    write_unlock();
    flush_hooks();
    if (!result) printf("Runtime: mapped flexible memory %p, size=%" PRIu64 ", prot=0x%x\n", *inout, size, prot);
    else fprintf(stderr,"Runtime: flexible map of %" PRIu64 " bytes failed (0x%x), in use=%" PRIu64 "\n",
                 size, (unsigned)result, flexible_bytes);
    if (result && flex_fail_need) /* sotc: R5: the span, not the 448 MiB budget, ran out */
        fprintf(stderr,"Runtime: WARNING flexible span exhausted: no contiguous run of %" PRIu64 " MiB (largest free run %" PRIu64 " MiB, "
                "%" PRIu64 " MiB free of the %" PRIu64 " MiB span, high-water %" PRIu64 " MiB); the guest section cannot grow after boot, "
                "set SOTC_FLEX_SPAN_MB (up to 1024) and restart\n",
                flex_fail_need*PAGE>>20,flex_fail_run*PAGE>>20,flex_fail_free*PAGE>>20,flex_span>>20,flex_high_water>>20);
    return result;
}
static ABI int32_t map_flexible_named(void **inout, uint64_t size, int prot, int flags, const char *name) {
    (void)name; return map_flexible(inout,size,prot,flags);
}
static ABI int32_t release_flexible(void *address, uint64_t size) { return direct_unmap(address,size); }
static ABI int32_t reserve_range(void **inout, uint64_t size, int flags, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!inout || !size || size%PAGE || !valid_alignment(alignment)) return INVALID;
    write_lock();
    if ((flags & MAP_FIXED_FLAG) && overlaps((uintptr_t)*inout,(uintptr_t)*inout+size,0)) {
        write_unlock(); return NO_MEMORY;
    }
    int32_t result=place(inout,size,0,flags,alignment,KIND_RESERVED,0,0);
    write_unlock();
    /* sotc: Shadow of the Colossus reserves thousands of ranges while streaming; BB_MEMORY_LOG=1 lists all. */
    static unsigned reserved_logged;
    static int memory_log=-1;
    if (memory_log<0) memory_log=getenv("BB_MEMORY_LOG")!=NULL;
    if (!result && (memory_log || reserved_logged<16)) {
        printf("Runtime: reserved virtual range %p, size=%" PRIu64 "%s\n", *inout, size,
               ++reserved_logged==16 && !memory_log ? " (further ones: BB_MEMORY_LOG=1)" : "");
    }
    return result;
}
static int32_t protect_locked(uintptr_t start, uint64_t size, int prot, int type) {
    uintptr_t end=start+align_up(size,PAGE);
    start&=~(uintptr_t)(PAGE-1);
    if ((prot & ~0x37) || !covered(start,end,0)) return INVALID;
    if (host_protect(start,end-start,host_prot(prot))) return INVALID;
    int error; size_t i=carve(start,end,&error);
    if (error) return NO_MEMORY;
    for (; i<vma_count && vmas[i].start<end; ++i) { vmas[i].prot=prot; if (type>=0) vmas[i].type=type; }
    queue_hook(HOOK_INVALIDATE,start,end-start);
    ++protects;
    return 0;
}
static ABI int32_t kernel_mprotect(const void *address, uint64_t size, int prot) {
    write_lock();
    int32_t result=protect_locked((uintptr_t)address,size,prot,-1);
    write_unlock();
    flush_hooks();
    return result;
}
static ABI int32_t query_protection(void *address, void **start, void **end, uint32_t *prot) {
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || vmas[i].start>(uintptr_t)address || vmas[i].kind==KIND_RESERVED) {
        read_unlock(); return ACCESS;
    }
    /* Report the whole contiguous run with identical protection. */
    size_t first=i, last=i;
    while (first && vmas[first-1].end==vmas[first].start && vmas[first-1].kind!=KIND_RESERVED && vmas[first-1].prot==vmas[i].prot) --first;
    while (last+1<vma_count && vmas[last+1].start==vmas[last].end && vmas[last+1].kind!=KIND_RESERVED && vmas[last+1].prot==vmas[i].prot) ++last;
    if (start) *start=(void *)vmas[first].start;
    if (end) *end=(void *)vmas[last].end;
    if (prot) *prot=(uint32_t)vmas[i].prot;
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t virtual_query(const void *address, int flags, VirtualQueryInfo *info, uint64_t info_size) {
    if (!info || info_size!=sizeof(*info)) return INVALID;
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || (vmas[i].start>(uintptr_t)address && !(flags & 1))) { read_unlock(); return ACCESS; }
    Vma v=vmas[i];
    memset(info,0,sizeof(*info));
    info->start=v.start; info->end=v.end;
    info->offset=v.kind==KIND_DIRECT ? v.phys : 0;
    info->protection=v.prot; info->memory_type=v.type;
    info->flags=(v.kind==KIND_FLEXIBLE ? 1u : 0) | (v.kind==KIND_DIRECT ? 2u : 0) | (v.kind!=KIND_RESERVED ? 16u : 0);
    strcpy(info->name, v.kind==KIND_RESERVED ? "reserved" : v.kind==KIND_DIRECT ? "direct" : "flexible");
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t direct_memory_type(uint64_t phys, int *type, void **start, void **end) {
    read_lock();
    for (int i=0;i<LIMIT;++i) if (blocks[i].used && phys>=blocks[i].start && phys-blocks[i].start<blocks[i].size) {
        if (type) *type=blocks[i].type;
        if (start) *start=(void *)(uintptr_t)blocks[i].start;
        if (end) *end=(void *)(uintptr_t)(blocks[i].start+blocks[i].size);
        read_unlock(); return 0;
    }
    read_unlock();
    return ACCESS;
}
static ABI int32_t batch_map2(BatchEntry *entries, int count, int *processed, int flags) {
    if (!entries || count<0) return INVALID;
    int32_t result=0; int done=0;
    /* sotc: SOTC_BATCH_CHUNK (default on, 0 = one lock for the whole batch as before): sceKernelBatchMap2
     * held the exclusive lock for up to 60 ms (a batch of maps, unmaps and protects), so the draw thread's
     * per-draw region lookups and the command processor waited (a 92-100 ms frame with nothing else in
     * it: guest-lock waits 175 ms over all threads). Each entry takes the lock on its own and, between
     * entries, lets the threads waiting for it run first. A batch was never atomic for the other threads
     * of the game (it is one call, but the kernel takes its map lock per entry as well). */
    static int chunk=-1;
    if (chunk<0) { const char *e=getenv("SOTC_BATCH_CHUNK"); chunk=!(e && e[0]=='0'); }
    if (!chunk) write_lock();
#ifdef _WIN32
    LARGE_INTEGER batch_t0; if (stall_log_on()) QueryPerformanceCounter(&batch_t0);
#endif
    for (; done<count; ++done) {
        BatchEntry *e=&entries[done];
        if (chunk) write_lock();
#ifdef _WIN32
        LARGE_INTEGER t0; if (stall_log_on()) QueryPerformanceCounter(&t0);
#endif
        switch (e->operation) {
        case 0: result=map_direct_locked(&e->start,e->length,(uint8_t)e->prot,flags,(int64_t)e->offset,0); break;
        case 1: result=unmap_locked((uintptr_t)e->start,e->length); break;
        case 2: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,-1); break;
        case 3: result=map_flexible_locked(&e->start,e->length,(uint8_t)e->prot,flags); break;
        case 4: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,e->type); break;
        default: result=INVALID;
        }
#ifdef _WIN32
        if (stall_log_on()) {
            LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
            double ms=(double)lock_ticks_to_ns(t1.QuadPart-t0.QuadPart)/1e6;
            if (ms>=1.0) fprintf(stderr,"SOTC STALL batch_map2 entry %d of %d: op %d, %.1f MB took %.1f ms\n",
                                 done,count,(int)e->operation,e->length/1048576.0,ms);
        }
#endif
        if (chunk) {
            write_unlock();
#ifdef _WIN32
            for (int i=0; i<20 && __atomic_load_n(&lock_waiters,__ATOMIC_RELAXED); ++i) SwitchToThread();
#endif
        }
        if (result) break;
    }
    if (!chunk) write_unlock();
#ifdef _WIN32
    if (stall_log_on()) {
        LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
        double ms=(double)lock_ticks_to_ns(t1.QuadPart-batch_t0.QuadPart)/1e6;
        if (ms>=5.0) fprintf(stderr,"SOTC STALL batch_map2 of %d entries took %.1f ms\n",count,ms);
    }
#endif
    flush_hooks();
    if (processed) *processed=done;
    return result;
}
static ABI int32_t batch_map(BatchEntry *entries, int count, int *processed) {
    return batch_map2(entries,count,processed,MAP_FIXED_FLAG);
}
/* sotc: largest free direct-memory range in [low,high) at the given alignment. */
static ABI int32_t direct_available(int64_t low, int64_t high, uint64_t alignment, int64_t *physical, uint64_t *size) {
    if (!alignment) alignment = PAGE;
    if (!physical || !size || low < 0 || high < 0 || !valid_alignment(alignment)) return INVALID;
    write_lock();
    (void)pool();
    uint64_t end = (uint64_t)high < POOL_SIZE ? (uint64_t)high : POOL_SIZE, best=0, best_start=0;
    uint64_t cursor = (uint64_t)low;
    while (cursor < end) {
        uint64_t next = end, resume = end;
        for (int i=0; i<LIMIT; ++i) {
            Block b = blocks[i];
            if (!b.used || b.start+b.size <= cursor) continue;
            if (b.start <= cursor) { next = cursor; resume = b.start+b.size; break; }
            if (b.start < next) { next = b.start; resume = b.start+b.size; }
        }
        uint64_t start = align_up(cursor, alignment);
        if (next > start && next-start > best) { best = next-start; best_start = start; }
        cursor = resume;
    }
    write_unlock();
    if (!best) return NO_MEMORY;
    *physical = (int64_t)best_start; *size = best;
    return 0;
}
static ABI int32_t flexible_available(uint64_t *size) {
    if (!size) return INVALID;
    *size = FLEXIBLE_SIZE - flexible_bytes;
    return 0;
}
static const RuntimeExport exports[]={
    {"sceKernelAvailableDirectMemorySize",direct_available}, {"sceKernelAvailableFlexibleMemorySize",flexible_available},
    {"sceKernelGetDirectMemorySize",direct_size}, {"sceKernelAllocateDirectMemory",direct_allocate},
    {"sceKernelMapDirectMemory",direct_map}, {"sceKernelMapNamedDirectMemory",direct_map_named},
    {"sceKernelReleaseDirectMemory",direct_release}, {"sceKernelMunmap",direct_unmap}, {"munmap",direct_unmap},
    {"sceKernelMapFlexibleMemory",map_flexible}, {"sceKernelMapNamedFlexibleMemory",map_flexible_named},
    {"sceKernelReleaseFlexibleMemory",release_flexible}, {"sceKernelReserveVirtualRange",reserve_range},
    {"sceKernelMprotect",kernel_mprotect}, {"sceKernelQueryMemoryProtection",query_protection},
    {"sceKernelVirtualQuery",virtual_query}, {"sceKernelGetDirectMemoryType",direct_memory_type},
    {"sceKernelBatchMap",batch_map}, {"sceKernelBatchMap2",batch_map2},
};
uintptr_t runtime_memory_resolve(const char *name) {
    /* Direct-memory NIDs are exercised by test_runtime.c before names are loaded. */
    if (!strcmp(name, "pO96TwzOm5E")) return (uintptr_t)direct_size;
    if (!strcmp(name, "rTXw65xmLIA")) return (uintptr_t)direct_allocate;
    if (!strcmp(name, "L-Q3LEjIbgA")) return (uintptr_t)direct_map;
    if (!strcmp(name, "MBuItvba6z8")) return (uintptr_t)direct_release;
    if (!strcmp(name, "cQke9UuBQOk")) return (uintptr_t)direct_unmap;
    return RUNTIME_LOOKUP(exports,name);
}
/* Host-owned memory the guest can see (image, stacks, trampolines) lives in
 * [LOW_MIN, USER_MIN): PS4 code packs pointers into 40-bit fields. */
#define LOW_MIN UINT64_C(0x0800000000)
static uintptr_t low_next=LOW_MIN;
void *runtime_low_map(size_t size, int prot) {
    size=align_up(size,PAGE);
    write_lock();
#ifdef _WIN32
    void *p=NULL;
    while (!p && low_next+size<=USER_MIN) {
        p=VirtualAlloc((void *)low_next,size,MEM_RESERVE|MEM_COMMIT,win_prot(prot));
        low_next+=size+UINT64_C(65536); /* unmapped gap catches overruns; allocation granularity */
    }
    write_unlock();
    return p;
#else
    void *p=MAP_FAILED;
    while (low_next+size<=USER_MIN) {
        p=mmap((void *)low_next,size,prot,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        low_next+=size+PAGE; /* unmapped gap catches overruns */
        if (p!=MAP_FAILED) break;
    }
    write_unlock();
    return p==MAP_FAILED ? NULL : p;
#endif
}
/* ---- GPU library interface (gpu/shim/bbgpu.cpp) ---- */
/* Optimizations switched off at run time (diagnostics): the number in the file named by
 * BB_TOGGLE_FILE, re-read every 250 ms. Bits: 1 region cache, 2 fetch shader cache,
 * 4 page tracking early exit, 8 pending-op poll limit, 16 threaded Vulkan recording,
 * 32 texture descriptor cache, 64 lock-free upload check, 128 image lookup cache,
 * 256 buffer uploads on the recording thread, 512 barrier tracker insert memo,
 * 1024 texture binding memo, 2048 page-granular read tracking for barriers,
 * 4096 resource sharps read by the draw-preparation workers,
 * 8192 prepared draws from the draw-preparation workers,
 * 16384 small read-only buffer copies on the recording thread,
 * 32768 hot pages (opt-in with BB_HOT_PAGES=1),
 * 65536 unprotect the 256 KiB window around a guest write fault (BB_FAULT_WINDOW KiB),
 * 131072 large guest memory copies split across copy threads (BB_COPY_THREADS),
 * 262144 with BB_ASYNC_FENCES=1: wait for guest copies at fences again,
 * 524288 small guest copies batched for the copy threads instead of the recording thread,
 * 1073741824 the lock-free UpdateImage path for clean, tracked images. */
/* Bits 32 and up: the draw pipeline and related GPU thread work (gpu/shim/bbport_toggles.h). */
/* sotc: bit 58 (PipelineKeyMemo) set: rebuild the graphics pipeline key on every draw even when
 * no register chunk changed since the last one (A/B of the key memo). */
/* sotc: bit 61 (LowLatencyWait) set: keep the 15.6 ms Sleep(1) lock backoff AND the default timer
 * resolution (see lock_backoff and runtime_set_timer_resolution); clear: WaitOnAddress and 1 ms.
 * The bit decides both, at boot (runtime_toggle_init reads BB_TOGGLE_FILE before the timer) and on
 * an in-run change (toggle_watcher); SOTC_TIMER_RES=0/1 forces it over the file. */
/* sotc: bit 59 (GcLruAdvance) is off by default too: V1 keeps kept tiled images at the LRU head
 * during GC, which only pays off once V14 scales the GC thresholds with the VRAM budget. Until it
 * is validated on 3-4 GB cards the absolute thresholds make the strong GC passes cost up to 16%
 * of the draw thread on 12 GB (and the pass itself is only useful once V1 fixed the GC). Bit 59
 * set = the old LRU behaviour. Bit 60 (VmaBlock64) is gone: the VMA block size is fixed when the
 * allocator is created, before any toggle file is applied (SOTC_VMA_BLOCK_MB). */
/* sotc: bit 39 (PendingFenceWaits) is off by default. It lets a graphics WAIT_REG_MEM pass on a
 * value a fence still queued for the draw recording thread will write, but Shadow of the
 * Colossus also waits there for labels its compute queues write: the frame then went ahead
 * without the compute results and the scene was never drawn (black screen in about half of
 * the boots). A BB_TOGGLE_FILE value replaces this default. */
/* sotc: bit 60 (DrawFixedCostOpt) is off by default too: the register-diff transport and the
 * dynamic-state memo measured 0 +-2% FPS (2026-10-22, field and attract), and a desync between
 * back_shadow and back_regs would corrupt registers silently. Kept for experiments. */
/* sotc: bit 28 (DepthFastPath) is clear = on by default (set = the old path): a depth-only draw replays the
 * render target and dynamic-state work whose inputs did not change (docs/sotc/DEPTH-FASTPATH.md). It also makes
 * the command processor send registers as changed dwords (the transport bit 60 would give), whichever the
 * state of bit 60. Honoured live, read per draw. */
/* sotc: bit 20 (CpFastPaths) is SET = off by default: the command processor's Nop skip, per-packet poll guard
 * and fine pipeline key memo are exact (SOTC_CP_VERIFY=1: 0 bad in 13 M checks) and take 0.4 ms of the
 * command processor's ~11 ms in the heavy temple, but the draw thread is the limiter there and the frame time
 * did not move measurably (docs/sotc/SIGUIENTE-LIMITE.md). SOTC_CP_FAST=1 turns it on (see toggle_mask_env). */
/* sotc: bit 40 (DispatchFastPath) is SET = off by default: exact (SOTC_DISPFAST_VERIFY=1: 0 bad in >100 M checks) but it
 * saved ~0.3 +- 0.17 ms of the draw thread in the heavy temple, under the 0.5 ms bar and with no visible FPS gain
 * (docs/sotc/DISPATCH-FASTPATH.md). SOTC_DISPFAST=1 turns it on (see toggle_mask_env). */
/* sotc: bit 41 (DrawReplay) is clear = on by default (set = the old path): unchanged flat buffers are not copied per op and the
 * reusable-bind key of a draw is compared field by field instead of hashed. Exact (SOTC_DRAWREPLAY_VERIFY=1: 0 differences against the
 * hash in >30 M comparisons); -1.2 +- 0.33 ms of draw thread and +2.7 +- 0.8 FPS in the heavy temple (docs/sotc/DRAW-REPLAY.md).
 * SOTC_DRAWREPLAY=0 pins the old path (see toggle_mask_env). */
/* sotc: bits 20 (CpFastPaths) and 40 (DispatchFastPath) are on by default since 0.60.6-plus14: ABBA on the RTX 5060 Ti
 * gave +4% in the Shrine (73.4 -> 76.4 FPS, same cmds/frame) and about +6% on the wide desert route; SOTC_CP_VERIFY=1 and
 * SOTC_DISPFAST_VERIFY=1 reported 0 mismatches in >50 M checks. SOTC_CP_FAST=0 / SOTC_DISPFAST=0 turn them off. */
uint64_t runtime_disabled_optimizations=UINT64_C(1)<<39 | UINT64_C(1)<<59 | UINT64_C(1)<<60;
/* Speculative readers of guest memory (GPU draw-preparation workers) register a recovery
 * point: a fault on that thread jumps back to it instead of terminating (probe.c). */
__thread sigjmp_buf *runtime_fault_recover;
/* sotc: settings resolved at startup (the timer resolution) cannot wait for the watcher's first
 * 250 ms poll, so the file is read once synchronously here (runtime_memory_reserve, before the
 * timer; the watcher repeats it only for the bits that can change in a run).
 * SOTC_TIMER_RES=0/1 overrides bit 61 and pins it: the watcher keeps applying it, so a bench that
 * A/B toggles the bit in the file must leave the variable unset. */
static uint64_t toggle_mask_initial; /* the watcher starts from this, so it does not re-print */
/* sotc: the mask in the toggle file (the watcher's format: one unsigned number, 0 when the file
 * is missing or unreadable). Returns 0 when nothing could be read, like the watcher's poll. */
static int toggle_mask_read(const char *path, uint64_t *mask) {
    FILE *f=path ? fopen(path,"r") : NULL;
    unsigned long long value=0;
    if (!f) return 0;
    int ok=fscanf(f,"%llu",&value)==1;
    fclose(f);
    if (ok) *mask=(uint64_t)value; /* sotc: a missing/unreadable file must not apply mask 0 */
    return ok;
}
static uint64_t toggle_mask_env(uint64_t mask) {
    /* sotc: SOTC_CP_FAST=1/0 pins bit 20 (CpFastPaths) like SOTC_TIMER_RES pins bit 61: a bench that A/B toggles
     * the bit in the file must leave the variable unset. */
    const char *fast=getenv("SOTC_CP_FAST");
    if (fast && fast[0]=='1') mask&=~(UINT64_C(1)<<20);
    else if (fast && fast[0]=='0') mask|=(UINT64_C(1)<<20);
    /* sotc: SOTC_DISPFAST=1/0 pins bit 40 (DispatchFastPath, set = off by default until accepted), see SOTC_CP_FAST. */
    const char *dfast=getenv("SOTC_DISPFAST");
    if (dfast && dfast[0]=='1') mask&=~(UINT64_C(1)<<40);
    else if (dfast && dfast[0]=='0') mask|=(UINT64_C(1)<<40);
    /* sotc: SOTC_DRAWREPLAY=1/0 pins bit 41 (DrawReplay, set = the old path, clear by default), see SOTC_CP_FAST. */
    const char *dreplay=getenv("SOTC_DRAWREPLAY");
    if (dreplay && dreplay[0]=='1') mask&=~(UINT64_C(1)<<41);
    else if (dreplay && dreplay[0]=='0') mask|=(UINT64_C(1)<<41);
    const char *env=getenv("SOTC_TIMER_RES");
    if (!env) return mask;
    if (env[0]=='1') return mask&~(UINT64_C(1)<<61);
    if (env[0]=='0') return mask|(UINT64_C(1)<<61);
    return mask;
}
static void runtime_toggle_init(void) {
    static int done;
    if (__atomic_exchange_n(&done,1,__ATOMIC_RELAXED)) return;
    const uint64_t compiled=runtime_disabled_optimizations;
    uint64_t mask=compiled, file_mask=0;
    if (toggle_mask_read(getenv("BB_TOGGLE_FILE"),&file_mask)) mask=file_mask;
    mask=toggle_mask_env(mask);
    toggle_mask_initial=mask;
    __atomic_store_n(&runtime_disabled_optimizations,mask,__ATOMIC_RELEASE);
    /* sotc: always printed: the A/B analyzers take each run's starting mask from this line. */
    printf("Runtime: disabled optimizations mask=%llu\n",(unsigned long long)mask);
    runtime_set_timer_resolution(!(mask&(UINT64_C(1)<<61)));
}
static void *toggle_watcher(void *path) {
    runtime_toggle_init(); /* no-op when runtime_memory_reserve already read the file */
    for (uint64_t last=toggle_mask_initial;;) {
        /* sotc: a poll that reads nothing (file missing, or caught mid-rewrite by an A/B script)
         * keeps the current mask. Applying 0 would also clear bit 39 (PendingFenceWaits), which
         * blacks out Shadow of the Colossus. */
        uint64_t value=last;
        if (toggle_mask_read(path,&value)) value=toggle_mask_env(value);
        if (value!=last) {
            const uint64_t previous=__atomic_load_n(&runtime_disabled_optimizations,__ATOMIC_RELAXED);
            __atomic_store_n(&runtime_disabled_optimizations,value,__ATOMIC_RELEASE);
            printf("Runtime: disabled optimizations mask=%llu\n",(unsigned long long)value);
            /* sotc: an in-run A/B of bit 61 must switch the timer too, not only the Sleep(1). */
            if ((previous^value)&(UINT64_C(1)<<61))
                runtime_set_timer_resolution(!(value&(UINT64_C(1)<<61)));
            last=value;
        }
#ifdef _WIN32
        compat_sleep_ns(250000000);
#else
        struct timespec t={0,250000000}; nanosleep(&t,NULL);
#endif
    }
    return NULL;
}
void runtime_memory_set_gpu_hooks(GpuRange map, GpuRange unmap, GpuRange invalidate) {
    const char *toggles=getenv("BB_TOGGLE_FILE");
    static pthread_t watcher;
    if (toggles && !watcher) pthread_create(&watcher,NULL,toggle_watcher,(void *)toggles);
    write_lock();
    hook_map=map; hook_unmap=unmap; hook_invalidate=invalidate;
    for (size_t i=0;i<vma_count;++i) if (vmas[i].kind!=KIND_RESERVED) queue_hook(HOOK_MAP,vmas[i].start,vmas[i].end-vmas[i].start);
    write_unlock();
    flush_hooks();
}
/* Writes through the backing view; 0 when part of the range has no backing. */
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size) {
    read_lock();
    int ok=1;
    for (uintptr_t at=address, end=address+size; at<end && ok;) {
        size_t i=vma_index(at);
        if (i==vma_count || vmas[i].start>at || vmas[i].kind==KIND_RESERVED) { ok=0; break; }
        uint64_t n=(vmas[i].end<end ? vmas[i].end : end)-at;
        memcpy(backing_base+vmas[i].phys+(at-vmas[i].start),(const unsigned char *)data+(at-address),n);
        at+=n;
    }
    read_unlock();
    return ok;
}
/* sotc: host pointer to the backing of a mapped guest address (bypasses GPU page protection),
 * valid to the end of its VMA; NULL for reserved or unmapped addresses. */

/* Per-thread cache of recently found regions for the GPU's per-draw queries:
 * entries hold for as long as the table generation they were read at. */
typedef struct { uint64_t generation; uintptr_t start, end; int kind; const unsigned char *backing; } CachedRegion; /* backing: view of start */
static __thread CachedRegion region_cache[8];
static __thread unsigned region_cache_next;
/* sotc: plus 512 direct-mapped entries by 16 KiB of address. The command processor's buffer binds
 * touch more regions than the 8 recent ones, so most lookups took the read lock and a binary
 * search (~4.5% of that thread: libwinpthread, vma_index). SOTC_REGION_DIRECT=0: ring only. */
#define REGION_DIRECT_SLOTS 512
/* sotc: PERFIL-2.md C2 (BbToggle::MicroCostSweep, bit 23 of the toggle mask; set = the code above
 * and below as it was). The direct table was filled only at the slot of the address that missed,
 * and a VMA is far bigger than 16 KiB: every other 16 KiB of the same region missed again, and so
 * did every region that aliased in the 8 MiB the table spans (the buffer binds of the draw thread
 * touch many more). The new path has 2048 slots and, behind them, a per-thread copy of the VMA
 * table valid for one even table generation (the invariant of cached_region), searched without a
 * lock; it is rebuilt under the read lock by the first lookup that misses after a generation
 * change. Same answers as the locked search: a hit means the address lies inside a VMA of that
 * generation, anything else (a gap included) still takes the lock. */
#define REGION_DIRECT_NEW_SLOTS 2048
typedef struct { uintptr_t start, end; int kind; const unsigned char *backing; } SnapRegion;
static __thread SnapRegion *region_snap;
static __thread size_t region_snap_count, region_snap_capacity;
static __thread uint64_t region_snap_gen=1; /* odd: never valid */
static uint64_t region_stat_slow, region_stat_slow_draw, region_stat_snap_builds; /* relaxed counters, slow paths only */
static __thread CachedRegion region_direct[REGION_DIRECT_SLOTS];
/* sotc: nothing here is allocated by a thread that never takes a slow lookup: the 96 KiB direct table and
 * the VMA copy belong to a heap RegionOwner created by the first slow lookup of the thread, released by
 * the pthread key destructor when the thread exits (guest threads are host pthreads; fibers only
 * switch contexts inside one host thread, so the thread is the right owner). region_live_* feed the
 * "SOTC region cache" report line. */
typedef struct { SnapRegion *snap; size_t capacity; CachedRegion *direct; } RegionOwner;
static __thread CachedRegion *region_direct_new;
static __thread RegionOwner *region_owner;
static pthread_key_t region_key;
static pthread_once_t region_key_once=PTHREAD_ONCE_INIT;
static int region_key_ok;
static uint64_t region_live_threads, region_live_bytes;
static void region_owner_free(void *p) {
    RegionOwner *o=p;
    if (!o) return;
    const uint64_t bytes=sizeof(*o)+(uint64_t)o->capacity*sizeof(SnapRegion)+(o->direct ? sizeof(CachedRegion)*REGION_DIRECT_NEW_SLOTS : 0);
    free(o->snap); free(o->direct); free(o);
    __atomic_sub_fetch(&region_live_threads,1,__ATOMIC_RELAXED);
    __atomic_sub_fetch(&region_live_bytes,bytes,__ATOMIC_RELAXED);
    /* the exiting thread's own view: later lookups on it (other exit callbacks) take the lock */
    region_owner=NULL; region_direct_new=NULL; region_snap=NULL; region_snap_count=0; region_snap_capacity=0; region_snap_gen=1;
}
static void region_key_make(void) { region_key_ok=pthread_key_create(&region_key,region_owner_free)==0; }
static int region_direct_on=-1; /* -1 until pool(): lookups only, no fills */
static void region_direct_init(void) {
    const char *v=getenv("SOTC_REGION_DIRECT");
    region_direct_on=!(v && v[0]=='0');
}
static size_t region_direct_slot(uintptr_t address) { return (address>>14)&(REGION_DIRECT_SLOTS-1); }
static int region_sweep_on(void) {
    return region_direct_on>0 && !(__atomic_load_n(&runtime_disabled_optimizations,__ATOMIC_RELAXED)&(UINT64_C(1)<<23));
}
static const CachedRegion *cached_region_sweep(uintptr_t address, uint64_t generation) {
    if (!region_direct_new) return NULL; /* no copy yet: this thread never took a slow lookup */
    CachedRegion *d=&region_direct_new[(address>>14)&(REGION_DIRECT_NEW_SLOTS-1)];
    if (d->generation==generation && d->end && d->start<=address && address<d->end) return d;
    if (region_snap_gen!=generation) return NULL;
    size_t lo=0, hi=region_snap_count; /* first region with end > address, as vma_index */
    while (lo<hi) { size_t mid=(lo+hi)/2; if (region_snap[mid].end<=address) lo=mid+1; else hi=mid; }
    if (lo==region_snap_count || region_snap[lo].start>address) return NULL;
    const SnapRegion *r=&region_snap[lo];
    const CachedRegion entry={generation,r->start,r->end,r->kind,r->backing};
    *d=entry;
    return d;
}
/* Under the read lock (the generation is even and cannot change): copy the table. */
static void region_snap_build(uint64_t generation) {
    if (!region_owner) {
        pthread_once(&region_key_once,region_key_make);
        if (!region_key_ok) return;
        RegionOwner *o=calloc(1,sizeof(*o));
        CachedRegion *d=o ? calloc(REGION_DIRECT_NEW_SLOTS,sizeof(*d)) : NULL;
        if (!d) { free(o); return; }
        o->direct=d;
        if (pthread_setspecific(region_key,o)) { free(d); free(o); return; }
        region_owner=o; region_direct_new=d;
        __atomic_add_fetch(&region_live_threads,1,__ATOMIC_RELAXED);
        __atomic_add_fetch(&region_live_bytes,sizeof(*o)+sizeof(CachedRegion)*REGION_DIRECT_NEW_SLOTS,__ATOMIC_RELAXED);
    }
    if (region_snap_capacity<vma_count) {
        size_t capacity=region_snap_capacity ? region_snap_capacity : 256;
        while (capacity<vma_count) capacity*=2;
        SnapRegion *next=realloc(region_snap,capacity*sizeof(*next));
        if (!next) return;
        __atomic_add_fetch(&region_live_bytes,(uint64_t)(capacity-region_snap_capacity)*sizeof(*next),__ATOMIC_RELAXED);
        region_snap=next; region_snap_capacity=capacity;
        region_owner->snap=next; region_owner->capacity=capacity;
    }
    for (size_t i=0;i<vma_count;++i) {
        const Vma *v=&vmas[i];
        region_snap[i]=(SnapRegion){v->start,v->end,v->kind,
            v->kind!=KIND_RESERVED && backing_base ? backing_base+v->phys : NULL};
    }
    region_snap_count=vma_count; region_snap_gen=generation;
    __atomic_add_fetch(&region_stat_snap_builds,1,__ATOMIC_RELAXED);
}
/* sotc: C2, the whole answer from the snapshot when it is current (no lock). snap_ready is true
 * while the table generation is even and equals the one the snapshot was copied at, the same
 * validity cached_region relies on. snap_index is vma_index over the copy. */
static int snap_ready(void) {
    const uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    return !(generation & 1) && region_snap_gen==generation && region_sweep_on();
}
static size_t snap_index(uintptr_t a) {
    size_t lo=0, hi=region_snap_count;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (region_snap[mid].end<=a) lo=mid+1; else hi=mid; }
    return lo;
}
/* Slow paths, under the read lock: make sure the next lookup of this generation can be answered
 * from the snapshot even when the lookup itself (a gap, a reserved range) caches nothing. */
static void snap_refresh_locked(void) {
    const uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if (!(generation & 1) && region_snap_gen!=generation && region_sweep_on()) region_snap_build(generation);
}
/* sotc: SOTC_REGION_VERIFY=1 (diagnostic, C2): every answer taken from the snapshot is compared with
 * the locked search of the table, when the table generation did not move in between; mismatches are
 * logged (first 8) and counted. */
static uint64_t region_stat_verified, region_stat_mismatch;
static int region_verify_on(void) {
    static int on=-1;
    if (on<0) { const char *e=getenv("SOTC_REGION_VERIFY"); on=(e && e[0]=='1') ? 1 : 0; }
    return on;
}
static void region_verify_done(const char *what, uintptr_t address, uint64_t size, int same) {
    __atomic_add_fetch(&region_stat_verified,1,__ATOMIC_RELAXED);
    if (same) return;
    if (__atomic_add_fetch(&region_stat_mismatch,1,__ATOMIC_RELAXED)<=8)
        fprintf(stderr,"SOTC REGION MISMATCH %s address %#llx size %#llx\n",what,(unsigned long long)address,(unsigned long long)size);
}
/* Takes the read lock for a locked recomputation; 0 (lock released) when the generation moved. */
static int region_verify_begin(uint64_t generation) {
    read_lock();
    if (__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE)!=generation) { read_unlock(); return 0; }
    return 1;
}
static void region_verify_clamp(uintptr_t address, uint64_t size, uint64_t got, uint64_t generation) {
    if (!region_verify_begin(generation)) return;
    uint64_t length=0;
    for (size_t i=vma_index(address); i<vma_count && length<size; ++i) {
        if (vmas[i].start>address+length || vmas[i].kind==KIND_RESERVED) break;
        length=vmas[i].end-address;
    }
    read_unlock();
    region_verify_done("clamp",address,size,(length<size ? length : size)==got);
}
static void region_verify_mapped(uintptr_t address, uint64_t size, int got, uint64_t generation) {
    if (!region_verify_begin(generation)) return;
    const int want=covered(address,address+size,0);
    read_unlock();
    region_verify_done("is_mapped",address,size,want==got);
}
static void region_verify_region(uintptr_t address, int got, uintptr_t start, uintptr_t end, int mapped, uint64_t generation) {
    if (!region_verify_begin(generation)) return;
    const size_t i=vma_index(address);
    const int found=i<vma_count;
    uintptr_t s=start, e=end; int m=mapped;
    if (found && vmas[i].start<=address) { s=vmas[i].start; e=vmas[i].end; m=vmas[i].kind!=KIND_RESERVED; }
    else if (found) { s=i ? vmas[i-1].end : 0; e=vmas[i].start; m=0; }
    read_unlock();
    region_verify_done("region",address,0,found==got && (!found || (s==start && e==end && m==mapped)));
}
static void region_verify_backing(uintptr_t address, const unsigned char *got, uint64_t generation) {
    if (!region_verify_begin(generation)) return;
    const size_t i=vma_index(address);
    const unsigned char *p=NULL;
    if (i<vma_count && vmas[i].start<=address && vmas[i].kind!=KIND_RESERVED && backing_base)
        p=backing_base+vmas[i].phys+(address-vmas[i].start);
    read_unlock();
    region_verify_done("backing",address,0,p==got);
}
static __attribute__((noinline)) void region_slow_note(void) {
    __atomic_add_fetch(&region_stat_slow,1,__ATOMIC_RELAXED);
#ifdef _WIN32
    if (lock_draw_thread==GetCurrentThreadId()) __atomic_add_fetch(&region_stat_slow_draw,1,__ATOMIC_RELAXED);
#endif
}
static const CachedRegion *cached_region(uintptr_t address) {
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if (generation & 1) return NULL;
    if (region_sweep_on()) return cached_region_sweep(address,generation);
    if (region_direct_on) {
        const CachedRegion *d=&region_direct[region_direct_slot(address)];
        if (d->generation==generation && d->end && d->start<=address && address<d->end) return d;
    }
    for (unsigned i=0;i<8;++i) {
        const CachedRegion *c=&region_cache[i];
        if (c->generation==generation && c->end && c->start<=address && address<c->end) return c;
    }
    return NULL;
}
/* Called under the read lock after a lookup of address. */
static void cache_region(size_t i, uintptr_t address) {
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if (i==vma_count || (generation & 1)) return;
    const unsigned char *backing=vmas[i].kind!=KIND_RESERVED && backing_base ? backing_base+vmas[i].phys : NULL;
    const CachedRegion entry={generation,vmas[i].start,vmas[i].end,vmas[i].kind,backing};
    if (region_sweep_on()) { /* sotc: C2, the ring is not consulted on this path */
        if (region_snap_gen!=generation) region_snap_build(generation);
        if (region_direct_new) region_direct_new[(address>>14)&(REGION_DIRECT_NEW_SLOTS-1)]=entry;
        return;
    }
    region_cache[region_cache_next++ % 8]=entry;
    if (region_direct_on>0) region_direct[region_direct_slot(address)]=entry;
}
/* sotc: slow lookups (the read lock and a binary search), snapshot rebuilds and the table
 * generation, for the "SOTC region cache" line of the report. */
void runtime_memory_region_stats(uint64_t *slow, uint64_t *slow_draw, uint64_t *snap_builds, uint64_t *generation,
                                 uint64_t *verified, uint64_t *mismatch, uint64_t *threads, uint64_t *bytes) {
    *threads=__atomic_load_n(&region_live_threads,__ATOMIC_RELAXED);
    *bytes=__atomic_load_n(&region_live_bytes,__ATOMIC_RELAXED);
    *verified=__atomic_load_n(&region_stat_verified,__ATOMIC_RELAXED);
    *mismatch=__atomic_load_n(&region_stat_mismatch,__ATOMIC_RELAXED);
    *slow_draw=__atomic_load_n(&region_stat_slow_draw,__ATOMIC_RELAXED);
    *slow=__atomic_load_n(&region_stat_slow,__ATOMIC_RELAXED);
    *snap_builds=__atomic_load_n(&region_stat_snap_builds,__ATOMIC_RELAXED);
    *generation=__atomic_load_n(&table_generation,__ATOMIC_RELAXED);
}
/* sotc: the VMA table generation, for caches of backing pointers outside this file: a pointer
 * obtained while it was even stays valid until it changes (the invariant cached_region relies
 * on: only write_lock/write_unlock bump it; page protection takes the read lock). Odd (never
 * cache) while the table changes or when the GPU caches are disabled. */
uint64_t runtime_memory_generation(void) {
    if (runtime_disabled_optimizations & 1) return 1;
    return __atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
}
/* sotc: the GPU reads guest memory through the backing view on every upload; the per-thread
 * region cache spares it the table lock (a winpthread rwlock, slow) and the binary search. */
const unsigned char *runtime_memory_backing_pointer(uintptr_t address) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && c->backing) return c->backing+(address-c->start);
    if (snap_ready()) { /* sotc: C2, what the locked search below answers, from the copy */
        const size_t i=snap_index(address);
        const unsigned char *answer=NULL; int known=1;
        if (i==region_snap_count || region_snap[i].start>address) answer=NULL;
        else if (region_snap[i].backing) answer=region_snap[i].backing+(address-region_snap[i].start);
        else if (region_snap[i].kind==KIND_RESERVED) answer=NULL;
        else known=0; /* mapped without a backing view when copied: let the locked path decide */
        if (known) {
            if (region_verify_on()) region_verify_backing(address,answer,region_snap_gen);
            return answer;
        }
    }
    region_slow_note();
    read_lock();
    snap_refresh_locked();
    size_t i=vma_index(address);
    const unsigned char *p=NULL;
    if (i<vma_count && vmas[i].start<=address) {
        cache_region(i,address);
        if (vmas[i].kind!=KIND_RESERVED && backing_base) p=backing_base+vmas[i].phys+(address-vmas[i].start);
    }
    read_unlock();
    return p;
}
/* Mapped length from address, up to size, across adjacent mappings. */
/* sotc: mapped length of [address, address+size) from the cached regions alone, walking
 * adjacent mappings (buffers that span two VMAs missed the one-region check every time and took
 * the read lock); 0 when a region on the way is not cached. */
static uint64_t cached_mapped_length(uintptr_t address, uint64_t size) {
    if (runtime_disabled_optimizations & 1) return 0;
    uint64_t length=0;
    for (unsigned hops=0; length<size && hops<4; ++hops) {
        const CachedRegion *c=cached_region(address+length);
        if (!c || c->kind==KIND_RESERVED) return 0;
        length=c->end-address;
    }
    return length>=size ? size : 0;
}
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size) {
    if (size && cached_mapped_length(address,size)==size) return size;
    if (snap_ready()) { /* sotc: C2, the loop below over the copy (a range running past the end of its mapping never caches) */
        uint64_t length=0;
        for (size_t i=snap_index(address); i<region_snap_count && length<size; ++i) {
            if (region_snap[i].start>address+length || region_snap[i].kind==KIND_RESERVED) break;
            length=region_snap[i].end-address;
        }
        if (region_verify_on()) region_verify_clamp(address,size,length<size ? length : size,region_snap_gen);
        return length<size ? length : size;
    }
    region_slow_note();
    read_lock();
    snap_refresh_locked();
    uint64_t length=0;
    for (size_t i=vma_index(address); i<vma_count && length<size; ++i) {
        if (vmas[i].start>address+length || vmas[i].kind==KIND_RESERVED) break;
        cache_region(i,vmas[i].start>address ? vmas[i].start : address);
        length=vmas[i].end-address;
    }
    read_unlock();
    return length<size ? length : size;
}
/* Region around address: [start,end) mapped or a gap; 0 past the last mapping. */
int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c) { *start=c->start; *end=c->end; *mapped=c->kind!=KIND_RESERVED; return 1; }
    if (snap_ready()) { /* sotc: C2, the locked answer below (gaps included), from the copy */
        const size_t i=snap_index(address);
        const int found=i<region_snap_count;
        if (found && region_snap[i].start<=address) {
            *start=region_snap[i].start; *end=region_snap[i].end; *mapped=region_snap[i].kind!=KIND_RESERVED;
        } else if (found) {
            *start=i ? region_snap[i-1].end : 0; *end=region_snap[i].start; *mapped=0;
        }
        if (region_verify_on()) region_verify_region(address,found,*start,*end,*mapped,region_snap_gen);
        return found;
    }
    region_slow_note();
    read_lock();
    snap_refresh_locked();
    size_t i=vma_index(address);
    int found=i<vma_count;
    if (found && vmas[i].start<=address) {
        cache_region(i,address);
        *start=vmas[i].start; *end=vmas[i].end; *mapped=vmas[i].kind!=KIND_RESERVED;
    } else if (found) {
        *start=i ? vmas[i-1].end : 0; *end=vmas[i].start; *mapped=0;
    }
    read_unlock();
    return found;
}
/* GPU page tracking: restrict host access, never beyond the guest's own protection. */
/* sotc: diagnostics for page tracking problems: the runtime's view of a page (VMA kind and guest
 * protection) next to the protection the host really has. */
void runtime_memory_describe(uintptr_t address, char *out, size_t capacity) {
    read_lock();
    const size_t i=vma_index(address);
    int kind=0, prot=-1;
    uintptr_t start=0, end=0;
    if (i<vma_count && vmas[i].start<=address) { kind=vmas[i].kind; prot=vmas[i].prot; start=vmas[i].start; end=vmas[i].end; }
    read_unlock();
    unsigned long host=0;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void *)address,&mbi,sizeof(mbi))) host=mbi.Protect;
#endif
    snprintf(out,capacity,"vma kind %d prot %#x [%#llx,%#llx), host protect %#lx",kind,prot,
             (unsigned long long)start,(unsigned long long)end,host);
}
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write) {
    read_lock();
    for (size_t i=vma_index(address); i<vma_count && vmas[i].start<address+size; ++i) {
        if (vmas[i].kind==KIND_RESERVED) continue;
        uintptr_t a=vmas[i].start>address ? vmas[i].start : address;
        uintptr_t b=vmas[i].end<address+size ? vmas[i].end : address+size;
        int guest=host_prot(vmas[i].prot);
        int want=(read ? PROT_READ : 0)|(write ? PROT_WRITE|PROT_READ : 0)|(guest & PROT_EXEC);
        host_protect(a,b-a,guest & want);
    }
    read_unlock();
}
/* Host view of a guest range, for the GPU backend: the range must be mapped. */
int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    if (size && cached_mapped_length(address,size)==size) return 1;
    if (snap_ready()) { /* sotc: C2, covered(address,address+size,0) over the copy */
        uintptr_t at=address;
        for (size_t i=snap_index(address); at<address+size; ++i) {
            if (i==region_snap_count || region_snap[i].start>at || region_snap[i].kind==KIND_RESERVED) {
                if (region_verify_on()) region_verify_mapped(address,size,0,region_snap_gen);
                return 0;
            }
            at=region_snap[i].end;
        }
        if (region_verify_on()) region_verify_mapped(address,size,1,region_snap_gen);
        return 1;
    }
    region_slow_note();
    read_lock();
    snap_refresh_locked();
    int result=covered(address,address+size,0);
    if (result) {
        uintptr_t at=address;
        for (size_t i=vma_index(address); i<vma_count && at<address+size; ++i) {
            cache_region(i,at);
            at=vmas[i].end;
        }
    }
    read_unlock();
    return result;
}
void runtime_memory_report(void) {
    printf("Runtime: direct memory allocations=%zu, maps=%zu, live=%" PRIu64 ", budget=%" PRIu64 "\n",
           allocations, maps, live_bytes, POOL_SIZE);
    printf("Runtime: flexible maps=%zu, in use=%" PRIu64 ", protects=%zu, queries=%zu, regions=%zu\n",
           flexible_maps, flexible_bytes, protects, queries, vma_count);
}

