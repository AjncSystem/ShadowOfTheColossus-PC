#!/usr/bin/env python3
"""sotc: classifies the slow frames of a SOTC_TRACE session (user/trace/<session>/*.json and
*.sotctrace).

For the slow frame of each file it measures, inside the frame:
  - how busy the port's threads were (draw thread, command processor, recorder) and the GPU;
  - per game thread, the time spent in each kind of recorded wait (readback, write/read fault,
    file read, upload fence, mutex, semaphore/cond/event flag/equeue, sleep) and the time with no
    recorded wait ("running": on the CPU, or in a wait the recorder does not see).
Then it names the most likely cause:
  draw     the draw thread was busy for almost the whole frame
  gpu      the GPU was busy for almost the whole frame
  readback game threads waited mostly on GPU readbacks while the draw thread had room
  fault    game threads waited mostly in page faults (write faults over GPU data)
  io       game threads waited mostly on file reads
  game-cpu the main game threads were mostly running, not waiting: the game's own work
  other    none of the above

A .sotctrace stream file gives one row per slow frame; a dump gives the frame it is named after.

    python tools/sotc_trace_classify.py user/trace/<session>
"""
import collections
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / 'trace'))
import sotc_trace  # noqa: E402  (the .sotctrace reader, tools/trace/sotc_trace.py)

GUEST_KEYS = {
    'readback': ('readback',),
    'fault': ('write fault', 'read fault'),
    'io': ('file read',),
    'upload fence': ('upload fence (runtime write)',),
    'mutex': ('mutex wait',),
    'sync': ('sema wait', 'cond wait', 'cond timedwait', 'evf wait', 'equeue wait'),
    'sleep': ('sleep',),
}
# The game threads whose time decides the frame (names as the game sets them).
MAIN_GAME = ('Shadow Game Upd', 'Game Render', 'RenderFlip', 'BPE JobWorker')


def union(spans, f0, f1, skip=()):
    """Time covered by the (start, end, name, ...) spans inside [f0, f1), overlaps counted once."""
    cut = sorted((max(sp[0], f0), min(sp[1], f1)) for sp in spans
                 if sp[2] not in skip and sp[0] < f1 and sp[1] > f0)
    total, end = 0.0, f0
    for a, b in cut:
        if b > end:
            total += b - max(a, end)
            end = b
    return total


def frame_stats(f0, f1, lanes):
    """The cause of one frame. lanes: (thread name, spans) pairs; a span is a tuple whose first
    three items are (start, end, name), in the unit of f0 and f1 (µs). The busy fractions are
    of the frame; the waits are in that unit."""
    dur = (f1 - f0) or 1.0
    draw = cp = rec = gpu = 0.0
    waits = collections.Counter()
    running = []
    for n, spans in lanes:
        if 'GpuDraw' in n:
            draw = max(draw, union(spans, f0, f1, skip=('draw idle',)))
        elif 'GpuCommandProcessor' in n:
            cp = max(cp, union(spans, f0, f1, skip=('cp idle',)))
        elif 'GpuRecorder' in n:
            rec = max(rec, union(spans, f0, f1))
        elif n.startswith('GPU: draw'):
            gpu = max(gpu, union(spans, f0, f1))
        elif n.startswith('game: '):
            short = n[6:]
            main = any(short.startswith(m) for m in MAIN_GAME)
            for key, kinds in GUEST_KEYS.items():
                t = union([sp for sp in spans if sp[2] in kinds], f0, f1)
                if main:
                    waits[key] += t
            if main and not short.startswith('RenderFlip'):
                busy = union(spans, f0, f1)
                running.append((short, (dur - busy) / dur))
    main_run = max((r for _, r in running), default=0.0)
    blocking = {k: v for k, v in waits.items() if k in ('readback', 'fault', 'io', 'upload fence')}
    top = max(blocking, key=blocking.get) if blocking else None
    if draw / dur >= 0.85:
        cause = 'draw'
    elif gpu / dur >= 0.85:
        cause = 'gpu'
    elif top and blocking[top] >= 0.3 * dur:
        cause = {'readback': 'readback', 'fault': 'fault', 'io': 'io',
                 'upload fence': 'fault'}[top]
    elif main_run >= 0.6:
        cause = 'game-cpu'
    else:
        cause = 'other'
    return {'dur': dur, 'draw': draw / dur, 'cp': cp / dur, 'rec': rec / dur, 'gpu': gpu / dur,
            'game_running': main_run, 'waits': dict(waits), 'cause': cause}


def _row(name, st):
    return {'file': name, 'ms': st['dur'] / 1000, 'draw': st['draw'], 'cp': st['cp'],
            'rec': st['rec'], 'gpu': st['gpu'], 'game_running': st['game_running'],
            'waits': {k: v / 1000 for k, v in st['waits'].items() if v > 0},
            'cause': st['cause']}


def classify(path):
    """The row of a Chrome JSON dump (None when it has no slow frame)."""
    d = json.loads(path.read_text())
    ev = d['traceEvents']
    names = {e['tid']: e['args']['name'] for e in ev if e.get('name') == 'thread_name'}
    slow = [e for e in ev if e.get('name') == 'SLOW FRAME']
    if not slow:
        return None
    # The frame the file is named after (otherData.frame); a window may hold others too.
    want = d.get('otherData', {}).get('frame')
    named = [e for e in slow if e.get('args', {}).get('a') == want]
    f = named[0] if named else max(slow, key=lambda e: e['dur'])
    by_tid = {}
    for e in ev:
        if e.get('ph') == 'X':
            by_tid.setdefault(e['tid'], []).append((e['ts'], e['ts'] + e['dur'], e['name']))
    lanes = [(names.get(tid, ''), spans) for tid, spans in by_tid.items()]
    st = frame_stats(f['ts'], f['ts'] + f['dur'], lanes)
    return _row(path.name, st)


def classify_trace(path):
    """The rows of a .sotctrace file: one for the dump's frame, one per slow frame of a stream."""
    t = sotc_trace.load(path)
    slow = [e for e in t.all_events() if e.kind == sotc_trace.K_FRAME
            and e.flags & sotc_trace.FLAG_SLOW]
    if not slow:
        return []
    if t.meta.get('kind') == 'stream':
        chosen = sorted(slow, key=lambda e: e.t0)
    else:
        want = t.meta.get('frame')
        named = [e for e in slow if e.a == want]
        chosen = [named[0] if named else max(slow, key=lambda e: e.t1 - e.t0)]
    lanes = sotc_trace.span_lanes(t)
    rows = []
    for f in chosen:
        f0, f1 = f.t0 / 1000.0, f.t1 / 1000.0
        st = frame_stats(f0, f1, [(lane.label, lane.window(f0, f1)) for lane in lanes])
        name = path.name if len(chosen) == 1 else f'{path.name} #{f.a}'
        rows.append(_row(name, st))
    return rows


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    folder = pathlib.Path(sys.argv[1])
    rows = []
    for p in sorted([*folder.glob('*.json'), *folder.glob('*.sotctrace')]):
        if p.suffix == '.sotctrace':
            rows.extend(classify_trace(p))
        elif (r := classify(p)):
            rows.append(r)
    print(f"{'file':26s} {'ms':>5s} {'draw':>5s} {'cp':>5s} {'rec':>5s} {'gpu':>5s} "
          f"{'run':>5s}  cause     main game threads' waits (ms, summed)")
    for r in rows:
        w = ', '.join(f"{k} {v:.0f}" for k, v in sorted(r['waits'].items(), key=lambda x: -x[1]))
        print(f"{r['file'][:26]:26s} {r['ms']:5.0f} {r['draw']:5.0%} {r['cp']:5.0%} "
              f"{r['rec']:5.0%} {r['gpu']:5.0%} {r['game_running']:5.0%}  {r['cause']:8s}  {w}")
    print()
    counts = collections.Counter(r['cause'] for r in rows)
    ms = collections.Counter()
    for r in rows:
        ms[r['cause']] += r['ms']
    for cause, n in counts.most_common():
        print(f"{cause:9s} {n:3d} slow frames, {ms[cause]:6.0f} ms in total")


if __name__ == '__main__':
    main()
