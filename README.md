# Shadow of the Colossus — PC native port · v0.70-plus4

Port nativo para Windows do **Shadow of the Colossus** de PS4 (Bluepoint, 2018; CUSA08809 EU 1.01 ou CUSA08034 US 1.00):
o código x86-64 do próprio jogo roda direto na CPU do PC e a GPU do PS4 é traduzida para Vulkan. Este é o fork
Lucas-blip-png do [port do AjncSystem](https://github.com/AjncSystem/ShadowOfTheColossus-PC), com tudo dele e o trabalho deste
fork por cima. **O jogo pode ser terminado do começo ao fim.**

A native Windows port of the PS4 **Shadow of the Colossus**: the game's own x86-64 code runs on the PC's CPU and the PS4 GPU is
translated to Vulkan. This is Lucas-blip-png's fork of [AjncSystem's port](https://github.com/AjncSystem/ShadowOfTheColossus-PC).
**The game can be finished from start to end.**

![Shrine of Worship, uncapped, Ryzen 7 5700X + RTX 5060 Ti, 0.60.6-plus14 (the port's FPS panel; it reads about 3 FPS lower while the screen is being recorded)](docs/img/sotc-fork.gif)

**[⬇ Baixar a última versão / Download the latest release](../../releases/latest)** · traga o seu próprio dump do jogo / bring your own game dump

> **Nenhum arquivo do jogo ou da Sony está incluído. / No game or Sony files are included.** É preciso o seu próprio dump e,
> para o som, dois módulos do seu console. / You need your own game dump and, for sound, two modules from your own console.
> Este projeto não tem ligação com a Sony Interactive Entertainment nem com a Bluepoint Games. / This project is not
> affiliated with Sony Interactive Entertainment or Bluepoint Games.

[Requisitos](#requisitos) · [Roteiro](#roteiro) · [Novidades](#novidades) · [Requirements](#requirements) · [Roadmap](#roadmap) · [What's new](#whats-new) · [Changelog](CHANGELOG.md)

---

## Requisitos

**Para rodar:** Windows 10/11 64 bits · placa de vídeo com **Vulkan 1.3** e driver recente (NVIDIA GTX 10xx ou mais nova;
AMD RX 5000 ou mais nova; AMD RX 400/500/Vega e Intel Arc em teste) · CPU x86-64 com **AVX** (BMI1 não é exigido) e pelo
menos **4 threads** (8 ou mais recomendado) · **16 GB de RAM** recomendados · jogo num **SSD** (cada área nova lê centenas
de MB do disco; num HD as trocas de área travam mais).

**Por preset e resolução.** Medido por este fork em 30/10/2026 no Santuário de Culto, a cena mais pesada (~5.600 desenhos
por quadro), num **Ryzen 7 5700X + RTX 5060 Ti 8 GB**, sem limite de FPS (os presets limitam a 30 ou 60, o Máximo não),
com DLSS (FSR 3.1 dá números parecidos). Nesse PC a CPU limita o FPS em uns 70-80.

| Preset | Saída | Render | VRAM recomendada | FPS médio / mínimo | VRAM usada pelo jogo |
|---|---|---|---|---|---|
| Compatibilidade (FSR 1, texturas desligadas, 30 FPS) | 1920×1080 | 1920×1080 | 4 GB | 70,7 / 68,9 | 2,8 GB |
| Equilibrado (DLSS/FSR 3.1 Qualidade, texturas Alta, 60 FPS) | 1920×1080 | 1280×720 | 6 GB | 81,2 / 78,9 | 3,0 GB |
| Equilibrado | 2560×1440 | 1704×960 | 6 GB | 75,8 / 73,0 | 3,5 GB |
| Equilibrado | 3840×2160 | 2560×1440 | 8 GB | 57,0 / 55,8 | 4,4 GB |
| Máximo (texturas Máxima, grama longa, 3 cascatas, FPS livre) | 1920×1080 | 1920×1080 (DLAA) | 8 GB | 65,4 / 63,0 | 3,7 GB |
| Máximo | 2560×1440 | 2560×1440 (DLAA) | 8 GB | 59,4 / 56,8 | 4,2 GB |
| Máximo | 3840×2160 | 2560×1440 (Qualidade) | 12 GB | 53,6 / 52,1 | 4,5 GB |

- "VRAM usada pelo jogo" é o contador do port (texturas, buffers, imagens) no Santuário. O driver, a tela e o upscaler
  ocupam mais 1-2 GB, e as vistas amplas do mundo aberto usam mais que o Santuário: por isso a coluna "recomendada" tem folga.
- A geração de quadros (F8) soma memória: com DLSS-G a 4K uma placa de 8 GB estourou numa sessão longa.
- No lançador, **Imagem e FPS → Sugerir para meu PC** escolhe saída, upscaler, render e texturas pela placa, VRAM, CPU e
  tela, e explica por quê. Outros PCs: abra uma issue com os seus números (painel F1) e o zip de relatório, e eles entram aqui.

## Roteiro

Plano público deste fork. Detalhe de cada item: [docs/sotc/ROADMAP.md](docs/sotc/ROADMAP.md) (atualizado em 30/10/2026).

**Em andamento**
- [ ] **Trocas de área:** o pico que sobra é o envio de 200-250 MB de buffers num quadro só (até 123 ms). O 0.70-plus1 trouxe
      do upstream o limite de textura por quadro; a trava de memória não era o gargalo aqui (espera de até 2 ms).
- [ ] DLSS com o **Low Latency Mode: Ultra** do driver (RTX 2080 Ti, 2mg): o plus27 deixa de carregar Reflex e DLSS-G sem
      geração de quadros, a causa provável; falta confirmar com o Ultra ligado.
- [ ] **AMD GCN** (RX 470-590, Vega): o AMD-Fix (raulbezerro-dev) parou as quedas numa RX 570 (brxzzi, #9). Falta achar qual
      dos ajustes dele resolve e ligar só esse nas GCN, sem a camada.

**Próximos**
- [ ] Água e barra de stamina tremendo (RenderDoc) · Intel Arc · FSR 4.1 (quando o SDK da AMD tiver Vulkan)
- [ ] Ruído colorido no chão do deserto depois de uma carga (sem causa ainda)
- [ ] Saídas silenciosas com código `0xffffffff` (#244, #291, #299, #61): só diagnóstico até agora

**Medido e descartado**
- [x] Limite de CPU do Santuário (70-80 FPS no 5700X): é uma cadeia de latência entre as threads, não uma thread só;
      somar 2 ms na thread de desenho custa 0,5 ms no quadro. Agrupar `VirtualProtect` e otimizar só a `GpuDraw`: ganho ~0.
- [x] 2ª fila Vulkan para readback, ASC em fila própria, divisão da `GpuDraw`, 4K nativo (texturas finas somem, 42-44 FPS).

## Novidades

**0.70-plus4** (31/10/2026): DLSS direto pelo driver, sem o Streamline (o "Modo de baixa latência" Ultra não pesa mais);
máscara reativa no FSR 3.1; quadros no ritmo do jogo (tabnul, F6 compara); o F10 mostra qual DLSS amplia; menus com FSR 1;
opção "Compatibilidade AMD GCN" no lançador.

**0.70-plus3** (31/10/2026): FSR 3.1 sem o visual pixelado em movimento (a cor ia marcada como HDR); envios de memória para a
GPU mais rápidos (HiddenUserHere); aviso do Low Latency Mode Ultra com DLSS no lançador.

**0.70-plus2** (31/10/2026): formato de textura inválido não fecha mais o jogo (#10 do fork); `SOTC_GCN_NV=1` para testar nas
AMD GCN as opções de shader que o AMD-Fix liga (#8, #9).

**0.70-plus1** (30/10/2026): do 0.70 do upstream (AjncSystem), no máximo 32 MB de texturas novas por quadro nas trocas de área
(pior quadro 133 ms contra 174 no plus27, no teste do usuário). O resto do 0.70 já existia aqui ou não ajudou; detalhes no
[CHANGELOG](CHANGELOG.md).

**0.60.6-plus27** (30/10/2026, detalhes no [CHANGELOG](CHANGELOG.md)):
- **DLSS:** a espera do Reflex só roda com a geração de quadros DLSS-G (+6% no Santuário com o DLSS só ampliando); sem
  geração de quadros o jogo carrega só o plugin do DLSS (causa provável da queda com o Low Latency Mode: Ultra, #5 do fork, 2mg).
- **A nitidez (RCAS) agora vale no FSR 3.1, no DLSS e no FSR 1 sem ampliação** (pedido do 2mg).
- **O F8 não desliga mais o DLSS/FSR 3.1** depois de passar pelo FSR 3; 4K com DLSS Desempenho sem chão preto; menos VRAM
  presa com a placa quase cheia; DLSS sem o contexto do FSR 3.1 junto (até ~200 MB a menos em 4K).
- **Lançador:** "Sugerir para meu PC", preset **Máximo**, saves em Documentos\My Games, pular a abertura, opções de geração
  de quadros que a placa não usa em cinza.
- **Menu F1:** opções que só valem ao abrir o jogo aparecem em cinza com o motivo; a qualidade das texturas muda só no lançador.

Versões anteriores: tabela em [What's new](#whats-new) e o [CHANGELOG](CHANGELOG.md) completo.

---

## Requirements

**To run:** Windows 10/11 64-bit · a **Vulkan 1.3** graphics card with a recent driver (NVIDIA GTX 10xx or newer; AMD RX
5000 or newer; AMD RX 400/500/Vega and Intel Arc being tested) · an x86-64 CPU with **AVX** (BMI1 not needed) and at least
**4 threads** (8 or more recommended) · **16 GB of RAM** recommended · the game on an **SSD** (each new area reads hundreds
of MB; on a hard disk area changes stall more).

**By preset and resolution.** Measured by this fork on 30 Oct 2026 in the Shrine of Worship, the heaviest scene (~5,600
draws per frame), on a **Ryzen 7 5700X + RTX 5060 Ti 8 GB**, uncapped (the presets cap at 30 or 60, Maximum does not),
with DLSS (FSR 3.1 gives similar numbers). The CPU limits this PC at about 70-80 FPS.

| Preset | Output | Render | Recommended VRAM | FPS avg / min | VRAM used by the game |
|---|---|---|---|---|---|
| Compatibility (FSR 1, textures off, 30 FPS) | 1920×1080 | 1920×1080 | 4 GB | 70.7 / 68.9 | 2.8 GB |
| Balanced (DLSS/FSR 3.1 Quality, High textures, 60 FPS) | 1920×1080 | 1280×720 | 6 GB | 81.2 / 78.9 | 3.0 GB |
| Balanced | 2560×1440 | 1704×960 | 6 GB | 75.8 / 73.0 | 3.5 GB |
| Balanced | 3840×2160 | 2560×1440 | 8 GB | 57.0 / 55.8 | 4.4 GB |
| Maximum (Maximum textures, long grass, 3 cascades, uncapped) | 1920×1080 | 1920×1080 (DLAA) | 8 GB | 65.4 / 63.0 | 3.7 GB |
| Maximum | 2560×1440 | 2560×1440 (DLAA) | 8 GB | 59.4 / 56.8 | 4.2 GB |
| Maximum | 3840×2160 | 2560×1440 (Quality) | 12 GB | 53.6 / 52.1 | 4.5 GB |

- "VRAM used by the game" is the port's counter (textures, buffers, images) in the Shrine. The driver, the screen and the
  upscaler take 1-2 GB more, and wide open-world views use more than the Shrine, hence the margin in "recommended".
- Frame generation (F8) adds memory: with DLSS-G at 4K an 8 GB card ran out in a long session.
- In the launcher, **Display & FPS → Suggest for my PC** picks the output, upscaler, render size and textures from your
  card, video memory, CPU and screen, and says why. Other PCs: open an issue with your numbers (F1 panel) and the report zip.

## Roadmap

Detail of each item (Portuguese): [docs/sotc/ROADMAP.md](docs/sotc/ROADMAP.md) (updated 30 Oct 2026).

**In progress**
- [ ] **Area changes:** the spike left is uploading 200-250 MB of buffers in one frame (up to 123 ms). 0.70-plus1 brought the
      per-frame texture budget from upstream; the memory lock was not the bottleneck here (waits up to 2 ms).
- [ ] DLSS with the driver's **Low Latency Mode: Ultra** (RTX 2080 Ti, 2mg): plus27 no longer loads Reflex and DLSS-G
      without frame generation, the likely cause; not confirmed with Ultra yet.
- [ ] **AMD GCN** (RX 470-590, Vega): the AMD-Fix layer (raulbezerro-dev) stopped the crashes on an RX 570 (brxzzi, #9).
      Next: find which of its changes does it and turn only that one on for GCN, without the layer.

**Next**
- [ ] Water and shaking stamina bar (RenderDoc) · Intel Arc · FSR 4.1 (once AMD's SDK has Vulkan)
- [ ] Coloured noise on the desert ground after a load (no cause yet)
- [ ] Silent exits with code `0xffffffff` (#244, #291, #299, #61): diagnostics only so far

**Measured and dropped**
- [x] The Shrine's CPU limit (70-80 FPS on the 5700X) is a latency chain across threads, not one thread; adding 2 ms to
      the draw thread costs 0.5 ms per frame. Batching `VirtualProtect` and tuning `GpuDraw` alone: ~0 gain.
- [x] Second Vulkan queue for readbacks, ASC on its own queue, splitting `GpuDraw`, native 4K (fine textures vanish, 42-44 FPS).

## What's new

**0.70-plus4:** DLSS straight through the driver, without Streamline (NVIDIA Low Latency Mode Ultra no longer costs FPS);
FSR 3.1 reactive mask; frames at the game's own pace (tabnul, F6 compares); F10 shows which DLSS upscales; menus with
FSR 1; launcher "AMD GCN compatibility" option.

**0.70-plus3:** FSR 3.1 no longer looks pixelated in motion (its colour input was flagged as HDR); faster uploads to the GPU
(HiddenUserHere); launcher warning about Low Latency Mode Ultra with DLSS.

**0.70-plus2:** an invalid texture format no longer closes the game (fork #10); `SOTC_GCN_NV=1` to test on AMD GCN the shader
options the AMD-Fix layer turns on (#8, #9).

**0.70-plus1:** from upstream 0.70 (AjncSystem), at most 32 MB of new textures per frame on area changes (worst frame 133 ms
against 174 ms on plus27 in the user's test). The rest of 0.70 was already here or did not help; details in the CHANGELOG.

**0.60.6-plus27:** DLSS no longer slowed by Reflex when only upscaling, and loads the DLSS plugin alone without frame
generation (likely cause of the drop with Low Latency Mode: Ultra, fork #5, 2mg); sharpening (RCAS) now works with FSR 3.1,
DLSS and same-size FSR 1; F8 no longer turns DLSS/FSR 3.1 off after FSR 3; 4K DLSS Performance without black ground; less
stuck VRAM near the limit; launcher "Suggest for my PC", Maximum preset, saves in Documents\My Games, skip the opening,
greyed frame generation choices the card cannot use; F1 greys launcher-only choices and texture quality changes in the
launcher only.

| Version | Improvements and fixes |
|---|---|
| **0.70-plus4** | **Lucas-blip-png fork:** DLSS through NGX directly, without Streamline (Low Latency Mode Ultra, 2mg #5); FSR 3.1 reactive mask; flip pacing at the game's rate (tabnul, upstream PR #351, F6); DLSS label on F10; FSR 1 for menus; launcher AMD GCN option (brxzzi, raulbezerro-dev). |
| 0.70-plus3 | **Lucas-blip-png fork:** FSR 3.1 colour no longer flagged as HDR (pixelated in motion, 2mg #5); guest-view upload copies and GPU fill of HLE tables (HiddenUserHere, upstream PR #339); Low Latency Mode Ultra warning. |
| 0.70-plus2 | **Lucas-blip-png fork:** invalid texture format no longer closes the game (pepinou123, #10); `SOTC_GCN_NV=1` for AMD GCN (brxzzi, raulbezerro-dev, #8/#9). |
| 0.70-plus1 | **Lucas-blip-png fork:** per-frame texture upload budget from upstream 0.70 (AjncSystem); free-tree dedup ported but off (long stalls in the user's A/B). |
| 0.60.6-plus27 | **Lucas-blip-png fork:** DLSS without the Reflex wait when only upscaling (+6% in the Shrine) and with the DLSS plugin alone without frame generation (2mg, fork #5); sharpening with FSR 3.1/DLSS; F8 keeps DLSS/FSR 3.1; Suggest for my PC and Maximum preset; saves in Documents; F1 greys launcher-only choices. |
| 0.60.6-plus26 | **Lucas-blip-png fork:** 4-thread CPUs +19% in the Shrine (job worker naps, from MMadmer); HUD text without trails with FSR 3.1/DLSS; HUD bars right with reduced render; 30 FPS cap in PS4 Pro mode (AjncSystem); DMA off on Intel (Oussama268); HDR fixes (tabnul); vibration strength (MMadmer, xeodeo). |
| 0.60.6-plus25 | **Lucas-blip-png fork:** 21:9 and 32:9: the 3D scene is no longer the 16:9 image stretched sideways (#4, with #327 and #331 upstream); game library shows version 01.01. |
| 0.60.6-plus24 | **Lucas-blip-png fork:** shader cache journal (one file per session instead of ~8,300), background cache writer and speculative compute pipelines (Lukto02, PR #3). |
| 0.60.6-plus23 | **Lucas-blip-png fork:** F8 pressed in a burst no longer crashes the NVIDIA driver (one switch per 1.5 s); `BB_WRITE_LOG=3` crash diagnostic (last GPU writes per memory page). |
| 0.60.6-plus22 | **Lucas-blip-png fork:** host memory leak of the command processor fixed (~44 MB/min, PR by Lukto02), texture cache fixes from 0.60.6-C1, per-frame time log, English update notes in the launcher. |
| 0.60.6-plus21 | **Lucas-blip-png fork:** fixed mappings over live memory now tell the GPU side; HostCached buffer last-resort fix; host page-protect counters in the FPS report. |
| 0.60.6-plus20 | **Lucas-blip-png fork:** shorter cold shader cache: new compute shaders are translated ahead, in parallel, on the pipeline workers (worst frame of the desert route with an empty cache 493-618 ms to 174 ms). |
| 0.60.6-plus19 | **Lucas-blip-png fork:** adjustable grass (tabnul, AjncSystem, Oussama Afassi), launcher presets, long-path warning, exit diagnostics, flexible-memory split fix. |
| 0.60.6-plus18 | **Lucas-blip-png fork:** adjustable field of view in the launcher (100-140%, `SOTC_FOV_SCALE`, #179); in-game vibration toggle (AjncSystem). |
| 0.60.6-plus17 | **Lucas-blip-png fork:** hotfix: the launcher closed on start in plus15 and plus16 (`KeyError: 'fx_sunshadows'`, #316, reported by geosaw616). |
| **0.60.6-plus16** | **Lucas-blip-png fork:** health and stamina bars in 21:9 and wider (#301, #202, #145; AjncSystem's 0.60.6-C2 work, original authorship kept); shader cache read from one pack file (`cache.sotcpack`, AjncSystem), faster boots on hard disks. |
| **0.60.6-plus15** | **Lucas-blip-png fork:** launcher option to turn the sun shadows off (+34% FPS in 4isaque4's measurement, #294); RX 9070 XT out-of-device-memory buffers are retried without dedicated memory and then in system memory (#293, reported by Coriolys); stutter diagnostics and notes (`docs/sotc/STUTTER.md`). |
| **0.60.6-plus14** | **Lucas-blip-png fork:** the command-processor and dispatch fast paths of Lukto02's PR #308, off by default there, are on (Shrine 71.7 -> 75.5 FPS in A/B, 0 mismatches in >50 M verify checks) with a retrained PGO profile. `SOTC_CP_FAST=0` / `SOTC_DISPFAST=0` turn them off. |
| **0.60.6-plus13** | **Lucas-blip-png fork:** performance from Lukto02's PR #308 (draw-thread fast paths, DrawReplay, readback intervals; Shrine +8.6%, desert route about +24% in A/B) with a retrained PGO profile (another +6-7%); three downloads: Livre (no preset), Otimizada NVIDIA and Otimizada AMD/Intel (docs/sotc/PGO.md). |
| **0.60.6-plus12** | **Lucas-blip-png fork:** Maximum texture quality allowed in PS4 Pro mode (experimental, from C1, now that the page allocator fix is in); F7 switches 30 -> 60 -> unlimited FPS. |
| **0.60.6-plus11** | **Lucas-blip-png fork:** 4-thread CPUs get thread priorities (34 to 47 FPS in the Shrine limited to 4 CPUs) and a fix for a read-fault hang; the page allocator fix of the 0x1a371 crash (tabnul, from C1); DLSS-G multiplier 3x/4x/6x on RTX 50; a detailed texture log and a bigger problem report .zip. |
| **0.60.6-plus10** | **Lucas-blip-png fork:** textures are no longer stuck at the coarsest level after every load (the page-pool valve only goes to mip 5 while the pool is falling, not within 60 s of a load). |
| **0.60.6-plus9** | **Lucas-blip-png fork:** launcher options to turn the controller vibration off and to generate frames without the interface (no shaking health/stamina bars); pad-recording fix. |
| **0.60.6-plus8** | **Lucas-blip-png fork:** automatic VRAM tier from the card's budget (new launcher default); FSR 3.1/DLSS render size up to 1440p; stronger texture-pool LOD valve against the `0x1a371` crash; GPU time and wait diagnostics. |
| **0.60.6-plus7** | **Lucas-blip-png fork:** the F1 in-game menu now says that FSR 3.1 and DLSS are chosen in the launcher (the render size is set when the game starts) and that the "FSR 3" further down is frame generation; new README GIF; `--video` of the Shrine benchmark records lighter. |
| **0.60.6-plus6** | **Lucas-blip-png fork:** the driver's pipeline cache is kept on disk (`SOTC_DRIVER_CACHE=0` turns it off; the 2,753-pipeline preload takes 0.2-0.3 s instead of 0.7-0.9 s on an RTX 5060 Ti); launcher option **Borderless fullscreen** (never changes the monitor mode); `--video` in the Shrine benchmark. |
| **0.60.6-plus5** | **Lucas-blip-png fork:** **the Shrine runs at 59–61 FPS uncapped (was 53–54)**: a GENERAL→GENERAL storage-image barrier is now issued once per fence epoch instead of before every draw, so render passes are no longer cut ~20k times a second (`SOTC_GENERAL_SYNC=0` turns it off); partial async readbacks (`SOTC_RB_PARTIAL`); depth sampled as target keeps a consistent layout (`SOTC_DEPTH_RO=0` turns it off); launcher **Report a problem** button (sanitized report: copy, .zip, or a pre-filled GitHub issue; nothing is sent by itself). |
| **0.60.6-plus4** | **Lucas-blip-png fork:** hot readbacks (`SOTC_RB_HOT`) are on by default again: about +14% in the Shrine (rainbow noise and the 0x49500 crash fixed by Oussama268's late checks; 26 min soak, 0 faults); GPU, launcher (HDR, skip logos, grass options, low-memory warning) and US-release (CUSA08034) work from Oussama268's fork. `SOTC_RB_HOT=0` turns it off. |
| **0.60.6-plus3** | **Lucas-blip-png fork:** descriptor reuse (`SOTC_FAST_BIND`) is on by default again: +8% in the Shrine (43.5 → 47.1 FPS) after an independent review fixed the races behind the black dots on sand of 0.60.5; `SOTC_FAST_BIND=0` turns it off. Open-world sand not tested yet. |
| **0.60.6-plus2** | **Lucas-blip-png fork:** **FSR 3.1 and DLSS Super Resolution (DLSS 4.5 model, NVIDIA RTX) upscalers** with the DLAA / Quality / Balanced / Performance / Ultra performance modes in the launcher, a texture LOD bias for them (sharper textures below the output size), images taken from a pool (no video memory allocation per frame: +4% in the Shrine, +6% in the attract demo), and many measured dead ends of the 120 FPS roadmap documented. DLSS needs the NVIDIA DLLs next to the game; without them FSR 3.1 is used. |
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

---

## Using the port

### In-game menu and shortcuts

Press **F1** (or **Insert**, or **L3+R3** on the controller) while playing. The mouse is free while
the menu is open; **Esc** or F1 closes it.

![In-game menu (F1)](docs/images/sotc-menu-f1-en.png)

- **Quick settings (apply at once):** FPS (30 / 60 / unlimited), upscaler (FSR 1 / bilinear, or back to the FSR 3.1 /
  DLSS the game started with), sharpening (RCAS), frame generation, far shadow reuse, volume. They are also saved as the
  launcher's choice for the next start. Choices that only apply when the game starts (FSR 3.1 / DLSS, texture quality,
  DLSS-G when the game started without frame generation) are listed greyed, with the reason.
- **Display:** fullscreen, FPS counter.
- **Camera & mouse:** mouse camera on/off, sensitivity, invert Y.
- The header shows FPS, frame time, the GPU and its video memory in use. Output resolution,
  effects (fog, vignette, SSAO), 16x AF, PS4 Pro mode and video memory use stay in the launcher
  (they apply when the game starts).

| Key | What it does |
|---|---|
| **F1** / Insert / L3+R3 | Opens or closes the in-game menu |
| **F7** | FPS: 30 → 60 → unlimited |
| **F8** | Frame generation: off → DLSS-G → FSR 3 (when the GPU supports it) |
| **F10** | Performance overlay (FPS and frame time) |
| **F11** | Window / fullscreen |

### Texture quality

In the launcher (**Quick settings → Textures**); it applies when the game starts (the in-game menu only shows it):

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
   - the game folder (the CUSA08809 or CUSA08034 dump with `eboot.bin`, `sce_module` and `sce_sys`);
   - the sound modules (`libSceNgs2.sprx` and `libSceUlt.sprx`) of an installed shadPS4
     (`user/sys_modules`), which it copies to the port's data folder;
   - a shadPS4 save of the game, which it offers to import;
   - the game's DLCs, if any (shadPS4's `addcont` folder, or a folder with one subfolder per DLC).
3. Check the **Game and saves** page. The **game library** works like shadPS4's.
4. Click **PLAY**.

Requirements: Windows 10/11 64-bit and a **Vulkan 1.3** GPU (NVIDIA, AMD or Intel with a recent
driver). No Python install needed.

Requirements and measured FPS / video memory per preset and resolution: see [Requirements](#requirements).

| What | Where from | What for |
|---|---|---|
| CUSA08809 (EU 1.01) or CUSA08034 (US 1.00) game dump | your PS4, through GoldHEN | the game |
| decrypted `libSceNgs2.sprx`, `libSceUlt.sprx` | your PS4 (`/system/common/lib/`, GoldHEN FTP), or shadPS4's `user/sys_modules` | sound (silent without them) |

The patch modes (60 FPS, 16x filtering) apply to EU 1.01 and US 1.00 (the same executable);
other versions run at 30 FPS. Each release keeps its own saves (`user/savedata/1/<title ID>`),
and only DLC of the same region is used.

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
  **KingJhoss**, **nosekefik**, **raulbezerro-dev**, **MMadmer**,
  **Lukto02**, **2mg**, **brxzzi** and everyone who reports issues with logs.
- 60 FPS patch: illusion (GoldHEN cheats).

License: **GPL-2.0-or-later** ([LICENSE](LICENSE)).
