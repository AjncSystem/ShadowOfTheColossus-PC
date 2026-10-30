#!/usr/bin/env bash
# sotc: benchmark on the title screen's attract demo (Agro and Wander across the map, the same
# route every time), no input of our own. Prints FPS per 5 s, the average and the minimum from
# 60 s on, and saves a shot every 30 s (out/dump/sheet.png). Extra arguments: --env X=Y.
# The demo replays recorded pad input frame by frame: above 30 FPS Agro walks and never leaves the
# Shrine (29 Oct), so by default the 30 FPS cap stays, and the logos and the ~4.5 min opening are
# skipped (skipSplash, SOTC_SKIP_INTRO): the title is up by ~60 s and the demo ends by ~450 s,
# when the opening starts over. BENCH_DEMO=0: the old run (opening, fps60,af16, 360 s).
# Environment: SOTC_GAME_DIR, BENCH_SECONDS, and PY / BENCH_CVARS (bench_common.sh).
# BENCH_RENDER=WxH: the game renders at that size (sotc_modes.py --render), e.g. for DLSS.
# BENCH_MODES: sotc_modes.py modes (default af16; fps60,af16 with BENCH_DEMO=0).
cd "$(dirname "$0")/.."
source tools/bench_common.sh
G=${SOTC_GAME_DIR:-"$(dirname "$0")/../../CUSA08809"}
if [[ ${BENCH_DEMO:-1} == 1 ]]; then
    SECONDS_TOTAL=${BENCH_SECONDS:-450}; MODES=${BENCH_MODES:-af16}
    CVARS="$BENCH_CVARS skipSplash=1"; SKIP=(--env SOTC_SKIP_INTRO=40)
else
    SECONDS_TOTAL=${BENCH_SECONDS:-360}; MODES=${BENCH_MODES:-fps60,af16}
    CVARS=$BENCH_CVARS; SKIP=()
fi
$PY scripts/sotc_modes.py "$G" --out out --modes "$MODES" ${BENCH_RENDER:+--render "$BENCH_RENDER"} --cvars "$CVARS" >/dev/null
cp out/patches.bin out/bench.patches; cp out/cmdline.bin out/bench.cmdline
bench_env "$MODES"
shots=(); for t in $(seq 60 30 $((SECONDS_TOTAL - 5))); do shots+=("$t:shot"); done
SOTC_GAME_DIR="$G" timeout $((SECONDS_TOTAL + 120)) $PY tools/sotc_play.py "$SECONDS_TOTAL" --patches out/bench.patches \
    --cmdline out/bench.cmdline --env BB_VBLANK_HZ=120 --env SOTC_FPS=1 "${BENCH_ENV[@]}" "${SKIP[@]}" "$@" "${shots[@]}" >/dev/null 2>&1
grep "SOTC FPS" out/run.txt | awk '{t=$5+0; f=$3+0; printf "%s@%d ", f, t; if (t>=60) {s+=f; n++; if (!m || f<m) m=f}}
    END {if (n) printf "\navg %.1f FPS, min %.1f (from 60 s, %d samples)\n", s/n, m, n;
         else print "\nno FPS samples from 60 s on: the game stopped early (see out/run.txt)"}'
# With the 30 FPS cap the average is ~30: the CPU time per frame of the draw thread and the command
# processor (SOTC_FPS=1, from the third report on, past the loading) tells two builds apart.
grep "SOTC CPU time per frame" out/run.txt | awk 'NR>2 {d+=$8; c+=$12; n++}
    END {if (n) printf "CPU per frame: draw thread %.2f ms, command processor %.2f ms (%d reports)\n", d/n, c/n, n}'
