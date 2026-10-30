"""sotc_modes.py: each patch family is checked against the eboot's bytes before it is written, and
patches.bin is BBPATCH3 (one group per family, the expected bytes next to the new ones)."""
from paths import ROOT  # noqa: F401  (puts scripts/ on sys.path)
import contextlib
import io
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import sotc_modes as m

BASE = m.BASE


def entry(address, expected, data):
    """What patches.bin holds for a write: the expected bytes of the overwritten span."""
    return (address - BASE, expected[:len(data)], data)


def fake_eboot(spots):
    """A fake eboot from {PS4 address: bytes}. Each spot is its own PT_LOAD segment, so the real
    image_bytes() reads it."""
    elf, ph = bytearray(), []
    for address, raw in spots.items():
        ph.append({'type': 1, 'vaddr': address - BASE, 'offset': len(elf), 'filesz': len(raw)})
        elf += raw
    return bytes(elf), ph


def spots_for(modes=(), families=(), render=False):
    """The bytes of a supported eboot: the version checks, the expected bytes of the given modes,
    of the given (address, expected) families and of the render sites."""
    spots = dict(m.VERSION_CHECKS)
    for mode in modes:
        for address, expected, _ in m.MODES.get(mode, []):
            spots[address] = expected
    for address, expected in families:
        spots[address] = expected
    if render:
        for address, _, original in m.RENDER_SITES:
            spots[address] = bytes.fromhex(original)
    return spots


def parse_bbpatch3(blob):
    """(base, entries, end) of a BBPATCH3 blob: entries are (group, offset, expected, data), end the
    offset just past the last entry."""
    magic, base, count = struct.unpack_from('<8sQQ', blob, 0)
    if magic != b'BBPATCH3':
        raise ValueError(f'bad magic {magic!r}')
    pos, entries = 24, []
    for _ in range(count):
        offset, length, group, reserved = struct.unpack_from('<QQII', blob, pos)
        if reserved != 0:
            raise ValueError('reserved field is not 0')
        pos += 24
        expected = blob[pos:pos + length]
        pos += length
        data = blob[pos:pos + length]
        pos += length
        entries.append((group, offset, expected, data))
    return base, entries, pos


class Clean(unittest.TestCase):
    def setUp(self):
        env = patch.dict(os.environ, {}, clear=True)  # SOTC_* switches off, as in a default launch
        env.start()
        self.addCleanup(env.stop)


class BuildTests(Clean):
    def test_matching_mode_written(self):
        elf, ph = fake_eboot(spots_for(modes=['fps60game']))
        log = []
        groups, cvars = m.build_patches(elf, ph, 'fps60game', log=log.append)
        self.assertEqual(groups, [[entry(a, e, d) for a, e, d in m.MODES['fps60game']]])
        self.assertEqual(cvars, ['TargetFramesPerSeconds=1'])
        self.assertIn('Mode fps60game: on', log)

    def test_mismatched_mode_skipped_as_a_whole(self):
        spots = spots_for(modes=['fps60game'])
        spots[0x0089a47f] = bytes.fromhex('c7458402000001')  # the last byte differs (02 expected)
        elf, ph = fake_eboot(spots)
        log = []
        groups, cvars = m.build_patches(elf, ph, 'fps60game', log=log.append)
        self.assertEqual(groups, [])  # the first entry matches, but the mode is all or nothing
        self.assertEqual(cvars, [])  # its cvar goes with it
        self.assertIn('modes: fps60game skipped: bytes at 0x89a47f differ', log)

    def test_mismatched_mode_does_not_stop_the_others(self):
        spots = spots_for(modes=['fps60game', 'grassbuf'])
        spots[0x0089a47f] = bytes.fromhex('c7458402000001')
        elf, ph = fake_eboot(spots)
        groups, cvars = m.build_patches(elf, ph, 'fps60game,grassbuf', log=[].append)
        self.assertEqual(groups, [[entry(a, e, d) for a, e, d in m.MODES['grassbuf']]])
        self.assertEqual(cvars, [])

    def test_other_game_version_writes_nothing(self):
        elf, ph = fake_eboot({0x01390090: b'\x00' * 6})  # no version check bytes
        log = []
        groups, cvars = m.build_patches(elf, ph, 'fps60,fps60game', log=log.append)
        self.assertEqual((groups, cvars), ([], []))
        self.assertIn('Mode fps60: this eboot is not EU 1.01 / US 1.00, not applied', log)

    def test_each_family_is_its_own_group(self):
        families = [(m.POOL_FIX[0], m.POOL_FIX[1]), (m.PSTATS_STORE[0], m.PSTATS_STORE[1])]
        elf, ph = fake_eboot(spots_for(modes=['grassbuf'], families=families))
        groups, _ = m.build_patches(elf, ph, 'grassbuf', pstats=True, log=[].append)
        self.assertEqual(groups[0], [entry(*m.POOL_FIX)])
        self.assertEqual(groups[1], [entry(*m.PSTATS_STORE)])
        self.assertEqual(len(groups[2]), len(m.MODES['grassbuf']))
        numbers = {g for g, *_ in parse_bbpatch3(m.patches_blob(groups))[1]}
        self.assertEqual(numbers, {1, 2, 3})

    def test_mismatched_pool_fix_not_written(self):
        families = [(m.POOL_FIX[0], m.POOL_FIX[1][:-1] + b'\x01')]
        elf, ph = fake_eboot(spots_for(families=families))
        groups, _ = m.build_patches(elf, ph, '', log=[].append)
        self.assertEqual(groups, [])

    def test_render_1080_writes_nothing(self):
        elf, ph = fake_eboot(spots_for(render=True))
        self.assertEqual(m.build_patches(elf, ph, '', '1920x1080', log=[].append), ([], []))

    def test_render_group_expected_bytes_are_the_eboot_bytes(self):
        families = [(m.POOL_FIX[0], m.POOL_FIX[1])]
        spots = spots_for(families=families, render=True)
        elf, ph = fake_eboot(spots)
        log = []
        groups, cvars = m.build_patches(elf, ph, '', '3840x1600', log=log.append)
        self.assertEqual(len(groups), 2)
        render = groups[1]
        self.assertEqual(len(render), len(m.RENDER_SITES))
        for offset, expected, data in render:
            self.assertEqual(m.image_bytes(elf, ph, offset, len(expected)), expected)
            self.assertEqual(len(data), len(expected))
        # the 'W' site at 0x495bf7: the 4 bytes after the opcode now hold 3840
        entry = next(e for e in render if e[0] == 0x00495bf8 - BASE)
        self.assertEqual(entry[1], bytes.fromhex('80070000'))
        self.assertEqual(entry[2], struct.pack('<I', 3840))
        # above LOW_MEM_PIXELS with the pool fix on: the texture pool reserve goes to 0
        self.assertIn('texturePoolReserveSystemMemMiBs=0', cvars)
        self.assertIn('Render size 3840x1600: on', log)

    def test_render_site_mismatch_exits(self):
        spots = spots_for(render=True)
        spots[0x00495bf7] = bytes.fromhex('b880070001')
        elf, ph = fake_eboot(spots)
        with self.assertRaises(SystemExit):
            m.build_patches(elf, ph, '', '3840x1600', log=[].append)


class FileTests(Clean):
    def test_patches_blob_round_trip(self):
        groups = [[(0x41a321 - BASE, b'\x01\x02\x03', b'\x04\x05\x06')],
                  [(0x10, b'ab', b'cd'), (0x20, b'\x00\xff', b'\xaa\xbb')]]
        blob = m.patches_blob(groups)
        base, entries, end = parse_bbpatch3(blob)
        self.assertEqual(base, BASE)
        self.assertEqual(end, len(blob))
        self.assertEqual(entries, [(1, 0x41a321 - BASE, b'\x01\x02\x03', b'\x04\x05\x06'),
                                   (2, 0x10, b'ab', b'cd'), (2, 0x20, b'\x00\xff', b'\xaa\xbb')])
        self.assertEqual(blob[:8], b'BBPATCH3')
        self.assertEqual(struct.unpack_from('<Q', blob, 16)[0], 3)

    def test_empty_patch_file(self):
        blob = m.patches_blob([])
        self.assertEqual(len(blob), 24)
        self.assertEqual(parse_bbpatch3(blob), (BASE, [], 24))

    def test_length_mismatch_rejected(self):
        with self.assertRaises(ValueError):
            m.patches_blob([[(0, b'ab', b'c')]])

    def run_main(self, game, out, modes, elf, ph):
        out.mkdir(exist_ok=True)
        with patch.object(m, 'parse_self', return_value=(elf, None, ph, None, None)), \
                patch.object(sys, 'argv', ['sotc_modes.py', str(game), '--out', str(out), '--modes', modes]):
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                m.main()
        return stdout.getvalue()

    def test_main_writes_bbpatch3_for_matching_mode(self):
        with tempfile.TemporaryDirectory() as tmp:
            game, out = Path(tmp) / 'game', Path(tmp) / 'out'
            game.mkdir()
            (game / 'eboot.bin').write_bytes(b'not read: parse_self is patched')
            elf, ph = fake_eboot(spots_for(modes=['fps60game']))
            text = self.run_main(game, out, 'fps60game', elf, ph)
            base, entries, end = parse_bbpatch3((out / 'patches.bin').read_bytes())
            self.assertEqual(base, BASE)
            self.assertEqual(end, (out / 'patches.bin').stat().st_size)
            self.assertEqual([g for g, *_ in entries], [1, 1, 1])
            self.assertEqual([(o, e, d) for _, o, e, d in entries],
                             [entry(a, e, d) for a, e, d in m.MODES['fps60game']])
            self.assertIn('Mode fps60game: on', text)
            cmd = (out / 'cmdline.bin').read_bytes()
            self.assertEqual(cmd[:8], b'SOTCCMD1')
            self.assertTrue(cmd.endswith(b'+TargetFramesPerSeconds=1'))

    def test_main_skips_mismatched_mode(self):
        with tempfile.TemporaryDirectory() as tmp:
            game, out = Path(tmp) / 'game', Path(tmp) / 'out'
            game.mkdir()
            (game / 'eboot.bin').write_bytes(b'x')
            spots = spots_for(modes=['fps60game'])
            spots[0x00b0a7a6] = bytes.fromhex('c745b800000000')
            elf, ph = fake_eboot(spots)
            text = self.run_main(game, out, 'fps60game', elf, ph)
            _, entries, end = parse_bbpatch3((out / 'patches.bin').read_bytes())
            self.assertEqual(entries, [])
            self.assertEqual(end, 24)
            self.assertIn('modes: fps60game skipped: bytes at 0xb0a7a6 differ', text)
            self.assertFalse((out / 'cmdline.bin').exists())


if __name__ == '__main__':
    unittest.main()
