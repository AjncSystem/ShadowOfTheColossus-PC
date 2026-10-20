"""Launcher frame rate modes: above 30 the game's own 60 FPS target (sotc_modes.py fps60game)."""
from paths import ROOT
import sys
import unittest

sys.path.insert(0, str(ROOT / 'launcher'))
try:
    import sotc_launcher_win as launcher  # noqa: E402
except ImportError as error:  # no Tkinter
    launcher = None
    REASON = str(error)


@unittest.skipIf(launcher is None, 'launcher not importable')
class FpsModeTests(unittest.TestCase):
    def env(self, fps_mode, af16=True):
        return launcher.game_environment({**launcher.APP_DEFAULTS, 'fps_mode': fps_mode, 'af16': af16})

    def test_modes(self):
        for fps_mode, modes, cap, limit in (('30', 'af16', '30', '0'),
                                            ('60', 'af16,fps60game', 'off', '60'),
                                            ('unlimited', 'af16,fps60game', 'off', '0')):
            env = self.env(fps_mode)
            self.assertEqual((env['SOTC_MODES'], env['SOTC_FPS_CAP'], env['SOTC_FPS_LIMIT']),
                             (modes, cap, limit), fps_mode)
        self.assertEqual(self.env('60', af16=False)['SOTC_MODES'], 'fps60game')


if __name__ == '__main__':
    unittest.main()
