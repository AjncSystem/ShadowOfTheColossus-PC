"""Tests for the SOTC_TRACE v2 reader and tools (tools/trace/sotc_trace.py, the classify and
summary tools). Synthetic files only: no game files, no out/re index needed."""
import contextlib
import gzip
import io
import json
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


if __name__ == '__main__':
    unittest.main()
