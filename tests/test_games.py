"""The supported releases: EU CUSA08809 1.01 and US CUSA08034 1.00, the same eboot build."""
from paths import ROOT
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import sotc_cpu
import sotc_modes

sys.path.insert(0, str(ROOT / 'launcher'))
import sotc_discovery  # noqa: E402
try:
    import sotc_launcher_win as launcher  # noqa: E402
except ImportError:  # no Tkinter
    launcher = None

EU, US = 'CUSA08809', 'CUSA08034'


def sfo_bytes(values):
    """A param.sfo with string values, as prepare.sfo parses it."""
    keys = b''.join(k.encode() + b'\0' for k in values)
    keys += b'\0' * (-len(keys) % 4)
    entries, data, key_off = b'', b'', 0
    for key, value in values.items():
        raw = value.encode() + b'\0'
        size = len(raw) + (-len(raw) % 4)
        entries += struct.pack('<HHIII', key_off, 0x204, len(raw), size, len(data))
        data += raw.ljust(size, b'\0')
        key_off += len(key) + 1
    keys_at = 20 + len(entries)
    return struct.pack('<4sIIII', b'\0PSF', 0x101, keys_at, keys_at + len(keys), len(values)) + entries + keys + data


def make_game(folder, title_id, eboot=b'eboot'):
    folder = Path(folder)
    (folder / 'sce_sys').mkdir(parents=True)
    (folder / 'sce_sys' / 'param.sfo').write_bytes(sfo_bytes({'TITLE_ID': title_id, 'APP_VER': '01.00',
                                                              'TITLE': 'SHADOW OF THE COLOSSUS'}))
    (folder / 'eboot.bin').write_bytes(eboot)
    return folder


def make_dlc(folder, content_id):
    folder = Path(folder)
    (folder / 'sce_sys').mkdir(parents=True)
    (folder / 'sce_sys' / 'param.sfo').write_bytes(sfo_bytes({'CATEGORY': 'ac', 'CONTENT_ID': content_id}))
    return folder


def make_pkg(path, content_id):
    header = bytearray(0x80)
    header[:4] = b'\x7fCNT'
    header[0x40:0x40 + len(content_id)] = content_id.encode()
    header[0x74:0x78] = b'\0\0\0\x1b'
    Path(path).write_bytes(bytes(header))


class GamesTests(unittest.TestCase):
    def test_both_releases(self):
        self.assertEqual(sotc_modes.GAMES, {EU: ('EU', '01.01'), US: ('US', '01.00')})

    def test_title_id_and_serial(self):
        with tempfile.TemporaryDirectory() as tmp:
            us = make_game(Path(tmp) / 'CUSA08034-app', US)
            other = make_game(Path(tmp) / 'other', 'CUSA00001')
            self.assertEqual(sotc_modes.title_id(us), US)
            self.assertEqual(sotc_modes.game_serial(us), US)
            self.assertEqual(sotc_modes.game_serial(other), EU)
            self.assertEqual(sotc_modes.title_id(Path(tmp) / 'missing'), '')

    def test_save_template_falls_back_to_the_other_release(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / EU).mkdir()
            self.assertEqual(sotc_modes.save_template(tmp, US), Path(tmp) / EU)
            (Path(tmp) / US).mkdir()
            self.assertEqual(sotc_modes.save_template(tmp, US), Path(tmp) / US)
            self.assertIsNone(sotc_modes.save_template(Path(tmp) / 'none', US))


class CpuPatchTests(unittest.TestCase):
    def red_zone_count(self, title_id):
        ph = [{'type': 1, 'vaddr': 0, 'filesz': 0x1000, 'memsz': 0x1000},
              {'type': sotc_cpu.PT_GNU_EH_FRAME, 'vaddr': 0x100, 'filesz': 0x10, 'memsz': 0x10}]
        with tempfile.TemporaryDirectory() as tmp:
            game = make_game(Path(tmp) / 'game', title_id)
            with patch.object(sotc_cpu, 'parse_self', return_value=(b'', None, ph, [], [])), \
                    patch.object(sys, 'argv', ['sotc_cpu.py', str(game), '--out', tmp]):
                sotc_cpu.main()
            return struct.unpack_from('<8sQQQ', (Path(tmp) / 'sotc-cpu.bin').read_bytes())[3]

    def test_red_zone_functions_for_both_releases(self):
        self.assertEqual(self.red_zone_count(EU), 3)
        self.assertEqual(self.red_zone_count(US), 3)
        self.assertEqual(self.red_zone_count('CUSA00001'), 0)


class DiscoveryTests(unittest.TestCase):
    def test_finds_the_us_dump(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = make_game(Path(tmp) / 'USA' / 'CUSA08034-app', US)
            self.assertEqual(sotc_discovery.find_game_in(tmp), str(game))

    def test_eu_first_when_both(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_game(Path(tmp) / 'a' / 'CUSA08034-app', US)
            eu = make_game(Path(tmp) / 'b' / EU, EU)
            self.assertEqual(sotc_discovery.find_game_in(tmp), str(eu))

    def test_dlc_of_the_same_region_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'addcont'
            make_dlc(root / EU / 'ANCIENTBOW000001', 'EP9000-CUSA08809_00-ANCIENTBOW000001')
            make_pkg(root / EU / 'bow.pkg', 'EP9000-CUSA08809_00-LIFESWORD0000001')
            self.assertEqual(sotc_discovery.dlc_count(root, EU), 2)
            self.assertEqual(sotc_discovery.dlc_count(root, US), 0)
            make_dlc(root / US / 'CLOAKOFFATE00001', 'UP9000-CUSA08034_00-CLOAKOFFATE00001')
            make_pkg(root / US / 'steed.pkg', 'UP9000-CUSA08034_00-SPOTTEDSTEED0001')
            self.assertEqual(sotc_discovery.dlc_count(root, US), 2)
            self.assertEqual(sotc_discovery.dlc_serial(root / EU / 'ANCIENTBOW000001'), EU)

    def test_dlc_root_follows_the_game_region(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp) / 'games' / 'sotc'  # the search also looks two folders up
            game = make_game(Path(tmp) / 'CUSA08034-app', US)
            make_dlc(Path(tmp) / 'dlc' / 'CLOAKOFFATE00001', 'EP9000-CUSA08809_00-CLOAKOFFATE00001')
            self.assertEqual(sotc_discovery.find_dlc_root(game), '')
            make_dlc(Path(tmp) / 'dlc-us' / US / 'CLOAKOFFATE00001', 'UP9000-CUSA08034_00-CLOAKOFFATE00001')
            self.assertEqual(sotc_discovery.find_dlc_root(game), str(Path(tmp) / 'dlc-us'))


class SerialGateTests(unittest.TestCase):
    """Every SotC workaround must apply to both releases. A serial literal anywhere else turns one
    on for a single release: the early-Z decal fix stayed EU only that way, in a file grep skipped
    as binary because of a NUL byte."""
    FOLDERS = ('gpu/shadps4', 'gpu/shim', 'gpu/sotc', 'src')
    ALLOWED = {'gpu/shadps4/common/elf_info.h',  # IsSotc(): the one list of SotC serials
               'src/runtime_content.c'}          # the DLC title ID until probe.c sets it

    @staticmethod
    def sources(folders, suffixes):
        for folder in folders:
            for path in sorted((ROOT / folder).rglob('*')):
                if path.suffix in suffixes and path.is_file():
                    yield path

    def test_serials_only_in_issotc(self):
        found = {path.relative_to(ROOT).as_posix()
                 for path in self.sources(self.FOLDERS, ('.c', '.h', '.cpp', '.hpp', '.inc'))
                 if b'"CUSA' in path.read_bytes()}
        self.assertEqual(sorted(found - self.ALLOWED), [])

    def test_no_nul_bytes(self):
        found = [path.relative_to(ROOT).as_posix()
                 for path in self.sources((*self.FOLDERS, 'scripts', 'launcher'),
                                          ('.c', '.h', '.cpp', '.hpp', '.inc', '.py'))
                 if b'\0' in path.read_bytes()]
        self.assertEqual(found, [])


@unittest.skipIf(launcher is None, 'launcher not importable')
class LauncherTests(unittest.TestCase):
    def test_version_shown_and_support(self):
        with tempfile.TemporaryDirectory() as tmp:
            eu = make_game(Path(tmp) / EU, EU)
            us = make_game(Path(tmp) / 'CUSA08034-app', US)
            with patch.object(launcher, 'eboot_supported', return_value=True):
                self.assertEqual(launcher.game_info(eu), (EU, '01.01'))
                self.assertEqual(launcher.game_info(us), (US, '01.00'))
                self.assertTrue(launcher.game_supported(us))
                self.assertEqual(launcher.game_serial(us), US)
            with patch.object(launcher, 'eboot_supported', return_value=False):
                self.assertFalse(launcher.game_supported(us))
            self.assertEqual(launcher.game_serial(Path(tmp) / 'missing'), EU)
            self.assertEqual(launcher.supported_games(), 'CUSA08809 01.01, CUSA08034 01.00')


if __name__ == '__main__':
    unittest.main()
