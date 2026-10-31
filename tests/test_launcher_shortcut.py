"""Desktop shortcuts of the launcher: the launcher window, or the game directly (--play, for Steam)."""
from paths import ROOT
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(ROOT / 'launcher'))
try:
    import sotc_launcher_win as launcher  # noqa: E402
except ImportError as error:  # no Tkinter
    launcher = None
    REASON = str(error)

EXE = r'C:\Games\SotC\SotC.exe'
PORT = r'C:\Games\SotC'


@unittest.skipIf(launcher is None, 'launcher not importable')
class ShortcutTests(unittest.TestCase):
    def test_frozen_launcher(self):
        script = launcher._shortcut_script(False, True, EXE, PORT, None)
        self.assertIn(r"+'\Shadow of the Colossus.lnk'", script)
        self.assertIn(rf"$s.TargetPath='{EXE}';$s.WorkingDirectory='{PORT}';$s.IconLocation='{EXE},0';", script)
        self.assertNotIn('Arguments', script)
        self.assertTrue(script.endswith('$s.Save()'))

    def test_frozen_play(self):
        script = launcher._shortcut_script(True, True, EXE, PORT, r'C:\data\icon0.ico')
        self.assertIn(r"+'\Shadow of the Colossus (play).lnk'", script)
        self.assertIn(r"$s.IconLocation='C:\data\icon0.ico';", script)
        self.assertIn("$s.Arguments='--play';", script)

    def test_dev_play(self):
        port = Path('/src/sotc')
        script = launcher._shortcut_script(True, False, '/no/such/python.exe', port, None)
        launcher_script = port / 'launcher' / 'sotc_launcher_win.py'
        self.assertIn("$s.TargetPath='/no/such/python.exe';", script)
        self.assertIn(f"""$s.Arguments='"{launcher_script}" --play';""", script)
        self.assertIn(f"$s.IconLocation='{port / 'launcher' / 'sotc.ico'}';", script)

    def test_dev_launcher(self):
        port = Path('/src/sotc')
        script = launcher._shortcut_script(False, False, '/no/such/python.exe', port, None)
        self.assertIn(f"""$s.Arguments='"{port / 'launcher' / 'sotc_launcher_win.py'}"';""", script)

    def test_quotes_and_dollar_escaped(self):
        port = "C:\\Kevin's $HOME \u2019x"
        script = launcher._shortcut_script(True, True, port + '\\SotC.exe', port, None)
        self.assertIn("$s.WorkingDirectory='C:\\Kevin''s $HOME \u2019\u2019x';", script)
        self.assertNotIn('"', script)


if __name__ == '__main__':
    unittest.main()
