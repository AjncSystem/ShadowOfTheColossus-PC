#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""sotc: game modes and engine settings for Shadow of the Colossus EU 1.01 (CUSA08809).

    python scripts/sotc_modes.py GAME --out out --modes af16,fps60 --cvars "r_motionBlurEnable=0 ..."

Writes two files for the loader:
  out/patches.bin  code patches (BBPATCH2, bb-probe --patches):
                   fps60  removes the game's 30 FPS cap (illusion's GoldHEN patch, the shadPS4
                          SotC fork's "SotC-Modes")
                   fps60game  the game's own 60 FPS target (PS4 Pro Performance mode): it asks for
                          60 FPS and its timing follows
                   --render WxH (e.g. 3840x1600)  the game renders at this size instead of
                          1920x1080, with its aspect for the cameras (Hor+: a wider view, not a
                          stretched one); PS4 mode only, the PS4 Pro path keeps its sizes
  out/cmdline.bin  the engine command line the game appends to its own (SOTCCMD1,
                   bb-probe --sotc-cmdline): cvars of the Bluepoint engine, e.g.
                   +r_motionBlurEnable=0. Modes that are only cvars:
                   af16   16x anisotropic filtering on every texture
The original bytes are checked first: another game version gets neither file's changes.
"""
import argparse
import re
from pathlib import Path
import struct
from prepare import parse_self

BASE = 0x400000
# The lea that loads the string the game appends to its command line ("+VSyncWindow=10%").
CMDLINE_LEA = 0x009367b9
CMDLINE_ORIGINAL = '+VSyncWindow=10%'
# Code patches: mode -> [(address, expected original bytes, new bytes)]
MODES = {
    'fps60': [
        # the frame limiter's import stub (sceVideoOutSetFlipRate) returns 0
        (0x01390090, bytes.fromhex('ff25a20567'), bytes.fromhex('31c0c3')),
    ],
    # The engine's own 60 FPS target, the one of the PS4 Pro Performance mode: TargetFramesPerSeconds
    # 1 (60 FPS) where the game sets 2 (30 FPS), i.e. the graphics mode code on a PS4 (0x70a6d0) and
    # the options code without the Performance mode (0x49a2d8). The game then asks flip rate 0 itself
    # and its timing code (display+0x28: 59.94 instead of 29.97) keeps step: without it, some physics
    # and timers run twice as fast at 60 FPS (AjncSystem #100, #120, #49).
    'fps60game': [
        (0x00b0a7a6, bytes.fromhex('c745b802000000'), bytes.fromhex('c745b801000000')),
        (0x0089a47f, bytes.fromhex('c7458402000000'), bytes.fromhex('c7458401000000')),
        (0x0089a499, bytes.fromhex('c7458002000000'), bytes.fromhex('c7458001000000')),
    ],
}
# Modes made of cvars only.
CVAR_MODES = {
    'fps60game': ['TargetFramesPerSeconds=1'],  # until the options code runs (MODES above)
    'af16': ['progSamplerAnisoLevel=16', 'progSamplerAnisoLevel_BasePS4=16', 'progSamplerAnisoLevel_Perf=16',
             'progSamplerAnisoLevel_Cinematic=16', 'linearSamplerAnisoLevel=16'],
}
# Render size (PS4 mode; the PS4 Pro values next to them stay): (address, kind, original).
# 'W'/'H' an integer immediate (the last 4 bytes of the instruction at address), 'w'/'h' a float
# (the PS4 entry of a [PS4, Pro] table), 'w2'/'h2' half of it, 'a' the aspect float W/H,
# 'P' the render target pool (322.5 MiB at 1080p, scaled by the pixel count).
RENDER_SITES = [
    # display init (0x495a90): the aspect of the display object, back buffer, display targets,
    # scene size
    (0x00495b3e, 'a', 'c74304398ee33f'),
    (0x00495bec, 'H', 'b938040000'), (0x00495bf7, 'W', 'b880070000'),
    (0x00495c91, 'W', 'ba80070000'), (0x00495c96, 'H', 'b938040000'),
    (0x00495d34, 'W', 'ba80070000'), (0x00495d39, 'H', 'b938040000'),
    (0x00495d61, 'W', 'b880070000'), (0x00495d7c, 'H', 'b938040000'),
    # the render target allocator: its check for the PS4 size, its pool (PS4 Pro: 573 MiB)
    (0x0049b693, 'W', '3d80070000'), (0x0049b6c6, 'H', '817c242038040000'),
    (0x0049b74f, 'P', 'be00002814'),
    # render target sizes
    (0x004a2ae9, 'W', '41be80070000'), (0x004a2af2, 'H', '41bf38040000'),
    # display width / height getters
    (0x0056f2f3, 'W', 'b880070000'), (0x0056f313, 'H', 'b838040000'),
    (0x00662ac6, 'W', 'c705109ac80280070000'), (0x00662ad0, 'H', 'c7050a9ac80238040000'),
    # viewport descriptors {W, H, ...}
    (0x00ab5888, 'W', 'c7054a2db80280070000'), (0x00ab5899, 'H', 'c7053d2db80238040000'),
    (0x00ab5978, 'W', 'c70780070000'), (0x00ab597e, 'H', 'c7470438040000'),
    (0x00ab6001, 'W', 'c70080070000'), (0x00ab6007, 'H', 'c7400438040000'),
    # float tables [PS4, Pro]: pixel sizes and screen centres
    (0x013c34a0, 'w', '0000f044'), (0x013c34a8, 'h', '00008744'),
    (0x013d7518, 'w', '0000f044'), (0x013d7520, 'h', '00008744'),
    (0x01667f14, 'w', '0000f044'), (0x01667f1c, 'h', '00008744'),
    (0x01667f24, 'w', '0000f044'), (0x01667f2c, 'h', '00008744'),
    (0x01667f34, 'w', '0000f044'), (0x01667f3c, 'h', '00008744'),
    (0x01667f44, 'w2', '00007044'), (0x01667f4c, 'w2', '00007044'),
    (0x01667f54, 'w', '0000f044'), (0x01667f5c, 'h', '00008744'),
    (0x01667f64, 'w2', '00007044'), (0x01667f6c, 'h2', '00000744'),
    (0x01667f74, 'w2', '00007044'), (0x01667f7c, 'h2', '00000744'),
    (0x01667f84, 'w', '0000f044'), (0x01667f8c, 'h', '00008744'),
    (0x01667f94, 'w2', '00007044'), (0x01667f9c, 'h2', '00000744'),
    # the default camera's aspect (16/9)
    (0x0166633c, 'a', '398ee33f'),
]
RT_POOL = 0x14280000
# EU 1.01: the command line lea and the fps60 site.
VERSION_CHECKS = [(CMDLINE_LEA, bytes.fromhex('488d352debad00')), (0x01390090, bytes.fromhex('ff25a20567'))]
CVAR = re.compile(r'^[A-Za-z_][A-Za-z0-9_]*(=[-+.0-9A-Za-z_%]+)?$')  # NAME=VALUE or a flag such as nointro


def image_bytes(elf, ph, vaddr, length):
    for p in ph:
        if p['type'] in (1, 0x61000010) and p['vaddr'] <= vaddr and vaddr + length <= p['vaddr'] + p['filesz']:
            offset = p['offset'] + vaddr - p['vaddr']
            return bytes(elf[offset:offset + length])
    return None


def is_supported(elf, ph):
    return all(image_bytes(elf, ph, address - BASE, len(original)) == original for address, original in VERSION_CHECKS)


def render_pool(w, h):
    """The render target pool for WxH: the PS4 one (1080p) scaled by the pixels, 64 KiB pages."""
    pool = max(RT_POOL, RT_POOL * w * h // (1920 * 1080))
    return (pool + 0xffff) & ~0xffff


def render_patches(elf, ph, size):
    """The writes for render size WxH; SystemExit when a site does not hold the expected bytes."""
    match = re.fullmatch(r'(\d+)x(\d+)', size)
    if not match:
        raise SystemExit(f'bad render size {size!r} (WxH, e.g. 3840x1600)')
    w, h = int(match[1]), int(match[2])
    if not (640 <= w <= 7680 and 360 <= h <= 4320) or w % 8 or h % 8:
        raise SystemExit(f'render size {size}: 640x360 to 7680x4320, multiples of 8')
    values = {'W': struct.pack('<I', w), 'H': struct.pack('<I', h), 'w': struct.pack('<f', w),
              'h': struct.pack('<f', h), 'w2': struct.pack('<f', w / 2), 'h2': struct.pack('<f', h / 2),
              'a': struct.pack('<f', w / h), 'P': struct.pack('<I', render_pool(w, h))}
    writes = []
    for address, kind, original in RENDER_SITES:
        original = bytes.fromhex(original)
        if image_bytes(elf, ph, address - BASE, len(original)) != original:
            raise SystemExit(f'render size: unexpected bytes at {address:#x}')
        data = values[kind]
        writes.append((address - BASE + len(original) - len(data), data))
    return writes


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'out')
    parser.add_argument('--modes', default='', help='comma-separated: ' + ', '.join([*MODES, *CVAR_MODES]))
    parser.add_argument('--cvars', default='', help='engine cvars NAME=VALUE separated by spaces')
    parser.add_argument('--render', default='', help='render size WxH instead of 1920x1080, e.g. 3840x1600')
    args = parser.parse_args()
    elf, _, ph, _, _ = parse_self((args.game / 'eboot.bin').read_bytes())
    supported = is_supported(elf, ph)
    writes, cvars = [], []
    for mode in filter(None, (m.strip().lower() for m in args.modes.split(','))):
        if mode not in MODES and mode not in CVAR_MODES:
            raise SystemExit(f'unknown mode {mode!r} (known: {", ".join([*MODES, *CVAR_MODES])})')
        if not supported:
            print(f'Mode {mode}: this eboot is not EU 1.01, not applied')
            continue
        writes += [(address - BASE, data) for address, _, data in MODES.get(mode, [])]
        cvars += CVAR_MODES.get(mode, [])
        print(f'Mode {mode}: on')
    if args.render and args.render != '1920x1080':
        if supported:
            writes += render_patches(elf, ph, args.render)
            print(f'Render size {args.render}: on')
        else:
            print(f'Render size {args.render}: this eboot is not EU 1.01, not applied')
    for item in args.cvars.split():
        item = item.lstrip('+')
        if not CVAR.match(item):
            raise SystemExit(f'bad cvar {item!r} (NAME=VALUE or NAME)')
        cvars.append(item)
    blob = struct.pack('<8sQQ', b'BBPATCH2', BASE, len(writes))
    for offset, data in writes:
        blob += struct.pack('<QQ', offset, len(data)) + data
    (args.out / 'patches.bin').write_bytes(blob)
    cmdline = args.out / 'cmdline.bin'
    if supported and cvars:
        text = ' '.join([CMDLINE_ORIGINAL, *('+' + c for c in cvars)])
        cmdline.write_bytes(struct.pack('<8sQ', b'SOTCCMD1', CMDLINE_LEA - BASE) + text.encode('ascii'))
    elif cmdline.exists():
        cmdline.unlink()


if __name__ == '__main__':
    main()
