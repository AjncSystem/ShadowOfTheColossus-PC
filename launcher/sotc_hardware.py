# SPDX-License-Identifier: GPL-2.0-or-later
"""Will it run, and how well: this PC's processor, memory, video card and the disk of the game
folder, and the play mode they suit (Normal, Pro, Extreme). After the bloodborne_pc launcher's
hardware check (bbport_hardware.py). Windows, and Linux through /proc and /sys; every probe answers
None when it cannot tell."""
import ctypes
import os
import re
import subprocess

NO_WINDOW = 0x08000000
IS_WINDOWS = os.name == 'nt'
BYTES_PER_GIB = 2 ** 30


def cpu_name():
    if not IS_WINDOWS:
        try:
            with open('/proc/cpuinfo', encoding='utf-8', errors='replace') as info:
                return next((' '.join(line.split(':', 1)[1].split()) for line in info
                             if line.startswith('model name')), None)
        except OSError:
            return None
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'HARDWARE\DESCRIPTION\System\CentralProcessor\0') as key:
            return ' '.join(str(winreg.QueryValueEx(key, 'ProcessorNameString')[0]).split())
    except OSError:
        return None


def memory_gb():
    if not IS_WINDOWS:
        try:
            return round(os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES') / BYTES_PER_GIB)
        except (OSError, ValueError):
            return None

    class MEMORYSTATUSEX(ctypes.Structure):
        _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong), ('total', ctypes.c_ulonglong),
                    ('avail', ctypes.c_ulonglong), ('page_total', ctypes.c_ulonglong),
                    ('page_avail', ctypes.c_ulonglong), ('virtual_total', ctypes.c_ulonglong),
                    ('virtual_avail', ctypes.c_ulonglong), ('extended', ctypes.c_ulonglong)]
    status = MEMORYSTATUSEX()
    status.length = ctypes.sizeof(status)
    try:
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            return round(status.total / BYTES_PER_GIB)
    except (AttributeError, OSError):
        pass
    return None


def video_memory_gb(gpu_text):
    """The video memory in the launcher's graphics card line ('…, discrete, 8006 MiB')."""
    found = re.search(r'(\d+)\s*MiB', gpu_text or '')
    return round(int(found.group(1)) / 1024) if found else None


def linux_disk_kind(path):
    """The disk under `path` from sysfs: its block device, up to the whole disk a partition is on."""
    try:
        device = os.stat(os.path.abspath(path or '.')).st_dev
        block = os.path.realpath(f'/sys/dev/block/{os.major(device)}:{os.minor(device)}')
    except OSError:
        return None
    if os.path.exists(os.path.join(block, 'partition')):
        block = os.path.dirname(block)
    name = os.path.basename(block)
    if name.startswith('nvme'):
        return 'nvme'
    try:
        with open(os.path.join(block, 'queue', 'rotational'), encoding='ascii') as rotational:
            return 'hdd' if rotational.read().strip() == '1' else 'ssd'
    except OSError:  # a virtual device (LVM, dm-crypt, btrfs over several disks): not told apart
        return None


def disk_kind(path):
    """'nvme', 'ssd' or 'hdd' for the drive holding `path`, or None."""
    if not IS_WINDOWS:
        return linux_disk_kind(path)
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
