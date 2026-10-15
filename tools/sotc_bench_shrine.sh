#!/usr/bin/env bash
# sotc: Shrine of Worship benchmark (wrapper for tools/sotc_bench_shrine.py, which drives the game
# from the log so varying load times do not matter): Wander idle at the altar, ~5900 draws per
# frame, 60 FPS mode. Needs out/save-template/CUSA08809 (a save after colossus 1).
# Environment: SOTC_GAME_DIR, PY (Windows Python with Pillow), MSYS2_ROOT, BENCH_CVARS,
# BENCH_MEASURE (seconds measured after the idle scene is reached, default 40).
# Extra arguments go to the driver (e.g. --env X=Y).
cd "$(dirname "$0")/.."
G=${SOTC_GAME_DIR:-"$(dirname "$0")/../../CUSA08809"}
PY=${PY:-/c/PyBuild312/python.exe}
$PY scripts/sotc_modes.py "$G" --out out --modes fps60,af16 --cvars "${BENCH_CVARS:-}" >/dev/null
cp out/patches.bin out/bench.patches; cp out/cmdline.bin out/bench.cmdline
SOTC_GAME_DIR="$G" timeout 420 $PY tools/sotc_bench_shrine.py --measure "${BENCH_MEASURE:-40}" "$@"
