# Building and running natively on Linux

The port builds and runs natively on Linux (POSIX). This was verified on
**CachyOS (Arch) · clang 23 · NVIDIA RTX 3060 (driver 615.78, Vulkan 1.4.363) · SDL3 3.4.18**.
The `build.sh` POSIX branch produces `out/bb-probe` (the loader) and `out/gpu/libbbgpu.so`
(the shadPS4 video core); `run.sh` launches the game from your own dump.

The NixOS/SteamOS path (`shell.nix`, `packaging/default.nix`, the AppImage) is unchanged.
This document covers a **plain, non-Nix** Linux build.

## 1. Dependencies

Arch / CachyOS:

```sh
sudo pacman -S --needed \
  base-devel cmake ninja pkgconf git \
  vulkan-icd-loader vulkan-headers \
  sdl3 ffmpeg fmt boost robin-map xxhash \
  glslang spirv-cross spirv-headers zydis \
  libx11 libxcb wayland
```

`miniz` is optional: when the system package is absent, CMake builds the vendored
`gpu/third_party/miniz` submodule instead.

### VulkanMemoryAllocator header

The GPU library needs `vk_mem_alloc.h`. `gpu/CMakeLists.txt` looks for it with
`find_path(VMA_INCLUDE_DIR vk_mem_alloc.h PATH_SUFFIXES vma REQUIRED)`. Arch has no
package for it, so drop the single-header library (MIT) where CMake can find it:

```sh
mkdir -p third_party/vma
curl -L -o third_party/vma/vk_mem_alloc.h \
  https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/master/include/vk_mem_alloc.h
```

On NixOS this comes from the `vulkan-memory-allocator` package in `shell.nix`.

## 2. Build

```sh
git clone --recursive https://github.com/AjncSystem/ShadowOfTheColossus-PC
cd ShadowOfTheColossus-PC

export CMAKE_INCLUDE_PATH="$PWD/third_party"   # so CMake finds third_party/vma/vk_mem_alloc.h
bash build.sh
```

Recommended on a rolling-release distro with a very new GCC: build with clang (what the
project is tested against) and disable LTO if the toolchain's LTO is problematic:

```sh
CC=clang CXX=clang++ BB_LTO=OFF bash build.sh
```

Expected result:

```
out/bb-probe              (~1 MB)
out/gpu/libbbgpu.so       (~68 MB, the shadPS4 video core)
out/bb-gpu-capabilities
```

Smoke tests:

```sh
./out/bb-gpu-capabilities     # should report your GPU and "supports live presets"
bash build.sh --test          # runtime unit tests
```

## 3. Run

Point the loader at your own **decrypted** game folder (the dump with `eboot.bin`,
`sce_module/` and `sce_sys/`), then:

```sh
BB_GAME_DIR=/path/to/CUSA08809 bash run.sh
```

`run.sh` runs `scripts/prepare.py`, `link_libc.py`, `link_modules.py` and
`content_profile.py` to produce `out/boot.bin`, then execs `out/bb-probe`.

For sound, place the decrypted `libSceNgs2.sprx` and `libSceUlt.sprx` **from your own
console** in `user/sys_modules/` (see the main README). Note the audio backend is still
HLE: `runtime_sotc.c` renders silence for now.

## 4. Game version note

The enhancement patches in `patches/Bloodborne.xml` target **APP_VER 01.09** (the bbport
lineage). A dump of a different version (e.g. EU 1.01) is detected and runs **unpatched**
at the game's native 30 FPS — `run.sh` prints:

```
Patches: game version 01.01, patches are for 01.09: none applied (30 FPS, no effect or resolution patches)
```

This is expected and not an error; the core game still runs.

## 5. Known Linux build issues fixed here

The following were required to build on a current Linux toolchain (clang 23 / GCC 16,
`-Werror`). They are included in this branch:

| File | Issue | Fix |
|---|---|---|
| `build.sh` | committed with CRLF endings → `set: pipefail: invalid option name` | convert to LF |
| `gpu/shadps4/video_core/page_manager.cpp` | `MAX_PATH` used outside `#ifdef _WIN32` | define it on POSIX (as `common/path_util.cpp` does) |
| `gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp` | lambda returns two different 64-bit types (`unsigned long long` vs `u64`) | `static_cast<u64>` |
| `gpu/CMakeLists.txt` | `third_party/xbyak` added to includes only under `if(WIN32)` | move to the common include list |
| `src/runtime_sotc.c` | uses `syscall`/`usleep`/`random`/`tm_gmtoff` without `_GNU_SOURCE` | define `_GNU_SOURCE` |
| `src/runtime_services.c` | `common_initialized` set-but-unused (`-Werror`) | `volatile` |
| `src/probe.c` | calls `install_heap_hook()` (Windows-only) unguarded | wrap in `#ifdef _WIN32` |
| `src/runtime_content.c` | `sfo_string`/`addon_known` unused on POSIX (Windows-only scan path) | `__attribute__((unused))` |
| `gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp` | references `runtime_sotc_heap` (defined only under `_WIN32` in `probe.c`) | POSIX stub returning 0 (no heap hook on Linux) |
| `scripts/link_libc.py` | `identity()` raised on non-NID symbols like `__start__Ztext` | only parse `name#lib#mod` symbols |
