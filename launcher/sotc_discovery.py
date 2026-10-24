# SPDX-License-Identifier: GPL-2.0-or-later
"""Finds what the launcher needs on this PC: PS4 game folders (a library like shadPS4's game
folders), the user's own decrypted sound modules (libSceNgs2/libSceUlt from a shadPS4 install)
and shadPS4 saves of the game. Nothing here comes with the port: it only looks for the user's
own files."""
import ctypes
import os
from pathlib import Path
import string
import time

from sotc_modes import GAMES

# The supported releases (EU CUSA08809 first, then US CUSA08034): the same eboot build.
GAME_IDS = tuple(GAMES)
SYS_MODULES = ('libSceUlt.sprx', 'libSceNgs2.sprx')

# Folders never worth entering on a drive scan.
SKIP = {'windows', '$recycle.bin', 'system volume information', 'programdata', 'recovery',
        'perflogs', 'msocache', '$windows.~bt', '$windows.~ws', 'windows.old', 'node_modules',
        '.git', '__pycache__', 'winsxs', 'microsoft', 'nvidia corporation', 'intel', 'amd',
        'appdata', 'steamapps', 'system32', 'syswow64', 'driverstore', 'installer','wpsystem'}


def game_record(folder):
    """{'path', 'id', 'title', 'version', 'icon'} of a PS4 game folder, or None."""
    folder = Path(folder)
    sfo_path = folder / 'sce_sys' / 'param.sfo'
    if not (folder / 'eboot.bin').is_file() or not sfo_path.is_file():
        return None
    try:
        from prepare import sfo
        values = sfo(sfo_path.read_bytes())
    except (OSError, ValueError, ImportError):
        values = {}
    icon = folder / 'sce_sys' / 'icon0.png'
    return {
        'path': str(folder),
        'id': values.get('TITLE_ID', folder.name),
        # Undecodable bytes of a title (replacement characters) are dropped.
        'title': str(values.get('TITLE', folder.name)).replace('�', '').strip(),
        'version': values.get('APP_VER', '?'),
        'icon': str(icon) if icon.is_file() else '',
    }


def dlc_serial(folder):
    """The supported title ID an add-on content folder belongs to (param.sfo: CATEGORY ac, the
    title ID inside CONTENT_ID), or None."""
    try:
        from prepare import sfo
        values = sfo((Path(folder) / 'sce_sys' / 'param.sfo').read_bytes())
    except (OSError, ValueError, ImportError):
        return None
    if not str(values.get('CATEGORY', '')).startswith('ac'):
        return None
    content = str(values.get('CONTENT_ID', ''))
    return next((serial for serial in GAME_IDS if serial in content), None)


def is_game_dlc(folder, serial):
    """True for an add-on content folder of the game serial (the same region only)."""
    return dlc_serial(folder) == serial


def _walk_dirs(root, depth):
    """Folders under root down to depth levels (root included), skipping the usual noise."""
    pending = [(str(root), 0)]
    while pending:
        path, level = pending.pop()
        yield path
        if level >= depth:
            continue
        try:
            for entry in os.scandir(path):
                if entry.is_dir(follow_symlinks=False) and entry.name.lower() not in SKIP \
                        and not entry.name.startswith('.'):
                    pending.append((entry.path, level + 1))
        except OSError:
            pass


def is_dlc_pkg(path, serial):
    """True for an add-on package of the game serial not installed (header: content type 0x1b, a
    content ID of that title). The runtime lists these without extracting them."""
    try:
        with open(path, 'rb') as f:
            header = f.read(0x80)
    except OSError:
        return False
    return (len(header) == 0x80 and header[:4] == b'\x7fCNT' and header[0x74:0x78] == b'\0\0\0\x1b'
            and serial.encode() in header[0x40:0x64])


def _dlc_entries(folder, serial):
    """The DLC of the game serial in one folder: add-on folders (sce_sys/param.sfo) and add-on
    packages. DLC of the other region is left out, as the runtime does."""
    found = []
    try:
        entries = list(os.scandir(folder))
    except OSError:
        return found
    for entry in entries:
        try:
            if entry.is_dir(follow_symlinks=False):
                if (Path(entry.path) / 'sce_sys' / 'param.sfo').is_file() and is_game_dlc(entry.path, serial):
                    found.append(entry.path)
            elif entry.name.lower().endswith('.pkg') and is_dlc_pkg(entry.path, serial):
                found.append(entry.path)
        except OSError:  # a folder this user can't read
            pass
    return found


def game_serial(game_dir):
    """The title ID of a game folder, CUSA08809 (the EU release) when it can't be read."""
    record = game_record(game_dir) if game_dir else None
    return record['id'] if record and record['id'] in GAME_IDS else GAME_IDS[0]


def find_dlc_root(game_dir, chosen=None):
    """The folder to give the runtime as SOTC_DLC_DIR: where the most DLC of the game's region
    (installed folders or .pkg files) is, looking inside the game folder, in the main folder that
    holds it and in the folders next to that one (a "DLC pack" folder beside the game). '' without
    any."""
    game_dir = Path(game_dir)
    serial = game_serial(game_dir)
    counts = {}
    roots = [(game_dir, 2), (game_dir.parent, 3), (game_dir.parent.parent, 2)]
    if chosen:  # the folder the user picked holds the game and, likely, the DLC too
        roots.insert(0, (Path(chosen), 4))
    for root, depth in roots:
        for path in _walk_dirs(root, depth):
            if os.path.normcase(path) == os.path.normcase(str(game_dir)):
                continue  # the game's own folders are big and hold no DLC
            entries = _dlc_entries(path, serial)
            if entries:
                counts[path] = len(entries)
    if not counts:
        return ''
    best = Path(max(counts, key=counts.get))
    # shadPS4 layout: <addcont>/<title id>/<dlc>; the runtime looks in the <title id> subfolder.
    return str(best.parent if best.name.upper() == serial else best)


def dlc_count(root, serial):
    """How many DLC the runtime will list for SOTC_DLC_DIR root when the game serial runs."""
    if not root or not Path(root).is_dir():
        return 0
    base = Path(root) / serial if (Path(root) / serial).is_dir() else Path(root)
    return len(_dlc_entries(base, serial))


UPDATE_SUFFIXES = ('-UPDATE', '-patch')


def find_game_in(folder):
    """A supported base game folder (EU or US): folder itself or one below it (depth 3), as
    shadPS4 finds it from the main folder; update folders (<game>-UPDATE, <game>-patch) only when
    nothing else; the EU release before the US one."""
    games = []
    for path in _walk_dirs(folder, 3):
        if (Path(path) / 'eboot.bin').is_file():
            record = game_record(path)
            if record and record['id'] in GAME_IDS:
                games.append((path, record['id']))
    games.sort(key=lambda g: (Path(g[0]).name.lower().endswith(tuple(s.lower() for s in UPDATE_SUFFIXES)),
                              GAME_IDS.index(g[1])))
    return games[0][0] if games else None


def find_update(game_dir):
    """The update folder next to the base game (<game>-UPDATE or <game>-patch, as shadPS4)."""
    game_dir = Path(game_dir)
    for suffix in UPDATE_SUFFIXES:
        folder = game_dir.with_name(game_dir.name + suffix)
        if folder.is_dir():
            return str(folder)
    return ''


def library_games(folders, depth=3):
    """Games in the given library folders and below (shadPS4's "game folders"), each once."""
    games, seen = [], set()

    def visit(path, level):
        try:
            entries = list(os.scandir(path))
        except OSError:
            return
        record = game_record(path)
        if record:
            key = os.path.normcase(os.path.abspath(record['path']))
            if key not in seen:
                seen.add(key)
                games.append(record)
            return
        if level >= depth:
            return
        for entry in entries:
            if entry.is_dir(follow_symlinks=False) and entry.name.lower() not in SKIP:
                visit(entry.path, level + 1)

    for folder in folders:
        if folder and Path(folder).is_dir():
            visit(folder, 0)
    return sorted(games, key=lambda g: (g['id'] not in GAME_IDS, g['title'].lower()))


def local_drives():
    """Fixed drive roots (no network, CD or removable media: too slow to walk)."""
    roots = []
    try:
        mask = ctypes.windll.kernel32.GetLogicalDrives()
        get_type = ctypes.windll.kernel32.GetDriveTypeW
    except (AttributeError, OSError):
        return [Path.home().anchor]
    for i, letter in enumerate(string.ascii_uppercase):
        if mask & (1 << i):
            root = f'{letter}:\\'
            if get_type(root) == 3:  # DRIVE_FIXED
                roots.append(root)
    return roots


def scan(seconds=25.0, depth=5, extra_roots=()):
    """Walks the drives (the likely places first) for the game, sound modules and saves.
    Returns {'games': [records of a supported title], 'sys_modules': [folders],
    'saves': [folders, each named after its title ID], 'dlc': [(folder, title ID)]}."""
    found = {'games': [], 'sys_modules': [], 'saves': [], 'dlc': []}
    seen = set()
    deadline = time.monotonic() + seconds
    home = Path.home()
    appdata = Path(os.environ.get('APPDATA', home))
    likely = [*extra_roots, appdata / 'shadPS4', home / 'Desktop', home / 'Downloads', home / 'Documents',
              home / 'Games', home]
    roots = [str(p) for p in likely if Path(p).is_dir()] + local_drives()

    def visit(path, level):
        if time.monotonic() > deadline:
            return
        key = os.path.normcase(path)
        if key in seen:
            return
        seen.add(key)
        try:
            entries = list(os.scandir(path))
        except OSError:
            return
        names = {e.name.lower(): e for e in entries}
        if 'eboot.bin' in names:
            record = game_record(path)
            if record and record['id'] in GAME_IDS:
                found['games'].append(record)
            return
        if all(m.lower() in names for m in SYS_MODULES):
            found['sys_modules'].append(path)
        serial = dlc_serial(path) if 'sce_sys' in names else None
        if serial:
            parent = os.path.dirname(path)
            if os.path.basename(parent).upper() == serial:  # shadPS4's addcont/<title id>/<dlc>
                parent = os.path.dirname(parent)
            if (parent, serial) not in found['dlc']:
                found['dlc'].append((parent, serial))
            return
        base = os.path.basename(path).lower()
        if base == 'savedata':
            for serial in GAME_IDS:
                for sub in (Path(path) / serial, *Path(path).glob('*/' + serial)):
                    if sub.is_dir() and any(sub.iterdir()):
                        found['saves'].append(str(sub))
        if level >= depth:
            return
        for entry in entries:
            if time.monotonic() > deadline:
                return
            name = entry.name.lower()
            if name in SKIP or name.startswith('.'):
                continue
            try:
                if entry.is_dir(follow_symlinks=False):
                    visit(entry.path, level + 1)
            except OSError:
                pass

    for root in roots:
        visit(root, 0)
    # shadPS4's own sys_modules folders first.
    found['sys_modules'].sort(key=lambda p: (os.path.basename(p).lower() != 'sys_modules',
                                             'shadps4' not in p.lower()))
    return found
