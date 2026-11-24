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
import ssl
import subprocess
import sys
import time
import urllib.request
import zipfile

REPOSITORY = 'AjncSystem/ShadowOfTheColossus-PC'


def package_repository(folder=None):
    """The repository a packaged copy updates from.

    A fork's package carries launcher/update_repo.txt ("owner/name", written by package_sotc.sh
    from SOTC_UPDATE_REPO), so its players get the fork's releases. The file is never in git:
    the source always names this repository, and merging a fork cannot change it.
    """
    if folder is None:
        frozen = getattr(sys, 'frozen', False)
        folder = Path(sys.executable).resolve().parent / 'launcher' if frozen else Path(__file__).resolve().parent
    try:
        text = (Path(folder) / 'update_repo.txt').read_text(encoding='utf-8').strip()
    except (OSError, ValueError):
        return REPOSITORY
    return text if re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9-]*/[A-Za-z0-9_][A-Za-z0-9_.-]*', text) else REPOSITORY


def release_urls(repository):
    return (f'https://api.github.com/repos/{repository}/releases/latest',
            f'https://github.com/{repository}/releases')


RELEASES_URL, RELEASES_PAGE = release_urls(package_repository())
ASSET_NAME = 'SotC-PC.zip'
# sotc: the Linux AppImage of a release (packaging/linux, .github/workflows/linux-appimage.yml).
APPIMAGE_ASSET = 'SotC-PC-linux-x86_64.AppImage'
TOP = 'SotC-PC'


def running_appimage():
    """The AppImage file this launcher runs from (AppRun's $APPIMAGE), or None (Windows, a source
    checkout). An AppImage is updated by replacing that one file."""
    path = os.environ.get('APPIMAGE') if sys.platform.startswith('linux') else None
    return Path(path) if path and Path(path).is_file() else None


def can_update():
    """True where the launcher can install an update: the frozen Windows launcher or an AppImage."""
    return bool(getattr(sys, 'frozen', False) or running_appimage())
KEEP = ('user', 'update')  # top-level names an update never writes into
USER_AGENT = 'SotC-PC-launcher'


def version_key(text):
    """'0.57-opt5' -> (0, 57, 5); '0.58' -> (0, 58)."""
    return tuple(int(n) for n in re.findall(r'\d+', text or ''))


_context = None


def _ssl_context():
    """Verifying context with every root available: the system's (on Windows the ROOT and CA
    stores, loaded by create_default_context) plus certifi's bundle when it is installed, which
    has the roots Windows has not downloaded yet (issue #359). Verification is never disabled."""
    global _context
    if _context is None:
        context = ssl.create_default_context()
        try:
            import certifi
            context.load_verify_locations(cafile=certifi.where())
        except (ImportError, OSError, ssl.SSLError):
            pass
        _context = context
    return _context


def is_cert_error(error):
    """True when the error (or the URLError wrapping it) is a failed certificate check."""
    return isinstance(error, ssl.SSLCertVerificationError) or \
        isinstance(getattr(error, 'reason', None), ssl.SSLCertVerificationError)


def _open(url, timeout):
    request = urllib.request.Request(url, headers={'Accept': 'application/vnd.github+json',
                                                   'User-Agent': USER_AGENT})
    context = _ssl_context() if url.lower().startswith('https:') else None
    return urllib.request.urlopen(request, timeout=timeout, context=context)


def latest_release(timeout=10):
    """The latest release with this system's asset (SotC-PC.zip on Windows, the AppImage when the
    launcher runs from one), as a dict, or None."""
    with _open(os.environ.get('SOTC_UPDATE_URL') or RELEASES_URL, timeout) as response:
        data = json.load(response)
    appimage = running_appimage() is not None
    name = APPIMAGE_ASSET if appimage else ASSET_NAME
    asset = next((a for a in data.get('assets', []) if a.get('name') == name), None)
    if not asset or not data.get('tag_name'):
        return None
    digest = asset.get('digest') or ''
    return {'version': data['tag_name'].lstrip('vV'), 'name': data.get('name') or data['tag_name'],
            'notes': data.get('body') or '', 'page': data.get('html_url') or '',
            'url': asset['browser_download_url'], 'size': int(asset.get('size') or 0),
            'sha256': digest[7:].lower() if digest.startswith('sha256:') else '', 'appimage': appimage}


_ENGLISH = re.compile(r'^##[ \t]+(?:in[ \t]+)?english(?:[ \t]+version)?[ \t:]*$', re.I | re.M)
_OTHER_LANGUAGE = re.compile(r'^##[ \t]+(?:portugu[eê]s[a-z]*|em[ \t]+portugu[eê]s|vers[aã]o[ \t]+em[ \t]+portugu[eê]s)'
                             r'[ \t:]*$', re.I | re.M)


def notes_for(notes, english):
    """The part of a release body in the launcher's language: a '## English' section holds the
    English notes; the text before it is the Portuguese one. Without that section, the whole body."""
    notes = notes or ''
    heading = _ENGLISH.search(notes)
    if not heading:
        return notes.strip()
    if not english:
        return notes[:heading.start()].strip() or notes.strip()
    rest = notes[heading.end():]
    other = _OTHER_LANGUAGE.search(rest)
    return (rest[:other.start()] if other else rest).strip() or notes.strip()


def is_newer(release, current):
    return bool(release) and version_key(release['version']) > version_key(current)


def update_dir_for(port_dir):
    """Where downloads go: <port>/update on Windows; next to the AppImage on Linux (the AppImage's
    own folder is read-only, and the new file must be on the same file system to replace it)."""
    appimage = running_appimage()
    return appimage.parent / '.sotc-update' if appimage else Path(port_dir) / 'update'


def download(release, update_dir, progress=None):
    """Downloads and checks the release zip, unpacks it; returns the new SotC-PC folder. For an
    AppImage release: downloads and checks the file; returns its path."""
    update_dir.mkdir(parents=True, exist_ok=True)
    if release.get('appimage'):
        return _download_appimage(release, update_dir, progress)
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


def _download_appimage(release, update_dir, progress=None):
    part = update_dir / (APPIMAGE_ASSET + '.part')
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
    with open(part, 'rb') as check:
        head = check.read(16)
    # An ELF executable with the AppImage type-2 magic ("AI" 0x02 at offset 8).
    if head[:4] != b'\x7fELF' or head[8:11] != b'AI\x02':
        raise ValueError('not an AppImage')
    os.chmod(part, 0o755)
    return part


def apply_appimage(new_file):
    """Replaces the running AppImage with the downloaded one and starts it; the caller then closes.
    The old file is kept as <name>.old (a running AppImage keeps working after its file is replaced:
    its contents stay mounted until it exits)."""
    target = running_appimage()
    if not target:
        raise ValueError('not running from an AppImage')
    backup = target.with_name(target.name + '.old')
    try:
        backup.unlink()
    except FileNotFoundError:
        pass
    try:
        os.link(target, backup)  # the old file's data, without copying it
    except OSError:
        shutil.copy2(target, backup)
    os.replace(new_file, target)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('APPDIR', 'APPIMAGE', 'ARGV0', 'OWD', 'TCL_LIBRARY', 'TK_LIBRARY',
                                'PYTHONHOME', 'PYTHONPATH', 'LD_LIBRARY_PATH'))}
    subprocess.Popen([str(target)], cwd=str(target.parent), env=env, start_new_session=True,
                     close_fds=True)


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
