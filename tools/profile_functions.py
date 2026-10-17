#!/usr/bin/env python3
"""sotc: per-function self time (leaf samples) of a SOTC_PROFILE_OUT dump:
profile_functions.py dump [exe] [top] [--inlines] [--qualified]
--inlines: an address counts for every function inlined at it (self time for the innermost one),
so code inlined into big callers (LTO/PGO builds) shows under its own name.
--qualified: Namespace::Class::function names instead of short ones (lambdas: operator() of ...)."""
import collections, re, subprocess, sys
flags = {a for a in sys.argv[1:] if a.startswith('--')}
args = [a for a in sys.argv[1:] if not a.startswith('--')]
exe = args[1] if len(args) > 1 else 'out/gpu/bb-probe.exe'
top = int(args[2]) if len(args) > 2 else 45
inlines = '--inlines' in flags
qualified = '--qualified' in flags
rows, samples = [], 1
for line in open(args[0], encoding='utf-8', errors='replace'):
    parts = line.split()
    if parts[0] == 'samples':
        samples = int(parts[1]); continue
    rows.append((' '.join(parts[:-2]), int(parts[-2]), int(parts[-1])))
own = [r for r in rows if r[0].startswith('bb-probe.exe+')]
addrs = [hex(0x140000000 + int(r[0].split('+')[1], 16)) for r in own]


def short(name):
    """Drops parameter lists and template arguments so names stay readable."""
    if not qualified:
        return name
    out, depth = [], 0
    for ch in name:
        if ch in '<(':
            depth += 1
        elif ch in '>)':
            depth -= 1
        elif depth == 0:
            out.append(ch)
    return re.sub(r'\s+', ' ', ''.join(out)).strip()


chains = []  # per address: function names, innermost first
for i in range(0, len(addrs), 2000):
    cmd = ['llvm-symbolizer', f'--obj={exe}', '--functions=linkage' if qualified else '--functions=short',
           '--inlines' if inlines else '--no-inlines', '--demangle']
    out = subprocess.run(cmd + addrs[i:i + 2000], capture_output=True, text=True).stdout.strip().split('\n\n')
    for block in out:
        lines = block.split('\n')
        chains.append([short(lines[j]) for j in range(0, len(lines), 2)] or ['??'])
self_time = collections.Counter()
incl = collections.Counter()
for (where, leaf, inc), chain in zip(own, chains):
    self_time[chain[0]] += leaf
    seen = set()
    for depth, name in enumerate(chain):
        if name in seen:
            continue
        seen.add(name)
        # The leaf address's innermost frame is self time; every other frame here is a caller.
        incl[name] += inc if depth > 0 else inc - leaf
other = collections.Counter()
for where, leaf, inc in rows:
    if not where.startswith('bb-probe.exe+'):
        other[re.sub(r'\+0x[0-9a-f]+$', '', where)] += leaf
print(f'{samples} samples; self time by function:')
for name, n in self_time.most_common(40):
    print(f'  {100*n/samples:5.1f}%  {name[:110]}')
print('outside the loader (by module):')
for name, n in other.most_common(10):
    print(f'  {100*n/samples:5.1f}%  {name}')
print('inclusive (callees) by function:')
for name, n in incl.most_common(top):
    print(f'  {100*n/samples:5.1f}%  {name[:110]}')
