#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Describe the CPU patches for the loader (out/sotc-cpu.bin, format SOTCCPU1).

The eboot's PT_GNU_EH_FRAME search table gives the function starts that the SSE4a and
red-zone patchers decode (gpu/sotc/sotc_patches.cpp). The red-zone functions are the ones
the shadPS4 SotC fork protects for Shadow of the Colossus EU 1.01 (CUSA08809); the US 1.00 eboot
(CUSA08034) is the same build, so they apply to it too.
"""
import argparse
import struct
from pathlib import Path
from prepare import parse_self, sfo
from sotc_modes import GAMES

PT_GNU_EH_FRAME = 0x6474e550
SOTC_RED_ZONE = (
    # Keeps pointers at [rsp-0x8..-0x18] across a loop.
    0xEDFB30,
    # A tracked-page write at +0xF67EF7 clobbers a pointer kept below rsp.
    0xF67D2B,
    # Leaf keeps its output pointer at [rsp-0x10] while reading tracked memory.
    0xF52530)
RED_ZONE_FUNCTIONS = {serial: SOTC_RED_ZONE for serial in GAMES}


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
    with (args.out / 'sotc-cpu.bin').open('wb') as f:
        f.write(struct.pack('<8sQQQ', b'SOTCCPU1', eh['vaddr'] if eh else 0, eh['memsz'] if eh else 0, len(functions)))
        for offset in functions:
            f.write(struct.pack('<Q', offset))
    print(f'CPU patches: {serial}, eh_frame_hdr={eh["vaddr"]:#x}+{eh["memsz"]:#x}, '
          f'{len(functions)} red-zone functions')


if __name__ == '__main__':
    main()
