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
                      fps60 (removes the 30 FPS cap; experimental, vblank at 120 Hz),
                      fps60game (the game's own 60 FPS target, as the PS4 Pro Performance mode;
                      with SOTC_FPS_CAP=off)
                      hdr (HDR10 output and the game's HDR rendering; Windows HDR must be on)
    SOTC_RENDER_RES   optional: the game renders at WxH instead of 1920x1080 (e.g. 2560x1080 for
                      21:9, a wider view); the output follows its aspect; PS4 mode only; at most 1080
                      lines tall and 4.2 million pixels, larger sizes are scaled down
    SOTC_CVARS        engine settings, NAME=VALUE separated by spaces (e.g. r_motionBlurEnable=0)
    BB_GAMEPAD        the gamepad to use (its name or part of it), when several are connected
    key.<input>=Keys, pad.<input>=buttons   remap the controls (README.md, Controles)
The image is prepared again only when the game, the modules or the scripts change.
Extra arguments go to bb-probe.exe. Run from a console, the session is also saved to
<user>/last_run.log (the 4 before it as last_run.1.log ... last_run.4.log; a crash's log is
copied to <user>/crashes).
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
    import ctypes
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def find_probe():
    for candidate in (PORT / 'bin' / 'bb-probe.exe', PORT / 'out' / 'bb-probe.exe'):
        if candidate.is_file():
            return candidate
    fail('bb-probe.exe not found: build it with build.sh (MSYS2 CLANG64) or use a packaged build.')


# Above about 4.2 million rendered pixels the fine detail textures (ground, cloth) shimmer, flicker
# and then disappear (2736x1536 is fine, 3008x1696 shimmers, 3840x2160 has none); cause unknown.
# The game itself never renders more than 1080 lines in PS4 mode, so the render height stays at
# most 1080 (3840x1080 for 32:9 is tested fine) and the pixel limit covers wider aspects.
MAX_RENDER_HEIGHT = 1080
MAX_RENDER_PIXELS = 4_200_000


def cap_render(size):
    """SOTC_RENDER_RES WxH, scaled down (same aspect, multiples of 8) to MAX_RENDER_HEIGHT lines
    and MAX_RENDER_PIXELS."""
    try:
        w, h = map(int, size.split('x'))
    except ValueError:
        return size  # sotc_modes.py reports it
    if w <= 0 or h <= 0:
        return size  # sotc_modes.py reports it
    scale = min(1.0, MAX_RENDER_HEIGHT / h, (MAX_RENDER_PIXELS / (w * h)) ** 0.5)
    if scale == 1.0:
        return size
    capped = f'{int(w * scale) // 8 * 8}x{int(h * scale) // 8 * 8}'
    print(f'sotc: render size {size} is above {MAX_RENDER_HEIGHT} lines or {MAX_RENDER_PIXELS:,} pixels '
          f'(fine textures disappear), rendering at {capped}', file=sys.stderr)
    return capped


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
    for folder in (game, update):
        if folder and (folder / 'eboot.bin').is_file() and (folder / 'eboot.bin').stat().st_size == 0:
            fail(f'{folder / "eboot.bin"} is empty (0 bytes): the copy was interrupted. Copy the file again.')
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
    if env.get('SOTC_NOTE'):
        print(f"sotc: {env['SOTC_NOTE']}", file=sys.stderr)
    render = cap_render(env.get('SOTC_RENDER_RES', '').strip())
    if render == '1920x1080':
        # A 16:9 output above 1080p renders at 1080p as before: no patches, no extra memory.
        render = ''
    if render:
        env['SOTC_RENDER_RES'] = render  # the GPU library reads it (bilinear 2D art above 1080p)
    else:
        env.pop('SOTC_RENDER_RES', None)
    run_script('sotc_modes.py', source, '--out', out, '--modes', modes, '--cvars', cvars,
               *(['--render', render] if render else []))
    if render:
        # The output keeps the render size's aspect (a 16:9 output would stretch a 21:9 image).
        try:
            rw, rh = map(int, render.split('x'))
            ow, oh = map(int, env.get('SOTC_OUTPUT_RES', '').split('x'))
            same_aspect = abs(ow * rh - oh * rw) <= oh * rw // 100
        except ValueError:
            same_aspect = False
        if not same_aspect:
            env['SOTC_OUTPUT_RES'] = render
        # Larger render targets: the game's heaps take what direct memory there is (run.sh).
        env.setdefault('BB_DMEM_MB', '9152')
    # The game's direct memory allocator (a 64 KiB page bitmap over all of it) filled up and read
    # past the bitmap (guest offset 0x1a371, read of -1): in PS4 Pro mode with 5696 MiB when a save
    # loaded, and in PS4 mode with 5056 MiB on loading the last colossus' area (user logs, 19
    # Oct; high quality textures keep more fine mips resident than on a PS4). Every mode gets the
    # 9152 MiB the wide render sizes already used.
    env.setdefault('BB_DMEM_MB', '9152')
    # The game waits for vblanks: at 60 Hz an uncapped frame (fps60, or F7 in game) still rounds
    # up to 30 FPS. With the cap, 120 Hz only shows a late frame sooner (and SOTC_FPS_LIMIT counts
    # in 120 Hz vblanks).
    env.setdefault('BB_VBLANK_HZ', '120')
    if 'hdr' in modes.lower():
        # HDR10 swapchain (Rec.2020 PQ, when Windows HDR is on); the game then offers HDR too.
        env.setdefault('BB_HDR', '1')

    probe = find_probe()
    # Development builds run from the MSYS2 tree: its CLANG64 DLLs (SDL3, FFmpeg, ...).
    if not (probe.parent / 'SDL3.dll').is_file():
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
    # In a console the session also goes to user/last_run.log (as the launcher does; the launcher
    # reads this script's output into its own log, so a pipe means it is not written here).
    if not sys.stdout.isatty():
        return subprocess.run(command, stdin=subprocess.DEVNULL, creationflags=no_console()).returncode
    with open_session_log(user) as log:
        game = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        try:
            for line in game.stdout:
                sys.stdout.buffer.write(line)
                sys.stdout.buffer.flush()
                log.write(line)
        finally:
            code = game.wait()
    keep_crash_log(user, code)
    return code


LOGS_KEPT = 5        # last_run.log and last_run.1.log ... last_run.4.log
CRASH_LOGS_KEPT = 20


def open_session_log(user):
    """A fresh user/last_run.log; the previous sessions' logs move to last_run.1.log (the one
    before) ... last_run.4.log. Same files as the launcher (launcher/sotc_launcher_win.py)."""
    for i in range(LOGS_KEPT - 1, 0, -1):
        older = user / ('last_run.log' if i == 1 else f'last_run.{i - 1}.log')
        if older.is_file():
            try:
                os.replace(older, user / f'last_run.{i}.log')
            except OSError:
                pass
    return open(user / 'last_run.log', 'wb', buffering=0)


def keep_crash_log(user, code):
    """After a session that ended with an error code, copies its log to
    user/crashes/<date>_<time>_<code>.log (the last 20, and the last 5 minidumps, are kept)."""
    if code == 0:
        return
    import time
    crashes = user / 'crashes'
    try:
        crashes.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime('%Y-%m-%d_%H-%M-%S')
        shutil.copy2(user / 'last_run.log', crashes / f'{stamp}_{code & 0xffffffff:#x}.log')
        for old in sorted(crashes.glob('*.log'))[:-CRASH_LOGS_KEPT]:
            old.unlink()
        for old in sorted(crashes.glob('*.dmp'))[:-5]:
            old.unlink()
    except OSError:
        pass


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
