# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
from sotc_cache import cache_path
spec = importlib.util.spec_from_file_location('sotc_frame_summary', ROOT / 'tools/sotc_frame_summary.py')
metrics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(metrics)


class StutterTests(unittest.TestCase):
    def test_release_move_keeps_cache_and_portable_override(self):
        env = {'LOCALAPPDATA': 'local'}
        self.assertEqual(cache_path('old/user', env), cache_path('new/user', env))
        env['SOTC_PORTABLE_CACHE'] = '1'
        self.assertEqual(cache_path('new/user', env), Path('new/user/cache'))
        env['SOTC_CACHE_DIR'] = 'custom-cache'
        self.assertEqual(cache_path('new/user', env), Path('custom-cache'))
        self.assertEqual(cache_path('user', {}), Path('user/cache'))

    def test_single_hitch_affects_low_not_typical_frame_time(self):
        deltas = [0] + [20_000_000] * 99 + [500_000_000]
        time = 0
        rows = []
        for delta in deltas:
            time += delta
            rows.append({'time_ns': str(time), 'interval_ns': str(delta)})
        result = metrics.summarize(rows)
        self.assertAlmostEqual(result['average_fps'], 100 / 2.48)
        self.assertEqual(result['one_percent_low_fps'], 2)
        self.assertEqual(result['median_ms'], 20)
        self.assertEqual(result['intervals_over_40_ms'], 1)
        # Selecting a stable interval excludes the later hitch.
        stable = metrics.summarize(rows, 0.2, 1.5)
        self.assertEqual(stable['average_fps'], 50)
        self.assertEqual(stable['one_percent_low_fps'], 50)

    def test_empty_window_is_not_a_successful_benchmark(self):
        with self.assertRaises(ValueError):
            metrics.summarize([])
        with self.assertRaises(ValueError):
            metrics.summarize([{'time_ns': '1', 'interval_ns': '0'}])
