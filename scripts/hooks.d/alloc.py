"""Allocator probes (gpu/sotc/probes.h, probe_alloc.cpp). They only observe.

ALLOC_BLOCK  wrap at mem_allocator_alloc (0x479a0), the block allocator entry of both families.
             rdi = family object: enabled byte +0x9, name (char *) +0x10, kind (int32) +0x18.
             rsi = requested size (rounded up to 64 KiB by the callee). rdx and rcx are not read.
             rax (post) = block node, or 0. Node: guest base +0x60, size +0x58, pool slot byte +0x68.
FREE_BLOCK   entry at pool_free_block (0x1b0e0). rdi = block node (the pointer ALLOC_BLOCK returns).
BPE_HEAP_CODE  range over the BPE heap allocator code, 0x48a70-0x4a477: 20 functions with NOP
             padding between them, no code change.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='ALLOC_BLOCK',
        switch='SOTC_HOOK_ALLOC_PROBE',
        group='alloc_probe',
        mode='wrap',
        # push rbp; mov rbp,rsp; push rbx; push rax; mov rbx,rdi; xor edi,edi; cmp byte [rbx+9],0 (opcode 80)
        pattern='55 48 89 e5 53 50 48 89 fb 31 ff 80',
        expected={EU_FINGERPRINT: 0x479a0},
    ),
    dict(
        name='FREE_BLOCK',
        switch='SOTC_HOOK_ALLOC_PROBE',
        group='alloc_probe',
        mode='entry',
        # push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp,0x18;
        # mov r14,[rip+..] (the lock word, wildcarded); mov r13,rdi
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 4c 8b 35 ?? ?? ?? ?? 49 89 fd bb',
        expected={EU_FINGERPRINT: 0x1b0e0},
    ),
    dict(
        name='BPE_HEAP_CODE',
        switch='SOTC_HOOK_HEAP_RANGE',
        group='heap_range',
        mode='range',
        # BpeHeap_Walk (0x48a70) starts the range; it ends at the end of sub_4a210 (0x4a477).
        pattern='55 48 89 e5 41 57 41 56 53 50 49 89 ff 49 89 f6 4d 8b 87 c0',
        range_len=0x4a477 - 0x48a70,
        expected={EU_FINGERPRINT: 0x48a70},
    ),
]
