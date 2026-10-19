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

/* Region table lock. Lookups (GPU per-draw queries, VirtualQuery, GPU page
 * protection) share it; map/unmap/protect take it exclusively. Both nest: code
 * under the lock may write guest memory, and a GPU-tracked page then faults into
 * runtime_memory_gpu_protect on the same thread. Readers nest because glibc's
 * default rwlock prefers readers; a thread holding it exclusively skips relocking. */
/* sotc: which functions readers waited in and writers held the lock in (lock sites report). */
typedef struct { const char *name; uint64_t count, ticks; } LockSite;
#define LOCK_SITES 32
static LockSite read_sites[LOCK_SITES] __attribute__((unused)), write_sites[LOCK_SITES] __attribute__((unused));
static __thread const char *lock_site_name, *lock_write_site;
__attribute__((unused)) static void lock_site_add(LockSite *sites, const char *name, uint64_t ticks) {
    if (!name) return;
    for (int i=0;i<LOCK_SITES;++i) {
        const char *n=__atomic_load_n(&sites[i].name,__ATOMIC_ACQUIRE);
        if (!n) {
            const char *expected=NULL;
            if (!__atomic_compare_exchange_n(&sites[i].name,&expected,name,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE) && expected!=name) continue;
            n=name;
        }
        if (n==name) {
            __atomic_add_fetch(&sites[i].count,1,__ATOMIC_RELAXED);
            __atomic_add_fetch(&sites[i].ticks,ticks,__ATOMIC_RELAXED);
            return;
        }
    }
}
#ifdef _WIN32
/* sotc: winpthreads' rwlock queues new readers behind a waiting writer, so a nested read (a
 * reader faulting into runtime_memory_gpu_protect) deadlocked against a mapping thread
 * waiting to write: the GPU command processor, two job workers in the fault handler and
 * sceKernelBatchMap2 all stopped. Each read also took a mutex, thousands of times per frame.
 * This lock prefers readers like glibc's: a read is one atomic operation and never waits for
 * a waiting writer; a writer waits until no reader holds it (maps and unmaps are rare). */
static volatile int64_t lock_state; /* readers, or -1 while a writer holds it */
static void lock_backoff(unsigned *spins) {
    if (++*spins < 64) YieldProcessor();
    else if (*spins < 256) SwitchToThread();
    else Sleep(1); /* Sleep(0) only yields to equal or higher priority: a preempted lower-priority holder could wait seconds */
}
/* sotc: what the writers wait for and hold (maps, unmaps, protection changes): loading a new
 * area stalled with every thread around this table; runtime_memory_lock_stats reports it. */
static uint64_t lock_writes, lock_wait_ticks, lock_hold_ticks, lock_worst_wait_ticks, lock_worst_hold_ticks;
static uint64_t lock_read_wait_ticks, lock_read_waits;
static __thread uint64_t lock_held_since;
static uint64_t lock_ticks(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }
/* sotc: SOTC_LOCK_WAKE=0 goes back to Sleep(1) for readers that find a writer. A WPR trace of an
 * area change: the draw thread waited 0.9 s of 1.9 s here, about 1.2 ms per wait for writes that
 * held the lock 0.1 ms on average (Sleep(1) sleeps a whole timer tick). Readers now block on the
 * lock word and the writer wakes them as it lets go; the 2 ms timeout only bounds a missed wake. */
static int lock_wake(void) {
    static int v=-1;
    if (v<0) { const char *e=getenv("SOTC_LOCK_WAKE"); v=!(e && e[0]=='0'); }
    return v;
}
static volatile LONG lock_sleepers;
static void rw_rdlock(void) {
    int64_t s=__atomic_load_n(&lock_state,__ATOMIC_RELAXED);
    if (s>=0 && __atomic_compare_exchange_n(&lock_state,&s,s+1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return;
    const uint64_t start=lock_ticks();
    for (unsigned spins=0;;) {
        s=__atomic_load_n(&lock_state,__ATOMIC_RELAXED);
        if (s>=0 && __atomic_compare_exchange_n(&lock_state,&s,s+1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) break;
        if (s<0 && spins>=64 && lock_wake()) {
            __atomic_add_fetch(&lock_sleepers,1,__ATOMIC_SEQ_CST);
            int64_t held=-1;
            WaitOnAddress((volatile void *)&lock_state,&held,sizeof(held),2);
            __atomic_sub_fetch(&lock_sleepers,1,__ATOMIC_SEQ_CST);
        } else {
            lock_backoff(&spins);
        }
    }
    const uint64_t waited=lock_ticks()-start;
    __atomic_add_fetch(&lock_read_wait_ticks,waited,__ATOMIC_RELAXED);
    __atomic_add_fetch(&lock_read_waits,1,__ATOMIC_RELAXED);
    lock_site_add(read_sites,lock_site_name,waited);
}
static void rw_rdunlock(void) { __atomic_sub_fetch(&lock_state,1,__ATOMIC_RELEASE); }
static void rw_wrlock(void) {
    const uint64_t start=lock_ticks();
    for (unsigned spins=0;;lock_backoff(&spins)) {
        int64_t s=0;
        if (__atomic_compare_exchange_n(&lock_state,&s,-1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) break;
    }
    const uint64_t now=lock_ticks(), waited=now-start;
    lock_held_since=now;
    ++lock_writes; /* under the lock */
    lock_wait_ticks+=waited;
    if (waited>lock_worst_wait_ticks) lock_worst_wait_ticks=waited;
}
static void rw_wrunlock(void) {
    const uint64_t held=lock_ticks()-lock_held_since;
    lock_hold_ticks+=held;
    if (held>lock_worst_hold_ticks) lock_worst_hold_ticks=held;
    lock_site_add(write_sites,lock_write_site,held);
    /* seq_cst: the store must not pass the load of lock_sleepers (see rw_rdlock) */
    __atomic_store_n(&lock_state,0,__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&lock_sleepers,__ATOMIC_SEQ_CST)) WakeByAddressAll((void *)&lock_state);
}
/* Since the last call: writes, ms writers waited, ms they held it, the longest wait in ms. */
void runtime_memory_lock_stats(uint64_t *writes, double *wait_ms, double *hold_ms, double *worst_ms) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    const double ms=1000.0/(double)f.QuadPart;
    *writes=__atomic_exchange_n(&lock_writes,0,__ATOMIC_RELAXED);
    *wait_ms=(double)__atomic_exchange_n(&lock_wait_ticks,0,__ATOMIC_RELAXED)*ms;
    *hold_ms=(double)__atomic_exchange_n(&lock_hold_ticks,0,__ATOMIC_RELAXED)*ms;
    *worst_ms=(double)__atomic_exchange_n(&lock_worst_wait_ticks,0,__ATOMIC_RELAXED)*ms;
}
/* Since the last call, the sites readers waited longest in and writers held the lock longest in:
 * "name count/ms" by descending ms. */
static void lock_sites_format(LockSite *sites, char *out, size_t capacity, double ms) {
    LockSite copy[LOCK_SITES];
    int n=0;
    for (int i=0;i<LOCK_SITES;++i) {
        const char *name=__atomic_load_n(&sites[i].name,__ATOMIC_ACQUIRE);
        if (!name) break;
        copy[n]=(LockSite){name,__atomic_exchange_n(&sites[i].count,0,__ATOMIC_RELAXED),__atomic_exchange_n(&sites[i].ticks,0,__ATOMIC_RELAXED)};
        if (copy[n].count) ++n;
    }
    size_t used=0; out[0]=0;
    for (int k=0;k<6 && n;++k) {
        int best=0;
        for (int i=1;i<n;++i) if (copy[i].ticks>copy[best].ticks) best=i;
        int w=snprintf(out+used,capacity-used,"%s%s %llu/%.1f",k ? ", " : "",copy[best].name,(unsigned long long)copy[best].count,(double)copy[best].ticks*ms);
        if (w<0 || (size_t)w>=capacity-used) break;
        used+=(size_t)w; copy[best]=copy[--n];
    }
}
void runtime_memory_lock_sites(char *readers, char *writers, size_t capacity) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    const double ms=1000.0/(double)f.QuadPart;
    lock_sites_format(read_sites,readers,capacity,ms);
    lock_sites_format(write_sites,writers,capacity,ms);
}
/* Since the last call: the longest hold in ms, and how often and how long readers waited. */
void runtime_memory_lock_stats2(double *worst_hold_ms, uint64_t *read_waits, double *read_wait_ms) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    const double ms=1000.0/(double)f.QuadPart;
    *worst_hold_ms=(double)__atomic_exchange_n(&lock_worst_hold_ticks,0,__ATOMIC_RELAXED)*ms;
    *read_waits=__atomic_exchange_n(&lock_read_waits,0,__ATOMIC_RELAXED);
    *read_wait_ms=(double)__atomic_exchange_n(&lock_read_wait_ticks,0,__ATOMIC_RELAXED)*ms;
}
#else
static pthread_rwlock_t lock=PTHREAD_RWLOCK_INITIALIZER;
static void rw_rdlock(void) { pthread_rwlock_rdlock(&lock); }
static void rw_rdunlock(void) { pthread_rwlock_unlock(&lock); }
static void rw_wrlock(void) { pthread_rwlock_wrlock(&lock); }
static void rw_wrunlock(void) { pthread_rwlock_unlock(&lock); }
void runtime_memory_lock_stats(uint64_t *writes, double *wait_ms, double *hold_ms, double *worst_ms) {
    *writes=0; *wait_ms=*hold_ms=*worst_ms=0;
}
void runtime_memory_lock_stats2(double *worst_hold_ms, uint64_t *read_waits, double *read_wait_ms) {
    *worst_hold_ms=*read_wait_ms=0; *read_waits=0;
}
void runtime_memory_carve_stats(uint64_t *mib, uint64_t *runs, double *split_ms, double *worst_ms, double *other_ms) {
    *mib=*runs=0; *split_ms=*worst_ms=*other_ms=0;
}
void runtime_memory_lock_sites(char *readers, char *writers, size_t capacity) {
    if (capacity) readers[0]=writers[0]=0;
}
#endif
static __thread unsigned exclusive_depth;
/* Odd while a writer holds the lock; readers' region caches are valid for one even value. */
static uint64_t table_generation;
/* sotc: the VMA range each write changed (SOTC_REGION_LOG=0: none kept). An area change makes
 * ~2000 writes per second (maps, unmaps, the GPU hook queue), so caches valid for one generation
 * were always stale and every GPU lookup took the read lock, then waited for the writer. A
 * cached region stays valid while no write since it was read touched its range, even while a
 * write elsewhere is under way (dirty_lo/hi is published before each change). Slot of the
 * write that ended at even generation g: (g/2) % DIRTY_LOG; generation 0 while it is rewritten. */
#define DIRTY_LOG 64
typedef struct { uint64_t generation; uintptr_t start, end; } DirtyRange;
static DirtyRange dirty_log[DIRTY_LOG];
static uintptr_t dirty_lo, dirty_hi; /* of the write under way */
static int region_log_on(void) {
    static int v=-1;
    if (v<0) { const char *e=getenv("SOTC_REGION_LOG"); v=!(e && e[0]=='0'); }
    return v;
}
/* Before a change of the VMAs in [a,b) (under the write lock). */
static void dirty(uintptr_t a, uintptr_t b) {
    if (a<dirty_lo) __atomic_store_n(&dirty_lo,a,__ATOMIC_SEQ_CST);
    if (b>dirty_hi) __atomic_store_n(&dirty_hi,b,__ATOMIC_SEQ_CST);
}
static void write_lock_at(const char *site) {
    if (!exclusive_depth++) {
        rw_wrlock();
        lock_write_site=site;
        __atomic_store_n(&dirty_lo,UINTPTR_MAX,__ATOMIC_SEQ_CST);
        __atomic_store_n(&dirty_hi,0,__ATOMIC_SEQ_CST);
        __atomic_add_fetch(&table_generation,1,__ATOMIC_SEQ_CST);
    }
}
static void write_unlock(void) {
    if (!--exclusive_depth) {
        const uint64_t ended=table_generation+1;
        DirtyRange *d=&dirty_log[(ended/2)%DIRTY_LOG];
        const int any=dirty_lo<dirty_hi;
        __atomic_store_n(&d->generation,0,__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&d->start,any ? dirty_lo : 0,__ATOMIC_RELAXED);
        __atomic_store_n(&d->end,any ? dirty_hi : 0,__ATOMIC_RELAXED);
        __atomic_store_n(&d->generation,ended,__ATOMIC_RELEASE);
        __atomic_add_fetch(&table_generation,1,__ATOMIC_RELEASE);
        rw_wrunlock();
    }
}
/* Whether a region read at even generation `since` is unchanged at generation now. */
static int region_unchanged(uintptr_t start, uintptr_t end, uint64_t since, uint64_t now) {
    const uint64_t done=now & ~(uint64_t)1;
    if (done-since>2*(DIRTY_LOG-1)) return 0;
    for (uint64_t g=since+2; g<=done; g+=2) {
        DirtyRange *d=&dirty_log[(g/2)%DIRTY_LOG];
        if (__atomic_load_n(&d->generation,__ATOMIC_ACQUIRE)!=g) return 0;
        uintptr_t a=__atomic_load_n(&d->start,__ATOMIC_RELAXED), b=__atomic_load_n(&d->end,__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&d->generation,__ATOMIC_RELAXED)!=g) return 0;
        if (a<end && start<b) return 0;
    }
    if (now & 1) {
        uintptr_t lo=__atomic_load_n(&dirty_lo,__ATOMIC_SEQ_CST), hi=__atomic_load_n(&dirty_hi,__ATOMIC_SEQ_CST);
        if (__atomic_load_n(&table_generation,__ATOMIC_SEQ_CST)!=now) return 0;
        if (lo<end && start<hi) return 0;
    }
    return 1;
}
static void read_lock_at(const char *site) { if (!exclusive_depth) { lock_site_name=site; rw_rdlock(); } }
#define write_lock() write_lock_at(__func__)
#define read_lock() read_lock_at(__func__)
static void read_unlock(void) { if (!exclusive_depth) rw_rdunlock(); }
static Block blocks[LIMIT];
/* sotc: the VMAs, sorted, in blocks of at most VMA_BLOCK entries (indices stay global: V(i)).
 * Riding the world keeps ~11k VMAs, and one sorted array moved ~440 KB at every insert and erase:
 * ~3 s per riding benchmark under the write lock, inside the vma_seq sections the GPU's lock-free
 * lookups wait out. The block index has a fixed capacity and blocks are never freed, so a
 * lock-free read that races a change reads stale but valid memory (vma_seq then rejects it). */
#define VMA_BLOCK 512
#define VMA_BLOCKS_MAX 8192
typedef struct { Vma *e; size_t n, first; } VmaBlock; /* first: global index of e[0] */
static VmaBlock vblocks[VMA_BLOCKS_MAX];
static size_t vblock_count, vma_count;
static Vma *vspare[16]; static size_t vspare_count;
static __thread size_t vblock_hint;
static size_t vblock_of(size_t i) { /* the block holding global index i (i < vma_count) */
    const size_t h=vblock_hint;
    if (h<vblock_count && i-vblocks[h].first<vblocks[h].n) return h;
    size_t lo=0, hi=vblock_count;
    while (hi-lo>1) { size_t mid=(lo+hi)/2; if (vblocks[mid].first<=i) lo=mid; else hi=mid; }
    return vblock_hint=lo;
}
static Vma *vreg(size_t i) {
    const size_t b=vblock_of(i);
    return &vblocks[b].e[i-vblocks[b].first];
}
#define V(i) (*vreg(i))
#ifndef _WIN32
static int pool_fd=-1;
#endif
static void region_direct_init(void); /* sotc: SOTC_REGION_DIRECT, read before any fault */
static unsigned char *backing_base; /* second view of the pool: host writes bypass guest/GPU page protection */
#define FLEX_SPAN (UINT64_C(1024) * 1024 * 1024)
static uint64_t flex_bitmap[FLEX_SPAN/PAGE/64];
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
/* sotc: the host regions, sorted, in blocks of at most REGION_BLOCK entries. The game's mappings
 * cut the address range into ~20k views and placeholders, and one sorted array moved ~600 KB at
 * every insert and erase: ~4.8 s per riding benchmark under the memory write lock, two thirds of
 * all carve time. A block moves at most REGION_BLOCK entries. Indices stay global (R(i)). */
#define REGION_BLOCK 512
typedef struct { HostRegion *e; size_t n, first; } RegionBlock; /* first: global index of e[0] */
static RegionBlock *rblocks; static size_t rblock_count, rblock_capacity;
static HostRegion *rspare[8]; static size_t rspare_count; /* blocks ready for splits */
static size_t region_count, rblock_last;
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
static size_t rblock_of(size_t i) { /* the block holding global index i (i < region_count) */
    if (rblock_last<rblock_count && i-rblocks[rblock_last].first<rblocks[rblock_last].n) return rblock_last;
    size_t lo=0, hi=rblock_count;
    while (hi-lo>1) { size_t mid=(lo+hi)/2; if (rblocks[mid].first<=i) lo=mid; else hi=mid; }
    return rblock_last=lo;
}
static HostRegion *reg(size_t i) {
    const size_t b=rblock_of(i);
    return &rblocks[b].e[i-rblocks[b].first];
}
#define R(i) (*reg(i))
static size_t region_index(uintptr_t a) { /* first region with end > a */
    if (!region_count) return 0;
    size_t lo=0, hi=rblock_count;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (rblocks[mid].e[rblocks[mid].n-1].end<=a) lo=mid+1; else hi=mid; }
    if (lo==rblock_count) return region_count;
    const RegionBlock *b=&rblocks[lo];
    size_t l=0, h=b->n;
    while (l<h) { size_t mid=(l+h)/2; if (b->e[mid].end<=a) l=mid+1; else h=mid; }
    return b->first+l;
}
static HostRegion *rblock_alloc(void) {
    if (rspare_count) return rspare[--rspare_count];
    HostRegion *e=malloc(REGION_BLOCK*sizeof(*e));
    if (!e) { fputs("Out of memory (host regions)\n",stderr); exit(1); }
    return e;
}
/* The next `extra` inserts allocate nothing (carve_host stops the other guest threads meanwhile,
 * and one of them may hold the heap). */
static void region_reserve(size_t extra) {
    if (extra>8) extra=8;
    while (rspare_count<extra) {
        HostRegion *e=malloc(REGION_BLOCK*sizeof(*e));
        if (!e) { fputs("Out of memory (host regions)\n",stderr); exit(1); }
        rspare[rspare_count++]=e;
    }
    while (rblock_count+extra+1>rblock_capacity) {
        size_t capacity=rblock_capacity ? rblock_capacity*2 : 64;
        RegionBlock *next=realloc(rblocks,capacity*sizeof(*rblocks));
        if (!next) { fputs("Out of memory (host regions)\n",stderr); exit(1); }
        rblocks=next; rblock_capacity=capacity;
    }
}
static void rblock_renumber(size_t from) {
    for (size_t b=from; b<rblock_count; ++b) rblocks[b].first=b ? rblocks[b-1].first+rblocks[b-1].n : 0;
}
static void region_insert(size_t at, HostRegion r) {
    if (rblock_count+2>rblock_capacity) region_reserve(1);
    if (!rblock_count) {
        rblocks[0]=(RegionBlock){rblock_alloc(),0,0};
        rblock_count=1;
    }
    size_t b=at==region_count ? rblock_count-1 : rblock_of(at);
    if (rblocks[b].n==REGION_BLOCK) { /* split it in two halves */
        memmove(rblocks+b+2,rblocks+b+1,(rblock_count-b-1)*sizeof(*rblocks));
        HostRegion *e=rblock_alloc();
        const size_t half=REGION_BLOCK/2;
        memcpy(e,rblocks[b].e+half,(REGION_BLOCK-half)*sizeof(*e));
        rblocks[b+1]=(RegionBlock){e,REGION_BLOCK-half,rblocks[b].first+half};
        rblocks[b].n=half;
        ++rblock_count;
        if (at-rblocks[b].first>half) ++b; /* at == first+half stays in block b, at its end */
    }
    RegionBlock *k=&rblocks[b];
    const size_t j=at-k->first;
    memmove(k->e+j+1,k->e+j,(k->n-j)*sizeof(*k->e));
    k->e[j]=r; ++k->n; ++region_count;
    rblock_renumber(b+1);
    rblock_last=b;
}
static void region_erase(size_t at, size_t n) {
    while (n) {
        const size_t b=rblock_of(at);
        RegionBlock *k=&rblocks[b];
        const size_t j=at-k->first;
        const size_t take=k->n-j<n ? k->n-j : n;
        memmove(k->e+j,k->e+j+take,(k->n-j-take)*sizeof(*k->e));
        k->n-=take; region_count-=take; n-=take;
        if (!k->n && rblock_count>1) { /* drop the empty block */
            if (rspare_count<8) rspare[rspare_count++]=k->e; else free(k->e);
            memmove(rblocks+b,rblocks+b+1,(rblock_count-b-1)*sizeof(*rblocks));
            --rblock_count;
        }
        rblock_renumber(b);
        rblock_last=0;
    }
}
/* sotc: something got into a guest range before the reservation (issue #110: error 487, "spamming
 * the play button and eventually it will launch": an overlay or security product placing memory at
 * random). Says what is there and exits with RANGE_BUSY_EXIT: sotc.py starts the game again. */
#define RANGE_BUSY_EXIT 87
static void range_busy(const char *what, uintptr_t lo, uintptr_t hi) {
    const DWORD error=GetLastError();
    fprintf(stderr,"Runtime: cannot reserve %s at %p (+0x%" PRIx64 "), error %lu; already there:\n",what,(void *)lo,(uint64_t)(hi-lo),error);
    int shown=0;
    for (uintptr_t at=lo; at<hi && shown<8;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)at,&m,sizeof(m)) || !m.RegionSize) break;
        if (m.State!=MEM_FREE) {
            char name[260]="";
            if (m.Type==MEM_IMAGE && !GetModuleFileNameA((HMODULE)m.AllocationBase,name,sizeof(name))) name[0]=0;
            fprintf(stderr,"Runtime:   %p +0x%" PRIx64 " %s %s\n",m.BaseAddress,(uint64_t)m.RegionSize,
                    m.Type==MEM_IMAGE ? "module" : m.Type==MEM_MAPPED ? "mapped" : "private",name);
            ++shown;
        }
        at=(uintptr_t)m.BaseAddress+m.RegionSize;
    }
    fflush(NULL);
    exit(RANGE_BUSY_EXIT);
}
/* The guest range is reserved before anything else can be placed there (runtime_start). */
void runtime_memory_reserve(void) {
    if (region_count) return;
    /* SOTC_TEST_RANGE_BUSY=n: the first n starts find the range taken (tests sotc.py's retry). */
    const char *busy_test=getenv("SOTC_TEST_RANGE_BUSY"), *attempt=getenv("SOTC_RANGE_RETRY");
    if (busy_test && (attempt ? atoi(attempt) : 0)<atoi(busy_test))
        VirtualAlloc((void *)(USER_MIN+(UINT64_C(1)<<32)),65536,MEM_RESERVE,PAGE_NOACCESS);
    if (!VirtualAlloc2(NULL,(void *)USER_MIN,USER_MAX-USER_MIN,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0))
        range_busy("the PS4 address range",USER_MIN,USER_MAX);
    region_insert(0,(HostRegion){USER_MIN,USER_MAX,0,0});
    /* sotc: Shadow of the Colossus maps fixed ranges below the user area (0x7f0000000),
     * in the PS4 system-managed window. A second placeholder covers it; mappings never
     * span both reservations because the host's low maps lie between them. */
    if (!VirtualAlloc2(NULL,(void *)SYSTEM_LOW_MIN,SYSTEM_LOW_MAX-SYSTEM_LOW_MIN,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0))
        range_busy("the PS4 system-managed window",SYSTEM_LOW_MIN,SYSTEM_LOW_MAX);
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
/* sotc: the cost of carving live views (under the write lock): splits, MiB of views mapped
 * again, protection runs restored, ms they took and the longest one; ms of all other carves. */
static uint64_t carve_split_bytes, carve_split_runs, carve_split_ticks, carve_split_worst, carve_other_ticks;
void runtime_memory_carve_stats(uint64_t *mib, uint64_t *runs, double *split_ms, double *worst_ms, double *other_ms) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    const double ms=1000.0/(double)f.QuadPart;
    *mib=__atomic_exchange_n(&carve_split_bytes,0,__ATOMIC_RELAXED)>>20;
    *runs=__atomic_exchange_n(&carve_split_runs,0,__ATOMIC_RELAXED);
    *split_ms=(double)__atomic_exchange_n(&carve_split_ticks,0,__ATOMIC_RELAXED)*ms;
    *worst_ms=(double)__atomic_exchange_n(&carve_split_worst,0,__ATOMIC_RELAXED)*ms;
    *other_ms=(double)__atomic_exchange_n(&carve_other_ticks,0,__ATOMIC_RELAXED)*ms;
}
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
    if (first==region_count || R(first).start>a) win_fail("guest range outside the reservation",a,b-a);
    while (last<region_count && R(last).start<b) ++last;
    if (R(last-1).end<b) win_fail("guest range outside the reservation",a,b-a);
    uintptr_t lo=R(first).start, hi=R(last-1).end;
    HostRegion left=R(first), right=R(last-1);
    int keep_left=left.mapped && lo<a, keep_right=right.mapped && b<hi;
    if (first==last-1 && !left.mapped && lo==a && hi==b) return first;
    const uint64_t carve_start=lock_ticks();
    /* before the views change (see runtime_memory_gpu_protect): [a,b) and the views around it
     * that are mapped again; placeholders around it hold no view to protect */
    dirty(keep_left ? lo : a,keep_right ? hi : b);
    ProtRun *runs=NULL; size_t nruns=0, capacity=0, left_runs=0;
    if (keep_left) left_runs=nruns=save_protections(lo,a,&runs,nruns,&capacity);
    if (keep_right) nruns=save_protections(b,hi,&runs,nruns,&capacity);
    /* sotc: a live view is split: everything in it (fiber stacks included) disappears until it is
     * mapped again below. Other guest threads stop meanwhile (see runtime_thread_suspend_others).
     * The region table is grown first: no heap call while they are stopped (one may hold the heap). */
    if ((keep_left || keep_right) && carve_suspend_enabled()) {
        region_reserve(3);
        carve_nstopped=runtime_thread_suspend_others(carve_stopped,256);
        carve_stopping=1;
        __atomic_add_fetch(&carve_splits,1,__ATOMIC_RELAXED);
    }
    for (size_t i=first;i<last;++i) if (R(i).mapped) {
        if (!UnmapViewOfFile2(GetCurrentProcess(),(void *)R(i).start,MEM_PRESERVE_PLACEHOLDER))
            win_fail("UnmapViewOfFile2",R(i).start,R(i).end-R(i).start);
    }
    if (last-first>1 && !VirtualFree((void *)lo,hi-lo,MEM_RELEASE|MEM_COALESCE_PLACEHOLDERS))
        win_fail("coalescing placeholders",lo,hi-lo);
    region_erase(first,last-first);
    region_insert(first,(HostRegion){lo,hi,0,0});
    size_t at=first;
    if (lo<a) {
        if (!VirtualFree((void *)lo,a-lo,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) win_fail("splitting a placeholder",lo,a-lo);
        R(at).end=a;
        region_insert(at+1,(HostRegion){a,hi,0,0});
        if (keep_left) {
            R(at).phys=left.phys; R(at).mapped=1;
            view_map(&R(at),PAGE_READWRITE);
        }
        ++at;
    }
    if (b<hi) {
        if (!VirtualFree((void *)a,b-a,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) win_fail("splitting a placeholder",a,b-a);
        R(at).end=b;
        region_insert(at+1,(HostRegion){b,hi,0,0});
        if (keep_right) {
            R(at+1).phys=right.phys+(b-right.start); R(at+1).mapped=1;
            view_map(&R(at+1),PAGE_READWRITE);
        }
    }
    for (size_t i=0;i<nruns;++i) {
        DWORD old;
        if (runs[i].protect!=PAGE_READWRITE && !VirtualProtect((void *)runs[i].start,runs[i].end-runs[i].start,runs[i].protect,&old))
            win_fail("restoring page protection",runs[i].start,runs[i].end-runs[i].start);
    }
    if (carve_stopping) { carve_stopping=0; runtime_thread_resume(carve_stopped,carve_nstopped); }
    (void)left_runs;
    free(runs);
    const uint64_t carve_took=lock_ticks()-carve_start;
    if (keep_left || keep_right) {
        carve_split_bytes+=(keep_left ? a-lo : 0)+(keep_right ? hi-b : 0); /* under the write lock */
        carve_split_runs+=nruns;
        carve_split_ticks+=carve_took;
        if (carve_took>carve_split_worst) carve_split_worst=carve_took;
    } else {
        carve_other_ticks+=carve_took;
    }
    return at;
}
/* sotc: SOTC_VIEW_CHUNK_MB (default 2, 0 = one view per mapping). A view cannot be unmapped in
 * part, so unmapping a piece of a big mapping unmapped and mapped again everything around it
 * (live view splits): at an area change the game's munmaps remapped 60-346 GB of views per 5 s
 * under the write lock and the frame stopped. Mappings are made of views that end at chunk
 * boundaries, so a split only remaps the chunks at its two ends. */
static uint64_t view_chunk(void) {
    static int64_t v=-1;
    if (v<0) {
        /* SOTC_VIEW_CHUNK_KB overrides the MB value (multiple of the 64 KiB view granularity) */
        const char *e=getenv("SOTC_VIEW_CHUNK_MB"), *k=getenv("SOTC_VIEW_CHUNK_KB");
        v=k ? (atoll(k)<<10)&~(int64_t)0xFFFF : (e ? atoll(e) : 2)<<20;
        if (v<0) v=0;
    }
    return (uint64_t)v;
}
static int host_map(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t phys) {
    size_t i=carve_host(address,address+size);
    if (reserved) return 0;
    const uint64_t chunk=view_chunk();
    const uintptr_t end=address+size;
    for (uintptr_t at=address;; ++i) {
        uintptr_t next=chunk ? (at/chunk+1)*chunk : end;
        if (next>end) next=end;
        if (next<end) { /* R(i) is the placeholder [at,end): split off [at,next) */
            if (!VirtualFree((void *)at,next-at,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER))
                win_fail("splitting a placeholder",at,next-at);
            R(i).end=next;
            region_insert(i+1,(HostRegion){next,end,0,0});
        }
        R(i).phys=phys+(at-address); R(i).mapped=1;
        view_map(&R(i),win_prot(prot));
        if (next==end) break;
        at=next;
    }
    return 0;
}
static int host_unmap(uintptr_t address, uint64_t size) { carve_host(address,address+size); return 0; }
static int host_protect(uintptr_t address, uint64_t size, int prot) {
    DWORD protect=win_prot(prot), old;
    uintptr_t end=address+size;
    for (size_t i=region_index(address); i<region_count && R(i).start<end; ++i) {
        if (!R(i).mapped) continue;
        uintptr_t a=R(i).start>address ? R(i).start : address, b=R(i).end<end ? R(i).end : end;
        if (!VirtualProtect((void *)a,b-a,protect,&old)) return -1;
    }
    return 0;
}
/* Released pages read as zero when reused, like the memfd's punched holes. */
static uint64_t discarded_bytes;
uint64_t runtime_memory_discarded(void) { return __atomic_exchange_n(&discarded_bytes,0,__ATOMIC_RELAXED); }
static void host_discard(uint64_t phys, uint64_t size) { __atomic_add_fetch(&discarded_bytes,size,__ATOMIC_RELAXED); memset(backing_base+phys,0,size); }
/* Direct memory occupies [0,POOL_SIZE) of the section, flexible memory [POOL_SIZE,+FLEX_SPAN). */
static int pool(void) {
    if (backing_base) return 0;
    region_direct_init();
    runtime_memory_reserve();
    uint64_t bytes=POOL_SIZE+FLEX_SPAN;
    section=CreateFileMapping2(INVALID_HANDLE_VALUE,NULL,FILE_MAP_ALL_ACCESS,PAGE_EXECUTE_READWRITE,SEC_COMMIT,bytes,NULL,NULL,0);
    if (!section && pool_size_mb>pool_base_mb()) {
        /* sotc: the larger pool (BB_DMEM_MB, 9152 MiB by default since 0.59) is committed at once;
         * with little RAM and a small page file Windows refuses it (error 1450) and the game
         * stopped at boot. Fall back to the console's size. */
        fprintf(stderr,"Runtime: %" PRIu64 " MiB of guest memory refused (error %lu): using %" PRIu64 " MiB; a larger page file allows more\n",
                bytes>>20,GetLastError(),pool_base_mb());
        pool_size_mb=pool_base_mb();
        bytes=POOL_SIZE+FLEX_SPAN;
        section=CreateFileMapping2(INVALID_HANDLE_VALUE,NULL,FILE_MAP_ALL_ACCESS,PAGE_EXECUTE_READWRITE,SEC_COMMIT,bytes,NULL,NULL,0);
    }
    if (!section) { fprintf(stderr,"Runtime: cannot create %" PRIu64 " MiB of guest memory (error %lu); is the page file large enough?\n",bytes>>20,GetLastError()); return -1; }
    backing_base=MapViewOfFile3(section,GetCurrentProcess(),NULL,0,bytes,0,PAGE_READWRITE,NULL,0);
    return backing_base ? 0 : -1;
}
#else
/* Direct memory occupies [0,POOL_SIZE) of the memfd, flexible memory [POOL_SIZE,+FLEX_SPAN). */
static int pool(void) {
    if (pool_fd>=0) return 0;
    region_direct_init();
    pool_fd=memfd_create("bb-guest-memory", MFD_CLOEXEC);
    if (pool_fd<0 || ftruncate(pool_fd,(off_t)(POOL_SIZE+FLEX_SPAN))) return -1;
    void *view=mmap(NULL,POOL_SIZE+FLEX_SPAN,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_NORESERVE,pool_fd,0);
    if (view==MAP_FAILED) return -1;
    backing_base=view;
    return 0;
}
void runtime_memory_reserve(void) {}
/* mmap(MAP_FIXED) replaces mappings in place: a live view is never split here (SOTC_FPS report). */
uint64_t runtime_memory_carve_splits(void) { return 0; }
static int host_map(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t phys) {
    void *mapped = !reserved
        ? mmap((void *)address,size,prot,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)phys)
        : mmap((void *)address,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED,-1,0);
    return mapped==MAP_FAILED ? -1 : 0;
}
static int host_unmap(uintptr_t address, uint64_t size) { return munmap((void *)address,size); }
static int host_protect(uintptr_t address, uint64_t size, int prot) { return mprotect((void *)address,size,prot); }
static void host_discard(uint64_t phys, uint64_t size) {
    fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)phys,(off_t)size);
}
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
static uint64_t flex_alloc(uint64_t size) {
    uint64_t pages=size/PAGE, total=FLEX_SPAN/PAGE, run=0;
    for (uint64_t p=0;p<total;++p) {
        run = flex_test(p) ? 0 : run+1;
        if (run==pages) { flex_set(p+1-pages,pages,1); return POOL_SIZE+(p+1-pages)*PAGE; }
    }
    return UINT64_MAX;
}
static void flex_free(uint64_t phys, uint64_t size) {
    flex_set((phys-POOL_SIZE)/PAGE,size/PAGE,0);
    host_discard(phys,size);
}
static size_t vma_index(uintptr_t a) { /* first VMA with end > a */
    if (!vma_count) return 0;
    size_t lo=0, hi=vblock_count;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (vblocks[mid].e[vblocks[mid].n-1].end<=a) lo=mid+1; else hi=mid; }
    if (lo==vblock_count) return vma_count;
    const VmaBlock *b=&vblocks[lo];
    size_t l=0, h=b->n;
    while (l<h) { size_t mid=(l+h)/2; if (b->e[mid].end<=a) l=mid+1; else h=mid; }
    vblock_hint=lo;
    return b->first+l;
}
/* sotc: odd while the VMA array is being changed in place (insert, erase, field writes). The
 * GPU's lookups read the array without the lock and retry when it changed meanwhile
 * (vma_lookup_fast); a grown array is never freed, so such a read never touches freed memory. */
static uint64_t vma_seq;
static void vma_seq_begin(void) { __atomic_add_fetch(&vma_seq,1,__ATOMIC_SEQ_CST); }
static void vma_seq_end(void) { __atomic_add_fetch(&vma_seq,1,__ATOMIC_RELEASE); }
/* A lock-free reader follows .e before vma_seq can reject what it read, so .e is always stored
 * whole: memmove's copy width belongs to the C library, and a torn pointer would be a wild read. */
static void vblock_set(size_t at, Vma *e, size_t n, size_t first) {
    __atomic_store_n(&vblocks[at].e,e,__ATOMIC_RELAXED);
    __atomic_store_n(&vblocks[at].n,n,__ATOMIC_RELAXED);
    __atomic_store_n(&vblocks[at].first,first,__ATOMIC_RELAXED);
}
static Vma *vblock_alloc(void) {
    if (vspare_count) return vspare[--vspare_count];
    return malloc(VMA_BLOCK*sizeof(Vma));
}
static void vblock_renumber(size_t from) {
    for (size_t b=from; b<vblock_count; ++b)
        vblocks[b].first=b ? vblocks[b-1].first+vblocks[b-1].n : 0;
}
static int vma_insert(size_t at, Vma v) {
    if (!vblock_count) {
        Vma *e=vblock_alloc();
        if (!e) return -1;
        vma_seq_begin();
        vblock_set(0,e,0,0);
        vblock_count=1;
        vma_seq_end();
    }
    size_t b=at==vma_count ? vblock_count-1 : vblock_of(at);
    Vma *spare=NULL;
    if (vblocks[b].n==VMA_BLOCK) { /* split it: the new block before any change */
        if (vblock_count==VMA_BLOCKS_MAX || !(spare=vblock_alloc())) return -1;
    }
    dirty(v.start,v.end);
    vma_seq_begin();
    if (spare) {
        const size_t half=VMA_BLOCK/2;
        memcpy(spare,vblocks[b].e+half,(VMA_BLOCK-half)*sizeof(*spare));
        for (size_t x=vblock_count; x>b+1; --x) vblock_set(x,vblocks[x-1].e,vblocks[x-1].n,vblocks[x-1].first);
        vblock_set(b+1,spare,VMA_BLOCK-half,vblocks[b].first+half);
        vblocks[b].n=half;
        ++vblock_count;
        if (at-vblocks[b].first>half) ++b; /* at == first+half: the end of block b */
    }
    VmaBlock *k=&vblocks[b];
    const size_t j=at-k->first;
    memmove(k->e+j+1,k->e+j,(k->n-j)*sizeof(*k->e));
    k->e[j]=v; ++k->n; ++vma_count;
    vblock_renumber(b+1);
    vma_seq_end();
    vblock_hint=b;
    return 0;
}
static void vma_erase(size_t at, size_t n) {
    if (!n) return;
    dirty(V(at).start,V(at+n-1).end);
    vma_seq_begin();
    while (n) {
        const size_t b=vblock_of(at);
        VmaBlock *k=&vblocks[b];
        const size_t j=at-k->first;
        const size_t take=k->n-j<n ? k->n-j : n;
        memmove(k->e+j,k->e+j+take,(k->n-j-take)*sizeof(*k->e));
        k->n-=take; vma_count-=take; n-=take;
        if (!k->n && vblock_count>1) { /* drop the empty block; never freed (lock-free reads) */
            Vma *e=k->e;
            for (size_t x=b; x+1<vblock_count; ++x) vblock_set(x,vblocks[x+1].e,vblocks[x+1].n,vblocks[x+1].first);
            --vblock_count;
            vblock_set(vblock_count,e,0,0); /* a stale slot keeps a valid block */
            if (vspare_count<16) vspare[vspare_count++]=e; /* else left to the stale slot */
        }
        vblock_renumber(b);
        vblock_hint=0;
    }
    vma_seq_end();
}
static int split_at(uintptr_t a) {
    size_t i=vma_index(a);
    if (i==vma_count || V(i).start>=a) return 0;
    Vma right=V(i);
    right.start=a;
    /* sotc: flexible pages have a backing offset too. Kept unchanged, the right half of a
     * split flexible mapping pointed at the left half's storage: the runtime's copies went to
     * the wrong pages and its release freed the left half's pages, still in use, which the
     * next flexible map handed out again (T38: mosaic textures, heap crashes). */
    if (right.kind!=KIND_RESERVED) {
        right.phys+=a-V(i).start;
        static unsigned long flex_splits;
        if (right.kind==KIND_FLEXIBLE && flex_splits++ < 8)
            fprintf(stderr,"Runtime: flexible mapping %p-%p split at %p\n",
                    (void *)V(i).start,(void *)V(i).end,(void *)a);
    }
    dirty(V(i).start,V(i).end);
    vma_seq_begin();
    V(i).end=a;
    vma_seq_end();
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
        if (i==vma_count || V(i).start>at || (!allow_reserved && V(i).kind==KIND_RESERVED)) return 0;
        at=V(i).end;
    }
    return 1;
}
static int overlaps(uintptr_t start, uintptr_t end, int ignore_reserved) {
    for (size_t i=vma_index(start); i<vma_count && V(i).start<end; ++i)
        if (!ignore_reserved || V(i).kind!=KIND_RESERVED) return 1;
    return 0;
}
/* First-fit search in the PS4 user range, starting from the hint. */
static uintptr_t find_free(uintptr_t hint, uint64_t size, uint64_t alignment) {
    uintptr_t at=align_up(hint<USER_MIN ? USER_MIN : hint, alignment);
    for (size_t i=vma_index(at); at+size<=USER_MAX; ++i) {
        if (i==vma_count || V(i).start>=at+size) return at;
        at=align_up(V(i).end,alignment);
    }
    return 0;
}
/* sotc: T38. The game keeps the same physical pages mapped at two guest addresses for minutes
 * (Time Attack: up to ~1600 pages, 350 MB; it writes them through one and draws them through the
 * other). The GPU caches go by address and watch writes by protecting pages, so a write through
 * the other address went unseen: textures kept the old contents (mosaic) and readback-protected
 * pages were read stale. Each direct physical page keeps the addresses it is mapped at (two);
 * the GPU's protections are copied to the other address and its faults handed to the GPU at the
 * watched one. SOTC_ALIAS_SYNC=1 turns it on (02/11: mosaic and crashes unchanged with it). */
static uintptr_t (*phys_views)[2];
static uint64_t phys_view_pages, alias_pages, alias_overflow, alias_faults;
static int alias_sync_on(void) {
    static int on=-1;
    if (on<0) { const char *v=getenv("SOTC_ALIAS_SYNC"); on=v && v[0]=='1'; }
    return on;
}
static void views_add(uint64_t phys, uintptr_t address, uint64_t size) {
    if (!alias_sync_on()) return;
    if (!phys_views) {
        phys_view_pages=POOL_SIZE/PAGE;
        phys_views=calloc(phys_view_pages,sizeof(*phys_views));
        if (!phys_views) { phys_view_pages=0; return; }
    }
    for (uint64_t k=0; k<size/PAGE; ++k) {
        const uint64_t page=phys/PAGE+k;
        if (page>=phys_view_pages) break;
        uintptr_t *v=phys_views[page]; const uintptr_t at=address+k*PAGE;
        if (v[0]==at || v[1]==at) continue;
        if (!v[0]) v[0]=at;
        else if (!v[1]) { v[1]=at; __atomic_add_fetch(&alias_pages,1,__ATOMIC_RELAXED); }
        else if (alias_overflow++<4) fprintf(stderr,"Runtime: physical %#" PRIx64 " mapped at a third address %p (not kept)\n",page*PAGE,(void *)at);
    }
}
static void views_remove(uint64_t phys, uintptr_t address, uint64_t size) {
    if (!phys_views) return;
    for (uint64_t k=0; k<size/PAGE; ++k) {
        const uint64_t page=phys/PAGE+k;
        if (page>=phys_view_pages) break;
        uintptr_t *v=phys_views[page]; const uintptr_t at=address+k*PAGE;
        uintptr_t left=0;
        if (v[1]==at) { left=v[0]; v[1]=0; }
        else if (v[0]==at) { left=v[1]; v[0]=v[1]; v[1]=0; }
        if (!left) continue;
        __atomic_sub_fetch(&alias_pages,1,__ATOMIC_RELAXED);
        /* The address left may carry a protection copied from the one going away, which no
         * tracker there would lift (writes then faulted forever): the guest's protection again,
         * and the GPU rereads the page. */
        const size_t j=vma_index(left);
        if (j<vma_count && V(j).start<=left && V(j).kind==KIND_DIRECT) {
            host_protect(left,PAGE,host_prot(V(j).prot));
            queue_hook(HOOK_INVALIDATE,left,PAGE);
        }
    }
}
/* The other guest address of the direct page at [address], or 0. Under the lock. */
static uintptr_t view_other(uintptr_t address, size_t *vma) {
    size_t i=vma_index(address);
    if (i==vma_count || V(i).start>address || V(i).kind!=KIND_DIRECT) return 0;
    if (vma) *vma=i;
    const uint64_t off=(address-V(i).start) & ~(PAGE-1), phys=V(i).phys+off, page=phys/PAGE;
    if (page>=phys_view_pages) return 0;
    const uintptr_t at=V(i).start+off, *v=phys_views[page];
    const uintptr_t other=v[0]==at ? v[1] : v[1]==at ? v[0] : 0;
    return other ? other+((address-V(i).start)-off) : 0;
}
/* Gives the other addresses of [address,address+size) the GPU's protection (read/write wanted). */
static void views_protect(uintptr_t address, uint64_t size, int read, int write) {
    uintptr_t run=0, run_end=0; int run_prot=0;
    for (uintptr_t at=address & ~(uintptr_t)(PAGE-1); at<address+size; at+=PAGE) {
        const uintptr_t other=view_other(at,NULL);
        size_t j=other ? vma_index(other) : vma_count;
        int prot=0;
        if (j<vma_count && V(j).start<=other) {
            const int guest=host_prot(V(j).prot);
            /* writes only: a read of the other address is never stopped (GPU threads read guest
             * memory by address, and a read fault there waited for the GPU itself) */
            const int want=PROT_READ|(write ? PROT_WRITE : 0)|(guest & PROT_EXEC);
            (void)read;
            prot=guest & want;
        } else j=vma_count;
        if (run && (j==vma_count || other!=run_end || prot!=run_prot)) { host_protect(run,run_end-run,run_prot); run=0; }
        if (j==vma_count) continue;
        if (!run) { run=other; run_prot=prot; }
        run_end=other+PAGE;
    }
    if (run) host_protect(run,run_end-run,run_prot);
}
/* sotc: T38, SOTC_POOL_OWNER (default on; 0 off). The game's 64 KiB page pool (eboot 0x19e00)
 * shrinks a texture's block by freeing pages without unmapping them (release eboot 0x1abf0 with
 * no unmap); those pages go to another block, mapped at its address. When the first block is
 * freed whole, the release asks sceKernelVirtualQuery for every page of the block's range and
 * frees the physical page found there again: the page now has two owners (mosaic textures,
 * heap blocks overwritten, the free tally drifting up until the 5th Time Attack). Each physical
 * 64 KiB page keeps the address it was mapped at last; asked from the release, a page mapped
 * elsewhere since reads as not committed, and the release skips it. */
static uintptr_t *page_owner; static uint64_t page_owner_count, owner_skipped_pages, owner_skips;
static uintptr_t release_site; /* return address of the release's VirtualQuery (probe.c) */
static int pool_owner_on(void) {
    static int on=-1;
    if (on<0) { const char *v=getenv("SOTC_POOL_OWNER"); on=!(v && v[0]=='0'); }
    return on;
}
void runtime_memory_set_release_site(uintptr_t site) { release_site=site; }
static void owner_note(uint64_t phys, uintptr_t address, uint64_t size) {
    if (!pool_owner_on()) return;
    if (!page_owner) {
        page_owner_count=POOL_SIZE>>16;
        page_owner=calloc(page_owner_count,sizeof(*page_owner));
        if (!page_owner) { page_owner_count=0; return; }
    }
    for (uint64_t p=(phys+0xffff)&~(uint64_t)0xffff; p+0x10000<=phys+size; p+=0x10000)
        if ((p>>16)<page_owner_count) page_owner[p>>16]=address+(p-phys);
}
static void drop_range(uintptr_t start, uintptr_t end) {
    int error; size_t i=carve(start,end,&error), n=0;
    while (i+n<vma_count && V(i+n).end<=end && V(i+n).start>=start) {
        if (V(i+n).kind==KIND_FLEXIBLE) {
            flexible_bytes-=V(i+n).end-V(i+n).start;
            flex_free(V(i+n).phys,V(i+n).end-V(i+n).start);
        }
        if (V(i+n).kind==KIND_DIRECT) views_remove(V(i+n).phys,V(i+n).start,V(i+n).end-V(i+n).start);
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
    drop_range(address,address+size);
    if (vma_insert(vma_index(address),(Vma){address,address+size,kind,prot,type,phys})) return NO_MEMORY;
    if (kind==KIND_FLEXIBLE) flexible_bytes+=size;
    if (kind==KIND_DIRECT) {
        owner_note(phys,address,size);
        views_add(phys,address,size);
#ifdef _WIN32
        /* a new second address takes the GPU's protection of the first */
        if (__atomic_load_n(&alias_pages,__ATOMIC_RELAXED))
            for (uintptr_t at=address; at<address+size; at+=PAGE) {
                const uintptr_t other=view_other(at,NULL);
                MEMORY_BASIC_INFORMATION m;
                if (!other || !VirtualQuery((void *)other,&m,sizeof(m))) continue;
                const int guest=host_prot(prot);
                if (m.Protect==PAGE_NOACCESS || m.Protect==PAGE_READONLY || m.Protect==PAGE_EXECUTE_READ)
                    host_protect(at,PAGE,guest & (PROT_READ|PROT_EXEC));
            }
#endif
    }
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

/* sotc: the pool is created before its size is reported: when Windows refuses the larger pool
 * the fallback shrinks it, and the game must size its heaps for the memory it really gets. */
static ABI uint64_t direct_size(void) {
    write_lock();
    pool();
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
static uint64_t alias_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
#endif
}
/* sotc: SOTC_VIEW_CHECK=1 (diagnostic, T38): the host views against the region table, once a
 * second during maps. A view whose section offset is not the one the table gives its guest
 * address means the CPU writes one physical page and the GPU (which reads the backing by the
 * table) another. */
static void views_check(void) {
#ifdef _WIN32
    static int on=-1;
    if (on<0) { const char *v=getenv("SOTC_VIEW_CHECK"); on=v && v[0]=='1'; }
    if (!on) return;
    static uint64_t last; static unsigned long bad_total, checks;
    const uint64_t now=alias_ms();
    if (now-last<1000) return;
    last=now; ++checks;
    unsigned long bad=0;
    for (size_t r=0; r<region_count; ++r) {
        if (!R(r).mapped) continue;
        for (uintptr_t at=R(r).start; at<R(r).end;) {
            const size_t i=vma_index(at);
            if (i==vma_count || V(i).start>at) { /* a view where the table has nothing */
                if (bad++<8 && bad_total<200) fprintf(stderr,"Runtime: VIEW CHECK view %p-%p (offset %#" PRIx64 ") over no mapping at %p\n",
                    (void *)R(r).start,(void *)R(r).end,R(r).phys,(void *)at);
                break;
            }
            const uintptr_t stop=V(i).end<R(r).end ? V(i).end : R(r).end;
            const uint64_t view_phys=R(r).phys+(at-R(r).start);
            const uint64_t table_phys=V(i).phys+(at-V(i).start);
            if (V(i).kind!=KIND_RESERVED && view_phys!=table_phys && bad++<8 && bad_total<200)
                fprintf(stderr,"Runtime: VIEW CHECK %p-%p: view offset %#" PRIx64 ", table %#" PRIx64 " (kind %d, mapping %p-%p)\n",
                        (void *)at,(void *)stop,view_phys,table_phys,V(i).kind,(void *)V(i).start,(void *)V(i).end);
            at=stop;
        }
    }
    bad_total+=bad;
    if (bad || checks%30==1) fprintf(stderr,"Runtime: VIEW CHECK %lu wrong now, %lu in all (%zu views, %zu mappings)\n",bad,bad_total,region_count,vma_count);
#endif
}
static int32_t map_direct_locked(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!out || physical < 0 || (uint64_t)physical % PAGE || !size || size % PAGE || !valid_alignment(alignment) ||
        (prot & ~0x37)) return INVALID;
    if (!direct_range_allocated((uint64_t)physical,size)) return INVALID;
    int32_t result=place(out,size,prot,flags,alignment,KIND_DIRECT,block_type((uint64_t)physical),(uint64_t)physical);
    if (!result) ++maps;
    views_check();
    /* sotc: SOTC_ALIAS_LOG=1 (diagnostic, T38): physical pages mapped at a second address while
     * still mapped at the first. The GPU caches go by address and do not see one through the other. */
    static int alias_log=-1;
    if (alias_log<0) { const char *v=getenv("SOTC_ALIAS_LOG"); alias_log=v && v[0]=='1'; }
    if (!result && alias_log) {
        static unsigned long aliases, logged;
        const uintptr_t at=(uintptr_t)*out; const uint64_t p=(uint64_t)physical;
        /* Pairs seen, checked once a second: how long both addresses stay on the same pages. */
        enum { PAIRS=4096 };
        static struct { uintptr_t a, b; uint64_t phys, size, since; } pairs[PAIRS];
        static size_t npairs; static uint64_t last_check, worst_ms;
        const uint64_t now=alias_ms();
        if (now-last_check>=1000) {
            last_check=now;
            size_t live=0; uint64_t live_bytes=0, oldest=0;
            for (size_t k=0;k<npairs;) {
                size_t ia=vma_index(pairs[k].a), ib=vma_index(pairs[k].b);
                const int both=ia<vma_count && ib<vma_count && V(ia).kind==KIND_DIRECT && V(ib).kind==KIND_DIRECT &&
                    V(ia).start<=pairs[k].a && V(ib).start<=pairs[k].b &&
                    V(ia).phys+(pairs[k].a-V(ia).start)==pairs[k].phys &&
                    V(ib).phys+(pairs[k].b-V(ib).start)==pairs[k].phys;
                const uint64_t age=now-pairs[k].since;
                if (!both) {
                    if (age>worst_ms) worst_ms=age;
                    pairs[k]=pairs[--npairs]; continue;
                }
                ++live; live_bytes+=pairs[k].size; if (age>oldest) oldest=age; ++k;
            }
            if (live || worst_ms)
                fprintf(stderr,"Runtime: ALIAS live %zu pairs (%" PRIu64 " KiB), oldest %" PRIu64 " ms; longest ended %" PRIu64 " ms; %lu in all; %" PRIu64 " pages in two views, %" PRIu64 " faults handed over\n",
                        live,live_bytes>>10,oldest,worst_ms,aliases,alias_pages,alias_faults);
        }
        for (size_t i=0;i<vma_count;++i) {
            const Vma *v=&V(i);
            if (v->kind!=KIND_DIRECT || (v->start<at+size && at<v->end)) continue;
            if (v->phys<p+size && p<v->phys+(v->end-v->start)) {
                ++aliases;
                if (npairs<PAIRS) {
                    const uint64_t lo=v->phys>p ? v->phys : p;
                    const uint64_t hi=v->phys+(v->end-v->start)<p+size ? v->phys+(v->end-v->start) : p+size;
                    pairs[npairs++]=(__typeof__(pairs[0])){at+(lo-p),v->start+(lo-v->phys),lo,hi-lo,now};
                }
                if (logged<60) {
                    ++logged;
                    fprintf(stderr,"Runtime: ALIAS physical %#" PRIx64 "+%#" PRIx64 " mapped at %p, already at %p-%p (physical %#" PRIx64 "), %lu so far\n",
                            p,size,(void *)at,(void *)v->start,(void *)v->end,v->phys,aliases);
                }
                break;
            }
        }
    }
    return result;
}
static ABI int32_t direct_map(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    write_lock();
    int32_t result=map_direct_locked(out,size,prot,flags,physical,alignment);
    write_unlock();
    flush_hooks();
    if (!result) printf("Runtime: mapped direct memory %p, size=%" PRIu64 ", prot=0x%x\n", *out, size, prot);
    else {
        static unsigned long failed;
        if (failed++<40) fprintf(stderr,"Runtime: MAP DIRECT failed (%#x): at %p +%#" PRIx64 " offset %#" PRIx64 " prot %#x flags %#x align %#" PRIx64 ", from %p\n",
                                 (unsigned)result,out ? *out : NULL,size,(uint64_t)physical,prot,flags,alignment,__builtin_return_address(0));
    }
    return result;
}
static ABI int32_t direct_map_named(void **out, uint64_t size, int prot, int flags, int64_t physical,
                                    uint64_t alignment, const char *name) {
    (void)name; return direct_map(out,size,prot,flags,physical,alignment);
}
static int32_t unmap_locked(uintptr_t start, uint64_t size) {
    if (start%PAGE || !size) return INVALID;
    uint64_t end=start+align_up(size,PAGE);
    for (size_t i=vma_index(start); i<vma_count && V(i).start<end; ++i)
        if (V(i).kind!=KIND_RESERVED) {
            uintptr_t a=V(i).start>start ? V(i).start : start, b=V(i).end<end ? V(i).end : end;
            queue_hook(HOOK_UNMAP,a,b-a);
        }
    for (size_t i=vma_index(start); i<vma_count && V(i).start<end; ++i) {
        uintptr_t a=V(i).start>start ? V(i).start : start, b=V(i).end<end ? V(i).end : end;
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
    if (result) {
        static unsigned long failed;
        if (failed++<40) fprintf(stderr,"Runtime: MUNMAP failed (%#x): %p +%#" PRIx64 ", from %p\n",(unsigned)result,address,size,__builtin_return_address(0));
    }
    return result;
}
static ABI int32_t direct_release(uint64_t start, uint64_t size) {
    if (start % PAGE || size % PAGE) return INVALID;
    if (!size) return 0;
    write_lock();
    if (!direct_range_allocated(start,size)) { write_unlock(); return INVALID; }
    /* Mappings of released physical pages disappear with them. */
    for (size_t i=0; i<vma_count;) {
        Vma v=V(i);
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
    dirty(start,end); /* before the host protection changes: see runtime_memory_gpu_protect */
    if (host_protect(start,end-start,host_prot(prot))) return INVALID;
    int error; size_t i=carve(start,end,&error);
    if (error) return NO_MEMORY;
    vma_seq_begin();
    for (; i<vma_count && V(i).start<end; ++i) { V(i).prot=prot; if (type>=0) V(i).type=type; }
    vma_seq_end();
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
    if (i==vma_count || V(i).start>(uintptr_t)address || V(i).kind==KIND_RESERVED) {
        read_unlock(); return ACCESS;
    }
    /* Report the whole contiguous run with identical protection. */
    size_t first=i, last=i;
    while (first && V(first-1).end==V(first).start && V(first-1).kind!=KIND_RESERVED && V(first-1).prot==V(i).prot) --first;
    while (last+1<vma_count && V(last+1).start==V(last).end && V(last+1).kind!=KIND_RESERVED && V(last+1).prot==V(i).prot) ++last;
    if (start) *start=(void *)V(first).start;
    if (end) *end=(void *)V(last).end;
    if (prot) *prot=(uint32_t)V(i).prot;
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t virtual_query(const void *address, int flags, VirtualQueryInfo *info, uint64_t info_size) {
    if (!info || info_size!=sizeof(*info)) return INVALID;
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || (V(i).start>(uintptr_t)address && !(flags & 1))) { read_unlock(); return ACCESS; }
    Vma v=V(i);
    memset(info,0,sizeof(*info));
    info->start=v.start; info->end=v.end;
    info->offset=v.kind==KIND_DIRECT ? v.phys : 0;
    info->protection=v.prot; info->memory_type=v.type;
    info->flags=(v.kind==KIND_FLEXIBLE ? 1u : 0) | (v.kind==KIND_DIRECT ? 2u : 0) | (v.kind!=KIND_RESERVED ? 16u : 0);
    /* sotc: T38, the pool release asking about pages another block owns now (owner_note) */
    if (release_site && page_owner && v.kind==KIND_DIRECT && (uintptr_t)__builtin_return_address(0)==release_site &&
        (uintptr_t)address>=v.start) {
        const uintptr_t page=(uintptr_t)address & ~(uintptr_t)0xffff;
        uintptr_t at=page<v.start ? v.start : page;
        uint64_t stale=0;
        for (; at<v.end; at=(at & ~(uintptr_t)0xffff)+0x10000, ++stale) {
            const uint64_t phys=v.phys+(at-v.start);
            if ((phys>>16)>=page_owner_count || !page_owner[phys>>16] || page_owner[phys>>16]==(at & ~(uintptr_t)0xffff)) break;
        }
        if (stale) {
            info->start=(uintptr_t)address; info->end=at;
            info->offset=v.phys+((uintptr_t)address-v.start);
            info->flags&=~16u; /* not committed: the release skips [address, at) */
            __atomic_add_fetch(&owner_skipped_pages,stale,__ATOMIC_RELAXED);
            const uint64_t n=__atomic_add_fetch(&owner_skips,1,__ATOMIC_RELAXED);
            if (n<=20 || !(n&(n-1)))
                fprintf(stderr,"Runtime: POOL OWNER release of %p skips %" PRIu64 " pages mapped at another block since (%" PRIu64 " skips, %" PRIu64 " pages so far)\n",
                        address,stale,n,owner_skipped_pages);
        }
    }
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
    /* sotc: SOTC_BATCH_SPLIT=0 holds the lock for the whole batch. At an area change the game's
     * batches held it ~0.5 s per 5 s and every GPU lookup in that time waited for the batch to
     * end; one entry at a time lets them in between (each entry is whole for them). */
    static int split=-1;
    if (split<0) { const char *v=getenv("SOTC_BATCH_SPLIT"); split=!(v && v[0]=='0'); }
    write_lock();
    for (; done<count; ++done) {
        if (split && done) { write_unlock(); write_lock(); }
        BatchEntry *e=&entries[done];
        switch (e->operation) {
        case 0: result=map_direct_locked(&e->start,e->length,(uint8_t)e->prot,flags,(int64_t)e->offset,0); break;
        case 1: result=unmap_locked((uintptr_t)e->start,e->length); break;
        case 2: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,-1); break;
        case 3: result=map_flexible_locked(&e->start,e->length,(uint8_t)e->prot,flags); break;
        case 4: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,e->type); break;
        default: result=INVALID;
        }
        if (result) {
            /* sotc: a failed entry leaves what was mapped there before: the game, which may not
             * check, then writes and frees through the old pages (T38) */
            static unsigned long failed;
            if (failed++<40)
                fprintf(stderr,"Runtime: BATCH MAP entry %d of %d failed (%#x): op %d at %p +%#" PRIx64 " offset %#" PRIx64 " prot %#x type %d flags %#x, from %p\n",
                        done,count,(unsigned)result,e->operation,e->start,e->length,e->offset,(unsigned)(uint8_t)e->prot,e->type,flags,
                        __builtin_return_address(0));
            break;
        }
    }
    write_unlock();
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
    pool();
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
/* sotc: bit 39 (PendingFenceWaits) is off by default. It lets a graphics WAIT_REG_MEM pass on a
 * value a fence still queued for the draw recording thread will write, but Shadow of the
 * Colossus also waits there for labels its compute queues write: the frame then went ahead
 * without the compute results and the scene was never drawn (black screen in about half of
 * the boots). A BB_TOGGLE_FILE value replaces this default. */
uint64_t runtime_disabled_optimizations=UINT64_C(1)<<39 | UINT64_C(1)<<59 | UINT64_C(1)<<60; /* sotc: 59, 60 off as in Lukto02 #308 */
/* Speculative readers of guest memory (GPU draw-preparation workers) register a recovery
 * point: a fault on that thread jumps back to it instead of terminating (probe.c). */
__thread sigjmp_buf *runtime_fault_recover;
static void *toggle_watcher(void *path) {
    for (unsigned long long last=ULLONG_MAX;;) {
        FILE *f=fopen(path,"r");
        unsigned long long value=0;
        if (f) { if (fscanf(f,"%llu",&value)!=1) value=0; fclose(f); }
        if (value!=last) {
            __atomic_store_n(&runtime_disabled_optimizations,(uint64_t)value,__ATOMIC_RELEASE);
            printf("Runtime: disabled optimizations mask=%llu\n",value);
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
    for (size_t i=0;i<vma_count;++i) if (V(i).kind!=KIND_RESERVED) queue_hook(HOOK_MAP,V(i).start,V(i).end-V(i).start);
    write_unlock();
    flush_hooks();
}
/* Writes through the backing view; 0 when part of the range has no backing. */
/* sotc: host pointer to the backing of a mapped guest address (bypasses GPU page protection),
 * valid to the end of its VMA; NULL for reserved or unmapped addresses. */

/* Per-thread cache of recently found regions for the GPU's per-draw queries:
 * entries hold for as long as the table generation they were read at. */
typedef struct { uint64_t generation; uintptr_t start, end; int kind, prot; const unsigned char *backing; } CachedRegion; /* backing: view of start */
static __thread CachedRegion region_cache[8];
static __thread unsigned region_cache_next;
/* sotc: plus 512 direct-mapped entries by 16 KiB of address. The command processor's buffer binds
 * touch more regions than the 8 recent ones, so most lookups took the read lock and a binary
 * search (~4.5% of that thread: libwinpthread, vma_index). SOTC_REGION_DIRECT=0: ring only. */
#define REGION_DIRECT_SLOTS 512
static __thread CachedRegion region_direct[REGION_DIRECT_SLOTS];
static int region_direct_on=-1; /* -1 until pool(): lookups only, no fills */
static void region_direct_init(void) {
    const char *v=getenv("SOTC_REGION_DIRECT");
    region_direct_on=!(v && v[0]=='0');
}
static size_t region_direct_slot(uintptr_t address) { return (address>>14)&(REGION_DIRECT_SLOTS-1); }
/* An entry read at an older generation is checked against the writes since (region_unchanged)
 * and, while no write is under way, stamped with the current one. */
/* sotc: [qa,qb) is the part of the region the caller uses (qb 0: the whole region). A write that
 * changed another part of a direct region (the game maps 64 KiB pages inside big mappings) leaves
 * the used part as it was: direct memory stays contiguous across splits (split_at). */
static int region_current(CachedRegion *c, uint64_t generation, uintptr_t qa, uintptr_t qb) {
    if (c->generation==generation) return 1;
    if (!region_log_on()) return 0;
    if (region_unchanged(c->start,c->end,c->generation,generation)) {
        if (!(generation & 1)) c->generation=generation;
        return 1;
    }
    if (!qb || c->kind!=KIND_DIRECT) return 0;
    return region_unchanged(qa,qb<c->end ? qb : c->end,c->generation,generation);
}
/* sotc: lookups that found no entry, and that found one a write had made stale (lock report) */
static uint64_t region_misses, region_stale;
static uint64_t region_fast_hits;
void runtime_memory_cache_stats(uint64_t *misses, uint64_t *stale, uint64_t *fast) {
    *misses=__atomic_exchange_n(&region_misses,0,__ATOMIC_RELAXED);
    *stale=__atomic_exchange_n(&region_stale,0,__ATOMIC_RELAXED);
    *fast=__atomic_exchange_n(&region_fast_hits,0,__ATOMIC_RELAXED);
}
/* sotc: the VMA holding address, read without the lock (0: none there, or the array kept changing:
 * take the lock). A torn read is caught by vma_seq. With gap_lo/gap_hi, an address in no VMA
 * returns 2 and the unmapped range around it instead of 0. */
/* sotc: SOTC_SEQ_SPIN (default 256): reads retried while a writer is inside a vma_seq section,
 * with a short pause between them (~25 us at most). Those sections last a memmove of the VMA
 * array; with 4 immediate tries the read gave up nearly every time it met one, and the GPU then
 * took the read lock and waited for the writer's whole map entry (~0.4 ms): ~14k waits per ride. */
static int seq_spin_tries(void) {
    static int v=-1;
    if (v<0) { const char *e=getenv("SOTC_SEQ_SPIN"); v=e ? atoi(e) : 256; if (v<4) v=4; }
    return v;
}
static inline void seq_pause(int tries) {
    if (tries<4) return;
    for (int i=0;i<8;++i) __builtin_ia32_pause();
}
static int vma_lookup_gap(uintptr_t address, Vma *out, uintptr_t *gap_lo, uintptr_t *gap_hi) {
    const int max_tries=seq_spin_tries();
    for (int tries=0; tries<max_tries; seq_pause(++tries)) {
        const uint64_t s=__atomic_load_n(&vma_seq,__ATOMIC_ACQUIRE);
        if (s & 1) continue;
        const size_t nb=__atomic_load_n(&vblock_count,__ATOMIC_RELAXED);
        if (nb>VMA_BLOCKS_MAX) continue;
        /* the first block whose last VMA ends above address */
        size_t lo=0, hi=nb; int torn=0;
        while (lo<hi) {
            size_t mid=(lo+hi)/2;
            const Vma *e=__atomic_load_n(&vblocks[mid].e,__ATOMIC_RELAXED);
            const size_t k=__atomic_load_n(&vblocks[mid].n,__ATOMIC_RELAXED);
            if (!e || !k || k>VMA_BLOCK) { torn=1; break; }
            if (__atomic_load_n(&e[k-1].end,__ATOMIC_RELAXED)<=address) lo=mid+1; else hi=mid;
        }
        if (torn) continue;
        Vma e={0};
        uintptr_t before=0;
        int found=0;
        if (lo>0) {
            const Vma *pe=__atomic_load_n(&vblocks[lo-1].e,__ATOMIC_RELAXED);
            const size_t pk=__atomic_load_n(&vblocks[lo-1].n,__ATOMIC_RELAXED);
            if (!pe || !pk || pk>VMA_BLOCK) continue;
            before=__atomic_load_n(&pe[pk-1].end,__ATOMIC_RELAXED);
        }
        if (lo<nb) {
            const Vma *be=__atomic_load_n(&vblocks[lo].e,__ATOMIC_RELAXED);
            const size_t bk=__atomic_load_n(&vblocks[lo].n,__ATOMIC_RELAXED);
            if (!be || !bk || bk>VMA_BLOCK) continue;
            size_t l=0, h=bk;
            while (l<h) {
                size_t mid=(l+h)/2;
                if (__atomic_load_n(&be[mid].end,__ATOMIC_RELAXED)<=address) l=mid+1; else h=mid;
            }
            if (l<bk) {
                if (l) before=__atomic_load_n(&be[l-1].end,__ATOMIC_RELAXED);
                e.start=__atomic_load_n(&be[l].start,__ATOMIC_RELAXED);
                e.end=__atomic_load_n(&be[l].end,__ATOMIC_RELAXED);
                e.kind=__atomic_load_n(&be[l].kind,__ATOMIC_RELAXED);
                e.prot=__atomic_load_n(&be[l].prot,__ATOMIC_RELAXED);
                e.phys=__atomic_load_n(&be[l].phys,__ATOMIC_RELAXED);
                found=1;
            }
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&vma_seq,__ATOMIC_RELAXED)!=s) continue;
        if (!found || e.start>address) {
            if (!gap_lo) return 0;
            *gap_lo=before; *gap_hi=!found ? UINTPTR_MAX : e.start;
            return 2;
        }
        *out=e;
        return 1;
    }
    return 0;
}
static int vma_lookup_fast(uintptr_t address, Vma *out) { return vma_lookup_gap(address,out,NULL,NULL); }
static __thread CachedRegion region_scratch;
static const CachedRegion *cached_region_q(uintptr_t address, uintptr_t qend) {
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if ((generation & 1) && !region_log_on()) return NULL;
    int found=0;
    if (region_direct_on) {
        CachedRegion *d=&region_direct[region_direct_slot(address)];
        if (d->end && d->start<=address && address<d->end) {
            if (region_current(d,generation,address,qend)) return d;
            found=1;
        }
    }
    for (unsigned i=0;i<8;++i) {
        CachedRegion *c=&region_cache[i];
        if (c->end && c->start<=address && address<c->end) {
            if (region_current(c,generation,address,qend)) return c;
            found=1;
        }
    }
    __atomic_add_fetch(found ? &region_stale : &region_misses,1,__ATOMIC_RELAXED);
    /* sotc: SOTC_REGION_FAST=0: misses take the lock. The cache missed ~100k lookups per second at
     * an area change (the GPU touches more regions than it holds), and each miss waited for the
     * writer of the moment: the VMA is read without the lock instead, and used when no write
     * since the generation read first changed its range (region_unchanged, the write under way
     * included). */
    static int fast=-1;
    if (fast<0) { const char *v=getenv("SOTC_REGION_FAST"); fast=!(v && v[0]=='0'); }
    if (!fast || !region_log_on() || !backing_base) return NULL;
    Vma e;
    if (!vma_lookup_fast(address,&e)) return NULL;
    const uint64_t now=__atomic_load_n(&table_generation,__ATOMIC_SEQ_CST);
    const uint64_t since=generation & ~(uint64_t)1;
    if (!((now==generation && !(now & 1)) || region_unchanged(e.start,e.end,since,now) ||
          (qend && e.kind==KIND_DIRECT && region_unchanged(address,qend<e.end ? qend : e.end,since,now)))) return NULL;
    const CachedRegion entry={since,e.start,e.end,e.kind,e.prot,e.kind!=KIND_RESERVED ? backing_base+e.phys : NULL};
    region_scratch=entry;
    region_cache[region_cache_next++ % 8]=entry;
    if (region_direct_on>0) region_direct[region_direct_slot(address)]=entry;
    __atomic_add_fetch(&region_fast_hits,1,__ATOMIC_RELAXED);
    return &region_scratch;
}
/* A lookup of address alone. */
static const CachedRegion *cached_region(uintptr_t address) { return cached_region_q(address,address+1); }
/* sotc: SOTC_WRITE_FAST=0 always takes the read lock. Readbacks write here thousands of times per
 * frame and at an area change waited for map batches of other ranges (~0.4 s per 5 s). Copies up
 * to 256 KiB whose regions the cache holds as unchanged (no write under way on them) are made
 * without the lock: every region is resolved before the first byte is written, else all of it
 * takes the lock. */
static int write_backing_fast(uintptr_t address, const void *data, uint64_t size) {
    static int on=-1;
    if (on<0) { const char *v=getenv("SOTC_WRITE_FAST"); on=!(v && v[0]=='0') && region_log_on(); }
    if (!on || exclusive_depth || (runtime_disabled_optimizations & 1) || !size || size>(256u<<10)) return 0;
    unsigned char *dst[4]; uint64_t len[4]; int parts=0;
    for (uintptr_t at=address, end=address+size; at<end; ++parts) {
        if (parts==4) return 0;
        const CachedRegion *c=cached_region_q(at,end);
        if (!c || c->kind==KIND_RESERVED || !c->backing) return 0;
        dst[parts]=(unsigned char *)c->backing+(at-c->start);
        len[parts]=(c->end<end ? c->end : end)-at;
        at+=len[parts];
    }
    const unsigned char *src=data;
    for (int i=0;i<parts;++i) { memcpy(dst[i],src,len[i]); src+=len[i]; }
    return 1;
}
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size) {
    if (write_backing_fast(address,data,size)) return 1;
    read_lock();
    int ok=1;
    for (uintptr_t at=address, end=address+size; at<end && ok;) {
        size_t i=vma_index(at);
        if (i==vma_count || V(i).start>at || V(i).kind==KIND_RESERVED) { ok=0; break; }
        uint64_t n=(V(i).end<end ? V(i).end : end)-at;
        memcpy(backing_base+V(i).phys+(at-V(i).start),(const unsigned char *)data+(at-address),n);
        at+=n;
    }
    read_unlock();
    return ok;
}
/* Called under the read lock after a lookup of address. */
static void cache_region(size_t i, uintptr_t address) {
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if (i==vma_count || (generation & 1)) return;
    const unsigned char *backing=V(i).kind!=KIND_RESERVED && backing_base ? backing_base+V(i).phys : NULL;
    const CachedRegion entry={generation,V(i).start,V(i).end,V(i).kind,V(i).prot,backing};
    region_cache[region_cache_next++ % 8]=entry;
    if (region_direct_on>0) region_direct[region_direct_slot(address)]=entry;
}
/* sotc: the VMA table generation, for caches of backing pointers outside this file: a pointer
 * obtained while it was even stays valid until it changes (the invariant cached_region relies
 * on: only write_lock/write_unlock bump it; page protection takes the read lock). Odd (never
 * cache) while the table changes or when the GPU caches are disabled. */
/* sotc: T38. A fault at the second address of a page: the GPU's watched address, or 0. */
uintptr_t runtime_memory_alias_of(uintptr_t address) {
    if (!__atomic_load_n(&alias_pages,__ATOMIC_RELAXED)) return 0;
    read_lock();
    const uintptr_t other=view_other(address,NULL);
    read_unlock();
    return other;
}
/* The 4 KiB page at [address] back to the guest's own protection after a write fault the
 * trackers saw at both addresses; 0 when the guest itself does not allow writing there. */
int runtime_memory_alias_release(uintptr_t address) {
    int writable=0;
    read_lock();
    const size_t i=vma_index(address);
    if (i<vma_count && V(i).start<=address && V(i).kind!=KIND_RESERVED) {
        const int guest=host_prot(V(i).prot);
        writable=(guest & PROT_WRITE)!=0;
        if (writable) host_protect(address & ~(uintptr_t)4095,4096,guest);
    }
    read_unlock();
    if (writable) __atomic_add_fetch(&alias_faults,1,__ATOMIC_RELAXED);
    return writable;
}
uint64_t runtime_memory_generation(void) {
    if (runtime_disabled_optimizations & 1) return 1;
    return __atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
}
/* sotc: the whole backing view (direct then flexible memory) and its size, for the GPU to import
 * it in place (SOTC_PC_MEMORY, docs/sotc/MODELO-MEMORIA-PC.md); NULL before the pool exists. */
const unsigned char *runtime_memory_backing_view(uint64_t *size) {
    if (size) *size=backing_base ? POOL_SIZE+FLEX_SPAN : 0;
    return backing_base;
}
/* sotc: SOTC_GAP_FAST=0: lookups of unmapped addresses take the read lock. At area changes the
 * GPU's lookups that land in a gap waited for the game's map batches although the answer (nothing
 * is mapped there) needs no lock: the gap is seen in a consistent (vma_seq) read and used when no
 * write since the generation read first changed [check_lo,check_hi) - a gap that exists only
 * during a write lies in its dirty range. 1: [*lo,*hi) is the gap around at (hi UINTPTR_MAX past
 * the last VMA); 0: unknown, take the lock. check_hi 0: check the whole gap. */
static int gap_fast(uintptr_t at, uintptr_t check_hi, uintptr_t *lo, uintptr_t *hi) {
    static int on=-1;
    if (on<0) { const char *v=getenv("SOTC_GAP_FAST"); on=!(v && v[0]=='0') && region_log_on(); }
    if (!on || !backing_base || exclusive_depth || (runtime_disabled_optimizations & 1)) return 0;
    const uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    Vma e;
    if (vma_lookup_gap(at,&e,lo,hi)!=2) return 0;
    const uint64_t now=__atomic_load_n(&table_generation,__ATOMIC_SEQ_CST);
    const uintptr_t a=check_hi ? at : *lo, b=check_hi ? (check_hi<*hi ? check_hi : *hi) : *hi;
    return (now==generation && !(now & 1)) || region_unchanged(a,b,generation & ~(uint64_t)1,now);
}
/* sotc: the GPU reads guest memory through the backing view on every upload; the per-thread
 * region cache spares it the table lock (a winpthread rwlock, slow) and the binary search. */
const unsigned char *runtime_memory_backing_pointer(uintptr_t address) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && c->backing) return c->backing+(address-c->start);
    if (c && c->kind==KIND_RESERVED) return NULL;
    uintptr_t gap_lo, gap_hi;
    if (!c && gap_fast(address,address+1,&gap_lo,&gap_hi)) return NULL;
    read_lock();
    size_t i=vma_index(address);
    const unsigned char *p=NULL;
    if (i<vma_count && V(i).start<=address) {
        cache_region(i,address);
        if (V(i).kind!=KIND_RESERVED && backing_base) p=backing_base+V(i).phys+(address-V(i).start);
    }
    read_unlock();
    return p;
}
/* Mapped length from address, up to size, across adjacent mappings. */
/* sotc: mapped length of [address, address+size) from the cached regions alone, walking
 * adjacent mappings (buffers that span two VMAs missed the one-region check every time and took
 * the read lock); 0 when a region on the way is not cached. */
/* sotc: SOTC_CLAMP_FAST=0 takes the read lock whenever the range is not mapped to its end. On a
 * world-streaming ride the GPU's clamps waited ~6 s per 5 minutes for the game's map batches,
 * nearly all of them for ranges that run into a reserved range or a gap (2.5M lookups), where
 * the answer is just where the mapping stops. That end is read without the lock too: a reserved
 * region the cache holds, or a gap seen in a consistent read of the VMAs that no write since
 * touched (as for mapped regions in cached_region_q). 1: *length is the mapped length from
 * address (capped at size); 0: unknown, take the lock. */
static int mapped_length_fast(uintptr_t address, uint64_t size, uint64_t *length) {
    static int on=-1;
    if (on<0) { const char *v=getenv("SOTC_CLAMP_FAST"); on=!(v && v[0]=='0') && region_log_on(); }
    if ((runtime_disabled_optimizations & 1) || size>UINTPTR_MAX-address) return 0;
    uint64_t n=0;
    int end_seen=0;
    for (unsigned hops=0; n<size && hops<4 && !end_seen; ++hops) {
        const uintptr_t at=address+n;
        const CachedRegion *c=cached_region_q(at,address+size);
        if (c) {
            if (c->kind==KIND_RESERVED) {
                if (!on) return 0;
                end_seen=1;
                continue;
            }
            n=c->end-address;
            continue;
        }
        uintptr_t lo, hi;
        if (!on || !gap_fast(at,address+size,&lo,&hi)) return 0;
        end_seen=1;
    }
    if (n<size && !end_seen) return 0;
    *length=n<size ? n : size;
    return 1;
}
/* sotc: 0.73: our lock-free gap check (SOTC_CLAMP_GAP, 0.72.2) is replaced by Oussama268's
 * mapped_length_fast (SOTC_CLAMP_FAST), which covers the same case; the counter stays at 0. */
static uint64_t clamp_gap_hits;
uint64_t runtime_memory_clamp_gap_hits(void) {
    return __atomic_exchange_n(&clamp_gap_hits,0,__ATOMIC_RELAXED);
}
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size) {
    uint64_t fast;
    if (size && mapped_length_fast(address,size,&fast)) return fast;
    read_lock();
    uint64_t length=0;
    for (size_t i=vma_index(address); i<vma_count && length<size; ++i) {
        if (V(i).start>address+length || V(i).kind==KIND_RESERVED) break;
        cache_region(i,V(i).start>address ? V(i).start : address);
        length=V(i).end-address;
    }
    read_unlock();
    return length<size ? length : size;
}
/* Region around address: [start,end) mapped or a gap; 0 past the last mapping. */
int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region_q(address,0);
    if (c) { *start=c->start; *end=c->end; *mapped=c->kind!=KIND_RESERVED; return 1; }
    uintptr_t gap_lo, gap_hi;
    if (gap_fast(address,0,&gap_lo,&gap_hi)) {
        if (gap_hi==UINTPTR_MAX) return 0;
        *start=gap_lo; *end=gap_hi; *mapped=0;
        return 1;
    }
    read_lock();
    size_t i=vma_index(address);
    int found=i<vma_count;
    if (found && V(i).start<=address) {
        cache_region(i,address);
        *start=V(i).start; *end=V(i).end; *mapped=V(i).kind!=KIND_RESERVED;
    } else if (found) {
        *start=i ? V(i-1).end : 0; *end=V(i).start; *mapped=0;
    }
    read_unlock();
    return found;
}
/* sotc: SOTC_COPY_LOCK_ONCE=1 (bbgpu CopySparseMemory): guest memory copied out through the
 * backing view in one call, for the GPU's texture and buffer uploads. They looked up the region
 * and then the backing pointer of every mapping run, and at an area change each lookup the region
 * cache could not answer waited for a map batch: 41-58 ms for 12-20 MB of textures in one frame
 * (user's route, 29 Oct). The cached regions are copied first; the rest under one read lock.
 * Reserved and unmapped bytes read as zero, as before. 0 (caller copies run by run) before the
 * backing view exists: a copy from guest addresses can fault, and the fault must not find the
 * read lock held. */
int runtime_memory_copy_out(uintptr_t source, unsigned char *dest, uint64_t size) {
    if (!backing_base) return 0;
    uintptr_t at=source;
    const uintptr_t end=source+size;
    if (!(runtime_disabled_optimizations & 1)) {
        while (at<end) {
            const CachedRegion *c=cached_region_q(at,end);
            if (!c || c->kind==KIND_RESERVED || !c->backing) break;
            const uintptr_t stop=c->end<end ? c->end : end;
            memcpy(dest+(at-source),c->backing+(at-c->start),stop-at);
            at=stop;
        }
    }
    if (at==end) return 1;
    read_lock();
    while (at<end) {
        const size_t i=vma_index(at);
        uintptr_t stop;
        if (i<vma_count && V(i).start<=at) {
            stop=V(i).end<end ? V(i).end : end;
            cache_region(i,at);
            if (V(i).kind==KIND_RESERVED) memset(dest+(at-source),0,stop-at);
            else memcpy(dest+(at-source),backing_base+V(i).phys+(at-V(i).start),stop-at);
        } else {
            stop=i<vma_count && V(i).start<end ? V(i).start : end;
            memset(dest+(at-source),0,stop-at);
        }
        at=stop;
    }
    read_unlock();
    return 1;
}
/* GPU page tracking: restrict host access, never beyond the guest's own protection. */
/* sotc: diagnostics for page tracking problems: the runtime's view of a page (VMA kind and guest
 * protection) next to the protection the host really has. */
void runtime_memory_describe(uintptr_t address, char *out, size_t capacity) {
    read_lock();
    const size_t i=vma_index(address);
    int kind=0, prot=-1;
    uintptr_t start=0, end=0;
    if (i<vma_count && V(i).start<=address) { kind=V(i).kind; prot=V(i).prot; start=V(i).start; end=V(i).end; }
    read_unlock();
    unsigned long host=0;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void *)address,&mbi,sizeof(mbi))) host=mbi.Protect;
#endif
    snprintf(out,capacity,"vma kind %d prot %#x [%#llx,%#llx), host protect %#lx",kind,prot,
             (unsigned long long)start,(unsigned long long)end,host);
}
/* sotc: SOTC_PROTECT_FAST=0 always takes the read lock. GPU page protection was the lock's
 * busiest reader at area changes (~0.3 s of waits per 5 s): it waited for map and unmap batches
 * of other ranges. When the range lies in one cached region that no write is changing, the host
 * protection is set without the lock, split where views can end (chunk boundaries, within one
 * VMA). A write that started meanwhile on the range (its carve or protect publishes the range
 * first, dirty()) may have saved and restored the old protection: it is then set again under the
 * lock, after that write. */
#ifdef _WIN32
/* sotc: SOTC_PROTECT_SPAN=0: only a range inside one region is protected without the lock. GPU
 * ranges that cross into another mapping, a reserved range or a gap (which the locked path skips)
 * took the lock and waited for map batches on the ride benchmark. Up to 8 pieces are resolved
 * first (cached regions, gaps seen consistently: gap_fast), then protected; the final check
 * covers the whole range as before. */
static int protect_fast(uintptr_t address, uint64_t size, int read, int write) {
    static int on=-1, span=-1;
    if (on<0) { const char *v=getenv("SOTC_PROTECT_FAST"); on=!(v && v[0]=='0') && region_log_on(); }
    if (span<0) { const char *v=getenv("SOTC_PROTECT_SPAN"); span=!(v && v[0]=='0'); }
    if (!on || exclusive_depth || (runtime_disabled_optimizations & 1) || !size || size>UINTPTR_MAX-address) return 0;
    const uint64_t before=__atomic_load_n(&table_generation,__ATOMIC_SEQ_CST);
    struct { uintptr_t a, b; DWORD protect; } piece[8];
    int pieces=0;
    for (uintptr_t at=address, end=address+size; at<end;) {
        const CachedRegion *c=cached_region_q(at,end);
        uintptr_t next;
        if (c) {
            next=c->end<end ? c->end : end;
            if (c->kind!=KIND_RESERVED) {
                if (pieces==8) return 0;
                const int guest=host_prot(c->prot);
                const int want=(read ? PROT_READ : 0)|(write ? PROT_WRITE|PROT_READ : 0)|(guest & PROT_EXEC);
                piece[pieces].a=at; piece[pieces].b=next; piece[pieces].protect=win_prot(guest & want);
                ++pieces;
            }
        } else {
            uintptr_t lo, hi;
            if (!gap_fast(at,end,&lo,&hi)) return 0;
            next=hi<end ? hi : end;
        }
        if (!span && next<end) return 0;
        at=next;
    }
    const uint64_t chunk=view_chunk();
    for (int i=0;i<pieces;++i) {
        for (uintptr_t at=piece[i].a, end=piece[i].b; at<end;) {
            uintptr_t next=chunk ? (at/chunk+1)*chunk : end;
            if (next>end) next=end;
            DWORD old;
            if (!VirtualProtect((void *)at,next-at,piece[i].protect,&old)) return 0;
            at=next;
        }
    }
    /* an odd generation is a write under way, which may have reached this range since */
    const uint64_t now=__atomic_load_n(&table_generation,__ATOMIC_SEQ_CST);
    return (now==before && !(now & 1)) || region_unchanged(address,address+size,before & ~(uint64_t)1,now);
}
#else
static int protect_fast(uintptr_t address, uint64_t size, int read, int write) {
    (void)address; (void)size; (void)read; (void)write;
    return 0;
}
#endif
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write) {
    if (protect_fast(address,size,read,write)) {
        if (__atomic_load_n(&alias_pages,__ATOMIC_RELAXED)) { read_lock(); views_protect(address,size,read,write); read_unlock(); }
        return;
    }
    read_lock();
    if (__atomic_load_n(&alias_pages,__ATOMIC_RELAXED)) views_protect(address,size,read,write);
    for (size_t i=vma_index(address); i<vma_count && V(i).start<address+size; ++i) {
        if (V(i).kind==KIND_RESERVED) continue;
        uintptr_t a=V(i).start>address ? V(i).start : address;
        uintptr_t b=V(i).end<address+size ? V(i).end : address+size;
        int guest=host_prot(V(i).prot);
        int want=(read ? PROT_READ : 0)|(write ? PROT_WRITE|PROT_READ : 0)|(guest & PROT_EXEC);
        host_protect(a,b-a,guest & want);
    }
    read_unlock();
}
/* Host view of a guest range, for the GPU backend: the range must be mapped. */
int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    uint64_t fast;
    if (size && mapped_length_fast(address,size,&fast)) return fast==size;
    read_lock();
    int result=covered(address,address+size,0);
    if (result) {
        uintptr_t at=address;
        for (size_t i=vma_index(address); i<vma_count && at<address+size; ++i) {
            cache_region(i,at);
            at=V(i).end;
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

