#!/usr/bin/env python3
"""Call stacks whose live heap grew between two reports of a BB_HEAP_TRACK build.

usage: heap_track_report.py <SOTC_HEAP_TRACK file> <bb-probe.exe> [first block] [last block] [count]

Blocks are numbered from 0 (default: the second and the last, skipping boot). Frames in the exe
are symbolized with llvm-symbolizer (function and file:line when the exe has debug info).
"""
import re
import subprocess
import sys


def blocks(path):
    out, current = [], None
    for line in open(path, errors='replace'):
        if line.startswith('==='):
            current = {'header': line.strip(), 'stacks': {}}
            out.append(current)
        elif current is not None and '|' in line:
            sizes, frames = line.split('|', 1)
            size, count = map(int, sizes.split())
            current['stacks'][frames.strip()] = (size, count)
    return out


def image_base(exe):
    text = subprocess.run(['llvm-readobj', '--file-headers', exe], capture_output=True, text=True).stdout
    match = re.search(r'ImageBase:\s*0x([0-9A-Fa-f]+)', text)
    return int(match.group(1), 16) if match else 0x140000000


def main():
    path, exe = sys.argv[1], sys.argv[2]
    data = blocks(path)
    first = data[int(sys.argv[3]) if len(sys.argv) > 3 else min(1, len(data) - 1)]
    last = data[int(sys.argv[4]) if len(sys.argv) > 4 else -1]
    count = int(sys.argv[5]) if len(sys.argv) > 5 else 12
    print('from', first['header'])
    print('to  ', last['header'])
    growth = []
    for frames, (size, number) in last['stacks'].items():
        old_size, old_number = first['stacks'].get(frames, (0, 0))
        growth.append((size - old_size, number - old_number, size, frames))
    growth.sort(reverse=True)
    name = exe.replace('\\', '/').rsplit('/', 1)[-1].lower()
    base = image_base(exe)
    addresses = set()
    for item in growth[:count]:
        for frame in item[3].split():
            module, _, offset = frame.partition('+')
            if module.lower() == name and offset:
                addresses.add(base + int(offset, 16) - 1)  # return address -> call instruction
    symbols = {}
    if addresses:
        ordered = sorted(addresses)
        result = subprocess.run(['llvm-symbolizer', '--obj=' + exe, '--functions=short', '--no-inlines'],
                                input='\n'.join(hex(a) for a in ordered), capture_output=True, text=True)
        lines = [l for l in result.stdout.split('\n\n') if l.strip()]
        for address, text in zip(ordered, lines):
            function, _, location = text.strip().partition('\n')
            location = re.sub(r'^.*[\\/](gpu|src)[\\/]', r'\1/', location.strip())
            symbols[address] = f'{function} ({location})'
    for grown, grown_count, size, frames in growth[:count]:
        print(f'\n+{grown / 1048576:.1f} MiB in +{grown_count} allocations (live {size / 1048576:.1f} MiB)')
        for frame in frames.split():
            module, _, offset = frame.partition('+')
            if module.lower() == name and offset:
                print('   ', symbols.get(base + int(offset, 16) - 1, frame))
            else:
                print('   ', frame)


if __name__ == '__main__':
    main()
