# Changelog

## Próxima versão
Correções para placas NVIDIA GTX 10xx (Pascal), testadas numa GTX 1080 Ti (fork de JihedMrouki).
- **Texturas nas placas sem `VK_KHR_fragment_shader_barycentric`** (issue #57): o SotC interpola
  à mão quase todas as entradas dos fragment shaders; sem a extensão elas ficavam constantes por
  triângulo (superfícies sem textura). Um geometry shader gerado entrega ao fragment shader os
  valores dos três vértices e as coordenadas baricêntricas. `SOTC_BARY_GS=0` desliga.
- **Sombras distantes sem piscar** nessas placas: o reaproveitamento das cascatas distantes
  (`SOTC_SHADOW_REUSE`) vem desligado nelas; `SOTC_SHADOW_REUSE=1` liga.
- **Menos engasgos ao compilar shaders:** cada shader novo abria um arquivo inexistente do cache
  (~28 ms no Windows). Demo da tela de título com cache vazio: 32 → 4 s de engasgos de shaders.
- **Lançador:** botão **Pré-compilar shaders** (o demo da tela de título roda sozinho numa janela
  atrás das outras e enche o cache, ~10 min), oferecido também quando o cache foi refeito;
  opção **Tela de carregamento durante o jogo**; **Contador de FPS no jogo** (só o número).
- O cache de shaders é refeito na primeira abertura.

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
