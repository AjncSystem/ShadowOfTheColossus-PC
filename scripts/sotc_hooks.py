#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Engine hooks of Shadow of the Colossus (docs/HOOK-DESIGN.md): the declarations become out/hooks.bin.

    python scripts/sotc_hooks.py GAME --out out            GAME: the game folder (with eboot.bin) or its eboot
    python scripts/sotc_hooks.py GAME --out out --include-examples

Each scripts/hooks.d/*.py whose name does not start with "_" is loaded and must define HOOKS, a list
of dicts, one per hook. Only the loader in the game reads the result (bb-probe --sotc-hooks); this
script checks the declarations and writes them, it does not look at the eboot's code (that is
tools/re/hooks_check.py). A file that does not parse turns every hook off, it never guesses.

    HOOKS = [dict(
        name='EXAMPLE_HOOK',              # required: a letter, then letters, digits and _ (47 at most)
        switch='SOTC_HOOK_EXAMPLE_HOOK',  # default SOTC_HOOK_<NAME>; the hook runs when it is set non-zero
        group='example',                  # default: the name; a group is all or nothing at load time
        mode='entry',                     # 'entry' (detour, may return early), 'wrap' (before and after),
                                          # 'site' (one instruction inside a function), 'range' (no code change),
                                          # 'time' (a trace span per call of the function; no handler, see below)
        pattern='55 48 89 e5 ?? ?? be 28 00 00 00',  # required: hex bytes, ?? matches any byte
        expected={'<eboot fingerprint, 64 hex>': 0x925c0},  # optional: image offset for that eboot
        range_len=0,                      # range mode only (required there): bytes the range covers
        anchor=None,                      # None, or the name of another hook: this hook's match must lie
                                          # in the function that holds that hook's site
        deps=[],                          # other hooks that must resolve too, or this one is off
        derived=[dict(field='event', offset=11, size=4, kind='imm', insn_end=15)],
    )]

A derived field is decoded from the matched bytes (offset into the pattern, size 1/2/4/8):
kind 'imm' is an unsigned immediate; 'disp' is a signed displacement; 'rip_target' is the image
offset of a RIP-relative operand: the instruction ends at insn_end (a pattern offset), and the
target is match + insn_end + the signed displacement.

A 'time' hook (SOTC_TRACE_FUNCS) is a slim timed entry at one function: the loader records a trace
span named after the hook for each call, and the function's own return is kept. Its pattern must be
the function's first bytes. Its switch is one group switch in practice: any non-empty value other
than 0 turns the hook on (the value is not read), and it needs SOTC_TRACE on. Give each time hook
its own group (the default), so a function that does not resolve or is refused does not turn the
others off. Scanning costs one pass of the code per hook when the fingerprint is unknown, so declare
the expected offset for the release in use.

The eboot fingerprint is the SHA-256 of its PT_LOAD and PT_SCE_RELRO segments (see fingerprint()).
expected offsets are used only for the fingerprint they name; with any other eboot the loader
scans. The file out/hooks.bin is SOTCHOOK1, the format at the end of gpu/sotc/hooks.h.
"""
import argparse
import hashlib
import importlib.util
import re
import struct
import sys
from pathlib import Path

from prepare import parse_self

PORT = Path(__file__).resolve().parent.parent
HOOKS_DIR = Path(__file__).resolve().parent / 'hooks.d'
MAGIC = b'SOTCHOOK'
VERSION = 1
PT_LOAD, PT_SCE_RELRO = 1, 0x61000010
MODES = {'entry': 0, 'wrap': 1, 'site': 2, 'range': 3, 'time': 4}
MODE_NAMES = {number: name for name, number in MODES.items()}
KINDS = {'imm': 0, 'disp': 1, 'rip_target': 2}
KIND_NAMES = {number: name for name, number in KINDS.items()}
ANCHOR_NONE, ANCHOR_FUNCTION = 0, 1
NAME = re.compile(r'[A-Za-z][A-Za-z0-9_]{0,46}')
FIELD = re.compile(r'[A-Za-z_][A-Za-z0-9_]{0,30}')
GROUP = re.compile(r'[A-Za-z0-9_]{1,31}')
SWITCH = re.compile(r'SOTC_[A-Z0-9_]{1,58}')
FINGERPRINT = re.compile(r'[0-9a-f]{64}')
DECLARATION_KEYS = {'name', 'switch', 'group', 'mode', 'pattern', 'expected', 'range_len', 'anchor',
                    'deps', 'derived'}
DERIVED_KEYS = {'field', 'offset', 'size', 'kind', 'insn_end'}
# SOTCHOOK1 (little endian, no padding): header, then one record per hook (gpu/sotc/hooks.h).
HEADER = struct.Struct('<8sII32sII')          # magic, version, count, fingerprint, known, reserved
NAMES = struct.Struct('<48s64s32s')           # name, switch, group (NUL-padded)
FIXED = struct.Struct('<IIQQII')              # mode, pattern_len, expected, range_len, anchor_kind, anchor_index
DERIVED = struct.Struct('<32sIIII')           # field, offset, size, kind, insn_end


class DeclarationError(ValueError):
    """A declaration that cannot be written; the message names the file and the hook."""


def parse_pattern(text):
    """'48 8b ?? 10' -> (pattern bytes, mask bytes): mask 0xff compares a byte, 0x00 is a wildcard."""
    if not isinstance(text, str):
        raise DeclarationError('pattern must be text such as "48 8b ?? 10"')
    pattern, mask = bytearray(), bytearray()
    for token in text.split():
        if token == '??':
            pattern.append(0)
            mask.append(0)
        elif re.fullmatch(r'[0-9a-fA-F]{2}', token):
            pattern.append(int(token, 16))
            mask.append(0xff)
        else:
            raise DeclarationError(f'pattern byte {token!r} is neither two hex digits nor ??')
    if not pattern:
        raise DeclarationError('empty pattern')
    if not any(mask):
        raise DeclarationError('pattern has no fixed byte (everything is a wildcard)')
    return bytes(pattern), bytes(mask)


def fingerprint_of(elf, ph):
    """SHA-256 over every PT_LOAD and PT_SCE_RELRO header, in program header order: its virtual
    address and file size (two little-endian u64), then its file bytes (docs/HOOK-DESIGN.md)."""
    digest = hashlib.sha256()
    view = memoryview(elf)
    for p in ph:
        if p['type'] in (PT_LOAD, PT_SCE_RELRO):
            digest.update(struct.pack('<QQ', p['vaddr'], p['filesz']))
            digest.update(view[p['offset']:p['offset'] + p['filesz']])
    return digest.hexdigest()


def fingerprint(data):
    """The fingerprint of an eboot given as bytes: a SELF, or a plain ELF such as out/eboot.elf."""
    elf, _, ph, _, _ = parse_self(data)
    return fingerprint_of(elf, ph)


def load_declarations(folder, include_examples=False):
    """[(file name, hook dict)] of every declaration file in folder, files in name order. Files whose
    name starts with "_" (such as _example.py) are skipped unless include_examples is set."""
    found = []
    if not folder.is_dir():
        return found
    for path in sorted(folder.glob('*.py')):
        if path.name.startswith('_') and not include_examples:
            continue
        spec = importlib.util.spec_from_file_location(f'sotc_hook_file_{path.stem}', path)
        module = importlib.util.module_from_spec(spec)
        try:
            spec.loader.exec_module(module)
        except Exception as error:
            raise DeclarationError(f'{path.name}: {error!r}') from error
        hooks = getattr(module, 'HOOKS', None)
        if not isinstance(hooks, list) or not all(isinstance(hook, dict) for hook in hooks):
            raise DeclarationError(f'{path.name}: HOOKS must be a list of dicts')
        found.extend((path.name, hook) for hook in hooks)
    return found


def _hook_record(source, hook, fingerprint_hex):
    """One declaration checked and turned into a record; anchor and deps stay names here.
    Returns (record, whether the declared expected offsets name this eboot)."""
    unknown = sorted(set(hook) - DECLARATION_KEYS)
    if unknown:
        raise DeclarationError(f'{source}: unknown key {unknown[0]!r}')
    name = hook.get('name')
    if not isinstance(name, str) or not NAME.fullmatch(name):
        raise DeclarationError(f'{source}: name {name!r} must start with a letter and use letters, digits or _')
    where = f'{source}: hook {name}'
    switch = hook.get('switch', 'SOTC_HOOK_' + name.upper())
    if not isinstance(switch, str) or not SWITCH.fullmatch(switch):
        raise DeclarationError(f'{where}: switch {switch!r} must be SOTC_ followed by capitals, digits or _')
    group = hook.get('group', name[:31])  # the group field holds 31 characters
    if not isinstance(group, str) or not GROUP.fullmatch(group):
        raise DeclarationError(f'{where}: group {group!r} must be letters, digits or _ (31 at most)')
    mode = hook.get('mode')
    if not isinstance(mode, str) or mode not in MODES:
        raise DeclarationError(f'{where}: mode {mode!r} is not one of {", ".join(MODES)}')
    try:
        pattern, mask = parse_pattern(hook.get('pattern'))
    except DeclarationError as error:
        raise DeclarationError(f'{where}: {error}') from None
    # The loader's limits (gpu/sotc/hooks.cpp kMaxPattern/kMaxDeps/kMaxDerived): a file past them
    # is rejected whole at boot, so refuse it here.
    if len(pattern) > 4096:
        raise DeclarationError(f'{where}: pattern longer than 4096 bytes')
    if len(hook.get('deps', []) or []) > 256:
        raise DeclarationError(f'{where}: more than 256 deps')
    if len(hook.get('derived', []) or []) > 64:
        raise DeclarationError(f'{where}: more than 64 derived fields')
    range_len = hook.get('range_len', 0)
    if not isinstance(range_len, int) or isinstance(range_len, bool) or range_len < 0:
        raise DeclarationError(f'{where}: range_len must be a byte count')
    if (mode == 'range') != (range_len > 0):
        raise DeclarationError(f'{where}: range_len is for range hooks only, and range hooks need it')
    expected = hook.get('expected', {})
    if not isinstance(expected, dict):
        raise DeclarationError(f'{where}: expected must be {{fingerprint: offset}}')
    for key, offset in expected.items():
        if not isinstance(key, str) or not FINGERPRINT.fullmatch(key):
            raise DeclarationError(f'{where}: expected key {key!r} is not a 64-digit lowercase fingerprint')
        if not isinstance(offset, int) or isinstance(offset, bool) or offset <= 0:
            raise DeclarationError(f'{where}: expected offset for {key[:12]} must be a positive integer')
    anchor = hook.get('anchor')
    if anchor is not None and not isinstance(anchor, str):
        raise DeclarationError(f'{where}: anchor must be None or the name of another hook')
    deps = hook.get('deps', [])
    if not isinstance(deps, list) or not all(isinstance(dep, str) for dep in deps):
        raise DeclarationError(f'{where}: deps must be a list of hook names')
    derived, fields = [], set()
    derived_list = hook.get('derived', [])
    if not isinstance(derived_list, list):
        raise DeclarationError(f'{where}: derived must be a list of dicts')
    for item in derived_list:
        if not isinstance(item, dict) or set(item) - DERIVED_KEYS or not {'field', 'offset', 'size', 'kind'} <= set(item):
            raise DeclarationError(f'{where}: a derived entry needs field, offset, size and kind')
        field, offset, size, kind = item['field'], item['offset'], item['size'], item['kind']
        if not isinstance(field, str) or not FIELD.fullmatch(field) or field in fields:
            raise DeclarationError(f'{where}: derived field {field!r} is invalid or repeated')
        fields.add(field)
        if not isinstance(offset, int) or offset < 0 or size not in (1, 2, 4, 8) or not isinstance(kind, str) \
                or kind not in KINDS:
            raise DeclarationError(f'{where}: derived {field}: need offset >= 0, size 1/2/4/8, kind imm/disp/rip_target')
        insn_end = item.get('insn_end', offset + size)
        if not isinstance(insn_end, int) or not offset + size <= insn_end <= len(pattern):
            raise DeclarationError(f'{where}: derived {field}: insn_end must lie after the field, inside the pattern')
        derived.append(dict(field=field, offset=offset, size=size, kind=kind, insn_end=insn_end))
    record = dict(name=name, switch=switch, group=group, mode=mode, pattern=pattern, mask=mask,
                  expected=expected.get(fingerprint_hex, 0), range_len=range_len, anchor=anchor,
                  deps=list(deps), derived=derived)
    return record, fingerprint_hex in expected


def build(declarations, fingerprint_hex):
    """Records in declaration order for the eboot with this fingerprint, and whether any declaration
    names this fingerprint (then its expected offsets are for this eboot). Anchors and deps become
    record indices; unknown names, self references and cycles are errors."""
    records, known = [], False
    for source, hook in declarations:
        record, hook_known = _hook_record(source, hook, fingerprint_hex)
        records.append(record)
        known = known or hook_known
    index = {}
    for i, record in enumerate(records):
        if record['name'] in index:
            raise DeclarationError(f'hook {record["name"]} is declared twice')
        index[record['name']] = i
    for record in records:
        for ref in record['deps'] + ([record['anchor']] if record['anchor'] is not None else []):
            if ref not in index:
                raise DeclarationError(f'hook {record["name"]}: {ref!r} is not a declared hook')
            if ref == record['name']:
                raise DeclarationError(f'hook {record["name"]} refers to itself')
    state = {}

    def visit(name):
        if state.get(name) == 'done':
            return
        if state.get(name) == 'busy':
            raise DeclarationError(f'hook {name}: anchor and deps form a cycle')
        state[name] = 'busy'
        record = records[index[name]]
        for ref in record['deps'] + ([record['anchor']] if record['anchor'] is not None else []):
            visit(ref)
        state[name] = 'done'

    for record in records:
        visit(record['name'])
    for record in records:
        record['anchor'] = index[record['anchor']] if record['anchor'] is not None else None
        record['deps'] = [index[dep] for dep in record['deps']]
    return records, known


def encode(fingerprint_hex, known, records):
    """The SOTCHOOK1 bytes of build()'s records (gpu/sotc/hooks.h)."""
    out = bytearray(HEADER.pack(MAGIC, VERSION, len(records), bytes.fromhex(fingerprint_hex), int(known), 0))
    for record in records:
        out += NAMES.pack(*(text.encode('ascii') for text in (record['name'], record['switch'], record['group'])))
        anchor = record['anchor']
        out += FIXED.pack(MODES[record['mode']], len(record['pattern']), record['expected'], record['range_len'],
                          ANCHOR_NONE if anchor is None else ANCHOR_FUNCTION, 0 if anchor is None else anchor)
        out += struct.pack('<I', len(record['deps'])) + struct.pack(f'<{len(record["deps"])}I', *record['deps'])
        out += struct.pack('<I', len(record['derived']))
        for item in record['derived']:
            out += DERIVED.pack(item['field'].encode('ascii'), item['offset'], item['size'],
                                KINDS[item['kind']], item['insn_end'])
        out += record['pattern'] + record['mask']
    return bytes(out)


def decode(data):
    """Parses SOTCHOOK1 bytes back into {'fingerprint', 'known', 'records'} (the records of build())."""
    if len(data) < HEADER.size:
        raise ValueError('hooks file is truncated')
    magic, version, count, fingerprint_raw, known, _ = HEADER.unpack_from(data, 0)
    if magic != MAGIC or version != VERSION:
        raise ValueError('not a SOTCHOOK1 file')
    pos = HEADER.size

    def take(size):
        nonlocal pos
        if pos + size > len(data):
            raise ValueError('hooks file is truncated')
        chunk = bytes(data[pos:pos + size])
        pos += size
        return chunk

    records = []
    for _ in range(count):
        name, switch, group = (text.rstrip(b'\0').decode('ascii') for text in NAMES.unpack(take(NAMES.size)))
        mode, pattern_len, expected, range_len, anchor_kind, anchor_index = FIXED.unpack(take(FIXED.size))
        dep_count, = struct.unpack('<I', take(4))
        deps = list(struct.unpack(f'<{dep_count}I', take(4 * dep_count)))
        derived = []
        derived_count, = struct.unpack('<I', take(4))
        for _ in range(derived_count):
            field, offset, size, kind, insn_end = DERIVED.unpack(take(DERIVED.size))
            derived.append(dict(field=field.rstrip(b'\0').decode('ascii'), offset=offset, size=size,
                                kind=KIND_NAMES[kind], insn_end=insn_end))
        pattern = take(pattern_len)
        mask = take(pattern_len)
        records.append(dict(name=name, switch=switch, group=group, mode=MODE_NAMES[mode], pattern=pattern,
                            mask=mask, expected=expected, range_len=range_len,
                            anchor=anchor_index if anchor_kind == ANCHOR_FUNCTION else None,
                            deps=deps, derived=derived))
    if pos != len(data):
        raise ValueError('hooks file has bytes after the last record')
    return dict(fingerprint=fingerprint_raw.hex(), known=bool(known), records=records)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path, help='the game folder (with eboot.bin) or an eboot file')
    parser.add_argument('--out', type=Path, default=PORT / 'out')
    parser.add_argument('--hooks-dir', type=Path, default=HOOKS_DIR, help=argparse.SUPPRESS)
    parser.add_argument('--include-examples', action='store_true',
                        help='also load the files whose names start with "_"')
    args = parser.parse_args(argv)
    eboot = args.game / 'eboot.bin' if args.game.is_dir() else args.game
    try:
        fingerprint_hex = fingerprint(eboot.read_bytes())
        records, known = build(load_declarations(args.hooks_dir, args.include_examples), fingerprint_hex)
    except (DeclarationError, ValueError, TypeError) as error:
        print(f'sotc_hooks: {error}', file=sys.stderr)
        return 1
    args.out.mkdir(parents=True, exist_ok=True)
    target = args.out / 'hooks.bin'
    target.write_bytes(encode(fingerprint_hex, known, records))
    print(f'Engine hooks: {len(records)} declared, eboot {fingerprint_hex[:12]} '
          f'({"declared offsets apply" if known else "no declared offsets for this eboot"}) -> {target}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
