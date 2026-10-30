/* libkernel/libScePosix process services: clocks, sleeping, errno mapping,
 * pthread once/keys, signal bookkeeping and a narrow sysctl. Guest values
 * use FreeBSD numbering; host errno values never reach the guest directly. */
#define _GNU_SOURCE
#include "runtime.h"
#include "trace_imports.h"
#include "trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <x86intrin.h>
#ifdef _WIN32
#include <windows.h>
#endif
#ifndef _WIN32
#include <sys/random.h>
#include <sys/resource.h>
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define PAGE 16384

typedef struct { int64_t sec, nsec; } GuestTimespec;
typedef struct { int64_t sec, usec; } GuestTimeval;

int32_t runtime_guest_errno(int e) {
    switch (e) {
    case 0: return 0;
    case EPERM: return 1; case ENOENT: return 2; case ESRCH: return 3; case EINTR: return 4;
    case EIO: return 5; case ENXIO: return 6; case E2BIG: return 7; case ENOEXEC: return 8;
    case EBADF: return 9; case ECHILD: return 10; case EDEADLK: return 11; case ENOMEM: return 12;
    case EACCES: return 13; case EFAULT: return 14; case EBUSY: return 16; case EEXIST: return 17;
    case EXDEV: return 18; case ENODEV: return 19; case ENOTDIR: return 20; case EISDIR: return 21;
    case EINVAL: return 22; case ENFILE: return 23; case EMFILE: return 24; case ENOTTY: return 25;
    case EFBIG: return 27; case ENOSPC: return 28; case ESPIPE: return 29; case EROFS: return 30;
    case EMLINK: return 31; case EPIPE: return 32; case ERANGE: return 34; case EAGAIN: return 35;
    case ENAMETOOLONG: return 63; case ENOTEMPTY: return 66; case ETIMEDOUT: return 60;
    case ELOOP: return 62; case ENOSYS: return 78; case EOVERFLOW: return 84; case ECANCELED: return 85;
    default: return 5; /* EIO: unmapped host error */
    }
}
static int32_t fail_posix(int e) { *runtime_errno()=runtime_guest_errno(e); return -1; }

/* ---- clocks and sleeping ---- */
static struct timespec process_start;
__attribute__((constructor)) static void remember_start(void) { clock_gettime(CLOCK_MONOTONIC,&process_start); }
static int host_clock(uint32_t id,clockid_t *out) {
    switch (id) {
    case 0: case 9: case 10: *out=CLOCK_REALTIME; return 1;            /* REALTIME(_PRECISE/_FAST) */
    case 4: case 11: case 12: case 5: case 7: case 8: *out=CLOCK_MONOTONIC; return 1; /* MONOTONIC/UPTIME */
    case 13: *out=CLOCK_REALTIME_COARSE; return 1;                    /* SECOND */
    case 14: *out=CLOCK_THREAD_CPUTIME_ID; return 1;
    case 2: case 15: *out=CLOCK_PROCESS_CPUTIME_ID; return 1;         /* PROF/PROCTIME */
    case 16: case 17: case 18: case 19: *out=CLOCK_MONOTONIC; return 1; /* PS4 network clocks */
    default: return 0;
    }
}
static int clock_read(uint32_t id,GuestTimespec *ts) {
    clockid_t host;
    if (!ts || !host_clock(id,&host)) return EINVAL;
    struct timespec t;
    if (clock_gettime(host,&t)) return errno;
    if (id==13) t.tv_nsec=0;
    ts->sec=t.tv_sec; ts->nsec=t.tv_nsec; return 0;
}
static ABI int32_t kernel_clock_gettime(uint32_t id,GuestTimespec *ts) {
    int e=clock_read(id,ts); return e ? ERR(runtime_guest_errno(e)) : 0;
}
static ABI int32_t posix_clock_gettime(uint32_t id,GuestTimespec *ts) {
    int e=clock_read(id,ts); return e ? fail_posix(e) : 0;
}
static ABI int32_t posix_clock_getres(uint32_t id,GuestTimespec *ts) {
    clockid_t host; struct timespec t;
    if (!host_clock(id,&host)) return fail_posix(EINVAL);
    if (clock_getres(host,&t)) return fail_posix(errno);
    if (ts) { ts->sec=t.tv_sec; ts->nsec=t.tv_nsec; }
    return 0;
}
static ABI uint64_t process_time(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)(t.tv_sec-process_start.tv_sec)*1000000+(uint64_t)((t.tv_nsec-process_start.tv_nsec)/1000);
}
static ABI uint64_t process_time_counter(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)(t.tv_sec-process_start.tv_sec)*1000000000+(uint64_t)(t.tv_nsec-process_start.tv_nsec);
}
static ABI uint64_t process_time_frequency(void) { return 1000000000; }
/* The guest TSC is the host TSC unless the cores disagree on it (probe.c checks at startup);
 * then it is QPC, and the eboot's own rdtsc instructions are patched to read QPC too. */
static int tsc_from_qpc;
void runtime_tsc_use_qpc(void) { tsc_from_qpc=1; }
static uint64_t host_tsc(void) {
#ifdef _WIN32
    if (tsc_from_qpc) return compat_qpc_tsc();
#endif
    return __rdtsc();
}
static ABI uint64_t read_tsc(void) { return host_tsc(); }
static uint64_t tsc_hz;
static ABI uint64_t tsc_frequency(void) {
#ifdef _WIN32
    if (tsc_from_qpc) return compat_qpc_tsc_frequency();
#endif
    if (!tsc_hz) {
        struct timespec a,b,nap={0,20000000};
        clock_gettime(CLOCK_MONOTONIC,&a); uint64_t t0=__rdtsc();
#ifdef _WIN32
        (void)nap; compat_sleep_ns(20000000);
#else
        nanosleep(&nap,NULL);
#endif
        clock_gettime(CLOCK_MONOTONIC,&b); uint64_t t1=__rdtsc();
        uint64_t ns=(uint64_t)(b.tv_sec-a.tv_sec)*1000000000+(uint64_t)(b.tv_nsec-a.tv_nsec);
        tsc_hz=(t1-t0)*1000000000/(ns ? ns : 1);
    }
    return tsc_hz;
}
/* Shared with the GPU library so flip/label timestamps use the guest's clock. */
uint64_t runtime_process_time_us(void) { return process_time(); }
uint64_t runtime_process_time_counter(void) { return process_time_counter(); }
uint64_t runtime_tsc_frequency(void) { return tsc_frequency(); }
uint64_t runtime_read_tsc(void) { return host_tsc(); }
/* sotc: SOTC_TRACE: ra is the guest return address (the caller of the import). */
static int sleep_ns_from(uint64_t ns, void *ra) {
    const uint64_t t0=trace_now();
#ifdef _WIN32
    compat_sleep_ns(ns);
#else
    struct timespec t={.tv_sec=(time_t)(ns/1000000000),.tv_nsec=(long)(ns%1000000000)};
    while (nanosleep(&t,&t)) if (errno!=EINTR) { trace_span("sleep",t0,ns,(uint64_t)ra); return errno; }
#endif
    trace_span("sleep",t0,ns,(uint64_t)ra);
    return 0;
}
#define sleep_ns(ns) sleep_ns_from((ns),(void *)(uintptr_t)GUEST_CALLER())
/* sotc: usleep statistics (on when SOTC_FPS or SOTC_USLEEP_YIELD is set) and SOTC_USLEEP_YIELD.
 * Config is packed and read once: bits 0-1 the yield mode (SOTC_USLEEP_YIELD: 1 yields, 2 only
 * counts), bit 2 counting on, bits 8+ the yield threshold in us (SOTC_USLEEP_YIELD_US, default
 * 10). With everything off a usleep costs one relaxed load and the plain sleep. */
#define USLEEP_UNREAD UINT64_MAX
#define USLEEP_SLOTS 64
#define USLEEP_TOP 8
static _Atomic uint64_t usleep_config = USLEEP_UNREAD;
static struct {
    _Atomic uint64_t calls, requested_us, actual_ns, yielded, overflow;
    _Atomic uint64_t buckets[4];                   /* <=10 us, <=100 us, <=1 ms, more */
    _Atomic uintptr_t caller[USLEEP_SLOTS];        /* guest return addresses of requests <=100 us */
    _Atomic uint64_t caller_hits[USLEEP_SLOTS];
} usleep_stats;
static uint64_t usleep_read_config(void) {
    const char *yield = getenv("SOTC_USLEEP_YIELD");
    const char *threshold = getenv("SOTC_USLEEP_YIELD_US");
    unsigned long long us = 10;
    char *end = NULL;
    if (threshold) { unsigned long long v = strtoull(threshold, &end, 10); if (end != threshold) us = v; }
    if (us > 1000000) us = 1000000;
    int mode = yield ? atoi(yield) : 0;
    if (mode != 1 && mode != 2) mode = 0;
    int counting = mode != 0 || getenv("SOTC_FPS") != NULL;
    return (uint64_t)mode | (counting ? 4 : 0) | ((uint64_t)us << 8);
}
static uint64_t usleep_config_get(void) {
    uint64_t config = atomic_load_explicit(&usleep_config, memory_order_relaxed);
    if (config == USLEEP_UNREAD) {
        config = usleep_read_config();
        atomic_store_explicit(&usleep_config, config, memory_order_relaxed);
    }
    return config;
}
static uint64_t usleep_now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static void usleep_note_caller(uintptr_t caller) {
    for (size_t i=0;i<USLEEP_SLOTS;++i) {
        uintptr_t seen=atomic_load_explicit(&usleep_stats.caller[i],memory_order_relaxed);
        if (!seen) {
            uintptr_t empty=0;
            if (!atomic_compare_exchange_strong(&usleep_stats.caller[i],&empty,caller) && empty!=caller) continue;
            seen=caller;
        }
        if (seen==caller) { atomic_fetch_add(&usleep_stats.caller_hits[i],1); return; }
    }
    atomic_fetch_add(&usleep_stats.overflow,1);
}
static void usleep_yield_thread(void) {
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}
/* ra is the guest caller of the import. */
static void usleep_guest(uint32_t usec, void *ra) {
    const uint64_t config = usleep_config_get();
    if (!(config & 4)) { sleep_ns_from((uint64_t)usec*1000,ra); return; }
    const unsigned mode = (unsigned)(config & 3);
    const uint64_t yield_us = config >> 8;
    const int would_yield = mode && usec<=yield_us;
    const int yields = mode==1 && would_yield;
    const uint64_t t0 = usleep_now_ns();
    if (would_yield) atomic_fetch_add_explicit(&usleep_stats.yielded,1,memory_order_relaxed);
    if (yields) usleep_yield_thread(); else sleep_ns_from((uint64_t)usec*1000,ra);
    const uint64_t actual = usleep_now_ns()-t0;
    const size_t bucket = usec<=10 ? 0 : usec<=100 ? 1 : usec<=1000 ? 2 : 3;
    atomic_fetch_add_explicit(&usleep_stats.calls,1,memory_order_relaxed);
    atomic_fetch_add_explicit(&usleep_stats.buckets[bucket],1,memory_order_relaxed);
    atomic_fetch_add_explicit(&usleep_stats.requested_us,usec,memory_order_relaxed);
    atomic_fetch_add_explicit(&usleep_stats.actual_ns,actual,memory_order_relaxed);
    if (usec<=100) usleep_note_caller((uintptr_t)ra);
}
static ABI int32_t kernel_usleep(uint32_t usec) { usleep_guest(usec,(void *)(uintptr_t)GUEST_CALLER()); return 0; }
static ABI int32_t posix_usleep(uint32_t usec) { usleep_guest(usec,(void *)(uintptr_t)GUEST_CALLER()); return 0; }
/* sotc: one "SOTC USLEEP:" line for the SOTC_FPS report, then the counters are reset. seconds is
 * the window length (per-second rates). Callers are guest return addresses (host addresses of
 * the mapped image; subtract the image base for an eboot offset). */
void runtime_usleep_stats(char *out, size_t size, double seconds) {
    if (!out || !size) return;
    const uint64_t config = usleep_config_get();
    const unsigned mode = (unsigned)(config & 3);
    const uint64_t yield_us = config >> 8;
    const uint64_t calls = atomic_exchange_explicit(&usleep_stats.calls,0,memory_order_relaxed);
    uint64_t b[4];
    for (int i=0;i<4;++i) b[i]=atomic_exchange_explicit(&usleep_stats.buckets[i],0,memory_order_relaxed);
    const uint64_t requested=atomic_exchange_explicit(&usleep_stats.requested_us,0,memory_order_relaxed);
    const uint64_t actual=atomic_exchange_explicit(&usleep_stats.actual_ns,0,memory_order_relaxed);
    const uint64_t yielded=atomic_exchange_explicit(&usleep_stats.yielded,0,memory_order_relaxed);
    const uint64_t overflow=atomic_exchange_explicit(&usleep_stats.overflow,0,memory_order_relaxed);
    uintptr_t addr[USLEEP_SLOTS]; uint64_t hits[USLEEP_SLOTS]; size_t n=0;
    for (size_t i=0;i<USLEEP_SLOTS;++i) {
        uintptr_t a=atomic_load_explicit(&usleep_stats.caller[i],memory_order_relaxed);
        uint64_t h=atomic_exchange_explicit(&usleep_stats.caller_hits[i],0,memory_order_relaxed);
        if (a && h) { addr[n]=a; hits[n]=h; ++n; }
    }
    if (!calls) { out[0]='\0'; return; } /* sotc: no line in an idle window */
    const double window = seconds>0 ? seconds : 1.0;
    const char *yield_name = mode==1 ? "yield" : mode==2 ? "count" : "off";
    int w=snprintf(out,size,
        "SOTC USLEEP: calls=%llu (%.0f/s) short<=100us=%.0f/s <=10us=%llu <=100us=%llu <=1ms=%llu more=%llu "
        "requested=%.1f ms actual=%.1f ms yield=%s<=%llu us yielded=%llu overflow=%llu top:",
        (unsigned long long)calls, calls/window, (b[0]+b[1])/window,
        (unsigned long long)b[0], (unsigned long long)b[1], (unsigned long long)b[2], (unsigned long long)b[3],
        requested/1000.0, actual/1000000.0, yield_name, (unsigned long long)yield_us,
        (unsigned long long)yielded, (unsigned long long)overflow);
    size_t used = w<0 ? 0 : (size_t)w;
    if (used>=size) used=size-1;
    for (size_t k=0;k<n && k<USLEEP_TOP;++k) {
        size_t best=k;
        for (size_t j=k+1;j<n;++j) if (hits[j]>hits[best]) best=j;
        uintptr_t ta=addr[k]; addr[k]=addr[best]; addr[best]=ta;
        uint64_t th=hits[k]; hits[k]=hits[best]; hits[best]=th;
        int t=snprintf(out+used,size-used," 0x%llx=%llu",(unsigned long long)addr[k],(unsigned long long)hits[k]);
        if (t>0) { used+=(size_t)t; if (used>=size) used=size-1; }
    }
}
static ABI uint32_t posix_sleep(uint32_t seconds) { sleep_ns((uint64_t)seconds*1000000000); return 0; }
static ABI int32_t posix_nanosleep(const GuestTimespec *rq,GuestTimespec *rem) {
    if (!rq || rq->nsec<0 || rq->nsec>=1000000000 || rq->sec<0) return fail_posix(EINVAL);
    sleep_ns((uint64_t)rq->sec*1000000000+(uint64_t)rq->nsec);
    if (rem) { rem->sec=0; rem->nsec=0; }
    return 0;
}
static ABI int32_t kernel_nanosleep(const GuestTimespec *rq,GuestTimespec *rem) {
    if (!rq || rq->nsec<0 || rq->nsec>=1000000000 || rq->sec<0) { fail_posix(EINVAL); return ERR(*runtime_errno()); }
    sleep_ns((uint64_t)rq->sec*1000000000+(uint64_t)rq->nsec);
    if (rem) { rem->sec=0; rem->nsec=0; }
    return 0;
}
typedef struct { int32_t minuteswest, dsttime; } GuestTimezone;
static ABI int32_t kernel_gettimezone(GuestTimezone *tz) {
    if (!tz) return ERR(22);
    time_t now=time(NULL);
#ifdef _WIN32
    tz->minuteswest=(int32_t)(-compat_utc_offset(now)/60); tz->dsttime=0;
#else
    struct tm local; localtime_r(&now,&local);
    tz->minuteswest=(int32_t)(-local.tm_gmtoff/60); tz->dsttime=0;
#endif
    return 0;
}
static ABI int32_t posix_gettimeofday(GuestTimeval *tv,GuestTimezone *tz) {
    struct timeval t;
    gettimeofday(&t,NULL);
    if (tv) { tv->sec=t.tv_sec; tv->usec=t.tv_usec; }
    if (tz) kernel_gettimezone(tz);
    return 0;
}
static ABI int32_t kernel_gettimeofday(GuestTimeval *tv) {
    if (!tv) return ERR(22);
    return posix_gettimeofday(tv,NULL);
}
static ABI int64_t posix_time(int64_t *out) { int64_t t=(int64_t)time(NULL); if (out) *out=t; return t; }

/* ---- process ---- */
static ABI int32_t get_pagesize(void) { return PAGE; }
static ABI int32_t get_pid(void) { return 1000; }
static ABI int32_t yield(void) { sched_yield(); return 0; }
static ABI __attribute__((noreturn)) void hard_exit(int status) {
    printf("Runtime: guest requested _exit(%d)\n",status);
    runtime_report();
    fflush(stdout);
    _exit(status);
}
static ABI __attribute__((noreturn)) void raise_exception(uint32_t code,uint64_t argument) {
    fprintf(stderr,"STOP: guest raised debug exception code=0x%x argument=0x%llx\n",code,(unsigned long long)argument);
    runtime_report();
    exit(23);
}
static ABI int32_t print_backtrace(void) {
    fputs("Runtime: guest requested a backtrace (not available)\n",stderr);
    return 0;
}
typedef struct { uint32_t bits[4]; } GuestSigset;
static _Thread_local GuestSigset signal_mask;
static uintptr_t handlers[128];
static ABI uintptr_t guest_signal(int sig,uintptr_t handler) {
    if (sig<=0 || sig>=128) { *runtime_errno()=22; return (uintptr_t)-1; } /* SIG_ERR */
    uintptr_t old=handlers[sig]; handlers[sig]=handler;
    printf("Runtime: guest signal(%d) handler recorded (host signals are not forwarded)\n",sig);
    return old;
}
static ABI int32_t guest_sigprocmask(int how,const GuestSigset *set,GuestSigset *old) {
    if (old) *old=signal_mask;
    if (!set) return 0;
    for (int i=0;i<4;++i) {
        if (how==1) signal_mask.bits[i]|=set->bits[i];        /* SIG_BLOCK */
        else if (how==2) signal_mask.bits[i]&=~set->bits[i];  /* SIG_UNBLOCK */
        else if (how==3) signal_mask.bits[i]=set->bits[i];    /* SIG_SETMASK */
        else return fail_posix(EINVAL);
    }
    return 0;
}
static ABI int32_t guest_sigfillset(GuestSigset *set) { if (!set) return fail_posix(EINVAL); memset(set,0xff,sizeof(*set)); return 0; }
static ABI int32_t guest_sigemptyset(GuestSigset *set) { if (!set) return fail_posix(EINVAL); memset(set,0,sizeof(*set)); return 0; }
typedef struct { GuestTimeval utime, stime; int64_t rest[14]; } GuestRusage;
static ABI int32_t guest_getrusage(int who,GuestRusage *out) {
    if (!out || (who!=0 && who!=1)) return fail_posix(EINVAL);
    memset(out,0,sizeof(*out));
#ifdef _WIN32
    int64_t user=0,kernel=0;
    compat_cpu_times(who,&user,&kernel);
    out->utime=(GuestTimeval){user/1000000,user%1000000};
    out->stime=(GuestTimeval){kernel/1000000,kernel%1000000};
#else
    struct rusage r;
    getrusage(who==0 ? RUSAGE_SELF : RUSAGE_THREAD,&r);
    out->utime=(GuestTimeval){r.ru_utime.tv_sec,r.ru_utime.tv_usec};
    out->stime=(GuestTimeval){r.ru_stime.tv_sec,r.ru_stime.tv_usec};
#endif
    return 0;
}
static ABI int32_t guest_sysctl(const int32_t *name,uint32_t namelen,void *old,uint64_t *oldlen,const void *new_value,uint64_t newlen) {
    (void)newlen;
    if (!name || namelen<2 || new_value) return fail_posix(EINVAL);
    if (name[0]==1 && name[1]==37) { /* kern.arandom */
        if (!old || !oldlen) return fail_posix(EINVAL);
#ifdef _WIN32
        if (compat_random(old,(size_t)*oldlen)) return fail_posix(EIO);
#else
        if (getrandom(old,(size_t)*oldlen,0)<0) return fail_posix(errno);
#endif
        return 0;
    }
    if (name[0]==6 && (name[1]==7 || name[1]==3)) { /* hw.pagesize / hw.ncpu */
        if (!oldlen) return fail_posix(EINVAL);
        int32_t value=name[1]==7 ? PAGE : 7;
        if (old) { if (*oldlen<4) return fail_posix(ENOMEM); memcpy(old,&value,4); }
        *oldlen=4; return 0;
    }
    fprintf(stderr,"STOP: unsupported sysctl mib");
    for (uint32_t i=0;i<namelen && i<8;++i) fprintf(stderr," %d",name[i]);
    fputc('\n',stderr);
    exit(21);
}

/* ---- pthread_once and thread-specific data ---- */
static ABI int32_t thread_once(int32_t *once,void (ABI *routine)(void)) {
    if (!once || !routine) return ERR(22);
    for (;;) {
        int32_t state=__atomic_load_n(once,__ATOMIC_ACQUIRE);
        if (state==1) return 0;
        if (state==0 && __atomic_compare_exchange_n(once,&state,2,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            routine();
            __atomic_store_n(once,1,__ATOMIC_RELEASE);
            return 0;
        }
        sched_yield();
    }
}
static ABI int32_t posix_once(int32_t *once,void (ABI *routine)(void)) { return thread_once(once,routine) ? 22 : 0; }
#define KEYS 256
typedef void (ABI *KeyDestructor)(void *);
static struct { int used; KeyDestructor destructor; } keys[KEYS];
static pthread_mutex_t key_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local void *key_values[KEYS];
static ABI int32_t key_create(uint32_t *key,KeyDestructor destructor) {
    if (!key) return ERR(22);
    pthread_mutex_lock(&key_lock);
    for (uint32_t i=1;i<KEYS;++i) if (!keys[i].used) {
        keys[i].used=1; keys[i].destructor=destructor;
        pthread_mutex_unlock(&key_lock);
        *key=i; return 0;
    }
    pthread_mutex_unlock(&key_lock);
    return ERR(35);
}
static ABI int32_t key_delete(uint32_t key) {
    if (!key || key>=KEYS) return ERR(22);
    pthread_mutex_lock(&key_lock);
    int32_t r=keys[key].used ? 0 : ERR(22);
    keys[key].used=0; keys[key].destructor=NULL;
    pthread_mutex_unlock(&key_lock);
    return r;
}
static ABI int32_t key_set(uint32_t key,void *value) {
    if (!key || key>=KEYS || !keys[key].used) return ERR(22);
    key_values[key]=value; return 0;
}
static ABI void *key_get(uint32_t key) { return key && key<KEYS ? key_values[key] : NULL; }
void runtime_thread_keys_cleanup(void) {
    for (int round=0;round<4;++round) {
        int any=0;
        for (uint32_t i=1;i<KEYS;++i) {
            void *value=key_values[i];
            if (!value || !keys[i].used || !keys[i].destructor) continue;
            key_values[i]=NULL; any=1;
            keys[i].destructor(value);
        }
        if (!any) break;
    }
}
static ABI int32_t posix_key_create(uint32_t *k,KeyDestructor d) { int32_t r=key_create(k,d); return r ? r&0xffff : 0; }
static ABI int32_t posix_key_delete(uint32_t k) { int32_t r=key_delete(k); return r ? r&0xffff : 0; }
static ABI int32_t posix_key_set(uint32_t k,void *v) { int32_t r=key_set(k,v); return r ? r&0xffff : 0; }

static const RuntimeExport exports[]={
    {"sceKernelClockGettime",kernel_clock_gettime}, {"clock_gettime",posix_clock_gettime},
    {"clock_getres",posix_clock_getres},
    {"sceKernelGetProcessTime",process_time}, {"sceKernelGetProcessTimeCounter",process_time_counter},
    {"sceKernelGetProcessTimeCounterFrequency",process_time_frequency},
    {"sceKernelReadTsc",read_tsc}, {"sceKernelGetTscFrequency",tsc_frequency},
    {"sceKernelUsleep",kernel_usleep}, {"usleep",posix_usleep}, {"sleep",posix_sleep},
    {"nanosleep",posix_nanosleep}, {"sceKernelNanosleep",kernel_nanosleep},
    {"sceKernelGettimezone",kernel_gettimezone}, {"gettimeofday",posix_gettimeofday},
    {"sceKernelGettimeofday",kernel_gettimeofday},
    {"time",posix_time},
    {"getpagesize",get_pagesize}, {"getpid",get_pid}, {"sched_yield",yield},
    {"_exit",hard_exit},
    {"sceKernelDebugRaiseException",raise_exception},
    {"sceKernelDebugRaiseExceptionOnReleaseMode",raise_exception},
    {"sceKernelPrintBacktraceWithModuleInfo",print_backtrace},
    {"signal",guest_signal}, {"sigprocmask",guest_sigprocmask}, {"_sigprocmask",guest_sigprocmask},
    {"sigfillset",guest_sigfillset}, {"sigemptyset",guest_sigemptyset},
    {"getrusage",guest_getrusage}, {"sysctl",guest_sysctl},
    {"scePthreadOnce",thread_once}, {"pthread_once",posix_once},
    {"scePthreadKeyCreate",key_create}, {"scePthreadKeyDelete",key_delete},
    {"scePthreadSetspecific",key_set}, {"scePthreadGetspecific",key_get},
    {"pthread_key_create",posix_key_create}, {"pthread_key_delete",posix_key_delete},
    {"pthread_setspecific",posix_key_set}, {"pthread_getspecific",key_get},
};
uintptr_t runtime_kernel_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
