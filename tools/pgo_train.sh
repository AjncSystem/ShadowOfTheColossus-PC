#!/usr/bin/env bash
# sotc: profile-guided optimization for the Windows build (MSYS2 CLANG64). Builds an instrumented
# exe (out/gpu-pgogen), plays the attract demo and, when out/save-template exists, the Shrine of
# Worship route with it, merges the profile into pgo/bbgpu.profdata and rebuilds: build.sh then
# uses that profile by default (BB_PGO=off skips it). Re-run after large GPU-side changes;
# functions changed since the profile are compiled without it.
# Measured (RTX 3060 laptop, idle shrine ~5800 draws/frame): ~20 FPS plain, ~21 with ThinLTO,
# ~23.5 with ThinLTO + this profile.
# Environment: SOTC_GAME_DIR, PY (Windows Python with Pillow), MSYS2_ROOT.
set -euo pipefail
[[ -x ${PY:-/c/PyBuild312/python.exe} ]] || { echo "Python not found: ${PY:-/c/PyBuild312/python.exe} (set PY)" >&2; exit 1; }
cd "$(dirname "$0")/.."
# shellcheck source=../msys2-env.sh
source ./msys2-env.sh
mkdir -p pgo
rm -f pgo/*.profraw
cmake -S gpu -B out/gpu-pgogen -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO=generate \
    -DBB_PGO_DIR="$PWD/pgo" -DBB_LTO=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_RC_COMPILER=llvm-windres >/dev/null
ninja -C out/gpu-pgogen bb-probe > out/gpu-pgogen-build.log 2>&1 ||
    { tail -20 out/gpu-pgogen-build.log >&2; echo 'Instrumented build failed' >&2; exit 1; }
# The instrumented exe replaces out/bb-probe.exe only for the training runs, whatever happens.
[[ -f out/bb-probe.exe ]] && cp out/bb-probe.exe out/bb-probe.exe.pgo-backup
restore() { [[ -f out/bb-probe.exe.pgo-backup ]] && mv -f out/bb-probe.exe.pgo-backup out/bb-probe.exe; true; }
trap restore EXIT
cp out/gpu-pgogen/bb-probe.exe out/bb-probe.exe
# Training: the instrumented exe writes (and merges) its profile every 30 s.
BENCH_SECONDS=330 bash tools/sotc_bench_attract.sh || true
if [[ -d out/save-template/CUSA08809 ]]; then
    BENCH_MEASURE=60 bash tools/sotc_bench_shrine.sh --timeout 400 || true
fi
ls pgo/*.profraw >/dev/null 2>&1 || { echo 'No profile was written' >&2; exit 1; }
llvm-profdata merge -o pgo/bbgpu.profdata pgo/*.profraw
rm -f pgo/*.profraw
restore
echo "Profile: $PWD/pgo/bbgpu.profdata"
# Ninja does not track the profile file: recompile everything with the new one.
[[ -d out/gpu ]] && ninja -C out/gpu -t clean >/dev/null
bash build.sh
