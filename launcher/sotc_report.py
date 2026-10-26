"""Report a problem: a sanitized report (versions, hardware, settings, tail of the logs) that the
player copies, saves as a .zip or sends to a new GitHub issue. Nothing is sent by the launcher
itself: the issue page only opens in the browser, and the player presses the button there."""
import getpass
import json
import os
import platform
import subprocess
import time
import urllib.parse
import zipfile
from pathlib import Path

ISSUES_URL = 'https://github.com/Lucas-blip-png/ShadowOfTheColossus-PC/issues/new'
TAIL_LINES = 150
URL_BODY_MAX = 1500    # a browser URL longer than ~8000 characters is refused; the report goes by clipboard
LOGS = ('last_run.log', 'events.log', 'last_run.1.log', 'events.1.log')
DIAG_TAIL = 8 * 1024 * 1024  # the detailed texture log (up to 400 MB): only its last part goes in


def _powershell(command):
    try:
        out = subprocess.run(['powershell', '-NoProfile', '-Command', command], capture_output=True,
                             text=True, timeout=15, creationflags=0x08000000)  # CREATE_NO_WINDOW
        return out.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ''


def hardware():
    """GPU (name, driver, VRAM), CPU and RAM through CIM; empty lines when PowerShell is missing."""
    gpu = _powershell("Get-CimInstance Win32_VideoController | ForEach-Object { '{0} | driver {1} | VRAM (WMI, max 4095) {2} MB' "
                      "-f $_.Name, $_.DriverVersion, [math]::Round($_.AdapterRAM/1MB) }")
    cpu = _powershell('(Get-CimInstance Win32_Processor | Select-Object -First 1).Name')
    ram = _powershell('[math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory/1GB)')
    return gpu, cpu, ram


def scrub(text, extra=()):
    """Removes the Windows user name and home folder from text. extra: more strings to hide."""
    names = {getpass.getuser(), Path.home().name, *extra}
    home = str(Path.home())
    for old in (home, home.replace('\\', '/')):
        text = text.replace(old, '<home>')
    for name in sorted((n for n in names if n and len(n) > 2), key=len, reverse=True):
        text = text.replace(name, '<user>')
    return text


def tail(path, lines=TAIL_LINES):
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            return ''.join(f.readlines()[-lines:])
    except OSError:
        return ''


def build(version, settings, user_dir):
    """The report as text. settings: the launcher's settings dict (paths are shortened)."""
    gpu, cpu, ram = hardware()
    keys = ('fps_mode', 'textures', 'output_res', 'upscaler', 'sharpness', 'framegen', 'vram_use', 'af16',
            'fullscreen', 'shadow_reuse', 'extra_env')
    config = {k: settings.get(k) for k in keys if k in settings}
    crashes = sorted((Path(user_dir) / 'crashes').glob('*.log'))[-3:] if (Path(user_dir) / 'crashes').is_dir() else []
    parts = [
        f'Shadow of the Colossus PC {version}',
        f'Windows {platform.version()} ({platform.machine()})',
        f'CPU: {cpu or "?"} | RAM: {ram or "?"} GB',
        'GPU:', gpu or '?',
        'Settings: ' + json.dumps(config, ensure_ascii=False),
        'Recent crash logs: ' + (', '.join(c.name for c in crashes) or 'none'),
    ]
    for name in LOGS:
        parts += [f'--- {name} (last {TAIL_LINES} lines) ---', tail(Path(user_dir) / name)]
    return scrub('\n'.join(parts))


def save_zip(version, report, user_dir):
    """user/reports/report_<date>.zip: the report plus the whole session logs and recent crash logs."""
    out_dir = Path(user_dir) / 'reports'
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / ('report_' + time.strftime('%Y-%m-%d_%H-%M-%S') + '.zip')
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('report.txt', report)
        for name in LOGS:
            f = Path(user_dir) / name
            if f.is_file():
                z.writestr(name, scrub(f.read_text(encoding='utf-8', errors='replace')))
        diag = Path(user_dir) / 'texture_diag.log'
        if diag.is_file():
            with open(diag, 'rb') as fh:
                fh.seek(0, os.SEEK_END)
                fh.seek(max(0, fh.tell() - DIAG_TAIL))
                z.writestr('texture_diag_tail.log', scrub(fh.read().decode('utf-8', errors='replace')))
        crashes = Path(user_dir) / 'crashes'
        for f in (sorted(crashes.glob('*.log'))[-3:] if crashes.is_dir() else []):
            z.writestr('crashes/' + f.name, scrub(f.read_text(encoding='utf-8', errors='replace')))
    return path


def issue_url(version, report):
    """A new-issue URL with the first lines of the report; the full text is on the clipboard."""
    head = report[:URL_BODY_MAX]
    body = ('**O que aconteceu / What happened:**\n\n\n'
            '**Relatório (o texto completo está na área de transferência, cole abaixo) / Report '
            '(full text is on the clipboard, paste below):**\n\n```\n' + head + '\n```\n')
    query = urllib.parse.urlencode({'title': f'[{version}] ', 'body': body})
    return ISSUES_URL + '?' + query


def open_issue(version, report):
    os.startfile(issue_url(version, report))
