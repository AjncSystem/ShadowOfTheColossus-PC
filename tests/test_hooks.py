"""Engine hooks: the SOTCHOOK1 writer and reader (scripts/sotc_hooks.py), the eboot fingerprint, the
declaration loader, and the resolver of tools/re/hooks_check.py on synthetic code and on out/eboot.elf
when the prepared image is present."""
from paths import ROOT  # noqa: F401  (puts scripts/ on sys.path)
import contextlib
import hashlib
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(ROOT / 'tools' / 're'))
import sotc_hooks as H  # noqa: E402
import hooks_check as C  # noqa: E402

EBOOT = ROOT / 'out' / 'eboot.elf'
EU_FINGERPRINT = '72c1d4a99c42e34dc0c26d714a7137aceb33616195d8a8d23087ae167e3bb225'
FP = 'ab' * 32       # a fingerprint that is not the EU one
OTHER = 'cd' * 32
CODE = 0x1000        # synthetic code segment base
FUNCTIONS = ((0x1000, 0x1040), (0x1040, 0x1100))


def hook(**fields):
    base = dict(name='SAMPLE', mode='entry', pattern='48 8b ?? 10')
    base.update(fields)
    return base


def build(*hooks, fingerprint=FP):
    return H.build([(f'{i}.py', h) for i, h in enumerate(hooks)], fingerprint)


def image(code=None, data=b'', functions=FUNCTIONS):
    segments = [(CODE, bytes(code if code is not None else 0x100), True)]
    if data:
        segments.append((0x8000, bytes(data), False))
    return C.Segments(segments, functions)


def tiny_elf(segments):
    """A plain ELF64 with program headers only: segments = [(p_type, vaddr, data)]."""
    count = len(segments)
    offset = 64 + 56 * count
    headers, body = [], b''
    for kind, vaddr, data in segments:
        headers.append(struct.pack('<IIQQQQQQ', kind, 4, offset, vaddr, vaddr, len(data), len(data), 0x1000))
        body += data
        offset += len(data)
    head = struct.pack('<16sHHIQQQIHHHHHH', b'\x7fELF\x02\x01\x01' + bytes(9), 2, 62, 1, 0, 64, 0, 0,
                       64, 56, count, 0, 0, 0)
    return head + b''.join(headers) + body


class FormatTests(unittest.TestCase):
    def test_header_and_first_record_bytes(self):
        records, known = build(hook(expected={FP: 0x1000},
                                    derived=[dict(field='disp', offset=2, size=1, kind='disp', insn_end=4)]))
        data = H.encode(FP, known, records)
        self.assertEqual(data[:8], b'SOTCHOOK')
        self.assertEqual(struct.unpack_from('<II', data, 8), (1, 1))             # version, count
        self.assertEqual(data[16:48], bytes.fromhex(FP))                           # fingerprint
        self.assertEqual(struct.unpack_from('<II', data, 48), (1, 0))             # known, reserved
        self.assertEqual(data[56:62], b'SAMPLE')
        self.assertEqual(data[62:104], bytes(42))                                 # name is NUL padded
        self.assertEqual(data[104:120], b'SOTC_HOOK_SAMPLE')                       # switch
        self.assertEqual(data[168:174], b'SAMPLE')                                # group
        # mode, pattern_len, expected, range_len, anchor_kind, anchor_index
        self.assertEqual(struct.unpack_from('<IIQQII', data, 200), (0, 4, 0x1000, 0, 0, 0))
        self.assertEqual(struct.unpack_from('<II', data, 232), (0, 1))            # no deps, one derived
        self.assertEqual(data[240:245], b'disp\0')                                # derived field
        self.assertEqual(struct.unpack_from('<IIII', data, 272), (2, 1, 1, 4))    # offset, size, kind, insn_end
        self.assertEqual(data[288:296], bytes([0x48, 0x8b, 0, 0x10, 0xff, 0xff, 0, 0xff]))
        self.assertEqual(len(data), 56 + 240)

    def test_round_trip(self):
        records, known = build(
            hook(name='FIRST', pattern='55 48 89 e5 be 28 00 00 00', expected={FP: 0x1020},
                 derived=[dict(field='event', offset=5, size=4, kind='imm', insn_end=9)]),
            hook(name='SECOND', mode='range', pattern='e8 ?? ?? ?? ??', range_len=0x20,
                 anchor='FIRST', deps=['FIRST']),
            hook(name='THIRD', mode='wrap', pattern='55 48 89 e5', deps=['SECOND']))
        data = H.encode(FP, known, records)
        decoded = H.decode(data)
        self.assertEqual(decoded['fingerprint'], FP)
        self.assertTrue(decoded['known'])
        self.assertEqual(decoded['records'], records)
        self.assertEqual(H.encode(FP, known, decoded['records']), data)

    def test_other_fingerprint_gets_no_expected_offsets(self):
        records, known = build(hook(expected={FP: 0x1020}), fingerprint=OTHER)
        self.assertFalse(known)
        self.assertEqual(records[0]['expected'], 0)
        self.assertFalse(H.decode(H.encode(OTHER, known, records))['known'])

    def test_truncated_and_foreign_files_are_refused(self):
        records, known = build(hook(name='ONE'))
        data = H.encode(FP, known, records)
        with self.assertRaises(ValueError):
            H.decode(data[:-1])
        with self.assertRaises(ValueError):
            H.decode(data + b'\0')
        with self.assertRaises(ValueError):
            H.decode(b'BBPATCH3' + data[8:])


class FingerprintTests(unittest.TestCase):
    def test_matches_the_documented_formula(self):
        code, rodata = b'\xcc' * 48, b'\x01\x02\x03\x04'
        elf = tiny_elf([(1, 0x400000, code), (0x61000010, 0x500000, rodata)])
        digest = hashlib.sha256()
        digest.update(struct.pack('<QQ', 0x400000, len(code)) + code)
        digest.update(struct.pack('<QQ', 0x500000, len(rodata)) + rodata)
        self.assertEqual(H.fingerprint(elf), digest.hexdigest())

    def test_only_load_and_relro_count_and_order_matters(self):
        code = b'\x90' * 16
        plain = tiny_elf([(1, 0x400000, code)])
        with_note = tiny_elf([(4, 0x1000, b'note-one'), (1, 0x400000, code)])
        with_other_note = tiny_elf([(4, 0x1000, b'note-two!'), (1, 0x400000, code)])
        self.assertEqual(H.fingerprint(with_note), H.fingerprint(plain))
        self.assertEqual(H.fingerprint(with_note), H.fingerprint(with_other_note))
        self.assertNotEqual(H.fingerprint(tiny_elf([(1, 0x400000, code)])),
                            H.fingerprint(tiny_elf([(1, 0x400000, b'\x91' * 16)])))
        a, b = (1, 0x400000, code), (0x61000010, 0x500000, b'\x05' * 8)
        self.assertNotEqual(H.fingerprint(tiny_elf([a, b])), H.fingerprint(tiny_elf([b, a])))


class DeclarationTests(unittest.TestCase):
    def test_pattern_text(self):
        self.assertEqual(H.parse_pattern('48 8b ?? 10'), (b'\x48\x8b\x00\x10', b'\xff\xff\x00\xff'))
        for bad in ('4g', '', '?? ??', '48 8b 1', 5):
            with self.subTest(pattern=bad), self.assertRaises(H.DeclarationError):
                H.parse_pattern(bad)

    def test_defaults_for_switch_and_group(self):
        records, _ = build(hook(name='Label_Writer'))
        self.assertEqual(records[0]['switch'], 'SOTC_HOOK_LABEL_WRITER')
        self.assertEqual(records[0]['group'], 'Label_Writer')

    def test_invalid_declarations_are_named(self):
        cases = {
            'unknown key': hook(colour='red'),
            'bad name': hook(name='9lives'),
            'bad mode': hook(mode='jump'),
            'bad switch': hook(switch='HOOK_X'),
            'range without length': hook(mode='range'),
            'length without range': hook(range_len=8),
            'bad fingerprint key': hook(expected={'EU': 0x1000}),
            'zero offset': hook(expected={FP: 0}),
            'missing anchor': hook(anchor='NOPE'),
            'self dependency': hook(deps=['SAMPLE']),
            'derived past the pattern': hook(derived=[dict(field='x', offset=3, size=4, kind='imm')]),
            'derived size': hook(derived=[dict(field='x', offset=0, size=3, kind='imm')]),
            'derived kind': hook(derived=[dict(field='x', offset=0, size=1, kind='ptr')]),
            'list as mode': hook(mode=['entry']),
            'list as kind': hook(derived=[dict(field='x', offset=0, size=1, kind=['imm'])]),
            'derived not a list': hook(derived={'field': 'x'}),
        }
        for label, declaration in cases.items():
            with self.subTest(case=label), self.assertRaises(H.DeclarationError):
                build(declaration)

    def test_duplicate_names_and_cycles_are_refused(self):
        with self.assertRaises(H.DeclarationError):
            build(hook(name='A'), hook(name='A'))
        with self.assertRaises(H.DeclarationError):
            build(hook(name='A', anchor='B'), hook(name='B', deps=['A']))

    def test_dependency_and_anchor_indices(self):
        records, _ = build(hook(name='A'), hook(name='B', anchor='A', deps=['A']), hook(name='C'))
        self.assertEqual((records[1]['anchor'], records[1]['deps']), (0, [0]))
        self.assertIsNone(records[2]['anchor'])


class LoaderTests(unittest.TestCase):
    def write(self, folder, name, text):
        (folder / name).write_text(text, encoding='ascii')

    def test_files_load_in_name_order_and_underscore_files_only_on_request(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            self.write(folder, 'b_second.py', "HOOKS = [dict(name='SECOND', mode='entry', pattern='c3')]\n")
            self.write(folder, 'a_first.py', "HOOKS = [dict(name='FIRST', mode='entry', pattern='c3')]\n")
            self.write(folder, '_skip.py', "raise SystemExit('must not be loaded')\n")
            self.assertEqual([h['name'] for _, h in H.load_declarations(folder)], ['FIRST', 'SECOND'])
            self.write(folder, '_skip.py', "HOOKS = [dict(name='SKIPPED', mode='entry', pattern='c3')]\n")
            names = [h['name'] for _, h in H.load_declarations(folder, include_examples=True)]
            # "_" sorts before letters, so the underscore file comes first
            self.assertEqual(names, ['SKIPPED', 'FIRST', 'SECOND'])

    def test_bad_files_are_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            self.write(folder, 'bad.py', "HOOKS = 'nope'\n")
            with self.assertRaises(H.DeclarationError):
                H.load_declarations(folder)
            self.write(folder, 'bad.py', "raise ValueError('boom')\n")
            with self.assertRaises(H.DeclarationError):
                H.load_declarations(folder)

    def test_shipped_example_is_ignored_by_default(self):
        # _example.py (name starts with "_") is loaded only with include_examples; real hook
        # files in scripts/hooks.d/ load either way, so test the example by name, not the count.
        default = {h['name'] for _, h in H.load_declarations(H.HOOKS_DIR)}
        with_examples = {h['name'] for _, h in H.load_declarations(H.HOOKS_DIR, include_examples=True)}
        self.assertEqual(with_examples - default, {'EXAMPLE_CS_DONE_LABEL', 'EXAMPLE_EVENT_SITE'})
        self.assertNotIn('EXAMPLE_CS_DONE_LABEL', default)

    def test_writer_end_to_end(self):
        elf = tiny_elf([(1, 0x400000, b'\xc3' * 32)])
        with tempfile.TemporaryDirectory() as tmp:
            game, out, hooks = Path(tmp) / 'game', Path(tmp) / 'out', Path(tmp) / 'hooks.d'
            game.mkdir()
            hooks.mkdir()
            (game / 'eboot.bin').write_bytes(elf)
            self.write(hooks, 'one.py', "HOOKS = [dict(name='ONE', mode='entry', pattern='c3', "
                                        "expected={'" + H.fingerprint(elf) + "': 0x400000})]\n")
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(H.main([str(game), '--out', str(out), '--hooks-dir', str(hooks)]), 0)
            decoded = H.decode((out / 'hooks.bin').read_bytes())
            self.assertEqual(decoded['fingerprint'], H.fingerprint(elf))
            self.assertTrue(decoded['known'])
            self.assertEqual(decoded['records'][0]['expected'], 0x400000)
            self.write(hooks, 'two.py', "HOOKS = [dict(name='ONE', mode='entry', pattern='c3')]\n")
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(H.main([str(game), '--out', str(out), '--hooks-dir', str(hooks)]), 1)


class ResolverTests(unittest.TestCase):
    def resolve(self, records, img):
        return {r.name: r for r in C.resolve(records, img)}

    def test_unique_match_is_scanned(self):
        code = bytearray(0x100)
        code[0x20:0x24] = b'\x48\x8b\x05\x10'
        records, _ = build(hook())
        result = self.resolve(records, image(code))['SAMPLE']
        self.assertEqual((result.state, result.site), ('scanned', CODE + 0x20))

    def test_expected_offset_is_used_when_the_pattern_is_there(self):
        code = bytearray(0x100)
        code[0x20:0x24] = b'\x48\x8b\x05\x10'
        records, _ = build(hook(expected={FP: CODE + 0x20}))
        self.assertEqual(self.resolve(records, image(code))['SAMPLE'].state, 'expected')
        records, _ = build(hook(expected={FP: CODE + 0x30}))
        result = self.resolve(records, image(code))['SAMPLE']
        self.assertEqual((result.state, result.site), ('scanned', CODE + 0x20))

    def test_two_matches_are_off(self):
        code = bytearray(0x100)
        code[0x10:0x14] = code[0x40:0x44] = b'\x48\x8b\x05\x10'
        records, _ = build(hook())
        result = self.resolve(records, image(code))['SAMPLE']
        self.assertEqual(result.state, 'off')
        self.assertIn('2 matches', result.reason)

    def test_overlapping_matches_are_counted(self):
        code = b'\xaa' * 4 + bytes(0xfc)
        records, _ = build(hook(pattern='aa aa'))
        self.assertIn('3 matches', self.resolve(records, image(code))['SAMPLE'].reason)

    def test_wildcards(self):
        code = bytearray(0x100)
        code[0x10:0x15] = b'\xe8\x01\x02\x03\x04'
        code[0x40:0x45] = b'\xe8\x05\x06\x07\x08'
        records, _ = build(hook(pattern='e8 01 ?? ?? ??'))
        self.assertEqual(self.resolve(records, image(code))['SAMPLE'].site, CODE + 0x10)
        records, _ = build(hook(pattern='e8 ?? ?? ?? ??'))
        self.assertEqual(self.resolve(records, image(code))['SAMPLE'].state, 'off')

    def test_data_segments_are_not_searched(self):
        records, _ = build(hook(pattern='de ad be ef'))
        result = self.resolve(records, image(data=b'\xde\xad\xbe\xef'))['SAMPLE']
        self.assertEqual(result.state, 'off')
        self.assertIn('0 matches', result.reason)

    def test_anchor_limits_the_scan_to_its_function(self):
        code = bytearray(0x100)
        code[0x00:0x05] = b'\xb8\x01\x00\x00\x00'                  # anchor, at the first function
        code[0x10:0x15] = b'\xbe\x28\x00\x00\x00'                  # inside the first function
        code[0x50:0x55] = b'\xbe\x28\x00\x00\x00'                  # inside the second function
        records, _ = build(hook(name='ANCHOR', pattern='b8 01 00 00 00'),
                           hook(name='SITE', mode='site', pattern='be 28 00 00 00', anchor='ANCHOR'))
        results = self.resolve(records, image(code))
        self.assertEqual((results['SITE'].state, results['SITE'].site), ('scanned', CODE + 0x10))
        records, _ = build(hook(name='SITE', mode='site', pattern='be 28 00 00 00'))
        self.assertIn('2 matches', self.resolve(records, image(code))['SITE'].reason)

    def test_anchor_and_dependency_failures_switch_the_hook_off(self):
        code = bytearray(0x100)
        code[0x10:0x15] = b'\xbe\x28\x00\x00\x00'
        records, _ = build(hook(name='ANCHOR', pattern='b8 77 66 55 44'),
                           hook(name='SITE', mode='site', pattern='be 28 00 00 00', anchor='ANCHOR'))
        self.assertIn('anchor ANCHOR off', self.resolve(records, image(code))['SITE'].reason)
        records, _ = build(hook(name='NEEDED', pattern='de ad c0 de'),
                           hook(name='SITE', mode='site', pattern='be 28 00 00 00', deps=['NEEDED']))
        self.assertIn('dependency NEEDED off', self.resolve(records, image(code))['SITE'].reason)

    def test_anchor_outside_every_function_is_off(self):
        code = bytearray(0x200)
        code[0x180:0x185] = b'\xb8\x01\x00\x00\x00'
        records, _ = build(hook(name='ANCHOR', pattern='b8 01 00 00 00'),
                           hook(name='SITE', mode='site', pattern='be 28 00 00 00', anchor='ANCHOR'))
        results = self.resolve(records, image(code, functions=((CODE, CODE + 0x40),)))
        self.assertEqual(results['SITE'].state, 'off')
        self.assertIn('not in a function', results['SITE'].reason)

    def test_derived_fields_are_decoded(self):
        code = bytearray(0x100)
        code[0x10:0x14] = b'\x48\x8b\x47\xf0'                              # mov rax,[rdi-0x10]
        code[0x20:0x27] = b'\x48\x8d\x05\x10\x00\x00\x00'                  # lea rax,[rip+0x10]
        code[0x30:0x35] = b'\xbe\x28\x00\x00\x00'                          # mov esi,0x28
        records, _ = build(
            hook(name='DISP', pattern='48 8b ?? ??',
                 derived=[dict(field='disp', offset=3, size=1, kind='disp', insn_end=4)]),
            hook(name='LEA', pattern='48 8d 05 ?? ?? ?? ??',
                 derived=[dict(field='target', offset=3, size=4, kind='rip_target', insn_end=7)]),
            hook(name='IMM', mode='site', pattern='be ?? ?? ?? ??',
                 derived=[dict(field='event', offset=1, size=4, kind='imm', insn_end=5)]))
        results = self.resolve(records, image(code))
        self.assertEqual(results['DISP'].values['disp'], -16)
        self.assertEqual(results['LEA'].values['target'], CODE + 0x20 + 7 + 0x10)
        self.assertEqual(results['IMM'].values['event'], 0x28)

    def test_range_length_must_stay_in_its_segment(self):
        code = bytearray(0x100)
        code[0xfc:0x100] = b'\x01\x02\x03\x04'
        records, _ = build(hook(name='SPAN', mode='range', pattern='01 02', range_len=8))
        result = self.resolve(records, image(code))['SPAN']
        self.assertEqual(result.state, 'off')
        self.assertIn('range', result.reason)
        records, _ = build(hook(name='SPAN', mode='range', pattern='01 02', range_len=2))
        result = self.resolve(records, image(code))['SPAN']
        self.assertEqual((result.state, result.range_end), ('scanned', CODE + 0xfc + 2))


@unittest.skipUnless(EBOOT.is_file(), 'out/eboot.elf is absent (prepare the game with sotc.py)')
class RealEbootTests(unittest.TestCase):
    def test_fingerprint_of_the_eu_eboot(self):
        self.assertEqual(H.fingerprint(EBOOT.read_bytes()), EU_FINGERPRINT)

    def test_declared_hooks_resolve_on_the_eu_eboot(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(C.main([]), 0)

    def test_examples_resolve_on_the_eu_eboot(self):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(C.main(['--include-examples']), 0)
        self.assertIn('SOTC HOOK EXAMPLE_CS_DONE_LABEL 0x925c0 expected', out.getvalue())
        self.assertIn('SOTC HOOK EXAMPLE_EVENT_SITE 0x925ca expected', out.getvalue())


if __name__ == '__main__':
    unittest.main()
