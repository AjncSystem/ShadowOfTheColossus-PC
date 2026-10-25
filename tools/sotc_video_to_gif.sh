#!/usr/bin/env bash
# sotc: turns a demo video (tools/sotc_bench_shrine.py --video) into the README GIF.
# usage: tools/sotc_video_to_gif.sh in.mp4 out.gif [start_seconds] [length_seconds] [width] [fps]
# Defaults: from 2 s, 6 s long, 720 px wide, 12 FPS. ffmpeg: FFMPEG or PATH.
set -euo pipefail
in=$1; out=$2; start=${3:-2}; len=${4:-6}; width=${5:-720}; fps=${6:-12}
ffmpeg=${FFMPEG:-ffmpeg}
palette=$(mktemp --suffix=.png)
filters="fps=$fps,scale=$width:-1:flags=lanczos"
"$ffmpeg" -v error -y -ss "$start" -t "$len" -i "$in" -vf "$filters,palettegen=max_colors=128" "$palette"
"$ffmpeg" -v error -y -ss "$start" -t "$len" -i "$in" -i "$palette" -lavfi "$filters [x]; [x][1:v] paletteuse=dither=bayer:bayer_scale=5" "$out"
rm -f "$palette"
ls -la "$out"
