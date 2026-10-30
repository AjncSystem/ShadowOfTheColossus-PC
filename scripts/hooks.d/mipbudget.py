# SPDX-License-Identifier: GPL-2.0-or-later
"""Engine hook of the per-frame mip copy budget (docs/re-stutters/reports2/03-streaming-bursts.md, C4).

MIP_BUDGET_COPY  sub_98ae0, the texture chunk upgrade/downgrade (wrap). An upgrade to chunk N
                 memcpy's the ready mip into the streaming pool and then sets the texture's current
                 chunk to N. Observe only (gpu/sotc/mip_budget.cpp), switch SOTC_MIP_BUDGET.

The signature is the shortest unique one from tools/re/eboot_re.py sig, checked with sigfind on the
EU 1.01 eboot (one match, 0x98ae0). The expected offset applies to that eboot only.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='MIP_BUDGET_COPY',
        switch='SOTC_MIP_BUDGET',
        group='mip_budget',
        mode='wrap',
        # push rbp; mov rbp,rsp; push r15..r12; push rbx; push rax; mov ebx,esi; mov r12,rdi; ...
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 50 89 f3 49 89 fc 48',
        expected={EU_FINGERPRINT: 0x98ae0},
    ),
]
