# Plano: troca de área sem queda de FPS (17/11/2026)

Regra do usuário: **nenhuma perda de qualidade de imagem nem de FPS**. Cada etapa tem uma chave para desligar
(`=0`) e um critério de aprovação medido antes de virar padrão.

Base do plano: medições do port (ROADMAP T48/T50, PLANO-THREAD-DESENHO, MODELO-MEMORIA-PC), estudo na web de
emuladores e de Vulkan, e uma revisão independente do código (três consultas em paralelo, cruzadas e conferidas
no código em 17/11).

## O problema em uma frase

A cada quadro a GPU calcula a visibilidade dos objetos (6 shaders de cull) e o jogo lê o resultado **no mesmo
quadro** para montar a lista de desenho. No PS4 essa leitura é de graça (memória unificada). No port ela vira uma
leitura de emergência: o jogo esbarra numa página protegida, o port grava uma cópia, envia, espera a GPU terminar
**tudo** que estava na fila antes, e aplica. Numa troca de área isso se repete página por página: 30–65 ms por
quadro lento. A GPU não é lenta (sem modo Pro a espera é igual): é **fila e serialização**.

## O caminho de hoje (conferido no código)

| Passo | Onde | Custo medido nos quadros lentos |
|---|---|---|
| 1. O processador de comandos chega ao dispatch de cull (fica atrás do resto do quadro) | `liverpool.cpp` SubmitDispatch :841, BackAlloc :327 | não medido (etapa 0) |
| 2. A thread de desenho grava o dispatch e marca as páginas da máscara como escritas pela GPU | `vk_rasterizer.cpp` DispatchDirect :2513; `buffer_cache.cpp` :1303 | — |
| 3. O rótulo (EOP) é escrito **na hora da gravação**, antes de a GPU rodar | `liverpool.cpp` :1793, :2360 | — |
| 4. O jogo lê, falta de página → pedido de leitura vai para a fila da thread de desenho, que só atende **entre** operações (um BindTextures de 20–55 ms na entrada da área) | `buffer_cache.cpp` ReadMemory :244; `liverpool.cpp` BackLoop :195 | fila 1–20 ms |
| 5. Grava a cópia no buffer atual e manda enviar | BeginAsyncReadback :459 | — |
| 6. O gravador reenvia tudo desde o último envio | `vk_scheduler.cpp` :514 | < 1 ms |
| 7. A GPU termina o envio inteiro (uma fila só, em ordem) | — | 10–180 ms |
| 8. Volta para a fila da thread de desenho e aplica (com a guarda do heap) | FinishAsyncReadback :554 | 10–60 ms |
| 9. A próxima página da máscara que não estava junto repete 4–8 | GatherDownloads :779 | multiplica tudo |

## O que já foi descartado (e por quê)

- Máscara do quadro anterior (fork HiddenUserHere): cavalo, capa, grama e escadas tremendo; objetos somem 3 quadros
  ao virar a câmera. Mesma família de falhas no yuzu (descarga "adivinhada"), RPCS3 ZCULL Relaxed, Xenia "fast".
- Visibilidade calculada na CPU: precisa de tabelas que a GPU escreve; cálculo só por frustum desenha objetos
  demais (8–17 FPS com o XPR desligado).
- `SOTC_EOP_DEFER` (todo rótulo só com a GPU pronta): FPS pela metade.
- `SOTC_VIS_DIRECT` v1 (sem esperar): quedas e heap corrompido; v2: 58,8 → 41,8 FPS.
- Memória ReBAR (VRAM visível à CPU) para a CPU ler: NVIDIA e AMD desaconselham, leitura até 1000× mais lenta.
- Prioridade global de fila na NVIDIA: exige privilégio de administrador no Windows.

## A ideia nossa: "leitura entregue", não "leitura pedida"

O jogo diz exatamente **qual shader** escreve a máscara e **onde**. Então o port não precisa esperar o jogo
esbarrar na página: logo depois de gravar o dispatch de cull, ele já grava a cópia das faixas que esse dispatch
escreve para a memória do PC, e envia tudo junto no rótulo que o jogo espera. Quando o jogo lê, a cópia já está
pronta ou a caminho; espera-se só ela, uma vez por conjunto, e não uma fila inteira por página. A imagem é
**idêntica** (a máscara é a do quadro certo, byte a byte), e o caminho de emergência de hoje continua por baixo
como rede de segurança.

## Etapas

### Etapa 0 — Medir o que falta medir (sem mudar nada)
- Rota do usuário na 0.76.1 com `SOTC_DRAW_SPLIT=1 SOTC_SLOW_OPS=1` (lançador já preparado).
- Contadores novos (`SOTC_RB_AGE`): tempo do dispatch de cull publicado até o rótulo; do rótulo até a falta; e,
  por falta, se o dado já estava numa leitura em andamento. Diz quanto do tempo é o passo 1 (processador de
  comandos atrasado) e quanto é GPU.
- Aprovação: números por trecho no `tools/entry_report.py`.

### Etapa 1 — Agrupar por shader (`SOTC_RB_GROUP`, menor e mais segura)
- Cada página de 64 KiB escrita pela GPU guarda o programa que a escreveu (hoje só com `SOTC_RB_AGE`; passa a ser
  sempre, barato).
- Na primeira falta numa máscara de cull, a leitura traz junto **todas** as faixas escritas por esse programa no
  envio atual e no anterior (mesmo caminho de aplicação, com a guarda do heap e o pulo de páginas que a CPU
  escreveu).
- Ganho: some a repetição do passo 9 (várias esperas viram uma). Risco: baixo. Limite de bytes por leitura.

### Etapa 2 — Leitura entregue (`SOTC_RB_PUSH`, o coração do plano)
- Lista de programas de cull: semente com os 6 conhecidos (`0x50784801, 0x93aec24b, 0x2e0f474b, 0x4defcf7f,
  0xf94d23d4, 0x729c541e`), mais os aprendidos (programa cujas faixas o jogo leu ainda sendo escritas).
- Depois do `cmdbuf.dispatch` desses programas (ponto do `AfterVisDirect`, `vk_rasterizer.cpp` :2628/:2684):
  barreira cálculo→cópia e cópia das faixas escritas (só as residentes, como `GatherDownloads`) para memória
  HOST_CACHED; registro como leitura assíncrona "sem dono" em `inflight_readbacks`.
- Envio uma vez, no rótulo EOP/ReleaseMem (`liverpool.cpp` :1728/:2334), se houver cópias entregues pendentes;
  nunca um envio por dispatch (o `0x50784801` roda ~280 vezes por quadro).
- Lado do jogo: a falta acha a leitura em andamento (`FindInflightReadback`, caminho "dependent" que já existe,
  `buffer_cache.cpp` :476/:555), espera só o tick dela e aplica como hoje. Nada é escrito na memória do jogo sem
  falta: zero escrita especulativa.
- Se outro passe reescrever a máscara depois, `MarkInflightReadbacksDirty` já marca a entrega como velha e o
  caminho normal assume.
- `SOTC_RB_PUSH_VERIFY=1` (teste): compara a entrega com a leitura normal byte a byte; tem que dar 0 diferenças.
- Ganho esperado: a primeira falta do quadro deixa de pagar a fila do pedido e o reenvio; as seguintes acham os
  dados prontos. Risco: baixo-médio; um envio a mais por rótulo com entregas (dezenas de µs).

### Etapa 3 — Aplicar sem esperar a operação longa (`SOTC_RB_SAFEPOINT`)
- A thread de desenho atende os pedidos de **término** de leitura (que não gravam comandos na GPU) também em pontos
  seguros dentro das operações longas: entre pedaços do envio de textura (`CopySparseSerial`) e entre buffers no
  `BindBuffers`. O início de leitura continua só entre operações.
- Ganho: o passo 8 (10–60 ms) deixa de esperar um BindTextures de 20–55 ms. Risco: médio (só pontos auditados).

### Etapa 4 — Envios de textura fora do caminho da visibilidade (`SOTC_UPLOAD_QUEUE`)
- As cópias grandes da entrada de área (19–28 MB, ~55 ms; texturas de 64–128 MB "across chunks") vão para uma fila
  de cópia separada, com semáforo antes do primeiro desenho que usa a textura (sem pop-in: o desenho espera a
  textura, não o contrário).
- Ganho: a GPU deixa de ter dezenas de ms de cópia na frente do cull. Risco: médio (transferência entre filas).

### Etapa 5 — Só se a etapa 0 mostrar que compensa: cull numa segunda fila
- Segunda fila da mesma família gráfica com prioridade maior (sem privilégio), esperando só os ticks das entradas
  do cull. Só ajuda se as entradas (Hi-Z, pré-shader) ficam prontas bem antes do dispatch; a etapa 0 mede isso.
- Risco alto, semanas; na Vega 8 pode competir com o desenho. Fica por último e só com prova.

## Como aprovar cada etapa (sem perda)
- Imagem: `SOTC_RB_PUSH_VERIFY` com 0 diferenças; capturas lado a lado na tela de título, Santuário e floresta.
- FPS: Santuário parado, demo de abertura e rota de 60 FPS iguais ou melhores (Máxima + PS4 Pro, 60 FPS).
- Trocas de área: quadros > 50 ms e pior quadro na rota do usuário, antes e depois, com `tools/entry_report.py`.
- Estabilidade: uma sessão longa do usuário sem queda antes de virar padrão no lançador (DEFAULT_UPGRADES).

## Ordem
0 → 1 → 2 → (medir) → 3 → 4 → 5 só com prova. As etapas 1 e 2 juntas devem levar a maior parte do ganho.

## Fontes do estudo
- shadPS4 PR #3404 (descargas preventivas, nunca entrou), RFC #4316 (leituras em lote).
- RPCS3 `VKGSRender.cpp` (envio antecipado com limite), `texture_cache_predictor.h`; PR #4973 (ZCULL).
- Ryujinx: envio antecipado periódico e por troca de alvo (#3689, #4667, #4387).
- Xenia-canary PR #1325 (copiar de volta faixas inteiras apagou escritas da CPU: aplicar só o que mudou).
- NVIDIA "Vulkan Do's and Don'ts", AMD RDNA Performance Guide, VK_EXT_global_priority, Blender PR #165812
  (memória HOST_CACHED para leitura).
