"""SotC Lab live switches: LIVE is a subset of TUNING, every live name has a SotcLive::Int
definition under gpu/, and the TSV export of tuning switches round-trips (scripts/tuning_export.py)."""
from paths import ROOT  # noqa: F401  (puts scripts/ on sys.path)
import contextlib
import io
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))

import sotc_tuning  # noqa: E402
import tuning_export  # noqa: E402

GPU = ROOT / 'gpu'
DEFINITION = re.compile(r'SotcLive::Int\s+\w+\s*\{\s*"([A-Z0-9_]+)"')


def live_definitions():
    """Names of every SotcLive::Int defined under gpu/ (sources and headers)."""
    names = set()
    for path in GPU.rglob('*'):
        if path.suffix not in ('.cpp', '.h', '.hpp', '.cc'):
            continue
        text = path.read_text(encoding='utf-8', errors='replace')
        # Comment lines (the usage example in sotc_live.h) do not count as definitions.
        code = '\n'.join(line for line in text.splitlines()
                        if not line.lstrip().startswith(('//', '/*', '*')))
        names.update(DEFINITION.findall(code))
    return names


def parse_tsv(text):
    """Reads the export back: (groups, switches) with every field unescaped."""

    def unescape(field):
        out = []
        i = 0
        while i < len(field):
            ch = field[i]
            if ch == '\\' and i + 1 < len(field):
                nxt = field[i + 1]
                out.append({'\\': '\\', 't': '\t', 'n': '\n'}.get(nxt, '\\' + nxt))
                i += 2
            else:
                out.append(ch)
                i += 1
        return ''.join(out)

    groups, switches = [], []
    for line in text.split('\n'):
        if not line:
            continue
        fields = line.split('\t')
        if fields[0] == '#group':
            groups.append([unescape(f) for f in fields[1:]])
        else:
            switches.append([unescape(f) for f in fields])
    return groups, switches


class LiveListTest(unittest.TestCase):
    def test_live_names_are_tuning_switches(self):
        tuning_names = {row[0] for row in sotc_tuning.TUNING}
        self.assertTrue(sotc_tuning.LIVE, 'LIVE should list the migrated switches')
        self.assertEqual(set(), set(sotc_tuning.LIVE) - tuning_names)

    def test_live_values_are_booleans(self):
        for name, value in sotc_tuning.LIVE.items():
            self.assertIs(value, True, name)

    def test_is_live(self):
        self.assertTrue(sotc_tuning.is_live('SOTC_RB_SPIN_US'))
        self.assertFalse(sotc_tuning.is_live('SOTC_BIND_CACHE'))
        self.assertFalse(sotc_tuning.is_live('SOTC_NOT_A_SWITCH'))

    def test_never_live(self):
        # Pipeline/shader keys, recompiler output and descriptor layouts stay restart-only.
        for name in ('SOTC_BIND_CACHE', 'SOTC_DEDUP_STATE', 'SOTC_BINDLESS_SAMP',
                     'SOTC_DESC_BUFFER'):
            self.assertNotIn(name, sotc_tuning.LIVE)

    def test_every_live_name_has_a_definition(self):
        defined = live_definitions()
        missing = sorted(set(sotc_tuning.LIVE) - defined)
        self.assertEqual([], missing, 'LIVE names without a SotcLive::Int definition under gpu/')

    def test_definitions_are_live_names(self):
        defined = live_definitions()
        self.assertTrue(defined, 'expected SotcLive::Int definitions under gpu/')
        self.assertEqual([], sorted(defined - set(sotc_tuning.LIVE)),
                         'SotcLive::Int defined for a switch missing from LIVE')

    def test_lab_entries_are_in_diag(self):
        groups = {row[0]: row[2] for row in sotc_tuning.TUNING}
        for name in ('SOTC_LAB', 'SOTC_FREECAM_MAXSPEED', 'SOTC_CTL'):
            self.assertEqual('diag', groups[name], name)


class TuningExportTest(unittest.TestCase):
    def export(self, folder, json_path=None):
        out = os.path.join(folder, 'tuning.tsv')
        argv = ['--out', out] + (['--json', json_path] if json_path else [])
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, tuning_export.main(argv))
        return Path(out).read_text(encoding='utf-8')

    def test_round_trip_field_counts(self):
        with tempfile.TemporaryDirectory() as folder:
            text = self.export(folder)
        groups, switches = parse_tsv(text)
        self.assertEqual(len(sotc_tuning.GROUPS), len(groups))
        self.assertEqual(len(sotc_tuning.TUNING), len(switches))
        for fields in groups:
            self.assertEqual(3, len(fields))
        for fields in switches:
            self.assertEqual(11, len(fields), fields[0])

    def test_round_trip_values(self):
        with tempfile.TemporaryDirectory() as folder:
            text = self.export(folder)
        _groups, switches = parse_tsv(text)
        by_name = {fields[0]: fields for fields in switches}
        for name, kind, group, label, hint, default, options in sotc_tuning.TUNING:
            fields = by_name[name]
            self.assertEqual(kind, fields[1])
            self.assertEqual(group, fields[2])
            self.assertEqual(label[0], fields[3])
            self.assertEqual(label[1], fields[4])
            self.assertEqual(hint[0], fields[5])
            self.assertEqual(hint[1], fields[6])
            self.assertEqual('1' if sotc_tuning.is_live(name) else '0', fields[8])
            if kind == 'choice':
                self.assertEqual(options[0][0] if options else '', fields[7])
                self.assertEqual(len(options), len(fields[10].split(';;')) if options else 0)

    def test_current_value_comes_from_environment(self):
        with tempfile.TemporaryDirectory() as folder:
            old = os.environ.get('SOTC_RB_SPIN_US')
            os.environ['SOTC_RB_SPIN_US'] = '77'
            try:
                text = self.export(folder)
            finally:
                if old is None:
                    os.environ.pop('SOTC_RB_SPIN_US', None)
                else:
                    os.environ['SOTC_RB_SPIN_US'] = old
        _groups, switches = parse_tsv(text)
        row = next(f for f in switches if f[0] == 'SOTC_RB_SPIN_US')
        self.assertEqual('77', row[9])

    def test_escaping(self):
        self.assertEqual('a\\\\b\\tc\\nd', tuning_export.escape('a\\b\tc\nd'))
        groups, switches = parse_tsv(
            tuning_export.tsv_text([('g', 'x\ty', 'z\nw')],
                                   [('N', 'value', 'g', 'a\\b', 'c\td', 'e\nf', 'g', '1', '0',
                                     '', '')]))
        self.assertEqual([['g', 'x\ty', 'z\nw']], groups)
        self.assertEqual(['N', 'value', 'g', 'a\\b', 'c\td', 'e\nf', 'g', '1', '0', '', ''],
                         switches[0])

    def test_json_export(self):
        import json
        with tempfile.TemporaryDirectory() as folder:
            json_path = os.path.join(folder, 'tuning.json')
            self.export(folder, json_path)
            data = json.loads(Path(json_path).read_text(encoding='utf-8'))
        self.assertEqual(len(sotc_tuning.TUNING), len(data['switches']))
        self.assertEqual(len(sotc_tuning.GROUPS), len(data['groups']))


if __name__ == '__main__':
    unittest.main()
