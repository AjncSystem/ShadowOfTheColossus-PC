# SPDX-License-Identifier: GPL-2.0-or-later
"""Engine hooks of the ResourceFactory loader (docs/re-stutters/reports/05-streaming.md).

LOADER_WAIT_ALL     ResourceFactory::WaitForAllCmdsToFinish, the frame barrier (wrap).
LOADER_SYNCPOINT    ResourceFactory::SyncPointProcessPendingResources, completion drain (wrap).
LOADER_HAS_PENDING  ResourceFactory::HasPendingWork, called only by the barrier (wrap).

Each pattern is the shortest unique signature from tools/re/eboot_re.py sig, checked with sigfind on
the EU 1.01 eboot. The expected offsets apply to that eboot only; any other eboot is scanned.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='LOADER_WAIT_ALL',
        switch='SOTC_HOOK_LOADER_PROBE',
        group='loader_probe',
        mode='wrap',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x88; mov r14,[rip+..]; mov rbx,rdi; ...
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 88 00 00 00 4c 8b 35 ?? ?? ?? ?? 48 89 fb 48 8d',
        expected={EU_FINGERPRINT: 0x1469d0},
    ),
    dict(
        name='LOADER_SYNCPOINT',
        switch='SOTC_HOOK_LOADER_PROBE',
        group='loader_probe',
        mode='wrap',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x58; mov rax,[rip+..]; mov r15,rdi; ...
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 58 48 8b 05 ?? ?? ?? ?? 49 89 ff bb 01 00 00 00',
        expected={EU_FINGERPRINT: 0x1480f0},
    ),
    dict(
        name='LOADER_HAS_PENDING',
        switch='SOTC_HOOK_LOADER_NOSTREAM',
        group='loader_nostream',
        mode='wrap',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x68; mov rax,[rip+..]; lea rdx,[rip+..]
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 68 48 8b 05 ?? ?? ?? ?? 48 8d 15 ??',
        expected={EU_FINGERPRINT: 0x147f40},
    ),
]
