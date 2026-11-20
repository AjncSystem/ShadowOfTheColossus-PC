"""Launcher HDR picture settings: presets the same as the in-game menu's, old UI brightness in nits."""
from paths import ROOT
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(ROOT / 'launcher'))
try:
    import sotc_launcher_win as launcher  # noqa: E402
except ImportError as error:  # no Tkinter
    launcher = None
    REASON = str(error)

TONEMAPS = {0: 'vanilla', 1: 'vanilla_plus', 2: 'none', 3: 'vanilla_plus_hue', 4: 'renodrt'}


@unittest.skipIf(launcher is None, 'launcher not importable')
class HdrPictureTests(unittest.TestCase):
    def test_presets_match_the_game_menu(self):
        header = (ROOT / 'gpu/shadps4/video_core/sotc_live.h').read_text(encoding='utf-8')
        game = re.findall(r'\{"(\w+)", (\d), \{([-\d, .]+)\}\}', header)
        self.assertEqual(len(game), len(launcher.HDR_PRESETS))
        for (key, tonemap, grades), (l_key, _label, l_tonemap, l_grades) in zip(game, launcher.HDR_PRESETS):
            self.assertEqual(key, l_key)
            self.assertEqual(TONEMAPS[int(tonemap)], l_tonemap, key)
            self.assertEqual([float(g) for g in grades.split(',')], [float(g) for g in l_grades], key)
            self.assertEqual(len(l_grades), len(launcher.HDR_GRADE_KEYS), key)
        keys = re.search(r'kGradeKeys\[kGradeCount\] = \{([^}]*)\}', header).group(1)
        self.assertEqual(tuple(re.findall(r'"(\w+)"', keys)), launcher.HDR_GRADE_KEYS)

    def test_old_ui_brightness_in_nits(self):
        with tempfile.TemporaryDirectory() as tmp:
            config = Path(tmp) / 'launcher_settings.json'
            saved = (launcher.CONFIG_FILE, launcher.LEGACY_CONFIG_FILE)
            launcher.CONFIG_FILE, launcher.LEGACY_CONFIG_FILE = config, Path(tmp) / 'none.json'
            try:
                config.write_text(json.dumps({'hdr_ui': 160}), encoding='utf-8')
                self.assertEqual(launcher.load_settings()['hdr_ui_nits'], '480')
                config.write_text(json.dumps({'hdr_ui': 160, 'hdr_ui_nits': '300'}), encoding='utf-8')
                self.assertEqual(launcher.load_settings()['hdr_ui_nits'], '300')
                config.write_text(json.dumps({'hdr_white': 200}), encoding='utf-8')
                self.assertEqual(launcher.load_settings()['hdr_ui_nits'], '0')
            finally:
                launcher.CONFIG_FILE, launcher.LEGACY_CONFIG_FILE = saved

    def test_level_text_as_the_menu_writes_it(self):
        self.assertEqual([launcher.level_text(v) for v in (200, 200.0, -0.5, 0)], ['200', '200', '-0.5', '0'])


if __name__ == '__main__':
    unittest.main()
