#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Shadow of the Colossus (CUSA08809, EU 1.01) on PC: prepares the game image and starts it.

    python sotc.py                      (or double-click SotC.bat)

Settings (environment variables, or sotc.ini beside this script as NAME=value lines):
    SOTC_GAME_DIR     the dumped game folder (the one with eboot.bin); default ../CUSA08809
    SOTC_SYS_MODULES  folder with YOUR OWN console's decrypted libSceNgs2.sprx and
                      libSceUlt.sprx (shadPS4's user/sys_modules). Without it the game is silent.
                      These files are Sony firmware: they are never distributed with the port.
    SOTC_USER_DIR     saves, shader/pipeline caches; default ./user
    SOTC_UPDATE_DIR   optional: the game update; by default <game>-UPDATE or <game>-patch next to
                      the dump (as shadPS4); its files take the place of the base game's
    SOTC_DLC_DIR      optional: the game's DLC (one folder per add-on with sce_sys/param.sfo,
                      or shadPS4's addcont folder holding CUSA08809)
    SOTC_MODES        optional, comma-separated: af16 (16x anisotropic filtering),
                      fps60 (removes the 30 FPS cap; experimental, vblank at 120 Hz)
    SOTC_CVARS        engine settings, NAME=VALUE separated by spaces (e.g. r_motionBlurEnable=0)
    BB_GAMEPAD        the gamepad to use (its name or part of it), when several are connected
    key.<input>=Keys, pad.<input>=buttons   remap the controls (README.md, Controles)
The image is prepared again only when the game, the modules or the scripts change.
Extra arguments go to bb-probe.exe.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys

PORT = Path(__file__).resolve().parent
SCRIPTS = PORT / 'scripts'
SYS_MODULES = ('libSceUlt.sprx', 'libSceNgs2.sprx')


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def load_ini():
    ini = PORT / 'sotc.ini'
    if ini.is_file():
        for line in ini.read_text(encoding='utf-8', errors='replace').splitlines():
            line = line.strip()
            if line and not line.startswith(('#', ';')) and '=' in line:
                name, value = line.split('=', 1)
                os.environ.setdefault(name.strip(), value.strip())


def no_console():
    """Started without a console (the launcher): console programs must not open their own."""
    if os.name != 'nt':
        return 0
    import ctypes
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def find_probe():
    name = 'bb-probe.exe' if os.name == 'nt' else 'bb-probe'  # Linux: run.sh builds out/bb-probe
    for candidate in (PORT / 'bin' / name, PORT / 'out' / name):
        if candidate.is_file():
            return candidate
    fail(f'{name} not found: build it with build.sh (MSYS2 CLANG64 on Windows) or use a packaged build.')


def run_script(name, *args):
    # Inside the packaged SotC.exe (PyInstaller) the executable runs the scripts itself.
    script = ['--script'] if getattr(sys, 'frozen', False) else []
    result = subprocess.run([sys.executable, *script, str(SCRIPTS / name), *map(str, args)], stdin=subprocess.DEVNULL,
                            creationflags=no_console())
    if result.returncode:
        sys.exit(result.returncode)


def fingerprint(game, modules):
    """Inputs of the prepared image: eboot, bundled modules, the user's modules, the scripts."""
    digest = hashlib.sha256()
    files = [game / 'eboot.bin', *sorted((game / 'sce_module').glob('*.prx')), *modules,
             *sorted(SCRIPTS.glob('*.py'))]
    for path in files:
        stat = path.stat()
        digest.update(f'{path.resolve()}|{stat.st_size}|{stat.st_mtime_ns}\n'.encode())
    return digest.hexdigest()


def game_update(game):
    """The game update next to the dump, as shadPS4 uses it (<game>-UPDATE or <game>-patch), or
    SOTC_UPDATE_DIR; None without one."""
    named = os.environ.get('SOTC_UPDATE_DIR')
    if named:
        return Path(named).resolve()
    for suffix in ('-UPDATE', '-patch'):
        folder = game.with_name(game.name + suffix)
        if folder.is_dir():
            return folder
    return None


def merged_source(game, update, out):
    """The files the image is prepared from when an update is present: its eboot.bin, the base
    modules with the update's on top, its param.sfo (out/app-update, refreshed when they change)."""
    merged = out / 'app-update'
    pairs = [(update / 'eboot.bin', merged / 'eboot.bin')]
    for folder in (game / 'sce_module', update / 'sce_module'):
        if folder.is_dir():
            pairs += [(p, merged / 'sce_module' / p.name) for p in folder.iterdir() if p.is_file()]
    for folder in (game / 'sce_sys', update / 'sce_sys'):
        if (folder / 'param.sfo').is_file():
            pairs.append((folder / 'param.sfo', merged / 'sce_sys' / 'param.sfo'))
    for source, target in pairs:
        stat = source.stat()
        current = target.stat() if target.is_file() else None
        if not current or current.st_size != stat.st_size or current.st_mtime_ns != stat.st_mtime_ns:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    return merged


def main():
    load_ini()
    env = os.environ
    os.chdir(PORT)
    out = PORT / 'out'
    out.mkdir(exist_ok=True)
    game = Path(env.get('SOTC_GAME_DIR', PORT.parent / 'CUSA08809')).resolve()
    if not (game / 'eboot.bin').is_file():
        fail(f'No eboot.bin in {game}: set SOTC_GAME_DIR (environment or sotc.ini).')

    modules = []
    sys_modules = env.get('SOTC_SYS_MODULES')
    if sys_modules:
        modules = [Path(sys_modules) / name for name in SYS_MODULES]
        missing = [m.name for m in modules if not m.is_file()]
        if missing:
            print(f'SOTC_SYS_MODULES: {", ".join(missing)} not found in {sys_modules}; no sound', file=sys.stderr)
            modules = []
    else:
        print('SOTC_SYS_MODULES not set: no sound (see sotc.py)', file=sys.stderr)

    # A complete update (shadPS4's <game>-UPDATE): the image comes from its eboot, and its files
    # take the place of the base game's while the game runs (src/runtime_file.c).
    update = game_update(game)
    source = game
    if update:
        print(f'Game update: {update}')
        env['SOTC_UPDATE_DIR'] = str(update)
        if (update / 'eboot.bin').is_file():
            source = merged_source(game, update, out)
    stamp = out / 'prepared.txt'
    wanted = fingerprint(source, modules)
    images = [out / name for name in ('boot-linked.bin', 'content.bin', 'sotc-cpu.bin')]
    if not stamp.is_file() or stamp.read_text().strip() != wanted or not all(p.is_file() for p in images):
        print('Preparing the game image...')
        run_script('prepare.py', source, '--out', out)
        link = ['link_modules.py', source, '--out', out]
        if modules:
            link += ['--sys-modules', modules[0].parent]
        run_script(*link)
        run_script('content_profile.py', source, '--out', out)
        run_script('sotc_cpu.py', source, '--out', out)
        stamp.write_text(wanted + '\n')

    modes = env.get('SOTC_MODES', '')
    cvars = env.get('SOTC_CVARS')
    if cvars is None:
        # Without the launcher: the culling settings of Pink-shadPS4 (same image, fewer GPU waits).
        cvars = 'doIndexBufferCulling=0 doSunShadowIndexBufferCulling=0'
    run_script('sotc_modes.py', source, '--out', out, '--modes', modes, '--cvars', cvars)
    if 'fps60' in modes.lower():
        # The game waits for vblanks: at 60 Hz an uncapped frame still rounds up to 30 FPS.
        env.setdefault('BB_VBLANK_HZ', '120')

    probe = find_probe()
    # Development builds run from the MSYS2 tree: its CLANG64 DLLs (SDL3, FFmpeg, ...).
    if os.name == 'nt' and not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
    user = Path(env.get('SOTC_USER_DIR', PORT / 'user')).resolve()
    user.mkdir(parents=True, exist_ok=True)
    env.setdefault('BB_GPU_USER_DIR', str(user))
    # Controls: key.<input>= / pad.<input>= lines of sotc.ini remap keys and buttons (src/runtime_pad.c).
    env.setdefault('BB_CONFIG', str(PORT / 'sotc.ini'))
    command = [str(probe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
               '--sotc-cpu', str(out / 'sotc-cpu.bin'), '--patches', str(out / 'patches.bin'),
               *(['--sotc-cmdline', str(out / 'cmdline.bin')] if (out / 'cmdline.bin').is_file() else []),
               '--app0', str(game), '--user', str(user), *sys.argv[1:]]
    # SOTC_WRAP: a JSON list put in front of the game, e.g. RenderDoc
    # ["C:/Program Files/RenderDoc/renderdoccmd.exe", "capture", "-w", "-c", "<file>"]; F12 then
    # captures the frame (SOTC_RDOC_SUBMITS submissions).
    if env.get('SOTC_WRAP'):
        import json
        command = json.loads(env['SOTC_WRAP']) + command
    return subprocess.run(command, stdin=subprocess.DEVNULL, creationflags=no_console()).returncode


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
