# Shadow of the Colossus — PC (port nativo / native port) · v0.59-plus5

**The original 0.59.1, plus 60 FPS through the game's own 60 FPS mode, a smaller readback
window, 120 Hz fullscreen, textures on GTX 10xx cards and CPUs without BMI1 (pre-Haswell): about
7% more FPS at the Shrine.** Everything in
[AjncSystem's 0.59.1](https://github.com/AjncSystem/ShadowOfTheColossus-PC) (the ending crash fix,
F11, video memory and volume options, launcher self-update, AMD shadow/water stripes fix, Far
shadows option), with this fork's work on top. The draw thread and the incremental DMA sweep from
this fork's 0.58.1-plus are part of the original since 0.59, and the far shadows fix since 0.59.1.

![Shrine of Worship, 60 FPS mode, RTX 5060 Ti (FPS from RTSS)](docs/img/sotc-fork.gif)

| | Original 0.59.1 | **This fork (0.59-plus5)** |
|---|---|---|
| Shrine of Worship, unlimited FPS (RTX 5060 Ti, Ryzen 7 5700X; median of 3 runs) | 45.7 FPS | **48.8 FPS (+7%)** |

What it adds (each piece has a switch to turn it off):
- **60 FPS through the game's own 60 FPS mode** (the PS4 Pro Performance one): the 60 FPS mode
  used to ignore the 30 FPS rate the game asks while the game still timed itself for 30, so parts
  of its logic ran twice as fast (likely the bird colossus flying away, the 2nd colossus' wall,
  weak point damage). Now the game targets 60 itself and its timing follows; F7 switches its
  target between 30 and 60. Not yet confirmed at colossi 2 and 5 (`SOTC_MODES=af16` goes back).
- **CPUs without BMI1/LZCNT/MOVBE** (Intel before Haswell: 2nd/3rd generation Core, Xeon E3 v1/v2;
  AMD FX), which got "This CPU is not supported": the game's ~5,000 such instructions are replaced
  by equivalent routines at startup, only on those CPUs (Oussama268). AVX is still required. Not
  yet confirmed on a real one.
- **Heap guards no longer rebuilt every frame** (+6% at the Shrine): readbacks that land in
  in-use blocks of the game's heaps skip the allocator guard, which was rebuilt about once per
  frame (`SOTC_HEAP_FAST=0`).
- **Textures on GTX 10xx (Pascal) cards:** without `VK_KHR_fragment_shader_barycentric` the
  rocks and ground came out black, faceted or noisy; a small geometry shader now hands the
  fragment shader the three vertices' values (`SOTC_BARYCENTRIC_GS=0`). The shader cache is
  rebuilt on the first launch on those cards only. Confirmed on a GTX 1080 Ti; one terrain
  blend layer in front of the Shrine is still wrong there.
- **Smaller readback window**, 64 KiB (+3% at the Shrine): fewer GPU-written pages are downloaded and unprotected
  for nothing (`SOTC_RB_NARROW=0`).
- **Fullscreen at 120 Hz** on 144/165 Hz monitors, so every frame stays on screen as long (Anthonypc23).
- **Frame generation without the HUD (experimental, off):** with `SOTC_FG_HUDLESS=1`, FSR 3 and
  DLSS-G get the frame drawn again without the UI. With UI on screen FSR 3 still shows artifacts,
  so it stays off (PR #154 of the original).
- The launcher's self-update follows this fork's releases; the launcher no longer stops when its
  console refuses writes (issue #104).

**[⬇ Download the latest release](../../releases/latest)** · bring your own game dump · Windows 10/11, Vulkan 1.3

**[Português](#português)** · **[English](#english)** · [Releases](../../releases) · [Changelog](CHANGELOG.md)

> **Nenhum arquivo do jogo nem da Sony está incluído / No game or Sony files are included.**
> Você precisa do seu próprio dump do jogo e, para ter som, de dois módulos do seu próprio
> console. / You need your own game dump and, for sound, two modules from your own console.
> Este projeto não tem ligação com a Sony Interactive Entertainment nem com a Bluepoint Games. /
> This project is not affiliated with Sony Interactive Entertainment or Bluepoint Games.

---

## Português

Port nativo do **Shadow of the Colossus** de PS4 (Bluepoint, 2018; CUSA08809, versão EU 1.01)
para **Windows 10/11**. O código x86-64 do próprio jogo roda direto na CPU do PC e a GPU do PS4
é traduzida para Vulkan. Não é um emulador completo: é um carregador com as funções de sistema
de que o jogo precisa, no estilo do [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc).

### Novidades

| Versão | Melhorias e correções |
|---|---|
| **0.59-plus5** | **Fork Lucas-blip-png:** sem a **queda com texturas em alta no modo PS4 Pro** (0x1a371: o jogo enchia o pool de memória dele; agora pede menos detalhe quando ele fica quase cheio) e correção para **CPUs sem BMI1** que travavam na primeira tela de carregamento. |
| **0.59-plus4** | **Fork Lucas-blip-png:** a 0.59-plus3 em cima da **0.59.1** do original: **listras da AMD** nas sombras e na água corrigidas, correções para Intel, opção **Sombras distantes** no lançador e o registro mostra a placa e o driver (Oussama268 e AjncSystem). |
| **0.59-plus3** | **Fork Lucas-blip-png:** **60 FPS pelo modo 60 do próprio jogo** (o do Desempenho do PS4 Pro): o tempo do jogo acompanha os 60 FPS, o que deve corrigir o colosso pássaro que voa para o céu, a parede do 2º colosso e o dano no ponto fraco; o F7 troca o alvo do jogo entre 30 e 60. Texturas nas GTX 10xx confirmadas numa GTX 1080 Ti. |
| **0.59-plus2** | **Fork Lucas-blip-png:** a 0.59-plus mais **processadores sem BMI1/LZCNT/MOVBE** (Intel de 2ª e 3ª geração, Xeon E3 v1/v2, AMD FX; do Oussama268), que antes não abriam o jogo, **sem blocos pretos nos quadros gerados pelo FSR 3 nas AMD RX 7000** (KingJhoss), e o botão JOGAR do lançador não é mais cortado com a escala do Windows acima de 100%. |
| **0.59-plus** | **Fork Lucas-blip-png:** a 0.59 inteira, mais **~7% de FPS** no Santuário (47 → 50,6 FPS: heap guard sem remontar a cada quadro e janela de readback menor), **texturas nas GTX 10xx**, **sombras distantes sem piscar**, **tela cheia a 120 Hz** em monitores de 144/165 Hz (Anthonypc23) e geração de quadros sem o HUD como opção experimental (desligada). A atualização automática do lançador segue as releases deste fork. A thread de desenho e a varredura de DMA da 0.58.1-plus agora fazem parte do original. |
| **0.58.1-plus** | **Fork Lucas-blip-png:** a 0.58.1 inteira, **sem a queda de um jogo novo** (as texturas em alta, que a causam, vêm desligadas), **sombras distantes sem piscar**, **geração de quadros sem o HUD** (FSR 3/DLSS-G) e aviso de versão nova no lançador, mais **~19% de FPS** no Santuário (42 → 50 FPS, FPS ilimitado): thread de desenho separada do processador de comandos, heap guard sem remontar a cada readback, janela de readback menor e varredura de DMA incremental. **Tela cheia a 120 Hz** em monitores de 144/165 Hz (Anthonypc23). Cache de shaders próprio do modo PS4 Pro. |
| **0.59.1** | **Sombras distantes sem piscar** (AMD e NVIDIA; opção nova no lançador), **listras da AMD** nas sombras e na água corrigidas, correções para Intel, registro mostra placa e driver. |
| **0.59** | **Mais FPS** (fork do Oussama268: thread de desenhos, shaders traduzidos em paralelo; ponte 30 → 37–40 FPS), **correção para a queda no final do jogo e ao carregar saves** (em teste: conte nas issues), **F11 tela cheia**, opções de **uso de memória de vídeo** e **volume**, **atualização automática** no lançador, ultrawide com o modo PS4 Pro, correções para placas de 6 GB e Windows em chinês. |
| **0.58.1** | **Texturas em alta qualidade** (folhas e pedras nítidas; o jogo não carregava as versões de alta resolução desde a 0.0.13) e **ícone próprio da janela do jogo** (ou o do jogo, ou uma imagem sua). |
| **0.58** | **Versão da comunidade** (Lucas-blip-png, Gabriell-os, Oussama268, jownatan, xeodeo, nakaionyt/y7ngboy, tabnul): **menos quedas** (falta de memória de vídeo, jogo congelando, queda pós-colosso no modo PS4 Pro), **+15% de FPS** e menos memória de vídeo, correções AMD (bloco preto RDNA3/4), **widescreen 21:9/32:9**, **FPS 30/60/ilimitado**, **ícones Xbox**, geração de quadros FSR 3 / DLSS-G (experimental), lançador em espanhol, chinês, coreano e russo. |
| **0.57** | **Placas AMD:** correção das cores verdes/rosas na névoa e nos personagens (as AMD passam a usar o caminho de 32 threads da NVIDIA; diagnóstico da issue #7, ainda não testado numa AMD). **Opção nova** no lançador para desligar o acesso direto à memória: ~10% mais FPS, água distante mais lisa. |
| **0.56** | **Água do mundo aberto com ondas:** lagos e rios abaixo das cascatas não ficam mais lisos como espelho (uma instrução de shader, `S_WQM_B64`, era ignorada). **Lago de cima ondula e reflete até longe** (acesso direto à memória ligado por padrão, como no Pink-shadPS4). Árvores distantes sem "lixo colorido". |
| **0.0.13** | **Sem quedas:** corrigida a queda na abertura/menu da 0.0.12 (o jogo pedia mais texturas do que cabe no pool). Testado ~40 min pelo mapa, com dois carregamentos de save. **Reflexos do lago** em quase toda a superfície, com ondulação (uma trava de laços dos shaders cortava os raios do reflexo). |
| **0.0.12** | **Texturas nítidas de volta, sem o jogo fechar:** a corrupção de memória que derrubava o jogo depois de carregar um save foi corrigida, e o streaming de texturas voltou (pedras e paredes bem mais nítidas). **Água:** respingos, anéis e espuma ao nadar; o lago reflete o céu e o brilho do sol; reflexos de ambiente com o *cubemap* certo de cada *probe* (correção do Pink-shadPS4). |
| **0.0.11** | Corrigida a grama com textura de "lixo colorido" da 0.0.10. |

Detalhes de cada versão no [CHANGELOG](CHANGELOG.md).

### Problemas conhecidos

- Na primeira abertura depois de atualizar, os shaders são compilados de novo: o primeiro
  carregamento demora e engasga.
- Placas **AMD**: as correções da 0.57 e da 0.58 ainda não foram testadas aqui numa AMD. Se algo
  continuar errado, mande o seu `user/last_run.log` na issue.
- **Geração de quadros** (experimental): a barra de vida/stamina pode tremer, e ela só faz
  diferença com o FPS em 30.
- **60 FPS / ilimitado:** desde a 0.59-plus3 o jogo usa o próprio modo 60 (o do Desempenho do PS4
  Pro), o que deve corrigir a parede do 2º colosso e os colossos "voando"; ainda não confirmado
  nesses pontos. Se acontecer, use 30 FPS ali (F7) e conte nas issues.
- Com 8 GB de memória de vídeo, o uso fica perto do limite em áreas grandes.

### Download e uso

1. Baixe o `SotC-PC.zip` da página [Releases](../../releases) e descompacte numa pasta.
2. Abra o **`SotC.exe`**. Na primeira vez o lançador procura sozinho, nos discos do PC:
   - a pasta do jogo (o dump CUSA08809, com `eboot.bin`, `sce_module` e `sce_sys`);
   - os módulos de som (`libSceNgs2.sprx` e `libSceUlt.sprx`) de um shadPS4 que você já
     tenha, na pasta `user/sys_modules`, e copia para a pasta de dados do port;
   - um save do jogo no shadPS4, que ele oferece importar;
   - as DLCs do jogo, se houver: a pasta `addcont` do shadPS4, ou uma pasta com uma subpasta por DLC.
     Também dá para escolher em **Jogo e saves > Pasta de DLC**.
3. Confira a página **Jogo e saves**. A **biblioteca de jogos** funciona como a do shadPS4:
   adicione as pastas dos seus jogos e escolha o jogo na lista.
4. Clique em **JOGAR**.

Requisitos: Windows 10/11 64-bit, placa de vídeo com **Vulkan 1.3** (NVIDIA, AMD ou Intel
com driver recente). Não precisa instalar Python.

#### Os arquivos que você precisa ter

| O quê | De onde | Para quê |
|---|---|---|
| Dump do jogo CUSA08809 (EU 1.01 de preferência) | o seu PS4, pelo GoldHEN | o jogo |
| `libSceNgs2.sprx`, `libSceUlt.sprx` decriptados | o seu PS4 (`/system/common/lib/`, FTP do GoldHEN), ou o `user/sys_modules` do shadPS4 | som (sem eles o jogo roda mudo) |

Os modos de patch (60 FPS, filtro 16x) só valem para a versão EU 1.01; nas outras o jogo roda
normalmente, a 30 FPS.

### O lançador

- **Jogar:** confere a versão do jogo, o som, os saves e a placa de vídeo antes de abrir;
  opções rápidas de saída, upscaler, 60 FPS, filtro 16x e tela cheia.
- **Jogo e saves:** biblioteca de jogos, busca automática no PC, importação de saves do shadPS4.
- **Imagem e FPS:** saída 720p a 4K, também 16:10, 21:9 e 32:9 (o jogo renderiza no tamanho da saída, até 1080 linhas
  de altura, e o FSR 1 + RCAS amplia o resto),
  modo PS4 Pro (o jogo oferece Cinematográfico/Desempenho no próprio menu), efeitos (vinheta,
  névoa, SSAO) e opções de desempenho.
- **Controles:** controle (DualShock 4, DualSense, Xbox) automático; câmera no mouse;
  **troca de teclas** clicando na ação e apertando a tecla nova.
- **Avançado:** cvars do motor Bluepoint, diagnóstico, atalho na área de trabalho (com o ícone
  do seu jogo). O registro de cada sessão fica em `user/last_run.log`.

### O que foi feito

| Área | Resultado |
|---|---|
| Boot | O jogo inicia com o carregador do bbport: imports por NID, fibras, patches de CPU (SSE4a), imagem abaixo de 2 GiB (o jogo guarda ponteiros em 31 bits). |
| Vídeo | Núcleo de vídeo do fork shadPS4 SotC dentro do port: logos, abertura, menus, campo aberto sem polígonos pretos, sombras com iluminação correta. |
| Texturas | Streaming de texturas (o jogo carrega os mips mais finos), sem corrupção de memória. |
| Água | Reflexos em espaço de tela, reflexos de ambiente por *probe*, respingos e anéis ao nadar. |
| Saves | Gravação, carregamento e exclusão pela lista de saves na tela. |
| Som | Áudio real com os módulos Ngs2/Ult do próprio usuário. |
| Controles | Teclado e controle juntos, câmera no mouse, troca de teclas no lançador. |
| Modos | 60 FPS (patch de illusion), filtro anisotrópico 16x, modo PS4 Pro, saída até 4K. |
| Desempenho | No demo da tela de título, em modo 60 FPS: média de **37–45 → ~67 FPS**, trecho mais pesado (ponte do cânion) **~17 → 29–34 FPS**. |

### Perguntas comuns

- **DirectX 11/12?** Não. O núcleo de vídeo traduz a GPU do PS4 para Vulkan (shaders em SPIR-V).
  Toda placa com DirectX 12 moderno também tem Vulkan 1.3: basta o driver atualizado.
- **AMD e Intel?** Quase todas as otimizações valem para qualquer placa. Duas só ligam
  sozinhas na NVIDIA: o envio periódico à GPU e as leituras antecipadas. Em **Imagem e FPS >
  Envio de trabalho à GPU**, escolha **Ligado** para usá-las numa AMD ou Intel. Se a tela ficar
  preta, volte para **Automático**.
- **Por que não vem o som pronto?** Os dois `.sprx` são firmware da Sony; distribuí-los não é
  permitido. O lançador acha e copia os seus automaticamente.
- **O jogo fechou sozinho.** Abra uma *issue* com o arquivo `user/last_run.log`.

---

## English

A native Windows port of the PS4 **Shadow of the Colossus** (Bluepoint, 2018; CUSA08809, EU 1.01)
for **Windows 10/11**. The game's own x86-64 code runs directly on the PC's CPU and the PS4 GPU
is translated to Vulkan. It is not a full emulator: it is a loader with the system functions the
game needs, in the style of [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc).

### What's new

| Version | Improvements and fixes |
|---|---|
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

### Known issues

- The first start after updating rebuilds the shaders: the first load is slow and stutters.
- **AMD** GPUs: the 0.57 and 0.58 fixes are not yet tested here on an AMD card. If something is
  still wrong, attach your `user/last_run.log` to the issue.
- **Frame generation** (experimental): the health/stamina bars may shake, and it only matters at
  30 FPS.
- **60 FPS / unlimited:** since 0.59-plus3 the game uses its own 60 FPS mode (the PS4 Pro
  Performance one), which should fix the 2nd colossus' wall and the "flying" colossi; not yet
  confirmed there. If it happens, use 30 FPS there (F7) and report it.
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

## Compilar / Building

Windows com / with [MSYS2](https://www.msys2.org/) (CLANG64). Pacotes / packages:
[packaging/windows/README.md](packaging/windows/README.md). Para congelar o `SotC.exe` / to
freeze `SotC.exe`: um Python do Windows com PyInstaller e Pillow / a Windows Python with
PyInstaller and Pillow (`WINPYTHON`).

```
bash build.sh                              # out/bb-probe.exe
bash packaging/windows/package_sotc.sh     # dist/SotC-PC.zip
```

O empacotamento recusa qualquer `.sprx`/`.prx` / packaging refuses any `.sprx`/`.prx`: Sony
firmware never goes into the package. Ferramentas de teste / test tools: `tools/`. Variáveis de
diagnóstico / diagnostic variables: [docs/sotc/README-SotC.md](docs/sotc/README-SotC.md).

## Créditos / Credits

- [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) (Supermedo, deadinside28 and
  forks): carregador, runtime nativo e lançador / loader, native runtime and launcher. README
  original: [docs/bbport](docs/bbport/README.md).
- [shadPS4](https://github.com/shadps4-emu/shadPS4) e o fork / and the shadPS4 SotC fork:
  núcleo de vídeo Vulkan / Vulkan video core.
- [Pink-shadPS4](https://github.com/luizgustavs/Pink-shadPS4) (luizgustavs): cvars de culling /
  culling cvars, BPE heap guard, envio periódico / periodic submission, gravador de comandos em
  thread própria / command recording thread, correções de shader / shader fixes (fetch-shader
  constants, `V_ALIGNBIT`, `V_CVT_PK_U8_F32`), tabelas de texturas indexadas / dynamic T# tables
  (`dynamic_tsharp_array_size`), a abordagem das estatísticas de LOD / the LOD statistics
  approach e o acesso direto à memória para o SotC / and direct memory access for SotC.
- Patch de 60 FPS / 60 FPS patch: illusion (GoldHEN cheats).

Licença / License: **GPL-2.0-or-later** ([LICENSE](LICENSE)).
