#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""sotc: the slow stretches of a play session, one line each (T48, area changes).

    python tools/entry_report.py [user/last_run.log]

Reads a session log made with SOTC_DRAW_SPLIT=1 (the launcher's user/last_run.log) and groups the
5 s windows that had frames over 50 ms into stretches (an area entrance shows as one). For each
stretch: when, the frames over 50 ms, the worst frame, and per slow frame (over 40 ms) the draw
thread's CPU and the parts that weigh most. The player plays the route the usual way; the
stretches are compared entrance by entrance, not by fixed times.
"""
import re
import sys
from pathlib import Path

FPS = re.compile(r'^SOTC FPS ([\d.]+) at (\d+)s; (\d+) cmds/frame; readbacks (\d+)/s')
FRAMES = re.compile(r'^SOTC FRAMES .*worst ([\d.]+) ms.*>50 ms: (\d+)')
SLOW = re.compile(r'^SOTC DRAW SPLIT frames > 40 ms: (\d+), per frame: (.*)$')
LOAD = re.compile(r'^\[(\d\d):(\d\d):(\d\d)\.\d+\] \[GAME +\] Carregando')
PARTS = {
    'draw': r'draw cpu ([\d.]+) ms',
    'cp': r'cp cpu ([\d.]+)',
    'bind': r'bind ([\d.]+) \[',
    'clamp': r'clamp ([\d.]+)',
    'obtain': r'obtain ([\d.]+)',
    'tex': r'textures ([\d.]+) \(',
    'upload': r'texture uploads ([\d.]+)',
    'apply': r'apply ([\d.]+)',
    'dirty': r'dirty reread ([\d.]+)',
    'gpuwait': r'draw waits gpu ([\d.]+)',
    'rbwait': r'guest readback waits ([\d.]+)',
    'queue': r'queue max (\d+)',
}


def main():
    path = Path(sys.argv[1] if len(sys.argv) > 1 else 'user/last_run.log')
    windows, loads = [], []
    pending = {'over50': 0, 'worst': 0.0, 'slow': 0, 'parts': {}}
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        if m := LOAD.match(line):
            loads.append(int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)))
        elif m := FRAMES.match(line):
            pending['worst'] = float(m.group(1))
            pending['over50'] = int(m.group(2))
        elif m := SLOW.match(line):
            pending['slow'] = int(m.group(1))
            for name, rx in PARTS.items():
                if p := re.search(rx, m.group(2)):
                    pending['parts'][name] = float(p.group(1))
        elif m := FPS.match(line):
            windows.append({'t': int(m.group(2)), 'fps': float(m.group(1)), 'cmds': int(m.group(3)),
                            'rb': int(m.group(4)), **pending})
            pending = {'over50': 0, 'worst': 0.0, 'slow': 0, 'parts': {}}
    stretches, current = [], []
    for w in windows:
        if w['over50'] or (w['slow'] >= 3):
            current.append(w)
        elif current:
            stretches.append(current)
            current = []
    if current:
        stretches.append(current)
    total50 = sum(w['over50'] for w in windows)
    print(f'{path}: {len(windows)} windows of 5 s, {total50} frames over 50 ms, area loads at '
          + (', '.join(f'{t}s' for t in loads) or 'none'))
    print('stretch  time       >50ms  worst   per slow frame (ms): draw cp | bind clamp obtain tex '
          'upload | apply dirty gpuwait | guest-rb queue  rb/s')
    for i, s in enumerate(stretches, 1):
        slow = sum(w['slow'] for w in s)
        avg = {}
        for name in PARTS:
            weights = [(w['parts'][name], w['slow']) for w in s if name in w['parts'] and w['slow']]
            n = sum(k for _, k in weights)
            avg[name] = sum(v * k for v, k in weights) / n if n else 0.0
        t0, t1 = s[0]['t'] - 5, s[-1]['t']
        print(f'{i:>7}  {t0:>4}-{t1:<4}s {sum(w["over50"] for w in s):>5}  {max(w["worst"] for w in s):>5.0f}'
              f'   {avg["draw"]:5.1f} {avg["cp"]:5.1f} | {avg["bind"]:4.1f} {avg["clamp"]:4.1f} '
              f'{avg["obtain"]:4.1f} {avg["tex"]:4.1f} {avg["upload"]:5.1f} | {avg["apply"]:4.1f} '
              f'{avg["dirty"]:4.1f} {avg["gpuwait"]:4.1f} | {avg["rbwait"]:5.1f} {avg["queue"]:5.0f} '
              f'{max(w["rb"] for w in s):5d}   ({slow} frames > 40 ms)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
