#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""sotc: adaptive Shrine of Worship benchmark.

Loads out/save-template (a save from after the first colossus, its shrine cutscene not yet seen),
skips the opening cinematic (hold Cross), presses Continue, then keeps skipping the shrine cutscene
until the log shows Wander standing idle at the altar (several FPS samples in a row at a steady
~5900 draws/dispatches per frame), stops all input and measures the next MEASURE seconds.
Load times vary between runs, so the cutscene skip is driven by the log, not by fixed times.

    python tools/sotc_bench_shrine.py [--measure 40] [--env NAME=VALUE ...]

Needs out/bb-probe.exe and the prepared image in out/ (sotc.py), SOTC_GAME_DIR, and
out/bench.patches / out/bench.cmdline (tools/sotc_bench_shrine.sh writes them).
Prints one summary line: "avg X FPS, min Y (...)", or "INVALID: ..." when the idle scene was
never reached.

Open-world routes recorded by a player (src/runtime_pad.c BB_PAD_RECORD / BB_PAD_REPLAY):
    --record out/route.txt   gets to the idle scene, then hands the game to the player with
                             recording armed (F9 starts and stops it) and waits for the window
                             to close.
    --replay out/route.txt   gets to the idle scene, plays the recording back and measures the
                             whole replay (frames dumped every 15 s).
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
GAME = os.environ.get('SOTC_GAME_DIR', str(Path(__file__).resolve().parents[2] / 'CUSA08809'))
IDLE_LOW, IDLE_HIGH, IDLE_SPREAD, IDLE_SAMPLES = 5500, 6400, 300, 3
FPS_LINE = re.compile(r'SOTC FPS ([\d.]+) at (\d+)s; (\d+) cmds/frame')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--measure', type=float, default=40)
    parser.add_argument('--timeout', type=float, default=330, help='give up after this many seconds')
    parser.add_argument('--log', default='out/run.txt')
    parser.add_argument('--env', action='append', default=[])
    parser.add_argument('--patches', default='out/bench.patches')
    parser.add_argument('--cmdline', default='out/bench.cmdline')
    parser.add_argument('--shots', type=int, default=3, help='frames dumped during the measurement')
    parser.add_argument('--template', default='out/save-template', help='save folder holding CUSA08809/')
    parser.add_argument('--record', help='hand over to the player with BB_PAD_RECORD=<file> (F9)')
    parser.add_argument('--replay', help='measure a replay of a BB_PAD_RECORD file')
    parser.add_argument('--assist', default='auto',
                        help='during a replay, also tap BUTTON every 0.4 s between START and END seconds '
                             '(START:END:BUTTON; "" for none). Agro, an AI character, does not stop at '
                             'exactly the same spot every run: tapping Cross mounts him once in reach '
                             '(and kicks him once mounted, as the recorded route does).')
    args = parser.parse_args()
    replay_seconds = 0.0
    if args.replay:
        lines = [l.split() for l in Path(args.replay).read_text().splitlines() if l.strip()]
        if not lines:
            print(f'INVALID: empty recording {args.replay}')
            return 1
        replay_seconds = int(lines[-1][0]) / 1000.0
        if args.assist == 'auto':
            # From just before the route's first Cross press (mounting Agro) to 5 s after it.
            first = next((int(l[0]) / 1000.0 for l in lines if int(l[1]) & 0x4000), None)
            args.assist = f'{max(0.0, first - 0.5):.1f}:{first + 5:.1f}:cross' if first is not None else ''
        args.measure = replay_seconds
        args.shots = max(1, int(replay_seconds // 15))
        args.timeout = max(args.timeout, 200 + replay_seconds)
    if args.record:
        args.timeout = max(args.timeout, 3600)
    os.chdir(ROOT)
    out = Path('out')
    template = Path(args.template) / 'CUSA08809'
    if not template.is_dir():
        print(f'INVALID: need {template}')
        return 1
    target = out / 'user' / 'savedata' / '1' / 'CUSA08809'
    shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(template, target)
    shutil.rmtree(out / 'dump', ignore_errors=True)
    (out / 'dump').mkdir(parents=True)
    pad = out / 'pad.txt'
    pad.write_text('')
    env = dict(os.environ)
    msys = os.environ.get('MSYS2_ROOT', r'C:\msys64')
    env['PATH'] = str(Path(msys) / 'clang64' / 'bin') + os.pathsep + env['PATH']
    env.update(BB_FRAME_DUMP_TRIGGER=str(out / 'dump.trigger'), BB_DUMP_DIR=str(out / 'dump'), BB_PAD_FILE=str(pad),
               BB_STACKS_TRIGGER=str(out / 'stacks.trigger'), BB_VBLANK_HZ='120', SOTC_FPS='1',
               SOTC_SAVE_PROMPT='auto')  # sotc: unattended runs never wait on a save prompt
    for item in args.env:
        name, value = item.split('=', 1)
        env[name] = value
    if args.record:
        env['BB_PAD_RECORD'] = str(Path(args.record).resolve())
    if args.replay:
        env['BB_PAD_REPLAY'] = str(Path(args.replay).resolve())
    command = [str(out / 'bb-probe.exe'), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
               '--sotc-cpu', str(out / 'sotc-cpu.bin'), '--app0', GAME, '--user', str(out / 'user'),
               '--patches', args.patches, '--sotc-cmdline', args.cmdline]
    log = open(args.log, 'wb')
    process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    start = time.monotonic()
    started = time.time()
    state = {'idle_at': None, 'stop': False, 'game_starts': 0, 'gameplay_at': None}
    samples = []  # (fps, cmds, arrival seconds)

    def press(what, hold=0.25, until=None):
        pad.write_text(what)
        end = time.monotonic() + hold
        while time.monotonic() < end and not (until and until()):
            time.sleep(0.02)
        pad.write_text('')

    def wait_until(t):
        while process.poll() is None and not state['stop'] and time.monotonic() - start < t:
            time.sleep(0.05)
        return process.poll() is None and not state['stop']

    def route():
        # Opening cinematic: "Hold X to skip" (the first press shows the prompt).
        for at, hold in ((34, 0.25), (35, 6), (42, 6)):
            if not wait_until(at):
                return
            press('cross', hold)
        # Title ("press any button"), menu (Continue is selected): tap X until the game starts
        # loading the save. The game logs "st_gameflow() starting game" once for that load...
        at = 50
        while state['game_starts'] < 1:
            if not wait_until(at):
                return
            if state['game_starts'] >= 1:
                break
            press('cross', 0.3)
            at += 3
        # ...and once more when the shrine cutscene is skipped and gameplay begins. Hold X until
        # then; no input afterwards (X would make Wander act).
        at = time.monotonic() - start + 4
        while state['game_starts'] < 2:
            if not wait_until(at):
                return
            if state['game_starts'] >= 2:
                break
            press('cross', 5, until=lambda: state['game_starts'] >= 2)
            at += 7
        state['gameplay_at'] = time.monotonic() - start

    def follow_log():
        seen = 0
        while process.poll() is None and not state['stop']:
            time.sleep(0.5)
            try:
                text = Path(args.log).read_text(encoding='utf-8', errors='replace')
            except OSError:
                continue
            state['game_starts'] = text.count('st_gameflow() starting game')
            for match in FPS_LINE.finditer(text, seen):
                seen = match.end()
                samples.append((float(match.group(1)), int(match.group(3)), time.monotonic() - start))
                # Idle: gameplay has begun and the draw count has settled (steady camera).
                recent = [s for s in samples[-IDLE_SAMPLES:] if state['gameplay_at'] is not None and
                          s[2] > state['gameplay_at'] + 2]
                if (state['idle_at'] is None and len(recent) == IDLE_SAMPLES and
                        max(s[1] for s in recent) - min(s[1] for s in recent) <=
                        max(IDLE_SPREAD, 0.06 * max(s[1] for s in recent))):
                    state['idle_at'] = time.monotonic() - start

    threading.Thread(target=route, daemon=True).start()
    threading.Thread(target=follow_log, daemon=True).start()
    shot_times = []
    handed_over = False
    while process.poll() is None and time.monotonic() - start < args.timeout:
        idle_at = state['idle_at']
        # Recordings and replays start on the same fixed trigger, 1 s after the shrine cutscene
        # skip hands over control (Agro has not started wandering yet), not on the settled-camera
        # detection, which can take minutes.
        if (args.record or args.replay) and state['gameplay_at'] is not None:
            idle_at = state['gameplay_at'] + 1
            if time.monotonic() - start < idle_at:
                idle_at = None
            else:
                state['idle_at'] = idle_at
        if idle_at is not None and args.record:
            if not handed_over:
                handed_over = True
                # Recording starts now, where a replay will start (pad file token "record").
                pad.write_text('record')
                print('READY: recording has started. Play now; press F9 to stop, then close the window.',
                      flush=True)
            time.sleep(0.5)
            continue
        if idle_at is not None and args.replay and not state.get('replaying'):
            state['replaying'] = True
            state['replay_at'] = time.monotonic() - start
            pad.write_text('replay')
        if args.replay and args.assist and state.get('replaying'):
            a_start, a_end, a_button = args.assist.split(':')
            t = time.monotonic() - start - state['replay_at']
            if float(a_start) <= t <= float(a_end):
                # The token "replay" stays in the file: it only (re)arms a replay that is not running.
                down = int((t - float(a_start)) / 0.2) % 2 == 0
                want = f'replay {a_button}' if down else 'replay'
                if state.get('assist_state') != want:
                    pad.write_text(want)
                    state['assist_state'] = want
            elif t > float(a_end) and state.get('assist_state') not in (None, 'done'):
                pad.write_text('replay')
                state['assist_state'] = 'done'

        if idle_at is not None:
            if not shot_times:
                shot_times = [idle_at + (i + 1) * args.measure / (args.shots + 1) for i in range(args.shots)]
            now = time.monotonic() - start
            if shot_times and now >= shot_times[0]:
                (out / 'dump.trigger').touch()
                shot_times.pop(0)
                if not shot_times:
                    shot_times = [float('inf')]
            if now >= idle_at + 5 + args.measure:
                break
        time.sleep(0.1)
    state['stop'] = True
    if process.poll() is None:
        process.kill()
        process.wait()
    log.close()
    raws = sorted((out / 'dump').glob('output_*.raw'))
    if raws:
        subprocess.run([sys.executable, str(ROOT / 'tools' / 'raw2png.py'), *map(str, raws)], stdout=subprocess.DEVNULL)
    # The trigger saves PNG screenshots in the user screenshots folder (as tools/sotc_play.py).
    shots = sorted(p for d in (ROOT / 'user' / 'screenshots', out / 'user' / 'screenshots') if d.is_dir()
                   for p in d.glob('*.png') if p.stat().st_mtime >= started)
    for i, shot in enumerate(shots):
        shutil.copy(shot, out / 'dump' / f'output_{i:03d}_shot.png')
    pngs = sorted((out / 'dump').glob('output_*.png'))
    if pngs:
        try:
            from PIL import Image
            images = [Image.open(p).convert('RGB').resize((640, 360)) for p in pngs]
            sheet = Image.new('RGB', (1280, 360 * ((len(images) + 1) // 2)))
            for i, image in enumerate(images):
                sheet.paste(image, ((i % 2) * 640, (i // 2) * 360))
            sheet.save(out / 'dump' / 'sheet.png')
        except ImportError:
            pass
    # Final pass over the whole log (the follower may have missed the last lines).
    text = Path(args.log).read_text(encoding='utf-8', errors='replace')
    print(' '.join(f'{m.group(1)}@{m.group(2)}' for m in FPS_LINE.finditer(text)))
    idle_at = state['idle_at']
    if idle_at is None:
        crashed = re.search(r'^(Fault|STOP:).*$|Assertion Failed', text, re.M)
        print('INVALID: idle scene never reached' + (f' ({crashed.group(0)[:80]})' if crashed else '') +
              f', rc={process.returncode}')
        return 1
    if args.record:
        recorded = Path(args.record)
        count = len(recorded.read_text().splitlines()) if recorded.exists() else 0
        print(f'recorded {count} pad samples to {args.record}' if count else 'nothing recorded (F9 not pressed?)')
        return 0 if count else 1
    window = [s for s in samples if idle_at + 5 <= s[2] <= idle_at + 5 + args.measure + 1]
    if not window:
        print('INVALID: no samples in the measured window')
        return 1
    fps = [s[0] for s in window]
    cmds = sum(s[1] for s in window) / len(window)
    # The idle scene seen in every good run draws ~5900 commands per frame.
    tag = '' if args.replay or IDLE_LOW <= cmds <= IDLE_HIGH else 'INVALID (unexpected scene): '
    print(f'{tag}avg {sum(fps) / len(fps):.1f} FPS, min {min(fps):.1f} '
          f'({len(fps)} samples over {args.measure:.0f} s after idle at {idle_at:.0f} s, {cmds:.0f} cmds/frame)')
    return 0 if not tag else 1


if __name__ == '__main__':
    sys.exit(main())
