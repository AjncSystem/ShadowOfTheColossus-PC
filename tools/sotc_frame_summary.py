#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarize a visually selected window of SOTC_FRAME_TIMES (host presentations)."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path


def summarize(rows, start=0.0, end=None):
    if not rows:
        raise ValueError('No game presentations in CSV')
    origin = int(rows[0]['time_ns'])
    selected = [r for r in rows if (int(r['time_ns']) - origin) / 1e9 >= start and
                (end is None or (int(r['time_ns']) - origin) / 1e9 <= end)]
    # Exclude the interval crossing into the requested window and the first row's zero.
    intervals = [int(r['interval_ns']) / 1e6 for r in selected[1:] if int(r['interval_ns']) > 0]
    if not intervals:
        raise ValueError('Window needs at least two presentations')
    ordered = sorted(intervals)
    slow = ordered[-max(1, math.ceil(len(ordered) * 0.01)):]

    def percentile(q):
        index = (len(ordered) - 1) * q
        lower = math.floor(index)
        upper = math.ceil(index)
        return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)

    return {
        'frames': len(selected), 'intervals': len(intervals),
        'observed_seconds': sum(intervals) / 1000,
        'average_fps': len(intervals) * 1000 / sum(intervals),
        'one_percent_low_fps': 1000 / (sum(slow) / len(slow)),
        'median_ms': percentile(0.5), 'p95_ms': percentile(0.95), 'p99_ms': percentile(0.99),
        'worst_ms': ordered[-1], 'intervals_over_40_ms': sum(v > 40 for v in intervals),
        'measured_worker_ms': {name: sum(int(r.get(name, 0)) for r in selected[1:]) / 1e6
            for name in ('shader_ns', 'graphics_ns', 'compute_ns', 'readback_ns',
                         'submit_wait_ns', 'cache_snapshot_ns')},
        'limits': ['Host presentation intervals; not GPU execution or display scanout.',
                   'Worker counters are attributed when scopes complete and may overlap; do not add them as frame costs.',
                   'Scene, input, cache warmth and capture/readback settings must match for comparisons.'],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv', type=Path)
    parser.add_argument('--start', type=float, default=0)
    parser.add_argument('--end', type=float)
    parser.add_argument('--scene', required=True)
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    if not math.isfinite(args.start) or args.start < 0 or (args.end is not None and
        (not math.isfinite(args.end) or args.end <= args.start)):
        parser.error('Use a finite nonnegative start and an end after start')
    with args.csv.open(newline='', encoding='utf-8') as file:
        result = summarize(list(csv.DictReader(file)), args.start, args.end)
    result.update(csv=str(args.csv.resolve()), csv_sha256=hashlib.sha256(args.csv.read_bytes()).hexdigest(),
                  scene=args.scene, window_seconds=[args.start, args.end])
    if args.runtime:
        result['runtime_sha256'] = hashlib.sha256(args.runtime.read_bytes()).hexdigest()
    output = args.out or args.csv.with_suffix('.summary.json')
    output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
