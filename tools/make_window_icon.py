#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""sotc: draws the game window's icon (original art drawn here with Pillow, nothing taken from the
game or from anyone's artwork): a young swordsman's silhouette raising his sword, whose beam of
light goes up into a pale sky. The launcher keeps the game's own icon, so the two are told apart in
the taskbar.

    python tools/make_window_icon.py   ->  launcher/window_icon.png, launcher/window_icon.ico
"""
import math
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

S = 1024
OUT = Path(__file__).resolve().parent.parent / 'launcher'
random.seed(3)
FIG = (14, 16, 22, 255)


def layer():
    return Image.new('RGBA', (S, S), (0, 0, 0, 0))


def main():
    mask = Image.new('L', (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((16, 16, S - 16, S - 16), radius=170, fill=255)
    # Sky: pale and bright around the sword, darker at the edges.
    sky = Image.new('RGBA', (S, S))
    px = sky.load()
    for y in range(S):
        for x in range(S):
            d = math.hypot(x - S * 0.6, y - S * 0.2) / S
            v = max(0.0, 1.0 - d * 1.25)
            px[x, y] = (int(70 + 175 * v), int(84 + 166 * v), int(100 + 150 * v), 255)
    img = layer()
    img.paste(sky, (0, 0), mask)

    def clip(src):
        return Image.composite(src, layer(), mask)

    # The beam of light from the sword's tip, up and out of the frame.
    beam = layer()
    ImageDraw.Draw(beam).line([(642, 70), (700, -260)], fill=(255, 252, 230, 255), width=70)
    ImageDraw.Draw(beam).ellipse((560, -20, 720, 140), fill=(255, 252, 230, 200))
    img = Image.alpha_composite(img, clip(beam.filter(ImageFilter.GaussianBlur(40))))

    # Ground: a dark ridge with grass tufts.
    ground = layer()
    gd = ImageDraw.Draw(ground)
    gd.ellipse((-300, 860, S + 300, S + 600), fill=FIG)
    for k in range(140):
        x = random.uniform(0, S)
        top = 880 + 120 * ((x - S / 2) / S) ** 2 * 4 - random.uniform(10, 60)
        gd.polygon([(x - 8, 940), (x + random.uniform(-14, 14), top), (x + 8, 940)], fill=FIG)
    img = Image.alpha_composite(img, clip(ground))

    d = ImageDraw.Draw(img)
    # The swordsman, facing away, seen from slightly below.
    cx, feet = 500, 900
    # legs (a slight stance), boots
    d.polygon([(462, 640), (504, 640), (494, feet), (446, feet)], fill=FIG)
    d.polygon([(500, 640), (544, 640), (572, feet), (524, feet)], fill=FIG)
    d.polygon([(436, feet - 8), (498, feet - 8), (498, feet + 10), (426, feet + 10)], fill=FIG)
    d.polygon([(522, feet - 8), (578, feet - 8), (588, feet + 10), (522, feet + 10)], fill=FIG)
    # tunic flaring at the hips
    d.polygon([(450, 466), (554, 466), (562, 600), (582, 672), (424, 672), (442, 600)], fill=FIG)
    # cape: from the shoulders, blown to the left
    d.polygon([(452, 470), (410, 560), (360, 706), (396, 720), (442, 644), (468, 520)], fill=FIG)
    # head: round, hair falling to the neck
    d.ellipse((458, 350, 540, 438), fill=FIG)
    d.pieslice((452, 342, 546, 430), 180, 360, fill=FIG)
    d.polygon([(458, 398), (462, 446), (482, 440), (480, 404)], fill=FIG)
    d.polygon([(484, 430), (516, 430), (520, 472), (480, 472)], fill=FIG)                    # neck
    # left arm, down and a little out
    d.polygon([(452, 474), (424, 560), (414, 642), (440, 648), (452, 572), (476, 498)], fill=FIG)
    # right arm raised, holding the sword up
    d.polygon([(528, 476), (560, 466), (610, 330), (632, 250), (606, 238), (578, 316), (536, 440)], fill=FIG)
    d.ellipse((598, 222, 642, 266), fill=FIG)                                                # fist
    # The sword: guard, a slim bright blade with a ridge, the pommel under the fist.
    d.line([(588, 238), (652, 258)], fill=FIG, width=16)                                     # guard
    d.polygon([(612, 240), (632, 246), (652, 92), (644, 62), (628, 86)], fill=(236, 240, 246, 255))
    d.line([(624, 238), (645, 72)], fill=(255, 255, 255, 255), width=4)
    d.ellipse((606, 262, 630, 286), fill=FIG)                                                # pommel

    img.save(OUT / 'window_icon.png')
    img.save(OUT / 'window_icon.ico', sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print('launcher/window_icon.png, launcher/window_icon.ico')


if __name__ == '__main__':
    main()
