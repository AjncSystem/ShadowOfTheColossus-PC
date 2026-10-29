# Thread de desenho: plano para 30 FPS cravados nas trocas de área

Pedido do usuário em 29/10/2026, depois do T48 (commit local `7fde988`): as grandes travadas
saíram (pior quadro 534 ms → 58–75 ms), mas cada troca de área ainda perde 1–6 quadros. A
conclusão da sessão anterior foi que o limite é estrutural: a thread de desenho
(`shadPS4:GpuDraw`) aparece ocupada 28–32 ms de cada quadro de 33 ms.

**Este documento é só o plano.** Nada foi implementado. Antes de qualquer código, o usuário
aprova a ordem e a etapa 0.

## Ordem combinada com o usuário (29/10)

Fora das trocas de área, o jogo já roda em 30 FPS cravados: o quadro típico fica em 33,3 ms e as
janelas de 5 s dão 30,0 FPS em todos os logs de 29/10. A ordem acertada é:

1. **Fase 1: polir os 30 FPS.** Tirar os 1–6 quadros perdidos nas trocas de área. São as etapas
   0–4 da seção 4, medidas a 30 FPS.
2. **Fase 2: corrigir as falhas abertas.** A lista sai do ROADMAP e o usuário escolhe quais
   entram. Nova desde o T48: queda em `guest offset 0x49800` (0xc0000005, leitura de
   `ffffffffffffffff`) aos 24 min da sessão das 10:00–10:24 de 29/10, com o exe de `7fde988`.
   É o alocador do heap do BPE, vizinho do 0x497f0 que a guarda do heap protege. Na última janela
   antes da queda, os blocos livres caíram de 5870 para 3432 e a guarda "abonou" 30 escritas.
   Log e dump em `E:\...\SotC-PC\user\crashes\2026-10-29_10-24-5*`.
3. **Fase 3: publicar uma versão nova no site** (release no GitHub, sem apagar as antigas),
   com a aprovação do usuário na hora de publicar.
4. **Fase 4: chegar perto dos 60 FPS.** A 60 FPS cada quadro tem 16,7 ms. Pelo perfil de 28/10
   numa área pesada, a thread de desenho gasta ~22 ms de CPU por quadro e o processador de
   comandos ~17 ms (15,6 ms só lendo comandos). A etapa 0 se repete no modo 60. As etapas 5–6
   (binds preparados fora da thread, gravação em várias threads) viram o caminho principal,
   junto com um item novo: aliviar o processador de comandos.

Configuração de referência (o lançador real do usuário, `user/launcher_settings.json` em
29/10): 30 FPS (`fps_mode 30`), modo PS4 Pro, texturas Máxima, DLSS Ultra (1600x900 → 1920x1080),
Fifo, VRAM máxima, thread de gravação ligada, `SOTC_LOD_FEEDBACK=1 SOTC_MIP_TAIL=0`.
i9-13900K, RTX 2060 SUPER (8 GB).

## 1. O que os números já mostram (e um problema na medição)

### 1.1 "draw busy" não mede trabalho

A linha `SOTC threads per frame: draw busy` (`core/libraries/videoout/driver.cpp:777`) é
`(tempo da janela − tempo ocioso) / flips`. O tempo ocioso só conta quando a fila da thread de
desenho está **vazia** (`Liverpool::BackLoop`, `liverpool.cpp:130`). Tudo o que a thread faz
*dentro* de uma operação conta como ocupado, inclusive **esperar**:

- o flip roda na thread de desenho (`SubmitFlip` → `SendOrderedCommand` →
  `SubmitFlipInternal` → `Presenter::PrepareFrame` → `GetRenderFrame`, que espera um quadro
  livre e a fence de apresentação com Fifo);
- as leituras da GPU (`BeginAsyncReadback`/`FinishAsyncReadback`) e os flushes pedidos por
  outras threads (`SendCommand<true>`) rodam lá, entre duas operações;
- `scheduler.Finish()`, `Scheduler::Wait`, `waitIdle` e as esperas da trava de memória também.

Prova no log do usuário de 29/10 (`E:\...\SotC-PC\user\last_run.log`, 293 janelas de 5 s):

| janelas | comandos por quadro | "draw busy" médio |
|---|---|---|
| 4 | menos de 500 | 28,8 ms |
| 188 | 3000 ou mais (até 13 711) | 28,3 ms |

Com 25 vezes mais comandos, o número não muda. Ou seja, ele mede o ritmo do quadro (30 FPS
travados), e não quanto a thread trabalha. **"A thread de desenho está saturada" ainda não está
provado.**

### 1.2 O tempo de CPU real é menor, mas não é pequeno

O perfil de todas as threads de 28/10 (`out/prof_all_trans.run.txt`, `SOTC_PROFILE=*`, ~10 s
numa área pesada, 6000 comandos por quadro) tem o tempo de CPU de cada thread (`GetThreadTimes`):

| thread | usuário + kernel | por quadro de 33 ms (≈) |
|---|---|---|
| `shadPS4:GpuRecorder` | 8 938 + 859 ms | ~32 ms (quase tudo espera ativa) |
| `shadPS4:GpuDraw` | 5 391 + 1 172 ms | ~22 ms |
| `shadPS4:GpuCommandProcessor` | 4 391 + 734 ms | ~17 ms (`command parse 15,6 ms`) |
| 4 × `BPE JobWorkerTh` (jogo) | ~4,2–4,5 s cada | ~14 ms cada |

Mesmo esses ~22 ms incluem espera ativa: a thread de desenho gira 4096 `pause` antes de dormir,
e o gravador também gira (`RECORDER_PAUSE`). Nas amostras da GpuDraw, ~22% estão dentro de uma
espera do kernel (`KERNELBASE+0x22a58`, `ntdll+0x1659c4`). Os nomes das funções pequenas do
build LTO não são confiáveis (ICF junta funções iguais), então endereço de função pequena não
basta para culpar ninguém.

Conclusão: a thread de desenho tem folga de verdade só se o trabalho dela for bem menor que
22 ms; hoje não dá para afirmar nem uma coisa nem outra.

### 1.3 O que pesa nos quadros perdidos

Linha `SOTC FRAMES` das janelas com quadro acima de 40 ms no mesmo log (o pior quadro de cada
janela, com a causa). Os temporizadores (`SotcStats::FrameTimer`) contam em qualquer thread:

| pior quadro | maior causa | outras |
|---|---|---|
| 816 ms | leitura da GPU 599 ms | shader 90, ocioso 90 |
| 384 ms | leitura da GPU 112 | shader 60, pipeline 59, ocioso 77 |
| 250 ms | leitura da GPU 133 | ocioso 85, shader 25 |
| 200 ms | leitura da GPU 74 | ocioso 61, shader 36 |
| 200 ms | ocioso 94 | pipeline 37, leitura 34 |
| 200 ms | ocioso 67 | buffer 23, io 17, upload 13 |
| 87–92 ms | imagem nova 27–57 | — |
| 68–73 ms | imagem nova 52–62 | — |
| 74–108 ms | envio de textura 53–61 | alloc 7–8 |
| 159–192 ms | ocioso 48–136 | DMA 44, buffer 16–20, io 19 |
| 58–67 ms (os mais comuns) | ocioso 10–78 | nada acima de 10 ms |

"Ocioso" (`FrameIdle`) é o processador de comandos esperando o jogo enviar trabalho: nesses
quadros quem atrasou foi o **jogo**, muitas vezes esperando uma leitura da GPU, arquivo (io) ou
os próprios workers do BPE. Em vários quadros longos, o desenho não estava no caminho crítico.

**Resumo:** os quadros perdidos têm três origens que se misturam: (a) esperas longas de leitura
da GPU pelo jogo, (b) trabalho único na thread de desenho ao entrar em área nova (shader,
pipeline, imagem nova, envio de textura, DMA) e (c) a carga contínua de 5–10 mil comandos por
quadro. Dividir a thread de desenho ajuda (b) e (c), mas não (a). Por isso a etapa 0 mede antes
de mexer.

## 2. Etapa 0 — medir de verdade (obrigatória antes de escolher)

Chave nova `SOTC_DRAW_SPLIT=1` (desligada por padrão; só contadores, não muda o comportamento).
Linha nova no log a cada 5 s:

```
SOTC DRAW SPLIT per frame: cpu 14.2 ms (cycles), draw 6.1, indirect 3.0, dispatch 1.2,
  bind 4.4 [buffers 1.9, textures 2.1, dma 0.4], flip 0.3 (waited 9.8), readback 0.6
  (waited 2.1), commands 0.4, other waits 1.7; recorder cpu 4.0 (spin 20.1); cp→draw drains 4
```

Como medir cada parte:

1. **CPU real por thread:** `QueryThreadCycleTime` da GpuDraw, do processador de comandos e do
   gravador, a cada flip, descontando o tempo nos laços de espera ativa (contador próprio em
   volta dos `CpuRelax`/`RECORDER_PAUSE`).
2. **Tempo por tipo de operação:** em `ExecuteBackOp`, acumular por `BackOp::Kind` (Draw,
   DrawIndirect, Dispatch, Func) e, dentro de `Rasterizer::Draw`, os blocos `BindResources`
   (separando `BindBuffers`, `BindTextures`, `SynchronizeDmaBuffers`), `BeginRendering` e a
   gravação.
3. **Espera dentro da operação:** cronometrar separado o que bloqueia: `GetRenderFrame`, os
   `Scheduler::Wait`/`Finish`, as esperas da trava de memória vistas pela GpuDraw, `waitIdle`.
   Isso sai do "busy".
4. **Quem estava no caminho crítico nos quadros > 40 ms:** guardar, junto com o pior quadro, a
   CPU da GpuDraw, a CPU do processador de comandos, a fila da thread de desenho
   (`back_head − back_tail`) e quanto o jogo esperou leitura da GPU.

Custo: um `QueryThreadCycleTime` e `NowNs` por bloco, só com a chave ligada (como `SOTC_FPS`).

**Implementado em 29/10 (sem commit), instalado no E: às 11:04.** Linhas `SOTC DRAW SPLIT per
frame`, `worst frame` e `frames > 40 ms` a cada 5 s (`sotc_stats.h`/`.cpp`, `driver.cpp`). As
esperas de leitura da GPU pelo jogo somam todas as threads do jogo, por isso podem passar de
33 ms. Primeiro teste, numa cena leve (~290 comandos por quadro): "draw busy" 28,9 ms, mas a
CPU real da thread de desenho é **6,8 ms**. A thread fica **25,3 ms por quadro parada no flip**
(`GetRenderFrame`, esperando um quadro de apresentação livre), e nesse tempo as leituras da GPU
que o jogo espera também ficam paradas (fase "finish" de 28–70 ms somada entre as threads). Ainda
falta medir isso nas áreas pesadas.

### Resultado da etapa 0 — rota do usuário (29/10, 11:06–11:14)

Lançador do usuário (30 FPS, Pro, Máxima, DLSS), santuário → carregar o save → duas trocas de
área → colosso; 86 janelas de 5 s. Valores por quadro (ms):

| trecho | comandos | CPU desenho (giro) | CPU proc. comandos | binds | espera no flip | leituras do jogo (soma) |
|---|---|---|---|---|---|---|
| santuário / menus | 150–1 500 | 6–11 (1–2) | 5–10 | 2–5 | 19–25 | 4–17 |
| campo, áreas médias | 1 200–3 000 | 15–17 (2,5) | 7–10 | 5–6 | 13–16 | 28–32 |
| áreas pesadas, 30 FPS | 4 000–7 400 | 20–24 (2) | 15–24 | 8–13 | 3–10 | 45–55 |
| quadros > 40 ms nas trocas (21–22 por troca) | 6 000–7 000 | 33–36 (4) | 24–39 | 13–22 | 0 | 60–70 |

Leitura:

1. **Nas áreas pesadas a thread de desenho tem 3–10 ms de folga, não zero.** O trabalho real fica
   em 18–22 ms; o resto do antigo "draw busy" é espera no flip.
2. **Na entrada de cada área nova (~1 s, 21–22 quadros) ela passa do limite**: 29–32 ms de
   trabalho real, espera no flip 0, quadros de 40–50 ms. Os binds são a maior parte (13–22 ms
   por quadro, metade das operações de desenho); o processador de comandos também chega a
   24–39 ms nesses quadros.
3. **Os poucos quadros de 100 ms são envios de textura dentro do `BindTextures`**: 108 ms (t=356)
   com texturas 59 ms e "upload" 55 ms; 99 ms (t=401) com texturas 64 ms e upload 56 ms, para só
   19–28 MB copiados (~0,5 GB/s). Um de 108 ms (t=110) foi shader (59 ms) e um de 84 ms (t=85)
   foi o jogo esperando arquivo (io 172 ms).
4. GPU, envio e gravador quase não esperam (≤ 1,5 ms); a fila da thread de desenho fica cheia
   (512) em todo trecho pesado.

### Etapa 1 — envios de textura (29/10, instalado às 11:35)

Segunda rota (11:23, fases do envio medidas): nos quadros de 90–175 ms o tempo do envio está
quase todo na **cópia** da memória do jogo para a área temporária: 41–58 ms para 12–20 MB. A reserva da
área temporária, a destilização e a gravação somam menos de 9 ms. Nessas janelas o jogo mapeia memória da área nova
(`batch_map2` segura a trava da tabela 330–440 ms a cada 5 s), e a cópia
(`CopySparseSerial`) consultava a tabela duas vezes por pedaço contínuo (`runtime_memory_region`
e `runtime_memory_backing_pointer`). Cada consulta que o cache de regiões não respondia esperava
o lote inteiro. Mover o envio para outra thread não adiantaria: ela esperaria igual.

Correção: `SOTC_COPY_LOCK_ONCE=1` (`runtime_memory_copy_out`, `src/runtime_memory.c`). Os
pedaços que o cache de regiões já conhece são copiados sem trava; o resto, sob uma trava de
leitura só, para a cópia inteira. O resultado é o mesmo de antes (pela visão `backing`; memória
reservada ou não mapeada vira zero). Vale também para os envios de buffer (os picos de "buffer"
de 50–90 ms). Ligada só no lançador do usuário; desligada é o comportamento do `7fde988`.

**Resultado (rotas das 11:35 e 11:45): sem ganho.** A cópia nova quase nunca esperou a trava
(`runtime_memory_copy_out` ~0 ms nas esperas), mas a rota das 11:35 ainda teve duas cópias de
41–45 ms para 14–19 MB. A trava não era a causa. A rota das 11:45 (130 s, mais curta) mediu
também as faltas de página: nela não houve cópia lenta; as mais lentas foram 12,5 ms (24,5 MB) e
17,4 ms (88,5 MB), com ~5 500 faltas de página do processo no quadro, contra ~200–1 500 nos
outros. No menu, 25,8 MB foram copiados em 0,9 ms. Conclusão provisória: a cópia lenta é rara
(2–3 quadros por rota longa) e coincide com muitas faltas de página, mas a causa não está
provada. As médias nas áreas pesadas ficaram iguais nas três rotas (desenho 21–22 ms, binds
9–10 ms). Os ~20 quadros de 40–50 ms de cada troca, que perdem mais quadros no total, vêm dos
binds e do processador de comandos.

### Etapa 2 — perfil da thread de desenho (rota das 11:56, 30 s–2 min de jogo)

`SOTC_PROFILE=GpuDraw` (54 938 amostras; dados em `out/prof_draw_rota_29-10.txt`, simbolizados
com `llvm-symbolizer` no `out/gpu/bb-probe.exe` da mesma compilação). Tempo da thread:

| parte | amostras |
|---|---|
| esperando quadro livre no flip (`GetRenderFrame`, Fifo) | 28% |
| sem trabalho (espera da fila) | 9–10% |
| esperando a GPU (`SubmitAheadAndWait`, `Semaphore::Wait`), trava de memória, `yield` | ~8% |
| despachos de compute (`DispatchDirect`/`Indirect`) | 15% |
| desenhos indiretos (`DrawIndirect`) | 8% |
| dentro deles: `BindBuffers` 6%, `BindTextures` 4%, `FindTexture` 3%, `SynchronizeDmaBuffers` 3% | |
| proteção de páginas (`ZwProtectVirtualMemory` via `protect_fast`/`PageManager::Protect`) | 4% |
| guarda do heap ao aplicar leituras (`BpeHeapGuard`, `FinishAsyncReadback`) | 4–6% |
| cópias da memória do jogo (`CopySparseSerial`) | 3,5% |

Não há um ponto quente nos binds: o trabalho está espalhado por uma dúzia de funções de 3–6%
cada. A guarda do heap não pode mudar de comportamento. Os ~20 quadros lentos de cada troca
saem da soma desses custos na thread de desenho e no processador de comandos (que também chega
a 24–39 ms nesses quadros), não de uma função só.

**Conclusão da fase 1 (29/10):** o que dava para tirar com correção pequena e segura já saiu no
T48. O que sobra (1–6 quadros por troca de área, ~20 quadros de 40–50 ms por ~1 s) só cai com a
mudança estrutural das opções A/C: preparar binds em paralelo e gravar em várias threads. É o
mesmo trabalho que a fase 4 (60 FPS) exige, onde ele rende duas vezes. Ficam desligadas e sem
uso: `SOTC_COPY_LOCK_ONCE` (sem ganho). Fica a medição `SOTC_DRAW_SPLIT` (só contadores).

### Perfil de todas as threads (rota das 12:14, 30 s–2 min de jogo)

`SOTC_PROFILE=*` (dados em `out/prof_all_rota_29-10.txt`). Média da janela inteira de 90 s, com
uma troca de área:

| thread | esperando | trabalho principal |
|---|---|---|
| desenho (`GpuDraw`) | 44% no flip (`PrepareFrame`), ~9% sem trabalho, ~5% GPU | operações 19% (dispatch 11, indireto 10, binds 6+4), leituras do jogo (`FinishAsyncReadback`, guarda do heap) ~6%, proteção de páginas ~3% |
| processador de comandos | 63% esperando o jogo enviar, ~7% esperando a thread de desenho | leitura dos comandos 21%, código JIT 20%, busca de pipeline 6% |
| workers do jogo (`BPE JobWorkerTh`, 4) | mutex do jogo 33%, eventos ~25% | **24% do tempo dentro de faltas de página de leitura da GPU** (`vectored_fault_impl` → `ReadMemory`) |

Na média nenhuma das duas threads da GPU está cheia. Os quadros perdidos ficam nas rajadas de
~1 s de cada troca de área, quando as duas chegam a 33–40 ms ao mesmo tempo (contadores
`SOTC DRAW SPLIT`), e o perfil por amostragem não separa essas rajadas do resto.

### Opções estruturais, com custo e risco honestos

| | o que é | ganho esperado nas rajadas | tamanho | risco |
|---|---|---|---|---|
| A1 | o processador de comandos (63% ocioso na média) já lê os sharps: ele passa a decodificar T#/V#/S#, `ImageInfo`/`ImageViewInfo` e o tamanho mapeado dos buffers junto com cada operação; a thread de desenho só busca nos caches e grava | 3–6 ms por quadro de rajada (a parte de decodificação dos binds) | 3–5 dias | médio: sharps lidos antes da hora (mesma regra do `WaitBackWrites`); nas rajadas o processador de comandos também está cheio, então parte do ganho some |
| A2 | buscas nos caches (`FindTexture`, `ObtainBuffer`) feitas em lote por uma thread de preparo | +5–8 ms | 1–2 semanas | alto: caches de textura e buffer não são thread-safe (lição do T48: trava mal posta criou as piores travadas) |
| C | gravar em 2–4 threads (passadas separadas, command buffers secundários) | o único que tira 10–15 ms | semanas | alto: estado do rasterizador inteiro (barreiras, layouts, HUD sem UI, DLSS que observa os draws) feito para uma thread só |
| D' | faltas de página de leitura da GPU nas threads do jogo (24% do tempo delas) | o jogo envia trabalho mais cedo (quadros com "ocioso" alto) | grande (é o "modelo de memória do PC") | alto: passa ao lado da guarda do heap, que não pode mudar de comportamento |

Nenhuma opção sozinha zera as perdas: nas rajadas, a thread de desenho e o processador de
comandos passam ambos de 33 ms. A1 é o primeiro passo de A2 e de C, e o único de risco médio.

**Decisão (tabela acima: CPU real 15–25 ms):** B primeiro, começando pelos envios de textura que
fazem os quadros de 100 ms; depois tirar trabalho dos binds (opção A), que é o que estoura nos
~20 quadros de cada troca. A espera no flip na frente das leituras do jogo (opção D) fica para
depois: nos trechos pesados ela cai para 0–7 ms.

**Medição da etapa 0:** o usuário joga a rota de sempre com o lançador dele (30 FPS, Pro,
Máxima, DLSS), com `SOTC_FPS=1 SOTC_DRAW_SPLIT=1` em `extra_env`: santuário → sair a cavalo →
duas trocas de área → um colosso. Eu só leio `user/last_run.log` depois. Se quiser um perfil por
endereço, `SOTC_PROFILE=* SOTC_PROFILE_DELAY=<s> SOTC_PROFILE_SECONDS=10 SOTC_PROFILE_OUT=...`
na mesma rodada, simbolizado com `llvm-symbolizer` no `bb-probe.exe` **sem strip** (só funções
grandes são confiáveis).

**Decisão depois da etapa 0:**

| se a CPU real da GpuDraw nas áreas pesadas for... | então |
|---|---|
| menos de ~15 ms | a thread não está cheia: ir direto às opções B e D (picos e leituras); a opção A fica guardada |
| 15–25 ms | B e D primeiro, depois A |
| mais de ~25 ms | A entra cedo, B e D em paralelo |

## 3. Opções para tirar trabalho da thread de desenho

### Opção A — preparar os binds fora da thread (pipeline de duas fases)

Hoje cada draw faz, em sequência na GpuDraw: aplicar registradores → `BindResources`
(`BindBuffers` + `BindTextures`: procurar buffers e imagens nos caches, sincronizar, montar
`set_writes`/`push_data`) → `BeginRendering` → gravar. O `FASTBIND` já pula ~58–63% dos binds
repetidos.

Proposta (`SOTC_BIND_PREP=1`): o processador de comandos já resolve o pipeline
(`CaptureBackOp`). Uma thread "preparadora" entre ele e a GpuDraw calcula o que é **só
leitura**: hash do FASTBIND, `MakeUserData`, leitura das V#/T#/S# (sharps) da memória do jogo,
chaves de busca de imagem/buffer. A GpuDraw recebe a op já com essas chaves e faz só as buscas
que mexem nos caches e a gravação.

- Ganho esperado: a parte de leitura de sharps e hash (a medir na etapa 0; estimativa 20–35% do
  bind).
- Riscos:
  - os sharps estão na memória do jogo, que muda; ler antes exige a mesma regra do
    `WaitBackWrites` (esperar a última op que escreve memória), senão o draw usa recursos
    velhos (textura errada, piscadas);
  - o cache de texturas e o de buffers **não são thread-safe**; a preparadora só pode ler
    estruturas imutáveis ou com contador de geração (como `BindGeneration`);
  - mais uma fila aumenta a latência das leituras da GPU que o jogo espera.
- O que não muda: guarda do heap, `POOL_FIX`, regra do pool (nada disso está no bind).

### Opção B — tirar da thread os trabalhos únicos de área nova

Nos quadros perdidos, os picos de "imagem nova" (27–62 ms), "envio" (53–61 ms), "buffer"/"dma"
(16–45 ms) e "hash" acontecem **na GpuDraw**, no meio do quadro, quando um draw encontra algo
que ainda não existe na GPU.

Proposta, em três partes independentes, cada uma com sua chave:

1. `SOTC_IMAGE_AHEAD=1` — criar imagens antes: quando o processador de comandos lê uma T# nova
   (já lê os sharps para achar o pipeline), pede a criação da `VkImage` + memória VMA numa
   thread de trabalho. Quando o draw chega, a imagem já existe; ele só registra e envia. A
   criação de imagem é o que mais aparece sozinho nos quadros de 68–92 ms.
2. `SOTC_SYNC_WORKER=1` — as cópias de memória do jogo para a área temporária (DMA, envio de
   buffers, `memcpy` das texturas, hash do conteúdo) vão para as 4 threads de cópia que já
   existem (`bbport_copy.cpp`). A GpuDraw só grava o `vkCmdCopy` depois que a cópia terminou,
   como já se faz com o orçamento de envio (`SOTC_UPLOAD_BUDGET_MB`).
3. Shader e pipeline: o `ASYNC TRANSLATE`/`ASYNC COMPUTE` já existem, mas os picos de shader
   (16–90 ms) e pipeline (28–59 ms) ainda aparecem na entrada de área. Medir quais estágios
   ainda compilam na thread (contador por motivo) e cobrir esses casos.

- Ganho esperado: tirar os picos de 50–60 ms que sobram em trocas sem leitura longa; é o que
  mais se parece com "quadro perdido por um trabalho grande".
- Riscos:
  - **imagem criada para uma T# que o jogo muda antes do draw**: só memória e tempo perdidos (a
    imagem é descartada); limitar quantas por quadro;
  - VRAM: criar antes aumenta o pico de uso (hoje 5,9–6,9 GB de 6,5 GB de orçamento); respeitar
    o orçamento e a coleta de emergência;
  - cópia feita antes de uma escrita do jogo: precisa da mesma proteção de páginas que o DMA
    usa hoje (a escrita depois da cópia marca a página e o draw refaz); **sem isso aparecem
    blocos verdes/lixo**, que não podem voltar;
  - o orçamento de envio continua: textura adiada fica preta um quadro, como hoje.

### Opção C — gravar em várias threads

A mais radical: dividir os draws de um quadro (por passada de renderização, por exemplo sombra
/ G-buffer / pós) entre 2–4 threads de bind+gravação, cada uma com seu command buffer
secundário, e o gravador junta tudo na ordem.

- Ganho: escala de verdade com 5–10 mil comandos por quadro, se a CPU do bind for o limite.
- Riscos (altos):
  - o estado do rasterizador (`render_state`, barreiras, layouts das imagens, `fast_bind`,
    `needs_barrier`, o HUD sem UI, a sombra de UI, o DLSS que observa os draws) é todo de uma
    thread só; dividir exige separar estado por thread e uma fase de junção das barreiras;
  - os caches de textura e buffer teriam que ganhar travas finas, o que pode custar mais do que
    ganha (lição do T48: travas mal colocadas criaram as piores travadas);
  - ordem de escrita na memória do jogo e leituras da GPU: um draw que escreve e outro que lê
    na mesma passada não podem ir para threads diferentes;
  - semanas de trabalho, e muitos lugares onde a guarda do heap e o readback se encontram.
- Só vale se, depois de A e B, a CPU real da GpuDraw ainda passar de ~25 ms nas áreas pesadas.

### Opção D (não é a thread de desenho, mas pesa mais) — leituras da GPU mais curtas

Nos piores quadros (200–816 ms) a maior causa é o jogo esperando leitura da GPU. O caminho
(`BufferCache::ReadMemory` com `SOTC_RB_ASYNC`): a thread do jogo pede a cópia na GpuDraw
(`BeginAsyncReadback`), espera o command buffer ser enviado (`WaitSubmitted`), espera a GPU, e
pede de novo a GpuDraw para aplicar (`FinishAsyncReadback`, onde está a guarda do heap).

Proposta (`SOTC_RB_PRIORITY=1`): quando há leitura esperando, a GpuDraw envia o command buffer
na hora (hoje pode esperar o próximo flush periódico) e a aplicação final roda antes da próxima
op. Medir antes: na etapa 0, separar quanto da espera é fila da GpuDraw, quanto é envio e quanto
é GPU.

- Riscos: mais envios por quadro (custo do driver); **a aplicação continua passando pela
  `ApplyDownloads` com a guarda do heap, sem mudar nada nela.**

## 4. Ordem proposta

Cada etapa fica atrás da sua chave, desligada até o usuário aprovar; cada uma é medida antes da
próxima. Eu compilo, confiro que o jogo abre (sem rodar nada enquanto o usuário joga), instalo
direto em `E:\GAMES\Shadow Of The Colossus\SotC-PC` com backup do exe ao lado e aviso "instalado
às HH:MM".

| # | Etapa | Chave | Medição (usuário jogando, lançador dele) | Pronto quando |
|---|---|---|---|---|
| 0 | Medição dividida da thread de desenho | `SOTC_DRAW_SPLIT=1` | rota padrão (santuário → 2 trocas de área → colosso); ler `SOTC DRAW SPLIT` e `SOTC FRAMES` | sabemos a CPU real da GpuDraw e quem está no caminho crítico dos quadros > 40 ms |
| 1 | Imagens criadas antes | `SOTC_IMAGE_AHEAD=1` | mesma rota; comparar "image" nos quadros > 50 ms e VRAM | "image" some dos quadros perdidos, VRAM dentro do orçamento, nenhuma textura errada |
| 2 | Leituras da GPU com prioridade | `SOTC_RB_PRIORITY=1` | mesma rota + o final do jogo (save 15 derrotados, slot 4) até os créditos | piores quadros caem; final sem queda; guarda do heap protegendo como antes |
| 3 | Cópias e hash nas threads de cópia | `SOTC_SYNC_WORKER=1` | mesma rota; "buffer", "dma", "upload", "hash" e CPU da GpuDraw | CPU da GpuDraw menor nas áreas pesadas; sem blocos verdes |
| 4 | Shader/pipeline que ainda compilam na thread | (estende `SOTC_ASYNC_TRANSLATE`) | primeira visita a uma área sem cache | "shader"/"pipeline" ~0 nos quadros perdidos |
| 5 | Preparação dos binds | `SOTC_BIND_PREP=1` | mesma rota; CPU da GpuDraw e "late" na linha `SOTC FRAMES` | só se a etapa 0 mostrar GpuDraw > 15 ms depois das etapas 1–4 |
| 6 | Gravação em várias threads | `SOTC_MT_RECORD=1` | — | só se ainda passar de ~25 ms depois da etapa 5; plano próprio antes |

As etapas 1, 2 e 3 não dependem uma da outra e podem trocar de ordem conforme o que a etapa 0
mostrar (a de maior peso nos quadros perdidos vai primeiro).

**Critério final (30 FPS cravados):** na rota padrão, com o lançador do usuário, `late 0 of
150` e nenhum quadro acima de 50 ms nas janelas das trocas de área, em duas rodadas seguidas;
e o final do jogo até os créditos sem queda.

Testes automáticos (harness, 60 FPS com `BB_VBLANK_HZ=120`, Máxima + Pro) só com o usuário fora
do jogo e avisado antes, conferindo `tasklist | grep bb-probe`. Eles servem para ver a folga da
thread (a 60 FPS o limite de 16,7 ms aparece sem o ritmo de 30 esconder), não para aprovar a
etapa.

## 5. Restrições que valem para todas as etapas

- Não mexer na semântica da guarda do heap do BPE (`BpeHeapGuard`, `ApplyDownloads`), no
  `POOL_FIX` nem na regra do pool: o final do jogo depende deles.
- Nenhum bloco verde ou textura com lixo: qualquer cópia ou leitura feita mais cedo precisa da
  mesma proteção de páginas de hoje.
- Qualidade máxima fica: PS4 Pro, texturas Máxima, DLSS. Nada de ganhar FPS baixando qualidade.
- Toda mudança atrás de uma chave de ambiente, e com a chave desligada o comportamento é
  idêntico ao `7fde988`.
- Depois de cada etapa, atualizar o T48 no `ROADMAP.md` e mostrar ao usuário.
