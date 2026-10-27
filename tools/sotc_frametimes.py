#!/usr/bin/env python3
"""Whole-run frame-time statistics from SOTC_FRAMETIME_OUT files ("seconds interval_ms cmds" per flip).

usage: sotc_frametimes.py [--from SECONDS] [--to SECONDS] [--target-ms 16.67] FILE...

Prints, per file and for all files together: frames, average FPS (frames / time), p50/p95/p99/p99.9
frame times, 1% and 0.1% low (average FPS of the slowest 1% / 0.1% of frames), the share of frames
and of time above the target and above 20/25/33.3 ms, and hitches (a frame over 25 ms and over twice
the median of the 30 frames before it). Then the same split by load (cmds per frame of the frame).
"""
import argparse
import statistics


def load(path, start, end):
    frames = []
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) != 3:
                continue
            t, ms, cmds = float(parts[0]), float(parts[1]), int(parts[2])
            if start <= t <= end:
                frames.append((t, ms, cmds))
    return frames


def hitches(frames):
    count = 0
    for i in range(30, len(frames)):
        ms = frames[i][1]
        if ms > 25.0 and ms > 2.0 * statistics.median(f[1] for f in frames[i - 30:i]):
            count += 1
    return count


def summary(frames, target, hitch_count=None):
    if not frames:
        return 'no frames'
    ms = sorted(f[1] for f in frames)
    n, total = len(ms), sum(ms)

    def pct(p):
        return ms[min(n - 1, int(p * n))]

    def low(share):
        worst = ms[n - max(1, int(n * share)):]
        return 1000.0 * len(worst) / sum(worst)

    def over(limit):
        slow = [m for m in ms if m > limit]
        return f'{100.0 * len(slow) / n:.1f}% fr / {100.0 * sum(slow) / total:.1f}% t'

    text = (f'{n} frames, {total / 1000:.0f} s, avg {1000.0 * n / total:.1f} FPS | p50 {pct(0.5):.1f} '
            f'p95 {pct(0.95):.1f} p99 {pct(0.99):.1f} p99.9 {pct(0.999):.1f} max {ms[-1]:.1f} ms | '
            f'1% low {low(0.01):.1f}, 0.1% low {low(0.001):.1f} FPS | >{target:.1f} ms {over(target)}, '
            f'>20 {over(20.0)}, >25 {over(25.0)}, >33.3 {over(33.3)}')
    if hitch_count is not None:
        text += f' | hitches {hitch_count}'
    return text


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('files', nargs='+')
    parser.add_argument('--from', dest='start', type=float, default=0.0)
    parser.add_argument('--to', dest='end', type=float, default=1e9)
    parser.add_argument('--target-ms', type=float, default=1000.0 / 60 + 0.5,
                        help='late threshold (default 17.2: a 60 FPS frame plus vblank jitter)')
    args = parser.parse_args()
    runs = [(path, load(path, args.start, args.end)) for path in args.files]
    every = []
    total_hitches = 0
    for path, frames in runs:
        h = hitches(frames)
        total_hitches += h
        every += frames
        print(f'{path}: {summary(frames, args.target_ms, h)}')
    if len(runs) > 1:
        print(f'all: {summary(every, args.target_ms, total_hitches)}')
    print('by load (cmds of the frame):')
    for lo, hi in ((0, 3000), (3000, 5000), (5000, 6000), (6000, 7000), (7000, 8000), (8000, 10 ** 9)):
        part = [f for f in every if lo <= f[2] < hi]
        if part:
            label = f'>= {lo}' if hi == 10 ** 9 else f'{lo}-{hi}'
            print(f'  {label:>10}: {summary(part, args.target_ms)}')


if __name__ == '__main__':
    main()
