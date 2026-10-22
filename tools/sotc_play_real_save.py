#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""sotc: boots straight into the player's real, already-placed save (out/user/savedata/1/CUSA08809,
left untouched - no template copy) instead of a fresh/template one. Skips the opening cinematic and
the title/Continue prompt the same way tools/sotc_bench_shrine.py does (driven by the log, not fixed
times), then leaves input idle so SOTC_PROFILE/SOTC_FPS measure real in-game play, not a cutscene or
menu. Prints every "SOTC FPS ... at Ns" line seen and whether real gameplay was reached.

    python tools/sotc_play_real_save.py <total seconds> [--env NAME=VALUE ...]
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
FPS_LINE = re.compile(r'SOTC FPS ([\d.]+) at (\d+)s; (\d+) cmds/frame')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('total', type=float)
    parser.add_argument('--log', default='out/run_real_save.txt')
    parser.add_argument('--env', action='append', default=[])
    args = parser.parse_args()
    os.chdir(ROOT)
    out = Path('out')
    save = out / 'user' / 'savedata' / '1' / 'CUSA08809'
    if not save.is_dir():
        print(f'INVALID: no save at {save}')
        return 1
    shutil.rmtree(out / 'dump', ignore_errors=True)
    (out / 'dump').mkdir(parents=True)
    pad = out / 'pad.txt'
    pad.write_text('')
    env = dict(os.environ)
    msys = os.environ.get('MSYS2_ROOT', r'C:\msys64')
    env['PATH'] = str(Path(msys) / 'clang64' / 'bin') + os.pathsep + env['PATH']
    env.update(BB_PAD_FILE=str(pad), SOTC_FPS='1', SOTC_SAVE_PROMPT='auto')
    for item in args.env:
        name, value = item.split('=', 1)
        env[name] = value
    # sotc_play_real_save: this MSYS2-built python's Path objects stringify with forward slashes
    # even natively on Windows, which _winapi.CreateProcess can fail to resolve for the exe name.
    command = [str((out / 'bb-probe.exe').resolve()).replace('/', '\\'), str(out / 'boot-linked.bin'),
               '--content-profile', str(out / 'content.bin'), '--sotc-cpu', str(out / 'sotc-cpu.bin'),
               '--app0', GAME, '--user', str(out / 'user')]
    log = open(args.log, 'wb')
    process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    start = time.monotonic()
    state = {'stop': False, 'game_starts': 0, 'gameplay_at': None}
    samples = []

    def press(what, hold=0.25):
        pad.write_text(what)
        time.sleep(hold)
        pad.write_text('')

    def wait_until(t):
        while process.poll() is None and not state['stop'] and time.monotonic() - start < t:
            time.sleep(0.05)
        return process.poll() is None and not state['stop']

    def route():
        # Adaptive, not fixed-time: a cold shader cache can make the opening cinematic and its
        # "Hold X to skip" prompt arrive much later than on a fast/warm-cache machine. Tap/hold
        # Cross repeatedly from early on - harmless if nothing is listening yet - until the save
        # starts loading ("st_gameflow() starting game" in the log), however long that takes.
        at = 5.0
        while state['game_starts'] < 1:
            if not wait_until(at):
                return
            if state['game_starts'] >= 1:
                break
            press('cross', 1.5)
            at += 2.5
        state['gameplay_at'] = time.monotonic() - start
        print(f'[route] save loading started at {state["gameplay_at"]:.0f}s; leaving input idle from here', flush=True)

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

    threading.Thread(target=route, daemon=True).start()
    threading.Thread(target=follow_log, daemon=True).start()
    try:
        process.wait(timeout=args.total)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
    state['stop'] = True
    log.close()
    text = Path(args.log).read_text(encoding='utf-8', errors='replace')
    print(' '.join(f'{m.group(1)}@{m.group(2)}s/{m.group(3)}cmd' for m in FPS_LINE.finditer(text)))
    if state['gameplay_at'] is None:
        print('INVALID: never got past the title/Continue prompt')
        return 1
    print(f'gameplay (save load) started at {state["gameplay_at"]:.0f}s')
    return 0


if __name__ == '__main__':
    sys.exit(main())
