"""Launcher render sizes: sotc_modes.py only patches multiples of 8 (1600x900 renders at 1536x864)."""
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
class RenderSizeTests(unittest.TestCase):
    def test_on_grid_sizes_render_as_they_are(self):
        for size in ('1920x1200', '2560x1440', '3440x1440', '3840x2160'):
            self.assertEqual(launcher.render_size(size), size)

    def test_900p_renders_at_the_same_aspect_on_the_grid(self):
        self.assertEqual(launcher.render_size('1600x900'), '1536x864')
        self.assertEqual(launcher.dlss_render('1600x900', 'dlaa'), '1536x864')

    def test_launcher_sends_the_render_size(self):
        env = launcher.game_environment({**launcher.APP_DEFAULTS, 'output_res': '1600x900'})
        self.assertEqual((env['SOTC_OUTPUT_RES'], env['SOTC_RENDER_RES']), ('1600x900', '1536x864'))


if __name__ == '__main__':
    unittest.main()
