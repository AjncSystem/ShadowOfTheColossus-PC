#!/usr/bin/env bash
# sotc: builds bb-probe.exe with the PGO profile of one GPU vendor (pgo/vendor/<vendor>/bbgpu.profdata,
# vendor = nvidia, amd_intel) into out/vendor-bin/<vendor>/bb-probe.exe; package_sotc.sh puts it in the
# matching "Otimizada" zip. Without a profile for the vendor nothing is built and the zip keeps the
# default build (pgo/bbgpu.profdata, trained on NVIDIA). See docs/sotc/PGO.md.
#   tools/pgo_vendor_build.sh nvidia|amd_intel
set -euo pipefail
cd "$(dirname "$0")/.."
vendor=${1:?usage: pgo_vendor_build.sh nvidia|amd_intel}
profile=pgo/vendor/$vendor/bbgpu.profdata
[[ -f $profile ]] || { echo "no profile for $vendor ($profile): default build kept" >&2; exit 0; }
# shellcheck source=../msys2-env.sh
source ./msys2-env.sh
build=out/gpu-$vendor
cmake -S gpu -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO=use \
    -DBB_PGO_DIR="$PWD/pgo/vendor/$vendor" -DBB_LTO=ON -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_RC_COMPILER=llvm-windres >/dev/null
ninja -C "$build" bb-probe
mkdir -p "out/vendor-bin/$vendor"
cp "$build/bb-probe.exe" "out/vendor-bin/$vendor/bb-probe.exe"
echo "out/vendor-bin/$vendor/bb-probe.exe built with $profile"
