/* libSceFiber for Shadow of the Colossus: the game's loader and job system run on fibers.
 * Port of shadPS4's HLE (src/core/libraries/fiber, GPL-2.0-or-later). A fiber switch only
 * moves the guest's callee-saved registers and stack pointer; the host thread stays the same,
 * so the current fiber context lives in host TLS instead of the PS4 TCB. */
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIBER_ERROR_NULL ((int32_t)UINT32_C(0x80590001))
#define FIBER_ERROR_ALIGNMENT ((int32_t)UINT32_C(0x80590002))
#define FIBER_ERROR_RANGE ((int32_t)UINT32_C(0x80590003))
#define FIBER_ERROR_INVALID ((int32_t)UINT32_C(0x80590004))
#define FIBER_ERROR_PERMISSION ((int32_t)UINT32_C(0x80590005))
#define FIBER_ERROR_STATE ((int32_t)UINT32_C(0x80590006))
#define CONTEXT_MINIMUM_SIZE 512
#define MAX_NAME 31

enum { STATE_RUN=1, STATE_IDLE=2, STATE_TERMINATED=3 };
enum { FLAG_CONTEXT_SIZE_CHECK=0x10, FLAG_SET_FPU_REGS=0x100 };
static const uint32_t SIGNATURE0=0xdef1649c, SIGNATURE1=0xb37592a0, OPT_SIGNATURE=0xbb40e64d;
static const uint64_t STACK_SIGNATURE=UINT64_C(0x7149f2ca7149f2ca), STACK_SIZE_CHECK=UINT64_C(0xdeadbeefdeadbeef);

typedef void (ABI *FiberEntry)(uint64_t arg_on_initialize, uint64_t arg_on_run);
struct Fiber;
/* Host-private; offsets are used by the assembly below. */
typedef struct FiberContext {
    uint64_t rax, rcx, rdx, rbx, rsp, rbp, r8, r9, r10, r11, r12, r13, r14, r15; /* 0x00..0x68 */
    uint16_t fpucw; uint16_t pad; uint32_t mxcsr;                                /* 0x70, 0x74 */
    struct Fiber *current_fiber, *prev_fiber;
    uint64_t arg_on_run_to, arg_on_return, return_val;
} FiberContext;
_Static_assert(__builtin_offsetof(FiberContext,fpucw)==0x70 && __builtin_offsetof(FiberContext,mxcsr)==0x74,"context layout");
typedef struct {
    FiberEntry entry;            /* 0x00 */
    uint64_t arg_on_initialize;  /* 0x08 */
    uint64_t arg_on_run_to;      /* 0x10 */
    void *stack_addr;            /* 0x18 */
    _Atomic uint32_t *state;     /* 0x20 */
    uint16_t fpucw; uint16_t pad; /* 0x28 */
    uint32_t mxcsr;              /* 0x2c */
} FiberData;
_Static_assert(__builtin_offsetof(FiberData,state)==0x20 && __builtin_offsetof(FiberData,mxcsr)==0x2c,"data layout");
/* Guest-allocated SceFiber (256 bytes); same layout as shadPS4. */
typedef struct Fiber {
    uint32_t magic_start;
    _Atomic uint32_t state;
    FiberEntry entry;
    uint64_t arg_on_initialize;
    void *addr_context;
    uint64_t size_context;
    char name[MAX_NAME+1];
    FiberContext *context;
    uint32_t flags;
    void *context_start, *context_end;
    uint32_t magic_end;
} Fiber;
_Static_assert(sizeof(Fiber)<=256,"SceFiber is 256 bytes");
typedef struct {
    uint64_t size;
    FiberEntry entry;
    uint64_t arg_on_initialize;
    void *addr_context;
    uint64_t size_context;
    char name[MAX_NAME+1];
    uint64_t size_context_margin;
    uint8_t pad[48];
} FiberInfo;
_Static_assert(sizeof(FiberInfo)==128,"SceFiberInfo is 128 bytes");
typedef struct { uint32_t magic; } FiberOptParam;

static _Thread_local FiberContext *thread_fiber_slot;
/* A fiber resumed by a long jump may be running on another host thread than the one that
 * saved it (job workers pick up each other's fibers). The compiler caches the TLS block
 * address within a function, so every access goes through these opaque calls. */
static __attribute__((noinline)) FiberContext *thread_fiber_get(void) { __asm__ volatile(""); return thread_fiber_slot; }
static __attribute__((noinline)) void thread_fiber_set(FiberContext *ctx) { __asm__ volatile(""); thread_fiber_slot=ctx; }
static _Atomic unsigned fibers_created, fibers_run, fiber_switches;

ABI __attribute__((returns_twice)) int32_t fiber_setjmp(FiberContext *ctx) __asm__("sotc_fiber_setjmp");
ABI __attribute__((noreturn)) void fiber_longjmp(FiberContext *ctx) __asm__("sotc_fiber_longjmp");
ABI __attribute__((noreturn)) void fiber_switch_entry(FiberData *data, int set_fpu) __asm__("sotc_fiber_switch_entry");
ABI __attribute__((noreturn, used)) void fiber_force_quit(uint64_t value) __asm__("sotc_fiber_force_quit");

__asm__(
    ".text\n"
    ".globl sotc_fiber_setjmp\n"
    "sotc_fiber_setjmp:\n"
    "  movq %rax, 0x00(%rdi)\n"
    "  movq (%rsp), %rdx\n"        /* return address, restored by the long jump */
    "  movq %rdx, 0x10(%rdi)\n"
    "  movq %rcx, 0x08(%rdi)\n"
    "  movq %rbx, 0x18(%rdi)\n"
    "  movq %rsp, 0x20(%rdi)\n"
    "  movq %rbp, 0x28(%rdi)\n"
    "  movq %r8,  0x30(%rdi)\n"
    "  movq %r9,  0x38(%rdi)\n"
    "  movq %r10, 0x40(%rdi)\n"
    "  movq %r11, 0x48(%rdi)\n"
    "  movq %r12, 0x50(%rdi)\n"
    "  movq %r13, 0x58(%rdi)\n"
    "  movq %r14, 0x60(%rdi)\n"
    "  movq %r15, 0x68(%rdi)\n"
    "  fnstcw  0x70(%rdi)\n"
    "  stmxcsr 0x74(%rdi)\n"
    "  xorl %eax, %eax\n"
    "  ret\n"
    ".globl sotc_fiber_longjmp\n"
    "sotc_fiber_longjmp:\n"
    /* MXCSR = (current & 0x3f status flags) | (saved control bits) */
    "  stmxcsr -0x4(%rsp)\n"
    "  movl 0x74(%rdi), %eax\n"
    "  andl $0xffffffc0, %eax\n"
    "  movl -0x4(%rsp), %ecx\n"
    "  andl $0x3f, %ecx\n"
    "  xorl %eax, %ecx\n"
    "  movl %ecx, -0x4(%rsp)\n"
    "  ldmxcsr -0x4(%rsp)\n"
    "  movq 0x00(%rdi), %rax\n"
    "  movq 0x08(%rdi), %rcx\n"
    "  movq 0x10(%rdi), %rdx\n"
    "  movq 0x18(%rdi), %rbx\n"
    "  movq 0x20(%rdi), %rsp\n"
    "  movq 0x28(%rdi), %rbp\n"
    "  movq 0x30(%rdi), %r8\n"
    "  movq 0x38(%rdi), %r9\n"
    "  movq 0x40(%rdi), %r10\n"
    "  movq 0x48(%rdi), %r11\n"
    "  movq 0x50(%rdi), %r12\n"
    "  movq 0x58(%rdi), %r13\n"
    "  movq 0x60(%rdi), %r14\n"
    "  movq 0x68(%rdi), %r15\n"
    "  fldcw 0x70(%rdi)\n"
    "  movq %rdx, 0x00(%rsp)\n"
    "  movl $1, %eax\n"
    "  ret\n"
    ".globl sotc_fiber_switch_entry\n"
    "sotc_fiber_switch_entry:\n"
    "  movq %rdi, %r11\n"
    "  movq 0x18(%r11), %rsp\n"
    "  xorl %ebp, %ebp\n"
    "  movq 0x20(%r11), %r10\n"
    "  testq %r10, %r10\n"
    "  jz 1f\n"
    "  movl $2, (%r10)\n"          /* previous fiber is idle once its stack is left */
    "1:\n"
    "  testl %esi, %esi\n"
    "  jz 2f\n"
    "  ldmxcsr 0x2c(%r11)\n"
    "  fldcw 0x28(%r11)\n"
    "2:\n"
    "  movq 0x08(%r11), %rdi\n"
    "  movq 0x10(%r11), %rsi\n"
    "  movq 0x00(%r11), %r11\n"
    "  xorl %eax, %eax\n"
    "  xorl %ebx, %ebx\n"
    "  xorl %ecx, %ecx\n"
    "  xorl %edx, %edx\n"
    "  xorq %r8, %r8\n"
    "  xorq %r9, %r9\n"
    "  xorq %r10, %r10\n"
    "  xorq %r12, %r12\n"
    "  xorq %r13, %r13\n"
    "  xorq %r14, %r14\n"
    "  xorq %r15, %r15\n"
    "  vzeroall\n"
    "  call *%r11\n"
    "  movl $1, %edi\n"             /* the entry must never return */
    "  call sotc_fiber_force_quit\n"
    "  ud2\n");

ABI void fiber_force_quit(uint64_t value) {
    FiberContext *ctx=thread_fiber_get();
    fprintf(stderr,"STOP: fiber entry function returned\n");
    if (!ctx) abort();
    ctx->return_val=value;
    fiber_longjmp(ctx);
}

static int valid(const Fiber *f) { return f->magic_start==SIGNATURE0 && f->magic_end==SIGNATURE1; }
/* Fiber calls the game gets an error from are reported (the first 32): a refused run or switch
 * can lose a job. */
static int32_t fiber_refused(const char *call, const Fiber *f, int32_t error) {
    static _Atomic unsigned reported;
    if (atomic_fetch_add(&reported,1)<32)
        fprintf(stderr,"Runtime: %s refused fiber '%s' (state %u): 0x%08x\n",call,f ? f->name : "?",
                f ? atomic_load(&((Fiber *)f)->state) : 0,(unsigned)error);
    return error;
}
/* The GPU page tracker must never protect a fiber stack: a fault there could not push its
 * exception frame and Windows ends the process without running any handler. */
static void stack_prepare(Fiber *f) {
    bbgpu_register_guest_stack((uintptr_t)f->addr_context,f->size_context);
    f->context_start=f->addr_context;
    f->context_end=(unsigned char *)f->addr_context+f->size_context;
    *(uint64_t *)f->addr_context=STACK_SIGNATURE;
    if (f->flags & FLAG_CONTEXT_SIZE_CHECK)
        for (uint64_t *p=(uint64_t *)f->context_start+1;p<(uint64_t *)f->context_end;++p) *p=STACK_SIZE_CHECK;
}
static void check_overflow(FiberContext *ctx) {
    Fiber *f=ctx->current_fiber;
    if (f && f->addr_context && *(uint64_t *)f->addr_context!=STACK_SIGNATURE) {
        fprintf(stderr,"STOP: stack overflow in fiber '%s' (context 0x%llx bytes)\n",f->name,(unsigned long long)f->size_context);
        abort();
    }
}
static int32_t attach_context(Fiber *f, void *addr, uint64_t size) {
    if (size && size<CONTEXT_MINIMUM_SIZE) return FIBER_ERROR_RANGE;
    if ((size & 15) || !addr || !size || f->addr_context) return FIBER_ERROR_INVALID;
    f->addr_context=addr; f->size_context=size;
    stack_prepare(f);
    return 0;
}
static __attribute__((noreturn)) void enter(Fiber *f, uint64_t arg, void *stack, _Atomic uint32_t *previous_state) {
    FiberData data={.entry=f->entry,.arg_on_initialize=f->arg_on_initialize,.arg_on_run_to=arg,
                    .stack_addr=stack,.state=previous_state,.fpucw=0x037f,.mxcsr=0x9fc0};
    fiber_switch_entry(&data,(f->flags & FLAG_SET_FPU_REGS)!=0);
}
static __attribute__((noreturn)) void switch_to_fiber(Fiber *f, uint64_t arg, FiberContext *ctx) {
    if (f->context) { ctx->arg_on_run_to=arg; fiber_longjmp(f->context); }
    _Atomic uint32_t *previous=NULL;
    if (ctx->prev_fiber) { previous=&ctx->prev_fiber->state; ctx->prev_fiber=NULL; }
    enter(f,arg,(unsigned char *)f->addr_context+f->size_context,previous);
}
static __attribute__((noreturn)) void switch_fiber(Fiber *current, Fiber *f, uint64_t arg, FiberContext *ctx) {
    ctx->prev_fiber=current; ctx->current_fiber=f;
    if (!f->addr_context) {
        /* A fiber without its own context runs on the thread's stack below the run point. */
        ctx->prev_fiber=NULL;
        enter(f,arg,(void *)(ctx->rsp & ~UINT64_C(15)),&current->state);
    }
    switch_to_fiber(f,arg,ctx);
}

static int32_t initialize(Fiber *f, const char *name, FiberEntry entry, uint64_t arg, void *addr, uint64_t size,
                          const FiberOptParam *opt, uint32_t flags, uint32_t build_version) {
    if (!f || !name || !entry) return FIBER_ERROR_NULL;
    if (((uintptr_t)f & 7) || ((uintptr_t)addr & 15) || (opt && ((uintptr_t)opt & 7))) return FIBER_ERROR_ALIGNMENT;
    if (size && size<CONTEXT_MINIMUM_SIZE) return FIBER_ERROR_RANGE;
    if ((size & 15) || (!addr)!=(!size) || (opt && opt->magic!=OPT_SIGNATURE)) return FIBER_ERROR_INVALID;
    if (build_version>=0x03500000) flags|=FLAG_SET_FPU_REGS;
    memset(f->name,0,sizeof(f->name));
    strncpy(f->name,name,MAX_NAME);
    f->entry=entry; f->arg_on_initialize=arg; f->addr_context=addr; f->size_context=size;
    f->context=NULL; f->flags=flags;
    f->magic_start=SIGNATURE0; f->magic_end=SIGNATURE1;
    if (addr) stack_prepare(f);
    static int log_stacks=-1;
    if (log_stacks<0) log_stacks=getenv("SOTC_FIBER_STACKS")!=NULL;
    if (log_stacks) fprintf(stderr,"FIBER '%s' stack %p-%p\n",f->name,addr,(void *)((unsigned char *)addr+size));
    atomic_store(&f->state,STATE_IDLE);
    atomic_fetch_add(&fibers_created,1);
    return 0;
}
static ABI int32_t fiber_initialize(Fiber *f, const char *name, FiberEntry entry, uint64_t arg, void *addr,
                                    uint64_t size, const FiberOptParam *opt, uint32_t build_version) {
    return initialize(f,name,entry,arg,addr,size,opt,0,build_version);
}
static ABI int32_t fiber_initialize_internal(Fiber *f, const char *name, FiberEntry entry, uint64_t arg, void *addr,
                                             uint64_t size, const FiberOptParam *opt, uint32_t flags, uint32_t build_version) {
    return initialize(f,name,entry,arg,addr,size,opt,flags,build_version);
}
static ABI int32_t opt_param_initialize(FiberOptParam *opt) {
    if (!opt) return FIBER_ERROR_NULL;
    if ((uintptr_t)opt & 7) return FIBER_ERROR_ALIGNMENT;
    opt->magic=OPT_SIGNATURE;
    return 0;
}
static ABI int32_t fiber_finalize(Fiber *f) {
    if (!f) return FIBER_ERROR_NULL;
    if ((uintptr_t)f & 7) return FIBER_ERROR_ALIGNMENT;
    if (!valid(f)) return FIBER_ERROR_INVALID;
    uint32_t expected=STATE_IDLE;
    if (!atomic_compare_exchange_strong(&f->state,&expected,STATE_TERMINATED)) return fiber_refused("sceFiberFinalize",f,FIBER_ERROR_STATE);
    if (f->addr_context) bbgpu_register_guest_stack_end((uintptr_t)f->addr_context,f->size_context);
    return 0;
}
static ABI int32_t fiber_run_impl(Fiber *f, void *addr, uint64_t size, uint64_t arg, uint64_t *arg_on_return) {
    if (!f) return FIBER_ERROR_NULL;
    if (((uintptr_t)f & 7) || ((uintptr_t)addr & 15)) return FIBER_ERROR_ALIGNMENT;
    if (!valid(f)) return FIBER_ERROR_INVALID;
    if (thread_fiber_get()) return fiber_refused("sceFiberRun",f,FIBER_ERROR_PERMISSION);
    if (addr || size) { int32_t r=attach_context(f,addr,size); if (r<0) return r; }
    uint32_t expected=STATE_IDLE;
    if (!atomic_compare_exchange_strong(&f->state,&expected,STATE_RUN)) return fiber_refused("sceFiberRun",f,FIBER_ERROR_STATE);
    atomic_fetch_add(&fibers_run,1);
    FiberContext ctx={0};
    ctx.current_fiber=f;
    thread_fiber_set(&ctx);
    if (!fiber_setjmp(&ctx)) {
        if (f->addr_context) switch_to_fiber(f,arg,&ctx);
        enter(f,arg,(void *)(ctx.rsp & ~UINT64_C(15)),NULL);
    }
    /* Back from sceFiberReturnToThread (or a returning entry). */
    Fiber *current=ctx.current_fiber;
    ctx.current_fiber=NULL;
    atomic_store(&current->state,STATE_IDLE);
    if (ctx.return_val) { fputs("STOP: fiber entry function returned\n",stderr); abort(); }
    if (arg_on_return) *arg_on_return=ctx.arg_on_return;
    thread_fiber_set(NULL);
    return 0;
}
static ABI int32_t fiber_switch_impl(Fiber *f, void *addr, uint64_t size, uint64_t arg, uint64_t *arg_on_run) {
    if (!f) return FIBER_ERROR_NULL;
    if (((uintptr_t)f & 7) || ((uintptr_t)addr & 15)) return FIBER_ERROR_ALIGNMENT;
    if (!valid(f)) return FIBER_ERROR_INVALID;
    FiberContext *g=thread_fiber_get();
    if (!g) return FIBER_ERROR_PERMISSION;
    if (addr || size) { int32_t r=attach_context(f,addr,size); if (r<0) return r; }
    uint32_t expected=STATE_IDLE;
    if (!atomic_compare_exchange_strong(&f->state,&expected,STATE_RUN)) return fiber_refused("sceFiberSwitch",f,FIBER_ERROR_STATE);
    atomic_fetch_add(&fiber_switches,1);
    Fiber *current=g->current_fiber;
    if (!current->addr_context) switch_fiber(current,f,arg,g);
    FiberContext ctx={0};
    if (!fiber_setjmp(&ctx)) {
        current->context=&ctx;
        check_overflow(g);
        switch_fiber(current,f,arg,g);
    }
    g=thread_fiber_get();
    if (g->prev_fiber) { atomic_store(&g->prev_fiber->state,STATE_IDLE); g->prev_fiber=NULL; }
    if (arg_on_run) *arg_on_run=g->arg_on_run_to;
    return 0;
}
static ABI int32_t fiber_run(Fiber *f, uint64_t arg, uint64_t *arg_on_return) { return fiber_run_impl(f,NULL,0,arg,arg_on_return); }
static ABI int32_t fiber_switch(Fiber *f, uint64_t arg, uint64_t *arg_on_run) { return fiber_switch_impl(f,NULL,0,arg,arg_on_run); }
static ABI int32_t fiber_get_self(Fiber **out) {
    if (!out) return FIBER_ERROR_NULL;
    if (!thread_fiber_get()) return FIBER_ERROR_PERMISSION;
    *out=thread_fiber_get()->current_fiber;
    return 0;
}
static ABI int32_t fiber_return_to_thread(uint64_t arg_on_return, uint64_t *arg_on_run) {
    FiberContext *g=thread_fiber_get();
    if (!g) return FIBER_ERROR_PERMISSION;
    Fiber *current=g->current_fiber;
    FiberContext ctx={0};
    if (current->addr_context) {
        if (fiber_setjmp(&ctx)) {
            g=thread_fiber_get();
            if (g->prev_fiber) { atomic_store(&g->prev_fiber->state,STATE_IDLE); g->prev_fiber=NULL; }
            if (arg_on_run) *arg_on_run=g->arg_on_run_to;
            return 0;
        }
        current->context=&ctx;
        check_overflow(g);
    }
    g->arg_on_return=arg_on_return;
    fiber_longjmp(g);
}
static ABI int32_t fiber_get_info(Fiber *f, FiberInfo *info) {
    if (!f || !info) return FIBER_ERROR_NULL;
    if (((uintptr_t)f & 7) || ((uintptr_t)info & 7)) return FIBER_ERROR_ALIGNMENT;
    if (info->size!=sizeof(FiberInfo) || !valid(f)) return FIBER_ERROR_INVALID;
    info->entry=f->entry; info->arg_on_initialize=f->arg_on_initialize;
    info->addr_context=f->addr_context; info->size_context=f->size_context;
    memset(info->name,0,sizeof(info->name));
    memcpy(info->name,f->name,MAX_NAME);
    info->size_context_margin=UINT64_MAX;
    if ((f->flags & FLAG_CONTEXT_SIZE_CHECK) && f->addr_context && *(uint64_t *)f->context_start==STACK_SIGNATURE) {
        uint64_t *p=(uint64_t *)f->context_start+1;
        while (p<(uint64_t *)f->context_end && *p==STACK_SIZE_CHECK) ++p;
        info->size_context_margin=(uint64_t)((unsigned char *)p-(unsigned char *)((uint64_t *)f->context_start+1));
    }
    return 0;
}
static ABI int32_t fiber_rename(Fiber *f, const char *name) {
    if (!f || !name) return FIBER_ERROR_NULL;
    if ((uintptr_t)f & 7) return FIBER_ERROR_ALIGNMENT;
    if (!valid(f)) return FIBER_ERROR_INVALID;
    memset(f->name,0,sizeof(f->name));
    strncpy(f->name,name,MAX_NAME);
    return 0;
}
static ABI int32_t fiber_frame_pointer_address(uint64_t *out) {
    if (!out) return FIBER_ERROR_NULL;
    if (!thread_fiber_get()) return FIBER_ERROR_PERMISSION;
    *out=thread_fiber_get()->rbp;
    return 0;
}

static const struct { const char *nid; void *function; } exports[]={
    {"hVYD7Ou2pCQ",fiber_initialize},          /* _sceFiberInitializeImpl */
    {"7+OJIpko9RY",fiber_initialize_internal}, /* _sceFiberInitializeWithInternalOptionImpl */
    {"asjUJJ+aa8s",opt_param_initialize}, {"JeNX5F-NzQU",fiber_finalize},
    {"a0LLrZWac0M",fiber_run}, {"PFT2S-tJ7Uk",fiber_switch},
    {"p+zLIOg27zU",fiber_get_self}, {"B0ZX2hx9DMw",fiber_return_to_thread},
    {"avfGJ94g36Q",fiber_run_impl},    /* _sceFiberAttachContextAndRun */
    {"ZqhZFuzKT6U",fiber_switch_impl}, /* _sceFiberAttachContextAndSwitch */
    {"uq2Y5BFz0PE",fiber_get_info}, {"JzyT91ucGDc",fiber_rename},
    {"0dy4JtMUcMQ",fiber_frame_pointer_address},
};
uintptr_t runtime_fiber_resolve(const char *name) {
    for (size_t i=0;i<sizeof(exports)/sizeof(*exports);++i)
        if (!strcmp(name,exports[i].nid)) return (uintptr_t)exports[i].function;
    return 0;
}
void runtime_fiber_report(void) {
    printf("Runtime: fibers created=%u, runs=%u, switches=%u\n",atomic_load(&fibers_created),
           atomic_load(&fibers_run),atomic_load(&fiber_switches));
}
