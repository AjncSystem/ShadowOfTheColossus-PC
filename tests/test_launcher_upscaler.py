"""FSR4 choices, the recorded render-size matrix, and launcher control synchronization."""
from paths import ROOT
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(ROOT / 'launcher'))
import sotc_launcher_win as launcher


class UpscalerTests(unittest.TestCase):
    def setUp(self):
        self.data = tempfile.TemporaryDirectory()
        self.addCleanup(self.data.cleanup)
        self.folder = Path(self.data.name)
        for name, value in (('DATA_DIR', self.folder), ('CONFIG_DIR', self.folder / 'user'),
                            ('CONFIG_FILE', self.folder / 'user/launcher_settings.json'), ('LANG', 'en')):
            override = patch.object(launcher, name, value)
            override.start()
            self.addCleanup(override.stop)
        override = patch.object(launcher, 'window_icon_file', return_value=None)
        override.start()
        self.addCleanup(override.stop)
        override = patch.dict(os.environ, {}, clear=True)
        override.start()
        self.addCleanup(override.stop)

    def settings(self, **values):
        return {**launcher.APP_DEFAULTS, 'upscaler': 'fsr4', 'output_res': '2560x1440', **values}

    def test_fsr_choices_and_old_saved_modes(self):
        self.assertIn('fsr4', dict(launcher.UPSCALERS))
        self.assertEqual(list(dict(launcher.upscaler_modes('fsr4'))), ['quality', 'performance', 'dlaa'])
        self.assertEqual(len(launcher.upscaler_modes('dlss')), 5)
        for mode in ('balanced', 'ultra'):
            env = launcher.game_environment(self.settings(dlss_mode=mode))
            self.assertEqual(env['SOTC_RENDER_RES'], '1664x936')

    def test_windows_sdk_presets_stay_available(self):
        with patch.object(launcher, 'IS_WINDOWS', True):
            self.assertEqual(launcher.upscaler_modes('fsr4'), launcher.DLSS_MODES)
            self.assertEqual(launcher.upscaler_mode('fsr4', 'balanced'), 'balanced')
            self.assertEqual(launcher.upscaler_mode('fsr4', 'ultra'), 'ultra')

    def test_native_aa_and_fsr3_frame_generation_are_independent(self):
        env = launcher.game_environment(self.settings(dlss_mode='dlaa', framegen='fsr'))
        self.assertEqual(env['SOTC_RENDER_RES'], '2560x1440')
        self.assertEqual(env['SOTC_FRAMEGEN'], 'fsr')

    def test_native_aa_matches_output_and_preserves_framegen_setting(self):
        for output, _label in launcher.OUTPUTS:
            with self.subTest(output=output):
                values = self.settings(output_res=output, dlss_mode='dlaa', framegen='off')
                env = launcher.game_environment(values)
                self.assertEqual(env['SOTC_RENDER_RES'], output)
                self.assertEqual(env['SOTC_OUTPUT_RES'], output)
                self.assertEqual(env['SOTC_UPSCALER'], 'fsr4')
                self.assertEqual(env['SOTC_FRAMEGEN'], 'off')
                self.assertEqual(launcher.fsr4_model(output, output), 'native')
                preview = launcher.upscale_preview(values)
                self.assertIn(f'{output} → {output}', preview)
                self.assertIn('no upscaling', preview)
                self.assertNotIn('Try Performance', preview)

    def test_native_aa_persists_and_imports_from_ini(self):
        (self.folder / 'sotc.ini').write_text('SOTC_UPSCALER=fsr4\nSOTC_OUTPUT_RES=2560x1440\n'
                                            'SOTC_RENDER_RES=2560x1440\n')
        self.assertEqual(launcher.load_settings()['dlss_mode'], 'dlaa')
        launcher.CONFIG_DIR.mkdir(exist_ok=True)
        launcher.CONFIG_FILE.write_text(json.dumps(self.settings(dlss_mode='dlaa')))
        self.assertEqual(launcher.load_settings()['dlss_mode'], 'dlaa')

    def test_recorded_quality_performance_matrix(self):
        for output, quality, performance, quality_model in (
                ('1280x720', '896x504', '640x360', 'native'),
                ('1920x1080', '1280x720', '968x544', 'quality'),
                ('1920x1200', '1280x800', '960x600', 'quality'),
                ('2560x1080', '1704x720', '1288x544', 'quality'),
                ('2560x1440', '1664x936', '1280x720', 'quality')):
            for mode, render in (('quality', quality), ('performance', performance)):
                with self.subTest(output=output, mode=mode):
                    env = launcher.game_environment(self.settings(output_res=output, dlss_mode=mode))
                    self.assertEqual(env['SOTC_RENDER_RES'], render)
            self.assertEqual(launcher.fsr4_model(quality, output), quality_model)
        # Recorded mismatches: assets exist, but launcher ratios select other models.
        self.assertEqual(launcher.fsr4_model('1536x864', '2560x1440'), 'quality')
        self.assertEqual(launcher.fsr4_model('896x504', '2560x1440'), 'performance')

    def test_pro_and_widescreen(self):
        for fps in ('30', '60', 'unlimited'):
            values = self.settings(neo=True, fps_mode=fps)
            env = launcher.game_environment(values)
            self.assertEqual(env['SOTC_RENDER_RES'], '')
            self.assertIn('preset ignored', launcher.upscale_preview(values))
        values = self.settings(neo=True, output_res='3440x1440')
        self.assertEqual(launcher.game_environment(values)['SOTC_NEO'], '0')
        self.assertNotIn('preset ignored', launcher.upscale_preview(values))

    def test_stale_ini_does_not_override_launcher(self):
        (self.folder / 'sotc.ini').write_text('SOTC_UPSCALER=fsr4\nSOTC_RENDER_RES=1664x936\n')
        for upscaler in ('fsr1', 'bilinear'):
            env = launcher.game_environment(self.settings(upscaler=upscaler, neo=True))
            for line in (self.folder / 'sotc.ini').read_text().splitlines():
                key, value = line.split('=', 1)
                env.setdefault(key, value)
            self.assertEqual(env['SOTC_UPSCALER'], upscaler)
            self.assertEqual(env['SOTC_RENDER_RES'], '')

    def test_first_start_and_saved_settings(self):
        (self.folder / 'sotc.ini').write_text('SOTC_GAME_DIR=/games/CUSA08809\n'
                                            'SOTC_UPSCALER=fsr4\nSOTC_FRAMEGEN=auto\n'
                                            'SOTC_OUTPUT_RES=2560x1440\n')
        settings = launcher.load_settings()
        self.assertEqual(settings['game_dir'], '/games/CUSA08809')
        self.assertEqual(settings['upscaler'], 'fsr4')
        launcher.CONFIG_DIR.mkdir(exist_ok=True)
        launcher.CONFIG_FILE.write_text(json.dumps(self.settings(dlss_mode='ultra')))
        self.assertEqual(launcher.load_settings()['dlss_mode'], 'quality')

    def test_preview_native_warning(self):
        with patch.object(launcher, 'LANG', 'en'):
            text = launcher.upscale_preview(self.settings(output_res='1280x720'))
            self.assertIn('896x504 → 1280x720', text)
            self.assertIn('native', text)
            self.assertIn('Try Performance', text)


if __name__ == '__main__':
    unittest.main()
