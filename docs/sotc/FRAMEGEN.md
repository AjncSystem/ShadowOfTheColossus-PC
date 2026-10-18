# Geração de quadros (FG) — notas de trabalho

Branch `perf/framegen` (worktree `C:/Users/Lucass/sotc-fg`), a partir do cp-mt `062b49f` (0.65).
Plano: `C:/Users/Lucass/SotC-framegen-plano.md` (a revisão crítica no fim do arquivo manda sobre o plano).

Alvo principal: **30 → 60** (modo padrão, sem fps60). fps60 + FG é secundário, só faz sentido com VRR.

Com `SOTC_FG` desligado (padrão), o build se comporta exatamente como o cp-mt: o device é criado com as
mesmas extensões e recursos, e nenhum código do FSR é chamado.

## Feito

### Fase 0.B + 1 (build e recursos de dispositivo)

- **fsr-vulkan no build** (`gpu/CMakeLists.txt`): `add_subdirectory(third_party/fsr-vulkan EXCLUDE_FROM_ALL)`
  com só o necessário ligado. O `bbgpu` liga, estático, `ffx-vulkan::fsr3-vk-framegeneration-3.1.6` e
  `ffx-vulkan::framegeneration-presenter-policy` (que puxam `ffx-vulkan::portable`). Host 1.1.4, backend 1.1.4,
  FSR4, probe, testes e install ficam desligados. Compila com MSYS2 CLANG64 sem erro (só avisos do SDK da AMD).
- **Ponte com buffers constantes reciclados**: o alvo 3.1.6 usa a cópia local
  `gpu/sotc/fsr/ffx_vk_fsr3_3_1_5_bridge.cpp` no lugar de `src/ffx_vk_fsr3_3_1_5_bridge.cpp` (troca feita no
  CMake, o submódulo não é modificado). Mudanças marcadas com `sotc:`:
  - os buffers constantes ficam mapeados e voltam para uma lista livre no `RetireFrame`, em vez de
    `vkCreateBuffer` + `vkAllocateMemory` + map a cada um (12 por quadro);
  - `ffxVkSotcSetRequiredSubgroupSize(32)` recria cada pipeline compute com subgroup fixo em 32 (para AMD,
    ver abaixo). O host tem que chamar antes de criar o contexto, com `Instance::FrameGenSubgroupSize()`.
  - ~~Medição no `fg-bench`: CPU igual com e sem reciclagem~~. Errado: a reciclagem **nunca rodava** (ver
    "Correção: vazamento por quadro na ponte" abaixo); o "igual" era o mesmo código vazando nas duas medições.
- **Correção: vazamento por quadro na ponte** (revisão): o wrapper 3.1.6 do fsr-vulkan nunca chamava
  `ffxVkFsr3_3_1_5BridgeBeginFrame`/`BridgeRetireFrame` (só o upscaler 3.1.5 chama). Então todo buffer
  constante e todo descriptor set (um `VkDescriptorPool` por job compute) ficava com `frameId` 0, que o retire
  da ponte nunca libera: ~13 `vkAllocateMemory` + ~34 pools por interpolação, até destruir o contexto. A 30 → 60
  isso passa do `maxMemoryAllocationCount` (4096 na NVIDIA) em ~10 s; quando a alocação falha, o `FgPass` marca
  `failed` e o FG desliga pelo resto da sessão (antes disso a memória e a CPU de gravação só crescem).
  - Correção: cópia local `gpu/sotc/fsr/ffx_vk_fsr3_3_1_6_framegen.cpp` (trocada no CMake como a ponte). O
    `RecordPrepare` abre o quadro na ponte com o `frameId` (antes dos jobs de layout do primeiro uso; recusa
    `frameId` 0) e o `ContextRetireFrame` chama `ffxVkFsr3_3_1_5BridgeRetireFrame`: buffers voltam para a lista
    livre, pools são destruídos. O `FgPass` já chamava o `ContextRetireFrame` depois do tick, nada mudou nele.
  - A ponte conta os objetos vivos (`ffxVkSotcBridgeLiveObjects`) e o `fg-bench` falha (`LEAKING`, código 1)
    se a contagem crescer depois do quadro 10. RTX 5060 Ti, 130 quadros:

    | | Buffers constantes (quadro 10 / 130) | Descriptor sets (10 / 130) | CPU gravação p50 |
    |---|---|---|---|
    | antes (wrapper do submódulo) | 129 / 1689 | 335 / 4415 | 1,1-1,8 ms |
    | depois | 14 / 14 | 0 / 0 | 0,18 ms |

    A interpolação e o tempo de GPU não mudaram (barra no meio nas três saídas; 1,07 / 1,61 / 3,11 ms p50).
- **Recursos de dispositivo** (`vk_instance.cpp`, só com `SOTC_FG=1`):
  - `Instance::FrameGenSupported()` exige `VK_KHR_compute_shader_derivatives` com `computeDerivativeGroupLinear`
    (só a KHR: os SPIR-V usam `SPV_KHR_compute_shader_derivatives`, a variante NV não serve),
    `shaderStorageImageRead/WriteWithoutFormat`, operações de subgroup basic/arithmetic/quad em compute, e
    subgroup de 32 (fixo, como na NVIDIA, ou fixável com `subgroupSizeControl`, como na AMD/Intel).
  - Quando suportado, liga a extensão e os recursos. `Instance::FrameGenSubgroupSize()` diz se é preciso fixar 32.
  - `Instance::IsPresentWaitSupported()`: liga `VK_KHR_present_id` + `VK_KHR_present_wait` quando existirem
    (opcional, para a Fase 2 esperar o present fora do `Scheduler::submit_mutex`).
  - Uma linha no log, uma vez: `Frame generation (SOTC_FG): supported - compute_shader_derivatives true, ...`.
  - Capacidade da lista de extensões subiu de 32 para 40 (sem efeito com FG desligado).
- `build.sh` confere também o submódulo `gpu/third_party/fsr-vulkan`. `gpu/VENDOR.txt` registra o uso.

### Spike 0.B na RTX 5060 Ti (sem abrir o jogo)

- `ffx_vk_fsr3_3_1_6_framegen_api_smoke` (RGBA8 e RGBA16F) **passou** de verdade (não "skip"), com a ponte
  local. Detalhe: a máquina não tem a camada `VK_LAYER_KHRONOS_validation`, então o teste original devolve 77
  (skip); rodei uma cópia de rascunho sem a camada. Ou seja: passou, mas **sem validação**.
- `fg-bench` (`tools/fg_bench.cpp`, `ninja -C out/gpu fg-bench`, roda sozinho, sem janela): tempo de GPU de
  `RecordPrepare` + `RecordDispatch`, render (profundidade/MV) 1920x1080, cor RGBA8, 120 quadros:

  | Saída | GPU p50 | GPU p95 | CPU gravação p50 | Memória do FG |
  |---|---|---|---|---|
  | 1920x1080 | 1,13 ms | 1,26 ms | 1,8 ms* | 96 MiB |
  | 2560x1440 | 1,23 ms | 1,24 ms | 1,0 ms | 112 MiB |
  | 3840x2160 | 1,55 ms | 1,56 ms | 1,1 ms | 160 MiB |

  \* o primeiro tamanho ainda inclui aquecimento. **Go do 0.B:** bem abaixo do limite de 2,5 ms a 1080p, e até
  4K cabe. As entradas do bench são imagens limpas (clear); os passes têm tamanho fixo de dispatch, então o
  conteúdo pouco muda o custo, mas convém confirmar com conteúdo real na Fase 3.

### Fase 2 (thread de pacing e `Flip` dividido, sem gerar quadros)

- **Nova thread `shadPS4:FramePacer`** (`video_core/renderer_vulkan/vk_frame_pacer.{h,cpp}`), criada pelo
  `Presenter` quando `SOTC_FG=1` **e** o device tem `VK_KHR_present_wait` (a RTX 5060 Ti tem). Sem
  present_wait, aviso no log e o caminho de sempre. Nome por `SetThreadDescription` e prioridade por
  `SetThreadPriority(HIGHEST)` (via `Common::SetCurrentThreadPriority(VeryHigh)`), porque
  `SetCurrentThreadName`/`SetCurrentThreadRealtime` não fazem nada no Windows.
- **`Flip` dividido** (`driver.cpp`): com o pacer, a `PresentThread` não apresenta mais. No tick de vblank em
  que pega o flip, ela entrega o `Frame` ao pacer (`Enqueue`, só um push numa fila) e segue com a metade
  "guest" igual a antes: estatísticas `SOTC FPS` (contam quadros reais, o regex dos scripts não muda),
  `flip_status`, eventos de flip e label. `DrawBlankFrame` vira um pedido ao pacer; `DrawLastFrame` não faz
  nada (em FIFO a imagem fica na tela). **A GpuDraw não muda nada.** A entrega é feita na `PresentThread` e não
  em `SubmitFlipInternal` (como o plano dizia) porque ali os quadros chegam na grade do vblank do guest
  (cadência regular, 33,3 ms em 30 FPS), e não com o jitter da GpuDraw; a latência é a mesma de hoje.
- **O pacer é o dono do swapchain e do `present_scheduler`**: acquire, blit, `BbOverlay`, present e `SetHDR`
  rodam nele. `Present` foi quebrado em `PresentImage` (acquire…present) + `ReleaseFrame` + `CountGameFrame`;
  sem o pacer o comportamento é o mesmo de antes.
- **Present nunca bloqueia dentro do `Scheduler::submit_mutex`**: escolhi `VK_KHR_present_wait`/`present_id`, e
  não uma fila só de present. Cada present recebe um id; logo depois o pacer espera (`vkWaitForPresentKHR`,
  timeout 100 ms, **fora** do lock) até ele estar na tela. Assim, quando o próximo `vkQueuePresentKHR` roda
  sob o lock, não há nenhum present na fila do FIFO e ele não tem por que bloquear. Motivos: (1) a extensão já
  era ligada pela Fase 1, sem mudar a criação de filas; (2) o mesmo wait dá o **instante real em que cada
  imagem entrou na tela**, que o pacing precisa de qualquer jeito; (3) limita a fila do FIFO a 0-1 imagem
  (latência). Uma 2ª fila só para present tiraria o present do lock, mas não daria timing e deixaria a fila do
  FIFO crescer. O tempo que o present segura o lock é medido (`present lock max` na linha de estatística).
- **Swapchain com FG**: FIFO forçado (ignora `BB_PRESENT_MODE`; linha `Frame pacer (SOTC_FG): Fifo present
  mode` no log), `minImageCount + 2` imagens, pool de `Frame` com pelo menos 4.
- **Pacing**:
  - `D` = EMA (α 0,1) dos intervalos entre quadros reais na entrega; intervalos > 100 ms (carregamento) ficam
    fora.
  - `P` = período real da tela, medido pelos instantes do present_wait: dois presents seguidos ficam a k
    refreshes (k = 1..8) um do outro; a estimativa parte da taxa do modo de vídeo (`BbDisplayRefreshHz`,
    inteira) e é refinada (EMA 0,05) para a real (59,94, 143,9…). Intervalos que não são múltiplo inteiro
    são rejeitados (contados em `rejected`).
  - Nunca mais de 1 quadro real esperando: se chegaram dois, o mais velho é descartado (`dropped`).
  - `SOTC_FG_DEBUG=passthrough`: o real é apresentado assim que chega (cópia 1, que na Fase 3 será a gerada) e
    de novo em `t_cópia1 + D/2` (cópia 2, que na Fase 3 será o real). Como o FIFO mostra no 1º vblank depois do
    submit, a cópia 2 é submetida meio refresh antes (`D/2 - P/2`), para cair no vblank mais perto do meio.
    Se o próximo real chegar antes, a cópia 2 é pulada (`late`). Se `D < SOTC_FG_MIN_RATIO × P` (padrão 1,9;
    ex.: 60 FPS reais em 60 Hz) a cópia 2 não é feita (`ratio`), senão o FIFO seguraria a GpuDraw.
  - **Vblank do guest preso à tela** (`SOTC_FG_VBLANK_LOCK`, padrão 1 com FG): depois de 120 amostras, se a
    estimativa de `P` ficou a menos de 1% da taxa do modo de vídeo e um múltiplo inteiro de `P` fica a menos
    de 0,5% do período do guest (60 Hz do guest em tela de 59,94 ou 119,88 Hz), o `AccurateTimer` da
    `PresentThread` passa a usar `m × P`. Sem isso os reais escorregam em relação à tela (uma vez a cada ~16 s
    em 59,94 Hz) e aparece um engasgo periódico. Em 144 Hz não há múltiplo (144/60 = 2,4): fica nominal.
    Só trava a taxa, não a fase (`ponytail:` no código).
- **Estatística** (com `SOTC_FPS=1`, uma linha a mais a cada 5 s, sem mudar a `SOTC FPS`):
  `SOTC FG pacer[ passthrough]: real X/s, presents X/s, second X/s (skipped: late N, ratio N), dropped N;
  display X Hz (N rejected), real interval X ms, guest vblank X Hz[ (locked)]; pace err p50 X p99 X ms;
  present lock max X ms, timeouts N`. O erro de pacing é |intervalo mostrado − ideal| (ideal `D` sem
  passthrough, `D/2` com).
- **Teste** sem jogo: `ninja -C out/gpu frame-pacer-test && out/gpu/frame-pacer-test.exe` confere a estimativa
  de refresh e a trava do vblank (`tests/test_frame_pacer.cpp`).

### Fase 3 (interpolação FSR 3.1.6 só com optical flow, entradas neutras)

- **Novo `video_core/renderer_vulkan/host_passes/fg_pass.{h,cpp}`** (`HostPasses::FgPass`), criado pelo pacer
  quando `SOTC_FG=1`, o device passa em `FrameGenSupported()` e não é `passthrough`. Grava
  `RecordPrepare` (optical flow + preparação do FI) e `RecordDispatch` (o quadro gerado) da ponte 3.1.6 nos
  command buffers **do `present_scheduler`, na thread do pacer** (command buffers reais; nunca na GpuDraw, cujo
  recorder usa handles falsos), num submit próprio que espera o `ready_tick` do quadro real.
- **Imagens**:
  - os `Frame` do pool passam a ser `R8G8B8A8_UNORM` quando o FG pode rodar (`fg_frames`), e o pp pass é
    criado nesse formato. O FI lê o `frame->image` **direto** (importado com `SAMPLED|TRANSFER_SRC`, em
    `General`, que é onde o pp pass deixa o quadro); não há anel de cópias. O blit para a swapchain BGRA já
    converte. Com `SOTC_FG` desligado o formato continua o da surface;
  - próprias do FG: 2 saídas RGBA8 no tamanho do quadro (`SAMPLED|STORAGE|TRANSFER_SRC`, alternadas: a gerada
    de N fica válida até o N+2), uma profundidade `R32_SFLOAT` constante (0,5) e um MV `R16G16_SFLOAT` zerado,
    no tamanho de render (≤ 1920 de largura), limpos uma vez a cada (re)criação.
- **Parâmetros neutros** (todos finitos e positivos, como a ponte exige): `cameraNear 0,1`, `cameraFar 10000`,
  `FOV 1,0`, `viewSpaceToMeters 1`, câmera na origem com up (0,1,0), right (1,0,0), forward (0,0,−1), escala de
  MV = tamanho de render, jitter 0 (o perfil fixo do 3.1.6 não tem jitter), transferência sRGB, luminância 0-1,
  `frameTimeMilliseconds` = EMA `D`.
- **Contexto**: criado no primeiro quadro e recriado quando o tamanho de saída muda (as extensões são fixas).
  Antes de criar, `ffxVkSotcSetRequiredSubgroupSize(instance.FrameGenSubgroupSize())`. Uma linha no log a
  cada criação, com `ContextGetMemoryUsage`:
  `Frame generation (SOTC_FG): FSR 3.1.6 frame interpolation 1920x1080 (neutral depth and motion 1920x1080),
  FI/OF memory 95.9 MiB + own images 31.6 MiB, subgroup 0, timestamps true` (valores esperados na RTX 5060 Ti a 1080p).
- **Ordem no pacer** (por quadro real N): `Generate(N)` → present do gerado N−1→N logo em seguida → espera
  `D/2 − P/2` → present do real N. Ou seja, as duas cópias do passthrough da Fase 2 viram gerado + real.
  `RetireFrame(id)` só depois que o tick do submit terminou (lido sem bloquear no `Generate` seguinte).
- **Sem gerar (só o real, e a história recomeça com `reset`)**: primeiro quadro; quadro em branco; intervalo
  ≥ 100 ms ou > 2×EMA (carregamento, compilação de shader); HDR ligado (`frame->is_hdr`, o perfil do 3.1.6 é
  sRGB); mudança de tamanho; `D < SOTC_FG_MIN_RATIO × P` (sem lugar para duas apresentações; ex. 60 FPS reais em
  60 Hz); e depois de um `late` (o real seguinte chegou antes do real atual ir para a tela: o real atual é
  mostrado na hora e o próximo não ganha gerado, para a fila não atrasar). O detector de corte de cena do
  próprio OF cobre cortes de câmera. Se uma chamada da ponte falhar, o FG desliga na sessão (log de erro) e só
  os reais são mostrados.
- **Sincronização**: depois do FI, uma barreira de memória compute/transfer → all commands no mesmo command
  buffer. Ela ordena as leituras do quadro real (e a cópia para o histórico do FI) antes da transição de layout
  do present e antes de a GpuDraw reescrever o quadro (mesma fila, submit posterior); o present do gerado espera
  o semáforo do submit do FI.
- **Tempo de GPU**: 2 timestamps por interpolação (query pool próprio, lidos sem esperar quando o submit já
  terminou), se a fila tiver `timestampComputeAndGraphics`.
- **Estatística**: a linha `SOTC FG pacer` (com `SOTC_FPS=1`) ganha no fim
  `; FG generated X/s, interpolations X/s (new history N), fi gpu avg X max X ms`. `generated` = gerados que
  foram para a tela; `new history` = interpolações que recomeçaram a história (não mostram nada). A linha
  `SOTC FPS` não muda (continua contando só quadros reais).
- **`SOTC_FG_DEBUG=gen`**: cada gerado é apresentado duas vezes e nenhum real (para inspecionar o FI; capturas
  da janela só mostram gerados).
- **`fg-bench` corrigido e com verificação de movimento**: o bench antigo limpava a imagem de MV com
  (0, 1, 0, 1), ou seja, um MV de uma tela inteira na vertical, e o "gerado" saía como o quadro atual
  deslocado meia tela. Isso fazia parecer que o FI não interpolava. Agora o bench usa as entradas neutras do
  port (MV 0, profundidade 0,5, quadro em `General`, mesmos usos de imagem) e quadros reais de verdade: uma
  barra com textura andando 32 px por quadro sobre ruído estático. No último gerado a barra tem que estar no
  meio do caminho entre os dois reais. Resultado na RTX 5060 Ti:

  | Saída | Barra anterior / atual / gerada | GPU p50 | GPU p95 | Memória FI/OF |
  |---|---|---|---|---|
  | 1920x1080 | 799,5 / 831,5 / 813,0 (meio 815,5) | 1,09 ms | 1,10 ms | 96 MiB |
  | 2560x1440 | 1119,5 / 1151,5 / 1136,5 (meio 1135,5) | 1,6-4,2 ms | 2,1-7,9 ms | 112 MiB |
  | 3840x2160 | 1759,5 / 1791,5 / 1779,0 (meio 1775,5) | 3,1-4,0 ms | 3,8-9,4 ms | 160 MiB |

  Ou seja: **o optical flow interpola de verdade com entradas neutras**. A 1080p (saída padrão do jogo) o custo
  é ~1,1 ms. Acima de 1080p o tempo ficou bem maior e instável com conteúdo real (antes, com imagens limpas,
  era 1,2-1,6 ms); pode ser a upload de 16-33 MB por quadro do próprio bench disputando a GPU/clocks, mas não
  foi isolado. Conferir a linha `fi gpu` no jogo com `SOTC_OUTPUT_RES`.

## Variáveis

| Variável | Valores (padrão) | Efeito hoje |
|---|---|---|
| `SOTC_FG` | 0/1 (0) | Com 1: confere e liga os recursos de device do FG e escreve a linha no log. Com present_wait, os quadros passam pelo pacer (FIFO). Se o device suporta o FI, gera 1 quadro interpolado (FSR 3.1.6, só optical flow) entre cada par de reais; senão, 1 present por real. |
| `SOTC_FG_DEBUG` | passthrough / gen (vazio) | `passthrough`: sem FI, cada real é apresentado duas vezes, a 2ª meio intervalo depois. `gen`: cada gerado duas vezes e nenhum real (inspeção). |
| `SOTC_FG_MIN_RATIO` | número (1.9) | Mínimo de refreshes por intervalo real para fazer a 2ª apresentação. |
| `SOTC_FG_VBLANK_LOCK` | 0/1 (1) | Prende o vblank do guest a um múltiplo do refresh medido quando ficam a < 0,5%. |

## Como testar (dono)

1. Build normal (`bash build.sh`) ou, no worktree, `cmake` + `ninja -C out/gpu bb-probe`.
2. Sem `SOTC_FG`: tem que ser idêntico ao 0.65 (mesmo FPS, nenhuma linha "Frame generation" no log).
3. Com `SOTC_FG=1`: o jogo roda igual; em `user/last_run.log` deve aparecer
   `Frame generation (SOTC_FG): supported - compute_shader_derivatives true, storage images without format true,
   compute subgroup quad/arithmetic true, subgroup 32-32 (fixed 32), present_wait true` (ou o que faltar).
   Também as linhas `Enabling extension: VK_KHR_compute_shader_derivatives` (e present_id/present_wait).
4. Opcional: `ninja -C out/gpu fg-bench && out/gpu/fg-bench.exe` mede o custo do FG na GPU.

### Fase 2 (pacer)

Sempre com `SOTC_FPS=1` para ter as linhas `SOTC FPS` e `SOTC FG pacer` no log. Modo padrão (30 FPS, sem
fps60) é o alvo principal.

1. **Linha de base**: `SOTC_FG=0 BB_PRESENT_MODE=Fifo`. Anotar o `SOTC FPS` no Santuário/attract.
2. **Pacer, 1 present por real**: `SOTC_FG=1`. No log: `Frame pacer (SOTC_FG): real frames only, display seed
   144 Hz, guest vblank 60 Hz, vblank lock true` e `Frame pacer (SOTC_FG): Fifo present mode`. Na linha
   `SOTC FG pacer`: `real` ≈ `SOTC FPS` (≈30), `presents` ≈ `real`, `display` ≈ 143,9-144,0 Hz, `present lock
   max` bem abaixo de 1 ms, `timeouts 0`, `dropped` ~0. `SOTC FPS` tem que ficar no ruído (±2%) do passo 1.
3. **Passthrough**: `SOTC_FG=1 SOTC_FG_DEBUG=passthrough`. Esperado em 144 Hz a 30 FPS: `presents` ≈ 60/s,
   `second` ≈ 30/s, `late`/`ratio` ~0, `pace err p99` ≤ ~7 ms (um refresh: em 144 Hz os 16,7 ms viram 2 ou 3
   vblanks, 13,9/20,8 ms, isso é do FIFO sem VRR). A imagem tem que parecer igual a 30 FPS (cada quadro duas
   vezes), sem engasgos novos. `SOTC FPS` no ruído do passo 1.
4. **Monitor em 120 Hz** (opcional, no Windows): passthrough deve dar `pace err` perto de 0 (16,7 ms = 2
   vblanks exatos) e, se a tela for 119,88 Hz, `guest vblank 59.94x Hz (locked)` depois de alguns segundos.
   `SOTC_FG_VBLANK_LOCK=0` desliga a trava para comparar.
5. **fps60** (secundário): `SOTC_MODES=…,fps60` com passthrough: `second` ≈ `real` em 144 Hz (razão ~3).
6. **Robustez**: redimensionar a janela, minimizar e voltar, alternar tela cheia, abrir o diálogo de save
   (`BbOverlay` tem que aparecer), carregar save. Nada pode travar; `timeouts` pode subir enquanto minimizado.
7. PresentMon (opcional): ~2× presents em passthrough; `MsBetweenDisplayChange` alternando 13,9/20,8 ms
   em 144 Hz.

### Fase 3 (interpolação)

Sempre `SOTC_FPS=1`, modo padrão (30 FPS) primeiro. Com `SOTC_FG=1` (sem `SOTC_FG_DEBUG`) o FI já está
ligado.

1. **Log**: `Frame pacer (SOTC_FG): frame interpolation (optical flow, neutral depth and motion), ...` e, no
   primeiro quadro do jogo, `Frame generation (SOTC_FG): FSR 3.1.6 frame interpolation WxH ..., FI/OF memory
   X MiB + own images Y MiB, subgroup 0, timestamps true`. Nenhum `interpolation failed` / `context ... failed`.
2. **Linha `SOTC FG pacer`** no Santuário/attract a 30 FPS em 144 Hz: `real` ≈ 30, `presents` ≈ 60,
   `second` ≈ 30, `FG generated` ≈ 30/s, `interpolations` ≈ 30/s, `new history` só em carregamentos/cortes,
   `fi gpu avg` ~1-1,5 ms a 1080p, `late`/`ratio`/`dropped` ~0, `present lock max` < 1 ms.
3. **FPS real**: `SOTC FPS` no ruído (±3%) de `SOTC_FG=0 BB_PRESENT_MODE=Fifo`. Se cair mais, a causa provável é
   a fila única (o FI na frente dos readbacks da GpuDraw): anotar e partir para a fila compute (6a).
4. **Imagem**: tem que parecer 60 fps, sem cores trocadas (BGRA/RGBA), sem tela preta/piscando, sem meia tela
   deslocada. Pan de câmera, Agro galopando, escalada. Artefatos esperados: bordas de objetos em movimento e o
   HUD (medidor, legendas) "arrastando", porque o MV é zero e o HUD está queimado na imagem.
5. **`SOTC_FG_DEBUG=gen`**: só gerados na tela (cada um duas vezes). Bom para ver os artefatos do FI isolados e
   para capturas (`shot_garbage.ps1`).
6. **fps60** (secundário, VRR): `SOTC_MODES=…,fps60` em 144 Hz: `FG generated` ≈ `real`. Em 60 Hz fixo o FI se
   desliga sozinho (`ratio` sobe, `interpolations` 0).
7. **Robustez**: os mesmos testes da Fase 2 (redimensionar, minimizar, tela cheia, diálogo de save,
   carregar). Mudar `SOTC_OUTPUT_RES` recria o contexto (1 linha nova no log). `port_soak.ps1` 10 min com
   `SOTC_FG=1`: memória estável e o FG ainda ligado no fim (`FG generated` subindo, nenhuma linha
   `interpolation failed` no log). Sem a correção da ponte a memória crescia sem parar e o FG podia se desligar sozinho.
8. **Sem jogo**: `ninja -C out/gpu fg-bench && out/gpu/fg-bench.exe` tem que imprimir `interpolated` e
   `recycled` nas três saídas e terminar com código 0.

## Pendências e riscos

- **Fases 2 e 3 não foram rodadas com o jogo** (regra: só o dono roda). O build liga, o `frame-pacer-test`
  passa e o `fg-bench` mostra a interpolação funcionando com as entradas neutras do port.
- **Qualidade só com optical flow é desconhecida no jogo**: com MV zero o FI ainda monta o campo de vetores do
  "jogo" (tudo parado) e escolhe, pixel a pixel, entre ele e o OF pela similaridade de cor. No bench a barra
  sai no meio do caminho; em cena real (folhagem, pelo do colosso, partículas, HUD) pode haver "ghosting".
  É o 0.D da revisão: o dono avalia; se ficar ruim, a Fase 4 (MV/profundidade do jogo) é que resolve.
- **Custo acima de 1080p**: no bench o FI a 1440p/4K deu 1,6-4 ms (p95 até 9 ms, variando muito entre execuções) com conteúdo real; a 1080p,
  1,1 ms. Pode ser artefato do bench (upload grande por quadro), mas se a linha `fi gpu` no jogo com
  `SOTC_OUTPUT_RES` alto confirmar, limitar o FG a ≤ 1440p.
- **Fila única**: o FI (≈1 ms × 30/s) entra na mesma VkQueue dos readbacks da GpuDraw. A 30 FPS sobra GPU e
  tempo, mas medir o `SOTC FPS` (passo 3 acima).
- **Latência**: em 30 → 60 o real N vai para a tela ~D/2 = 16,7 ms depois de chegar (o gerado vai antes), mais
  o FI (~1 ms) e o FIFO. Total esperado +20-35 ms em relação ao Mailbox padrão.
- **HDR** desliga o FI (só reais) enquanto estiver ligado; os `Frame` ficam RGBA8 com FG (como já eram 8 bits
  no formato da surface, não há perda nova).
- **CPU de gravação** ~0,18 ms por quadro real (bench, depois da correção do retire da ponte; antes era
  1,1-2,2 ms e crescendo). Ainda cria um `VkDescriptorPool` por job compute e o destrói no retire (~34 por
  quadro); um pool reciclado seria o próximo passo se a linha de CPU no jogo pedir.
- **VRR**: não é detectado. Com G-Sync a cópia 2 aparece meio refresh antes do meio do intervalo (o "−P/2"
  só vale com refresh fixo), e a estimativa de `P` vira o intervalo de present (a trava do vblank não liga,
  pela guarda de 1%). Ainda pendente (Fase 3 também não faz): detectar VRR (intervalos que não são múltiplos) e mirar `D/2` direto.
- **Precisão do sono**: a espera da cópia 2 é `condition_variable::wait_until` (≈1 ms com o `timeBeginPeriod(1)`
  do `probe.c`). Basta para FIFO (o present é submetido meio refresh antes); se o `pace err` pedir, trocar por
  `CreateWaitableTimerExW(HIGH_RESOLUTION)`.
- **Trava do vblank só de taxa**, não de fase, e só quando a tela é múltiplo de 60 (59,94/60/119,88/120 Hz).
  Em 144 Hz, 30 → 60 em FIFO sem VRR sempre alterna 2/3 vblanks; recomendação para o dono: tela em 120 Hz
  ou VRR.
- **Semente do refresh** é a taxa inteira da tela **principal** (`BbDisplayRefreshHz`). Com a janela numa tela
  secundária de outra taxa, a estimativa não sai da semente (`rejected` sobe). O driver expõe
  `VK_EXT_present_timing`, que daria o período exato por swapchain: caminho de upgrade.
- **FIFO na sessão inteira** com `SOTC_FG=1` (a revisão já apontava): sem `VK_EXT_swapchain_maintenance1` não
  dá para trocar o modo por present. FIFO custa ~1 refresh de latência a mais que Mailbox.
- **MMCSS** (`AvSetMmThreadCharacteristicsW`) não é usado; só `THREAD_PRIORITY_HIGHEST`.
- O pacer só faz present quando chega quadro (ou pedido de quadro em branco); com o guest pausado
  (`IsGuestThreadsPaused`, depurador) a tela fica parada, e o `BbOverlay` só atualiza com quadros novos (o
  jogo continua dando flip com o diálogo de save aberto, mas conferir).
- **Exports**: o SDK 2.3 marca `ffxCreateContext` etc. com `__declspec(dllexport)` no Windows; agora que o
  `bb-probe.exe` usa o FI (Fase 3), esses símbolos são exports do exe. Inofensivo, mas impede a ThinLTO de
  internalizá-los. Não testei LTO/PGO com o FSR referenciado (build deste worktree é `BB_LTO=OFF`, `BB_PGO=off`).
- **Validação**: sem a camada de validação instalada, nem o FSR nem o pacer/FI rodaram com validação. Instalar
  o Vulkan SDK (ou `mingw-w64-clang-x86_64-vulkan-validation-layers`) e rodar uma vez com `BB_VK_VALIDATION=1
  SOTC_FG=1` (procurar erros de layout/sincronização nas imagens "FG output", "Frame image #").
- **Sem `SOTC_FG_DEBUG=tint`** (o plano citava): o `gen` já serve para inspecionar. Sem `SOTC_FG_INPUTS`: só
  existe `flow` até a Fase 4.
- **AMD**: o pin de subgroup 32 está implementado mas não foi testado em AMD.
- **Créditos no pacote** (LICENSE/NOTICE do fsr-vulkan e avisos MIT da AMD) ficam para quando o recurso for
  entregue (Fase 6). Não chamar de "FSR 3"/"FSR4" no launcher.
- `bb-gpu-capabilities` não mostra os recursos do FG (é um probe do launcher para resolução ao vivo); a linha
  no log do jogo cumpre esse papel.
