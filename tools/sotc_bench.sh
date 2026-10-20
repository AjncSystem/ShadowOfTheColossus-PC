#!/usr/bin/env bash
# sotc: repeatable benchmark in 60 FPS mode: +nointro starts outside the Shrine of Worship,
# Wander walks forward for 25 s; prints the average FPS of 20..45 s. Extra arguments: --env X=Y.
# Environment: SOTC_GAME_DIR, and PY / BENCH_CVARS (bench_common.sh; nointro is always added).
cd "$(dirname "$0")/.."
source tools/bench_common.sh
G=${SOTC_GAME_DIR:-"$(dirname "$0")/../../CUSA08809"}
$PY scripts/sotc_modes.py "$G" --out out --modes fps60,af16 --cvars "nointro $BENCH_CVARS" >/dev/null
cp out/patches.bin out/bench.patches; cp out/cmdline.bin out/bench.cmdline
bench_env fps60,af16
SOTC_GAME_DIR="$G" timeout 150 $PY tools/sotc_play.py 50 --patches out/bench.patches --cmdline out/bench.cmdline \
    --env BB_VBLANK_HZ=120 --env SOTC_FPS=1 "${BENCH_ENV[@]}" "$@" 18:ly=0:27000 >/dev/null 2>&1
grep "SOTC FPS" out/run.txt | awk '{t=$5+0; if (t>=25 && t<=46) {s+=$3; n++}}
    END {if (n) printf "avg FPS %.1f over %d samples\n", s/n, n; else print "no FPS samples: the game stopped early (see out/run.txt)"}'
