#!/usr/bin/env python3
"""sotc: summary of SOTC_TRACE files (user/trace/<session>/*.json): for the slow frame of each file,
the time each thread spent in each kind of span inside it (clipped to the frame), longest first.

    python tools/sotc_trace_summary.py user/trace/<session> [--top 6] [--files 10]
"""
import argparse, collections, json, pathlib


def summarize(path, top):
    d = json.loads(path.read_text())
    ev = d['traceEvents']
    names = {e['tid']: e['args']['name'] for e in ev if e.get('name') == 'thread_name'}
    slow = [e for e in ev if e.get('name') == 'SLOW FRAME']
    if not slow:
        return
    f = max(slow, key=lambda e: e['dur'])
    f0, f1 = f['ts'], f['ts'] + f['dur']
    per = collections.defaultdict(lambda: collections.Counter())
    biggest = collections.defaultdict(list)
    for e in ev:
        if e.get('ph') != 'X' or e['tid'] == f['tid']:
            continue
        a, b = max(e['ts'], f0), min(e['ts'] + e['dur'], f1)
        if b <= a:
            continue
        per[e['tid']][e['name']] += b - a
        biggest[e['tid']].append((b - a, e['name'], e['args']))
    print(f"== {path.name}: slow frame {f['dur'] / 1000:.1f} ms")
    order = sorted(per, key=lambda t: -sum(per[t].values()))
    for tid in order:
        c = per[tid]
        parts = ', '.join(f"{n} {v / 1000:.1f}" for n, v in c.most_common(top))
        print(f"  {names.get(tid, tid)[:40]:40s} {parts}")
        longest = sorted(biggest[tid], key=lambda x: -x[0])[:2]
        for dur, n, args in longest:
            if dur >= 8000:
                print(f"      longest: {n} {dur / 1000:.1f} ms {args}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir', type=pathlib.Path)
    ap.add_argument('--top', type=int, default=6)
    ap.add_argument('--files', type=int, default=10)
    a = ap.parse_args()
    for p in sorted(a.dir.glob('*.json'))[:a.files]:
        summarize(p, a.top)


if __name__ == '__main__':
    main()
