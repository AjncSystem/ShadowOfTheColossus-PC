"""Tests for tools/sotc_matrix.py (planning, dry run, summary math, hang detection), the boss boot and
pending-env helpers of sotc.py, and the NAME(ARGS) cvar form of scripts/sotc_modes.py."""
from paths import ROOT  # noqa: F401  (puts scripts/ on sys.path)
import contextlib
import io
import json
import os
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT))
import sotc_matrix as sm  # noqa: E402
import sotc  # noqa: E402
import sotc_modes  # noqa: E402


def matrix(**extra):
    data = {'scenarios': [{'name': 'route', 'route': 'tools/routes/x.json'}, {'name': 'boss1', 'boss': 1}],
            'laps': 2, 'settle_s': 1, 'configs': [{'name': 'baseline', 'env': {}},
                                                   {'name': 'live', 'env': {'LIVE_A': '1'}},
                                                   {'name': 'restart', 'env': {'RESTART_B': 1}}]}
    data.update(extra)
    return sm.normalize_matrix(data)


def is_live_for(*names):
    return lambda name: name in names


class PlanTests(unittest.TestCase):
    def test_configs_grouped_by_restart_only_part(self):
        plan = sm.plan_matrix(matrix(), is_live_for('LIVE_A'))
        groups = plan['groups']
        self.assertEqual(len(groups), 2)  # {} for baseline and live; {RESTART_B} for restart
        self.assertEqual(groups[0]['restart'], {})
        self.assertEqual([c['name'] for c in groups[0]['configs']], ['baseline', 'live'])
        self.assertEqual(groups[0]['configs'][1]['live'], {'LIVE_A': '1'})
        self.assertEqual(groups[1]['restart'], {'RESTART_B': '1'})
        self.assertEqual(groups[1]['configs'][0]['live'], {})

    def test_base_env_is_shared_and_split(self):
        m = matrix(base_env={'BASE_R': 'x', 'BASE_L': '2'})
        plan = sm.plan_matrix(m, is_live_for('BASE_L'))
        self.assertEqual(plan['groups'][0]['restart'], {'BASE_R': 'x'})
        self.assertEqual(plan['groups'][0]['configs'][0]['live'], {'BASE_L': '2'})

    def test_launches_per_boss_with_items_in_config_order(self):
        plan = sm.plan_matrix(matrix(), is_live_for('LIVE_A'))
        launches = plan['groups'][0]['launches']
        self.assertEqual([(l['boss'], l['items']) for l in launches],
                         [(0, [('baseline', 'route'), ('live', 'route')]),
                          (1, [('baseline', 'boss1'), ('live', 'boss1')])])

    def test_launch_env_has_lab_and_boss(self):
        group = {'restart': {'R': '1'}}
        self.assertEqual(sm.launch_env(group, 0), {'R': '1', 'SOTC_LAB': '1'})
        self.assertEqual(sm.launch_env(group, 7), {'R': '1', 'SOTC_LAB': '1', 'SOTC_BOOT_BOSS': '7'})

    def test_set_commands_reset_names_the_previous_config_set(self):
        self.assertEqual(sm.set_commands({'A': '1'}, previous={'A', 'B'}), ['set B=', 'set A=1'])
        self.assertEqual(sm.set_commands({}, previous={'B'}), ['set B='])
        self.assertEqual(sm.set_commands({'A': '1'}), ['set A=1'])
        self.assertEqual(sm.set_commands({}), [])

    def test_bool_env_values_become_flags(self):
        m = matrix(configs=[{'name': 'x', 'env': {'F': True, 'G': False, 'N': 3}}])
        self.assertEqual(m['configs'][0]['env'], {'F': '1', 'G': '0', 'N': '3'})


class DryRunTests(unittest.TestCase):
    def run_dry(self, data, is_live):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'm.json'
            path.write_text(json.dumps(data), encoding='utf-8')
            stdout = io.StringIO()
            with patch.object(sm, 'load_is_live', return_value=is_live), \
                    contextlib.redirect_stdout(stdout):
                code = sm.main(['--matrix', str(path), '--dry-run'])
        self.assertEqual(code, 0)
        return stdout.getvalue()

    def test_dry_run_prints_sets_and_launches_without_launching(self):
        data = {'scenarios': [{'name': 'route', 'route': 'tools/routes/x.json'}], 'laps': 1, 'settle_s': 2,
                'configs': [{'name': 'a', 'env': {'LIVE_A': '1'}}, {'name': 'b', 'env': {}}]}
        with patch.object(sm.subprocess, 'Popen', side_effect=AssertionError('no launch')):
            text = self.run_dry(data, is_live_for('LIVE_A'))
        lines = [l.strip() for l in text.splitlines()]
        self.assertIn('set LIVE_A=1', lines)
        # switching from a to b resets the name a set
        self.assertIn('set LIVE_A=', lines)
        self.assertLess(lines.index('set LIVE_A=1'), lines.index('item a / route'))
        self.assertIn('path load ' + sm.route_path(sm.normalize_matrix(data)['scenarios'][0]), lines)
        self.assertIn('mark a-route-lap1', lines)

    def test_dry_run_boss_launch_has_boot_env(self):
        data = {'scenarios': [{'name': 'b7', 'boss': 7, 'measure_s': 5}], 'laps': 1,
                'configs': [{'name': 'x', 'env': {}}]}
        text = self.run_dry(data, is_live_for())
        self.assertIn('SOTC_BOOT_BOSS', text)
        self.assertIn('boss 7', text)

    def test_bad_matrix_is_rejected(self):
        with self.assertRaises(ValueError):
            sm.normalize_matrix({'scenarios': []})
        with self.assertRaises(ValueError):
            sm.normalize_matrix({'scenarios': [{'name': 'x', 'route': 'a', 'boss': 1}]})
        with self.assertRaises(ValueError):
            sm.normalize_matrix({'scenarios': [{'name': 'x', 'boss': 17}]})
        with self.assertRaises(ValueError):
            sm.normalize_matrix({'scenarios': [{'name': 'bad name', 'boss': 1}]})


class SummaryTests(unittest.TestCase):
    def rows(self):
        base = {'frames': 100, 'p50': 10.0, 'p99': 20.0, 'max': 40.0, 'hitch33': 4, 'hitch50': 2, 'hitch100': 1}
        other = {'frames': 100, 'p50': 12.0, 'p99': 30.0, 'max': 40.0, 'hitch33': 6, 'hitch50': 3, 'hitch100': 2}
        rows = []
        for lap, (a, b) in enumerate([(9.0, 10.0), (11.0, 12.0), (10.0, 11.0)], 1):
            rows.append({'scenario': 's', 'config': 'base', 'lap': lap, 'segment': 'route',
                         **{**base, 'p50': a}})
            rows.append({'scenario': 's', 'config': 'other', 'lap': lap, 'segment': 'route',
                         **{**other, 'p50': b}})
        rows.append({'scenario': 's', 'config': 'other', 'lap': 4, 'segment': 'hang', 'frames': 0,
                     'note': 'no frames for 30 s'})
        return rows

    def test_medians_over_laps(self):
        summary = sm.summarize(self.rows(), ['base', 'other'])
        base = next(e for e in summary if e['config'] == 'base')
        self.assertEqual(base['laps'], 3)
        self.assertEqual(base['p50'], 10.0)  # median of 9, 11, 10
        other = next(e for e in summary if e['config'] == 'other')
        self.assertEqual(other['p50'], 11.0)  # median of 10, 12, 11
        self.assertEqual(other['hitch33'], 6)

    def test_deltas_against_first_config(self):
        summary = sm.summarize(self.rows(), ['base', 'other'])
        base = next(e for e in summary if e['config'] == 'base')
        other = next(e for e in summary if e['config'] == 'other')
        self.assertIsNone(base['d_p99'])
        self.assertAlmostEqual(other['d_p50'], 10.0)   # 11 against 10
        self.assertAlmostEqual(other['d_p99'], 50.0)   # 30 against 20 (the medians)
        self.assertEqual(other['d_max'], 0.0)

    def test_zero_base_gives_no_delta(self):
        rows = [{'scenario': 's', 'config': 'a', 'lap': 1, 'segment': 'fight', 'frames': 5, 'p50': 0,
                 'p99': 0, 'max': 0, 'hitch33': 0, 'hitch50': 0, 'hitch100': 0},
                {'scenario': 's', 'config': 'b', 'lap': 1, 'segment': 'fight', 'frames': 5, 'p50': 3,
                 'p99': 3, 'max': 3, 'hitch33': 0, 'hitch50': 0, 'hitch100': 0}]
        summary = sm.summarize(rows, ['a', 'b'])
        self.assertIsNone(next(e for e in summary if e['config'] == 'b')['d_p50'])

    def test_markdown_lists_hangs(self):
        rows = self.rows()
        text = sm.summary_markdown(sm.summarize(rows, ['base', 'other']), rows)
        self.assertIn('## s (route)', text)
        self.assertIn('| other | 3 |', text)
        self.assertIn('no frames for 30 s', text)


class FakeCtl:
    def __init__(self, state):
        self.state = state

    def send(self, line):
        if line == 'get state':
            return json.dumps(self.state)
        return 'ok'


class SilentCtl:
    """A game that never answers: send() blocks until released."""

    def __init__(self):
        self.release = threading.Event()

    def send(self, line):
        self.release.wait(60)
        return None


class SessionTests(unittest.TestCase):
    def test_unanswered_command_is_a_hang(self):
        ctl = SilentCtl()
        session = sm.Session(ctl, None, False, 3600)
        with patch.object(sm, 'HANG_S', 0.5), self.assertRaises(sm.Hang):
            session.poll()
        ctl.release.set()
        session.close()

    def test_no_frame_progress_is_a_hang(self):
        state = {'window': {'frames': 5}, 'segment': 'a'}
        session = sm.Session(FakeCtl(state), None, False, 3600)
        session.poll()  # the first frame count is progress
        session.progress_at -= 100  # no change for 100 s
        with patch.object(sm, 'HANG_S', 30.0), self.assertRaises(sm.Hang):
            session.poll()
        session.close()

    def test_progress_resets_the_hang_clock(self):
        state = {'window': {'frames': 5}, 'segment': 'a'}
        session = sm.Session(FakeCtl(state), None, False, 3600)
        self.assertIsNotNone(session.poll())
        state['window']['frames'] = 6
        session.progress_at -= 100
        self.assertIsNotNone(session.poll())  # frames moved: no hang
        session.close()

    def test_relaunch_request_only_when_attached(self):
        state = {'window': {'frames': 5}, 'segment': 'a', 'relaunch': {'requested': True, 'boss': 3}}
        attached = sm.Session(FakeCtl(state), None, True, 3600)
        with self.assertRaises(sm.Relaunch) as caught:
            attached.poll()
        self.assertEqual(caught.exception.boss, 3)
        attached.close()
        standalone = sm.Session(FakeCtl(state), None, False, 3600)
        self.assertIsNotNone(standalone.poll())
        standalone.close()

    def test_autopilot_state(self):
        self.assertTrue(sm.autopilot_playing({'autopilot': {'playing': True}}))
        self.assertFalse(sm.autopilot_playing({'autopilot': {'playing': False}}))
        self.assertIsNone(sm.autopilot_playing({'autopilot': {}}))
        self.assertIsNone(sm.autopilot_playing(None))


class PendingAndBossTests(unittest.TestCase):
    def test_pending_env_merged_once_and_deleted(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'lab_pending_env.json'
            path.write_text(json.dumps({'env': {'SOTC_X': 2, 'SOTC_Y': 'a b'}, 'boss': 4}), encoding='utf-8')
            env = {'KEEP': '1'}
            self.assertEqual(sotc.apply_pending_env(path, env), 4)
            self.assertEqual(env, {'KEEP': '1', 'SOTC_X': '2', 'SOTC_Y': 'a b'})
            self.assertFalse(path.exists())
            env2 = {}
            self.assertEqual(sotc.apply_pending_env(path, env2), 0)
            self.assertEqual(env2, {})

    def test_bad_pending_file_is_removed_and_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'lab_pending_env.json'
            path.write_text('{not json', encoding='utf-8')
            env = {}
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(sotc.apply_pending_env(path, env), 0)
            self.assertEqual(env, {})
            self.assertFalse(path.exists())

    def test_pending_names_must_be_plain_identifiers(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'p.json'
            path.write_text(json.dumps({'env': {'A B': '1', 'OK': '2'}}), encoding='utf-8')
            env = {}
            sotc.apply_pending_env(path, env)
            self.assertEqual(env, {'OK': '2'})

    def test_valid_boss(self):
        self.assertEqual(sotc.valid_boss(''), 0)
        self.assertEqual(sotc.valid_boss('16'), 16)
        with self.assertRaises(ValueError):
            sotc.valid_boss(17)

    def test_boss_plan_indexes_from_zero_and_skips_horse_for_7_and_12(self):
        plan = sotc.boss_plan(1)
        self.assertIn('shadow_jumpToBoss 0', plan)
        self.assertIn('shadow_horseDebugWarpToPlayer', plan)
        self.assertIn('*stage "JumpToBoss"', plan)
        self.assertNotIn('shadow_horseDebugWarpToPlayer', sotc.boss_plan(7))
        self.assertNotIn('shadow_horseDebugWarpToPlayer', sotc.boss_plan(12))
        self.assertIn('shadow_jumpToBoss 11', sotc.boss_plan(12))
        self.assertTrue(plan.endswith('\n'))

    def test_boss_cvars_pass_the_modes_validator(self):
        for item in sotc.BOSS_CVARS.split():
            self.assertIsNotNone(sotc_modes.CVAR.match(item), item)
        self.assertIn('automate_loadfile(route_bossjump.txt)', sotc.BOSS_CVARS.split())

    def test_hold_cross_holds_until_second_gameflow_line_then_releases(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / 'last_run.log'
            pad = Path(tmp) / 'pad.txt'
            log.write_bytes(b'st_gameflow() starting game\n')
            stop = threading.Event()
            worker = threading.Thread(target=sotc.hold_cross, args=(pad, log, stop), daemon=True)
            worker.start()
            time.sleep(0.8)
            self.assertEqual(pad.read_text(), 'cross')  # one line seen: still holding
            with log.open('ab') as handle:
                handle.write(b'more output\nst_gameflow() starting game\n')
            worker.join(timeout=10)
            self.assertFalse(worker.is_alive())
            self.assertEqual(pad.read_text(), '')


class CvarFormTests(unittest.TestCase):
    def test_name_with_arguments_accepted(self):
        for item in ('automate_loadfile(route_bossjump.txt)', 'nointro', 'RetailBuild=0', 'automate_go',
                     'r_motionBlurEnable=0'):
            self.assertIsNotNone(sotc_modes.CVAR.match(item), item)

    def test_malformed_items_rejected(self):
        for item in ('automate_loadfile(', 'a(b c)', '(x)', '1abc', 'x=(1)'):
            self.assertIsNone(sotc_modes.CVAR.match(item), item)


if __name__ == '__main__':
    unittest.main()
