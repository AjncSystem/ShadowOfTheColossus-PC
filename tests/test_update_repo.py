"""sotc: the launcher updates from this repository's releases (AjncSystem), never a fork's.

The 0.72.1-opt13 merge (v0.73) brought Oussama268's release URL into sotc_update.py: the
launchers of v0.73 and v0.74 then looked for updates in that fork and never offered v0.74.
"""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'launcher'))

import sotc_update  # noqa: E402


class UpdateRepositoryTest(unittest.TestCase):
    def test_release_urls_point_to_this_repository(self):
        repo = 'AjncSystem/ShadowOfTheColossus-PC'
        self.assertIn(f'api.github.com/repos/{repo}/releases/latest', sotc_update.RELEASES_URL)
        self.assertIn(f'github.com/{repo}/releases', sotc_update.RELEASES_PAGE)
        self.assertEqual(sotc_update.REPOSITORY, repo)
        self.assertFalse((Path(sotc_update.__file__).parent / 'update_repo.txt').exists(),
                         'update_repo.txt belongs to a package, never to the source tree')

    def test_a_package_can_name_its_own_repository(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(sotc_update.package_repository(folder), sotc_update.REPOSITORY)
            name = Path(folder) / 'update_repo.txt'
            name.write_text('Someone/SotC-Fork\n', encoding='utf-8')
            self.assertEqual(sotc_update.package_repository(folder), 'Someone/SotC-Fork')
            api, page = sotc_update.release_urls('Someone/SotC-Fork')
            self.assertEqual(api, 'https://api.github.com/repos/Someone/SotC-Fork/releases/latest')
            self.assertEqual(page, 'https://github.com/Someone/SotC-Fork/releases')
            for bad in ('', 'https://evil.example/x', 'a/b/c', 'a b/c', '../x'):
                name.write_text(bad, encoding='utf-8')
                self.assertEqual(sotc_update.package_repository(folder), sotc_update.REPOSITORY, bad)


if __name__ == '__main__':
    unittest.main()
