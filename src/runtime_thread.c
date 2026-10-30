/* Guest threads on host pthreads. Each guest thread owns a FreeBSD-style TCB
 * (variant II: static TLS below the TCB). The loader rewrites the eboot's
 * `mov rax, fs:[0]` into `mov rax, gs:[0]`, so GS base = guest TCB while glibc
 * keeps FS. Priorities/affinity are recorded, not enforced by a PS4 scheduler.
 * Windows: GS is the TEB and cannot be moved. The guest TCB lives in a TLS slot of the TEB
 * instead, and the loader rewrites the load to `mov rax, gs:[0x1480+8*slot]` (TlsSlots),
 * the same 9-byte instruction with another displacement. */
#define _GNU_SOURCE
#include "runtime.h"
#include "trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <errno.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/syscall.h>
#include <sys/mman.h>
#include <asm/prctl.h>
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define ATTR_MAGIC UINT32_C(0x41545452)
#define STACK_MARGIN (256*1024)
#define MIN_STACK (64*1024)
#define DEFAULT_STACK (1024*1024)
#define DEFAULT_PRIO 700

typedef void *(ABI *GuestEntry)(void *);
typedef struct ThreadAttr {
    uint32_t magic;
    int detached, policy, prio, inherit;
    uint64_t stack, guard, affinity;
    struct ThreadAttr *next;
} ThreadAttr;
typedef struct GuestThread {
    /* Guest-visible TCB is allocated separately; this is the ScePthread handle. */
    uint64_t *tcb;
    unsigned char *tls_block;
    pthread_t host;
    GuestEntry entry;
    void *argument, *result;
    ThreadAttr attr;
    char name[32];
    int detached, finished, joined, host_owned;
#ifdef _WIN32
    void *os_handle; /* sotc: set by the thread itself under `lock` (runtime_thread_suspend_others) */
    bb_jmp_buf exit_jump; /* no SEH unwinding through guest frames */
#else
    jmp_buf exit_jump;
#endif
    struct GuestThread *next;
} GuestThread;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static ThreadAttr *attributes;
static GuestThread *threads;
static _Thread_local GuestThread *current;
#ifdef _WIN32
/* sotc: stops every other guest thread (returns once each one is really stopped) while the memory
 * runtime unmaps and maps again live views of guest memory (runtime_memory.c carve_host): a guest
 * thread touching the view meanwhile, its own fiber stack included, would fault where Windows
 * cannot even push the exception frame, and the process died silently at startup. Called under
 * the memory runtime's write lock. `lock` stays held until runtime_thread_resume: a thread that is
 * starting blocks in host_start before running guest code. Only OS handles the threads opened
 * themselves are used (no winpthreads call while others are stopped: one may hold its lock). */
size_t runtime_thread_suspend_others(void **out, size_t capacity) {
    size_t n=0;
    pthread_mutex_lock(&lock);
    /* sotc: every thread is asked to stop first, then each is waited for: the stops proceed in
     * parallel instead of one kernel round trip after the other (17+ guest threads). */
    for (GuestThread *t=threads; t && n<capacity; t=t->next) {
        if (t==current || t->finished || !t->os_handle) continue;
        HANDLE h=t->os_handle;
        if (SuspendThread(h)==(DWORD)-1) continue;
        out[n++]=h;
    }
    for (size_t i=0;i<n;++i) {
        CONTEXT c;
        c.ContextFlags=CONTEXT_CONTROL;
        GetThreadContext(out[i],&c); /* returns once the thread is really stopped */
    }
    return n;
}
/* Resumes what runtime_thread_suspend_others stopped and releases `lock` (always call it). */
void runtime_thread_resume(void **handles, size_t n) {
    for (size_t i=0;i<n;++i) ResumeThread(handles[i]);
    pthread_mutex_unlock(&lock);
}
#endif
/* sotc: true on threads that run guest code (created by the guest, or the main thread). */
int runtime_thread_is_guest(void) { return current!=NULL; }
static _Thread_local int32_t guest_errno;
static const unsigned char *tls_template;
static uint64_t tls_filesz, tls_memsz, tls_align=16;
static size_t created, joined_count, exited;

void runtime_set_main_tls(const void *data,uint64_t filesz,uint64_t memsz,uint64_t align) {
    tls_template=data; tls_filesz=filesz; tls_memsz=memsz; tls_align=align ? align : 16;
}
static uint64_t tls_offset(void) { return (tls_memsz+tls_align-1)&~(tls_align-1); }
#ifdef _WIN32
/* TEB TlsSlots[64] start at gs:0x1480; slots 64+ live elsewhere and cannot be used. */
static DWORD tls_slot=TLS_OUT_OF_INDEXES;
uint32_t runtime_thread_tcb_offset(void) {
    if (tls_slot==TLS_OUT_OF_INDEXES) {
        tls_slot=TlsAlloc();
        if (tls_slot>=64) { fprintf(stderr,"STOP: no TEB TLS slot below 64 (got %lu)\n",tls_slot); exit(21); }
    }
    return 0x1480+8*(uint32_t)tls_slot;
}
static void set_gs(void *base) {
    runtime_thread_tcb_offset();
    if (!TlsSetValue(tls_slot,base)) { fprintf(stderr,"STOP: TlsSetValue failed (%lu)\n",GetLastError()); exit(21); }
}
#define tls_block_alloc(size) _aligned_malloc(size,64)
#else
static void set_gs(void *base) {
    if (syscall(SYS_arch_prctl,ARCH_SET_GS,(unsigned long)base)) { perror("STOP: arch_prctl(ARCH_SET_GS)"); exit(21); }
}
#define tls_block_alloc(size) aligned_alloc(64,size)
#endif
/* Build TCB/static TLS for the calling host thread and point GS at it. */
static void attach(GuestThread *t) {
    uint64_t offset=tls_offset();
    size_t total=offset+256;
    unsigned char *block=tls_block_alloc((total+63)&~(size_t)63);
    if (!block) { fputs("Cannot allocate guest TLS\n",stderr); exit(1); }
    memset(block,0,total);
    if (tls_filesz) memcpy(block,tls_template,tls_filesz);
    uint64_t *tcb=(uint64_t *)(block+offset);
    static uint64_t dtv[3];
    tcb[0]=(uint64_t)(uintptr_t)tcb;         /* tcb_self */
    tcb[1]=(uint64_t)(uintptr_t)dtv;         /* tcb_dtv (static module only) */
    tcb[2]=(uint64_t)(uintptr_t)t;           /* tcb_thread */
    t->tls_block=block; t->tcb=tcb;
    set_gs(tcb);
    current=t;
}
static GuestThread *new_thread(void) {
    GuestThread *t=calloc(1,sizeof(*t));
    if (!t) return NULL;
    t->attr=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.stack=DEFAULT_STACK,.affinity=0x7f};
    return t;
}
static void publish(GuestThread *t) {
    pthread_mutex_lock(&lock); t->next=threads; threads=t; pthread_mutex_unlock(&lock);
}
/* Host threads that call into guest code without being created by the guest
 * (the main thread, test threads) get a record on first use. */
GuestThread *runtime_thread_current(void) {
    if (current) return current;
    GuestThread *t=new_thread();
    if (!t) { fputs("Cannot allocate guest thread\n",stderr); exit(1); }
    t->host=pthread_self(); t->host_owned=1;
    snprintf(t->name,sizeof(t->name),"host");
    attach(t); publish(t);
    return t;
}
/* Host threads that call guest code (HLE decoders invoking guest callbacks) need
 * a TCB/TLS like guest threads; the record lives until process exit. */
void runtime_thread_attach_host(const char *name) {
    GuestThread *t=runtime_thread_current();
    snprintf(t->name,sizeof(t->name),"%s",name ? name : "host");
}
void runtime_thread_attach_main(void) {
    GuestThread *t=runtime_thread_current();
    snprintf(t->name,sizeof(t->name),"main");
}
static GuestThread *find_thread(void *handle) {
    GuestThread *found=NULL;
    pthread_mutex_lock(&lock);
    for (GuestThread *t=threads;t;t=t->next) if (t==handle) { found=t; break; }
    pthread_mutex_unlock(&lock);
    return found;
}
static ThreadAttr *find_attr(ThreadAttr **slot) {
    if (!slot || !*slot) return NULL;
    ThreadAttr *found=NULL;
    pthread_mutex_lock(&lock);
    for (ThreadAttr *a=attributes;a;a=a->next) if (a==*slot && a->magic==ATTR_MAGIC) { found=a; break; }
    pthread_mutex_unlock(&lock);
    return found;
}

static ABI void *thread_self(void) { return runtime_thread_current(); }
static ABI int32_t *guest_error(void) { return &guest_errno; }
int32_t *runtime_errno(void) { return &guest_errno; }

static ABI int32_t attr_init(ThreadAttr **out) {
    if (!out) return ERR(22);
    ThreadAttr *a=calloc(1,sizeof(*a));
    if (!a) return ERR(12);
    *a=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.inherit=4,.stack=DEFAULT_STACK,.guard=4096,.affinity=0x7f};
    pthread_mutex_lock(&lock); a->next=attributes; attributes=a; pthread_mutex_unlock(&lock);
    *out=a; return 0;
}
static ABI int32_t attr_destroy(ThreadAttr **slot) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    pthread_mutex_lock(&lock);
    ThreadAttr **link=&attributes;
    while (*link!=a) link=&(*link)->next;
    *link=a->next; a->magic=0;
    pthread_mutex_unlock(&lock);
    free(a); *slot=NULL; return 0;
}
static ABI int32_t attr_get(void *thread,ThreadAttr **out) {
    ThreadAttr *a=find_attr(out);
    if (!a || !thread) return ERR(22);
    GuestThread *t=find_thread(thread);
    if (!t) return ERR(3);
    ThreadAttr *next=a->next;
    *a=t->attr; a->magic=ATTR_MAGIC; a->next=next;
    a->detached=t->detached;
    return 0;
}
static ABI int32_t attr_set_stack(ThreadAttr **slot,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || size<16384) return ERR(22);
    a->stack=size; return 0;
}
/* Stacks are placed by the runtime, never by the guest. */
static ABI int32_t attr_get_stackaddr(ThreadAttr **slot,void **address) {
    if (!find_attr(slot) || !address) return ERR(22);
    *address=NULL; return 0;
}
static ABI int32_t attr_get_stack(ThreadAttr **slot,uint64_t *size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !size) return ERR(22);
    *size=a->stack; return 0;
}
static ABI int32_t attr_set_detach(ThreadAttr **slot,int state) {
    ThreadAttr *a=find_attr(slot);
    if (!a || (state!=0 && state!=1)) return ERR(22);
    a->detached=state; return 0;
}
static ABI int32_t attr_get_detach(ThreadAttr **slot,int *state) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !state) return ERR(22);
    *state=a->detached; return 0;
}
static ABI int32_t attr_set_policy(ThreadAttr **slot,int policy) {
    ThreadAttr *a=find_attr(slot);
    if (!a || policy<1 || policy>3) return ERR(22);
    a->policy=policy; return 0;
}
static ABI int32_t attr_set_inherit(ThreadAttr **slot,int inherit) {
    ThreadAttr *a=find_attr(slot);
    if (!a || (inherit!=0 && inherit!=4)) return ERR(22);
    a->inherit=inherit; return 0;
}
static ABI int32_t attr_set_param(ThreadAttr **slot,const int *param) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !param) return ERR(22);
    a->prio=*param; return 0;
}
static ABI int32_t attr_get_param(ThreadAttr **slot,int *param) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !param) return ERR(22);
    *param=a->prio; return 0;
}
static ABI int32_t attr_set_affinity(ThreadAttr **slot,uint64_t mask) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    a->affinity=mask; return 0;
}
static ABI int32_t attr_affinity(ThreadAttr **slot,uint64_t *mask) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !mask) return ERR(22);
    *mask=a->affinity; return 0;
}
static ABI int32_t attr_set_guard(ThreadAttr **slot,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    a->guard=size; return 0;
}

/* Linux thread names hold 15 characters; longer ones would be rejected. */
static void set_host_name(const char *name) {
    char host[16]={0};
    memcpy(host,name,strnlen(name,sizeof(host)-1));
    pthread_setname_np(pthread_self(),host);
}
#ifdef _WIN32
#define exit_setjmp(t) bb_setjmp((t)->exit_jump)
#define exit_longjmp(t) bb_longjmp((t)->exit_jump,1)
#else
#define exit_setjmp(t) setjmp((t)->exit_jump)
#define exit_longjmp(t) longjmp((t)->exit_jump,1)
#endif
/* sotc: SOTC_STALL_DETAIL: the GPU library counts this thread's CPU time (weak: absent builds skip it). */
void bbgpu_register_guest_thread(void) __attribute__((weak));
#ifdef _WIN32
/* sotc: SOTC_GAME_AFFINITY=1 (off by default). The game's job workers ("BPE JobWorkerThread CPUn",
 * report 16) get a Windows affinity that leaves out the logical CPUs the port pins its own GPU threads
 * to with SOTC_CPUSETS=1 (gpu/shim/bbport_threads.h, not edited here): the first logical CPU of the
 * first four physical cores, in the same order (fastest efficiency class first, then core index). The
 * workers get every other logical CPU this process may run on. Placement needs SOTC_CPUSETS=1 as well,
 * and the same layout gate as GetCpuLayout (at least 6 physical cores, at least 4 in the fastest
 * efficiency class). Otherwise nothing is placed and one line says which condition failed. Guest
 * priorities are not mapped. Each placed worker logs one line.
 * The affinity is set by the worker itself, before any guest code runs on it. */
#define GAME_JOB_WORKER_NAME "BPE JobWorkerThread"
static int game_affinity_on(void) {
    static int on=-1; /* read once; a benign race writes the same value */
    if (on<0) { const char *v=getenv("SOTC_GAME_AFFINITY"); on=(v && v[0]=='1'); }
    return on;
}
/* sotc: SOTC_CPUSETS=1 (same parsing as CpuSetsEnabled in gpu/shim/bbport_threads.h). */
static int cpusets_on(void) {
    static int on=-1; /* read once; a benign race writes the same value */
    if (on<0) { const char *v=getenv("SOTC_CPUSETS"); on=(v && v[0]=='1'); }
    return on;
}
static pthread_mutex_t game_affinity_lock=PTHREAD_MUTEX_INITIALIZER;
static int game_affinity_ready;
static DWORD_PTR game_affinity_mask_value;
typedef struct { BYTE core, lp, eff; } GameCpu;

/* Computes the worker mask (group 0 only), or 0 when placement does not apply. The gate is the one in
 * GetCpuLayout (gpu/shim/bbport_threads.h): SOTC_CPUSETS=1, at least 6 physical cores among the CPUs this
 * process may run on, and at least 4 of them in the fastest efficiency class. The port only reserves its
 * four cores under that same gate, so the two sides agree. */
static DWORD_PTR game_affinity_compute(void) {
    if (!cpusets_on()) {
        printf("Runtime: SOTC_GAME_AFFINITY: SOTC_CPUSETS is not 1, job workers not placed\n");
        return 0;
    }
    DWORD_PTR process=0, system=0;
    if (!GetProcessAffinityMask(GetCurrentProcess(),&process,&system) || !process) process=~(DWORD_PTR)0;
    ULONG length=0;
    GetSystemCpuSetInformation(NULL,0,&length,NULL,0);
    if (length==0) {
        printf("Runtime: SOTC_GAME_AFFINITY: no CPU set information, job workers not placed\n");
        return 0;
    }
    BYTE *buffer=malloc(length);
    if (!buffer) return 0;
    if (!GetSystemCpuSetInformation((SYSTEM_CPU_SET_INFORMATION*)buffer,length,&length,NULL,0)) {
        printf("Runtime: SOTC_GAME_AFFINITY: GetSystemCpuSetInformation failed (%lu), job workers not placed\n",(unsigned long)GetLastError());
        free(buffer);
        return 0;
    }
    /* Logical CPUs of group 0 that this process may run on. */
    GameCpu cpus[64];
    size_t n=0;
    for (BYTE *p=buffer; p<buffer+length;) {
        SYSTEM_CPU_SET_INFORMATION *info=(SYSTEM_CPU_SET_INFORMATION*)p;
        if (info->Size==0) break;
        if (info->Type==CpuSetInformation) {
            const BYTE lp=info->CpuSet.LogicalProcessorIndex;
            if (info->CpuSet.Group==0 && lp<64 && ((process>>lp)&1) && n<64) {
                cpus[n].core=info->CpuSet.CoreIndex;
                cpus[n].lp=lp;
                cpus[n].eff=info->CpuSet.EfficiencyClass;
                ++n;
            }
        }
        p+=info->Size;
    }
    free(buffer);
    /* Sort by (core, lp), insertion sort (at most 64 entries). */
    for (size_t i=1;i<n;++i) {
        GameCpu v=cpus[i];
        size_t j=i;
        while (j>0 && (cpus[j-1].core>v.core || (cpus[j-1].core==v.core && cpus[j-1].lp>v.lp))) {
            cpus[j]=cpus[j-1];
            --j;
        }
        cpus[j]=v;
    }
    /* One entry per physical core: its first (lowest) logical CPU. */
    GameCpu firsts[64];
    size_t nc=0;
    for (size_t i=0;i<n;++i) {
        if (nc==0 || firsts[nc-1].core!=cpus[i].core) firsts[nc++]=cpus[i];
    }
    if (nc<6) {
        printf("Runtime: SOTC_GAME_AFFINITY: %zu physical cores (need 6), job workers not placed\n",nc);
        return 0;
    }
    /* Fastest efficiency class and how many physical cores it holds (need 4). */
    BYTE fastest=0;
    for (size_t i=0;i<nc;++i) if (firsts[i].eff>fastest) fastest=firsts[i].eff;
    size_t fast=0;
    for (size_t i=0;i<nc;++i) if (firsts[i].eff==fastest) ++fast;
    if (fast<4) {
        printf("Runtime: SOTC_GAME_AFFINITY: %zu physical cores in the fastest class (need 4), job workers not placed\n",fast);
        return 0;
    }
    /* The port's four exclusive threads: fastest efficiency class first, then core index. */
    DWORD_PTR allowed=0, reserved=0;
    for (size_t i=0;i<n;++i) allowed|=(DWORD_PTR)1<<cpus[i].lp;
    unsigned char used[64]={0};
    for (int k=0;k<4;++k) {
        size_t best=(size_t)-1;
        for (size_t i=0;i<nc;++i) {
            if (!used[i] && (best==(size_t)-1 || firsts[i].eff>firsts[best].eff)) best=i;
        }
        used[best]=1;
        reserved|=(DWORD_PTR)1<<firsts[best].lp;
    }
    DWORD_PTR workers=allowed&~reserved;
    if (!workers) {
        printf("Runtime: SOTC_GAME_AFFINITY: no logical CPU left for job workers, not placed\n");
        return 0;
    }
    printf("Runtime: SOTC_GAME_AFFINITY: port threads reserve CPU mask 0x%llx, job workers get 0x%llx\n",
           (unsigned long long)reserved,(unsigned long long)workers);
    return workers;
}
static DWORD_PTR game_affinity_worker_mask(void) {
    pthread_mutex_lock(&game_affinity_lock);
    if (!game_affinity_ready) {
        game_affinity_mask_value=game_affinity_compute();
        game_affinity_ready=1;
    }
    DWORD_PTR mask=game_affinity_mask_value;
    pthread_mutex_unlock(&game_affinity_lock);
    return mask;
}
/* Called by the new thread itself. Only game job workers are placed. */
static void game_affinity_apply(const char *name) {
    if (!game_affinity_on() || !strstr(name,GAME_JOB_WORKER_NAME)) return;
    DWORD_PTR mask=game_affinity_worker_mask();
    if (!mask) return;
    DWORD_PTR previous=SetThreadAffinityMask(GetCurrentThread(),mask);
    if (!previous) {
        printf("Runtime: SOTC_GAME_AFFINITY: guest thread '%s' not placed (error %lu)\n",name,(unsigned long)GetLastError());
    } else {
        printf("Runtime: SOTC_GAME_AFFINITY: guest thread '%s' (host thread %lu) on CPU mask 0x%llx (was 0x%llx)\n",
               name,(unsigned long)GetCurrentThreadId(),(unsigned long long)mask,(unsigned long long)previous);
    }
}
#endif
static void *host_start(void *p) {
    GuestThread *t=p;
#ifdef _WIN32
    /* sotc: an OS handle for runtime_thread_suspend_others, published under `lock`; taking `lock`
     * also waits out a live view split in progress before any guest code runs. */
    pthread_mutex_lock(&lock);
    t->os_handle=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,
                            GetCurrentThreadId());
    pthread_mutex_unlock(&lock);
#endif
    attach(t);
    set_host_name(t->name);
#ifdef _WIN32
    game_affinity_apply(t->name); /* sotc: SOTC_GAME_AFFINITY=1 only (game job workers) */
#endif
    trace_thread_init(); /* sotc: SOTC_TRACE ring before any guest code (faults never allocate one) */
    if (bbgpu_register_guest_thread) bbgpu_register_guest_thread(); /* sotc: SOTC_STALL_DETAIL guest CPU */
#ifdef _WIN32
    /* sotc: host thread ids of guest threads, for traces that print GetCurrentThreadId. */
    if (getenv("SOTC_THREAD_IDS")) fprintf(stderr,"Runtime: guest thread '%s' is host thread %lu\n",t->name,GetCurrentThreadId());
#endif
    if (!exit_setjmp(t)) t->result=t->entry(t->argument);
    runtime_thread_keys_cleanup();
    pthread_mutex_lock(&lock);
    t->finished=1; ++exited;
#ifdef _WIN32
    if (t->os_handle) { CloseHandle(t->os_handle); t->os_handle=NULL; }
#endif
    pthread_mutex_unlock(&lock);
    return t->result;
}
static int32_t create(GuestThread **out,ThreadAttr **attr_slot,GuestEntry entry,void *argument,const char *name) {
    if (!out || !entry) return ERR(22);
    ThreadAttr *a=NULL;
    if (attr_slot && !(a=find_attr(attr_slot))) return ERR(22);
    GuestThread *t=new_thread();
    if (!t) return ERR(12);
    if (a) { t->attr=*a; t->attr.next=NULL; }
    t->entry=entry; t->argument=argument; t->detached=t->attr.detached;
    snprintf(t->name,sizeof(t->name),"%s",name ? name : "guest");
    pthread_attr_t host;
    pthread_attr_init(&host);
    uint64_t stack=t->attr.stack<MIN_STACK ? MIN_STACK : t->attr.stack;
    /* Stacks below 1 TiB as on PS4; guest code may pack stack addresses. */
    size_t stack_bytes=(size_t)stack+STACK_MARGIN;
#ifdef _WIN32
    /* Windows places the stack itself (low: no high-entropy ASLR) and commits all of it:
     * guest code has no stack probes and would step over the guard page. */
    pthread_attr_setstacksize(&host,stack_bytes);
#else
    void *stack_memory=runtime_low_map(stack_bytes,PROT_READ|PROT_WRITE);
    if (stack_memory) pthread_attr_setstack(&host,stack_memory,stack_bytes);
    else pthread_attr_setstacksize(&host,stack_bytes);
#endif
    publish(t);
    /* Publish the handle before the thread can run and inspect itself. */
    *out=t;
    int e=pthread_create(&t->host,&host,host_start,t);
    pthread_attr_destroy(&host);
    if (e) { fprintf(stderr,"STOP: host pthread_create failed: %d\n",e); exit(21); }
    if (t->detached) pthread_detach(t->host);
    pthread_mutex_lock(&lock); ++created; pthread_mutex_unlock(&lock);
    printf("Runtime: guest thread '%s' created (stack=%llu, prio=%d)\n",t->name,(unsigned long long)stack,t->attr.prio);
    return 0;
}
static ABI int32_t thread_create(GuestThread **out,ThreadAttr **attr,GuestEntry entry,void *argument,const char *name) {
    return create(out,attr,entry,argument,name);
}
static ABI int32_t thread_join(GuestThread *t,void **result) {
    if (!find_thread(t) || t->host_owned) return ERR(3);
    if (t==current) return ERR(11);
    if (t->detached || t->joined) return ERR(22);
    t->joined=1;
    void *value=NULL;
    int e=pthread_join(t->host,&value);
    if (e) { fprintf(stderr,"STOP: host pthread_join failed: %d\n",e); exit(21); }
    if (result) *result=t->result;
    pthread_mutex_lock(&lock); ++joined_count; pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t thread_detach(GuestThread *t) {
    if (!find_thread(t)) return ERR(3);
    if (t->detached) return ERR(22);
    t->detached=1;
    if (!t->host_owned) pthread_detach(t->host);
    return 0;
}
static ABI __attribute__((noreturn)) void thread_exit(void *value) {
    GuestThread *t=runtime_thread_current();
    if (t->host_owned) { fputs("STOP: pthread_exit on host-owned/main thread\n",stderr); exit(21); }
    t->result=value;
    /* No host unwinder: guest frames have no registered FDEs. */
    exit_longjmp(t);
}
static ABI int32_t thread_yield(void) { sched_yield(); return 0; }
static ABI int32_t thread_get_prio(GuestThread *t,int *prio) {
    if (!find_thread(t)) return ERR(3);
    if (!prio) return ERR(22);
    *prio=t->attr.prio; return 0;
}
static ABI int32_t thread_set_prio(GuestThread *t,int prio) {
    if (!find_thread(t)) return ERR(3);
    t->attr.prio=prio; return 0;
}
static ABI int32_t thread_set_affinity(GuestThread *t,uint64_t mask) {
    if (!find_thread(t)) return ERR(3);
    t->attr.affinity=mask; return 0;
}
static ABI int32_t thread_get_affinity(GuestThread *t,uint64_t *mask) {
    if (!find_thread(t)) return ERR(3);
    if (!mask) return ERR(22);
    *mask=t->attr.affinity; return 0;
}
static ABI int32_t thread_rename(GuestThread *t,const char *name) {
    if (!find_thread(t)) return ERR(3);
    if (!name) return ERR(22);
    snprintf(t->name,sizeof(t->name),"%s",name);
    if (t==current) set_host_name(t->name);
    return 0;
}
static ABI int32_t thread_equal(GuestThread *a,GuestThread *b) { return a==b; }

/* POSIX (libScePosix) variants return positive errno values. */
static int32_t posix(int32_t r) { return r ? (int32_t)((uint32_t)r&0xffff) : 0; }
static ABI int32_t posix_attr_init(ThreadAttr **a) { return posix(attr_init(a)); }
static ABI int32_t posix_attr_destroy(ThreadAttr **a) { return posix(attr_destroy(a)); }
static ABI int32_t posix_attr_set_detach(ThreadAttr **a,int s) { return posix(attr_set_detach(a,s)); }
static ABI int32_t posix_attr_set_stack(ThreadAttr **a,uint64_t s) { return posix(attr_set_stack(a,s)); }
static ABI int32_t posix_attr_set_param(ThreadAttr **a,const int *p) { return posix(attr_set_param(a,p)); }
static ABI int32_t posix_create(GuestThread **t,ThreadAttr **a,GuestEntry e,void *arg) { return posix(create(t,a,e,arg,"posix")); }
static ABI int32_t posix_create_name(GuestThread **t,ThreadAttr **a,GuestEntry e,void *arg,const char *name) { return posix(create(t,a,e,arg,name)); }
static ABI int32_t posix_join(GuestThread *t,void **r) { return posix(thread_join(t,r)); }

uintptr_t runtime_thread_resolve(const char *name) {
    static const struct { const char *nid; void *fn; } table[]={
        {"aI+OeCz8xrQ",thread_self}, {"EotR8a3ASf4",thread_self},
        {"9BcDykPmo1I",guest_error},
        {"nsYoNRywwNg",attr_init}, {"62KCwEMmzcM",attr_destroy},
        {"x1X76arYMxU",attr_get}, {"8+s5BzZjxSg",attr_affinity},
        {"UTXzJbWhhTE",attr_set_stack}, {"-Wreprtu0Qs",attr_set_detach},
        {"4+h9EzwKF4I",attr_set_policy}, {"DzES9hQF4f4",attr_set_param},
        {"3qxgM4ezETA",attr_set_affinity}, {"eXbUSpEaTsA",attr_set_inherit},
        {"JaRMy+QcpeU",attr_get_detach}, {"-fA+7ZlGDQs",attr_get_stack},
        {"Ru36fiTtJzA",attr_get_stackaddr}, {"7Xl257M4VNI",thread_equal},
        {"FXPWHNk8Of0",attr_get_param}, {"El+cQ20DynU",attr_set_guard},
        {"6UgtwV+0zb4",thread_create}, {"onNY9Byn-W8",thread_join},
        {"4qGrR6eoP9Y",thread_detach}, {"3kg7rT0NQIs",thread_exit},
        {"T72hz6ffq08",thread_yield}, {"1tKyG7RlMJo",thread_get_prio},
        {"W0Hpm2X0uPE",thread_set_prio}, {"bt3CTBKmGyI",thread_set_affinity}, {"rcrVFJsQWRY",thread_get_affinity},
        {"GBUY7ywdULE",thread_rename}, {"3PtV6p3QNX4",thread_equal},
        {"wtkt-teR1so",posix_attr_init}, {"zHchY8ft5pk",posix_attr_destroy},
        {"E+tyo3lp5Lw",posix_attr_set_detach}, {"2Q0z6rnBrTE",posix_attr_set_stack},
        {"euKRgm0Vn2M",posix_attr_set_param}, {"OxhIB8LB-PQ",posix_create},
        {"Jmi+9w9u0E4",posix_create_name}, {"h9CcP3J0oVM",posix_join},
        {"FJrT5LuUBAU",thread_exit},
    };
    for (size_t i=0;i<sizeof(table)/sizeof(*table);++i)
        if (!strcmp(name,table[i].nid)) return (uintptr_t)table[i].fn;
    return 0;
}
void runtime_thread_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: guest threads created=%zu, exited=%zu, joined=%zu\n",created,exited,joined_count);
    pthread_mutex_unlock(&lock);
}

