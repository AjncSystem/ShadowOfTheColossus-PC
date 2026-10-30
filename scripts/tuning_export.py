"""Exports the SOTC_* tuning switches (launcher/sotc_tuning.py) for the in-game SotC Lab menu.

    python scripts/tuning_export.py --out out/tuning.tsv [--json out/tuning.json]

The TSV (UTF-8, LF line ends) is read by the C++ menu. Two kinds of line:

    #group<TAB><id><TAB><label_pt><TAB><label_en>
    <name><TAB><kind><TAB><group><TAB><label_pt><TAB><label_en><TAB><hint_pt><TAB><hint_en>
        <TAB><default><TAB><live 0|1><TAB><current><TAB><options>

default: "0" for flag, "1" for flag_on, the first option value for choice, the default text for
value. current: the variable's value in this process's environment at export time ("" if unset).
options: for choice, "<value>=<label_pt>|<label_en>" entries joined by ";;"; empty otherwise.

Every field escapes backslash as \\\\, tab as \\t and newline as \\n (two-character sequences), so a
field never breaks a line or a column. Stdlib only, so it also runs from the packaged build.
"""

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, 'launcher'))

import sotc_tuning  # noqa: E402  (path set up above)


def escape(text):
    """Escapes one TSV field: backslash, tab and newline become two-character sequences."""
    return str(text).replace('\\', '\\\\').replace('\t', '\\t').replace('\n', '\\n')


def default_text(kind, default, options):
    if kind == 'flag':
        return '0'
    if kind == 'flag_on':
        return '1'
    if kind == 'choice':
        return options[0][0] if options else ''
    return default


def options_text(kind, options):
    if kind != 'choice':
        return ''
    return ';;'.join('%s=%s|%s' % (value, label[0], label[1]) for value, label in options)


def rows():
    """(groups, switches): plain lists of field tuples, before escaping."""
    groups = [(gid, label[0], label[1]) for gid, label in sotc_tuning.GROUPS]
    switches = []
    for name, kind, group, label, hint, default, options in sotc_tuning.TUNING:
        switches.append((
            name, kind, group, label[0], label[1], hint[0], hint[1],
            default_text(kind, default, options),
            '1' if sotc_tuning.is_live(name) else '0',
            os.environ.get(name, ''),
            options_text(kind, options),
        ))
    return groups, switches


def tsv_text(groups, switches):
    lines = []
    for gid, label_pt, label_en in groups:
        lines.append('\t'.join(escape(f) for f in ('#group', gid, label_pt, label_en)))
    for fields in switches:
        lines.append('\t'.join(escape(f) for f in fields))
    return '\n'.join(lines) + '\n'


def json_data(groups, switches):
    keys = ('name', 'kind', 'group', 'label_pt', 'label_en', 'hint_pt', 'hint_en', 'default',
            'live', 'current', 'options')
    return {
        'groups': [{'id': g, 'label_pt': a, 'label_en': b} for g, a, b in groups],
        'switches': [dict(zip(keys, fields)) for fields in switches],
    }


def write_text(path, text):
    folder = os.path.dirname(os.path.abspath(path))
    os.makedirs(folder, exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--out', required=True, help='TSV file to write')
    parser.add_argument('--json', help='optional JSON file with the same data')
    args = parser.parse_args(argv)

    groups, switches = rows()
    write_text(args.out, tsv_text(groups, switches))
    if args.json:
        write_text(args.json, json.dumps(json_data(groups, switches), ensure_ascii=False,
                                         indent=1) + '\n')
    print('tuning export: %d groups, %d switches -> %s' % (len(groups), len(switches), args.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
