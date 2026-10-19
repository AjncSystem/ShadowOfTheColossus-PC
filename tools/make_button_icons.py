#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""sotc: draws the Xbox-style button icons that replace the game's PlayStation prompts.

Original art drawn here with Pillow (shapes and letters only, nothing taken from the game). Each
icon is a 128x128 RGBA PNG named after the guest hash of the game texture it replaces (the
texture cache's SOTC_TEX_REPLACE, see texture_cache.cpp), in glyphs/xbox/.

    python tools/make_button_icons.py [--out glyphs/xbox]
"""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

SIZE = 128
SCALE = 4  # drawn at 512x512, then reduced: smooth edges
GREEN, RED, BLUE, YELLOW = (96, 196, 52), (226, 52, 44), (52, 120, 222), (244, 196, 32)
BODY_TOP, BODY_BOTTOM, RIM = (58, 60, 66), (18, 19, 22), (120, 124, 132)

# Guest hash of the game's prompt texture -> the icon that replaces it.
ICONS = {
    'dbb1926987bdbf8e': ('face', 'A', GREEN),   # cross
    'f87d7fa10c4d64d9': ('face', 'B', RED),     # circle
    'cc10ab8a4ccb5c32': ('face', 'X', BLUE),    # square
    '25b54a4ac0e4e967': ('face', 'Y', YELLOW),  # triangle
    '957207112e25fe2c': ('bumper', 'RB'),       # R1
    '8359520e683c7c4c': ('trigger', 'LT'),      # L2
    'cd10bb02a59e9d9d': ('trigger', 'RT'),      # R2
    '59acf8ac22bce6e6': ('stick', 'LS'),        # L3
    '8ed179fdd7069c2a': ('stick', 'RS'),        # R3
    '202c5bb2cb1587f9': ('menu',),              # OPTIONS
    '6ff0fae0c49fb077': ('view',),              # SHARE
    'c269ad059ce3b620': ('view',),              # touch pad
}


def font(px):
    for name in ('segoeuib.ttf', 'arialbd.ttf', 'DejaVuSans-Bold.ttf'):
        try:
            return ImageFont.truetype(name, px)
        except OSError:
            continue
    return ImageFont.load_default()


def gradient(size, top, bottom):
    w, h = size
    g = Image.new('RGBA', size)
    px = g.load()
    for y in range(h):
        t = y / max(h - 1, 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)
        for x in range(w):
            px[x, y] = c
    return g


def body(mask):
    """The dark glossy button body inside `mask` (L image), with a light rim and a drop shadow."""
    n = mask.size[0]
    out = Image.new('RGBA', mask.size, (0, 0, 0, 0))
    shadow = mask.filter(ImageFilter.GaussianBlur(n // 40))
    out.paste((0, 0, 0, 150), (0, n // 60), shadow)
    rim = Image.new('RGBA', mask.size, RIM + (255,))
    out.paste(rim, (0, 0), mask)
    inner = mask.filter(ImageFilter.MinFilter(n // 48 * 2 + 1))
    out.paste(gradient(mask.size, BODY_TOP, BODY_BOTTOM), (0, 0), inner)
    # a soft highlight on the upper half
    shine = Image.new('L', mask.size, 0)
    ImageDraw.Draw(shine).ellipse((n * 0.22, n * 0.1, n * 0.78, n * 0.45), fill=46)
    shine = Image.composite(shine.filter(ImageFilter.GaussianBlur(n // 20)), Image.new('L', mask.size, 0), inner)
    out.paste((255, 255, 255, 255), (0, 0), shine)
    return out


def label(img, text, color, box, px):
    d = ImageDraw.Draw(img)
    f = font(px)
    l, t, r, b = d.textbbox((0, 0), text, font=f)
    x = (box[0] + box[2]) / 2 - (r - l) / 2 - l
    y = (box[1] + box[3]) / 2 - (b - t) / 2 - t
    d.text((x + px * 0.03, y + px * 0.05), text, font=f, fill=(0, 0, 0, 170))
    d.text((x, y), text, font=f, fill=color + (255,))


def draw(kind, *args):
    n = SIZE * SCALE
    mask = Image.new('L', (n, n), 0)
    m = ImageDraw.Draw(mask)
    if kind == 'face':
        m.ellipse((n * 0.06, n * 0.06, n * 0.94, n * 0.94), fill=255)
        img = body(mask)
        label(img, args[0], args[1], (0, 0, n, n), int(n * 0.52))
    elif kind == 'bumper':
        m.rounded_rectangle((n * 0.04, n * 0.24, n * 0.96, n * 0.76), radius=n * 0.2, fill=255)
        img = body(mask)
        label(img, args[0], (245, 245, 245), (0, n * 0.24, n, n * 0.76), int(n * 0.34))
    elif kind == 'trigger':
        m.rounded_rectangle((n * 0.16, n * 0.06, n * 0.84, n * 0.94), radius=n * 0.26, fill=255)
        m.rectangle((n * 0.16, n * 0.6, n * 0.84, n * 0.94), fill=255)
        img = body(mask)
        label(img, args[0], (245, 245, 245), (0, n * 0.12, n, n * 0.94), int(n * 0.34))
    elif kind == 'stick':
        m.ellipse((n * 0.06, n * 0.06, n * 0.94, n * 0.94), fill=255)
        img = body(mask)
        d = ImageDraw.Draw(img)
        w = n // 40
        d.ellipse((n * 0.2, n * 0.2, n * 0.8, n * 0.8), outline=(150, 154, 162, 255), width=w)
        label(img, args[0], (245, 245, 245), (0, 0, n, n), int(n * 0.3))
    elif kind == 'menu':
        m.ellipse((n * 0.14, n * 0.14, n * 0.86, n * 0.86), fill=255)
        img = body(mask)
        d = ImageDraw.Draw(img)
        for y in (0.38, 0.5, 0.62):
            d.rounded_rectangle((n * 0.33, n * (y - 0.035), n * 0.67, n * (y + 0.035)), radius=n * 0.03,
                                fill=(245, 245, 245, 255))
    elif kind == 'view':
        m.ellipse((n * 0.14, n * 0.14, n * 0.86, n * 0.86), fill=255)
        img = body(mask)
        d = ImageDraw.Draw(img)
        w = n // 34
        d.rounded_rectangle((n * 0.42, n * 0.34, n * 0.68, n * 0.56), radius=n * 0.03,
                            outline=(245, 245, 245, 255), width=w)
        d.rounded_rectangle((n * 0.32, n * 0.44, n * 0.58, n * 0.66), radius=n * 0.03,
                            fill=(40, 42, 47, 255), outline=(245, 245, 245, 255), width=w)
    else:
        raise ValueError(kind)
    return img.resize((SIZE, SIZE), Image.LANCZOS)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'glyphs' / 'xbox')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for hash_, spec in ICONS.items():
        draw(*spec).save(args.out / f'{hash_}.png', optimize=True)
    print(f'{len(ICONS)} icons in {args.out}')


if __name__ == '__main__':
    main()
