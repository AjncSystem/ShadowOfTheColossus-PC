#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Captures the game window of a process with PrintWindow (PW_RENDERFULLCONTENT), without
activating or raising it: works while the window is behind other windows.

    python tools/wincap.py <pid> <out-prefix> [count] [interval-ms]
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

user32 = ctypes.WinDLL('user32', use_last_error=True)
gdi32 = ctypes.WinDLL('gdi32', use_last_error=True)
PW_RENDERFULLCONTENT = 2


def find_window(pid):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def callback(hwnd, _):
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd) and not user32.GetWindow(hwnd, 4):
            found.append(hwnd)
            return False
        return True

    user32.EnumWindows(callback, 0)
    return found[0] if found else None


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [('biSize', wt.DWORD), ('biWidth', wt.LONG), ('biHeight', wt.LONG),
                ('biPlanes', wt.WORD), ('biBitCount', wt.WORD), ('biCompression', wt.DWORD),
                ('biSizeImage', wt.DWORD), ('biXPelsPerMeter', wt.LONG),
                ('biYPelsPerMeter', wt.LONG), ('biClrUsed', wt.DWORD), ('biClrImportant', wt.DWORD)]


def capture(hwnd):
    rect = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    w, h = rect.right - rect.left, rect.bottom - rect.top
    if w <= 0 or h <= 0:
        return None
    window_dc = user32.GetDC(hwnd)
    mem_dc = gdi32.CreateCompatibleDC(window_dc)
    bitmap = gdi32.CreateCompatibleBitmap(window_dc, w, h)
    old = gdi32.SelectObject(mem_dc, bitmap)
    # PW_CLIENTONLY (1) | PW_RENDERFULLCONTENT (2)
    ok = user32.PrintWindow(hwnd, mem_dc, 1 | PW_RENDERFULLCONTENT)
    info = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem_dc, bitmap, 0, h, buf, ctypes.byref(info), 0)
    gdi32.SelectObject(mem_dc, old)
    gdi32.DeleteObject(bitmap)
    gdi32.DeleteDC(mem_dc)
    user32.ReleaseDC(hwnd, window_dc)
    if not ok:
        return None
    from PIL import Image
    return Image.frombuffer('RGBA', (w, h), buf.raw, 'raw', 'BGRA', 0, 1).convert('RGB')


def main():
    pid, prefix = int(sys.argv[1]), sys.argv[2]
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    interval = float(sys.argv[4]) / 1000 if len(sys.argv) > 4 else 0.05
    hwnd = find_window(pid)
    if not hwnd:
        print('no window')
        return 1
    for i in range(count):
        image = capture(hwnd)
        if image:
            image.save(f'{prefix}{i:02d}.png')
        time.sleep(interval)
    return 0


if __name__ == '__main__':
    sys.exit(main())
