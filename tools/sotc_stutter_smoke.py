#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded boot/cache check using an installed prepared image and isolated user data."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--probe', required=True, type=Path)
    p.add_argument('--package', required=True, type=Path)
    p.add_argument('--launcher', type=Path, help='Exercise packaged --run and preparation too')
    p.add_argument('--shared-cache', action='store_true', help='Test default path/migration in isolated LOCALAPPDATA')
    p.add_argument('--game', required=True, type=Path)
    p.add_argument('--update', type=Path)
    p.add_argument('--work', required=True, type=Path)
    p.add_argument('--timeout', type=float, default=60)
    p.add_argument('--vblank-hz', type=int, default=120, choices=(60, 120))
    p.add_argument('--screenshot-after', type=float, default=0)
    p.add_argument('--msys2', type=Path, default=Path('D:/Dev/msys64'))
    args = p.parse_args()
    if not 0 < args.timeout <= 600:
        p.error('timeout must be 1..600 seconds')
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    user = work / 'user'
    if not user.exists():
        user.mkdir()
        source = args.package / 'user' / 'savedata'
        if source.is_dir():
            shutil.copytree(source, user / 'savedata')
        # Preserve the installed cache, using only a copy in this experiment.
        source = args.package / 'user' / 'cache'
        if source.is_dir():
            shutil.copytree(source, user / 'cache' if args.shared_cache else work / 'cache', dirs_exist_ok=True)
    run = work / f'run-{time.time_ns()}'
    run.mkdir()
    (run / 'pad.txt').write_text('replay')
    (run / 'neutral-replay.txt').write_text('0 0 128 128 128 128 0 0\n3600000 0 128 128 128 128 0 0\n')
    env = dict(os.environ)
    env.update(BB_GPU_USER_DIR=str(user), SOTC_CACHE_DIR=str(work / 'cache'),
               SOTC_FRAME_STATS='1', SOTC_FRAME_TIMES=str(run / 'frames.csv'),
               BB_FULLSCREEN='0', BB_VBLANK_HZ=str(args.vblank_hz), SOTC_RECORD_THREAD='1',
               BB_PAD_FILE=str(run / 'pad.txt'), BB_PAD_REPLAY=str(run / 'neutral-replay.txt'),
               SOTC_FRAME_LOG='60', BB_CONFIG=str(args.package.resolve() / 'sotc.ini'))
    env['BB_GPU_LOG'] = 'info'
    env['BB_FRAME_DUMP_TRIGGER'] = str(run / 'screenshot.trigger')
    cache = work / 'cache'
    if args.shared_cache:
        env.pop('SOTC_CACHE_DIR', None)
        env['SOTC_PORTABLE_CACHE'] = '0'
        env['LOCALAPPDATA'] = str(work / 'local')
        cache = work / 'local/ShadowOfTheColossus-PC/cache'
    if args.update:
        env['SOTC_UPDATE_DIR'] = str(args.update.resolve())
    if not (args.probe.parent / 'SDL3.dll').is_file():
        env['PATH'] = str(args.msys2 / 'clang64/bin') + os.pathsep + env.get('PATH', '')
    image = args.package.resolve() / 'out'
    command = [str(args.probe.resolve()), str(image / 'boot-linked.bin'),
               '--content-profile', str(image / 'content.bin'), '--sotc-cpu', str(image / 'sotc-cpu.bin'),
               '--patches', str(image / 'patches.bin'), '--app0', str(args.game.resolve()),
               '--user', str(user)]
    if (image / 'cmdline.bin').is_file():
        command += ['--sotc-cmdline', str(image / 'cmdline.bin')]
    if args.launcher:
        command = [str(args.launcher.resolve()), '--run']
        env.update(SOTC_GAME_DIR=str(args.game.resolve()), SOTC_USER_DIR=str(user),
                   SOTC_MODES='af16,fps60' if args.vblank_hz >= 120 else 'af16',
                   SOTC_CVARS='doIndexBufferCulling=0 doSunShadowIndexBufferCulling=0')
    # A frozen launcher starts preparation helpers and the game: bind them to our own job.
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    kernel.CreateJobObjectW.restype = wintypes.HANDLE
    kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    kernel.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    job = kernel.CreateJobObjectW(None, None)
    if not job: raise ctypes.WinError(ctypes.get_last_error())
    runtime_hash = hashlib.sha256(args.probe.read_bytes()).hexdigest()
    began = time.monotonic()
    expired = False
    with (run / 'game.log').open('wb') as log:
        process = subprocess.Popen(command, cwd=work, env=env, stdin=subprocess.DEVNULL,
            stdout=log, stderr=subprocess.STDOUT, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        if not kernel.AssignProcessToJobObject(job, wintypes.HANDLE(int(process._handle))):
            process.terminate()
            kernel.CloseHandle(job)
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            if 0 < args.screenshot_after < args.timeout:
                try:
                    process.wait(timeout=args.screenshot_after)
                except subprocess.TimeoutExpired:
                    (run / 'screenshot.trigger').touch()
            process.wait(timeout=max(0.1, args.timeout - (time.monotonic() - began)))
        except subprocess.TimeoutExpired:
            expired = True
        finally:
            if process.poll() is None:
                kernel.TerminateJobObject(job, 1)
                process.wait(timeout=10)
            kernel.CloseHandle(job)
    report = dict(command=command, process_id=process.pid, exit_code=process.returncode,
                  timeout=expired, elapsed_seconds=time.monotonic() - began,
                  runtime_sha256=runtime_hash,
                  image_sha256=hashlib.sha256((image / 'boot-linked.bin').read_bytes()).hexdigest(),
                  cache=str(cache), log=str(run / 'game.log'), csv=str(run / 'frames.csv'),
                  scope='Boot/cache smoke test without gameplay input; not a campaign benchmark',
                  settings={k: env[k] for k in ('BB_VBLANK_HZ', 'SOTC_RECORD_THREAD', 'BB_FULLSCREEN',
                                                'SOTC_FRAME_STATS')})
    (run / 'run.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
