"""Tests for the SOTC_TRACE v2 reader and tools (tools/trace/sotc_trace.py, the classify and
summary tools). Synthetic files only: no game files, no out/re index needed."""
import contextlib
import gzip
import io
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools' / 'trace'))
sys.path.insert(0, str(ROOT / 'tools'))
import sotc_trace  # noqa: E402
import sotc_trace_classify  # noqa: E402

MS = 1_000_000  # ns per millisecond
IMAGE_BASE = 0x1000000
IMAGE_SIZE = 0x100000


def chunk(typ, payload, compress=True, flags=None):
    """One chunk: header plus payload (zlib when compress)."""
    stored = zlib.compress(payload, 1) if compress else payload
    fl = (1 if compress else 0) if flags is None else flags
    return struct.pack('<IIII', typ, fl, len(stored), len(payload)) + stored


def record(t0, t1, name_id, kind=0, flags=0, aux=0, a=0, b=0):
    return struct.pack('<QQIBBHIIQQ', t0, t1, name_id, kind, 0, flags, aux, 0, a, b)


def events_chunk(ring, records, compress=True):
    body = struct.pack('<II', ring, len(records)) + b''.join(records)
    return chunk(4, body, compress)


def json_chunk(typ, obj, compress=True):
    return chunk(typ, json.dumps(obj).encode('utf-8'), compress)


def write(path, parts):
    Path(path).write_bytes(b'SOTCTRC\x01' + b''.join(parts))
    return Path(path)


class LoaderTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_mixed_chunks_merge_and_tolerate_damage(self):
        parts = [
            json_chunk(1, {'version': 1, 'mode': 2, 'kind': 'stream', 'image_base': 0x40,
                           'image_size': 0x1000}),                             # compressed
            json_chunk(1, {'frame': 7, 'mode': 1}, compress=False),            # raw, merged
            json_chunk(2, [{'id': 1, 'os_tid': 100, 'name': 'GpuDraw', 'kind': 'draw',
                            'sort': 2000},
                           {'id': 2, 'os_tid': 101, 'name': 'Old', 'kind': 'guest', 'sort': 1}],
                       compress=False),
            json_chunk(2, [{'id': 2, 'os_tid': 101, 'name': 'Shadow Game Upd',
                            'kind': 'guest', 'sort': 1000}]),                 # replaces id 2
            json_chunk(3, {'1': 'wait gpu', '2': 'draw'}, compress=False),    # NAMES, part 1
            json_chunk(3, {'3': 'frame'}),                                     # NAMES, part 2
            events_chunk(1, [record(10, 20, 1, a=5, b=0x1000010)], compress=False),
            events_chunk(1, [record(5, 9, 2, flags=3, aux=4, a=1, b=2),
                             record(30, 30, 2, kind=1)]),       # second chunk, same ring
            events_chunk(2, [record(40, 50, 3, kind=6, flags=1, a=99)]),
            chunk(0x99, b'future chunk type'),                                 # unknown: skipped
            json_chunk(5, [{'name': 'Draw', 'file': 'a.cpp', 'line': 3, 'calls': 2,
                            'total_ns': 500, 'max_ns': 300}]),
        ]
        # A truncated tail: a header that promises 1000 bytes, only 10 present.
        parts.append(struct.pack('<IIII', 4, 1, 1000, 4000) + b'x' * 10)
        t = sotc_trace.load(write(self.dir / 'a.sotctrace', parts))

        self.assertEqual(t.format, 'sotctrace')
        self.assertFalse(t.complete)          # no END chunk
        self.assertTrue(t.truncated)
        self.assertEqual(t.unknown[0x99], 1)
        self.assertEqual(t.meta['mode'], 1)  # the raw META replaced the compressed value
        self.assertEqual(t.meta['frame'], 7)
        self.assertEqual(t.meta['kind'], 'stream')  # the first META survives the merge
        self.assertEqual(t.threads[2]['name'], 'Shadow Game Upd')
        self.assertEqual(t.thread_label(2), 'game: Shadow Game Upd')
        self.assertEqual(t.thread_label(1), 'GpuDraw')
        self.assertEqual(t.thread_label(9), 'thread 9')
        self.assertEqual(t.names[3], 'frame')
        self.assertEqual(t.zone_stats[0]['name'], 'Draw')
        # Ring 1 got two EVENTS chunks; the records are sorted by t0 with names resolved.
        ring1 = t.events[1]
        self.assertEqual([e.t0 for e in ring1], [5, 10, 30])
        self.assertEqual([e.name for e in ring1], ['draw', 'wait gpu', 'draw'])
        self.assertEqual((ring1[0].kind, ring1[0].flags, ring1[0].aux), (0, 3, 4))
        self.assertEqual((ring1[0].a, ring1[0].b), (1, 2))
        self.assertEqual(ring1[2].kind, 1)
        self.assertEqual(ring1[1].b, 0x1000010)
        frame = t.events[2][0]
        self.assertEqual((frame.kind, frame.flags, frame.a, frame.name), (6, 1, 99, 'frame'))

    def test_end_chunk_marks_a_clean_file(self):
        parts = [json_chunk(1, {'version': 1}), events_chunk(1, [record(1, 2, 0)]),
                 chunk(0xFFFFFFFF, b'')]
        t = sotc_trace.load(write(self.dir / 'b.sotctrace', parts))
        self.assertTrue(t.complete)
        self.assertFalse(t.truncated)
        self.assertEqual(t.events[1][0].name, '')    # name id 0 means no name

    def test_damaged_payload_is_counted_not_fatal(self):
        bad = struct.pack('<IIII', 4, 1, 5, 40) + b'notzl'   # claims zlib, not a zlib stream
        t = sotc_trace.load(write(self.dir / 'c.sotctrace',
                                  [bad, json_chunk(1, {'frame': 3}, compress=False)]))
        self.assertEqual(t.bad_chunks, 1)
        self.assertEqual(t.meta['frame'], 3)

    def test_chrome_json_dump_loads(self):
        doc = {'displayTimeUnit': 'ms', 'otherData': {'frame': 12, 'worst_ms': 200.0},
               'traceEvents': [
                   {'name': 'thread_name', 'ph': 'M', 'pid': 1, 'tid': 4,
                    'args': {'name': 'game: Shadow Game Upd'}},
                   {'name': 'thread_sort_index', 'ph': 'M', 'pid': 1, 'tid': 4,
                    'args': {'sort_index': 1000}},
                   {'name': 'wait gpu', 'ph': 'X', 'pid': 1, 'tid': 4, 'ts': 10.0, 'dur': 50.0,
                    'args': {'a': 3, 'b': 'eboot+0x1234'}},
                   {'name': 'mark', 'ph': 'i', 'pid': 1, 'tid': 4, 'ts': 11.0, 'args': {}},
                   {'name': 'SLOW FRAME', 'ph': 'X', 'pid': 1, 'tid': 9, 'ts': 0.0,
                    'dur': 200000.0, 'args': {'a': 12, 'b': 0}}]}
        path = self.dir / 'dump.json'
        path.write_text(json.dumps(doc))
        t = sotc_trace.load(path)
        self.assertEqual(t.format, 'json')
        self.assertEqual(t.meta['kind'], 'dump')
        self.assertEqual(t.meta['frame'], 12)
        self.assertEqual(t.thread_label(4), 'game: Shadow Game Upd')
        wait = t.events[4][0]
        self.assertEqual((wait.t0, wait.t1, wait.name), (10000, 60000, 'wait gpu'))
        self.assertEqual(wait.b, sotc_trace.JSON_IMAGE_BASE + 0x1234)
        slow = t.events[9][0]
        self.assertEqual((slow.kind, slow.flags, slow.a), (sotc_trace.K_FRAME, 1, 12))

    def test_not_a_trace_raises(self):
        path = self.dir / 'junk.txt'
        path.write_text('hello')
        with self.assertRaises(ValueError):
            sotc_trace.load(path)


class SymbolizerTest(unittest.TestCase):
    def setUp(self):
        self.sym = sotc_trace.Symbolizer(
            IMAGE_BASE, IMAGE_SIZE, funcs=[(0x1100, 0x1200), (0x1000, 0x1100)],
            names={0x1000: 'Game::Wait', 0x1100: 'Game::Draw'})

    def test_guest_address_to_name_and_offset(self):
        self.assertEqual(self.sym.label(IMAGE_BASE + 0x1010), 'Game::Wait+0x10')
        self.assertEqual(self.sym.label(IMAGE_BASE + 0x1100), 'Game::Draw+0x0')
        self.assertEqual(self.sym.label(IMAGE_BASE + 0x1234), 'eboot+0x1234')  # no function
        self.assertTrue(self.sym.is_guest(IMAGE_BASE + 5))
        self.assertFalse(self.sym.is_guest(IMAGE_BASE + IMAGE_SIZE))

    def test_unnamed_function_and_host_address(self):
        sym = sotc_trace.Symbolizer(IMAGE_BASE, IMAGE_SIZE, funcs=[(0x2000, 0x2100)])
        self.assertEqual(sym.label(IMAGE_BASE + 0x2004), 'sub_2000+0x4')
        self.assertEqual(self.sym.label(0x500), '0x500')          # no host exe: hex
        self.assertEqual(self.sym.label(0x140001000), '0x140001000')

    def test_names_file_format(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'names.txt'
            path.write_text('# comment\n0xf8f390 import___cxa_atexit\n'
                            '0x1000 Game::Wait  # report; note\nbogus line\n0xZZ bad\n',
                            encoding='utf-8')
            names = sotc_trace.read_names(path)
            self.assertEqual(names, {0xf8f390: 'import___cxa_atexit', 0x1000: 'Game::Wait'})
            self.assertEqual(sotc_trace.read_names(Path(tmp) / 'missing.txt'), {})


def hitch_trace(path):
    """Normal frames of 33 ms and one 200 ms slow frame whose draw-thread wait grows."""
    names = {1: 'wait gpu', 2: 'draw', 3: 'frame'}
    threads = [{'id': 1, 'os_tid': 100, 'name': 'GpuDraw', 'kind': 'draw', 'sort': 2000},
               {'id': 3, 'os_tid': 102, 'name': 'frames', 'kind': 'frames', 'sort': 0}]
    draw, frames = [], []
    for i in range(10):
        start = i * 40 * MS
        frames.append(record(start, start + 33 * MS, 3, kind=6, a=i))
        draw.append(record(start + 1 * MS, start + 3 * MS, 2, b=IMAGE_BASE + 0x1010))
        draw.append(record(start + 4 * MS, start + 5 * MS, 1, b=IMAGE_BASE + 0x1010))
    slow0 = 500 * MS
    frames.append(record(slow0, slow0 + 200 * MS, 3, kind=6, flags=1, a=99))
    draw.append(record(slow0, slow0 + 20 * MS, 2, b=IMAGE_BASE + 0x1010))
    draw.append(record(slow0 + 20 * MS, slow0 + 190 * MS, 1, b=IMAGE_BASE + 0x1010))
    parts = [json_chunk(1, {'version': 1, 'mode': 2, 'kind': 'stream',
                            'image_base': IMAGE_BASE, 'image_size': IMAGE_SIZE}),
             json_chunk(2, threads), json_chunk(3, {str(k): v for k, v in names.items()}),
             events_chunk(1, draw), events_chunk(3, frames)]
    return write(path, parts)


class HitchTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_wait_gpu_ranks_first_and_draw_is_the_cause(self):
        t = sotc_trace.load(hitch_trace(self.dir / 'hitch.sotctrace'))
        sym = sotc_trace.Symbolizer(IMAGE_BASE, IMAGE_SIZE, funcs=[(0x1000, 0x1100)],
                                    names={0x1000: 'Game::Wait'})
        results = sotc_trace.hitch_file(t, sym)
        self.assertEqual(len(results), 1)
        r = results[0]
        self.assertEqual(r['frame'], 99)
        self.assertEqual(r['segment'], 'frame')
        self.assertAlmostEqual(r['ms'], 200.0)
        self.assertEqual(r['normal_frames'], 10)
        self.assertAlmostEqual(r['typical_ms'], 33.0)
        self.assertEqual(r['verdict'], 'draw')
        first = r['top'][0]
        self.assertEqual((first['thread'], first['name']), ('GpuDraw', 'wait gpu'))
        self.assertAlmostEqual(first['slow_ms'], 170.0)
        self.assertAlmostEqual(first['typical_ms'], 1.0)
        self.assertAlmostEqual(first['delta_ms'], 169.0)
        self.assertEqual(first['callers'][0][0], 'Game::Wait+0x10')
        self.assertEqual(r['top'][1]['name'], 'draw')
        md = '\n'.join(sotc_trace.hitch_markdown(t.path.name, results))
        self.assertIn('frame 99 (frame): 200.0 ms, verdict: draw', md)
        self.assertIn('| 1 | GpuDraw | wait gpu |', md)

    def test_classify_reads_sotctrace(self):
        rows = sotc_trace_classify.classify_trace(hitch_trace(self.dir / 'hitch.sotctrace'))
        self.assertEqual([r['cause'] for r in rows], ['draw'])

    def test_no_slow_frame_gives_no_results(self):
        parts = [json_chunk(1, {'kind': 'stream'}), events_chunk(3, [record(0, 33 * MS, 0,
                                                                          kind=6, a=1)])]
        t = sotc_trace.load(write(self.dir / 'calm.sotctrace', parts))
        self.assertEqual(sotc_trace.hitch_file(t, sotc_trace.Symbolizer()), [])


class ChromeJsonTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_json_conversion_opens_after_gunzip(self):
        src = hitch_trace(self.dir / 'hitch.sotctrace')
        out = self.dir / 'hitch.json.gz'
        with contextlib.redirect_stderr(io.StringIO()):
            rc = sotc_trace.main(['json', str(src), '-o', str(out), '--no-index', '--no-host'])
        self.assertEqual(rc, 0)
        with gzip.open(out, 'rt', encoding='utf-8') as f:
            doc = json.load(f)
        ev = doc['traceEvents']
        names = {e['tid']: e['args']['name'] for e in ev if e.get('name') == 'thread_name'}
        self.assertEqual(names[1], 'GpuDraw')
        slow = [e for e in ev if e.get('name') == 'SLOW FRAME']
        self.assertEqual(len(slow), 1)
        self.assertEqual(slow[0]['args']['a'], 99)
        waits = [e for e in ev if e.get('name') == 'wait gpu' and e['ph'] == 'X']
        self.assertTrue(all(e['dur'] > 0 for e in waits))
        # The guest return address is written as text (no function index here: eboot offset).
        self.assertEqual(waits[0]['args']['b'], 'eboot+0x1010')

    def test_json_round_trip_keeps_the_classify_shape(self):
        src = hitch_trace(self.dir / 'hitch.sotctrace')
        t = sotc_trace.load(src)
        doc = sotc_trace.to_chrome(t, sotc_trace.Symbolizer())
        path = self.dir / 'back.json'
        path.write_text(json.dumps(doc))
        row = sotc_trace_classify.classify(path)
        self.assertEqual(row['cause'], 'draw')


def sample_trace(path):
    """A game thread (ring 1) and its samples track (ring 5). Stack 1 (8 samples): Game::Wait
    called from Game::Draw; stack 2 (2 samples): Game::Draw alone. One sample per ms."""
    threads = [{'id': 1, 'os_tid': 100, 'name': 'Shadow Game Upd', 'kind': 'guest', 'sort': 1000},
               {'id': 5, 'os_tid': 100, 'name': 'samples', 'kind': 'samples', 'sort': 9}]
    stacks = {1: [IMAGE_BASE + 0x1010, IMAGE_BASE + 0x1110], 2: [IMAGE_BASE + 0x1150]}
    recs = [record(i * MS, i * MS, 0, kind=3, aux=1, a=IMAGE_BASE + 0x1010) for i in range(8)]
    recs += [record(i * MS, i * MS, 0, kind=3, aux=2, a=IMAGE_BASE + 0x1150) for i in (8, 9)]
    parts = [json_chunk(1, {'version': 1, 'kind': 'stream', 'image_base': IMAGE_BASE,
                            'image_size': IMAGE_SIZE}),
             json_chunk(2, threads), json_chunk(7, {str(k): v for k, v in stacks.items()}),
             events_chunk(5, recs)]
    return write(path, parts)


SYM_FUNCS = [(0x1000, 0x1100), (0x1100, 0x1200)]
SYM_NAMES = {0x1000: 'Game::Wait', 0x1100: 'Game::Draw'}


def sym():
    return sotc_trace.Symbolizer(IMAGE_BASE, IMAGE_SIZE, funcs=SYM_FUNCS, names=SYM_NAMES)


class SampleTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_inclusive_ranks_the_hot_callee_and_flat_the_leaf(self):
        t = sotc_trace.load(sample_trace(self.dir / 's.sotctrace'))
        self.assertEqual(t.stacks[1], [IMAGE_BASE + 0x1010, IMAGE_BASE + 0x1110])
        rows = sotc_trace.sample_rows(t, sym())
        self.assertEqual(len(rows), 10)
        prof = sotc_trace.sample_profiles(rows)
        self.assertEqual(list(prof), ['game: Shadow Game Upd'])   # the samples track is merged
        p = prof['game: Shadow Game Upd']
        self.assertEqual(p['flat'].most_common(1)[0], ('Game::Wait', 8))
        self.assertEqual(p['incl'].most_common(1)[0], ('Game::Draw', 10))
        self.assertEqual(p['incl']['Game::Wait'], 8)
        lines = sotc_trace.top_report(t, 5, rows)
        self.assertIn('  inclusive:', lines)
        self.assertTrue(any('Game::Draw' in ln for ln in lines))

    def test_folded_stacks_root_first(self):
        t = sotc_trace.load(sample_trace(self.dir / 's.sotctrace'))
        folded = sotc_trace.folded_lines(sotc_trace.sample_rows(t, sym()))
        self.assertIn('game: Shadow Game Upd;Game::Draw;Game::Wait 8', folded)
        self.assertIn('game: Shadow Game Upd;Game::Draw 2', folded)

    def test_top_command_writes_folded_file(self):
        src = sample_trace(self.dir / 's.sotctrace')
        out = self.dir / 'out.folded'
        with mock.patch.object(sotc_trace, 'eboot_funcs', return_value=SYM_FUNCS),                 mock.patch.object(sotc_trace, 'repo_names', return_value=SYM_NAMES),                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            rc = sotc_trace.main(['top', str(src), '--no-host', '--folded', str(out)])
        self.assertEqual(rc, 0)
        self.assertIn('game: Shadow Game Upd;Game::Draw;Game::Wait 8',
                      out.read_text(encoding='utf-8').splitlines())

    def test_parse_sig_output(self):
        text = '55 48 89 e5 ??\n; unique at 0x1000 (Game::Wait), 8 bytes\n'
        self.assertEqual(sotc_trace.parse_sig_output(text, 0x1000), ('55 48 89 e5 ??', ''))
        self.assertIsNone(sotc_trace.parse_sig_output(text, 0x2000)[0])
        self.assertIsNone(sotc_trace.parse_sig_output(
            '; not unique within 64 bytes: 3 matches\n', 0x1000)[0])

    def test_eboot_sig_runs_the_tool(self):
        text = '55 48 89 e5 ??\n; unique at 0x1000 (x), 8 bytes\n'
        done = subprocess.CompletedProcess([], 0, stdout=text, stderr='')
        with mock.patch.object(sotc_trace.subprocess, 'run', return_value=done) as run:
            self.assertEqual(sotc_trace.eboot_sig(0x1000), ('55 48 89 e5 ??', ''))
        self.assertEqual(run.call_args[0][0][-2:], ['sig', '0x1000'])

    def test_hook_name_is_an_identifier(self):
        self.assertEqual(sotc_trace.hook_name('Game::Wait', 0x1000, set()), 'Game_Wait')
        self.assertEqual(sotc_trace.hook_name('123', 0x1000, set()), 'F_123')
        self.assertEqual(sotc_trace.hook_name('Game::Wait', 0x1000, {'Game_Wait'}),
                         'Game_Wait_1000')


class ImportGpuTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_import_stats_and_gpu_calib_load(self):
        parts = [json_chunk(6, [{'index': 3, 'name': 'sceKernelWaitEventFlag', 'calls': 10,
                                 'total_ns': 10 * MS, 'max_ns': 2 * MS}]),
                 json_chunk(8, {'timestamp_period_ns': 1.0, 'valid_bits': 64,
                                'calibrated': True, 'pairs': [[0, 0]]})]
        t = sotc_trace.load(write(self.dir / 'i.sotctrace', parts))
        self.assertEqual(t.import_stats[0]['name'], 'sceKernelWaitEventFlag')
        self.assertTrue(t.gpu_calib['calibrated'])
        self.assertFalse(t.unknown)
        self.assertEqual(t.chunks[6], 1)

    def test_evgpu_spans_in_top_and_on_the_gpu_track(self):
        threads = [{'id': 6, 'os_tid': 103, 'name': 'GPU: draw', 'kind': 'gpu', 'sort': 5}]
        parts = [json_chunk(1, {'kind': 'stream'}), json_chunk(2, threads),
                 json_chunk(3, {'1': 'cmdbuf'}),
                 json_chunk(8, {'timestamp_period_ns': 1.0, 'valid_bits': 64,
                                'calibrated': True, 'pairs': [[0, 0]]}),
                 events_chunk(6, [record(10 * MS, 20 * MS, 1, kind=5, a=7, b=3),
                                  record(30 * MS, 40 * MS, 1, kind=5, a=8)])]
        t = sotc_trace.load(write(self.dir / 'g.sotctrace', parts))
        self.assertIn('GPU (EvGpu): 20.00 ms in 2 spans', sotc_trace.top_report(t, 5, []))
        doc = sotc_trace.to_chrome(t, sotc_trace.Symbolizer())
        gpu = [e for e in doc['traceEvents'] if e.get('ph') == 'X' and e.get('cat') == 'gpu']
        self.assertEqual(len(gpu), 2)
        self.assertTrue(all(e['tid'] == sotc_trace.GPU_TID for e in gpu))
        self.assertIn({'name': 'thread_name', 'ph': 'M', 'pid': 1, 'tid': sotc_trace.GPU_TID,
                       'args': {'name': 'GPU'}}, doc['traceEvents'])

    def test_samples_become_instants_with_the_stack(self):
        t = sotc_trace.load(sample_trace(self.dir / 's.sotctrace'))
        doc = sotc_trace.to_chrome(t, sym())
        samples = [e for e in doc['traceEvents'] if e.get('cat') == 'sample']
        self.assertEqual(len(samples), 10)
        self.assertEqual(samples[0]['name'], 'Game::Wait')
        self.assertEqual(samples[0]['args']['stack'], 'Game::Wait <- Game::Draw')


def import_trace(path, draw_ms, import_total_ms):
    """Five draw spans of draw_ms each, and the cumulative IMPORT_STATS of one import."""
    threads = [{'id': 1, 'os_tid': 100, 'name': 'GpuDraw', 'kind': 'draw', 'sort': 2000}]
    spans = [record(i * 20 * MS, i * 20 * MS + int(draw_ms * MS), 1) for i in range(5)]
    parts = [json_chunk(1, {'kind': 'stream'}), json_chunk(2, threads),
             json_chunk(3, {'1': 'draw'}), events_chunk(1, spans),
             json_chunk(6, [{'index': 3, 'name': 'sceKernelWaitEventFlag', 'calls': 10,
                             'total_ns': int(import_total_ms * MS), 'max_ns': 5 * MS}])]
    return write(path, parts)


class DiffTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_diff_shows_the_slower_side(self):
        a, b = self.dir / 'A', self.dir / 'B'
        a.mkdir()
        b.mkdir()
        import_trace(a / 'run1.sotctrace', 2, 10)
        import_trace(b / 'run1.sotctrace', 4, 48)
        md = self.dir / 'diff.md'
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = sotc_trace.main(['diff', str(a), str(b), '--no-index', '--no-host',
                                  '--md', str(md)])
        self.assertEqual(rc, 0)
        text = md.read_text(encoding='utf-8')
        self.assertEqual(text, out.getvalue())
        self.assertIn('| draw ms | 10.0 | 20.0 | +10.0 |', text)
        self.assertIn('| sceKernelWaitEventFlag | 10.0 | 48.0 | +38.0 |', text)

    def test_diff_of_a_missing_side_fails(self):
        with contextlib.redirect_stderr(io.StringIO()):
            rc = sotc_trace.main(['diff', str(self.dir / 'none'), str(self.dir / 'none'),
                                  '--no-index', '--no-host'])
        self.assertEqual(rc, 1)


def grown_trace(path):
    """Normal frames of 33 ms (a 1 ms draw span, one sample in the wait, a 1 ms GPU span each)
    and one 200 ms slow frame: a 38 ms import, 50 samples in Game::Draw, a 190 ms GPU span."""
    names = {1: 'draw', 2: 'frame', 3: 'sceKernelWaitEventFlag', 4: 'cmdbuf'}
    threads = [{'id': 1, 'os_tid': 100, 'name': 'Shadow Game Upd', 'kind': 'guest', 'sort': 1000},
               {'id': 3, 'os_tid': 102, 'name': 'frames', 'kind': 'frames', 'sort': 0},
               {'id': 6, 'os_tid': 103, 'name': 'GPU: draw', 'kind': 'gpu', 'sort': 5},
               {'id': 7, 'os_tid': 100, 'name': 'samples', 'kind': 'samples', 'sort': 9}]
    frames, guest, gpu, samp = [], [], [], []
    for i in range(10):
        start = i * 40 * MS
        frames.append(record(start, start + 33 * MS, 2, kind=6, a=i))
        guest.append(record(start + MS, start + 2 * MS, 1, b=IMAGE_BASE + 0x1010))
        gpu.append(record(start, start + MS, 4, kind=5))
        samp.append(record(start + 5 * MS, start + 5 * MS, 0, kind=3, aux=1,
                           a=IMAGE_BASE + 0x1010))
    slow0 = 500 * MS
    frames.append(record(slow0, slow0 + 200 * MS, 2, kind=6, flags=1, a=99))
    guest.append(record(slow0 + MS, slow0 + 39 * MS, 3, kind=4, b=IMAGE_BASE + 0x1010))
    gpu.append(record(slow0, slow0 + 190 * MS, 4, kind=5))
    samp += [record(slow0 + i * MS, slow0 + i * MS, 0, kind=3, aux=2, a=IMAGE_BASE + 0x1150)
             for i in range(50)]
    stacks = {1: [IMAGE_BASE + 0x1010, IMAGE_BASE + 0x1110], 2: [IMAGE_BASE + 0x1150]}
    parts = [json_chunk(1, {'version': 1, 'kind': 'stream', 'image_base': IMAGE_BASE,
                            'image_size': IMAGE_SIZE}),
             json_chunk(2, threads), json_chunk(3, {str(k): v for k, v in names.items()}),
             json_chunk(7, {str(k): v for k, v in stacks.items()}),
             events_chunk(1, guest), events_chunk(3, frames), events_chunk(6, gpu),
             events_chunk(7, samp)]
    return write(path, parts)


class HitchGrowthTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_samples_imports_and_gpu_verdict_in_the_slow_frame(self):
        t = sotc_trace.load(grown_trace(self.dir / 'h.sotctrace'))
        results = sotc_trace.hitch_file(t, sym())
        self.assertEqual(len(results), 1)
        r = results[0]
        self.assertEqual(r['funcs'][0]['func'], 'Game::Draw')
        # 50 samples in the slow frame; each normal frame has one sample in Game::Draw (median 1).
        self.assertAlmostEqual(r['funcs'][0]['delta'], 49.0)
        self.assertEqual(r['imports'][0]['name'], 'sceKernelWaitEventFlag')
        self.assertAlmostEqual(r['imports'][0]['delta_ms'], 38.0)
        self.assertEqual(r['gpu']['bound'], 'GPU-bound')
        md = '\n'.join(sotc_trace.hitch_markdown(t.path.name, results))
        self.assertIn('+38.0 ms sceKernelWaitEventFlag on game: Shadow Game Upd', md)
        self.assertIn('GPU (EvGpu): busy', md)


class AutofuncsTest(unittest.TestCase):
    FP = 'a' * 64

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    @staticmethod
    def fake_sig(start):
        if start == 0x1000:
            return '55 48 89 e5 ?? ?? 41 57', ''
        return None, 'not unique within 64 bytes: 3 matches'

    def test_scores_and_pick(self):
        t = sotc_trace.load(sample_trace(self.dir / 's.sotctrace'))
        scores = sotc_trace.function_scores([t], SYM_FUNCS)
        self.assertAlmostEqual(scores[0x1000][0], 8.0)     # 8 samples of 1 ms each
        self.assertAlmostEqual(scores[0x1100][0], 10.0)    # inclusive: all 10
        entries, skipped = sotc_trace.pick_functions(scores, {0x1000: 'Game::Wait'},
                                                     self.fake_sig, top=5, min_ms=0.5)
        self.assertEqual([e['name'] for e in entries], ['Game_Wait'])
        self.assertEqual(skipped, [('sub_1100', 'not unique within 64 bytes: 3 matches')])

    def test_emitted_hooks_file_parses_and_has_the_hook_keys(self):
        t = sotc_trace.load(sample_trace(self.dir / 's.sotctrace'))
        scores = sotc_trace.function_scores([t], SYM_FUNCS)
        entries, _ = sotc_trace.pick_functions(scores, {0x1000: 'Game::Wait'}, self.fake_sig,
                                               top=5, min_ms=0.5)
        text = sotc_trace.hooks_source(entries, self.FP, 1)
        ns = {}
        exec(compile(text, 'trace_funcs.py', 'exec'), ns)
        self.assertEqual(ns['EBOOT_FINGERPRINT'], self.FP)
        self.assertEqual(len(ns['HOOKS']), 1)
        hook = ns['HOOKS'][0]
        # No 'group': one group per hook (the default), so one unresolved function does not
        # switch off the rest (time-mode convention, matches gpu/sotc/hooks.cpp).
        self.assertEqual(set(hook), {'name', 'switch', 'mode', 'pattern', 'expected'})
        self.assertEqual(hook['name'], 'Game_Wait')
        self.assertEqual(hook['switch'], 'SOTC_TRACE_FUNCS')
        self.assertEqual(hook['mode'], 'time')
        self.assertEqual(hook['pattern'], '55 48 89 e5 ?? ?? 41 57')
        self.assertEqual(hook['expected'], {self.FP: 0x1000})
        self.assertTrue(re.fullmatch(r'[A-Za-z][A-Za-z0-9_]{0,46}', hook['name']))

    def test_cli_writes_the_file_with_injected_index_and_sig(self):
        src = sample_trace(self.dir / 's.sotctrace')
        out = self.dir / 'hooks.d' / 'trace_funcs.py'
        buf = io.StringIO()
        with mock.patch.object(sotc_trace, 'eboot_funcs', return_value=SYM_FUNCS), \
                mock.patch.object(sotc_trace, 'eboot_fingerprint', return_value=self.FP), \
                mock.patch.object(sotc_trace, 'eboot_sig', side_effect=self.fake_sig), \
                contextlib.redirect_stdout(buf):
            rc = sotc_trace.main(['autofuncs', str(src), '-o', str(out), '--top', '5',
                                  '--min-ms', '0.5'])
        self.assertEqual(rc, 0)
        self.assertIn('wrote 1 function(s)', buf.getvalue())
        self.assertIn('skipped 1', buf.getvalue())
        ns = {}
        exec(compile(out.read_text(encoding='utf-8'), str(out), 'exec'), ns)
        self.assertEqual(ns['HOOKS'][0]['expected'], {self.FP: 0x1000})


EXE_BASE = 0x7FF712340000     # bb-probe.exe loaded by ASLR
EXE_PREF = 0x140000000        # its preferred base (PE ImageBase)
DLL_BASE = 0x7FFA00010000
DLL_PREF = 0x180000000
MODULES = [
    {'name': 'bb-probe.exe', 'base': EXE_BASE, 'size': 0x800000, 'preferred_base': EXE_PREF},
    {'name': 'nvoglv64.dll', 'base': DLL_BASE, 'size': 0x20000, 'preferred_base': DLL_PREF},
]


def module_trace(path, modules=MODULES):
    """A stream with one sample whose stack is a bb-probe.exe frame and a driver frame. The
    MODULES chunk is left out when modules is None (an old file)."""
    stack = [EXE_BASE + 0x1000, DLL_BASE + 0x1234]
    parts = [json_chunk(1, {'kind': 'stream', 'mode': 2}), json_chunk(3, {'1': 'sample'}),
             json_chunk(7, {'1': stack}),
             events_chunk(1, [record(MS, MS, 1, kind=3, aux=1, a=stack[0])])]
    if modules is not None:
        parts.insert(0, json_chunk(9, modules))
    return write(path, parts)


class ModulesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_modules_chunk_loads_and_old_files_have_none(self):
        t = sotc_trace.load(module_trace(self.dir / 'm.sotctrace'))
        self.assertEqual(t.modules, MODULES)
        self.assertEqual(t.chunks[9], 1)
        old = sotc_trace.load(module_trace(self.dir / 'old.sotctrace', modules=None))
        self.assertEqual(old.modules, [])

    def test_dll_address_is_module_and_rva_without_llvm(self):
        t = sotc_trace.load(module_trace(self.dir / 'd.sotctrace'))
        sym = sotc_trace.Symbolizer(modules=t.modules)   # no host_exe: llvm-symbolizer is not needed
        with mock.patch.object(sotc_trace.subprocess, 'run',
                               side_effect=AssertionError('llvm-symbolizer called for a DLL')):
            self.assertEqual(sym.label(DLL_BASE + 0x1234), 'nvoglv64.dll+0x1234')
            self.assertEqual(sym.label(DLL_BASE + 0x20000), f'{DLL_BASE + 0x20000:#x}')  # past it
            self.assertEqual(sym.label(0x500), '0x500')                                 # no module

    def test_exe_address_is_slide_adjusted_for_llvm(self):
        t = sotc_trace.load(module_trace(self.dir / 'e.sotctrace'))
        proc = mock.Mock(stdout='ReportArea\nprobe_area.cpp:247:0\n\n')
        with mock.patch.object(sotc_trace, 'find_llvm_symbolizer', return_value='llvm-symbolizer'), \
                mock.patch.object(sotc_trace.subprocess, 'run', return_value=proc) as run:
            sym = sotc_trace.Symbolizer(modules=t.modules, host_exe='C:/bb-probe.exe')
            self.assertEqual(sym.label(EXE_BASE + 0x1000), 'ReportArea')
        self.assertEqual(run.call_count, 1)
        cmd = run.call_args[0][0]
        slide = EXE_BASE - EXE_PREF
        self.assertIn('--obj=C:/bb-probe.exe', cmd)
        self.assertIn(f'--adjust-vma={slide:#x}', cmd)
        self.assertEqual(cmd[-1], f'{EXE_BASE + 0x1000:#x}')   # the runtime address, unchanged

    def test_exe_negative_slide_is_passed_as_unsigned(self):
        mods = [{'name': 'bb-probe.exe', 'base': 0x100000, 'size': 0x100000,
                 'preferred_base': EXE_PREF}]
        proc = mock.Mock(stdout='Low\nx.cpp:1:0\n\n')
        with mock.patch.object(sotc_trace, 'find_llvm_symbolizer', return_value='llvm-symbolizer'), \
                mock.patch.object(sotc_trace.subprocess, 'run', return_value=proc) as run:
            sym = sotc_trace.Symbolizer(modules=mods, host_exe='C:/bb-probe.exe')
            self.assertEqual(sym.label(0x100010), 'Low')
        slide = 0x100000 - EXE_PREF
        self.assertIn(f'--adjust-vma={slide % (1 << 64):#x}', run.call_args[0][0])

    def test_old_file_keeps_the_raw_host_lookup(self):
        t = sotc_trace.load(module_trace(self.dir / 'o.sotctrace', modules=None))
        proc = mock.Mock(stdout='Raw\nx.cpp:1:0\n\n')
        with mock.patch.object(sotc_trace, 'find_llvm_symbolizer', return_value='llvm-symbolizer'), \
                mock.patch.object(sotc_trace.subprocess, 'run', return_value=proc) as run:
            sym = sotc_trace.Symbolizer(modules=t.modules, host_exe='C:/bb-probe.exe')
            self.assertEqual(sym.label(EXE_BASE + 0x1000), 'Raw')
        cmd = run.call_args[0][0]
        self.assertFalse(any(arg.startswith('--adjust-vma') for arg in cmd))
        self.assertEqual(cmd[-1], f'{EXE_BASE + 0x1000:#x}')


def make_pe(path, image_base, size_of_image, magic=0x20B):
    """A minimal PE file: DOS header, PE signature, COFF header and an optional header with the
    given ImageBase (PE32+ when magic is 0x20B, PE32 when 0x10B) and SizeOfImage. No sections."""
    dos = bytearray(0x80)
    dos[0:2] = b'MZ'
    struct.pack_into('<I', dos, 0x3C, 0x80)
    coff = struct.pack('<HHIIIHH', 0x8664, 0, 0, 0, 0, 240, 0x22)
    opt = bytearray(240)
    struct.pack_into('<H', opt, 0, magic)
    if magic == 0x20B:
        struct.pack_into('<Q', opt, 24, image_base)
    else:
        struct.pack_into('<I', opt, 28, image_base)
    struct.pack_into('<I', opt, 56, size_of_image)
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(bytes(dos) + b'PE\x00\x00' + coff + bytes(opt))
    return Path(path)


class PeFileSlideTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_pe_headers_give_image_base_and_size(self):
        exe = make_pe(self.dir / 'p.exe', 0x140000000, 0xBB9C000)
        self.assertEqual(sotc_trace.pe_image_info(exe),
                         {'image_base': 0x140000000, 'size_of_image': 0xBB9C000})
        pe32 = make_pe(self.dir / 'p32.exe', 0x400000, 0x9000, magic=0x10B)
        self.assertEqual(sotc_trace.pe_image_info(pe32),
                         {'image_base': 0x400000, 'size_of_image': 0x9000})

    def test_not_a_pe_file_gives_none(self):
        (self.dir / 'text.exe').write_bytes(b'hello, not a PE file')
        self.assertIsNone(sotc_trace.pe_image_info(self.dir / 'text.exe'))
        self.assertIsNone(sotc_trace.pe_image_info(self.dir / 'missing.exe'))

    def test_slide_uses_the_file_base_not_the_recorded_preferred_base(self):
        # The recorded preferred_base is the runtime base (a relocated image): the file decides.
        base = 0x7FF7EB7E0000
        mods = [{'name': 'bb-probe.exe', 'base': base, 'size': 0x800000, 'preferred_base': base}]
        exe = make_pe(self.dir / 'bb-probe.exe', 0x140000000, 0x800000)
        proc = mock.Mock(stdout='Fn\nx.cpp:1:0\n\n')
        with mock.patch.object(sotc_trace, 'find_llvm_symbolizer', return_value='llvm-symbolizer'), \
                mock.patch.object(sotc_trace.subprocess, 'run', return_value=proc) as run:
            sym = sotc_trace.Symbolizer(modules=mods, host_exe=str(exe),
                                        exe_image_base=sotc_trace.pe_image_info(exe)['image_base'])
            self.assertEqual(sym.label(base + 0x10FB), 'Fn')
        cmd = run.call_args[0][0]
        self.assertIn(f'--adjust-vma={base - 0x140000000:#x}', cmd)
        self.assertEqual(cmd[-1], f'{base + 0x10FB:#x}')

    def test_without_the_file_the_recorded_base_is_still_the_fallback(self):
        base = 0x7FF7EB7E0000
        mods = [{'name': 'bb-probe.exe', 'base': base, 'size': 0x800000, 'preferred_base': 0x140000000}]
        proc = mock.Mock(stdout='Fn\nx.cpp:1:0\n\n')
        with mock.patch.object(sotc_trace, 'find_llvm_symbolizer', return_value='llvm-symbolizer'), \
                mock.patch.object(sotc_trace.subprocess, 'run', return_value=proc) as run:
            sym = sotc_trace.Symbolizer(modules=mods, host_exe='C:/bb-probe.exe')
            sym.label(base + 0x10)
        self.assertIn(f'--adjust-vma={base - 0x140000000:#x}', run.call_args[0][0])

    def test_make_symbolizer_takes_the_newer_copy_and_its_base(self):
        base = 0x7FF7EB7E0000
        old = make_pe(self.dir / 'old' / 'bb-probe.exe', 0x140000000, 0x800000)
        new = make_pe(self.dir / 'new' / 'bb-probe.exe', 0x140000000, 0x800000)
        os.utime(old, (1_000_000_000, 1_000_000_000))
        mods = [{'name': 'bb-probe.exe', 'base': base, 'size': 0x800000, 'preferred_base': base}]
        t = sotc_trace.load(module_trace(self.dir / 'mk.sotctrace', modules=mods))
        proc = mock.Mock(stdout='Fn\nx.cpp:1:0\n\n')
        with mock.patch.object(sotc_trace, 'EXE_CANDIDATES', (old, new)), \
                mock.patch.object(sotc_trace, 'find_llvm_symbolizer', return_value='llvm-symbolizer'), \
                mock.patch.object(sotc_trace.subprocess, 'run', return_value=proc) as run:
            self.assertEqual(sotc_trace.host_exe_path(), new)   # the older copy has 2001 mtime
            sym = sotc_trace.make_symbolizer(t, use_index=False)
            self.assertEqual(sym.exe_image_base, 0x140000000)
            self.assertEqual(sym.label(base + 0x1000), 'Fn')
        self.assertIn(f'--adjust-vma={base - 0x140000000:#x}', run.call_args[0][0])

    def test_newer_file_warns_once(self):
        exe = make_pe(self.dir / 'late' / 'bb-probe.exe', 0x140000000, 0x800000)
        os.utime(exe, (4_000_000_000, 4_000_000_000))
        mods = [{'name': 'bb-probe.exe', 'base': 0x7FF7EB7E0000, 'size': 0x800000,
                 'preferred_base': 0x7FF7EB7E0000}]
        t = sotc_trace.load(module_trace(self.dir / 'warn.sotctrace', modules=mods))
        t.meta['created_unix'] = 1_700_000_000
        err = io.StringIO()
        with mock.patch.object(sotc_trace, 'EXE_CANDIDATES', (exe,)), \
                mock.patch.object(sotc_trace, 'find_llvm_symbolizer', return_value=None), \
                contextlib.redirect_stderr(err):
            sotc_trace.make_symbolizer(t, use_index=False)
            sotc_trace.make_symbolizer(t, use_index=False)
        self.assertEqual(err.getvalue().count('is newer than warn.sotctrace'), 1)

    def test_size_mismatch_with_the_recorded_module_warns(self):
        exe = make_pe(self.dir / 'size' / 'bb-probe.exe', 0x140000000, 0x900000)
        mods = [{'name': 'bb-probe.exe', 'base': 0x7FF7EB7E0000, 'size': 0x800000,
                 'preferred_base': 0x7FF7EB7E0000}]
        t = sotc_trace.load(module_trace(self.dir / 'size.sotctrace', modules=mods))
        err = io.StringIO()
        with mock.patch.object(sotc_trace, 'EXE_CANDIDATES', (exe,)), \
                mock.patch.object(sotc_trace, 'find_llvm_symbolizer', return_value=None), \
                contextlib.redirect_stderr(err):
            sotc_trace.make_symbolizer(t, use_index=False)
        self.assertIn('SizeOfImage 0x900000 on disk, 0x800000', err.getvalue())


MUTEX_NAMES = {1: 'mutex wait', 2: 'mutex holder', 3: 'frame'}


def mutex_trace(path, holders=True):
    """A slow frame (500-600 ms) with three contended waits on the draw thread: two with a holder
    span (same t0 and t1; the holders are thread os_tid 101 and an unknown id 999) and one without.
    A wait of 1 ms sits in a normal frame and must not count. holders=False drops the holder
    spans (an old trace)."""
    threads = [{'id': 1, 'os_tid': 100, 'name': 'GpuDraw', 'kind': 'draw', 'sort': 2000},
               {'id': 2, 'os_tid': 101, 'name': 'Shadow Game Upd', 'kind': 'guest', 'sort': 1000},
               {'id': 3, 'os_tid': 102, 'name': 'frames', 'kind': 'frames', 'sort': 0}]
    frames = [record(i * 40 * MS, i * 40 * MS + 33 * MS, 3, kind=6, a=i) for i in range(3)]
    frames.append(record(500 * MS, 600 * MS, 3, kind=6, flags=1, a=99))
    draw = [record(1 * MS, 2 * MS, 1, b=IMAGE_BASE + 0x1010)]
    waits = [(510 * MS, 550 * MS, IMAGE_BASE + 0x1010, 101, IMAGE_BASE + 0x1110),
             (570 * MS, 590 * MS, IMAGE_BASE + 0x1020, 999, IMAGE_BASE + 0x1120),
             (590 * MS, 595 * MS, IMAGE_BASE + 0x1010, None, None)]
    for t0, t1, b, a, site in waits:
        draw.append(record(t0, t1, 1, b=b))
        if holders and a is not None:
            draw.append(record(t0, t1, 2, a=a, b=site))
    parts = [json_chunk(1, {'version': 1, 'mode': 2, 'kind': 'stream', 'created_unix': 1000,
                            'image_base': IMAGE_BASE, 'image_size': IMAGE_SIZE}),
             json_chunk(2, threads), json_chunk(3, {str(k): v for k, v in MUTEX_NAMES.items()}),
             events_chunk(1, draw), events_chunk(3, frames)]
    return write(path, parts)


def mutex_sym():
    return sotc_trace.Symbolizer(IMAGE_BASE, IMAGE_SIZE,
                                 funcs=[(0x1000, 0x1100), (0x1100, 0x1200)],
                                 names={0x1000: 'Game::Wait', 0x1100: 'Game::Lock'})


class MutexHolderTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_holder_table_of_the_slow_frame(self):
        t = sotc_trace.load(mutex_trace(self.dir / 'm.sotctrace'))
        results = sotc_trace.hitch_file(t, mutex_sym())
        self.assertEqual(len(results), 1)
        m = results[0]['mutex']
        self.assertAlmostEqual(m['total_ms'], 65.0)
        self.assertEqual(m['sites'], [('Game::Lock+0x10', 'Game::Wait+0x10', 40.0),
                                      ('Game::Lock+0x20', 'Game::Wait+0x20', 20.0),
                                      ('-', 'Game::Wait+0x10', 5.0)])
        self.assertEqual(m['holders'], [('game: Shadow Game Upd', 40.0),
                                        ('thread id 999', 20.0), ('-', 5.0)])
        self.assertFalse(any(row['name'] == 'mutex holder' for row in results[0]['top']))
        md = '\n'.join(sotc_trace.hitch_markdown(t.path.name, results))
        self.assertIn('| 40.0 | Game::Lock+0x10 | Game::Wait+0x10 |', md)
        self.assertIn('Holder threads: game: Shadow Game Upd 40.0 ms; thread id 999 20.0 ms; - 5.0 ms.',
                      md)

    def test_old_trace_without_holders_prints_nothing_extra(self):
        t = sotc_trace.load(mutex_trace(self.dir / 'old.sotctrace', holders=False))
        results = sotc_trace.hitch_file(t, mutex_sym())
        self.assertIsNone(results[0]['mutex'])
        md = '\n'.join(sotc_trace.hitch_markdown(t.path.name, results))
        self.assertNotIn('Mutex', md)
        self.assertNotIn('Holder threads', md)

    def test_top_lists_the_whole_trace_table(self):
        t = sotc_trace.load(mutex_trace(self.dir / 'top.sotctrace'))
        lines = sotc_trace.top_report(t, 8, None, mutex_sym())
        self.assertIn('mutex waits, whole trace (66.00 ms): holder site <- waiter caller, '
                      'then holder threads:', lines)
        self.assertIn('  %10.2f ms  %s  <-  %s' % (40.0, 'Game::Lock+0x10', 'Game::Wait+0x10'),
                      lines)
        self.assertTrue(any(line.startswith('  holders: game: Shadow Game Upd 40.00 ms')
                            for line in lines))

    def test_top_without_holders_has_no_mutex_lines(self):
        t = sotc_trace.load(mutex_trace(self.dir / 'top-old.sotctrace', holders=False))
        lines = sotc_trace.top_report(t, 8, None, mutex_sym())
        self.assertFalse(any('mutex waits' in line for line in lines))

    def test_pairing_needs_the_same_times(self):
        t = sotc_trace.load(mutex_trace(self.dir / 'pair.sotctrace'))
        pairs = sotc_trace.mutex_pairs(t)
        self.assertEqual(len(pairs), 4)
        self.assertEqual(sum(1 for _, h in pairs if h is not None), 2)


def copy_trace(path, copies=(), redirty=(), faults=(), frames=(), window=None, hle=()):
    """A dump with sync copy spans (t0, t1, aux, bytes, b), re-dirty instants (t, kind, bytes,
    addr), write faults (t, addr), frames (t0, t1, number, slow) and hle fill instants (t, path,
    log2 period, bytes, addr). window: META window_ns."""
    names = {}

    def nid(name):
        return names.setdefault(name, len(names) + 1)

    recs = []
    for t0, t1, aux, nbytes, addr in copies:
        recs.append(record(t0, t1, nid('sync copy'), aux=aux, a=nbytes, b=addr))
    for t, kind, nbytes, addr in redirty:
        recs.append(record(t, t, nid('redirty ' + kind), kind=1, a=nbytes, b=addr))
    for t, addr in faults:
        recs.append(record(t, t + 1000, nid('write fault'), a=addr))
    for t0, t1, num, slow in frames:
        recs.append(record(t0, t1, nid('frame'), kind=6, flags=1 if slow else 0, a=num))
    for t, path_code, log2, nbytes, addr in hle:
        recs.append(record(t, t, nid('hle fill'), kind=1, aux=path_code, flags=log2, a=nbytes,
                           b=addr))
    meta = {'version': 1, 'mode': 2, 'kind': 'dump'}
    if window is not None:
        meta['window_ns'] = list(window)
    threads = [{'id': 1, 'os_tid': 100, 'name': 'Shadow Game Upd', 'kind': 'guest', 'sort': 1}]
    parts = [json_chunk(1, meta), json_chunk(2, threads),
             json_chunk(3, {str(v): k for k, v in names.items()}), events_chunk(1, recs)]
    return write(path, parts)


# Overlap data: bind copies in regions 16 (3 MiB) and 17 (1 MiB); written copies in region 18.
# First touch marks region 16, evict marks region 17; write faults hit regions 17 and 18.
OVERLAP_COPIES = [(1 * MS, 2 * MS, 1, 3 << 20, 0x40000000),
                  (3 * MS, 4 * MS, 1, 1 << 20, 0x44000000),
                  (5 * MS, 6 * MS, 2, 2 << 20, 0x48000000)]
OVERLAP_REDIRTY = [(1 * MS, 'first touch', 3 << 20, 0x40000010),
                   (2 * MS, 'evict', 1 << 20, 0x44000010)]
OVERLAP_FAULTS = [(1 * MS, 0x44000000), (2 * MS, 0x48000000)]


class CopySrcTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def load(self, name, **kw):
        return sotc_trace.load(copy_trace(self.dir / name, **kw))

    def test_new_format_split_by_reason_and_window(self):
        copies = [(1 * MS, 3 * MS, 1, 1 << 20, 0x40001000),
                  (5 * MS, 8 * MS, 1, 100 << 10, 0x44000000),
                  (10 * MS, 14 * MS, 2, 2 << 20, 0x40002000),
                  (20 * MS, 28 * MS, 3, 40 << 20, 0x40000000),
                  (30 * MS, 31 * MS, 7, 10, 0x40000000),
                  (200 * MS, 201 * MS, 1, 1000, 0x40000000)]    # after the dump window
        tot = sotc_trace.copy_totals(self.load('n.sotctrace', copies=copies,
                                               window=(0, 100 * MS)))
        self.assertEqual(tot['window'], 'dump window 0.0-100.0 ms')
        bind = tot['reasons'][1]
        self.assertEqual((bind['count'], bind['bytes'], bind['unknown']),
                         (2, (1 << 20) + (100 << 10), 0))
        self.assertAlmostEqual(bind['ms'], 5.0)
        self.assertEqual(dict(bind['buckets']), {16: 1 << 20, 17: 100 << 10})
        self.assertEqual(dict(bind['sizes_n']), {'1-8M': 1, '64K-1M': 1})
        self.assertEqual(dict(tot['reasons'][2]['sizes_n']), {'1-8M': 1})
        self.assertEqual(dict(tot['reasons'][3]['sizes_n']), {'>=32M': 1})
        self.assertEqual(tot['reasons'][7]['bytes'], 10)
        self.assertEqual(sotc_trace.reason_label(3), 'dma')
        self.assertEqual(sotc_trace.reason_label(7), 'code 7')
        self.assertEqual(sotc_trace.size_label(63 << 10), '<64K')
        self.assertEqual(sotc_trace.size_label(64 << 10), '64K-1M')
        self.assertEqual(sotc_trace.size_label(8 << 20), '8-32M')
        self.assertEqual(sotc_trace.size_label(32 << 20), '>=32M')
        text = '\n'.join(sotc_trace.copy_lines([('n', tot)], tot))
        self.assertIn('| bind | 2 | 1.1 | 5.0 | 0.0 |', text)
        self.assertIn('| 0x40000000-0x43ffffff | 1.0 |', text)
        self.assertIn('Conclusion: the dominant copy reason is dma', text)

    def test_old_format_copies_count_as_untagged_without_address(self):
        copies = [(1 * MS, 2 * MS, 0, 1 << 20, 3), (5 * MS, 6 * MS, 0, 2 << 20, 2)]  # b = pieces
        tot = sotc_trace.copy_totals(self.load('old.sotctrace', copies=copies))
        r = tot['reasons'][0]
        self.assertEqual((r['count'], r['bytes'], r['unknown']), (2, 3 << 20, 3 << 20))
        self.assertEqual(dict(r['buckets']), {})
        row = sotc_trace.copy_overlap(tot)[0]
        self.assertEqual(row['known'], 0)
        self.assertIsNone(row['faults'])
        text = '\n'.join(sotc_trace.copy_lines([('old', tot)], tot))
        self.assertIn('| untagged | 2 | 3.0 | 2.0 | 3.0 |', text)
        self.assertIn('No copy with a known address', text)
        self.assertIn('the dominant copy reason is untagged (3.0 of 3.0 MiB, 100% of the bytes)',
                      text)

    def test_redirty_totals_and_top_regions(self):
        redirty = [(1 * MS, 'first touch', 1 << 20, 0x40000000),
                   (2 * MS, 'first touch', 2 << 20, 0x44000000),
                   (3 * MS, 'evict', 4096, 0x40001000)]
        tot = sotc_trace.copy_totals(self.load('r.sotctrace', redirty=redirty,
                                               window=(0, 100 * MS)))
        ft, ev = tot['redirty']['first touch'], tot['redirty']['evict']
        self.assertEqual((ft['count'], ft['bytes']), (2, 3 << 20))
        self.assertEqual((ev['count'], ev['bytes']), (1, 4096))
        text = '\n'.join(sotc_trace.copy_lines([('r', tot)], tot))
        self.assertIn('| first touch | 2 | 3.0 |', text)
        self.assertIn('| evict | 1 | 0.0 |', text)
        # Region 17 holds 2 MiB and region 16 about 1 MiB: the bigger one comes first.
        self.assertLess(text.index('| 0x44000000-0x47ffffff | 2.0 | first touch 2.0 |'),
                        text.index('| 0x40000000-0x43ffffff | 1.0 | evict 0.0, first touch 1.0 |'))
        self.assertIn('Conclusion: no sync copy span in the window.', text)

    def test_overlap_fractions_by_reason(self):
        t = self.load('o.sotctrace', copies=OVERLAP_COPIES, redirty=OVERLAP_REDIRTY,
                      faults=OVERLAP_FAULTS, window=(0, 100 * MS))
        by = {r['reason']: r for r in sotc_trace.copy_overlap(sotc_trace.copy_totals(t))}
        self.assertEqual(by[1]['known'], 4 << 20)
        self.assertAlmostEqual(by[1]['kinds']['first touch'], 0.75)   # 3 of 4 MiB in region 16
        self.assertAlmostEqual(by[1]['kinds']['evict'], 0.25)         # 1 of 4 MiB in region 17
        self.assertAlmostEqual(by[1]['faults'], 0.25)                 # region 17 faulted
        self.assertEqual(by[2]['kinds'], {'evict': 0.0, 'first touch': 0.0})
        self.assertAlmostEqual(by[2]['faults'], 1.0)                  # region 18 faulted

    def test_pooling_adds_bytes_and_keeps_the_shares(self):
        pooled = sotc_trace.new_copy_totals()
        for name in ('p1.sotctrace', 'p2.sotctrace'):
            t = self.load(name, copies=OVERLAP_COPIES, redirty=OVERLAP_REDIRTY,
                          faults=OVERLAP_FAULTS)
            sotc_trace.merge_copy_totals(pooled, sotc_trace.copy_totals(t))
        self.assertEqual(pooled['files'], 2)
        self.assertEqual(pooled['reasons'][1]['bytes'], 8 << 20)
        self.assertEqual(pooled['redirty']['first touch']['count'], 2)
        by = {r['reason']: r for r in sotc_trace.copy_overlap(pooled)}
        self.assertAlmostEqual(by[1]['kinds']['first touch'], 0.75)
        text = '\n'.join(sotc_trace.copy_lines([], pooled))
        self.assertIn('## Pooled over 2 dump(s)', text)
        self.assertIn('dominant re-dirty kind is first touch (6.0 MiB in 2 markers)', text)

    def test_hle_fill_paths_are_pooled(self):
        hle = [(1 * MS, 1, 16, 8 << 20, 0x1068b00000),
               (2 * MS, 1, 16, 8 << 20, 0x1068b00000),
               (3 * MS, 2, 2, 1 << 20, 0x40000000)]
        pooled = sotc_trace.new_copy_totals()
        for name in ('h1.sotctrace', 'h2.sotctrace'):
            sotc_trace.merge_copy_totals(pooled, sotc_trace.copy_totals(self.load(name, hle=hle)))
        self.assertEqual(pooled['hle'][(1, 16)], {'count': 4, 'bytes': 32 << 20})
        self.assertEqual(pooled['hle'][(2, 2)], {'count': 2, 'bytes': 2 << 20})
        text = '\n'.join(sotc_trace.copy_lines([], pooled))
        self.assertIn('| replace (CPU-written) | 65536 B | 4 | 32.0 |', text)
        self.assertLess(text.index('replace (CPU-written)'), text.index('| GPU fill | 4 B | 2 |'))

    def test_frame_only_counts_the_slow_frame(self):
        copies = [(10 * MS, 11 * MS, 1, 1 << 20, 0x40000000),     # normal frame
                  (100 * MS, 101 * MS, 2, 2 << 20, 0x40000000),   # slow frame
                  (300 * MS, 301 * MS, 3, 4 << 20, 0x40000000)]   # outside every frame
        frames = [(0, 33 * MS, 1, False), (50 * MS, 250 * MS, 7, True)]
        whole = sotc_trace.copy_totals(self.load('f.sotctrace', copies=copies, frames=frames))
        self.assertEqual(sorted(whole['reasons']), [1, 2, 3])
        slow = sotc_trace.copy_totals(self.load('fs.sotctrace', copies=copies, frames=frames),
                                      frame_only=True)
        self.assertEqual(sorted(slow['reasons']), [2])
        self.assertEqual(slow['reasons'][2]['bytes'], 2 << 20)
        self.assertEqual(slow['window'], 'slow frame 7')
        calm = sotc_trace.copy_totals(self.load('calm.sotctrace', copies=copies,
                                                frames=[(0, 33 * MS, 1, False)]),
                                      frame_only=True)
        self.assertEqual(calm['reasons'], {})
        self.assertEqual(calm['window'], 'no slow frame, nothing counted')

    def test_hitch_line_gives_the_copy_split_of_the_slow_frame(self):
        frames = [(i * 40 * MS, i * 40 * MS + 33 * MS, i, False) for i in range(5)]
        frames.append((500 * MS, 700 * MS, 99, True))
        copies = [(510 * MS, 515 * MS, 1, 3 << 20, 0x40000000),
                  (600 * MS, 601 * MS, 3, 1 << 20, 0x40000000),
                  (800 * MS, 801 * MS, 2, 1 << 20, 0x40000000)]   # after the slow frame
        t = self.load('h.sotctrace', copies=copies, frames=frames)
        results = sotc_trace.hitch_file(t, sym())
        self.assertEqual(results[0]['copy'], {'bind': 3 << 20, 'dma': 1 << 20})
        md = '\n'.join(sotc_trace.hitch_markdown(t.path.name, results))
        self.assertIn('Sync copy by caller: bind 3.0 MiB, dma 1.0 MiB.', md)

    def test_old_format_hitch_has_no_copy_line(self):
        frames = [(0, 33 * MS, 1, False), (500 * MS, 700 * MS, 99, True)]
        copies = [(510 * MS, 515 * MS, 0, 3 << 20, 4)]
        t = self.load('oh.sotctrace', copies=copies, frames=frames)
        results = sotc_trace.hitch_file(t, sym())
        self.assertIsNone(results[0]['copy'])
        md = '\n'.join(sotc_trace.hitch_markdown(t.path.name, results))
        self.assertNotIn('Sync copy by caller', md)

    def test_cli_prints_and_writes_the_report(self):
        frames = [(0, 33 * MS, 1, False), (50 * MS, 250 * MS, 7, True)]
        path = copy_trace(self.dir / 'cli.sotctrace', copies=OVERLAP_COPIES,
                          redirty=OVERLAP_REDIRTY, faults=OVERLAP_FAULTS, frames=frames)
        md = self.dir / 'copy.md'
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = sotc_trace.main(['copysrc', str(path), '--md', str(md), '--frame-only'])
        self.assertEqual(rc, 0)
        self.assertIn('Window: slow frame 7.', out.getvalue())
        self.assertEqual(md.read_text(encoding='utf-8'), out.getvalue())


if __name__ == '__main__':
    unittest.main()
