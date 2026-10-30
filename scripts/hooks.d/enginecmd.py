"""Engine console execution (docs/re-stutters/engine-commands.md). Switch SOTC_ENGINE_CMD, one group.

    ENGINE_CMD_TICK   entry at sub_3c7740 (0x3c7740), the in-game console's per-frame tick. It is
                      called from GameCoreLoop::GameLoop on the update thread, once per frame. rdi is
                      the console object: the first argument of sub_3c9c80, the function that runs one
                      console line (echo, parse, execute). The tick is the drain point of
                      gpu/sotc/engine_cmd.cpp: it captures the console pointer and, when a line is
                      queued, returns HookAction::CallGuest for sub_3c9c80(console, &line).
                      Signature: 36 bytes, unique (tools/re/eboot_re.py sig 0x3c7740).

    ENGINE_CMD_PRINT  entry at sub_3c9a10 (0x3c9a10), the console output append. rdi = the console,
                      rsi = a bpe::String (data pointer at +0, length at +8). Observe only: the handler
                      copies the line into the output ring that RecentOutput() returns.
                      Signature: 32 bytes, unique (tools/re/eboot_re.py sig 0x3c9a10).

Both expected offsets are for the EU 1.01 eboot fingerprint (the same one dt.py uses). Other eboots scan
for the signature. The RIP-relative bytes inside the patterns are wildcards; the displaced prologue of
each entry hook contains no RIP-relative instruction.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='ENGINE_CMD_TICK',
        switch='SOTC_ENGINE_CMD',
        group='enginecmd',
        mode='entry',
        # push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx; and rsp,-32;
        # sub rsp,0xa0; mov rax,[rip+x]; vmovss [rsp+0x...],xmm0 ...
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 e4 e0 48 81 ec a0 00 00 00 48 8b 05 '
                '?? ?? ?? ?? c5 fa 11 44 24',
        expected={EU_FINGERPRINT: 0x3c7740},
    ),
    dict(
        name='ENGINE_CMD_PRINT',
        switch='SOTC_ENGINE_CMD',
        group='enginecmd',
        mode='entry',
        # push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp,0x28;
        # mov rax,[rip+x]; mov r12,rdi; lea rdx,[rip+x]
        pattern='55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 28 48 8b 05 ?? ?? ?? ?? 49 89 fc '
                '48 8d 15 ?? ??',
        expected={EU_FINGERPRINT: 0x3c9a10},
    ),
]
