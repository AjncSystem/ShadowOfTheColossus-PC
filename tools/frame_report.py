#!/usr/bin/env python3
"""Frame-time summary of a run log made with SOTC_FPS=1 (user/last_run.log, out/run.txt).

Reads the per-5 s "SOTC FRAMES" and "SOTC FPS" lines (gpu/shadps4/core/libraries/videoout/
driver.cpp) and prints: average FPS, the median of the windows' 1% lows, the worst frame and what
the command processor did during it, and the frames over 50 ms with the time they spent per
cause (shader: shader translation, pipeline: pipeline creation, upload: textures, gc: emergency texture collections,
readback: GPU-to-CPU waits, image: new images, buffer: buffer uploads and arena memory, idle: the
command processor waiting for the game, io: game file reads on all threads).

    python tools/frame_report.py out/run.txt [--from 84] [--to 124] [--top 5]

--from/--to select windows by the "at Ns" time of their FPS line (seconds since the first flip);
--top also lists the slowest windows' worst frames.
"""
import argparse
import re
import statistics
import sys

FRAMES = re.compile(r'SOTC FRAMES p50 ([\d.]+) ms, p99 ([\d.]+) ms \(1% low ([\d.]+) FPS\), '
                    r'worst ([\d.]+) ms \[([^\]]*)\]; >50 ms: (\d+) \[([^\]]*)\]; compiled (\d+)')
FPS = re.compile(r'SOTC FPS ([\d.]+) at (\d+)s')


def columns(text):
    out = {}
    for item in text.split(','):
        name, _, value = item.strip().rpartition(' ')
        if name:
            out[name] = float(value)
    return out


def windows(text):
    """Per window: (seconds, fps, worst ms, worst columns, slow count, slow columns, compiled,
    1% low). A FRAMES line precedes its window's FPS line."""
    pending = None
    for line in text.splitlines():
        if m := FRAMES.search(line):
            pending = m
        elif (m := FPS.search(line)) and pending:
            p = pending
            yield (int(m.group(2)), float(m.group(1)), float(p.group(4)), columns(p.group(5)),
                   int(p.group(6)), columns(p.group(7)), int(p.group(8)), float(p.group(3)))
            pending = None


def fmt(cols, digits=0):
    return ', '.join(f'{k} {v:.{digits}f}' for k, v in cols.items() if v >= 0.5 or k == 'idle')


def summarize(rows, top):
    if not rows:
        return 'no SOTC FRAMES lines (run with SOTC_FPS=1 on a build that prints them)'
    worst = max(rows, key=lambda r: r[2])
    slow_cols = {}
    for r in rows:
        for k, v in r[5].items():
            slow_cols[k] = slow_cols.get(k, 0.0) + v
    lows = [r[7] for r in rows]
    lines = [f'frames: avg {statistics.mean(r[1] for r in rows):.1f} FPS, 1% low '
             f'{statistics.median(lows):.1f} (min {min(lows):.1f}), worst {worst[2]:.0f} ms at '
             f'{worst[0]}s [{fmt(worst[3])}]; >50 ms frames {sum(r[4] for r in rows)} '
             f'[{fmt(slow_cols)} ms]; compiled {sum(r[6] for r in rows)} over {len(rows) * 5} s']
    for r in sorted(rows, key=lambda r: -r[2])[:top]:
        lines.append(f'  {r[0]:4d}s worst {r[2]:6.0f} ms [{fmt(r[3])}]')
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('log')
    parser.add_argument('--from', dest='start', type=float, default=0)
    parser.add_argument('--to', dest='end', type=float, default=float('inf'))
    parser.add_argument('--top', type=int, default=0)
    args = parser.parse_args()
    with open(args.log, encoding='utf-8', errors='replace') as f:
        text = f.read()
    rows = [r for r in windows(text) if args.start <= r[0] <= args.end]
    print(summarize(rows, args.top))


if __name__ == '__main__':
    sys.exit(main())
