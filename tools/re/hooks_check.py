#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Resolves every engine hook of scripts/hooks.d against an eboot, with the loader's rules
(docs/HOOK-DESIGN.md). Offline: it does not start the game.

    python tools/re/hooks_check.py                   out/eboot.elf (the prepared image)
    python tools/re/hooks_check.py --eboot PATH      an ELF, an eboot.bin or a game folder (another release)
    python tools/re/hooks_check.py --include-examples    also the files named _*.py (scripts/hooks.d/_example.py)

Rules, the same as the loader's:
- when the fingerprint has an expected offset, the pattern must match there (fully inside an executable
  segment, and inside the anchor's function when anchored); otherwise
- the pattern is searched in every executable segment and must match exactly once (overlapping matches
  count), inside the anchor's function when anchored;
- a hook is off when its anchor or a dependency is off, or a derived field cannot be read.

Prints one line per hook, "SOTC HOOK <name> 0x<site> expected|scanned|off(<reason>)", the form of the
loader's manifest, and exits 1 unless every hook resolves to exactly one site.
"""
import argparse
import bisect
import contextlib
import re
import sys
from collections import namedtuple
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'tools' / 're'))

import eboot_re  # noqa: E402  (segments and function ranges of the eboot)
import sotc_hooks  # noqa: E402
from prepare import parse_self  # noqa: E402

PF_X = 1
Resolution = namedtuple('Resolution', 'name state site reason values range_end')


class Segments:
    """The loadable segments of an eboot in memory and its function ranges.

    segments: (vaddr, bytes, executable) tuples; functions: sorted (start, end) tuples."""

    def __init__(self, segments, functions=()):
        self.segments = list(segments)
        self.functions = sorted(functions)
        self._starts = [start for start, _ in self.functions]

    def read(self, va, size):
        for base, data, _ in self.segments:
            if base <= va < base + len(data):
                return bytes(data[va - base:va - base + size])
        return b''

    def code(self):
        return [(base, data) for base, data, executable in self.segments if executable]

    def function_of(self, va):
        i = bisect.bisect_right(self._starts, va) - 1
        if i >= 0 and self.functions[i][0] <= va < self.functions[i][1]:
            return self.functions[i]
        return None


def find_sites(image, pattern, mask):
    """Every image offset in an executable segment where the pattern matches (overlaps included)."""
    body = b''.join(b'.' if m == 0 else re.escape(bytes([p])) for p, m in zip(pattern, mask))
    matcher = re.compile(b'(?=' + body + b')', re.S)
    sites = []
    for base, data in image.code():
        sites += [base + m.start() for m in matcher.finditer(data)]
    return sites


def matches_at(image, va, pattern, mask):
    for base, data in image.code():
        if base <= va and va + len(pattern) <= base + len(data):
            off = va - base
            return all(m == 0 or data[off + i] == p for i, (p, m) in enumerate(zip(pattern, mask)))
    return False


def _signed(raw):
    return int.from_bytes(raw, 'little', signed=True)


def derive(image, site, record):
    """(values, error): the derived fields of a resolved site. disp is signed, imm unsigned,
    rip_target the image offset match + insn_end + displacement."""
    values = {}
    for item in record['derived']:
        raw = image.read(site + item['offset'], item['size'])
        if len(raw) != item['size']:
            return {}, f'derived {item["field"]} unreadable'
        if item['kind'] == 'imm':
            values[item['field']] = int.from_bytes(raw, 'little')
        elif item['kind'] == 'disp':
            values[item['field']] = _signed(raw)
        else:
            values[item['field']] = site + item['insn_end'] + _signed(raw)
    return values, ''


def resolve(records, image):
    """One Resolution per record, in record order. Anchors and deps resolve first."""
    memo = {}

    def visit(i):
        if i not in memo:
            memo[i] = _resolve_one(records, i, image, visit)
        return memo[i]

    return [visit(i) for i in range(len(records))]


def _resolve_one(records, i, image, visit):
    record = records[i]
    name = record['name']

    def off(reason, site=0):
        return Resolution(name, 'off', site, reason, {}, 0)

    for dep in record['deps']:
        if visit(dep).state == 'off':
            return off(f'dependency {records[dep]["name"]} off')
    function = None
    if record['anchor'] is not None:
        anchor = visit(record['anchor'])
        if anchor.state == 'off':
            return off(f'anchor {anchor.name} off')
        function = image.function_of(anchor.site)
        if function is None:
            return off(f'anchor {anchor.name} is not in a function')

    def inside(va):
        return function is None or function[0] <= va < function[1]

    pattern, mask = record['pattern'], record['mask']
    expected = record['expected']
    if expected and inside(expected) and matches_at(image, expected, pattern, mask):
        site, state = expected, 'expected'
    else:
        hits = [va for va in find_sites(image, pattern, mask) if inside(va)]
        if len(hits) != 1:
            where = ' in the anchor function' if function else ''
            return off(f'{len(hits)} matches{where}')
        site, state = hits[0], 'scanned'
    range_end = 0
    if record['mode'] == 'range':
        if len(image.read(site, record['range_len'])) != record['range_len']:
            return off('range leaves its segment', site)
        range_end = site + record['range_len']
    values, error = derive(image, site, record)
    if error:
        return off(error, site)
    return Resolution(name, state, site, '', values, range_end)


def format_line(record, result):
    if result.state == 'off':
        return f'SOTC HOOK {result.name} 0x0 off({result.reason})'
    fields = ''.join(f' {key}={value:#x}' if value >= 0 else f' {key}=-{-value:#x}'
                     for key, value in result.values.items())
    line = f'SOTC HOOK {result.name} {result.site:#x} {result.state} mode={record["mode"]}'
    return line + (f' derived:{fields}' if fields else '')


@contextlib.contextmanager
def _eboot_elf_source(elf):
    """Lets eboot_re.Image read these bytes instead of out/eboot.elf."""
    class Source:
        def read_bytes(self):
            return elf

    saved = eboot_re.ELF
    eboot_re.ELF = Source()
    try:
        yield
    finally:
        eboot_re.ELF = saved


def load_image(data):
    """(Segments, fingerprint) of an eboot given as bytes (a SELF or a plain ELF)."""
    elf, _, ph, _, _ = parse_self(data)
    fingerprint = sotc_hooks.fingerprint_of(elf, ph)
    with _eboot_elf_source(bytes(elf)):
        img = eboot_re.Image()
        try:
            functions = img.functions()
        except Exception as error:  # a damaged dump: its eh_frame_hdr is not readable
            raise ValueError(f'the eboot has no readable function table ({error!r}); '
                             'is the dump complete?') from None
    segments = [(vaddr, bytes(img.data[off:off + filesz]), bool(flags & PF_X))
                for vaddr, filesz, off, flags in img.segs]
    return Segments(segments, functions), fingerprint


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--eboot', type=Path, default=ROOT / 'out' / 'eboot.elf',
                        help='an ELF, an eboot.bin or a game folder (default out/eboot.elf)')
    parser.add_argument('--hooks-dir', type=Path, default=sotc_hooks.HOOKS_DIR, help=argparse.SUPPRESS)
    parser.add_argument('--include-examples', action='store_true',
                        help='also check the files whose names start with "_"')
    args = parser.parse_args(argv)
    eboot = args.eboot / 'eboot.bin' if args.eboot.is_dir() else args.eboot
    if not eboot.is_file():
        print(f'hooks_check: {eboot} not found (run sotc.py once to prepare out/eboot.elf, or pass --eboot)',
              file=sys.stderr)
        return 2
    try:
        image, fingerprint = load_image(eboot.read_bytes())
        declarations = sotc_hooks.load_declarations(args.hooks_dir, args.include_examples)
        records, known = sotc_hooks.build(declarations, fingerprint)
        # The loader reads the file, so check what it reads.
        decoded = sotc_hooks.decode(sotc_hooks.encode(fingerprint, known, records))['records']
    except (sotc_hooks.DeclarationError, ValueError) as error:
        print(f'hooks_check: {error}', file=sys.stderr)
        return 2
    results = resolve(decoded, image)
    for record, result in zip(decoded, results):
        print(format_line(record, result))
    failed = sum(1 for result in results if result.state == 'off')
    print(f'hooks_check: {len(results) - failed}/{len(results)} hooks resolve to one site; '
          f'eboot {fingerprint[:12]} ({"declared offsets checked" if known else "no declared offsets for this eboot"})')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
