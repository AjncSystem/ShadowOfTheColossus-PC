#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""sotc: the game environment the launcher would start with, for the benchmark scripts.

    python tools/launcher_env.py <launcher_settings.json> [--modes-file F] [--extra NAME=VALUE ...]

Prints one --env NAME=VALUE argument per line (SOTC_*, BB_* and SHADPS4_* variables of
launcher.sotc_launcher_win.game_environment), so a recorded route replays with the settings the
player plays with. --modes-file writes the modes, cvars and render size sotc.py would prepare the
patches with (three lines), for scripts/sotc_modes.py.
"""
import argparse
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'launcher'))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('settings')
    parser.add_argument('--modes-file')
    parser.add_argument('--extra', action='append', default=[], help='NAME=VALUE added after the settings')
    parser.add_argument('--skip', action='append', default=[], help='a variable left out')
    args = parser.parse_args()
    sys.stdout.reconfigure(newline='\n')  # bash reads the lines (no CR)
    before = dict(os.environ)
    import sotc_launcher_win as launcher  # noqa: E402 (reads the environment at import)
    settings = dict(launcher.APP_DEFAULTS)
    settings.update(json.loads(Path(args.settings).read_text(encoding='utf-8')))
    env = launcher.game_environment(settings)
    for item in args.extra:
        name, value = item.split('=', 1)
        env[name] = value
    keep = ('SOTC_', 'BB_', 'SHADPS4_')
    for name in sorted(env):
        if not name.startswith(keep) or before.get(name) == env[name]:
            continue
        if name in ('SOTC_GAME_DIR', 'SOTC_LAUNCHER_SETTINGS', 'SOTC_EVENT_LOG', 'SOTC_WINDOW_ICON', *args.skip):
            continue
        print(f'--env\n{name}={env[name]}')
    if args.modes_file:
        Path(args.modes_file).write_text('\n'.join((env.get('SOTC_MODES', ''), env.get('SOTC_CVARS', ''),
                                                    env.get('SOTC_RENDER_RES', ''))) + '\n',
                                         encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
