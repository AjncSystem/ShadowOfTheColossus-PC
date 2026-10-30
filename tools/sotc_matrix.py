#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""sotc: SotC Lab matrix runner. Measures a set of configs (engine switches) over scenarios (a route
recorded with the lab's path recorder, or a colossus boss fight) and writes the frame statistics.

    python tools/sotc_matrix.py --matrix tools/matrices/example.json [--out DIR] [--dry-run]
    python tools/sotc_matrix.py --matrix FILE --attach      (started by the game's F2 menu)

Matrix file (JSON):
    {"scenarios": [{"name": "route", "route": "tools/routes/x.json", "lap_s": 120},
                   {"name": "boss1", "boss": 1, "measure_s": 60}],
     "laps": 3, "settle_s": 10, "base_env": {"SOTC_MODES": "af16"},
     "configs": [{"name": "baseline", "env": {}}, {"name": "x", "env": {"SOTC_FOO": "1"}}]}
Optional top-level "trace": 1 or 2 turns SOTC_TRACE on for every launch (restart-only), with
SOTC_TRACE_DIR=<out>/traces/launch-NNN; every lap also sends "trace mark <config>-<scenario>-lapN".
"trace_funcs": a file for SOTC_TRACE_FUNCS. summary.md then compares each config's dumps with the
first config's through tools/trace/sotc_trace.py diff.

Switches are split with launcher/sotc_tuning.is_live: a live switch is sent to the running game
with "set NAME=value" (a switch that is not set by a config is reset to empty first); a restart-only
switch goes into the launch environment, so the configs are grouped by their restart-only part and
each group is one launch of sotc.py (SOTC_LAB=1). A boss scenario launches with SOTC_BOOT_BOSS=N.

Per launch, every (config, scenario) item runs: a route is loaded and played lap by lap ("path load",
"path play", "mark", and one lap ends when the autopilot stops); a boss scenario sends "boss N" and
measures a fixed window per lap. Every window is read with "get state" at its end. A launch without
frame progress for HANG_S seconds is killed and recorded as a hang; the rest of its items run in a
new launch. A game that asks for a relaunch ("relaunch" in "get state") is relaunched, when attached.

Output (out/matrix/<timestamp>/): results.csv (one row per lap and segment), summary.md (medians over
the laps, deltas against the first config), run.log (the replies that were not "ok"), launch-N.txt.
"""
import argparse
import csv
import json
import os
import queue
import re
import statistics
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HANG_S = 30.0            # no frame progress for this long: the launch is a hang
HEARTBEAT_S = 3.0        # "attach" is sent this often (the game shows the orchestrator as attached)
CONNECT_S = 180.0        # wait for the control pipe after a launch
SETTLE_DEFAULT_S = 10.0  # after the live switches of a config are set
LAPS_DEFAULT = 3
LAP_S_DEFAULT = 120.0    # a route lap without autopilot information
LAP_MAX_S = 900.0        # a route lap never runs longer than this
MEASURE_S_DEFAULT = 60.0 # a boss window
BOSS_WAIT_S = 180.0      # the boss boot: wait for the area to be known before the fight
LAUNCH_LIMIT_S = 3600.0  # per launch, whatever the items
POLL_S = 1.0
STATS = ('frames', 'p50', 'p99', 'max', 'hitch33', 'hitch50', 'hitch100')
DELTA_STATS = ('p50', 'p99', 'max', 'hitch50', 'hitch100')
RESULT_FIELDS = ['scenario', 'config', 'lap', 'segment', *STATS]
NAME = re.compile(r'[A-Za-z0-9_.-]+')


class Abort(Exception):
    """A launch cannot go on: the current item is recorded as `kind`."""
    kind = 'abort'


class Hang(Abort):
    kind = 'hang'


class Crashed(Abort):
    kind = 'crash'


class Relaunch(Abort):
    """The game asked for a relaunch (attached mode); boss is the number it asked for, 0 for none."""
    kind = 'relaunch'

    def __init__(self, boss):
        super().__init__(f'relaunch requested (boss {boss})')
        self.boss = boss


# --- matrix ----------------------------------------------------------------------------------------

def format_value(value):
    if isinstance(value, bool):
        return '1' if value else '0'
    return str(value)


def normalize_matrix(data):
    """The matrix with its defaults filled in and every value checked; ValueError when it is bad."""
    if not isinstance(data, dict):
        raise ValueError('the matrix must be a JSON object')
    scenarios, names = [], set()
    for raw in data.get('scenarios') or []:
        name = str(raw.get('name') or '')
        if not NAME.fullmatch(name):
            raise ValueError(f'bad scenario name {name!r} (letters, digits, _ . -)')
        if name in names:
            raise ValueError(f'duplicate scenario {name!r}')
        names.add(name)
        if ('route' in raw) == ('boss' in raw):
            raise ValueError(f'scenario {name}: give either "route" or "boss"')
        scenario = {'name': name, 'route': None, 'boss': 0,
                    'lap_s': float(raw.get('lap_s', LAP_S_DEFAULT)),
                    'measure_s': float(raw.get('measure_s', MEASURE_S_DEFAULT))}
        if 'route' in raw:
            scenario['route'] = str(raw['route'])
        else:
            scenario['boss'] = int(raw['boss'])
            if not 1 <= scenario['boss'] <= 16:
                raise ValueError(f'scenario {name}: boss must be 1..16')
        scenarios.append(scenario)
    if not scenarios:
        raise ValueError('the matrix has no scenarios')
    configs = []
    for raw in data.get('configs') or [{'name': 'baseline', 'env': {}}]:
        name = str(raw.get('name') or '')
        if not NAME.fullmatch(name):
            raise ValueError(f'bad config name {name!r} (letters, digits, _ . -)')
        if any(c['name'] == name for c in configs):
            raise ValueError(f'duplicate config {name!r}')
        env = raw.get('env') or {}
        if not isinstance(env, dict):
            raise ValueError(f'config {name}: env must be an object')
        configs.append({'name': name, 'env': {str(k): format_value(v) for k, v in env.items()}})
    laps = int(data.get('laps', LAPS_DEFAULT))
    if laps < 1:
        raise ValueError('laps must be at least 1')
    base_env = {str(k): format_value(v) for k, v in (data.get('base_env') or {}).items()}
    trace = data.get('trace', 0)
    if isinstance(trace, bool) or trace not in (0, 1, 2):
        raise ValueError('trace must be 0, 1 or 2')
    trace_funcs = str(data.get('trace_funcs') or '')
    if trace_funcs:
        path = Path(trace_funcs)
        trace_funcs = str(path if path.is_absolute() else (ROOT / path).resolve())
        base_env['SOTC_TRACE_FUNCS'] = trace_funcs
    if trace:
        base_env['SOTC_TRACE'] = str(int(trace))  # restart-only: SOTC_TRACE is not in LIVE
    return {'scenarios': scenarios, 'configs': configs, 'laps': laps,
            'settle_s': float(data.get('settle_s', SETTLE_DEFAULT_S)), 'base_env': base_env,
            'launch_limit_s': float(data.get('launch_limit_s', LAUNCH_LIMIT_S)),
            'trace': int(trace), 'trace_funcs': trace_funcs}


def load_matrix(path):
    return normalize_matrix(json.loads(Path(path).read_text(encoding='utf-8')))


def load_is_live():
    """launcher/sotc_tuning.is_live(name): True for a switch that can be set on the running game.
    Without it, the names in its LIVE table (empty when absent) are the live ones."""
    launcher = str(ROOT / 'launcher')
    if launcher not in sys.path:
        sys.path.insert(0, launcher)
    try:
        import sotc_tuning
    except Exception as error:  # the launcher module is not loadable: every switch is restart-only
        print(f'sotc_matrix: launcher/sotc_tuning not loaded ({error}); all switches are restart-only',
              file=sys.stderr)
        return lambda name: False
    function = getattr(sotc_tuning, 'is_live', None)
    if callable(function):
        return lambda name: bool(function(name))
    live = getattr(sotc_tuning, 'LIVE', {})
    return lambda name: name in live


def split_env(env, is_live):
    """(live, restart-only) parts of an environment mapping."""
    live = {k: v for k, v in env.items() if is_live(k)}
    restart = {k: v for k, v in env.items() if not is_live(k)}
    return live, restart


def plan_matrix(matrix, is_live):
    """{'groups': [...]}: configs grouped by their restart-only environment (one group per distinct
    part). Each group has 'restart' (its environment), 'configs' ([{name, live}] in matrix order)
    and 'launches' ([{boss, items}]: items are (config, scenario) names; route scenarios run on a
    launch without a boss, each boss number on its own launch)."""
    groups, index = [], {}
    for config in matrix['configs']:
        full = {**matrix['base_env'], **config['env']}
        live, restart = split_env(full, is_live)
        key = json.dumps(restart, sort_keys=True)
        if key not in index:
            index[key] = len(groups)
            groups.append({'restart': restart, 'configs': []})
        groups[index[key]]['configs'].append({'name': config['name'], 'live': live})
    bosses = []
    for scenario in matrix['scenarios']:
        if scenario['boss'] not in bosses:
            bosses.append(scenario['boss'])
    for group in groups:
        group['launches'] = []
        for boss in bosses:
            items = [(c['name'], s['name']) for c in group['configs'] for s in matrix['scenarios']
                     if s['boss'] == boss]
            if items:
                group['launches'].append({'boss': boss, 'items': items})
    return {'groups': groups}


def set_commands(live, previous=()):
    """The "set" lines for a config's live part. A name the previous config set and this one does
    not is reset to empty first."""
    commands = [f'set {name}=' for name in sorted(set(previous) - set(live))]
    commands += [f'set {name}={value}' for name, value in live.items()]
    return commands


def launch_env(group, boss, trace_dir=None):
    """The environment of a launch of sotc.py for a group (its restart-only part). trace_dir is the
    folder of this launch's trace dumps (SOTC_TRACE_DIR), when the matrix turns the trace on."""
    env = dict(group['restart'])
    env['SOTC_LAB'] = '1'
    if boss:
        env['SOTC_BOOT_BOSS'] = str(boss)
    if trace_dir is not None:
        env['SOTC_TRACE_DIR'] = str(trace_dir)
    return env


def item_steps(matrix, scenario, mark_prefix):
    """The commands one item sends, in order, for the dry run: "mark" names the window, "wait" and
    "window" are the waits and the reads of "get state" between them."""
    steps = []
    if scenario['boss']:
        steps.append('wait until the area is known (boss boot, at most 180 s)')
        for lap in range(1, matrix['laps'] + 1):
            name = f'{mark_prefix}-lap{lap}'
            steps += [f'boss {scenario["boss"]}', f'mark {name}', f'wait {scenario["measure_s"]:g} s',
                      'get state (window)']
    else:
        steps += [f'path load {route_path(scenario)}', 'path play']
        for lap in range(1, matrix['laps'] + 1):
            name = f'{mark_prefix}-lap{lap}'
            if lap > 1:
                steps += ['path seek 0', 'path play']
            steps += [f'mark {name}', f'wait until the autopilot stops ({scenario["lap_s"]:g} s without it)',
                      'get state (window)']
    return steps


def route_path(scenario):
    route = Path(scenario['route'])
    return str(route if route.is_absolute() else (ROOT / route).resolve())


def describe_plan(plan, matrix):
    """The plan as text (the dry run): launches, set lines and the steps of each item."""
    lines = [f'{len(plan["groups"])} restart-only group(s), {matrix["laps"]} lap(s), '
             f'settle {matrix["settle_s"]:g} s']
    for number, group in enumerate(plan['groups'], 1):
        lines.append(f'group {number}: restart-only {json.dumps(group["restart"], sort_keys=True)}')
        for config in group['configs']:
            lines.append(f'  config {config["name"]}: live {json.dumps(config["live"], sort_keys=True)}')
        for launch in group['launches']:
            env = launch_env(group, launch['boss'])
            lines.append(f'  launch sotc.py boss={launch["boss"] or "none"} env {json.dumps(env, sort_keys=True)}')
            previous_config, previous_live = None, set()
            for config_name, scenario_name in launch['items']:
                config = next(c for c in group['configs'] if c['name'] == config_name)
                scenario = next(s for s in matrix['scenarios'] if s['name'] == scenario_name)
                if config_name != previous_config:
                    for command in set_commands(config['live'], previous_live):
                        lines.append(f'    {command}')
                    lines.append(f'    settle {matrix["settle_s"]:g} s')
                    previous_config, previous_live = config_name, set(config['live'])
                lines.append(f'    item {config_name} / {scenario_name}')
                for step in item_steps(matrix, scenario, f'{config_name}-{scenario_name}'):
                    lines.append(f'      {step}')
    return '\n'.join(lines)


def summarize(rows, config_order=()):
    """Medians over the laps of each (scenario, segment, config), and the change against the first
    config of the same scenario and segment (percent; None for that config itself or a zero base).
    Hang rows and rows without frames are left out. Returns a list of dicts."""
    groups = {}
    for row in rows:
        if row['segment'] not in ('route', 'fight') or not float(row['frames'] or 0):
            continue
        groups.setdefault((row['scenario'], row['segment'], row['config']), []).append(row)
    order = {name: i for i, name in enumerate(config_order)}
    result = []
    for (scenario, segment, config), items in groups.items():
        entry = {'scenario': scenario, 'segment': segment, 'config': config, 'laps': len(items)}
        for stat in STATS:
            entry[stat] = statistics.median(float(item[stat]) for item in items)
        result.append(entry)
    result.sort(key=lambda e: (e['scenario'], e['segment'], order.get(e['config'], len(order))))
    bases = {}
    for entry in result:
        bases.setdefault((entry['scenario'], entry['segment']), entry)
    for entry in result:
        base = bases[(entry['scenario'], entry['segment'])]
        for stat in DELTA_STATS:
            entry[f'd_{stat}'] = None if entry is base or not base[stat] else \
                (entry[stat] - base[stat]) / base[stat] * 100
    return result


def summary_markdown(summary, rows, matrix_name='', trace_text=''):
    lines = [f'# SotC Lab matrix {matrix_name}'.rstrip(), '',
             'Medians over the laps. The deltas are against the first config of the scenario '
             '(percent; positive is more time, for the frame times and hitches).', '']
    segments = []
    for entry in summary:
        key = (entry['scenario'], entry['segment'])
        if key not in segments:
            segments.append(key)
    for scenario, segment in segments:
        lines += [f'## {scenario} ({segment})', '',
                  '| config | laps | frames | p50 ms | p99 ms | max ms | hitch33 | hitch50 | hitch100 | '
                  'd p50 | d p99 | d max | d hitch100 |',
                  '|---|---|---|---|---|---|---|---|---|---|---|---|---|']
        for entry in summary:
            if (entry['scenario'], entry['segment']) != (scenario, segment):
                continue

            def delta(stat):
                value = entry[f'd_{stat}']
                return '-' if value is None else f'{value:+.1f}%'
            lines.append(f'| {entry["config"]} | {entry["laps"]} | {entry["frames"]:.0f} | '
                         f'{entry["p50"]:.2f} | {entry["p99"]:.2f} | {entry["max"]:.1f} | '
                         f'{entry["hitch33"]:.1f} | {entry["hitch50"]:.1f} | {entry["hitch100"]:.1f} | '
                         f'{delta("p50")} | {delta("p99")} | {delta("max")} | {delta("hitch100")} |')
        lines.append('')
    if trace_text:
        lines += [trace_text, '']
    hangs = [r for r in rows if r['segment'] not in ('route', 'fight')]
    lines += ['## Hangs and aborts', '']
    lines += [f'- {r["config"]} / {r["scenario"]} lap {r["lap"]}: {r["segment"]}: {r.get("note", "")}'
              for r in hangs] or ['None.']
    lines.append('')
    return '\n'.join(lines)


# --- running -----------------------------------------------------------------------------------------

TRACE_TOOL = ROOT / 'tools' / 'trace' / 'sotc_trace.py'


def trace_section(out, order, folders):
    """The "where the time went" part of summary.md: the trace folders of each config and, for each
    config after the first, the diff against the first config's folder (tools/trace/sotc_trace.py diff).
    '' when no config has a trace folder; the diff is left out quietly when the tool is missing."""
    if not any(folders.get(name) for name in order):
        return ''

    def rel(path):
        return Path(os.path.relpath(path, out)).as_posix()

    base = order[0]
    base_dir = (folders.get(base) or [None])[0]
    lines = ['## Onde o tempo foi (where the time went)', '',
             f'Trace folders per config; the first config ({base}) is the baseline.', '']
    for name in order:
        mine = folders.get(name) or []
        if not mine:
            lines.append(f'- {name}: no trace folder (the trace is off or the config did not run).')
            continue
        dumps = sum(len(list(folder.glob('*.sotctrace'))) for folder in mine)
        links = ', '.join(f'[{rel(folder)}]({rel(folder)})' for folder in mine)
        lines.append(f'- {name}: {dumps} dump(s) in {links}')
        for folder in mine:
            for report in sorted(folder.glob('*.md')):
                lines.append(f'  - hitch report: [{rel(report)}]({rel(report)})')
    lines += ['', '### Diff against the baseline', '']
    for name in order[1:]:
        mine = folders.get(name) or []
        other = next((folder for folder in mine if folder != base_dir), None)
        if base_dir is None or other is None:
            lines.append(f'- {name}: not compared (its dumps share a launch with {base}, so they mix '
                         'the configs).')
            continue
        if not TRACE_TOOL.is_file():
            continue  # the tool is missing: quietly no diff
        report = out / 'traces' / f'diff-{name}.md'
        try:
            done = subprocess.run([sys.executable, str(TRACE_TOOL), 'diff', str(base_dir), str(other),
                                   '--md', str(report)],
                                  capture_output=True, text=True, timeout=600)
        except (OSError, subprocess.SubprocessError) as error:
            lines.append(f'- {name}: not compared ({error}).')
            continue
        if done.returncode or not report.is_file():
            detail = (done.stderr or '').strip().splitlines()[-1:] or ['no output']
            lines.append(f'- {name}: not compared (sotc_trace.py diff exit {done.returncode}: {detail[0]}).')
            continue
        lines += [f'#### {name} against {base}', '', report.read_text(encoding='utf-8').strip(), '']
    return '\n'.join(lines)


def autopilot_playing(state):
    """True/False from the autopilot part of "get state"; None when the state does not tell."""
    autopilot = (state or {}).get('autopilot')
    if isinstance(autopilot, dict):
        for key in ('playing', 'active', 'running'):
            if key in autopilot:
                return bool(autopilot[key])
    return None


def make_ctl():
    """A connection to the game's control pipe (tools/sotc_ctl.py: class Ctl, send(line) -> reply)."""
    from sotc_ctl import Ctl  # imported here: planning and the tests do not need the pipe
    return Ctl()


def close_ctl(ctl):
    close = getattr(ctl, 'close', None)
    if callable(close):
        try:
            close()
        except Exception:
            pass


def kill_game(process):
    """Ends a launched sotc.py with its bb-probe.exe; without a process (an attached game) every
    bb-probe.exe is ended."""
    if process is None:
        if os.name == 'nt':
            subprocess.run(['taskkill', '/F', '/IM', 'bb-probe.exe'], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        return
    if process.poll() is None:
        if os.name == 'nt':
            subprocess.run(['taskkill', '/F', '/T', '/PID', str(process.pid)], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        else:
            process.kill()
    try:
        process.wait(timeout=20)
    except subprocess.TimeoutExpired:
        pass


class Session:
    """One connection to a game: its commands, the heartbeat, the "get state" polls and the hang
    checks. The pipe takes one client, and a read can block for as long as the game stays silent,
    so every command goes through one sender thread that owns the connection; a command without a
    reply within HANG_S seconds is a hang (the caller kills the game, which closes the pipe)."""

    def __init__(self, ctl, process, attached, launch_limit):
        self.ctl = ctl
        self.process = process
        self.attached = attached
        self.launch_started = time.monotonic()
        self.launch_limit = launch_limit
        self.progress_key = None
        self.progress_at = time.monotonic()
        self.where = {'config': '', 'scenario': '', 'lap': 0}
        self.replies = []  # replies that were not "ok" or a state, and errors (for run.log)
        self.closed = threading.Event()
        self.requests = queue.Queue()
        threading.Thread(target=self._sender, daemon=True).start()
        threading.Thread(target=self._heartbeat, daemon=True).start()

    def _sender(self):
        while True:
            item = self.requests.get()
            if item is None:
                break
            line, box, event = item
            try:
                box['reply'] = self.ctl.send(line)
            except Exception as error:  # the pipe closed or failed: the replies say so
                box['reply'] = None
                self.replies.append(f'{line!r}: {error}')
            event.set()
        close_ctl(self.ctl)

    def _enqueue(self, line):
        box, event = {}, threading.Event()
        self.requests.put((line, box, event))
        return box, event

    def ask(self, line, timeout=None):
        """The reply line to one command ('' when the pipe failed). Hang when there is none in time."""
        box, event = self._enqueue(line)
        if not event.wait(HANG_S if timeout is None else timeout):
            raise Hang(f'no reply to {line!r} within {HANG_S if timeout is None else timeout:.0f} s')
        reply = '' if box.get('reply') is None else str(box['reply']).strip()
        if reply and not reply.startswith(('ok', '{')):
            self.replies.append(f'{line!r}: {reply}')
        return reply

    def _heartbeat(self):
        while not self.closed.wait(HEARTBEAT_S):
            if self.requests.qsize() < 3:  # a silent game does not pile up "attach" lines
                self._enqueue('attach')

    def close(self):
        self.closed.set()
        self.requests.put(None)

    def poll(self):
        """"get state" as a dict (None without a reply). Raises Abort when the launch cannot go on."""
        reply = self.ask('get state')
        state = None
        if reply.startswith('{'):
            try:
                state = json.loads(reply)
            except ValueError:
                state = None
        now = time.monotonic()
        if state is not None:
            window = state.get('window') or {}
            key = (window.get('frames'), state.get('segment'))
            if key != self.progress_key:
                self.progress_key, self.progress_at = key, now
            relaunch = state.get('relaunch') or {}
            if self.attached and relaunch.get('requested'):
                raise Relaunch(int(relaunch.get('boss') or 0))
        if self.process is not None and self.process.poll() is not None:
            raise Crashed(f'the game exited with code {self.process.returncode}')
        if now - self.progress_at > HANG_S:
            raise Hang(f'no frames for {HANG_S:.0f} s')
        if now - self.launch_started > self.launch_limit:
            raise Hang(f'launch longer than {self.launch_limit:.0f} s')
        return state

    def wait(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.poll()
            time.sleep(POLL_S)

    def wait_until(self, condition, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            state = self.poll()
            if state is not None and condition(state):
                return True
            time.sleep(POLL_S)
        return False


class Runner:
    def __init__(self, matrix, plan, out, attach):
        self.matrix = matrix
        self.plan = plan
        self.out = out
        self.attach = attach
        self.scenarios = {s['name']: s for s in matrix['scenarios']}
        self.rows = []
        self.launches = 0
        self.process = None      # the sotc.py this matrix started, while it runs
        self.process_log = None
        self.trace_dirs = {}     # config -> the trace folders of the launches it ran in
        self.trace_dir = None    # the trace folder of the launch that runs now, when the trace is on
        self.csv_file = open(out / 'results.csv', 'w', newline='', encoding='utf-8')
        self.writer = csv.DictWriter(self.csv_file, fieldnames=RESULT_FIELDS + ['note'], extrasaction='ignore')
        self.writer.writeheader()
        self.log_file = open(out / 'run.log', 'w', encoding='utf-8')

    def log(self, text):
        self.log_file.write(text + '\n')
        self.log_file.flush()

    def add_row(self, row):
        self.rows.append(row)
        self.writer.writerow(row)
        self.csv_file.flush()

    def run(self):
        adopt = self.attach  # an attached game serves the first launch when that launch needs nothing
        try:
            for group in self.plan['groups']:
                for launch in group['launches']:
                    can_adopt = adopt and not group['restart'] and not launch['boss']
                    adopt = False
                    self.run_launch(group, launch, can_adopt)
        finally:
            self.stop_game()
            self.csv_file.close()
            self.log_file.close()
        order = [c['name'] for c in self.matrix['configs']]
        summary = summarize(self.rows, order)
        trace_text = trace_section(self.out, order, self.trace_dirs)
        (self.out / 'summary.md').write_text(summary_markdown(summary, self.rows, trace_text=trace_text),
                                             encoding='utf-8')
        return 0

    def stop_game(self):
        """Ends the game: the sotc.py this matrix started, else (attached) every bb-probe.exe."""
        if self.process is not None:
            kill_game(self.process)
        elif self.attach:
            kill_game(None)
        self.process = None
        if self.process_log is not None:
            self.process_log.close()
            self.process_log = None

    def run_launch(self, group, launch, adopt):
        """Runs the items of one launch. An item cut short by a hang or a crash is recorded and the
        rest goes to a new launch; a relaunch request (attached) restarts the same items."""
        items = list(launch['items'])
        boss, relaunches = launch['boss'], 0
        while items:
            self.trace_dir = None
            if adopt:
                ctl = self.connect(None)
                adopted = True
            else:
                self.stop_game()
                self.start(group, boss)
                ctl = self.connect(self.process)
                adopted = False
            adopt = False
            session = Session(ctl, None if adopted else self.process, self.attach,
                              self.matrix['launch_limit_s'])
            previous_config = None
            previous_live = set() if not adopted else self.all_live(group)
            try:
                while items:
                    config_name, scenario_name = items[0]
                    if config_name != previous_config:
                        previous_live = self.apply_config(session, group, config_name, previous_live)
                        previous_config = config_name
                    self.run_item(session, config_name, self.scenarios[scenario_name])
                    items.pop(0)
            except Relaunch as request:
                relaunches += 1
                self.log(f'launch {self.launches}: {request}')
                if relaunches > 3:
                    self.abandon(items.pop(0), session, 'too many relaunches', 'relaunch')
                boss = request.boss or boss
            except Abort as abort:
                self.log(f'launch {self.launches}: {abort.kind}: {abort}')
                self.abandon(items.pop(0), session, str(abort), abort.kind)
            finally:
                session.close()
                for reply in session.replies:
                    self.log(f'  reply {reply}')

    def all_live(self, group):
        names = set()
        for config in group['configs']:
            names.update(config['live'])
        return names

    def abandon(self, item, session, note, kind):
        config_name, scenario_name = item
        self.add_row({'scenario': scenario_name, 'config': config_name, 'lap': session.where.get('lap', 0),
                      'segment': kind, 'frames': 0, 'note': note})

    def start(self, group, boss):
        self.launches += 1
        env = dict(os.environ)
        for name in ('SOTC_LAB', 'SOTC_BOOT_BOSS'):
            env.pop(name, None)
        self.trace_dir = None
        if self.matrix['trace']:
            self.trace_dir = self.out / 'traces' / f'launch-{self.launches:03d}'
            self.trace_dir.mkdir(parents=True, exist_ok=True)
        env.update(launch_env(group, boss, self.trace_dir))
        self.process_log = open(self.out / f'launch-{self.launches:03d}.txt', 'wb')
        self.process = subprocess.Popen([sys.executable, str(ROOT / 'sotc.py')], cwd=str(ROOT), env=env,
                                        stdin=subprocess.DEVNULL, stdout=self.process_log,
                                        stderr=subprocess.STDOUT)
        where = f' trace={self.trace_dir}' if self.trace_dir is not None else ''
        self.log(f'launch {self.launches}: sotc.py boss={boss or "none"} '
                 f'restart-only={json.dumps(group["restart"], sort_keys=True)}{where}')

    def connect(self, process, timeout=CONNECT_S):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if process is not None and process.poll() is not None:
                raise RuntimeError(f'sotc.py exited with code {process.returncode} before the control pipe opened')
            try:
                ctl = make_ctl()
                if str(ctl.send('ping') or '').startswith('ok'):
                    return ctl
                close_ctl(ctl)
            except Exception:
                pass
            time.sleep(1)
        raise RuntimeError('no control pipe (ping) within the timeout')

    def apply_config(self, session, group, config_name, previous_live):
        config = next(c for c in group['configs'] if c['name'] == config_name)
        session.where['config'] = config_name
        for command in set_commands(config['live'], previous_live):
            session.ask(command)
        session.wait(self.matrix['settle_s'])
        return set(config['live'])

    def run_item(self, session, config_name, scenario):
        session.where.update(scenario=scenario['name'], config=config_name)
        mark_prefix = f'{config_name}-{scenario["name"]}'
        if self.trace_dir is not None:
            folders = self.trace_dirs.setdefault(config_name, [])
            if self.trace_dir not in folders:
                folders.append(self.trace_dir)
        if scenario['boss']:
            session.wait_until(lambda state: bool(state.get('area')), BOSS_WAIT_S)
            for lap in range(1, self.matrix['laps'] + 1):
                session.where['lap'] = lap
                mark = f'{mark_prefix}-lap{lap}'
                session.ask(f'boss {scenario["boss"]}')
                self.mark(session, mark)
                session.wait(scenario['measure_s'])
                self.record(session, scenario['name'], config_name, lap, 'fight', mark)
            return
        session.ask(f'path load {route_path(scenario)}')
        session.ask('path play')
        for lap in range(1, self.matrix['laps'] + 1):
            session.where['lap'] = lap
            mark = f'{mark_prefix}-lap{lap}'
            if lap > 1:
                session.ask('path seek 0')
                session.ask('path play')
            self.mark(session, mark)
            self.wait_lap(session, scenario)
            self.record(session, scenario['name'], config_name, lap, 'route', mark)

    def mark(self, session, mark):
        """A lap mark; with the trace on, a trace mark of the same name goes into the dumps too."""
        session.ask(f'mark {mark}')
        if self.matrix['trace']:
            session.ask(f'trace mark {mark}')

    def wait_lap(self, session, scenario):
        """Waits for the end of a route lap: the autopilot stops (after it was seen playing). Without
        autopilot information the lap lasts lap_s seconds; it never lasts longer than LAP_MAX_S."""
        started = time.monotonic()
        seen_playing = False
        while True:
            playing = autopilot_playing(session.poll())
            seen_playing = seen_playing or bool(playing)
            elapsed = time.monotonic() - started
            if playing is None and elapsed >= scenario['lap_s']:
                return
            if seen_playing and playing is False:
                return
            if elapsed >= LAP_MAX_S:
                return
            time.sleep(POLL_S)

    def record(self, session, scenario_name, config_name, lap, segment, mark):
        state = session.poll()
        row = {'scenario': scenario_name, 'config': config_name, 'lap': lap, 'segment': segment}
        if state is None:
            row.update({stat: 0 for stat in STATS}, note='no reply to get state')
        else:
            window = state.get('window') or {}
            for stat in STATS:
                row[stat] = window.get(stat, 0)
            if state.get('segment') not in (None, mark):
                self.log(f'window of {mark}: the game is in segment {state.get("segment")!r}')
        self.add_row(row)


def ensure_routes(matrix):
    missing = [s['route'] for s in matrix['scenarios'] if s['route'] and not Path(route_path(s)).is_file()]
    if missing:
        raise SystemExit('route file(s) not found: ' + ', '.join(missing))
    if matrix['trace_funcs'] and not Path(matrix['trace_funcs']).is_file():
        raise SystemExit('trace_funcs file not found: ' + matrix['trace_funcs'])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--matrix', required=True, type=Path, help='the matrix JSON file')
    parser.add_argument('--attach', action='store_true',
                        help='the game is already running (started by the F2 menu): use it for the first launch')
    parser.add_argument('--out', type=Path, help='output folder (default out/matrix/<timestamp>)')
    parser.add_argument('--dry-run', action='store_true', help='print the plan and launch nothing')
    args = parser.parse_args(argv)
    try:
        matrix = load_matrix(args.matrix)
    except (OSError, ValueError, KeyError) as error:
        raise SystemExit(f'{args.matrix}: {error}')
    plan = plan_matrix(matrix, load_is_live())
    if args.dry_run:
        print(describe_plan(plan, matrix))
        return 0
    ensure_routes(matrix)
    out = args.out or ROOT / 'out' / 'matrix' / datetime.now().strftime('%Y%m%d-%H%M%S')
    out.mkdir(parents=True, exist_ok=True)
    print(f'results in {out}', flush=True)
    return Runner(matrix, plan, out, args.attach).run()


if __name__ == '__main__':
    sys.exit(main())
