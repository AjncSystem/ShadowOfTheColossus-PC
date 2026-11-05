#!/usr/bin/env bash
# sotc: the forest entrance route (T48): record it once, then replay and measure it with the
# player's launcher settings (tools/launcher_env.py).
#   tools/route_forest.sh record            the player rides the route, F9 stops, close the game
#   tools/route_forest.sh replay [NAME=VALUE ...]   measures a replay (out/route_forest.run.txt)
cd "$(dirname "$0")/.."
INSTALL=${SOTC_INSTALL:-"E:/GAMES/Shadow Of The Colossus/SotC-PC"}
export SOTC_GAME_DIR=${SOTC_GAME_DIR:-"E:/GAMES/Shadow Of The Colossus/CUSA08809"}
export PATH="/c/PyBuild312:/usr/bin:/c/WINDOWS/system32:/c/WINDOWS:/i/msys64/clang64/bin"
PY=/c/PyBuild312/python.exe
mode=$1; shift
extra=()
for item in "$@"; do extra+=(--extra "$item"); done
mapfile -t envargs < <($PY -I tools/launcher_env.py "$INSTALL/user/launcher_settings.json" \
    --modes-file out/route_modes.txt "${extra[@]}" --skip BB_CONFIG --skip SOTC_USER_DIR --skip SOTC_TEX_REPLACE)
envargs+=(--env "BB_CONFIG=$INSTALL/sotc.ini" --env "SOTC_TEX_REPLACE=$INSTALL/glyphs/xbox")
{ read -r modes; read -r cvars; read -r render; } < <(tr -d '\r' < out/route_modes.txt)
$PY scripts/sotc_modes.py "$SOTC_GAME_DIR" --out out --modes "$modes" --cvars "$cvars" \
    ${render:+--render "$render"} >/dev/null || exit 1
cp out/patches.bin out/route.patches; cp out/cmdline.bin out/route.cmdline
case $mode in
record)
    # The player's save as it is now, kept for every replay (later saves do not move the start).
    rm -rf out/route-save && mkdir -p out/route-save && cp -r "$INSTALL/user/savedata/1/." out/route-save/
    exec $PY tools/sotc_bench_shrine.py --record out/route_forest.txt --template out/route-save --after-load 15 --patches out/route.patches \
        --cmdline out/route.cmdline --log out/route_forest.record.txt "${envargs[@]}" ;;
replay)
    export SOTC_WINDOW_BEHIND=1
    $PY tools/sotc_bench_shrine.py --replay out/route_forest.txt --template out/route-save --after-load 15 --assist "" --patches out/route.patches \
        --cmdline out/route.cmdline --log out/route_forest.run.txt "${envargs[@]}"
    $PY tools/route_report.py out/route_forest.run.txt ;;
*) echo "usage: $0 record|replay [NAME=VALUE ...]" >&2; exit 2 ;;
esac
