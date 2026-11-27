"""FG-aware dynamic FPS stepping, Light mode pacing, and VRAM estimator/warning tests."""
from paths import ROOT
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
try:
    import sotc_launcher_win as launcher  # noqa: E402
except ImportError as error:  # no Tkinter
    launcher = None
    REASON = str(error)


@unittest.skipIf(launcher is None, 'launcher not importable')
class FrameGenAndVramTests(unittest.TestCase):
    def test_dynamic_fps_step_without_fg(self):
        cases = ((60, '30'), (75, '37.5'), (90, '45'), (100, '50'), (120, '40'), (144, '48'), (165, '41.25'), (240, '48'))
        for hz, expected in cases:
            self.assertEqual(launcher.dynamic_fps_step(hz), expected, hz)
            self.assertEqual(launcher.dynamic_fps_step(hz, 1), expected, hz)

    def test_dynamic_fps_step_with_fg_2x(self):
        cases = ((60, '30'), (75, '37.5'), (90, '45'), (100, '50'), (120, '30'), (144, '36'), (165, '41.25'), (240, '40'))
        for hz, expected in cases:
            self.assertEqual(launcher.dynamic_fps_step(hz, 2), expected, hz)

    def test_light_mode_enables_fps_dynamic(self):
        self.assertTrue(launcher.PLAY_MODE_VALUES['light'].get('fps_dynamic'))

    def test_game_environment_fg_pacing(self):
        base = {**launcher.APP_DEFAULTS, 'fps_mode': '60', 'fps_dynamic': True}
        with mock.patch.object(launcher, 'IS_WINDOWS', False):
            # FG off: no SOTC_FPS_FG_CAP
            with mock.patch.object(launcher, 'screen_refresh_hz', return_value=60):
                env = launcher.game_environment({**base, 'framegen': 'off'})
                self.assertEqual(env['SOTC_FPS_DYNAMIC'], '30')
                self.assertNotIn('SOTC_FPS_FG_CAP', env)

            # FG on at 60 Hz / 75 Hz: locks real FPS directly to saturate screen
            for hz, expected in ((60, '30'), (75, '37.5'), (90, '45')):
                with mock.patch.object(launcher, 'screen_refresh_hz', return_value=hz):
                    env = launcher.game_environment({**base, 'framegen': 'fsr'})
                    self.assertEqual(env['SOTC_FPS_DYNAMIC'], expected)
                    self.assertEqual(env['SOTC_FPS_FG_CAP'], expected)

            # FG on at 144 Hz custom: adjusts SOTC_FPS_DYNAMIC to 36 without hard-locking
            with mock.patch.object(launcher, 'screen_refresh_hz', return_value=144):
                env = launcher.game_environment({**base, 'framegen': 'fsr', 'play_mode': 'custom'})
                self.assertEqual(env['SOTC_FPS_DYNAMIC'], '36')
                self.assertNotIn('SOTC_FPS_FG_CAP', env)

            # Light mode at 144 Hz and 120 Hz: locks straight to steady FG step
            for hz, expected in ((144, '36'), (120, '30')):
                with mock.patch.object(launcher, 'screen_refresh_hz', return_value=hz):
                    light_cfg = {**base, **launcher.PLAY_MODE_VALUES['light'], 'play_mode': 'light'}
                    env = launcher.game_environment(light_cfg)
                    self.assertEqual(env['SOTC_FPS_DYNAMIC'], expected)
                    self.assertEqual(env['SOTC_FPS_FG_CAP'], expected)

    def test_estimate_vram_monotonic(self):
        base = {**launcher.APP_DEFAULTS, 'gpu_vram_mb': 16384, 'vram_profile': 'full',
                'output_res': '1920x1080', 'textures': 'off', 'neo': False, 'upscaler': 'fsr1',
                'framegen': 'off', 'vram_use': 'normal'}
        est_base = launcher.estimate_vram_mb(base)
        self.assertGreater(launcher.estimate_vram_mb({**base, 'textures': 'high'}), est_base)
        self.assertGreater(launcher.estimate_vram_mb({**base, 'textures': 'max'}),
                           launcher.estimate_vram_mb({**base, 'textures': 'high'}))
        self.assertGreater(launcher.estimate_vram_mb({**base, 'neo': True}), est_base)
        self.assertGreater(launcher.estimate_vram_mb({**base, 'framegen': 'fsr'}), est_base)
        self.assertGreater(launcher.estimate_vram_mb({**base, 'upscaler': 'fsr4'}), est_base)
        est_1440 = launcher.estimate_vram_mb({**base, 'output_res': '2560x1440'})
        est_4k = launcher.estimate_vram_mb({**base, 'output_res': '3840x2160'})
        self.assertGreater(est_1440, est_base)
        self.assertGreater(est_4k, est_1440)

    def test_light_vram_auto_and_warning_8gb_vs_16gb(self):
        # 8 GB card at 1080p Normal: stays Full, no warning
        s1080 = {**launcher.APP_DEFAULTS, 'gpu_vram_mb': 8192, 'vram_profile': 'auto',
                 'output_res': '1920x1080', 'textures': 'high', 'neo': False,
                 'upscaler': 'fsr1', 'framegen': 'off', 'vram_use': 'normal'}
        self.assertFalse(launcher.light_vram(s1080))
        self.assertIsNone(launcher.vram_warning_info(s1080))

        # 8 GB card with 1440p + Pro + FSR 3.1 + FG + High VRAM use: Auto enables Light
        s1440_heavy = {**s1080, 'output_res': '2560x1440', 'neo': True, 'upscaler': 'fsr4',
                       'dlss_mode': 'quality', 'framegen': 'auto', 'fg_hudless': True,
                       'vram_use': 'high'}
        self.assertTrue(launcher.light_vram(s1440_heavy))
        # Explicit full overrides auto
        self.assertFalse(launcher.light_vram({**s1440_heavy, 'vram_profile': 'full'}))
        self.assertTrue(launcher.light_vram({**s1080, 'vram_profile': 'light'}))

        # 8 GB card at 4K + FSR 3.1 + FG + High textures: warns before crash
        s4k_8gb = {**s1080, 'output_res': '3840x2160', 'upscaler': 'fsr4',
                   'dlss_mode': 'quality', 'framegen': 'fsr', 'textures': 'high'}
        warn = launcher.vram_warning_info(s4k_8gb)
        self.assertIsNotNone(warn)
        self.assertGreater(warn[0], 8192 * launcher.VRAM_WARN_RATIO)
        self.assertTrue(warn[2])

        # Same 4K config on a 16 GB card: no warning
        self.assertIsNone(launcher.vram_warning_info({**s4k_8gb, 'gpu_vram_mb': 16384}))


if __name__ == '__main__':
    unittest.main()
