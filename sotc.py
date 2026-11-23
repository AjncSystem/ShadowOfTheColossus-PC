#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Shadow of the Colossus (CUSA08809 EU 1.01 or CUSA08034 US 1.00) on PC: prepares the game image
and starts it.

    python sotc.py                      (or double-click SotC.bat)

Settings (environment variables, or sotc.ini beside this script as NAME=value lines):
    SOTC_GAME_DIR     the dumped game folder (the one with eboot.bin); default ../CUSA08809,
                      else ../CUSA08034
    SOTC_SYS_MODULES  folder with YOUR OWN console's decrypted libSceNgs2.sprx and
                      libSceUlt.sprx (shadPS4's user/sys_modules). Without it a built-in mixer plays the sound.
                      These files are Sony firmware: they are never distributed with the port.
    SOTC_USER_DIR     saves, shader/pipeline caches; default ./user
    SOTC_UPDATE_DIR   optional: the game update; by default <game>-UPDATE or <game>-patch next to
                      the dump (as shadPS4); its files take the place of the base game's
    SOTC_DLC_DIR      optional: the game's DLC (one folder per add-on with sce_sys/param.sfo,
                      or shadPS4's addcont folder holding CUSA08809 / CUSA08034); only DLC of the
                      game's own region is listed
    SOTC_MODES        optional, comma-separated: af16 (16x anisotropic filtering),
                      fps60 (removes the 30 FPS cap; experimental, vblank at 120 Hz),
                      fps60game (the game's own 60 FPS target, as the PS4 Pro Performance mode;
                      with SOTC_FPS_CAP=off)
                      hdr (HDR10 output and the game's HDR rendering; Windows HDR must be on)
    SOTC_RENDER_RES   optional: the game renders at WxH instead of 1920x1080 (e.g. 2560x1080 for
                      21:9, a wider view); the output follows its aspect; PS4 mode only
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
import time

PORT = Path(__file__).resolve().parent
DATA = Path(os.environ.get('SOTC_DATA_DIR', PORT)).resolve()
SCRIPTS = PORT / 'scripts'
SYS_MODULES = ('libSceUlt.sprx', 'libSceNgs2.sprx')


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def load_ini():
    ini = DATA / 'sotc.ini'
    if ini.is_file():
        for line in ini.read_text(encoding='utf-8', errors='replace').splitlines():
            line = line.strip()
            if line and not line.startswith(('#', ';')) and '=' in line:
                name, value = line.split('=', 1)
                os.environ.setdefault(name.strip(), value.strip())


def load_launcher_settings():
    """Apply settings saved by the in-game F1 menu (launcher_settings.json) when present."""
    import json
    user_dir = Path(os.environ.get('SOTC_USER_DIR', DATA / 'user')).resolve()
    cfg_path = Path(os.environ.get('SOTC_LAUNCHER_SETTINGS') or (user_dir / 'launcher_settings.json'))
    if not cfg_path.is_file():
        return
    try:
        s = json.loads(cfg_path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return
    if not isinstance(s, dict):
        return
    os.environ.setdefault('SOTC_LAUNCHER_SETTINGS', str(cfg_path))
    env = os.environ
    if 'fullscreen' in s:
        env['BB_FULLSCREEN'] = '1' if s['fullscreen'] else '0'
    output_res = str(s.get('output_res') or '')
    if output_res:
        env['SOTC_OUTPUT_RES'] = output_res
    neo = bool(s.get('neo')) and output_res not in ('2560x1080', '3440x1440', '3840x1080', '5120x1440')
    if 'neo' in s:
        env['SOTC_NEO'] = '1' if neo else '0'
    if s.get('render_res'):
        env['SOTC_RENDER_RES'] = str(s['render_res'])
    elif output_res and output_res not in ('1280x720', '1920x1080') and not neo:
        env['SOTC_RENDER_RES'] = output_res
    if 'fps_mode' in s or 'af16' in s or 'hdr' in s:
        fps_mode = str(s.get('fps_mode') or '60')
        modes = ['af16'] if s.get('af16', True) else []
        if fps_mode != '30':
            modes.append('fps60game')
        modes.append('grassbuf')
        if s.get('hdr'):
            modes.append('hdr')
        env['SOTC_MODES'] = ','.join(modes)
        env['SOTC_FPS_CAP'] = '30' if fps_mode == '30' else 'off'
        env['SOTC_FPS_LIMIT'] = '60' if fps_mode == '60' else '0'
        if fps_mode == '60':
            if s.get('fps_dynamic', True):
                env.setdefault('SOTC_FPS_DYNAMIC', '48')
            else:
                env['SOTC_FPS_DYNAMIC'] = '0'
    if s.get('present_mode'):
        env['BB_PRESENT_MODE'] = str(s['present_mode'])
    if s.get('language'):
        env['BB_LANGUAGE'] = str(s['language'])
    if s.get('ui_language'):
        env['SOTC_UI_LANG'] = str(s['ui_language'])
    if 'show_fps' in s:
        if s['show_fps']:
            env['SOTC_FPS'] = '1'
        else:
            env.pop('SOTC_FPS', None)
    upscaler = str(s.get('upscaler') or '')
    if upscaler:
        env['BB_FSR1'] = '1' if upscaler in ('fsr1', 'dlss', 'fsr4') else '0'
        if upscaler in ('dlss', 'fsr4'):
            env['SOTC_UPSCALER'] = upscaler
        else:
            env.pop('SOTC_UPSCALER', None)
    if s.get('sharpness'):
        sharp_rcas = {'high': '0.00', 'medium': '0.30', 'low': '0.60', 'off': '1.00'}
        sharp_dlss = {'high': '1.0', 'medium': '0.6', 'low': '0.3', 'off': '-1'}
        val = str(s['sharpness'])
        env['BB_FSR_SHARPEN'] = '0' if val == 'off' else '1'
        if val in sharp_rcas:
            env['BB_FSR_SHARPNESS'] = sharp_rcas[val]
        env['SOTC_DLSS_SHARPEN'] = sharp_dlss.get(val, val)
    if s.get('framegen'):
        env['SOTC_FRAMEGEN'] = str(s['framegen'])
    if 'textures' in s:
        tex = str(s['textures'] or 'high')
        if tex != 'off':
            env.setdefault('SOTC_LOD_LINGER', '1')
            env.setdefault('SOTC_LOD_FEEDBACK', '1')
            tex_map = {'low': '3', 'medium': '2', 'high': '1', 'max': '0'}
            if tex in tex_map:
                env['SOTC_LOD_LEVEL'] = tex_map[tex]
        else:
            env['SOTC_LOD_FEEDBACK'] = '0'
    env.setdefault('SOTC_POOL_FREEZE', '1')
    env.setdefault('SOTC_RELEASE_DEFER', '1')
    env.setdefault('SOTC_IMAGE_DEDICATED', '1')
    if 'perf_record_thread' in s:
        env['SOTC_RECORD_THREAD'] = '1' if s['perf_record_thread'] else '0'
    if 'shadow_reuse' in s:
        env['SOTC_SHADOW_REUSE'] = str(s['shadow_reuse'])
    if 'volume' in s:
        env['SOTC_AUDIO_VOLUME'] = str(s['volume'])
    if str(s.get('audio_channels') or '') in ('2', '6', '8'):
        env['SOTC_AUDIO_CHANNELS'] = str(s['audio_channels'])
    if 'audio_lfe' in s and not s['audio_lfe']:
        env['SOTC_AUDIO_LFE'] = '0'
    if str(s.get('audio_device') or '').strip():
        env['SOTC_AUDIO_DEVICE'] = str(s['audio_device']).strip()
    if 'mouse' in s:
        env['SOTC_MOUSE'] = '1' if s['mouse'] else '0'
    if 'mouse_sens' in s:
        env['SOTC_MOUSE_SENS'] = str(s['mouse_sens'])
    if 'mouse_invert' in s:
        env['SOTC_MOUSE_INVERT'] = '1' if s['mouse_invert'] else '0'
    if 'rumble' in s:
        env['SOTC_RUMBLE'] = '1' if s['rumble'] else '0'
    if s.get('button_icons') == 'xbox' and (PORT / 'glyphs' / 'xbox').is_dir():
        env['SOTC_TEX_REPLACE'] = str(PORT / 'glyphs' / 'xbox')
    elif s.get('button_icons') == 'ps':
        env.pop('SOTC_TEX_REPLACE', None)
    if any(k in s for k in ('grass_distance', 'grass_height', 'weed_distance', 'weed_height', 'grass_detail')):
        env['SOTC_GRASS'] = ' '.join(str(s.get(k) or '1') for k in (
            'grass_distance', 'grass_height', 'weed_distance', 'weed_height', 'grass_detail'))
    culling = ['doIndexBufferCulling=0', 'doSunShadowIndexBufferCulling=0'] if s.get('perf_culling', True) else []
    cvars = [c for c in env.get('SOTC_CVARS', '').split()
             if c not in ('skipSplash=1', 'nointro', 'doIndexBufferCulling=0', 'doSunShadowIndexBufferCulling=0')
             and not c.startswith(('r_virtualGeometryNumShadowCascades', 'r_virtualGeometryFadeMultiplier='))]
    cvars = culling + cvars
    if s.get('skip_logos'):
        cvars.append('skipSplash=1')
    if s.get('skip_film'):
        cvars.append('nointro')
    grass = str(s.get('grass_shadows') or '2')
    if grass in ('3', '4'):
        cvars += [f'r_virtualGeometryNumShadowCascades{m}={grass}' for m in ('', '_BasePS4', '_Perf', '_Cinematic')]
    fade = str(s.get('grass_fade') or '1')
    if fade in ('0.5', '0.75', '2', '3'):
        cvars.append(f'r_virtualGeometryFadeMultiplier={fade}')
    env['SOTC_CVARS'] = ' '.join(cvars)


def no_console():
    """Started without a console (the launcher): console programs must not open their own."""
    if sys.platform != 'win32':
        return 0
    import ctypes
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def find_probe():
    names = ('bb-probe.exe',) if sys.platform == 'win32' else ('bb-probe', 'bb-probe.exe')
    for folder in (PORT / 'bin', PORT / 'out'):
        for name in names:
            candidate = folder / name
            if candidate.is_file():
                return candidate
    fail('bb-probe not found: build it with build.sh or use a packaged build.')


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
    load_launcher_settings()
    env = os.environ
    os.chdir(DATA)
    out = DATA / 'out'
    out.mkdir(exist_ok=True)
    # The EU dump (CUSA08809) next to the port, else the US one (CUSA08034).
    default = next((DATA.parent / g for g in ('CUSA08809', 'CUSA08034') if (DATA.parent / g / 'eboot.bin').is_file()),
                   DATA.parent / 'CUSA08809')
    game = Path(env.get('SOTC_GAME_DIR', default)).resolve()
    if not (game / 'eboot.bin').is_file():
        fail(f'No eboot.bin in {game}: set SOTC_GAME_DIR (environment or sotc.ini) to the CUSA08809 (EU) '
             'or CUSA08034 (US) dump.')

    modules = []
    sys_modules = env.get('SOTC_SYS_MODULES')
    if sys_modules:
        modules = [Path(sys_modules) / name for name in SYS_MODULES]
        missing = [m.name for m in modules if not m.is_file()]
        if missing:
            print(f'SOTC_SYS_MODULES: {", ".join(missing)} not found in {sys_modules}; the built-in mixer plays the sound', file=sys.stderr)
            modules = []
    else:
        print('SOTC_SYS_MODULES not set: the built-in mixer plays the sound (see sotc.py)', file=sys.stderr)

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
    prebuilt = env.get('SOTC_PREBUILT') == '1' and all(p.is_file() for p in images)
    if not prebuilt and (not stamp.is_file() or stamp.read_text().strip() != wanted or not all(p.is_file() for p in images)):
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
    render = env.get('SOTC_RENDER_RES', '').strip()
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
    user = Path(env.get('SOTC_USER_DIR', DATA / 'user')).resolve()
    user.mkdir(parents=True, exist_ok=True)
    env.setdefault('BB_GPU_USER_DIR', str(user))
    # Controls: key.<input>= / pad.<input>= lines of sotc.ini remap keys and buttons (src/runtime_pad.c).
    env.setdefault('BB_CONFIG', str(DATA / 'sotc.ini'))
    command = [str(probe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
               '--sotc-cpu', str(out / 'sotc-cpu.bin'), '--patches', str(out / 'patches.bin'),
               *(['--sotc-cmdline', str(out / 'cmdline.bin')] if (out / 'cmdline.bin').is_file() else []),
               '--app0', str(game), '--user', str(user), '--timeout', '0', *sys.argv[1:]]
    # SOTC_WRAP: a JSON list put in front of the game, e.g. RenderDoc
    # ["C:/Program Files/RenderDoc/renderdoccmd.exe", "capture", "-w", "-c", "<file>"]; F12 then
    # captures the frame (SOTC_RDOC_SUBMITS submissions).
    if env.get('SOTC_WRAP'):
        import json
        command = json.loads(env['SOTC_WRAP']) + command
    # In a console the session also goes to user/last_run.log (as the launcher does; the launcher
    # reads this script's output into its own log, so a pipe means it is not written here).
    if not sys.stdout.isatty():
        def play():
            return subprocess.run(command, stdin=subprocess.DEVNULL, creationflags=no_console()).returncode
        return with_range_retries(env, play)
    with open_session_log(user) as log:
        def play():
            game = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT)
            try:
                for line in game.stdout:
                    sys.stdout.buffer.write(line)
                    sys.stdout.buffer.flush()
                    log.write(line)
            finally:
                code = game.wait()
            return code
        code = with_range_retries(env, play)
    keep_crash_log(user, code)
    return code


RANGE_BUSY_EXIT = 87  # src/runtime_memory.c: the guest's address range was taken at start
RANGE_RETRIES = 10


def with_range_retries(env, play):
    """Runs play() and starts it again while the game reports its address range taken (error 487,
    issue #110). What sits there usually differs per process, so a new start gets through."""
    for attempt in range(RANGE_RETRIES):
        env['SOTC_RANGE_RETRY'] = str(attempt)
        code = play()
        if code != RANGE_BUSY_EXIT:
            return code
        print(f'sotc: address range taken, starting again ({attempt + 1}/{RANGE_RETRIES})', flush=True)
        time.sleep(0.5)
    print('sotc: the address range stayed taken. The lines above name what sits there; closing '
          'overlays and hooking tools (recording, RGB, tuning, antivirus exceptions) should free it.',
          flush=True)
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
