# Changelog

## 0.60.6-opt11 — 2026-10-24 (fork do Oussama268)
- **+25% de FPS no Santuário e quadros mais estáveis:** as leituras antecipadas da GPU
  ("hot readbacks", da fusão de 8/10) voltam ligadas por padrão (`SOTC_RB_HOT=1`; `=0` desliga).
  Antes de escrever, cada página é conferida sob a trava da sua região: se o jogo já escreveu
  nela, a cópia é pulada. Era a corrida que deixava o arco-íris nas texturas na 0.60.5.
  Santuário 34,6 → 43,3 FPS, demonstração da tela título 54 → 60,7.
- Descritores reaproveitados ligados por padrão (`SOTC_FAST_BIND`, `=0` desliga).
- Correções do tabnul: grama, HDR, menu do DLSS, pular os logos (#91, #92, #94, #96).
- Lançador: aviso de pouca memória (#95).
- Do upstream #258 (JihedMrouki): o cache de shaders confere se o arquivo existe, dica de
  trabalho na fila do Liverpool, posição invariante nos shaders de vértice (GTX 10xx).
  **O cache de shaders é refeito na primeira abertura.**
- Diagnóstico: `SOTC_PROFILE_SLOW_MS=<ms>` só guarda amostras dos quadros mais longos que isso.
- A travada ao cavalgar continua: nos quadros lentos as threads do jogo esperam umas pelas
  outras (mutex e event flags do próprio jogo) e há cópias de ~200 MB nas trocas de cena.

### Versão americana (CUSA08034)
- **O dump americano CUSA08034 (1.00, a única versão americana) funciona.** O executável é o
  mesmo da versão europeia 1.01 (código e dados idênticos byte a byte; os arquivos `.psarc`
  também), então todas as correções, os modos de patch (60 FPS, filtro 16x, tamanho de
  renderização) e a proteção da red zone valem para os dois. Só os logos e telas legais mudam
  (o jogo escolhe a região pelo `gameversion.txt`). Testado até o segundo colosso.
- As correções da GPU antes restritas ao CUSA08809 (barreiras LDS, wave64, tabelas de T#,
  testes de profundidade antecipados dos decalques, flush periódico) agora valem para os dois
  seriais (`IsSotc()`). Sem os testes antecipados, os decalques atrás de um objeto pintavam a
  caixa inteira por cima dele (bordas e texturas do fundo na frente). O log mostra
  `SOTC early fragment tests: on`.
- **Saves separados por versão** (`user/savedata/1/CUSA08034`) e cache de shaders próprio
  (`user/cache/CUSA08034`). **DLCs só da mesma região** (pasta `addcont/CUSA08034`, content ID
  `UP9000-CUSA08034_00-...`).
- **Lançador:** acha o dump americano pelo `TITLE_ID` do `param.sfo`, com qualquer nome de pasta
  (como `CUSA08034-app`), mostra "✓ CUSA08034 versão 01.00: todos os modos disponíveis" e não
  marca mais o dump como não suportado. Com os dois dumps, o europeu vem primeiro.
- Ferramentas (`tools/`) e o modelo de save dos benchmarks aceitam os dois seriais. Testes novos
  (`tests/test_games.py`) falham se algum código voltar a testar um serial só.
- A versão americana não tem os 3 textos em árabe da atualização europeia 1.01.

## 0.60.6 — 2026-10-22
- **Fim do ruído arco-íris nas texturas e das quedas "do nada" (`0x49500`).** A causa eram as
  leituras "quentes" da GPU (`SOTC_RB_HOT`): junto com cada leitura, o port escrevia na memória do
  jogo, por especulação, regiões lidas antes; depois de carregar um save ou uma área nova o jogo já
  tinha reaproveitado essa memória para texturas novas e para os seus heaps, e o desenho antigo da
  GPU caía por cima. Agora vêm desligadas (o FPS médio do demo cai ~4%). Achado com a receita do
  usuário: área nova → esperar → sair → recarregar o save.
- **Pontos pretos em grade no chão de areia** (0.60.5): a otimização de binds da PR #232 sai
  desligada por padrão (`SOTC_FAST_BIND=1` liga; a correção dela aguarda teste).
- **PR #219 (vazamento de memória) desligada por padrão**: com ela, ruído colorido e polígonos
  pretos gigantes ("explosão de vértices") apareciam com muito mais frequência
  (`SOTC_CP_FREE_TASKS=1` liga).
- **Menu do jogo (F1):** só o que funciona no SotC (as opções de upscaler, preset, resolução e
  efeitos herdadas do Bloodborne não faziam nada); **FPS (30/60/ilimitado), Upscaler e Nitidez**
  mudam na hora; textura **Máxima** desativada no modo PS4 Pro (fechava em minutos); dica explica
  o que vale na hora.
- **Disco rígido:** depois que o jogo carrega, os arquivos do jogo (13,7 GB) são lidos uma vez em
  ordem e ficam na RAM: lugares novos não travam mais esperando o HD (`SOTC_PREFETCH=0` desliga).
- **Carregamentos:** a cada save ou área nova, as texturas sem uso são descartadas
  (`SOTC_LOAD_FLUSH=0` desliga).
- **Log de eventos `user/events.log`:** configuração por extenso, sistema (CPU, RAM, HD/SSD), menu,
  teclas, controle, carregamentos, texturas, desempenho por minuto, erros explicados; idioma do
  lançador; as 5 últimas sessões e uma cópia em `user/crashes` após uma queda.
- PRs: #215 (pré-carga de pipelines nos workers: driver novo 106 s → 7 s), #198 (CPUs de 4
  threads), #205 (barreiras LDS na AMD GCN), #207, #212.
- Revisão de código: guarda dos heaps do mundo, vigia de travamento, memória em PC com pouca RAM,
  "Ilimitado" do lançador preservado, tela de carregamento translúcida em engasgos curtos.

## 0.60.5 — 2026-10-21
- **Seletor de qualidade das texturas: Desligada / Alta / Máxima** (#235), no lançador
  (Ajustes rápidos → Texturas) e no menu do jogo (muda na hora). **Alta (estável)**, o padrão,
  mantém a proteção da 0.60.3/0.60.4 (texturas um nível mais grossas quando a memória de texturas
  do jogo está acabando); como essa memória fica quase sempre cheia nas áreas grandes, a 0.60.4
  ficava menos nítida que a 0.60.3. **Máxima (nítida)** tira a proteção: sempre o nível mais fino,
  mas **pode fechar em vistas muito amplas** (espada contra o sol, Santuário, área do último
  colosso; `0x1a371`, #186), principalmente no modo PS4 Pro; se acontecer, volte para Alta.
  **Desligada** = só as versões 256×256 do jogo. A antiga caixa "Texturas em alta qualidade" vira
  Alta (ou Desligada).
- **Vazamento de memória** (~60 MB por minuto, mais de 7 GB em meia hora; engasgos em sessões
  longas): cada buffer de comandos aninhado deixava a sua corrotina na memória (PR #219,
  Oussama268).
- **Mais FPS:** os descritores do desenho anterior são reaproveitados quando o desenho seguinte usa
  o mesmo pipeline e os mesmos recursos (PR #232, Tadomikaari312-Hy): demo +3,3% na média, +12% na
  mínima (72,8 / 37,1 → 75,2 / 41,7). `SOTC_FAST_BIND=0` desliga.
- **Carregamento depois de atualizar o driver da placa: 106 s → 7 s** (os pipelines guardados são
  montados pelas threads auxiliares; PR #215, Oussama268). `SOTC_ASYNC_PRELOAD=0` volta ao antigo.
- **AMD GCN** (RX 470–590, Vega, APUs): barreiras de memória compartilhada que essas placas não
  recebiam, provável correção dos personagens e da fumaça brancos (PR #205, Lucas-blip-png; teste
  em placas GCN pelos jogadores). `SOTC_AMD64_LDS=0` desliga.
- **CPUs com 4 threads:** o gravador de comandos e o processador de comandos não ocupam mais um
  núcleo inteiro esperando (preso no carregamento → 73 FPS; PR #198, Lucas-blip-png).
- **Lançador:** não para quando o console recusa escrita (#104, PR #207); dump ou módulos ainda
  criptografados dizem qual arquivo é (PR #212).
- **Tela de carregamento:** engasgos curtos no jogo só escurecem a imagem; fica opaca depois de 3 s
  sem quadro (antes piscava preto em travadas de 1 s).
- **Vigia de travamento:** depois de 15 s sem quadro do jogo, as pilhas das threads vão para o log
  (`SOTC_WATCHDOG=0` desliga), para achar o "fechou do nada".
- **Revisão de código:** a proteção dos heaps do mundo pulava a guarda nas peças seguintes do heap
  e deixava peças novas sem guarda por até 2 s; PCs com pouca RAM recebiam o tamanho da memória
  antes da redução; o botão 60 FPS do menu trocava "Ilimitado" por "60".
- Menu do jogo e atalhos (F1, F7, F8, F10, F11) explicados no README, com imagem.

## 0.60.4 — 2026-10-20
- **Blocos ciano nas pedras e flores piscando** (com "Texturas em alta qualidade"): o ajuste da
  0.60.3 que evita a queda trocava o nível de detalhe das texturas a cada um ou dois segundos nas
  áreas grandes (4, 3, 2, 3, 2…), e cada troca fazia o jogo descartar ou recarregar texturas que
  estavam sendo desenhadas. Agora ele só baixa o detalhe com menos de 8% de memória livre e só
  volta acima de 16%. Testado do Novo Jogo+ até o 2º colosso: 3 trocas em vez de ~8, sem blocos
  ciano, sem flores piscando e sem quedas.

## 0.60.3 — 2026-10-20
**O jogo é zerável do início ao fim.** Jogado do save antes do último colosso até a luta com o
Malus, as cenas finais e os créditos, com "Texturas em alta qualidade", modo PS4 Pro e 60 FPS
ligados. Obrigado a **Lucas-blip-png**.

- **Queda com "Texturas em alta qualidade" em vistas amplas** (espada erguida contra o sol, área
  do último colosso; `0x1a371`, #186): quando a memória de texturas do jogo começa a acabar, as
  texturas vistas pedem um nível de detalhe a menos (dois quando está quase cheia) e o jogo libera
  espaço antes de travar. Continua mais nítido que com a opção desligada (PR #191, Lucas-blip-png).
- **Queda no carregamento depois dos créditos** (`0x49500`): a proteção da memória do jogo não
  enxergava os reservatórios que o mundo cria, porque eles crescem em pedaços de 1 MB espalhados;
  agora cada um é protegido onde estão os seus blocos livres (#146).
- **+2,7% de FPS no templo:** leituras da GPU em janelas de 64 KB (PR #196, Lucas-blip-png).

## 0.60.2 — 2026-10-20
- **Ajuste rápido dentro do jogo:** no menu (Insert, F1 ou L3+R3), sem sair do jogo: 60 FPS,
  geração de quadros (desligada / DLSS-G / FSR 3), texturas em alta qualidade, reaproveitar
  sombras distantes e volume. Valem na hora e ficam salvos no lançador para a próxima vez. AF 16x,
  modo PS4 Pro, resolução e uso de memória de vídeo continuam no lançador (precisam reiniciar).
- **Mouse no menu:** com o menu aberto o mouse fica solto e visível para clicar; ao fechar, volta
  a controlar a câmera.
- **Tela de carregamento animada também na montagem dos shaders** ao abrir o jogo (antes a janela
  ficava parada, às vezes por minutos depois de atualizar o driver), com fundo preto opaco e
  animação no centro (círculo girando, título pulsando, "Carregando…").
- Menu com título e Ajuste rápido no idioma do lançador.

## 0.60.1 — 2026-10-20
Obrigado a **nosekefik, Oussama268, plassardewen-cpu e lunar-me**.

- **Menu dentro do jogo** (Insert, F1 ou L3+R3): FPS, placa e memória de vídeo, tela cheia,
  resolução, escala e nitidez, câmera e mouse, efeitos (névoa, vinheta, oclusão, reflexos da
  água). Era o menu do Bloodborne, escondido no port; agora é do SotC e vem ligado
  (`BB_OVERLAY_MENU=0` desliga). Textos em inglês (PR #94, nosekefik).
- **Sobras do Bloodborne removidas** (~11.700 linhas: lançador antigo, patches e mods do
  Bloodborne, ícones, documentos), a partir da PR #60 do Oussama268. O executável do jogo
  (`bb-probe.exe`) passa a ter o ícone do SotC.
- A busca pelo jogo ignora a pasta `WpSystem` do Windows (PR #110, plassardewen-cpu).
- Página do GitHub só em inglês.

## 0.60 — 2026-10-20
Obrigado a **Lucas-blip-png, Einar e KingJhoss**.

**Correções**
- **Texturas nas GTX 1060/1070/1080 (Pascal):** superfícies sem textura, cores chapadas ou quase
  pretas (#57, #68, #102). A Pascal não tem o recurso de interpolação que o jogo usa; ele é
  imitado com um geometry shader (PR #169, Lucas-blip-png).
- **Jogo travando ou fechando na tela de título** em alguns PCs (núcleos da CPU com relógios
  diferentes): o relógio do jogo passa a vir do Windows (PR #174, Einar).
- **Não abria em PCs com pouca memória** (regressão da 0.59): se o Windows recusar os 9152 MB,
  o jogo usa o tamanho do console e avisa no registro.
- **FSR 3: quadrados pretos nos quadros gerados** em AMD RX 7000 (PR #177, KingJhoss).
- **Botão JOGAR cortado** com escala do Windows acima de 100% (PR #171).
- Correção no código herdado do shadPS4: mínimo/máximo atômico com decimais em placas sem
  `VK_EXT_shader_atomic_float2` executava duas operações em vez de uma.

**Novidades**
- **Modo 60 FPS usando o modo Desempenho do próprio jogo** (o do PS4 Pro): o jogo passa a mirar
  60 FPS, então o tempo dele acompanha. Deve corrigir física errada em 60 FPS (#100, #120); ainda
  em teste nesses colossos (PR #183, Lucas-blip-png). `SOTC_MODES=af16` volta ao modo antigo.
- **Cada cópia do port tem a própria pasta `user`** (saves, cache, registros e configurações do
  lançador em `user\launcher_settings.json`). Antes todas usavam a pasta da primeira cópia. Na
  primeira vez, a cópia nova herda as configurações e **copia** os saves e módulos de som.
- **+6% de FPS no templo:** a proteção da memória do jogo não é mais refeita a cada quadro
  (PR #168, Lucas-blip-png).
- Geração de quadros sem a interface (barra de vida sem tremer), **experimental e desligada**:
  `SOTC_FG_HUDLESS=1` (PR #154, Lucas-blip-png).
- Jogando pelo `SotC.bat` o registro também vai para `user\last_run.log` (PR #175, Einar);
  ferramentas de medição corrigidas (PR #181, Einar).

**Conhecido:** "Texturas em alta qualidade" ainda pode fechar o jogo em vistas muito amplas
(espada erguida contra o sol, área do último colosso; #186): desligue-a se acontecer. Personagens
brancos em AMD GCN (RX 470–590, Vega, APUs): em investigação.

## 0.59.1 — 2026-10-20
Obrigado a **Lucas-blip-png e Oussama268**.

- **Sombras distantes piscando** (#178, #157, #163; AMD e NVIDIA GTX 1080, RTX 40/50): a
  reutilização de sombras (desde a 0.58) redesenhava as sombras distantes só a cada dois quadros e
  o jogo não guarda a do quadro pulado. Agora vem desligada (PR #170, Lucas-blip-png). Medido no
  demo inteiro: desligada 61,8 FPS de média / 37,2 de mínima, ligada 59,5 / 35,6, ou seja não
  ganhava FPS. Nova opção **Sombras distantes** no lançador (Desligado / Ligado / Automático).
- **AMD: listras nas sombras e na água** (fork do Oussama268, opt8): o acesso direto à memória
  vem desligado por padrão em placas AMD (dá para ligar no lançador).
- **Intel, correções parciais** (Oussama268): tamanhos de dispatch e layouts válidos, sem
  inteiros de 64 bits quando a GPU não tem, tamanho de subgroup fixo.
- **O registro mostra a placa de vídeo e o driver** (linha `GPU:`), para os relatos nas issues;
  `SOTC_GPU_VENDOR=10de|1002|8086` escolhe a GPU em notebooks com duas (Oussama268).
- Correções menores de revisão (Oussama268).

## 0.59 — 2026-10-20
Obrigado a **Oussama268, Lucas-blip-png e EdouardSence**.

**Desempenho**
- **Mais FPS e menos travadas** (fork do Oussama268, opt5–opt7): os desenhos são preparados numa
  thread própria (#99 Lucas-blip-png), a memória direta é enviada aos poucos e os shaders e
  pipelines de compute são traduzidos em threads auxiliares (o objeto espera alguns quadros em vez
  de a tela travar). Demo da tela de título: média 63 → 65–67 FPS, mínima 30 → 34–37, ponte
  30–32 → 37–40.

**Estabilidade**
- **Queda no final do jogo e ao carregar saves** (`0x1a371` / `0x49500`): a GPU às vezes devolvia
  dados velhos para memória que o jogo já tinha liberado, corrompendo o alocador. A proteção do
  Pink-shadPS4 cuidava de um só dos reservatórios de memória do jogo; agora cobre todos os do
  mesmo tipo, inclusive os criados quando o mundo carrega (área do último colosso). O jogo também
  recebe 9152 MB de memória direta em todos os modos (#146, #149). Causa achada com logs de
  quedas reais; a passagem completa até os créditos ainda está em teste.
- **Placas de 6 GB** fechando por falta de memória de vídeo: tenta o tamanho exato e a RAM do PC
  antes de desistir (#137).
- **Lançador em Windows chinês** quebrava com o "™" do título (#151).

**Novidades**
- **F11** alterna janela / tela cheia (#118, EdouardSence).
- **Uso de memória de vídeo:** Econômico, Padrão, Alto e Máximo (antes o jogo limpava texturas a
  partir de 60% da placa, o "limite de 6 GB" das placas de 8–16 GB) (#143).
- **Volume:** 100 a 300%, sem estourar (#95).
- **Atualização automática:** o lançador avisa quando sai versão nova e instala sozinho, mantendo
  saves, cache de shaders e sotc.ini (#136, Oussama268). Vale a partir desta versão.
- **Ultrawide com o modo PS4 Pro:** 21:9 e 32:9 ficavam com bordas pretas com o modo Pro ligado;
  agora a tela larga tem prioridade (#142, #145).

## 0.58.1 — 2026-10-19
- **Texturas em alta qualidade** (ligada por padrão nos Ajustes rápidos): desde a 0.0.13 o port
  nunca informava ao jogo quais texturas estavam em uso, e ele não carregava as versões de alta
  resolução (folhas da árvore da abertura e pedras borradas). Desligada, volta ao comportamento
  antigo, para placas com pouca memória de vídeo.
- **Ícone da janela do jogo:** um desenho do port (o menino erguendo a espada), para não ficar igual
  ao do lançador na barra de tarefas. Em Imagem e FPS: padrão, o ícone do jogo ou uma imagem sua.

## 0.58 — 2026-10-19
Versão da comunidade: junta as pull requests e forks de várias pessoas, revisados e testados juntos.
Obrigado a **Lucas-blip-png, Gabriell-os, Oussama268, jownatan, xeodeo, nakaionyt/y7ngboy e tabnul**.

**Estabilidade**
- **Quedas por falta de memória de vídeo** (`EnsureResident: Assertion Failed`, placas AMD e de
  4–6 GB): a memória dos buffers vem em blocos de 64 MB (antes milhares de alocações, acima do
  limite dos drivers) e a memória parada é devolvida com o uso, conferindo os dados antes
  (Gabriell-os #98, Oussama268 #59/#83/#103).
- **Jogo congelando** ("GPU hook queue overflow"): a fila de avisos de memória cresce sob demanda.
- **Queda depois do 1º e do 2º colosso no modo PS4 Pro** (`0x1a371`): o modo Pro recebe a
  memória de um PS4 Pro (Lucas-blip-png #106).
- Pacote de preenchimento na fila de compute que corrompia o pacote seguinte; arredondamento RTZ
  desligado na NVIDIA (o driver travava); trava de regiões sem deadlock no Windows (jownatan #81).

**Desempenho:** demo da tela de título 54–55 → **62 FPS de média**, mínima 26 → 31; memória de
vídeo ~4,3 → ~2,7 GB (cortes no processador de comandos, pipelines compilados em segundo plano,
caches lidos em paralelo, sombras distantes reaproveitadas).

**AMD:** bloco preto no canto da tela nas RDNA3/RDNA4 (#43) e texturas BC5/BC6H (xeodeo);
ramos uniformes de wave64 e envio periódico ligados na AMD (Gabriell-os #98). Ainda sem teste
numa AMD aqui: conte nas issues.

**Novidades**
- **Widescreen / ultrawide:** 16:10, 21:9 e 32:9 com imagem mais larga, sem esticar (tabnul #54).
- **FPS: 30 / 60 / ilimitado** no lançador.
- **Ícones dos botões Xbox** no jogo (A B X Y, RB, LT/RT, Menu, View), desenhados para o port;
  opção PlayStation / Xbox em Controles (mecanismo de troca de texturas do xeodeo).
- **Geração de quadros (experimental):** FSR 3 em qualquer placa e DLSS-G nas RTX 40/50 (runtime
  oficial da NVIDIA no pacote, com as licenças), com o jogo a 30 → 60 na tela (nakaionyt/y7ngboy).
  Só faz diferença com o FPS em 30, e a barra de vida/stamina pode tremer.
- Overlay de desempenho (F10), F7 liga/desliga o limite de 30, F8 troca a geração de quadros.
- **Lançador em espanhol, chinês, coreano e russo.**
- Janela com o ícone do jogo e, no título, a versão e o FPS.
- **Lista de saves com a imagem de cada save**, como no PS4 / shadPS4.
- **Tela de carregamento** com barra de porcentagem enquanto o cache de shaders é lido.
- Idioma escolhido já na primeira tela do lançador; Modo PS4 Pro e Nitidez (RCAS) nos Ajustes rápidos.
- O cache de shaders é refeito na primeira abertura.

## 0.57 — 2026-10-14
- **Placas AMD: correção das cores verdes/rosas** na névoa, no Wander e no Agro (issues #6, #7, #9,
  possivelmente #10). A AMD informa grupos de 64 threads, mas só deixa fixar esse tamanho nos
  shaders de compute; o port exigia 64 em todos os estágios e deixava de converter os shaders de
  compute. Agora as placas AMD usam o mesmo caminho de 32 threads da NVIDIA (compute fixado em 32,
  barreiras de memória compartilhada, sem a otimização de ramos uniformes que deixava os
  personagens pretos na sombra). Diagnóstico de raulbezerro-dev na issue #7. Na NVIDIA nada muda.
  **Ainda não testado numa placa AMD:** conte na issue se melhorou.
- **Opção nova no lançador:** "Reflexos completos da água (acesso direto à memória)", ligada por
  padrão. Desligada, ganha ~10% de FPS (demo da tela de título: 54 → 59), mas a água distante
  fica mais lisa. Cada modo tem a sua própria pasta de cache de shaders.
- O cache de shaders é refeito na primeira abertura.

## 0.56 — 2026-10-13
- **Água do mundo aberto com ondas:** lagos e rios abaixo das cascatas ficavam lisos como espelho,
  sem movimento. O shader dessa água guarda a máscara de threads ativas com `s_wqm_b64 vcc, exec`
  e depois decide por ela se aplica o mapa de fluxo (as ondas). O recompilador tratava
  `S_WQM_B64` como "não faça nada", o VCC ficava com lixo e o shader sempre escrevia uma normal
  reta para cima. Agora a instrução faz o que o hardware faz: expande a máscara para quads
  inteiros de 2×2 pixels. Uma simples cópia corrigia a água, mas deixava "lixo colorido" nas copas
  das árvores distantes, que usam a expansão no teste de transparência. Detalhes em
  `docs/sotc/AGUA.md`.
- **Acesso direto à memória (DMA) ligado por padrão,** como na configuração do Pink-shadPS4 para
  o SotC. Leituras de tabelas com índice calculado no shader (os registros de *probe* usados pelo
  reflexo em espaço de tela) liam sempre a posição 0; agora leem o endereço certo. Com isso o
  lago de cima ondula e reflete até longe. `BB_DIRECT_MEMORY_ACCESS=0` desliga.
- O cache de shaders é refeito na primeira abertura (o primeiro carregamento engasga).

## 0.0.13 — 2026-10-13
- **Correção de queda da 0.0.12:** o jogo podia fechar na abertura ou no menu (falha em `0x1a371`,
  1 em 3 testes). Causa: o port marcava cada textura vista como "ainda em uso" por ~3 segundos, e o
  jogo tentava carregar mais texturas do que cabe no seu pool. Agora só contam as texturas usadas
  desde o último pedido (como no Pink-shadPS4). Testado: 0 quedas em 5 rodadas da abertura e uma
  sessão de quase 40 minutos andando pelo mapa, com dois carregamentos de save.
- **Reflexos da água bem maiores:** o lago agora reflete paredões, templos e vegetação em quase
  toda a superfície, com ondulação. Um limite de segurança de laços nos shaders cortava os raios
  do reflexo em espaço de tela; agora o limite é por laço.
- Em investigação: onde a água deveria refletir o céu, ela ainda fica escura e lisa.

## 0.0.12 — 2026-10-13
- **Texturas nítidas de volta, sem o jogo fechar:** a corrupção de memória que fazia o jogo fechar
  logo depois de carregar um save (falha em `0x1a371`) e deixava texturas com "lixo colorido" vinha
  do emulador copiando dados da GPU para a memória que o jogo estava liberando. Isso foi corrigido,
  e o streaming de texturas voltou ligado no nível 2 (texturas de 2048 até 512×512): pedras e
  paredes bem mais nítidas. Testado por mais de 25 minutos perto da cascata, sem queda.
- **Água:** respingos, anéis e espuma em volta do Wander ao nadar; o lago reflete o céu e o
  brilho do sol. Os reflexos de ambiente (passada de IBL local) agora usam o *cubemap* certo de
  cada *probe* — correção trazida do Pink-shadPS4 (`dynamic_tsharp_array_size`).
- Em investigação: a água perto da câmera ainda fica sem ondulação.
- Opções para teste: `SOTC_LOD_STATS=0` (streaming desligado), `SOTC_LOD_LEVEL`,
  `SOTC_UNMAP_READBACK=1`, `SOTC_TSHARP_ARRAY=0`.

## 0.0.11 — 2026-10-13
- **Correção da 0.0.10 — grama com textura de "lixo colorido":** no nível 3 do streaming o
  jogo liberava mips que a GPU ainda usava (visto na grama perto da cascata). O streaming de
  texturas agora vem **desligado**, como na 0.0.9: grama normal, cortinas das cascatas visíveis e
  sem as quedas ao carregar um save. Para testar o streaming: `SOTC_LOD_STATS=1` (com
  `SOTC_LOD_LEVEL=2` fica mais nítido, mas pode fechar o jogo).
- Em investigação: a água parada (lago como espelho, sem ondas) e o borrifo das cascatas.

## 0.0.10 — 2026-10-13
- **Gravação corrigida:** o jogo nunca conseguia gravar ("sem espaço livre"); agora grava.
- **Lista de saves na tela:** "Carregar jogo" e "Gravação manual" mostram os saves (região,
  tempo jogado, data) para escolher, com "+ Nova gravação" ao gravar. Setas/direcional para
  escolher, Enter/X para confirmar, Esc/O para voltar, duplo clique com o mouse.
- **Excluir save:** na lista, Delete (teclado) ou Triângulo/Y (controle), com confirmação.
- **Texturas de alta resolução (streaming):** o port passou a responder ao comando de
  estatísticas de textura da GPU (`GET_LOD_STATS`) que o jogo usa para decidir o que carregar.
  No nível 2 as texturas de 2048 chegavam a 512×512 e as cascatas ganhavam as cortinas de água.
- **Jogo fechando sozinho depois de carregar um save:** o streaming no nível 2 corrompia a
  memória do jogo (3 quedas em 4 testes perto da cascata). O padrão agora é o nível 3, que rodou
  4 de 4 testes sem queda; nele as texturas de 2048 ficam em 256×256, como antes do streaming
  (as cortinas das cascatas ainda não foram conferidas neste nível). `SOTC_LOD_LEVEL=2` volta ao
  nível mais nítido, com o risco de queda.
- **Sombras pretas corrigidas:** grama, pedras e paredes em sombra ficavam pretas. Três correções
  do recompilador de shaders trazidas do Pink-shadPS4 (constantes 1.0 do fetch shader que eram
  ignoradas, `V_ALIGNBIT`/`V_ALIGNBYTE` com deslocamento 0, `V_CVT_PK_U8_F32`).
- **Partículas da GPU:** barreiras de memória compartilhada em grupos menores que uma wave; um
  sistema de partículas que nunca aparecia voltou. O borrifo e a espuma das cascatas ainda não
  aparecem (em investigação).
- O lançador grava o registro do jogo em `user/last_run.log`, com o código de saída.
- Ferramentas de diagnóstico: F12 captura um quadro quando o jogo roda sob o RenderDoc.
- Ícone novo (rosto de um colosso).
- **Atenção:** se trocar de save na mesma sessão fechar o jogo, reinicie antes de carregar outro.
- Revisão completa do projeto em `docs/sotc/REVISAO-DO-PROJETO.md`.

## 0.0.9 — 2026-10-12
- **Configuração inicial automática:** na primeira vez o `SotC.exe` abre numa tela de boas-vindas
  que só pede a pasta do jogo (ou já oferece a que encontrou no PC). Depois ele configura sozinho
  as DLCs (dentro da pasta do jogo e ao lado dela), os saves (copia um save do shadPS4 se o port
  ainda não tiver) e o som, e mostra o resultado. **Configurar de novo** repete o processo.
- **Pasta principal como no shadPS4:** você aponta a pasta principal do jogo e ele acha as
  subpastas do jogo base (CUSA08809), da atualização e das DLCs.
- **Atualização:** a pasta `CUSA08809-UPDATE` ou `CUSA08809-patch` ao lado do jogo é usada como no
  shadPS4: os arquivos dela têm prioridade sobre os do jogo base (`SOTC_UPDATE_DIR`). Se ela tiver
  `eboot.bin`, é ele que roda.
- Tela principal mais limpa: o registro do jogo virou um link discreto; a verificação mostra as DLCs.
- **Pasta de DLC** no lançador (Jogo e saves), como no shadPS4. Também é encontrada na busca
  automática: a pasta `addcont` do shadPS4 ou uma pasta com uma subpasta por DLC.
- O runtime lista ao jogo as DLCs instaladas (`sceAppContentGetAddcontInfoList`), lendo o
  `CONTENT_ID` do `param.sfo` de cada uma (`SOTC_DLC_DIR`).
- **DLC em .pkg sem instalar:** os pacotes de DLC (tipo `0x1b`) são lidos direto do cabeçalho,
  sem extrair. Isso basta para as DLCs do jogo, que só desbloqueiam itens (arco antigo, capa,
  espada, cavalos). A busca olha na pasta escolhida, na pasta do jogo e nas pastas vizinhas.
- **Modo PS4 Pro:** o registro do jogo mostra se ele foi aberto como PS4 Pro
  (`sceKernelIsNeoMode -> 1`). A opção só libera o Modo Grafismo do PS4 Pro: é preciso escolher
  **Cinematográfico** ou **Desempenho** no jogo, em Opções > Apresentação.

## 0.0.8 — 2026-10-12

Primeira versão pública.

### Lançador (`SotC.exe`)
- **Busca automática** ao abrir: encontra nos discos do PC o jogo (CUSA08809), os módulos de som
  do usuário num shadPS4 instalado (copiados para a pasta de dados do port) e saves do shadPS4
  (pergunta antes de importar). **Procurar no PC** repete a busca.
- **Biblioteca de jogos** como a do shadPS4: pastas de jogos, lista com ícone, título, ID e versão.
- **Troca de teclas:** clique na ação e aperte a tecla nova (gravado como `key.<entrada>=` no
  `sotc.ini`).
- **Ícone novo** (arte original). A janela e o atalho na área de trabalho usam o `icon0.png` do
  seu dump.
- **Envio de trabalho à GPU:** Automático (NVIDIA), Ligado em qualquer placa (AMD/Intel,
  experimental) ou Desligado.

### Desempenho
Demo da tela de título, modo 60 FPS, i9-13900K + RTX 2060 SUPER: média de 37–45 → **~67 FPS**,
trecho mais pesado ~17 → **29–34 FPS**.
- Comandos Vulkan gravados e enviados numa thread própria (`cp_record_thread` do Pink-shadPS4).
- Envio periódico de trabalho à GPU por tempo (a cada 0,5 ms), em vez de por número de comandos.
- Draws indiretos com argumentos na CPU viram draws diretos.
- Caches: descrição de imagens por T#, buscas de imagens, informações de binário de shader e
  ponteiro de memória por thread.
- Verificação de sincronia de buffers sem trava quando nada mudou; `RangeSet::Add` não reinsere
  intervalos já marcados.
- Falhas de escrita: a página que falhou e as seguintes, sem dados da GPU, são liberadas juntas
  (~50 mil → ~4 mil falhas por segundo).
- Readbacks:
  - faixas lidas recentemente são baixadas juntas;
  - leituras "limpas" nas tabelas de recursos dos shaders, com o bitmap acompanhando a proteção
    real das páginas;
  - o preenchimento de tabelas de descritores (compute `0x50784801`) é feito na CPU;
  - leituras antecipadas.
- Do Pink-shadPS4: cvars `doIndexBufferCulling=0 doSunShadowIndexBufferCulling=0` e a proteção
  do heap BPE contra escritas da GPU.
- Opcionais, para teste: readback assíncrono (`SOTC_RB_ASYNC=1`) e marcador de espera
  (`SOTC_WAIT_MARKER=2`).

### Correções
- Travamento da fila da GPU com a gravação em thread própria: a apresentação espera o quadro
  chegar à fila.
- Morte silenciosa por proteção de página na pilha de uma fibra.
- Tela preta no boot; crash no modo PS4 Pro (`alt_tile` nos buffers de vídeo).

## Antes (desenvolvimento, 07–08/10/2026)
- Boot do jogo no carregador do bbport (imports por NID, fibras, SSE4a, imagem abaixo de 2 GiB).
- Núcleo de vídeo do fork shadPS4 SotC; som com os módulos do usuário.
- Modos 60 FPS e 16x AF como patches do carregador; lançador Tkinter congelado (`SotC.exe`).
- Teclado e controle juntos, câmera no mouse, saída 720p–4K, modo PS4 Pro, efeitos do motor.
