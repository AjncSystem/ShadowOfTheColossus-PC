#!/usr/bin/env bash
# Shadow of the Colossus PC on Linux: settings in sotc.ini (see sotc.py). Build first: ./build.sh
set -euo pipefail
cd -- "$(dirname -- "$0")"
exec python3 sotc.py "$@"
