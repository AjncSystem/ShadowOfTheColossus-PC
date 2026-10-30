"""Range probe of the engine cvars gameMaxDeltaTime and gameDeltaTimeCorrectionTolorance (probe_cvars.cpp).

sub_262a80 registers both cvars at startup (EU 1.01 eboot). Each cvar is a Bluepoint cvar object
in the data segment. Its +0x60 holds a pointer to the float the engine reads: the constructor is
given object+0xd0 as that storage, and sub_25ccc0 reads the value as *(float *)(*(obj + 0x60)).
The registration then writes the default to object+0xd0 and to a second copy at object+0x68.

The range covers both registrations: the match runs from the lea of the gameMaxDeltaTime object
through the lea of the tolerance object's name string. Derived fields (image offsets):
    storage    the gameMaxDeltaTime object (float at +0x60 -> +0xd0)
    tolerance  the gameDeltaTimeCorrectionTolorance object (same layout)
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

# 257 bytes from 0x263483 (EU 1.01). RIP displacements and call rel32 are wildcards.
PATTERN = (
    '48 8d 1d ?? ?? ?? ?? 48 8d 05 ?? ?? ?? ?? 4c 8d 64 24 18 4c 8d 7c 24 40 c5 fc 57 c0 48 c7 84 24 90 00 00 00 00 00 00 00 '
    'c5 f0 57 c9 4c 8d 44 24 60 b9 01 00 00 00 48 89 44 24 18 48 8d 05 ?? ?? ?? ?? c5 fc 11 44 24 60 4c 89 e6 4d 89 f9 48 c7 '
    '44 24 20 10 00 00 00 c5 f8 11 4c 24 28 48 89 84 24 a0 00 00 00 48 8d 05 ?? ?? ?? ?? 48 c7 84 24 a8 00 00 00 00 00 00 00 '
    '48 89 44 24 40 c5 f8 11 4c 24 48 48 c7 44 24 58 00 00 00 00 48 8d 93 d0 00 00 00 48 89 df 4c 89 33 e8 ?? ?? ?? ?? 4c 89 '
    'ff e8 ?? ?? ?? ?? 48 8b 84 24 a0 00 00 00 48 8d bc 24 80 00 00 00 ff 50 18 48 89 df 4c 89 33 c7 83 d0 00 00 00 cd cc cc 3d '
    'c7 43 68 cd cc cc 3d e8 ?? ?? ?? ?? 4c 89 e7 e8 ?? ?? ?? ?? 48 8d 3d ?? ?? ?? ?? 48 8d 15 ?? ?? ?? ?? 48 89 de e8 ?? ?? '
    '?? ?? 4c 8d 25 ?? ?? ?? ?? 48 8d 05 ?? ?? ?? ??'
)

HOOKS = [
    dict(
        name='CVAR_MAX_DELTA',
        switch='SOTC_HOOK_CVAR_PROBE',
        group='cvar_probe',
        mode='range',
        pattern=PATTERN,
        range_len=257,
        expected={EU_FINGERPRINT: 0x263483},
        derived=[
            # lea rbx,[rip+disp]: the gameMaxDeltaTime object
            dict(field='storage', offset=3, size=4, kind='rip_target', insn_end=7),
            # lea r12,[rip+disp]: the gameDeltaTimeCorrectionTolorance object
            dict(field='tolerance', offset=246, size=4, kind='rip_target', insn_end=250),
        ],
    ),
]
