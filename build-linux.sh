#!/usr/bin/env bash
# build.sh for Linux: cross builds bb-probe.exe / bb-gpu-capabilities.exe (host clang + lld and an
# MSYS2 CLANG64 sysroot) into out/cross. First run: packaging/linux-cross/fetch_sysroot.py.
# CLANG64_ROOT (default ~/.cache/sotc-clang64), BB_LTO (default ON), BB_PGO (default off).
set -euo pipefail
cd -- "$(dirname -- "$0")"
root=${CLANG64_ROOT:-$HOME/.cache/sotc-clang64}
if [[ ! -d $root/clang64/include ]]; then
    echo "No sysroot in $root: run packaging/linux-cross/fetch_sysroot.py $root" >&2; exit 1
fi
for tool in clang clang++ ld.lld llvm-windres llvm-ar cmake ninja pkg-config glslang; do
    command -v "$tool" >/dev/null || { echo "Need $tool on the host." >&2; exit 1; }
done
if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ||
      ! -f gpu/third_party/xbyak/xbyak/xbyak.h ]]; then
    git submodule update --init --recursive
fi
for patch in gpu/patches/fsr-vulkan/*.patch; do
    if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
        git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
    fi
done
mkdir -p out/cross pgo
cmake -S gpu -B out/cross/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/packaging/linux-cross/clang64-toolchain.cmake" \
    -DCLANG64_ROOT="$root" -DBB_PGO="${BB_PGO:-off}" -DBB_PGO_DIR="$PWD/pgo" \
    -DBB_LTO="${BB_LTO:-ON}" > out/cross/cmake.log 2>&1 || { tail -30 out/cross/cmake.log >&2; exit 1; }
echo "Cross build: LTO ${BB_LTO:-ON}, PGO ${BB_PGO:-off}, sysroot $root"
if ! ninja -C out/cross/gpu ${NINJA_FLAGS:-} bb-probe bb-gpu-capabilities > out/cross/build.log 2>&1; then
    grep -v '^\[' out/cross/build.log | tail -40 >&2
    echo 'Build failed (full log: out/cross/build.log)' >&2; exit 1
fi
cp out/cross/gpu/bb-probe.exe out/cross/gpu/bb-gpu-capabilities.exe out/cross/
echo "Built $PWD/out/cross/bb-probe.exe"
