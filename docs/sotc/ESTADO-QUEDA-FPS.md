# Queda de FPS: estado em 17/11/2026 e o caminho daqui

## 0. Revisão de tudo (23/11/2026) — ler antes de qualquer teste novo

Meta do usuário: **60 FPS estável e nenhuma travada na troca de área** (no PS5 não parece travar).
São dois sintomas diferentes, com causas diferentes:

- **Travada** = quadros de 100–320 ms (a imagem para). Sente-se como "travou".
- **Mergulho** = alguns segundos com quadros de 20–55 ms (contador da tela 37–45). Sente-se como "caiu o FPS".

### Estado agora (exe fa5edfc3 no E:, commit a0328659)
| Trecho (partida do usuário) | 23/11 09:33 | 23/11 10:20 |
|---|---|---|
| Depois da carga do save | mínimo 30, quadros 150–320 ms | **mínimo 43, um quadro de 95 ms** |
| Troca de área da floresta | 40–48 | **37–45 por ~4 s, pior quadro 55 ms** |
| Resto do caminho | 58–60 | 58–60 |

As travadas grandes acabaram (leitura antecipada dos arquivos só em HD). **Sobra o mergulho da floresta.**

### O que já foi tentado, por hipótese (não repetir)
**H1 — o jogo espera a GPU devolver o mapa de visibilidade (leituras de volta).** Medido: 48–68 ms de espera
por quadro no pior quadro do mergulho; a GPU não está cheia (~13 ms de trabalho). **14 tentativas, nenhuma
tirou o mergulho:** `SOTC_CLEAN_GUEST_READS` (pequena melhora, ruído), `SOTC_STALE_READS` (cena piscando),
`SOTC_RB_STREAM`, `SOTC_RB_EARLY`, Mailbox no lugar de FIFO, `SOTC_RB_HOT=2`, `SOTC_RB_GROUP` (etapa 1),
`SOTC_RB_PUSH` (etapa 2), `SOTC_RB_SAFEPOINT` (etapa 3), `SOTC_RB_NARROW=256`, `SOTC_CULL_QUEUE` (2 versões),
`SOTC_VIS_DIRECT` v1 (quedas), v2 (41 FPS), **v3 `SOTC_VIS_DIRECT_ORDER` (23/11 10:26: espera 20 → 12 ms, mas o
resto do caminho caiu para 50–55 e o mergulho seguiu em 37)**. **Linha esgotada** enquanto a causa for a
mesma: encurtar ou adiantar a leitura não muda o mergulho.

**H2 — a thread de desenho do port é o limite.** Testado: envio de texturas (etapa 1 do plano da thread, sem
ganho), `SOTC_COPY_LOCK_ONCE` (sem ganho), `SOTC_IDLE_FLUSH` (sem ganho), `SOTC_FLIP_QUEUE=2` (sem ganho).
Ganharam: `SOTC_TEX_DIRECT`/`SOTC_TEX_GATHER`, `SOTC_RELEASE_DEFER`, `SOTC_CLAMP_GAP` (trancos), e **tirar as
medições do lançador** (18/11: resto 54–57 → 59–60, troca de área 40–47 → 49–55). No pior quadro de 23/11 a
thread trabalha ~17 ms de 55 (o resto é espera). **Nunca feito, com pista medida:** juntar as chamadas de
proteção de páginas (`Protect`/`VirtualProtect`, ~16% da thread nos quadros lentos, 18/11 22:10).

**H3 — o que o port põe em cima do jogo (configuração).** Ganharam: FPS dinâmico ajustado (sem 10–15 s presos
em 48), cache de shaders mantido, pré-tradução de compute, leitura antecipada só em HD (23/11). Sem diferença:
sem modo Pro (o mergulho é igual: não depende de resolução).

### Armadilhas que já custaram horas
- Harness `idle.py`, `bench_shrine` e a demo não chegam ao trecho: números inválidos.
- O teste do final pelo harness (`I:/tmp/endrun/end.py`, menu de debug) cai em `0x1a2c0` **também sem
  mudança nenhuma** (23/11, 3 de 3 rodadas): não serve para provar estabilidade.
- Média de 5 s esconde o mergulho: comparar pela linha `SOTC SCREEN FPS` (lowest) e pelo pior quadro.
- **Medições ligadas pesam no jogo** (`SOTC_DRAW_SPLIT`, `SOTC_SLOW_OPS`, `SOTC_GPU_TIMES`, `SOTC_TICKWATCH`):
  só na partida que mede, depois tirar. As partidas de 23/11 tiveram `DRAW_SPLIT`/`SLOW_OPS`/`SYNC_COMPILE_LOG`
  ligados: o "resto do caminho" real pode estar melhor que o medido.
- Editar `launcher_settings.json` com o lançador aberto: ele regrava por cima ao fechar.

### Caminho daqui (em ordem, uma partida cada)
1. **Partida limpa** (só o padrão do lançador, sem medições) para saber o estado real do mergulho com a
   correção de hoje. Barato e evita otimizar o que já não existe.
2. **H2: juntar a proteção de páginas** (a única pista medida e nunca feita na thread de desenho).
3. Só se 1–2 não bastarem: a parte estrutural (gravar em várias threads, `PLANO-THREAD-DESENHO.md` opção C),
   cara e arriscada.

O problema, nas palavras do usuário: **o FPS cai bruscamente** na troca de área da floresta (correção do usuário em 18/11: o trecho que eu
chamava de "floresta" é a entrada da floresta).
Enquanto ele cair, nada melhorou. O único critério que vale: **na partida do usuário, nesses trechos, o FPS
não cai.**

## 1. Regras de teste (para não perder horas)

### Proibido
- **Harness "parado" (`I:/tmp/dense/idle.py`)**: nunca chegou ao save. Parava na tela de título ou na demo,
  porque a linha `st_gameflow() starting game` também aparece no título. Todo número tirado com ele em 17/11
  (`gt1`, `if1`, `dep1`, `dep2`, `idle1`, `idle2`) é da tela de título (~480 comandos/quadro, 90–130 FPS), não
  do floresta (5–8 mil comandos/quadro, 44–48 FPS). Só valeu para ver se o exe abria.
- **Harness "pulando" (`tools/sotc_bench_shrine.py` com o save-template)**: o save não começa no floresta; o
  script apertava X sem parar e ficou num portão. Não representa o problema.
- **Demo de abertura (`gather_ab.sh`, `cold_run.sh`)**: a intro não tem as trocas de área nem os trechos densos.
- Rodar qualquer teste com o jogo do usuário aberto, ou abrir o jogo no PC dele sem avisar.

### Como medir (o único jeito que vale)
1. Eu troco as variáveis em `E:\GAMES\Shadow Of The Colossus\SotC-PC\user\launcher_settings.json` (só com
   `SotC.exe`/`bb-probe.exe` fechados, backup em `I:/lixo-sotc-05-11/`), instalo o exe com backup e abro o
   lançador. Aviso "instalado HH:MM" e o que mudou.
2. O usuário joga **2–3 minutos** no trecho que cai (floresta) e fecha.
3. Antes de olhar qualquer número no `user/last_run.log`, conferir que a partida é a certa:
   - `md5sum` do `bin/bb-probe.exe` = o exe que eu instalei;
   - linhas `SOTC FPS ... cmds/frame` com **3.000+ comandos/quadro** (floresta denso: 5–8 mil). Com ~500 é
     título ou demo: não vale;
   - a chave testada aparece ativa no log (ex.: `SOTC CULL QUEUE: on`).
   - **como a sessão terminou**: "O jogo caiu"/"Queda em" no `user/events.log` ou `Crash dump` = caiu.
     "Jogo parado pelo lançador" é o usuário fechando pelo lançador (aparece em partidas normais). Na dúvida,
     perguntar ao usuário.
   - **"queda" do usuário = queda de FPS.** A queda que importa é a da **troca de área da floresta, sempre no
     trecho final do teste**: comparar as últimas janelas de 5 s de cada partida, não médias da partida toda.
4. **O número que o usuário vê é o contador da tela** (média dos últimos ~20 quadros, `bbport_overlay.cpp`).
   Desde 18/11 22:16 o log tem a linha `SOTC SCREEN FPS <s>: lowest X, now Y` a cada segundo: **comparar pelo
   "lowest" dessa linha no trecho da troca de área**, não pela média `SOTC FPS` de 5 s.
5. Resposta curta ao usuário: **FPS nos mesmos trechos antes × depois** (as janelas de 5 s com 5+ mil
   comandos/quadro), quadros > 50 ms, queda/crash. Subiu ou não subiu.
5. Uma mudança por partida. Se não subir, desligar e seguir para a próxima; sem rodadas extras de medição.

### Referência "antes" (partidas do usuário, 17/11, exe sem as mudanças de FPS)
Trechos do floresta com 5–8 mil comandos/quadro: **44–48 FPS** (21:59, 22:04, 22:19, 22:23, 22:30, 22:38, 22:47);
nos outros trechos 57–60.

### Harness que ainda vale
- `I:/tmp/ride/ride.py` (passeio automático `setPlayerPos`, save de 15 derrotados): carrega o save de verdade
  (conferido: plano termina, ~14 mil quadros). Serve para trancos de troca de área com cache frio/quente,
  **não** para o floresta. Usa `RetailBuild=0` (menu de debug): 1 queda `0x1a2c0` em 5 rodadas (família T41).

## 2. O que foi medido (partidas do usuário; válido)

| Medida (trechos densos do floresta, 44–48 FPS) | Valor |
|---|---|
| GPU por quadro (`SOTC_GPU_TIMES`) | ~10 ms desenhos/dispatches, ~3–4 ms barreiras/cópias, **~6–9 ms parada entre envios** |
| GPU ocupada (`SOTC_TICKWATCH`) | 71–76% (15–17 ms por quadro) |
| Threads do jogo (`SOTC_PROFILE=*`) | 13,5% no jogo, ~35% girando em travas, ~24% esperando leitura da GPU |
| Espera de leitura do jogo | 20–27 ms por quadro somados; quase tudo "gpu" (fila do port ~1 ms, envio 0,3, aplicação ~1,5) |
| Cálculo de visibilidade `0x50784801` (30–86 por quadro) | começa **~10 ms de GPU depois** de suas entradas estarem prontas; maior por quadro 14–24 ms |
| Threads do port | desenho ~17,6 ms, processador de comandos ~14,5 ms por quadro de 22 ms: não são o limite |

**Causa:** o jogo espera, a cada quadro, o mapa de visibilidade que a GPU calcula no mesmo quadro; esse cálculo
fica na fila atrás do desenho do quadro anterior. A GPU termina, para, o jogo recebe e só então manda o resto.

## 3. O que foi feito (nada commitado além do `SOTC_TEX_GATHER`)

| Mudança | Chave | Resultado | Estado |
|---|---|---|---|
| Texturas juntadas pela GPU | `SOTC_TEX_GATHER` | entrada de área 7–9 → 1 quadro > 50 ms | **commit `fc20cf11`**, padrão no lançador |
| Pré-tradução dos shaders de compute | `SOTC_COMPUTE_PREFETCH` (padrão 1) | trancos de shader na 1ª visita (passeio frio > 50 ms 94 → 67); **não mexe na queda para 48** | working tree |
| Cache de shaders mantido entre atualizações | lançador `SHADER_CACHE_VERSION`; `SOTC_CACHE_CLEAR_EVERY_UPDATE=1` volta | 7 casos testados; só chega com `SotC.exe` novo | working tree |
| Registro de traduções no quadro | `SOTC_SYNC_COMPILE_LOG=1` | diagnóstico | working tree |
| Tempo da GPU por etapa, causas, dependências | `SOTC_GPU_TIMES=1` (`sotc_gpu_times*.h`) | diagnóstico (tabela acima) | working tree |
| Envio quando a thread de desenho fica sem trabalho | `SOTC_IDLE_FLUSH` (padrão 0) | **sem ganho** (46–55 FPS, igual) | working tree, desligado |
| Visibilidade numa segunda fila | `SOTC_CULL_QUEUE` (padrão 0), `_VERIFY` | escrito por outra sessão; 0 diferenças na comparação **só na tela de título**; **nunca testado no floresta** | working tree, desligado; exe pronto em `out/bb-probe.exe` (23:30) |
| Leitura entregue (etapa 2 do plano da floresta) | `SOTC_RB_PUSH` | derrubou a GPU no jogo | patch guardado `I:/tmp/rb_push_work.patch`, fora do código |

Instalado no E: agora: exe de teste das 22:46 (medições, sem a fila). Oficial 0.76.2 em
`I:/lixo-sotc-05-11/bb-probe-0.76.2-oficial-1841.exe`.

## 4. O caminho mais eficiente

São dois problemas com causas diferentes; cada um tem a sua ordem.

### A. Trechos densos a 44–48 FPS (o que o usuário sente)

**Resultado do passo 1 (18/11): a fila separada não subiu o FPS; desligada e parada.** O que sobra para a
entrada da floresta é a ordem do plano (B abaixo), começando pela etapa 1.

A causa medida é a espera da GPU (~10 ms por cálculo de visibilidade). As etapas 1–3 do
`PLANO-TROCA-DE-AREA.md` cortam fila do port, envio e aplicação, que aqui somam ~3 ms dos 20–27: **sozinhas
não tiram a queda para 48.** O que ataca a causa é a fila separada, que já está escrita e compilada.

1. **Teste decisivo da fila separada** (uma partida de 2–3 min no floresta): instalar `out/bb-probe.exe` (23:30)
   com `SOTC_CULL_QUEUE=1 SOTC_GPU_TIMES=1`.
   - Se o FPS nesses trechos subir e não houver erro de imagem (grama, cavalo, capa, escadas): ampliar para os
     outros culls (`0xc54081d1`, `0x4defcf7f`; depois os que leem a imagem Hi-Z) e fazer a sessão longa + final
     do jogo antes de virar padrão.
   - Se não subir: desligar, voltar o exe, registrar e parar essa linha (fica no código, desligada).
2. Só depois: corte do custo do port nas cópias (~1 ms recuperável).

### Resultado da etapa 1 (18/11 21:09, `SOTC_RB_GROUP`)
Partida do usuário na entrada da floresta, comparada por faixa de carga com as de 17/11:

| Partida | 4–6 mil comandos/quadro | 6 mil+ |
|---|---|---|
| 17/11 22:30 / 22:38 / 22:48 | 56,3 / 57,4 / 53,6 FPS | 46,9 / 50,3 / 44,8 |
| 18/11 21:02 (fila separada) | 49,0 | 40,8 |
| 18/11 21:09 (etapa 1) | 55,7 | 41,9 |

**Não subiu.** Agiu (300–1.100 leituras por 5 s trouxeram as outras faixas do mesmo shader), mas trouxe muito
mais dados (até 213 MB a cada 5 s: esses shaders escrevem faixas grandes) e as esperas do jogo seguiram em
12–30 ms de GPU por quadro. Desligada (exe das 22:46 de volta no E:); código no working tree com padrão ligado:
**pôr o padrão em 0 antes de qualquer commit**.

Leitura dos números: no trecho mais pesado (6 mil+ comandos) a GPU já fica ocupada 15–17 ms por quadro e o jogo
espera por ela dentro do quadro. As etapas 1–3 só cortam a parte do port na espera (fila ~1 ms, envio 0,3,
aplicação ~1,5). O que sobra para ganhar é o custo de emulação na GPU (barreiras/cópias ~3–4 ms, ~1 ms
recuperável) e a sobreposição CPU/GPU, que a fila separada não conseguiu.

### Resultado da etapa 2 (18/11 21:23, `SOTC_RB_PUSH=1`)
Reaplicada do patch `I:/tmp/rb_push_work.patch` no código atual, com a causa provável do "device lost" de
17/11 corrigida: a cópia entregue ficava num envio ainda não mandado e a liberação adiada desvinculava a memória
antes dela rodar (`PushTickInRange` em `ReleaseBlocks`: espera a cópia antes do unbind; 0 esperas nesta partida).
Partida na entrada da floresta: sem crash ("Jogo parado pelo lançador" também aparece na partida de referência de 17/11 22:48: é o usuário fechando pelo lançador). **4–6 mil comandos 50,7 FPS, 6 mil+ 45,0: não subiu.**
~92% das cópias ficaram velhas antes de o jogo ler (outra passada reescreve), o `0x50784801` foi desligado
sozinho (o jogo leu 3–6 de cada 1.000 cópias dele), até 35 mil cópias a cada 5 s. Desligada (exe das 22:46 de
volta); código no working tree, só liga com `SOTC_RB_PUSH=1`.

### Trecho final (a troca de área da floresta), 17–18/11

| Partida | FPS nas últimas janelas de 5 s |
|---|---|
| 17/11 22:38 (referência) | 46,4 · 46,5 · 47,3 |
| 17/11 22:48 (referência) | 41,7 · 46,3 · 45,7 · 45,2 |
| 18/11 21:02 fila separada | 39,3 · 41,1 · 40,8 · 39,3 · 40,0 |
| 18/11 21:09 etapa 1 | 44,0 · 42,9 |
| 18/11 21:23 etapa 2 | 45,0 · 45,0 |
| 18/11 21:37 etapa 3 (`SOTC_RB_SAFEPOINT=1`) | 48,7 · 40,4 · 42,1 · 44,4 · 44,0 |

**Etapa 3** (`SOTC_RB_SAFEPOINT=1`, padrão desligado): a aplicação das leituras prontas tem fila própria
(`Liverpool::SendFinish`/`ServeFinishes`) e roda também entre os buffers de um bind e entre o bind de buffers e o
de texturas (só as limpas: suja, dependente ou da fila separada espera o ponto normal). Agiu (100–650 aplicações
por 5 s no meio do bind), sem queda; **o trecho final não subiu.** Desligada.

**Diagnóstico sem o modo PS4 Pro (18/11 21:47, só para medir):** trecho final 51,9 · 49,7 · **40,5 FPS** — a
mesma queda; GPU ocupada 76–83% (14–19 ms por quadro), igual ao Pro. Menos resolução não muda: o tempo da GPU
nesse trecho não depende da resolução, e sim da quantidade de comandos (5–8 mil por quadro), dispatches e
barreiras. Pro religado depois do teste.

**Quem a GPU espera (18/11 21:59, `SOTC_TICKWATCH`, linha nova `SOTC GPU IDLE BY`):** no trecho final (44,4 ·
43,3 FPS) a GPU fica parada 6,7–7,1 ms por quadro e, nesse tempo, **a thread de desenho estava ocupada 6,2–6,6
ms**; processador de comandos 0,0; o jogo (os dois ociosos) 0,4–0,5. **O gargalo da queda é a thread de desenho
do port** (gravar 5–8 mil comandos por quadro), não o jogo nem a GPU. Caminho: `PLANO-THREAD-DESENHO.md`
(reduzir o custo por comando da thread de desenho); próximo passo: perfil só da thread de desenho no trecho final.

**Medições desligadas (18/11 22:06, mesmo exe, variáveis extras só `SOTC_TEX_GATHER=1`):** o perfil da thread de
desenho nos quadros lentos (`SOTC_PROFILE=GpuDraw SOTC_PROFILE_SLOW_MS=20`) mostrou as medições pesando nela
(`Ticks`/`SplitTimer` do `SOTC_DRAW_SPLIT`, contadores do `SOTC_SLOW_OPS`, mapas do `SOTC_GPU_TIMES`). Sem elas:
meio do caminho 54–57 → **59–60 FPS**; troca de área 40–47 → **49–55 FPS** (6–9 mil comandos/quadro). O ganho é
tirar a nossa própria medição do jogo do usuário (outros jogadores não têm essas variáveis). **Regra: medições
só na partida que mede; depois, tirar do lançador.** Sobra a queda a ~49 no ponto mais pesado (9 mil comandos):
seguir aliviando a thread de desenho (próxima pista: o "mostrar FPS" liga `SOTC_FPS`, que ativa cronômetros
internos `kFpsLog` nessa thread).

**Perfil limpo (18/11 22:10, só `SOTC_TEX_GATHER=1` + perfil da thread de desenho nos quadros > 20 ms):**
trecho final 56,4 · **53,4** FPS e o resto 56–60; **o FPS dinâmico não caiu para 48 nenhuma vez** (nem na partida
de 22:06; nas partidas com as medições ligadas ele travava em 48). Nos quadros lentos que sobram, a thread de
desenho gasta **~16% na proteção de páginas** (`Protect`/`VirtualProtect`, vigiar o que a GPU escreve) e ~20%
esperando a hora do flip (`SubmitFlip`, ritmo, não trabalho; o `SOTC GPU IDLE BY` contava isso como "ocupada").
Próximo alvo real: juntar as chamadas de proteção de páginas.

**O que o usuário vê (18/11 22:15): a média de 5 s esconde os mergulhos.** Nas mesmas partidas, na troca de
área, há quadros de 30–49 ms (1% mais lento 24–32 FPS) — é isso que o contador da tela mostra. **Comparar por
quadro (linha `SOTC FRAMES`: p99, 1% low, pior), não pela média `SOTC FPS`.** Nesses quadros o port quase não
trabalha (buffer 1–3 ms, dma 1–8 ms) e o processador de comandos fica **ocioso 17–30 ms esperando o jogo** mandar
comandos: o tranco vem do lado do jogo na troca de área (threads do jogo esperando leituras da GPU e travas, ver
seção 2). Próximo diagnóstico: o que as threads do jogo fazem exatamente nesses quadros (perfil de todas as
threads só nos quadros > 25 ms).

**Perfil de todas as threads disparado pela queda (18/11 22:20, `SOTC_PROFILE_TRIGGER_FPS=50`, novo em
`src/probe.c`; disparou a 36,9 FPS na troca de área):** threads de trabalho do jogo 57% dentro da leitura de volta
(`ReadMemory`: ~29% esperando a GPU, ~8% esperando a thread de desenho atender o pedido, ~11% tratando a falta de
página), ~24% girando em travas, 13% no jogo; threads principais do jogo esperando essas. Thread de desenho **não**
saturada: ~36–47% trabalhando, ~16% sem trabalho, **~13% bloqueada no `SubmitFlip`** (esperando a apresentação
aceitar o quadro anterior: `SOTC_FLIP_QUEUE`, padrão 1). Processador de comandos quase todo parado. Correção do
que eu disse antes: o "thread de desenho ocupada" do `SOTC GPU IDLE BY` incluía essas esperas. Próximo teste:
`SOTC_FLIP_QUEUE=2` (só variável; +1 quadro de atraso no máximo).

**`SOTC_FLIP_QUEUE=2` (18/11 22:24):** tela na troca de área 41 · 42 · 47 · 47 (referência 45 · 40 · 41): **não
ajudou**; tirado.

**GPU na queda (18/11 22:28, `SOTC_GPU_TIMES=1 SOTC_GPU_TIMES_EVERY=10`, dependências agora só com
`SOTC_GPU_TIMES_DEPS=1`):** quedas na tela aos 23–39 s (39–48, FPS dinâmico travou em 48) e 82–83 s (39). Por
quadro: a 60 FPS, GPU 16–17 ms do 1º ao último comando (~11–12 de trabalho, ~5 parada); na queda 18–20 ms (~13
de trabalho, 5–7,5 parada). **A GPU não está sobrecarregada na queda.** Leituras de volta 1.100–1.500/s na queda
contra 700–850/s a 60. Retrato: na troca de área o jogo faz mais idas e voltas CPU→GPU→CPU, uma atrás da outra, e
as threads dele esperam em série (leituras + travas). As tentativas de 18/11 encurtaram pedaços de cada ida e volta,
não o número delas.

**Fila separada cobrindo faixas novas (18/11 22:38, `SOTC_CULL_QUEUE=1`, tabela de páginas
`SOTC_CULL_QUEUE_PAGES`):** harness com `SOTC_CULL_QUEUE_VERIFY=1`: "learning" 0 (antes 4500–7300/5 s), ~10 mil
dispatches na fila nova a cada 5 s, 0 diferenças em ~30 mil comparações, sem queda. Partida do usuário: tela na
troca de área **38 · 39 · 42** (referência 40–45): **não ajudou** — as leituras do jogo seguiram quase todas pela
fila gráfica (113–488 a cada 5 s pela fila nova, de ~5000). Desligar.

**FPS dinâmico prolonga a queda:** depois de cada mergulho a tela fica **48 cravado por 10–15 s** (40–56 s e
96–108 s nesta partida): `SOTC_FPS_DYNAMIC` trava em 48 após 1,5 s abaixo de 50 e só tenta 60 a cada 10 s. Na
partida de 22:16 (sem a trava) o mergulho durou ~3 s e voltou a 60. Teste barato: `fps_dynamic` desligado.

**FPS dinâmico desligado (19/11 15:12, `fps_dynamic: false`, só `SOTC_TEX_GATHER=1`):** sem travar em 48 em
nenhum momento. Depois da carga do save 44–56 por alguns segundos e volta a 58–60; troca de área 56 → **39 · 42**
→ 52 → 58 → 60 a partir de 87 s. O mergulho (~2 s) continua, mas acaba ali; com o FPS dinâmico ligado o mesmo
mergulho virava 10–15 s a 48. Pendente: decidir se o FPS dinâmico sai do padrão do lançador (ou se ele passa a
travar só depois de uma queda longa e a tentar 60 de novo mais cedo), e o mergulho de ~2 s em si.

**FPS dinâmico ajustado (19/11 15:16, `driver.cpp`):** trava em 48 só depois de **5 s** acumulados abaixo de
50 FPS (`SOTC_FPS_DYNAMIC_SLOW_MS`, era 1,5 s) e tenta 60 de novo depois de **4 s** (`SOTC_FPS_DYNAMIC_HOLD_S`,
era 10 s), dobrando a cada tentativa que falha até 10 s. Partida do usuário com o FPS dinâmico ligado: **nenhuma
trava em 48**; troca de área 40 · 40 · 40 · 46 · 48 · 48 · 54 → 60 a partir de 108 s; resto 57–60. Sobra o
mergulho em si (~3 s em ~40 e ~3 s voltando). No exe de teste; não commitado.

**Janela de leitura 256 KiB (19/11 15:22, `SOTC_RB_NARROW=256`, só variável):** troca de área 45 · 37 · 39 · 43
→ 60 em ~7 s (antes 40 · 40 · 40 → 60 em ~7 s); leituras ~1000/s no mergulho (antes 1100–1400); resto 59–60.
**Não ajudou**: o número de idas e voltas não é o que limita. Tirado.

**Onde está o mergulho (resumo 19/11):** ~3 s em ~37–40 FPS na tela e ~4 s voltando, na troca de área da
floresta. Nesses quadros o port quase não trabalha (buffers/cópias 2–8 ms) e espera o jogo 20–36 ms; as threads do
jogo esperam leituras da GPU e travas entre si; a GPU não está sobrecarregada (~13 ms de trabalho). Testado sem
ganho: fila separada (2 versões), etapas 1–3, `IDLE_FLUSH`, `FLIP_QUEUE=2`, janela 256 KiB, sem modo Pro. Ganho
real: tirar as medições do lançador (resto do caminho 54–57 → 59–60) e o FPS dinâmico ajustado (sem 10–15 s presos
em 48).

**Conclusão das 4 tentativas de 18/11:** nenhuma mudança no caminho das leituras (fila separada, etapas 1–3)
mexeu na queda da troca de área da floresta. O port já entrega as leituras em ~1–3 ms; o que segura o trecho
final é a GPU ocupada 15–17 ms por quadro com o jogo esperando por ela dentro do quadro (seção 2).

### B. Trancos nas trocas de área (floresta)
Aqui fila e aplicação pesam (fila 1–20 ms, aplicação 10–60 ms em 03–05/11), então vale a ordem do plano:
**etapa 1** (`SOTC_RB_GROUP`, agrupar leituras por shader) → **etapa 2** (consertar o `SOTC_RB_PUSH` a partir
do patch; antes, descobrir por que derrubou a GPU) → **etapa 3** (`SOTC_RB_SAFEPOINT`). Cada uma: uma partida do
usuário na rota da floresta, mesma regra da seção 1.

### Antes de qualquer commit
Revisar o working tree inteiro (19 arquivos), separar em commits por assunto (prefetch + lançador; medições;
fila separada desligada), e só com OK do usuário.
