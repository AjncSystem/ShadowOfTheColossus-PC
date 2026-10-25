#!/usr/bin/env bash
# sotc: replays a recorded route (tools/routes/*.txt) at 3840x1080 and reports the page pool and crashes.
#   tools/sotc_route_test.sh [route] [label] [extra --env NAME=VALUE ...]
# Needs out/wide/{patches,cmdline}.bin (scripts/sotc_modes.py --render 3840x1080 --modes fps60 --out out/wide).
cd "$(dirname "$0")/.." || exit 1
export PATH=/c/msys64/clang64/bin:$PATH
source tools/bench_common.sh
route=${1:-tools/routes/deserto_wide.txt}; label=${2:-route}; shift 2
log=out/route_$label.log
timeout 900 $PY tools/sotc_bench_shrine.py --replay "$route" --assist "" --log "$log" \
  --patches out/wide/patches.bin --cmdline out/wide/cmdline.bin --env SOTC_FPS_CAP=off \
  --env SOTC_RENDER_RES=3840x1080 --env SOTC_OUTPUT_RES=3840x1080 --env SOTC_LOD_LINGER=1 \
  --env SOTC_LOD_POOL_LOG=1 --env BB_DMEM_MB=9152 --env SOTC_NEO=0 "$@" 2>&1 | tail -1
echo "min free pages: $(grep 'LOD POOL' "$log" | sed 's/.*(\([0-9]*\) of.*/\1/' | sort -n | head -1)  mip changes: $(grep -c 'SOTC LOD: game' "$log")  fault 0x1a371: $(grep -c '0x1a371' "$log")"
