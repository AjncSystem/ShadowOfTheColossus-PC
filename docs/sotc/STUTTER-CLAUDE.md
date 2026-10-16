# Thread de comandos e compilação de pipelines — Claude, 13–14/10/2026

Continuação de [STUTTER-CODEX.md](STUTTER-CODEX.md), sobre o mesmo código.

## O que limitava o FPS e de onde vinha o stutter

Medições numa RTX 5070 + Ryzen 7 9800X3D, Windows 11.

- **A GPU fica ociosa; o limite é uma thread de CPU.** A thread `shadPS4:GpuCommandProcessor` (CP)
  traduz os comandos do PS4: 4–7 mil por quadro no campo aberto, a ~3–4 µs cada, ~95% ocupada
  com o FPS destravado. Perfil por amostragem (`SOTC_PROFILE=GpuCommandProcessor`, 24.873
  amostras no campo): `BindResources` 37%, compute 18%, chave e busca de pipeline ~12%,
  compilação no driver ~8%, esperas de readback ~5%, locks + `VirtualProtect` ~7%.
- **Stutter, primeira visita a um lugar:** criação de pipelines no meio do quadro. Numa sessão de
  199 s cavalgando: 327 pipelines gráficos novos, média de 14 ms cada (até 340 ms num quadro).
  No demo da tela de título, uma troca de cena cria ~90 pipelines de **compute** de uma vez,
  20–190 ms cada: **5,4 s de tela parada**, sempre no mesmo ponto.
- Na segunda visita tudo vem do cache: o warm-up do boot recria os pipelines e o stutter some.

## Mudanças (cada uma com variável para A/B)

### Compilação de pipelines

| Mudança | Onde | Desligar |
|---|---|---|
| Pipelines **gráficos** novos são criados em threads `sotc:PipelineCompile` (até 4). O draw é pulado enquanto compila, **só** quando é seguro (abaixo); senão espera. A tradução do shader e a especialização continuam na CP, na hora do draw. | `vk_async_compiler.h`, `vk_pipeline_cache.cpp` (`EnqueueGraphics`, `PublishGraphics`), `vk_rasterizer.cpp` (`GetDrawPipeline`, `MaySkipForCompile`) | `SOTC_ASYNC_PIPELINES=0` |
| Warm-up do boot em paralelo (até 12 threads): 2.442 pipelines em 0,88 s contra 3,0 s serial (cache do driver quente). | `vk_pipeline_serialization.cpp` | `SOTC_PARALLEL_WARMUP=0` |
| Snapshot do cache do driver fora da CP, só quando não há pipeline novo há 2 s (no máximo a cada 120 s mesmo assim). O destrutor tenta um save final (o fechamento da janela usa `std::_Exit`, então não é garantido). | `SaveDriverCache`, `OnFrame` | `SOTC_DRIVER_CACHE_ASYNC=0` |
| Cache de pipeline por worker, fundido no principal nos checkpoints. Precaução, sem efeito medido. | `worker_caches`, `MergeWorkerCaches` | `SOTC_WORKER_PIPELINE_CACHES=0` |

**Quando um draw pode ser pulado** (`MaySkipForCompile`): nenhum estágio escreve memória; nenhum
clear de depth/stencil por `DB_RENDER_CONTROL`; todo alvo de cor é tiled, maior que 64 px de
pitch, com pelo menos 256×256 px e nunca copiado para buffer (alias de texel buffer); cada alvo
(cor e depth) foi desenhado em quadros **seguidos** até o anterior, numa sequência iniciada há
pelo menos 2 quadros (`SOTC_ASYNC_PIPELINE_MIN_FRAMES`); o pipeline está pendente há no máximo
4 quadros / 100 ms (`SOTC_ASYNC_PIPELINE_MAX_FRAMES`, `SOTC_ASYNC_PIPELINE_MAX_MS`). O draw
pulado marca os bancos de LOD das texturas que usaria (streaming com linger 0).

Compute nunca é pulado: os dispatches alimentam readbacks.

### Custo de CPU por draw na CP

| Mudança | Ganho estimado no perfil | Desligar |
|---|---|---|
| Texel buffer sincronizado uma vez por bind (era duas) e registrado como escrito só quando a GPU escreve (HTILE ou `TileImage`, com a extensão de todos os mips). | ~4–5% | `SOTC_TEXEL_SYNC_LEGACY=1` |
| `BindBuffers`: uma consulta de mapeamento em vez de três (equivalência exata). | ~3,7% | `SOTC_BIND_MAPCHECK=1` |
| Cache de regiões por thread em `runtime_memory.c`: 16 entradas, MRU, fim do trecho mapeado contínuo (`run_end`), `is_mapped` também preenche o cache. | ~1,5% | bit 1 de `runtime_disabled_optimizations` |
| Memo de residência no `EnsureResident`. | ~1,6% | `SOTC_RESIDENT_MEMO=0` |
| `IntervalList::Add` sai cedo se o intervalo já está coberto. | 1,5–3% | `SOTC_ACCESS_FASTADD=0` |
| `AddGpuModified` não varre a cache de páginas limpas se o conjunto não mudou. | ~0,7% | — |
| Memo de especialização de shader por programa (fora de VS com fetch shader e de tesselação, que leem memória do jogo). | ~3,5% | `SOTC_SPEC_MEMO=0` |
| `SearchBinaryInfoCached` com atalho por thread, validado pela geração da tabela de mapeamentos. | ~2,5% | `SOTC_BININFO_FAST=0` |
| Cache de texturas: `UpdateImage` sem lock quando não há nada a fazer; LRU tocado uma vez por tick; flags das imagens atômicas (corrige uma corrida antiga que podia perder `CpuDirty`); falhas do jogo em regiões sem imagens não pegam o mutex. | ~5% | `SOTC_TEX_FASTPATH=0`, `SOTC_TEX_FAULT_FILTER=0` |
| `FindView` tenta a última view; `PopPendingOperations` só consulta a GPU com algo pendente; HLE de replicate invalida só texturas; aviso de metadado por textura só com `SOTC_META_WARN=1`; consulta de VRAM a cada 50 ms. | ~2–3% | `SOTC_POP_LAZY=0`, `SOTC_HLE_FULL_INVALIDATE=1`, `SOTC_VRAM_QUERY_MS=0` |

Nenhuma dessas mudanças adia ou reordena trabalho da GPU em relação ao fluxo de comandos (a regra de
[CAMINHO-B.md](CAMINHO-B.md)): ou pulam trabalho que comprovadamente não faz nada, ou trocam uma busca
por outra que dá a mesma resposta.

### Diagnóstico

- `SOTC_FPS=1` ganha a linha `SOTC pipelines: N built on workers, N draws skipped, N waits`.
- `SOTC slow compute pipeline <hash>: N ms` para criações de compute de 20 ms ou mais.
- `SOTC pipeline cache: preloaded N pipelines in N ms (N threads)` no boot.

## O que não funcionou

- **`VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION` para compute** (`SOTC_COMPUTE_FAST=1`). Parecia eliminar
  a parada de 5,4 s, mas era o cache de shaders do próprio driver NVIDIA (`%LOCALAPPDATA%\NVIDIA\GLCache`,
  um arquivo novo de ~58 MB por execução) acertando entre as rodadas. Com o cache do driver isolado
  e vazio (`__GL_SHADER_DISK_CACHE_PATH=<pasta nova>`), as duas variantes deram o mesmo: 117–120
  compilações lentas, 9,2 s no total. Fica como opção de teste, desligada.
- **Trocar compute otimizado em segundo plano** (`SOTC_COMPUTE_OPTIMIZE=1`): depende da opção acima.

**Para medir compilação, isole o cache do driver** (`__GL_SHADER_DISK_CACHE_PATH`), senão a segunda
rodada mede acertos de cache e não o código.

## Revisão

Uma revisão adversarial do diff (5 áreas, cada achado contestado por um verificador) confirmou a
mecânica: snapshot completo de `Shader::Info`, construtor seguro em threads, tempo de vida dos jobs,
ordem do destrutor, sincronização exigida pelo Vulkan. As correções que ela pediu já estão aplicadas:
recorrência por quadros seguidos, clears de DB e alvos pequenos ou com alias nunca pulados, um snapshot
de cache por vez, compute rápido só com o store aberto, pool de descritores atualizando o tick antes de
criar outro.

Riscos conhecidos e aceitos: um draw pulado num alvo que acumula entre quadros (sem clear) perde aquela
contribuição; um alvo cujo conteúdo chega à CPU por amostragem num passe que escreve buffer não é
detectado (só o alias direto de texel buffer). Os dois só acontecem na primeira compilação de um
pipeline que não estava no cache do boot. `SOTC_ASYNC_PIPELINES=0` remove ambos.

## Ideias que ficaram de fora

- **Pacote de cache** (chaves, metadados e SPIR-V de uma partida inteira, sem o blob do driver) lido
  como segunda fonte no warm-up: a única forma de evitar a primeira parada de compute. Os arquivos são
  derivados dos shaders do jogo; distribuir é decisão do projeto.
- Antecipar no fluxo de comandos os dispatches de compute de uma rajada para compilar em paralelo.
- `ImageDescCache`, memo de alvo de render, reciclagem de `VkImage`, seqlock no VMA, readbacks com
  cópia antecipada: próximos itens do perfil.
- LTO (`BB_LTO=ON`): +1% medido; o `build.sh` do Windows o deixa desligado.

## Medições

Demo da tela de título (rota fixa, sem entrada), FPS destravado (vblank 120 Hz, `BB_FPS_LIMIT=0`),
mesmo cache inicial do port, de 60 a ~215 s. Harness: `out/claude-ab/ab.sh` sobre
`tools/sotc_route_benchmark.py`; resumo: `out/claude-ab/compare.py`.

Cada build rodou com o cache de shaders do driver NVIDIA isolado e vazio no início, duas sessões
seguidas: a 1ª é a primeira visita (compila o que é novo), a 2ª usa o cache que a 1ª deixou.
Build do Codex: `out/claude-ab/base.exe` (`-O2`, sem LTO). Final: `out/claude-ab/final.exe` (ThinLTO).

| | Codex, 1ª sessão | Final, 1ª sessão | Codex, 2ª sessão | Final, 2ª sessão |
|---|---|---|---|---|
| FPS médio | 79,2 | **86,7** (+9,5%) | 84,3 | **92,1** (+9,3%) |
| Trecho pesado (190–215 s) | 43,6 | **47,1** | 49,5 | **57,5** (+16%) |
| Quadros acima de 40 ms | 81 | **23** | 6 | 6 |
| Quadros acima de 100 ms | 38 | **12** | 1 | 1 |
| Pior quadro | 5.567 ms | 5.484 ms | 142 ms | **117 ms** |
| Pipelines gráficos nos workers / draws pulados / esperas | — | 271 / 669 / 100 | — | 2 / 4 / 0 |
| Warm-up do boot | serial | 2.442 em 24,5 s (cache do driver frio) | serial | 2.841 em 0,95 s |

A parada de ~5,5 s da 1ª sessão é a rajada de ~90 pipelines de compute na troca de cena do demo:
continua nos dois builds (ver "O que não funcionou") e some na 2ª sessão.

## Segundo lote (14/10/2026, instalado como `batch2`)

| Mudança | Onde | Desligar |
|---|---|---|
| Registro de metadados (CMASK/FMASK/HTILE) só quando muda: uma geração incrementada a cada remoção diz quando o `emplace` seria inútil. | `texture_cache.cpp` `FindRenderTarget`/`FindDepthTarget` | `SOTC_META_MEMO=0` |
| Destruição de `VkImage`/views na thread `sotc:VkDeleter`, depois que a GPU terminou com elas; a contabilidade fica na CP. | `texture_cache.cpp` `ReclaimImage` | `SOTC_IMAGE_DELETER=0` |
| Último pipeline: chave idêntica à do draw anterior dispensa a busca. | `GetGraphicsPipeline` | `SOTC_PIPE_MEMO=0` |
| Memo de especialização também para VS com fetch shader: no acerto, re-parseia o fetch shader e compara cada V# (16 bytes), as mesmas leituras que a especialização faria. | `GetProgram` | `SOTC_SPEC_MEMO_VS=0` |
| **Gêmea estacionada:** o depth de meia resolução em `0x10511c0000` vira R32F (storage de um compute) e volta a depth duas vezes por quadro; antes cada troca alocava e destruía uma imagem (80–240 por segundo). Agora a imagem trocada fica estacionada e é reaproveitada na volta (uid novo, estado de imagem nova, mesma cópia de GPU). | `ResolveDepthOverlap`, `ParkImage`, `TakeParkedImage` | `SOTC_TWIN_REUSE=0` |
| Relógio do submit periódico lido a cada 8 comandos. | `FlushPeriodic` | `SOTC_FLUSH_CLOCK_EVERY=1` |
| HLE de replicate monta o padrão direto na memória de destino (sem vetor temporário). | `vk_shader_hle.cpp`, `TryReplaceFromCpuInPlace` | `SOTC_FILL_INPLACE=0` |

Diagnóstico novo: `SOTC images/s: overlap-recreate, expand/twin-reuse, new, stencil, deleted` na saída de
`SOTC_FPS=1`; `SOTC_TEX_CHURN=<n>` registra as n primeiras recriações por overlap.

Medição justa (`out/claude-ab/abp.sh`: o executável sempre no mesmo caminho, porque o cache do driver
NVIDIA é indexado pelo executável, e cópias do mesmo cache quente do port e do driver em cada rodada):
lote 1 contra o build anterior, trecho pesado +5%; `batch2` contra o lote 1, média 87,2 → 89,8 (+3%),
destruições de imagem ~150/s → ~0. O ruído entre rodadas no trecho pesado é grande (45 a 55 FPS na mesma
configuração), por isso cada comparação usa rodadas alternadas.

## Por que não copiar a preparação de draws do Bloodborne/GoW3

- A validação do bbport (`TryPreparedPipeline`) compara registradores e o buffer de user data "flattened",
  com um comentário dizendo que ele cobre tudo o que a especialização lê da memória. Não cobre: o código
  do fetch shader e os V# dos atributos de vértice são lidos por ponteiros da user data. Um draw preparado
  podia passar com formato de vértice velho. É a explicação mais provável da quebra registrada em
  [CAMINHO-B.md](CAMINHO-B.md) (`Unpack10_11_11`, programas inválidos durante o streaming).
- Mesmo corrigida, no SotC ela renderia +1–3%: os memos da CP já cobrem a maior parte do trabalho que as
  threads de preparação tiram, e a validação ainda precisa rodar o SRT walker na CP.
- O teto real está nos readbacks (~900 por segundo, 11–22% da CP) e no rastreamento de páginas (~12%):
  é o que impede dividir a CP em estágios como o Bloodborne faz. Próximo passo: reduzir esses custos e
  avaliar o Caminho B.


## Readbacks: o estágio 1 do "Track R" foi medido e não ajuda (14/10/2026, tarde)

Nova métrica: com `SOTC_CMD_STATS=1`, a linha `SOTC cp/frame` divide o tempo da CP por quadro em trabalho,
espera de readback, espera de flip (rótulo de vídeo) e ociosidade. O FPS do benchmark é arredondado pelo
vblank de 120 Hz (o jogo espera vblanks), então ganhos de CP quase não aparecem na média; essa linha não
sofre disso (`out/claude-ab/cmprb.py` resume as rodadas).

Base (demo de abertura, t ≥ 60 s): ~11,8 ms por quadro = trabalho ~9,1 + espera de readback ~2,0 +
espera de flip ~0,6. Os readbacks (~3,5 reais e ~5 duplicados por quadro) vêm da rejeição por oclusor na GPU
(`XPRRunFragmentCullShader*`), lida por jobs de culling da CPU no mesmo quadro.

Testado com ABAB, todos ficam desligados por padrão:

| Variável | O que faz | Resultado |
|---|---|---|
| `SOTC_RB_FENCE_FLUSH=1` | envia o buffer de comandos no rótulo que libera o jogo | finish 2,99 → 1,45 por quadro, espera −10%, FPS igual |
| `SOTC_RB_EARLY_US=N` | envia logo depois do draw que escreve a janela lida | idem |
| `SOTC_RB_EAGER=1` (2 = verificação) | copia a janela logo atrás do escritor | verificação: 0 diferenças; espera −0,27 ms, mas +0,72 ms de trabalho (envios extras): pior |
| `SOTC_RB_DEDUP=1` | threads do jogo que pedem a mesma janela esperam a primeira | duplicados 4,9 → 0,15 por quadro, FPS igual |
| `SOTC_RB_ASYNC=1` + `SOTC_RB_DEDUP=1` | a thread do jogo espera a GPU, a CP segue | espera da CP 1,94 → 0,47 ms, mas a CP fica 1,41 ms ociosa |
| `+UseOccluderRejection=0` (cvar) | — | nenhum efeito nos readbacks |

Conclusão: a espera não é a CP drenando trabalho. É a GPU (clock máximo, 40–55% de uso) executando, em ordem,
o trabalho pesado enviado antes da sincronização, e essa latência está no caminho crítico do jogo: tirar a
espera da CP só a transforma em espera do jogo. Os estágios 2–5 do plano (`out/claude-plans/track-r-plan.md`)
atacavam o mesmo ponto e não foram feitos. O que reduz essa espera é a GPU chegar antes ao escritor
(menos trabalho de GPU por quadro) — e o resto da CP (~9 ms) é trabalho por draw.

Também: `SOTC_PROTECT_FAST` (padrão ligado) pula o conjunto de intervalos e o mutex das pilhas do jogo quando
a faixa protegida está fora delas; `UpdatePageWatchersForRegion` destrava as páginas pelos ponteiros que
travou, sem nova busca.

## DMA ligado por padrão (0.56/0.57): quanto custa e por quê (14/10/2026)

ABAB na 0.57: DMA ligado 85,7–86,0 FPS (CP 10,0–10,2 ms de trabalho por quadro), desligado 91,7–92,2 FPS
(9,46 ms). Com DMA, `SynchronizeDmaBuffers` roda 2× por quadro e percorre e envia **toda** a memória residente
que a CPU sujou: envios 38 → 98 por quadro, 7 → 18 MB por quadro, faults de escrita 7,9 → 14,4 mil/s. No profile
da CP são 7,5% só nessa função. O Bloodborne e o GoW3 deixam o DMA desligado.

`SOTC_DMA_DIAG=1` (com cache de shaders vazio, para recompilar) mostra quais leituras caem no DMA e de onde vem o
endereço. São 4 shaders de compute, todos lendo `P + índice × passo + campo`, com `P` vindo do SRT:

| Shader | Passo | Índice | Observação |
|---|---|---|---|
| `0xfb974c0e` | 0xe0 | contagem em `P+0` menos laço/bits de uma máscara na memória compartilhada | sincroniza todo quadro |
| `0x57bd9f27` | 0x70 | valor lido da memória compartilhada | sincroniza todo quadro |
| `0x9064727d`, `0xcc4d91f3` | 0x10 | 0 ou 1 | a janela dinâmica cobriria, se `SimplifyReadConstAddressAdd` olhasse através do `ReadFirstLane` |

As tabelas ficam no heap grande do jogo (mapeamento de ~469 MB). Testado e neutro (variáveis, desligadas):
`SOTC_DMA_SYNC_MEMO` (nunca pula: o jogo sempre escreve algo entre duas passadas), `SOTC_DMA_BATCH` (memcpy em
paralelo), `SOTC_DMA_HOT` (páginas mantidas graváveis). Solução exata que sobra: as leituras DMA irem direto à
memória do host importada (`VK_EXT_external_memory_host`), usando a arena só nas faixas modificadas pela GPU — aí
nenhuma sincronização é necessária. Não foi feita.

## Intervalo de envio 500 → 250 µs (+4%) e o que não ajudou (14/10/2026, noite)

Com o medidor de GPU (`SOTC_GPU_TIME=1`, só o scheduler da CP): a GPU trabalha ~4,5–5 ms de ~12,8 ms por
quadro e recebe só ~12 envios por quadro. Em cada readback a CP espera a GPU terminar o último bloco enviado,
então blocos menores encurtam essa espera. Varredura de `SOTC_FLUSH_US`, dois ciclos:

| Intervalo | FPS | CP por quadro | espera de readback |
|---|---|---|---|
| 500 µs (antigo padrão) | 87,3 / 87,5 | 12,51 / 12,47 ms | 2,25 ms |
| **250 µs (novo padrão)** | **91,1 / 90,6** | **12,00 / 12,19 ms** | 1,8 ms |
| 125 µs | 91,3 / 91,1 | 12,31 / 12,41 ms | 1,4 ms (os envios extras custam o que economizam) |

Não ajudaram (ficam desligados):
- `SOTC_FLUSH_IDLE_US=N` (enviar assim que a GPU termina tudo): ela fica ociosa quase sempre, dispara 80–135
  envios por quadro, 80–81 FPS.
- `SOTC_SEL_FAST=1` (reaproveitar a seleção de pipeline do draw anterior quando nenhum registrador relevante
  mudou — `Liverpool::sel_regs_seq`, `amdgpu/sotc_sel_regs.h`): o modo 2 confere com a seleção completa (0
  diferenças, 0 escritas de registrador não percebidas, ~60% reaproveitado) e duas revisões independentes não
  acharam falha, mas o ganho ficou abaixo do ruído: os draws aceitos já eram acertos do SpecMemo.

Estudo do DrawPrep do GoW3 para o SotC (`out/claude-plans/drawprep-*.md`): teto realista 4,5–7,5% da CP, porque
o SpecMemo já tira a maior parte do que os workers do GoW3 tiravam e o walker precisa continuar na CP; a
validação do GoW3/Bloodborne não confere o código do fetch shader nem os V# (o bug que quebrou o SotC); uma
thread auxiliar que lê página protegida hoje abriria a página para todas as threads (`helper_read_fault`).

## Multi-thread: seleção de pipeline preparada numa thread auxiliar (`SOTC_DRAW_PREP`, 14/10/2026, noite)

A seleção de pipeline de cada draw (chave, runtime info, especialização, permutação) agora pode ser preparada
por uma thread auxiliar antes de a CP chegar ao draw. A CP só confere e aceita. Peças, em ordem:

1. **Pré-requisitos de segurança.** O walker do SRT não altera mais o próprio código quando falha
   (`SrtWalkerSignalHandler` emula a leitura), então outra thread pode rodar o mesmo walker. Em `src/probe.c`,
   uma falha de memória do jogo numa thread com `runtime_fault_recover` volta ao ponto de recuperação antes de
   qualquer outro tratamento: não abre página, não dispara readback.
2. **Scanner** (`amdgpu/sotc_draw_scan.*`, thread `sotc:DrawScan`): copia cada command buffer gráfico no
   envio, numerado sob a trava da fila, e reaplica as escritas de registrador (`ApplyStatePacket`, o espelho
   das escritas de `ProcessGraphics`) numa cópia própria do banco de registradores. Um hash de histórico de
   todos os pacotes de estado, calculado igual pelos dois lados, prova a cada draw que o scanner viu exatamente
   o que a CP aplicou. `SOTC_DRAW_SCAN=2` compara o banco inteiro a cada 128 draws: ~400 mil comparações no
   demo, 0 diferenças.
3. **Preparação** (`renderer_vulkan/vk_draw_prep.*`): no scanner, para cada draw de vértice + pixel com
   entrada de vértice dinâmica, roda o mesmo código da CP (as passadas da chave e as partes de vértice/pixel
   do `BuildRuntimeInfo` foram extraídas em funções compartilhadas), o walker com leituras pelo backing e
   protegido contra falha, o fetch shader numa cópia (parser que devolve falso em vez de abortar), os V# pelo
   backing, a especialização e a busca da permutação. Nunca compila, nunca dispara readback, nunca pega trava
   que a CP espera. Programas e permutações chegam à thread por uma lista sem trava.
4. **Validação na CP** (`PipelineCache::TryPreparedSelection`): hash do histórico igual no draw, parâmetros
   indiretos, hash do código de cada estágio, bindings iniciais, o walker refeito na CP com os dwords que a
   especialização lê comparados (os sharps; constantes por draw mudam depois da leitura do scanner e não
   mudam a permutação — `SOTC_PREP_FULL_FLAT=1` compara tudo), fetch shader refeito e cada V# relido.

Resultado no demo: 100% das propostas aceitas; `SOTC_DRAW_PREP=2` (prepara, faz a seleção completa e compara)
0 diferenças; 0 falhas na thread auxiliar. A/B intercalado, mesmo executável:

| Rodada | FPS médio | Trecho pesado | Trabalho da CP |
|---|---|---|---|
| desligado | 93,8 / 95,5 / 95,7 | 53,4 / 57,2 / 60,2 | 9,55 / 9,07 / 9,08 ms |
| `SOTC_DRAW_PREP=1` | 95,7 / 100,9 / 98,8 | 54,0 / 68,6 / 63,2 | 8,97 / 8,09 / 8,41 ms |

Três pares: trabalho da CP −8%, FPS médio +3,6%, trecho pesado +10%.

Rota com save e carregamento (menus, streaming, cutscene): verificação limpa (0 diferenças, ~30 mil comparações
do banco de registradores), 94–100% aceitos; desligado contra ligado: p99 17,2 / 17,1 ms, quadros acima de
33 ms 11 / 11.

**Ligado por padrão** (`SOTC_DRAW_PREP` padrão 1; `0` desliga; opção no lançador). Na primeira rodada com
ele ligado, o jogo parou num `UNREACHABLE` do `MapNumberConversion` (`data_fmt=8`) **dentro da thread
auxiliar**: ela especializou um V# ainda não escrito pelo jogo. Uma assertiva só prova que o dado especulativo
estava errado, então agora a thread marca o trabalho especulativo (`g_bb_speculative_asserts` em
`shim/common/assert.cpp`) e uma assertiva ali volta ao ponto de recuperação e descarta a proposta, igual a uma
falha de memória. Fora dessa janela nada mudou: assertiva na CP continua parando o jogo. Três rodadas
completas depois disso: nenhuma parada, 100% aceitos, 0 diferenças, 0 assertivas na auxiliar (97,8 / 99,4 /
101,3 FPS). A linha `SOTC prep/frame` conta falhas e assertivas recuperadas.

## Readbacks relaxados (o padrão do Bloodborne/GoW3) medidos no SotC (15/10/2026)

`BB_READBACKS` 2 (exato, padrão), 1 (relaxado: a CPU não espera a GPU ao ler memória escrita pela GPU) e
0 (desligado), mesmo executável (`r057v`) e mesmo cache, demo de abertura:

| Modo | FPS médio | Trecho pesado | Trabalho da CP | Draws indiretos por quadro | Quadros > 33 ms | Pior quadro |
|---|---|---|---|---|---|---|
| exato | 98,9 | 65,6 | 8,47 ms | ~1.010 | 41 | 1,1 s |
| relaxado | 76,2 | 44,7 | 13,47 ms | ~2.525 | 761 | 5,8 s |
| desligado | 28,9 | 5,0 | 94,1 ms | ~11.500 | 1.842 | 2,8 s |

Rota com save e carregamento: exato 107,8 FPS e 11 quadros > 33 ms; relaxado 83,8 FPS e 531.

O culling do próprio jogo lê na CPU o resultado da rejeição de oclusores que a GPU calcula. Com dado velho, o
jogo acha que tudo está visível e manda 2,5 vezes mais draws (11 vezes, desligado). Relaxado não dá crash e as
quatro capturas do demo saíram normais, mas jogando aparecem defeitos em elementos da tela (relato do usuário;
uma captura a cada 60 s não pega defeito que pisca); desligado corrompe a imagem. No SotC os readbacks exatos são o que faz o culling
funcionar: relaxar custa 23% de FPS e multiplica as travadas.

## Barreiras de imagem: o modelo e a sincronização do próprio jogo (15/10/2026)

**Sintoma.** Nos quadros pesados do demo (~7.500 draws), a GPU era parada ~460 vezes por quadro: cada
barreira dentro de um render pass o encerra, e a GPU esvazia o trabalho em voo. Isso alongava as esperas de
readback (a CP espera a GPU terminar o que já recebeu).

**Diagnóstico** (`SOTC_BREAK_STATS=1`: quem fecha render passes; `SOTC_BARRIER_TRACE=<flips>`: histograma das
barreiras de imagem a partir daquele quadro; linha `SOTC barrier breaks/frame`, com o motivo):

1. **Depth/stencil rastreado como um bloco.** Um draw que escreve stencil sobre um depth só lido escolhia
   `DepthReadOnlyStencilAttachmentOptimal`; o draw seguinte, amostrando o depth, levava a imagem inteira a
   `DepthStencilReadOnlyOptimal`, e o próximo de volta. ~3.000 draws por quadro pesado escrevem stencil.
2. **Imagens de storage (UAV) em draws seguidos sem nenhuma sincronização do jogo.** Três imagens 1920x1080
   escritas por pixel shaders recebiam ~630 barreiras por quadro. Contando os comandos de sincronização do jogo
   (`SOTC sync/frame`): end-of-pipe ~290, `WAIT_REG_MEM` ~237, `ACQUIRE_MEM` ~86, cache flush ~31 por quadro, e
   **nenhum** entre esses draws. Na GPU do PS4 esses acessos não são ordenados (draws se sobrepõem, escritas UAV
   não têm ordem); quem precisa de ordem a pede com esses comandos, que o emulador ignorava.

**Correção na origem** (`SOTC_ASPECT_BARRIERS=0` volta ao modelo anterior inteiro; `SOTC_GAME_SYNC=0` só a
parte 3):

1. Depth e stencil têm estado próprio (layout, acessos, sub-recursos), como o Vulkan os modela
   (`separateDepthStencilLayouts`): `Image::SplitAspects`, `GetBarriers(..., aspects)`, `LayoutOf`. O alvo de
   profundidade declara por aspecto o que o draw faz: depth escrito com `depth_write_enable` ou limpeza; stencil
   escrito quando o estado de stencil pode escrever (`WritesStencil`: teste ligado, máscara de escrita não nula e
   operação diferente de Keep, ou limpeza). Cada aspecto fica no layout em que está se ele permite o uso
   (`TargetLayout`, `SampledLayout`); só escrita depois de amostragem, ou o inverso, o move. O render pass recebe
   um layout para o depth e outro para o stencil.
2. Regra de barreira (`NeedsBarrier`, `image.cpp`): mudança de layout, ou escrita junto de um acesso que não é de
   attachment. Leitura depois de leitura não precisa; acessos ao mesmo attachment no mesmo layout são ordenados
   entre si (o rastreamento sempre assumiu isso). Antes, qualquer mudança da máscara de acesso custava uma
   barreira.
3. Acessos de shader a uma imagem em `General` sem comando de sincronização do jogo desde a última escrita não
   recebem barreira (`Image::State::write_epoch` contra `SotcStats::sync_epoch`, avançado pelo
   `Liverpool::NoteSync` em partial flushes, cache flushes, EOS/EOP, `ACQUIRE_MEM`, `WAIT_REG_MEM`, também na
   fila de compute). Uma imagem em `General` amostrada fica em `General`.

**Resultado** (`r058n`, mesmo executável, 3 pares ABAB, `SOTC_ASPECT_BARRIERS=0` contra o padrão):

| | Modelo antigo | Modelo novo |
|---|---|---|
| CP por comando, trecho pesado | 2,66 / 2,54 / 2,80 µs | 2,49 / 2,46 / 2,42 µs (−8%) |
| CP por comando, demo todo | 2,83 / 2,74 / 2,86 µs | 2,75 / 2,69 / 2,78 µs (−2,5%) |
| Espera de readback, trecho pesado | 1,82 / 1,88 / 1,64 ms | 0,80 / 0,98 / 1,14 ms (−45%) |
| Render pass quebrados por quadro (fim do demo) | 327 / 222 / 223 | 84 / 72 / 72 |
| FPS médio | 96,0 / 95,3 / 94,0 | 97,8 / 97,3 / 96,2 (+2,1%) |
| FPS no trecho pesado | 58,7 / 56,6 / 53,3 | 59,9 / 61,1 / 53,3 (+3,4%) |

Rota com save e carregamento: 107,9 FPS, sem erro. Capturas do demo corretas (iluminação, sombras, névoa,
vegetação). Uma primeira versão (`r058m`) relia o layout e gravava o estado da imagem a cada textura de cada
draw: o ganho de GPU vinha comido por +3,5% de CP; o layout escolhido agora vai direto para o descritor e o
attachment, e o estado só é gravado quando muda.

## Memos da CP por desenho (15/10/2026)

Cada um guarda o resultado de uma função das suas entradas e tem modo de verificação que refaz o caminho normal
e compara (0 diferenças no demo e na rota de salvar/carregar):

- O conjunto de imagens pertence à CP: imagens só são criadas e destruídas na thread de comandos. As duas
  outras origens passaram para lá: `Rasterizer::UnmapMemory` manda o `UnmapMemory` do cache de texturas para a
  CP (`SendCommand<true>`, só quando há imagem no intervalo) e o `VideoOutDriver::RegisterBuffers` manda o
  registro das superfícies de vídeo. Com isso o acerto do memo do `FindImage` é uma leitura simples, sem o
  mutex (o release custava 3-4% da CP nos quadros pesados, contendido pelas faltas de escrita do jogo em páginas
  de imagem) e sem cerca. `CheckImageSetOwner` registra no log qualquer criação ou destruição fora da CP
  (nenhuma no demo nem na rota). A primeira versão (`SOTC_FIND_LOCKFREE`, `r058n`) deixava outras threads
  mexerem no conjunto e protegia o leitor com uma cerca `seq_cst` por binding: empatou com o instalado.
- `SOTC_TEX_BIND_MEMO` (`=2` verifica): T# + bits do recurso + índice → descrição e imagem do `FindImage`,
  válido enquanto o conjunto de imagens não muda; só correspondências perfeitas (sem resolução de sobreposição,
  que tem efeitos colaterais).
- `SOTC_RT_MEMO` (`=2` verifica): alvos de cor e profundidade feitos dos mesmos registradores do draw anterior.
- `SOTC_STENCIL_MEMO`: a imagem de stencil do alvo de profundidade, sem percorrer a tabela de páginas por draw.
- `SOTC_ACCESS_HINT`: as listas de intervalos de acesso a buffers guardam onde entrou o último intervalo
  (coberto ou estendido sem mover o vetor); teste aleatório contra referência passou.

Medidos juntos (`cpheavy.py`: trabalho da CP por comando, que não depende de quanto o demo desenhou em cada
rodada), mesmo executável, 3 pares: −7% por comando no demo todo, −11% no trecho pesado; FPS +1,6%.

**Instalado** (`r058o`: modelo de barreiras + memos + conjunto de imagens da CP) contra o anterior (`r057v`), 3
pares ABAB (`bat26b`), 0 diferenças nos memos, sem erro:

| | `r057v` | `r058o` |
|---|---|---|
| CP por comando, demo todo | 2,80 / 2,78 / 2,84 µs | 2,63 / 2,66 / 2,64 µs (−5,8%) |
| CP por comando, trecho pesado | 2,64 / 2,61 / 2,68 µs | 2,38 / 2,56 / 2,52 µs (−5,9%) |
| Espera de readback, trecho pesado | 1,66 / 1,70 / 1,90 ms | 0,86 / 0,96 / 0,93 ms (−48%) |
| Render pass quebrados por quadro (fim do demo) | 377 / 399 / 328 | 106 / 86 / 113 |
| FPS médio | 97,6 / 97,9 / 96,3 | 101,4 / 99,4 / 101,3 (+3,5%) |
| FPS no trecho pesado | 65,8 / 64,0 / 58,1 | 67,1 / 59,7 / 73,0 (+6%, ruidoso) |

**Listas de acesso a buffers** (`r058r`, no código, não instalado sozinho): os registros por buffer das
barreiras (`Runtime::resources`) eram destruídos a cada flush, e as listas eram alocadas e cresciam de novo na
janela seguinte; agora ficam num pool (`SOTC_ACCESS_POOL=0` volta ao antigo). E cada draw registrava de novo os
intervalos dos seus buffers, quase sempre já registrados na mesma janela (as listas só crescem até o flush):
uma tabela de 256 entradas os pula (`SOTC_ACCESS_DEDUP=0`), ~78% das ~45 mil adições por quadro. No perfil,
`IntervalList::Add` caiu de 685 para 377 amostras e `RtlAllocateHeap` de 1,13% para 0,51%; no A/B (`bat27`, 3
pares contra `r058o`) a CP por comando ficou em −1,3% no demo todo e 0% no trecho pesado, FPS igual: dentro do
ruído.

**Cuidado com o perfil por amostragem** (`SOTC_PROFILE`, `SuspendThread` a cada ~1 ms): ele concentra
amostras em instruções isoladas. `Image::FindView` aparecia com 5,7% da CP, 430 amostras numa única carga de 8
bytes; um memo que eliminava a memória percorrida não mudou nada, e contadores de ciclos (rdtsc) mediram ~40
ciclos por chamada, rdtsc incluído (1-2% da CP). Totais por função servem; uma "instrução quente" sozinha não é
prova. `SOTC_PHASE_TIME=1` mede em ciclos as fases de cada draw e dispatch (linha "SOTC phase ms/frame").

**Estado de render reaproveitado** (`r058w`). Medido em ciclos (`SOTC_PHASE_TIME`), o `BeginRendering`
custava ~2,1 ms por quadro pesado (~300 ns por draw) para chegar quase sempre ao mesmo estado do draw anterior:
busca dos alvos, metadados de fast clear (duas buscas em hash por alvo), transições de layout, montagem dos
attachments. A causa é derivar tudo de novo a cada draw sem saber o que mudou. Agora:

- `TextureCache::TakeMetaCleared` junta `IsMetaCleared` e `TouchMeta(false)` e lembra, por alvo, um slice já
  visto não limpo; vale enquanto nenhum slice de nenhuma superfície fica limpo (`meta_clear_generation`, que
  avança em `ClearMeta`, `TouchMeta(true)` e em superfícies novas). `SOTC_META_CLEAR_MEMO=0` sempre busca.
- `Rasterizer::RenderStateMemo` guarda o último estado derivado sem barreira e de onde ele veio: imagens,
  backings e a versão do estado de cada um (`BackingImage::state_version`, que avança em `TransitAspect` e
  `SetBackingSamples`), gerações do conjunto de imagens e dos metadados, e os registradores de profundidade e
  stencil que o `BeginRendering` lê além dos memos de alvo. Com tudo igual, alvos vindos dos memos, nenhum alvo
  de cor amostrado no draw e nenhum a reprocurar, o estado é o mesmo e sem barreiras.
  `SOTC_RENDER_STATE_MEMO=0` sempre deriva; `=2` deriva também e compara: 0 diferenças no demo e na rota de
  salvar/carregar.
- `TextureCache::SamplerMemo`: S# e flag de profundidade (a chave do `GetSampler`) para o sampler, enquanto
  nenhum é destruído; poupa hash, mutex e busca por sampler de cada draw (`SOTC_SAMPLER_MEMO=0`).

A verificação achou um bug meu na `r058o` instalada: `RenderState state;` não era inicializado, e os campos que
acrescentei ao `RenderAttachment` (o layout de stencil, que um attachment de cor não preenche, e a palavra de
preenchimento) ficavam com lixo da pilha. O scheduler compara estados byte a byte para manter o render pass, então
lixo diferente fechava e reabria passes à toa. Agora o estado nasce zerado.

`BeginRendering` caiu de ~2,0 para ~0,7 ms por quadro pesado.

**Instalado** (`r058w`, com as listas de acesso acima) contra `r058o`, 3 pares ABAB (`bat30`), sem erro; a
`r058w` ganhou nos três pares:

| | `r058o` | `r058w` |
|---|---|---|
| CP por comando, demo todo | 2,63 / 2,72 / 2,77 µs | 2,43 / 2,52 / 2,43 µs (−9,2%) |
| CP por comando, trecho pesado | 2,37 / 2,49 / 2,66 µs | 2,16 / 2,25 / 2,19 µs (−12,1%) |
| Trabalho da CP por quadro | 8,74 / 8,21 / 8,71 ms | 7,92 / 7,97 / 8,03 ms (−6,8%) |
| FPS médio | 98,6 / 101,4 / 100,1 | 102,2 / 102,3 / 101,4 (+2,0%) |

**Ligações de buffers e texturas** (`r059b`). Medido em ciclos num quadro pesado: `ObtainBuffer` 1,7 ms,
consulta das texturas 1,3 ms, views e layouts 1,2 ms, checagens de intervalo dos buffers 0,8 ms.

- Buffers formatados lidos: cada ligação perguntava se uma superfície de metadados ou uma imagem começa no
  endereço (busca em hash e varredura da tabela de imagens) para re-tile da imagem no buffer. Contado: ~22 mil
  por quadro pesado, nenhuma achou nada. A resposta só muda com o conjunto de imagens e de metadados:
  `BufferCache::TexelSyncMemo` guarda os intervalos sem nada, enquanto as gerações não mudam (só quando nenhuma
  imagem começa ali, independentemente das flags que o `FindImageFromRange` filtra). `SOTC_TEXEL_SYNC_MEMO=0`
  sempre busca.
- Texturas: o T# era validado (busca no mapa de memória e duas buscas em enum) antes de consultar o memo de
  ligação; um T# que o memo conhece passou nessas checagens e ainda passa (os formatos são os bits dele, e o
  endereço está numa imagem do conjunto inalterado; desmapear desregistra a imagem). O memo agora vem antes.

**Instalado** (`r059b`) contra `r058w`, 3 pares ABAB (`bat31`), verificação dos memos de textura e de estado de
render sem diferenças no demo e na rota, sem erro: CP por comando no trecho pesado 2,39 / 2,38 / 2,21 →
2,19 / 2,22 / 2,11 µs (−6,6%, os três pares), no demo todo 2,49 → 2,43 µs (−2,2%); FPS médio igual (102,7 /
102,4, ruído).

Depois (`r059e`, não instalado): o memo por endereço quase não acerta (a maioria desses buffers é dado por draw
num anel, endereço novo a cada vez). A checagem exata que não depende do endereço: uma imagem que começa em A
está registrada no grânulo de 1 MiB de A (`TextureCache::MayHaveImages`), e as superfícies de metadados ganharam
uma contagem por grânulo (`meta_granules`, mantida por `EmplaceMeta`/`EraseMeta`; `IsMeta` responde sem busca
fora deles). `ObtainBuffer` caiu ~0,2-0,3 ms por quadro pesado, mas no A/B (`bat32`, 3 pares contra `r059b`) não
apareceu: CP por comando igual no trecho pesado (2,20 / 2,21 µs), FPS 103,4 / 101,7 (ruído). O que resta do
`ObtainBuffer` (~1,2-1,4 ms) é trabalho real: ~50-60 uploads por quadro (6-10 MB) de memória que o jogo reescreve
todo quadro, e a reproteção das páginas depois de cada um. A correção de raiz disso é a GPU ler essa memória
direto (memória do host importada), não um memo.

Testados e descartados no mesmo dia: espera da GPU com spin ou marcador (`SOTC_WAIT_SPIN_US`,
`SOTC_WAIT_MARKER`; a espera é a GPU executando, não o custo de acordar) e prioridade da thread da CP
(`SOTC_CP_PRIORITY`): neutros.

## Registro de escritas e dados da thread auxiliar (16/10/2026)

A CP refaz o walker de SRT de cada estágio que a thread auxiliar (`SOTC_DRAW_PREP`) já leu, para provar que a
memória não mudou entre as duas leituras: 1,9 ms de 20,5 ms num quadro pesado. A ideia era trocar essa
releitura por uma conferência barata.

- `video_core/sotc_write_ledger.h` (`SotcLedger`): cada escrita na memória do jogo que o emulador faz ou vê
  recebe o próximo número de uma sequência, gravado nas páginas de 4 KiB que tocou: `NoteHostWrite` (agora real:
  `WRITE_DATA`, `DumpConstRam`, readbacks, falhas de escrita do jogo em páginas rastreadas), `TryWriteBacking`
  (rótulos, imagens, HLE por cópia), o HLE no lugar, a cópia por CPU do `CopyBuffer`, semáforos, `RELEASE_MEM`,
  GDS. Faixas marcadas para escrita da GPU (tipos 7 e 8) não entram: eram ~1,2 milhão de páginas por quadro sem
  nenhuma invalidação, e a conferência olha o estado GPU-modificado das páginas em vez disso.
- O walker gerado anota o endereço de cada leitura num log do contexto (`SrtWalkerContext::read_log`; a cópia de
  janelas e `RefreshSharpTables` também); o prólogo marca `read_log_supported`, para um walker antigo vindo do
  cache não passar por "não leu nada". Versão do cache de shaders 34/23 (o código do walker vai para o cache).
- A auxiliar guarda as páginas lidas e o número da sequência antes de ler; a CP aceita os dados dela se nenhuma
  página foi escrita depois e nenhuma está GPU-modificada (`SOTC_PREP_FLAT`; `=2` verifica).

Medido (`SOTC_LEDGER_CHECK=1`, `SOTC_PREP_FLAT=2`): ~3,6-4 páginas por estágio; a conferência aceita ~50-60% dos
estágios. As invalidações vêm do HLE de replicate (tipo 12, ~3 mil por quadro: ele preenche tabelas que o walker
lê, depois da leitura da auxiliar) e de escritas do jogo pegas pela proteção (tipo 9, ~500-1.600). **Mas a
verificação achou ~1.000 estágios aceitos com dado diferente** no demo e na rota: sempre um dword (posição 27 de
33, três shaders) que alterna `0x91000`/`0x191000` a cada quadro, reescrito pelo jogo numa página que nada
protege entre a leitura da auxiliar e a da CP. A regra do PS4 (o jogo não mexe nos dados de um desenho já
enviado) não vale aqui, ou a CP lê esses dados tarde demais; de todo modo, sem ver essa escrita a conferência
não é segura. `SOTC_PREP_FLAT` ficou desligado por padrão.

Para seguir por aqui seria preciso proteger contra escrita as páginas que o walker lê (um mecanismo novo de
vigia no page manager) e reler só os dwords mudados (o log guardar valor e posição no buffer). Teto medido:
~60% do walker, ~4% da CP no trecho pesado.

**Retirado do código** no mesmo dia: o registro custava operações atômicas em toda escrita notada, e o log
do walker mudava o código guardado no cache de shaders (versão 34/23, que obrigaria a retraduzir tudo). O
patch completo ficou em `out/claude-ab/step1-ledger.patch`; as versões do cache voltaram a 33/22.

## Conjunto de texturas por estágio (16/10/2026)

Antes de mexer, a CPU de cada thread foi medida no trecho pesado do demo (`GetThreadTimes` por thread,
r059b): a CP trabalha 92% do tempo (13,8 ms por quadro a 66 FPS) e é o limite; no resto do demo, 68%. A
thread que grava os comandos Vulkan (`shadPS4:GpuRecorder`, `vk_recorder.cpp`) aparece com 97%, mas gira
250 µs entre lotes: o trabalho real dela é 3,5-4 ms por quadro pesado (`SOTC_PHASE_TIME=1`, linha
"recorder busy"; `SOTC_RECORDER_SPIN_US` muda a espera para testes). Upscaling não ajuda: a GPU já fica
metade do tempo parada.

Muitos desenhos ligam exatamente as mesmas texturas e samplers (o mesmo material). `BindTextures` montava
tudo de novo a cada estágio: busca de cada imagem, view, layout, transição e sampler.

- `Rasterizer::TryTextureBlock` (`SOTC_TEX_BLOCK_MEMO`, `=2` liga do jeito normal também e compara): a
  chave são os bits de recurso de cada imagem e sampler do estágio, os T#/S# que eles leem e a base de
  cor de borda. O resultado (descritores, imagens ligadas, bindings) vale enquanto o conjunto de imagens é
  o mesmo, cada imagem tem o mesmo backing na mesma `state_version`, a mesma ligação a uma imagem de
  profundidade, nada a enviar, e nenhuma flag do desenho atual (alvo, storage, rebind) a manda por outro
  caminho. Estágios com storage ou arrays de descritores ficam no caminho normal.
- Só se guarda um conjunto que ligado de novo não muda estado nem pede barreira: os sub-recursos do range
  da view, no aspecto que a view lê, sem escrita registrada. Duas descobertas no caminho: o stencil de uma
  imagem de profundidade amostrada pelo depth é escrito pelo próprio alvo do desenho (só o aspecto lido
  conta), e as texturas de streaming deixam os mips finos em `TransferDst` até o jogo liberar as views para
  eles (só o range da view conta).
- `TransitAspect`: uma imagem rastreada por sub-recurso cujo range já está onde o acesso precisa volta sem
  mudar nada (antes percorria os sub-recursos e mudava a versão a cada ligação); quando todos os
  sub-recursos ficam iguais, um estado volta a valer pela imagem inteira.

Verificação (`=2`, também `SOTC_RENDER_STATE_MEMO=2` e `SOTC_TEX_BIND_MEMO=2`) no demo e na rota de
salvar/carregar: 0 diferenças; ~90% dos conjuntos reaproveitados no trecho pesado (~3-4 mil por quadro).
Medido em ciclos no trecho pesado: texturas 2,87 → 2,13 ms por quadro (o que sobra é quase todo de
dispatches com imagens de storage).

**Instalado** (`r061g`) contra `r059b`, 3 pares ABAB (`bat36`), a `r061g` ganhou os três no demo todo:

| | `r059b` | `r061g` |
|---|---|---|
| CP por comando, demo todo | 2,46 / 2,46 / 2,44 µs | 2,41 / 2,37 / 2,25 µs (−4,5%) |
| CP por comando, trecho pesado | 2,19 / 2,15 / 2,20 µs | 2,20 / 2,09 / 2,00 µs (−3,5%) |
| Trabalho da CP por quadro | 7,82 / 8,02 / 8,13 ms | 7,58 / 7,73 / 7,37 ms (−5,4%) |
| FPS médio | 103,1 / 102,0 / 100,7 | 104,2 / 103,4 / 105,0 (+2,3%) |

Onde está o trecho pesado agora (ms por quadro, `r061g`): seleção de pipeline 2,7 (walker 1,3), buffers
3,5 (`ObtainBuffer` 1,6, checagem de range 0,9, barreira+descritor 0,6), texturas 2,1, gravação 1,7,
dispatches 3,0 (pipeline+HLE 1,0, bindings 1,8), alvos+`BeginRendering` 1,0, depois do desenho 0,7.

## Compilação guiada por perfil (PGO, 16/10/2026)

Do PR #59 (Oussama268, que mediu +14% numa RTX 3060 de notebook), aplicado só na árvore local para medir
(fica no #59, não neste PR): o `gpu/CMakeLists.txt` ganhou os ramos
do Clang (`BB_PGO=generate`: `-fprofile-generate` com contadores atômicos; `BB_PGO=use`:
`-fprofile-use=pgo/bbgpu.profdata`, funções mudadas desde o perfil compiladas sem ele) e o `bbgpu.cpp`
grava o perfil a cada 30 s pelo runtime do compiler-rt (o jogo termina por `_Exit`). Nenhuma mudança de
comportamento: o mesmo código, com o compilador sabendo quais ramos e funções são quentes.

`out/claude-ab/pgo.sh train` compila o executável instrumentado, roda o demo e a rota de
salvar/carregar com ele (o instrumentado roda a ~16 FPS no trecho pesado), junta o perfil e compila
`out/gpu-pgo`; sem `train`, só recompila com o perfil existente (depois de mudar o código; retreinar
depois de mudanças grandes).

**Instalado** (`r061g-pgo`: o código da `r061g` compilado com o perfil) contra `r061g`, 3 pares ABAB no
demo e um par na rota (`bat37`), sem erro:

| | `r061g` | `r061g-pgo` |
|---|---|---|
| CP por comando, demo todo | 2,40 / 2,53 / 2,41 µs | 2,08 / 2,03 / 2,05 µs (−16%) |
| CP por comando, trecho pesado | 2,11 / 2,34 / 2,18 µs | 1,80 / 1,76 / 1,83 µs (−19%) |
| Trabalho da CP por quadro | 8,00 / 8,43 / 7,58 ms | 6,65 / 6,46 / 6,49 ms (−18%) |
| FPS médio | 101,4 / 99,5 / 104,2 | 107,3 / 108,2 / 107,9 (+6%) |
| FPS no trecho pesado | 68,3 / 62,8 / 71,7 | 78,7 / 81,8 / 80,2 (+19%) |

Na rota (que bate nos 120 Hz do teste, FPS 114,3 / 113,8) a CP por comando caiu de 2,58 para 2,23 µs
(−13%): o ganho não se limita ao demo usado no treino.

## Readbacks assíncronos: duas causas corrigidas, ainda neutro aqui (16/10/2026)

`SOTC_RB_ASYNC=1` (do PR #32; o PR #83 o liga por padrão): a CP grava a cópia e segue, e só a thread do
jogo que pediu os dados espera a GPU. Medido de novo sobre a `r061g-pgo`:

1. **Readbacks em dobro** (3,4 → 6,5 reais por quadro): as job threads do jogo pedem as mesmas janelas
   (os bitsets do culling) quase juntas. Esperando, a CP as atendia uma após a outra e as seguintes
   achavam a janela já lida (os ~5 noop por quadro); assíncronas, chegavam com a primeira cópia em voo e
   cada uma gravava a sua. Agora uma leitura cuja janela está numa readback em voo espera essa
   (`FindInflightWindow`, contagem "async joined" na linha `SOTC rb/frame`): ~3 por quadro, reais de volta
   a 3,5. A janela em voo tem todo byte modificado pela GPU que tinha na cópia, e uma escrita da GPU
   gravada depois a marca suja (o fim a lê de novo).
2. **A assertiva "protect region as write-only"** (a parada vista em 14/10): numa falha de escrita do jogo
   servida assim, a CP grava novas escritas da GPU naquelas páginas entre a cópia e o fim; o fim marcava a
   página como modificada pela CPU com ela ainda modificada pela GPU. Nenhuma proteção serve (o Windows
   não tem página só-escrita) e a leitura dos dados novos apagaria a escrita do jogo. Um detector
   (`ReportBothModified` em `RegionManager::UpdateStateAndProtection`, com a pilha) achou o caminho:
   `FinishAsyncReadback` → `MarkRegionAsCpuModified`. Agora `GiveToGuestWrite` lê de volta o que a GPU
   escreveu ali desde a cópia antes de entregar a página (contagem "write refreshes"), como o caminho que
   espera faz no mesmo ponto. O detector fica no código.

Resultado (`bat39`, `r062c-pgo`, 3 pares desligado/ligado + rota, sem parada nem estado inválido): espera
da CP por readback 1,3 → 0,4 ms por quadro, FPS médio 107,0 / 107,1, trecho pesado 82,5 / 78,7 (ruído),
CP por comando igual. O tempo que a CP deixa de esperar volta como trabalho de gerenciar as readbacks
(dois comandos e um submit por readback) e como espera da thread do jogo, que fica no caminho crítico do
quadro de qualquer jeito. Fica desligado por padrão aqui.

## A GPU ler direto a memória do jogo: viável no driver, não correto aqui (16/10/2026)

A maior fatia que sobra na CP é a ligação de buffers (~3 ms por quadro pesado), e dentro dela ~1,2 ms são os
uploads de 6-10 MB que o jogo reescreve todo quadro e a reproteção das páginas depois de cada um. No PS4 a
GPU lê a mesma memória que a CPU. Teste isolado (`hostimport.cpp`, fora do repositório) com o mesmo arranjo
do runtime (seção `CreateFileMapping2` `SEC_COMMIT`, vista `MapViewOfFile3`), RTX 5070: o driver importa a
vista com `VK_EXT_external_memory_host` em qualquer tamanho, até os 6 GB inteiros (tipos de memória
host-visible + coherent, alinhamento 4 KiB, ~12 ms por 64 MB, ~0,25 s para tudo), e a GPU a lê a ~50 GB/s.

**Não entra**, por correção: os rótulos de fim de pipe (`EventWriteEop`, `ReleaseMem`) são escritos quando a
CP lê o pacote, não quando a GPU termina (`SOTC_EOP_DEFER=1` os adia e corta o FPS pela metade). Hoje isso é
seguro porque os dados de cada desenho são copiados na tradução: o jogo pode reescrevê-los logo que vê o
rótulo. Lendo direto, a GPU leria na execução, depois do rótulo, possivelmente dados já do quadro seguinte. O
SotC reescreve dados de desenhos já enviados (o dword alternado que bloqueou o registro de escritas). Uma
variante exata seria fazer os uploads numa thread com prazo: todos prontos antes de a CP escrever o próximo
rótulo ou interrupção.

## PR #32 do repositório (Gabriell-os, desempenho e estabilidade em AMD) integrado (14/10/2026)

Merge de três vias por arquivo sobre o nosso trabalho (sem o `9dda2b8`, wave64 em AMD, que troca a versão do
cache de shaders). Entraram: arenas de buffer em blocos de 64 MB e uma arena por grupo de 4 páginas de 4 GB
(`SOTC_ARENA_GROUP`), cache das propriedades de formato de imagem (chave tornada exata), fila de notificações
da GPU que cresce, `Close()` do cache de pipelines ao fechar a janela, `SOTC_FLIP_LATE`, linhas de estatística
(VRAM, GC de texturas, `SOTCSTALL`) e os padrões de AMD. A troca de cache incompatível já existia aqui
(`DataBase::Reset`), então a do PR ficou de fora.

`SOTC_RB_ASYNC` continua desligado: o PR o ligou (ganho medido em AMD), mas na RTX 5070 ele era neutro e,
sobre o nosso rastreamento de páginas, o demo parou na assertiva "protect region as write-only" do
`page_manager`. Com ele desligado, o build com o PR roda o demo igual ao sem PR (97,0 contra 98,8 FPS, dentro do
ruído): os ganhos do PR são em travadas, memória de vídeo e estabilidade, não em FPS médio.
