# SPDX-License-Identifier: GPL-2.0-or-later
"""Engine hook of the render-job draw tag probe (plan B3, docs/re-stutters/reports2/14-draw-sources.md H1).

RENDER_JOB_RUN  RenderThread::RunFunctionAsRenderJob (0xc12a0), wrap. Observe only. The probe
                (gpu/sotc/probe_drawtag.cpp) counts the render jobs per body address and times the
                ones that run inline. Switch SOTC_DRAW_TAG.

The 28-byte pattern is the shortest unique signature from tools/re/eboot_re.py sig, checked with
sigfind on the EU 1.01 eboot (one match, 0xc12a0). The expected offset applies to that eboot only;
any other eboot is scanned.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='RENDER_JOB_RUN',
        switch='SOTC_DRAW_TAG',
        group='draw_tag',
        mode='wrap',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x138; mov r12,[rip+..] (wildcarded); ...
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 38 01 00 00 4c 8b 25 ?? ?? ?? ?? 41',
        expected={EU_FINGERPRINT: 0xc12a0},
    ),
]
