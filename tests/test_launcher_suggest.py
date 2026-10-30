"""Launcher "Suggest for my PC": settings from the card, VRAM, CPU threads and screen."""
from paths import ROOT
import sys
import unittest

sys.path.insert(0, str(ROOT / 'launcher'))
try:
    import sotc_launcher_win as launcher  # noqa: E402
except ImportError:  # no Tkinter
    launcher = None


@unittest.skipIf(launcher is None, 'launcher not importable')
class SuggestTests(unittest.TestCase):
    def suggest(self, gpu, vram=8192, threads=16, screen='1920x1080'):
        return launcher.suggest_settings(gpu, vram, threads, screen)

    def test_rtx_1080p_is_dlaa(self):
        s, reasons = self.suggest('NVIDIA GeForce RTX 5060 Ti')
        self.assertEqual((s['upscaler'], s['render_preset'], s['output_res']), ('dlss', 'native', '1920x1080'))
        self.assertTrue(any('DLSS-G' in r for r in reasons))

    def test_4k_renders_smaller(self):
        s, _ = self.suggest('NVIDIA GeForce RTX 3060', screen='3840x2160')
        self.assertEqual(s['render_preset'], 'performance')
        s, _ = self.suggest('AMD Radeon RX 6650 XT', screen='2560x1440')
        self.assertEqual((s['upscaler'], s['render_preset']), ('fsr3', 'quality'))

    def test_small_vram_and_unknown_screen(self):
        s, _ = self.suggest('NVIDIA GeForce GTX 750 Ti', vram=2048, screen='1366x768')
        self.assertEqual((s['textures'], s['fps_mode'], s['output_res']), ('off', '30', '1920x1080'))

    def test_gcn_and_four_threads(self):
        _, reasons = self.suggest('AMD Radeon RX 570 Series', threads=4)
        self.assertTrue(any('GCN' in r for r in reasons))
        self.assertTrue(any('45-50' in r for r in reasons))
        _, reasons = self.suggest('AMD Radeon RX 5700 XT')
        self.assertFalse(any('GCN' in r for r in reasons))


if __name__ == '__main__':
    unittest.main()
