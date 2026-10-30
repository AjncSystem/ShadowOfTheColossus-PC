#!/usr/bin/env python3
"""sotc: summary of SOTC_TRACE files (user/trace/<session>/*.json and *.sotctrace): for the slow
frame of each file, the time each thread spent in each kind of span inside it (clipped to the
frame), longest first.

    python tools/sotc_trace_summary.py user/trace/<session> [--top 6] [--files 10]
"""
import argparse, collections, json, pathlib, sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / 'trace'))
import sotc_trace  # noqa: E402  (the .sotctrace reader, tools/trace/sotc_trace.py)


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


def summarize_trace(path, top):
    t = sotc_trace.load(path)
    slow = [e for e in t.all_events() if e.kind == sotc_trace.K_FRAME
            and e.flags & sotc_trace.FLAG_SLOW]
    if not slow:
        return
    f = max(slow, key=lambda e: e.t1 - e.t0)
    f0, f1 = f.t0 / 1000.0, f.t1 / 1000.0
    sym = sotc_trace.make_symbolizer(t, use_host=False)
    per = collections.defaultdict(lambda: collections.Counter())
    biggest = collections.defaultdict(list)
    for lane in sotc_trace.span_lanes(t):
        if lane.ring == f.thread:
            continue
        for s, e, name, b, _kind, _flags in lane.window(f0, f1):
            a0, a1 = max(s, f0), min(e, f1)
            if a1 <= a0:
                continue
            args = {'b': sotc_trace.format_arg(b, sym)}
            per[lane.label][name] += a1 - a0
            biggest[lane.label].append((a1 - a0, name, args))
    print(f"== {path.name}: slow frame {(f1 - f0) / 1000:.1f} ms")
    order = sorted(per, key=lambda lbl: -sum(per[lbl].values()))
    for label in order:
        c = per[label]
        parts = ', '.join(f"{n} {v / 1000:.1f}" for n, v in c.most_common(top))
        print(f"  {label[:40]:40s} {parts}")
        longest = sorted(biggest[label], key=lambda x: -x[0])[:2]
        for dur, n, args in longest:
            if dur >= 8000:
                print(f"      longest: {n} {dur / 1000:.1f} ms {args}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir', type=pathlib.Path)
    ap.add_argument('--top', type=int, default=6)
    ap.add_argument('--files', type=int, default=10)
    a = ap.parse_args()
    paths = sorted([*a.dir.glob('*.json'), *a.dir.glob('*.sotctrace')])[:a.files]
    for p in paths:
        if p.suffix == '.sotctrace':
            summarize_trace(p, a.top)
        else:
            summarize(p, a.top)


if __name__ == '__main__':
    main()
