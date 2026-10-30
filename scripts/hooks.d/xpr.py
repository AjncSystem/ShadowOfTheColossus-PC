"""XPR probe hooks (docs/re-stutters/reports/01-xpr-cull.md, 02-gpu-cpu-sync.md). Observe only.

    XPR_CS_DONE_LABEL   0x925c0  the completion-label writer (EOP event 0x28, data 1); entry.
                                 rsi = the label address the caller passes.
    XPR_CONSUME_MASK    0x1a6d20 the CPU consumer of the visibility mask; entry.
                                 rsi = the bitset (qwords), rdi = the object-set record,
                                 rdx = the draw record, rcx = the frame context.

Both use the switch SOTC_HOOK_XPR_PROBE (scripts/sotc_hooks.py). Patterns: the first instructions
of each function, no rel32 or RIP displacement inside them (so no wildcards); checked with
tools/re/eboot_re.py sigfind on the EU 1.01 eboot.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='XPR_CS_DONE_LABEL',
        switch='SOTC_HOOK_XPR_PROBE',
        group='xpr_probe',
        mode='entry',
        # push rbp; mov rbp,rsp; mov rdi,[rdi]; mov rax,rsi; mov esi,0x28 (the EOP event id)
        pattern='55 48 89 e5 48 8b 3f 48 89 f0 be 28 00 00 00',
        expected={EU_FINGERPRINT: 0x925c0},
        # the event id is an immediate at pattern offset 11; no struct offset is in these bytes
        derived=[dict(field='event', offset=11, size=4, kind='imm', insn_end=15)],
    ),
    dict(
        name='XPR_CONSUME_MASK',
        switch='SOTC_HOOK_XPR_PROBE',
        group='xpr_probe',
        mode='entry',
        # push rbp; mov rbp,rsp; push r15,r14,r13,r12,rbx; sub rsp,0x68; mov r9,rdi; mov r8,rdx;
        # mov r13,rsi; movzx r10d,word ptr [r9+0x64]   (the u16 entry count of the object set)
        pattern=('55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 68 49 89 f9 '
                 '49 89 d0 49 89 f5 45 0f b7 51 64'),
        expected={EU_FINGERPRINT: 0x1a6d20},
        # the displacement 0x64 (the count) is the disp8 at pattern offset 30
        derived=[dict(field='count_offset', offset=30, size=1, kind='disp', insn_end=31)],
    ),
]
