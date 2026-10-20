#!/usr/bin/env bash
# Linux / Steam Deck: builds the port (unless BB_PREBUILT=1) and starts Shadow of the Colossus
# through sotc.py, the same preparation and launch as on Windows (settings: see sotc.py).
#   bash run.sh [--software] [extra bb-probe arguments]
#   --software: Mesa's Lavapipe CPU renderer (slow; for machines without a Vulkan GPU).
# BB_PREBUILT=1 (packaged builds): out/bb-probe and its GPU library are installed next to this
# script; nothing is built and no nix-shell is needed.
# BB_GAME_DIR is accepted for SOTC_GAME_DIR (the dumped game folder, default ../CUSA08809).
set -euo pipefail
cd -- "$(dirname -- "$0")"
if [[ ${1:-} == --software ]]; then
    shift
    if [[ -z ${VK_DRIVER_FILES:-} ]]; then
        for candidate in /run/opengl-driver/share/vulkan/icd.d/lvp_icd*.json /usr/share/vulkan/icd.d/lvp_icd*.json; do
            if [[ -f $candidate ]]; then export VK_DRIVER_FILES=$candidate; break; fi
        done
    fi
    if [[ -z ${VK_DRIVER_FILES:-} ]]; then echo 'Lavapipe not found; set VK_DRIVER_FILES.' >&2; exit 1; fi
    export VK_LOADER_LAYERS_DISABLE='~implicit~'
fi
if [[ -z ${BB_PREBUILT:-} && -z ${BB_IN_NIX_SHELL:-} ]] && ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3; } && command -v nix-shell >/dev/null; then
    args=''; if (( $# )); then args=$(printf '%q ' "$@"); fi
    exec env BB_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash run.sh $args"
fi
if [[ -z ${PYTHON:-} ]]; then
    PYTHON=$(command -v python3 || true)
    if [[ -z $PYTHON ]]; then
        for candidate in /nix/store/*-python3-*/bin/python3; do
            if [[ -x $candidate ]]; then PYTHON=$candidate; break; fi
        done
    fi
fi
if [[ -z ${PYTHON:-} ]]; then echo 'Install Python 3 or set PYTHON.' >&2; exit 1; fi
if [[ -n ${BB_GAME_DIR:-} && -z ${SOTC_GAME_DIR:-} ]]; then export SOTC_GAME_DIR=$BB_GAME_DIR; fi
if [[ -z ${BB_PREBUILT:-} ]]; then bash build.sh; fi
exec "$PYTHON" sotc.py "$@"
