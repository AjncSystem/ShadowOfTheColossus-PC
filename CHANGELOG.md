# Changelog

## 0.57-opt5 — 2026-10-17 (fork Oussama268)
- **Sessões longas sem a memória de vídeo crescer:** o jogo mantém mapeado o seu bloco de 4 GB
  de memória e reaproveita os endereços o tempo todo, então a memória de buffers na GPU só
  crescia (~45 MB por minuto no demo da tela de título: 1,6 → 4,3–4,4 GB em 30 minutos). Numa
  placa de 6 GB isso passava do orçamento depois de ~40 minutos de jogo e os engasgos voltavam.
  Agora, quando a memória de buffers passa de 1,5 GB, os blocos que não foram usados há 30
  segundos e não têm dados escritos pela GPU são devolvidos (no máximo 64 MB a cada 2 segundos,
  ~20 ms cada vez). Antes de devolver, o port lê o bloco de volta e compara com a memória do jogo,
  então a imagem não muda. Em 30 minutos de demo: pico de memória de vídeo **3,4 GB, estável**
  (antes 4,3–4,4 GB e subindo), nenhuma coleta de emergência. `SOTC_ARENA_EVICT_MB=0` desliga;
  `SOTC_ARENA_EVICT_MB`, `SOTC_ARENA_EVICT_IDLE_S` e `SOTC_ARENA_EVICT_STEP_MB` ajustam.
- **Mundo aberto um pouco mais rápido:** quando um comando escreve numa página que o jogo vai ler
  de volta, o port já manda o trabalho para a GPU (`SOTC_RB_EARLY`, agora padrão). Demo: 55,0 →
  56,2 FPS (+2%); santuário igual.
- **Correção de queda rara:** a 0.57-opt4 às vezes fechava ao carregar o save com o erro
  "Attempted to protect region as write-only" (2 vezes em ~16 testes no santuário). A causa
  provável foi corrigida e, se ainda acontecer, a página é protegida por inteiro e o jogo segue;
  o log (`user/last_run.log`) registra a linha "Write-only protection" para investigação.
- Perfil PGO refeito para o código novo.
- Diagnóstico: com `SOTC_FPS=1` o log mostra a linha `SOTC ARENA` (memória devolvida).
- Testado: 30 minutos de demo (memória estável), 15 minutos com o build final, demo e santuário
  sem erro (pares intercalados contra a 0.57-opt4), imagens iguais (folha de capturas).

## 0.57-opt4 — 2026-10-16 (fork Oussama268)
- **Mais FPS em todo lugar:** a thread que transforma os comandos do jogo em comandos Vulkan (o
  gargalo do port) gastava ~42% do tempo dentro do Windows e da biblioteca C: travas, mudanças de
  proteção de página, cópias e alocações. Medido contra a 0.57-opt3, no mesmo PC e na mesma hora
  (RTX 3060 Laptop, pares intercalados):
  - Santuário: 29,3 / 29,1 → **35,0 / 34,1 FPS** (+18%).
  - Demo da tela de título: 47,4 → **53,5 FPS**, 1% low 29,4 → **37,9**, quadros acima de 50 ms
    129 → **26**.
- **O que mudou:**
  - A trava do cache de texturas é pega uma vez por grupo de texturas, não duas por textura (62
    mil vezes por quadro), e do lado da thread de comandos ficou sem instrução atômica. Falhas de
    escrita do jogo em páginas sem textura nem passam pela trava. `SOTC_TC_BATCH=0`,
    `SOTC_TC_BIASED=0` e `SOTC_TC_SKIP_EMPTY=0` desligam as partes.
  - As tabelas que o jogo preenche todo quadro com um shader de cópia (feito na CPU pelo port) são
    comparadas página por página, e só as páginas que mudaram são escritas, direto na memória da
    GPU, sem marcar a memória como alterada: os envios de buffers por quadro caíram de 21 para
    ~13 MB. `SOTC_FILL_HLE_PAGES=0` volta ao jeito antigo.
  - Um par de imagens de profundidade 960x540 era criado e destruído todo quadro; agora é
    reaproveitado ("depth twin", que já existia como opção e passou a ser o padrão).
    `SOTC_DEPTH_TWIN=0` desliga.
  - Buscas de memória mais rápidas: cache maior das regiões de memória do jogo por thread
    (`SOTC_REGION_DIRECT=0` desliga), cache das faixas de buffer já residentes, relógio lido a cada
    8 comandos.
  - Perfil PGO refeito para o código novo.
- Diagnóstico: com `SOTC_CMD_STATS=1` o log mostra a linha `SOTC sys/frame` (proteções de página,
  travas, alocações de memória de vídeo por quadro).
- Testado: santuário e demo sem erro, imagens iguais (folha de capturas), 15 minutos de demo.
## 0.57-opt3 — 2026-10-16 (fork Oussama268)
- **Menos travadas na primeira visita a uma área:** os pipelines gráficos novos agora são
  montados em threads separadas, e o objeto que precisa de um pipeline ainda em montagem não é
  desenhado por alguns quadros (aparece um instante depois) em vez de o jogo inteiro parar. Com o
  cache de shaders vazio, no demo da tela de título: quadros acima de 50 ms 235 → 156, 1% low
  23,9 → 29,1 FPS. Passes de tela cheia (pós-processamento) continuam sendo montados na hora,
  para não aparecer quadro preto. `SOTC_ASYNC_SHADERS=0` volta ao comportamento antigo.
- A tradução dos shaders (a maior parte do que sobrou da travada) ainda roda na thread principal
  de comandos; levá-la para segundo plano é o próximo passo.
- A pasta de patches de shader é listada uma vez só, em vez de duas consultas ao disco por shader.
- Diagnóstico: com `SOTC_FPS=1`, a linha `SOTC COMPILE` mostra onde vai o tempo de compilação.
- Testado: 15 minutos de demo com cache vazio sem erro; com cache cheio, santuário 32,4 FPS e
  demo 50,0 FPS (sem perda).

## 0.57-opt2 — 2026-10-16 (fork Oussama268)
- **Muito menos engasgos ao entrar em áreas novas (placas de 6 GB):** texturas cujo T# começa num
  nível de mip acima de 0 nunca usam os mips mais finos, mas o port guardava a cadeia inteira na
  memória de vídeo. Agora essas texturas só de leitura guardam apenas os mips usados ("mip tail").
  No demo da tela de título o pico de memória de vídeo caiu de ~6,1 GB para ~3,4 GB numa placa
  de 6 GB, os quadros acima de 50 ms caíram de 150 para ~105 e o pior quadro ao carregar uma área
  de 460 ms para ~280 ms. Imagem idêntica. `SOTC_MIP_TAIL=0` desliga.
- **Leituras da GPU assíncronas por padrão:** quando o jogo lê memória escrita pela GPU (todo
  quadro), só a thread do jogo espera a GPU; o processador de comandos segue trabalhando. Santuário
  29,2 → 30,7 FPS; no demo, ~25% menos quadros acima de 50 ms. `SOTC_RB_ASYNC=0` desliga.
- **Memória de buffers devolvida ao desmapear:** antes ela nunca voltava e a memória de vídeo
  crescia ~90 MB por minuto (1,6 → 6,0 GB em 48 minutos no demo), até faltar memória. Agora cresce
  bem mais devagar (24 minutos: 4,1 GB em vez de 4,8–5,0 GB). Era opcional por causa de uma queda
  antiga ao carregar save, que não se repetiu nos testes (3 carregamentos, 25 minutos de demo).
  `SOTC_ARENA_RELEASE=0` volta ao comportamento antigo.
- Com tudo isso ligado, o demo rodou a 49,9 FPS de média com 47 quadros acima de 50 ms (antes:
  ~47 FPS e 100–150).
- **Quedas deixam registro:** o lançador guarda os logs das últimas 5 sessões (`last_run.1.log`
  …) e copia o log de uma sessão que terminou com erro para `user/crashes/`; o bb-probe grava ali
  um minidump (`.dmp`) quando o jogo cai. `SOTC_MINIDUMP=0` desliga o dump.
- **Relatório de engasgos para desenvolvedores:** com `SOTC_FPS=1` o log mostra a cada 5 s o
  tempo de quadro (mediana, 1% low, pior quadro) e onde foi o tempo dos quadros lentos;
  `tools/frame_report.py` resume um log.
- Memória de buffers em blocos de 4 MB em vez de uma alocação por pedido (`SOTC_ARENA_SLAB_MB`),
  cópia de envios grandes em paralelo (`SOTC_COPY_BATCH`).
- **Ainda não resolvido:** na primeira vez em cada área os shaders são compilados e o jogo engasga
  (até 1–2 s); da segunda vez em diante fica liso. Compilação em segundo plano é o próximo passo.
- Na primeira abertura alguns shaders novos são compilados (poucos segundos); o cache existente
  continua valendo.

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
