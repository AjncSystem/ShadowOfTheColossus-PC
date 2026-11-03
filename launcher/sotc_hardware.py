# SPDX-License-Identifier: GPL-2.0-or-later
"""Will it run, and how well: this PC's processor, memory, video card and the disk of the game
folder, and the play mode they suit (Normal, Pro, Extreme). After the bloodborne_pc launcher's
hardware check (bbport_hardware.py). Windows only; every probe answers None when it cannot tell."""
import ctypes
import os
import re
import subprocess

NO_WINDOW = 0x08000000


def cpu_name():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'HARDWARE\DESCRIPTION\System\CentralProcessor\0') as key:
            return ' '.join(str(winreg.QueryValueEx(key, 'ProcessorNameString')[0]).split())
    except OSError:
        return None


def memory_gb():
    class MEMORYSTATUSEX(ctypes.Structure):
        _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong), ('total', ctypes.c_ulonglong),
                    ('avail', ctypes.c_ulonglong), ('page_total', ctypes.c_ulonglong),
                    ('page_avail', ctypes.c_ulonglong), ('virtual_total', ctypes.c_ulonglong),
                    ('virtual_avail', ctypes.c_ulonglong), ('extended', ctypes.c_ulonglong)]
    status = MEMORYSTATUSEX()
    status.length = ctypes.sizeof(status)
    try:
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            return round(status.total / 2 ** 30)
    except (AttributeError, OSError):
        pass
    return None


def video_memory_gb(gpu_text):
    """The video memory in the launcher's graphics card line ('…, discrete, 8006 MiB')."""
    found = re.search(r'(\d+)\s*MiB', gpu_text or '')
    return round(int(found.group(1)) / 1024) if found else None


def disk_kind(path):
    """'nvme', 'ssd' or 'hdd' for the drive holding `path`, or None."""
    drive = os.path.splitdrive(os.path.abspath(path or '.'))[0].rstrip(':')
    if len(drive) != 1:
        return None
    script = (f"$d = Get-Partition -DriveLetter {drive} -ErrorAction Stop | Get-Disk | Get-PhysicalDisk; "
              "Write-Output ($d.BusType, $d.MediaType -join ' ')")
    try:
        result = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', script],
                                capture_output=True, text=True, timeout=20, creationflags=NO_WINDOW)
    except (OSError, subprocess.TimeoutExpired):
        return None
    text = result.stdout.lower()
    if 'nvme' in text:
        return 'nvme'
    if 'ssd' in text:
        return 'ssd'
    if 'hdd' in text:
        return 'hdd'
    return None


def check(game_dir, gpu_text):
    """Everything at once: a dict with cpu, threads, memory, video_memory, disk."""
    return {'cpu': cpu_name(), 'threads': os.cpu_count(), 'memory': memory_gb(),
            'video_memory': video_memory_gb(gpu_text), 'disk': disk_kind(game_dir)}


def recommend(info):
    """The play mode this PC suits: 'extreme', 'pro' or 'normal' (unknown counts as little)."""
    threads, memory, video = info.get('threads') or 0, info.get('memory') or 0, info.get('video_memory') or 0
    if video >= 8 and memory >= 16 and threads >= 12:
        return 'extreme'
    if video >= 6 and memory >= 12 and threads >= 8:
        return 'pro'
    return 'normal'
