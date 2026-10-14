# Linux: opções do `sotc.ini` / `sotc.ini` options

`~/.local/share/sotc-pc/sotc.ini` (AppImage) ou / or `sotc.ini` ao lado do / next to `sotc.py`.
Uma opção por linha / one option per line, `NOME=valor`; `#` comenta / comments. Variáveis de
ambiente têm prioridade sobre o arquivo / environment variables take precedence over the file
(`BB_FULLSCREEN=0 ./SotC-PC-...AppImage`). São as mesmas que o lançador do Windows ajusta / the
same ones the Windows launcher sets (`launcher/sotc_launcher_win.py`, `game_environment`).

## Jogo e arquivos / Game and files

| Opção / Option | Valor / Value |
|---|---|
| `SOTC_GAME_DIR` | pasta do dump / dump folder (`eboot.bin`, `sce_module`, `sce_sys`) |
| `SOTC_SYS_MODULES` | pasta com / folder with `libSceNgs2.sprx` e / and `libSceUlt.sprx` (sem ela, sem som / without it, no sound) |
| `SOTC_UPDATE_DIR` | atualização do jogo / game update (padrão / default `<jogo>-UPDATE` ou / or `-patch`) |
| `SOTC_DLC_DIR` | DLCs (`addcont` do shadPS4 ou uma subpasta por DLC / or one subfolder per DLC) |
| `SOTC_USER_DIR` | saves e caches / saves and caches (padrão / default `~/.local/share/sotc-pc/user`) |

## Modos e imagem / Modes and image

| Opção / Option | Valor / Value |
|---|---|
| `SOTC_MODES` | `af16` (filtro 16x / 16x AF), `fps60` (experimental); ex. `af16,fps60`. Só / only EU 1.01 |
| `SOTC_NEO` | `1`: modo PS4 Pro / PS4 Pro mode |
| `SOTC_OUTPUT_RES` | saída / output, ex. `2560x1440` (padrão / default `1920x1080`; o jogo renderiza em / renders at 1080p) |
| `BB_FSR1` | `1`: FSR 1 para ampliar / to upscale (pelo `.ini` vem desligado / off by default here) |
| `BB_RCAS`, `BB_RCAS_ATTENUATION` | nitidez / sharpness: `BB_RCAS=0` desliga / off; atenuação `0` alta / high, `250` média / medium (padrão / default), `600` baixa / low |
| `BB_FULLSCREEN` | `1`: tela cheia / fullscreen |
| `BB_PRESENT_MODE` | `Mailbox` (padrão / default), `Fifo` (VSync), `Immediate` |
| `SOTC_FPS` | `1`: FPS no registro a cada 5 s / FPS in the log every 5 s |

## Efeitos do motor / Engine effects (`SOTC_CVARS`)

Cvars separadas por espaço / space-separated. Definir `SOTC_CVARS` substitui o padrão; mantenha
as duas primeiras (mesma imagem, menos esperas da GPU) / Setting it replaces the default; keep the
first two (same image, fewer GPU waits):

```
SOTC_CVARS=doIndexBufferCulling=0 doSunShadowIndexBufferCulling=0 fog_enable=0
```

- `r_vignetteOverrideEnable=1 r_vignetteEnable=0`: sem vinheta / no vignette
- `fog_enable=0`: sem névoa / no fog
- `AO_Mode=0`: sem SSAO / no SSAO

## Desempenho / Performance

| Opção / Option | Valor / Value |
|---|---|
| `BB_DIRECT_MEMORY_ACCESS` | `0`: ~10% mais FPS, água distante mais lisa / ~10% more FPS, flatter distant water |
| `SOTC_FLUSH_US=500` + `SOTC_RB_AHEAD=1` | envio periódico à GPU (automático só na NVIDIA); em AMD/Intel teste, e se a tela ficar preta remova / periodic GPU submission (automatic on NVIDIA only); try on AMD/Intel, remove on a black screen |
| `SOTC_RECORD_THREAD` | `0`/`1`: gravação Vulkan em thread própria / Vulkan recording on its own thread |
| `BB_READBACKS` | não use `1` (relaxado, pode travar) / don't use `1` (relaxed, may hang) |
| `BB_TIMEOUT` | segundos do watchdog de diagnóstico (padrão desligado) / diagnostic watchdog seconds (off by default) |

## Idioma e controles / Language and controls

| Opção / Option | Valor / Value |
|---|---|
| `BB_LANGUAGE` | `15` pt-BR, `7` pt-PT, `1` inglês / English (padrão / default), `3` es, `2` fr, `4` de, `5` it, `0` ja |
| `BB_USER_NAME` | nome do jogador / player name |
| `BB_GAMEPAD` | parte do nome do controle / part of the controller's name |
| `SOTC_MOUSE`, `SOTC_MOUSE_SENS`, `SOTC_MOUSE_INVERT` | câmera no mouse / mouse camera: `0` desliga / off; sensibilidade / sensitivity (padrão / default `1.0`); `1` inverte / inverts Y |
| `key.<ação>=Teclas` | teclas do SDL, vírgula separa / SDL key names, comma-separated (`key.cross=Space,Return`) |
| `pad.<ação>=botões` | botões do SDL / SDL buttons (`a b x y back start leftshoulder rightshoulder lefttrigger righttrigger dpup ...`) |

Ações / actions: `cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad touchpad_right
up down left right`; no teclado também / on the keyboard also `move_up/down/left/right` e / and
`look_up/down/left/right`. Menu sobre o jogo / in-game overlay: **Insert** ou / or **L3+R3**.
