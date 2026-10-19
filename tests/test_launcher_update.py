"""Launcher update notice (issue #136): which release tags count as newer than the launcher."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'launcher'))
from sotc_launcher_win import is_newer_release

CASES = [  # (release tag, launcher VERSION, newer)
    ('v0.59', '0.58.1', True), ('0.58.2', '0.58.1', True), ('v1.0', '0.58.1', True),
    ('0.58.10', '0.58.9', True), ('0.68', '0.67-cp-mt', True),
    ('v0.58.1', '0.58.1', False), ('v0.58', '0.58.1', False), ('0.0.13', '0.58.1', False),
    ('v0.58.0', '0.58', False), ('0.58.1-plus', '0.58.1', False), ('0.58.1', '0.58.1-plus', False),
    ('0.67-cp-mt', '0.67', False), ('', '0.58.1', False), ('latest', '0.58.1', False),
]


class NewerReleaseTests(unittest.TestCase):
    def test_newer_release(self):
        for tag, current, newer in CASES:
            with self.subTest(tag=tag, current=current):
                self.assertEqual(is_newer_release(tag, current), newer)


if __name__ == '__main__':
    unittest.main()
