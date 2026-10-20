# SPDX-License-Identifier: GPL-2.0-or-later
"""sotc: launcher self-update from the port's GitHub releases.

The launcher asks GitHub for the latest release (SOTC_UPDATE_URL replaces the API address, for
tests: a file:// or http:// URL of the same JSON). When its tag is newer than the running
version and the user agrees, SotC-PC.zip is downloaded to <port>/update/, checked (size, the
SHA-256 digest GitHub reports for the asset, zip integrity, layout) and unpacked to
<port>/update/new/SotC-PC/.

The files are replaced by a copy of the running launcher (SotC.exe and _internal/ copied to
<port>/update/applier/), started with --apply-update once the launcher has closed, so no file in
use is overwritten. It copies the new files over the port folder, never touching user/ (saves,
shader caches), update/ or an existing sotc.ini, and leaves files the release no longer has in
place. Every replaced file is backed up first; on any error the backups are put back. The
launcher is then started again. update/update.log records what happened.
"""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile

RELEASES_URL = 'https://api.github.com/repos/Lucas-blip-png/ShadowOfTheColossus-PC/releases/latest'  # sotc: this fork's releases
ASSET_NAME = 'SotC-PC.zip'
TOP = 'SotC-PC'
KEEP = ('user', 'update')  # top-level names an update never writes into
USER_AGENT = 'SotC-PC-launcher'


def version_key(text):
    """'0.57-opt5' -> (0, 57, 5); '0.58' -> (0, 58)."""
    return tuple(int(n) for n in re.findall(r'\d+', text or ''))


def _open(url, timeout):
    request = urllib.request.Request(url, headers={'Accept': 'application/vnd.github+json',
                                                   'User-Agent': USER_AGENT})
    return urllib.request.urlopen(request, timeout=timeout)


def latest_release(timeout=10):
    """The latest release with a SotC-PC.zip asset, as a dict, or None."""
    with _open(os.environ.get('SOTC_UPDATE_URL') or RELEASES_URL, timeout) as response:
        data = json.load(response)
    asset = next((a for a in data.get('assets', []) if a.get('name') == ASSET_NAME), None)
    if not asset or not data.get('tag_name'):
        return None
    digest = asset.get('digest') or ''
    return {'version': data['tag_name'].lstrip('vV'), 'name': data.get('name') or data['tag_name'],
            'notes': data.get('body') or '', 'page': data.get('html_url') or '',
            'url': asset['browser_download_url'], 'size': int(asset.get('size') or 0),
            'sha256': digest[7:].lower() if digest.startswith('sha256:') else ''}


def is_newer(release, current):
    return bool(release) and version_key(release['version']) > version_key(current)


def download(release, update_dir, progress=None):
    """Downloads and checks the release zip, unpacks it; returns the new SotC-PC folder."""
    update_dir.mkdir(parents=True, exist_ok=True)
    part = update_dir / (ASSET_NAME + '.part')
    digest, done = hashlib.sha256(), 0
    with _open(release['url'], 60) as response, open(part, 'wb') as out:
        total = int(response.headers.get('Content-Length') or release['size'] or 0)
        while True:
            chunk = response.read(1 << 20)
            if not chunk:
                break
            out.write(chunk)
            digest.update(chunk)
            done += len(chunk)
            if progress:
                progress(done, total)
    if release['size'] and done != release['size']:
        raise ValueError(f'download incomplete: {done} of {release["size"]} bytes')
    if release['sha256'] and digest.hexdigest() != release['sha256']:
        raise ValueError('download damaged: SHA-256 does not match the release')
    new = update_dir / 'new'
    shutil.rmtree(new, ignore_errors=True)
    with zipfile.ZipFile(part) as archive:
        names = archive.namelist()
        if f'{TOP}/SotC.exe' not in names:
            raise ValueError(f'not a SotC-PC release (no {TOP}/SotC.exe)')
        for name in names:
            parts = Path(name).parts
            if name.startswith(('/', '\\')) or ':' in name or '..' in parts or parts[0] != TOP:
                raise ValueError(f'unexpected path in the release: {name}')
        if archive.testzip() is not None:
            raise ValueError('the release zip is damaged')
        archive.extractall(new)
    part.unlink()
    return new / TOP


def start_applier(new_root, port_dir):
    """Copies the running launcher to update/applier and starts it with --apply-update; the
    caller then closes. Only for the frozen launcher (SotC.exe + _internal/)."""
    exe = Path(sys.executable).resolve()
    applier = port_dir / 'update' / 'applier'
    shutil.rmtree(applier, ignore_errors=True)
    applier.mkdir(parents=True)
    shutil.copy2(exe, applier / exe.name)
    internal = exe.parent / '_internal'
    if internal.is_dir():
        shutil.copytree(internal, applier / '_internal')
    flags = getattr(subprocess, 'DETACHED_PROCESS', 0) | getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)
    subprocess.Popen([str(applier / exe.name), '--apply-update', str(new_root), str(port_dir),
                      str(os.getpid())], cwd=str(applier), creationflags=flags, close_fds=True)


def _wait_for_exit(pid, seconds=60):
    if not pid or os.name != 'nt':
        return
    SYNCHRONIZE = 0x00100000
    handle = ctypes.windll.kernel32.OpenProcess(SYNCHRONIZE, False, pid)
    if handle:
        ctypes.windll.kernel32.WaitForSingleObject(handle, seconds * 1000)
        ctypes.windll.kernel32.CloseHandle(handle)


def _copy(src, dst):
    # A file may stay locked for a moment (antivirus scan of the new exe): retry for ~10 s.
    for attempt in range(20):
        try:
            shutil.copy2(src, dst)
            return
        except PermissionError:
            if attempt == 19:
                raise
            time.sleep(0.5)


def apply_update(new_root, port_dir, pid=0, restart=True):
    """Copies new_root over port_dir (see the module docstring). Returns True on success."""
    new_root, port_dir = Path(new_root), Path(port_dir)
    update_dir = port_dir / 'update'
    update_dir.mkdir(parents=True, exist_ok=True)
    log = open(update_dir / 'update.log', 'a', encoding='utf-8')

    def note(text):
        log.write(time.strftime('%Y-%m-%d %H:%M:%S ') + text + '\n')
        log.flush()

    _wait_for_exit(pid)
    backup = update_dir / 'backup'
    shutil.rmtree(backup, ignore_errors=True)
    done = []  # (installed file, its backup or None)
    ok = False
    try:
        if not (new_root / 'SotC.exe').is_file():
            raise ValueError(f'{new_root} is not a SotC-PC folder')
        for src in sorted(p for p in new_root.rglob('*') if p.is_file()):
            rel = src.relative_to(new_root)
            if rel.parts[0] in KEEP or (rel == Path('sotc.ini') and (port_dir / rel).exists()):
                continue
            dst = port_dir / rel
            saved = None
            if dst.exists():
                saved = backup / rel
                saved.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(dst, saved)
            done.append((dst, saved))
            dst.parent.mkdir(parents=True, exist_ok=True)
            _copy(src, dst)
        note(f'updated: {len(done)} files from {new_root}')
        ok = True
    except Exception as error:  # put every replaced file back
        note(f'FAILED: {error!r}; restoring {len(done)} files')
        for dst, saved in reversed(done):
            try:
                if saved:
                    _copy(saved, dst)
                elif dst.exists():
                    dst.unlink()
            except OSError as restore_error:
                note(f'could not restore {dst}: {restore_error!r}')
        (update_dir / 'failed').write_text(str(error), encoding='utf-8')
    log.close()
    if restart and (port_dir / 'SotC.exe').is_file():
        flags = getattr(subprocess, 'DETACHED_PROCESS', 0) | getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)
        subprocess.Popen([str(port_dir / 'SotC.exe')], cwd=str(port_dir), creationflags=flags, close_fds=True)
    return ok


def cleanup(port_dir):
    """At launcher start: the error of a failed update (or None), and update/ removed."""
    update_dir = Path(port_dir) / 'update'
    failed = update_dir / 'failed'
    error = failed.read_text(encoding='utf-8', errors='replace') if failed.is_file() else None
    for name in ('new', 'applier', 'backup', 'failed', ASSET_NAME + '.part'):
        path = update_dir / name
        if path.is_dir():
            shutil.rmtree(path, ignore_errors=True)  # the applier may still be closing
        elif path.exists():
            try:
                path.unlink()
            except OSError:
                pass
    return error
