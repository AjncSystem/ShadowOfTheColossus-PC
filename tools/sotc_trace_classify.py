#!/usr/bin/env python3
"""sotc: classifies the slow frames of a SOTC_TRACE session (user/trace/<session>/*.json).

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

    python tools/sotc_trace_classify.py user/trace/<session>
"""
import collections
import json
import pathlib
import sys

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
    """Time covered by the spans inside [f0, f1), overlaps counted once."""
    cut = sorted((max(e['ts'], f0), min(e['ts'] + e['dur'], f1)) for e in spans
                 if e['name'] not in skip and e['ts'] < f1 and e['ts'] + e['dur'] > f0)
    total, end = 0.0, f0
    for a, b in cut:
        if b > end:
            total += b - max(a, end)
            end = b
    return total


def classify(path):
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
    f0, f1 = f['ts'], f['ts'] + f['dur']
    dur = f['dur']
    by_tid = collections.defaultdict(list)
    for e in ev:
        if e.get('ph') == 'X':
            by_tid[e['tid']].append(e)
    row = {'file': path.name, 'ms': dur / 1000}
    draw = cp = rec = gpu = 0.0
    waits = collections.Counter()
    running = []
    for tid, spans in by_tid.items():
        n = names.get(tid, '')
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
                t = union([e for e in spans if e['name'] in kinds], f0, f1)
                if main:
                    waits[key] += t
            if main and not short.startswith('RenderFlip'):
                busy = union(spans, f0, f1)
                running.append((short, (dur - busy) / dur))
    row.update(draw=draw / dur, cp=cp / dur, rec=rec / dur, gpu=gpu / dur)
    main_run = max((r for _, r in running), default=0.0)
    row['game_running'] = main_run
    row['waits'] = {k: v / 1000 for k, v in waits.items() if v > 0}
    blocking = {k: v for k, v in waits.items() if k in ('readback', 'fault', 'io', 'upload fence')}
    top = max(blocking, key=blocking.get) if blocking else None
    if row['draw'] >= 0.85:
        cause = 'draw'
    elif row['gpu'] >= 0.85:
        cause = 'gpu'
    elif top and blocking[top] >= 0.3 * dur:
        cause = {'readback': 'readback', 'fault': 'fault', 'io': 'io',
                 'upload fence': 'fault'}[top]
    elif main_run >= 0.6:
        cause = 'game-cpu'
    else:
        cause = 'other'
    row['cause'] = cause
    return row


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    rows = [r for p in sorted(pathlib.Path(sys.argv[1]).glob('*.json')) if (r := classify(p))]
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
