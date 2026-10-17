#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Copies a SotC save and moves where Wander (and Agro) start, for benchmarks in other places.

The progress save (SAVEDATA0ShadowProgress0/USR-DATA) is a plain record list. Wander's start is
the record with id 0x60 (x, y, z float32 followed by two angles), Agro's the one with id 0x78
right after it. The save's z axis is the renderer's -z (render position = (x, y, -z)). The header
holds a FNV-1a 32 hash of everything after its 16 bytes (at offset 4) and that size (offset 8).

    python tools/save_place.py <source save folder> <new folder> <x> <y> <z> [--render]

--render: x y z are renderer coordinates (as SOTC_CAM_LOG prints them).
"""
import argparse
import shutil
import struct
from pathlib import Path

WANDER_ID, AGRO_ID = 0x60, 0x78


def fnv1a(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def find_record(data, record_id):
    for off in range(0, len(data) - 16, 4):
        if struct.unpack_from('<I', data, off)[0] == record_id:
            x, y, z = struct.unpack_from('<3f', data, off + 4)
            if all(v == v and abs(v) < 10000 for v in (x, y, z)) and abs(x) + abs(z) > 1:
                return off + 4
    raise SystemExit(f'record {record_id:#x} not found')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('source')
    parser.add_argument('dest')
    parser.add_argument('x', type=float)
    parser.add_argument('y', type=float)
    parser.add_argument('z', type=float)
    parser.add_argument('--render', action='store_true')
    args = parser.parse_args()
    x, y, z = args.x, args.y, (-args.z if args.render else args.z)
    shutil.rmtree(args.dest, ignore_errors=True)
    shutil.copytree(args.source, args.dest)
    root = Path(args.dest) / 'CUSA08809'
    for path in (root / 'SAVEDATA0ShadowProgress0' / 'USR-DATA',
                 root / 'SAVEDATA0ShadowProgress0.sce_backup' / 'data' / 'USR-DATA'):
        data = bytearray(path.read_bytes())
        wander = find_record(data, WANDER_ID)
        agro = find_record(data, AGRO_ID)
        old = struct.unpack_from('<3f', data, wander)
        struct.pack_into('<3f', data, wander, x, y, z)
        struct.pack_into('<3f', data, agro, x + 0.3, y - 1.2, z - 6.6)  # as in the original save
        struct.pack_into('<I', data, 4, fnv1a(bytes(data[16:])))
        path.write_bytes(bytes(data))
        print(f'{path}: Wander {old} -> {(x, y, z)}')


if __name__ == '__main__':
    main()
