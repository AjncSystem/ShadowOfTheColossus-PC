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
    parser.add_argument('--save', default='out/save-template', help='folder with the CUSA08809 save to load')
    parser.add_argument('--explore', type=float, default=0,
                        help='run straight ahead after turning the camera this many seconds (heading)')
    parser.add_argument('--ride-speed', type=float, default=9.0,
                        help='camera speed (units/s) above which Wander is taken to be riding')
    parser.add_argument('--mount-rise', type=float, default=0.3,
                        help='camera rise (units) after Triangle that suggests Wander got on Agro')
    parser.add_argument('--horse', action='store_true',
                        help='with --walk: after 30 s, call Agro, mount and gallop around')
    parser.add_argument('--capture-ms', type=int, default=30, help='interval between window captures')
    parser.add_argument('--exe', default='out/bb-probe.exe', help='the executable (e.g. the installed copy)')
    parser.add_argument('--capture', type=int, default=0,
                        help='window captures (PrintWindow, window stays behind) in the middle of the measurement')
    args = parser.parse_args()
    os.chdir(ROOT)
    out = Path('out')
    template = Path(args.save) / 'CUSA08809'
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
    command = [str(Path(args.exe).resolve()), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
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
        start = now()
        while alive() and not (args.horse and now() - start > 30):
            for what, hold in walk_route:
                pad.write_text(what)
                if not wait_until(now() + hold):
                    return
        if args.horse:
            # Whistle for Agro (Triangle), wait for it, then hold Triangle next to it to mount. The
            # camera position (SOTC_CAM_LOG lines of the game log) tells riding from walking: a
            # spurred Agro runs much faster than Wander. Never press Triangle once mounted (it
            # dismounts).
            cam_line = re.compile(r'SOTC CAM (\d+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)')

            def speed(window_ms=1500):
                pts = [(int(m[1]), float(m[2]), float(m[4])) for m in cam_line.finditer(log_text())]
                if len(pts) < 2:
                    return 0.0
                recent = [p for p in pts if p[0] >= pts[-1][0] - window_ms]
                a, b = recent[0], recent[-1]
                dt = (b[0] - a[0]) / 1000.0
                return ((b[1] - a[1]) ** 2 + (b[2] - a[2]) ** 2) ** 0.5 / dt if dt > 0.3 else 0.0

            def height(window_ms=600):
                pts = [(int(m[1]), float(m[3])) for m in cam_line.finditer(log_text())]
                if not pts:
                    return 0.0
                recent = [p[1] for p in pts if p[0] >= pts[-1][0] - window_ms]
                return sum(recent) / len(recent)

            def hold_pad(sequence):
                for what, hold in sequence:
                    pad.write_text(what)
                    if not wait_until(now() + hold):
                        return False
                return True

            print(f'horse: on-foot speed {speed():.1f}/s', flush=True)
            if not hold_pad((('', 1.0), ('triangle', 0.3), ('', 12.0))):
                return
            steps = ('', 'ly=60', 'lx=60', 'ly=196', 'lx=196', 'lx=60 ly=60', 'lx=196 ly=196')
            state['mounted'] = False
            # Walk up to Agro, guided by window captures (agro_finder: the black horse on screen),
            # then hold Triangle next to it; whistle and turn the camera when it is out of sight.
            sys.path.insert(0, str(ROOT / 'tools'))
            import wincap
            import agro_finder
            hwnd = wincap.find_window(process.pid)

            def look():
                image = wincap.capture(hwnd) if hwnd else None
                return agro_finder.find_agro(image) if image is not None else None

            unseen = 0
            for step in range(200):
                seen = look()
                if seen is None:
                    unseen += 1
                    if unseen % 6 == 0 and not hold_pad((('triangle', 0.3), ('', 3.0))):  # whistle
                        return
                    if not hold_pad((('rx=210', 0.4), ('', 0.2))):
                        return
                    continue
                unseen = 0
                x, y, size = seen
                dx, dy = x - 0.5, y - 0.62  # from Wander, who stands near the screen centre
                lx = max(0, min(255, int(128 + dx * 600)))
                ly = max(0, min(255, int(128 + dy * 600)))
                if abs(dx) < 0.16 and abs(dy) < 0.2 and 120 < size < 2500:
                    # At Agro. Cross mounts from its side while facing it (at the head it pets
                    # it): walk around it in short sidesteps, face it again and press Cross; the
                    # camera rises once Wander is in the saddle.
                    image = wincap.capture(hwnd) if hwnd else None
                    if image is not None:
                        image.save(out / 'dump' / f'near{step:03d}.png')
                    side = 1 if state.setdefault('laps', 0) % 2 == 0 else -1
                    state['laps'] += 1
                    for k in range(8):
                        seen = look()
                        if seen is None:
                            break
                        x, y, size = seen
                        dx, dy = x - 0.5, y - 0.62
                        norm = max(1e-3, (dx * dx + dy * dy) ** 0.5)
                        tx, ty = -dy / norm * side, dx / norm * side  # around Agro
                        fx, fy = dx / norm, dy / norm                 # towards Agro
                        before = height()
                        if not hold_pad(((f'lx={int(128 + tx * 90)} ly={int(128 + ty * 90)}', 0.4),
                                         (f'lx={int(128 + fx * 45)} ly={int(128 + fy * 45)}', 0.2),
                                         ('', 0.25), ('cross', 0.25))):
                            return
                        if state.setdefault('bursts', 0) < 3:  # what does Cross do here?
                            state['bursts'] += 1
                            subprocess.Popen([sys.executable, str(ROOT / 'tools' / 'wincap.py'),
                                              str(process.pid), str(out / 'dump' / f'burst{step:03d}_{k}_'),
                                              '5', '250'])
                        if not hold_pad((('', 1.8),)):
                            return
                        rise = height() - before
                        print(f'horse: step {step}.{k} rise {rise:+.2f}', flush=True)
                        if rise > args.mount_rise:
                            if not hold_pad((('ly=0 cross', 0.25), ('ly=0', 0.6), ('ly=0 cross', 0.25),
                                             ('ly=0', 2.0))):
                                return
                            v = speed()
                            print(f'horse: speed {v:.1f}/s after the rise', flush=True)
                            if v > args.ride_speed:
                                state['mounted'] = True
                            break
                    if state['mounted']:
                        break
                    continue
                if not hold_pad(((f'lx={lx} ly={ly}', 0.35), ('', 0.05))):
                    return
            print(f'horse: mounted={state["mounted"]} at {now():.0f}s', flush=True)
            if state['mounted']:
                # Captures of the ride itself (ride_NN.png), starting 6 s into the gallop.
                threading.Timer(6.0, lambda: subprocess.Popen(
                    [sys.executable, str(ROOT / 'tools' / 'wincap.py'), str(process.pid),
                     str(out / 'dump' / 'ride_'), '8', '2000'])).start()
            ride = (('ly=0 cross', 0.25), ('ly=0', 1.2), ('ly=0 cross', 0.25), ('ly=0 rx=190', 2.0),
                    ('ly=0 cross', 0.25), ('lx=70 ly=0', 2.0), ('ly=0 cross', 0.25), ('ly=0', 2.0),
                    ('lx=190 ly=0 rx=80', 2.0))
            while alive():
                for what, hold in ride:
                    pad.write_text(what)
                    if not wait_until(now() + hold):
                        return
        pad.write_text('')

    # --explore SECONDS: turn the camera right for SECONDS (the heading: ~90 degrees a second at
    # full stick), then run straight ahead; when the camera (SOTC_CAM_LOG, needs SOTC_DLSSG=1)
    # barely moves for 2 s, Wander is blocked: turn the camera a little and go on.
    explore_cam = re.compile(r'SOTC CAM (\d+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)')

    def explore():
        while alive() and state['gameplay_at'] is None:
            time.sleep(0.1)
        if not wait_until(state['gameplay_at'] + 2):
            return
        pad.write_text('rx=255')
        if not wait_until(now() + args.explore):
            return
        turns = 0
        while alive():
            pad.write_text('ly=0')
            if not wait_until(now() + 2.0):
                return
            pts = [(int(m[1]), float(m[2]), float(m[4])) for m in explore_cam.finditer(log_text())]
            recent = [p for p in pts if pts and p[0] >= pts[-1][0] - 2000]
            if len(recent) >= 2:
                a, b = recent[0], recent[-1]
                if ((b[1] - a[1]) ** 2 + (b[2] - a[2]) ** 2) ** 0.5 < 2.0:
                    turns += 1
                    pad.write_text('rx=255 ly=0' if turns % 2 else 'rx=0 ly=0')
                    if not wait_until(now() + 0.6 + 0.2 * (turns % 4)):
                        return

    if args.explore:
        threading.Thread(target=explore, daemon=True).start()
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
                                  str(out / 'dump' / 'win_'), str(args.capture), str(args.capture_ms)])
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
