# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher's own title bar instead of Windows' caption, on a normal Windows window. From the
bloodborne_pc launcher (bbport_frame.py, github.com/Supermedo/bloodborne_pc, GPL-2.0).

The window keeps its native frame, so moving, Aero Snap, the snap keys, resizing, the shadow, the
taskbar and Alt+Tab stay Windows' own. Only the caption goes: the window answers WM_NCCALCSIZE with
the client area reaching the window's top. The launcher draws the bar (Launcher.build_titlebar) and
the window tells Windows where it is (WM_NCHITTEST): HTCAPTION over the bar, HTTOP… over its top
edge, so Windows itself moves the window (with Snap), maximizes it on a double click, shows the
window menu on a right click and resizes it. The windows Tk puts over that area (its client window
and the bar) answer HTTRANSPARENT there, so Windows asks the frame window.

No Windows loop is ever started from a Tk callback: inside one, a Tk event handler would re-enter
Python without its thread state (tkinter), which ends the process. For the same reason the window
procedure never calls Tk: what it does on a minimize (take_cover) is Windows calls only.

When anything here fails, install() returns False and the launcher keeps Windows' caption."""
import ctypes
import functools
import time
from ctypes import wintypes

WM_NCCALCSIZE, WM_NCHITTEST, WM_NCRBUTTONUP, WM_SYSCOMMAND = 0x0083, 0x0084, 0x00A5, 0x0112
SC_SIZE, SC_MOVE, SC_MINIMIZE, SC_MAXIMIZE, SC_RESTORE = 0xF000, 0xF010, 0xF020, 0xF030, 0xF120
TPM_RETURNCMD = 0x0100
HTTRANSPARENT, HTCLIENT, HTCAPTION, HTTOP, HTTOPLEFT, HTTOPRIGHT = -1, 1, 2, 12, 13, 14
GWLP_WNDPROC = -4
WS_CHILD, WS_VISIBLE, SS_BITMAP, STM_SETIMAGE, IMAGE_BITMAP, SRCCOPY = 0x40000000, 0x10000000, 0x0E, 0x0172, 0, 0x00CC0020
SWP_TOP_SHOWN = 0x0001 | 0x0002 | 0x0040  # no size, no move, show: at the top of its siblings
SWP_FRAMECHANGED = 0x0001 | 0x0002 | 0x0004 | 0x0020  # no size, no move, no z-order, frame changed
WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wintypes.HWND, wintypes.UINT, ctypes.c_size_t, ctypes.c_ssize_t)


class NCCALCSIZE_PARAMS(ctypes.Structure):
    _fields_ = [('rgrc', wintypes.RECT * 3), ('lppos', ctypes.c_void_p)]


@functools.lru_cache(maxsize=1)
def _user32():
    user32 = ctypes.WinDLL('user32')  # its own: the prototypes set here stay out of ctypes.windll
    user32.SetWindowLongPtrW.restype = ctypes.c_void_p
    user32.SetWindowLongPtrW.argtypes = (wintypes.HWND, ctypes.c_int, ctypes.c_void_p)
    user32.CallWindowProcW.restype = ctypes.c_ssize_t
    user32.CallWindowProcW.argtypes = (ctypes.c_void_p, wintypes.HWND, wintypes.UINT, ctypes.c_size_t,
                                       ctypes.c_ssize_t)
    user32.GetSystemMenu.restype = wintypes.HMENU
    user32.TrackPopupMenu.argtypes = (wintypes.HMENU, wintypes.UINT, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                      wintypes.HWND, ctypes.c_void_p)
    user32.EnableMenuItem.argtypes = (wintypes.HMENU, wintypes.UINT, wintypes.UINT)
    user32.PostMessageW.argtypes = (wintypes.HWND, wintypes.UINT, ctypes.c_size_t, ctypes.c_ssize_t)
    user32.GetDC.restype = ctypes.c_void_p
    user32.ReleaseDC.argtypes = (wintypes.HWND, ctypes.c_void_p)
    user32.CreateWindowExW.restype = wintypes.HWND
    user32.CreateWindowExW.argtypes = (wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
                                       ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.HWND,
                                       ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
    user32.SendMessageW.argtypes = (wintypes.HWND, wintypes.UINT, ctypes.c_size_t, ctypes.c_void_p)
    return user32


@functools.lru_cache(maxsize=1)
def _gdi32():
    gdi32 = ctypes.WinDLL('gdi32')
    for name in ('CreateCompatibleDC', 'CreateCompatibleBitmap', 'SelectObject'):
        getattr(gdi32, name).restype = ctypes.c_void_p
    gdi32.CreateCompatibleDC.argtypes = (ctypes.c_void_p,)
    gdi32.CreateCompatibleBitmap.argtypes = (ctypes.c_void_p, ctypes.c_int, ctypes.c_int)
    gdi32.SelectObject.argtypes = (ctypes.c_void_p, ctypes.c_void_p)
    gdi32.BitBlt.argtypes = (ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                             ctypes.c_void_p, ctypes.c_int, ctypes.c_int, wintypes.DWORD)
    gdi32.DeleteDC.argtypes = gdi32.DeleteObject.argtypes = (ctypes.c_void_p,)
    gdi32.GetDIBits.argtypes = (ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_void_p,
                                ctypes.c_void_p, ctypes.c_uint)
    return gdi32


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [('size', wintypes.DWORD), ('w', ctypes.c_long), ('h', ctypes.c_long), ('planes', wintypes.WORD),
                ('bits', wintypes.WORD), ('compression', wintypes.DWORD), ('image', wintypes.DWORD),
                ('x', ctypes.c_long), ('y', ctypes.c_long), ('used', wintypes.DWORD), ('important', wintypes.DWORD)]


def copy_of(hwnd, w, h):
    """A bitmap (a GDI handle, the caller deletes it) of what a window shows on the screen now: the
    pixels Windows holds for it, nothing drawn anew."""
    u, g = _user32(), _gdi32()
    screen = u.GetDC(hwnd)
    memory = g.CreateCompatibleDC(screen)
    bitmap = g.CreateCompatibleBitmap(screen, w, h)
    before = g.SelectObject(memory, bitmap)
    g.BitBlt(memory, 0, 0, w, h, screen, 0, 0, SRCCOPY)
    g.SelectObject(memory, before)
    g.DeleteDC(memory)
    u.ReleaseDC(hwnd, screen)
    return bitmap


def picture(hwnd, w, h):
    """What a window shows on the screen now, as a PIL image; None when it cannot be had."""
    from PIL import Image
    g = _gdi32()
    bitmap = copy_of(hwnd, w, h)
    if not bitmap:
        return None
    try:
        memory = g.CreateCompatibleDC(None)
        pixels = ctypes.create_string_buffer(w * h * 4)
        header = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
        copied = g.GetDIBits(memory, bitmap, 0, h, pixels, ctypes.byref(header), 0)
        g.DeleteDC(memory)
    finally:
        g.DeleteObject(bitmap)
    return Image.frombuffer('RGBA', (w, h), pixels, 'raw', 'BGRA', 0, 1).convert('RGB') if copied == h else None


class Frame:
    """The caption removed from one window. `hwnd` is its frame (Tk's wrapper, root.wm_frame());
    `covers` the windows Tk puts over the bar (the toplevel's client window, the bar)."""

    def __init__(self, hwnd, covers):
        self.hwnd, self.covers = hwnd, list(covers)
        self.user32 = _user32()
        self.procs, self.olds = {}, {}
        self.bar, self.buttons, self.edge, self.corner = 32, 138, 5, 10  # physical pixels, set by the launcher
        self.cover_on_minimize = False  # a picture over the window when it goes to the task bar (take_cover)
        self.cover, self.cover_time = None, 0.0

    def thickness(self):
        """The resize frame Windows keeps off screen around a maximized window."""
        u = self.user32
        try:
            dpi = u.GetDpiForWindow(self.hwnd)
            return u.GetSystemMetricsForDpi(33, dpi) + u.GetSystemMetricsForDpi(92, dpi)  # SM_CYSIZEFRAME, PADDEDBORDER
        except AttributeError:  # before Windows 10 1607
            return u.GetSystemMetrics(33) + u.GetSystemMetrics(92)

    def hit(self, lparam):
        """What a screen point (WM_NCHITTEST's lParam) is on: HTCAPTION, HTTOP…, or HTCLIENT."""
        u = self.user32
        x, y = ctypes.c_short(lparam & 0xFFFF).value, ctypes.c_short((lparam >> 16) & 0xFFFF).value
        origin = wintypes.POINT(0, 0)
        u.ClientToScreen(self.hwnd, ctypes.byref(origin))
        rect = wintypes.RECT()
        u.GetClientRect(self.hwnd, ctypes.byref(rect))
        rx, ry, width = x - origin.x, y - origin.y, rect.right
        if not 0 <= ry < self.bar or not 0 <= rx < width:
            return HTCLIENT
        if ry < self.edge and not u.IsZoomed(self.hwnd):
            return HTTOPLEFT if rx < self.corner else HTTOPRIGHT if rx >= width - self.corner else HTTOP
        return HTCAPTION if rx < width - self.buttons else HTCLIENT

    def wndproc(self, hwnd, msg, wparam, lparam):
        u, old = self.user32, self.olds.get(hwnd)
        try:
            if hwnd == self.hwnd:
                if msg == WM_NCCALCSIZE and wparam:
                    params = NCCALCSIZE_PARAMS.from_address(lparam)
                    top = params.rgrc[0].top
                    result = u.CallWindowProcW(old, hwnd, msg, wparam, lparam)  # the sides and the bottom
                    params.rgrc[0].top = top + (self.thickness() if u.IsZoomed(hwnd) else 0)  # no caption
                    return result
                if msg == WM_NCHITTEST:
                    result = u.CallWindowProcW(old, hwnd, msg, wparam, lparam)
                    return self.hit(lparam) if result == HTCLIENT else result
                if msg == WM_SYSCOMMAND and wparam & 0xFFF0 == SC_MINIMIZE and self.cover_on_minimize:
                    self.take_cover()  # the task bar button, Win+Down; then Windows minimizes as always
                if msg == WM_NCRBUTTONUP and wparam == HTCAPTION:  # Tk does not pass it on to Windows
                    self.window_menu(ctypes.c_short(lparam & 0xFFFF).value, ctypes.c_short((lparam >> 16) & 0xFFFF).value)
                    return 0
            elif msg == WM_NCHITTEST and self.hit(lparam) != HTCLIENT:
                return HTTRANSPARENT  # Tk's window over the bar: let the frame window answer
        except Exception:  # never leave a message unanswered
            pass
        return u.CallWindowProcW(old, hwnd, msg, wparam, lparam)

    def window_menu(self, x, y):
        """The window's menu at a screen point, its items as the state allows (from the window
        procedure, never from a Tk callback)."""
        u = self.user32
        menu = u.GetSystemMenu(self.hwnd, False)
        if not menu:
            return
        zoomed = bool(u.IsZoomed(self.hwnd))
        for item, on in ((SC_RESTORE, zoomed), (SC_MAXIMIZE, not zoomed), (SC_MOVE, not zoomed),
                         (SC_SIZE, not zoomed)):
            u.EnableMenuItem(menu, item, 0 if on else 1)  # MF_BYCOMMAND | MF_ENABLED or MF_GRAYED
        command = u.TrackPopupMenu(menu, TPM_RETURNCMD, x, y, 0, self.hwnd, None)
        if command:
            u.PostMessageW(self.hwnd, WM_SYSCOMMAND, command, 0)

    def take_cover(self):
        """A still picture of the window's client laid over it: a plain Windows window holding a copy
        of the pixels on the screen now. Back from the task bar, the window shows it at once while
        Tk maps its elements under it (else they would appear one by one); the launcher takes it
        away (drop_cover). Windows calls only: this runs in the window procedure too."""
        if self.cover or not self.covers:
            return
        u, g, client = self.user32, _gdi32(), self.covers[0]
        rect = wintypes.RECT()
        u.GetClientRect(client, ctypes.byref(rect))
        w, h = rect.right, rect.bottom
        if w < 2 or h < 2:
            return
        bitmap = copy_of(client, w, h)
        if not bitmap:
            return
        window = u.CreateWindowExW(0, 'STATIC', None, WS_CHILD | WS_VISIBLE | SS_BITMAP, 0, 0, w, h, client,
                                   None, None, None)
        if not window:
            g.DeleteObject(bitmap)
            return
        u.SendMessageW(window, STM_SETIMAGE, IMAGE_BITMAP, bitmap)
        u.SetWindowPos(window, 0, 0, 0, 0, 0, SWP_TOP_SHOWN)
        self.cover, self.cover_time = (window, bitmap), time.monotonic()

    def drop_cover(self):
        if self.cover:
            (window, bitmap), self.cover = self.cover, None
            self.user32.DestroyWindow(window)
            _gdi32().DeleteObject(bitmap)

    def install(self):
        try:
            # Once per window: the frame can be the toplevel's own window (wm_frame() before Tk wraps
            # it), and a second subclass would keep the first as Tk's procedure, then free it.
            for hwnd in dict.fromkeys([self.hwnd] + self.covers):
                proc = WNDPROC(self.wndproc)  # kept: Windows calls it for as long as the window lives
                old = self.user32.SetWindowLongPtrW(hwnd, GWLP_WNDPROC, ctypes.cast(proc, ctypes.c_void_p))
                if not old:
                    self.uninstall()
                    return False
                self.procs[hwnd], self.olds[hwnd] = proc, old
            self.user32.SetWindowPos(self.hwnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED)
            return True
        except (AttributeError, OSError, ValueError):
            self.uninstall()
            return False

    def uninstall(self):
        """Tk's own window procedures back (after a failed install)."""
        for hwnd, old in list(self.olds.items()):
            try:
                self.user32.SetWindowLongPtrW(hwnd, GWLP_WNDPROC, old)
            except (OSError, ValueError):
                pass
        self.olds.clear()
