#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Owned Windows run with copied saves, native pad events and per-frame evidence."""
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


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--probe', type=Path, required=True)
    p.add_argument('--package', type=Path, required=True)
    p.add_argument('--saves', type=Path, required=True)
    p.add_argument('--cache-seed', type=Path, required=True)
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--events', type=Path, required=True)
    p.add_argument('--seconds', type=float, default=120)
    p.add_argument('--env', action='append', default=[])
    p.add_argument('--allow-host-pad', action='store_true',
                   help='Allow keyboard/gamepad input alongside the scripted route')
    args = p.parse_args()
    if not 0 < args.seconds <= 600: p.error('seconds must be 1..600')
    probe=args.probe.resolve()
    package=args.package.resolve()
    work=args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    template=work/'save-template'
    if not template.exists(): shutil.copytree(args.saves,template)
    cache=work/'cache'
    if not cache.exists(): shutil.copytree(args.cache_seed,cache)
    run=work/f'run-{time.time_ns()}'
    user=run/'user'
    user.mkdir(parents=True)
    shutil.copytree(template,user/'savedata')
    pad=run/'pad.txt'
    pad.write_text('' if args.allow_host_pad else 'replay')
    # Native replay overrides physical controls. A long neutral recording keeps
    # incidental keyboard/gamepad movement out of the isolated benchmark; pad.txt
    # injections still override its axes and add its buttons.
    neutral_replay=run/'neutral-replay.txt'
    neutral_replay.write_text('0 0 128 128 128 128 0 0\n3600000 0 128 128 128 128 0 0\n')
    events=json.loads(args.events.read_text())
    (run/'events.json').write_text(json.dumps(events,indent=2)+'\n')
    env=dict(os.environ)
    env.update(BB_GPU_USER_DIR=str(user),SOTC_CACHE_DIR=str(cache),BB_PAD_FILE=str(pad),
               SOTC_FRAME_TIMES=str(run/'frames.csv'),SOTC_FRAME_STATS='1',SOTC_FPS='1',
               BB_GPU_LOG='warning',BB_VBLANK_HZ='120',SOTC_RECORD_THREAD='1',
               BB_FULLSCREEN='0',SOTC_MOUSE='0',BB_FRAME_DUMP_TRIGGER=str(run/'shot.trigger'),
               SOTC_UPDATE_DIR='D:/Dev/recompilacao/CUSA08809-patch',
               PATH='D:/Dev/msys64/clang64/bin'+os.pathsep+env.get('PATH',''))
    if not args.allow_host_pad:
        env['BB_PAD_REPLAY']=str(neutral_replay)
    for item in args.env:
        key,value=item.split('=',1)
        env[key]=value
    image=package/'out'
    command=[str(probe),str(image/'boot-linked.bin'),'--content-profile',str(image/'content.bin'),
             '--sotc-cpu',str(image/'sotc-cpu.bin'),'--patches',str(image/'patches.bin'),
             '--sotc-cmdline',str(image/'cmdline.bin'),'--app0','D:/Dev/recompilacao/CUSA08809',
             '--user',str(user)]
    report=dict(runtime_sha256=sha(probe),image_sha256=sha(image/'boot-linked.bin'),command=command,
                events=events,events_sha256=sha(run/'events.json'),cache=str(cache),
                save_files={str(x.relative_to(template)):sha(x) for x in template.rglob('*') if x.is_file()},
                settings={k:v for k,v in env.items() if k.startswith(('BB_','SOTC_'))},
                log=str(run/'game.log'),csv=str(run/'frames.csv'),gameplay_verified=False)
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.CreateJobObjectW.restype=wintypes.HANDLE
    kernel.AssignProcessToJobObject.argtypes=[wintypes.HANDLE,wintypes.HANDLE]
    kernel.TerminateJobObject.argtypes=[wintypes.HANDLE,wintypes.UINT]
    kernel.CloseHandle.argtypes=[wintypes.HANDLE]
    job=kernel.CreateJobObjectW(None,None)
    if not job: raise ctypes.WinError(ctypes.get_last_error())
    applied=[]
    neutral_at=None
    index=0
    began=time.monotonic()
    origin=None
    print('RUN',run,flush=True)
    with (run/'game.log').open('wb') as log:
        child=subprocess.Popen(command,cwd=run,env=env,stdin=subprocess.DEVNULL,stdout=log,
            stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        if not kernel.AssignProcessToJobObject(job,wintypes.HANDLE(int(child._handle))):
            child.terminate()
            raise ctypes.WinError(ctypes.get_last_error())
        report['process_id']=child.pid
        (run/'run.json').write_text(json.dumps(report,indent=2)+'\n')
        print('OWNED PID',child.pid,'PAD',pad,flush=True)
        try:
            while child.poll() is None:
                if origin is None:
                    if b'Runtime: pad file:' in (run/'game.log').read_bytes():
                        origin=time.monotonic()
                        report['pad_origin_monotonic_ns']=time.monotonic_ns()
                        report['startup_seconds']=origin-began
                        print('PAD ORIGIN after',origin-began,flush=True)
                    elif time.monotonic()-began>180:
                        break
                    else:
                        time.sleep(0.05)
                        continue
                elapsed=time.monotonic()-origin
                if elapsed>=args.seconds: break
                if neutral_at is not None and elapsed>=neutral_at:
                    pad.write_text('')
                    neutral_at=None
                while index<len(events) and elapsed>=events[index]['at']:
                    event=events[index]
                    if event['kind']=='shot':
                        (run/'shot.trigger').touch()
                    else:
                        pad.write_text(event['tokens'])
                        neutral_at=elapsed+event.get('hold',0.25)
                    applied.append(dict(event,applied_at=elapsed))
                    print('EVENT',elapsed,event,flush=True)
                    index+=1
                time.sleep(0.02)
        finally:
            pad.write_text('')
            report['timeout']=child.poll() is None
            if child.poll() is None: kernel.TerminateJobObject(job,1)
            child.wait(timeout=15)
            kernel.CloseHandle(job)
    report.update(exit_code=child.returncode,elapsed_seconds=time.monotonic()-began,applied=applied,
                  captures=[str(x) for x in (user/'screenshots').glob('*.png')])
    (run/'run.json').write_text(json.dumps(report,indent=2)+'\n')
    print('RESULT',run,child.returncode,'timeout',report['timeout'],flush=True)


if __name__=='__main__': main()
