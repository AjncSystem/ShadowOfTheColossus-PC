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
  - Medição no `fg-bench` (RTX 5060 Ti): o tempo de CPU de gravação ficou igual dentro do ruído com e sem a
    reciclagem (~1,0-1,1 ms a 1440p/4K). O ganho esperado é não disputar o kernel do WDDM com as outras
    threads, coisa que o bench isolado não mostra.
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

## Variáveis

| Variável | Valores (padrão) | Efeito hoje |
|---|---|---|
| `SOTC_FG` | 0/1 (0) | Com 1: confere e liga os recursos de device do FG e escreve a linha no log. Com present_wait, os quadros passam pelo pacer (FIFO, 1 present por real). Ainda não gera quadros. |
| `SOTC_FG_DEBUG` | passthrough (vazio) | `passthrough`: cada real é apresentado duas vezes, a 2ª meio intervalo depois. |
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

## Pendências e riscos

- **Fase 2 não foi rodada com o jogo** (regra: só o dono roda). O build liga e o `frame-pacer-test` passa.
- **VRR**: não é detectado. Com G-Sync a cópia 2 aparece meio refresh antes do meio do intervalo (o "−P/2"
  só vale com refresh fixo), e a estimativa de `P` vira o intervalo de present (a trava do vblank não liga,
  pela guarda de 1%). Fase 3: detectar VRR (intervalos que não são múltiplos) e mirar `D/2` direto.
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
- **Ainda não gera quadros**: o `bb-probe.exe` não referencia o FSR, então o linker não puxa os objetos (o
  `fg-bench` prova que o fechamento inteiro liga com CLANG64). A Fase 3 (FI só com optical flow, 30 → 60
  primeiro) entra nas duas cópias do passthrough: cópia 1 = gerada N-1→N, cópia 2 = real N.
- **Exports**: o SDK 2.3 marca `ffxCreateContext` etc. com `__declspec(dllexport)` no Windows, então quando o
  código for puxado esses símbolos vão aparecer como exports do `bb-probe.exe`. Inofensivo, mas impede a ThinLTO
  de internalizá-los. Não testei LTO/PGO com o FSR referenciado (build deste worktree é `BB_LTO=OFF`, `BB_PGO=off`).
- **Validação**: sem a camada de validação instalada, nenhum teste do FSR rodou com validação. Instalar o
  Vulkan SDK (ou `mingw-w64-clang-x86_64-vulkan-validation-layers`) antes da Fase 3.
- **CPU de gravação ~1 ms por quadro real** na thread que gravar o FG (pacer). Aceitável para 30 → 60;
  boa parte deve ser criação de descriptor sets por pass (a ponte ainda cria e destrói descriptor sets por
  quadro). Medir de novo na Fase 3.
- **AMD**: o pin de subgroup 32 está implementado mas não foi testado em AMD.
- **Créditos no pacote** (LICENSE/NOTICE do fsr-vulkan e avisos MIT da AMD) ficam para quando o recurso for
  entregue (Fase 6). Não chamar de "FSR 3"/"FSR4" no launcher.
- `bb-gpu-capabilities` não mostra os recursos do FG (é um probe do launcher para resolução ao vivo); a linha
  no log do jogo cumpre esse papel.
