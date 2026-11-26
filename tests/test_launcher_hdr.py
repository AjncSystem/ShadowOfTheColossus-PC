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
            saved = (launcher.CONFIG_FILE, launcher.LEGACY_CONFIG_FILE, launcher._SETTINGS_FILE)
            launcher.CONFIG_FILE, launcher.LEGACY_CONFIG_FILE = config, Path(tmp) / 'none.json'
            launcher._SETTINGS_FILE = config  # v0.77 (#467): the settings file is chosen once and kept
            try:
                config.write_text(json.dumps({'hdr_ui': 160}), encoding='utf-8')
                self.assertEqual(launcher.load_settings()['hdr_ui_nits'], '480')
                config.write_text(json.dumps({'hdr_ui': 160, 'hdr_ui_nits': '300'}), encoding='utf-8')
                self.assertEqual(launcher.load_settings()['hdr_ui_nits'], '300')
                config.write_text(json.dumps({'hdr_white': 200}), encoding='utf-8')
                self.assertEqual(launcher.load_settings()['hdr_ui_nits'], '0')
            finally:
                launcher.CONFIG_FILE, launcher.LEGACY_CONFIG_FILE, launcher._SETTINGS_FILE = saved

    def test_level_text_as_the_menu_writes_it(self):
        self.assertEqual([launcher.level_text(v) for v in (200, 200.0, -0.5, 0)], ['200', '200', '-0.5', '0'])

    def test_dither_on_by_default_and_reloaded_after_a_game(self):
        self.assertIs(launcher.APP_DEFAULTS['dither'], True)
        self.assertIn('dither', launcher.Launcher.LIVE_KEYS)

    def test_dlss_sharpness_keeps_its_old_names(self):
        # Before 1.5 and 2 were added, 0 was High, 0.5 Medium and 1 Low: a saved choice keeps its name.
        names = {value: label[1] for value, label in launcher.DLSS_SHARPNESS}
        self.assertEqual(names['0'], 'High (default)')
        self.assertEqual(names['0.5'], 'Medium')
        self.assertEqual(names['1'], 'Low')
        # The F1 menu's slider, from off to the sharpest, has the launcher's names.
        overlay = (ROOT / 'gpu/shim/bbport_overlay.cpp').read_text(encoding='utf-8')
        item = overlay[overlay.index('"DLSS / FSR 4 sharpening"'):]
        item = item[:item.index('[] {')]
        menu = re.findall(r'L\("[^"]*", "([^"]*)"\), "\d"', item)
        order = ['off', '2', '1.5', '1', '0.5', '0']
        self.assertEqual(menu, [names[v] for v in order])


class GradeNeutralTests(unittest.TestCase):
    def test_neutral_check_matches_the_shader(self):
        """The presenter skips the scene key when PostProcessingPass::GradeNeutral says grade() in
        post_process.frag returns early: both must test the same controls at the same values."""
        shader = (ROOT / 'gpu/shadps4/video_core/host_shaders/post_process.frag').read_text(encoding='utf-8')
        early = re.search(r'vec3 grade\(vec3 nits\) \{.*?if \((.*?)\) \{', shader, re.S).group(1)
        header = (ROOT / 'gpu/shadps4/video_core/renderer_vulkan/host_passes/pp_pass.h').read_text(encoding='utf-8')
        neutral = re.search(r'GradeNeutral\(const Settings& s\) \{\s*return (.*?);', header, re.S).group(1)
        in_shader = {k: float(v) for k, v in re.findall(r'pp\.(\w+) == ([\d.]+)', early)}
        in_header = {k: float(v) for k, v in re.findall(r's\.(\w+) == ([\d.]+)f', neutral)}
        self.assertTrue(in_shader)
        self.assertEqual(in_header, in_shader)


if __name__ == '__main__':
    unittest.main()
