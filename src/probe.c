/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <cpuid.h>
#include "runtime.h"
#include "runtime_event.h"
#include "gpu/bbgpu.h"
#include "gpu/sotc/sotc_patches.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <dbghelp.h>
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
/* The PS4's Jaguar CPU has BMI1/LZCNT/MOVBE and the game uses them (ANDN, BEXTR, TZCNT, ...); older
   CPUs raise an illegal instruction or silently compute TZCNT/LZCNT wrong. Fail with a clear message. */
static void require_cpu_features(void) {
    unsigned a, b, c, d;
    int bmi1 = __get_cpuid_count(7, 0, &a, &b, &c, &d) && (b & 1u << 3);
    int lzcnt = __get_cpuid(0x80000001, &a, &b, &c, &d) && (c & 1u << 5);
    int movbe = __get_cpuid(1, &a, &b, &c, &d) && (c & 1u << 22);
    if (bmi1 && lzcnt && movbe) return;
    char msg[160];
    snprintf(msg, sizeof msg, "This CPU is not supported: it lacks%s%s%s (Intel Haswell / AMD Zen or newer is required).",
             bmi1 ? "" : " BMI1", lzcnt ? "" : " LZCNT", movbe ? "" : " MOVBE");
#ifdef _WIN32
    MessageBoxA(NULL, msg, "Shadow of the Colossus", MB_OK | MB_ICONERROR);
#endif
    fail(msg);
}
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
#ifdef _WIN32
/* sotc: SOTC_HEAP_WATCH=1 (#146): every 5 s, the engine's heaps (the table at image+0x2dfaab0,
 * indexed by heap id) with their 64 KiB page bitmaps (set bit = free page: +0x50 bitmap, +0x58
 * words, +0x60 pages; +0x30 bytes in use). The ending crashed when one bitmap had no free page
 * left (guest offset 0x1a371). Reads go through ReadProcessMemory, so a wrong guess is harmless. */
static unsigned char *heap_watch_image;
static int heap_read(uint64_t address, void *out, size_t size) {
    SIZE_T done=0;
    return ReadProcessMemory(GetCurrentProcess(),(const void *)(uintptr_t)address,out,size,&done) && done==size;
}
static DWORD WINAPI heap_watch(void *unused) {
    (void)unused;
    enum { HEAPS=48 };
    uint64_t last_free[HEAPS]={0};
    for (unsigned tick=0;;++tick) {
        Sleep(5000);
        uint64_t table[HEAPS];
        if (!heap_read((uint64_t)(uintptr_t)(heap_watch_image+0x2dfaab0),table,sizeof(table))) continue;
        if (tick%12==1) {  /* raw layout, to check the guessed offsets */
            char raw[4096]; int r=snprintf(raw,sizeof(raw),"SOTC HEAP TABLE");
            for (int i=0;i<HEAPS && r<(int)sizeof(raw)-24;++i) r+=snprintf(raw+r,sizeof(raw)-r," %d:%llx",i,(unsigned long long)table[i]);
            puts(raw);
            for (int i=0;i<HEAPS;++i) {
                uint64_t q[24];
                if (!table[i] || !heap_read(table[i],q,sizeof(q))) continue;
                r=snprintf(raw,sizeof(raw),"SOTC HEAP %d @%llx:",i,(unsigned long long)table[i]);
                for (int k=0;k<24;++k) r+=snprintf(raw+r,sizeof(raw)-r," %llx",(unsigned long long)q[k]);
                puts(raw);
            }
            fflush(stdout);
        }
        char line[2048]; int n=0, changed=0;
        for (int i=0;i<HEAPS;++i) {
            uint64_t bitmap=0, used=0, pages=0; uint32_t words=0;
            if (!table[i] || !heap_read(table[i]+0x50,&bitmap,8) || !heap_read(table[i]+0x58,&words,4) ||
                !heap_read(table[i]+0x60,&pages,8) || !heap_read(table[i]+0x30,&used,8)) continue;
            if (!bitmap || !words || words>(1u<<20) || pages>(uint64_t)words*64) continue;
            uint64_t free_pages=0, chunk[512];
            for (uint32_t w=0;w<words;w+=512) {
                uint32_t count=words-w<512 ? words-w : 512;
                if (!heap_read(bitmap+(uint64_t)w*8,chunk,count*8)) { free_pages=UINT64_MAX; break; }
                for (uint32_t k=0;k<count;++k) free_pages+=(uint64_t)__builtin_popcountll(chunk[k]);
            }
            if (free_pages==UINT64_MAX) continue;
            if (free_pages!=last_free[i]) changed=1;
            last_free[i]=free_pages;
            if (n<(int)sizeof(line)-80)
                n+=snprintf(line+n,sizeof(line)-n," h%d %llu/%llu MB free (in use %llu MB)",i,
                            (unsigned long long)(free_pages>>4),(unsigned long long)(pages>>4),
                            (unsigned long long)(used>>20));
        }
        if (n && (changed || tick%12==0)) { printf("SOTC HEAPS%s\n",line); fflush(stdout); }
    }
    return 0;
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
        /* sotc: only registers and raw stack words are read while the thread is suspended
         * (syscalls, no locks); describing and printing wait until it runs again. stdio, the heap
         * or the loader lock (GetModuleHandleEx) held by the suspended thread would otherwise
         * block this one for good, and the watchdog would freeze the game it reports on. */
        static uintptr_t chain[24], words[0x3000/8];
        int chain_count=0, word_count=0, captured=0;
        CONTEXT c={.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER};
        if (SuspendThread(thread)!=(DWORD)-1) {
            if (GetThreadContext(thread,&c)) {
                captured=1;
                uintptr_t rbp=(uintptr_t)c.Rbp, frame[2];
                while (chain_count<24 && rbp) {
                    if (!read_word(rbp,&frame[0]) || !read_word(rbp+8,&frame[1]) || !frame[1]) break;
                    chain[chain_count++]=frame[1];
                    if (frame[0]<=rbp) break;
                    rbp=frame[0];
                }
                while (word_count<(int)(sizeof(words)/sizeof(words[0])) &&
                       read_word((uintptr_t)c.Rsp+(uintptr_t)word_count*8,&words[word_count])) ++word_count;
            }
            ResumeThread(thread);
        }
        if (captured) {
            char where[MAX_PATH+64];
            describe_address(where,sizeof(where),(uintptr_t)c.Rip);
            fprintf(stderr,"thread %lu '%s': rip %s rsp=%016llx\n",entry.th32ThreadID,name,where,c.Rsp);
            for (int depth=0;depth<chain_count;++depth) {
                describe_address(where,sizeof(where),chain[depth]);
                fprintf(stderr,"    #%d %s\n",depth,where);
            }
            /* Guest and loader return addresses near the top of the stack (frames without
             * rbp); loader ones as exe offsets for llvm-addr2line (ImageBase 0x140000000). */
            int found=0, host=0;
            const uintptr_t exe=(uintptr_t)GetModuleHandleA(NULL);
            for (int k=0; k<word_count && found<14+host; ++k) {
                const uintptr_t value=words[k];
                if (image && value-(uintptr_t)image<0x4000000) {
                    fprintf(stderr,"    [rsp+0x%llx] guest 0x%llx\n",(unsigned long long)k*8,
                            (unsigned long long)(value-(uintptr_t)image));
                    ++found;
                } else if (host<24 && value-exe>0x1000 && value-exe<0x4000000) {
                    fprintf(stderr,"    [rsp+0x%llx] exe 0x%llx\n",(unsigned long long)k*8,
                            (unsigned long long)(value-exe+0x140000000ull));
                    ++host;
                }
            }
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    fprintf(stderr,"STACKS: end\n");
    fflush(stderr);
}
/* sotc: SOTC_PEEK=<image offset>[,<offset>...]: after SOTC_PEEK_DELAY s (default 40) prints
 * 0xd8 bytes at each offset as 32-bit words (reading engine globals such as cvars). */
static DWORD WINAPI peek_thread(void *list) {
    const char *d=getenv("SOTC_PEEK_DELAY");
    Sleep((DWORD)(d ? atoi(d) : 40)*1000u);
    char buffer[512];
    snprintf(buffer,sizeof(buffer),"%s",(const char *)list);
    for (char *item=strtok(buffer,","); item; item=strtok(NULL,",")) {
        const uintptr_t at=(uintptr_t)image+(uintptr_t)strtoull(item,NULL,16);
        uint32_t words[0xd8/4];
        SIZE_T n=0;
        if (!ReadProcessMemory(GetCurrentProcess(),(const void *)at,words,sizeof(words),&n) || n!=sizeof(words)) continue;
        fprintf(stderr,"SOTC PEEK %s:",item);
        for (size_t k=0;k<sizeof(words)/4;++k) fprintf(stderr," %x",words[k]);
        fprintf(stderr,"\n");
    }
    fflush(stderr);
    return 0;
}
/* sotc: the session's system in the event log: CPU, RAM, threads and whether the game's folder is
 * on a hard disk (seek penalty: new areas stall on scattered reads there) or an SSD. */
static void event_session_start(const char *app0) {
    unsigned regs[12]={0};
    char cpu[49]="?";
    unsigned max_ext=0;
    __cpuid(0x80000000,max_ext,regs[1],regs[2],regs[3]);
    if (max_ext>=0x80000004) {
        for (unsigned i=0;i<3;++i) __cpuid(0x80000002+i,regs[i*4],regs[i*4+1],regs[i*4+2],regs[i*4+3]);
        memcpy(cpu,regs,48); cpu[48]=0;
        char *p=cpu; while (*p==' ') ++p;
        memmove(cpu,p,strlen(p)+1);
    }
    MEMORYSTATUSEX mem={.dwLength=sizeof(mem)};
    GlobalMemoryStatusEx(&mem);
    const char *disk=SOTC_T("desconhecido","unknown");
    if (app0 && app0[0] && app0[1]==':') {
        char volume[16];
        snprintf(volume,sizeof(volume),"\\\\.\\%c:",app0[0]);
        HANDLE h=CreateFileA(volume,0,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
        if (h!=INVALID_HANDLE_VALUE) {
            STORAGE_PROPERTY_QUERY query={.PropertyId=StorageDeviceSeekPenaltyProperty,.QueryType=PropertyStandardQuery};
            DEVICE_SEEK_PENALTY_DESCRIPTOR seek={0};
            DWORD got=0;
            if (DeviceIoControl(h,IOCTL_STORAGE_QUERY_PROPERTY,&query,sizeof(query),&seek,sizeof(seek),&got,NULL) &&
                got>=sizeof(seek))
                disk=seek.IncursSeekPenalty ? SOTC_T("HD (disco mecânico: lugares novos podem travar)",
                                                     "HDD (spinning disk: new areas may stall)") : "SSD";
            CloseHandle(h);
        }
    }
    sotc_event("SESSION",SOTC_T("CPU: %s | %lu threads | RAM: %llu GB | pasta do jogo: %s (%s)",
                                "CPU: %s | %lu threads | RAM: %llu GB | game folder: %s (%s)"),
               cpu,(unsigned long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
               (unsigned long long)((mem.ullTotalPhys+(1ull<<29))>>30),app0 ? app0 : "?",disk);
}
/* sotc: read-ahead of the game's archives (SOTC_PREFETCH=0 turns it off). A new area makes the
 * game read many scattered pieces of pakN.psarc (12 GB); on a hard disk each waits for a seek and
 * the frame stalls (54 ms of 83 in "io" on the user's HDD, FPS down to 5). Once the game has
 * shown 20 s of frames the *.psarc files are read once, in order (~150 MB/s on a HDD), with a low
 * I/O priority hint so the game's own reads go first; Windows keeps them in its file cache (RAM), and later
 * reads of the game come from memory. Skipped when they are more than half the RAM. */
static char prefetch_dir[1024];
static DWORD WINAPI prefetch_thread(void *unused) {
    (void)unused;
    /* Not while the shader cache loads (thousands of small files, on the same disk as a rule: the
     * boot's loading screen took 2 minutes instead of 2 s with both; the game's logos already
     * flip then): once the preload is over and 20 s of game frames came after it. */
    while (!bbgpu_warmup_finished()) Sleep(500);
    const uint64_t after=bbgpu_flip_count()+1200;
    while (bbgpu_flip_count()<after) Sleep(1000);
    char pattern[1100];
    snprintf(pattern,sizeof(pattern),"%s\\*.psarc",prefetch_dir);
    WIN32_FIND_DATAA found;
    HANDLE find=FindFirstFileA(pattern,&found);
    if (find==INVALID_HANDLE_VALUE) return 0;
    char names[64][MAX_PATH]; unsigned long long sizes[64]; int count=0;
    unsigned long long total=0;
    do {
        if (count<64 && !(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            snprintf(names[count],MAX_PATH,"%s",found.cFileName);
            sizes[count]=((unsigned long long)found.nFileSizeHigh<<32)|found.nFileSizeLow;
            total+=sizes[count++];
        }
    } while (FindNextFileA(find,&found));
    FindClose(find);
    MEMORYSTATUSEX mem={.dwLength=sizeof(mem)};
    if (!GlobalMemoryStatusEx(&mem) || total>mem.ullTotalPhys/2) {
        fprintf(stderr,"SOTC prefetch: skipped (%llu MiB of archives, %llu MiB of RAM)\n",total>>20,
                (unsigned long long)(mem.ullTotalPhys>>20));
        return 0;
    }
    /* The small archives first (boot and common data), then the big one. */
    for (int i=0;i<count;++i) for (int j=i+1;j<count;++j) if (sizes[j]<sizes[i]) {
        unsigned long long s=sizes[i]; sizes[i]=sizes[j]; sizes[j]=s;
        char t[MAX_PATH]; memcpy(t,names[i],MAX_PATH); memcpy(names[i],names[j],MAX_PATH); memcpy(names[j],t,MAX_PATH);
    }
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_LOWEST);
    const DWORD chunk=4u<<20;
    void *buffer=VirtualAlloc(NULL,chunk,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (!buffer) return 0;
    const DWORD start=GetTickCount();
    unsigned long long done=0;
    for (int i=0;i<count;++i) {
        char path[1400];
        snprintf(path,sizeof(path),"%s\\%s",prefetch_dir,names[i]);
        HANDLE f=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,
                             OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
        if (f==INVALID_HANDLE_VALUE) continue;
        FILE_IO_PRIORITY_HINT_INFO hint={.PriorityHint=IoPriorityHintLow};
        SetFileInformationByHandle(f,FileIoPriorityHintInfo,&hint,sizeof(hint));
        DWORD n=0;
        while (ReadFile(f,buffer,chunk,&n,NULL) && n) done+=n;
        CloseHandle(f);
    }
    VirtualFree(buffer,0,MEM_RELEASE);
    sotc_event("DISK",SOTC_T("Arquivos do jogo lidos antecipadamente: %llu MiB em %.1f s (agora vêm da RAM)",
                             "Game archives read ahead: %llu MiB in %.1f s (now served from RAM)"),done>>20,
               (GetTickCount()-start)/1000.0);
    return 0;
}
/* sotc: hang watchdog (SOTC_WATCHDOG=0 turns it off). Now and then the game stops presenting for
 * good: no flip, PS4SyncEvent waits growing, the player has to close it (user, 20 Oct). It could
 * not be caught on purpose (8 attract runs without one), so after 15 s without a game frame the
 * thread stacks go to the log once per stall, like BB_STACKS_TRIGGER. */
static DWORD WINAPI hang_watchdog(void *unused) {
    (void)unused;
    uint64_t last=0;
    int still=0, dumped=0;
    for (;;) {
        Sleep(1000);
        const uint64_t flips=bbgpu_flip_count();
        if (flips!=last || flips<100) { last=flips; still=0; dumped=0; continue; }
        if (++still>=15 && !dumped) {
            dumped=1;
            sotc_event("ERROR",SOTC_T("Jogo parado: nenhum quadro há %d s (pilhas das threads no log da sessão)",
                                      "Game stalled: no frame for %d s (thread stacks in the session log)"),still);
            fprintf(stderr,"SOTC WATCHDOG: no game frame for %d s (flips %llu); thread stacks:\n",still,(unsigned long long)flips);
            dump_thread_stacks();
        }
    }
    return 0;
}
/* sotc: SOTC_PROFILE=<thread name part>: samples that thread every ~1 ms (SuspendThread +
 * GetThreadContext) from SOTC_PROFILE_DELAY (default 20) s for SOTC_PROFILE_SECONDS (default 15)
 * s, then prints the hottest instructions (leaf) and the hottest frames anywhere on the
 * frame-pointer chain (inclusive). exe offsets: llvm-symbolizer on out/gpu/bb-probe.exe.
 * SOTC_PROFILE_SLOW_MS=<ms>: only the samples of frames (flip to flip) longer than that count;
 * the samples of each frame wait until its flip decides (what a stutter frame spends time on). */
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
/* SOTC_PROFILE_SLOW_MS: the current frame's samples, kept until its flip. */
typedef struct { uintptr_t address, caller; int kind; } ProfilePending; /* 1 leaf, 0 caller, 2 pair */
#define PROFILE_PENDING 262144
static ProfilePending *profile_pending;
static size_t profile_pending_count;
static int profile_slow_ms;
static void profile_note(uintptr_t address, int leaf) {
    if (!profile_slow_ms) { profile_add(address,leaf); return; }
    if (profile_pending_count<PROFILE_PENDING)
        profile_pending[profile_pending_count++]=(ProfilePending){address,0,leaf};
}
static void profile_note_pair(uintptr_t exe, uintptr_t leaf, uintptr_t caller) {
    if (!profile_slow_ms) { profile_pair(exe,leaf,caller); return; }
    if (profile_pending_count<PROFILE_PENDING)
        profile_pending[profile_pending_count++]=(ProfilePending){leaf,caller,2};
}
static void profile_settle(int keep) {
    for (size_t i=0; keep && i<profile_pending_count; ++i) {
        const ProfilePending *p=&profile_pending[i];
        if (p->kind==2) profile_pair(0,p->address,p->caller);
        else profile_add(p->address,p->kind);
    }
    profile_pending_count=0;
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
/* sotc: SOTC_THREAD_TIMES=1: per-thread CPU time over the profile window (SOTC_PROFILE_DELAY to
 * +SOTC_PROFILE_SECONDS), with the flip count, to see which thread gains or loses busy time
 * between two flip modes without picking a thread up front. */
typedef struct { DWORD id; ULONGLONG cycles; char name[32]; } ThreadTimeRow;
static int thread_times_snapshot(ThreadTimeRow *rows, int max) {
    typedef HRESULT (WINAPI *Describe)(HANDLE, PWSTR *);
    static Describe describe;
    if (!describe) describe=(Describe)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),"GetThreadDescription");
    int n=0;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    THREADENTRY32 te={.dwSize=sizeof(te)};
    for (BOOL ok=Thread32First(snapshot,&te); ok && n<max; ok=Thread32Next(snapshot,&te)) {
        if (te.th32OwnerProcessID!=GetCurrentProcessId()) continue;
        HANDLE t=OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,te.th32ThreadID);
        if (!t) continue;
        ThreadTimeRow *r=&rows[n];
        r->id=te.th32ThreadID; r->name[0]=0;
        if (!QueryThreadCycleTime(t,&r->cycles)) r->cycles=0;
        PWSTR text=NULL;
        if (describe && SUCCEEDED(describe(t,&text)) && text) {
            WideCharToMultiByte(CP_UTF8,0,text,-1,r->name,sizeof(r->name),NULL,NULL);
            LocalFree(text);
        }
        CloseHandle(t); ++n;
    }
    CloseHandle(snapshot);
    return n;
}
static DWORD WINAPI thread_times(void *unused) {
    (void)unused;
    const char *delay=getenv("SOTC_PROFILE_DELAY"), *seconds=getenv("SOTC_PROFILE_SECONDS");
    Sleep((delay ? atoi(delay) : 20)*1000);
    static ThreadTimeRow before[256], after[256];
    const uint64_t flips0=bbgpu_flip_count();
    const int nb=thread_times_snapshot(before,256);
    const ULONGLONG t0=GetTickCount64();
    Sleep((seconds ? atoi(seconds) : 15)*1000);
    const int na=thread_times_snapshot(after,256);
    const double ms=(double)(GetTickCount64()-t0);
    const uint64_t frames=bbgpu_flip_count()-flips0;
    fprintf(stderr,"SOTC thread times: %.0f ms, %llu flips\n",ms,(unsigned long long)frames);
    for (int i=0;i<na;++i) {
        ULONGLONG start=0;
        for (int j=0;j<nb;++j) if (before[j].id==after[i].id) { start=before[j].cycles; break; }
        if (after[i].cycles<=start) continue;
        /* Cycles, not time; report both raw and per-flip so two runs compare by share. */
        const double mcycles=(double)(after[i].cycles-start)/1e6;
        if (mcycles<1.0) continue;
        fprintf(stderr,"  tid %6lu %-32s %10.1f Mcycles  %8.3f Mc/flip\n",
                (unsigned long)after[i].id,after[i].name[0]?after[i].name:"?",
                mcycles,frames?mcycles/(double)frames:0.0);
    }
    return 0;
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
    if (getenv("SOTC_PROFILE_SLOW_MS")) profile_slow_ms=atoi(getenv("SOTC_PROFILE_SLOW_MS"));
    if (profile_slow_ms) profile_pending=calloc(PROFILE_PENDING,sizeof(*profile_pending));
    if (!thread || !profile_slots || (profile_slow_ms && !profile_pending)) return 0;
    const ULONGLONG end=GetTickCount64()+(ULONGLONG)(seconds ? atoi(seconds) : 15)*1000;
    unsigned samples=0, frame_samples=0, slow_frames=0;
    uint64_t frame_flips=bbgpu_flip_count();
    ULONGLONG frame_start=GetTickCount64();
    timeBeginPeriod(1);
    while (GetTickCount64()<end) {
        if (profile_slow_ms && bbgpu_flip_count()!=frame_flips) {
            const ULONGLONG now=GetTickCount64();
            const int slow=now-frame_start>=(ULONGLONG)profile_slow_ms;
            profile_settle(slow);
            if (slow) { samples+=frame_samples; ++slow_frames; }
            frame_samples=0; frame_flips=bbgpu_flip_count(); frame_start=now;
        }
        if (SuspendThread(thread)!=(DWORD)-1) {
            CONTEXT c={.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER};
            if (GetThreadContext(thread,&c)) {
                if (profile_slow_ms) ++frame_samples; else ++samples;
                profile_note((uintptr_t)c.Rip,1);
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
                    profile_note((uintptr_t)c.Rip,0);
                    /* The first frame in the loader/GPU library that leads to a system leaf:
                     * who waits, locks or calls the driver. */
                    if (first_own && (uintptr_t)c.Rip-(uintptr_t)GetModuleHandleA(NULL)<0x4000000 &&
                        leaf-(uintptr_t)GetModuleHandleA(NULL)>=0x4000000) {
                        first_own=0;
                        profile_note_pair((uintptr_t)GetModuleHandleA(NULL),leaf,(uintptr_t)c.Rip);
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
    if (profile_slow_ms) fprintf(stderr,"SOTC profile: %u frames over %d ms\n",slow_frames,profile_slow_ms);
    if (!samples) return 0;
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
/* sotc: a fatal fault also writes a minidump (threads, stacks and registers, no guest memory) to
 * <user>/crashes/, where the launcher keeps the session log of a crash. dbghelp is loaded at
 * start-up, not in the handler. SOTC_MINIDUMP=0 turns it off. */
typedef BOOL (WINAPI *MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                           PMINIDUMP_EXCEPTION_INFORMATION,
                                           PMINIDUMP_USER_STREAM_INFORMATION,
                                           PMINIDUMP_CALLBACK_INFORMATION);
static MiniDumpWriteDumpFn minidump_write;
static char crash_dump_dir[MAX_PATH];
static void minidump_prepare(const char *user_dir) {
    const char *v=getenv("SOTC_MINIDUMP");
    if (v && v[0]=='0') return;
    HMODULE dbghelp=LoadLibraryA("dbghelp.dll");
    if (dbghelp) minidump_write=(MiniDumpWriteDumpFn)(void *)GetProcAddress(dbghelp,"MiniDumpWriteDump");
    snprintf(crash_dump_dir,sizeof(crash_dump_dir),"%s/crashes",user_dir);
}
static void minidump_write_now(EXCEPTION_POINTERS *info) {
    if (!minidump_write || !crash_dump_dir[0]) return;
    CreateDirectoryA(crash_dump_dir,NULL);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char path[MAX_PATH+64];
    snprintf(path,sizeof(path),"%s/%04u-%02u-%02u_%02u-%02u-%02u_bb-probe.dmp",crash_dump_dir,
             t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
    HANDLE file=CreateFileA(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if (file==INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION exception={GetCurrentThreadId(),info,FALSE};
    const BOOL ok=minidump_write(GetCurrentProcess(),GetCurrentProcessId(),file,
                                 (MINIDUMP_TYPE)(MiniDumpWithThreadInfo|MiniDumpWithUnloadedModules),
                                 &exception,NULL,NULL);
    CloseHandle(file);
    fprintf(stderr,ok ? "Crash dump: %s\n" : "Crash dump failed: %s\n",path);
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
    {
        /* sotc: the event log names the known faults (runtime_event.h). */
        const uintptr_t offset=(uintptr_t)c->Rip-(uintptr_t)image;
        const char *meaning=NULL;
        if (image && offset==0x1a371)
            meaning=SOTC_T("memória de páginas de textura do jogo esgotada (texturas Máxima ou vistas muito amplas)",
                           "the game's texture page pool ran out (Maximum textures or very wide views)");
        else if (image && (offset==0x49500 || offset==0x12c2b))
            meaning=SOTC_T("lista de memória livre de um heap do jogo corrompida (escrita da GPU sobre memória reaproveitada)",
                           "a game heap's free list was corrupted (a GPU write over reused memory)");
        else if (image && offset==0xa5d064)
            meaning=SOTC_T("o jogo não achou um arquivo dele ao iniciar (abra de novo)",
                           "the game did not find one of its files at start (start it again)");
        sotc_event("ERROR",SOTC_T("Queda em %s%s%s (código 0x%08lx)","Crash at %s%s%s (code 0x%08lx)"),where,
                   meaning ? ": " : "",meaning ? meaning : "",record->ExceptionCode);
    }
    fprintf(stderr,"  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n",
            c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->Rbp,c->Rsp);
    fprintf(stderr,"  r8=%016llx r9=%016llx r10=%016llx r11=%016llx\n  r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n",
            c->R8,c->R9,c->R10,c->R11,c->R12,c->R13,c->R14,c->R15);
    if (gpu_enabled) bbgpu_dump_guest_writes(info);
    /* rbp chain (guest and host code keep frame pointers). */
    uintptr_t rbp=(uintptr_t)c->Rbp;
    for (int depth=0; depth<24 && rbp; ++depth) {
        uintptr_t frame[2];
        SIZE_T got=0;
        if (!ReadProcessMemory(GetCurrentProcess(),(void *)rbp,frame,sizeof(frame),&got) || got!=sizeof(frame) || frame[0]<=rbp) break;
        describe_address(where,sizeof(where),frame[1]);
        fprintf(stderr,"  #%d %s\n",depth,where);
        /* The frame's locals and saved registers (rbp-0x80..rbp), for the arguments. */
        uintptr_t locals[16];
        if (depth<8 && ReadProcessMemory(GetCurrentProcess(),(void *)(rbp-0x80),locals,sizeof(locals),&got) && got==sizeof(locals)) {
            fprintf(stderr,"     rbp-80:");
            for (int k=0;k<16;++k) fprintf(stderr," %llx",(unsigned long long)locals[k]);
            fprintf(stderr,"\n");
        }
        rbp=frame[0];
    }
    fflush(NULL);
    minidump_write_now(info);
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
/* sotc: the BPE heap of Shadow of the Colossus (EU 1.01 / US 1.00, the same eboot). GPU writes
 * of dispatches on stale descriptors land in freed heap blocks and readbacks then overwrite the
 * allocator's metadata (access violations in eboot+0x48xxx/0x49xxx). The GPU side keeps that
 * metadata (video_core/buffer_cache/bpe_heap_guard.cpp, from Pink-shadPS4) but needs the heap
 * object: a breakpoint on the heap's free function (eboot+0x495e0) records its first argument for
 * the first calls, then is removed. SOTC_HEAP_GUARD=0 leaves it off. */
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
/* sotc (#146): the engine's heap table (heap id -> heap object, eboot EU 1.01 / US 1.00). The world's own
 * heaps (created when a world loads, e.g. the last colossus' area) include more heaps of the
 * guarded heap's class; the GPU side guards those too. 0 until the guarded heap is chosen. */
#define HEAP_TABLE_OFFSET 0x2dfaab0
uintptr_t runtime_sotc_heap_table(void) { return heap_chosen ? (uintptr_t)(image+HEAP_TABLE_OFFSET) : 0; }
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
#ifdef _WIN32
/* The engine times itself with inline rdtsc ((now - start) / frequency, unsigned). Some
 * systems do not keep the cores' TSCs in step (a Ryzen 9 5900X under Hyper-V: each core about
 * 1.4 ms behind the previous one, core 0 three seconds ahead), so a thread that changes core
 * sees time run backwards: timers wrap to hundreds of hours, a keyframe lookup indexes before
 * its array (fault at 0x28b133 on the title screen) or a wait never ends. There every rdtsc of
 * the eboot and sceKernelReadTsc read QPC instead. SOTC_TSC=qpc forces that, =native skips the
 * check. */
static void *tsc_clock;
static void choose_tsc(void) {
    const char *mode=getenv("SOTC_TSC");
    if (mode && !strcmp(mode,"native")) return;
    if (!mode || strcmp(mode,"qpc")) {
        unsigned core; double offset_us;
        int skew=compat_tsc_skew(&core,&offset_us);
        if (skew<0) { puts("Runtime: TSC check across cores failed; using the TSC"); return; }
        if (!skew) return;
        printf("Runtime: the CPU cores disagree on the TSC (core %u is %+.3f ms from core 0); "
               "the game's clock reads QPC instead (SOTC_TSC=native keeps the TSC)\n",core,offset_us/1000);
    } else puts("Runtime: SOTC_TSC=qpc: the game's clock reads QPC instead of the TSC");
    tsc_clock=(void *)compat_qpc_tsc;
    runtime_tsc_use_qpc();
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
                              code,n,offsets,(uint32_t)functions,NULL};
#ifdef _WIN32
    request.tsc_clock=tsc_clock;
#endif
    SotcPatchResult result;
    if (sotc_patch_eboot(&request,&result)) fail("CPU patches: cannot decode the eboot function table");
    printf("CPU patches: SSE4a %s, %" PRIu64 " functions, %" PRIu64 " short SSE4a relocated, "
           "red zone: %" PRIu64 " functions / %" PRIu64 " instructions\n",
           result.needs_sse4a ? "emulated" : "native", result.function_starts, result.short_sse4a,
           result.red_zone_functions, result.red_zone_instructions);
    if (request.tsc_clock)
        printf("CPU patches: %" PRIu64 " rdtsc read QPC, %" PRIu64 " left on the TSC\n",
               result.rdtsc, result.rdtsc_unpatched);
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
/* BBPATCH2 (patches.py): the patches' image base, then byte writes at image offsets, applied
 * after relocation. A write may replace a whole base-relative pointer slot (60/90 FPS++ swap
 * function pointers): the patch holds the address at the patches' base, rebased here. */
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
/* Restarts the game through run.sh (the settings menu: a new render resolution is a patch
 * applied at start). Descriptors are closed first so the old GPU device and its memory are
 * released before the new process opens its own. */
void runtime_restart(void) {
    fflush(NULL);
#ifdef _WIN32
    /* run.py sets BB_RESTART_COMMAND: the launch command line, started again (new patches). */
    const char *command=getenv("BB_RESTART_COMMAND");
    puts("Runtime: restarting through run.py");
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
    /* The new launch waits for this process's GPU device to go away (run.py --after). */
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
    require_cpu_features();
#ifdef _WIN32
    choose_tsc();
#endif
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
#ifdef _WIN32
    minidump_prepare(user_dir ? user_dir : "user");
#endif
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
#ifdef _WIN32
        event_session_start(app0);
#endif
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
        char title_id[16]="";
        if (sfo_value(sfo,"TITLE_ID",title_id,sizeof(title_id),NULL)) runtime_content_title(title_id);
    }
    bbgpu_register_kernel();
#ifdef _WIN32
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
    AddVectoredExceptionHandler(1, vectored_fault);
    if (getenv("BB_FAULT_RING")) fault_ring_open(getenv("BB_FAULT_RING"));
    if (getenv("BB_STACKS_TRIGGER")) CreateThread(NULL,0,stacks_watcher,(void *)getenv("BB_STACKS_TRIGGER"),0,NULL);
    if (getenv("SOTC_PROFILE")) CreateThread(NULL,0,profiler,(void *)getenv("SOTC_PROFILE"),0,NULL);
    if (getenv("SOTC_THREAD_TIMES")) CreateThread(NULL,0,thread_times,NULL,0,NULL);
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
        char title[128]="Bloodborne", serial[16]="UNKNOWN", sfo[4096];
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
    {   /* sotc: fps60game applied (EU 1.01 / US 1.00, scripts/sotc_modes.py): F7 sets the engine's own frame
         * rate target, the TargetFramesPerSeconds cvar (object 0x2eed7a0, value pointer at +0x60). */
        static const unsigned char fps60game[]={0xc7,0x45,0x84,0x01,0x00,0x00,0x00};
        if (size>0x2eed800 && !memcmp(image+0x49a47f,fps60game,sizeof fps60game))
            bbgpu_set_fps_target_slot(image+0x2eed7a0+0x60);
    }
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
#ifdef _WIN32
    if (gpu_enabled && !(getenv("SOTC_WATCHDOG") && getenv("SOTC_WATCHDOG")[0]=='0'))
        CloseHandle(CreateThread(NULL,0,hang_watchdog,NULL,0,NULL));
    if (getenv("SOTC_PEEK")) CloseHandle(CreateThread(NULL,0,peek_thread,(void *)getenv("SOTC_PEEK"),0,NULL));
    if (app0 && !(getenv("SOTC_PREFETCH") && getenv("SOTC_PREFETCH")[0]=='0')) {
        snprintf(prefetch_dir,sizeof(prefetch_dir),"%s",app0);
        CloseHandle(CreateThread(NULL,0,prefetch_thread,NULL,0,NULL));
    }
    if (getenv("SOTC_HEAP_WATCH") && getenv("SOTC_HEAP_WATCH")[0]=='1') {
        heap_watch_image=image;
        CloseHandle(CreateThread(NULL,0,heap_watch,NULL,0,NULL));
    }
#endif
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
