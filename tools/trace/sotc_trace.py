#!/usr/bin/env python3
"""sotc: reader and analysis CLI for SOTC_TRACE files (.sotctrace, trace v2, format in
docs/sotc/TRACE-FORMAT.md) and for the old Chrome JSON dumps (traceEvents, .json or .json.gz).

    python tools/trace/sotc_trace.py info  <file>
    python tools/trace/sotc_trace.py top   <file|folder...> [--top 8] [--folded out.folded]
    python tools/trace/sotc_trace.py hitch <file|folder...> [--md out.md] [--json out.json]
    python tools/trace/sotc_trace.py json  <file> [-o out.json.gz]
    python tools/trace/sotc_trace.py diff  A B [--md out.md] [--top 15]
    python tools/trace/sotc_trace.py autofuncs <file|folder...> -o scripts/hooks.d/trace_funcs.py
                                              [--top 20] [--min-ms 1.0]

top: total time by span name per thread, the sample profiles (flat and inclusive, symbolized),
the IMPORT_STATS totals and the GPU time (EvGpu). --folded writes Brendan Gregg folded stacks
(flamegraph.pl input).

hitch: for each slow frame, the time each thread spent in each span name inside it, against the
median of the normal frames of the same segment; the biggest growth first, with the guest callers
(the span's b field) and the cause of the frame (draw, gpu, readback, fault, io, game-cpu, other).
When the trace has samples, imports or GPU spans: the sampled functions that grew, the import time
that grew and the GPU verdict (GPU-bound or CPU-bound).

json: Chrome JSON that Perfetto opens (gzip when the name ends in .gz). Samples are instants with
the symbolized stack in args; EvGpu spans are on one "GPU" track.

diff: aggregates the traces of A and B (a file, or a folder: every .sotctrace under it) per
category, and prints B-A per category, per import, per wait kind, per span and per sampled
function (means per trace).

autofuncs: picks the guest functions with the most sampled time and hitch span time, reads a
unique byte signature of each with tools/re/eboot_re.py sig, and writes a scripts/hooks.d file of
mode 'time' hooks (switch SOTC_TRACE_FUNCS, group trace_funcs).

Guest addresses are symbolized from out/re/index.pkl.gz (build it once with tools/re/eboot_re.py
build), names from tools/re/names.txt and out/re/ghidra/names2.txt. Host addresses use
llvm-symbolizer on out/gpu/bb-probe.exe when llvm-symbolizer is on PATH. Pure standard library.
"""
import argparse
import bisect
import collections
import functools
import gzip
import json
import os
import re
import shutil
import statistics
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAGIC = b'SOTCTRC\x01'
CHUNK_HDR = struct.Struct('<IIII')          # type, flags, stored_size, raw_size
EVENTS_HDR = struct.Struct('<II')           # ring id, count
RECORD = struct.Struct('<QQIBBHIIQQ')       # 48 bytes
RECORD_SIZE = RECORD.size
T_META, T_THREADS, T_NAMES, T_EVENTS, T_ZONE_STATS = 1, 2, 3, 4, 5
T_IMPORT_STATS, T_STACKS, T_GPU_CALIB, T_MODULES, T_END = 6, 7, 8, 9, 0xFFFFFFFF
CHUNK_NAMES = {T_META: 'META', T_THREADS: 'THREADS', T_NAMES: 'NAMES', T_EVENTS: 'EVENTS',
               T_ZONE_STATS: 'ZONE_STATS', T_IMPORT_STATS: 'IMPORT_STATS', T_STACKS: 'STACKS',
               T_GPU_CALIB: 'GPU_CALIB', T_MODULES: 'MODULES', T_END: 'END'}
K_SPAN, K_INSTANT, K_COUNTER, K_SAMPLE, K_IMPORT, K_GPU, K_FRAME, K_MARK = range(8)
KIND_NAMES = ['span', 'instant', 'counter', 'sample', 'import', 'gpu', 'frame', 'mark']
FLAG_SLOW = 1
# Chrome JSON dumps write guest addresses as "eboot+0x..": they map into this fake image.
JSON_IMAGE_BASE = 0x4000_0000_0000
JSON_IMAGE_SIZE = 0x1000_0000
PLAIN_LIMIT = 1_000_000   # smaller values are plain numbers, not addresses (recorder rule)
HOST_MIN = 0x10000        # smaller values are not host addresses
GPU_TID = 999_999         # the "GPU" track of the Chrome JSON
SAMPLE_CAP_NS = 100_000_000   # a sample weighs the time to the next one, at most this
CATEGORIES = ('draw ms', 'gpu ms', 'imports ms', 'waits ms', 'samples', 'slow frames')
Sample = collections.namedtuple('Sample', 'ns thread stack')   # stack: names, leaf first


class Ev:
    """One recorded event. t0 and t1 are ns on the file's clock (JSON: µs * 1000); thread is the
    ring id. Until Trace.finish() runs, name holds the NAMES id of a .sotctrace file."""
    __slots__ = ('t0', 't1', 'name', 'kind', 'flags', 'aux', 'a', 'b', 'thread')

    def __init__(self, t0, t1, name, kind, flags, aux, a, b, thread):
        self.t0 = t0
        self.t1 = t1
        self.name = name
        self.kind = kind
        self.flags = flags
        self.aux = aux
        self.a = a
        self.b = b
        self.thread = thread


class Trace:
    """A loaded file. meta is the merged META object, threads maps ring id -> THREADS entry,
    events maps ring id -> [Ev] sorted by t0, stacks maps stack id -> [guest or host address],
    leaf first, import_stats is the last IMPORT_STATS list, gpu_calib the GPU_CALIB object,
    modules the last MODULES list (host modules, see _module)."""

    def __init__(self, path, fmt):
        self.path = Path(path)
        self.format = fmt                      # 'sotctrace' or 'json'
        self.meta = {}
        self.threads = {}
        self.names = {}
        self.events = {}
        self.zone_stats = []
        self.stacks = {}
        self.import_stats = []
        self.gpu_calib = {}
        self.modules = []                      # MODULES: host modules, the last list wins
        self.complete = False                  # an END chunk was read
        self.truncated = False                 # a chunk header or payload ran past the end
        self.bad_chunks = 0                    # a payload did not inflate or parse
        self.chunks = collections.Counter()    # known chunk type -> count
        self.unknown = collections.Counter()   # unknown chunk type -> count

    def thread_label(self, ring):
        """The track name: the game's threads get the "game: " prefix, as the recorder wrote them."""
        t = self.threads.get(ring)
        if t is None:
            return f'thread {ring}'
        name = t.get('name') or f"thread {t.get('os_tid', ring)}"
        if t.get('kind') == 'guest' and not name.startswith(('shadPS4', 'game: ')):
            return 'game: ' + name
        return name

    def all_events(self):
        for evs in self.events.values():
            yield from evs

    def finish(self):
        for evs in self.events.values():
            for e in evs:
                if isinstance(e.name, int):
                    e.name = '' if e.name == 0 else self.names.get(e.name, f'name#{e.name}')
            evs.sort(key=lambda e: e.t0)


def load(path):
    """Reads a .sotctrace file, or a Chrome JSON dump (.json or .json.gz)."""
    path = Path(path)
    data = path.read_bytes()
    if data[:2] == b'\x1f\x8b':
        data = gzip.decompress(data)
    if data[:7] == MAGIC[:7] and data[:8] != MAGIC:
        raise ValueError(f'{path}: unsupported .sotctrace version {data[7:8].hex()}')
    if data[:8] == MAGIC:
        return _load_binary(path, data)
    return _load_json(path, data)


def _load_binary(path, data):
    t = Trace(path, 'sotctrace')
    pos, n = len(MAGIC), len(data)
    while pos < n:
        if pos + CHUNK_HDR.size > n:
            t.truncated = True
            break
        typ, flags, stored, _raw = CHUNK_HDR.unpack_from(data, pos)
        pos += CHUNK_HDR.size
        if pos + stored > n:
            t.truncated = True
            break
        payload = data[pos:pos + stored]
        pos += stored
        if flags & 1:
            try:
                payload = zlib.decompress(payload)
            except zlib.error:
                t.bad_chunks += 1
                continue
        if typ == T_END:
            t.complete = True
            t.chunks[typ] += 1
            continue
        if typ not in CHUNK_NAMES:
            t.unknown[typ] += 1
            continue
        try:
            _apply_chunk(t, typ, payload)
            t.chunks[typ] += 1
        except (ValueError, TypeError, KeyError, AttributeError, UnicodeDecodeError, struct.error):
            t.bad_chunks += 1
    t.finish()
    return t


def _addr(v):
    """A STACKS address: a JSON number, or hex text."""
    return int(v, 0) if isinstance(v, str) else int(v)


def _module(m):
    """A MODULES entry: name, base, size and preferred_base (ints; 0 when unknown)."""
    return {'name': str(m.get('name', '')), 'base': _addr(m.get('base') or 0),
            'size': _addr(m.get('size') or 0), 'preferred_base': _addr(m.get('preferred_base') or 0)}


def _apply_chunk(t, typ, payload):
    if typ == T_META:
        t.meta.update(json.loads(payload.decode('utf-8')))
    elif typ == T_THREADS:
        for entry in json.loads(payload.decode('utf-8')):
            t.threads[int(entry['id'])] = entry
    elif typ == T_NAMES:
        for key, value in json.loads(payload.decode('utf-8')).items():
            t.names[int(key)] = value
    elif typ == T_EVENTS:
        ring, count = EVENTS_HDR.unpack_from(payload, 0)
        body = payload[EVENTS_HDR.size:]
        count = min(count, len(body) // RECORD_SIZE)   # a short chunk keeps its whole records
        out = t.events.setdefault(ring, [])
        for t0, t1, name, kind, _r, flags, aux, _r2, a, b in RECORD.iter_unpack(
                body[:count * RECORD_SIZE]):
            out.append(Ev(t0, t1, name, kind, flags, aux, a, b, ring))
    elif typ == T_ZONE_STATS:
        t.zone_stats = json.loads(payload.decode('utf-8'))
    elif typ == T_IMPORT_STATS:
        t.import_stats = json.loads(payload.decode('utf-8'))      # cumulative: the last wins
    elif typ == T_STACKS:
        for key, addrs in json.loads(payload.decode('utf-8')).items():   # incremental
            t.stacks[int(key)] = [_addr(x) for x in addrs]
    elif typ == T_GPU_CALIB:
        t.gpu_calib = json.loads(payload.decode('utf-8'))
    elif typ == T_MODULES:
        t.modules = [_module(m) for m in json.loads(payload.decode('utf-8')) if isinstance(m, dict)]


def _json_num(v):
    """A recorder-style argument: a number, "0x.." hex text, or "eboot+0x.." guest text."""
    if v is None or isinstance(v, bool):
        return 0
    if isinstance(v, (int, float)):
        return int(v)
    s = str(v)
    if s.startswith('eboot+0x'):
        return JSON_IMAGE_BASE + int(s[6:], 16)
    try:
        return int(s, 16) if s.startswith('0x') else int(s)
    except ValueError:
        return 0


def _load_json(path, data):
    try:
        doc = json.loads(data.decode('utf-8'))
    except (UnicodeDecodeError, ValueError) as err:
        raise ValueError(f'{path}: neither a .sotctrace file nor a Chrome JSON trace ({err})') \
            from None
    if isinstance(doc, dict):
        events = doc.get('traceEvents', [])
        other = doc.get('otherData') or {}
    else:
        events, other = doc, {}
    if not isinstance(events, list):
        raise ValueError(f'{path}: traceEvents is not a list')
    t = Trace(path, 'json')
    for key in ('frame', 'worst_ms', 'slow_frames', 'window_ms'):
        if key in other:
            t.meta[key] = other[key]
    if 'frame' in other:
        t.meta['kind'] = 'dump'
        t.meta['reason'] = 'slow'
    t.meta['image_base'] = JSON_IMAGE_BASE
    t.meta['image_size'] = JSON_IMAGE_SIZE
    for e in events:
        if not isinstance(e, dict):
            continue
        ph = e.get('ph')
        tid = int(e.get('tid', 0) or 0)
        name = e.get('name', '')
        args = e.get('args') or {}
        if ph == 'M':
            entry = t.threads.setdefault(
                tid, {'id': tid, 'os_tid': tid, 'kind': None, 'sort': tid, 'name': ''})
            if name == 'thread_name':
                entry['name'] = str(args.get('name', ''))
            elif name == 'thread_sort_index':
                entry['sort'] = _json_num(args.get('sort_index', tid))
            continue
        if ph not in ('X', 'i'):
            continue
        t0 = round(float(e.get('ts', 0)) * 1000)
        t1 = t0 + round(float(e.get('dur', 0) or 0) * 1000) if ph == 'X' else t0
        flags = 0
        kind = K_SPAN if ph == 'X' else K_INSTANT
        if name in ('SLOW FRAME', 'frame'):
            kind = K_FRAME
            flags = FLAG_SLOW if name == 'SLOW FRAME' else 0
            name = 'frame'          # the Lab segment is not in the old format
        ev = Ev(t0, t1, name, kind, flags, 0, _json_num(args.get('a')),
                _json_num(args.get('b')), tid)
        t.events.setdefault(tid, []).append(ev)
    t.finish()
    return t


# ---------------------------------------------------------------- symbols

def read_names(path):
    """"0xADDR name  # note" lines (tools/re/names.txt format) -> {eboot offset: name}."""
    out = {}
    try:
        text = Path(path).read_text(encoding='utf-8', errors='replace')
    except OSError:
        return out
    for line in text.splitlines():
        parts = line.split(None, 1)
        if len(parts) != 2 or not parts[0].startswith('0x'):
            continue
        try:
            out[int(parts[0], 16)] = parts[1].split('  #', 1)[0].strip()
        except ValueError:
            continue
    return out


@functools.lru_cache(maxsize=1)
def repo_names():
    """Curated names (tools/re/names.txt) win over the Ghidra export (out/re/ghidra/names2.txt)."""
    names = read_names(ROOT / 'out' / 're' / 'ghidra' / 'names2.txt')
    names.update(read_names(ROOT / 'tools' / 're' / 'names.txt'))
    return names


@functools.lru_cache(maxsize=1)
def eboot_funcs():
    """(start, end) of every eboot function, from out/re/index.pkl.gz; [] when it is not built."""
    tools_re = str(ROOT / 'tools' / 're')
    if tools_re not in sys.path:
        sys.path.insert(0, tools_re)
    try:
        import eboot_re
        return list(eboot_re.Index().funcs)
    except (SystemExit, Exception):
        return []


def find_llvm_symbolizer():
    """Path to llvm-symbolizer, or None. Tries SOTC_LLVM_SYMBOLIZER, then PATH, then the MSYS2
    CLANG64 toolchain (which Windows Python usually does not have on PATH). This is why host
    symbolization worked only inside the MSYS2 shell before: the tool now finds it on its own."""
    override = os.environ.get('SOTC_LLVM_SYMBOLIZER')
    if override and os.path.isfile(override):
        return override
    found = shutil.which('llvm-symbolizer')
    if found:
        return found
    roots = [os.environ.get('MSYS2_ROOT', ''), r'C:\msys64', r'I:\msys64', '/c/msys64', '/i/msys64']
    for root in roots:
        if not root:
            continue
        for name in ('llvm-symbolizer.exe', 'llvm-symbolizer'):
            cand = os.path.join(root, 'clang64', 'bin', name)
            if os.path.isfile(cand):
                return cand
    return None


EXE_MODULE = 'bb-probe.exe'   # the host module with DWARF: llvm-symbolizer on host_exe, slide-adjusted


class Symbolizer:
    """Guest address (image_base <= x < image_base + image_size) -> eboot offset -> "name+0xoff".
    funcs: sorted (start, end) eboot offsets; names: {start offset: name}.

    Host addresses. With modules (the MODULES chunk, TRACE-FORMAT.md type 9) the module that holds x
    names it: "name+0xrva". bb-probe.exe goes to llvm-symbolizer on host_exe with
    --adjust-vma=slide, slide = base - preferred_base, which resolves the runtime address itself; a
    host address in no module stays hex. Without modules (old files) every host address goes to
    llvm-symbolizer on host_exe as it is, when given, else stays hex."""

    def __init__(self, image_base=0, image_size=0, funcs=(), names=None, host_exe=None, modules=()):
        self.image_base = int(image_base or 0)
        self.image_size = int(image_size or 0)
        self.funcs = sorted(funcs)
        self._starts = [s for s, _ in self.funcs]
        self.names = dict(names or {})
        self.modules = sorted((dict(m) for m in modules or ()), key=lambda m: m['base'])
        self._mod_bases = [m['base'] for m in self.modules]
        self._llvm = find_llvm_symbolizer() if host_exe else None
        self.host_exe = host_exe if self._llvm else None
        self._host = {}

    def is_guest(self, x):
        return self.image_size > 0 and self.image_base <= x < self.image_base + self.image_size

    def guest_name(self, off):
        i = bisect.bisect_right(self._starts, off) - 1
        if i >= 0 and self.funcs[i][0] <= off < self.funcs[i][1]:
            start = self.funcs[i][0]
            return f"{self.names.get(start, f'sub_{start:x}')}+{off - start:#x}"
        return f'eboot+{off:#x}'

    def module_of(self, x):
        """The MODULES entry whose [base, base + size) holds the host address x, or None."""
        i = bisect.bisect_right(self._mod_bases, x) - 1
        if i >= 0 and x < self.modules[i]['base'] + self.modules[i]['size']:
            return self.modules[i]
        return None

    def _module_label(self, x):
        """name+0xrva for an address in a module, else hex."""
        m = self.module_of(x)
        return f'{x:#x}' if m is None else f"{m['name']}+{x - m['base']:#x}"

    def label(self, x):
        if self.is_guest(x):
            return self.guest_name(x - self.image_base)
        if x not in self._host:
            self.resolve_host([x])
        return self._host.get(x, f'{x:#x}')

    def resolve_host(self, addrs):
        """Batch lookup of the host addresses not seen yet (see the class docstring)."""
        todo = sorted({x for x in addrs if x >= HOST_MIN and x not in self._host
                       and not self.is_guest(x)})
        if not self.modules:   # old file: no module table, the raw addresses go to llvm-symbolizer
            if self.host_exe and todo:
                self._lookup(None, todo)
            return
        by_slide = collections.defaultdict(list)
        for x in todo:
            m = self.module_of(x)
            if m is None:
                self._host[x] = f'{x:#x}'
            elif self.host_exe and m['name'].lower() == EXE_MODULE:
                slide = m['base'] - m['preferred_base'] if m['preferred_base'] else 0
                by_slide[slide].append(x)
            else:
                self._host[x] = self._module_label(x)
        for slide, group in sorted(by_slide.items()):
            self._lookup(slide, group)

    def _lookup(self, slide, addrs):
        """llvm-symbolizer on host_exe for addrs, in batches of 2000. slide None: no --adjust-vma.
        A name it cannot give is the module label; a failed run turns host_exe off, and the rest
        get the module label. A negative slide is passed as its 64-bit two's complement."""
        if not self.host_exe:
            for x in addrs:
                self._host.setdefault(x, self._module_label(x))
            return
        adjust = [] if slide is None else [f'--adjust-vma={slide % (1 << 64):#x}']
        for i in range(0, len(addrs), 2000):
            chunk = addrs[i:i + 2000]
            cmd = ([self._llvm, f'--obj={self.host_exe}'] + adjust +
                   ['--functions=short', '--no-inlines', '--demangle'] + [f'{x:#x}' for x in chunk])
            try:
                out = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8',
                                     errors='replace', timeout=120).stdout
            except (OSError, subprocess.SubprocessError):
                self.host_exe = None
                for x in addrs[i:]:
                    self._host.setdefault(x, self._module_label(x))
                return
            blocks = out.strip('\n').split('\n\n')
            for x, block in zip(chunk, blocks):
                first = block.split('\n', 1)[0].strip()
                self._host[x] = first if first and first != '??' else self._module_label(x)
            for x in chunk[len(blocks):]:
                self._host.setdefault(x, self._module_label(x))


def make_symbolizer(trace, use_index=True, use_host=True):
    host = ROOT / 'out' / 'gpu' / 'bb-probe.exe'
    return Symbolizer(trace.meta.get('image_base'), trace.meta.get('image_size'),
                      eboot_funcs() if use_index else (), repo_names(),
                      host if use_host and host.exists() else None,
                      modules=getattr(trace, 'modules', ()))


_OFFSET_TAIL = re.compile(r'(.+?)\+0x[0-9a-f]+')


def _fn_name(label):
    """A symbol label without its offset ("Game::Wait+0x10" -> "Game::Wait"). "eboot+0x.." (no
    function there) keeps its offset: those addresses are not one function."""
    m = _OFFSET_TAIL.fullmatch(label)
    if m and m.group(1) != 'eboot':
        return m.group(1)
    return label


def format_arg(v, sym=None):
    """The recorder's rule for a and b: small numbers as they are, guest addresses as symbol
    text, other addresses as hex text."""
    if v < PLAIN_LIMIT:
        return v
    if sym is not None and sym.is_guest(v):
        return sym.label(v)
    return f'0x{v:x}'


# ---------------------------------------------------------------- spans per thread

class Lane:
    """The spans (t1 > t0) of one ring, in µs: items are (start, end, name, b, kind, flags)."""
    __slots__ = ('ring', 'label', 'items', 'starts', 'max_dur')

    def __init__(self, ring, label, items):
        self.ring = ring
        self.label = label
        self.items = items
        self.starts = [x[0] for x in items]
        self.max_dur = max((x[1] - x[0] for x in items), default=0.0)

    def window(self, f0, f1):
        """The items that overlap [f0, f1)."""
        lo = bisect.bisect_left(self.starts, f0 - self.max_dur)
        hi = bisect.bisect_left(self.starts, f1)
        return [x for x in self.items[lo:hi] if x[1] > f0]


def span_lanes(trace):
    lanes = []
    for ring in sorted(trace.events):
        items = [(e.t0 / 1000.0, e.t1 / 1000.0, e.name, e.b, e.kind, e.flags)
                 for e in trace.events[ring] if e.t1 != e.t0]
        lanes.append(Lane(ring, trace.thread_label(ring), items))
    return lanes


def profile_label(trace, ring):
    """The thread a ring's samples belong to: a samples track takes the name of the thread with
    the same os_tid, so its samples land on the sampled thread's profile."""
    t = trace.threads.get(ring, {})
    if t.get('kind') == 'samples':
        for other, o in trace.threads.items():
            if other != ring and o.get('os_tid') == t.get('os_tid') \
                    and o.get('kind') not in ('samples', 'gpu'):
                return trace.thread_label(other)
    return trace.thread_label(ring)


# ---------------------------------------------------------------- samples

def _frame_name(sym, addr, cache):
    name = cache.get(addr)
    if name is None:
        name = cache[addr] = _fn_name(sym.label(addr))
    return name


def _all_sample_addrs(trace):
    out = set()
    for e in trace.all_events():
        if e.kind == K_SAMPLE:
            out.update(trace.stacks.get(e.aux) or [e.a])
    return out


def sample_rows(trace, sym):
    """Every EvSample of the trace in time order: Sample(ns, profile label, stack names leaf
    first). The stack is the STACKS entry of the sample (its leaf alone when that is missing)."""
    raw = []
    for ring, evs in trace.events.items():
        for e in evs:
            if e.kind == K_SAMPLE:
                raw.append((e.t0, ring, trace.stacks.get(e.aux) or [e.a]))
    if not raw:
        return []
    sym.resolve_host({x for _, _, addrs in raw for x in addrs})
    labels, cache, rows = {}, {}, []
    for t0, ring, addrs in sorted(raw, key=lambda r: r[0]):
        if ring not in labels:
            labels[ring] = profile_label(trace, ring)
        rows.append(Sample(t0, labels[ring], tuple(_frame_name(sym, x, cache) for x in addrs)))
    return rows


def sample_profiles(samples):
    """{thread: {'n': samples, 'flat': Counter(leaf name), 'incl': Counter(name, once per sample)}}"""
    out = {}
    for s in samples:
        p = out.setdefault(s.thread, {'n': 0, 'flat': collections.Counter(),
                                      'incl': collections.Counter()})
        p['n'] += 1
        if s.stack:
            p['flat'][s.stack[0]] += 1
        p['incl'].update(set(s.stack))
    return out


def folded_lines(samples):
    """Brendan Gregg folded stacks: "thread;root;...;leaf count" (flamegraph.pl input)."""
    counts = collections.Counter()
    for s in samples:
        frames = [s.thread] + list(reversed(s.stack))
        counts[';'.join(f.replace(';', ':') for f in frames)] += 1
    return [f'{k} {v}' for k, v in sorted(counts.items())]


def _counts_in(samples, ns_list, t0, t1):
    """(thread, name) -> inclusive samples with a time in [t0, t1) (ns)."""
    lo = bisect.bisect_left(ns_list, t0)
    hi = bisect.bisect_left(ns_list, t1)
    out = collections.Counter()
    for s in samples[lo:hi]:
        out.update({(s.thread, n) for n in s.stack})
    return out


# ---------------------------------------------------------------- hitch

def _classify_module():
    tools = str(Path(__file__).resolve().parents[1])
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import sotc_trace_classify
    return sotc_trace_classify


def _tally(lanes, f0, f1, kinds=None):
    """(thread, span name) -> [µs inside [f0, f1), [(µs, b), ...]]. kinds None: every span except
    frames and marks; otherwise only the spans of those kinds."""
    out = {}
    for lane in lanes:
        for s, e, name, b, kind, _flags in lane.window(f0, f1):
            if kinds is None:
                if kind in (K_FRAME, K_MARK):
                    continue
            elif kind not in kinds:
                continue
            d = min(e, f1) - max(s, f0)
            if d <= 0:
                continue
            slot = out.setdefault((lane.label, name), [0.0, []])
            slot[0] += d
            slot[1].append((d, b))
    return out


def _grown(slow, normals, top):
    """[(key, slow value, typical value, ...)] for the keys that grew: the typical value is the
    median over the normal frames. slow and normals map key -> value."""
    keys = set(slow)
    for m in normals:
        keys.update(m)
    typical = {k: (statistics.median([m.get(k, 0.0) for m in normals]) if normals else 0.0)
               for k in keys}
    ranked = sorted(keys, key=lambda k: slow.get(k, 0.0) - typical[k], reverse=True)
    return [(k, slow.get(k, 0.0), typical[k]) for k in ranked[:top]
            if slow.get(k, 0.0) - typical[k] > 0]


def hitch_file(trace, sym, top=10, callers=3):
    """One result per slow frame of a trace (see the module docstring)."""
    frames = sorted((e for e in trace.all_events() if e.kind == K_FRAME), key=lambda e: e.t0)
    slow = [f for f in frames if f.flags & FLAG_SLOW]
    if not slow and 'frame' in trace.meta:
        slow = [f for f in frames if f.a == trace.meta['frame']]
    lanes = span_lanes(trace)
    cls = _classify_module()
    samples = sample_rows(trace, sym)
    sample_ns = [s.ns for s in samples]
    has_gpu = any(e.kind == K_GPU for e in trace.all_events())
    results = []
    for f in slow:
        f0, f1 = f.t0 / 1000.0, f.t1 / 1000.0
        dur = (f1 - f0) or 1.0
        normal = [n for n in frames if not n.flags & FLAG_SLOW and n.name == f.name
                  and n.t1 > n.t0]
        slow_t = _tally(lanes, f0, f1)
        normal_t = [_tally(lanes, n.t0 / 1000.0, n.t1 / 1000.0) for n in normal]
        keys = set(slow_t)
        for nt in normal_t:
            keys.update(nt)
        typical = {k: (statistics.median([nt[k][0] if k in nt else 0.0 for nt in normal_t])
                       if normal_t else 0.0) for k in keys}
        ranked = sorted(keys, key=lambda k: slow_t.get(k, [0.0])[0] - typical[k], reverse=True)
        picked = [k for k in ranked[:top] if slow_t.get(k, [0.0])[0] - typical[k] > 0]
        for k in picked:
            sym.resolve_host(b for _, b in slow_t.get(k, [0.0, []])[1])
        rows = []
        for k in picked:
            s_us = slow_t.get(k, [0.0])[0]
            by_caller = collections.Counter()
            for d, b in slow_t.get(k, [0.0, []])[1]:
                by_caller[_caller_label(b, sym)] += d
            rows.append({'thread': k[0], 'name': k[1], 'slow_ms': s_us / 1000.0,
                         'typical_ms': typical[k] / 1000.0,
                         'delta_ms': (s_us - typical[k]) / 1000.0,
                         'callers': [(lbl, us / 1000.0)
                                     for lbl, us in by_caller.most_common(callers)]})
        func_rows = [{'thread': k[0], 'func': k[1], 'slow': v, 'typical': t, 'delta': v - t}
                     for k, v, t in _grown(
                         _counts_in(samples, sample_ns, f.t0, f.t1),
                         [_counts_in(samples, sample_ns, n.t0, n.t1) for n in normal], top)]
        imp_slow = {k: v[0] / 1000.0 for k, v in _tally(lanes, f0, f1, {K_IMPORT}).items()}
        imp_normal = [{k: v[0] / 1000.0 for k, v in _tally(lanes, n.t0 / 1000.0, n.t1 / 1000.0,
                                                           {K_IMPORT}).items()} for n in normal]
        imp_rows = [{'thread': k[0], 'name': k[1], 'slow_ms': v, 'typical_ms': t,
                     'delta_ms': v - t} for k, v, t in _grown(imp_slow, imp_normal, top)]
        gpu_row = None
        if has_gpu:
            def gpu_us(a, b):
                spans = [x for lane in lanes for x in lane.window(a, b) if x[4] == K_GPU]
                return cls.union(spans, a, b)
            g = gpu_us(f0, f1)
            gn = statistics.median([gpu_us(n.t0 / 1000.0, n.t1 / 1000.0) for n in normal]) \
                if normal else 0.0
            busy = g / dur
            gpu_row = {'busy': busy, 'slow_ms': g / 1000.0, 'typical_ms': gn / 1000.0,
                       'delta_ms': (g - gn) / 1000.0,
                       'bound': 'GPU-bound' if busy >= 0.85 else
                       ('CPU-bound' if busy < 0.5 else 'mixed')}
        stats = cls.frame_stats(f0, f1, [(lane.label, lane.window(f0, f1)) for lane in lanes])
        results.append({
            'file': trace.path.name, 'frame': f.a, 'segment': f.name,
            'ms': (f1 - f0) / 1000.0, 'normal_frames': len(normal),
            'typical_ms': (statistics.median([(n.t1 - n.t0) / 1e6 for n in normal])
                           if normal else None),
            'verdict': stats['cause'], 'top': rows,
            'funcs': func_rows, 'imports': imp_rows, 'gpu': gpu_row})
    return results


def _caller_label(b, sym):
    if b < PLAIN_LIMIT:
        return '-' if b == 0 else str(b)
    return sym.label(b)


def _md(text):
    return str(text).replace('|', '/')


def hitch_markdown(name, results):
    out = [f'## {_md(name)}', '']
    if not results:
        return out + ['No slow frame in this file.', '']
    for r in results:
        typical = (f"{r['typical_ms']:.1f} ms over {r['normal_frames']} normal frames"
                   if r['typical_ms'] is not None else 'no normal frames to compare with')
        out += [f"### frame {r['frame']} ({_md(r['segment'])}): {r['ms']:.1f} ms, "
                f"verdict: {r['verdict']}", '', f'Typical frame: {typical}.', '']
        if not r['top']:
            out += ['No span grew inside this frame.', '']
        else:
            out += ['| # | thread | span | slow ms | typical ms | delta ms | main callers |',
                    '|---|---|---|---|---|---|---|']
            for i, t in enumerate(r['top'], 1):
                callers = '; '.join(f'{_md(lbl)} ({ms:.1f} ms)' for lbl, ms in t['callers']) or '-'
                out.append(f"| {i} | {_md(t['thread'])} | {_md(t['name'])} | {t['slow_ms']:.1f} | "
                           f"{t['typical_ms']:.1f} | {t['delta_ms']:+.1f} | {callers} |")
            out.append('')
        if r.get('funcs'):
            out += ['Sampled functions that grew (inclusive samples, slow frame against typical):',
                    '', '| # | thread | function | slow | typical | delta |',
                    '|---|---|---|---|---|---|']
            for i, t in enumerate(r['funcs'], 1):
                out.append(f"| {i} | {_md(t['thread'])} | {_md(t['func'])} | {t['slow']:.0f} | "
                           f"{t['typical']:.1f} | {t['delta']:+.1f} |")
            out.append('')
        if r.get('imports'):
            out += ['Imports that grew (EvImport time inside the frame):', '']
            for t in r['imports']:
                out.append(f"- {t['delta_ms']:+.1f} ms {_md(t['name'])} on {_md(t['thread'])} "
                           f"(slow {t['slow_ms']:.1f} ms, typical {t['typical_ms']:.1f} ms)")
            out.append('')
        if r.get('gpu'):
            g = r['gpu']
            out += [f"GPU (EvGpu): busy {g['busy']:.0%} of the frame, verdict {g['bound']}; "
                    f"{g['slow_ms']:.1f} ms against {g['typical_ms']:.1f} ms typical "
                    f"({g['delta_ms']:+.1f} ms).", '']
    return out


# ---------------------------------------------------------------- top / info

def top_report(trace, limit, samples=None):
    lines = []
    per = collections.defaultdict(collections.Counter)
    calls = collections.Counter()
    for ring, evs in trace.events.items():
        label = trace.thread_label(ring)
        for e in evs:
            if e.kind == K_FRAME or e.t1 == e.t0:
                continue
            per[label][e.name] += e.t1 - e.t0
            calls[(label, e.name)] += 1
    for label, c in sorted(per.items(), key=lambda kv: -sum(kv[1].values())):
        lines.append(f'{label} ({sum(c.values()) / 1e6:.2f} ms)')
        for name, ns in c.most_common(limit):
            lines.append(f'  {ns / 1e6:10.2f} ms {calls[(label, name)]:9d}x  {name}')
    gpu = [e for e in trace.all_events() if e.kind == K_GPU]
    if gpu:
        lines.append(f'GPU (EvGpu): {sum(e.t1 - e.t0 for e in gpu) / 1e6:.2f} ms in '
                     f'{len(gpu)} spans')
    if samples:
        for thread, p in sorted(sample_profiles(samples).items(), key=lambda kv: -kv[1]['n']):
            lines.append(f"{thread} ({p['n']} samples), flat (leaf):")
            for name, n in p['flat'].most_common(limit):
                lines.append(f"  {n:7d} {100.0 * n / p['n']:5.1f}%  {name}")
            lines.append('  inclusive:')
            for name, n in p['incl'].most_common(limit):
                lines.append(f"  {n:7d} {100.0 * n / p['n']:5.1f}%  {name}")
    if trace.import_stats:
        lines.append('imports (IMPORT_STATS, cumulative):')
        rows = sorted(trace.import_stats, key=lambda r: -r.get('total_ns', 0))[:limit]
        for r in rows:
            lines.append(f"  {r.get('total_ns', 0) / 1e6:10.2f} ms {r.get('calls', 0):9d}x "
                         f"max {r.get('max_ns', 0) / 1e6:8.3f} ms  {r.get('name', '?')}")
    if trace.zone_stats:
        lines.append('zones (cumulative):')
        zones = sorted(trace.zone_stats, key=lambda z: -z.get('total_ns', 0))[:limit * 4]
        for z in zones:
            lines.append(f"  {z.get('total_ns', 0) / 1e6:10.2f} ms {z.get('calls', 0):9d}x "
                         f"max {z.get('max_ns', 0) / 1e6:8.3f} ms  {z.get('name', '')} "
                         f"({z.get('file', '')}:{z.get('line', 0)})")
    return lines


def info_report(trace):
    lines = [f'{trace.path.name}: format {trace.format}, complete {trace.complete}, '
             f'truncated {trace.truncated}, bad chunks {trace.bad_chunks}']
    if trace.chunks:
        lines.append('chunks: ' + ', '.join(f'{CHUNK_NAMES.get(k, k)} x{v}'
                                            for k, v in sorted(trace.chunks.items())))
    if trace.unknown:
        lines.append('unknown chunk types: ' + ', '.join(
            f'{k:#x} x{v}' for k, v in sorted(trace.unknown.items())))
    lines.append('META:')
    for key, value in trace.meta.items():
        lines.append(f'  {key}: {json.dumps(value)[:200]}')
    counts = collections.Counter()
    for ring, evs in trace.events.items():
        counts[ring] = len(evs)
    lines.append('threads:')
    for ring in sorted(set(trace.threads) | set(trace.events)):
        t = trace.threads.get(ring, {})
        lines.append(f"  {ring:4d} os {str(t.get('os_tid', '-')):>7s} "
                     f"{str(t.get('kind') or '-'):8s} {trace.thread_label(ring)}  "
                     f"{counts[ring]} events")
    kinds = collections.Counter(e.kind for e in trace.all_events())
    lines.append('events by kind: ' + (', '.join(
        f'{KIND_NAMES[k] if k < len(KIND_NAMES) else k} {v}' for k, v in sorted(kinds.items()))
        or 'none'))
    first = [evs[0].t0 for evs in trace.events.values() if evs]
    last = [max(e.t1 for e in evs) for evs in trace.events.values() if evs]
    if first:
        lines.append(f'time span: {(max(last) - min(first)) / 1e6:.1f} ms')
    lines.append(f'zone stats: {len(trace.zone_stats)} entries, stacks: {len(trace.stacks)}, '
                 f'import stats: {len(trace.import_stats)}, '
                 f'gpu calibration: {"yes" if trace.gpu_calib else "no"}')
    return lines


# ---------------------------------------------------------------- diff

SECTIONS = (('imports', 'imports (IMPORT_STATS total, ms)', str),
            ('waits', 'waits (game threads, ms)', str),
            ('spans', 'spans (ms, thread: name)', lambda k: f'{k[0]}: {k[1]}'),
            ('funcs', 'sampled functions (inclusive samples)', lambda k: f'{k[0]}: {k[1]}'))


def trace_profile(trace, sym):
    """What one trace spent its time on, as the numbers diff compares them."""
    cls = _classify_module()
    cats = collections.Counter()
    spans, waits, imports, funcs = (collections.Counter() for _ in range(4))
    for lane in span_lanes(trace):
        for s, e, name, _b, kind, _flags in lane.items:
            if kind in (K_FRAME, K_MARK):
                continue
            ms = (e - s) / 1000.0
            spans[(lane.label, name)] += ms
            if 'GpuDraw' in lane.label and name != 'draw idle':
                cats['draw ms'] += ms
            if kind == K_GPU or lane.label.startswith('GPU'):
                cats['gpu ms'] += ms
            if lane.label.startswith('game: '):
                for key, kinds in cls.GUEST_KEYS.items():
                    if name in kinds:
                        waits[key] += ms
            if kind == K_IMPORT and not trace.import_stats:
                imports[name] += ms
    for row in trace.import_stats:
        imports[str(row.get('name', '?'))] += row.get('total_ns', 0) / 1e6
    samples = sample_rows(trace, sym)
    for thread, p in sample_profiles(samples).items():
        for name, n in p['incl'].items():
            funcs[(thread, name)] += n
    cats['samples'] = len(samples)
    cats['slow frames'] = sum(1 for e in trace.all_events()
                              if e.kind == K_FRAME and e.flags & FLAG_SLOW)
    cats['imports ms'] = sum(imports.values())
    cats['waits ms'] = sum(waits.values())
    return {'traces': 1, 'categories': dict(cats), 'spans': dict(spans),
            'imports': dict(imports), 'waits': dict(waits), 'funcs': dict(funcs)}


def aggregate_profiles(profiles):
    """The mean of per-trace profiles: every number divided by the trace count."""
    n = len(profiles)
    out = {'traces': n}
    for section in ('categories', 'spans', 'imports', 'waits', 'funcs'):
        total = collections.Counter()
        for p in profiles:
            total.update(p[section])
        out[section] = {k: v / n for k, v in total.items()}
    return out


def diff_lines(label_a, pa, label_b, pb, top=15):
    """Markdown tables: the categories, then the top changes per section (B-A)."""
    out = [f'# diff: {_md(label_a)} (A) vs {_md(label_b)} (B)', '',
           f"A: {pa['traces']} trace(s), B: {pb['traces']} trace(s). Values are means per trace; "
           'B-A is positive where B spent more.', '',
           '## categories', '', '| category | A | B | B-A |', '|---|---:|---:|---:|']
    for cat in CATEGORIES:
        a = pa['categories'].get(cat, 0.0)
        b = pb['categories'].get(cat, 0.0)
        out.append(f'| {cat} | {a:.1f} | {b:.1f} | {b - a:+.1f} |')
    for section, title, fmt in SECTIONS:
        a_map, b_map = pa[section], pb[section]
        keys = sorted(set(a_map) | set(b_map),
                      key=lambda k: (-abs(b_map.get(k, 0.0) - a_map.get(k, 0.0)), str(k)))[:top]
        out += ['', f'## {title}', '', '| name | A | B | B-A |', '|---|---:|---:|---:|']
        if not keys:
            out.append('| (none) | | | |')
        for k in keys:
            a = a_map.get(k, 0.0)
            b = b_map.get(k, 0.0)
            out.append(f'| {_md(fmt(k))} | {a:.1f} | {b:.1f} | {b - a:+.1f} |')
    return out


def expand_inputs(paths):
    """The files as given; a folder gives every .sotctrace under it (recursively), in name order."""
    out = []
    for p in paths:
        path = Path(p)
        if path.is_dir():
            out += sorted(path.rglob('*.sotctrace'))
        else:
            out.append(path)
    return out


# ---------------------------------------------------------------- autofuncs

def function_scores(traces, funcs):
    """{function start (eboot offset): (sampled ms, hitch span ms)} over the traces. A sample
    weighs the time to the next sample of its ring (at most 100 ms) and counts once for each
    function on its stack. A hitch span counts its time inside the slow frames, for the function
    that holds its guest caller (the span's b field)."""
    ordered = sorted(funcs)
    starts = [s for s, _ in ordered]

    def start_of(off):
        i = bisect.bisect_right(starts, off) - 1
        return ordered[i][0] if i >= 0 and off < ordered[i][1] else None

    scores = collections.defaultdict(lambda: [0.0, 0.0])
    for t in traces:
        base = int(t.meta.get('image_base') or 0)
        size = int(t.meta.get('image_size') or 0)

        def guest_start(x, base=base, size=size):
            return start_of(x - base) if size and base <= x < base + size else None

        for evs in t.events.values():
            ss = sorted((e for e in evs if e.kind == K_SAMPLE), key=lambda e: e.t0)
            times = [e.t0 for e in ss]
            gaps = [min(b - a, SAMPLE_CAP_NS) for a, b in zip(times, times[1:])]
            weights = gaps + gaps[-1:] if gaps else [0] * len(ss)
            for e, w in zip(ss, weights):
                for x in {guest_start(a) for a in (t.stacks.get(e.aux) or [e.a])} - {None}:
                    scores[x][0] += w / 1e6
        frames = [e for e in t.all_events() if e.kind == K_FRAME and e.flags & FLAG_SLOW]
        if frames:
            lanes = span_lanes(t)
            for f in frames:
                f0, f1 = f.t0 / 1000.0, f.t1 / 1000.0
                for lane in lanes:
                    for s, e_, _name, b, kind, _flags in lane.window(f0, f1):
                        if kind in (K_FRAME, K_MARK) or e_ <= s:
                            continue
                        st = guest_start(b)
                        if st is not None:
                            scores[st][1] += (min(e_, f1) - max(s, f0)) / 1000.0
    return {k: (v[0], v[1]) for k, v in scores.items()}


def parse_sig_output(text, offset):
    """(pattern, reason) from the output of tools/re/eboot_re.py sig ADDR: the pattern is the
    byte text ("55 48 ?? ..."); reason says why there is none."""
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    if len(lines) >= 2 and not lines[0].startswith(';') and lines[1].startswith('; unique at'):
        m = re.match(r'; unique at (0x[0-9a-f]+)', lines[1])
        if not m or int(m.group(1), 16) != offset:
            return None, 'signature unique elsewhere'
        tokens = lines[0].split()
        if tokens and all(re.fullmatch(r'\?\?|[0-9a-f]{2}', t) for t in tokens):
            return ' '.join(tokens), ''
        return None, 'unreadable signature'
    for ln in lines:
        if ln.startswith(';'):
            return None, ln.lstrip('; ')
    return None, 'no signature output'


def eboot_sig(offset):
    """(pattern, reason) of the function at offset, from tools/re/eboot_re.py sig."""
    cmd = [sys.executable, str(ROOT / 'tools' / 're' / 'eboot_re.py'), 'sig', f'{offset:#x}']
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8',
                              errors='replace', timeout=600)
    except (OSError, subprocess.SubprocessError) as err:
        return None, f'eboot_re.py did not run ({err})'
    return parse_sig_output(proc.stdout, offset)


def eboot_fingerprint(path):
    """The fingerprint of an eboot (an ELF such as out/eboot.elf, or a SELF), as sotc_hooks.py
    computes it."""
    scripts = str(ROOT / 'scripts')
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    import sotc_hooks
    return sotc_hooks.fingerprint(Path(path).read_bytes())


def hook_name(raw, start, used):
    """A hooks.d name for a function: letters, digits and _, a letter first, unique in used."""
    name = re.sub(r'[^A-Za-z0-9_]+', '_', raw).strip('_') or 'F'
    if not name[0].isalpha():
        name = 'F_' + name
    name = name[:47]
    if name in used:
        name = f'{name[:40]}_{start:x}'
    return name


def pick_functions(scores, names, sig_runner, top=20, min_ms=1.0):
    """The top functions by sampled plus hitch ms (at least min_ms). Returns (entries, skipped):
    an entry has name, start, pattern, label, sample_ms, hitch_ms; skipped is (label, reason)."""
    ranked = sorted(((s + h, start, s, h) for start, (s, h) in scores.items() if s + h >= min_ms),
                    key=lambda r: (-r[0], r[1]))[:top]
    entries, skipped, used = [], [], set()
    for _total, start, s, h in ranked:
        raw = names.get(start, f'sub_{start:x}')
        pattern, reason = sig_runner(start)
        if pattern is None:
            skipped.append((raw, reason))
            continue
        name = hook_name(raw, start, used)
        used.add(name)
        entries.append({'name': name, 'start': start, 'pattern': pattern, 'label': raw,
                        'sample_ms': s, 'hitch_ms': h})
    return entries, skipped


def _comment(text):
    return re.sub(r'[^\x20-\x7e]', '?', text)


def hooks_source(entries, fingerprint, trace_count):
    """The text of a scripts/hooks.d file with one mode 'time' hook per entry."""
    lines = [
        '# SPDX-License-Identifier: GPL-2.0-or-later',
        '"""Guest functions picked from SOTC_TRACE samples and hitch spans by',
        f'tools/trace/sotc_trace.py autofuncs ({trace_count} trace file(s)). Generated: regenerate it',
        'instead of editing it.',
        '',
        "Every entry is mode 'time': with SOTC_TRACE_FUNCS set non-zero the port times each call of",
        'the function. The patterns are read from the eboot of the fingerprint below; other eboots',
        'scan for them.',
        '"""',
        '',
        f'EBOOT_FINGERPRINT = {fingerprint!r}',
        '',
        'HOOKS = [',
    ]
    for e in entries:
        lines += [
            f"    # {_comment(e['label'])}: {e['sample_ms']:.1f} ms sampled, "
            f"{e['hitch_ms']:.1f} ms in hitch spans",
            '    dict(',
            f"        name={e['name']!r},",
            "        switch='SOTC_TRACE_FUNCS',",
            # No group: one group per hook (the default), so a function whose signature no longer
            # resolves does not switch off the rest of the list (time-mode convention).
            "        mode='time',",
            f"        pattern={e['pattern']!r},",
            f"        expected={{EBOOT_FINGERPRINT: {e['start']:#x}}},",
            '    ),',
        ]
    lines.append(']')
    return '\n'.join(lines) + '\n'


# ---------------------------------------------------------------- Chrome JSON

def _stack_names(trace, sym, e, cache):
    return [_frame_name(sym, x, cache) for x in (trace.stacks.get(e.aux) or [e.a])]


def _chrome_event(trace, sym, e, tid, zero, cache):
    ts = round((max(e.t0, zero) - zero) / 1000.0, 3)
    if e.kind == K_FRAME:
        return {'name': 'SLOW FRAME' if e.flags & FLAG_SLOW else e.name,
                'ph': 'X', 'pid': 1, 'tid': tid, 'ts': ts,
                'dur': round((e.t1 - max(e.t0, zero)) / 1000.0, 3),
                'args': {'a': e.a, 'b': format_arg(e.b), 'segment': e.name}}
    if e.kind == K_SAMPLE:
        names = _stack_names(trace, sym, e, cache)
        return {'name': names[0] if names else '?', 'cat': 'sample', 'ph': 'i', 's': 't',
                'pid': 1, 'tid': tid, 'ts': ts, 'args': {'stack': ' <- '.join(names[:48])}}
    cat = KIND_NAMES[e.kind] if e.kind < len(KIND_NAMES) else str(e.kind)
    args = {'a': format_arg(e.a), 'b': format_arg(e.b, sym)}
    if e.t1 == e.t0 or e.kind == K_MARK:
        return {'name': e.name, 'cat': cat, 'ph': 'i', 's': 't', 'pid': 1, 'tid': tid,
                'ts': ts, 'args': args}
    return {'name': e.name, 'cat': cat, 'ph': 'X', 'pid': 1, 'tid': tid, 'ts': ts,
            'dur': round((e.t1 - max(e.t0, zero)) / 1000.0, 3), 'args': args}


def to_chrome(trace, sym=None):
    """The Chrome JSON document (the shape the recorder wrote, plus frame, mark, sample and GPU
    tracks)."""
    if sym is None:
        sym = make_symbolizer(trace)
    sym.resolve_host(_all_sample_addrs(trace))
    zero = _zero_ns(trace)
    cache = {}
    gpu_rings = {r for r in trace.events if trace.threads.get(r, {}).get('kind') == 'gpu'}
    events = [{'name': 'process_name', 'ph': 'M', 'pid': 1,
               'args': {'name': 'Shadow of the Colossus (sotc-pc)'}}]
    if gpu_rings or any(e.kind == K_GPU for e in trace.all_events()):
        events.append({'name': 'thread_name', 'ph': 'M', 'pid': 1, 'tid': GPU_TID,
                       'args': {'name': 'GPU'}})
        events.append({'name': 'thread_sort_index', 'ph': 'M', 'pid': 1, 'tid': GPU_TID,
                       'args': {'sort_index': 0}})
    for ring in sorted(set(trace.threads) | set(trace.events)):
        if ring not in gpu_rings:
            t = trace.threads.get(ring, {})
            events.append({'name': 'thread_name', 'ph': 'M', 'pid': 1, 'tid': ring,
                           'args': {'name': trace.thread_label(ring)}})
            events.append({'name': 'thread_sort_index', 'ph': 'M', 'pid': 1, 'tid': ring,
                           'args': {'sort_index': int(t.get('sort', ring))}})
        for e in trace.events.get(ring, []):
            tid = GPU_TID if (ring in gpu_rings or e.kind == K_GPU) else ring
            events.append(_chrome_event(trace, sym, e, tid, zero, cache))
    other = {k: trace.meta[k] for k in ('frame', 'worst_ms', 'slow_frames', 'window_ms')
             if k in trace.meta}
    other['source'] = trace.path.name
    return {'displayTimeUnit': 'ms', 'otherData': other, 'traceEvents': events}


def _zero_ns(trace):
    win = trace.meta.get('window_ns')
    if isinstance(win, list) and len(win) == 2:
        return int(win[0])
    first = [evs[0].t0 for evs in trace.events.values() if evs]
    return min(first) if first else 0


def write_chrome(doc, out):
    out = Path(out)
    opener = gzip.open if out.name.endswith('.gz') else open
    with opener(out, 'wt', encoding='utf-8') as f:
        json.dump(doc, f, separators=(',', ':'))


# ---------------------------------------------------------------- CLI

def _open_trace(path):
    try:
        return load(path)
    except (OSError, ValueError) as err:
        print(f'{path}: {err}', file=sys.stderr)
        return None


def _symbolizer_for(trace, args):
    return make_symbolizer(trace, use_index=not args.no_index, use_host=not args.no_host)


def cmd_info(args):
    trace = _open_trace(args.file)
    if trace is None:
        return 1
    print('\n'.join(info_report(trace)))
    return 0


def cmd_top(args):
    status, folded = 0, []
    for path in expand_inputs(args.files):
        trace = _open_trace(path)
        if trace is None:
            status = 1
            continue
        samples = sample_rows(trace, _symbolizer_for(trace, args))
        print(f'== {trace.path.name}')
        print('\n'.join(top_report(trace, args.top, samples)))
        print()
        folded += folded_lines(samples)
    if args.folded:
        Path(args.folded).write_text('\n'.join(folded) + ('\n' if folded else ''),
                                     encoding='utf-8')
        print(f'wrote {len(folded)} folded stacks to {args.folded}', file=sys.stderr)
    return status


def cmd_hitch(args):
    reports, as_json, status = [], [], 0
    for path in expand_inputs(args.files):
        trace = _open_trace(path)
        if trace is None:
            status = 1
            continue
        results = hitch_file(trace, _symbolizer_for(trace, args))
        reports.append((trace.path.name, results))
        as_json.extend(results)
    text = '\n'.join(line for name, res in reports for line in hitch_markdown(name, res))
    print(text)
    if args.md:
        Path(args.md).write_text(text + '\n', encoding='utf-8')
    if args.json:
        Path(args.json).write_text(json.dumps(as_json, indent=2), encoding='utf-8')
    return status


def cmd_json(args):
    trace = _open_trace(args.file)
    if trace is None:
        return 1
    stem = Path(args.file).name.removesuffix('.gz').removesuffix('.json') \
        .removesuffix('.sotctrace')
    out = Path(args.out) if args.out else Path(args.file).with_name(stem + '.json.gz')
    doc = to_chrome(trace, _symbolizer_for(trace, args))
    write_chrome(doc, out)
    print(f'wrote {len(doc["traceEvents"])} trace events to {out}', file=sys.stderr)
    return 0


def _side_profile(arg, args):
    """(profile, trace count) of a file or folder; None when nothing loads."""
    profiles = []
    for path in expand_inputs([arg]):
        trace = _open_trace(path)
        if trace is not None:
            profiles.append(trace_profile(trace, _symbolizer_for(trace, args)))
    if not profiles:
        print(f'{arg}: no trace to compare', file=sys.stderr)
        return None
    return aggregate_profiles(profiles)


def cmd_diff(args):
    pa = _side_profile(args.a, args)
    pb = _side_profile(args.b, args)
    if pa is None or pb is None:
        return 1
    text = '\n'.join(diff_lines(args.a, pa, args.b, pb, args.top))
    print(text)
    if args.md:
        Path(args.md).write_text(text + '\n', encoding='utf-8')
    return 0


def cmd_autofuncs(args):
    traces = [t for t in (_open_trace(p) for p in expand_inputs(args.files)) if t is not None]
    if not traces:
        print('autofuncs: no trace loaded', file=sys.stderr)
        return 1
    funcs = eboot_funcs()
    if not funcs:
        print('autofuncs: out/re/index.pkl.gz is missing (run tools/re/eboot_re.py build)',
              file=sys.stderr)
        return 2
    try:
        fingerprint = eboot_fingerprint(args.eboot)
    except OSError as err:
        print(f'autofuncs: {err}', file=sys.stderr)
        return 2
    names = read_names(ROOT / 'tools' / 're' / 'names.txt')
    scores = function_scores(traces, funcs)
    entries, skipped = pick_functions(scores, names, eboot_sig, args.top, args.min_ms)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(hooks_source(entries, fingerprint, len(traces)), encoding='utf-8')
    print(f'autofuncs: wrote {len(entries)} function(s) to {out}; skipped {len(skipped)} '
          f'(no unique signature)')
    for raw, reason in skipped:
        print(f'  skipped {raw}: {reason}')
    return 0


def main(argv=None):
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except (AttributeError, OSError):
        pass
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('info', help='META, threads, event counts')
    p.add_argument('file')
    p.set_defaults(func=cmd_info)
    p = sub.add_parser('top', help='total time by span name per thread, samples, imports, GPU')
    p.add_argument('files', nargs='+')
    p.add_argument('--top', type=int, default=8)
    p.add_argument('--folded', help='also write folded stacks (flamegraph.pl) here')
    p.add_argument('--no-index', action='store_true', help='do not load out/re/index.pkl.gz')
    p.add_argument('--no-host', action='store_true', help='do not call llvm-symbolizer')
    p.set_defaults(func=cmd_top)
    p = sub.add_parser('hitch', help='why the slow frames were slow')
    p.add_argument('files', nargs='+')
    p.add_argument('--md', help='also write the markdown report here')
    p.add_argument('--json', help='also write the results as JSON here')
    p.add_argument('--no-index', action='store_true', help='do not load out/re/index.pkl.gz')
    p.add_argument('--no-host', action='store_true', help='do not call llvm-symbolizer')
    p.set_defaults(func=cmd_hitch)
    p = sub.add_parser('json', help='Chrome JSON for Perfetto (.gz when named .gz)')
    p.add_argument('file')
    p.add_argument('-o', '--out')
    p.add_argument('--no-index', action='store_true', help='do not load out/re/index.pkl.gz')
    p.add_argument('--no-host', action='store_true', help='do not call llvm-symbolizer')
    p.set_defaults(func=cmd_json)
    p = sub.add_parser('diff', help='where the time went: B-A per category, import, span, function')
    p.add_argument('a', help='a trace file or a folder of traces (A)')
    p.add_argument('b', help='a trace file or a folder of traces (B)')
    p.add_argument('--md', help='also write the tables here')
    p.add_argument('--top', type=int, default=15)
    p.add_argument('--no-index', action='store_true', help='do not load out/re/index.pkl.gz')
    p.add_argument('--no-host', action='store_true', help='do not call llvm-symbolizer')
    p.set_defaults(func=cmd_diff)
    p = sub.add_parser('autofuncs', help='write a scripts/hooks.d file of the hot functions')
    p.add_argument('files', nargs='+')
    p.add_argument('-o', '--out', required=True, help='the hooks.d file to write')
    p.add_argument('--top', type=int, default=20)
    p.add_argument('--min-ms', type=float, default=1.0)
    p.add_argument('--eboot', default=str(ROOT / 'out' / 'eboot.elf'),
                   help='the eboot whose fingerprint the hooks name (default out/eboot.elf)')
    p.set_defaults(func=cmd_autofuncs)
    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == '__main__':
    sys.exit(main())
