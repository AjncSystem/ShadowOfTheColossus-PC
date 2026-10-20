"""Launcher self-update (launcher/sotc_update.py) with a local test release."""
from paths import ROOT
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(ROOT / 'launcher'))
import sotc_update  # noqa: E402


def release_zip(path, files):
    with zipfile.ZipFile(path, 'w') as archive:
        for name, data in files.items():
            archive.writestr(name, data)


class VersionTests(unittest.TestCase):
    def test_order(self):
        key = sotc_update.version_key
        self.assertGreater(key('0.57-opt6'), key('0.57-opt5'))
        self.assertGreater(key('0.58'), key('0.57-opt9'))
        self.assertGreater(key('v0.58-opt1'), key('0.58'))
        self.assertEqual(key('0.57-opt5'), key('v0.57-opt5'))

    def test_is_newer(self):
        self.assertTrue(sotc_update.is_newer({'version': '0.57-opt6'}, '0.57-opt5'))
        self.assertFalse(sotc_update.is_newer({'version': '0.57-opt5'}, '0.57-opt5'))
        self.assertFalse(sotc_update.is_newer(None, '0.57-opt5'))


class DownloadTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)
        self.old_url = os.environ.get('SOTC_UPDATE_URL')
        self.addCleanup(self.restore_url)

    def restore_url(self):
        if self.old_url is None:
            os.environ.pop('SOTC_UPDATE_URL', None)
        else:
            os.environ['SOTC_UPDATE_URL'] = self.old_url

    def publish(self, files, digest=None, size=None):
        zip_path = self.dir / 'SotC-PC.zip'
        release_zip(zip_path, files)
        data = zip_path.read_bytes()
        meta = {'tag_name': 'v9.99-opt1', 'name': 'test', 'body': 'notes', 'html_url': 'about:blank',
                'assets': [{'name': 'SotC-PC.zip', 'browser_download_url': zip_path.as_uri(),
                            'size': len(data) if size is None else size,
                            'digest': 'sha256:' + (digest or hashlib.sha256(data).hexdigest())}]}
        (self.dir / 'latest.json').write_text(json.dumps(meta))
        os.environ['SOTC_UPDATE_URL'] = (self.dir / 'latest.json').as_uri()
        return sotc_update.latest_release()

    def test_good_release(self):
        release = self.publish({'SotC-PC/SotC.exe': b'exe', 'SotC-PC/bin/a.dll': b'dll'})
        self.assertEqual(release['version'], '9.99-opt1')
        new = sotc_update.download(release, self.dir / 'update')
        self.assertEqual((new / 'bin' / 'a.dll').read_bytes(), b'dll')
        self.assertFalse((self.dir / 'update' / 'SotC-PC.zip.part').exists())

    def test_bad_digest(self):
        release = self.publish({'SotC-PC/SotC.exe': b'exe'}, digest='0' * 64)
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            sotc_update.download(release, self.dir / 'update')

    def test_bad_size(self):
        release = self.publish({'SotC-PC/SotC.exe': b'exe'}, size=1)
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            sotc_update.download(release, self.dir / 'update')

    def test_wrong_layout(self):
        release = self.publish({'Other/SotC.exe': b'exe'})
        with self.assertRaisesRegex(ValueError, 'not a SotC-PC release'):
            sotc_update.download(release, self.dir / 'update')

    def test_path_escape(self):
        release = self.publish({'SotC-PC/SotC.exe': b'exe', 'SotC-PC/../evil.txt': b'x'})
        with self.assertRaisesRegex(ValueError, 'unexpected path'):
            sotc_update.download(release, self.dir / 'update')


class ApplyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.port, self.new = root / 'port', root / 'new' / 'SotC-PC'
        for base, files in ((self.port, {'SotC.exe': b'old exe', 'bin/a.dll': b'old dll', 'sotc.ini': b'mine',
                                         'user/save.dat': b'save', 'extra.txt': b'kept'}),
                            (self.new, {'SotC.exe': b'new exe', 'bin/a.dll': b'new dll', 'bin/b.dll': b'added',
                                        'sotc.ini': b'default', 'user/save.dat': b'overwritten?'})):
            for name, data in files.items():
                path = base / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)

    def test_apply_keeps_user_files(self):
        self.assertTrue(sotc_update.apply_update(self.new, self.port, restart=False))
        read = lambda name: (self.port / name).read_bytes()  # noqa: E731
        self.assertEqual(read('SotC.exe'), b'new exe')
        self.assertEqual(read('bin/a.dll'), b'new dll')
        self.assertEqual(read('bin/b.dll'), b'added')
        self.assertEqual(read('sotc.ini'), b'mine')
        self.assertEqual(read('user/save.dat'), b'save')
        self.assertEqual(read('extra.txt'), b'kept')
        self.assertIsNone(sotc_update.cleanup(self.port))
        self.assertFalse((self.port / 'update' / 'backup').exists())

    def test_failure_restores_everything(self):
        # bin/c.dll cannot be written (a folder of that name is in the way) after SotC.exe and
        # bin/a.dll were already replaced: both come back and the added bin/b.dll goes.
        (self.new / 'bin' / 'c.dll').write_bytes(b'blocked')
        (self.port / 'bin' / 'c.dll').mkdir()
        self.assertFalse(sotc_update.apply_update(self.new, self.port, restart=False))
        self.assertEqual((self.port / 'SotC.exe').read_bytes(), b'old exe')
        self.assertEqual((self.port / 'bin' / 'a.dll').read_bytes(), b'old dll')
        self.assertFalse((self.port / 'bin' / 'b.dll').exists())
        self.assertIn('', sotc_update.cleanup(self.port))  # the error is reported once
        self.assertIsNone(sotc_update.cleanup(self.port))


if __name__ == '__main__':
    unittest.main()
