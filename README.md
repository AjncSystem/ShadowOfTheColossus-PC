# Shadow of the Colossus — PC (native port) · v0.58-rc1

[Releases](../../releases) · [Changelog](CHANGELOG.md)

> **No game or Sony files are included.**
> You need your own game dump and, for sound, two modules from your own console.
> This project is not affiliated with Sony Interactive Entertainment or Bluepoint Games.

---

## English

A native Windows port of the PS4 **Shadow of the Colossus** (Bluepoint, 2018; CUSA08809, EU 1.01)
for **Windows 10/11**. The game's own x86-64 code runs directly on the PC's CPU and the PS4 GPU
is translated to Vulkan. It is not a full emulator: it is a loader with the system functions the
game needs, in the style of [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc).

### What's new

| Version | Improvements and fixes |
|---|---|
| **0.58-rc1 (AMD preview)** | Caches Vulkan image-format capability queries, based on reports of repeated slow queries on AMD; not yet measured on a Radeon. `SOTC_WAVE64_UNIFORM=0/1` is now part of the shader profile and uses a separate cache. **AMD validation is still needed; the RDNA3 graphics corruption in [#43](../../issues/43) is not fixed in this preview.** |
| **0.57** | **AMD cards:** fix for the green/pink fog and characters (AMD now uses the NVIDIA 32-lane path; diagnosis from issue #7, not yet tested on an AMD card). **New launcher option** to turn off direct memory access: ~10% more FPS, flatter distant water. |
| **0.56** | **Open-world water with waves:** lakes and rivers below the waterfalls are no longer flat as a mirror (a shader instruction, `S_WQM_B64`, was ignored). **The upper lake ripples and reflects far away** (direct memory access on by default, as in Pink-shadPS4). Distant trees without "colored garbage". |
| **0.0.13** | **No more crashes:** fixed the 0.0.12 crash in the intro/menu (the game asked for more textures than its pool holds). Tested ~40 min across the map with two save loads. **Lake reflections** over almost the whole surface, with ripples (a shader loop guard cut the reflection rays short). |
| **0.0.12** | **Sharp textures back, without crashes:** fixed the memory corruption that closed the game after loading a save, and texture streaming is back (much sharper rocks and walls). **Water:** splashes, rings and foam while swimming; the lake reflects the sky and the sun's glint; environment reflections use each probe's correct cubemap (fix from Pink-shadPS4). |
| **0.0.11** | Fixed the "colored garbage" grass texture of 0.0.10. |

Full details in the [CHANGELOG](CHANGELOG.md) (in Portuguese).

### Known issues

- The first start after updating rebuilds the shaders: the first load is slow and stutters.
- **AMD** GPUs: the 0.57 color fix and 0.58-rc1 optimizations still need Radeon testing. The
  RDNA3 graphics corruption in [#43](../../issues/43) has no confirmed fix; attach
  `user/last_run.log` and include GPU model/driver when reporting.
- With 8 GB of video memory, usage gets close to the limit in large areas.

### Download and use

1. Download `SotC-PC.zip` from [Releases](../../releases) and unzip it into a folder.
2. Run **`SotC.exe`**. On first start the launcher searches the PC's drives for:
   - the game folder (the CUSA08809 dump with `eboot.bin`, `sce_module` and `sce_sys`);
   - the sound modules (`libSceNgs2.sprx` and `libSceUlt.sprx`) of an installed shadPS4
     (`user/sys_modules`), which it copies to the port's data folder;
   - a shadPS4 save of the game, which it offers to import;
   - the game's DLCs, if any (shadPS4's `addcont` folder, or a folder with one subfolder per DLC).
3. Check the **Game and saves** page. The **game library** works like shadPS4's.
4. Click **PLAY**.

Requirements: Windows 10/11 64-bit and a **Vulkan 1.3** GPU (NVIDIA, AMD or Intel with a recent
driver). No Python install needed.

| What | Where from | What for |
|---|---|---|
| CUSA08809 game dump (EU 1.01 preferred) | your PS4, through GoldHEN | the game |
| decrypted `libSceNgs2.sprx`, `libSceUlt.sprx` | your PS4 (`/system/common/lib/`, GoldHEN FTP), or shadPS4's `user/sys_modules` | sound (silent without them) |

The patch modes (60 FPS, 16x filtering) only apply to EU 1.01; other versions run at 30 FPS.

### File and texture mods

The port accepts author-prepared file replacement mods; no game files or texture packs are
distributed. Create `mods/Mod name/dvdroot_ps4/...` beside `SotC.exe` and put the replacement
files there. They are applied through a temporary view of the dump, leaving the original files,
saves and game installation untouched. New mods are enabled automatically; order and disabled
mods can be configured in `user/mods.json`.

This does not convert PNG/JPEG images into game textures. Textures must be supplied in compatible
files and formats already prepared by the mod author. Replacements for executables or system
modules are not supported.

### Launcher features

Game library and automatic search, shadPS4 save import, 720p–4K output (the game renders at
1080p, upscaled with FSR 1 + RCAS), PS4 Pro mode, 60 FPS and 16x AF modes, controller support
(DualShock 4, DualSense, Xbox) with mouse camera, key remapping, Bluepoint engine cvars, desktop
shortcut and a session log in `user/last_run.log`.

In 60 FPS mode the title-screen attract demo averages ~67 FPS on an RTX 2060 SUPER, with
29–34 FPS in its heaviest section. Vulkan only (no DirectX). AMD and Intel can enable the
NVIDIA-only submission options under **Image and FPS > GPU work submission**.

**Why no sound files?** The two `.sprx` files are Sony firmware and may not be redistributed;
the launcher finds and copies yours automatically. **The game closed by itself?** Open an issue
with `user/last_run.log`.

---

## Building

Windows with [MSYS2](https://www.msys2.org/) (CLANG64). Packages:
[packaging/windows/README.md](packaging/windows/README.md). To freeze `SotC.exe`: a Windows
Python with PyInstaller and Pillow (`WINPYTHON`).

```
bash build.sh                              # out/bb-probe.exe
bash packaging/windows/package_sotc.sh     # dist/SotC-PC.zip
```

Packaging refuses any `.sprx`/`.prx`: Sony firmware never goes into the package. Test tools:
`tools/`. Diagnostic variables: [docs/sotc/README-SotC.md](docs/sotc/README-SotC.md).

## Credits

- [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) (Supermedo, deadinside28 and
  forks): loader, native runtime and launcher. Original README:
  [docs/bbport](docs/bbport/README.md).
- [shadPS4](https://github.com/shadps4-emu/shadPS4) and the shadPS4 SotC fork: Vulkan video core.
- [Pink-shadPS4](https://github.com/luizgustavs/Pink-shadPS4) (luizgustavs): culling cvars, BPE
  heap guard, periodic submission, command recording thread, shader fixes (fetch-shader
  constants, `V_ALIGNBIT`, `V_CVT_PK_U8_F32`), dynamic T# tables (`dynamic_tsharp_array_size`),
  the LOD statistics approach and direct memory access for SotC.
- 60 FPS patch: illusion (GoldHEN cheats).

License: **GPL-2.0-or-later** ([LICENSE](LICENSE)).
