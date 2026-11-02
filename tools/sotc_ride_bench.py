#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""sotc: world-streaming benchmark - Wander is carried along a fixed ~5.8 km loop through the
Forbidden Land by the engine's own setPlayerPos console command (an automation plan run through
tools/sotc_route.py --plan), so every run streams the same areas in the same order with no pad
input and no horse AI to drift.

    python tools/sotc_ride_bench.py --install DIR [--runs 3] [--env NAME=VALUE ...]
    python tools/sotc_ride_bench.py --write-plan FILE      (just write the plan)

Wander is held 80 m above the shrine level (above the terrain on this loop) and moved 2.5 m per
step; each step costs about a frame, so the loop takes ~280 s at ~15 m/s, gallop speed. Landing
from that height would kill him (fall damage), so he is only set down at the shrine at both ends,
at ground level. The plan has a fixed number of steps, so every run logs the same frame count.

Each run prints frame-time stats between the first and last printPlayerPos of the plan (the
ride itself): p50/p99, the count of frames over 25/33/50/100 ms and the worst one. Runs 1 after a
shader-cache change compile new pipelines; compare warm runs.
"""
import argparse
import math
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# (x, z) waypoints in engine coordinates (printPlayerPos): shrine, east, north, south-west,
# south, shrine.
WAYPOINTS = [(298, -198), (1387, -324), (294, 296), (-831, -996), (269, -909), (298, -198)]
SHRINE = 'setPlayerPos 298 -6 -198'
HEIGHT = 80
STEP = 2.5
FRAME = re.compile(r'SOTCFRAME \d+ t=\d+ms us=(\d+)')


def plan_text():
    lines = ['*stage "Ride"', 'waitForGameToStart', 'wait 8', SHRINE, 'wait 10', 'printPlayerPos']
    for (ax, az), (bx, bz) in zip(WAYPOINTS, WAYPOINTS[1:]):
        n = max(1, round(math.hypot(bx - ax, bz - az) / STEP))
        for i in range(n):
            lines += [f'setPlayerPos {ax + (bx - ax) * i / n:.1f} {HEIGHT} {az + (bz - az) * i / n:.1f}',
                      'wait 0.1']
    lines += [SHRINE, 'wait 3', 'printPlayerPos', '*endstage']
    return '\n'.join(lines) + '\n'


def ride_stats(log):
    lines = Path(log).read_text(encoding='utf-8', errors='replace').splitlines()
    marks = [i for i, line in enumerate(lines) if 'Player Position:' in line]
    if len(marks) < 2:
        return 'no ride in the log'
    us = [int(m.group(1)) for line in lines[marks[0]:marks[-1]] if (m := FRAME.match(line))]
    if len(us) < 100:
        return 'no frame log (SOTC_FRAME_LOG)'
    seq = [(b - a) / 1000 for a, b in zip(us, us[1:])]
    d = sorted(seq)
    n = len(d)
    p = lambda q: d[min(n - 1, int(q * n))]  # noqa: E731
    over = lambda ms: sum(x > ms for x in d)  # noqa: E731
    jitter = sum(abs(b - a) for a, b in zip(seq, seq[1:])) / (n - 1)
    return (f'fps {n / ((us[-1] - us[0]) / 1e6):.1f} p50 {p(.5):.1f} p99 {p(.99):.1f} worst {d[-1]:.0f} ms | '
            f'>25 {over(25)} >33 {over(33.4)} >50 {over(50)} >100 {over(100)} | jitter {jitter:.2f} ms ({n} frames)')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--install', help='the port folder (launcher/, user/)')
    parser.add_argument('--runs', type=int, default=1)
    parser.add_argument('--env', action='append', default=[])
    parser.add_argument('--write-plan', help='write the plan to this file and exit')
    parser.add_argument('--stats', help='print the stats of an existing log and exit')
    args = parser.parse_args()
    if args.write_plan:
        Path(args.write_plan).write_text(plan_text())
        return 0
    if args.stats:
        print(ride_stats(args.stats))
        return 0
    if not args.install:
        parser.error('--install is needed')
    with tempfile.TemporaryDirectory() as tmp:
        plan = Path(tmp) / 'ride.txt'
        plan.write_text(plan_text())
        for run in range(1, args.runs + 1):
            cmd = [sys.executable, str(ROOT / 'tools' / 'sotc_route.py'), '--install', args.install,
                   '--plan', str(plan), '--timeout', '900']
            for e in args.env:
                cmd += ['--env', e]
            out = subprocess.run(cmd, capture_output=True, text=True, errors='replace').stdout
            done = re.search(r'plan done, log (.+)', out)
            if not done:
                print(f'run {run}: INVALID {out.strip().splitlines()[-1:] or ""}', flush=True)
                continue
            print(f'run {run}: {ride_stats(done.group(1).strip())}  [{done.group(1).strip()}]', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
