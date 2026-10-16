# Shadow of the Colossus — PC (native port)

Native Windows port of **Shadow of the Colossus** for PS4 (CUSA08809, EU version 1.01).
The game code runs directly on the CPU. This is not a full emulator: it includes a loader and
the system functions the game needs. The GPU uses the Vulkan video core from the shadPS4 SotC
fork. It is based on [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) (GPL-2.0).

This package **does not include the game or Sony firmware**. You must provide your own.

## Requirements

- Windows 10/11 64-bit and a GPU with Vulkan 1.3.
- Python is not required for `SotC.exe`; only `SotC.bat` requires
  [Python 3](https://www.python.org/downloads/).
- **Your own dump** of game CUSA08809 (folder containing `eboot.bin`, `sce_module`, `sce_sys`, etc.).
- For audio, you need decrypted `libSceNgs2.sprx` and `libSceUlt.sprx` from **your own console**.
  These are the same files as in shadPS4’s `user/sys_modules` folder; on a PS4 with GoldHEN,
  they are in `/system/common/lib/` (FTP on port 2121). The game works without them, but has no sound.

## How to play

1. Open `SotC.exe`, the launcher.
2. On first launch, it **automatically searches** the PC’s drives for:
   - the game folder (CUSA08809);
   - the two audio `.sprx` files from an installed shadPS4 (the `user/sys_modules` folder), which it copies
     into the port’s data folder;
   - a game save in shadPS4, which it offers to import;
   - the game’s DLCs (shadPS4’s `addcont` folder or a folder with one subfolder per DLC).
3. Check **Game and saves**. The **game library** works like shadPS4: add the folders where your
   games are stored, then select a game from the list (double-click). **Search PC** repeats the
   automatic search.
4. Click **PLAY**.

The **Play** screen checks everything first: the game version, audio modules, saves, and graphics card.
The game image is prepared the first time, which takes a few seconds. This is repeated only when
the game or modules change. Saves and the shader cache are stored in `user/`.

Without the launcher, `SotC.bat` does the same by reading `sotc.ini`. `SotC.exe --play` starts the game
directly with the launcher’s most recent settings, which is useful for shortcuts and Steam.

## File and texture mods

Ready-made mods that replace game files can be placed in `mods/Mod name/dvdroot_ps4/...`
next to `SotC.exe`. The port applies replacements to a temporary view; it does not modify the
original dump, saves, or installation. New mod folders are enabled automatically. The order and
disabled mods are stored in `user/mods.json`.

This does not turn PNG/JPEG files into game textures: use only resource files in formats the game
loads, prepared by the mod author. No texture pack is included. To change the paths/behavior, set
`SOTC_MODS_DIR`, `SOTC_MODS_CONFIG`, or `SOTC_MODS_ENABLED=0` in the environment or in `sotc.ini`.
Mods that replace `eboot.bin`, modules, or system files are not supported.

## Graphics and performance

All of these settings are in the launcher’s **Graphics and FPS** page (the main ones are also under **Play**):

- **Output** 720p, 1080p, 1440p, or 4K. The game renders at 1080p, as on PS4, and the image is
  upscaled to the output resolution with **FSR 1** + sharpening (RCAS) or bilinear filtering. The
  window never exceeds 90% of the screen; fullscreen uses the entire monitor.
- **60 FPS** (experimental): removes the game’s 30 FPS cap (illusion’s GoldHEN patch), with
  vblank at 120 Hz. Measured 38 FPS in the open field and ~25 in the temple.
- **Sharp textures**: 16x anisotropic filtering, with almost no cost.
- **PS4 Pro mode**: the game identifies as a PS4 Pro and offers **Cinematic** (1440p, more detail)
  or **Performance** graphics modes in its own menu (Options > Presentation). It is more demanding:
  ~25 FPS in the open field.
- **Effects**: vignette, atmospheric fog, and ambient occlusion (SSAO) can be disabled.
  Motion blur, brightness, and filters are controlled in the game’s own menu.
- **Engine cvars** (Advanced): Bluepoint engine variables in `NAME=value` format, for those who want
  to experiment.

Patch modes apply only to EU version 1.01; they are not applied to other versions.

## Controls

**Mouse:** click the game window to capture the mouse, which then rotates the camera; press Esc or
switch windows to release it. Left button = R1 (grab), right = □ (attack), middle = L1. Sensitivity
and inversion are in **Controls** in the launcher. To remap mouse buttons, use
`mouse.<input>=left,right,middle,x1,x2`.

A controller (DualShock 4, DualSense, Xbox...) is used automatically. If several are connected,
select one in **Controls** in the launcher (or with `BB_GAMEPAD=` in `sotc.ini`). The keyboard works
alongside the controller:

| Keyboard | PS4 |
|---|---|
| W A S D | left stick |
| setas | right stick (camera) |
| Espaço | ✕ |
| Shift esquerdo | ○ |
| E | □ |
| Q | △ |
| 1 / 3 | L1 / R1 |
| R / F | L2 / R2 |
| Z / C | L3 / R3 |
| Enter | Options |
| I J K L | D-pad |
| Tab / Backspace | touchpad (left / right) |

Keys can be changed in **Controls** in the launcher: click an action and press the new key.
The controller and mouse can also be remapped in `sotc.ini`, for example `pad.circle=b`.
The inputs are:

- buttons: `cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad touchpad_right`;
- D-pad: `up down left right`;
- for keyboard sticks: `move_up` `move_down` `move_left` `move_right` and
  `look_up` `look_down` `look_left` `look_right`.

## Status

Working: logos, intro, menus with text, loading saves, horseback exploration,
audio (with the user’s modules).

Performance measured in the title-screen demo, 60 FPS mode, i9-13900K + RTX 2060 SUPER:
~67 FPS average; the heaviest section (the canyon bridge) runs at 29–34 FPS. In normal
mode, the game is locked at 30 FPS, as on PS4.

## Frequently asked questions

- **DirectX 11/12?** No. The video core (shadPS4) translates the PS4 GPU to Vulkan; the game’s
  shaders become SPIR-V. Any recent DirectX 12 graphics card also supports Vulkan 1.3: update your
  driver. A DirectX mode would require rewriting the entire video core.
- **AMD and Intel cards?** The optimizations work on any card, except for two that are enabled
  automatically only on NVIDIA (periodic GPU work submission and read-ahead). In **Graphics and FPS >
  GPU work submission**, choose **On** to test them on AMD or Intel; if the screen turns black,
  switch back to **Automatic**.
- **Why does audio require my own files?** `libSceNgs2.sprx` and `libSceUlt.sprx` are Sony
  firmware and cannot be distributed. Get them from your PS4 (or the shadPS4 you already use).

## Credits

- [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) (bbport, deadinside28, and forks): loader,
  native runtime, and launcher; the base for this port.
- [shadPS4](https://github.com/shadps4-emu/shadPS4) and the shadPS4 SotC fork: Vulkan video core.
- [Pink-shadPS4](https://github.com/luizgustavs/Pink-shadPS4) (luizgustavs): culling cvars, protection
  of the game heap against GPU reads (`bpe_heap_guard`), periodic flush, read-ahead, the wait marker,
  and the Vulkan command recorder on its own thread (`cp_record_thread`).

## Diagnostics

These environment variables help investigate issues:

| Variable | Effect |
|---|---|
| `SOTC_FPS=1` | frame rate every 5 s |
| `SOTC_FRAME_TIMES=1` | p50/p95/p99 percentiles and maximum frame interval, every 5 s; in the benchmark, pass `--env SOTC_FRAME_TIMES=1` |
| `BB_FAULT_RING=file` | records recent exceptions (survives crashes) |
| `BB_STACKS_TRIGGER=file` | creating the file dumps thread stacks |
| `BB_READBACKS=0/1/2` | GPU readbacks; the default 2 is required by the game |
| `SOTC_RECORD_THREAD=0` | records Vulkan commands on the command thread itself (default: separate thread) |
| `SOTC_INDIRECT_DIRECT=0` | keeps draws with CPU-side arguments indirect |
| `SOTC_STREAM_THRESHOLD=<bytes>` | threshold for read-only buffers copied to the stream buffer (`0` disables; default: `0`) |
| `SOTC_WAIT_MARKER=1/2` | GPU waits poll a marker written by the GPU (experimental) |
| `SOTC_CMD_STATS=1/2` | draws and dispatches per frame (2: most-used compute shaders) |
