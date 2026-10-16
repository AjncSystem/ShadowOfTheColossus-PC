#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Shadow of the Colossus launcher for Windows (Tkinter), derived from the bbport launcher.

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
from pathlib import Path
import queue
import runpy
import shutil
import subprocess
import sys
import threading
import time

FROZEN = getattr(sys, 'frozen', False)
PORT_DIR = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT_DIR / 'scripts'))
CONFIG_DIR = Path(os.environ.get('APPDATA', Path.home())) / 'sotc-launcher'
CONFIG_FILE = CONFIG_DIR / 'settings.json'
GAME_ID, GAME_VERSION = 'CUSA08809', '01.01'
SYS_MODULES = ('libSceUlt.sprx', 'libSceNgs2.sprx')
VERSION = '0.57-opt3'
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


def run_role(argv):
    """--run [args]: sotc.py; --script <file> [args]: a preparation script. Returns an exit code."""
    attach_stdio()
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(line_buffering=True)
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


def _(pt, en):
    return pt if LANG == 'pt' else en


def windows_language():
    try:
        primary = ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff
    except (AttributeError, OSError):
        primary = 0
    return 'pt' if primary == 0x16 else 'en'


# ---------------------------------------------------------------------------------------------
# Settings, passed to sotc.py as environment variables.

APP_DEFAULTS = {'ui_language': '', 'game_dir': str(PORT_DIR.parent / GAME_ID), 'sys_modules': '',
                'user_dir': '', 'af16': True, 'fps60': False, 'fullscreen': False,
                'present_mode': 'Mailbox', 'language': '', 'player_name': '', 'show_fps': False,
                'readbacks': '', 'extra_env': '', 'close_on_play': False, 'gamepad': '',
                'output_res': '1920x1080', 'upscaler': 'fsr1', 'sharpness': '250', 'mouse': True,
                'mouse_sens': '1.0', 'mouse_invert': False, 'neo': False, 'fx_vignette': True,
                'fx_fog': True, 'fx_ssao': True, 'engine_cvars': '', 'perf_culling': True,
                'perf_record_thread': True, 'perf_dma': True, 'gpu_submit': 'auto', 'library_dirs': [], 'keys': {},
                'auto_setup': True, 'dlc_dir': ''}
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
           ('fx_ssao', ('Oclusão ambiente (SSAO)', 'Ambient occlusion (SSAO)'), 'AO_Mode=0')]
OUTPUTS = [('1280x720', ('1280 × 720 (720p)', '1280 × 720 (720p)')), ('1920x1080', ('1920 × 1080 (1080p)', '1920 × 1080 (1080p)')),
           ('2560x1440', ('2560 × 1440 (1440p)', '2560 × 1440 (1440p)')), ('3840x2160', ('3840 × 2160 (4K)', '3840 × 2160 (4K)'))]
UPSCALERS = [('fsr1', ('FSR 1 (AMD, qualquer placa)', 'FSR 1 (AMD, any GPU)')),
             ('bilinear', ('Bilinear (simples)', 'Bilinear (plain)'))]
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
UI_LANGUAGES = [('', ('Automático', 'Automatic')), ('pt', ('Português', 'Portuguese')), ('en', ('Inglês', 'English'))]
CONTROLS = [('W A S D', ('analógico esquerdo', 'left stick')), (('setas', 'arrows'), ('analógico direito (câmera)', 'right stick (camera)')),
            ('Espaço / Space', '✕'), ('Shift', '○'), ('E', '□'), ('Q', '△'), ('1 / 3', 'L1 / R1'), ('R / F', 'L2 / R2'),
            ('Z / C', 'L3 / R3'), ('Enter', 'Options'), ('I J K L', ('direcional', 'd-pad')),
            ('Tab / Backspace', ('touchpad (esquerda / direita)', 'touchpad (left / right)'))]


def load_json(path, default):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return default


_EBOOT_VERSIONS = {}


def eboot_version(eboot):
    """'01.01' when the eboot holds the EU 1.01 code the modes patch (dumps often keep the base
    game's param.sfo next to the updated eboot), else None. Cached per file state."""
    try:
        stat = eboot.stat()
    except OSError:
        return None
    key = (str(eboot), stat.st_size, stat.st_mtime_ns)
    if key not in _EBOOT_VERSIONS:
        try:
            from prepare import parse_self
            from sotc_modes import is_supported
            elf, _h, ph, _s, _m = parse_self(eboot.read_bytes())
            _EBOOT_VERSIONS[key] = GAME_VERSION if is_supported(elf, ph) else None
        except (OSError, ValueError, ImportError):
            _EBOOT_VERSIONS[key] = None
    return _EBOOT_VERSIONS[key]


def game_info(folder):
    """(title id, game version) of a game folder, or None without eboot.bin."""
    folder = Path(folder or '.')
    if not (folder / 'eboot.bin').is_file():
        return None
    try:
        from prepare import sfo
        values = sfo((folder / 'sce_sys/param.sfo').read_bytes())
        title, version = values.get('TITLE_ID', '?'), values.get('APP_VER', '?')
    except (OSError, ValueError, ImportError):
        title, version = '?', '?'
    return title, eboot_version(folder / 'eboot.bin') or version


def user_dir(s):
    """The data folder (saves in savedata/1/CUSA08809 below it). A folder picked one level too
    deep (savedata itself, or a save folder) is taken back up to the data folder."""
    folder = Path(s.get('user_dir') or PORT_DIR / 'user')
    parts = [p.lower() for p in folder.parts]
    if 'savedata' in parts:
        folder = Path(*folder.parts[:len(parts) - 1 - parts[::-1].index('savedata')])
    return folder


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


def short_path(path):
    """A path for display: relative to the port folder when inside it, else as is."""
    try:
        return str(Path(path).resolve().relative_to(PORT_DIR.resolve()))
    except (ValueError, OSError):
        return str(path)


def game_environment(s):
    env = dict(os.environ)
    env['SOTC_GAME_DIR'] = s['game_dir']
    env['SOTC_USER_DIR'] = str(user_dir(s))
    if s['sys_modules']:
        env['SOTC_SYS_MODULES'] = s['sys_modules']
    if s.get('dlc_dir'):
        env['SOTC_DLC_DIR'] = s['dlc_dir']
    env['SOTC_MODES'] = ','.join(m for m in ('af16', 'fps60') if s.get(m))
    env['BB_FULLSCREEN'] = '1' if s['fullscreen'] else '0'
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
    cvars += str(s.get('engine_cvars', '')).split()
    env['SOTC_CVARS'] = ' '.join(cvars)
    env['SOTC_RECORD_THREAD'] = '1' if s.get('perf_record_thread', True) else '0'
    if not s.get('perf_dma', True):
        env['BB_DIRECT_MEMORY_ACCESS'] = '0'
    # Periodic submits and readbacks ahead: by default only with NVIDIA (vk_rasterizer.cpp).
    if s.get('gpu_submit') == 'on':
        env['SOTC_FLUSH_US'], env['SOTC_RB_AHEAD'] = '1000', '1'
    elif s.get('gpu_submit') == 'off':
        env['SOTC_FLUSH_EVERY'], env['SOTC_RB_AHEAD'] = '0', '0'
    env['SOTC_NEO'] = '1' if s.get('neo') else '0'
    env['SOTC_OUTPUT_RES'] = s.get('output_res') or '1920x1080'
    env['BB_FSR1'] = '1' if s.get('upscaler', 'fsr1') == 'fsr1' else '0'
    if s.get('sharpness') == 'off':
        env['BB_RCAS'] = '0'
    else:
        env['BB_RCAS'] = '1'
        env['BB_RCAS_ATTENUATION'] = str(s.get('sharpness') or '250')
    env['SOTC_MOUSE'] = '1' if s.get('mouse', True) else '0'
    env['SOTC_MOUSE_SENS'] = str(s.get('mouse_sens') or '1.0')
    env['SOTC_MOUSE_INVERT'] = '1' if s.get('mouse_invert') else '0'
    if str(s.get('gamepad', '')).strip():
        env['BB_GAMEPAD'] = str(s['gamepad']).strip()
    env['BB_CONFIG'] = str(PORT_DIR / 'sotc.ini')
    for item in str(s['extra_env']).split():
        if '=' in item:
            key, value = item.split('=', 1)
            env[key] = value
    env['PYTHONUNBUFFERED'] = '1'
    env['PYTHONIOENCODING'] = 'utf-8'
    return env


def shadps4_saves():
    """Save folders of shadPS4 installations next to the port or in the usual places."""
    roots = [PORT_DIR.parent, Path.home(), Path(os.environ.get('APPDATA', Path.home()))]
    found = []
    for root in roots:
        for pattern in ('*/user/home/*/savedata/' + GAME_ID, '*/*/user/home/*/savedata/' + GAME_ID,
                        'shadPS4/savedata/*/' + GAME_ID):
            try:
                found += [p for p in root.glob(pattern) if p.is_dir()]
            except OSError:
                pass
    return found


# ---------------------------------------------------------------------------------------------
# The window.

BG, PANEL, CARD, LINE = '#0d0f10', '#14171a', '#1b1f23', '#2a3036'
TEXT, MUTED, GOLD, ACCENT, ACCENT_HI = '#e6e2da', '#8f9499', '#d7c08a', '#3e5f73', '#4f7790'


class Launcher:
    def __init__(self, root, tk, ttk, filedialog, messagebox):
        self.tk, self.ttk, self.filedialog, self.messagebox = tk, ttk, filedialog, messagebox
        self.root = root
        self.app = {**APP_DEFAULTS, **load_json(CONFIG_FILE, {})}
        self.vars = {}
        self.process = self.job = None
        self.stopping = False
        self.output = queue.Queue()
        self.ui_calls = queue.Queue()
        self.gpu_text = _('• Verificando a placa de vídeo…', '• Checking the graphics card…')
        self.banner_source = self.banner_image = None
        root.title('Shadow of the Colossus — port nativo' if LANG == 'pt' else 'Shadow of the Colossus — native port')
        root.configure(bg=BG)
        self.dpi = root.winfo_fpixels('1i') / 96.0
        root.geometry(f'{self.px(1080)}x{self.px(700)}')
        root.minsize(self.px(940), self.px(620))
        self.set_icon()
        self.style()
        self.build()
        self.show('play')
        self.refresh_library()
        # First start, or the game folder is gone: the setup screen asks for it.
        if not self.app.get('setup_done') or not game_info(self.var('game_dir').get()):
            root.after(50, self.show_setup)
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.drain_output)
        threading.Thread(target=self.detect_gpu, daemon=True).start()

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
    def check_images(self):
        """16 px (DPI-scaled) check box images with a tick: the clam theme draws a cross."""
        n = max(14, int(16 * self.dpi))
        images = []
        for checked in (False, True):
            image = self.tk.PhotoImage(width=n + self.px(8), height=n)  # unset pixels stay transparent
            image.put(GOLD if checked else '#4a535b', to=(0, 0, n, n))
            image.put(ACCENT if checked else CARD, to=(1, 1, n - 1, n - 1))
            if checked:
                x0, y0, x1, y1, x2, y2 = .22 * n, .52 * n, .42 * n, .72 * n, .78 * n, .28 * n
                width = max(1, round(n / 9))
                for t in range(40):
                    f = t / 39
                    for x, y in ((x0 + (x1 - x0) * f, y0 + (y1 - y0) * f), (x1 + (x2 - x1) * f, y1 + (y2 - y1) * f)):
                        image.put('#f4f1ea', to=(int(x), int(y), int(x) + width, int(y) + width))
            images.append(image)
        self.check_off, self.check_on = images

    def style(self):
        ttk = self.ttk
        s = ttk.Style(self.root)
        s.theme_use('clam')
        self.check_images()
        s.element_create('Sotc.indicator', 'image', self.check_off, ('selected', self.check_on), sticky='')
        s.layout('TCheckbutton', [('Checkbutton.padding', {'sticky': 'nswe', 'children': [
            ('Sotc.indicator', {'side': 'left', 'sticky': ''}),
            ('Checkbutton.focus', {'side': 'left', 'sticky': 'w', 'children': [
                ('Checkbutton.label', {'sticky': 'nswe'})]})]})])
        base = ('Segoe UI', 10)
        self.root.option_add('*TCombobox*Listbox.background', CARD)
        self.root.option_add('*TCombobox*Listbox.foreground', TEXT)
        self.root.option_add('*TCombobox*Listbox.selectBackground', ACCENT)
        self.root.option_add('*TCombobox*Listbox.font', base)
        s.configure('.', background=PANEL, foreground=TEXT, fieldbackground=CARD, bordercolor=LINE,
                    lightcolor=LINE, darkcolor=LINE, troughcolor=CARD, focuscolor=GOLD, font=base)
        s.configure('TFrame', background=PANEL)
        s.configure('TLabel', background=PANEL, foreground=TEXT)
        s.configure('Muted.TLabel', background=PANEL, foreground=MUTED, font=('Segoe UI', 9))
        s.configure('Section.TLabel', background=PANEL, foreground=GOLD, font=('Georgia', 13))
        s.configure('TCheckbutton', background=PANEL, foreground=TEXT, padding=(0, 3), indicatorbackground=CARD,
                    indicatorforeground=GOLD)
        s.map('TCheckbutton', background=[('active', PANEL)], indicatorbackground=[('selected', ACCENT)])
        s.configure('Warning.TLabel', background='#2a2312', foreground='#e3c25a', padding=(10, 6))
        s.configure('TCombobox', arrowcolor=GOLD, foreground=TEXT, padding=4)
        s.map('TCombobox', fieldbackground=[('readonly', CARD)], foreground=[('readonly', TEXT)],
              selectbackground=[('readonly', CARD)], selectforeground=[('readonly', TEXT)])
        s.configure('TEntry', foreground=TEXT, insertcolor=TEXT, padding=4)
        s.configure('TButton', background=CARD, foreground=TEXT, padding=(12, 6), borderwidth=1)
        s.map('TButton', background=[('active', LINE), ('disabled', PANEL)], foreground=[('disabled', MUTED)])
        s.configure('Play.TButton', background=ACCENT, foreground='#f4f1ea', font=('Georgia', 15, 'bold'),
                    padding=(36, 10), borderwidth=0)
        s.map('Play.TButton', background=[('active', ACCENT_HI), ('disabled', '#263038')],
              foreground=[('disabled', '#7d858c')])
        s.configure('Vertical.TScrollbar', background=CARD, arrowcolor=GOLD, troughcolor=PANEL, bordercolor=PANEL)

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
        return box

    def next_row(self, parent):
        return parent.grid_size()[1]

    def row(self, parent, title, widget, hint=None):
        ttk = self.ttk
        r = self.next_row(parent)
        ttk.Label(parent, text=title).grid(row=r, column=0, sticky='nw', padx=(0, 18), pady=(8, 0))
        widget.grid(row=r, column=1, sticky='w', pady=(5, 0))
        if hint:
            ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(560), justify='left').grid(
                row=r + 1, column=1, sticky='w', pady=(2, 2))
        return widget

    def check(self, parent, key, title, hint=None):
        ttk = self.ttk
        r = self.next_row(parent)
        ttk.Checkbutton(parent, text=title, variable=self.var(key)).grid(row=r, column=0, columnspan=2, sticky='w', pady=(4, 0))
        if hint:
            ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(640), justify='left').grid(
                row=r + 1, column=0, columnspan=2, sticky='w', padx=(26, 0))

    def note(self, parent, text, top=12):
        self.ttk.Label(parent, text=text, style='Muted.TLabel', wraplength=self.px(680), justify='left').grid(
            row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 0))

    def section(self, parent, title, top=18):
        self.ttk.Label(parent, text=title, style='Section.TLabel').grid(
            row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 2))

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
        return self.row(parent, title, holder, hint)

    def scrolled_page(self, name, title, subtitle):
        tk, ttk = self.tk, self.ttk
        outer = ttk.Frame(self.content)
        head = ttk.Frame(outer, padding=(28, 22, 28, 6))
        head.pack(fill='x')
        ttk.Label(head, text=title, background=PANEL, foreground=TEXT, font=('Georgia', 20)).pack(anchor='w')
        ttk.Label(head, text=subtitle, style='Muted.TLabel').pack(anchor='w')
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
        self.pages[name] = outer
        return inner

    # ---- layout ------------------------------------------------------------------------------
    def build(self):
        tk, ttk = self.tk, self.ttk
        side = tk.Frame(self.root, bg=BG, width=self.px(220))
        side.pack(side='left', fill='y')
        side.pack_propagate(False)
        tk.Label(side, text='SHADOW OF\nTHE COLOSSUS', bg=BG, fg=GOLD, font=('Georgia', 15), justify='left').pack(
            anchor='w', padx=22, pady=(24, 0))
        tk.Label(side, text=_('port nativo · Windows', 'native port · Windows') + f'  ·  v{VERSION}', bg=BG, fg=MUTED,
                 font=('Segoe UI', 9)).pack(anchor='w', padx=22, pady=(2, 20))
        self.nav, self.current_page = {}, None
        for name, title in (('play', _('Jogar', 'Play')), ('game', _('Jogo e saves', 'Game & saves')),
                            ('graphics', _('Imagem e FPS', 'Display & FPS')), ('controls', _('Controles', 'Controls')),
                            ('advanced', _('Avançado', 'Advanced'))):
            item = tk.Label(side, text='    ' + title, bg=BG, fg=TEXT, anchor='w', font=('Segoe UI', 11),
                            pady=10, cursor='hand2')
            item.pack(fill='x')
            item.bind('<Button-1>', lambda _e, n=name: self.show(n))
            item.bind('<Enter>', lambda _e, n=name: n != self.current_page and self.nav[n].configure(bg='#171b1f'))
            item.bind('<Leave>', lambda _e, n=name: n != self.current_page and self.nav[n].configure(bg=BG))
            self.nav[name] = item
        tk.Frame(side, bg=BG).pack(fill='both', expand=True)
        for text, command in ((_('Registro do jogo', 'Game log'), lambda: self.show('log')),
                              (_('Configurar de novo', 'Set up again'), self.show_setup)):
            link = tk.Label(side, text=text, bg=BG, fg=MUTED, font=('Segoe UI', 9, 'underline'), cursor='hand2')
            link.pack(anchor='w', padx=22, pady=(0, 6))
            link.bind('<Button-1>', lambda _e, c=command: c())
        tk.Frame(side, bg=BG, height=12).pack()

        right = tk.Frame(self.root, bg=PANEL)
        right.pack(side='left', fill='both', expand=True)
        bar = tk.Frame(right, bg=BG, height=72)
        bar.pack(fill='x', side='bottom')
        bar.pack_propagate(False)
        self.status = tk.Label(bar, text='', bg=BG, fg=MUTED, font=('Segoe UI', 10), anchor='w', justify='left')
        self.status.pack(side='left', padx=24)
        self.play_button = ttk.Button(bar, text=_('JOGAR', 'PLAY'), style='Play.TButton', command=self.play)
        self.play_button.pack(side='right', padx=(10, 24), pady=11)
        self.stop_button = ttk.Button(bar, text=_('Parar', 'Stop'), command=self.stop, state='disabled')
        self.stop_button.pack(side='right', pady=11)
        self.content = tk.Frame(right, bg=PANEL)
        self.content.pack(fill='both', expand=True)

        self.pages = {}
        self.build_play()
        self.build_game()
        self.build_graphics()
        self.build_controls()
        self.build_advanced()
        self.build_log()
        self.root.bind_all('<MouseWheel>', self.wheel)
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
        for n, item in self.nav.items():
            item.configure(bg=PANEL if n == name else BG, fg=GOLD if n == name else TEXT)
        if name == 'play':
            self.refresh_status()

    def build_play(self):
        tk, ttk = self.tk, self.ttk
        page = tk.Frame(self.content, bg=PANEL)
        self.pages['play'] = page
        self.banner = tk.Canvas(page, height=self.px(290), bg=BG, highlightthickness=0, bd=0)
        self.banner.pack(fill='x')
        self.banner.bind('<Configure>', lambda _e: self.draw_banner())
        body = ttk.Frame(page, padding=(28, 12, 28, 8))
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
        self.row(quick, _('Saída', 'Output'), self.choice(quick, 'output_res', OUTPUTS, 24))
        self.row(quick, _('Upscaler', 'Upscaler'), self.choice(quick, 'upscaler', UPSCALERS, 24))
        self.check(quick, 'fps60', _('60 FPS (experimental)', '60 FPS (experimental)'))
        self.check(quick, 'af16', _('Texturas nítidas (filtro 16x)', 'Sharp textures (16x filtering)'))
        self.check(quick, 'fullscreen', _('Tela cheia', 'Fullscreen'))
        for key in ('af16', 'fps60', 'fullscreen', 'output_res', 'upscaler'):
            self.vars[key].trace_add('write', lambda *_a: self.refresh_status())

    def draw_banner(self):
        """The dump's own art (sce_sys/pic1.png) under the title."""
        tk, c = self.tk, self.banner
        c.delete('all')
        w, h = max(c.winfo_width(), 400), int(c['height'])
        art = Path(self.var('game_dir').get() or '.') / 'sce_sys' / 'pic1.png'
        if self.banner_source != art:
            self.banner_source, self.banner_image, self.banner_size = art, None, None
            try:
                self.banner_image = tk.PhotoImage(file=str(art))
            except (tk.TclError, OSError):
                pass
        if self.banner_image:
            if self.banner_size != w:
                self.banner_size, self.banner_scaled = w, self.scaled_art(art, w)
            c.create_image(w // 2, int(h * 0.5), image=self.banner_scaled)
            for i, stipple in enumerate(('gray12', 'gray25', 'gray50', 'gray75')):
                c.create_rectangle(0, h - 120 + i * 24, w, h - 96 + i * 24, fill=PANEL, outline='', stipple=stipple)
            c.create_rectangle(0, h - 24, w, h, fill=PANEL, outline='')
        else:
            c.create_text(30, h - 80, text='Shadow of the Colossus', anchor='w', fill='#f2ead9', font=('Georgia', 32))
            c.create_text(w // 2, h // 2 - 40, text=_('Escolha a pasta do jogo (Jogo e saves)',
                                                       'Choose your game folder (Game & saves)'),
                          fill=MUTED, font=('Segoe UI', 11))
        info = game_info(self.var('game_dir').get())
        sub = (_('{} · versão do jogo {}', '{} · game version {}').format(*info) if info
               else _('Pasta do jogo não escolhida', 'Game folder not set'))
        c.create_text(33, h - 30, text=sub, anchor='w', fill=GOLD, font=('Segoe UI', 11))

    def scaled_art(self, path, width):
        try:
            from PIL import Image, ImageTk
            with Image.open(path) as image:
                height = round(image.height * width / image.width)
                return ImageTk.PhotoImage(image.convert('RGB').resize((width, height), Image.LANCZOS))
        except (ImportError, OSError):
            factor = max(1, round(self.banner_image.width() / max(width, 1)))
            return self.banner_image.subsample(factor) if factor > 1 else self.banner_image

    def build_game(self):
        ttk = self.ttk
        f = self.scrolled_page('game', _('Jogo e saves', 'Game & saves'),
                               _('O seu dump do jogo, os módulos de som do seu console e a pasta dos saves.',
                                 'Your game dump, your console\'s sound modules and the saves folder.'))
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
                    _('O dump CUSA08809 (versão europeia 1.01), com eboot.bin, sce_module e sce_sys.',
                      'The CUSA08809 dump (EU version 1.01) with eboot.bin, sce_module and sce_sys.'),
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
        self.section(f, _('DLC', 'DLC'))
        self.folder(f, 'dlc_dir', _('Pasta de DLC', 'DLC folder'), _('Pasta com as DLCs do jogo', "Folder with the game's DLC"),
                    _('Como no shadPS4: uma pasta com uma subpasta por DLC (cada uma com sce_sys/param.sfo), ou a '
                      'pasta addcont do shadPS4 (que tem a subpasta CUSA08809). Vazio = sem DLC.',
                      "As in shadPS4: a folder with one subfolder per DLC (each with sce_sys/param.sfo), or "
                      "shadPS4's addcont folder (which holds the CUSA08809 subfolder). Empty = no DLC."),
                    on_change=self.refresh_status)
        self.section(f, _('Saves', 'Saves'))
        self.folder(f, 'user_dir', _('Pasta de dados', 'Data folder'), _('Pasta dos saves', 'Saves folder'),
                    _('Saves, cache de shaders e capturas. Vazio = a pasta user ao lado do port.',
                      'Saves, shader cache and screenshots. Empty = the user folder next to the port.'),
                    on_change=self.refresh_status)
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(10, 0))
        ttk.Button(holder, text=_('Importar save do shadPS4…', 'Import a shadPS4 save…'), command=self.import_save).pack(side='left')
        ttk.Label(holder, text=_('Copia a pasta savedata/CUSA08809 do shadPS4 para cá.',
                                 'Copies shadPS4\'s savedata/CUSA08809 folder here.'), style='Muted.TLabel').pack(side='left', padx=10)
        self.section(f, _('Sistema', 'System'))
        self.row(f, _('Idioma do sistema', 'System language'), self.choice(f, 'language', LANGUAGES),
                 _('O idioma que o jogo vê no "console".', 'The language the game sees on the "console".'))
        self.row(f, _('Nome do jogador', 'Player name'), ttk.Entry(f, textvariable=self.var('player_name'), width=30))

    def build_graphics(self):
        f = self.scrolled_page('graphics', _('Imagem e FPS', 'Display & FPS'),
                               _('Os modos vêm do fork shadPS4 SotC e valem para a versão EU 1.01.',
                                 'The modes come from the shadPS4 SotC fork and apply to EU version 1.01.'))
        self.section(f, _('Modos', 'Modes'), top=4)
        self.check(f, 'af16', _('Texturas nítidas (filtro anisotrópico 16x)', 'Sharp textures (16x anisotropic filtering)'),
                   _('Chão, grama e rochas ficam nítidos à distância, quase sem custo.',
                     'Ground, grass and rocks stay sharp at a distance, at almost no cost.'))
        self.check(f, 'fps60', _('60 FPS (experimental)', '60 FPS (experimental)'),
                   _('Tira o limite de 30 FPS do jogo; o vblank vai a 120 Hz. Mediu 38 FPS no campo e ~25 no templo.',
                     'Removes the game\'s 30 FPS cap; vblank at 120 Hz. Measured 38 FPS in the open world, ~25 at the temple.'))
        self.section(f, _('Resolução', 'Resolution'))
        self.row(f, _('Saída', 'Output'), self.choice(f, 'output_res', OUTPUTS),
                 _('O jogo renderiza em 1080p, como no PS4; a imagem é ampliada ou reduzida para a saída. '
                   'Num monitor menor que a saída, a janela cabe na tela e a tela cheia usa o monitor todo.',
                   'The game renders at 1080p, as on PS4; the image is scaled to the output. On a monitor '
                   'smaller than the output the window fits the screen and fullscreen uses the whole monitor.'))
        self.row(f, _('Upscaler', 'Upscaler'), self.choice(f, 'upscaler', UPSCALERS),
                 _('FSR 1 amplia com bordas nítidas (acima de 1080p).', 'FSR 1 upscales with sharp edges (above 1080p).'))
        self.row(f, _('Nitidez (RCAS)', 'Sharpening (RCAS)'), self.choice(f, 'sharpness', SHARPNESS))
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
                     'Ao trocar, os shaders são compilados de novo na primeira vez.',
                     'Lets shaders read tables straight from game memory (as Pink-shadPS4 does): the lake ripples '
                     'and reflects far away. Off gains ~10% FPS, but distant water is flatter. After a change '
                     'the shaders are compiled again the first time.'))
        self.row(f, _('Envio de trabalho à GPU', 'GPU work submission'), self.choice(f, 'gpu_submit', GPU_SUBMIT),
                 _('Manda trabalho à GPU a cada 0,5 ms e lê resultados por um atalho (técnicas do Pink-shadPS4). '
                   'Medido em NVIDIA (+10–15%); em AMD/Intel use "Ligado" para testar e volte para "Automático" '
                   'se a tela ficar preta.',
                   'Sends work to the GPU every 0.5 ms and reads results through a shortcut (Pink-shadPS4 '
                   'techniques). Measured on NVIDIA (+10–15%); on AMD/Intel pick "On" to try it and go back to '
                   '"Automatic" if the screen turns black.'))
        self.section(f, _('Efeitos', 'Effects'))
        for key, title, _off in EFFECTS:
            self.check(f, key, _(*title))
        self.note(f, _('O motion blur, o brilho e os filtros ficam no menu do próprio jogo (Opções > Apresentação).',
                       "Motion blur, brightness and filters are in the game's own menu (Options > Presentation)."), top=4)
        self.section(f, _('Tela', 'Display'))
        self.check(f, 'fullscreen', _('Tela cheia', 'Fullscreen'))
        self.row(f, _('Apresentação', 'Presentation'), self.choice(f, 'present_mode', PRESENT_MODES))

    def build_controls(self):
        ttk = self.ttk
        f = self.scrolled_page('controls', _('Controles', 'Controls'),
                               _('Um controle (DualShock 4, DualSense, Xbox…) é usado automaticamente.',
                                 'A gamepad (DualShock 4, DualSense, Xbox…) is used automatically.'))
        self.section(f, _('Controle', 'Gamepad'), top=4)
        self.row(f, _('Controle preferido', 'Preferred gamepad'), ttk.Entry(f, textvariable=self.var('gamepad'), width=30),
                 _('Nome (ou parte) do controle a usar quando houver vários; vazio = o primeiro.',
                   'Name (or part of it) of the gamepad to use when several are connected; empty = the first.'))
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
                  font=('Segoe UI', 12)).pack(padx=30, pady=(24, 6))
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

    def build_advanced(self):
        ttk = self.ttk
        f = self.scrolled_page('advanced', _('Avançado', 'Advanced'),
                               _('Opções do lançador e diagnóstico.', 'Launcher options and diagnostics.'))
        self.section(f, _('Lançador', 'Launcher'), top=4)
        self.row(f, _('Idioma do lançador', 'Launcher language'), self.choice(f, 'ui_language', UI_LANGUAGES),
                 _('Vale na próxima vez que o lançador abrir.', 'Applies when the launcher opens again.'))
        self.check(f, 'close_on_play', _('Fechar o lançador quando o jogo abrir', 'Close the launcher when the game starts'))
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(10, 0))
        ttk.Button(holder, text=_('Atalho na área de trabalho', 'Desktop shortcut'), command=self.shortcut).pack(side='left')
        ttk.Button(holder, text=_('Pasta do port', 'Port folder'), command=lambda: self.open_path(PORT_DIR)).pack(side='left', padx=6)
        ttk.Button(holder, text=_('Limpar cache de shaders', 'Clear shader cache'), command=self.clear_cache).pack(side='left')
        self.section(f, _('Diagnóstico', 'Diagnostics'))
        self.check(f, 'show_fps', _('FPS no registro (a cada 5 s)', 'FPS in the log (every 5 s)'))
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
        ttk.Label(top, text=_('Registro', 'Log'), font=('Georgia', 20)).pack(side='left')
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
        elif info == (GAME_ID, GAME_VERSION):
            game = _('✓ {} versão {}: todos os modos disponíveis', '✓ {} version {}: every mode available').format(*info)
        else:
            game = _('⚠ {} versão {}: o port foi feito para {} {}; os modos não se aplicam',
                     '⚠ {} version {}: the port targets {} {}; the modes do not apply').format(*info, GAME_ID, GAME_VERSION)
        modules = Path(self.var('sys_modules').get() or '.')
        if self.var('sys_modules').get() and all((modules / m).is_file() for m in SYS_MODULES):
            sound = _('✓ Módulos de som encontrados', '✓ Sound modules found')
        else:
            sound = _('⚠ Sem os módulos de som do seu console: o jogo fica mudo (Jogo e saves)',
                      '⚠ No sound modules from your console: the game is silent (Game & saves)')
        user = user_dir({'user_dir': self.var('user_dir').get()})
        saves = user / 'savedata' / '1' / GAME_ID
        save = (_('✓ Saves em {}', '✓ Saves in {}').format(saves) if saves.is_dir() and any(saves.iterdir())
                else _('• Ainda sem saves: o jogo cria em {}', '• No saves yet: the game creates them in {}').format(saves))
        from sotc_discovery import dlc_count
        count = dlc_count(self.var('dlc_dir').get())
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
        parts = [_('60 FPS', '60 FPS') if self.var('fps60').get() else _('30 FPS (como no PS4)', '30 FPS (as on PS4)'),
                 self.var('output_res').get().replace('x', ' × '),
                 'FSR 1' if self.var('upscaler').get() == 'fsr1' else 'bilinear']
        if self.var('af16').get():
            parts.append(_('AF 16x', '16x AF'))
        if self.var('neo').get():
            parts.append('PS4 Pro')
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
                text = _('✓ Placa de vídeo: {}', '✓ Graphics card: {}').format(names[0])
            elif result.returncode:
                text = _('✗ Nenhuma placa com Vulkan 1.3 (atualize o driver)',
                         '✗ No Vulkan 1.3 graphics card found (update the driver)')
        except (OSError, subprocess.TimeoutExpired):
            pass
        self.gpu_text = text
        self.ui_calls.put(self.refresh_status)

    def collect(self):
        for key, var in self.vars.items():
            self.app[key] = var.get()
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        CONFIG_FILE.write_text(json.dumps(self.app, indent=2, ensure_ascii=False), encoding='utf-8')

    def import_save(self):
        found = shadps4_saves()
        initial = str(found[0]) if found else str(PORT_DIR.parent)
        chosen = self.filedialog.askdirectory(title=_('Pasta savedata/CUSA08809 do shadPS4', "shadPS4's savedata/CUSA08809 folder"),
                                              initialdir=initial)
        if not chosen:
            return
        source = Path(chosen)
        if source.name != GAME_ID and (source / GAME_ID).is_dir():
            source = source / GAME_ID
        target = user_dir({'user_dir': self.var('user_dir').get()}) / 'savedata' / '1' / GAME_ID
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

    # ---- first start: the setup screen -------------------------------------------------------
    def show_setup(self):
        """A full-window welcome screen: asks for the game folder, then sets up DLC, saves and
        sound on its own and shows what it found."""
        tk, ttk = self.tk, self.ttk
        if getattr(self, 'setup_frame', None):
            self.setup_frame.destroy()
        frame = self.setup_frame = tk.Frame(self.root, bg=BG)
        frame.place(relx=0, rely=0, relwidth=1, relheight=1)
        frame.lift()
        inner = tk.Frame(frame, bg=BG)
        inner.place(relx=0.5, rely=0.45, anchor='center')
        tk.Label(inner, text='SHADOW OF THE COLOSSUS', bg=BG, fg=GOLD, font=('Georgia', 26)).pack()
        tk.Label(inner, text=_('Bem-vindo. Só falta dizer onde está o jogo.',
                               'Welcome. Just tell us where the game is.'),
                 bg=BG, fg=TEXT, font=('Segoe UI', 13)).pack(pady=(8, 26))
        self.setup_button = ttk.Button(inner, text=_('Escolher a pasta do jogo…', 'Choose the game folder…'),
                                       style='Play.TButton', command=self.choose_game_for_setup)
        self.setup_button.pack(ipadx=18)
        tk.Label(inner, text=_('A pasta principal do jogo, como no shadPS4: as subpastas do jogo (CUSA08809), da '
                               'atualização (-UPDATE/-patch) e das DLCs são encontradas sozinhas.',
                               "The game's main folder, as in shadPS4: the game (CUSA08809), update (-UPDATE/-patch) "
                               'and DLC subfolders are found on their own.'),
                 bg=BG, fg=MUTED, font=('Segoe UI', 10), wraplength=self.px(560), justify='center').pack(pady=(10, 0))
        self.setup_hint = tk.Label(inner, text='', bg=BG, fg=MUTED, font=('Segoe UI', 10), cursor='hand2',
                                   wraplength=self.px(640), justify='center')
        self.setup_hint.pack(pady=(18, 0))
        self.setup_results = tk.Frame(inner, bg=BG)
        self.setup_results.pack(pady=(18, 0), fill='x')
        self.setup_found_game = None
        # Meanwhile look for the game on this PC, to offer it with one click.
        current = self.var('game_dir').get()
        if game_info(current):
            self.offer_game(current)
        else:
            self.setup_hint.configure(text=_('Procurando o jogo neste PC…', 'Looking for the game on this PC…'))

            def work():
                try:
                    from sotc_discovery import scan
                    games = scan(seconds=25, extra_roots=[str(PORT_DIR.parent)])['games']
                except Exception:  # never stop the launcher over a search problem
                    games = []
                self.ui_calls.put(lambda: self.offer_game(games[0]['path']) if games else
                                  self.setup_hint.configure(text=''))
            threading.Thread(target=work, daemon=True).start()

    def offer_game(self, path):
        if not getattr(self, 'setup_hint', None) or not self.setup_hint.winfo_exists():
            return
        self.setup_found_game = path
        self.setup_hint.configure(text=_('Encontrado: {}   —   clique aqui para usar', 'Found: {}   —   click here to use it')
                                  .format(path), fg=GOLD)
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
                'Não achei o jogo (eboot.bin do CUSA08809) em\n{}\n\nEscolha a pasta do seu dump.',
                'The game (eboot.bin of CUSA08809) is not in\n{}\n\nChoose the folder of your dump.').format(chosen))
            return
        self.configure_from_game(game, chosen)

    def configure_from_game(self, game, chosen=None):
        """Game chosen: DLC from the game folder, saves, sound — in the background."""
        self.setup_button.configure(state='disabled')
        self.setup_hint.unbind('<Button-1>')
        self.setup_hint.configure(text=_('Configurando…', 'Setting up…'), fg=MUTED)
        user = user_dir({'user_dir': self.var('user_dir').get()})

        def work():
            from sotc_discovery import find_dlc_root, find_update, dlc_count, scan, SYS_MODULES as modules
            result = {'game': game, 'dlc': find_dlc_root(game, chosen), 'update': find_update(game), 'sound': None,
                      'save': None}
            result['dlc_count'] = dlc_count(result['dlc'])
            sound_ready = self.app.get('sys_modules') and all(
                (Path(self.app['sys_modules']) / m).is_file() for m in modules)
            saves = user / 'savedata' / '1' / GAME_ID
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
                outside = [p for p in found['saves'] if not Path(p).resolve().is_relative_to(own)]
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
        for mark, text in lines:
            color = GOLD if mark == '✓' else '#d9b441' if mark == '⚠' else MUTED
            row = tk.Frame(self.setup_results, bg=BG)
            row.pack(anchor='w', pady=2)
            tk.Label(row, text=mark, bg=BG, fg=color, font=('Segoe UI', 12), width=2).pack(side='left')
            tk.Label(row, text=text, bg=BG, fg=TEXT, font=('Segoe UI', 11)).pack(side='left')
        buttons = tk.Frame(self.setup_results, bg=BG)
        buttons.pack(pady=(20, 0))
        if not result['sound']:
            ttk.Button(buttons, text=_('Escolher a pasta do som…', 'Choose the sound folder…'),
                       command=self.choose_sound_for_setup).pack(side='left', padx=(0, 8))
        ttk.Button(buttons, text=_('Começar', 'Start'), style='Play.TButton', command=self.finish_setup).pack(side='left')
        self.setup_button.configure(state='normal', text=_('Trocar a pasta do jogo…', 'Change the game folder…'))
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
        return user_dir({'user_dir': self.var('user_dir').get()}) / 'savedata' / '1' / GAME_ID

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
            best = min(found['games'],
                       key=lambda g: eboot_version(Path(g['path']) / 'eboot.bin') != GAME_VERSION)
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
        if not self.var('dlc_dir').get() and found.get('dlc'):
            self.var('dlc_dir').set(found['dlc'][0])
            done.append(_('DLC em {}', 'DLC in {}').format(found['dlc'][0]))
        if not self.has_saves():
            own = user_dir({'user_dir': self.var('user_dir').get()}).resolve()
            outside = [p for p in found['saves'] if not Path(p).resolve().is_relative_to(own)]
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
        if self.process:
            return
        self.collect()
        if not game_info(self.app['game_dir']):
            self.messagebox.showerror('Shadow of the Colossus', _('Escolha a pasta do jogo com eboot.bin (CUSA08809).',
                                                                  'Choose the game folder with eboot.bin (CUSA08809).'))
            self.show('game')
            return
        self.set_log('')
        try:
            write_key_bindings(self.app.get('keys'))
        except OSError as error:
            self.append(_('Não foi possível gravar as teclas no sotc.ini: {}',
                          'Could not write the keys to sotc.ini: {}').format(error) + '\n')
        try:
            self.process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(self.app),
                                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        except OSError as error:
            self.append(_('Não foi possível iniciar: {}', 'Could not start: {}').format(error) + '\n')
            self.process = None
            return
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
        self.stopping = False
        self.output.put((code,))

    def drain_output(self):
        while not self.ui_calls.empty():
            self.ui_calls.get_nowait()()
        try:
            for _i in range(500):
                item = self.output.get_nowait()
                if isinstance(item, tuple):
                    self.append(_('\n— o jogo terminou (código {}) —\n', '\n— the game exited (code {}) —\n').format(item[0]))
                    self.process = None
                    if self.job:
                        self.job.close()
                    self.stop_button.configure(state='disabled')
                    self.refresh_status()
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
            return
        for name in ('cache', 'shader'):
            shutil.rmtree(user_dir(self.app) / name, ignore_errors=True)
        self.messagebox.showinfo('Shadow of the Colossus', _(
            'Cache de shaders limpo. A próxima partida engasga alguns minutos enquanto ele é refeito.',
            'Shader cache cleared. The next start stutters for a few minutes while it is rebuilt.'))

    def shortcut(self):
        self.collect()
        game_icon = self.game_icon_file()
        if FROZEN:
            target, arguments, icon = sys.executable, '', game_icon or f'{sys.executable},0'
        else:
            pythonw = Path(sys.executable).with_name('pythonw.exe')
            target = str(pythonw if pythonw.exists() else sys.executable)
            arguments, icon = Path(__file__).resolve(), PORT_DIR / 'launcher' / 'sotc.ico'
        script = ('$s=(New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath("Desktop")'
                  '+"\\Shadow of the Colossus.lnk");'
                  f'$s.TargetPath="{target}";$s.WorkingDirectory="{PORT_DIR}";$s.IconLocation="{icon}";'
                  + (f"$s.Arguments='\"{arguments}\"';" if arguments else '') + '$s.Save()')
        result = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True,
                                creationflags=NO_WINDOW)
        if result.returncode == 0:
            self.messagebox.showinfo('Shadow of the Colossus', _('Atalho criado na área de trabalho.',
                                                                 'Shortcut created on the desktop.'))
        else:
            self.messagebox.showerror('Shadow of the Colossus', result.stderr.decode(errors='replace')[:400])

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


def play_without_window(settings):
    """--play: the game with the saved settings (shortcuts, Steam); output to user/last_run.log."""
    attach_stdio()
    log_dir = user_dir(settings)
    log_dir.mkdir(parents=True, exist_ok=True)
    try:
        write_key_bindings(settings.get('keys'))
    except OSError:
        pass
    with open_session_log(log_dir) as log:
        process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(settings),
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
    return code


def main():
    global LANG
    args = sys.argv[1:]
    if args and args[0] in ('--run', '--script'):
        sys.exit(run_role(args))
    settings = {**APP_DEFAULTS, **load_json(CONFIG_FILE, {})}
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
