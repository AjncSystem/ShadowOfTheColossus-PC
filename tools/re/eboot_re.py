#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Static reverse-engineering helper for the SotC eboot (out/eboot.elf, PS4 ET_SCE_EXEC).

The ELF has no sections and no symbols. This tool rebuilds what an analyser needs:
- function ranges from the PT_GNU_EH_FRAME search table (one FDE per function);
- import names: PLT stubs (jmp [rip+GOT]) and GOT calls resolved through out/boot.bin's
  relocations (GOT slot -> NID) and src/import_names.inc (NID -> symbol);
- a cross-reference index (direct calls/jumps, RIP-relative data references) built once with
  capstone and cached in out/re/index.pkl.gz;
- C strings of the read-only and data segments.

Addresses are eboot virtual addresses (the "guest offset" of the port's logs).

usage:
  eboot_re.py build                      build the index (a few minutes, once)
  eboot_re.py func ADDR                  the function containing ADDR
  eboot_re.py dis ADDR [-n N]            disassemble the function containing ADDR (annotated)
  eboot_re.py callers ADDR [-d DEPTH]    who calls the function containing ADDR
  eboot_re.py callees ADDR               what the function containing ADDR calls
  eboot_re.py str REGEX [-l LIMIT]       strings matching REGEX and the functions that use them
  eboot_re.py refs ADDR [-r RANGE]       code that references data at ADDR (or ADDR..ADDR+RANGE)
  eboot_re.py imports [REGEX]            imports and how many functions call each
  eboot_re.py icallers NAME              functions that call an import (substring match)
  eboot_re.py fstr ADDR                  strings referenced by the function containing ADDR
  eboot_re.py sig ADDR [-n MAXBYTES]     shortest byte signature from ADDR that is unique in the
                                         text segment, with rel32/RIP displacements wildcarded
  eboot_re.py sigfind PATTERN            addresses matching a signature ("48 8b ?? ?? e8 ...")
"""
import argparse
import bisect
import gzip
import pickle
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ELF = ROOT / 'out' / 'eboot.elf'
BOOT = ROOT / 'out' / 'boot.bin'
NAMES = ROOT / 'src' / 'import_names.inc'
INDEX = ROOT / 'out' / 're' / 'index.pkl.gz'
NOTES = ROOT / 'tools' / 're' / 'names.txt'  # optional "0xADDR name" lines, shared knowledge

PT_LOAD, PT_GNU_EH_FRAME, PT_SCE_RELRO = 1, 0x6474e550, 0x61000010


class Image:
    def __init__(self):
        self.data = ELF.read_bytes()
        phoff, = struct.unpack_from('<Q', self.data, 0x20)
        phnum, = struct.unpack_from('<H', self.data, 0x38)
        self.ph = [struct.unpack_from('<IIQQQQQQ', self.data, phoff + i * 56) for i in range(phnum)]
        # (vaddr, filesz, offset, flags) of the mapped segments
        self.segs = [(p[3], p[5], p[2], p[1]) for p in self.ph if p[0] in (PT_LOAD, PT_SCE_RELRO)]

    def read(self, va, n):
        for vaddr, filesz, off, _ in self.segs:
            if vaddr <= va < vaddr + filesz:
                n = min(n, vaddr + filesz - va)
                return self.data[off + va - vaddr: off + va - vaddr + n]
        return b''

    def u32(self, va):
        b = self.read(va, 4)
        return struct.unpack('<I', b)[0] if len(b) == 4 else None

    def functions(self):
        """(start, end) of every FDE, sorted."""
        eh = next(p for p in self.ph if p[0] == PT_GNU_EH_FRAME)
        hdr = eh[3]
        ver, ptr_enc, cnt_enc, tab_enc = self.read(hdr, 4)
        assert (ver, cnt_enc, tab_enc) == (1, 0x03, 0x3b), 'unexpected eh_frame_hdr encoding'
        count, = struct.unpack('<I', self.read(hdr + 8, 4))
        table = self.read(hdr + 12, count * 8)
        out = []
        for i in range(count):
            loc, fde = struct.unpack_from('<ii', table, i * 8)
            start, fde_va = hdr + loc, hdr + fde
            # FDE: length(4) cie_ptr(4) pc_begin(pcrel sdata4) pc_range(udata4)
            pc_range = self.u32(fde_va + 12) or 0
            out.append((start, start + pc_range))
        out.sort()
        return out


def import_slots():
    """GOT slot (guest offset) -> symbol name, from out/boot.bin relocations."""
    data = BOOT.read_bytes()
    size, entry, ns, nr, ni, caps = struct.unpack_from('<QQQQQQ', data, 8)
    pos = 56 + ns * 24
    nids = [data[pos + i * 128:pos + (i + 1) * 128].split(b'\0')[0].decode() for i in range(ni)]
    pos += ni * 128
    symbols = dict(re.findall(r'\{"([^"]+)","([^"]+)"\}', NAMES.read_text(encoding='utf-8')))
    slots = {}
    for i in range(nr):
        target, kind, value, addend = struct.unpack_from('<QQqq', data, pos + i * 32)
        if kind and 0 <= value < ni:
            nid = nids[value].split('#', 1)[0]  # "NID#lib#module" -> NID
            slots[target] = symbols.get(nid, nid)
    return slots


RIP = re.compile(r'\[rip ([+-]) 0x([0-9a-f]+)\]')


def build():
    try:
        import capstone
    except ImportError:
        raise SystemExit('pip install capstone')
    img = Image()
    funcs = img.functions()
    slots = import_slots()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    calls = []    # (site, target, kind) kind: 'c' call, 'j' jmp to a function start
    refs = []     # (site, target) RIP-relative memory reference
    gotcalls = [] # (site, import name)
    starts = set(s for s, _ in funcs)
    total = len(funcs)
    decoded = wanted = 0
    for n, (start, end) in enumerate(funcs):
        if n % 5000 == 0:
            print(f'  {n}/{total} functions', file=sys.stderr)
        code = img.read(start, end - start)
        wanted += len(code)
        for addr, size, mnem, ops in md.disasm_lite(code, start):
            decoded += size
            if mnem == 'call' or mnem == 'jmp':
                if ops.startswith('0x'):
                    t = int(ops, 16)
                    if mnem == 'call' or t in starts:
                        calls.append((addr, t, mnem[0]))
                    continue
            m = RIP.search(ops)
            if m:
                disp = int(m.group(2), 16)
                t = addr + size + (disp if m.group(1) == '+' else -disp)
                if (mnem in ('call', 'jmp')) and t in slots:
                    gotcalls.append((addr, slots[t]))
                else:
                    refs.append((addr, t))
    print(f'  decoded {decoded}/{wanted} function bytes', file=sys.stderr)
    # PLT stubs: jmp [rip+GOT], after the last FDE in the text segment
    plt = {}
    text = next(s for s in img.segs if s[3] & 1)  # the executable segment
    raw = img.read(text[0], text[1])
    for m in re.finditer(rb'\xff\x25', raw):
        a = m.start()
        disp, = struct.unpack_from('<i', raw, a + 2)
        t = a + 6 + disp
        if t in slots:
            plt[a] = slots[t]
    # strings: printable runs of 5+ chars ending in NUL, in every mapped segment outside code
    strings = {}
    fstarts = [s for s, _ in funcs]
    for vaddr, filesz, off, flags in img.segs:
        blob = img.data[off:off + filesz]
        for m in re.finditer(rb'[\x09\x20-\x7e]{5,}\x00', blob):
            va = vaddr + m.start()
            i = bisect.bisect_right(fstarts, va) - 1
            if i >= 0 and funcs[i][0] <= va < funcs[i][1]:
                continue
            strings[va] = m.group()[:-1].decode('latin-1')
    INDEX.parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(INDEX, 'wb') as f:
        pickle.dump({'funcs': funcs, 'calls': calls, 'refs': refs, 'gotcalls': gotcalls,
                     'plt': plt, 'strings': strings}, f, protocol=pickle.HIGHEST_PROTOCOL)
    print(f'{len(funcs)} functions, {len(calls)} calls, {len(refs)} rip refs, '
          f'{len(gotcalls)} GOT calls, {len(plt)} PLT stubs, {len(strings)} strings -> {INDEX}')


class Index:
    def __init__(self):
        if not INDEX.exists():
            raise SystemExit('run: eboot_re.py build')
        with gzip.open(INDEX, 'rb') as f:
            d = pickle.load(f)
        self.__dict__.update(d)
        self.fstarts = [s for s, _ in self.funcs]
        self.notes = {}
        if NOTES.exists():
            for line in NOTES.read_text(encoding='utf-8').splitlines():
                parts = line.split(None, 1)
                if len(parts) == 2 and parts[0].startswith('0x'):
                    self.notes[int(parts[0], 16)] = parts[1].split('  #', 1)[0].strip()

    def func_of(self, a):
        i = bisect.bisect_right(self.fstarts, a) - 1
        if i >= 0 and self.funcs[i][0] <= a < self.funcs[i][1]:
            return self.funcs[i]
        return None

    def name(self, a):
        if a in self.plt:
            return 'import:' + self.plt[a]
        f = self.func_of(a)
        if f and f[0] == a:
            return self.notes.get(a, f'sub_{a:x}')
        if f:
            return f'{self.notes.get(f[0], f"sub_{f[0]:x}")}+{a - f[0]:#x}'
        return f'{a:#x}'

    def describe_data(self, t):
        if t in self.strings:
            return repr(self.strings[t][:80])
        return None


def parse_addr(s):
    return int(s, 16) if s.lower().startswith('0x') else int(s, 16)


def cmd_func(ix, a):
    f = ix.func_of(a)
    print(f'{ix.name(f[0])} {f[0]:#x}-{f[1]:#x} ({f[1] - f[0]} bytes)' if f else 'not in a function')


def cmd_dis(ix, a, n):
    import capstone
    f = ix.func_of(a)
    if not f:
        raise SystemExit('not in a function')
    img = Image()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    gotmap = dict(ix.gotcalls)
    print(f'; {ix.name(f[0])} {f[0]:#x}-{f[1]:#x}')
    for i, (addr, size, mnem, ops) in enumerate(md.disasm_lite(img.read(f[0], f[1] - f[0]), f[0])):
        if i >= n:
            print('; ...')
            break
        note = ''
        if mnem in ('call', 'jmp') and ops.startswith('0x'):
            note = ix.name(int(ops, 16))
        elif addr in gotmap:
            note = 'import:' + gotmap[addr]
        else:
            m = RIP.search(ops)
            if m:
                disp = int(m.group(2), 16)
                t = addr + size + (disp if m.group(1) == '+' else -disp)
                note = ix.describe_data(t) or ix.name(t) if ix.func_of(t) else (ix.describe_data(t) or f'data {t:#x}')
        print(f'{addr:#10x}: {mnem:8} {ops:40} {"; " + note if note else ""}')


def callers_of(ix, start):
    out = {}
    for site, t, k in ix.calls:
        if t == start:
            f = ix.func_of(site)
            if f:
                out.setdefault(f[0], []).append(site)
    return out


def cmd_callers(ix, a, depth):
    f = ix.func_of(a)
    if not f and a not in ix.plt:
        raise SystemExit('not in a function')
    root = f[0] if f else a
    seen = set()

    def walk(fn, d):
        for c, sites in sorted(callers_of(ix, fn).items()):
            print('  ' * d + f'{ix.name(c)} ({len(sites)} site{"s" if len(sites) > 1 else ""}: '
                  + ', '.join(f'{s:#x}' for s in sites[:4]) + ')')
            if d + 1 < depth and c not in seen:
                seen.add(c)
                walk(c, d + 1)
    print(ix.name(root))
    walk(root, 1)


def cmd_callees(ix, a):
    f = ix.func_of(a)
    if not f:
        raise SystemExit('not in a function')
    out = {}
    for site, t, k in ix.calls:
        if f[0] <= site < f[1]:
            out.setdefault(t, 0)
            out[t] += 1
    for site, name in ix.gotcalls:
        if f[0] <= site < f[1]:
            out.setdefault(('import', name), 0)
            out[('import', name)] += 1
    for t, c in sorted(out.items(), key=lambda kv: str(kv[0])):
        print(f'{c:3}x  ' + (f'import:{t[1]}' if isinstance(t, tuple) else ix.name(t)))


def refs_to(ix, lo, hi):
    out = {}
    for site, t in ix.refs:
        if lo <= t < hi:
            f = ix.func_of(site)
            out.setdefault(f[0] if f else site, []).append((site, t))
    return out


def cmd_str(ix, pattern, limit):
    rx = re.compile(pattern)
    hits = [(a, s) for a, s in ix.strings.items() if rx.search(s)]
    hits.sort()
    byaddr = {}
    for site, t in ix.refs:
        byaddr.setdefault(t, []).append(site)
    for a, s in hits[:limit]:
        users = sorted({ix.func_of(x)[0] for x in byaddr.get(a, []) if ix.func_of(x)})
        print(f'{a:#x} {s[:100]!r}')
        for u in users[:8]:
            print(f'    used by {ix.name(u)}')
    if len(hits) > limit:
        print(f'... {len(hits) - limit} more')


def cmd_refs(ix, a, rng):
    for fn, sites in sorted(refs_to(ix, a, a + max(rng, 1)).items()):
        print(f'{ix.name(fn)}: ' + ', '.join(f'{s:#x}->{t:#x}' for s, t in sites[:6]))


def cmd_imports(ix, pattern):
    users = {}
    plt_by_addr = ix.plt
    for site, t, k in ix.calls:
        if t in plt_by_addr:
            f = ix.func_of(site)
            if f:
                users.setdefault(plt_by_addr[t], set()).add(f[0])
    for site, name in ix.gotcalls:
        f = ix.func_of(site)
        if f:
            users.setdefault(name, set()).add(f[0])
    rx = re.compile(pattern) if pattern else None
    for name in sorted(set(plt_by_addr.values()) | set(users)):
        if rx and not rx.search(name):
            continue
        print(f'{len(users.get(name, ())):5}  {name}')


def cmd_icallers(ix, name):
    stubs = [a for a, n in ix.plt.items() if name in n]
    fns = {}
    for site, t, k in ix.calls:
        if t in stubs:
            f = ix.func_of(site)
            if f:
                fns.setdefault(f[0], []).append((site, ix.plt[t]))
    for site, n in ix.gotcalls:
        if name in n:
            f = ix.func_of(site)
            if f:
                fns.setdefault(f[0], []).append((site, n))
    for fn, sites in sorted(fns.items()):
        print(f'{ix.name(fn)}: ' + ', '.join(f'{s:#x}({n})' for s, n in sites[:4]))


def cmd_fstr(ix, a):
    f = ix.func_of(a)
    if not f:
        raise SystemExit('not in a function')
    for site, t in ix.refs:
        if f[0] <= site < f[1] and t in ix.strings:
            print(f'{site:#x}: {ix.strings[t][:120]!r}')


def text_segment(img):
    seg = next(s for s in img.segs if s[3] & 1)
    return seg[0], img.read(seg[0], seg[1])


def masked_bytes(start, maxlen):
    """Instruction bytes from start with relative displacements (call/jmp/jcc rel, RIP-relative
    memory) replaced by None: they change when the code or data around them moves."""
    import capstone
    img = Image()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    out = []
    for ins in md.disasm(img.read(start, maxlen + 16), start):
        b = list(ins.bytes)
        enc = ins.disp_offset, ins.disp_size, ins.imm_offset, ins.imm_size
        relative = capstone.x86.X86_GRP_BRANCH_RELATIVE in ins.groups if hasattr(capstone.x86, 'X86_GRP_BRANCH_RELATIVE') else ins.group(capstone.CS_GRP_BRANCH_RELATIVE)
        if relative and ins.imm_size:
            for k in range(ins.imm_offset, ins.imm_offset + ins.imm_size):
                b[k] = None
        if ins.disp_size and 'rip' in ins.op_str:
            for k in range(ins.disp_offset, ins.disp_offset + ins.disp_size):
                b[k] = None
        out.extend(b)
        if len(out) >= maxlen:
            break
    return out[:maxlen]


def sig_regex(masked):
    return re.compile(b''.join(b'.' if x is None else re.escape(bytes([x])) for x in masked), re.S)


def cmd_sig(ix, a, maxlen):
    base, text = text_segment(Image())
    masked = masked_bytes(a, maxlen)
    for n in range(8, len(masked) + 1, 4):
        hits = [m.start() for m in sig_regex(masked[:n]).finditer(text)]
        if len(hits) == 1:
            print(' '.join('??' if x is None else f'{x:02x}' for x in masked[:n]))
            print(f'; unique at {base + hits[0]:#x} ({ix.name(base + hits[0])}), {n} bytes')
            return
    print(f'; not unique within {maxlen} bytes: {len(hits)} matches')


def cmd_sigfind(pattern):
    base, text = text_segment(Image())
    masked = [None if t == '??' else int(t, 16) for t in pattern.split()]
    for m in sig_regex(masked).finditer(text):
        print(f'{base + m.start():#x}')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('cmd')
    p.add_argument('arg', nargs='?')
    p.add_argument('-n', type=int, default=400)
    p.add_argument('-d', type=int, default=1)
    p.add_argument('-l', type=int, default=40)
    p.add_argument('-r', type=lambda s: int(s, 0), default=1)
    a = p.parse_args()
    if a.cmd == 'build':
        build()
        return
    if a.cmd == 'sigfind':
        cmd_sigfind(a.arg)
        return
    ix = Index()
    if a.cmd == 'func':
        cmd_func(ix, parse_addr(a.arg))
    elif a.cmd == 'dis':
        cmd_dis(ix, parse_addr(a.arg), a.n)
    elif a.cmd == 'callers':
        cmd_callers(ix, parse_addr(a.arg), a.d)
    elif a.cmd == 'callees':
        cmd_callees(ix, parse_addr(a.arg))
    elif a.cmd == 'str':
        cmd_str(ix, a.arg, a.l)
    elif a.cmd == 'refs':
        cmd_refs(ix, parse_addr(a.arg), a.r)
    elif a.cmd == 'imports':
        cmd_imports(ix, a.arg)
    elif a.cmd == 'icallers':
        cmd_icallers(ix, a.arg)
    elif a.cmd == 'fstr':
        cmd_fstr(ix, parse_addr(a.arg))
    elif a.cmd == 'sig':
        cmd_sig(ix, parse_addr(a.arg), a.n if a.n != 400 else 64)
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
