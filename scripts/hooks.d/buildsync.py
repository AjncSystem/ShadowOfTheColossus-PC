# -*- coding: utf-8 -*-
"""BuildSync probe (gpu/sotc/probe_buildsync.cpp, docs/re-stutters/reports2/17-buildsync-callers.md).

Observe only: both hooks are Wrap and their handlers always return HookAction::Continue.

BUILDSYNC_BUILD   wrap at ResourceFactory::BuildSync (0x146b60), vtable slot 2 of the resource
                  factory. Pre records the thread, the caller chain and the start time; post
                  records the wall time. rdi = the factory, rsi = the wrapper being built.
BUILDSYNC_ENSURE  wrap at EnsureBuilt, sub_14d7b0 (0x14d7b0). Pre reads the wrapper in rdi (the
                  function moves rdi to rbx at once) and counts how often its sync precondition
                  holds: state [+0x98] == 0xb, no data [+0x20] == 0, [+0x60] == 0. Post is empty.

Signatures (EU 1.01 eboot, tools/re/eboot_re.py sig / sigfind, both unique):
- BUILDSYNC_BUILD, 32 bytes: push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x1d8; ...
- BUILDSYNC_ENSURE, 16 bytes: push rbp; mov rbp,rsp; push r15; push r14; push rbx; push rax;
  mov rbx,rdi; cmp dword [rbx+0x98],... (the 0x98 displacement is in the pattern).
Both hooks: switch SOTC_BUILDSYNC_PROBE, group buildsync_probe. The two hooks are one group, so
either both install or neither does.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='BUILDSYNC_BUILD',
        switch='SOTC_BUILDSYNC_PROBE',
        group='buildsync_probe',
        mode='wrap',
        # push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx;
        # sub rsp,0x1d8; mov rbx,[rip+..] (wildcarded); mov r13,rsi (rsi = the wrapper)
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec d8 01 00 00 48 8b 1d ?? ?? ?? ?? 49 89 f5 49 89',
        expected={EU_FINGERPRINT: 0x146b60},
    ),
    dict(
        name='BUILDSYNC_ENSURE',
        switch='SOTC_BUILDSYNC_PROBE',
        group='buildsync_probe',
        mode='wrap',
        # push rbp; mov rbp,rsp; push r15; push r14; push rbx; push rax; mov rbx,rdi;
        # cmp dword [rbx+0x98],0xb
        pattern='55 48 89 e5 41 57 41 56 53 50 48 89 fb 83 bb 98',
        expected={EU_FINGERPRINT: 0x14d7b0},
    ),
]
