# Shadow of the Colossus — PC native port · v0.60.6-plus1

**The original 0.60.6, plus CPUs without BMI1 (pre-Haswell), CPUs with 4 threads, 120 Hz
fullscreen and a 32-lane fix for AMD GCN cards (RX 470–590, to test).** Everything in [AjncSystem's 0.60.6](https://github.com/AjncSystem/ShadowOfTheColossus-PC)
(the game can be finished from start to end; the in-game menu with quick settings; textures on
GTX 10xx cards, 60 FPS through the game's own 60 FPS mode, faster heap guards, HUD-less frame
generation as an option, no crash with high quality textures and the smaller readback window, all
from this fork; the title-screen hang fix, low-memory PCs, a `user` folder per copy), with this
fork's work on top.

![Shrine of Worship, 60 FPS mode, RTX 5060 Ti (FPS from RTSS)](docs/img/sotc-fork.gif)

What it adds:
- **CPUs without BMI1/LZCNT/MOVBE** (Intel before Haswell: 2nd/3rd generation Core, Xeon E3 v1/v2;
  AMD FX), which got "This CPU is not supported": the game's ~5,000 such instructions are replaced
  by equivalent routines at startup, only on those CPUs (Oussama268), also in code reached only
  through jump tables (a hang on the first loading screen on a real one). AVX is still required.
  Confirmed on a real one (issue #88 of the original: played up to the first colossus).
- **CPUs with 4 threads** (4 cores without SMT, 2 cores with SMT): two of the port's GPU threads no
  longer spin on a core while they wait, so the game's threads get it. A Ryzen 7 held to 4 logical
  processors stayed stuck loading the attract demo; now it runs at 76 FPS (60 FPS mode). With more
  than 8 logical processors nothing changes (sent to the original as
  [PR #198](https://github.com/AjncSystem/ShadowOfTheColossus-PC/pull/198)).
- **AMD GCN cards (RX 470–590, Vega), to test:** white, flickering characters and smoke and fog
  colours that change every run. These cards run the shaders 64 wide. The port now lowers them to 32
  lanes where the driver requires a 32-lane compute subgroup (`GPU: AMD wave32 on` in the log;
  `SOTC_AMD_WAVE32=0` turns it off, `=1` forces it), and the LDS barriers are on for 64-wide AMD
  (`SOTC_AMD64_LDS=0` turns them off). On an RX 570 on Windows the flicker went away. **Open:** some
  GCN runs stop with a lost GPU device right after the first colossus (issues #187, #123 and #41 of
  the original). If that happens to you, try `SOTC_AMD_WAVE32=0` and send `last_run.log`.
- **Fullscreen at 120 Hz** on 144/165 Hz monitors, so every frame stays on screen as long (Anthonypc23).
- The launcher's self-update follows this fork's releases; the launcher no longer stops when its
  console refuses writes (issue #104).

**[⬇ Download the latest release](../../releases/latest)** · bring your own game dump · Windows 10/11, Vulkan 1.3

---

[Releases](../../releases) · [Changelog](CHANGELOG.md)

> **No game or Sony files are included.** You need your own game dump and, for sound, two
> modules from your own console. This project is not affiliated with Sony Interactive
> Entertainment or Bluepoint Games.

---

**Status: the game can be finished from start to end** (v0.60.3: played through the last
colossus, the ending and the credits, with high quality textures, PS4 Pro mode and 60 FPS on).

A native Windows port of the PS4 **Shadow of the Colossus** (Bluepoint, 2018; CUSA08809, EU 1.01)
for **Windows 10/11**. The game's own x86-64 code runs directly on the PC's CPU and the PS4 GPU
is translated to Vulkan. It is not a full emulator: it is a loader with the system functions the
game needs, in the style of [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc).

### What's new

| Version | Improvements and fixes |
|---|---|
| **0.60.6** | **No more rainbow garbage on textures and far fewer "closed out of nowhere" crashes** (the cause: speculative GPU readbacks written over memory the game had reused, after loading a save or a new area), **no more black dots on sandy ground**, **in-game menu cleaned up** (FPS, upscaler and sharpening now change at once; Bloodborne leftovers removed), **hard disk read-ahead** (fewer stalls in new areas on HDDs), textures refreshed on every load, a readable session event log (`user/events.log`). |
| **0.60.5-plus1** | **Lucas-blip-png fork:** the original's 0.60.5 merged into the fork (texture quality selector, memory leak fix, faster loading after a driver update), with the fork's AMD GCN 32-lane path, memory fallback and FSR 1 fixes kept. Not tested on a GCN card with both LDS barriers on yet. |
| **0.60.5** | **Texture quality selector: Off / High / Maximum** (#235: sharp again), **memory leak fixed** (~60 MB/min, long-session stutter), **+3% average and +12% minimum FPS** (descriptor binds reused), **loading after a GPU driver update: 106 s → 7 s**, possible fix for **white characters on AMD GCN** (RX 470–590, Vega), **4-thread CPUs** no longer stuck loading, a hang watchdog, short hitches no longer flash a black loading screen, code review fixes. |
| **0.60.4** | **No more cyan blocks on stone or blinking flowers** with high quality textures (the 0.60.3 crash protection switched the texture detail every second or two; now with hysteresis). |
| **0.60.3-plus6** | **Lucas-blip-png fork:** fixes a crash after the first colossus in Pro-mode saves when Windows refuses the game memory (the fallback tries 8, 7 and 6 GiB first). |
| **0.60.3-plus5** | **Lucas-blip-png fork:** hotfix: the AMD GCN 32-lane conversion is on by default only where the driver requires it (fixes a crash in the intro on Linux RADV). |
| **0.60.3-plus4** | **Lucas-blip-png fork:** **AMD GCN cards (RX 470–590, Vega): the white/green flicker is gone** (confirmed on an RX 570) with the 32-lane conversion on by default there; `SOTC_AMD_WAVE32=0` turns it off. |
| **0.60.3-plus3** | **Lucas-blip-png fork:** fixes two crashes: at boot when Windows refuses the 9152 MiB of game memory (small page file), and with **frame generation on PCs with an AMD and an NVIDIA card**; FSR 1 barriers. |
| **0.60.3-plus2** | **Lucas-blip-png fork:** a fix to test for **AMD GCN cards** (RX 470–590, Vega: white, flickering characters and smoke) and a little more FPS on **CPUs with 4 threads** (+4% on 2 cores with SMT). |
| **0.60.3-plus** | **Lucas-blip-png fork:** all of the original's **0.60.3** (the game can be finished; quick settings in the in-game menu), plus **CPUs with 4 threads** (the GPU threads no longer spin on a core the game needs: from stuck loading to 76 FPS on 4 logical processors), **CPUs without BMI1** and **120 Hz fullscreen**. |
| **0.60.3** | **The game can be finished from start to end** (played from the last colossus through the ending and credits): fix for the high quality textures crash in wide views (#186), fix for the crash on the load after the credits, +2.7% FPS at the Shrine. |
| **0.60.2** | **Quick settings in the in-game menu** (60 FPS, frame generation, high quality textures, far shadows, volume: apply at once and are kept), **the mouse works in the menu**, **animated loading screen while the shaders are prepared** at start. |
| **0.60.1-plus** | **Lucas-blip-png fork:** all of the original's **0.60.1** (which already has this fork's GTX 10xx textures, 60 FPS through the game's own mode, faster heap guards and HUD-less frame generation), plus **CPUs without BMI1** (Oussama268, with the fix for the hang on the first loading screen), **no crash with high quality textures** (0x1a371), a smaller readback window and **120 Hz fullscreen** (Anthonypc23). |
| **0.60.1** | **In-game menu** (Insert / F1 / L3+R3: FPS, GPU and VRAM, fullscreen, resolution, sharpening, mouse camera, effects), **Bloodborne leftovers removed** (~11,700 lines), the game exe has the SotC icon. |
| **0.60** | **Textures on GTX 10xx**, no title-screen hang, **starts on low-memory PCs**, **60 FPS through the game's own Performance mode** (right physics), **each copy has its own `user` folder**, +6% FPS at the Shrine, FSR 3 without black tiles on RX 7000. |
| **0.59-plus5** | **Lucas-blip-png fork:** no more **crash with high quality textures in PS4 Pro mode** (0x1a371: the game filled its own memory pool; now it asks for less detail when the pool is nearly full) and a fix for **CPUs without BMI1** that hung on the first loading screen. |
| **0.59-plus4** | **Lucas-blip-png fork:** 0.59-plus3 on top of the original's **0.59.1**: **AMD stripes** on shadows and water fixed, Intel fixes, a **Far shadows** launcher option, and the log shows the GPU and driver (Oussama268 and AjncSystem). |
| **0.59-plus3** | **Lucas-blip-png fork:** **60 FPS through the game's own 60 FPS mode** (the PS4 Pro Performance one): the game's timing follows 60 FPS, which should fix the bird colossus flying away, the 2nd colossus' wall and weak point damage; F7 switches the game's target between 30 and 60. Textures on GTX 10xx confirmed on a GTX 1080 Ti. |
| **0.59-plus2** | **Lucas-blip-png fork:** 0.59-plus plus **CPUs without BMI1/LZCNT/MOVBE** (2nd/3rd generation Intel Core, Xeon E3 v1/v2, AMD FX; by Oussama268), which could not start the game, **no black tiles on FSR 3 generated frames on AMD RX 7000** (KingJhoss), and the launcher's PLAY button is no longer cut off above 100% display scale. |
| **0.59-plus** | **Lucas-blip-png fork:** all of 0.59, plus **~7% more FPS** in the Shrine (47 → 50.6 FPS: heap guards no longer rebuilt every frame and a smaller readback window), **textures on GTX 10xx cards**, **far shadows that no longer blink**, **120 Hz fullscreen** on 144/165 Hz monitors (Anthonypc23) and frame generation without the HUD as an experimental option (off). The launcher self-update follows this fork's releases. The draw thread and DMA sweep of 0.58.1-plus are now part of the original. |
| **0.58.1-plus** | **Lucas-blip-png fork:** all of 0.58.1, **no crash a minute into a new game** (high quality textures, which cause it, off by default), **far shadows that no longer blink**, **frame generation without the HUD** (FSR 3/DLSS-G) and an update notice in the launcher, plus **~19% more FPS** in the Shrine (42 → 50 FPS, unlimited FPS): a draw thread separate from the command processor, the heap guard no longer rebuilt per readback, a smaller readback window and an incremental DMA sweep. **120 Hz fullscreen** on 144/165 Hz monitors (Anthonypc23). Its own shader cache for PS4 Pro mode. |
| **0.59.1** | **Far shadows no longer blink** (AMD and NVIDIA; new launcher option), **AMD stripes** on shadows and water fixed, Intel fixes, the log shows the GPU and driver. |
| **0.59** | **More FPS** (Oussama268's fork: draw thread, shaders translated in parallel; bridge 30 → 37–40 FPS), **fix for the ending and save-load crash** (still being tested: report in the issues), **F11 fullscreen**, **video memory use** and **volume** options, launcher **self-update**, ultrawide with PS4 Pro mode, fixes for 6 GB cards and Chinese Windows. |
| **0.58.1** | **High quality textures** (sharp leaves and rocks; the game had not streamed its high resolution versions since 0.0.13) and the **game window's own icon** (or the game's, or a picture of yours). |
| **0.58** | **Community release** (Lucas-blip-png, Gabriell-os, Oussama268, jownatan, xeodeo, nakaionyt/y7ngboy, tabnul): **fewer crashes** (out of video memory, game freezing, post-colossus crash in PS4 Pro mode), **+15% FPS** and less video memory, AMD fixes (RDNA3/4 black tiles), **21:9/32:9 widescreen**, **FPS 30/60/unlimited**, **Xbox button icons**, FSR 3 / DLSS-G frame generation (experimental), launcher in Spanish, Chinese, Korean and Russian. |
| **0.57** | **AMD cards:** fix for the green/pink fog and characters (AMD now uses the NVIDIA 32-lane path; diagnosis from issue #7, not yet tested on an AMD card). **New launcher option** to turn off direct memory access: ~10% more FPS, flatter distant water. |
| **0.56** | **Open-world water with waves:** lakes and rivers below the waterfalls are no longer flat as a mirror (a shader instruction, `S_WQM_B64`, was ignored). **The upper lake ripples and reflects far away** (direct memory access on by default, as in Pink-shadPS4). Distant trees without "colored garbage". |
| **0.0.13** | **No more crashes:** fixed the 0.0.12 crash in the intro/menu (the game asked for more textures than its pool holds). Tested ~40 min across the map with two save loads. **Lake reflections** over almost the whole surface, with ripples (a shader loop guard cut the reflection rays short). |
| **0.0.12** | **Sharp textures back, without crashes:** fixed the memory corruption that closed the game after loading a save, and texture streaming is back (much sharper rocks and walls). **Water:** splashes, rings and foam while swimming; the lake reflects the sky and the sun's glint; environment reflections use each probe's correct cubemap (fix from Pink-shadPS4). |
| **0.0.11** | Fixed the "colored garbage" grass texture of 0.0.10. |

Full details in the [CHANGELOG](CHANGELOG.md) (in Portuguese).

### In-game menu and shortcuts

Press **F1** (or **Insert**, or **L3+R3** on the controller) while playing. The mouse is free while
the menu is open; **Esc** or F1 closes it.

![In-game menu (F1)](docs/images/sotc-menu-f1-en.png)

- **Quick settings (apply at once):** FPS (30 / 60 / unlimited), upscaler (FSR 1 / bilinear),
  sharpening (RCAS), frame generation, **texture quality**, far shadow reuse, volume. They are
  also saved as the launcher's choice for the next start.
- **Display:** fullscreen, FPS counter.
- **Camera & mouse:** mouse camera on/off, sensitivity, invert Y.
- The header shows FPS, frame time, the GPU and its video memory in use. Output resolution,
  effects (fog, vignette, SSAO), 16x AF, PS4 Pro mode and video memory use stay in the launcher
  (they apply when the game starts).

| Key | What it does |
|---|---|
| **F1** / Insert / L3+R3 | Opens or closes the in-game menu |
| **F7** | 30 FPS cap on/off (60 FPS without the cap) |
| **F8** | Frame generation: off → DLSS-G → FSR 3 (when the GPU supports it) |
| **F10** | Performance overlay (FPS and frame time) |
| **F11** | Window / fullscreen |

### Texture quality

In the launcher (**Quick settings → Textures**) and in the in-game menu (changes at once):

| Level | What it does | Possible problems |
|---|---|---|
| **Off** | The game's 256×256 versions only (the behaviour before 0.58.1). | Blurry ground, rocks and leaves. For GPUs short of video memory. |
| **High (stable)** — default | The game streams its high resolution textures. When the game's texture memory runs low, the textures ask for a coarser level so the game frees space before it runs out. | A little less sharp in large, wide areas (the game's texture memory is almost always full there, so it stays on the coarser level; #235). |
| **Maximum (sharp)** | High resolution textures always, without that protection. | It **may close the game in very wide views** (sword raised against the sun, the Shrine, the last colossus' area; fault `0x1a371`, #186), mostly in **PS4 Pro mode**. Not seen in our 5–10 minute runs, but reported before. If it happens, go back to **High**. Uses more video memory. |

### Known issues

- The first start after updating rebuilds the shaders: the first load is slow and stutters.
- **Texture quality "Maximum"** can close the game in very wide views (sword raised against the
  sun, the last colossus' area; #186). Since 0.60.6 it is not available in PS4 Pro mode, where it
  closed within minutes.
- The first start after an update rebuilds the GPU driver's shader cache: about 2 minutes of
  loading screen, and short stalls the first time each area is visited.
- **AMD GCN** cards (RX 470–590, Vega, APUs on the legacy driver): characters and Agro may glow
  white (#152, #158, #187). 0.60.5 adds the LDS barriers these cards were missing (PR #205), which
  is the likely fix: please report whether it helps. `SOTC_AMD64_LDS=0` turns it off.
- **60 FPS:** since 0.60 the game targets 60 FPS itself (its PS4 Pro Performance mode), which
  should fix the 60 FPS physics (the 2nd colossus' wall, colossi "flying"); still being tested
  there. `SOTC_MODES=af16` in the extra variables goes back to the old mode.
- **Frame generation** (experimental): the health/stamina bars may shake (`SOTC_FG_HUDLESS=1`
  tries a HUD-less image), and it only matters at 30 FPS.
- With 8 GB of video memory or less, pick a lower **Video memory use** level if the game runs out.

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

## Roadmap

Public plan for this fork, updated as work lands (last update: 23/10/2026). Issue numbers refer to
the original repository.

**Part 1: upscalers (more FPS on weaker GPUs, sharper 1440p/4K output)**
- [x] FSR 1 with RCAS (in place since 0.60.x)
- [x] Phase 0: rendering below 1080p works with the game's own patches (23/10: Shrine, 3840x2160 output with FSR 1:
      1080p 38.7 FPS, 720p 46.9, 576p 49.7; clean image, no device lost)
- [x] Phase 1: the temporal inputs (depth, motion vectors, jitter, camera) are captured without frame generation
- [x] Phase 2: FSR 3.1 temporal upscaler, off by default (`SOTC_UPSCALER=fsr3`, with `SOTC_RENDER_RES` below 1080p and
      `SOTC_OUTPUT_RES`; 23/10: Shrine 720p to 4K, clean image, ~48 FPS like FSR 1; HUD still goes through it)
- [x] Phase 3: launcher option (upscaler FSR 3.1 + quality preset) and the in-game F1 menu switch
- [ ] Phase 4: DLSS Super Resolution through Streamline, NVIDIA only (blocked: needs sl.dlss.dll and nvngx_dlss.dll, not in the repository; untested code is not shipped)
- [x] Phase 5: documentation (CHANGELOG, this roadmap)

**Part 2: up to 120 FPS in game, without frame generation**
- Where we are: the Shrine of Worship runs at ~42 FPS on 0.60.6 with the safe defaults (~54 FPS with
  the two readback options that caused rainbow noise, `0x49500` crashes and black sand dots turned
  back on, so they stay off). The render thread needs ~20 ms per frame; 120 FPS means 8.3 ms.
- [ ] T0: measure the current ceiling in the attract demo and the Shrine, per thread
- [ ] T1: find out how the game simulates above 60 FPS (its clock is tied to the 60 FPS target today)
- [ ] T2: reduce CPU work per draw: skip repeated register state, make the fast descriptor reuse safe,
      move pure precomputation off the render thread; splitting the render thread is the last resort
- [ ] T3: every change must pass the rainbow-noise and sand-dot checks before it becomes default
- [ ] T4: 120 Hz presentation options in the launcher and the in-game menu
- Milestones: M1 attract demo ≥ 100 FPS · M2 open world ≥ 90 · M3 Shrine ≥ 60 stable with safe
  defaults · M4 Shrine 90 · M5 all scenes up to 120. Each one is marked only with a measured number.

**Open items from the 0.60.6 release**
- [ ] GCN cards: some runs end with a lost GPU device after the first colossus (issues #187, #123, #41).
      The 32-lane change is not the cause; the event log from 0.60.6 is needed to continue.
- [ ] FSR 1 on RDNA4 (#208, #209): fix in 0.60.5-plus3 not yet confirmed on hardware.
- [ ] Upstream pull requests #198, #205 and #207: content already in 0.60.6; open decision on closing them.

---

## Roteiro (português)

Plano público deste fork, atualizado conforme o trabalho avança (última atualização: 23/10/2026).
Os números de issue se referem ao repositório original.

**Parte 1: upscalers (mais FPS em GPUs mais fracas, saída 1440p/4K mais nítida)**
- [x] FSR 1 com RCAS (já presente desde a 0.60.x)
- [x] Fase 0: renderizar abaixo de 1080p funciona com os ajustes do próprio jogo (23/10: Santuário, saída 3840x2160 com FSR 1:
      1080p 38,7 FPS, 720p 46,9, 576p 49,7; imagem limpa, sem perda de dispositivo)
- [x] Fase 1: as entradas temporais (profundidade, vetores de movimento, jitter, câmera) são capturadas sem geração de quadros
- [x] Fase 2: upscaler temporal FSR 3.1, desligado por padrão (`SOTC_UPSCALER=fsr3`, com `SOTC_RENDER_RES` abaixo de 1080p e
      `SOTC_OUTPUT_RES`; 23/10: Santuário 720p para 4K, imagem limpa, ~48 FPS como o FSR 1; a interface também passa por ele)
- [x] Fase 3: opção no launcher (upscaler FSR 3.1 + preset de qualidade) e a troca no menu F1 do jogo
- [ ] Fase 4: DLSS Super Resolution via Streamline, só NVIDIA (bloqueada: faltam sl.dlss.dll e nvngx_dlss.dll, que não ficam no repositório; código sem teste não é publicado)
- [x] Fase 5: documentação (CHANGELOG, este roteiro)

**Parte 2: até 120 FPS no jogo, sem geração de quadros**
- Onde estamos: o Santuário de Culto roda a ~42 FPS na 0.60.6 com os padrões seguros (~54 FPS
  com as duas opções de leitura que causavam ruído colorido, crashes `0x49500` e pontos pretos na
  areia religadas, então elas continuam desligadas). A thread de desenho gasta ~20 ms por quadro;
  120 FPS significam 8,3 ms.
- [ ] T0: medir o teto atual na demo de atração e no Santuário, por thread
- [ ] T1: descobrir como o jogo simula acima de 60 FPS (hoje o relógio do jogo está ligado à meta de 60)
- [ ] T2: reduzir o trabalho de CPU por desenho: pular estado de registradores repetido, tornar seguro o
      reaproveitamento de descritores, mover pré-cálculo puro para fora da thread de desenho; dividir a
      thread de desenho é o último recurso
- [ ] T3: cada mudança precisa passar pelos testes de ruído colorido e de pontos pretos antes de virar padrão
- [ ] T4: opções de apresentação a 120 Hz no launcher e no menu do jogo
- Marcos: M1 demo de atração ≥ 100 FPS · M2 mundo aberto ≥ 90 · M3 Santuário ≥ 60 estável com os padrões
  seguros · M4 Santuário 90 · M5 todas as cenas até 120. Cada marco só é marcado com um número medido.

**Pendências da versão 0.60.6**
- [ ] Placas GCN: algumas sessões terminam com a perda do dispositivo gráfico após o primeiro colosso
      (issues #187, #123, #41). A conversão para 32 lanes não é a causa; é preciso o log de eventos da 0.60.6.
- [ ] FSR 1 em RDNA4 (#208, #209): correção da 0.60.5-plus3 ainda não confirmada em hardware.
- [ ] Pull requests do original #198, #205 e #207: o conteúdo já está na 0.60.6; decisão pendente sobre fechá-los.

---

## Building

Windows with [MSYS2](https://www.msys2.org/) (CLANG64); packages:
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
  forks): loader, native runtime and launcher. Original README: [docs/bbport](docs/bbport/README.md).
- [shadPS4](https://github.com/shadps4-emu/shadPS4) and the shadPS4 SotC fork: Vulkan video core.
- [Pink-shadPS4](https://github.com/luizgustavs/Pink-shadPS4) (luizgustavs): culling cvars, BPE
  heap guard, periodic submission, command recording thread, shader fixes (fetch-shader
  constants, `V_ALIGNBIT`, `V_CVT_PK_U8_F32`), dynamic T# tables (`dynamic_tsharp_array_size`),
  the LOD statistics approach and direct memory access for SotC.
- Community pull requests and forks: **Oussama268**, **Lucas-blip-png**, **Gabriell-os**,
  **jownatan**, **xeodeo**, **nakaionyt / y7ngboy**, **tabnul**, **EdouardSence**, **Einar**,
  **KingJhoss**, **nosekefik**, **raulbezerro-dev** and everyone who reports issues with logs.
- 60 FPS patch: illusion (GoldHEN cheats).

License: **GPL-2.0-or-later** ([LICENSE](LICENSE)).
