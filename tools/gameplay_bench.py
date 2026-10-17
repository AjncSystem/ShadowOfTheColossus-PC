#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Gameplay benchmark on a save that loads straight into gameplay (no shrine cutscene).

Restores out/save-template, skips the opening cinematic, presses Continue until the log shows
"st_gameflow() starting game", then stops all input, waits --settle seconds and measures
--measure seconds. Prints one line: avg/min FPS, cmds/frame, p50/p99 frame times.

    python tools/gameplay_bench.py [--measure 40] [--settle 25] [--env NAME=VALUE ...]

Needs out/bb-probe.exe, the prepared image in out/, SOTC_GAME_DIR and out/bench.patches /
out/bench.cmdline (scripts/sotc_modes.py).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = os.environ.get('SOTC_GAME_DIR', str(ROOT.parent / 'CUSA08809'))
FPS_LINE = re.compile(r'SOTC FPS ([\d.]+) at (\d+)s; (\d+) cmds/frame')
FRAMES_LINE = re.compile(r'SOTC FRAMES p50 ([\d.]+) ms, p99 ([\d.]+) ms.*?worst ([\d.]+) ms')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--measure', type=float, default=40)
    parser.add_argument('--settle', type=float, default=25)
    parser.add_argument('--timeout', type=float, default=300)
    parser.add_argument('--log', default='out/run.txt')
    parser.add_argument('--env', action='append', default=[])
    parser.add_argument('--patches', default='out/bench.patches')
    parser.add_argument('--cmdline', default='out/bench.cmdline')
    parser.add_argument('--user', default='out/user', help='game user folder (saves, shader cache)')
    parser.add_argument('--pan', type=int, default=0,
                        help='right stick x (0-255, 128 = centre) held during the measurement: turns the camera')
    parser.add_argument('--walk', action='store_true', help='the character walks around during the run')
    parser.add_argument('--capture', type=int, default=0,
                        help='window captures (PrintWindow, window stays behind) in the middle of the measurement')
    args = parser.parse_args()
    os.chdir(ROOT)
    out = Path('out')
    template = out / 'save-template' / 'CUSA08809'
    target = Path(args.user) / 'savedata' / '1' / 'CUSA08809'
    shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(template, target)
    pad = out / 'pad.txt'
    pad.write_text('')
    env = dict(os.environ)
    env['PATH'] = str(Path(os.environ.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin') + os.pathsep + env['PATH']
    shutil.rmtree(out / 'dump', ignore_errors=True)
    (out / 'dump').mkdir(parents=True)
    env.update(BB_PAD_FILE=str(pad), BB_VBLANK_HZ='120', SOTC_FPS='1', SOTC_SAVE_PROMPT='auto',
               SOTC_WINDOW_BEHIND='1', SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0', SOTC_MOUSE='0',
               BB_FRAME_DUMP_TRIGGER=str(out / 'dump.trigger'), BB_DUMP_DIR=str(out / 'dump'))
    for item in args.env:
        name, value = item.split('=', 1)
        env[name] = value
    command = [str(out / 'bb-probe.exe'), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
               '--sotc-cpu', str(out / 'sotc-cpu.bin'), '--app0', GAME, '--user', str(Path(args.user))]
    if Path(args.patches).exists():
        command += ['--patches', args.patches]
    if Path(args.cmdline).exists():
        command += ['--sotc-cmdline', args.cmdline]
    log = open(args.log, 'wb')
    process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    start = time.monotonic()
    state = {'gameplay_at': None, 'stop': False}

    def now():
        return time.monotonic() - start

    def alive():
        return process.poll() is None and not state['stop']

    def press(what, hold):
        pad.write_text(what)
        end = time.monotonic() + hold
        while time.monotonic() < end and alive():
            time.sleep(0.02)
        pad.write_text('')

    def wait_until(t):
        while alive() and now() < t:
            time.sleep(0.05)
        return alive()

    def log_text():
        try:
            return Path(args.log).read_text(encoding='utf-8', errors='replace')
        except OSError:
            return ''

    def route():
        for at, hold in ((34, 0.25), (35, 6), (42, 6)):
            if not wait_until(at):
                return
            press('cross', hold)
        at = 50
        while alive():
            if 'st_gameflow() starting game' in log_text():
                state['gameplay_at'] = now()
                return
            if not wait_until(at):
                return
            if 'st_gameflow() starting game' not in log_text():
                press('cross', 0.3)
            at += 3

    # --walk: the character walks around from the start of gameplay (left stick, camera turning
    # with the right stick, changes of direction so that walls do not stop it for long).
    walk_route = (('ly=0', 4.0), ('ly=0 rx=210', 2.5), ('ly=0', 4.0), ('lx=40 ly=0', 3.0),
                  ('ly=0 rx=50', 2.5), ('ly=0', 3.0), ('lx=220 ly=30', 3.0), ('ly=0 rx=200 ry=100', 2.0),
                  ('ly=255', 2.0), ('ly=0 rx=60', 3.0))

    def walk():
        while alive() and state['gameplay_at'] is None:
            time.sleep(0.1)
        if not wait_until(state['gameplay_at'] + 2):
            return
        while alive():
            for what, hold in walk_route:
                pad.write_text(what)
                if not wait_until(now() + hold):
                    return
        pad.write_text('')

    if args.walk:
        threading.Thread(target=walk, daemon=True).start()
    threading.Thread(target=route, daemon=True).start()
    shot = False
    panning = False
    while alive() and now() < args.timeout:
        g = state['gameplay_at']
        if g is not None and args.pan and not panning and now() >= g + args.settle - 3:
            pad.write_text(f'rx={args.pan}')
            panning = True
        if g is not None and not shot and now() >= g + args.settle + args.measure / 2:
            (out / 'dump.trigger').touch()
            if args.capture:
                subprocess.Popen([sys.executable, str(ROOT / 'tools' / 'wincap.py'), str(process.pid),
                                  str(out / 'dump' / 'win_'), str(args.capture), '30'])
            shot = True
        if g is not None and now() >= g + args.settle + args.measure + 1:
            break
        time.sleep(0.2)
    state['stop'] = True
    if process.poll() is None:
        process.kill()
        process.wait()
    log.close()
    raws = sorted((out / 'dump').glob('output_*.raw'))
    if raws:
        subprocess.run([sys.executable, str(ROOT / 'tools' / 'raw2png.py'), *map(str, raws)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    text = log_text()
    g = state['gameplay_at']
    if g is None:
        crashed = re.search(r'^(Fault|STOP:).*$|Assertion Failed', text, re.M)
        print('INVALID: gameplay never started' + (f' ({crashed.group(0)[:80]})' if crashed else ''))
        return 1
    # FPS lines carry the run time in seconds since the game's main loop; the game starts about
    # as many seconds after the process, so match them by their order relative to the start line.
    head, _, tail = text.partition('st_gameflow() starting game')
    fps = [(float(m.group(1)), int(m.group(3))) for m in FPS_LINE.finditer(tail)]
    frames = [(float(m.group(1)), float(m.group(2)), float(m.group(3))) for m in FRAMES_LINE.finditer(tail)]
    skip = int(args.settle // 5)
    take = int(args.measure // 5)
    fps, frames = fps[skip:skip + take], frames[skip:skip + take]
    if not fps:
        print('INVALID: no samples')
        return 1
    avg = sum(f for f, _ in fps) / len(fps)
    cmds = sum(c for _, c in fps) / len(fps)
    p50 = sum(f[0] for f in frames) / max(1, len(frames))
    p99 = max((f[1] for f in frames), default=0)
    stop = re.search(r'^STOP:.*$', text, re.M)
    print(f'avg {avg:.1f} FPS, min {min(f for f, _ in fps):.1f}, {cmds:.0f} cmds/frame, '
          f'p50 {p50:.1f} ms, worst p99 {p99:.1f} ms ({len(fps)} samples)' + (f' CRASH {stop.group(0)}' if stop else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
