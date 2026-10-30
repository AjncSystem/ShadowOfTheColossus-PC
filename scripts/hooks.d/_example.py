"""Engine hook declarations (scripts/sotc_hooks.py). One file per hook, or a few related hooks.

Every file in this folder whose name does not start with "_" is loaded at each launch and must
define HOOKS, a list of dicts. A file that starts with "_" (this one) is ignored, so it documents
the format without changing the game. Check it with:

    python scripts/sotc_hooks.py ../CUSA08809 --out out --include-examples
    python tools/re/hooks_check.py --include-examples

What each field means (the complete list is in the docstring of scripts/sotc_hooks.py):

    name      required, unique. The switch is SOTC_HOOK_<NAME> unless "switch" says otherwise.
    mode      entry: detour at the function start, may return early (Entry).
              wrap:  detour with a pre and a post phase (Wrap).
              site:  one instruction inside a function (Site); registers are editable.
              range: no code change; the resolved span classifies faults (Range, range_len bytes).
    pattern   the instruction bytes at the hook site, ?? for any byte (rel32 and RIP-relative
              displacements are wildcards). tools/re/eboot_re.py sig ADDR writes one;
              tools/re/eboot_re.py sigfind PATTERN checks it.
    expected  {eboot fingerprint: image offset}. Used only when the fingerprint matches; the
              loader checks the pattern there first and scans the code when it does not match.
    anchor    another hook's name. This hook must match inside the function that holds that
              hook's site: a pattern that is not unique alone (short generic code) becomes unique.
    deps      hooks that must resolve too; if one of them is off, this hook is off.
    derived   data read from the matched bytes, not hard-coded: a struct offset in a
              displacement (kind disp, signed), an immediate (kind imm, unsigned) or the target of
              a RIP-relative operand (kind rip_target: match + insn_end + the signed displacement).

The fingerprint below is the EU 1.01 eboot (CUSA08809); the US 1.00 eboot has its own.

Note: the example is not a real hook. The byte pattern is the start of the function at 0x925c0
(the game's label writer), checked with tools/re/eboot_re.py sigfind on the EU eboot.
"""

EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'

HOOKS = [
    dict(
        name='EXAMPLE_CS_DONE_LABEL',
        group='example',
        mode='entry',
        # push rbp; mov rbp,rsp; mov rdi,[rdi]; mov rax,rsi; mov esi,0x28 (the event id)
        pattern='55 48 89 e5 48 8b 3f 48 89 f0 be 28 00 00 00',
        expected={EU_FINGERPRINT: 0x925c0},
        derived=[dict(field='event', offset=11, size=4, kind='imm', insn_end=15)],
    ),
    dict(
        name='EXAMPLE_EVENT_SITE',
        group='example',
        mode='site',
        # mov esi,0x28 alone is not unique in the eboot (51 matches); the anchor makes it unique
        pattern='be 28 00 00 00',
        anchor='EXAMPLE_CS_DONE_LABEL',
        deps=['EXAMPLE_CS_DONE_LABEL'],
        expected={EU_FINGERPRINT: 0x925ca},
        derived=[dict(field='event', offset=1, size=4, kind='imm', insn_end=5)],
    ),
]
