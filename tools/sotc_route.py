#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""sotc: record a route in an installed port and replay it (src/runtime_pad.c BB_PAD_RECORD /
BB_PAD_REPLAY), from the same save and the same moment every run.

    python tools/sotc_route.py --install DIR --record out/routes/walk.txt
    python tools/sotc_route.py --install DIR --replay out/routes/walk.txt [--env NAME=VALUE ...]
    python tools/sotc_route.py --install DIR --measure 60 --boss 6

--record copies the install's save next to the route (walk.save/), starts the game through the
launcher (--play), gets past the title by itself and starts recording 1 s after gameplay begins
(the second "st_gameflow() starting game" in the log). Play; F9 or closing the window ends it.
--replay puts that save back (the install's own save is restored afterwards), starts the replay at
the same moment and closes the game when it has finished. The run's log is user/last_run.log;
SOTC_FPS, SOTC_FRAME_LOG and SOTC_STALL_MS=40 are always set (tools/spike_report.py reads them).

--boss N (1-16) jumps into that boss's encounter first, through the engine's own automation
manager and debug command (shadow_jumpToBoss: scripted arena spawn, boss active, intro cutscene),
and brings Agro along; recording, replaying or measuring starts after the jump finishes. --kill
kills the boss shortly after the jump, so the death-and-collapse cinematic plays (and gets
measured). --outro skips the fight entirely: it marks the boss defeated and plays the aftermath
(wake up at the Shrine). Both mutate the save's progress. --measure SECONDS runs no pad route: it just sits in the
encounter that long (the colossus comes to you) and leaves the frame log for spike_report.py /
the bench skill - launch-into-boss performance analysis with no hands. --measure uses the
install's current save as it is (the jump makes the spawn deterministic regardless of progress).
--play hands the game over once the jump is done: nothing is killed or closed, so you can fight
the boss yourself (killBoss only ever runs when --kill is given).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

GAME_START = 'st_gameflow() starting game'
AUTOMATION_DONE = 'Automation completed'
AUTOMATION_FAIL = 'Command in root stage failed'
# The jump runs inside a *stage so one failing command cannot abort the plan (root-stage
# failures do). waitForGameToStart holds it until the save is loaded; the jump then does its
# own two gameflow transitions.
BOSS_PLAN = '''*stage "JumpToBoss"
waitForGameToStart
wait 8
{command} {index}
wait 6
waitForAreaLoader
wait 4
printPlayerPos
*endstage
*stage "Horse"
shadow_horseDebugWarpToPlayer
wait 2
*endstage
'''
# The wait lets the encounter's intro cutscene finish before the kill (longer with --intro,
# where nothing skips it).
KILL_STAGE = '''*stage "Kill"
wait {wait}
killBoss
wait 2
*endstage
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--install', required=True, help='the port folder (launcher/, user/)')
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--record')
    mode.add_argument('--replay')
    mode.add_argument('--measure', type=float, help='just sit in the game that many seconds')
    mode.add_argument('--play', action='store_true',
                      help='leave the game running for you to play (the boss stays alive)')
    parser.add_argument('--boss', type=int, choices=range(1, 17), metavar='1-16',
                        help='jump into this boss encounter first')
    parser.add_argument('--outro', action='store_true',
                        help='with --boss: skip the fight, mark it defeated and play the aftermath')
    parser.add_argument('--kill', '--killboss', action='store_true',
                        help='with --boss: kill the boss after the jump (death cinematic plays); '
                             'without it the boss stays alive')
    parser.add_argument('--intro', action='store_true',
                        help="with --boss: let the encounter's intro cutscene play instead of skipping it")
    parser.add_argument('--env', action='append', default=[])
    parser.add_argument('--timeout', type=float, default=3600)
    args = parser.parse_args()
    if (args.outro or args.kill) and not args.boss:
        print('INVALID: --outro/--kill need --boss')
        return 1
    if args.outro and args.kill:
        print('INVALID: --outro and --kill are different endings, pick one')
        return 1
    install = Path(args.install)
    user = install / 'user'
    saves = user / 'savedata' / '1' / 'CUSA08809'
    if subprocess.run(['tasklist', '/FI', 'IMAGENAME eq bb-probe.exe'], capture_output=True, text=True).stdout.count('bb-probe.exe'):
        print('INVALID: the game is running')
        return 1
    restore = None
    plan = None
    if args.record or args.replay:
        route = Path(args.record or args.replay).resolve()
        snapshot = route.with_suffix('.save')
        if args.record:
            route.parent.mkdir(parents=True, exist_ok=True)
            shutil.rmtree(snapshot, ignore_errors=True)
            shutil.copytree(saves, snapshot)
        else:
            if not route.is_file() or not snapshot.is_dir():
                print(f'INVALID: need {route} and {snapshot}')
                return 1
            restore = user / 'savedata' / '1' / f'CUSA08809.route-backup-{time.strftime("%Y%m%d-%H%M%S")}'
            shutil.copytree(saves, restore)
            shutil.rmtree(saves)
            shutil.copytree(snapshot, saves)
    pad = user / 'route-pad.txt'
    # Input present at the first frame makes the engine auto-detect '+nointro' and skip the
    # intro movies; the driver replaces this with its own holds once the pad is open.
    pad.write_text('cross')
    env = dict(os.environ, BB_PAD_FILE=str(pad), SOTC_FPS='1', SOTC_FRAME_LOG='1', SOTC_STALL_MS='40',
               SOTC_SAVE_PROMPT='auto')
    if args.record or args.replay:
        env['BB_PAD_RECORD' if args.record else 'BB_PAD_REPLAY'] = str(route)
    if args.boss:
        # The plan file must sit in the game dump (the engine loads it from app0).
        game_dir = Path(json.loads((user / 'launcher_settings.json').read_text(encoding='utf-8'))['game_dir'])
        # No hyphen in the name: the boot cvar parser rejects it (usage error on automate_loadfile).
        plan = game_dir / 'route_bossjump.txt'
        plan_text = BOSS_PLAN.format(command='boss_outro' if args.outro else 'shadow_jumpToBoss',
                                     index=args.boss - 1)
        if args.boss in (7, 12):
            # Water arenas (Eel, Poseidon): no place for the horse, and the warp there crashed.
            plan_text = plan_text[:plan_text.index('*stage "Horse"')]
        if args.kill:
            plan_text += KILL_STAGE.format(wait=110 if args.intro else 40)
        plan.write_text(plan_text)
        env['SOTC_EXTRA_CVARS'] = (env.get('SOTC_EXTRA_CVARS', '') +
                                   ' nointro RetailBuild=0 automate_loadfile(route_bossjump.txt) automate_go').strip()
    for item in args.env:
        name, value = item.split('=', 1)
        env[name] = value
    log = user / 'last_run.log'
    launched = time.time()
    launcher = subprocess.Popen([sys.executable, str(install / 'launcher' / 'sotc_launcher_win.py'), '--play'],
                                cwd=install, env=env)
    try:
        return drive(args, pad, log, launched, launcher)
    finally:
        if plan:
            plan.unlink(missing_ok=True)
        if restore:
            # the game may still be writing the save: wait for it to exit first
            for _ in range(60):
                if not running():
                    break
                time.sleep(1)
            shutil.rmtree(saves, ignore_errors=True)
            shutil.copytree(restore, saves)
            shutil.rmtree(restore)
            print('your save is restored')


def running():
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq bb-probe.exe'], capture_output=True, text=True).stdout
    return 'bb-probe.exe' in out


def drive(args, pad, log, launched, launcher):
    start = time.monotonic()

    def text():
        try:
            if log.stat().st_mtime < launched - 1:
                return ''
            return log.read_text(encoding='utf-8', errors='replace')
        except OSError:
            return ''

    def wait(seconds, until=lambda t: False):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            t = text()
            if until(t):
                return t
            if time.monotonic() - start > 20 and not running():
                return None
            time.sleep(0.2)
        return text()

    def starts(t):
        return t.count(GAME_START) if t else 0

    def invalid(why):
        # Keep the failed run's log too: the next launch overwrites last_run.log.
        subprocess.run(['taskkill', '/F', '/IM', 'bb-probe.exe'], capture_output=True)
        try:
            keep = log.with_name(f'invalid-{time.strftime("%Y%m%d-%H%M%S")}.log')
            shutil.copy2(log, keep)
            print(f'INVALID: {why} (log {keep})')
        except OSError:
            print(f'INVALID: {why}')
        return 1

    def hold(button, seconds, until):
        pad.write_text(button)
        t = wait(seconds, until)
        pad.write_text('')
        return t

    t = wait(120, lambda t: 'pad opened' in t)
    if not t:
        return invalid('the game did not start')
    # Opening cinematic ("hold X to skip"), title, menu (Continue is selected): hold and tap X
    # until the save starts loading.
    t = wait(8)
    while t is not None and starts(t) < 1 and time.monotonic() - start < 240:
        t = hold('cross', 3, lambda t: starts(t) >= 1)
        if t is not None and starts(t) < 1:
            t = wait(1.5, lambda t: starts(t) >= 1)
    if t is None or starts(t) < 1:
        return invalid('the save did not load')
    # Gameplay begins at the second start; a cutscene before it is skipped with X held.
    t = wait(25, lambda t: starts(t) >= 2)
    while t is not None and starts(t) < 2 and time.monotonic() - start < 360:
        t = hold('cross', 5, lambda t: starts(t) >= 2)
        if t is not None and starts(t) < 2:
            t = wait(2, lambda t: starts(t) >= 2)
    if t is None or starts(t) < 2:
        return invalid('gameplay did not begin')
    # On a fast boot the menu can pass before any hold() ran, leaving the launch-time 'cross'
    # (the +nointro trick) held forever - which would skip every cutscene from here on.
    pad.write_text('')
    wait(1)
    if args.boss:
        # The jump restarts the gameflow twice more and prints the position when it has landed.
        t = wait(240, lambda t: 'Player Position:' in t or AUTOMATION_FAIL in t)
        if t is None or (t and AUTOMATION_FAIL in t):
            return invalid('the boss jump did not land')
        if not args.intro:
            # The encounter's intro cutscene is hold-X skippable; in gameplay the holds just hop.
            for _ in range(3):
                if hold('cross', 3, lambda t: AUTOMATION_DONE in t) is None:
                    break
        t = wait(240, lambda t: AUTOMATION_DONE in t or AUTOMATION_FAIL in t)
        if t is None or AUTOMATION_DONE not in t:
            return invalid('the boss jump did not finish')
        print(f'in boss {args.boss} encounter', flush=True)
    if args.play:
        # Hand the game over: no timer, no kill, the boss is yours until you close the window.
        print('READY: the game is yours', flush=True)
        return 0
    if args.measure is not None:
        print(f'measuring {args.measure:g}s', flush=True)
        if args.kill:
            # The aftermath reloads the gameflow once the collapse cinematic has played out:
            # stop there (the boss is dead) instead of sitting out the clock.
            t = wait(args.measure, lambda t: starts(t) >= 3)
            if t is not None and starts(t) >= 3:
                print('boss is down', flush=True)
                wait(3)
        else:
            wait(args.measure)
        subprocess.run(['taskkill', '/F', '/IM', 'bb-probe.exe'], capture_output=True)
        launcher.wait(30)
        # The next launch overwrites last_run.log: keep this run's copy for spike_report.py.
        keep = log.with_name(f'measure-{time.strftime("%Y%m%d-%H%M%S")}.log')
        shutil.copy2(log, keep)
        print(f'measure done, log {keep}')
        return 0
    if args.record:
        pad.write_text('record')
        print('READY: recording. Play the route; F9 stops it, or close the window.', flush=True)
        while running() and time.monotonic() - start < args.timeout:
            time.sleep(1)
        lines = len(Path(args.record).read_text().splitlines()) if Path(args.record).is_file() else 0
        print(f'recorded {lines} pad changes to {args.record}')
        return 0 if lines else 1
    pad.write_text('replay')
    print('replaying', flush=True)
    t = wait(args.timeout, lambda t: 'pad replay finished' in t)
    if t is None:
        print('INVALID: the game ended during the replay')
        return 1
    wait(3)
    subprocess.run(['taskkill', '/F', '/IM', 'bb-probe.exe'], capture_output=True)
    launcher.wait(30)
    print(f'replay done, log {log}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
