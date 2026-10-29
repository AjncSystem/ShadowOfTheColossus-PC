#!/usr/bin/env python3
"""sotc: off-CPU analysis of one thread from a WPR "CPU" profile converted by tracerpt to CSV.

    wpr -start CPU -filemode   ...   wpr -stop trace.etl              (administrator)
    tracerpt trace.etl -o events.csv -of CSV -y
    python tools/etw_offcpu.py events.csv <thread id> [out/gpu/bb-probe.exe]

tracerpt does not decode the CSwitch payload, so the switches are rebuilt from three event kinds:
  - PerfInfo/SampleProf (1 kHz per CPU): the thread was on a CPU at that time;
  - StackWalk events of the thread that are not sample stacks: the stack taken when the thread
    was switched out (a CSwitch stack), at its switch-out time;
  - Thread/ReadyThread with the thread as target: it was woken from a wait.
A switch-out followed by a ReadyThread before the thread runs again was a WAIT (its stack says
where it waited); one with no ReadyThread was a PREEMPTION (it stayed runnable). Clock ticks are
100 ns; the sample rate bounds the end of each off-CPU span to about 1 ms.
User frames inside the exe are symbolized with llvm-symbolizer (MSYS2 CLANG64) when available.
"""
import bisect
import collections
import shutil
import struct
import subprocess
import sys

KERNEL = 0xFFFF800000000000


def pe_image_base(exe):
    with open(exe, 'rb') as f:
        data = f.read(4096)
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    return struct.unpack_from('<Q', data, pe + 24 + 24)[0]  # PE32+ OptionalHeader.ImageBase


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    path, tid = sys.argv[1], int(sys.argv[2])
    exe = sys.argv[3] if len(sys.argv) > 3 else None
    samples, readies, stacks = [], [], []
    sample_kernel = 0
    last_sample = {}             # processor -> clock of the thread's last sample there
    votes = collections.Counter()  # clock - qpc of (sample, its stack) pairs
    exe_base = exe_size = None
    images = []  # (base, size, module name)
    exe_name = (exe or 'bb-probe.exe').replace('\\', '/').split('/')[-1].lower()
    with open(path, 'r', errors='replace') as f:
        for line in f:
            head = line[:40]
            if 'SampleProf' in head:
                p = [x.strip() for x in line.split(',')]
                if int(p[20]) == tid:
                    clock = int(p[16])
                    samples.append(clock)
                    last_sample[p[11]] = clock
                    if int(p[19], 16) >= KERNEL:
                        sample_kernel += 1
            elif 'ReadyThread' in head:
                p = [x.strip() for x in line.split(',')]
                if int(p[19], 16) == tid:
                    readies.append(int(p[16]))
            elif 'StackWalk' in head:
                p = [x.strip() for x in line.split(',')]
                if int(p[21]) != tid:
                    continue
                qpc = int(p[19])
                if p[11] in last_sample:  # a sample stack follows its sample on the same CPU
                    votes[last_sample.pop(p[11]) - qpc] += 1
                stacks.append((qpc, [int(a, 16) for a in p[22:] if a.startswith('0x')]))
            elif 'Image' in head and ('DCStart' in head or 'Load' in head):
                p = [x.strip() for x in line.split(',')]
                try:
                    base, size = int(p[19], 16), int(p[20], 16)
                except ValueError:
                    continue
                name = line.rsplit(',', 1)[-1].strip().strip('"').replace(chr(92), '/').split('/')[-1]
                images.append((base, size, name.lower()))
                if exe_name in line.lower():
                    exe_base, exe_size = base, size
    if not samples or not votes:
        raise SystemExit('no samples or stacks for that thread id')
    offset = votes.most_common(1)[0][0]
    samples.sort()
    readies.sort()
    # Second pass: keep only the stacks whose event is a CSwitch (kernel Thread GUID, opcode 36).
    # The others are e.g. ReadyThread stacks: this thread waking another one.
    stack_clocks = {qpc + offset for qpc, _ in stacks}
    cswitch = set()
    with open(path, 'r', errors='replace') as f:
        for line in f:
            if line.startswith('{3d6fa8d1-fe05-11d0-9dda-00c04fd7ba7c},         36'):
                clock = int(line.split(',')[16])
                if clock in stack_clocks:
                    cswitch.add(clock)
    print(f"stacks of the thread: {len(stacks)}, of them context switches: {len(cswitch)}")
    waits, preempt = [], []
    for qpc, frames in stacks:
        clock = qpc + offset
        if clock not in cswitch:
            continue  # a sample stack or another event's stack
        nxt = bisect.bisect_right(samples, clock)
        if nxt >= len(samples):
            continue
        run_again = samples[nxt]
        r = bisect.bisect_right(readies, clock)
        if r < len(readies) and readies[r] <= run_again:
            waits.append((readies[r] - clock, run_again - readies[r], frames))
        else:
            preempt.append(run_again - clock)
    span = samples[-1] - samples[0]
    ms = lambda t: t / 10_000.0
    print(f"thread {tid}: {span / 1e7:.1f} s, {len(samples)} samples = on CPU about "
          f"{100.0 * len(samples) / (span / 1e4):.0f}% of the time; kernel IP in "
          f"{100.0 * sample_kernel / len(samples):.1f}% of the samples")
    print(f"waits: {len(waits)}, {ms(sum(w[0] for w in waits)):.0f} ms blocked + "
          f"{ms(sum(w[1] for w in waits)):.0f} ms from wake to running (<= 1 ms resolution)")
    print(f"preemptions: {len(preempt)}, <= {ms(sum(preempt)):.0f} ms runnable but not running")
    images.sort()

    def module_of(a):
        for base, size, name in images:
            if base <= a < base + size:
                return name
        return hex(a)

    groups = collections.defaultdict(lambda: [0, 0])
    for dur, _, frames in waits:
        user = [a for a in frames if a < KERNEL]
        mine = [a for a in user if exe_base and exe_base <= a < exe_base + exe_size]
        key = ('exe', tuple(mine[:5])) if mine else ('other', tuple(module_of(a) for a in user[:3]))
        groups[key][0] += 1
        groups[key][1] += dur
    sym_tool = shutil.which('llvm-symbolizer')
    vbase = pe_image_base(exe) if exe else 0
    cache = {}

    def sym(a):
        if not (exe and sym_tool and exe_base):
            return hex(a)
        if a not in cache:
            r = subprocess.run([sym_tool, '--obj=' + exe, '--functions=short', '--no-inlines',
                                hex(a - exe_base + vbase)], capture_output=True, text=True)
            cache[a] = (r.stdout.split('\n')[0].strip() or hex(a))[:60]
        return cache[a]

    print("where it waited (count, ms blocked): innermost frames of the exe")
    for (kind, addrs), (n, dur) in sorted(groups.items(), key=lambda kv: -kv[1][1])[:14]:
        names = [sym(a) for a in addrs] if kind == 'exe' else list(addrs)
        print(f"  {n:6d} {ms(dur):9.1f}  " + ' <- '.join(names))


if __name__ == '__main__':
    main()
