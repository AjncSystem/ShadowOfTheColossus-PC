"""sotc: the launcher updates from this repository's releases (AjncSystem), never a fork's.

The 0.72.1-opt13 merge (v0.73) brought Oussama268's release URL into sotc_update.py: the
launchers of v0.73 and v0.74 then looked for updates in that fork and never offered v0.74.
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'launcher'))

import sotc_update  # noqa: E402


class UpdateRepositoryTest(unittest.TestCase):
    def test_release_urls_point_to_this_repository(self):
        repo = 'AjncSystem/ShadowOfTheColossus-PC'
        self.assertIn(f'api.github.com/repos/{repo}/releases/latest', sotc_update.RELEASES_URL)
        self.assertIn(f'github.com/{repo}/releases', sotc_update.RELEASES_PAGE)


if __name__ == '__main__':
    unittest.main()
