#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarises gameplay_bench --explore runs: each 5 s FPS sample with where Wander was (SOTC CAM).

    python tools/explore_report.py out/run-exp*.txt
"""
import re
import sys

CAM = re.compile(r'SOTC CAM (\d+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)')
FPS = re.compile(r'SOTC FPS ([\d.]+) at (\d+)s; (\d+) cmds/frame')
FRAMES = re.compile(r'SOTC FRAMES p50 ([\d.]+) ms, p99 ([\d.]+) ms')


def samples(path):
    text = open(path, 'rb').read().decode('utf-8', 'replace').replace('\r', '')
    _, _, tail = text.partition('st_gameflow() starting game')
    pos = start = None
    out = []
    p99 = None
    for line in tail.split('\n'):
        m = CAM.search(line)
        if m:
            pos = (float(m[2]), float(m[3]), float(m[4]))
            if start is None:
                start = pos
            continue
        m = FRAMES.search(line)
        if m:
            p99 = float(m[2])
            continue
        m = FPS.search(line)
        if m and pos and start:
            dist = ((pos[0] - start[0]) ** 2 + (pos[2] - start[2]) ** 2) ** 0.5
            out.append({'fps': float(m[1]), 'cmds': int(m[3]), 'pos': pos, 'dist': dist, 'p99': p99})
    return out


def main():
    allrows = []
    for path in sys.argv[1:]:
        rows = samples(path)[2:]  # the first 10 s: loading, settling
        if not rows:
            print(path, 'no samples')
            continue
        name = path.split('run-')[-1].rsplit('.', 1)[0]
        fps = [r['fps'] for r in rows]
        far = max(r['dist'] for r in rows)
        print(f'{name}: {len(rows)} samples, avg {sum(fps) / len(fps):.1f}, min {min(fps):.1f}, '
              f'max {max(fps):.1f}, went {far:.0f} m')
        for lo, hi in ((0, 150), (150, 400), (400, 800), (800, 99999)):
            seg = [r for r in rows if lo <= r['dist'] < hi]
            if seg:
                f = [r['fps'] for r in seg]
                c = [r['cmds'] for r in seg]
                print(f'   {lo:>4}-{hi if hi < 99999 else "":<4} m: avg {sum(f) / len(f):5.1f} min '
                      f'{min(f):5.1f} ({len(seg)} samples, {sum(c) / len(c):.0f} cmds/frame, '
                      f'height {min(r["pos"][1] for r in seg):.0f}..{max(r["pos"][1] for r in seg):.0f})')
        for r in rows:
            r['run'] = name
        allrows += rows
    if allrows:
        worst = sorted(allrows, key=lambda r: r['fps'])[:8]
        print('worst 5 s windows:')
        for r in worst:
            print(f"   {r['fps']:5.1f} FPS {r['run']} at {r['dist']:.0f} m from the start, "
                  f"{r['cmds']} cmds/frame, p99 {r['p99']} ms, pos {tuple(round(v) for v in r['pos'])}")
        f = [r['fps'] for r in allrows]
        print(f'all: {len(f)} samples, avg {sum(f) / len(f):.1f}, min {min(f):.1f}, '
              f'{100 * sum(x >= 40 for x in f) / len(f):.0f}% >= 40, '
              f'{100 * sum(x >= 30 for x in f) / len(f):.0f}% >= 30')


if __name__ == '__main__':
    main()
