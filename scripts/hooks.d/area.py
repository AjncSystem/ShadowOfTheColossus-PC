# SPDX-License-Identifier: GPL-2.0-or-later
"""Area engine hooks (docs/re-stutters/reports/05-streaming.md and 07-first-use.md). Probes only observe.

AREA_PRECACHE: entry of AreaLoad_PrecacheLists (0x533100), the per-area loader. It calls the shader
list step (0x2569d0) with its two arguments, then builds the precache paths
($/Models/****/shadowmodels.crmp, $/cp3shadow/****/cp3shadowprecache.calt, ...).
Entry registers: rdi = the area loader object (this), rsi = the record passed to 0x2569d0.

AREA_LOADER_STATE: entry of sub_343890, the AreaLoaderState state setter. Its three callers are the
string property parser sub_343340, the virtual "apply requested state" sub_3436e0 (13 bytes, it tail
calls the setter) and the timed advance sub_343720. All state changes pass through here.
Entry registers: rdi = the AreaLoaderState object; rsi = int* of the current state (the value is
*rsi); rdx = int* of the requested state (the value is *rdx, read before the clamp of the setter).

Both hooks: switch SOTC_AREA_PREWARM, group area_prewarm.

Checks (EU 1.01 eboot, tools/re/eboot_re.py):
- 0x533100: displaced bytes push rbp / mov rbp,rsp / push r15 (6 bytes). The 32-byte pattern is unique.
- 0x343890: displaced bytes push rbp / mov rbp,rsp / push r15 / push r14 (8 bytes). The 424-byte
  pattern is unique; the 256-byte prefix matches 3 times.
- Neither function has a branch target in the displaced bytes or at the entry, and neither contains
  rdtsc, extrq or insertq.

Derived fields of AREA_LOADER_STATE (bytes of the object accesses r13 = rdi, from the setter):
- range_on  = 0x70, byte: range mode on (cmp byte [r13+0x70], 0);
- range_min = 0x6c, int: range minimum (mov ecx, [r13+0x6c]);
- range_max = 0x74, int: range maximum (mov r14d, [r13+0x74]);
- wrap_flags = 0x1c, byte, bit 0: wrap-around mode (test byte [r13+0x1c], 1).
The loader reads only the displacement; the handler reads the field at obj + value.

Value table of the setter (the eboot strings; the short name is the property value):
  0 Auto, 1 Unavailable, 2 Unloaded (long name NotLoaded), 3 Loading, 4 Loaded,
  5 Creating, 6 Hidden (long name Created_Hidden), 7 Fading In (Created_FadeIn), 8 Created,
  9 Fading Out (Created_FadeOut). The order 3 to 9 is inferred from the names.
  Value 2 is also the value of the table's last entry (no short name); not checked further.

Area identity of an AreaLoaderState object (sub_341950 builds "AreaLoader_<*param3>_<name>_<guid>"):
- the GUID: 16 bytes at (*(obj+0x30)) + 8, printed as hex by 0x510e0;
- the name: a C string at *(*(*(obj+0x18)+0x68)+8).
Not checked at run time. The name chain and the GUID come from the formatter's code, not from a run.

AREA_PRECACHE area identity: not established. Path templates are resolved through the resource
factory global DAT_02ee2d00: sub_14c2f0 replaces "****" with 4 characters found by sub_147e20 (a
map lookup in the factory, falling back to factory +0x40, then "_ps4"). AREA_PRECACHE writes
0x2a2a2a2a ("****") to factory +0x40 (sub_147e10) before it resolves the paths, so +0x40 is not the
area at that point. Where the area code comes from is still open.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='AREA_PRECACHE',
        switch='SOTC_AREA_PREWARM',
        group='area_prewarm',
        mode='entry',
        # push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp,0x2d8; ...
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec d8 02 00 00 48 8b 05 ?? ?? ?? ?? 48 89 bd 00 fd',
        expected={EU_FINGERPRINT: 0x533100},
    ),
    dict(
        name='AREA_LOADER_STATE',
        switch='SOTC_AREA_PREWARM',
        group='area_prewarm',
        mode='entry',
        # push rbp; mov rbp,rsp; push r15..rbx; sub rsp,0x18; mov r15,[rip+..]; mov r13,rdi; ...
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 4c 8b 3d ?? ?? ?? ?? 49 89 fd 49 8b 07 48 89 45 d0 41 80 7d 70 00 48 89 55 c0 74 ?? 8b 02 41 8b 4d 6c 45 8b 75 74 41 f6 45 1c 01 75 ?? 39 c8 0f 4d c8 44 39 f1 44 0f 4e f1 eb ?? 44 8b 32 eb ?? bf 01 00 00 00 29 c8 29 cf 99 44 01 f7 41 ff c6 f7 ff 85 d2 44 0f 49 f1 41 01 d6 48 8d 1d ?? ?? ?? ?? 8a 03 84 c0 0f 85 ?? ?? ?? ?? 48 8d 3d ?? ?? ?? ?? 49 89 f4 e8 ?? ?? ?? ?? 4c 89 e6 85 c0 0f 84 ?? ?? ?? ?? 48 8d 3d ?? ?? ?? ?? 48 8d 05 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ?? 48 8d 35 ?? ?? ?? ?? 48 8d 15 ?? ?? ?? ?? c4 e1 f9 6e c9 c4 e1 f9 6e c0 48 8d 0d ?? ?? ?? ?? c5 f1 6c c0 c4 e1 f9 6e ce 48 8d 35 ?? ?? ?? ?? c5 fa 7f 07 c4 e1 f9 6e c1 48 8d 0d ?? ?? ?? ?? c7 47 10 00 00 00 00 c5 f1 6c c0 c4 e1 f9 6e ca 48 8d 15 ?? ?? ?? ?? c5 fa 7f 47 18 c4 e1 f9 6e c1 48 8d 0d ?? ?? ?? ?? c7 47 28 01 00 00 00 c5 f1 6c c0 c4 e1 f9 6e ce 48 8d 35 ?? ?? ?? ?? c5 fa 7f 47 30 c4 e1 f9 6e c1 48 8d 0d ?? ?? ?? ?? c7 47 40 02 00 00 00 c5 f1 6c c0 c4 e1 f9 6e ca 48 8d 15 ?? ?? ?? ?? c5 fa 7f 47 48 c4 e1 f9 6e c1 48 8d 0d ?? ?? ?? ?? c7 47 58 03 00 00 00 c5 f1 6c c0 c5 fa 7f 47 60 c7 47 70 04 00 00 00 48 89 4f 78 48 89 b7 80 00 00 00 48 8d 35 ?? ?? ?? ?? c4 e1 f9 6e c2 48 8d 0d ?? ?? ?? ?? 48 8d 15 ?? ?? ?? ?? c7 87 88 00 00 00 06 00 00',
        expected={EU_FINGERPRINT: 0x343890},
        derived=[
            dict(field='range_on', offset=37, size=1, kind='disp', insn_end=39),
            dict(field='range_min', offset=50, size=1, kind='disp', insn_end=51),
            dict(field='range_max', offset=54, size=1, kind='disp', insn_end=55),
            dict(field='wrap_flags', offset=58, size=1, kind='disp', insn_end=60),
        ],
    ),
]
