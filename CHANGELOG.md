# Changelog

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

## 0.60.6-plus1 — fork Lucas-blip-png
- Junção com a 0.60.6 do original (AjncSystem). Nenhuma mudança própria nesta versão ainda;
  as correções do fork (wave32 nas AMD GCN, memória, Streamline só em NVIDIA, barreiras do FSR 1)
  seguem como estavam na 0.60.5-plus1.

## 0.60.5-plus1 — 2026-10-22 (fork Lucas-blip-png)
- **Junção com a 0.60.5 do original:** a seleção de texturas (Desligada / Alta / Máxima), o vazamento
  de memória, os descritores reaproveitados, o carregamento mais rápido depois de atualizar o driver
  e as barreiras de LDS para AMD de 64 lanes vêm da 0.60.5. Nada disso foi removido do fork.
- **Mantido do fork:** a conversão para 32 lanes nas AMD GCN (`GPU: AMD wave32 on`), com a
  proteção para o RADV, a correção de memória (8/7/6 GiB antes do mínimo), o Streamline só em NVIDIA
  e as barreiras do FSR 1.
- **Atenção:** as barreiras de LDS da 0.60.5 e a conversão para 32 lanes ficam ligadas juntas nas
  AMD GCN. Ainda não foi testado em uma RX 570; quem testar, mande o `last_run.log` (o relato de
  perda de dispositivo ainda não está resolvido: teste com `SOTC_AMD_WAVE32=0`).

## 0.60.3-plus6 — 2026-10-22 (fork Lucas-blip-png)
- **Queda depois do primeiro colosso com saves em modo Pro (#194 do original):** quando o Windows
  recusava os 9152 MiB de memória do jogo, a plus3 caía direto para 5056/5696 MiB, e isso fazia o
  save carregar em modo Pro ler além do bitmap de páginas (`guest offset 0x1a371`). Agora o port
  tenta 8, 7 e 6 GiB antes de cair para o mínimo. O log mostra `trying N MiB`.

## 0.60.3-plus5 — 2026-10-21 (fork Lucas-blip-png)
- **Correção urgente da plus4 no Linux (RADV):** a conversão para 32 lanes ligava por padrão em
  qualquer AMD com subgrupo 64, mas o RADV não exige o tamanho nos shaders de compute, e o jogo
  travava 30–90 s na introdução (#201 do original). Agora o padrão só liga onde o driver exige o
  tamanho nos shaders de compute (Windows, RX 570: o flicker sumiu com isso). `SOTC_AMD_WAVE32=1`
  continua forçando, mas no RADV dá falha: não use lá.

## 0.60.3-plus4 — 2026-10-21 (fork Lucas-blip-png)
- **Placas AMD GCN (RX 470–590, Vega, APUs mais antigas):** o flicker branco/verde/rosa de
  personagens, fumaça e névoa (#152, #187, #201 do original) **sumiu numa RX 570** com a nova
  conversão para 32 lanes (a que a camada SotC-AMD-Fix fazia): os shaders passam pela conversão de
  wave64 e os pipelines de compute exigem subgrupo de 32. Agora é o padrão nessas placas (AMD com
  subgrupo mínimo e máximo de 64); `SOTC_AMD_WAVE32=0` desliga e `=1` força em qualquer AMD. O log
  mostra `GPU: AMD wave32 on`. O cache de shaders é refeito na primeira abertura. **Um relato de
  queda** (GPU travada, `ErrorDeviceLost`) logo após derrotar o 1º colosso e carregar a cena
  seguinte: se acontecer com você, mande o `last_run.log` e teste com `SOTC_AMD_WAVE32=0`.
  Nada muda em NVIDIA, Intel nem nas AMD RDNA.

## 0.60.3-plus3 — 2026-10-20 (fork Lucas-blip-png)
- **Queda ao abrir em PCs com pouca memória virtual:** quando o Windows recusa os 9152 MiB de
  memória do jogo (erro 1450, arquivo de paginação pequeno ou RAM ocupada), o port já caía para
  5056 MiB, mas só depois de o jogo ter perguntado o tamanho. O jogo então alocava perto dos 9 GiB,
  não recebia nada e caía no boot (guest offset 0x18cae, #194 do original). Agora a memória é criada
  antes de o tamanho ser informado. O log mostra `guest memory refused ... using 5056 MiB`, e um
  arquivo de paginação maior devolve os 9152 MiB.
- **Queda ao ligar a geração de quadros com uma AMD e uma NVIDIA no mesmo PC:** o Streamline
  (DLSS-G) era carregado se houvesse qualquer placa NVIDIA, mas o jogo roda na placa de mais memória
  (uma RX 9070, no relato), e a criação do dispositivo falhava (`VK_NV_low_latency2`, #194). Agora o
  Streamline só carrega quando a placa usada é NVIDIA (`SOTC_GPU_VENDOR` também é respeitado).
- **FSR 1:** a imagem do jogo agora espera pelo compute shader do EASU, e a imagem de saída espera
  pelos últimos leitores. Pela especificação do Vulkan faltavam essas duas barreiras. É candidata a
  causa de falhas do FSR 1 em algumas AMD (#209), mas não está confirmada.

## 0.60.3-plus2 — 2026-10-20 (fork Lucas-blip-png)
- **Placas AMD GCN (RX 470–590, Vega, APUs mais antigas), para testar:** personagens piscando em
  branco, verde ou rosa, fumaça branca e névoa com cores que mudam a cada execução (#152, #187 e
  #201 do original). Essas placas rodam os shaders com 64 lanes, e nesse caso o port não punha as
  barreiras de LDS que o jogo precisa; agora põe. A camada não oficial SotC-AMD-Fix (raulbezerro-dev)
  resolvia numa RX 570 justamente por ligar essas barreiras. O log mostra
  `GPU: LDS barriers on for 64-wide AMD`; `SOTC_AMD64_LDS=0` desliga. Não testado numa GCN (não
  temos a placa): conte na #152 ou na #201 do original. O cache de shaders é refeito na primeira
  abertura nessas placas. Nada muda em NVIDIA, Intel ou nas AMD RDNA no Windows. Enviado ao
  original como PR #205.
- **CPUs com 4 threads:** o gravador de comandos da GPU também não gira entre um lote e outro
  quando o processo tem até 8 processadores lógicos. Demo de abertura, modo 60 FPS, Ryzen 7 5700X
  preso a 4 processadores lógicos: 2 núcleos com SMT 66,9 FPS (antes 64,3), 4 núcleos sem SMT
  81,8 (antes 79,9); com todos, igual (95 FPS). O log mostra
  `GPU recorder: N logical processors, spin N us, idle wait N us`.

## 0.60.3-plus — 2026-10-20 (fork Lucas-blip-png)
- A 0.60.1-plus em cima da **0.60.3** do original: o jogo é zerável do início ao fim, o **ajuste
  rápido no menu dentro do jogo** (60 FPS, geração de quadros, texturas em alta, sombras distantes
  e volume sem sair do jogo), o mouse solto no menu e a tela de carregamento animada na montagem
  dos shaders. A válvula das texturas em alta (#191) e a janela de readback de 64 KiB (#196), que
  eram só deste fork, agora fazem parte do original.
- **CPUs com 4 threads** (4 núcleos sem SMT ou 2 com SMT): o gravador de comandos da GPU dorme 2 ms
  quando o processo tem até 8 processadores lógicos, em vez de girar num núcleo inteiro, e o
  processador de comandos cede a vez quando todas as filas estão esperando (`SOTC_CP_YIELD=0`
  desliga). Demo de abertura, modo 60 FPS, Ryzen 7 5700X preso a 4 processadores lógicos: antes
  ficava parado no carregamento, agora 76 FPS (mínima 65). Com 16 processadores lógicos, igual
  (94 FPS). O log mostra `GPU recorder: N logical processors, idle wait N us`;
  `SOTC_REC_SLEEP_US` continua mandando.
- **A emulação de BMI1 não pesa no mundo aberto:** demo de abertura no modo 60 FPS, todos os
  núcleos, com a emulação forçada (`SOTC_EMULATE_BMI=1`) 93,3 e 93,4 FPS contra 91,2 e 92,2 sem
  ela. Nas CPUs sem BMI1 o FPS baixo no mundo aberto vem da CPU, não da emulação.
- Testado: demo de abertura com 4 processadores lógicos 77,1 FPS (mínima 68,5); Santuário com a
  emulação, o relógio do Windows e as texturas em alta forçados (`SOTC_EMULATE_BMI=1`,
  `SOTC_TSC=qpc`, `SOTC_LOD_LINGER=1`): sem falhas, imagem normal, 46 FPS.
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

## 0.60.1-plus — 2026-10-20 (fork Lucas-blip-png)
- A 0.59-plus5 em cima da **0.60.1** do original. Da 0.60.1: o **menu dentro do jogo** (Insert, F1
  ou L3+R3; nosekefik) e as sobras do Bloodborne removidas (Oussama268). Da 0.60: o relógio do jogo
  vem do Windows quando os núcleos da CPU discordam (Einar; o jogo travava na tela de título), o
  jogo abre em PCs com pouca memória e cada cópia do port tem a própria pasta `user` (as
  configurações do lançador ficam em `user\launcher_settings.json`; na primeira vez a cópia herda
  as de antes). As texturas nas GTX 10xx, os 60 FPS pelo modo do próprio jogo, o heap guard sem
  remontar a cada quadro e a geração de quadros sem o HUD, que vieram deste fork, agora fazem parte
  do original.
- **CPUs sem BMI1 confirmadas numa de verdade:** com a 0.59-plus5, o ta955995-cmd (issue #88 do
  original) jogou até o primeiro colosso. No mundo aberto o FPS ainda fica baixo nessas CPUs.
- Continua só aqui: CPUs sem BMI1/LZCNT/MOVBE (Oussama268, com as lacunas da 0.59-plus5), a
  válvula das texturas em alta (#186 do original; enviada como PR #191 do original), a janela de
  readback menor e a tela cheia a 120 Hz.
- Testado no Santuário com a emulação de CPU antiga, o relógio do Windows e as texturas em alta
  forçados ao mesmo tempo (`SOTC_EMULATE_BMI=1`, `SOTC_TSC=qpc`, `SOTC_LOD_LINGER=1`): sem falhas,
  imagem normal, 47 FPS.

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

## 0.59-plus5 — 2026-10-20 (fork Lucas-blip-png)
- **Queda com texturas em alta no modo PS4 Pro** (issue #186 do original, `read access to
  ffffffffffffffff at guest offset 0x1a371`): o port pede o mip 2 de toda textura amostrada,
  inclusive as distantes, e o jogo carregava mais detalhe do que o pool de páginas dele comporta
  (10% livre depois de carregar, 0% uns 40 s depois). Agora, quando o pool fica abaixo de 8% livre,
  as texturas pedem um mip a menos (abaixo de 4%, dois) e o jogo libera memória. No Santuário, em
  modo Pro: antes caiu nas 3 rodadas; agora rodou 3 minutos sem cair, com o pool estável em 4% e o
  chão e a grama com a mesma nitidez. `SOTC_LOD_POOL_LOW=0` desliga.
- **CPUs sem BMI1 (Sandy/Ivy Bridge, AMD FX) travavam na primeira tela de carregamento** (issue
  #88 do original, numa CPU dessas de verdade): a tradução das instruções seguia só o caminho de
  cada função e deixava de fora trechos alcançados por tabelas de salto. Um TZCNT/LZCNT esquecido
  ali roda como BSF/BSR sem dar erro e devolve outro valor. Agora esses trechos também são
  traduzidos (89 instruções a mais). Testado forçando a emulação numa CPU moderna: Santuário e
  demo de abertura inteiro sem falhas. Falta a confirmação numa CPU dessas. Nas CPUs
  modernas nada muda.

## 0.59-plus4 — 2026-10-20 (fork Lucas-blip-png)
- A 0.59-plus3 em cima da **0.59.1** do original: **listras da AMD** nas sombras e na água (o
  acesso direto à memória vem desligado nas AMD, do Oussama268), correções para Intel, opção
  **Sombras distantes** no lançador (desligada por padrão, como já era aqui), linha `GPU:` com a
  placa e o driver no `user/last_run.log` e `SOTC_GPU_VENDOR` para notebooks com duas GPUs.
- Santuário, FPS ilimitado, modo 60 (RTX 5060 Ti, mesma cena, 3 rodadas alternadas): 0.59.1 45,7 /
  45,6 / 46,7 FPS, 0.59-plus4 48,1 / 48,8 / 50,1 (+7% pela mediana), sem falhas.

## 0.59-plus3 — 2026-10-20 (fork Lucas-blip-png)
- **60 FPS pelo modo 60 do próprio jogo** (o do Desempenho do PS4 Pro). O modo 60 do lançador
  ignorava a taxa de 30 que o jogo pede, mas o jogo continuava se cronometrando para 30 (alvo
  29,97), e partes da lógica que andam por esse alvo corriam no dobro da velocidade: a causa
  provável do colosso pássaro que sobe para o céu (#100 do original), da parede do 2º colosso
  (#120) e do dano no ponto fraco (#49). Agora o jogo mira 60 sozinho (`TargetFramesPerSeconds` 1,
  onde ele gravava 2) e o tempo dele acompanha. Vale para 60 FPS e ilimitado. O F7 troca o alvo
  do próprio jogo entre 30 e 60, e a taxa e o tempo mudam juntos. Mesmo desempenho (Santuário
  35,8 FPS nos dois modos, mesma cena). Ainda não testado nos colossos 2 e 5: quem tiver um save
  ali, conte. Para voltar ao modo antigo: `SOTC_MODES=af16` em Avançado → Variáveis extras.
- **Texturas nas GTX 10xx** confirmadas numa GTX 1080 Ti (JustS73, issue #57 do original): voltaram
  quase em todo lugar, a 30 FPS. Resta uma camada de terreno em frente ao Santuário, em análise.

## 0.59-plus2 — 2026-10-20 (fork Lucas-blip-png)
Obrigado a **Oussama268** e **KingJhoss**.
- **Processadores sem BMI1/LZCNT/MOVBE** (Intel Core i3/i5/i7 de 2ª e 3ª geração, Xeon E3 v1/v2
  e outros anteriores ao Haswell; AMD FX), que antes recebiam "This CPU is not supported". O jogo
  usa ~5.000 instruções dessas do processador do PS4; nessas CPUs elas são trocadas por rotinas
  equivalentes ao abrir, no executável do jogo e nos módulos (libc, Fios2, áudio). Nas outras CPUs
  nada muda. AVX continua obrigatório (Sandy Bridge / Bulldozer ou mais novo). Feito pelo
  Oussama268 (PR #55 do fork dele); aqui ganhou um teste que compara cada instrução emulada com a
  real (39 formas, 200 mil entradas cada). Testado forçando a emulação numa CPU moderna
  (`SOTC_EMULATE_BMI=1`): no Santuário 50 FPS com e sem emulação, e o demo de abertura inteiro até a
  tela título com a imagem igual, sem nenhuma falha. Falta a confirmação de quem tem uma CPU
  dessas (issues #70, #88, #89 e #164–#166 do original).
- **Blocos pretos nos quadros gerados pelo FSR 3** nas AMD RX 7000: a mesma barreira entre limpar e
  copiar a imagem que corrigiu os quadros do jogo (issue #43 do original) agora vale também para os
  gerados. Feito e testado numa RX 7600 pelo KingJhoss (PR #1 deste fork).
- **Botão JOGAR cortado** com a escala do Windows acima de 100%: a barra de baixo do lançador
  agora cresce junto (PR #171 do original).
- Nas GTX 10xx, a linha "No fragment shader barycentrics: emulated by a geometry shader" agora
  aparece no `user/last_run.log`.

## 0.59-plus — 2026-10-20 (fork Lucas-blip-png)
A 0.59 do AjncSystem inteira, mais o que ainda é só deste fork. A thread de desenho, a varredura
de DMA incremental e as correções pequenas da 0.58.1-plus entraram no original na 0.59.
Santuário, FPS ilimitado (RTX 5060 Ti, Ryzen 7 5700X): 0.59 47 FPS → 0.59-plus 50,6 FPS (+7%).
- **Heap guards sem remontar a cada quadro** (+6% no Santuário, `SOTC_HEAP_FAST=0` desliga): os
  readbacks que caem em blocos em uso dos heaps do jogo não passam mais pela guarda do alocador,
  que era refeita cerca de uma vez por quadro. Refeito em cima da guarda de todos os heaps da 0.59.
- **Texturas nas GTX 10xx (Pascal)** (`SOTC_BARYCENTRIC_GS=0` desliga): sem
  `VK_KHR_fragment_shader_barycentric`, pedras e chão saíam pretos, facetados ou com ruído. Um
  geometry shader pequeno passa ao fragment shader os valores dos três vértices. Nessas placas o
  cache de shaders é refeito na primeira abertura. Testado forçando o modo numa RTX 5060 Ti; falta
  a confirmação de quem tem uma GTX 10xx.
- **Sombras distantes sem piscar** (`SOTC_SHADOW_REUSE=1` religa): o reaproveitamento de sombras
  desenha as cascatas distantes em quadros alternados, mas o jogo não guarda a cascata pulada, e a
  sombra no chão pisca a 15 Hz (issue #157 do original).
- **Janela de readback menor**, 64 KiB (+3% no Santuário, `SOTC_RB_NARROW=0`).
- **Tela cheia a 120 Hz** em monitores de 144/165 Hz (Anthonypc23).
- **Geração de quadros sem o HUD, agora opcional e desligada** (`SOTC_FG_HUDLESS=1` liga): com a UI
  na tela, os quadros gerados pelo FSR 3 saem com o logo desbotado e, nas telas de carregamento,
  com a cena velha que fica atrás da UI. Na 0.58.1-plus ela vinha ligada com o FG.
- **Atualização automática** (da 0.59) apontando para as releases deste fork.
- O lançador não para mais quando o console dele recusa escrita (`[Errno 22]`, issue #104).
- Fora desta versão: o aviso de versão nova da 0.58.1-plus (a 0.59 já se atualiza sozinha).
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
