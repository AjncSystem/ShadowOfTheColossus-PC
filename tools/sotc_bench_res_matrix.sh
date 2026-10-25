#!/usr/bin/env bash
# sotc: Shrine FPS and GPU use (nvidia-smi, 1 s samples) per render size and upscaler.
#   bash tools/sotc_bench_res_matrix.sh [measure_seconds]   -> out/res_matrix.txt
# Needs out/native4k, out/r1440 (scripts/sotc_modes.py --render WxH --modes fps60 --out ...) and out/bench.patches.
cd "$(dirname "$0")/.." || exit 1
export PATH=/c/msys64/clang64/bin:$PATH
source tools/bench_common.sh
M=${1:-30}
RES=out/res_matrix.txt
: > "$RES"
run() { # name patches_dir render output upscaler(none|fsr1|dlss)
    local name=$1 dir=$2 render=$3 output=$4 up=$5
    local patches=out/bench.patches cmdline=out/bench.cmdline
    [[ $dir != - ]] && patches=$dir/patches.bin && cmdline=$dir/cmdline.bin
    local env=(--env SOTC_FPS_CAP=off --env SOTC_OUTPUT_RES=$output --env SOTC_NEO=0 --env BB_RCAS=0 --env BB_DMEM_MB=9152)
    [[ $render != - ]] && env+=(--env SOTC_RENDER_RES=$render)
    case $up in
        none) env+=(--env BB_FSR1=0) ;;
        fsr1) env+=(--env BB_FSR1=1) ;;
        dlss) env+=(--env BB_FSR1=0 --env SOTC_UPSCALER=dlss) ;;
    esac
    ( while true; do nvidia-smi --query-gpu=utilization.gpu,memory.used --format=csv,noheader,nounits; sleep 1; done ) > out/res_gpu.txt &
    local poller=$!
    local line
    line=$(timeout 480 $PY tools/sotc_bench_shrine.py --measure "$M" --shots 1 --log out/res_$name.log --patches "$patches" --cmdline "$cmdline" "${env[@]}" 2>&1 | tail -1)
    kill $poller 2>/dev/null
    local gpu
    gpu=$(tail -n "$M" out/res_gpu.txt | awk -F', *' '{u+=$1; if ($1>m) m=$1; v=$2; n++} END {if (n) printf "GPU avg %.0f%% max %d%%, VRAM %d MiB", u/n, m, v}')
    echo "$name | $line | $gpu" | tee -a "$RES"
}
run 1080_native      -               -         1920x1080 none
run 1080_to_4k_fsr1  -               -         3840x2160 fsr1
run 1080_to_4k_dlss  -               -         3840x2160 dlss
run 1440_native      out/r1440       2560x1440 2560x1440 none
run 1440_to_4k_dlss  out/r1440       2560x1440 3840x2160 dlss
run 4k_native        out/native4k    3840x2160 3840x2160 none
