"""Steam Deck / Gamescope detection, 1280x800 support, and handheld preset tests."""
from paths import ROOT
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
import sotc_hardware  # noqa: E402
try:
    import sotc_launcher_win as launcher  # noqa: E402
except ImportError as error:  # no Tkinter
    launcher = None
    REASON = str(error)


class SteamDeckHardwareTests(unittest.TestCase):
    def test_is_steam_deck(self):
        self.assertTrue(sotc_hardware.is_steam_deck(env={'SteamDeck': '1'}, cpu='Generic CPU'))
        self.assertTrue(sotc_hardware.is_steam_deck(env={}, cpu='AMD Custom APU 0405'))
        self.assertTrue(sotc_hardware.is_steam_deck(env={}, cpu='AMD Custom APU 0932'))
        self.assertFalse(sotc_hardware.is_steam_deck(env={}, cpu='AMD Ryzen 5 5500'))

    def test_in_gamescope(self):
        self.assertTrue(sotc_hardware.in_gamescope(env={'GAMESCOPE_WAYLAND_DISPLAY': 'gamescope-0'}))
        self.assertTrue(sotc_hardware.in_gamescope(env={'XDG_CURRENT_DESKTOP': 'gamescope'}))
        self.assertFalse(sotc_hardware.in_gamescope(env={}))

    def test_recommend_deck(self):
        self.assertEqual(sotc_hardware.recommend({'steam_deck': True, 'threads': 8, 'memory': 16, 'video_memory': 4}),
                         'deck')


@unittest.skipIf(launcher is None, 'launcher not importable')
class SteamDeckLauncherTests(unittest.TestCase):
    def test_800p_supported_and_not_wide(self):
        self.assertIn('1280x800', [entry[0] for entry in launcher.OUTPUTS])
        self.assertFalse(launcher.is_wide('1280x800'))
        self.assertEqual(launcher.render_size('1280x800'), '1280x800')
        self.assertEqual(launcher.dlss_render('1280x800', 'quality'), '832x520')

    def test_deck_preset_environment_lcd_and_oled(self):
        deck_cfg = {**launcher.APP_DEFAULTS, **launcher.PLAY_MODE_VALUES['deck'], 'play_mode': 'deck'}
        with mock.patch.object(launcher, 'IS_WINDOWS', False):
            with mock.patch.object(launcher, 'screen_refresh_hz', return_value=60):
                env_lcd = launcher.game_environment(deck_cfg)
                self.assertEqual(env_lcd['SOTC_OUTPUT_RES'], '1280x800')
                self.assertEqual(env_lcd['SOTC_RENDER_RES'], '832x520')
                self.assertEqual(env_lcd['BB_FULLSCREEN'], '1')
                self.assertEqual(env_lcd['SOTC_UPSCALER'], 'fsr4')
                self.assertEqual(env_lcd['SOTC_FRAMEGEN'], 'fsr')
                self.assertEqual(env_lcd['SOTC_FG_HUDLESS'], '1')
                self.assertEqual(env_lcd['SOTC_FPS_DYNAMIC'], '30')
                self.assertEqual(env_lcd['SOTC_FPS_FG_CAP'], '30')

            with mock.patch.object(launcher, 'screen_refresh_hz', return_value=90):
                env_oled = launcher.game_environment(deck_cfg)
                self.assertEqual(env_oled['SOTC_FPS_DYNAMIC'], '45')
                self.assertEqual(env_oled['SOTC_FPS_FG_CAP'], '45')


if __name__ == '__main__':
    unittest.main()
