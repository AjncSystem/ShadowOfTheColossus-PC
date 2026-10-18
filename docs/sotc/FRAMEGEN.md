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

## Variáveis

| Variável | Valores (padrão) | Efeito hoje |
|---|---|---|
| `SOTC_FG` | 0/1 (0) | Com 1: confere e liga os recursos de device do FG e escreve a linha no log. Ainda não gera quadros. |

## Como testar (dono)

1. Build normal (`bash build.sh`) ou, no worktree, `cmake` + `ninja -C out/gpu bb-probe`.
2. Sem `SOTC_FG`: tem que ser idêntico ao 0.65 (mesmo FPS, nenhuma linha "Frame generation" no log).
3. Com `SOTC_FG=1`: o jogo roda igual; em `user/last_run.log` deve aparecer
   `Frame generation (SOTC_FG): supported - compute_shader_derivatives true, storage images without format true,
   compute subgroup quad/arithmetic true, subgroup 32-32 (fixed 32), present_wait true` (ou o que faltar).
   Também as linhas `Enabling extension: VK_KHR_compute_shader_derivatives` (e present_id/present_wait).
4. Opcional: `ninja -C out/gpu fg-bench && out/gpu/fg-bench.exe` mede o custo do FG na GPU.

## Pendências e riscos

- **Nada do FG roda ainda**: o `bb-probe.exe` não referencia o FSR, então o linker não puxa os objetos (o
  `fg-bench` prova que o fechamento inteiro liga com CLANG64). A Fase 2 (pacer + `Flip` dividido) e a Fase 3
  (FI só com optical flow) vêm depois.
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
