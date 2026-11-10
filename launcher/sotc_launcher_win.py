#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Shadow of the Colossus launcher for Windows (Tkinter), modelled on bbport_launcher_win.py.

One window for the port's settings: the game dump, the user's own sound modules, saves, modes
(16x AF, 60 FPS), display and diagnostics. They go to sotc.py as environment variables.
Launcher options live in %APPDATA%/sotc-launcher/settings.json.

Frozen with PyInstaller (packaging/windows/package_sotc.sh) the same SotC.exe also runs the
game without the window (`--play`), sotc.py (`--run`) and the preparation scripts (`--script`),
so a packaged port needs no Python installation.
"""
import ctypes
import json
import os
import platform
from pathlib import Path
import queue
import re
import runpy
import shutil
import subprocess
import sys
import threading
import time
import zipfile

import sotc_frame
import sotc_hardware
import sotc_pad
import sotc_search
import sotc_theme as theme
import sotc_update

FROZEN = getattr(sys, 'frozen', False)
PORT_DIR = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT_DIR / 'scripts'))
# Settings of the copy the launcher runs from, in its user folder (kept by updates): each copy
# of the port has its own saves, caches and logs. Before 0.59.2 every copy shared
# %APPDATA%\sotc-launcher\settings.json, so a new copy went on using the data folder of the one
# set up first.
LEGACY_CONFIG_DIR = Path(os.environ.get('APPDATA', Path.home())) / 'sotc-launcher'
LEGACY_CONFIG_FILE = LEGACY_CONFIG_DIR / 'settings.json'
CONFIG_DIR = PORT_DIR / 'user'
CONFIG_FILE = CONFIG_DIR / 'launcher_settings.json'
# The supported releases (title ID -> region, version) share one eboot build; scripts/ is on
# sys.path above. The EU one (CUSA08809) is the default game folder.
from sotc_modes import GAMES  # noqa: E402
GAME_ID = 'CUSA08809'
SYS_MODULES = ('libSceUlt.sprx', 'libSceNgs2.sprx')
VERSION = '0.74'
MAX_LOG_LINES = 6000
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


# ---------------------------------------------------------------------------------------------
# Command line roles of the frozen executable.

def attach_stdio():
    """A windowed executable starts without sys.stdout; inherited pipes or a console still exist."""
    import msvcrt
    for name, std in (('stdout', -11), ('stderr', -12)):
        if getattr(sys, name) is not None:
            continue
        stream = None
        handle = ctypes.windll.kernel32.GetStdHandle(std)
        if handle and handle != ctypes.c_void_p(-1).value:
            try:
                stream = open(msvcrt.open_osfhandle(handle, os.O_WRONLY), 'w', encoding='utf-8',
                              errors='replace', buffering=1)
            except OSError:
                stream = None
        setattr(sys, name, stream or open(os.devnull, 'w'))
    # An inherited handle can exist and still refuse writes ("[Errno 22] Invalid argument" from a
    # print in link_modules.py, issue #104 upstream): this output is informational, so a failed
    # write drops the stream instead of aborting the preparation.
    for name in ('stdout', 'stderr'):
        setattr(sys, name, _TolerantStream(getattr(sys, name)))


class _TolerantStream:
    def __init__(self, stream):
        self._stream = stream

    def write(self, text):
        try:
            return self._stream.write(text)
        except (OSError, ValueError):
            self._stream = open(os.devnull, 'w')
            return len(text)

    def flush(self):
        try:
            self._stream.flush()
        except (OSError, ValueError):
            pass

    def __getattr__(self, attr):
        return getattr(self._stream, attr)


def run_role(argv):
    """--run [args]: sotc.py; --script <file> [args]: a preparation script. Returns an exit code."""
    attach_stdio()
    for stream in (sys.stdout, sys.stderr):
        try:
            # UTF-8 whatever the Windows code page (gbk etc. cannot print the game title's ™).
            stream.reconfigure(line_buffering=True, encoding='utf-8', errors='replace')
        except (AttributeError, ValueError):
            pass
    if argv[0] == '--script':
        path, sys.argv = argv[1], argv[1:]
    else:
        path = str(PORT_DIR / 'sotc.py')
        sys.argv = [path, *argv[1:]]
    try:
        runpy.run_path(path, run_name='__main__')
    except SystemExit as stop:
        return stop.code if isinstance(stop.code, int) else (0 if stop.code is None else 1)
    return 0


def run_command():
    """The command that starts sotc.py: this executable when frozen, else Python."""
    return [sys.executable, '--run'] if FROZEN else [sys.executable, str(PORT_DIR / 'sotc.py')]


# ---------------------------------------------------------------------------------------------
# Texts: Portuguese first, English for every other Windows language.

LANG = 'pt'
# Spanish, Chinese (Simplified), Korean and Russian are looked up by the Portuguese text
# (launcher/sotc_i18n.py); a text missing there shows in English.
try:
    from sotc_i18n import TRANSLATIONS
except ImportError:
    TRANSLATIONS = {}


def _(pt, en):
    if LANG == 'pt':
        return pt
    table = TRANSLATIONS.get(LANG)
    return table.get(pt, en) if table else en


sotc_search._ = sotc_pad._ = _  # their texts in the launcher's language


def ui_font():
    """Interface font: Inter (sotc_theme), which, like Segoe UI, has no Chinese or Korean characters."""
    return {'zh': 'Microsoft YaHei UI', 'ko': 'Malgun Gothic'}.get(LANG, theme.BODY)


def title_font():
    """Titles: Cinzel (sotc_theme) for Latin and Cyrillic-free text; Russian keeps Georgia."""
    return {'zh': 'Microsoft YaHei UI', 'ko': 'Malgun Gothic', 'ru': 'Georgia'}.get(LANG, theme.HEAD)


def windows_language():
    try:
        primary = ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff
    except (AttributeError, OSError):
        primary = 0
    return {0x16: 'pt', 0x0a: 'es', 0x04: 'zh', 0x12: 'ko', 0x19: 'ru'}.get(primary, 'en')


# ---------------------------------------------------------------------------------------------
# Settings, passed to sotc.py as environment variables.

APP_DEFAULTS = {'ui_language': '', 'game_dir': str(PORT_DIR.parent / GAME_ID), 'sys_modules': '',
                'user_dir': '', 'af16': True, 'textures': 'high', 'hdr': False, 'window_icon_file': '', 'fps60': False, 'fps_mode': '30', 'fps_dynamic': True, 'button_icons': 'ps', 'fullscreen': False,
                'present_mode': 'Mailbox', 'language': '', 'player_name': '', 'show_fps': False,
                'readbacks': '', 'extra_env': '', 'debug_menu': False, 'debug_watch': False, 'rb_hot': True, 'skip_logos': True, 'skip_film': False, 'close_on_play': False, 'gamepad': '', 'rumble': True,
                'output_res': '1920x1080', 'upscaler': 'fsr1', 'sharpness': '250', 'mouse': True,
                'mouse_sens': '1.0', 'mouse_invert': False, 'neo': False, 'fx_vignette': True,
                'fx_fog': True, 'fx_ssao': True, 'fx_motionblur': False, 'engine_cvars': '', 'perf_culling': True,
                'perf_record_thread': True, 'perf_dma': True, 'gpu_submit': 'auto', 'vram_use': 'normal', 'vram_profile': 'auto', 'gpu_vram_mb': 0, 'volume': '100', 'shadow_reuse': '0', 'grass_shadows': '2', 'grass_distance': '1', 'grass_height': '1', 'weed_distance': '1', 'weed_height': '1', 'grass_fade': '1', 'grass_detail': '1', 'library_dirs': [], 'keys': {},
                'auto_setup': True, 'dlc_dir': '', 'framegen': 'off', 'fg_hudless': True, 'fg_mult': '2',
                'dlss_mode': 'quality', 'dlss_sharpen': '0', 'dlss_ui_sharpen': '3', 'dlss_sky_sharpen': '2',
                'dlss_compare': False, 'hdr_sdr': False,
                'check_updates': True, 'skip_version': '', 'play_mode': 'custom', 'exit_backup': True, 'news_seen': '', 'expert_view': False, 'windows_title_bar': False, 'game_monitor': '', 'minimize_to_tray': False}
# sotc: improvements that changed a default, by the version that changed it. Settings saved by an
# older launcher get them once when this one first loads them (load_settings). Every release that
# changes a default for the better adds its entry here.
DEFAULT_UPGRADES = [
    # HUD and letters stop shaking with frame generation; no shadow trail in dark areas (motion
    # blur); straight to the game.
    ('0.71', {'fg_hudless': True, 'fx_motionblur': False, 'skip_logos': True}),
    # Fast GPU readbacks; the grass back as on PS4 (the play modes no longer change it).
    # Dynamic FPS in the 60 FPS mode (a steady step below 60 when the game cannot keep it).
    ('0.73', {'fps_dynamic': True}),
    ('0.72', {'rb_hot': True, 'grass_shadows': '2', 'grass_distance': '1', 'grass_height': '1',
              'weed_distance': '1', 'weed_height': '1', 'grass_fade': '1', 'grass_detail': '1'}),
]
# Keyboard bindings (src/runtime_pad.c): PS4 input, its default SDL key name, label.
KEY_INPUTS = [
    ('move_up', 'W', ('Andar para frente', 'Move forward')), ('move_down', 'S', ('Andar para trás', 'Move back')),
    ('move_left', 'A', ('Andar para a esquerda', 'Move left')), ('move_right', 'D', ('Andar para a direita', 'Move right')),
    ('look_up', 'Up', ('Câmera para cima', 'Camera up')), ('look_down', 'Down', ('Câmera para baixo', 'Camera down')),
    ('look_left', 'Left', ('Câmera para a esquerda', 'Camera left')), ('look_right', 'Right', ('Câmera para a direita', 'Camera right')),
    ('cross', 'Space', ('✕', '✕')), ('circle', 'Left Shift', ('○', '○')), ('square', 'E', ('□', '□')),
    ('triangle', 'Q', ('△', '△')), ('l1', '1', ('L1', 'L1')), ('r1', '3', ('R1', 'R1')), ('l2', 'R', ('L2', 'L2')),
    ('r2', 'F', ('R2', 'R2')), ('l3', 'Z', ('L3', 'L3')), ('r3', 'C', ('R3', 'R3')),
    ('options', 'Return', ('Options', 'Options')), ('touchpad', 'Tab', ('Touchpad (esquerda)', 'Touchpad (left)')),
    ('touchpad_right', 'Backspace', ('Touchpad (direita)', 'Touchpad (right)')),
    ('up', 'I', ('Direcional ↑', 'D-pad ↑')), ('down', 'K', ('Direcional ↓', 'D-pad ↓')),
    ('left', 'J', ('Direcional ←', 'D-pad ←')), ('right', 'L', ('Direcional →', 'D-pad →')),
]
# Tk key symbols whose SDL scancode name differs (letters, digits, F-keys and arrows match).
TK_TO_SDL = {
    'space': 'Space', 'Return': 'Return', 'Tab': 'Tab', 'BackSpace': 'Backspace', 'Escape': 'Escape',
    'Shift_L': 'Left Shift', 'Shift_R': 'Right Shift', 'Control_L': 'Left Ctrl', 'Control_R': 'Right Ctrl',
    'Alt_L': 'Left Alt', 'Alt_R': 'Right Alt', 'Caps_Lock': 'CapsLock', 'Prior': 'PageUp', 'Next': 'PageDown',
    'Home': 'Home', 'End': 'End', 'Insert': 'Insert', 'Delete': 'Delete', 'comma': ',', 'period': '.',
    'slash': '/', 'semicolon': ';', 'apostrophe': "'", 'bracketleft': '[', 'bracketright': ']',
    'minus': '-', 'equal': '=', 'grave': '`', 'backslash': '\\', 'KP_Enter': 'Keypad Enter',
    'KP_Add': 'Keypad +', 'KP_Subtract': 'Keypad -', 'KP_Multiply': 'Keypad *', 'KP_Divide': 'Keypad /',
    'KP_Decimal': 'Keypad .',
}
GPU_SUBMIT = [('auto', ('Automático (ligado em NVIDIA)', 'Automatic (on with NVIDIA)')),
              ('on', ('Ligado em qualquer placa (AMD/Intel: experimental)', 'On with any card (AMD/Intel: experimental)')),
              ('off', ('Desligado', 'Off'))]
# Video memory use (#143): (texture collection from % of the card's budget, arena release from MB; 0 = never).
VRAM_USE = [('low', ('Econômico (placas de 4–6 GB)', 'Economy (4–6 GB cards)')),
            ('normal', ('Padrão', 'Default')),
            ('high', ('Alto (placas de 8–12 GB)', 'High (8–12 GB cards)')),
            ('max', ('Máximo (placas de 16 GB ou mais)', 'Maximum (16 GB cards or more)'))]
# Video memory profile (game_environment): the 0.72 texture improvements (Maximum at mip 0 with
# banks kept 3 s and no page pool switch, High at mip 1) used 5-7 GB on an 8 GB card; cards with
# less than 8 GB get the lighter set. Auto decides from the card the launcher found (gpu_vram_mb).
VRAM_PROFILES = [('auto', ('Automático (pela placa)', 'Automatic (by the card)')),
                 ('light', ('Leve (placas de até 6 GB)', 'Light (cards up to 6 GB)')),
                 ('full', ('Completo (placas de 8 GB ou mais)', 'Full (8 GB cards or more)'))]
LIGHT_VRAM_BELOW_MB = 7500


def light_vram(s):
    """The lighter texture set: chosen, or Auto on a card with less than 8 GB."""
    profile = s.get('vram_profile') or 'auto'
    if profile != 'auto':
        return profile == 'light'
    mb = int(s.get('gpu_vram_mb') or 0)
    return 0 < mb < LIGHT_VRAM_BELOW_MB


# Far sun shadows drawn on alternate frames (SOTC_SHADOW_REUSE): more FPS, but they may blink (#178).
SHADOW_REUSE = [('0', ('Desligado (sombras sem piscar)', 'Off (no blinking shadows)')),
                ('1', ('Ligado (mais FPS; sombras distantes podem piscar)', 'On (more FPS; far shadows may blink)')),
                ('auto', ('Automático (ligado na NVIDIA, desligado na AMD/Intel)', 'Automatic (on with NVIDIA, off on AMD/Intel)'))]
# Sun shadow cascades the grass casts shadows in (r_virtualGeometryNumShadowCascades): the PS4
# value 2 ends them at 8 m, a band of grass darkens ~10 m ahead while riding; 3 reaches 24 m (4 would
# reach 100 m, but there a blade is far below a shadow-map texel: no visible gain, only cost); 1 = 2.6 m.
GRASS_SHADOWS = [('1', ('Perto (até ~3 m, mais leve)', 'Near (to ~3 m, lighter)')),
                 ('2', ('Como no PS4 (até ~8 m)', 'As on PS4 (to ~8 m)')),
                 ('3', ('Longe (até ~24 m, mais pesado)', 'Far (to ~24 m, heavier)'))]
# Grass and weed view distance: the grass shaders thin the grass out in distance bands; these
# multiply the band distances (more grass far away, blades the same height).
VEG_DISTANCE = [('1', ('Como no PS4', 'As on PS4')),
                ('2', ('2x mais longe', '2x farther')),
                ('3', ('3x mais longe', '3x farther')),
                ('5', ('5x mais longe', '5x farther'))]
# The weeds take much larger multipliers without bands (10x tested): denser fields, like early autumn.
WEED_DISTANCE = VEG_DISTANCE + [('10', ('10x mais longe (campo denso)', '10x farther (dense field)'))]
VEG_HEIGHT = [('0.75', ('Mais baixa (75%)', 'Lower (75%)')),
              ('1', ('Como no PS4', 'As on PS4')),
              ('1.5', ('Mais alta (150%)', 'Taller (150%)')),
              ('2', ('Bem mais alta (200%)', 'Much taller (200%)'))]
# Fade (r_virtualGeometryFadeMultiplier) does not change the height: away from 1 it thins the grass,
# and 0.3 also drops its shadows.
GRASS_FADE = [('0.5', ('0.5 (grama rala, menos sombra)', '0.5 (sparse grass, less shadow)')),
              ('0.75', ('0.75 (grama mais rala)', '0.75 (sparser grass)')),
              ('1', ('Como no PS4', 'As on PS4')),
              ('2', ('2 (menos grama)', '2 (less grass)')),
              ('3', ('3 (pouca grama)', '3 (little grass)'))]
# Grass detail: scales the distance at which a grass patch gets its full blade count (the shader's LOD
# scale; smaller = full grass farther out). On PS4 the grass near the camera fills in late, and its
# shadow pops in with it; 0.01 distorts.
GRASS_DETAIL = [('1', ('Como no PS4', 'As on PS4')),
                ('0.25', ('Alto', 'High')),
                ('0.15', ('Mais alto', 'Higher')),
                ('0.1', ('Muito alto', 'Very high'))]
VOLUME = [('100', ('100% (original)', '100% (original)')), ('150', ('150%', '150%')),
          ('200', ('200%', '200%')), ('300', ('300% (máximo)', '300% (maximum)'))]
VRAM_LIMITS = {'low': ('50', '1024'), 'normal': ('60', '1536'), 'high': ('75', '4096'), 'max': ('85', '0')}
# Play modes (Jogar tab): each one sets the settings below; changing one of them by hand afterwards
# makes the mode Custom. Extreme's upscaler is DLSS on an NVIDIA RTX card, FSR 1 on the others.
PLAY_MODES = [('normal', ('Normal (PS4 puro)', 'Normal (plain PS4)')),
              ('pro', ('Pro (PS4 Pro + qualidade)', 'Pro (PS4 Pro + quality)')),
              ('extreme', ('Extremo (tudo no máximo)', 'Extreme (everything maxed)')),
              ('custom', ('Personalizado', 'Custom'))]
# The play modes keep the grass as on PS4 (user's choice, 3 Nov): the grass options are left to Custom.
_PS4_GRASS = {'grass_shadows': '2', 'grass_distance': '1', 'grass_height': '1', 'weed_distance': '1',
              'weed_height': '1', 'grass_fade': '1', 'grass_detail': '1'}
PLAY_MODE_VALUES = {
    'normal': {'neo': False, 'fps_mode': '30', 'textures': 'high', 'af16': False, 'upscaler': 'fsr1',
               'sharpness': '250', 'vram_use': 'normal', **_PS4_GRASS},
    'pro': {'neo': True, 'fps_mode': '30', 'textures': 'high', 'af16': True, 'vram_use': 'high',
            **_PS4_GRASS},
    'extreme': {'neo': True, 'fps_mode': '60', 'textures': 'max', 'af16': True, 'vram_use': 'max',
                **_PS4_GRASS, 'dlss_mode': 'quality'},
}
PLAY_MODE_HINTS = {
    'normal': ('Como no PS4: 30 FPS, texturas altas, grama e sombras do console, sem o modo Pro.',
               "As on PS4: 30 FPS, high textures, the console's grass and shadows, no Pro mode."),
    'pro': ('Modo PS4 Pro a 30 FPS com texturas altas e filtro 16x; grama como no PS4.',
            'PS4 Pro mode at 30 FPS with high textures and 16x filtering; grass as on PS4.'),
    'extreme': ('Modo Pro a 60 FPS, texturas máximas, DLSS na RTX; grama como no PS4. '
                'Pede uma placa forte (8 GB ou mais).',
                'Pro mode at 60 FPS, maximum textures, DLSS on RTX; grass as on PS4. '
                'Needs a strong card (8 GB or more).'),
    'custom': ('Os ajustes abaixo e as outras abas ficam como você deixar.',
               'The settings below and the other tabs stay as you set them.'),
}


def sdl_key_name(keysym):
    """SDL scancode name of a Tk key symbol, or None for keys the game cannot use."""
    if keysym in TK_TO_SDL:
        return TK_TO_SDL[keysym]
    if len(keysym) == 1 and keysym.isalnum():
        return keysym.upper()
    if keysym in ('Up', 'Down', 'Left', 'Right') or (keysym[:1] == 'F' and keysym[1:].isdigit()):
        return keysym
    if keysym.startswith('KP_') and keysym[3:].isdigit():
        return 'Keypad ' + keysym[3:]
    return None


def write_key_bindings(keys):
    """The launcher's key bindings as key.<input>= lines of sotc.ini (BB_CONFIG), replacing its
    earlier ones; other lines stay."""
    ini = PORT_DIR / 'sotc.ini'
    try:
        lines = ini.read_text(encoding='utf-8').splitlines()
    except OSError:
        lines = []
    lines = [line for line in lines if not line.startswith('key.') and line != '# key bindings (launcher)']
    custom = dict(keys or {})  # an empty value unbinds the input
    if custom:
        lines.append('# key bindings (launcher)')
        lines += [f'key.{name}={value}' for name, value in sorted(custom.items())]
    ini.write_text('\n'.join(lines) + '\n', encoding='utf-8')
# Engine effects verified to change the image when set on the command line (A/B, 08/10/2026).
EFFECTS = [('fx_vignette', ('Vinheta (cantos escurecidos)', 'Vignette (darkened corners)'),
            'r_vignetteOverrideEnable=1 r_vignetteEnable=0'),
           ('fx_fog', ('Névoa atmosférica', 'Atmospheric fog'), 'fog_enable=0'),
           ('fx_ssao', ('Oclusão ambiente (SSAO)', 'Ambient occlusion (SSAO)'), 'AO_Mode=0'),
           ('fx_motionblur', ('Motion blur (borrão de movimento)', 'Motion blur'), 'r_motionBlurEnable=0')]
# Common screen sizes: 16:9, 16:10, 21:9 (ultrawide) and 32:9 (super ultrawide).
# 5120x2160 is left out: the game faults at start (memory), 3840x2160 runs.
SCREENS = [('1920x1080', '16:9, 1080p'), ('2560x1440', '16:9, 1440p'), ('3840x2160', '16:9, 4K'),
           ('1920x1200', '16:10'), ('2560x1600', '16:10'),
           ('2560x1080', '21:9'), ('3440x1440', '21:9'), ('3840x1600', '21:9'),
           ('3840x1080', '32:9'), ('5120x1440', '32:9')]
OUTPUTS = [('1280x720', ('1280 × 720 (16:9, 720p)', '1280 × 720 (16:9, 720p)'))] + [
    (size, (f"{size.replace('x', ' × ')} ({label})",) * 2) for size, label in SCREENS]
UPSCALERS = [('fsr1', ('FSR 1 (AMD, qualquer placa)', 'FSR 1 (AMD, any GPU)')),
             ('dlss', ('NVIDIA DLSS (experimental)', 'NVIDIA DLSS (experimental)')),
             # AMD FSR SDK 2.4 (bb-probe: SOTC_UPSCALER=fsr4, vk_fsr4.cpp): FSR 4.1 on Radeon RX
             # 6000/7000/9000, FSR 3.1 on the other cards; render sizes as DLSS's.
             ('fsr4', ('AMD FSR 4.1 (RX 6000+; FSR 3.1 nas outras, experimental)',
                       'AMD FSR 4.1 (RX 6000+; FSR 3.1 on others, experimental)')),
             ('bilinear', ('Bilinear (simples)', 'Bilinear (plain)'))]
# Button icons in the game: the PlayStation prompts, or Xbox ones drawn for the port
# (tools/make_button_icons.py, glyphs/xbox) put in place of the game's textures.
BUTTON_ICONS = [('ps', ('PlayStation (do jogo)', 'PlayStation (the game\'s)')),
                ('xbox', ('Xbox (A B X Y)', 'Xbox (A B X Y)'))]
# Frame rate: 30 (the game's cap, as on PS4), 60 (the game's own 60 FPS mode, that of the PS4 Pro
# Performance mode, at most one frame per two 120 Hz vblanks: SOTC_FPS_LIMIT=60) or unlimited.
FPS_MODES = [('30', ('30 FPS (como no PS4)', '30 FPS (as on PS4)')),
             ('60', ('60 FPS (modo 60 do jogo)', "60 FPS (the game's 60 FPS mode)")),
             ('unlimited', ('Ilimitado (acima de 60)', 'Unlimited (above 60)'))]
# Texture level (#235): off (the game's 256x256 mips), high (fine mips streamed, coarser while the
# game's page pool is low) or maximum (always fine; may close in very wide views in PS4 Pro mode).
TEXTURES = [('off', ('Desligada', 'Off')),
            ('high', ('Alta (estável)', 'High (stable)')),
            ('max', ('Máxima (nítida, experimental no modo Pro)', 'Maximum (sharp, experimental in Pro mode)'))]
# Frame generation (bb-probe: SOTC_FRAMEGEN; F8 in game switches off / DLSS-G / FSR 3).
FRAMEGEN = [('off', ('Desligada', 'Off')),
            ('auto', ('Automática (DLSS-G ou FSR 3, experimental)', 'Automatic (DLSS-G or FSR 3, experimental)')),
            ('dlss', ('NVIDIA DLSS-G (RTX 40/50)', 'NVIDIA DLSS-G (RTX 40/50)')),
            ('fsr', ('AMD FSR 3 (qualquer placa)', 'AMD FSR 3 (any GPU)'))]
# DLSS-G multi frame generation (SOTC_DLSSG_FRAMES / SOTC_DLSSG_MODE, Lucas-blip-png #314): the
# card's limit applies (RTX 40: 2x).
FG_MULT = [('2', ('2x (padrão)', '2x (default)')), ('3', ('3x (RTX 50)', '3x (RTX 50)')),
           ('4', ('4x (RTX 50)', '4x (RTX 50)')), ('6', ('6x (RTX 50)', '6x (RTX 50)')),
           ('dynamic', ('Dinâmico (se o driver suportar; senão 6x)', 'Dynamic (if the driver supports it; else 6x)'))]
# DLSS super resolution (bb-probe: SOTC_UPSCALER=dlss): the game renders at the output size
# divided by the mode's factor and DLSS picks its mode from that ratio (vk_dlssg.cpp: DLAA at 1:1,
# DLAA mode from tabnul's fork PR #112).
DLSS_MODES = [('dlaa', ('DLAA (nativo)', 'DLAA (native)')),
              ('quality', ('Qualidade (1,5×)', 'Quality (1.5×)')),
              ('balanced', ('Equilibrado (1,72×)', 'Balanced (1.72×)')),
              ('performance', ('Desempenho (2×)', 'Performance (2×)')),
              ('ultra', ('Ultra Desempenho (3×)', 'Ultra Performance (3×)'))]
DLSS_FACTORS = {'dlaa': 1.0, 'quality': 1.5, 'balanced': 1.724, 'performance': 2.0, 'ultra': 3.0}
# RCAS after DLSS (SOTC_DLSS_SHARPEN, attenuation in stops: 0 = sharpest).
DLSS_SHARPNESS = [('0', ('Alta (padrão)', 'High (default)')), ('0.5', ('Média', 'Medium')), ('1', ('Baixa', 'Low')),
                  ('off', ('Desligada', 'Off'))]
# Unsharp amounts of the upscaled text/UI and of the sky (SOTC_DLSS_UI_SHARPEN / _SKY_SHARPEN).
DLSS_UI_SHARPNESS = [('3', ('Alta (padrão)', 'High (default)')), ('1.5', ('Média', 'Medium')), ('0', ('Desligada', 'Off'))]
DLSS_SKY_SHARPNESS = [('2', ('Alta (padrão)', 'High (default)')), ('1', ('Média', 'Medium')), ('0', ('Desligada', 'Off'))]
SHARPNESS = [('0', ('Alta', 'High')), ('250', ('Média (padrão)', 'Medium (default)')), ('600', ('Baixa', 'Low')),
             ('off', ('Desligada', 'Off'))]
MOUSE_SENS = [('0.5', ('0,5 (lenta)', '0.5 (slow)')), ('0.75', ('0,75', '0.75')), ('1.0', ('1,0 (padrão)', '1.0 (default)')),
              ('1.5', ('1,5', '1.5')), ('2.0', ('2,0 (rápida)', '2.0 (fast)'))]
PRESENT_MODES = [('Mailbox', ('Mailbox (baixa latência, sem cortes)', 'Mailbox (low latency, no tearing)')),
                 ('Fifo', ('FIFO (VSync)', 'FIFO (VSync)')),
                 ('Immediate', ('Immediate (pode cortar a imagem)', 'Immediate (tearing)'))]
# System language (BB_LANGUAGE, PS4 ids); '' keeps the game's own choice.
LANGUAGES = [('', ('Automático (o do jogo)', 'Automatic (the game\'s)')),
             ('7', ('Português (Portugal)', 'Portuguese (Portugal)')), ('15', ('Português (Brasil)', 'Portuguese (Brazil)')),
             ('1', ('Inglês', 'English')), ('3', ('Espanhol', 'Spanish')), ('2', ('Francês', 'French')),
             ('4', ('Alemão', 'German')), ('5', ('Italiano', 'Italian')), ('0', ('Japonês', 'Japanese'))]
READBACKS = [('', ('Precisas (padrão; o jogo precisa)', 'Precise (default; the game needs them)')),
             ('1', ('Relaxadas (pode travar)', 'Relaxed (may hang)'))]
UI_LANGUAGES = [('', ('Automático', 'Automatic')), ('pt', ('Português', 'Português')), ('en', ('English', 'English')),
                ('es', ('Español', 'Español')), ('zh', ('简体中文', '简体中文')), ('ko', ('한국어', '한국어')),
                ('ru', ('Русский', 'Русский'))]
CONTROLS = [('W A S D', ('analógico esquerdo', 'left stick')), (('setas', 'arrows'), ('analógico direito (câmera)', 'right stick (camera)')),
            ('Espaço / Space', '✕'), ('Shift', '○'), ('E', '□'), ('Q', '△'), ('1 / 3', 'L1 / R1'), ('R / F', 'L2 / R2'),
            ('Z / C', 'L3 / R3'), ('Enter', 'Options'), ('I J K L', ('direcional', 'd-pad')),
            ('Tab / Backspace', ('touchpad (esquerda / direita)', 'touchpad (left / right)'))]


def load_json(path, default):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return default


_EBOOT_SUPPORTED = {}


def eboot_supported(eboot):
    """True when the eboot holds the code the modes patch (EU 1.01 and US 1.00 are the same
    build; dumps often keep the base game's param.sfo next to the updated eboot). Cached per file
    state."""
    try:
        stat = eboot.stat()
    except OSError:
        return False
    key = (str(eboot), stat.st_size, stat.st_mtime_ns)
    if key not in _EBOOT_SUPPORTED:
        try:
            from prepare import parse_self
            from sotc_modes import is_supported
            elf, _h, ph, _s, _m = parse_self(eboot.read_bytes())
            _EBOOT_SUPPORTED[key] = is_supported(elf, ph)
        except (OSError, ValueError, ImportError):
            _EBOOT_SUPPORTED[key] = False
    return _EBOOT_SUPPORTED[key]


def game_info(folder):
    """(title id, game version) of a game folder, or None without eboot.bin. A supported release
    shows its version (EU 01.01, US 01.00) whatever its param.sfo says."""
    folder = Path(folder or '.')
    if not (folder / 'eboot.bin').is_file():
        return None
    try:
        from prepare import sfo
        values = sfo((folder / 'sce_sys/param.sfo').read_bytes())
        title, version = values.get('TITLE_ID', '?'), values.get('APP_VER', '?')
    except (OSError, ValueError, ImportError):
        title, version = '?', '?'
    if title in GAMES and eboot_supported(folder / 'eboot.bin'):
        version = GAMES[title][1]
    return title, version


def game_supported(folder):
    """True for a supported release (EU or US) whose eboot the modes patch."""
    info = game_info(folder)
    return bool(info) and info[0] in GAMES and eboot_supported(Path(folder) / 'eboot.bin')


def game_serial(folder):
    """The title ID of the game folder, for its saves and DLC; CUSA08809 (EU) when unknown."""
    info = game_info(folder) if folder else None
    return info[0] if info and info[0] in GAMES else GAME_ID


def supported_games():
    """'CUSA08809 01.01, CUSA08034 01.00' for messages."""
    return ', '.join(f'{serial} {version}' for serial, (_region, version) in GAMES.items())


def save_param(path):
    """The title, subtitle and detail the port keeps for a save (<name>.sce_sys/param.bin, the
    PS4 SaveDataParam: 128, 128 and 1024 bytes of UTF-8), or {}."""
    try:
        data = Path(path).read_bytes()
    except OSError:
        return {}
    text = lambda a, b: data[a:b].split(b'\0')[0].decode('utf-8', 'replace').strip()
    return {'MAINTITLE': text(0, 128), 'SUBTITLE': text(128, 256), 'DETAIL': text(256, 1280)} if len(data) >= 1280 else {}


def user_dir(s):
    """The data folder (saves in savedata/1/<title id> below it: CUSA08809 EU, CUSA08034 US). A
    folder picked one level too deep (savedata itself, or a save folder) is taken back up to the
    data folder."""
    folder = Path(s.get('user_dir') or PORT_DIR / 'user')
    parts = [p.lower() for p in folder.parts]
    if 'savedata' in parts:
        folder = Path(*folder.parts[:len(parts) - 1 - parts[::-1].index('savedata')])
    return folder


# Shown in the side bar only in the expert view (the search finds them in both).
EXPERT_PAGES = ('advanced', 'debug')
NAV_ICONS = {'help': 'help', 'play': 'play', 'game': 'gameplay', 'saves': 'saves', 'graphics': 'display', 'controls': 'controls',
             'advanced': 'advanced', 'debug': 'system'}
SAVE_BACKUPS_KEPT = 10  # user/backups/saves-*.zip
LOGS_KEPT = 5      # last_run.log and last_run.1.log ... last_run.4.log
CRASH_LOGS_KEPT = 20


def open_session_log(log_dir):
    """Opens a fresh user/last_run.log; the previous sessions' logs move to last_run.1.log (the
    one before) ... last_run.4.log, so a crash is not lost when the game is started again."""
    log_dir.mkdir(parents=True, exist_ok=True)
    for i in range(LOGS_KEPT - 1, 0, -1):
        older = log_dir / ('last_run.log' if i == 1 else f'last_run.{i - 1}.log')
        if older.is_file():
            try:
                os.replace(older, log_dir / f'last_run.{i}.log')
            except OSError:
                pass
    return open(log_dir / 'last_run.log', 'w', encoding='utf-8', buffering=1)


def keep_crash_log(log_dir, code):
    """After a session that ended with an error code, copies its log to
    user/crashes/<date>_<time>_<code>.log (the last 20 are kept)."""
    if code == 0:
        return
    crashes = log_dir / 'crashes'
    try:
        crashes.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime('%Y-%m-%d_%H-%M-%S')
        shutil.copy2(log_dir / 'last_run.log', crashes / f'{stamp}_{code & 0xffffffff:#x}.log')
        for old in sorted(crashes.glob('*.log'))[:-CRASH_LOGS_KEPT]:
            old.unlink()
        # Minidumps written by bb-probe on a fatal fault (src/probe.c): the last 5.
        for old in sorted(crashes.glob('*.dmp'))[:-5]:
            old.unlink()
    except OSError:
        pass


def start_event_log(log_dir, settings, env):
    """user/events.log: one line per event of the session (src/runtime_event.h), for players and
    developers to read. The launcher writes the header (date, version, system, every setting in
    words) and the game appends its events (SOTC_EVENT_LOG). The last 5 sessions are kept."""
    log_dir.mkdir(parents=True, exist_ok=True)
    for i in range(LOGS_KEPT - 1, 0, -1):
        older = log_dir / ('events.log' if i == 1 else f'events.{i - 1}.log')
        if older.is_file():
            try:
                os.replace(older, log_dir / f'events.{i}.log')
            except OSError:
                pass
    path = log_dir / 'events.log'
    words = lambda table, key, default='': dict((k, _(*v)) for k, v in table).get(settings.get(key) or default, str(settings.get(key)))
    on_off = lambda value: _('ligado', 'on') if value else _('desligado', 'off')
    lines = [
        _('Shadow of the Colossus PC {} - sessão de {}', 'Shadow of the Colossus PC {} - session of {}').format(
            VERSION, time.strftime('%Y-%m-%d %H:%M:%S')),
        _('Windows {} | jogo: {}', 'Windows {} | game: {}').format(platform.version(), settings.get('game_dir') or '?'),
        _('Saída: {} | Upscaler: {} | Nitidez: {}', 'Output: {} | Upscaler: {} | Sharpening: {}').format(
            words(OUTPUTS, 'output_res', '1920x1080'), words(UPSCALERS, 'upscaler', 'fsr1'), words(SHARPNESS, 'sharpness', '250')),
        _('FPS: {} | Texturas: {} | Filtro 16x: {} | Modo PS4 Pro: {} | Memória da placa: {} ({})',
          'FPS: {} | Textures: {} | 16x filtering: {} | PS4 Pro mode: {} | Card memory: {} ({})').format(
            words(FPS_MODES, 'fps_mode', '30'), words(TEXTURES, 'textures', 'high'), on_off(settings.get('af16')),
            on_off(env.get('SOTC_NEO') == '1'), words(VRAM_PROFILES, 'vram_profile', 'auto'),
            _('leve', 'light') if light_vram(settings) else _('completo', 'full')),
        _('Geração de quadros: {} | Memória de vídeo: {} | Sombras distantes: {} | Volume: {}%',
          'Frame generation: {} | Video memory use: {} | Far shadows: {} | Volume: {}%').format(
            words(FRAMEGEN, 'framegen', 'off'), words(VRAM_USE, 'vram_use', 'normal'),
            on_off(str(settings.get('shadow_reuse') or '0') != '0'), settings.get('volume') or '100'),
        _('Tela cheia: {} | Câmera pelo mouse: {} | Controle preferido: {} | Variáveis extras: {}',
          'Fullscreen: {} | Mouse camera: {} | Preferred controller: {} | Extra variables: {}').format(
            on_off(settings.get('fullscreen')), on_off(settings.get('mouse', True)),
            settings.get('gamepad') or _('qualquer', 'any'), settings.get('extra_env') or _('nenhuma', 'none')),
    ]
    if env.get('SOTC_NOTE'):
        lines.append(_('Aviso: {}', 'Note: {}').format(env['SOTC_NOTE']))
    try:
        with open(path, 'w', encoding='utf-8') as out:
            for line in lines:
                out.write(f'[00:00:00.000] [CONFIG  ] {line}' + chr(10))
    except OSError:
        return ''
    return str(path)


def end_event_log(log_dir, code, stopped=False):
    """The end of the session in user/events.log; after a crash its copy goes to user/crashes."""
    path = log_dir / 'events.log'
    if code == 0 or stopped:
        text = _('Jogo fechado normalmente', 'Game closed normally') if code == 0 else _('Jogo parado pelo lançador', 'Game stopped by the launcher')
    elif code & 0xffffffff == 3:
        text = _('O jogo caiu (código 3): o motivo está na linha ERROR acima e no log da sessão',
                 'The game crashed (code 3): the reason is in the ERROR line above and in the session log')
    else:
        text = _('O jogo terminou com erro (código {:#x})', 'The game ended with an error (code {:#x})').format(code & 0xffffffff)
    try:
        with open(path, 'a', encoding='utf-8') as out:
            out.write(f'[--:--:--.---] [SESSION ] {text}' + chr(10))
        if code != 0 and not stopped:
            crashes = log_dir / 'crashes'
            crashes.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, crashes / (time.strftime('%Y-%m-%d_%H-%M-%S') + f'_{code & 0xffffffff:#x}_events.log'))
    except OSError:
        pass


def short_path(path):
    """A path for display: relative to the port folder when inside it, else as is."""
    try:
        return str(Path(path).resolve().relative_to(PORT_DIR.resolve()))
    except (ValueError, OSError):
        return str(path)


def _ps_quote(value):
    """A PowerShell single-quoted string literal (no $ expansion; every quote PowerShell accepts doubled)."""
    return "'" + ''.join(c * 2 if c in "'\u2018\u2019\u201a\u201b" else c for c in str(value)) + "'"


def _shortcut_script(play, frozen, executable, port_dir, game_icon):
    """PowerShell that creates the desktop shortcut: the launcher window, or with play the game directly
    (--play, also usable as a non-Steam game in Steam)."""
    if frozen:
        target, arguments, icon = executable, '--play' if play else '', game_icon or f'{executable},0'
    else:
        pythonw = Path(executable).with_name('pythonw.exe')
        target = pythonw if pythonw.exists() else executable
        arguments = f'"{Path(port_dir) / "launcher" / "sotc_launcher_win.py"}"' + (' --play' if play else '')
        icon = Path(port_dir) / 'launcher' / 'sotc.ico'
    name = '\\Shadow of the Colossus (play).lnk' if play else '\\Shadow of the Colossus.lnk'
    return ("$s=(New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath('Desktop')"
            f"+{_ps_quote(name)});"
            f'$s.TargetPath={_ps_quote(target)};$s.WorkingDirectory={_ps_quote(port_dir)};'
            f'$s.IconLocation={_ps_quote(icon)};'
            + (f'$s.Arguments={_ps_quote(arguments)};' if arguments else '') + '$s.Save()')


def clear_shader_cache_after_update(settings):
    """sotc: 0.72.2: a port update clears the shader cache once. Shaders and pipelines built by the
    previous port can conflict with the new one (wrong or missing effects, crashes when one is
    loaded); the game builds them again, with some stutter at the start. The port version that
    built the cache is kept next to it (user/cache/port_version.txt; the --play path never saves
    the settings, so they cannot hold it)."""
    data = user_dir(settings)
    marker = data / 'cache' / 'port_version.txt'
    try:
        built_by = marker.read_text(encoding='utf-8').strip()
    except OSError:
        built_by = ''
    if built_by == VERSION:
        return
    # A first start (no cache yet) has nothing to clear; an older cache without the marker was
    # built by a port before 0.72.2.
    if (data / 'cache').exists() or (data / 'shader').exists():
        for name in ('cache', 'shader'):
            shutil.rmtree(data / name, ignore_errors=True)
    try:
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.write_text(VERSION + '\n', encoding='utf-8')
    except OSError:
        pass


def load_settings():
    """Saved settings over the defaults. Older settings that turned DLSS-G on with SOTC_DLSSG=1 in
    the extra variables get the frame generation setting instead."""
    saved = load_json(CONFIG_FILE, {})
    if not saved and LEGACY_CONFIG_FILE.is_file():
        # First start of this copy: the shared settings of older versions, but its own data
        # folder when the old one is another copy's user folder.
        saved = load_json(LEGACY_CONFIG_FILE, {})
        old = Path(str(saved.get('user_dir') or ''))
        if str(old) not in ('', '.') and old.name.lower() == 'user' and                 (old.parent / 'SotC.exe').is_file() and old.parent.resolve() != PORT_DIR.resolve():
            saved['user_dir'] = ''
            # Its saves and the user's sound modules come along (copied; the old copy keeps them).
            for name in ('savedata', 'sys_modules'):
                source, target = old / name, PORT_DIR / 'user' / name
                if source.is_dir() and not target.exists():
                    try:
                        shutil.copytree(source, target)
                    except OSError:
                        pass
    settings = {**APP_DEFAULTS, **saved}
    if 'fps_mode' not in saved:  # before the choice: the "uncapped FPS" box
        settings['fps_mode'] = '60' if saved.get('fps60') else '30'
    if 'textures' not in saved:  # before the levels: the "high quality textures" box
        settings['textures'] = 'high' if saved.get('hq_textures', True) else 'off'
    # Rule: an update turns its improvements on in settings saved by an older version, once
    # (DEFAULT_UPGRADES). A saved value would otherwise keep the old default for good.
    if saved:
        before = sotc_update.version_key(str(saved.get('settings_version') or '0'))
        for version, values in DEFAULT_UPGRADES:
            if before < sotc_update.version_key(version):
                settings.update(values)
    settings['settings_version'] = VERSION
    clear_shader_cache_after_update(settings)
    if 'framegen' not in saved:
        extra = str(settings.get('extra_env', '')).split()
        if 'SOTC_DLSSG=1' in extra:
            settings['framegen'] = 'auto'
            settings['extra_env'] = ' '.join(item for item in extra if item != 'SOTC_DLSSG=1')
    return settings


def window_icon_file(s):
    """The game window's icon as a 64x64 BMP in the data folder (bb-probe: SOTC_WINDOW_ICON):
    empty setting = the port's own (launcher/window_icon.png, original art: the swordsman), 'game'
    = icon0.png of the player's dump, else a picture the player chose (cropped to a centred
    square). Nothing of the game or of anyone's art is packaged."""
    try:
        from PIL import Image
        choice = s.get('window_icon_file') or ''
        custom = Path(choice) if choice not in ('', 'game') else Path('')
        if choice == 'game':
            source, target = Path(s['game_dir'] or '.') / 'sce_sys' / 'icon0.png', user_dir(s) / 'game_icon.bmp'
        elif choice and custom.is_file():
            source, target = custom, user_dir(s) / 'window_icon_custom.bmp'
        else:
            source, target = PORT_DIR / 'launcher' / 'window_icon.png', user_dir(s) / 'window_icon_port.bmp'
        if (not target.is_file() or target.stat().st_mtime < source.stat().st_mtime
                or source == custom):
            with Image.open(source) as image:
                image = image.convert('RGBA')
                side = min(image.size)
                left, top = (image.width - side) // 2, (image.height - side) // 2
                if source == custom:
                    image = image.crop((left, top, left + side, top + side))
                target.parent.mkdir(parents=True, exist_ok=True)
                image.resize((64, 64), Image.LANCZOS).save(target)
        return target
    except (ImportError, OSError, ValueError):
        return None


def monitors():
    """This PC's monitors: [(device name, label, (x, y) of its centre, primary)], the primary first."""
    from ctypes import wintypes

    class MONITORINFOEXW(ctypes.Structure):
        _fields_ = [('size', wintypes.DWORD), ('monitor', wintypes.RECT), ('work', wintypes.RECT),
                    ('flags', wintypes.DWORD), ('device', wintypes.WCHAR * 32)]
    found = []
    PROC = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(wintypes.RECT), ctypes.c_void_p)

    def one(handle, _dc, _rect, _data):
        info = MONITORINFOEXW()
        info.size = ctypes.sizeof(info)
        if ctypes.windll.user32.GetMonitorInfoW(ctypes.c_void_p(handle), ctypes.byref(info)):
            r = info.monitor
            found.append((info.device, r.right - r.left, r.bottom - r.top, ((r.left + r.right) // 2, (r.top + r.bottom) // 2),
                          bool(info.flags & 1)))
        return 1
    try:
        ctypes.windll.user32.EnumDisplayMonitors(None, None, PROC(one), None)
    except (AttributeError, OSError):
        return []
    found.sort(key=lambda m: (not m[4], m[0]))
    return [(device, '{} — {} × {}{}'.format(device.replace('\\\\.\\', '').replace('DISPLAY', _('Monitor ', 'Monitor ')),
                                          w, h, _(' (principal)', ' (primary)') if primary else ''), centre, primary)
            for device, w, h, centre, primary in found]


def screen_refresh_hz():
    """The primary screen's refresh rate (60 when unknown)."""
    class DEVMODEW(ctypes.Structure):
        _fields_ = [('name', ctypes.c_wchar * 32), ('spec', ctypes.c_ushort), ('driver', ctypes.c_ushort),
                    ('size', ctypes.c_ushort), ('extra', ctypes.c_ushort), ('fields', ctypes.c_ulong),
                    ('pos', ctypes.c_byte * 16), ('color', ctypes.c_short * 5), ('form', ctypes.c_wchar * 32),
                    ('logpixels', ctypes.c_ushort), ('bpp', ctypes.c_ulong), ('width', ctypes.c_ulong),
                    ('height', ctypes.c_ulong), ('flags', ctypes.c_ulong), ('hz', ctypes.c_ulong)]
    try:
        mode = DEVMODEW()
        mode.size = ctypes.sizeof(DEVMODEW)
        if ctypes.windll.user32.EnumDisplaySettingsW(None, -1, ctypes.byref(mode)) and mode.hz > 1:
            return int(mode.hz)
    except (AttributeError, OSError):
        pass
    return 60


def game_environment(s):
    env = dict(os.environ)
    info = game_info(s['game_dir'])
    if info and info[1] != '?':
        env['SOTC_GAME_VERSION'] = info[1]  # window title (with the frame rate)
    env['SOTC_PORT_VERSION'] = VERSION  # sotc: window title
    icon = window_icon_file(s)
    if icon:
        env['SOTC_WINDOW_ICON'] = str(icon)
    env['SOTC_GAME_DIR'] = s['game_dir']
    env['SOTC_USER_DIR'] = str(user_dir(s))
    if s['sys_modules']:
        env['SOTC_SYS_MODULES'] = s['sys_modules']
    if s.get('dlc_dir'):
        env['SOTC_DLC_DIR'] = s['dlc_dir']
    # The 30 FPS cap is applied or not by bb-probe (sotc_fps_cap.h), F7 switches it in game: no
    # fps60 code patch, vblanks at 120 Hz in every mode; 60 FPS limits the uncapped game. Above 30
    # the game also targets 60 FPS itself (fps60game), so its timing follows: ignoring its 30 FPS
    # target ran some physics and timers twice as fast (AjncSystem #100, #120, #49).
    fps_mode = s.get('fps_mode') or '30'
    modes = ['af16'] if s.get('af16') else []
    if fps_mode != '30':
        modes.append('fps60game')
    # At this resolution the grass overflows the game's grass buffers: the farthest grass then pops
    # in and out in patches, in a band ahead with longer grass distances. grassbuf makes them 4x larger.
    modes.append('grassbuf')
    if s.get('hdr'):
        modes.append('hdr')
    env['SOTC_MODES'] = ','.join(modes)
    env['SOTC_FPS_CAP'] = '30' if fps_mode == '30' else 'off'
    env['SOTC_FPS_LIMIT'] = '60' if fps_mode == '60' else '0'
    # sotc: 0.73 dynamic FPS (videoout driver.cpp, SOTC_FPS_DYNAMIC): with the 60 FPS limit, frames
    # that cannot keep 60 lower it to a step that divides the screen's refresh (48 at 144 Hz, 40 at
    # 120/240 Hz, 30 at 60 Hz), steady instead of swinging; it tries 60 again every 10 s.
    if fps_mode == '60' and s.get('fps_dynamic', True):
        hz = screen_refresh_hz()
        step = 48 if hz % 48 == 0 else 40 if hz % 40 == 0 else 45 if hz % 45 == 0 else 30
        env.setdefault('SOTC_FPS_DYNAMIC', str(step))
    env['BB_VBLANK_HZ'] = '120'
    env['BB_FULLSCREEN'] = '1' if s['fullscreen'] else '0'
    # The monitor the game window opens on (window.cpp): the centre of the chosen one, found again now
    # (the monitors may have moved since it was chosen).
    chosen = next((m for m in monitors() if m[0] == s.get('game_monitor')), None)
    if chosen and not chosen[3]:
        env['SOTC_MONITOR_AT'] = f'{chosen[2][0]},{chosen[2][1]}'
    env['BB_PRESENT_MODE'] = s['present_mode']
    if s['language']:
        env['BB_LANGUAGE'] = s['language']
    if str(s['player_name']).strip():
        env['BB_USER_NAME'] = str(s['player_name']).strip()
    if s['show_fps']:
        env['SOTC_FPS'] = '1'
    if s['readbacks']:
        env['BB_READBACKS'] = s['readbacks']
    cvars = [off for key, _t, off in EFFECTS if not s.get(key, True)]
    if s.get('perf_culling', True):
        # From Pink-shadPS4: no GPU index-buffer culling (always both), same image, far fewer
        # GPU results the CPU has to wait for.
        cvars += ['doIndexBufferCulling=0', 'doSunShadowIndexBufferCulling=0']
    grass = str(s.get('grass_shadows') or '2')
    if grass in ('3', '4'):
        # Every mode's copy: the game takes the one of its graphics mode (PS4 Pro Cinematic has 3).
        cvars += [f'r_virtualGeometryNumShadowCascades{mode}={grass}'
                  for mode in ('', '_BasePS4', '_Perf', '_Cinematic')]
    fade = str(s.get('grass_fade') or '1')
    if fade in ('0.5', '0.75', '2', '3'):
        cvars.append(f'r_virtualGeometryFadeMultiplier={fade}')
    if s.get('skip_logos'):
        # The engine cvar skipSplash=1: no opening logos and autosave notice (tabnul, AjncSystem #185).
        cvars.append('skipSplash=1')
    if s.get('skip_film'):
        # +nointro: the engine's developer switch (it also sets it when a button is held at start):
        # no intro film and no title screen, the game goes straight in (user, 26 Oct).
        cvars.append('nointro')
    cvars += str(s.get('engine_cvars', '')).split()
    # Tools (tools/sotc_route.py --boss) add cvars for one run without editing the settings.
    cvars += os.environ.get('SOTC_EXTRA_CVARS', '').split()
    env['SOTC_CVARS'] = ' '.join(cvars)
    # Multipliers for the grass shaders' view distances and blade height (vk_rasterizer.cpp).
    env['SOTC_GRASS'] = ' '.join(str(s.get(k) or '1') for k in
                                 ('grass_distance', 'grass_height', 'weed_distance', 'weed_height', 'grass_detail'))
    env['SOTC_RECORD_THREAD'] = '1' if s.get('perf_record_thread', True) else '0'
    if not s.get('perf_dma', True):
        env['BB_DIRECT_MEMORY_ACCESS'] = '0'
    # Periodic submits and readbacks ahead: by default only with NVIDIA (vk_rasterizer.cpp).
    if s.get('gpu_submit') == 'on':
        env['SOTC_FLUSH_US'], env['SOTC_RB_AHEAD'] = '1000', '1'
    elif s.get('gpu_submit') == 'off':
        env['SOTC_FLUSH_EVERY'], env['SOTC_RB_AHEAD'] = '0', '0'
    light = light_vram(s)
    vram_use = s.get('vram_use')
    if light and vram_use in ('high', 'max'):
        vram_use = 'normal'  # sotc: "High"/"Maximum" (Extreme mode) hold more than such a card has
    env['SOTC_VRAM_GC_PERCENT'], env['SOTC_ARENA_EVICT_MB'] = VRAM_LIMITS.get(vram_use, VRAM_LIMITS['normal'])
    if light:
        env.setdefault('SOTC_STAGING_KEEP_VRAM_MB', '0')
    env['SOTC_VOLUME'] = str(s.get('volume') or '100')
    # The in-game menu's quick settings are written back here (bbport_overlay.cpp).
    env['SOTC_LAUNCHER_SETTINGS'] = str(CONFIG_FILE)
    env['SOTC_SHADOW_REUSE'] = str(s.get('shadow_reuse') or '0')
    env['SOTC_OUTPUT_RES'] = s.get('output_res') or '1920x1080'
    # The PS4 Pro path keeps its 16:9 sizes: a wider screen (21:9, 32:9) wins over it (#142).
    env['SOTC_NEO'] = '1' if s.get('neo') and not is_wide(env['SOTC_OUTPUT_RES']) else '0'
    if s.get('neo') and env['SOTC_NEO'] == '0':
        env['SOTC_NOTE'] = f"{env['SOTC_OUTPUT_RES']} is wider than 16:9: PS4 Pro mode off (it renders 16:9 only)"
    env['SOTC_FRAMEGEN'] = s.get('framegen') or 'off'
    if s.get('fg_hudless'):  # Lucas-blip-png's option (#206, #210)
        env['SOTC_FG_HUDLESS'] = '1'
    fg_mult = str(s.get('fg_mult') or '2')  # DLSS-G multiplier (#314)
    if fg_mult == 'dynamic':
        env['SOTC_DLSSG_MODE'] = 'dynamic'
        env['SOTC_DLSSG_FRAMES'] = '5'  # the most the card allows; Streamline picks the number
    elif fg_mult in ('3', '4', '6'):
        env['SOTC_DLSSG_FRAMES'] = str(int(fg_mult) - 1)
    # sotc: high quality textures: the texture banks sampled since the last report read as sampled
    # (sotc_lod_stats.h; before, none ever did and the game never streamed its fine mips: blurry
    # leaves and rocks). Off: the old behaviour, for cards short of video memory.
    textures = s.get('textures') or 'high'
    if textures != 'off':
        env.setdefault('SOTC_LOD_LINGER', '1')
        # sotc: what the page pool's double free (fixed in 0.71, SOTC_POOL_OWNER) had held back,
        # tested on 3 Nov (Time Attack, every colossus, Maximum + PS4 Pro, no crash nor mosaic):
        # the measured mips, finer mips (High 1, Maximum 0, both were 2) and, on Maximum, banks
        # kept sampled for 3 s (they had exhausted the pool, 0x1a371).
        env.setdefault('SOTC_LOD_FEEDBACK', '1')
        # sotc: 0.72.2 had put both qualities back at mip 2: the finer mips with Maximum's 3 s
        # linger and no page pool switch copied up to 1.1 GB of textures in a frame at the forest
        # entrance (5 Nov). That left Maximum (the Extreme mode) blurry. Now the finer mips are
        # back (High 1, Maximum 0, light profile Maximum 1 / High 2), without the 3 s linger and
        # with the pool switch: the copies go through the GPU (SOTC_TEX_DIRECT) and the area
        # change fixes since then (T48). SOTC_LOD_LEVEL / _LINGER / _POOL_LOW still override.
        if light:
            env.setdefault('SOTC_LOD_LEVEL', '1' if textures == 'max' else '2')
        else:
            env.setdefault('SOTC_LOD_LEVEL', '0' if textures == 'max' else '1')
    # sotc: the heap guard's two shortcuts (SOTC_GUARD_STALE, SOTC_GUARD_USER_POINTER) stay off
    # again (0.73): on since 0.72, they spoilt the game's heap at the ending (objects found cleared
    # at 0x144c60 / 0x14cce7 / 0x564d24, the allocator at 0x499cc, 9 Nov), as T49 had suspected for
    # 0x49800; with them off the ending and the intro skip went through with nothing to repair.
    # sotc: 0.72.2 (T51): at the end of the game the game returns page pool pages under textures
    # it goes on drawing; a refresh then uploaded another texture's bytes into them (stretched
    # textures, flicker, the close between the ending and the intro). Two clean ending runs.
    env.setdefault('SOTC_POOL_FREEZE', '1')
    # sotc: 0.72.2 (T48): an unmap's arena release waited for all the GPU work, 28-63 times a
    # frame at area changes (10-20 ms a frame); now once the GPU passed the unmap.
    env.setdefault('SOTC_RELEASE_DEFER', '1')
    # sotc: 0.72.2: images of 2048x2048 and more get memory of their own: the shared 64 MiB
    # blocks kept up to 1.8 GB free after an area's textures went (over the budget on 8 GB cards).
    env.setdefault('SOTC_IMAGE_DEDICATED', '1')
    # sotc: 0.72.2 (T48): new textures read by the GPU in the game's memory (no CPU copy at the
    # first entry of an area; forest 14 -> 9-10 slow frames). The game's 10 GB are imported at
    # boot (~2.6 s) and stay pinned in RAM: only with 24 GB or more.
    if (sotc_hardware.memory_gb() or 0) >= 24:
        env.setdefault('SOTC_PC_MEMORY', '1')
        env.setdefault('SOTC_PC_MEMORY_PREIMPORT', '1')
        env.setdefault('SOTC_TEX_DIRECT', '1')
    # sotc: "Maximum" in PS4 Pro mode exhausted the game's page pool within minutes at the Shrine
    # (0x1a371, 22 Oct) until tabnul's page allocator fix (PR #288, sotc_modes.py POOL_FIX): an
    # hour without a crash with it (26 Oct). Experimental there; SOTC_MAX_IN_PRO=0 brings back High.
    if 'SOTC_MAX_IN_PRO=0' in str(s.get('extra_env', '')) and textures == 'max' and env['SOTC_NEO'] == '1':
        textures = 'high'
        env['SOTC_NOTE'] = 'Texture quality Maximum is not used in PS4 Pro mode: High'
    if s.get('button_icons') == 'xbox' and (PORT_DIR / 'glyphs' / 'xbox').is_dir():
        env['SOTC_TEX_REPLACE'] = str(PORT_DIR / 'glyphs' / 'xbox')
    env['SOTC_UI_LANG'] = s.get('ui_language') or windows_language()  # overlay and F7/F8 texts
    # The game renders at the output size. 720p renders at 1080p, as on PS4; the PS4 Pro path keeps
    # its own sizes.
    output = env['SOTC_OUTPUT_RES']
    upscaler = s.get('upscaler', 'fsr1')
    if upscaler in ('dlss', 'fsr4') and env['SOTC_NEO'] == '0':
        env['SOTC_RENDER_RES'] = dlss_render(output, s.get('dlss_mode'))
    elif output not in ('1280x720', '1920x1080') and env['SOTC_NEO'] == '0':
        env['SOTC_RENDER_RES'] = output
    # DLSS: FSR 1 stays the upscaler of frames without the game's scene (menus) and the fallback
    # without an RTX card or the NVIDIA DLLs.
    env['BB_FSR1'] = '1' if upscaler in ('fsr1', 'dlss', 'fsr4') else '0'
    if upscaler in ('dlss', 'fsr4'):
        env['SOTC_UPSCALER'] = upscaler
        env['SOTC_DLSS_SHARPEN'] = str(s.get('dlss_sharpen') or '0')
        env['SOTC_DLSS_UI_SHARPEN'] = str(s.get('dlss_ui_sharpen') or '3')
        env['SOTC_DLSS_SKY_SHARPEN'] = str(s.get('dlss_sky_sharpen') or '2')
        if s.get('dlss_compare'):
            env['SOTC_DLSS_COMPARE'] = '1'
    # Upstream's SDR-to-HDR10 output (post_process.frag hdr 2); 'hdr' is the game's own HDR rendering.
    if s.get('hdr_sdr'):
        env['SOTC_HDR'] = '1'
    if s.get('sharpness') == 'off':
        env['BB_RCAS'] = '0'
    else:
        env['BB_RCAS'] = '1'
        env['BB_RCAS_ATTENUATION'] = str(s.get('sharpness') or '250')
    env['SOTC_MOUSE'] = '1' if s.get('mouse', True) else '0'
    env['SOTC_MOUSE_SENS'] = str(s.get('mouse_sens') or '1.0')
    env['SOTC_MOUSE_INVERT'] = '1' if s.get('mouse_invert') else '0'
    # Hot readbacks (buffer_cache.cpp GatherDownloads): ~25% more FPS at the Shrine. On by default
    # (in upstream too since 0.72): the late check and per-page region locks keep them off reused memory.
    env['SOTC_RB_HOT'] = '1' if s.get('rb_hot', True) else '0'
    # Bluepoint's debug menu (+RetailBuild=0 served by the port; the game files are not touched).
    if s.get('debug_menu'):
        env['SOTC_DEBUG_MENU'] = '1'
    # The texture memory watch (events.log lines "Vigia: ..."), for bug reports.
    if s.get('debug_watch'):
        env['SOTC_POOL_WATCH'] = '1'
    if str(s.get('gamepad', '')).strip():
        env['BB_GAMEPAD'] = str(s['gamepad']).strip()
    if not s.get('rumble', True):
        env['SOTC_RUMBLE'] = '0'
    env['BB_CONFIG'] = str(PORT_DIR / 'sotc.ini')
    for item in str(s['extra_env']).split():
        item = item.strip().lstrip('=')  # "=KEY=1" typed by mistake still sets KEY
        if '=' in item:
            key, value = item.split('=', 1)
            if key:
                env[key] = value
    env['PYTHONUNBUFFERED'] = '1'
    env['PYTHONIOENCODING'] = 'utf-8'
    return env


def dlss_render(output, mode):
    """The render size for DLSS mode at output WxH: multiples of 8, at least 640x360 (DLAA: the
    output size). The exact aspect when a size within 6% of the factor has it: the
    presenter keeps the render's aspect, so 2552x1064 for 3840x1600 gave a 3838 wide output."""
    try:
        w, h = map(int, output.split('x'))
    except ValueError:
        return output
    if mode == 'dlaa':
        return output
    target = h / DLSS_FACTORS.get(mode, 1.5)
    exact = [rh for rh in range(360, h + 1, 8) if rh * w % (h * 8) == 0 and abs(rh - target) <= target * 0.06]
    rh = min(exact, key=lambda rh: abs(rh - target)) if exact else max(360, round(target / 8) * 8)
    rw = max(640, round(rh * w / h / 8) * 8)
    return f'{rw}x{rh}'


def is_wide(size):
    """True for a WxH wider than 16:9 (by more than 2%)."""
    try:
        w, h = map(int, str(size).split('x'))
    except ValueError:
        return False
    return w * 9 > h * 16 * 1.02


def shadps4_saves(serial):
    """Save folders of the game serial in shadPS4 installations next to the port or in the usual
    places."""
    roots = [PORT_DIR.parent, Path.home(), Path(os.environ.get('APPDATA', Path.home()))]
    found = []
    for root in roots:
        for pattern in ('*/user/home/*/savedata/' + serial, '*/*/user/home/*/savedata/' + serial,
                        'shadPS4/savedata/*/' + serial):
            try:
                found += [p for p in root.glob(pattern) if p.is_dir()]
            except OSError:
                pass
    return found


# ---------------------------------------------------------------------------------------------
# The window.

# Theme of the Forbidden Land: night stone, sunlight gold for titles, the sword's light (sigil blue)
# for what is active.
BG, PANEL, CARD, LINE = theme.DEEP, theme.BG, theme.CARD, theme.BORDER
TEXT, MUTED, GOLD, ACCENT, ACCENT_HI = theme.TEXT, theme.MUTED, theme.GOLD_LIGHT, theme.SIGIL, theme.SIGIL_HOVER
SIGIL = theme.SIGIL_LIGHT


class Launcher(sotc_search.SearchMixin, sotc_pad.PadMixin):
    def __init__(self, root, tk, ttk, filedialog, messagebox):
        self.tk, self.ttk, self.filedialog, self.messagebox = tk, ttk, filedialog, messagebox
        self.root = root
        self.app = load_settings()
        self.vars = {}
        self.process = self.job = None
        self.stopping = False
        self.output = queue.Queue()
        self.ui_calls = queue.Queue()
        self.gpu_text = _('• Verificando a placa de vídeo…', '• Checking the graphics card…')
        self.banner_source = self.banner_image = None
        root.title('Shadow of the Colossus — ' + _('port nativo', 'native port'))
        root.configure(bg=BG)
        self.dpi = root.winfo_fpixels('1i') / 96.0
        # Tall enough for every quick setting when the screen has the room (the Play page scrolls otherwise).
        height = min(self.px(820), root.winfo_screenheight() - self.px(80))
        root.geometry(f'{self.px(1080)}x{max(self.px(620), height)}')
        root.minsize(self.px(940), self.px(620))
        self.set_icon()
        theme.load_fonts(root)
        self.style()
        self.build()
        self.show('play')
        self.refresh_library()
        # First start, or the game folder is gone: the setup screen asks for it.
        if not self.app.get('setup_done') or not game_info(self.var('game_dir').get()):
            root.after(50, self.show_setup)
        root.protocol('WM_DELETE_WINDOW', self.close)
        if self.app.get('setup_done') and self.app.get('news_seen') != VERSION:
            root.after(600, self.show_news)
        root.after(100, self.drain_output)
        threading.Thread(target=self.detect_gpu, daemon=True).start()
        # sotc: self-update (sotc_update.py): the result of an update that just ran, then a check.
        self.updating = False
        if FROZEN:
            error = sotc_update.cleanup(PORT_DIR)
            if error:
                root.after(500, lambda: messagebox.showerror('Shadow of the Colossus', _(
                    'A atualização falhou e a versão anterior foi mantida:\n{}',
                    'The update failed and the previous version was kept:\n{}').format(error)))
        if self.app.get('check_updates', True) and (FROZEN or os.environ.get('SOTC_UPDATE_URL')):
            root.after(2000, lambda: self.check_updates(False))

    def px(self, size):
        return int(size * self.dpi)

    def set_icon(self):
        """The game's own icon (sce_sys/icon0.png of the user's dump) when there is one, else the
        port's: the package carries no game art."""
        game = self.vars['game_dir'].get() if 'game_dir' in self.vars else self.app['game_dir']
        try:
            from PIL import Image, ImageTk
            with Image.open(Path(game) / 'sce_sys' / 'icon0.png') as source:
                self.icon = ImageTk.PhotoImage(source.convert('RGBA').resize((64, 64), Image.LANCZOS))
            self.root.iconphoto(True, self.icon)
            return
        except (ImportError, OSError):
            pass
        try:
            self.root.iconbitmap(default=str(PORT_DIR / 'launcher' / 'sotc.ico'))
        except self.tk.TclError:
            pass

    def game_icon_file(self):
        """icon0.png of the dump as an .ico in the data folder (for the desktop shortcut)."""
        try:
            from PIL import Image
            target = user_dir(self.app) / 'game.ico'
            with Image.open(Path(self.app['game_dir']) / 'sce_sys' / 'icon0.png') as source:
                target.parent.mkdir(parents=True, exist_ok=True)
                source.convert('RGBA').save(target, sizes=[(16, 16), (32, 32), (48, 48), (64, 64), (128, 128),
                                                           (256, 256)])
            return target
        except (ImportError, OSError):
            return None

    # ---- look --------------------------------------------------------------------------------
    def style(self):
        """The theme's widgets (sotc_theme: rounded buttons and fields, switches, the Play button),
        then the launcher's own labels."""
        ttk = self.ttk
        s = ttk.Style(self.root)
        self.theme = theme.Theme(self.root, self.dpi)
        self.theme.style(s)
        s.configure('TLabel', background=PANEL, foreground=TEXT, font=(ui_font(), 10))
        s.configure('Muted.TLabel', background=PANEL, foreground=MUTED, font=(ui_font(), 9))
        s.configure('Section.TLabel', background=PANEL, foreground=GOLD, font=(title_font(), 13))
        s.configure('Warning.TLabel', background='#1e1a12', foreground='#e0b866', padding=(10, 6))

    # ---- widget helpers ----------------------------------------------------------------------
    def var(self, key):
        tk = self.tk
        if key not in self.vars:
            value = self.app.get(key, APP_DEFAULTS[key])
            self.vars[key] = tk.BooleanVar(value=bool(value)) if isinstance(APP_DEFAULTS[key], bool) \
                else tk.StringVar(value=str(value))
        return self.vars[key]

    def choice(self, parent, key, options, width=38):
        var = self.var(key)
        values = [v for v, _t in options]
        box = self.ttk.Combobox(parent, values=[_(*text) for _v, text in options], state='readonly', width=width)
        if var.get() not in values:
            var.set(values[0])

        def show(*_args):
            current = var.get()
            box.current(values.index(current) if current in values else 0)
        show()
        box.bind('<<ComboboxSelected>>', lambda _e: var.set(values[box.current()]))
        var.trace_add('write', show)
        box.sotc_key = key  # the search's words for the setting (row)
        return box

    def next_row(self, parent):
        return parent.grid_size()[1]

    def row(self, parent, title, widget, hint=None):
        ttk = self.ttk
        r = self.next_row(parent)
        label = ttk.Label(parent, text=title)
        label.grid(row=r, column=0, sticky='nw', padx=(0, 18), pady=(8, 0))
        widget.grid(row=r, column=1, sticky='w', pady=(5, 0))
        self.index_entry(label, title, hint or '', getattr(widget, 'sotc_key', None))
        if hint:
            ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(560), justify='left').grid(
                row=r + 1, column=1, sticky='w', pady=(2, 2))
        return widget

    def check(self, parent, key, title, hint=None):
        ttk = self.ttk
        r = self.next_row(parent)
        box = ttk.Checkbutton(parent, text=title, variable=self.var(key))
        box.grid(row=r, column=0, columnspan=2, sticky='w', pady=(4, 0))
        self.index_entry(box, title, hint or '', key)
        if hint:
            ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(640), justify='left').grid(
                row=r + 1, column=0, columnspan=2, sticky='w', padx=(26, 0))

    def note(self, parent, text, top=12):
        self.ttk.Label(parent, text=text, style='Muted.TLabel', wraplength=self.px(680), justify='left').grid(
            row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 0))

    def section(self, parent, title, top=18):
        r = self.next_row(parent)
        label = self.ttk.Label(parent, text=title, style='Section.TLabel')
        label.grid(row=r, column=0, columnspan=2, sticky='w', pady=(top, 2))
        self.index_entry(label, title)
        self.tk.Frame(parent, bg=LINE, height=1).grid(row=r + 1, column=0, columnspan=2, sticky='we', pady=(0, 4))

    def window_icon_row(self, parent):
        """The game window's icon: the game's (empty) or a picture of the player's choice."""
        ttk = self.ttk
        var = self.var('window_icon_file')
        holder = ttk.Frame(parent)
        ttk.Entry(holder, textvariable=var, width=40).pack(side='left')

        def browse():
            chosen = self.filedialog.askopenfilename(
                title=_('Imagem para o ícone da janela do jogo', "Picture for the game window's icon"),
                filetypes=[(_('Imagens', 'Pictures'), '*.png *.jpg *.jpeg *.bmp *.webp'), ('*', '*.*')])
            if chosen:
                var.set(str(Path(chosen)))
        ttk.Button(holder, text=_('Escolher imagem…', 'Choose picture…'), command=browse).pack(side='left', padx=(6, 0))
        ttk.Button(holder, text=_('Usar o do jogo', "Use the game's"), command=lambda: var.set('game')).pack(side='left', padx=(6, 0))
        ttk.Button(holder, text=_('Padrão', 'Default'), command=lambda: var.set('')).pack(side='left', padx=(6, 0))
        return self.row(parent, _('Ícone da janela do jogo', "Game window's icon"), holder,
                        _('Padrão: o desenho do port (o menino erguendo a espada). "game": o ícone do próprio jogo. '
                          'Ou escolha uma imagem sua (PNG/JPG): ela é recortada em quadrado no centro. Vale na '
                          'próxima vez que o jogo abrir.',
                          'Default: the port\'s drawing (the boy raising his sword). "game": the game\'s own icon. '
                          'Or choose a picture of yours (PNG/JPG): it is cropped to a centred square. Applies the '
                          'next time the game starts.'))

    def folder(self, parent, key, title, prompt, hint=None, on_change=None):
        ttk = self.ttk
        var = self.var(key)
        holder = ttk.Frame(parent)
        ttk.Entry(holder, textvariable=var, width=54).pack(side='left')

        def browse():
            chosen = self.filedialog.askdirectory(title=prompt, initialdir=var.get() or str(PORT_DIR))
            if chosen:
                var.set(str(Path(chosen)))
        ttk.Button(holder, text=_('Procurar…', 'Browse…'), command=browse).pack(side='left', padx=(6, 0))
        ttk.Button(holder, text=_('Abrir', 'Open'), command=lambda: self.open_path(var.get() or (
            user_dir(self.app) if key == 'user_dir' else PORT_DIR))).pack(side='left', padx=(6, 0))
        if on_change:
            var.trace_add('write', lambda *_a: on_change())
        holder.sotc_key = key
        return self.row(parent, title, holder, hint)

    def scrolled_page(self, name, title, subtitle):
        tk, ttk = self.tk, self.ttk
        outer = ttk.Frame(self.content)
        head = ttk.Frame(outer, padding=(28, 22, 28, 6))
        head.pack(fill='x')
        ttk.Label(head, text=title, background=PANEL, foreground=GOLD, font=(title_font(), 22)).pack(anchor='w')
        ttk.Label(head, text=subtitle, style='Muted.TLabel').pack(anchor='w', pady=(2, 8))
        tk.Frame(head, bg=LINE, height=1).pack(fill='x')
        canvas = tk.Canvas(outer, bg=PANEL, highlightthickness=0, bd=0)
        bar = ttk.Scrollbar(outer, orient='vertical', command=canvas.yview)
        inner = ttk.Frame(canvas, padding=(28, 0, 28, 24))
        inner.columnconfigure(1, weight=1)
        window = canvas.create_window(0, 0, window=inner, anchor='nw')
        inner.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(window, width=e.width))
        canvas.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y')
        canvas.pack(side='left', fill='both', expand=True)
        outer.scroll = lambda units: canvas.yview_scroll(units, 'units') if inner.winfo_height() > canvas.winfo_height() else None
        outer.canvas, outer.inner = canvas, inner  # the search scrolls to a setting (jump)
        self.pages[name] = outer
        self.building_page = name
        self.page_titles[name] = title
        return inner

    # ---- layout ------------------------------------------------------------------------------
    def build(self):
        tk, ttk = self.tk, self.ttk
        self.custom_frame, self.tray = None, None
        self.titlebar = None if self.app.get('windows_title_bar') else self.build_titlebar()
        self.root.bind('<Unmap>', self.minimized, add='+')
        side = tk.Frame(self.root, bg=BG, width=self.px(220))
        side.pack(side='left', fill='y')
        side.pack_propagate(False)
        tk.Label(side, text='SHADOW OF\nTHE COLOSSUS', bg=BG, fg=GOLD, font=(title_font(), 16), justify='left').pack(
            anchor='w', padx=22, pady=(26, 0))
        tk.Frame(side, bg=GOLD, height=1, width=self.px(46)).pack(anchor='w', padx=22, pady=(10, 8))
        tk.Label(side, text=_('port nativo · Windows', 'native port · Windows') + f'  ·  v{VERSION}', bg=BG, fg=MUTED,
                 font=(ui_font(), 9)).pack(anchor='w', padx=22, pady=(0, 14))
        self.page_titles = {}
        self.build_search(side)
        # The pages as in the bloodborne_pc launcher: a thin line icon, a card behind the current
        # page with the sword's light at its left edge, a softer card under the mouse.
        self.nav, self.current_page = {}, None
        px, width, height = self.px, 196, 38
        self.nav_pills = {'on': self.theme.box(width, height, 8, CARD, LINE, bg=BG),
                          'hover': self.theme.box(width, height, 8, theme.SURFACE, bg=BG)}
        accent = self.theme.box(3, 18, 1.5, SIGIL, bg=CARD)
        for name, title in (('play', _('Jogar', 'Play')), ('game', _('Jogo', 'Game')), ('saves', _('Saves', 'Saves')),
                            ('graphics', _('Imagem e FPS', 'Display & FPS')), ('controls', _('Controles', 'Controls')),
                            ('help', _('Ajuda', 'Help')), ('advanced', _('Avançado', 'Advanced')),
                            ('debug', _('Debug', 'Debug'))):
            c = tk.Canvas(side, width=px(width), height=px(height), bg=BG, highlightthickness=0, bd=0, cursor='hand2')
            c.pack(padx=px(12), pady=(0, px(2)))
            mid = px(height / 2)
            items = {'pill': c.create_image(0, 0, anchor='nw', state='hidden'),
                     'bar': c.create_image(px(1), mid, anchor='w', image=accent, state='hidden'),
                     'icon': c.create_text(px(22), mid, text=theme.ICON[NAV_ICONS[name]], font=(theme.ICONS, 12)),
                     'label': c.create_text(px(44), mid, text=title, anchor='w',
                                              font=(theme.BODY_MEDIUM if LANG not in ('zh', 'ko') else ui_font(), 10))}
            c.bind('<Button-1>', lambda _e, n=name: self.show(n))
            c.bind('<Enter>', lambda _e, n=name: self.nav_paint(n, hover=True))
            c.bind('<Leave>', lambda _e, n=name: self.nav_paint(n))
            self.nav[name] = (c, items)
            self.nav_paint(name)
        self.nav_end = tk.Frame(side, bg=BG)  # the entries are packed above it (apply_view)
        self.nav_end.pack(fill='x')
        tk.Frame(side, bg=BG).pack(fill='both', expand=True)
        ttk.Checkbutton(side, text=_('Visão avançada', 'Expert view'), variable=self.var('expert_view'),
                        style='Bar.TCheckbutton', command=self.apply_view).pack(anchor='w', padx=22, pady=(0, 10))
        for text, command in ((_('Registro do jogo', 'Game log'), lambda: self.show('log')),
                              (_('Procurar atualização', 'Check for updates'), lambda: self.check_updates(True)),
                              (_('Configurar de novo', 'Set up again'), self.show_setup)):
            link = tk.Label(side, text=text, bg=BG, fg=MUTED, font=(ui_font(), 9, 'underline'), cursor='hand2')
            link.pack(anchor='w', padx=22, pady=(0, 6))
            link.bind('<Button-1>', lambda _e, c=command: c())
        tk.Frame(side, bg=BG, height=12).pack()

        right = tk.Frame(self.root, bg=PANEL)
        right.pack(side='left', fill='both', expand=True)
        bar = tk.Frame(right, bg=BG, height=self.px(72))
        bar.pack(fill='x', side='bottom')
        tk.Frame(right, bg=LINE, height=1).pack(fill='x', side='bottom')
        bar.pack_propagate(False)
        self.play_button = ttk.Button(bar, text=_('JOGAR', 'PLAY'), style='Play.TButton', command=self.play)
        self.play_button.pack(side='right', padx=(10, 24), pady=11)
        self.stop_button = ttk.Button(bar, text=_('Parar', 'Stop'), command=self.stop, state='disabled')
        self.stop_button.pack(side='right', pady=11)
        ttk.Button(bar, text=_('Relatório', 'Report'), command=self.save_report).pack(side='right', padx=(0, 8), pady=11)
        # Packed after the buttons: on a narrow window the summary wraps instead of pushing them out.
        self.status = tk.Label(bar, text='', bg=BG, fg=MUTED, font=(ui_font(), 10), anchor='w', justify='left')
        self.status.pack(side='left', fill='x', expand=True, padx=(24, 12))
        self.status.bind('<Configure>', lambda e: self.status.configure(wraplength=max(self.px(120), e.width)))
        self.content = tk.Frame(right, bg=PANEL)
        self.content.pack(fill='both', expand=True)

        self.pages = {}
        self.build_play()
        self.build_game()
        self.build_saves()
        self.build_graphics()
        self.build_controls()
        self.build_help()
        self.build_advanced()
        self.build_debug()
        self.build_log()
        self.root.bind_all('<MouseWheel>', self.wheel)
        self.page_titles['log'] = _('Registro do jogo', 'Game log')
        self.search_ready(self.page_titles)
        self.apply_view()
        self.refresh_status()

    def wheel(self, event):
        page = self.pages.get(self.current_page)
        if hasattr(page, 'scroll') and not isinstance(event.widget, (self.tk.Text, self.ttk.Combobox)) \
                and 'popdown' not in str(event.widget):
            page.scroll(int(-event.delta / 120))

    def show(self, name):
        for page in self.pages.values():
            page.pack_forget()
        self.pages[name].pack(fill='both', expand=True)
        self.current_page = name
        for n in self.nav:
            self.nav_paint(n)
        if name == 'controls':  # the controller test reads only while its page is shown
            self.pad_start()
        else:
            self.pad_stop()
        if hasattr(self, 'nav_end'):  # an expert page found by the search shows its entry meanwhile
            self.apply_view()
        if name == 'play':
            self.refresh_status()

    TITLE_ICONS = {'minimize': '\ue921', 'maximize': '\ue922', 'restore': '\ue923', 'close': '\ue8bb'}

    def build_titlebar(self):
        """The launcher's own title bar, as in the bloodborne_pc launcher: a drag area in the side
        bar's colour and the window's three buttons. Windows' caption goes once the window exists
        (window_chrome, sotc_frame)."""
        tk, px = self.tk, self.px
        bar = tk.Frame(self.root, bg=BG, height=px(32))
        bar.pack(side='top', fill='x')
        bar.pack_propagate(False)
        self.title_buttons = {}
        for name, command, hover in (('close', self.close, '#8a1c1c'),
                                     ('maximize', self.toggle_maximized, theme.SURFACE),
                                     ('minimize', self.root.iconify, theme.SURFACE)):
            cell = tk.Frame(bar, bg=BG, width=px(46), height=px(32))
            cell.pack(side='right')
            cell.pack_propagate(False)
            glyph = tk.Label(cell, text=self.TITLE_ICONS[name], bg=BG, fg=theme.TEXT_SOFT, font=(theme.ICONS, 9))
            glyph.pack(fill='both', expand=True)

            def paint(on, cell=cell, glyph=glyph, hover=hover, name=name):
                cell.configure(bg=hover if on else BG)
                glyph.configure(bg=hover if on else BG,
                                fg=theme.TEXT_STRONG if on and name == 'close' else TEXT if on else theme.TEXT_SOFT)
            for widget in (cell, glyph):
                widget.bind('<Enter>', lambda _e, paint=paint: paint(True))
                widget.bind('<Leave>', lambda _e, paint=paint: paint(False))
                widget.bind('<ButtonRelease-1>', lambda e, command=command, cell=cell, glyph=glyph:
                            command() if e.widget.winfo_containing(e.x_root, e.y_root) in (cell, glyph) else None)
            self.title_buttons[name] = glyph
        self.root.bind('<Configure>', lambda e: self.title_state() if e.widget is self.root else None, add='+')
        self.root.after(100, self.window_chrome)
        return bar

    def minimized(self, event):
        """Minimized, with "Minimize to the system tray" on: off the task bar, an icon in the tray."""
        if event.widget is self.root and self.var('minimize_to_tray').get():
            self.root.after(1, self.to_tray)

    def to_tray(self):
        if self.root.state() != 'iconic':
            return
        try:
            if self.tray is None:
                import sotc_tray
                self.tray = sotc_tray.Tray(PORT_DIR / 'launcher' / 'sotc.ico', 'Shadow of the Colossus', self.bring_back,
                                           self.quit_from_tray, (_('Abrir', 'Open'), _('Sair', 'Quit')), root=self.root)
            if self.tray.show():
                self.root.withdraw()
        except OSError:
            pass  # no tray: it stays on the task bar

    def bring_back(self):
        if self.tray:
            self.tray.hide()
        self.root.deiconify()
        self.root.state('normal')
        self.root.lift()
        self.root.focus_force()

    def quit_from_tray(self):
        self.bring_back()
        self.close()

    def toggle_maximized(self):
        self.root.state('normal' if self.root.state() == 'zoomed' else 'zoomed')

    def title_state(self):
        """The maximize button follows the window's state; Windows learns where the bar is."""
        if not getattr(self, 'title_buttons', None):
            return
        glyph = self.TITLE_ICONS['restore' if self.root.state() == 'zoomed' else 'maximize']
        if self.title_buttons['maximize'].cget('text') != glyph:
            self.title_buttons['maximize'].configure(text=glyph)
        if self.custom_frame:  # where Windows finds the caption and the buttons (physical pixels)
            self.custom_frame.bar = self.titlebar.winfo_height()
            self.custom_frame.buttons = sum(cell.master.winfo_width() for cell in self.title_buttons.values())
            self.custom_frame.edge, self.custom_frame.corner = self.px(5), self.px(10)

    def window_chrome(self):
        """Windows' caption off (sotc_frame), the frame dark; Windows' caption back when that fails."""
        try:
            hwnd = int(self.root.wm_frame(), 16)
            if hwnd == self.root.winfo_id() and getattr(self, 'chrome_waits', 0) < 100:
                self.chrome_waits = getattr(self, 'chrome_waits', 0) + 1  # not wrapped by Tk yet
                self.root.after(50, self.window_chrome)
                return
            dwm = ctypes.windll.dwmapi

            def attribute(number, value):
                v = ctypes.c_int(value)
                return dwm.DwmSetWindowAttribute(hwnd, number, ctypes.byref(v), ctypes.sizeof(v))
            if attribute(20, 1):  # DWMWA_USE_IMMERSIVE_DARK_MODE; 19 before Windows 10 20H1
                attribute(19, 1)
            attribute(34, 0x000e0c0a)  # DWMWA_BORDER_COLOR (Windows 11): the side bar's dark
            if self.titlebar is not None and self.custom_frame is None:
                frame = sotc_frame.Frame(hwnd, [self.root.winfo_id(), self.titlebar.winfo_id()])
                self.custom_frame = frame if frame.install() else False
                if not self.custom_frame:
                    self.titlebar.pack_forget()
                self.title_state()
        except (AttributeError, OSError, ValueError, self.tk.TclError):
            pass

    def apply_view(self):
        """The side bar's entries: the expert pages only in the expert view."""
        expert = self.var('expert_view').get()
        for name, (c, _items) in self.nav.items():
            c.pack_forget()
        for name, (c, _items) in self.nav.items():
            if expert or name not in EXPERT_PAGES or name == self.current_page:
                c.pack(padx=self.px(12), pady=(0, self.px(2)), before=self.nav_end)

    def nav_paint(self, name, hover=False):
        """A side bar entry: a card behind the current page, a softer one under the mouse."""
        c, items = self.nav[name]
        on = name == self.current_page
        pill = self.nav_pills['on'] if on else self.nav_pills['hover'] if hover else ''
        c.itemconfigure(items['pill'], image=pill, state='normal' if pill else 'hidden')
        c.itemconfigure(items['bar'], state='normal' if on else 'hidden')
        c.itemconfigure(items['icon'], fill=GOLD if on else theme.TEXT_SOFT if hover else MUTED)
        c.itemconfigure(items['label'], fill=theme.TEXT_STRONG if on else TEXT if hover else theme.TEXT_SOFT)

    def build_play(self):
        tk, ttk = self.tk, self.ttk
        page = tk.Frame(self.content, bg=PANEL)
        self.pages['play'] = page
        self.building_page = 'play'
        self.page_titles['play'] = _('Jogar', 'Play')
        # The banner and the settings scroll together when the window is shorter than them.
        canvas = tk.Canvas(page, bg=PANEL, highlightthickness=0, bd=0)
        bar = ttk.Scrollbar(page, orient='vertical', command=canvas.yview)
        inner = tk.Frame(canvas, bg=PANEL)
        window = canvas.create_window(0, 0, window=inner, anchor='nw')
        inner.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(window, width=e.width))
        canvas.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y')
        canvas.pack(side='left', fill='both', expand=True)
        page.scroll = lambda units: canvas.yview_scroll(units, 'units') if inner.winfo_height() > canvas.winfo_height() else None
        page.canvas, page.inner = canvas, inner  # the search scrolls to a setting (jump)
        self.banner = tk.Canvas(inner, height=self.px(290), bg=BG, highlightthickness=0, bd=0)
        self.banner.pack(fill='x')
        self.banner.bind('<Configure>', lambda _e: self.draw_banner())
        body = ttk.Frame(inner, padding=(28, 12, 28, 8))
        body.pack(fill='both', expand=True)
        body.columnconfigure(0, weight=3)
        body.columnconfigure(1, weight=2)
        info = ttk.Frame(body)
        info.grid(row=0, column=0, sticky='nw', padx=(0, 24))
        ttk.Label(info, text=_('Verificação', 'Ready check'), style='Section.TLabel').pack(anchor='w', pady=(0, 6))
        self.checks = {}
        for key in ('game', 'dlc', 'sound', 'saves', 'gpu'):
            self.checks[key] = ttk.Label(info, text='', justify='left', wraplength=self.px(440))
            self.checks[key].pack(anchor='w', pady=3)
        quick = ttk.Frame(body)
        quick.grid(row=0, column=1, sticky='nw')
        ttk.Label(quick, text=_('Ajustes rápidos', 'Quick settings'), style='Section.TLabel').grid(
            row=0, column=0, columnspan=2, sticky='w', pady=(0, 2))
        self.row(quick, _('Modo de jogo', 'Play mode'), self.choice(quick, 'play_mode', PLAY_MODES, 24))
        self.play_mode_hint = ttk.Label(quick, text='', style='Muted.TLabel', wraplength=self.px(300), justify='left')
        self.play_mode_hint.grid(row=self.next_row(quick), column=0, columnspan=2, sticky='w', pady=(2, 2))
        self.row(quick, _('Saída', 'Output'), self.choice(quick, 'output_res', OUTPUTS, 24))
        self.row(quick, _('Upscaler', 'Upscaler'), self.choice(quick, 'upscaler', UPSCALERS, 24))
        self.row(quick, _('Nitidez (RCAS)', 'Sharpening (RCAS)'), self.choice(quick, 'sharpness', SHARPNESS, 24))
        self.row(quick, _('Geração de quadros', 'Frame generation'), self.choice(quick, 'framegen', FRAMEGEN, 24))
        self.row(quick, _('FPS', 'FPS'), self.choice(quick, 'fps_mode', FPS_MODES, 24))
        self.check(quick, 'fps_dynamic', _('FPS dinâmico (no modo 60)', 'Dynamic FPS (60 FPS mode)'),
                   _('Quando o jogo não segura 60, trava num degrau estável que combina com a tela (48 em 144 Hz, '
                     '40 em 120 Hz, 30 em 60 Hz) em vez de oscilar, e volta a 60 quando sobra folga.',
                     'When the game cannot keep 60 it locks to a steady step that suits the screen (48 at '
                     '144 Hz, 40 at 120 Hz, 30 at 60 Hz) instead of swinging, and goes back to 60 when it can.'))
        self.row(quick, _('Texturas', 'Textures'), self.choice(quick, 'textures', TEXTURES, 24))
        self.row(quick, _('Memória da placa', 'Card memory'), self.choice(quick, 'vram_profile', VRAM_PROFILES, 24))
        self.vram_profile_hint = ttk.Label(quick, text='', style='Muted.TLabel', wraplength=self.px(300),
                                           justify='left')
        self.vram_profile_hint.grid(row=self.next_row(quick), column=0, columnspan=2, sticky='w', pady=(0, 2))
        self.var('vram_profile').trace_add('write', lambda *_a: self.show_vram_profile_hint())
        self.show_vram_profile_hint()
        self.check(quick, 'af16', _('Texturas nítidas (filtro 16x)', 'Sharp textures (16x filtering)'))
        self.check(quick, 'hdr', _('HDR (HDR do Windows ligado)', 'HDR (Windows HDR on)'))
        self.check(quick, 'fullscreen', _('Tela cheia', 'Fullscreen'))
        self.check(quick, 'neo', _('Modo PS4 Pro', 'PS4 Pro mode'))
        self.check(quick, 'rb_hot', _('Mais FPS: leituras rápidas da GPU',
                                      'More FPS: fast GPU readbacks'))
        for key in ('af16', 'textures', 'fps_mode', 'hdr', 'fullscreen', 'neo', 'output_res', 'upscaler', 'sharpness', 'framegen'):
            self.vars[key].trace_add('write', lambda *_a: self.refresh_status())
        self.applying_mode = False
        self.var('play_mode').trace_add('write', lambda *_a: self.apply_play_mode())
        for key in {k for values in PLAY_MODE_VALUES.values() for k in values} | {'upscaler'}:
            self.var(key).trace_add('write', lambda *_a, k=key: self.play_mode_edited(k))
        self.show_play_mode_hint()

    def play_mode_values(self, mode):
        values = dict(PLAY_MODE_VALUES.get(mode, {}))
        if mode == 'extreme':
            name = getattr(self, 'gpu_name', '').upper()
            values['upscaler'] = 'dlss' if 'NVIDIA' in name and 'RTX' in name else 'fsr1'
        return values

    def show_vram_profile_hint(self):
        """What the video memory profile does with the card found."""
        if not hasattr(self, 'vram_profile_hint'):
            return
        mb = int(self.app.get('gpu_vram_mb') or 0)
        card = (_('placa com {:.0f} GB', 'card with {:.0f} GB').format(mb / 1024) if mb
                else _('placa ainda não verificada', 'card not checked yet'))
        current = dict(self.app, vram_profile=self.var('vram_profile').get())
        if light_vram(current):
            text = _('Leve ({}): uso de memória de vídeo no máximo Padrão, sem reserva de envio nela.',
                     'Light ({}): video memory use at most Default, no upload reserve in it.').format(card)
        else:
            text = _('Completo ({}): uso de memória de vídeo como escolhido.',
                     'Full ({}): video memory use as chosen.').format(card)
        self.vram_profile_hint.configure(text=text)

    def show_play_mode_hint(self):
        self.play_mode_hint.configure(text=_(*PLAY_MODE_HINTS.get(self.var('play_mode').get(), ('', ''))))

    def apply_play_mode(self):
        """The chosen mode's settings (Custom changes nothing)."""
        self.show_play_mode_hint()
        values = self.play_mode_values(self.var('play_mode').get())
        self.applying_mode = True
        try:
            for key, value in values.items():
                self.var(key).set(value)
        finally:
            self.applying_mode = False

    def play_mode_edited(self, key):
        """A setting of the mode changed by hand: the mode becomes Custom."""
        if self.applying_mode:
            return
        mode = self.var('play_mode').get()
        values = self.play_mode_values(mode)
        if key in values and str(self.var(key).get()) != str(values[key]):
            self.applying_mode = True
            try:
                self.var('play_mode').set('custom')
                self.show_play_mode_hint()
            finally:
                self.applying_mode = False

    def draw_banner(self):
        """The dump's own art (sce_sys/pic1.png), darker on the left and fading into the page
        (sotc_theme.hero), the game and its version under the title."""
        c = self.banner
        c.delete('all')
        w, h = max(c.winfo_width(), 400), int(c['height'])
        art = Path(self.var('game_dir').get() or '.') / 'sce_sys' / 'pic1.png'
        if self.banner_source != art:
            self.banner_source, self.banner_image = art, None
            try:
                from PIL import Image
                with Image.open(art) as image:
                    self.banner_image = image.convert('RGBA')
            except (ImportError, OSError):
                pass
        if self.banner_image is not None:
            self.banner_photo = self.theme.cached(('hero', str(art), w, h),
                                                  lambda: theme.hero(self.banner_image, (w, h), PANEL))
            c.create_image(0, 0, image=self.banner_photo, anchor='nw')
            c.create_text(33, h - 74, text='Shadow of the Colossus', anchor='w', fill=theme.TEXT_STRONG,
                          font=(title_font(), 26))
        else:
            c.create_text(30, h - 80, text='Shadow of the Colossus', anchor='w', fill=theme.TEXT_STRONG,
                          font=(title_font(), 30))
            c.create_text(w // 2, h // 2 - 40, text=_('Escolha a pasta do jogo (Jogo e saves)',
                                                       'Choose your game folder (Game & saves)'),
                          fill=MUTED, font=(ui_font(), 11))
        info = game_info(self.var('game_dir').get())
        sub = (_('{} · versão do jogo {}', '{} · game version {}').format(*info) if info
               else _('Pasta do jogo não escolhida', 'Game folder not set'))
        c.create_text(35, h - 38, text=sub, anchor='w', fill=GOLD, font=(ui_font(), 11))

    def build_game(self):
        ttk = self.ttk
        f = self.scrolled_page('game', _('Jogo', 'Game'),
                               _('O seu dump do jogo, a atualização, as DLCs e os módulos de som do seu console.',
                                 'Your game dump, the update, the DLC and your console\'s sound modules.'))
        self.section(f, _('Biblioteca de jogos', 'Game library'), top=4)
        self.note(f, _('Como no shadPS4: adicione as pastas onde ficam os seus jogos e escolha o Shadow of the '
                       'Colossus na lista (duplo clique).',
                       'As in shadPS4: add the folders that hold your games and pick Shadow of the Colossus in the '
                       'list (double click).'), top=0)
        tree_holder = ttk.Frame(f)
        tree_holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(6, 0))
        style = self.ttk.Style()
        style.configure('Library.Treeview', rowheight=self.px(40), background=CARD, fieldbackground=CARD,
                        foreground=TEXT, borderwidth=0)
        style.configure('Library.Treeview.Heading', background=PANEL, foreground=MUTED)
        self.library = ttk.Treeview(tree_holder, columns=('id', 'version', 'path'), height=4,
                                    style='Library.Treeview')
        for column, title, width in (('#0', _('Jogo', 'Game'), 300), ('id', 'ID', 100),
                                     ('version', _('Versão', 'Version'), 70), ('path', _('Pasta', 'Folder'), 380)):
            self.library.heading(column, text=title, anchor='w')
            self.library.column(column, width=self.px(width), anchor='w', stretch=column == 'path')
        self.library.pack(side='left', fill='x', expand=True)
        self.library.bind('<Double-1>', lambda _e: self.use_selected_game())
        self.library_icons = {}
        buttons = ttk.Frame(f)
        buttons.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(6, 0))
        ttk.Button(buttons, text=_('Usar este jogo', 'Use this game'), command=self.use_selected_game).pack(side='left')
        ttk.Button(buttons, text=_('Adicionar pasta de jogos…', 'Add a games folder…'),
                   command=self.add_library_dir).pack(side='left', padx=(6, 0))
        ttk.Button(buttons, text=_('Remover pasta', 'Remove folder'),
                   command=self.remove_library_dir).pack(side='left', padx=(6, 0))
        ttk.Button(buttons, text=_('Procurar no PC', 'Search this PC'),
                   command=lambda: self.start_discovery(manual=True)).pack(side='left', padx=(6, 0))
        self.library_label = ttk.Label(f, text='', style='Muted.TLabel')
        self.library_label.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(4, 0))
        self.section(f, _('Jogo', 'Game'))
        self.folder(f, 'game_dir', _('Pasta do jogo', 'Game folder'), _('Pasta com eboot.bin', 'Folder with eboot.bin'),
                    _('O dump CUSA08809 (versão europeia 1.01) ou CUSA08034 (versão americana 1.00), com eboot.bin, '
                      'sce_module e sce_sys.',
                      'The CUSA08809 dump (EU version 1.01) or CUSA08034 dump (US version 1.00) with eboot.bin, '
                      'sce_module and sce_sys.'),
                    on_change=self.game_changed)
        self.section(f, _('Som', 'Sound'))
        self.folder(f, 'sys_modules', _('Módulos de som', 'Sound modules'),
                    _('Pasta com libSceNgs2.sprx e libSceUlt.sprx', 'Folder with libSceNgs2.sprx and libSceUlt.sprx'),
                    _('libSceNgs2.sprx e libSceUlt.sprx do SEU console, decriptados. O lançador procura sozinho num '
                      'shadPS4 instalado no PC (user/sys_modules) e copia para a pasta de dados do port. São firmware '
                      'da Sony e nunca vêm com o port. Sem eles o jogo roda sem som.',
                      'libSceNgs2.sprx and libSceUlt.sprx from YOUR console, decrypted. The launcher looks for them in '
                      'a shadPS4 installed on this PC (user/sys_modules) and copies them to the port\'s data folder. '
                      'They are Sony firmware and never come with the port. Without them the game is silent.'),
                    on_change=self.refresh_status)
        self.row(f, _('Volume', 'Volume'), self.choice(f, 'volume', VOLUME),
                 _('Aumenta o som do jogo, que sai baixo. Acima de 100% os picos são suavizados em vez de '
                   'estourar. No jogo, o modo de alto-falante "TV" também deixa o som mais alto.',
                   'Raises the game sound, which is quiet. Above 100% the peaks are softened instead of '
                   'clipping. In game, the "TV" speaker mode is louder too.'))
        self.section(f, _('DLC', 'DLC'))
        self.folder(f, 'dlc_dir', _('Pasta de DLC', 'DLC folder'), _('Pasta com as DLCs do jogo', "Folder with the game's DLC"),
                    _('Como no shadPS4: uma pasta com uma subpasta por DLC (cada uma com sce_sys/param.sfo), ou a '
                      'pasta addcont do shadPS4 (que tem a subpasta CUSA08809 ou CUSA08034). Só valem as DLCs da '
                      'região do jogo. Vazio = sem DLC.',
                      "As in shadPS4: a folder with one subfolder per DLC (each with sce_sys/param.sfo), or "
                      "shadPS4's addcont folder (which holds the CUSA08809 or CUSA08034 subfolder). Only DLC of the "
                      "game's region counts. Empty = no DLC."),
                    on_change=self.refresh_status)
        self.section(f, _('Sistema', 'System'))
        self.row(f, _('Idioma do sistema', 'System language'), self.choice(f, 'language', LANGUAGES),
                 _('O idioma que o jogo vê no "console".', 'The language the game sees on the "console".'))
        self.row(f, _('Nome do jogador', 'Player name'), ttk.Entry(f, textvariable=self.var('player_name'), width=30))
        self.check(f, 'skip_logos', _('Pular os logotipos de abertura', 'Skip the opening logos'),
                   _('O jogo abre direto no menu, sem os logotipos de abertura e o aviso de salvamento automático.',
                     'The game opens straight to the menu, without the opening logos and the autosave notice.'))

    def build_graphics(self):
        f = self.scrolled_page('graphics', _('Imagem e FPS', 'Display & FPS'),
                               _('Os modos vêm do fork shadPS4 SotC e valem para as versões EU 1.01 e US 1.00.',
                                 'The modes come from the shadPS4 SotC fork and apply to EU version 1.01 and US '
                                 'version 1.00.'))
        self.section(f, _('Modos', 'Modes'), top=4)
        self.check(f, 'af16', _('Texturas nítidas (filtro anisotrópico 16x)', 'Sharp textures (16x anisotropic filtering)'),
                   _('Chão, grama e rochas ficam nítidos à distância, quase sem custo.',
                     'Ground, grass and rocks stay sharp at a distance, at almost no cost.'))
        self.row(f, _('FPS', 'FPS'), self.choice(f, 'fps_mode', FPS_MODES),
                 _('30: o limite do próprio jogo, como no PS4 (a física é feita para ele). 60: sem o limite do '
                   'jogo, no máximo 60 (40 a 60 conforme a região, num processador rápido). Ilimitado: acima de '
                   '60 em telas rápidas. Sem o limite, algumas cenas podem se comportar diferente. No jogo, F7 '
                   'liga e desliga o limite de 30 a qualquer momento.',
                   '30: the game\'s own cap, as on PS4 (its physics are made for it). 60: the game\'s cap off, at '
                   'most 60 (40 to 60 depending on the area, with a fast CPU). Unlimited: above 60 on fast '
                   'screens. Without the cap some scenes may behave differently. In game, F7 turns the 30 FPS '
                   'cap on and off at any time.'))
        self.row(f, _('Geração de quadros', 'Frame generation'), self.choice(f, 'framegen', FRAMEGEN),
                 _('Uma imagem gerada entre duas do jogo: com 30 FPS (limite ligado) a tela mostra 60. DLSS-G: '
                   'NVIDIA RTX 40/50. FSR 3: qualquer placa (RTX 20/30, AMD, Intel). Automática: DLSS-G se a '
                   'placa tiver, senão FSR 3. Experimental: só faz diferença com o modo 60 FPS desligado, e a '
                   'barra de vida e de stamina pode tremer (a imagem gerada inclui a interface). '
                   'No jogo, F8 troca o modo (desligada, DLSS-G, FSR 3). Melhor com '
                   'a tela a 60 ou 120 Hz, ou G-SYNC/FreeSync.',
                   'One generated frame between two game frames: at 30 FPS (capped) the screen shows '
                   '60. DLSS-G: NVIDIA RTX 40/50. FSR 3: any GPU (RTX 20/30, AMD, Intel). Automatic: DLSS-G when '
                   'the GPU has it, else FSR 3. Experimental: it only matters with the 60 FPS mode off, and the '
                   'health and stamina bars may shake (the generated frame includes the interface). '
                   'In game, F8 switches the mode (off, DLSS-G, FSR 3). Best with the '
                   'screen at 60 or 120 Hz, or G-SYNC/FreeSync.') + ' ' +
                 _('FSR 3 é experimental: numa RTX 3060 a taxa real do jogo caiu (24-30 FPS em vez de 40-50), '
                   'com mais travadas e falhas na imagem. DLSS-G só funciona nas RTX 40/50: nas outras placas '
                   'ela fica desligada e o F8 passa para FSR 3.',
                   "FSR 3 is experimental: on an RTX 3060 the game's real frame rate dropped (24-30 FPS instead of "
                   '40-50), with more stutter and image glitches. DLSS-G only works on RTX 40/50: on other cards '
                   'it stays off and F8 switches to FSR 3.'))
        self.row(f, _('Multiplicador do DLSS-G', 'DLSS-G multiplier'), self.choice(f, 'fg_mult', FG_MULT),
                 _('Só vale com a geração de quadros em DLSS-G. 2x é uma imagem gerada por imagem do jogo; 3x, 4x e '
                   '6x geram 2, 3 ou 5 (NVIDIA RTX 50; na RTX 40 vale 2x). Dinâmico escolhe sozinho para chegar à '
                   'taxa da tela (só se o driver suportar; senão vale 6x) e não funciona com limitador de FPS '
                   'nem V-Sync. Experimental: mais quadros gerados aumentam a latência.',
                   'Only with frame generation set to DLSS-G. 2x is one generated frame per game frame; 3x, 4x '
                   'and 6x generate 2, 3 or 5 (NVIDIA RTX 50; RTX 40 stays at 2x). Dynamic picks the number to '
                   "reach the screen's refresh rate (only if the driver supports it; else it is 6x) and does "
                   'not work with a frame limiter or V-Sync. Experimental: more generated frames mean more latency.'))
        self.check(f, 'hdr', _('HDR', 'HDR'),
                   _('Saída HDR10 e a renderização HDR do jogo. Ligue o HDR do Windows antes de jogar.',
                     'HDR10 output and the game\'s HDR rendering. Turn on Windows HDR before playing.'))
        self.check(f, 'fg_hudless', _('Geração de quadros sem a interface (experimental)',
                                      'Frame generation without the interface (experimental)'),
                   _('Gera os quadros intermediários só com a cena e põe a interface por cima, sem interpolá-la: '
                     'tira o tremor das barras de vida e stamina e das letras. Pode desbotar o logo na tela '
                     'de título e mostrar a cena antiga atrás das telas de carregamento.',
                     'Generates the in-between frames from the scene only and draws the interface on top, '
                     'uninterpolated: removes the shaking health/stamina bars and text. It can fade the '
                     'logo on the title screen and show the old scene behind loading screens.'))
        self.window_icon_row(f)
        self.section(f, _('Resolução', 'Resolution'))
        self.row(f, _('Saída', 'Output'), self.choice(f, 'output_res', OUTPUTS),
                 _('O jogo renderiza nesse tamanho. '
                   'Mais largo que 16:9 (21:9, 32:9) mostra mais dos lados (Hor+), sem esticar; menus e vídeos '
                   'podem ficar esticados. 720p renderiza em 1080p; o modo PS4 Pro usa os tamanhos do Pro. '
                   'Num monitor menor que a saída, a janela cabe na tela e a tela cheia usa o monitor todo.',
                   'The game renders at this size. Wider '
                   'than 16:9 (21:9, 32:9) shows more at the sides (Hor+), not stretched; menus and videos may '
                   'look stretched. 720p renders at 1080p; PS4 Pro mode uses the Pro sizes. On a monitor smaller '
                   'than the output the window fits the screen and fullscreen uses the whole monitor.'))
        self.row(f, _('Upscaler', 'Upscaler'), self.choice(f, 'upscaler', UPSCALERS),
                 _('FSR 1 amplia com bordas nítidas (quando o jogo renderiza menor que a saída).', 'FSR 1 upscales with sharp edges (when the game renders smaller than the output).'))
        self.row(f, _('Nitidez (RCAS)', 'Sharpening (RCAS)'), self.choice(f, 'sharpness', SHARPNESS))
        self.section(f, _('NVIDIA DLSS e AMD FSR 4', 'NVIDIA DLSS and AMD FSR 4'))
        self.note(f, _('Com o upscaler DLSS (placas RTX) ou AMD FSR 4.1: o jogo renderiza menor e o upscaler amplia '
                       'com detalhe reconstruído dos quadros anteriores. O FSR 4.1 (inteligência artificial) roda nas '
                       'Radeon RX 6000, 7000 e 9000; nas outras placas o mesmo item usa o FSR 3.1. Sem a placa ou '
                       'sem os arquivos, o FSR 1 amplia a mesma imagem menor. Os ajustes abaixo valem para os dois.',
                       'With the DLSS upscaler (RTX cards) or AMD FSR 4.1: the game renders smaller and the upscaler '
                       'enlarges it with detail rebuilt from earlier frames. FSR 4.1 (machine learning) runs on '
                       'Radeon RX 6000, 7000 and 9000; on other cards the same item uses FSR 3.1. Without the card '
                       'or the files, FSR 1 enlarges the same smaller image. The settings below apply to both.'),
                  top=2)
        self.row(f, _('Modo', 'Mode'), self.choice(f, 'dlss_mode', DLSS_MODES),
                 _('Fator entre a saída e o tamanho renderizado. DLAA: tamanho nativo com o anti-aliasing do '
                   'DLSS (mais pesado, usa mais memória de vídeo; sem memória bastante, volta ao FSR 1).',
                   'Factor between the output and the render size. DLAA: native size with DLSS anti-aliasing '
                   '(heavier, uses more video memory; without enough of it, back to FSR 1).'))
        self.row(f, _('Nitidez da cena', 'Scene sharpening'), self.choice(f, 'dlss_sharpen', DLSS_SHARPNESS))
        self.row(f, _('Nitidez do texto', 'Text sharpening'), self.choice(f, 'dlss_ui_sharpen', DLSS_UI_SHARPNESS))
        self.row(f, _('Nitidez do céu', 'Sky sharpening'), self.choice(f, 'dlss_sky_sharpen', DLSS_SKY_SHARPNESS))
        self.section(f, _('Modo PS4 Pro', 'PS4 Pro mode'))
        self.check(f, 'neo', _('Apresentar-se como PS4 Pro', 'Present as a PS4 Pro'),
                   _('O jogo passa a oferecer, no menu Opções > Apresentação, o Modo Grafismo do PS4 Pro: '
                     'Cinematográfico (1440p, mais detalhe) ou Desempenho. Mais pesado: no campo aberto mediu ~25 FPS '
                     'contra ~38 no modo normal.',
                     'The game then offers, in Options > Presentation, the PS4 Pro graphics mode: Cinematic '
                     '(1440p, more detail) or Performance. Heavier: ~25 FPS in the open world against ~38 normally.'))
        self.section(f, _('Desempenho', 'Performance'))
        self.check(f, 'perf_culling', _('Culling sem a GPU (recomendado)', 'Culling without the GPU (recommended)'),
                   _('Desliga o culling de índices feito na GPU (técnica do Pink-shadPS4): mesma imagem e bem menos '
                     'esperas pela GPU. No santuário: ~22 → ~28 FPS.',
                     'Turns off the GPU index-buffer culling (Pink-shadPS4): same image, far fewer GPU waits. '
                     'Sanctuary: ~22 → ~28 FPS.'))
        self.check(f, 'perf_record_thread', _('Comandos Vulkan em thread própria (recomendado)',
                                               'Vulkan commands on their own thread (recommended)'),
                   _('Grava e envia os comandos da GPU em outra thread (cp_record_thread do Pink-shadPS4). '
                     'Ponte do cânion no demo: ~24 → ~28 FPS. Desligue se notar travamentos.',
                     'Records and submits the GPU commands on another thread (cp_record_thread of '
                     'Pink-shadPS4). Canyon bridge in the demo: ~24 → ~28 FPS. Turn it off if the game hangs.'))
        self.check(f, 'perf_dma', _('Reflexos completos da água (acesso direto à memória)',
                                     'Full water reflections (direct memory access)'),
                   _('Deixa os shaders lerem tabelas da memória do jogo direto (como o Pink-shadPS4): o lago ondula '
                     'e reflete até longe. Desligado ganha ~10% de FPS, mas a água longe fica mais lisa. '
                     'Ao trocar, os shaders são compilados de novo na primeira vez. Em placas AMD e Intel fica '
                     'desligado (desenhava listras nas sombras e na água).',
                     'Lets shaders read tables straight from game memory (as Pink-shadPS4 does): the lake ripples '
                     'and reflects far away. Off gains ~10% FPS, but distant water is flatter. After a change '
                     'the shaders are compiled again the first time. Off on AMD and Intel cards (it drew stripes over '
                     'shadows and water).'))
        self.row(f, _('Envio de trabalho à GPU', 'GPU work submission'), self.choice(f, 'gpu_submit', GPU_SUBMIT),
                 _('Manda trabalho à GPU a cada 0,5 ms e lê resultados por um atalho (técnicas do Pink-shadPS4). '
                   'Medido em NVIDIA (+10–15%); em AMD/Intel use "Ligado" para testar e volte para "Automático" '
                   'se a tela ficar preta.',
                   'Sends work to the GPU every 0.5 ms and reads results through a shortcut (Pink-shadPS4 '
                   'techniques). Measured on NVIDIA (+10–15%); on AMD/Intel pick "On" to try it and go back to '
                   '"Automatic" if the screen turns black.'))
        self.row(f, _('Sombras distantes', 'Far shadows'), self.choice(f, 'shadow_reuse', SHADOW_REUSE),
                 _('Redesenha as sombras distantes do sol só a cada dois quadros e reaproveita no outro: um '
                   'pouco mais de FPS (~0,5%), mas elas piscam ao longe (AMD e NVIDIA GTX 1080, RTX 40/50). '
                   'Por isso vem desligado.',
                   'Draws the far sun shadows only every other frame and reuses them in between: a little more '
                   'FPS (~0.5%), but they blink in the distance (AMD and NVIDIA GTX 1080, RTX 40/50). Hence off '
                   'by default.'))
        self.row(f, _('Uso de memória de vídeo', 'Video memory use'), self.choice(f, 'vram_use', VRAM_USE),
                 _('Até onde o jogo enche a memória da placa antes de liberar texturas e buffers. Padrão: '
                   'libera a partir de 60%. Em placas de 8 GB ou mais, "Alto" ou "Máximo" evita recarregar '
                   'texturas (menos engasgos). Se aparecer erro de falta de memória, volte um nível.',
                   'How far the game fills the card memory before it frees textures and buffers. Default: '
                   'frees from 60%. On 8 GB cards or more, "High" or "Maximum" avoids reloading textures '
                   '(less stutter). On an out-of-memory error, go back one level.'))
        self.section(f, _('Grama', 'Grass'))
        self.row(f, _('Sombras da grama', 'Grass shadows'), self.choice(f, 'grass_shadows', GRASS_SHADOWS),
                 _('Até onde a grama projeta sombra. No PS4 só até ~8 m: ao cavalgar, uma faixa de grama '
                   'escurece logo à frente. "Longe" leva até ~24 m, com algum custo de FPS; "Perto" é mais leve, '
                   'para PCs fracos. Vale ao iniciar o jogo.',
                   'How far the grass casts shadows. On PS4 only to ~8 m: while riding, a band of grass '
                   'darkens just ahead. "Far" takes it to ~24 m, at some FPS cost; "Near" is lighter, for slow '
                   'PCs. Applies when the game starts.'))
        self.row(f, _('Distância da grama', 'Grass distance'), self.choice(f, 'grass_distance', VEG_DISTANCE),
                 _('Até onde a grama fica cheia. No PS4 ela rareia em faixas a poucos metros; mais longe '
                   'mantém a grama densa ao longe, sem deixá-la mais alta. Também no menu do jogo (vale na hora).',
                   'How far the grass stays full. On PS4 it thins out in bands a few metres ahead; farther '
                   'keeps the grass dense in the distance without making it taller. Also in the in-game '
                   'menu (applies at once).'))
        self.row(f, _('Altura da grama', 'Grass height'), self.choice(f, 'grass_height', VEG_HEIGHT),
                 _('Altura das folhas de grama. Recomendado: como no PS4, ou 150% para mais grama; 200% '
                   'vira quase um arbusto. Também no menu do jogo (vale na hora).',
                   'Height of the grass blades. Recommended: as on PS4, or 150% if you like more grass; 200% '
                   'turns it into bushes. Also in the in-game menu (applies at once).'))
        self.row(f, _('Distância do mato', 'Weed distance'), self.choice(f, 'weed_distance', WEED_DISTANCE),
                 _('O mesmo que a distância da grama, para as plantas mais altas entre a grama. Também no '
                   'menu do jogo (vale na hora).',
                   'The same as grass distance, for the taller weeds among the grass. Also in the in-game '
                   'menu (applies at once).'))
        self.row(f, _('Altura do mato', 'Weed height'), self.choice(f, 'weed_height', VEG_HEIGHT),
                 _('Altura das plantas mais altas entre a grama: "Bem mais alta" vira arbusto. Também no '
                   'menu do jogo (vale na hora).',
                   'Height of the taller weeds among the grass: "Much taller" turns them into shrubs. Also in '
                   'the in-game menu (applies at once).'))
        self.row(f, _('Detalhe da grama', 'Grass detail'), self.choice(f, 'grass_detail', GRASS_DETAIL),
                 _('A que distância a grama fica completa. No PS4 a grama (e sua sombra) perto da câmera '
                   'aparece tarde, quase em cima do jogador. Também no menu do jogo (vale na hora).',
                   'How far out the grass is drawn complete. On PS4 the grass near the camera (and its '
                   'shadow) appears late, almost on top of the player. Also in the in-game menu (applies at '
                   'once).'))
        self.row(f, _('Esmaecimento da grama', 'Grass fade'), self.choice(f, 'grass_fade', GRASS_FADE),
                 _('Como a grama some com a distância. Fora do valor do PS4 a grama fica mais rala; abaixo dele a '
                   'sombra da grama também some (0.5: quase sem sombra). Menos grama é um pouco mais leve para a '
                   'placa de vídeo, útil em PCs fracos. Vale ao iniciar o jogo.',
                   'How the grass fades out with distance. Away from the PS4 value the grass gets sparser; below it '
                   'the grass shadow fades too (0.5: almost no shadow). Less grass is a little lighter on the GPU, '
                   'useful on slow PCs. Applies when the game starts.'))
        self.section(f, _('Efeitos', 'Effects'))
        for key, title, _off in EFFECTS:
            self.check(f, key, _(*title))
        self.note(f, _('O motion blur, o brilho e os filtros ficam no menu do próprio jogo (Opções > Apresentação).',
                       "Motion blur, brightness and filters are in the game's own menu (Options > Presentation)."), top=4)
        self.section(f, _('Tela', 'Display'))
        screens = [('', (_('Principal', 'Primary'),) * 2)] + [(m[0], (m[1],) * 2) for m in monitors() if not m[3]]
        if len(screens) > 1 or self.var('game_monitor').get():
            self.row(f, _('Monitor do jogo', 'Game monitor'), self.choice(f, 'game_monitor', screens),
                     _('Em qual monitor a janela do jogo abre (e a tela cheia). Vale na próxima vez que o jogo abrir.',
                       'Which monitor the game window opens on (fullscreen too). Applies the next time the game starts.'))
        self.check(f, 'fullscreen', _('Tela cheia', 'Fullscreen'))
        self.row(f, _('Apresentação', 'Presentation'), self.choice(f, 'present_mode', PRESENT_MODES))
        self.check(f, 'hdr_sdr', _('HDR da imagem SDR (experimental)', 'HDR from the SDR image (experimental)'),
                   _('Saída HDR10 a partir da imagem do jogo. Precisa de HDR ligado no Windows e de um monitor '
                     'HDR; sem isso nada muda. Ajustes: SOTC_HDR_PAPER e SOTC_HDR_GAIN em "Variáveis extras".',
                     'HDR10 output from the game image. Needs HDR on in Windows and an HDR monitor; otherwise '
                     'nothing changes. Tuning: SOTC_HDR_PAPER and SOTC_HDR_GAIN in "Extra variables".'))

    def build_controls(self):
        ttk = self.ttk
        f = self.scrolled_page('controls', _('Controles', 'Controls'),
                               _('Um controle (DualShock 4, DualSense, Xbox…) é usado automaticamente.',
                                 'A gamepad (DualShock 4, DualSense, Xbox…) is used automatically.'))
        self.section(f, _('Teste do controle', 'Controller test'), top=4)
        self.controller_card(f, PORT_DIR).grid(row=self.next_row(f), column=0, columnspan=2, sticky='we',
                                              pady=(self.px(4), self.px(6)))
        self.section(f, _('Controle', 'Gamepad'))
        self.row(f, _('Ícones dos botões', 'Button icons'), self.choice(f, 'button_icons', BUTTON_ICONS),
                 _('Os ícones que o jogo mostra nas dicas: os do PlayStation ou os do Xbox (A B X Y, LB/RB, '
                   'LT/RT, Menu, View), desenhados para o port. Vale na próxima vez que o jogo abrir.',
                   'The icons the game shows in its prompts: PlayStation ones or Xbox ones (A B X Y, LB/RB, '
                   'LT/RT, Menu, View), drawn for the port. Applies the next time the game starts.'))
        self.row(f, _('Controle preferido', 'Preferred gamepad'), ttk.Entry(f, textvariable=self.var('gamepad'), width=30),
                 _('Nome (ou parte) do controle a usar quando houver vários; vazio = o primeiro.',
                   'Name (or part of it) of the gamepad to use when several are connected; empty = the first.'))
        self.check(f, 'rumble', _('Vibração do controle', 'Controller vibration'),
                   _('Desmarque para o controle não vibrar. Vale na próxima vez que o jogo abrir.',
                     'Clear it to stop the gamepad from rumbling. Applies the next time the game starts.'))
        self.section(f, _('Mouse', 'Mouse'))
        self.check(f, 'mouse', _('Câmera no mouse', 'Mouse camera'),
                   _('Clique na janela do jogo para capturar o mouse; Esc solta. Botão esquerdo = R1 (agarrar), '
                     'direito = □ (atacar), meio = L1. Remapeie com mouse.<entrada>= no sotc.ini.',
                     'Click the game window to capture the mouse; Esc releases it. Left button = R1 (grip), right = □ '
                     '(attack), middle = L1. Remap with mouse.<input>= in sotc.ini.'))
        self.row(f, _('Sensibilidade', 'Sensitivity'), self.choice(f, 'mouse_sens', MOUSE_SENS, 20))
        self.check(f, 'mouse_invert', _('Inverter o eixo vertical', 'Invert the vertical axis'))
        self.section(f, _('Teclado (funciona junto com o controle)', 'Keyboard (works next to a gamepad)'))
        self.note(f, _('Clique na tecla de uma ação e aperte a nova tecla (Esc cancela). Vale na próxima vez que '
                       'o jogo abrir.',
                       'Click the key of an action and press the new key (Esc cancels). Applies the next time the '
                       'game starts.'), top=0)
        self.key_buttons = {}
        for name, default, label in KEY_INPUTS:
            r = self.next_row(f)
            ttk.Label(f, text=_(*label)).grid(row=r, column=0, sticky='w', padx=(0, 24), pady=1)
            button = ttk.Button(f, width=18, command=lambda n=name: self.capture_key(n))
            button.grid(row=r, column=1, sticky='w', pady=1)
            self.key_buttons[name] = button
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(8, 0))
        ttk.Button(holder, text=_('Restaurar as teclas padrão', 'Restore the default keys'),
                   command=self.reset_keys).pack(side='left')
        self.show_keys()
        self.note(f, _('O controle também pode ser remapeado no sotc.ini, por exemplo pad.circle=b '
                       '(nomes de botão do SDL).',
                       'The gamepad can be remapped in sotc.ini too, e.g. pad.circle=b (SDL button names).'))

    def show_keys(self):
        keys = self.app.setdefault('keys', {})
        for name, default, _label in KEY_INPUTS:
            value = (keys[name] or '—') if name in keys else default
            self.key_buttons[name].configure(text=value + (' *' if name in keys else ''))

    def reset_keys(self):
        self.app['keys'] = {}
        self.show_keys()

    def capture_key(self, name):
        """A small window that waits for the new key of one action."""
        tk, ttk = self.tk, self.ttk
        window = tk.Toplevel(self.root)
        window.title(_('Nova tecla', 'New key'))
        window.configure(bg=PANEL)
        window.transient(self.root)
        window.resizable(False, False)
        label = next(_(*l) for n, _d, l in KEY_INPUTS if n == name)
        ttk.Label(window, text=_('Aperte a tecla para: {}', 'Press the key for: {}').format(label),
                  font=(ui_font(), 12)).pack(padx=30, pady=(24, 6))
        hint = ttk.Label(window, text=_('Esc cancela', 'Esc cancels'), style='Muted.TLabel')
        hint.pack(padx=30, pady=(0, 22))

        def pressed(event):
            if event.keysym == 'Escape':
                window.destroy()
                return
            key = sdl_key_name(event.keysym)
            if not key:
                hint.configure(text=_('Tecla não suportada: {}', 'Unsupported key: {}').format(event.keysym))
                return
            keys = self.app.setdefault('keys', {})
            default = next(d for n, d, _l in KEY_INPUTS if n == name)
            if key == default:
                keys.pop(name, None)
            else:
                keys[name] = key
            # The key leaves any other action that had it (one action per key keeps it clear).
            for other, other_default, _l in KEY_INPUTS:
                if other != name and (keys[other] if other in keys else other_default) == key:
                    keys[other] = ''
            self.show_keys()
            window.destroy()
        window.bind('<KeyPress>', pressed)
        window.grab_set()
        window.focus_force()

    def build_debug(self):
        f = self.scrolled_page('debug', _('Debug', 'Debug'),
                               _('Ferramentas de teste. Valem a partir da próxima abertura do jogo.',
                                 'Test tools. They apply the next time the game starts.'))
        self.section(f, _('Menu de debug do jogo', "The game's debug menu"), top=4)
        self.check(f, 'debug_menu', _('Ligar o menu de debug (F6 ou Start+Select no jogo)', 'Enable the debug menu (F6 or Start+Select in game)'),
                   _('O menu de testes da Bluepoint: carregador de áreas, câmera livre, estatísticas e opções de '
                     'desenvolvedor. Os arquivos do jogo não são alterados. O jogo pode ficar menos estável com '
                     'ele ligado: desligue para jogar normalmente.',
                     "Bluepoint's test menu: area loader, free camera, statistics and developer options. The "
                     'game files are not changed. The game can be less stable with it on: turn it off to play '
                     'normally.'))
        self.check(f, 'skip_logos', _('Pular os logotipos de abertura', 'Skip the opening logos'),
                   _('Sem os logotipos (Sony, Japan Studio, Bluepoint) e o aviso de salvamento automático.',
                     'No logos (Sony, Japan Studio, Bluepoint) and no autosave notice.'))
        self.check(f, 'skip_film', _('Ir direto para o jogo (pula o filme e a tela Start)',
                                     'Straight into the game (skips the film and the Start screen)'),
                   _('A chave de desenvolvedor do jogo (+nointro): sem o filme e sem a tela de título. Para só '
                     'pular o filme e parar na tela Start, deixe desligada e aperte um botão durante o filme.',
                     "The game's developer switch (+nointro): no film and no title screen. To skip only the film "
                     'and stop at the Start screen, leave it off and press a button during the film.'))
        self.section(f, _('Diagnóstico', 'Diagnostics'))
        self.check(f, 'debug_watch', _('Vigia da memória de texturas', 'Texture memory watch'),
                   _('Anota em user/events.log quando o jogo reaproveita memória de texturas ainda em uso. '
                     'Útil para relatar texturas com lixo colorido.',
                     'Writes to user/events.log when the game reuses memory of textures still in use. Useful '
                     'when reporting colour garbage on textures.'))
        self.section(f, _('Teclas no jogo', 'In-game keys'))
        for key, text in (('F5', _('Salva as texturas do quadro atual em user/log/texcap_N',
                                    'Saves the textures of the current frame to user/log/texcap_N')),
                          ('F6 / Start+Select', _('Abre o menu de debug (com a opção acima ligada)',
                                   'Opens the debug menu (with the option above on)')),
                          ('F9', _('Recarrega todas as texturas da memória do jogo (com "Comparar DLSS" ligado: troca DLSS / FSR 1)',
                                   'Reloads every texture from game memory (with "Compare DLSS" on: switches DLSS / FSR 1)'))):
            self.row(f, key, self.ttk.Label(f, text=text))

    def build_advanced(self):
        ttk = self.ttk
        f = self.scrolled_page('advanced', _('Avançado', 'Advanced'),
                               _('Opções do lançador e diagnóstico.', 'Launcher options and diagnostics.'))
        self.section(f, _('Lançador', 'Launcher'), top=4)
        self.row(f, _('Idioma do lançador', 'Launcher language'), self.choice(f, 'ui_language', UI_LANGUAGES),
                 _('Vale na próxima vez que o lançador abrir.', 'Applies when the launcher opens again.'))
        self.check(f, 'close_on_play', _('Fechar o lançador quando o jogo abrir', 'Close the launcher when the game starts'))
        self.check(f, 'minimize_to_tray', _('Minimizar para a bandeja do Windows', 'Minimize to the system tray'),
                   _('Minimizado, o lançador sai da barra de tarefas e fica como um ícone perto do relógio: um clique '
                     'o traz de volta, o botão direito tem Abrir e Sair.',
                     'Minimized, the launcher leaves the task bar for an icon by the clock: a click brings it back, the '
                     'right button has Open and Quit.'))
        self.check(f, 'check_updates', _('Procurar atualizações ao abrir', 'Check for updates at startup'),
                   _('Pergunta antes de baixar. Saves, cache de shaders e sotc.ini são mantidos.',
                     'Asks before downloading. Saves, shader cache and sotc.ini are kept.'))
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(10, 0))
        ttk.Button(holder, text=_('Atalho na área de trabalho', 'Desktop shortcut'), command=self.shortcut).pack(side='left')
        ttk.Button(holder, text=_('Atalho para jogar direto', 'Shortcut that starts the game'),
                   command=lambda: self.shortcut(play=True)).pack(side='left', padx=(6, 0))
        # A second row: on a narrow window (a laptop) the fourth button fell off the window's edge.
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(6, 0))
        ttk.Button(holder, text=_('Pasta do port', 'Port folder'), command=lambda: self.open_path(PORT_DIR)).pack(side='left')
        ttk.Button(holder, text=_('Limpar cache de shaders', 'Clear shader cache'), command=self.clear_cache).pack(side='left', padx=(6, 0))
        self.section(f, _('Diagnóstico', 'Diagnostics'))
        self.check(f, 'show_fps', _('FPS no registro (a cada 5 s)', 'FPS in the log (every 5 s)'))
        self.check(f, 'dlss_compare', _('Comparar DLSS com FSR 1 (F9)', 'Compare DLSS with FSR 1 (F9)'),
                   _('Com o upscaler DLSS: F9 no jogo troca entre DLSS e FSR 1, e o upscaler em uso aparece no '
                     'canto superior direito.',
                     'With the DLSS upscaler: F9 in game switches between DLSS and FSR 1, and the upscaler in '
                     'use shows in the top right corner.'))
        self.row(f, _('Leituras da GPU', 'GPU readbacks'), self.choice(f, 'readbacks', READBACKS))
        self.row(f, _('Cvars do motor', 'Engine cvars'), ttk.Entry(f, textvariable=self.var('engine_cvars'), width=58),
                 _('Avançado: NOME=valor separados por espaço, acrescentados à linha de comando do jogo.',
                   "Advanced: NAME=value separated by spaces, added to the game's command line."))
        self.row(f, _('Variáveis extras', 'Extra variables'), ttk.Entry(f, textvariable=self.var('extra_env'), width=58),
                 _('Pares NOME=valor separados por espaço (veja o README).',
                   'NAME=value pairs separated by spaces (see the README).'))

    def build_log(self):
        tk, ttk = self.tk, self.ttk
        page = ttk.Frame(self.content, padding=(20, 14, 20, 8))
        self.pages['log'] = page
        top = ttk.Frame(page)
        top.pack(fill='x', pady=(0, 8))
        ttk.Label(top, text=_('Registro', 'Log'), font=(title_font(), 20)).pack(side='left')
        ttk.Button(top, text=_('Copiar', 'Copy'), command=self.copy_log).pack(side='right')
        ttk.Button(top, text=_('Limpar', 'Clear'), command=lambda: self.set_log('')).pack(side='right', padx=6)
        self.log = tk.Text(page, wrap='none', bg='#090a0b', fg='#c9ccd0', insertbackground=TEXT, relief='flat',
                           font=('Consolas', 9), padx=8, pady=6, state='disabled', highlightthickness=0)
        bar = ttk.Scrollbar(page, command=self.log.yview)
        self.log.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y')
        self.log.pack(fill='both', expand=True)

    # ---- state -------------------------------------------------------------------------------
    def game_changed(self):
        game = self.var('game_dir').get()
        if game_info(game) and not self.var('dlc_dir').get():
            from sotc_discovery import find_dlc_root
            dlc = find_dlc_root(game)
            if dlc:
                self.var('dlc_dir').set(dlc)
        self.banner_source = None
        self.draw_banner()
        self.set_icon()
        self.refresh_status()

    def refresh_status(self):
        info = game_info(self.var('game_dir').get())
        if not info:
            game = _('✗ Não há eboot.bin na pasta do jogo (Jogo e saves)', '✗ No eboot.bin in the game folder (Game & saves)')
        elif game_supported(self.var('game_dir').get()):
            game = _('✓ {} versão {}: todos os modos disponíveis', '✓ {} version {}: every mode available').format(*info)
        else:
            game = _('⚠ {} versão {}: o port foi feito para {}; os modos não se aplicam',
                     '⚠ {} version {}: the port targets {}; the modes do not apply').format(*info, supported_games())
        modules = Path(self.var('sys_modules').get() or '.')
        if self.var('sys_modules').get() and all((modules / m).is_file() for m in SYS_MODULES):
            sound = _('✓ Módulos de som encontrados', '✓ Sound modules found')
        else:
            sound = _('⚠ Sem os módulos de som do seu console: o jogo fica mudo (Jogo e saves)',
                      '⚠ No sound modules from your console: the game is silent (Game & saves)')
        user = user_dir({'user_dir': self.var('user_dir').get()})
        serial = game_serial(self.var('game_dir').get())
        saves = user / 'savedata' / '1' / serial
        save = (_('✓ Saves em {}', '✓ Saves in {}').format(saves) if saves.is_dir() and any(saves.iterdir())
                else _('• Ainda sem saves: o jogo cria em {}', '• No saves yet: the game creates them in {}').format(saves))
        from sotc_discovery import dlc_count
        count = dlc_count(self.var('dlc_dir').get(), serial)
        dlc = (_('✓ DLC: {} instalada(s)', '✓ DLC: {} installed').format(count) if count
               else _('• Sem DLC', '• No DLC'))
        for key, text in (('game', game), ('dlc', dlc), ('sound', sound), ('saves', save), ('gpu', self.gpu_text)):
            self.checks[key].configure(text=text, foreground=MUTED if text.startswith('•') else
                                       '#d9b441' if text.startswith('⚠') else '#d36b5c' if text.startswith('✗') else TEXT)
        if not self.process:
            self.play_button.configure(state='normal' if info else 'disabled')
            self.status.configure(text=self.summary() if info else _('Escolha primeiro a pasta do jogo.',
                                                                     'Choose the game folder first.'), fg=MUTED)

    def summary(self):
        parts = [dict((k, _(*v)) for k, v in FPS_MODES).get(self.var('fps_mode').get(), '30 FPS'),
                 self.var('output_res').get().replace('x', ' × '),
                 {'fsr1': 'FSR 1', 'dlss': 'DLSS', 'fsr4': 'FSR 4'}.get(self.var('upscaler').get(), 'bilinear')]
        framegen = self.var('framegen').get()
        if framegen != 'off':
            parts.append({'auto': _('geração de quadros', 'frame generation'), 'dlss': 'DLSS-G',
                          'fsr': 'FSR 3'}.get(framegen, framegen))
        if self.var('af16').get():
            parts.append(_('AF 16x', '16x AF'))
        if self.var('hdr').get():
            parts.append('HDR')
        if self.var('neo').get():
            parts.append('PS4 Pro' if not is_wide(self.var('output_res').get())
                         else _('PS4 Pro desligado (tela larga)', 'PS4 Pro off (wide screen)'))
        parts.append(_('tela cheia', 'fullscreen') if self.var('fullscreen').get() else _('janela', 'window'))
        return '   ·   '.join(parts)

    def detect_gpu(self):
        exe = PORT_DIR / 'bin' / 'bb-gpu-capabilities.exe'
        if not exe.is_file():
            exe = PORT_DIR / 'out' / 'bb-gpu-capabilities.exe'
        env = dict(os.environ)
        if not (exe.parent / 'SDL3.dll').is_file():
            clang64 = Path(os.environ.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
        text = _('• Placa de vídeo: não verificada', '• Graphics card: not checked')
        try:
            result = subprocess.run([str(exe), '--live-resolution'], capture_output=True, text=True, timeout=30,
                                    env=env, creationflags=NO_WINDOW)
            names = [line[5:].split(':')[0] for line in result.stderr.splitlines() if line.startswith('GPU: ')]
            if names:
                self.gpu_name = names[0]
                found = re.search(r'(\d+)\s*MiB', names[0])
                if found:
                    self.app['gpu_vram_mb'] = int(found.group(1))
                    self.ui_calls.put(self.show_vram_profile_hint)
                text = _('✓ Placa de vídeo: {}', '✓ Graphics card: {}').format(names[0])
            elif result.returncode:
                text = _('✗ Nenhuma placa com Vulkan 1.3 (atualize o driver)',
                         '✗ No Vulkan 1.3 graphics card found (update the driver)')
        except (OSError, subprocess.TimeoutExpired):
            pass
        self.gpu_text = text
        self.ui_calls.put(self.refresh_status)

    def restart(self):
        """Opens the launcher again (after a language change) and closes this one."""
        command = [sys.executable] if FROZEN else [sys.executable, str(Path(__file__).resolve())]
        try:
            subprocess.Popen(command + [a for a in sys.argv[1:] if a != '--play'], cwd=str(PORT_DIR))
        except OSError:
            return
        self.root.destroy()

    def collect(self):
        for key, var in self.vars.items():
            self.app[key] = var.get()
        text = json.dumps(self.app, indent=2, ensure_ascii=False)
        try:
            CONFIG_DIR.mkdir(parents=True, exist_ok=True)
            CONFIG_FILE.write_text(text, encoding='utf-8')
        except OSError:  # a read-only port folder (Program Files): the shared settings
            LEGACY_CONFIG_DIR.mkdir(parents=True, exist_ok=True)
            LEGACY_CONFIG_FILE.write_text(text, encoding='utf-8')

    def import_save(self):
        serial = game_serial(self.var('game_dir').get())
        found = shadps4_saves(serial)
        initial = str(found[0]) if found else str(PORT_DIR.parent)
        chosen = self.filedialog.askdirectory(title=_('Pasta savedata/{} do shadPS4', "shadPS4's savedata/{} folder").format(serial),
                                              initialdir=initial)
        if not chosen:
            return
        source = Path(chosen)
        # A savedata folder: its subfolder of this game, else of the other release (EU and US
        # saves are of the same game build). The save goes to this game's folder.
        if source.name not in GAMES:
            source = next((source / s for s in (serial, *GAMES) if (source / s).is_dir()), source)
        target = user_dir({'user_dir': self.var('user_dir').get()}) / 'savedata' / '1' / serial
        # sotc: one save folder picked (it holds sce_sys/param.sfo): import it as one save. The
        # runtime creates the port's metadata for saves that come without it.
        single = (source / 'sce_sys' / 'param.sfo').is_file()
        try:
            if not single and target.exists() and any(target.iterdir()):
                if not self.messagebox.askyesno('Shadow of the Colossus', _(
                        'Já há saves em {}. Substituir? (uma cópia fica em {}.bak)',
                        'There are saves in {} already. Replace them? (a copy stays in {}.bak)').format(target, target.name)):
                    return
                backup = target.with_name(target.name + '.bak')
                shutil.rmtree(backup, ignore_errors=True)
                target.rename(backup)
            target.mkdir(parents=True, exist_ok=True)
            shutil.copytree(source, target / source.name if single else target, dirs_exist_ok=True)
        except OSError as error:
            self.messagebox.showerror('Shadow of the Colossus', _('Não foi possível importar o save: {}',
                                                                 'Could not import the save: {}').format(error))
            return
        self.refresh_status()
        self.messagebox.showinfo('Shadow of the Colossus', _('Save importado para {}.', 'Save imported to {}.').format(target))

    def build_help(self):
        """The Help page: asking for help (the report, the issues page), then the problems any
        installation can run into, each with what to do and buttons that lead there. After the
        bloodborne_pc launcher's Help page."""
        ttk = self.ttk
        f = self.scrolled_page('help', _('Ajuda', 'Help'),
                               _('Os problemas mais comuns e o que fazer. A busca (Ctrl+F) também acha estes tópicos.',
                                 'The most common problems and what to do. The search (Ctrl+F) finds these topics too.'))
        issues = 'https://github.com/AjncSystem/ShadowOfTheColossus-PC/issues'
        topics = [
            (_('Pedir ajuda', 'Asking for help'),
             _('Gere um relatório (registros das últimas sessões, quedas e um resumo do PC) e anexe numa issue no '
               'GitHub, dizendo o que aconteceu e onde no jogo.',
               'Make a report (the last sessions\' logs, crashes and a summary of the PC) and attach it to a GitHub '
               'issue, saying what happened and where in the game.'),
             [(_('Gerar relatório', 'Make a report'), self.save_report, True),
              (_('Abrir as issues', 'Open the issues'), lambda: self.open_url(issues), False)]),
            (_('O jogo não abre ou fecha logo ao abrir', 'The game does not start or closes at once'),
             _('Veja a Verificação na aba Jogar: o dump precisa estar decriptado (eboot.bin e sce_module), na versão '
               'EU 1.01 ou US 1.00. Atualize o driver de vídeo (Vulkan 1.3). Um antivírus pode barrar bin\\bb-probe.exe: '
               'ponha a pasta do port nas exceções. O registro do jogo mostra o motivo da queda.',
               'See the ready check on the Play page: the dump must be decrypted (eboot.bin and sce_module), version '
               'EU 1.01 or US 1.00. Update the graphics driver (Vulkan 1.3). An antivirus can block bin\\bb-probe.exe: '
               'add the port folder to its exceptions. The game log tells why it closed.'),
             [(_('Registro do jogo', 'Game log'), lambda: self.show('log'), False),
              (_('Verificação', 'Ready check'), lambda: self.show('play'), False)]),
            (_('Tela preta ou imagem congelada', 'Black screen or frozen picture'),
             _('Teste com a geração de quadros desligada e o upscaler FSR 1. Atualize o driver de vídeo. Se começou '
               'depois de atualizar o driver ou o port, limpe o cache de shaders (o jogo o refaz na próxima abertura, '
               'com algumas travadas no começo).',
               'Try with frame generation off and the FSR 1 upscaler. Update the graphics driver. If it began after a '
               'driver or port update, clear the shader cache (the game builds it again next time, with a few stutters '
               'at first).'),
             [(_('Geração de quadros', 'Frame generation'), lambda: self.jump_key('framegen'), False),
              (_('Limpar cache de shaders', 'Clear shader cache'), self.clear_cache, False)]),
            (_('Sem som', 'No sound'),
             _('O som do jogo precisa dos módulos libSceNgs2 e libSceUlt do seu próprio console (o port não os traz). '
               'Escolha a pasta onde eles estão; a do shadPS4 serve.',
               'The game\'s sound needs the libSceNgs2 and libSceUlt modules of your own console (the port does not '
               'bring them). Choose the folder that holds them; shadPS4\'s works.'),
             [(_('Módulos de som', 'Sound modules'), lambda: self.jump_key('sys_modules'), False)]),
            (_('Travadas nas trocas de área ou na primeira vez', 'Stutters at area changes or the first time'),
             _('Na primeira vez (e depois de atualizar) o cache de shaders é montado: as travadas somem com o tempo. '
               'Instale o jogo num SSD ou NVMe: num HD as trocas de área travam muito mais.',
               'The first time (and after an update) the shader cache is built: the stutters fade with play. Install '
               'the game on an SSD or NVMe drive: on a hard disk area changes stutter far more.'), []),
            (_('FPS baixo', 'Low frame rate'),
             _('Escolha o Modo de jogo Normal ou Pro, ou 30 FPS. Feche programas por cima do jogo (OBS, RTSS, '
               'Afterburner, gravação do Windows). Em placas com pouca memória, use "Uso de memória de vídeo" Econômico.',
               'Choose the Normal or Pro play mode, or 30 FPS. Close programs over the game (OBS, RTSS, Afterburner, '
               'Windows recording). On cards with little memory, set "Video memory use" to Economy.'),
             [(_('Modo de jogo', 'Play mode'), lambda: self.jump_key('play_mode'), False),
              (_('Memória de vídeo', 'Video memory'), lambda: self.jump_key('vram_use'), False)]),
            (_('HUD, letras ou barras de vida tremendo', 'HUD, letters or health bars shaking'),
             _('Com a geração de quadros (F8), ligue "Geração de quadros sem a interface": os quadros gerados ficam '
               'só com a cena e a interface vai por cima.',
               'With frame generation (F8), turn "Frame generation without the interface" on: the generated frames '
               'hold the scene only and the interface goes on top.'),
             [(_('Abrir a opção', 'Open the setting'), lambda: self.jump_key('fg_hudless'), False)]),
            (_('Texturas coloridas (mosaico) ou borradas', 'Coloured (mosaic) or blurry textures'),
             _('O mosaico depois das cargas foi corrigido na 0.71. Texturas borradas: use Texturas Alta ou Máxima e o '
               'filtro 16x. Se o mosaico voltar, gere um relatório logo depois.',
               'The mosaic after loads was fixed in 0.71. Blurry textures: use High or Maximum textures and 16x '
               'filtering. If the mosaic comes back, make a report right after.'),
             [(_('Texturas', 'Textures'), lambda: self.jump_key('textures'), False)]),
            (_('O controle não responde', 'The controller does not respond'),
             _('Conecte o controle antes de abrir o jogo. Com vários controles, escolha o preferido na aba Controles. '
               'Pela Steam, desligue o Steam Input para o atalho do jogo.',
               'Connect the controller before starting the game. With several, choose the preferred one on the '
               'Controls page. Through Steam, turn Steam Input off for the game\'s shortcut.'),
             [(_('Controles', 'Controls'), lambda: self.show('controls'), False)]),
            (_('Um save sumiu ou estragou, ou quero levá-lo a outro PC', 'A save is gone or broken, or I want it on another PC'),
             _('A aba Saves guarda um backup a cada vez que o jogo fecha: importe o .zip de antes do problema. Para '
               'outro PC, exporte os saves e importe lá.',
               'The Saves page keeps a backup every time the game closes: import the .zip from before the problem. '
               'For another PC, export the saves and import them there.'),
             [(_('Saves', 'Saves'), lambda: self.show('saves'), False)]),
            (_('Atualizar o port', 'Updating the port'),
             _('O launcher procura versões novas ao abrir. Saves, cache de shaders e sotc.ini são mantidos.',
               'The launcher looks for new versions when it starts. Saves, shader cache and sotc.ini are kept.'),
             [(_('Procurar atualização', 'Check for updates'), lambda: self.check_updates(True), False)]),
        ]
        f.columnconfigure(0, weight=1)
        for k, (title, text, actions) in enumerate(topics):
            card = ttk.Frame(f, style='Card.TFrame', padding=(self.px(18), self.px(14)))
            card.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(self.px(4) if k else 0, self.px(6)))
            card.columnconfigure(0, weight=1)
            head = ttk.Label(card, text=title, style='Card.Section.TLabel')
            head.grid(row=0, column=0, sticky='w')
            ttk.Label(card, text=text, style='Card.TLabel', wraplength=self.px(720), justify='left').grid(
                row=1, column=0, sticky='w', pady=(self.px(4), 0))
            if actions:
                buttons = ttk.Frame(card, style='Inner.TFrame')
                buttons.grid(row=2, column=0, sticky='w', pady=(self.px(10), 0))
                for label, command, accent in actions:
                    ttk.Button(buttons, text=label, command=command,
                               style='Card.Accent.TButton' if accent else 'Card.TButton').pack(side='left', padx=(0, self.px(6)))
            self.index_entry(head, title, text, page='help')

    def jump_key(self, key):
        """The page of a setting (not the Play page's copy), scrolled to it and marked."""
        entries = [e for e in getattr(self, 'search_index', []) if e.get('key') == key]
        entry = next((e for e in entries if e['page'] != 'play'), entries[0] if entries else None)
        if entry:
            self.jump(entry)

    def open_url(self, url):
        import webbrowser
        webbrowser.open(url)

    def build_saves(self):
        """The Saves page: the game's saves with their progress, export and import (.zip, a folder,
        shadPS4), backups, the data folder."""
        ttk = self.ttk
        f = self.scrolled_page('saves', _('Saves', 'Saves'),
                               _('Os seus jogos salvos: exportar para guardar ou levar a outro PC, importar, fazer backup.',
                                 'Your saved games: export to keep them or take them to another PC, import, back up.'))
        self.section(f, _('Seus saves', 'Your saves'), top=4)
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(6, 0))
        self.save_list = ttk.Treeview(holder, columns=('detail', 'date'), height=7, style='Library.Treeview',
                                      selectmode='extended')
        for column, title, width in (('#0', _('Save', 'Save'), 430), ('detail', _('Tempo de jogo', 'Play time'), 260),
                                     ('date', _('Gravado em', 'Saved'), 130)):
            self.save_list.heading(column, text=title, anchor='w')
            self.save_list.column(column, width=self.px(width), anchor='w', stretch=column == '#0')
        self.save_list.pack(side='left', fill='x', expand=True)
        self.save_label = ttk.Label(f, text='', style='Muted.TLabel')
        self.save_label.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(4, 0))
        buttons = ttk.Frame(f)
        buttons.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(8, 0))
        ttk.Button(buttons, text=_('Exportar selecionados…', 'Export selected…'),
                   command=lambda: self.export_saves(selected=True)).pack(side='left')
        ttk.Button(buttons, text=_('Exportar todos…', 'Export all…'),
                   command=lambda: self.export_saves(selected=False)).pack(side='left', padx=(6, 0))
        ttk.Button(buttons, text=_('Atualizar lista', 'Refresh'), command=self.refresh_saves).pack(side='left', padx=(6, 0))
        ttk.Button(buttons, text=_('Abrir pasta', 'Open folder'),
                   command=lambda: self.open_path(self.saves_folder() if self.saves_folder().is_dir()
                                                  else user_dir({'user_dir': self.var('user_dir').get()}))).pack(
            side='left', padx=(6, 0))

        self.section(f, _('Importar', 'Import'))
        self.note(f, _('Os saves com o mesmo nome são substituídos; antes disso o launcher faz um backup de todos.',
                       'Saves with the same name are replaced; the launcher backs them all up first.'), top=0)
        buttons = ttk.Frame(f)
        buttons.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(8, 0))
        ttk.Button(buttons, text=_('Importar .zip…', 'Import a .zip…'), command=self.import_saves_zip).pack(side='left')
        ttk.Button(buttons, text=_('Importar pasta…', 'Import a folder…'),
                   command=self.import_saves_folder).pack(side='left', padx=(6, 0))
        ttk.Button(buttons, text=_('Importar do shadPS4…', 'Import from shadPS4…'),
                   command=self.import_save).pack(side='left', padx=(6, 0))
        self.note(f, _('.zip: um arquivo exportado por este launcher. Pasta: um save (com sce_sys/param.sfo), a pasta '
                       'savedata/CUSA08809 de outra instalação ou um backup. shadPS4: a pasta savedata do shadPS4.',
                       '.zip: a file this launcher exported. Folder: one save (with sce_sys/param.sfo), the '
                       'savedata/CUSA08809 folder of another install or a backup. shadPS4: shadPS4\'s savedata folder.'))

        self.section(f, _('Backup', 'Backup'))
        buttons = ttk.Frame(f)
        buttons.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(4, 0))
        ttk.Button(buttons, text=_('Fazer backup agora', 'Back up now'), style='Accent.TButton',
                   command=lambda: self.backup_saves(notify=True)).pack(side='left')
        ttk.Button(buttons, text=_('Abrir pasta de backups', 'Open the backups folder'),
                   command=lambda: self.open_path(self.backups_folder())).pack(side='left', padx=(6, 0))
        self.note(f, _('Um .zip com todos os saves em user/backups (os {} mais novos ficam). Para restaurar, use '
                       '"Importar .zip…".', 'A .zip of every save in user/backups (the {} newest are kept). To '
                       'restore one, use "Import a .zip…".').format(SAVE_BACKUPS_KEPT))
        self.check(f, 'exit_backup', _('Backup automático quando o jogo fecha', 'Automatic backup when the game closes'),
                   _('Depois de cada sessão, também quando o jogo cai: um save estragado por uma queda volta pelo '
                     'backup anterior.', 'After every session, also when the game crashes: a save a crash spoiled '
                     'comes back from the backup before.'))

        self.section(f, _('Pasta de dados', 'Data folder'))
        self.folder(f, 'user_dir', _('Pasta de dados', 'Data folder'), _('Pasta dos saves', 'Saves folder'),
                    _('Saves, cache de shaders e capturas. Vazio = a pasta user ao lado do port.',
                      'Saves, shader cache and screenshots. Empty = the user folder next to the port.'),
                    on_change=lambda: (self.refresh_status(), self.refresh_saves()))
        self.refresh_saves()

    def save_entries(self):
        """The game's saves: (folder name, title, play time, saved at) newest first."""
        folder = self.saves_folder()
        entries = []
        try:
            from prepare import sfo
        except ImportError:
            sfo = None
        for path in (folder.iterdir() if folder.is_dir() else ()):
            name = path.name
            if not path.is_dir() or name.startswith('.') or name.endswith(('.sce_sys', '.sce_backup', '.bak')):
                continue
            values = save_param(folder / (name + '.sce_sys') / 'param.bin')
            if not values.get('SUBTITLE'):
                try:
                    values = sfo((path / 'sce_sys' / 'param.sfo').read_bytes()) if sfo else {}
                except (OSError, ValueError):
                    pass
            title = str(values.get('SUBTITLE') or values.get('MAINTITLE') or name)
            stamp = max((p.stat().st_mtime for p in path.rglob('*') if p.is_file()), default=path.stat().st_mtime)
            detail = ' · '.join(line.strip() for line in str(values.get('DETAIL') or '').splitlines() if line.strip())
            entries.append((name, title, detail, stamp))
        return sorted(entries, key=lambda e: -e[3])

    def refresh_saves(self):
        if not getattr(self, 'save_list', None):
            return
        self.save_list.delete(*self.save_list.get_children())
        entries = self.save_entries()
        for name, title, detail, stamp in entries:
            self.save_list.insert('', 'end', iid=name, text='  ' + title,
                                  values=(detail, time.strftime('%d/%m/%Y %H:%M', time.localtime(stamp))))
        self.save_label.configure(text=_('{} save(s) em {}', '{} save(s) in {}').format(len(entries), self.saves_folder())
                                  if entries else _('Nenhum save ainda em {}', 'No saves yet in {}').format(self.saves_folder()))

    def save_parts(self, name):
        """A save's folder and the port's folders beside it (<name>.sce_sys, <name>.sce_backup)."""
        folder = self.saves_folder()
        return [folder / n for n in (name, name + '.sce_sys', name + '.sce_backup') if (folder / n).exists()]

    def write_saves_zip(self, target, names):
        """`names` with their port folders into the .zip `target`, below <title id>/."""
        serial = self.saves_folder().name
        with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as archive:
            for name in names:
                for part in self.save_parts(name):
                    for file in part.rglob('*'):
                        if file.is_file():
                            archive.write(file, f'{serial}/{file.relative_to(part.parent).as_posix()}')

    def export_saves(self, selected):
        names = list(self.save_list.selection()) if selected else [e[0] for e in self.save_entries()]
        if not names:
            self.messagebox.showinfo('Shadow of the Colossus', _('Escolha um ou mais saves na lista.',
                                                                 'Choose one or more saves in the list.'))
            return
        default = f'SotC-saves-{time.strftime("%Y-%m-%d")}.zip' if len(names) > 1 else f'SotC-{names[0]}.zip'
        target = self.filedialog.asksaveasfilename(title=_('Exportar saves', 'Export saves'), defaultextension='.zip',
                                                   initialfile=default, filetypes=[('ZIP', '*.zip')])
        if not target:
            return
        try:
            self.write_saves_zip(target, names)
        except OSError as error:
            self.messagebox.showerror('Shadow of the Colossus', _('Não foi possível exportar: {}',
                                                                 'Could not export: {}').format(error))
            return
        self.messagebox.showinfo('Shadow of the Colossus', _('{} save(s) exportado(s) para\n{}',
                                                             '{} save(s) exported to\n{}').format(len(names), target))

    def backups_folder(self):
        folder = user_dir({'user_dir': self.var('user_dir').get()}) / 'backups'
        folder.mkdir(parents=True, exist_ok=True)
        return folder

    def news_items(self):
        """This version's entry of CHANGELOG.md: one text per item (its continuation lines joined),
        **bold** kept as markers."""
        try:
            text = (PORT_DIR / 'CHANGELOG.md').read_text(encoding='utf-8')
        except OSError:
            return []
        items, inside = [], False
        for line in text.splitlines():
            if line.startswith('## '):
                if inside:
                    break
                inside = line[3:].split(' ')[0] == VERSION
                continue
            if not inside or not line.strip():
                continue
            if line.startswith('- '):
                items.append(line[2:].strip())
            elif items:
                items[-1] += ' ' + line.strip()
        return [item.replace('`', '') for item in items]

    def show_news(self):
        """What's new, once per version (news_seen)."""
        tk, ttk = self.tk, self.ttk
        items = self.news_items()
        self.var('news_seen').set(VERSION)
        self.collect()
        if not items:
            return
        win = tk.Toplevel(self.root, bg=PANEL)
        win.title(_('Novidades', "What's new"))
        win.transient(self.root)
        win.geometry(f'{self.px(720)}x{self.px(560)}')
        ttk.Label(win, text=_('Novidades da v{}', "What's new in v{}").format(VERSION), background=PANEL, foreground=GOLD,
                  font=(title_font(), 20)).pack(anchor='w', padx=self.px(24), pady=(self.px(20), self.px(4)))
        tk.Frame(win, bg=LINE, height=1).pack(fill='x', padx=self.px(24), pady=(0, self.px(8)))
        bar = ttk.Frame(win)
        bar.pack(side='bottom', fill='x', padx=self.px(24), pady=self.px(14))
        ttk.Button(bar, text=_('Continuar', 'Continue'), style='Accent.TButton', command=win.destroy).pack(side='right')
        holder = ttk.Frame(win)
        holder.pack(fill='both', expand=True, padx=(self.px(24), self.px(8)))
        body = tk.Text(holder, wrap='word', bg=PANEL, fg=TEXT, relief='flat', bd=0, highlightthickness=0, height=8,
                       font=(ui_font(), 10), spacing1=self.px(6), spacing3=self.px(2), cursor='arrow')
        scroll = ttk.Scrollbar(holder, orient='vertical', command=body.yview)
        body.configure(yscrollcommand=scroll.set)
        scroll.pack(side='right', fill='y')
        body.pack(side='left', fill='both', expand=True)
        body.tag_configure('item', lmargin1=0, lmargin2=self.px(18))
        body.tag_configure('bold', font=(theme.BODY_SEMI if LANG not in ('zh', 'ko') else ui_font(), 10),
                           foreground=theme.TEXT_STRONG)
        body.tag_configure('mark', foreground=SIGIL)
        for item in items:
            body.insert('end', '•  ', ('item', 'mark'))
            for k, part in enumerate(item.split('**')):
                body.insert('end', part, ('item', 'bold') if k % 2 else ('item',))
            body.insert('end', '\n', 'item')
        body.configure(state='disabled')
        win.after(80, lambda: body.yview_moveto(0))  # once the text is laid out at the window's width
        body.bind('<MouseWheel>', lambda e: (body.yview_scroll(int(-e.delta / 120), 'units'), 'break')[1])
        win.bind('<Escape>', lambda _e: win.destroy())
        win.grab_set()

    def save_report(self):
        """A .zip to attach to an issue: the last sessions' logs and events, the newest crashes with
        their dump, the launcher settings (the player name left out) and a summary of this PC."""
        default = Path.home() / 'Desktop' / f'SotC-relatorio-{time.strftime("%Y-%m-%d_%H-%M")}.zip'
        target = self.filedialog.asksaveasfilename(
            title=_('Salvar relatório', 'Save report'), defaultextension='.zip', filetypes=[('ZIP', '*.zip')],
            initialdir=str(default.parent if default.parent.is_dir() else Path.home()), initialfile=default.name)
        if not target:
            return
        self.collect()
        data = user_dir(self.app)
        files = [data / n for n in ('last_run.log', 'last_run.1.log', 'events.log', 'events.1.log')]
        crashes = data / 'crashes'
        if crashes.is_dir():
            logs = sorted(crashes.glob('*.log'), key=lambda p: p.stat().st_mtime)[-6:]
            dumps = sorted(crashes.glob('*.dmp'), key=lambda p: p.stat().st_mtime)[-1:]
            files += logs + dumps
        settings = dict(self.app, player_name='')
        info = game_info(self.app.get('game_dir'))
        summary = '\n'.join([
            f'SotC-PC launcher {VERSION}',
            f'Windows {platform.version()} ({platform.machine()})',
            f'CPU: {platform.processor()}, {os.cpu_count()} threads',
            self.gpu_text.lstrip('✓✗• '),
            'Game: ' + ('{} {}'.format(*info) if info else 'not set'),
            'Summary: ' + self.summary(),
        ])
        try:
            with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as archive:
                archive.writestr('report.txt', summary + '\n')
                archive.writestr('launcher_settings.json', json.dumps(settings, indent=2, ensure_ascii=False))
                for file in files:
                    if file.is_file():
                        archive.write(file, file.relative_to(data).as_posix())
        except OSError as error:
            self.messagebox.showerror('Shadow of the Colossus', _('Não foi possível salvar o relatório: {}',
                                                                 'Could not save the report: {}').format(error))
            return
        self.messagebox.showinfo('Shadow of the Colossus', _(
            'Relatório salvo em\n{}\n\nAnexe este arquivo na issue do GitHub. Ele tem os registros das últimas '
            'sessões e as pastas do jogo e do port (sem o nome do jogador).',
            'Report saved to\n{}\n\nAttach this file to the GitHub issue. It holds the last sessions\' logs and '
            'the game and port folders (without the player name).').format(target))
        self.open_path(Path(target).parent)

    def exit_backup(self):
        """After the game closed (a crash too): the saves into user/backups (Saves > Backup when the
        game closes), the newest SAVE_BACKUPS_KEPT kept as for the manual ones."""
        if not self.var('exit_backup').get():
            return
        try:
            target = self.backup_saves(suffix='-exit')
        except OSError as error:
            self.append(_('Backup dos saves ao fechar falhou: {}', 'Saves backup on exit failed: {}').format(error) + '\n')
            return
        if target:
            self.append(_('Backup dos saves: {}', 'Saves backup: {}').format(target) + '\n')
        self.refresh_saves()

    def backup_saves(self, notify=False, suffix=''):
        """Every save into user/backups/saves-<title id>-<date>.zip; the newest SAVE_BACKUPS_KEPT stay."""
        names = [e[0] for e in self.save_entries()]
        if not names:
            if notify:
                self.messagebox.showinfo('Shadow of the Colossus', _('Ainda não há saves.', 'There are no saves yet.'))
            return None
        folder = self.backups_folder()
        target = folder / f'saves-{self.saves_folder().name}-{time.strftime("%Y-%m-%d_%H-%M-%S")}{suffix}.zip'
        self.write_saves_zip(target, names)
        for old in sorted(folder.glob('saves-*.zip'), key=lambda p: p.stat().st_mtime)[:-SAVE_BACKUPS_KEPT]:
            try:
                old.unlink()
            except OSError:
                pass
        if notify:
            self.messagebox.showinfo('Shadow of the Colossus', _('Backup de {} save(s) em\n{}',
                                                                 'Backup of {} save(s) in\n{}').format(len(names), target))
        return target

    def import_save_folders(self, saves):
        """Copies save folders (each with sce_sys/param.sfo) into the game's saves, with their port
        folders when they come along; backs up everything first."""
        if not saves:
            self.messagebox.showerror('Shadow of the Colossus', _(
                'Nenhum save encontrado aí (um save é uma pasta com sce_sys/param.sfo).',
                'No save found there (a save is a folder with sce_sys/param.sfo).'))
            return
        target = self.saves_folder()
        existing = [p.name for p in saves if (target / p.name).exists()]
        if existing and not self.messagebox.askyesno('Shadow of the Colossus', _(
                'Substituir {} save(s) com o mesmo nome? Um backup de todos é feito antes.',
                'Replace {} save(s) with the same name? Everything is backed up first.').format(len(existing))):
            return
        try:
            if existing:
                self.backup_saves()
            target.mkdir(parents=True, exist_ok=True)
            for save in saves:
                for part in (save, save.with_name(save.name + '.sce_sys'), save.with_name(save.name + '.sce_backup')):
                    if part.is_dir():
                        shutil.rmtree(target / part.name, ignore_errors=True)
                        shutil.copytree(part, target / part.name)
        except OSError as error:
            self.messagebox.showerror('Shadow of the Colossus', _('Não foi possível importar o save: {}',
                                                                 'Could not import the save: {}').format(error))
            return
        self.refresh_saves()
        self.refresh_status()
        self.messagebox.showinfo('Shadow of the Colossus', _('{} save(s) importado(s).', '{} save(s) imported.').format(len(saves)))

    @staticmethod
    def find_saves(folder):
        """The save folders in `folder` or below it: with sce_sys/param.sfo inside (PS4, shadPS4) or
        the port's <name>.sce_sys beside them; not the port's own folders and backups."""
        def is_save(d):
            return (d / 'sce_sys' / 'param.sfo').is_file() or d.with_name(d.name + '.sce_sys').is_dir()
        folder = Path(folder)
        if is_save(folder):
            return [folder]
        skip = ('.sce_sys', '.sce_backup', '.bak')
        found = set()
        for d in folder.rglob('*'):
            if d.is_dir() and not any(part.endswith(skip) for part in d.relative_to(folder).parts) and is_save(d):
                found.add(d)
        # a save's own subfolders (its sce_sys) are not saves
        return sorted(d for d in found if not any(o != d and o in d.parents for o in found))

    def import_saves_folder(self):
        chosen = self.filedialog.askdirectory(title=_('Pasta com os saves', 'Folder with the saves'),
                                              initialdir=str(PORT_DIR.parent))
        if chosen:
            self.import_save_folders(self.find_saves(chosen))

    def import_saves_zip(self):
        chosen = self.filedialog.askopenfilename(title=_('Saves exportados (.zip)', 'Exported saves (.zip)'),
                                                 initialdir=str(self.backups_folder()), filetypes=[('ZIP', '*.zip')])
        if not chosen:
            return
        import tempfile
        with tempfile.TemporaryDirectory(prefix='sotc-saves-') as temp:
            try:
                with zipfile.ZipFile(chosen) as archive:
                    root = Path(temp).resolve()
                    for member in archive.namelist():  # nothing written outside the temporary folder
                        if not (root / member).resolve().is_relative_to(root):
                            raise zipfile.BadZipFile(member)
                    archive.extractall(temp)
            except (OSError, zipfile.BadZipFile) as error:
                self.messagebox.showerror('Shadow of the Colossus', _('Arquivo .zip inválido: {}',
                                                                     'Invalid .zip file: {}').format(error))
                return
            self.import_save_folders(self.find_saves(temp))

    # ---- first start: the setup screen -------------------------------------------------------
    # The guide's steps: (title, under the title), named after the game's journey.
    SETUP_STEPS = [(('O Despertar', 'The Awakening'), ('Boas-vindas', 'Welcome')),
                   (('O Santuário', 'The Shrine'), ('O seu jogo', 'Your game')),
                   (('A Espada Antiga', 'The Ancient Sword'), ('O seu PC', 'Your PC')),
                   (('Agro', 'Agro'), ('Controles', 'Controls')),
                   (('A Terra Proibida', 'The Forbidden Land'), ('Pronto para jogar', 'Ready to play'))]

    def show_setup(self):
        """The first setup as a guide in five steps, as in the bloodborne_pc launcher: the steps on
        the left, the step on the right, Back and Next at the bottom."""
        tk, ttk, px = self.tk, self.ttk, self.px
        if getattr(self, 'setup_frame', None):
            self.setup_frame.destroy()
        top = self.titlebar.winfo_reqheight() if self.titlebar is not None and self.custom_frame is not False else 0
        frame = self.setup_frame = tk.Frame(self.root, bg=PANEL)
        frame.place(x=0, y=top, relwidth=1, relheight=1, height=-top)
        frame.lift()
        rail = tk.Frame(frame, bg=BG, width=px(280))
        rail.pack(side='left', fill='y')
        rail.pack_propagate(False)
        tk.Frame(frame, bg=LINE, width=1).pack(side='left', fill='y')
        tk.Label(rail, text=_('PRIMEIRA JORNADA', 'FIRST JOURNEY'), bg=BG, fg=GOLD, font=(title_font(), 9)).pack(
            anchor='w', padx=px(26), pady=(px(34), 0))
        tk.Label(rail, text=_('Configuração inicial', 'First setup'), bg=BG, fg=theme.TEXT_STRONG,
                 font=(title_font(), 14), wraplength=px(230), justify='left').pack(anchor='w', padx=px(26),
                                                                                  pady=(px(2), px(26)))
        self.setup_rail = []
        for k, (title, sub) in enumerate(self.SETUP_STEPS):
            c = tk.Canvas(rail, width=px(250), height=px(58), bg=BG, highlightthickness=0, bd=0)
            c.pack(padx=px(14))
            self.setup_rail.append((c, _(*title), _(*sub)))
        tk.Frame(rail, bg=BG).pack(fill='both', expand=True)
        skip = tk.Label(rail, text=_('Pular: eu configuro sozinho', 'Skip: I set it up myself'), bg=BG, fg=MUTED,
                        font=(ui_font(), 9, 'underline'), cursor='hand2')
        skip.pack(pady=(0, px(20)))
        skip.bind('<Button-1>', lambda _e: self.finish_setup())
        right = tk.Frame(frame, bg=PANEL)
        right.pack(side='left', fill='both', expand=True)
        bar = tk.Frame(right, bg=BG, height=px(72))
        bar.pack(side='bottom', fill='x')
        bar.pack_propagate(False)
        tk.Frame(right, bg=LINE, height=1).pack(side='bottom', fill='x')
        self.setup_counter = tk.Label(bar, text='', bg=BG, fg=MUTED, font=(title_font(), 10))
        self.setup_counter.pack(side='left', padx=px(30))
        self.setup_next = ttk.Button(bar, text='', style='Bar.Accent.TButton', command=lambda: self.setup_go(1))
        self.setup_next.pack(side='right', padx=(px(8), px(26)))
        self.setup_back = ttk.Button(bar, text=_('Voltar', 'Back'), style='Bar.TButton', command=lambda: self.setup_go(-1))
        self.setup_back.pack(side='right')
        head = tk.Frame(right, bg=PANEL)
        head.pack(fill='x', padx=px(40), pady=(px(26), px(8)))
        self.setup_kicker = tk.Label(head, text='', bg=PANEL, fg=GOLD, font=(title_font(), 9))
        self.setup_kicker.pack(anchor='w')
        self.setup_title = tk.Label(head, text='', bg=PANEL, fg=theme.TEXT_STRONG, font=(title_font(), 24))
        self.setup_title.pack(anchor='w')
        self.setup_lead = tk.Label(head, text='', bg=PANEL, fg=MUTED, font=(ui_font(), 10), justify='left',
                                   wraplength=px(640))
        self.setup_lead.pack(anchor='w', pady=(px(4), 0))
        self.setup_body = tk.Frame(right, bg=PANEL)
        self.setup_body.pack(fill='both', expand=True, padx=px(40), pady=(px(10), 0))
        self.setup_step, self.setup_found_game, self.setup_hw = 0, None, None
        self.setup_show(0)
        # Meanwhile: the game on this PC (offered with one click) and the PC check.
        current = self.var('game_dir').get()
        if not game_info(current):
            def work():
                try:
                    from sotc_discovery import scan
                    games = scan(seconds=25, extra_roots=[str(PORT_DIR.parent)])['games']
                except Exception:  # never stop the launcher over a search problem
                    games = []
                self.setup_found_game = games[0]['path'] if games else None
                self.ui_calls.put(lambda: self.setup_step == 1 and self.setup_show(1))
            threading.Thread(target=work, daemon=True).start()

    def setup_paint_rail(self):
        px = self.px
        for k, (c, title, sub) in enumerate(self.setup_rail):
            c.delete('all')
            on, done = k == self.setup_step, k < self.setup_step
            mid = px(29)
            if k < len(self.setup_rail) - 1:  # the path down to the next step
                c.create_line(px(22), mid + px(14), px(22), px(58), fill=GOLD if done else LINE, width=max(1, px(1)))
            ring = SIGIL if on else GOLD if done else theme.BORDER_STRONG
            c.create_oval(px(8), mid - px(14), px(36), mid + px(14), outline=ring, width=max(1, px(1.5)),
                          fill=theme.SIGIL if on else BG)
            numeral = ('I', 'II', 'III', 'IV', 'V')[k]
            c.create_text(px(22), mid, text='✓' if done else numeral, fill=theme.TEXT_STRONG if on else GOLD if done else MUTED,
                          font=(title_font(), 9))
            c.create_text(px(50), mid - px(8), text=title, anchor='w', font=(title_font(), 11),
                          fill=theme.TEXT_STRONG if on else TEXT if done else theme.TEXT_SOFT)
            c.create_text(px(50), mid + px(10), text=sub, anchor='w', font=(ui_font(), 9), fill=MUTED)

    def setup_card(self, title=None, warning=False):
        """A rounded card in the step's body."""
        card = self.ttk.Frame(self.setup_body, style='Card.TFrame', padding=(self.px(20), self.px(16)))
        card.pack(fill='x', pady=(0, self.px(12)))
        if title:
            self.ttk.Label(card, text=title, style='Card.Section.TLabel',
                           foreground='#e07a6a' if warning else None).pack(anchor='w', pady=(0, self.px(6)))
        return card

    def setup_line(self, parent, mark, text):
        color = GOLD if mark == '✓' else '#d9b441' if mark == '⚠' else '#d36b5c' if mark == '✗' else MUTED
        row = self.ttk.Frame(parent, style='Inner.TFrame')
        row.pack(anchor='w', pady=self.px(2), fill='x')
        self.ttk.Label(row, text=mark, style='Card.TLabel', foreground=color, width=2).pack(side='left', anchor='n')
        self.ttk.Label(row, text=text, style='Card.TLabel', wraplength=self.px(600), justify='left').pack(side='left')

    def setup_show(self, step):
        tk, ttk, px = self.tk, self.ttk, self.px
        self.setup_step = step
        for child in self.setup_body.winfo_children():
            child.destroy()
        title, sub = self.SETUP_STEPS[step]
        numeral = ('I', 'II', 'III', 'IV', 'V')[step]
        self.setup_kicker.configure(text=f'{numeral} · {_(*title).upper()}')
        self.setup_counter.configure(text=_('Etapa {} de 5', 'Step {} of 5').format(step + 1))
        self.setup_back.configure(state='normal' if step else 'disabled')
        self.setup_next.configure(text=_('Começar a jornada  ›', 'Begin the journey  ›') if step == 4 else _('Avançar  ›', 'Next  ›'),
                                  state='normal')
        self.setup_paint_rail()
        getattr(self, ('setup_welcome', 'setup_game', 'setup_pc', 'setup_controls', 'setup_ready')[step])()

    def setup_go(self, step):
        target = self.setup_step + step
        if step > 0 and self.setup_step == 2 and getattr(self, 'setup_mode_var', None):
            self.var('play_mode').set(self.setup_mode_var.get())  # the chosen mode sets its settings
        if target > 4:
            self.finish_setup()
            return
        self.collect()
        self.setup_show(max(0, target))

    # -- I · the awakening
    def setup_welcome(self):
        tk, ttk = self.tk, self.ttk
        self.setup_title.configure(text=_('Bem-vindo, viajante', 'Welcome, wanderer'))
        self.setup_lead.configure(text=_('Alguns passos até a Terra Proibida. Tudo pode ser mudado depois no launcher.',
                                         'A few steps to the Forbidden Land. Everything can be changed later in the launcher.'))
        card = self.setup_card()
        row = ttk.Frame(card, style='Inner.TFrame')
        row.pack(anchor='w')
        ttk.Label(row, text=_('Idioma do launcher', 'Launcher language'), style='Card.TLabel').pack(side='left', padx=(0, self.px(14)))
        names = [(code, labels[0]) for code, labels in UI_LANGUAGES if code]
        lang_var = tk.StringVar(value=dict(names).get(LANG, 'English'))
        combo = ttk.Combobox(row, textvariable=lang_var, values=[n for _c, n in names], state='readonly',
                             width=16, style='Card.TCombobox')
        combo.pack(side='left')

        def chosen(_event=None):
            code = next((c for c, n in names if n == lang_var.get()), '')
            if code and code != LANG:
                self.var('ui_language').set(code)
                self.collect()
                self.restart()
        combo.bind('<<ComboboxSelected>>', chosen)
        card = self.setup_card(_('Só com a sua própria cópia do jogo', 'Only with your own copy of the game'), warning=True)
        ttk.Label(card, text=_('O port não vem com o jogo e não distribui arquivos dele. Use o dump do seu console '
                               '(decriptado), como no shadPS4.',
                               'The port does not come with the game and shares none of its files. Use the dump of your '
                               'own console (decrypted), as with shadPS4.'),
                  style='Card.TLabel', wraplength=self.px(640), justify='left').pack(anchor='w')
        card = self.setup_card(_('O que você precisa', 'What you need'))
        for mark, text in (('•', _('O dump do jogo: CUSA08809 (europeu, versão 1.01) ou CUSA08034 (americano, 1.00), '
                                   'com a atualização ao lado.',
                                   'The game dump: CUSA08809 (European, version 1.01) or CUSA08034 (American, 1.00), '
                                   'with its update next to it.')),
                           ('•', _('Os módulos de som do seu console (libSceNgs2, libSceUlt), para ter som.',
                                   "Your console's sound modules (libSceNgs2, libSceUlt), for sound.")),
                           ('•', _('Uma placa de vídeo com Vulkan 1.3 e o driver atualizado.',
                                   'A video card with Vulkan 1.3 and an up-to-date driver.')),
                           ('•', _('O jogo num SSD ou NVMe: num HD as trocas de área travam muito mais.',
                                   'The game on an SSD or NVMe drive: on a hard disk area changes stutter far more.'))):
            self.setup_line(card, mark, text)

    # -- II · the shrine
    def setup_game(self):
        ttk = self.ttk
        self.setup_title.configure(text=_('Onde está o seu jogo?', 'Where is your game?'))
        self.setup_lead.configure(text=_('A pasta principal do jogo, como no shadPS4: as subpastas do jogo, da atualização '
                                         '(-UPDATE/-patch) e das DLCs são encontradas sozinhas.',
                                         "The game's main folder, as in shadPS4: the game, update (-UPDATE/-patch) and "
                                         'DLC subfolders are found on their own.'))
        card = self.setup_card()
        row = ttk.Frame(card, style='Inner.TFrame')
        row.pack(anchor='w')
        self.setup_button = ttk.Button(row, text=_('Escolher a pasta do jogo…', 'Choose the game folder…'),
                                       style='Card.Accent.TButton', command=self.choose_game_for_setup)
        self.setup_button.pack(side='left')
        self.setup_hint = ttk.Label(card, text='', style='Card.TLabel', cursor='hand2', wraplength=self.px(640))
        self.setup_hint.pack(anchor='w', pady=(self.px(10), 0))
        self.setup_results = ttk.Frame(self.setup_body, style='TFrame')
        self.setup_results.pack(fill='x')
        current = self.var('game_dir').get()
        if game_info(current):
            self.configure_from_game(current)
        elif self.setup_found_game:
            self.offer_game(self.setup_found_game)
        else:
            self.setup_hint.configure(text=_('Procurando o jogo neste PC…', 'Looking for the game on this PC…'),
                                      foreground=MUTED)
        if not game_info(current):
            self.setup_next.configure(state='disabled')

    # -- III · the ancient sword
    def setup_pc(self):
        ttk = self.ttk
        self.setup_title.configure(text=_('A força da sua espada', "Your sword's strength"))
        self.setup_lead.configure(text=_('O launcher olha o seu PC e escolhe o modo de jogo que ele aguenta. Você pode '
                                         'trocar aqui ou depois na aba Jogar.',
                                         'The launcher looks at your PC and picks the play mode it can hold. You can '
                                         'change it here or later on the Play page.'))
        self.setup_pc_card = self.setup_card(_('O seu PC', 'Your PC'))
        ttk.Label(self.setup_pc_card, text=_('Verificando…', 'Checking…'), style='Card.TLabel').pack(anchor='w')
        card = self.setup_card(_('Modo de jogo', 'Play mode'))
        row = ttk.Frame(card, style='Inner.TFrame')
        row.pack(anchor='w')
        self.setup_mode_var = self.tk.StringVar(value=self.var('play_mode').get() or 'custom')
        values = [v for v, _t in PLAY_MODES]
        combo = ttk.Combobox(row, values=[_(*t) for _v, t in PLAY_MODES], state='readonly', width=30, style='Card.TCombobox')
        combo.pack(side='left')
        combo.current(values.index(self.setup_mode_var.get()) if self.setup_mode_var.get() in values else 3)
        combo.bind('<<ComboboxSelected>>', lambda _e: self.setup_mode_var.set(values[combo.current()]))
        self.setup_mode_combo, self.setup_mode_values = combo, values
        self.setup_mode_hint = ttk.Label(card, text='', style='Card.TLabel', wraplength=self.px(640), justify='left',
                                         foreground=MUTED)
        self.setup_mode_hint.pack(anchor='w', pady=(self.px(8), 0))
        self.setup_mode_var.trace_add('write', lambda *_a: self.setup_mode_hint.configure(
            text=_(*PLAY_MODE_HINTS.get(self.setup_mode_var.get(), ('', '')))) if self.setup_mode_hint.winfo_exists() else None)
        self.setup_mode_hint.configure(text=_(*PLAY_MODE_HINTS.get(self.setup_mode_var.get(), ('', ''))))
        if self.setup_hw:
            self.setup_pc_done(self.setup_hw)
        else:
            game, gpu = self.var('game_dir').get(), self.gpu_text

            def work():
                info = sotc_hardware.check(game, gpu)
                self.ui_calls.put(lambda: self.setup_pc_done(info, fresh=True))
            threading.Thread(target=work, daemon=True).start()

    def setup_pc_done(self, info, fresh=False):
        self.setup_hw = info
        if self.setup_step != 2 or not self.setup_pc_card.winfo_exists():
            return
        for child in list(self.setup_pc_card.winfo_children())[1:]:
            child.destroy()
        threads, memory, video, disk = info['threads'], info['memory'], info['video_memory'], info['disk']
        self.setup_line(self.setup_pc_card, '✓' if (threads or 0) >= 8 else '⚠',
                        _('Processador: {} ({} threads)', 'Processor: {} ({} threads)').format(info['cpu'] or '?', threads or '?'))
        self.setup_line(self.setup_pc_card, '✓' if (memory or 0) >= 12 else '⚠',
                        _('Memória: {} GB', 'Memory: {} GB').format(memory or '?'))
        self.setup_line(self.setup_pc_card, '✓' if (video or 0) >= 6 else '⚠',
                        self.gpu_text.lstrip('✓✗• ') if video else _('Placa de vídeo: ainda verificando', 'Video card: still checking'))
        self.setup_line(self.setup_pc_card, '✓' if disk in ('nvme', 'ssd') else '⚠' if disk == 'hdd' else '•',
                        {'nvme': _('Disco do jogo: NVMe', 'Game disk: NVMe'), 'ssd': _('Disco do jogo: SSD', 'Game disk: SSD'),
                         'hdd': _('Disco do jogo: HD — instale o jogo num SSD ou NVMe para trocas de área sem travar',
                                  'Game disk: hard disk — install the game on an SSD or NVMe for smooth area changes')}
                        .get(disk, _('Disco do jogo: não foi possível saber o tipo', 'Game disk: type unknown')))
        if fresh or self.setup_mode_var.get() == 'custom':
            mode = sotc_hardware.recommend(info)
            self.setup_mode_var.set(mode)
            self.setup_mode_combo.current(self.setup_mode_values.index(mode))
            self.setup_line(self.setup_pc_card, '›', _('Recomendado para este PC: {}', 'Recommended for this PC: {}').format(
                _(*dict(PLAY_MODES)[mode]).split(' (')[0]))

    # -- IV · agro
    def setup_controls(self):
        self.setup_title.configure(text=_('Agro, sua companheira', 'Agro, your companion'))
        self.setup_lead.configure(text=_('Controle ou teclado e mouse: os dois funcionam juntos. Conecte o controle antes '
                                         'de abrir o jogo.', 'Controller or keyboard and mouse: both work together. Connect '
                                         'the controller before starting the game.'))
        card = self.setup_card(_('Controle', 'Controller'))
        row = self.ttk.Frame(card, style='Inner.TFrame')
        row.pack(anchor='w', pady=(0, self.px(6)))
        self.ttk.Label(row, text=_('Ícones dos botões', 'Button icons'), style='Card.TLabel').pack(side='left', padx=(0, self.px(14)))
        box = self.choice(row, 'button_icons', BUTTON_ICONS, 26)
        box.configure(style='Card.TCombobox')
        box.pack(side='left')
        self.ttk.Checkbutton(card, text=_('Vibração do controle', 'Controller vibration'), variable=self.var('rumble'),
                             style='Card.TCheckbutton').pack(anchor='w')
        card = self.setup_card(_('Teclado e mouse', 'Keyboard and mouse'))
        self.ttk.Checkbutton(card, text=_('Câmera no mouse', 'Mouse camera'), variable=self.var('mouse'),
                             style='Card.TCheckbutton').pack(anchor='w')
        self.ttk.Label(card, text=_('W A S D andam, as setas giram a câmera, Espaço é ✕. As teclas mudam na aba Controles.',
                                    'W A S D move, the arrows turn the camera, Space is ✕. The keys change on the Controls page.'),
                       style='Card.TLabel', wraplength=self.px(640), justify='left', foreground=MUTED).pack(anchor='w', pady=(self.px(6), 0))

    # -- V · the forbidden land
    def setup_ready(self):
        self.setup_title.configure(text=_('A Terra Proibida espera', 'The Forbidden Land awaits'))
        self.setup_lead.configure(text=_('Tudo pronto. A primeira abertura monta o cache de shaders: algumas travadas no '
                                         'começo são normais.', 'All set. The first start builds the shader cache: a few '
                                         'stutters at first are normal.'))
        card = self.setup_card(_('Resumo', 'Summary'))
        info = game_info(self.var('game_dir').get())
        self.setup_line(card, '✓' if info else '✗', _('Jogo: {} (versão {})', 'Game: {} (version {})').format(*info)
                        if info else _('Jogo: não escolhido', 'Game: not chosen'))
        self.setup_line(card, '✓' if self.sound_ready() else '⚠', _('Som: pronto', 'Sound: ready') if self.sound_ready()
                        else _('Som: sem os módulos, o jogo roda mudo', 'Sound: without the modules the game runs silent'))
        mode = self.var('play_mode').get()
        self.setup_line(card, '✓', _('Modo de jogo: {}', 'Play mode: {}').format(_(*dict(PLAY_MODES).get(mode, ('', '')))))
        self.setup_line(card, '✓', self.summary())

    def offer_game(self, path):
        if not getattr(self, 'setup_hint', None) or not self.setup_hint.winfo_exists():
            return
        self.setup_found_game = path
        self.setup_hint.configure(text=_('Encontrado: {}   —   clique aqui para usar', 'Found: {}   —   click here to use it')
                                  .format(path), foreground=GOLD)
        self.setup_hint.bind('<Button-1>', lambda _e: self.configure_from_game(path))

    def choose_game_for_setup(self):
        chosen = self.filedialog.askdirectory(title=_('Pasta do jogo Shadow of the Colossus', 'Shadow of the Colossus game folder'),
                                              initialdir=self.setup_found_game or str(PORT_DIR.parent))
        if not chosen:
            return
        from sotc_discovery import find_game_in
        game = find_game_in(chosen)
        if not game:
            self.messagebox.showerror('Shadow of the Colossus', _(
                'Não achei o jogo (eboot.bin do CUSA08809 ou CUSA08034) em\n{}\n\nEscolha a pasta do seu dump.',
                'The game (eboot.bin of CUSA08809 or CUSA08034) is not in\n{}\n\nChoose the folder of your dump.')
                .format(chosen))
            return
        self.configure_from_game(game, chosen)

    def configure_from_game(self, game, chosen=None):
        """Game chosen: DLC from the game folder, saves, sound — in the background."""
        self.setup_button.configure(state='disabled')
        self.setup_hint.unbind('<Button-1>')
        self.setup_hint.configure(text=_('Configurando…', 'Setting up…'), foreground=MUTED)
        user = user_dir({'user_dir': self.var('user_dir').get()})

        def work():
            from sotc_discovery import find_dlc_root, find_update, dlc_count, scan, SYS_MODULES as modules
            result = {'game': game, 'dlc': find_dlc_root(game, chosen), 'update': find_update(game), 'sound': None,
                      'save': None}
            serial = game_serial(game)
            result['dlc_count'] = dlc_count(result['dlc'], serial)
            sound_ready = self.app.get('sys_modules') and all(
                (Path(self.app['sys_modules']) / m).is_file() for m in modules)
            saves = user / 'savedata' / '1' / serial
            has_saves = saves.is_dir() and any(saves.iterdir())
            found = {'sys_modules': [], 'saves': []}
            if not sound_ready or not has_saves:
                try:
                    found = scan(seconds=30, extra_roots=[str(Path(game).parent), str(PORT_DIR.parent)])
                except Exception:
                    pass
            if sound_ready:
                result['sound'] = self.app['sys_modules']
            elif found['sys_modules']:
                target = user / 'sys_modules'
                try:
                    target.mkdir(parents=True, exist_ok=True)
                    for module in modules:
                        shutil.copy2(Path(found['sys_modules'][0]) / module, target / module)
                    result['sound'] = str(target)
                except OSError:
                    pass
            if not has_saves:
                own = user.resolve()
                outside = [p for p in found['saves']
                           if Path(p).name == serial and not Path(p).resolve().is_relative_to(own)]
                if outside:
                    try:
                        saves.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copytree(outside[0], saves, dirs_exist_ok=True)
                        result['save'] = outside[0]
                    except OSError:
                        pass
            self.ui_calls.put(lambda: self.setup_done_screen(result))
        threading.Thread(target=work, daemon=True).start()

    def setup_done_screen(self, result):
        tk, ttk = self.tk, self.ttk
        self.var('game_dir').set(result['game'])
        self.var('dlc_dir').set(result['dlc'])
        if self.var('user_dir').get():  # a data folder picked one level too deep, corrected
            self.var('user_dir').set(str(user_dir({'user_dir': self.var('user_dir').get()})))
        if result['sound']:
            self.var('sys_modules').set(result['sound'])
        folders = self.app.setdefault('library_dirs', [])
        if str(Path(result['game']).parent) not in folders:
            folders.append(str(Path(result['game']).parent))
        if self.setup_step != 1 or not self.setup_results.winfo_exists():
            self.collect()
            return
        self.setup_hint.configure(text='')
        for child in self.setup_results.winfo_children():
            child.destroy()
        info = game_info(result['game'])
        saves = self.saves_folder()
        lines = [
            ('✓', _('Jogo: {} (versão {})', 'Game: {} (version {})').format(*info) if info else result['game']),
            ('✓' if result['update'] else '•',
             _('Atualização: {} (ao lado do jogo)', 'Update: {} (next to the game)').format(Path(result['update']).name)
             if result['update']
             else _('Atualização: nenhuma pasta -UPDATE ou -patch', 'Update: no -UPDATE or -patch folder')),
            ('✓' if result['dlc_count'] else '•',
             _('DLC: {} encontrada(s)', 'DLC: {} found').format(result['dlc_count']) if result['dlc_count']
             else _('DLC: nenhuma encontrada na pasta do jogo', 'DLC: none found in the game folder')),
            ('✓', _('Saves: {}', 'Saves: {}').format(short_path(saves)) + (
                _('  (copiado do shadPS4)', '  (copied from shadPS4)') if result['save'] else '')),
            ('✓' if result['sound'] else '⚠',
             _('Som: pronto', 'Sound: ready') if result['sound'] else
             _('Som: módulos não encontrados — o jogo roda mudo', 'Sound: modules not found — the game runs silent')),
        ]
        card = ttk.Frame(self.setup_results, style='Card.TFrame', padding=(self.px(20), self.px(16)))
        card.pack(fill='x')
        ttk.Label(card, text=_('Encontrado', 'Found'), style='Card.Section.TLabel').pack(anchor='w', pady=(0, self.px(6)))
        for mark, text in lines:
            self.setup_line(card, mark, text)
        if not result['sound']:
            ttk.Button(card, text=_('Escolher a pasta do som…', 'Choose the sound folder…'), style='Card.TButton',
                       command=self.choose_sound_for_setup).pack(anchor='w', pady=(self.px(10), 0))
        self.setup_button.configure(state='normal', text=_('Trocar a pasta do jogo…', 'Change the game folder…'))
        self.setup_next.configure(state='normal')
        self.collect()

    def choose_sound_for_setup(self):
        chosen = self.filedialog.askdirectory(title=_('Pasta com libSceNgs2.sprx e libSceUlt.sprx',
                                                      'Folder with libSceNgs2.sprx and libSceUlt.sprx'))
        if chosen and all((Path(chosen) / m).is_file() for m in SYS_MODULES):
            target = user_dir(self.app) / 'sys_modules'
            target.mkdir(parents=True, exist_ok=True)
            for module in SYS_MODULES:
                shutil.copy2(Path(chosen) / module, target / module)
            self.var('sys_modules').set(str(target))
            self.messagebox.showinfo('Shadow of the Colossus', _('Som pronto.', 'Sound ready.'))
        elif chosen:
            self.messagebox.showerror('Shadow of the Colossus', _(
                'Essa pasta não tem libSceNgs2.sprx e libSceUlt.sprx.',
                'That folder does not have libSceNgs2.sprx and libSceUlt.sprx.'))

    def finish_setup(self):
        self.app['setup_done'] = True
        self.collect()
        self.setup_frame.destroy()
        self.setup_frame = None
        self.refresh_library()
        self.game_changed()
        self.show('play')

    # ---- library and automatic setup ---------------------------------------------------------
    def library_icon(self, path):
        if not path:
            return None
        if path not in self.library_icons:
            image = None
            try:
                from PIL import Image, ImageTk
                with Image.open(path) as source:
                    size = self.px(36)
                    image = ImageTk.PhotoImage(source.convert('RGBA').resize((size, size), Image.LANCZOS))
            except (ImportError, OSError):
                pass
            self.library_icons[path] = image
        return self.library_icons[path]

    def refresh_library(self):
        from sotc_discovery import library_games
        folders = list(self.app.get('library_dirs') or [])
        current = self.var('game_dir').get()
        extra = [str(Path(current).parent)] if game_info(current) else []
        tree = self.library
        tree.delete(*tree.get_children())
        for game in library_games(folders + extra):
            tree.insert('', 'end', iid=game['path'], text='  ' + game['title'],
                        image=self.library_icon(game['icon']) or '',
                        values=(game['id'], game['version'], game['path']))
            if current and os.path.normcase(game['path']) == os.path.normcase(current):
                tree.selection_set(game['path'])
        self.library_label.configure(text=_('Pastas da biblioteca: {}', 'Library folders: {}').format(
            '; '.join(folders) or _('nenhuma ainda', 'none yet')))

    def use_selected_game(self):
        selection = self.library.selection()
        if selection:
            self.var('game_dir').set(selection[0])

    def add_library_dir(self):
        chosen = self.filedialog.askdirectory(title=_('Pasta com os seus jogos de PS4', 'Folder with your PS4 games'),
                                              initialdir=str(PORT_DIR.parent))
        if chosen:
            folders = self.app.setdefault('library_dirs', [])
            if str(Path(chosen)) not in folders:
                folders.append(str(Path(chosen)))
            self.refresh_library()

    def remove_library_dir(self):
        folders = self.app.get('library_dirs') or []
        selection = self.library.selection()
        chosen = next((f for f in folders if selection and Path(selection[0]).is_relative_to(f)),
                      folders[-1] if folders else None)
        if chosen and self.messagebox.askyesno('Shadow of the Colossus', _(
                'Tirar {} da biblioteca? (nada é apagado do disco)',
                'Remove {} from the library? (nothing is deleted from the disk)').format(chosen)):
            folders.remove(chosen)
            self.refresh_library()

    def sound_ready(self):
        folder = self.var('sys_modules').get()
        return bool(folder) and all((Path(folder) / m).is_file() for m in SYS_MODULES)

    def saves_folder(self):
        return user_dir({'user_dir': self.var('user_dir').get()}) / 'savedata' / '1' / game_serial(self.var('game_dir').get())

    def has_saves(self):
        saves = self.saves_folder()
        return saves.is_dir() and any(saves.iterdir())

    def start_discovery(self, manual=False):
        """Looks for the game, the user's sound modules and shadPS4 saves in the background."""
        if getattr(self, 'discovering', False):
            return
        self.discovering = True
        self.library_label.configure(text=_('Procurando o jogo, os módulos de som e os saves neste PC…',
                                            'Looking for the game, the sound modules and saves on this PC…'))
        roots = [str(PORT_DIR.parent), *(self.app.get('library_dirs') or [])]

        def work():
            try:
                from sotc_discovery import scan
                found = scan(seconds=40, extra_roots=roots)
            except Exception:  # a scan problem never stops the launcher
                found = {'games': [], 'sys_modules': [], 'saves': [], 'dlc': []}
            self.ui_calls.put(lambda: self.apply_discovery(found, manual))
        threading.Thread(target=work, daemon=True).start()

    def apply_discovery(self, found, manual):
        self.discovering = False
        done = []
        folders = self.app.setdefault('library_dirs', [])
        for game in found['games']:
            parent = str(Path(game['path']).parent)
            if parent not in folders:
                folders.append(parent)
        if found['games'] and not game_info(self.var('game_dir').get()):
            best = min(found['games'], key=lambda g: not game_supported(g['path']))
            self.var('game_dir').set(best['path'])
            done.append(_('jogo em {}', 'game in {}').format(best['path']))
        if not self.sound_ready() and found['sys_modules']:
            # A copy next to the port: the game keeps sound even if that shadPS4 folder goes away.
            target = user_dir({'user_dir': self.var('user_dir').get()}) / 'sys_modules'
            try:
                target.mkdir(parents=True, exist_ok=True)
                for module in SYS_MODULES:
                    shutil.copy2(Path(found['sys_modules'][0]) / module, target / module)
                self.var('sys_modules').set(str(target))
                done.append(_('som (de {})', 'sound (from {})').format(found['sys_modules'][0]))
            except OSError:
                pass
        serial = game_serial(self.var('game_dir').get())
        dlc = next((path for path, dlc_serial in found.get('dlc', []) if dlc_serial == serial), None)
        if not self.var('dlc_dir').get() and dlc:
            self.var('dlc_dir').set(dlc)
            done.append(_('DLC em {}', 'DLC in {}').format(dlc))
        if not self.has_saves():
            own = user_dir({'user_dir': self.var('user_dir').get()}).resolve()
            outside = [p for p in found['saves']
                       if Path(p).name == serial and not Path(p).resolve().is_relative_to(own)]
            if outside and self.messagebox.askyesno('Shadow of the Colossus', _(
                    'Encontrei um save do jogo em\n{}\nImportar para o port?',
                    'Found a save of the game in\n{}\nImport it into the port?').format(outside[0])):
                self.copy_save(Path(outside[0]))
                done.append(_('save importado', 'save imported'))
        self.collect()
        self.refresh_library()
        self.refresh_status()
        if done:
            text = _('Configurado automaticamente: {}', 'Set up automatically: {}').format('; '.join(done))
        elif manual:
            text = _('Nada novo encontrado.', 'Nothing new found.')
        else:
            text = self.library_label.cget('text')
        self.library_label.configure(text=text)
        if done:
            self.append(text + '\n')

    def copy_save(self, source):
        target = self.saves_folder()
        if target.exists() and any(target.iterdir()):
            backup = target.with_name(target.name + '.bak')
            shutil.rmtree(backup, ignore_errors=True)
            target.rename(backup)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source, target, dirs_exist_ok=True)

    # ---- game process ------------------------------------------------------------------------
    def play(self):
        if self.process or self.updating:
            return
        self.collect()
        if not game_info(self.app['game_dir']):
            self.messagebox.showerror('Shadow of the Colossus', _('Escolha a pasta do jogo com eboot.bin (CUSA08809 ou CUSA08034).',
                                                                  'Choose the game folder with eboot.bin (CUSA08809 or CUSA08034).'))
            self.show('game')
            return
        free = free_commit_gb()
        if free is not None and free < MIN_FREE_COMMIT_GB and not self.messagebox.askyesno(
                'Shadow of the Colossus', _('O Windows só tem {:.1f} GB livres para programas (RAM + arquivo de paginação). O jogo reserva cerca de 6 GB ao abrir e precisa de mais alguns GB depois; com menos, ele fecha logo no início (erro 1455 no log).\n\nPara resolver: Iniciar → "Ajustar a aparência e o desempenho do Windows" → Avançado → Memória virtual → Alterar… → marque "Gerenciar automaticamente o tamanho do arquivo de paginação" (ou defina pelo menos 16 GB) e reinicie o PC. Fechar navegadores e outros programas grandes também ajuda.\n\nAbrir o jogo mesmo assim?',
                                            'Windows has only {:.1f} GB free for programs (RAM + page file). The game reserves about 6 GB when it starts and needs a few GB more after that; with less it closes right at the start (error 1455 in the log).\n\nTo fix it: Start → "Adjust the appearance and performance of Windows" → Advanced → Virtual memory → Change… → tick "Automatically manage paging file size for all drives" (or set at least 16 GB) and restart the PC. Closing browsers and other big programs also helps.\n\nStart the game anyway?').format(free)):
            return
        self.set_log('')
        try:
            write_key_bindings(self.app.get('keys'))
        except OSError as error:
            self.append(_('Não foi possível gravar as teclas no sotc.ini: {}',
                          'Could not write the keys to sotc.ini: {}').format(error) + '\n')
        try:
            env = game_environment(self.app)
            env['SOTC_EVENT_LOG'] = start_event_log(user_dir(self.app), self.app, env)
            self.process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=env,
                                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        except OSError as error:
            self.append(_('Não foi possível iniciar: {}', 'Could not start: {}').format(error) + '\n')
            self.process = None
            return
        self.pad_stop()  # the controllers are the game's now
        self.job = GameJob(self.process)
        threading.Thread(target=self.read_output, args=(self.process,), daemon=True).start()
        self.play_button.configure(state='disabled')
        self.stop_button.configure(state='normal')
        self.status.configure(text=_('Preparando o jogo; ele abre na própria janela…',
                                     'Preparing the game; it opens in its own window…'), fg=GOLD)
        if self.app.get('close_on_play'):
            self.root.after(5000, self.root.destroy)

    def read_output(self, process):
        # The output also goes to user/last_run.log, so a crash can be looked at afterwards.
        log_dir = user_dir(self.app)
        try:
            log = open_session_log(log_dir)
        except OSError:
            log = None
        for raw in iter(process.stdout.readline, b''):
            text = raw.decode('utf-8', errors='replace')
            self.output.put(text)
            if log:
                try:
                    log.write(text)
                except (OSError, ValueError):
                    pass
        code = process.wait()
        if log:
            try:
                log.write(f'[exit code {code} = {code & 0xffffffff:#x}]\n')
                log.close()
            except (OSError, ValueError):
                pass
            if not self.stopping:
                keep_crash_log(log_dir, code)
            end_event_log(log_dir, code, self.stopping)
        self.stopping = False
        self.output.put((code,))

    # Quick settings the in-game menu may have changed (it writes them to the settings file).
    LIVE_KEYS = ('fps_mode', 'framegen', 'textures', 'shadow_reuse', 'volume', 'upscaler', 'sharpness', 'rumble')

    def reload_live_settings(self):
        saved = load_json(CONFIG_FILE, {})
        for key in self.LIVE_KEYS:
            if key in saved and key in self.vars and saved[key] != self.vars[key].get():
                try:
                    self.vars[key].set(saved[key])
                except Exception:  # noqa: BLE001 - a value the widget does not take
                    continue
                self.app[key] = saved[key]

    def drain_output(self):
        while not self.ui_calls.empty():
            self.ui_calls.get_nowait()()
        try:
            for _i in range(500):
                item = self.output.get_nowait()
                if isinstance(item, tuple):
                    self.append(_('\n— o jogo terminou (código {}) —\n', '\n— the game exited (code {}) —\n').format(item[0]))
                    self.process = None
                    self.reload_live_settings()
                    if self.job:
                        self.job.close()
                    self.stop_button.configure(state='disabled')
                    self.refresh_status()
                    self.exit_backup()
                else:
                    if 'Entering original x86-64 code' in item:
                        self.status.configure(text=_('O jogo está rodando.', 'The game is running.'), fg=GOLD)
                    self.append(item)
        except queue.Empty:
            pass
        self.root.after(100, self.drain_output)

    def stop(self):
        if self.job:
            self.stopping = True  # stopped from the launcher: not a crash
            self.job.terminate()

    def append(self, text):
        self.log.configure(state='normal')
        self.log.insert('end', text)
        lines = int(self.log.index('end-1c').split('.')[0])
        if lines > MAX_LOG_LINES:
            self.log.delete('1.0', f'{lines - MAX_LOG_LINES}.0')
        self.log.see('end')
        self.log.configure(state='disabled')

    def set_log(self, text):
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        self.log.insert('end', text)
        self.log.configure(state='disabled')

    def copy_log(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.log.get('1.0', 'end'))

    def open_path(self, path):
        path = Path(path or PORT_DIR)
        if not path.exists():
            path.mkdir(parents=True, exist_ok=True)
        os.startfile(str(path))

    def clear_cache(self):
        if self.process:
            self.messagebox.showwarning('Shadow of the Colossus', _(
                'Feche o jogo primeiro: o cache de shaders só pode ser limpo com o jogo fechado.',
                'Close the game first: the shader cache can only be cleared while the game is closed.'))
            return
        for name in ('cache', 'shader'):
            shutil.rmtree(user_dir(self.app) / name, ignore_errors=True)
        self.messagebox.showinfo('Shadow of the Colossus', _(
            'Cache de shaders limpo. A próxima partida engasga alguns minutos enquanto ele é refeito.',
            'Shader cache cleared. The next start stutters for a few minutes while it is rebuilt.'))

    def shortcut(self, play=False):
        self.collect()
        script = _shortcut_script(play, FROZEN, sys.executable, PORT_DIR, self.game_icon_file())
        result = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True,
                                creationflags=NO_WINDOW)
        if result.returncode == 0:
            self.messagebox.showinfo('Shadow of the Colossus', _(
                'Atalho criado na área de trabalho. Ele abre o jogo direto e pode ser adicionado ao Steam como '
                'jogo não-Steam.',
                'Shortcut created on the desktop. It starts the game directly and can be added to Steam as a '
                'non-Steam game.') if play else _('Atalho criado na área de trabalho.',
                                                  'Shortcut created on the desktop.'))
        else:
            self.messagebox.showerror('Shadow of the Colossus', result.stderr.decode(errors='replace')[:400])

    # sotc: self-update from the GitHub releases (sotc_update.py).
    def check_updates(self, manual):
        if self.updating:
            return
        self.updating = True
        if manual:
            self.status.configure(text=_('Procurando atualização…', 'Checking for updates…'), fg=MUTED)

        def work():
            try:
                release, error = sotc_update.latest_release(), None
            except Exception as problem:  # offline or rate-limited: only a manual check says so
                release, error = None, problem
            self.ui_calls.put(lambda: self.offer_update(release, error, manual))
        threading.Thread(target=work, daemon=True).start()

    def offer_update(self, release, error, manual):
        self.updating = False
        if manual:
            self.refresh_status()
        if error or not sotc_update.is_newer(release, VERSION):
            if manual and error and sotc_update.is_cert_error(error):
                self.messagebox.showwarning('Shadow of the Colossus', _(
                    'Não foi possível verificar a conexão com o GitHub (certificado não reconhecido).\n'
                    'Baixe a versão nova manualmente em:\n{}',
                    'The connection to GitHub could not be verified (certificate not trusted).\n'
                    'You can download the new version manually from:\n{}').format(sotc_update.RELEASES_PAGE))
            elif manual and error:
                self.messagebox.showwarning('Shadow of the Colossus', _(
                    'Não foi possível procurar atualização:\n{}', 'Could not check for updates:\n{}').format(error))
            elif manual:
                self.messagebox.showinfo('Shadow of the Colossus', _(
                    'Você já tem a versão mais recente (v{}).', 'You have the latest version (v{}).').format(VERSION))
            return
        if not manual and release['version'] == self.app.get('skip_version'):
            return
        notes = sotc_update.notes_for(release['notes'], LANG != 'pt')  # other languages fall back to English
        if len(notes) > 900:
            notes = notes[:900].rsplit('\n', 1)[0] + '\n…'
        question = _('Há uma versão nova: v{new} (você tem v{cur}).\n\n{notes}\n\n',
                     'A new version is available: v{new} (you have v{cur}).\n\n{notes}\n\n').format(
            new=release['version'], cur=VERSION, notes=notes)
        if not FROZEN:  # running from the sources: nothing to replace, show the release
            if self.messagebox.askyesno('Shadow of the Colossus', question + _('Abrir a página?', 'Open the page?')):
                os.startfile(release['page'])
            return
        if self.messagebox.askyesno('Shadow of the Colossus', question + _(
                'Baixar e instalar agora? Saves, cache de shaders e sotc.ini são mantidos, e o lançador abre '
                'de novo sozinho.',
                'Download and install now? Saves, shader cache and sotc.ini are kept, and the launcher opens '
                'again by itself.')):
            self.install_update(release)
        else:
            self.app['skip_version'] = release['version']  # no question at startup for this one again

    def install_update(self, release):
        if self.process:
            self.messagebox.showinfo('Shadow of the Colossus', _('Feche o jogo antes de atualizar.',
                                                                 'Close the game before updating.'))
            return
        self.updating = True
        self.play_button.configure(state='disabled')

        def progress(done, total):
            text = _('Baixando a atualização: {} de {} MB', 'Downloading the update: {} of {} MB').format(
                done >> 20, (total >> 20) or '?')
            self.ui_calls.put(lambda: self.status.configure(text=text, fg=GOLD))

        def work():
            try:
                new_root, error = sotc_update.download(release, PORT_DIR / 'update', progress), None
            except Exception as problem:
                new_root, error = None, problem
            self.ui_calls.put(lambda: self.finish_update(new_root, error))
        threading.Thread(target=work, daemon=True).start()

    def finish_update(self, new_root, error):
        if not error:
            try:
                sotc_update.start_applier(new_root, PORT_DIR)
            except Exception as problem:
                error = problem
        if error and sotc_update.is_cert_error(error):
            error = _('não foi possível verificar a conexão com o GitHub. Baixe manualmente em:\n{}',
                      'the connection to GitHub could not be verified. Download it manually from:\n{}').format(
                sotc_update.RELEASES_PAGE)
        if error:
            self.updating = False
            self.refresh_status()
            self.messagebox.showerror('Shadow of the Colossus', _(
                'A atualização não foi instalada:\n{}', 'The update was not installed:\n{}').format(error))
            return
        self.status.configure(text=_('Instalando a atualização…', 'Installing the update…'), fg=GOLD)
        self.root.after(300, self.close)  # the applier waits for this process to end

    def close(self):
        try:
            self.collect()
        except Exception:  # never keep the window open over a settings problem
            pass
        self.root.destroy()


class GameJob:
    """A Windows job holding sotc.py and the bb-probe.exe it starts (Stop ends both)."""

    def __init__(self, process):
        kernel32 = ctypes.windll.kernel32
        kernel32.CreateJobObjectW.restype = ctypes.c_void_p
        self.handle = kernel32.CreateJobObjectW(None, None)
        if self.handle:
            kernel32.AssignProcessToJobObject(ctypes.c_void_p(self.handle), ctypes.c_void_p(int(process._handle)))

    def terminate(self):
        if self.handle:
            ctypes.windll.kernel32.TerminateJobObject(ctypes.c_void_p(self.handle), 1)

    def close(self):
        if self.handle:
            ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(self.handle))
            self.handle = None


def free_commit_gb():
    """Commit memory Windows can still give programs (RAM + page file), in GB; None if unknown."""
    class MemoryStatus(ctypes.Structure):
        _fields_ = [('length', ctypes.c_uint32), ('load', ctypes.c_uint32),
                    ('total_phys', ctypes.c_uint64), ('avail_phys', ctypes.c_uint64),
                    ('total_commit', ctypes.c_uint64), ('avail_commit', ctypes.c_uint64),
                    ('total_virtual', ctypes.c_uint64), ('avail_virtual', ctypes.c_uint64),
                    ('avail_extended', ctypes.c_uint64)]
    status = MemoryStatus()
    status.length = ctypes.sizeof(status)
    try:
        if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            return None
    except (AttributeError, OSError):
        return None
    return status.avail_commit / 1e9


# The guest memory pool (~6 GB, committed at once when the game starts) plus the process itself:
# below this the game stops at boot with error 1455 (issue #95).
MIN_FREE_COMMIT_GB = 8.0


def play_without_window(settings):
    """--play: the game with the saved settings (shortcuts, Steam); output to user/last_run.log."""
    attach_stdio()
    log_dir = user_dir(settings)
    log_dir.mkdir(parents=True, exist_ok=True)
    try:
        write_key_bindings(settings.get('keys'))
    except OSError:
        pass
    if not settings.get('gpu_vram_mb'):
        # sotc: the video memory profile's Auto needs the card (the window finds it in detect_gpu).
        exe = PORT_DIR / 'bin' / 'bb-gpu-capabilities.exe'
        try:
            result = subprocess.run([str(exe), '--live-resolution'], capture_output=True, text=True,
                                    timeout=30, creationflags=NO_WINDOW)
            found = re.search(r'^GPU: .*?(\d+)\s*MiB', result.stderr, re.MULTILINE)
            if found:
                settings['gpu_vram_mb'] = int(found.group(1))
        except (OSError, subprocess.TimeoutExpired):
            pass
    env = game_environment(settings)
    env['SOTC_EVENT_LOG'] = start_event_log(log_dir, settings, env)
    with open_session_log(log_dir) as log:
        process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=env,
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        for raw in iter(process.stdout.readline, b''):
            text = raw.decode('utf-8', errors='replace')
            log.write(text)
            try:
                sys.stdout.write(text)
            except (OSError, ValueError):
                pass
        code = process.wait()
        log.write(f'[exit code {code} = {code & 0xffffffff:#x}]\n')
    keep_crash_log(log_dir, code)
    end_event_log(log_dir, code)
    return code


def main():
    global LANG
    args = sys.argv[1:]
    if args and args[0] in ('--run', '--script'):
        sys.exit(run_role(args))
    # sotc: --apply-update <new folder> <port folder> <launcher pid>: the copied launcher that
    # installs an update (sotc_update.py). --update-now: check, download and install without
    # the window (scripted tests; SOTC_UPDATE_URL can point at a test release).
    if args and args[0] == '--apply-update' and len(args) == 4:
        sys.exit(0 if sotc_update.apply_update(args[1], args[2], int(args[3])) else 1)
    if args and args[0] == '--update-now':
        attach_stdio()
        try:
            release = sotc_update.latest_release()
            if not sotc_update.is_newer(release, VERSION):
                print(f'up to date (v{VERSION}, latest {release and release["version"]})')
                sys.exit(0)
            print(f'updating v{VERSION} -> v{release["version"]}')
            new_root = sotc_update.download(release, PORT_DIR / 'update')
        except OSError as error:
            if not sotc_update.is_cert_error(error):
                raise
            print(f'the connection to GitHub could not be verified ({error}); '
                  f'download the release manually from {sotc_update.RELEASES_PAGE}')
            sys.exit(1)
        if FROZEN:
            sotc_update.start_applier(new_root, PORT_DIR)
        sys.exit(0)
    settings = load_settings()
    LANG = settings.get('ui_language') or windows_language()
    if '--play' in args and (Path(settings['game_dir'] or '.') / 'eboot.bin').is_file():
        sys.exit(play_without_window(settings))
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    root = tk.Tk()
    Launcher(root, tk, ttk, filedialog, messagebox)
    root.mainloop()


if __name__ == '__main__':
    main()
