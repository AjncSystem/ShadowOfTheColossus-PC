#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Describe the CPU patches for the loader (out/sotc-cpu.bin, format SOTCCPU2).

The eboot's PT_GNU_EH_FRAME search table gives the function starts that the SSE4a and
red-zone patchers decode (gpu/sotc/sotc_patches.cpp). The red-zone functions are the ones
the shadPS4 SotC fork protects for Shadow of the Colossus EU 1.01 (CUSA08809). The linked
modules' tables (out/link.json, from link_modules.py) follow, for the BMI1/LZCNT/MOVBE patches
of their code on CPUs without those instructions.
"""
import argparse
import json
import struct
from pathlib import Path
from prepare import parse_self, sfo

PT_GNU_EH_FRAME = 0x6474e550
RED_ZONE_FUNCTIONS = {
    # Keeps pointers at [rsp-0x8..-0x18] across a loop.
    'CUSA08809': (0xEDFB30,
                  # A tracked-page write at +0xF67EF7 clobbers a pointer kept below rsp.
                  0xF67D2B,
                  # Leaf keeps its output pointer at [rsp-0x10] while reading tracked memory.
                  0xF52530),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'out')
    args = parser.parse_args()
    _, _, ph, _, _ = parse_self((args.game / 'eboot.bin').read_bytes())
    eh = next((p for p in ph if p['type'] == PT_GNU_EH_FRAME), None)
    loads = [p for p in ph if p['type'] in (1, 0x61000010)]
    if eh and not any(p['vaddr'] <= eh['vaddr'] and eh['vaddr'] + eh['memsz'] <= p['vaddr'] + p['filesz'] for p in loads):
        raise SystemExit('PT_GNU_EH_FRAME is not inside a loaded segment')
    serial = sfo((args.game / 'sce_sys/param.sfo').read_bytes()).get('TITLE_ID', '')
    functions = RED_ZONE_FUNCTIONS.get(serial, ())
    link = args.out / 'link.json'
    modules = json.loads(link.read_text(encoding='utf-8')).get('modules', []) if link.is_file() else []
    tables = [m['eh_frame_hdr'] for m in modules if m.get('eh_frame_hdr', [0, 0])[1]]
    with (args.out / 'sotc-cpu.bin').open('wb') as f:
        f.write(struct.pack('<8sQQQ', b'SOTCCPU2', eh['vaddr'] if eh else 0, eh['memsz'] if eh else 0, len(functions)))
        for offset in functions:
            f.write(struct.pack('<Q', offset))
        f.write(struct.pack('<Q', len(tables)))
        for offset, size in tables:
            f.write(struct.pack('<QQ', offset, size))
    print(f'CPU patches: {serial}, eh_frame_hdr={eh["vaddr"]:#x}+{eh["memsz"]:#x}, '
          f'{len(functions)} red-zone functions, {len(tables)} module tables')


if __name__ == '__main__':
    main()
