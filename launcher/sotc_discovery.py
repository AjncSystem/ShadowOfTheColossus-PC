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

GAME_ID = 'CUSA08809'
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


def is_game_dlc(folder):
    """True for an add-on content folder of the game (param.sfo: CATEGORY ac, CONTENT_ID of it)."""
    try:
        from prepare import sfo
        values = sfo((Path(folder) / 'sce_sys' / 'param.sfo').read_bytes())
    except (OSError, ValueError, ImportError):
        return False
    return str(values.get('CATEGORY', '')).startswith('ac') and GAME_ID in str(values.get('CONTENT_ID', ''))


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


def is_dlc_pkg(path):
    """True for an add-on package of the game not installed (header: content type 0x1b, a
    content ID of the game). The runtime lists these without extracting them."""
    try:
        with open(path, 'rb') as f:
            header = f.read(0x80)
    except OSError:
        return False
    return (len(header) == 0x80 and header[:4] == b'\x7fCNT' and header[0x74:0x78] == b'\0\0\0\x1b'
            and GAME_ID.encode() in header[0x40:0x64])


def _dlc_entries(folder):
    """The DLC in one folder: add-on folders (sce_sys/param.sfo) and add-on packages."""
    found = []
    try:
        entries = list(os.scandir(folder))
    except OSError:
        return found
    for entry in entries:
        if entry.is_dir(follow_symlinks=False):
            if (Path(entry.path) / 'sce_sys' / 'param.sfo').is_file() and is_game_dlc(entry.path):
                found.append(entry.path)
        elif entry.name.lower().endswith('.pkg') and is_dlc_pkg(entry.path):
            found.append(entry.path)
    return found


def find_dlc_root(game_dir, chosen=None):
    """The folder to give the runtime as SOTC_DLC_DIR: where the most DLC (installed folders or
    .pkg files) is, looking inside the game folder, in the main folder that holds it and in the
    folders next to that one (a "DLC pack" folder beside the game). '' without any."""
    game_dir = Path(game_dir)
    counts = {}
    roots = [(game_dir, 2), (game_dir.parent, 3), (game_dir.parent.parent, 2)]
    if chosen:  # the folder the user picked holds the game and, likely, the DLC too
        roots.insert(0, (Path(chosen), 4))
    for root, depth in roots:
        for path in _walk_dirs(root, depth):
            if os.path.normcase(path) == os.path.normcase(str(game_dir)):
                continue  # the game's own folders are big and hold no DLC
            entries = _dlc_entries(path)
            if entries:
                counts[path] = len(entries)
    if not counts:
        return ''
    best = Path(max(counts, key=counts.get))
    # shadPS4 layout: <addcont>/CUSA08809/<dlc>; the runtime looks in the CUSA08809 subfolder.
    return str(best.parent if best.name.upper() == GAME_ID else best)


def dlc_count(root):
    """How many DLC the runtime will list for SOTC_DLC_DIR root."""
    if not root or not Path(root).is_dir():
        return 0
    base = Path(root) / GAME_ID if (Path(root) / GAME_ID).is_dir() else Path(root)
    return len(_dlc_entries(base))


UPDATE_SUFFIXES = ('-UPDATE', '-patch')


def find_game_in(folder):
    """The GAME_ID base game folder: folder itself or one below it (depth 3), as shadPS4 finds it
    from the main folder; update folders (<game>-UPDATE, <game>-patch) only when nothing else."""
    games = []
    for path in _walk_dirs(folder, 3):
        if (Path(path) / 'eboot.bin').is_file():
            record = game_record(path)
            if record and record['id'] == GAME_ID:
                games.append(path)
    games.sort(key=lambda p: Path(p).name.lower().endswith(tuple(s.lower() for s in UPDATE_SUFFIXES)))
    return games[0] if games else None


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
    return sorted(games, key=lambda g: (g['id'] != GAME_ID, g['title'].lower()))


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
    Returns {'games': [records of GAME_ID], 'sys_modules': [folders], 'saves': [folders]}."""
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
            if record and record['id'] == GAME_ID:
                found['games'].append(record)
            return
        if all(m.lower() in names for m in SYS_MODULES):
            found['sys_modules'].append(path)
        if 'sce_sys' in names and is_game_dlc(path):
            parent = os.path.dirname(path)
            if os.path.basename(parent).upper() == GAME_ID:  # shadPS4's addcont/CUSA08809/<dlc>
                parent = os.path.dirname(parent)
            if parent not in found['dlc']:
                found['dlc'].append(parent)
            return
        base = os.path.basename(path).lower()
        if base == 'savedata':
            for sub in (Path(path) / GAME_ID, *Path(path).glob('*/' + GAME_ID)):
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
