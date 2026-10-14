# Shadow of the Colossus — PC (port nativo / native port) · v0.57

**[Português](#português)** · **[English](#english)** · **[Linux](#linux)** · [Releases](../../releases) · [Changelog](CHANGELOG.md)

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
| **0.57** | **Placas AMD:** correção das cores verdes/rosas na névoa e nos personagens (as AMD passam a usar o caminho de 32 threads da NVIDIA; diagnóstico da issue #7, ainda não testado numa AMD). **Opção nova** no lançador para desligar o acesso direto à memória: ~10% mais FPS, água distante mais lisa. |
| **0.56** | **Água do mundo aberto com ondas:** lagos e rios abaixo das cascatas não ficam mais lisos como espelho (uma instrução de shader, `S_WQM_B64`, era ignorada). **Lago de cima ondula e reflete até longe** (acesso direto à memória ligado por padrão, como no Pink-shadPS4). Árvores distantes sem "lixo colorido". |
| **0.0.13** | **Sem quedas:** corrigida a queda na abertura/menu da 0.0.12 (o jogo pedia mais texturas do que cabe no pool). Testado ~40 min pelo mapa, com dois carregamentos de save. **Reflexos do lago** em quase toda a superfície, com ondulação (uma trava de laços dos shaders cortava os raios do reflexo). |
| **0.0.12** | **Texturas nítidas de volta, sem o jogo fechar:** a corrupção de memória que derrubava o jogo depois de carregar um save foi corrigida, e o streaming de texturas voltou (pedras e paredes bem mais nítidas). **Água:** respingos, anéis e espuma ao nadar; o lago reflete o céu e o brilho do sol; reflexos de ambiente com o *cubemap* certo de cada *probe* (correção do Pink-shadPS4). |
| **0.0.11** | Corrigida a grama com textura de "lixo colorido" da 0.0.10. |

Detalhes de cada versão no [CHANGELOG](CHANGELOG.md).

### Problemas conhecidos

- Na primeira abertura depois de atualizar, os shaders são compilados de novo: o primeiro
  carregamento demora e engasga.
- Placas **AMD**: a 0.57 traz a correção das cores verdes/rosas ([#7](../../issues/7)), ainda sem
  teste numa AMD. Se continuar, mande o seu `user/last_run.log` na issue.
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
- **Imagem e FPS:** saída 720p a 4K (o jogo renderiza em 1080p e o FSR 1 + RCAS amplia),
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

### Linux

Port nativo para Linux, sem Wine nem Proton: o mesmo jogo e o mesmo núcleo de vídeo, com o
runtime usando sinais do Linux no lugar das exceções do Windows.

| Placa | Situação |
|---|---|
| **AMD** | Funciona (testado no Fedora): o jogo abre e renderiza a abertura. |
| **NVIDIA** | **Precisa de teste.** |
| **Intel** | **Precisa de teste.** |

Se você tem NVIDIA ou Intel, abra uma *issue* contando o resultado, com o modelo da placa, a
versão do driver e o arquivo `~/.local/share/sotc-pc/last_run.log`.

**Uso (AppImage):**

```
chmod +x SotC-PC-*-linux-x86_64.AppImage
./SotC-PC-*-linux-x86_64.AppImage            # na primeira vez pede a pasta do jogo
./SotC-PC-*-linux-x86_64.AppImage --setup    # escolhe de novo a pasta do jogo e a do som
```

- Requisitos: Linux x86-64 com glibc 2.38 ou mais nova (Fedora 39+, Ubuntu 24.04+), `python3` e
  um driver Vulkan 1.3 (Mesa ou NVIDIA). Funciona em Wayland e X11.
- Os módulos de som são achados sozinhos na pasta do shadPS4 (`~/.local/share/shadPS4/sys_modules`).
- Configurações, saves e o registro ficam em `~/.local/share/sotc-pc/`. As opções do `sotc.ini`
  (resolução, FSR, tela cheia, idioma, 60 FPS, controles) estão em
  [docs/sotc/LINUX.md](docs/sotc/LINUX.md).
- Como compilar e gerar a AppImage: [Compilar / Building](#compilar--building).

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
| **0.57** | **AMD cards:** fix for the green/pink fog and characters (AMD now uses the NVIDIA 32-lane path; diagnosis from issue #7, not yet tested on an AMD card). **New launcher option** to turn off direct memory access: ~10% more FPS, flatter distant water. |
| **0.56** | **Open-world water with waves:** lakes and rivers below the waterfalls are no longer flat as a mirror (a shader instruction, `S_WQM_B64`, was ignored). **The upper lake ripples and reflects far away** (direct memory access on by default, as in Pink-shadPS4). Distant trees without "colored garbage". |
| **0.0.13** | **No more crashes:** fixed the 0.0.12 crash in the intro/menu (the game asked for more textures than its pool holds). Tested ~40 min across the map with two save loads. **Lake reflections** over almost the whole surface, with ripples (a shader loop guard cut the reflection rays short). |
| **0.0.12** | **Sharp textures back, without crashes:** fixed the memory corruption that closed the game after loading a save, and texture streaming is back (much sharper rocks and walls). **Water:** splashes, rings and foam while swimming; the lake reflects the sky and the sun's glint; environment reflections use each probe's correct cubemap (fix from Pink-shadPS4). |
| **0.0.11** | Fixed the "colored garbage" grass texture of 0.0.10. |

Full details in the [CHANGELOG](CHANGELOG.md) (in Portuguese).

### Known issues

- The first start after updating rebuilds the shaders: the first load is slow and stutters.
- **AMD** GPUs: 0.57 has the fix for the green/pink tint ([#7](../../issues/7)), not yet tested
  on an AMD card. If it persists, attach your `user/last_run.log` to the issue.
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

### Linux

A native Linux port, no Wine or Proton: the same game and video core, with the runtime using
Linux signals in place of Windows exceptions.

| GPU | Status |
|---|---|
| **AMD** | Works (tested on Fedora): the game starts and renders the intro. |
| **NVIDIA** | **Needs testing.** |
| **Intel** | **Needs testing.** |

On NVIDIA or Intel, please open an issue with the result, your GPU model, driver version and
`~/.local/share/sotc-pc/last_run.log`.

**Usage (AppImage):**

```
chmod +x SotC-PC-*-linux-x86_64.AppImage
./SotC-PC-*-linux-x86_64.AppImage            # asks for the game folder the first time
./SotC-PC-*-linux-x86_64.AppImage --setup    # picks the game and sound folders again
```

- Requirements: x86-64 Linux with glibc 2.38 or newer (Fedora 39+, Ubuntu 24.04+), `python3`
  and a Vulkan 1.3 driver (Mesa or NVIDIA). Works on Wayland and X11.
- The sound modules are found automatically in shadPS4's folder (`~/.local/share/shadPS4/sys_modules`).
- Settings, saves and the log live in `~/.local/share/sotc-pc/`. The `sotc.ini` options
  (resolution, FSR, fullscreen, language, 60 FPS, controls) are listed in
  [docs/sotc/LINUX.md](docs/sotc/LINUX.md).
- Building and making the AppImage: [Compilar / Building](#compilar--building).

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

### Windows

Windows com / with [MSYS2](https://www.msys2.org/) (CLANG64). Pacotes / packages:
[packaging/windows/README.md](packaging/windows/README.md). Para congelar o `SotC.exe` / to
freeze `SotC.exe`: um Python do Windows com PyInstaller e Pillow / a Windows Python with
PyInstaller and Pillow (`WINPYTHON`).

```
bash build.sh                              # out/bb-probe.exe
bash packaging/windows/package_sotc.sh     # dist/SotC-PC.zip
```

### Linux

Precisa de / needs GCC 14+ (C++23), CMake, Ninja, Python 3, glslang, SDL3 e / and Vulkan
headers 1.4+. O FFmpeg das distribuições não serve / distribution FFmpeg won't do: o do Fedora
vem sem H.264/HEVC (os vídeos do jogo / the game's videos), e o do Ubuntu arrasta ~100
bibliotecas para a AppImage / drags ~100 libraries into the AppImage. O
`packaging/linux/build_ffmpeg.sh` compila um FFmpeg mínimo / builds a minimal one into
`out/ffmpeg`, que o / which `build.sh` usa sozinho / picks up by itself.

**Fedora (43+):**

```
sudo dnf install gcc gcc-c++ make cmake ninja-build pkgconf-pkg-config python3 git nasm patchelf \
    vulkan-headers vulkan-loader-devel glslang VulkanMemoryAllocator-devel SDL3-devel \
    fmt-devel boost-devel robin-map-devel xxhash-devel zydis-devel libX11-devel
git submodule update --init --recursive
bash packaging/linux/build_ffmpeg.sh        # out/ffmpeg (uma vez / once)
./build.sh                                  # out/bb-probe, out/gpu/libbbgpu.so
./sotc.sh                                   # roda / runs (sotc.ini: SOTC_GAME_DIR, SOTC_SYS_MODULES)
bash packaging/linux/build_appimage.sh      # dist/SotC-PC-<versão>-linux-x86_64.AppImage
```

**Ubuntu 24.04:** o GCC 13, o SDL e os headers do Vulkan do sistema são antigos / the system
GCC 13, SDL and Vulkan headers are too old:

```
sudo apt install build-essential gcc-14 g++-14 cmake ninja-build pkg-config python3 git nasm \
    patchelf libvulkan-dev glslang-tools libvulkan-memory-allocator-dev libfmt-dev libboost-dev \
    libxxhash-dev libzydis-dev libx11-dev libxext-dev libwayland-dev libxkbcommon-dev \
    libpulse-dev libasound2-dev
# SDL3, Vulkan-Headers e / and robin-map da fonte / from source (cmake + install):
#   https://github.com/libsdl-org/SDL (release-3.2.x), https://github.com/KhronosGroup/Vulkan-Headers,
#   https://github.com/Tessil/robin-map
bash packaging/linux/build_ffmpeg.sh
CC=gcc-14 CXX=g++-14 CPATH=/usr/local/include ./build.sh
bash packaging/linux/build_appimage.sh
```

**NixOS:** `./build.sh` entra sozinho no / enters `shell.nix` by itself (FFmpeg do Nix / Nix's FFmpeg).

Testes / tests: `./build.sh --test`. A AppImage leva o loader, a biblioteca de GPU, os scripts e
as bibliotecas que eles usam, menos o sistema base (glibc, libstdc++, loader do Vulkan) / The
AppImage carries the loader, GPU library, scripts and their libraries except the base system
(glibc, libstdc++, Vulkan loader). Precisa de / needs `patchelf`; o `appimagetool` é baixado em /
is downloaded into `out/tools` (ou / or `APPIMAGETOOL=<caminho / path>`).

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
