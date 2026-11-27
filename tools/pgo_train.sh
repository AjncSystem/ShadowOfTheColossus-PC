#!/usr/bin/env bash
# sotc: profile-guided optimization for Windows (MSYS2 CLANG64) and Linux (GCC or Clang).
# Builds an instrumented gpu target (out/gpu-pgogen), plays the attract demo and, when
# out/save-template exists, the Shrine of Worship route with it, merges/keeps the profile in
# pgo/ and rebuilds: build.sh then uses that profile by default (BB_PGO=off skips it). Re-run
# after large GPU-side changes; functions changed since the profile are compiled without it.
# Measured (RTX 3060 laptop, idle shrine ~5800 draws/frame): ~20 FPS plain, ~21 with ThinLTO,
# ~23.5 with ThinLTO + this profile.
# Environment: SOTC_GAME_DIR, PY, PGO_ATTRACT_SECONDS, MSYS2_ROOT (Windows), CC/CXX (Linux).
set -euo pipefail
cd "$(dirname "$0")/.."

case $(uname -s) in
MINGW*|MSYS*)
    [[ -x ${PY:-/c/PyBuild312/python.exe} ]] || { echo "Python not found: ${PY:-/c/PyBuild312/python.exe} (set PY)" >&2; exit 1; }
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
    if [[ -d out/save-template/CUSA08809 || -d out/save-template/CUSA08034 ]]; then
        BENCH_MEASURE=60 bash tools/sotc_bench_shrine.sh --timeout 400 || true
    fi
    # World streaming: the ride benchmark's draw thread is where the open-world stutter is. It runs from
    # a port folder, PGO_RIDE_INSTALL (SotC-PC with launcher/ and user/), whose exe is swapped meanwhile.
    if [[ -n ${PGO_RIDE_INSTALL:-} ]]; then
        cp "$PGO_RIDE_INSTALL/bin/bb-probe.exe" "$PGO_RIDE_INSTALL/bin/bb-probe.exe.pgo-backup"
        cp out/gpu-pgogen/bb-probe.exe "$PGO_RIDE_INSTALL/bin/bb-probe.exe"
        "${PY:-/c/PyBuild312/python.exe}" tools/sotc_ride_bench.py --install "$PGO_RIDE_INSTALL" || true
        mv -f "$PGO_RIDE_INSTALL/bin/bb-probe.exe.pgo-backup" "$PGO_RIDE_INSTALL/bin/bb-probe.exe"
    fi
    ls pgo/*.profraw >/dev/null 2>&1 || { echo 'No profile was written' >&2; exit 1; }
    llvm-profdata merge -o pgo/bbgpu.profdata pgo/*.profraw
    rm -f pgo/*.profraw
    restore
    echo "Profile: $PWD/pgo/bbgpu.profdata"
    # Ninja does not track the profile file: recompile everything with the new one.
    [[ -d out/gpu ]] && ninja -C out/gpu -t clean >/dev/null
    bash build.sh
    ;;
Linux)
    PY=${PY:-$(command -v python3 || command -v python || true)}
    [[ -n $PY ]] || { echo 'Python 3 not found (set PY)' >&2; exit 1; }
    if [[ -d ${HOME:-}/.local/opt/sotc-deps ]]; then
        export CMAKE_PREFIX_PATH="${HOME}/.local/opt/sotc-deps${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
    fi
    if [[ -z ${SOTC_GAME_DIR:-} ]]; then
        SOTC_GAME_DIR=$($PY -c '
import json, os
from pathlib import Path
for cfg in (Path("user/launcher_settings.json"),
            Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") / "SotC-PC" / "launcher_settings.json"):
    if cfg.is_file():
        d = json.loads(cfg.read_text(encoding="utf-8")).get("game_dir", "")
        if d and (Path(d) / "eboot.bin").is_file():
            print(d)
            break
' || true)
        SOTC_GAME_DIR=${SOTC_GAME_DIR:-../CUSA08809}
    fi
    export SOTC_GAME_DIR
    [[ -f "$SOTC_GAME_DIR/eboot.bin" ]] || { echo "Game folder with eboot.bin not found: $SOTC_GAME_DIR (set SOTC_GAME_DIR)" >&2; exit 1; }

    CC=${CC:-$(command -v cc || command -v gcc || true)}
    is_clang=0
    case ${CXX:-${CC:-}} in
        *clang*) is_clang=1 ;;
    esac
    mkdir -p pgo
    if (( is_clang )); then
        command -v llvm-profdata >/dev/null || { echo 'llvm-profdata is required for Clang PGO' >&2; exit 1; }
        rm -f pgo/*.profraw
    else
        find pgo -name '*.gcda' -delete
    fi

    if [[ ! -x out/bb-probe || ! -f out/gpu/libbbgpu.so ]]; then
        BB_PGO=off bash build.sh
    fi

    if [[ -d out/gpu-pgogen && ! -f out/gpu-pgogen/build.ninja ]]; then
        rm -rf out/gpu-pgogen
    fi
    cmake_compiler_args=()
    if [[ -n ${CXX:-} ]]; then
        cmake_compiler_args=(-DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX")
    fi
    echo 'Building instrumented GPU library (out/gpu-pgogen)...'
    cmake -S gpu -B out/gpu-pgogen -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO=generate \
        -DBB_PGO_DIR="$PWD/pgo" -DBB_LTO="${BB_LTO:-ON}" "${cmake_compiler_args[@]}" >/dev/null
    ninja -C out/gpu-pgogen bbgpu > out/gpu-pgogen-build.log 2>&1 ||
        { tail -20 out/gpu-pgogen-build.log >&2; echo 'Instrumented build failed (full log: out/gpu-pgogen-build.log)' >&2; exit 1; }

    cp out/gpu/libbbgpu.so out/gpu/libbbgpu.so.pgo-backup
    restore() { [[ -f out/gpu/libbbgpu.so.pgo-backup ]] && mv -f out/gpu/libbbgpu.so.pgo-backup out/gpu/libbbgpu.so; true; }
    trap restore EXIT
    cp out/gpu-pgogen/libbbgpu.so out/gpu/libbbgpu.so

    # Training: libbbgpu dumps PGO counters periodically (gpu/shim/bbgpu.cpp) and on clean exit.
    echo "Running PGO training on attract demo (${PGO_ATTRACT_SECONDS:-330}s)..."
    BENCH_SECONDS=${PGO_ATTRACT_SECONDS:-330} PY="$PY" bash tools/sotc_bench_attract.sh || true
    if [[ -d out/save-template/CUSA08809 || -d out/save-template/CUSA08034 ]]; then
        echo 'Running PGO training on Shrine of Worship route...'
        BENCH_MEASURE=60 PY="$PY" bash tools/sotc_bench_shrine.sh --timeout 400 || true
    fi

    if (( is_clang )); then
        ls pgo/*.profraw >/dev/null 2>&1 || { echo 'No Clang PGO profile was written to pgo/' >&2; exit 1; }
        llvm-profdata merge -o pgo/bbgpu.profdata pgo/*.profraw
        rm -f pgo/*.profraw
        echo "Profile: $PWD/pgo/bbgpu.profdata"
    else
        [[ -n $(find pgo -name '*.gcda' -print -quit 2>/dev/null) ]] || { echo 'No GCC PGO profile (*.gcda) was written to pgo/' >&2; exit 1; }
        # GCC mangles the build directory into .gcda filenames; match out/gpu so build.sh finds them.
        for f in pgo/*#out#gpu-pgogen#*.gcda; do
            [[ -f $f ]] && mv -f "$f" "${f//#out#gpu-pgogen#/#out#gpu#}"
        done
        echo "Profile: $PWD/pgo (*.gcda)"
    fi
    restore
    echo 'Rebuilding final PGO-optimized port...'
    [[ -d out/gpu ]] && ninja -C out/gpu -t clean >/dev/null
    bash build.sh
    ;;
*)
    echo "Unsupported OS for pgo_train.sh: $(uname -s)" >&2
    exit 1
    ;;
esac
