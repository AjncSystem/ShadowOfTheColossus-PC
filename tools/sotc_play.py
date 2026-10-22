#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""sotc: scripted run of Shadow of the Colossus.

Restores out/save-template (unless --keep-save), starts out/bb-probe.exe and plays a schedule:

    python tools/sotc_play.py 120 25:options 27:up 28.5:cross 50:shot 60:ly=0:3000

An event is seconds:what[:hold_ms]. "what" is pad tokens joined by ',' (BB_PAD_FILE names of
src/runtime_pad.c: cross, circle, options, up, lx=0..255, ...), "shot" (dump the presented frame
to out/dump, converted to PNG at the end), "menu" (opens or closes the in-game menu), "tex=off|high|max", "fps=30|60|unlimited", "up=fsr1|bilinear", "rcas=high|medium|low|off" (quick settings, as the in-game menu sets them), "stacks" (thread stacks to the log) or "log:text" (a marker in the output). Times count
from the "Starting main loop" line when --from-main-loop is given, else from the start.
"""
import argparse
import os
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = os.environ.get('SOTC_GAME_DIR', str(Path(__file__).resolve().parents[2] / 'CUSA08809'))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('total', type=float)
    parser.add_argument('events', nargs='*')
    parser.add_argument('--keep-save', action='store_true')
    parser.add_argument('--fresh', action='store_true', help='start without any save data')
    parser.add_argument('--from-main-loop', action='store_true')
    parser.add_argument('--log', default='out/run.txt')
    parser.add_argument('--env', action='append', default=[], help='NAME=VALUE for the game')
    parser.add_argument('--patches', help='BBPATCH2 file (scripts/sotc_modes.py)')
    parser.add_argument('--cmdline', help='SOTCCMD1 file (scripts/sotc_modes.py)')
    args = parser.parse_intermixed_args()  # events may follow options (the bench scripts' --env)
    os.chdir(ROOT)
    out = Path('out')
    template = out / 'save-template' / 'CUSA08809'
    if args.fresh:
        shutil.rmtree(out / 'user' / 'savedata', ignore_errors=True)
    elif template.is_dir() and not args.keep_save:
        target = out / 'user' / 'savedata' / '1' / 'CUSA08809'
        shutil.rmtree(target, ignore_errors=True)
        shutil.copytree(template, target)
    shutil.rmtree(out / 'dump', ignore_errors=True)
    (out / 'dump').mkdir(parents=True)
    for stale in ('set.trigger', 'menu.trigger', 'dump.trigger'):  # left by a run that ended first
        (out / stale).unlink(missing_ok=True)
    pad = out / 'pad.txt'
    pad.write_text('')
    env = dict(os.environ)
    msys = os.environ.get('MSYS2_ROOT') or next((p for p in (r'C:\msys64', r'I:\msys64') if Path(p).is_dir()),
                                                r'C:\msys64')
    env['PATH'] = str(Path(msys) / 'clang64' / 'bin') + os.pathsep + env['PATH']
    env.update(BB_FRAME_DUMP_TRIGGER=str(out / 'dump.trigger'), BB_DUMP_DIR=str(out / 'dump'), BB_PAD_FILE=str(pad),
               BB_STACKS_TRIGGER=str(out / 'stacks.trigger'), SOTC_MENU_TRIGGER=str(out / 'menu.trigger'),
               SOTC_SET_TRIGGER=str(out / 'set.trigger'),
               SOTC_SAVE_PROMPT='auto')
    for item in args.env:
        name, value = item.split('=', 1)
        env[name] = value
    command = [str(out / 'bb-probe.exe'), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
               '--sotc-cpu', str(out / 'sotc-cpu.bin'), '--app0', GAME, '--user', str(out / 'user')]
    if args.patches:
        command += ['--patches', args.patches]
    if args.cmdline:
        command += ['--sotc-cmdline', args.cmdline]
    # SOTC_PLAY_WRAP: a launcher in front of the game, a JSON list (renderdoccmd capture -w -c file); the
    # event "rdoc" then captures the next GPU submission (SOTC_RDOC_TRIGGER, gpu/shim/bbgpu.cpp).
    if os.environ.get('SOTC_PLAY_WRAP'):
        import json
        command = json.loads(os.environ['SOTC_PLAY_WRAP']) + command
        env['SOTC_RDOC_TRIGGER'] = str(out / 'rdoc.trigger')
    log = open(args.log, 'wb')
    process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    start = time.monotonic()
    started = time.time()
    if args.from_main_loop:
        while process.poll() is None and time.monotonic() - start < args.total:
            if b'Starting main loop' in Path(args.log).read_bytes():
                break
            time.sleep(0.1)
        start = time.monotonic()
        print(f'main loop after {start - (start - 0):.0f}s; schedule starts now', flush=True)

    def schedule():
        for event in args.events:
            parts = event.split(':')
            at, what = float(parts[0]), parts[1]
            hold = float(parts[2]) / 1000 if len(parts) > 2 and what != 'log' else 0.25
            while process.poll() is None and time.monotonic() - start < at:
                time.sleep(0.02)
            if process.poll() is not None:
                return
            if what == 'shot':
                (out / 'dump.trigger').touch()
            elif what == 'rdoc':
                (out / 'rdoc.trigger').touch()
            elif what.split('=')[0] in ('tex', 'fps', 'up', 'rcas'):
                (out / 'set.trigger').write_text(what)
            elif what == 'menu':
                (out / 'menu.trigger').touch()
            elif what == 'stacks':
                (out / 'stacks.trigger').touch()
            elif what == 'log':
                print(f'[{at:6.1f}s] {":".join(parts[2:])}', flush=True)
            else:
                pad.write_text(what.replace(',', ' '))
                time.sleep(hold)
                pad.write_text('')
    threading.Thread(target=schedule, daemon=True).start()
    try:
        process.wait(timeout=args.total)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
    log.close()
    print(f'rc={process.returncode} after {time.monotonic() - start:.0f}s')
    raws = sorted((out / 'dump').glob('output_*.raw'))
    if raws:
        subprocess.run([sys.executable, str(ROOT / 'tools' / 'raw2png.py'), *map(str, raws)], stdout=subprocess.DEVNULL)
    # Presenters without the raw dump save PNG screenshots (user/screenshots) instead.
    shots = sorted(p for p in (ROOT / 'user' / 'screenshots').glob('*.png') if p.stat().st_mtime >= started)
    for i, shot in enumerate(shots):
        shutil.copy(shot, out / 'dump' / f'output_{i:03d}_shot.png')
    pngs = sorted((out / 'dump').glob('output_*.png'))
    if pngs:
        from PIL import Image
        images = [Image.open(p).convert('RGB').resize((640, 360)) for p in pngs]
        sheet = Image.new('RGB', (1280, 360 * ((len(images) + 1) // 2)))
        for i, image in enumerate(images):
            sheet.paste(image, ((i % 2) * 640, (i // 2) * 360))
        sheet.save(out / 'dump' / 'sheet.png')
        print(f'{len(pngs)} frames -> out/dump/sheet.png')


if __name__ == '__main__':
    main()
