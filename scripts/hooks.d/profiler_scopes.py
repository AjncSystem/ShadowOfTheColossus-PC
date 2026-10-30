# SPDX-License-Identifier: GPL-2.0-or-later
"""Engine hooks of the engine's own profiler scopes (trace layer L3b, gpu/sotc/probe_scopes.cpp).

PROFILER_PUSH_SCOPE  Profiler::PushScope (0x16b8c0), entry. rsi = the scope name (a NUL-terminated
                     char*), rdi = the scope object (stack, 16 bytes), rdx = the command buffer context,
                     ecx = the category bit. The name is read by the handler; nothing is written.
PROFILER_POP_SCOPE   Profiler::PopScope (0x16b900), entry. rdi = the same scope object as the push.

Both hooks are entries: they run before the engine's own category check, so the handlers see every
call, whether or not the engine's profiler is enabled. Switch SOTC_TRACE_SCOPES, group profiler_scopes
(all or nothing). Observe only.

The patterns are the shortest unique signatures from tools/re/eboot_re.py sig, checked with sigfind on
the EU 1.01 eboot (one match each). The expected offsets apply to that eboot only; any other eboot is
scanned.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='PROFILER_PUSH_SCOPE',
        switch='SOTC_TRACE_SCOPES',
        group='profiler_scopes',
        mode='entry',
        # mov [rdi],rdx; mov [rdi+8],ecx; mov rax,[rdx+0x48]
        pattern='48 89 17 89 4f 08 48 8b',
        expected={EU_FINGERPRINT: 0x16b8c0},
    ),
    dict(
        name='PROFILER_POP_SCOPE',
        switch='SOTC_TRACE_SCOPES',
        group='profiler_scopes',
        mode='entry',
        # mov rax,[rdi]; mov rcx,[rax+0x48]
        pattern='48 8b 07 48 8b 48 48 8b',
        expected={EU_FINGERPRINT: 0x16b900},
    ),
]
