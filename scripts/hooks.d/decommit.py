# SPDX-License-Identifier: GPL-2.0-or-later
"""Engine hooks of the texture pool decommit (docs/re-stutters/reports2/03-streaming-bursts.md, C3).

The drop branch of sub_98ae0 (texture mip downgrade, 0x98ae0) calls the pool allocator 0x12950 with
mode 2 (decommit) at 0x98bf0 and then updates the pool cursor and its counters (0x98bf5..0x98c05).

DECOMMIT_BEGIN  Site on that call (0x98bf0): starts the timer and counts the drop.
DECOMMIT_END    Site on the instruction after it (0x98bf5): stops the timer.

Both are observe only (gpu/sotc/decommit_budget.cpp). Switch SOTC_DECOMMIT_BUDGET: unset or 0 off,
2 observe. Deferring the decommit is not implemented: 0x12950 picks its allocator from the calling
thread's TLS (sub_12950 0x12950..0x129d8), so a deferred call cannot be replayed from another thread.

The signatures are the shortest unique ones from tools/re/eboot_re.py sig, checked with sigfind on the
EU 1.01 eboot. The expected offsets apply to that eboot only; any other eboot is scanned.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='DECOMMIT_BEGIN',
        switch='SOTC_DECOMMIT_BUDGET',
        group='decommit',
        mode='site',
        # call 0x12950 (rel32 wildcarded), then the first bytes of the cursor update that follows it
        pattern='e8 ?? ?? ?? ?? 45 29 b4',
        expected={EU_FINGERPRINT: 0x98bf0},
    ),
    dict(
        name='DECOMMIT_END',
        switch='SOTC_DECOMMIT_BUDGET',
        group='decommit',
        mode='site',
        # sub dword ptr [r12+0x1b0], r14d (the pool counter update after the decommit)
        pattern='45 29 b4 24 b0 01 00 00',
        expected={EU_FINGERPRINT: 0x98bf5},
    ),
]
