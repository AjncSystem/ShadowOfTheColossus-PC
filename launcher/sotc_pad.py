# SPDX-License-Identifier: GPL-2.0-or-later
"""The controllers as the game sees them, and the controller test on the Controls page. From the
bloodborne_pc launcher (bbport_pad.py, github.com/Supermedo/bloodborne_pc, GPL-2.0).

The port's own SDL3 (bin/SDL3.dll) reads them: the same drivers and the same button mapping as the
game (gpu/shim/window.cpp, src/runtime_pad.c). Only the gamepad part of SDL is started, no window;
the launcher reads while the Controls page is shown and stops when it is left or the game starts."""
import ctypes
from pathlib import Path

import sotc_theme as theme


def _(pt, en):  # replaced by the launcher's translation (sotc_launcher_win)
    return pt


INIT_GAMEPAD = 0x00002000
# SDL_GamepadButton and SDL_GamepadAxis, in SDL's order
BUTTONS = ['south', 'east', 'west', 'north', 'back', 'guide', 'start', 'left_stick', 'right_stick',
           'left_shoulder', 'right_shoulder', 'dpad_up', 'dpad_down', 'dpad_left', 'dpad_right', 'misc1',
           'right_paddle1', 'left_paddle1', 'right_paddle2', 'left_paddle2', 'touchpad']
AXES = ['left_x', 'left_y', 'right_x', 'right_y', 'left_trigger', 'right_trigger']
# SDL_GamepadType: PS3/PS4/PS5 and the Switch controllers; every other one has Xbox buttons
KINDS = {4: 'playstation', 5: 'playstation', 6: 'playstation', 7: 'switch', 8: 'switch', 9: 'switch', 10: 'switch'}
KIND_NAMES = {'playstation': 'PlayStation', 'xbox': 'Xbox', 'switch': 'Switch'}
_c = ctypes
SIGNATURES = {
    'SDL_SetHint': ((_c.c_char_p, _c.c_char_p), _c.c_bool), 'SDL_Init': ((_c.c_uint32,), _c.c_bool),
    'SDL_QuitSubSystem': ((_c.c_uint32,), None), 'SDL_GetError': ((), _c.c_char_p),
    'SDL_UpdateGamepads': ((), None), 'SDL_GetGamepads': ((_c.POINTER(_c.c_int),), _c.POINTER(_c.c_uint32)),
    'SDL_free': ((_c.c_void_p,), None), 'SDL_OpenGamepad': ((_c.c_uint32,), _c.c_void_p),
    'SDL_CloseGamepad': ((_c.c_void_p,), None), 'SDL_GetGamepadName': ((_c.c_void_p,), _c.c_char_p),
    'SDL_GetGamepadType': ((_c.c_void_p,), _c.c_int), 'SDL_GetGamepadButton': ((_c.c_void_p, _c.c_int), _c.c_bool),
    'SDL_GetGamepadAxis': ((_c.c_void_p, _c.c_int), _c.c_int16),
}


class Pad:
    def __init__(self, ident, name, kind, buttons, axes):
        self.id, self.name, self.kind, self.buttons, self.axes = ident, name, kind, buttons, axes


class Pads:
    """SDL's gamepads; poll() gives the connected ones with their buttons and axes (-1..1, triggers 0..1)."""

    def __init__(self, dll):
        sdl = ctypes.CDLL(str(dll))
        for name, (arguments, result) in SIGNATURES.items():
            function = getattr(sdl, name)
            function.argtypes, function.restype = arguments, result
        self.sdl = sdl
        sdl.SDL_SetHint(b'SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS', b'1')  # the launcher's window has the focus
        if not sdl.SDL_Init(INIT_GAMEPAD):
            raise OSError((sdl.SDL_GetError() or b'SDL_Init failed').decode('utf-8', 'replace'))
        self.open = {}

    def poll(self):
        sdl = self.sdl
        sdl.SDL_UpdateGamepads()
        count = ctypes.c_int()
        found = sdl.SDL_GetGamepads(ctypes.byref(count))
        ids = [found[i] for i in range(count.value)] if found else []
        if found:
            sdl.SDL_free(found)
        for ident in [i for i in self.open if i not in ids]:
            sdl.SDL_CloseGamepad(self.open.pop(ident))
        pads = []
        for ident in ids:
            if ident not in self.open:
                handle = sdl.SDL_OpenGamepad(ident)
                if not handle:
                    continue
                self.open[ident] = handle
            handle = self.open[ident]
            name = (sdl.SDL_GetGamepadName(handle) or b'').decode('utf-8', 'replace')
            kind = KINDS.get(sdl.SDL_GetGamepadType(handle), 'xbox')
            buttons = {b: bool(sdl.SDL_GetGamepadButton(handle, i)) for i, b in enumerate(BUTTONS)}
            axes = {}
            for i, axis in enumerate(AXES):
                value = sdl.SDL_GetGamepadAxis(handle, i)
                axes[axis] = max(0.0, value / 32767) if 'trigger' in axis else max(-1.0, value / 32767)
            pads.append(Pad(ident, name, kind, buttons, axes))
        return pads

    def close(self):
        for handle in self.open.values():
            self.sdl.SDL_CloseGamepad(handle)
        self.open.clear()
        self.sdl.SDL_QuitSubSystem(INIT_GAMEPAD)


def sdl_path(port_dir):
    """The port's SDL3.dll (bin/, or out/ in a build; the MSYS2 tree for a run from the sources)."""
    import os
    candidates = [Path(port_dir) / folder / 'SDL3.dll' for folder in ('bin', 'out')]
    candidates.append(Path(os.environ.get('MSYS2_ROOT', r'I:\msys64')) / 'clang64' / 'bin' / 'SDL3.dll')
    return next((path for path in candidates if path.is_file()), None)


class PadMixin:
    """The controller test's card: the picture of the controller (sotc_pad_art), its name and a way to
    Help. It reads while the Controls page is shown and no game runs (Launcher.show, Launcher.play)."""

    def controller_card(self, parent, port_dir):
        tk, ttk, px = self.tk, self.ttk, self.px
        self.pad_port_dir = port_dir
        card = ttk.Frame(parent, style='Card.TFrame', padding=(px(22), px(16)))
        self.pad_picture = tk.Label(card, bg=theme.CARD, bd=0)
        self.pad_picture.pack(side='left')
        side = tk.Frame(card, bg=theme.CARD)
        side.pack(side='left', fill='x', expand=True, padx=(px(26), 0))
        self.pad_status = tk.Label(side, text='', bg=theme.CARD, fg=theme.TEXT, font=(theme.BODY_SEMI, 11),
                                   anchor='w', justify='left', wraplength=px(300))
        self.pad_status.pack(fill='x')
        tk.Label(side, text=_('Aperte os botões: o que acende aqui chega ao jogo.',
                              'Press the buttons: what lights up here reaches the game.'),
                 bg=theme.CARD, fg=theme.MUTED, font=(theme.BODY, 9), justify='left', anchor='w',
                 wraplength=px(300)).pack(fill='x', pady=(px(8), 0))
        link = tk.Label(side, text=_('O controle não responde', 'The controller does not respond') + '  ›',
                        bg=theme.CARD, fg=theme.GOLD_LIGHT, font=(theme.BODY_MEDIUM, 9), cursor='hand2')
        link.pack(anchor='w', pady=(px(14), 0))
        link.bind('<Button-1>', lambda _e: self.show('help'))
        self.pad_reader, self.pad_job, self.pad_shown, self.pad_kind = None, None, None, 'playstation'
        self.pad_draw(None)
        return card

    def pad_draw(self, pad):
        """The picture for the controller's state (None: at rest); drawn anew only when it changed."""
        kind = pad.kind if pad else self.pad_kind
        buttons = tuple(sorted(b for b, on in pad.buttons.items() if on)) if pad else ()
        axes = tuple(round(pad.axes.get(a, 0.0) * 20) / 20 for a in AXES) if pad else (0.0,) * 6
        state = (kind, buttons, axes)
        if state == self.pad_shown:
            return
        self.pad_shown, self.pad_kind = state, kind
        from PIL import Image, ImageTk
        import sotc_pad_art
        image = sotc_pad_art.render(kind, dict.fromkeys(buttons, True), dict(zip(AXES, axes)), width=self.px(420))
        image = Image.alpha_composite(Image.new('RGBA', image.size, theme.CARD), image)
        self.pad_photo = ImageTk.PhotoImage(image)
        self.pad_picture.configure(image=self.pad_photo)

    def pad_start(self):
        if self.pad_job or not getattr(self, 'pad_picture', None):
            return
        if self.process:
            self.pad_status.configure(text=_('O jogo está aberto: o teste espera ele fechar.',
                                             'The game is running: the test waits until it is closed.'), fg=theme.MUTED)
            return
        if self.pad_reader is None:
            dll = sdl_path(self.pad_port_dir)
            try:
                if not dll:
                    raise OSError(_('falta o SDL3.dll na pasta bin do port.', "SDL3.dll is missing in the port's bin folder."))
                self.pad_reader = Pads(dll)
            except OSError as error:
                self.pad_status.configure(text=_('O teste do controle não pôde começar: {}',
                                                 'The controller test cannot start: {}').format(error), fg=theme.AMBER)
                return
        self.pad_tick()

    def pad_tick(self):
        try:
            pads = self.pad_reader.poll()
        except OSError:
            pads = []
        if pads:
            self.pad_status.configure(text='   ·   '.join(f'{pad.name} ({KIND_NAMES[pad.kind]})' for pad in pads),
                                      fg=theme.TEXT)
            pressed = [pad for pad in pads if any(pad.buttons.values()) or any(abs(v) > 0.2 for v in pad.axes.values())]
            self.pad_draw((pressed or pads)[0])
        else:
            self.pad_status.configure(text=_('Nenhum controle encontrado. Conecte por cabo ou Bluetooth.',
                                             'No controller found. Connect it by cable or Bluetooth.'), fg=theme.MUTED)
            self.pad_draw(None)
        self.pad_job = self.root.after(40, self.pad_tick)

    def pad_stop(self):
        if getattr(self, 'pad_job', None):
            self.root.after_cancel(self.pad_job)
            self.pad_job = None
        if getattr(self, 'pad_reader', None) is not None:
            self.pad_reader.close()  # the controllers are free again (for the game)
            self.pad_reader = None
