#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Finds Agro (the black horse) in a game frame: centroid and size of the dark pixel mass, the
overlay corner excluded. Used by gameplay_bench --horse to walk Wander up to Agro.

    python tools/agro_finder.py <image> ...   (prints what it finds, for calibration)
"""
import sys

GRID_W, GRID_H = 192, 108
DARK = 16          # luminance (0-255) below which a pixel counts as horse (Agro is near black)
MIN_PIXELS = 50    # grid pixels: smaller masses are shadows, hair, belts


def find_agro(image):
    """(x, y, size) in 0..1 screen units and grid pixels, or None."""
    small = image.convert('L').resize((GRID_W, GRID_H))
    px = small.load()
    xs = ys = n = 0
    for y in range(int(GRID_H * 0.25), GRID_H):
        for x in range(GRID_W):
            if x > GRID_W * 0.66 and y < GRID_H * 0.3:
                continue  # the performance overlay
            if px[x, y] < DARK:
                xs += x
                ys += y
                n += 1
    if n < MIN_PIXELS:
        return None
    return xs / n / GRID_W, ys / n / GRID_H, n


if __name__ == '__main__':
    from PIL import Image
    for path in sys.argv[1:]:
        print(path, find_agro(Image.open(path)))
