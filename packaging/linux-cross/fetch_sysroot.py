#!/usr/bin/env python3
"""Download the MSYS2 CLANG64 packages of packaging/windows/README.md (with their dependencies)
and unpack them into a sysroot for the Linux cross build (build-linux.sh).

usage: fetch_sysroot.py <root>      (default: $CLANG64_ROOT or ~/.cache/sotc-clang64)

<root>/clang64/  the unpacked packages (headers, import libraries, DLLs)
<root>/res/      a clang resource dir: the host clang's headers + the sysroot's compiler-rt
<root>/pkgs/     the downloaded archives (kept: a re-run only fetches what is new)
The host clang must have the same major version as the CLANG64 compiler-rt.
"""
import io, os, re, subprocess, sys, tarfile, urllib.request
from pathlib import Path

REPO = 'https://repo.msys2.org/mingw/clang64/'
P = 'mingw-w64-clang-x86_64-'
# Runtime parts of the CLANG64 toolchain (not the Windows clang/LLVM binaries) and the libraries.
WANT = ['crt', 'headers', 'winpthreads', 'libwinpthread', 'libc++', 'libunwind', 'compiler-rt',
        'vulkan-headers', 'vulkan-loader', 'vulkan-memory-allocator', 'glslang', 'sdl3', 'fmt',
        'boost', 'robin-map', 'xxhash', 'ffmpeg', 'zydis', 'spirv-tools', 'spirv-headers',
        'spirv-cross']
# Build tools run on Linux instead; their Windows binaries are not needed.
SKIP = {P + x for x in ('python', 'cmake', 'ninja', 'pkgconf', 'perl', 'git', 'gdb', 'make',
                        'clang', 'llvm', 'lld', 'lldb')}
# Capitalized includes in the sources (MinGW names them in lower case).
CASE_ALIASES = ['Windows.h', 'Shlobj.h']


def read_db(path):
    raw = subprocess.run(['zstd', '-dc', str(path)], capture_output=True, check=True).stdout
    pkgs, provides = {}, {}
    with tarfile.open(fileobj=io.BytesIO(raw)) as t:
        for m in t:
            if not m.name.endswith('/desc'):
                continue
            fields, key = {}, None
            for line in t.extractfile(m).read().decode().splitlines():
                if line.startswith('%') and line.endswith('%'):
                    key = line[1:-1]
                    fields[key] = []
                elif line and key:
                    fields[key].append(line)
            name = fields['NAME'][0]
            pkgs[name] = fields
            for p in fields.get('PROVIDES', []):
                provides.setdefault(re.split('[<>=]', p)[0], name)
    return pkgs, provides


def resolve(pkgs, provides):
    todo, done = [P + w for w in WANT], set()
    while todo:
        name = re.split('[<>=]', todo.pop())[0]
        name = name if name in pkgs else provides.get(name)
        if not name or name in done or name in SKIP:
            continue
        done.add(name)
        todo += pkgs[name].get('DEPENDS', [])
    return sorted(done)


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else
                os.environ.get('CLANG64_ROOT') or Path.home() / '.cache/sotc-clang64').resolve()
    (root / 'pkgs').mkdir(parents=True, exist_ok=True)
    db = root / 'clang64.db'
    urllib.request.urlretrieve(REPO + 'clang64.db', db)
    pkgs, provides = read_db(db)
    names = resolve(pkgs, provides)
    files = [pkgs[n]['FILENAME'][0] for n in names]
    print(f'{len(files)} packages, '
          f'{sum(int(pkgs[n]["CSIZE"][0]) for n in names) / 2**20:.0f} MiB')
    for fn in files:
        if not (root / 'pkgs' / fn).exists():
            print('  get', fn)
            urllib.request.urlretrieve(REPO + fn, root / 'pkgs' / fn)
    # Unpack from scratch, so packages that left the list do not linger.
    sysroot = root / 'clang64'
    subprocess.run(['rm', '-rf', str(sysroot)], check=True)
    for fn in files:
        subprocess.run(['tar', '--zstd', '-xf', str(root / 'pkgs' / fn), '-C', str(root),
                        '--exclude=.BUILDINFO', '--exclude=.PKGINFO', '--exclude=.MTREE',
                        '--exclude=.INSTALL'], check=True)
    # Windows file names are case-insensitive and the sources (shadPS4, imgui) include some
    # headers capitalized; MinGW ships them in lower case.
    for alias in CASE_ALIASES:
        link = sysroot / 'include' / alias
        if not link.exists():
            link.symlink_to(alias.lower())
    host_res = Path(subprocess.run(['clang', '-print-resource-dir'], capture_output=True,
                                   text=True, check=True).stdout.strip())
    rt = sorted((sysroot / 'lib/clang').iterdir())[-1]
    if rt.name.split('.')[0] != host_res.name.split('.')[0]:
        print(f'warning: host clang {host_res.name} but CLANG64 compiler-rt {rt.name}')
    res = root / 'res'
    res.mkdir(exist_ok=True)
    for link, target in (('include', host_res / 'include'), ('lib', rt / 'lib')):
        (res / link).unlink(missing_ok=True)
        (res / link).symlink_to(target)
    print(f'sysroot ready: {root}')


if __name__ == '__main__':
    main()
