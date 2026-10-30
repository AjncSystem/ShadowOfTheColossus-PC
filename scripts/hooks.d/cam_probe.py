# -*- coding: utf-8 -*-
"""Camera and Wander probes (docs/re-stutters/freecam-re.md). Observe only.

PLAYER_XFORM  entry of sub_000000b0 (0xb0), the function that reads a player transform: it calls the
              type switch sub_660fc0 on the object in rdi, then copies the position floats
              (transform+0x150 pointer) and the quaternion (transform+0x160 pointer). Called by the
              printPlayerPos and setPlayerPos console commands (rare, command time only).
              Entry registers: rdi = the object (the Wander object, as the commands pass it),
              rsi = float* out position, rdx = float* out rotation. The handler reads the object's type
              byte at +0xc and logs the transform candidates it can resolve without guest calls.

Switch SOTC_CAM_PROBE, group cam_probe. The GameCamera request (sub_4948b0, 0x4948b0) is NOT hooked
here: gpu/sotc/freecam.cpp's FC_CAM uses that site, so one detour serves both.

Checks (EU 1.01 eboot, tools/re/eboot_re.py sigfind; both patterns unique):
- 0xb0: 28 bytes, push rbp; mov rbp,rsp; push r15; push r14; push r12; push rbx; sub rsp,0x80; the
  rip-relative load of the stack-protector slot is wildcarded.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='PLAYER_XFORM',
        switch='SOTC_CAM_PROBE',
        group='cam_probe',
        mode='entry',
        # push rbp; mov rbp,rsp; push r15; push r14; push r12; push rbx; sub rsp,0x80;
        # mov r11,[rip+..] (wildcarded: the stack guard); mov rcx,[rip+..]
        pattern='55 48 89 e5 41 57 41 56 41 54 53 48 81 ec 80 00 00 00 4c 8b 25 ?? ?? ?? ?? 48 8b 0d',
        expected={EU_FINGERPRINT: 0xb0},
    ),
]
