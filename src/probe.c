/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "runtime.h"
#include "gpu/bbgpu.h"
#include "gpu/sotc/sotc_patches.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>
#include <mmsystem.h>
#else
#include <sys/mman.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
int vulkan_smoke(void);

static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
#ifdef _WIN32
/* sotc: the PS4 maps the executable at 0x400000, and Shadow of the Colossus packs pointers
 * to its static data into 31 bits (bit 31 selects a heap-relative form). Windows already
 * uses 0x400000 before main, so, as shadPS4 does, the image goes to the first free range
 * above it, which must end below 2 GiB. It is claimed first thing in main, before the C
 * runtime, the drivers or anything else allocate there. */
#define EBOOT_MIN UINT64_C(0x400000)
#define EBOOT_LIMIT UINT64_C(0x80000000)
#define EBOOT_RESERVATION (UINT64_C(256) << 20)
static void *eboot_reservation;
static void reserve_eboot_range(void) {
    for (uintptr_t at=EBOOT_MIN; at+EBOOT_RESERVATION<=EBOOT_LIMIT;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)at,&m,sizeof(m))) return;
        uintptr_t start=((uintptr_t)m.BaseAddress+0xffff)&~(uintptr_t)0xffff, end=(uintptr_t)m.BaseAddress+m.RegionSize;
        if (m.State==MEM_FREE && end>start && end-start>=EBOOT_RESERVATION) {
            eboot_reservation=VirtualAlloc((void *)start,EBOOT_RESERVATION,MEM_RESERVE,PAGE_NOACCESS);
            if (eboot_reservation) return;
        }
        at=end;
    }
}
/* The image (eboot, linked modules, patch trampolines) below 2 GiB. */
static void *allocate_image(size_t size) {
    if (!eboot_reservation) fail("no free 256 MiB range below 2 GiB for the eboot");
    if (size > EBOOT_RESERVATION) fail("the image does not fit the eboot reservation");
    void *p=VirtualAlloc(eboot_reservation, size, MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("cannot commit the eboot range");
    printf("Image at %p (below 2 GiB, as the game's 31-bit pointers need); loader at %p\n",p,(void *)GetModuleHandleA(NULL));
    return p;
}
#endif
static void *allocate(size_t size) {
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) return low;
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
#endif
    return p;
}
static void protect(void *p, size_t size, unsigned flags) {
#ifdef _WIN32
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
#endif
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", runtime_import_name(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)image, argument);
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)image;
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,m==0 ? "libc.prx" : "system module",caller-modules[m].base);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
#ifndef _WIN32
/* enter_on_stack(entry, arg0, arg1, stack_top): call entry(arg0,arg1) on a new stack. */
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#endif
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
/* sotc: with the user's own libSceNgs2/libSceUlt linked (link_modules.py --sys-modules) their
 * native code replaces the runtime's silent audio stand-ins. */
static int prefer_native(const char *name, int bound) {
    if (!bound) return 0;
    const char *symbol=runtime_import_name(name);
    return !strncmp(symbol,"sceNgs2",7) || !strncmp(symbol,"sceUlt",6);
}
#ifdef _WIN32
static void describe_address(char *out, size_t size, uintptr_t address) {
    HMODULE module=NULL;
    char name[MAX_PATH]="?";
    if (image && address-(uintptr_t)image<0x20000000) { snprintf(out,size,"guest offset 0x%" PRIxPTR,address-(uintptr_t)image); return; }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)address,&module) && module) {
        GetModuleFileNameA(module,name,sizeof(name));
        const char *base=strrchr(name,'\\');
        snprintf(out,size,"%s+0x%" PRIxPTR,base ? base+1 : name,address-(uintptr_t)module);
        return;
    }
    snprintf(out,size,"%p",(void *)address);
}
/* sotc: BB_STACKS_TRIGGER=<file>: when the file appears it is removed and every other thread is
 * suspended in turn and reported (name, rip, frame-pointer chain) to stderr: hangs without a
 * debugger. Guest code keeps frame pointers. */
static int read_word(uintptr_t address, uintptr_t *value) {
    SIZE_T n=0;
    return ReadProcessMemory(GetCurrentProcess(),(const void *)address,value,sizeof(*value),&n) && n==sizeof(*value);
}
static void dump_thread_stacks(void) {
    typedef HRESULT (WINAPI *GetDescription)(HANDLE,PWSTR *);
    static GetDescription get_description;
    if (!get_description) get_description=(GetDescription)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),"GetThreadDescription");
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if (snapshot==INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry={.dwSize=sizeof(entry)};
    DWORD self=GetCurrentThreadId(), pid=GetCurrentProcessId();
    fprintf(stderr,"STACKS: begin\n");
    for (BOOL more=Thread32First(snapshot,&entry); more; more=Thread32Next(snapshot,&entry)) {
        if (entry.th32OwnerProcessID!=pid || entry.th32ThreadID==self) continue;
        HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ThreadID);
        if (!thread) continue;
        char name[96]="";
        PWSTR wide=NULL;
        if (get_description && SUCCEEDED(get_description(thread,&wide)) && wide) {
            WideCharToMultiByte(CP_UTF8,0,wide,-1,name,sizeof(name),NULL,NULL);
            LocalFree(wide);
        }
        if (SuspendThread(thread)!=(DWORD)-1) {
            CONTEXT c={.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER};
            if (GetThreadContext(thread,&c)) {
                char where[MAX_PATH+64];
                describe_address(where,sizeof(where),(uintptr_t)c.Rip);
                fprintf(stderr,"thread %lu '%s': rip %s rsp=%016llx\n",entry.th32ThreadID,name,where,c.Rsp);
                uintptr_t rbp=(uintptr_t)c.Rbp, frame[2];
                for (int depth=0;depth<24 && rbp;++depth) {
                    if (!read_word(rbp,&frame[0]) || !read_word(rbp+8,&frame[1]) || !frame[1]) break;
                    describe_address(where,sizeof(where),frame[1]);
                    fprintf(stderr,"    #%d %s\n",depth,where);
                    if (frame[0]<=rbp) break;
                    rbp=frame[0];
                }
                /* Guest and loader return addresses near the top of the stack (frames without
                 * rbp); loader ones as exe offsets for llvm-addr2line (ImageBase 0x140000000). */
                int found=0, host=0;
                const uintptr_t exe=(uintptr_t)GetModuleHandleA(NULL);
                for (uintptr_t at=(uintptr_t)c.Rsp; at<(uintptr_t)c.Rsp+0x3000 && found<14+host; at+=8) {
                    uintptr_t value;
                    if (!read_word(at,&value)) break;
                    if (image && value-(uintptr_t)image<0x4000000) {
                        fprintf(stderr,"    [rsp+0x%llx] guest 0x%llx\n",(unsigned long long)(at-c.Rsp),
                                (unsigned long long)(value-(uintptr_t)image));
                        ++found;
                    } else if (host<24 && value-exe>0x1000 && value-exe<0x4000000) {
                        fprintf(stderr,"    [rsp+0x%llx] exe 0x%llx\n",(unsigned long long)(at-c.Rsp),
                                (unsigned long long)(value-exe+0x140000000ull));
                        ++host;
                    }
                }
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    fprintf(stderr,"STACKS: end\n");
    fflush(stderr);
}
/* sotc: SOTC_PROFILE=<thread name part>: samples that thread every ~1 ms (SuspendThread +
 * GetThreadContext) from SOTC_PROFILE_DELAY (default 20) s for SOTC_PROFILE_SECONDS (default 15)
 * s, then prints the hottest instructions (leaf) and the hottest frames anywhere on the
 * frame-pointer chain (inclusive). exe offsets: llvm-symbolizer on out/gpu/bb-probe.exe. */
#define PROFILE_SLOTS 65536
typedef struct { uintptr_t address; unsigned leaf, inclusive; } ProfileSlot;
static ProfileSlot *profile_slots;
static void profile_add(uintptr_t address, int leaf) {
    size_t i=(address*0x9E3779B97F4A7C15ull)>>48;
    for (size_t n=0;n<PROFILE_SLOTS;++n,i=(i+1)&(PROFILE_SLOTS-1)) {
        ProfileSlot *s=&profile_slots[i];
        if (!s->address) s->address=address;
        if (s->address==address) { if (leaf) ++s->leaf; ++s->inclusive; return; }
    }
}
/* (system leaf, first own caller) pairs. */
typedef struct { uintptr_t leaf, caller; unsigned count; } ProfilePair;
static ProfilePair profile_pairs[4096];
static void profile_pair(uintptr_t exe, uintptr_t leaf, uintptr_t caller) {
    (void)exe;
    for (int i=0;i<4096;++i) {
        ProfilePair *p=&profile_pairs[i];
        if (!p->count) { p->leaf=leaf; p->caller=caller; }
        if (p->leaf==leaf && p->caller==caller) { ++p->count; return; }
    }
}
static int profile_by_pair(const void *a, const void *b) {
    return (int)((const ProfilePair *)b)->count-(int)((const ProfilePair *)a)->count;
}
static int profile_by_leaf(const void *a, const void *b) {
    return (int)((const ProfileSlot *)b)->leaf-(int)((const ProfileSlot *)a)->leaf;
}
static int profile_by_inclusive(const void *a, const void *b) {
    return (int)((const ProfileSlot *)b)->inclusive-(int)((const ProfileSlot *)a)->inclusive;
}
static DWORD WINAPI profiler(void *name) {
    typedef HRESULT (WINAPI *Describe)(HANDLE, PWSTR *);
    Describe describe=(Describe)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),"GetThreadDescription");
    const char *delay=getenv("SOTC_PROFILE_DELAY"), *seconds=getenv("SOTC_PROFILE_SECONDS");
    Sleep((delay ? atoi(delay) : 20)*1000);
    wchar_t wanted[128];
    MultiByteToWideChar(CP_UTF8,0,(const char *)name,-1,wanted,128);
    DWORD target=0;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    THREADENTRY32 te={.dwSize=sizeof(te)};
    for (BOOL ok=Thread32First(snapshot,&te); ok && !target; ok=Thread32Next(snapshot,&te)) {
        if (te.th32OwnerProcessID!=GetCurrentProcessId()) continue;
        HANDLE t=OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,te.th32ThreadID);
        PWSTR text=NULL;
        if (t && describe && SUCCEEDED(describe(t,&text)) && text) {
            if (wcsstr(text,wanted)) target=te.th32ThreadID;
            LocalFree(text);
        }
        if (t) CloseHandle(t);
    }
    CloseHandle(snapshot);
    if (!target) { fprintf(stderr,"SOTC profile: no thread named like %s\n",(const char *)name); return 0; }
    HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,target);
    profile_slots=calloc(PROFILE_SLOTS,sizeof(*profile_slots));
    if (!thread || !profile_slots) return 0;
    const ULONGLONG end=GetTickCount64()+(ULONGLONG)(seconds ? atoi(seconds) : 15)*1000;
    unsigned samples=0;
    timeBeginPeriod(1);
    while (GetTickCount64()<end) {
        if (SuspendThread(thread)!=(DWORD)-1) {
            CONTEXT c={.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER};
            if (GetThreadContext(thread,&c)) {
                ++samples;
                profile_add((uintptr_t)c.Rip,1);
                const uintptr_t leaf=(uintptr_t)c.Rip;
                int first_own=1;
                /* sotc: RtlVirtualUnwind on a snapshot taken mid-prologue can read a garbage
                 * address; that fault ended the whole game (ntdll, "not ours", unhandled). The
                 * handler's recovery point resumes here instead and the rest of the stack is
                 * dropped. depth is volatile: it is read after the jump. */
                sigjmp_buf unwind_recover;
                volatile int depth=0;
                if (sigsetjmp(unwind_recover,0)) depth=32;
                else runtime_fault_recover=&unwind_recover;
                /* Callers through the unwind tables (.pdata): system DLLs keep no frame pointers. */
                for (; depth<32; ++depth) {
                    DWORD64 base=0;
                    PRUNTIME_FUNCTION fn=RtlLookupFunctionEntry(c.Rip,&base,NULL);
                    if (fn) {
                        PVOID handler_data=NULL; DWORD64 establisher=0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,c.Rip,fn,&c,&handler_data,&establisher,NULL);
                    } else {
                        uintptr_t ret;
                        if (!read_word((uintptr_t)c.Rsp,&ret)) break;
                        c.Rip=ret; c.Rsp+=8;
                    }
                    if (!c.Rip) break;
                    profile_add((uintptr_t)c.Rip,0);
                    /* The first frame in the loader/GPU library that leads to a system leaf:
                     * who waits, locks or calls the driver. */
                    if (first_own && (uintptr_t)c.Rip-(uintptr_t)GetModuleHandleA(NULL)<0x4000000 &&
                        leaf-(uintptr_t)GetModuleHandleA(NULL)>=0x4000000) {
                        first_own=0;
                        profile_pair((uintptr_t)GetModuleHandleA(NULL),leaf,(uintptr_t)c.Rip);
                    }
                }
                runtime_fault_recover=NULL;
            }
            ResumeThread(thread);
        }
        Sleep(1);
    }
    timeEndPeriod(1);
    char where[MAX_PATH+64];
    fprintf(stderr,"SOTC profile of %s: %u samples\n",(const char *)name,samples);
    /* SOTC_PROFILE_OUT=<file>: every sampled address (module offset, leaf, inclusive), for
     * per-function totals offline (tools/profile_sym.py --functions). */
    if (getenv("SOTC_PROFILE_OUT")) {
        FILE *out=fopen(getenv("SOTC_PROFILE_OUT"),"w");
        for (size_t i=0; out && i<PROFILE_SLOTS; ++i) {
            if (!profile_slots[i].address) continue;
            describe_address(where,sizeof(where),profile_slots[i].address);
            fprintf(out,"%s %u %u\n",where,profile_slots[i].leaf,profile_slots[i].inclusive);
        }
        if (out) { fprintf(out,"samples %u\n",samples); fclose(out); }
    }
    qsort(profile_slots,PROFILE_SLOTS,sizeof(*profile_slots),profile_by_leaf);
    fprintf(stderr,"  leaf:\n");
    for (int i=0;i<40 && profile_slots[i].leaf;++i) {
        describe_address(where,sizeof(where),profile_slots[i].address);
        fprintf(stderr,"  %5.1f%% %s\n",100.0*profile_slots[i].leaf/samples,where);
    }
    qsort(profile_pairs,4096,sizeof(*profile_pairs),profile_by_pair);
    fprintf(stderr,"  system leaf <- first own caller:\n");
    for (int i=0;i<30 && profile_pairs[i].count;++i) {
        char caller[MAX_PATH+64];
        describe_address(where,sizeof(where),profile_pairs[i].leaf);
        describe_address(caller,sizeof(caller),profile_pairs[i].caller);
        fprintf(stderr,"  %5.1f%% %s <- %s\n",100.0*profile_pairs[i].count/samples,where,caller);
    }
    qsort(profile_slots,PROFILE_SLOTS,sizeof(*profile_slots),profile_by_inclusive);
    fprintf(stderr,"  inclusive:\n");
    for (int i=0;i<60 && profile_slots[i].inclusive;++i) {
        describe_address(where,sizeof(where),profile_slots[i].address);
        fprintf(stderr,"  %5.1f%% %s\n",100.0*profile_slots[i].inclusive/samples,where);
    }
    fflush(stderr);
    return 0;
}
static DWORD WINAPI stacks_watcher(void *path) {
    for (;;) {
        Sleep(500);
        if (!remove((const char *)path)) dump_thread_stacks();
    }
    return 0;
}
static int fatal_reported;
static LONG fatal_exception(EXCEPTION_POINTERS *info) {
    if (__atomic_exchange_n(&fatal_reported,1,__ATOMIC_ACQ_REL)) { Sleep(INFINITE); }
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    const CONTEXT *c=info->ContextRecord;
    char where[MAX_PATH+64];
    describe_address(where,sizeof(where),(uintptr_t)c->Rip);
    if (record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2)
        fprintf(stderr,"Fault: %s access to %p at %s (thread %lu)\n",
                record->ExceptionInformation[0]==1 ? "write" : record->ExceptionInformation[0]==8 ? "execute" : "read",
                (void *)record->ExceptionInformation[1],where,GetCurrentThreadId());
    else
        fprintf(stderr,"Fault: exception 0x%08lx at %s (thread %lu)\n",record->ExceptionCode,where,GetCurrentThreadId());
    fprintf(stderr,"  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n",
            c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->Rbp,c->Rsp);
    if (gpu_enabled) bbgpu_dump_guest_writes(info);
    /* rbp chain (guest and host code keep frame pointers). */
    uintptr_t rbp=(uintptr_t)c->Rbp;
    for (int depth=0; depth<24 && rbp; ++depth) {
        uintptr_t frame[2];
        SIZE_T got=0;
        if (!ReadProcessMemory(GetCurrentProcess(),(void *)rbp,frame,sizeof(frame),&got) || got!=sizeof(frame) || frame[0]<=rbp) break;
        describe_address(where,sizeof(where),frame[1]);
        fprintf(stderr,"  #%d %s\n",depth,where);
        rbp=frame[0];
    }
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(),3);
    return EXCEPTION_CONTINUE_SEARCH;
}
/* sotc: with precise readbacks GPU-written pages are read-protected. A loader or GPU helper
 * thread (draw preparation, recording, copies) reading one must not request a readback: the
 * command processor serving it waits for those very threads. The page is opened for the one
 * faulting instruction (trap flag) and its protection restored on the single-step trap. Those
 * threads need the CPU-side bytes only; GPU results reach the guest through its own faults. */
static _Thread_local uintptr_t step_page;
static _Thread_local DWORD step_protect;
static int helper_read_fault(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    if (record->ExceptionInformation[0]!=0 || step_page || runtime_thread_is_guest() ||
        bbgpu_is_gpu_side_thread() || bbgpu_readbacks_mode()!=2) return 0;
    const uintptr_t page=(uintptr_t)record->ExceptionInformation[1] & ~(uintptr_t)4095;
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery((void *)page,&m,sizeof(m)) || m.State!=MEM_COMMIT || m.Protect!=PAGE_NOACCESS) return 0;
    DWORD old;
    if (!VirtualProtect((void *)page,4096,PAGE_READONLY,&old)) return 0;
    step_page=page; step_protect=old;
    info->ContextRecord->EFlags|=0x100;
    return 1;
}
static int helper_single_step(EXCEPTION_POINTERS *info) {
    if (!step_page) return 0;
    MEMORY_BASIC_INFORMATION m;
    DWORD old;
    /* Only if nobody (the tracker) changed the page meanwhile. */
    if (VirtualQuery((void *)step_page,&m,sizeof(m)) && m.Protect==PAGE_READONLY)
        VirtualProtect((void *)step_page,4096,step_protect,&old);
    step_page=0;
    info->ContextRecord->EFlags&=~(DWORD)0x100;
    return 1;
}
/* sotc: the BPE heap of Shadow of the Colossus (EU 1.01). GPU writes of dispatches on stale
 * descriptors land in freed heap blocks and readbacks then overwrite the allocator's metadata
 * (access violations in eboot+0x48xxx/0x49xxx). The GPU side keeps that metadata
 * (video_core/buffer_cache/bpe_heap_guard.cpp, from Pink-shadPS4) but needs the heap object: a
 * breakpoint on the heap's free function (eboot+0x495e0) records its first argument for the
 * first calls, then is removed. SOTC_HEAP_GUARD=0 leaves it off. */
#define HEAP_FREE_OFFSET 0x495e0
const unsigned char *runtime_memory_backing_pointer(uintptr_t address);
#define HEAP_HOOK_CALLS 2000
static unsigned char heap_hook_original;
static volatile LONG heap_hook_calls, heap_hook_done;
static _Thread_local int heap_hook_rearm;
static struct { uintptr_t heap; LONG count; } heap_seen[8];
static volatile uintptr_t heap_chosen;
static void heap_hook_write(unsigned char value) {
    DWORD old;
    unsigned char *at=image+HEAP_FREE_OFFSET;
    if (VirtualProtect(at,1,PAGE_EXECUTE_READWRITE,&old)) { *at=value; VirtualProtect(at,1,old,&old); FlushInstructionCache(GetCurrentProcess(),at,1); }
}
static void install_heap_hook(void) {
    static const unsigned char expected[8]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56};
    const char *v=getenv("SOTC_HEAP_GUARD");
    if ((v && v[0]=='0') || memcmp(image+HEAP_FREE_OFFSET,expected,sizeof(expected))) return;
    heap_hook_original=image[HEAP_FREE_OFFSET];
    image[HEAP_FREE_OFFSET]=0xCC; /* the image is still writable here */
}
static void choose_heap(void) {
    uintptr_t best=0; LONG best_count=0;
    for (int i=0;i<8;++i) {
        uint64_t size=0;
        const unsigned char *backing=heap_seen[i].heap ? runtime_memory_backing_pointer(heap_seen[i].heap+0x30) : NULL;
        if (backing) memcpy(&size,backing,8);
        else if (heap_seen[i].heap-(uintptr_t)image<0x20000000) memcpy(&size,(const void *)(heap_seen[i].heap+0x30),8); /* a global of the eboot */
        if (heap_seen[i].heap && size>=0x1000000 && size<0x100000000ull && heap_seen[i].count>best_count) {
            best=heap_seen[i].heap; best_count=heap_seen[i].count;
        }
    }
    heap_chosen=best;
    fprintf(stderr,"Runtime: BPE heap guard on heap %p (%ld of %d frees)\n",(void *)best,best_count,HEAP_HOOK_CALLS);
    for (int i=0;i<8 && heap_seen[i].heap;++i) {
        uint64_t words[0x10]={0};
        const unsigned char *backing=runtime_memory_backing_pointer(heap_seen[i].heap);
        if (backing) memcpy(words,backing,sizeof(words));
        else if (heap_seen[i].heap-(uintptr_t)image<0x20000000) memcpy(words,(const void *)heap_seen[i].heap,sizeof(words));
        fprintf(stderr,"  heap %p: %ld frees; +0x28 %#llx +0x30 %#llx +0x38 %#llx +0x40 %#llx\n",(void *)heap_seen[i].heap,
                heap_seen[i].count,(unsigned long long)words[5],(unsigned long long)words[6],(unsigned long long)words[7],
                (unsigned long long)words[8]);
    }
}
uintptr_t runtime_sotc_heap(void) { return heap_chosen; }
static int heap_hook_trap(EXCEPTION_POINTERS *info) {
    CONTEXT *c=info->ContextRecord;
    if (info->ExceptionRecord->ExceptionCode==EXCEPTION_SINGLE_STEP) {
        if (!heap_hook_rearm) return 0;
        heap_hook_rearm=0;
        if (!heap_hook_done) heap_hook_write(0xCC);
        c->EFlags&=~(DWORD)0x100;
        return 1;
    }
    if (!image || c->Rip!=(DWORD64)(uintptr_t)(image+HEAP_FREE_OFFSET)) return 0;
    if (image[HEAP_FREE_OFFSET]!=0xCC) return 1; /* another thread restored it: just run it */
    const uintptr_t heap=(uintptr_t)c->Rdi;
    for (int i=0;i<8;++i) {
        if (heap_seen[i].heap==heap) { InterlockedIncrement(&heap_seen[i].count); break; }
        if (!heap_seen[i].heap && !InterlockedCompareExchangePointer((void *volatile *)&heap_seen[i].heap,(void *)heap,NULL)) {
            InterlockedIncrement(&heap_seen[i].count); break;
        }
    }
    heap_hook_write(heap_hook_original);
    if (InterlockedIncrement(&heap_hook_calls)>=HEAP_HOOK_CALLS) {
        if (!InterlockedExchange(&heap_hook_done,1)) choose_heap();
    } else {
        heap_hook_rearm=1;
        c->EFlags|=0x100; /* put the breakpoint back after this instruction */
    }
    return 1;
}
static LONG vectored_fault_impl(EXCEPTION_POINTERS *info);
/* sotc (from the shadPS4 SotC fork, signals.cpp): Shadow of the Colossus runs its jobs on fibers
 * with 16-64 KiB stacks packed back to back without guard pages. The exception handler runs on
 * whatever stack faulted, and the GPU page-tracking path needs far more: it overflowed into the
 * neighbouring stack or the process died without any report. A fault on a stack that is not the
 * thread's own runs the handler on a per-thread 512 KiB stack; the TEB bounds point at it
 * meanwhile so stack probes and nested faults see a consistent stack. */
LONG call_on_stack(EXCEPTION_POINTERS *info, LONG (*fn)(EXCEPTION_POINTERS *), void *top);
__asm__(".text\n.p2align 4\n.globl call_on_stack\ncall_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n mov %r8,%rsp\n sub $32,%rsp\n call *%rdx\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#define ALT_STACK_SIZE (512u * 1024u)
static _Thread_local unsigned char *alt_stack;
/* BB_FAULT_RING=<file>: every exception is recorded in a file-backed ring so the last ones
 * survive a process the kernel kills without dispatching (diagnostics). */
struct fault_entry { uint64_t tick, tid, code, addr, rip, rsp, region, below, outcome; }; /* region/below: TEB stack limit/base; outcome: handler path << 8 | 1 if execution continued */
static struct fault_entry *fault_ring;
static volatile LONG fault_ring_next;
static void fault_ring_open(const char *path) {
    HANDLE file=CreateFileA(path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,0,NULL);
    if (file==INVALID_HANDLE_VALUE) return;
    HANDLE map=CreateFileMappingA(file,NULL,PAGE_READWRITE,0,8192*sizeof(struct fault_entry),NULL);
    if (map) fault_ring=MapViewOfFile(map,FILE_MAP_WRITE,0,0,0);
}
static struct fault_entry *fault_ring_add(const EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    struct fault_entry *e=&fault_ring[(InterlockedIncrement(&fault_ring_next)-1)&8191];
    const uintptr_t rsp=(uintptr_t)info->ContextRecord->Rsp;
    const NT_TIB *tib=(const NT_TIB *)NtCurrentTeb();
    e->tick=GetTickCount64(); e->tid=GetCurrentThreadId(); e->code=record->ExceptionCode;
    e->addr=record->NumberParameters>=2 ? record->ExceptionInformation[1] : 0;
    e->rip=info->ContextRecord->Rip; e->rsp=rsp;
    e->region=(uintptr_t)tib->StackLimit; e->below=(uintptr_t)tib->StackBase; e->outcome=0;
    return e;
}
/* Which handler path resolved the current exception (fault ring diagnostics). */
static _Thread_local int fault_path;
static LONG CALLBACK vectored_fault(EXCEPTION_POINTERS *info) {
    NT_TIB *tib=(NT_TIB *)NtCurrentTeb();
    const uintptr_t sp=(uintptr_t)__builtin_frame_address(0);
    struct fault_entry *entry=fault_ring ? fault_ring_add(info) : NULL;
    fault_path=0;
    if (sp>=(uintptr_t)tib->StackLimit && sp<(uintptr_t)tib->StackBase) {
        const LONG own=vectored_fault_impl(info);
        if (entry) entry->outcome=((uint64_t)fault_path<<8)|(own==EXCEPTION_CONTINUE_EXECUTION);
        return own;
    }
    if (!alt_stack) alt_stack=VirtualAlloc(NULL,ALT_STACK_SIZE,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (!alt_stack) return vectored_fault_impl(info);
    void *const base=tib->StackBase, *const limit=tib->StackLimit;
    tib->StackBase=alt_stack+ALT_STACK_SIZE;
    tib->StackLimit=alt_stack;
    const LONG result=call_on_stack(info,vectored_fault_impl,alt_stack+ALT_STACK_SIZE);
    tib->StackBase=base;
    tib->StackLimit=limit;
    if (entry) entry->outcome=((uint64_t)fault_path<<8)|(result==EXCEPTION_CONTINUE_EXECUTION)|0x10000;
    return result;
}
static LONG vectored_fault_impl(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    DWORD code=record->ExceptionCode;
    if ((code==EXCEPTION_BREAKPOINT || code==EXCEPTION_SINGLE_STEP) && heap_hook_trap(info)) { fault_path=1; return EXCEPTION_CONTINUE_EXECUTION; }
    if (code==EXCEPTION_SINGLE_STEP && helper_single_step(info)) { fault_path=2; return EXCEPTION_CONTINUE_EXECUTION; }
    if (code==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2 && gpu_enabled && helper_read_fault(info)) {
        fault_path=3;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2) {
        /* GPU page tracking (write-protected guest pages) is resolved first. */
        if (gpu_enabled && bbgpu_handle_fault(info,(void *)record->ExceptionInformation[1])) { fault_path=4; return EXCEPTION_CONTINUE_EXECUTION; }
        fault_path=10; /* page tracking declined */
        if (runtime_fault_recover) {
            /* A speculative guest memory read (GPU draw preparation) faulted: the thread resumes
             * in bb_longjmp, outside the exception dispatcher, back at its recovery point. */
            unsigned long long *buffer=*runtime_fault_recover;
            runtime_fault_recover=NULL;
            CONTEXT *c=info->ContextRecord;
            c->Rsp=((c->Rsp-128)&~(DWORD64)15)-40;
            c->Rcx=(DWORD64)(uintptr_t)buffer;
            c->Rdx=1;
            c->Rip=(DWORD64)(uintptr_t)bb_longjmp;
            fault_path=5;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    /* sotc: an SSE4a site the load-time patchers could not rewrite is interpreted. */
    if (code==EXCEPTION_ILLEGAL_INSTRUCTION && image && sotc_handle_illegal(info)) { fault_path=6; return EXCEPTION_CONTINUE_EXECUTION; }
    if (code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_ILLEGAL_INSTRUCTION || code==EXCEPTION_PRIV_INSTRUCTION ||
        code==EXCEPTION_STACK_OVERFLOW || code==EXCEPTION_INT_DIVIDE_BY_ZERO || code==EXCEPTION_DATATYPE_MISALIGNMENT) {
        /* Faults in guest code, the loader or the runtime end the process with a report; drivers
         * and system libraries may handle their own exceptions. */
        uintptr_t rip=(uintptr_t)info->ContextRecord->Rip;
        HMODULE self=GetModuleHandleA(NULL), module=NULL;
        int ours=(image && rip-(uintptr_t)image<0x20000000) ||
                 (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)rip,&module) && module==self) ||
                 !module;
        if (ours) { fault_path=7; return fatal_exception(info); }
        fault_path=fault_path ? fault_path : 9; /* not ours: passed on */
    }
    /* BB_STRICT_HANDLES=1: a closed or invalid handle used anywhere is reported (diagnostics). */
    if (code==0xC0000008 /* STATUS_INVALID_HANDLE */) return fatal_exception(info);
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI unhandled_fault(EXCEPTION_POINTERS *info) { return fatal_exception(info); }
/* Guest code reads its TCB with `mov rax, gs:[0]` (link_*.py rewrote fs:[0]); on Windows
 * the displacement becomes the TEB TLS slot that holds the guest TCB (runtime_thread.c). */
uint32_t runtime_thread_tcb_offset(void);
static uint64_t patch_tcb_loads(const Segment *segments, uint64_t count) {
    static const unsigned char load[]={0x65,0x48,0x8b,0x04,0x25,0,0,0,0};
    uint32_t displacement=runtime_thread_tcb_offset();
    uint64_t patched=0;
    for (uint64_t s=0;s<count;++s) {
        if (!(segments[s].flags&1) || segments[s].size<sizeof(load)) continue;
        unsigned char *at=image+segments[s].address, *end=at+segments[s].size-sizeof(load);
        for (; at<=end; ++at)
            if (at[0]==0x65 && !memcmp(at,load,sizeof(load))) { memcpy(at+5,&displacement,4); ++patched; at+=sizeof(load)-1; }
    }
    return patched;
}
#endif
/* sotc: SSE4a and red-zone patches of the eboot (gpu/sotc), described by scripts/sotc_cpu.py. */
#define SOTC_TRAMPOLINE_SIZE (UINT64_C(16) << 20)
static void apply_sotc_cpu_patches(const char *path, const Segment *segments, uint64_t count, uint64_t size) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"SOTCCPU1",8)) fail("bad --sotc-cpu file (run scripts/sotc_cpu.py)");
    uint64_t eh=read64(f), eh_size=read64(f), functions=read64(f), offsets[16];
    if (functions>16) fail("too many red-zone functions");
    for (uint64_t i=0;i<functions;++i) offsets[i]=read64(f);
    fclose(f);
    uint64_t eboot_end=module_count ? modules[0].base : size;
    if (eh_size && (eh>=eboot_end || eh_size>eboot_end-eh)) fail("eh_frame_hdr outside the eboot");
    SotcCodeSegment code[16]; uint32_t n=0;
    for (uint64_t s=0;s<count && n<16;++s)
        if ((segments[s].flags&1) && segments[s].address<eboot_end)
            code[n++]=(SotcCodeSegment){segments[s].address,segments[s].size};
    SotcPatchRequest request={image,eboot_end,image+round_page(size),SOTC_TRAMPOLINE_SIZE,eh,eh_size,
                              code,n,offsets,(uint32_t)functions};
    SotcPatchResult result;
    if (sotc_patch_eboot(&request,&result)) fail("CPU patches: cannot decode the eboot function table");
    printf("CPU patches: SSE4a %s, %" PRIu64 " functions, %" PRIu64 " short SSE4a relocated, "
           "red zone: %" PRIu64 " functions / %" PRIu64 " instructions\n",
           result.needs_sse4a ? "emulated" : "native", result.function_starts, result.short_sse4a,
           result.red_zone_functions, result.red_zone_instructions);
}
/* sotc: extra engine command line (cvars such as +r_motionBlurEnable=0), described by
 * scripts/sotc_modes.py: SOTCCMD1, the image offset of the `lea rsi,[rip+x]` that loads the
 * string the game appends to its command line ("+VSyncWindow=10%"), then the new text. The text
 * is placed at the end of the trampoline area (loader memory right after the image, within the
 * lea's reach) instead of over strings of the game. */
static void apply_sotc_cmdline(const char *path, uint64_t size) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"SOTCCMD1",8)) fail("bad --sotc-cmdline file (run scripts/sotc_modes.py)");
    uint64_t lea=read64(f);
    char text[16384];
    size_t length=fread(text,1,sizeof(text)-1,f);
    fclose(f);
    text[length]=0;
    unsigned char *at=image+lea;
    if (lea+7>size || at[0]!=0x48 || at[1]!=0x8d || at[2]!=0x35) fail("--sotc-cmdline: no lea rsi at the given offset");
    unsigned char *place=image+round_page(size)+SOTC_TRAMPOLINE_SIZE-sizeof(text);
    memcpy(place,text,length+1);
    const int64_t disp=(int64_t)(place-(at+7));
    if (disp<INT32_MIN || disp>INT32_MAX) fail("--sotc-cmdline: text out of reach");
    const int32_t disp32=(int32_t)disp;
    memcpy(at+3,&disp32,4);
    printf("Engine command line: %s\n",text);
}
#ifndef _WIN32
static void fault(int sig, siginfo_t *info, void *context) {
    /* GPU page tracking (write-protected guest pages) is resolved first. */
    if (gpu_enabled && sig == SIGSEGV && bbgpu_handle_fault(context, info->si_addr)) return;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    ucontext_t *uc = context;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    char line[512];
    Dl_info where;
    if (rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), info->si_addr);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, info->si_addr);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    if (gpu_enabled) bbgpu_dump_guest_writes(context);
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
    _exit(128 + sig);
}
#endif
#ifndef _WIN32
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use process_vm_readv so bad frames cannot fault. */
static uintptr_t exe_base;
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip=(uintptr_t)uc->uc_mcontext.gregs[REG_RIP], rbp=(uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
    uint64_t tid=(uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,(uint64_t)uc->uc_mcontext.gregs[REG_RDI]);
    { ssize_t written_=write(2,arg,sizeof(arg)-1); (void)written_; }
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        { ssize_t written_=write(2,line,sizeof(line)-1); (void)written_; }
        uintptr_t frame[2];
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
static void thread_dump(int sig, siginfo_t *info, void *context) { (void)sig; (void)info; dump_frames(context); }
static void watchdog(int sig, siginfo_t *info, void *context) {
    (void)info;
    const char head[]="STOP: watchdog timeout; thread stacks:\n";
    { ssize_t written_=write(2,head,sizeof(head)-1); (void)written_; }
    dump_frames(context);
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self=gettid();
    while (dir>=0 && (n=syscall(SYS_getdents64,dir,buffer,sizeof(buffer)))>0)
        for (long at=0; at<n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d=(void *)(buffer+at);
            pid_t tid=(pid_t)strtol(d->name,NULL,10);
            if (tid>0 && tid!=self) { syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000); }
            at+=d->reclen;
        }
    usleep(100000);
    _exit(128 + sig);
}
#endif
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
/* BBPATCH2 (scripts/sotc_modes.py): the patches' image base, then byte writes at image offsets,
 * applied after relocation. A write may replace a whole base-relative pointer slot (a patch that
 * swaps function pointers): the patch holds the address at the patches' base, rebased here. */
static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"BBPATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0;
    unsigned char data[4096];
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !mapped(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data)/8]; size_t nslots=0;
        for (uint64_t r=0;r<nr;++r) {
            if (!(relocs[r].target<offset+length && offset<relocs[r].target+8)) continue;
            uint64_t target=relocs[r].target, value;
            if (relocs[r].kind || target<offset || target+8>offset+length) fail("patch overlaps a relocation");
            memcpy(&value,data+(target-offset),8);
            if (value<base || !mapped(segments,ns,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(image+offset,data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,image+slots[s],8);
            value=(uint64_t)(uintptr_t)image+(value-base);
            memcpy(image+slots[s],&value,8);
        }
        rebased+=nslots;
        bytes+=length;
    }
    if (fgetc(f)!=EOF) fail("trailing data in patch file");
    fclose(f);
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",count,bytes,rebased);
}
/* Restarts the game: BB_RESTART_COMMAND on Windows, run.sh on Linux. Descriptors are closed
 * first so the old GPU device and its memory are released before the new process opens its own. */
void runtime_restart(void) {
    fflush(NULL);
#ifdef _WIN32
    /* BB_RESTART_COMMAND: the launch command line, started again (sotc.py and the SotC launcher
     * do not set it). */
    const char *command=getenv("BB_RESTART_COMMAND");
    puts("Runtime: restarting (BB_RESTART_COMMAND)");
    if (!command || !*command) { fputs("runtime_restart: BB_RESTART_COMMAND is not set\n",stderr); _exit(1); }
    char *line=_strdup(command);
    /* Same output handles: a launcher reading the game's log keeps reading the new launch. */
    STARTUPINFOA startup={.cb=sizeof(startup),.dwFlags=STARTF_USESTDHANDLES,
                          .hStdInput=GetStdHandle(STD_INPUT_HANDLE),.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE),
                          .hStdError=GetStdHandle(STD_ERROR_HANDLE)};
    PROCESS_INFORMATION process;
    if (!line || !CreateProcessA(NULL,line,NULL,NULL,TRUE,0,NULL,NULL,&startup,&process)) {
        fprintf(stderr,"runtime_restart: CreateProcess failed (%lu)\n",GetLastError()); _exit(1);
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    /* The command must wait for this process's GPU device to go away before starting the game. */
    TerminateProcess(GetCurrentProcess(),0);
#else
    puts("Runtime: restarting through run.sh");
    syscall(SYS_close_range, 3u, ~0u, 0u);
    execlp("bash", "bash", "run.sh", (char *)NULL);
    perror("runtime_restart: exec");
    _exit(1);
#endif
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    reserve_eboot_range();
#endif
#ifndef _WIN32
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits. */
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
#endif
    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
#ifdef _WIN32
    /* The PS4 user range must stay free of host allocations (GPU driver, heaps), and the guest
     * TCB needs one of the 64 TEB TLS slots before DLLs (Vulkan drivers) take them. */
    runtime_memory_reserve();
    runtime_thread_tcb_offset();
#endif
    int cpu_only = 0, strict_imports = 0;
    unsigned timeout_seconds = 10;
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL, *patch_file=NULL, *sotc_cpu=NULL, *sotc_cmdline=NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--sotc-cpu") && i+1<argc) sotc_cpu=argv[++i];
        else if (!strcmp(argv[i], "--sotc-cmdline") && i+1<argc) sotc_cmdline=argv[++i];
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches") && i+1<argc) patch_file=argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--content-profile file] [--app0 dir] [--user dir] [--patches file] [--timeout seconds] | --vulkan-only\n", argv[0]);
        return 1;
    }
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"BBCONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        runtime_content_configure(values);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
    }
    bbgpu_register_kernel();
#ifdef _WIN32
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
    AddVectoredExceptionHandler(1, vectored_fault);
    if (getenv("BB_FAULT_RING")) fault_ring_open(getenv("BB_FAULT_RING"));
    if (getenv("BB_STACKS_TRIGGER")) CreateThread(NULL,0,stacks_watcher,(void *)getenv("BB_STACKS_TRIGGER"),0,NULL);
    if (getenv("SOTC_PROFILE")) CreateThread(NULL,0,profiler,(void *)getenv("SOTC_PROFILE"),0,NULL);
    SetUnhandledExceptionFilter(unhandled_fault);
    if (getenv("BB_STRICT_HANDLES")) {
        PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY strict={0};
        strict.RaiseExceptionOnInvalidHandleReference=1;
        strict.HandleExceptionsPermanentlyEnabled=1;
        if (!SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy,&strict,sizeof(strict)))
            fprintf(stderr,"BB_STRICT_HANDLES: policy not set (%lu)\n",GetLastError());
    }
    (void)timeout_seconds; /* no watchdog on Windows */
#else
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    alarm(timeout_seconds); /* 0 disables the watchdog */
#endif
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "BBPROBE1", 8) && memcmp(magic, "BBPROBE2", 8) && memcmp(magic,"BBPROBE3",8) && memcmp(magic,"BBPROBE4",8) && memcmp(magic,"BBPROBE5",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "BBPROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int multi=!memcmp(magic,"BBPROBE5",8);
    int linked=!memcmp(magic,"BBPROBE3",8) || !memcmp(magic,"BBPROBE4",8) || multi;
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        /* BBPROBE5: procparam, eboot TLS, module table, bindings (link_modules.py). */
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"BBPROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>7)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    /* sotc: the CPU patch trampolines follow the image (rel32 jumps reach them). */
#ifdef _WIN32
    image = allocate_image(round_page(size) + SOTC_TRAMPOLINE_SIZE);
#else
    image = allocate(round_page(size) + SOTC_TRAMPOLINE_SIZE);
#endif
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (!cpu_only) {
        char title[128]="Shadow of the Colossus", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        /* sotc: the window opens at the output resolution (SOTC_OUTPUT_RES=WxH). */
        int window_w=1920, window_h=1080;
        const char *output_res=getenv("SOTC_OUTPUT_RES");
        if (output_res && sscanf(output_res,"%dx%d",&window_w,&window_h)!=2) { window_w=1920; window_h=1080; }
#ifdef _WIN32
        /* A window never larger than 90% of the screen: the frame keeps the output size and is
         * scaled into it (fullscreen uses the whole screen anyway). */
        const int screen_w=GetSystemMetrics(SM_CXSCREEN), screen_h=GetSystemMetrics(SM_CYSCREEN);
        if (screen_w>0 && screen_h>0 && (window_w>screen_w*9/10 || window_h>screen_h*9/10)) {
            const double scale_w=screen_w*0.9/window_w, scale_h=screen_h*0.9/window_h;
            const double scale=scale_w<scale_h ? scale_w : scale_h;
            window_w=(int)(window_w*scale); window_h=(int)(window_h*scale);
        }
#endif
        BbGpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,window_w,window_h};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        if (bbgpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,bbgpu_symbol_count());
    }
    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        /* SysV: mov edi, index; movabs rax, handler; jmp rax. No fake return values. */
        uint32_t index = (uint32_t)i;
        uintptr_t handler = (uintptr_t)unresolved;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe; /* mov rsi,rdi: preserve arg0 */
        t[3] = 0xbf; memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &handler, 8);
        t[18] = 0xff; t[19] = 0xe0;
    }
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            uintptr_t resolved = prefer_native(names[relocs[i].value], native_libc && bindings[relocs[i].value])
                                 ? 0 : runtime_resolve(names[relocs[i].value], relocs[i].kind == 2);
            if (resolved) value = resolved + relocs[i].addend;
            else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address=bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value]!=relocs[i].kind || !mapped(segments,ns,address,relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)image+address+relocs[i].addend;
            }
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    /* sotc: BB_LIST_IMPORTS=1 prints how every import is served, then exits. */
    if (getenv("BB_LIST_IMPORTS")) {
        unsigned served[3]={0};
        for (uint64_t i=0;i<import_count;++i) {
            int data=0;
            for (uint64_t r=0;r<nr;++r) if (relocs[r].kind && relocs[r].value==i) { data=relocs[r].kind==2; break; }
            int how=!prefer_native(names[i],native_libc && bindings[i]) && runtime_resolve(names[i],data) ? 0 : native_libc && bindings[i] ? 1 : 2;
            ++served[how];
            printf("IMPORT %-7s %-28s %s\n",how==0 ? "runtime" : how==1 ? "native" : "MISSING",names[i],runtime_import_name(names[i]));
        }
        printf("IMPORTS: %u runtime, %u native, %u missing\n",served[0],served[1],served[2]);
        fflush(NULL);
        _exit(0);
    }
    if (patch_file) apply_patches(patch_file, segments, ns, relocs, nr);
#ifdef _WIN32
    printf("Guest TCB loads redirected to the TEB TLS slot: %" PRIu64 "\n", patch_tcb_loads(segments, ns));
#endif
    if (sotc_cpu) apply_sotc_cpu_patches(sotc_cpu, segments, ns, size);
    if (sotc_cmdline) apply_sotc_cmdline(sotc_cmdline, size);
    if (sotc_cpu) install_heap_hook();
    protect(traps, round_page((import_count + 1) * 32), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    protect(image + round_page(size), SOTC_TRAMPOLINE_SIZE, 5);
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image+procparam);
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            typedef int (ABI *ModuleInit)(uint64_t,void *,void *);
            int result=((ModuleInit)(image+modules[m].init))(0,NULL,NULL);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
    struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
#ifdef _WIN32
    typedef void (ABI *Entry)(void *, void (ABI *)(void));
    ((Entry)(image + entry))(&params, guest_exit);
#else
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks. */
    enum { MAIN_STACK=8*1024*1024 };
    unsigned char *stack=runtime_low_map(MAIN_STACK,PROT_READ|PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
    enter_on_stack(image+entry,&params,(void *)guest_exit,stack+MAIN_STACK-64);
#endif
    fail("entry unexpectedly returned");
}
