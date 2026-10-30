"""Delta-time probes (docs/re-stutters/reports2/05-frame-pacing.md, H1 and H2). Observe only.

    DT_CONSUMER  wrap at sub_25ccc0 (0x25ccc0), the real-time dt consumer that GameCoreLoop::GameLoop
                 calls once per iteration. rdi = the timer object T, esi = the reset flag.
                 Signature: 20 bytes, unique (tools/re/eboot_re.py sig 0x25ccc0).
                 Post phase: T+0x18 is the TSC time of this call, T+0x28 the output dt,
                 T+0x2c the carry (the clamped output).
    DT_UPDATE    site at 0x534a73 in ShadowGameUpdate_ThreadFunc: vmovss xmm0,[rax+0x48]. rax is the
                 time object, so the float read here is the dt the update uses (GameCoreLoop+0x3bc0).
                 Signature: 12 bytes, unique (tools/re/eboot_re.py sig 0x534a73). The displacement 0x48
                 is the derived field dt_offset (pattern offset 4, disp8), so no offset is hard-coded.

Switch SOTC_DT_PROBE (both hooks, one group). The cap (gameMaxDeltaTime) is read through the cvar
object that the CVAR_MAX_DELTA range hook (scripts/hooks.d/cvars.py) resolves, so SOTC_HOOK_CVAR_PROBE=1
must be set as well; without it the report says the cap is unknown.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='DT_CONSUMER',
        switch='SOTC_DT_PROBE',
        group='dt_probe',
        mode='wrap',
        # push rbp; mov rbp,rsp; push r15; push r14; push rbx; sub rsp,0x18; mov rbx,rdi;
        # mov r14d,esi; lea r15,[rbx+0x10]   (no rel32 or RIP displacement inside)
        pattern='55 48 89 e5 41 57 41 56 53 48 83 ec 18 48 89 fb 41 89 f6 4c',
        expected={EU_FINGERPRINT: 0x25ccc0},
    ),
    dict(
        name='DT_UPDATE',
        switch='SOTC_DT_PROBE',
        group='dt_probe',
        mode='site',
        # vmovss xmm0,[rax+0x48] (5 bytes, displaced by the detour); vmovss [rsp+0x18],xmm0; call
        pattern='c5 fa 10 40 48 c5 fa 11 44 24 18 e8',
        expected={EU_FINGERPRINT: 0x534a73},
        # the disp8 0x48 is the offset of the time object's dt float (pattern offset 4)
        derived=[dict(field='dt_offset', offset=4, size=1, kind='disp', insn_end=5)],
    ),
]
