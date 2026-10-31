# Changelog

## 0.70-plus3 — 2026-10-31 (fork Lucas-blip-png)
- **FSR 3.1 sem o visual pixelado em movimento (2mg, #5 do fork):** o FSR 3.1 recebe a imagem final do jogo, já com tonemap e em 8 bits, mas o contexto era criado dizendo que a cor era HDR e pedindo exposição automática. Com isso ele comparava as cores errado e jogava fora o histórico quando a câmera se mexia: grama em manchas, crina do cavalo em degraus, enquanto a cena parada ficava igual à do DLSS. Agora a cor vai como ela é. Demonstração, 720p → 1080p, mesmos momentos: grama com folhas finas e bordas lisas em movimento. Santuário: mesmo brilho, 70,4 FPS. `SOTC_FSR3_HDR=1` e `SOTC_FSR3_AUTOEXP=1` voltam ao antigo.
- **Envios de memória para a GPU mais rápidos (HiddenUserHere, PR #339 do upstream):**
  - `SOTC_COPY_GUEST_VIEW` (ligado): os envios leem a memória do jogo pelo endereço do próprio jogo, protegidos contra falha no tratador de exceções; antes liam por uma segunda visão da memória, que pagava uma falta de página por página recém-escrita. Demonstração: envios nos quadros lentos 2 a 3 vezes mais rápidos por MB, nenhum erro.
  - `SOTC_FILL_HLE_GPU` (ligado): tabelas que o jogo preenche com um padrão repetido são preenchidas na GPU, em vez de copiadas de novo pela CPU a cada varredura (uma tabela de 8 MB, ~33 vezes por carga de área). Demonstração: sem erros, imagem normal.
  - `=0` desliga cada uma.
- **Limite de textura por quadro também zera a cada 16 ms sem quadro novo:** na abertura (muito carregamento, poucos quadros) o limite do 0.70 podia deixar logos e o texto do menu pretos (#341 do upstream, achado pelo Gabriel-TwoBits). Não reproduzido aqui; acima de 60 FPS nada muda no jogo.
- **Lançador:** a dica do Upscaler avisa para deixar o "Modo de baixa latência" da NVIDIA em Desligado ou Ligado com DLSS: em Ultra, o DLSS perde FPS (o Streamline faz o driver aplicar esse modo). A causa em si continua (exigiria o DLSS sem o Streamline).
- **Do tabnul (PRs #349/#351 do upstream):** a contagem do SOTCSTALL não estoura mais depois de zerar as estatísticas; diagnóstico de tempo de CPU por thread e nomes das threads do jogo no profiler.
- O perfil de PGO não foi retreinado para esta versão.

## 0.70-plus2 — 2026-10-31 (fork Lucas-blip-png)
- **Formato de textura inválido não fecha mais o jogo (#10 do fork, pepinou123, RX 9070 XT):** depois de uma corrupção de memória, o tradutor de shaders lia um descritor com formato impossível e parava o jogo (código 0x17, "MapNumberConversion: Unreachable code"). Agora usa um valor neutro: no pior caso um texel errado em vez da queda. A corrupção de texturas "arco-íris" em si continua aberta.
- **AMD GCN (RX 470-590, Vega), para teste: `SOTC_GCN_NV=1` (#8 e #9 do fork, brxzzi; #7, valterf391-ptbr):** o AMD-Fix (raulbezerro-dev) faz a placa se apresentar como driver NVIDIA, e com ele (mais `SOTC_AMD_WAVE32=0`) uma RX 570 parou de perder o dispositivo. Numa placa AMD, isso muda três opções de shader do port: sem arredondamento RTZ, emulação de clip distance e interpolação manual (as barreiras de LDS já estavam ligadas nas GCN). `SOTC_GCN_NV=1` liga as três sem a camada; `SOTC_RTZ=0`, `SOTC_CLIP_EMU=1` e `SOTC_MANUAL_INTERP=1` ligam uma de cada vez, para achar qual resolve. Padrão sem mudança. O cache de shaders é refeito na primeira abertura com uma delas ligada.
- **Correção da nota do 0.70-plus1:** o heap guard sem repetir nós (`SOTC_GUARD_DEDUP`) continua desligado, mas o motivo dado estava errado. Os quadros de 200-575 ms sem causa no lado da GPU também apareceram com ele desligado (296 e 241 ms numa sessão de 20 min): são do próprio jogo, e o A/B de sessões curtas enganou.
- O perfil de PGO não foi retreinado para esta versão.

## 0.70-plus1 — 2026-10-30 (fork Lucas-blip-png)
- **Versão 0.70:** traz do 0.70 do upstream (AjncSystem) a parte que vale para este fork. O resto dele já existia aqui ou não ajudou nas medições.
- **Trocas de área com menos trancos de textura (AjncSystem, 0.70, 7fde988):** ao entrar numa área, o jogo pede ~100 MB de texturas novas num quadro só, e esse quadro levava 80-130 ms para enviá-las. Agora entram no máximo 32 MB de texturas grandes novas por quadro; as que ficam para o quadro seguinte aparecem pretas por um instante. `SOTC_UPLOAD_BUDGET_MB=0` desliga, outro número muda o limite. Teste do usuário (mesmo caminho a pé, mesmo save, 1440p DLSS-G): pior quadro 133 ms com ele contra 174 ms no plus27, 1 quadro acima de 100 ms em cada.
- **Não trazido, com o motivo:**
  - Percurso do heap guard sem repetir nós (`SOTC_GUARD_DEDUP`): veio portado, mas sai **desligado**. Em todas as sessões do usuário com ele ligado houve quadros de 200-575 ms sem causa no lado da GPU, e em nenhuma sem ele. `SOTC_GUARD_DEDUP=1` liga para teste.
  - Mudanças na trava da memória do jogo: nos piores quadros daqui a espera pela trava foi de até 2 ms; o pico que sobra é o envio de 200-250 MB de buffers num quadro (até 123 ms), que é o próximo trabalho.
  - Correção da queda `0x49800`: vinha de dois atalhos do heap guard que o upstream criou e desligou; este fork nunca os teve.
  - Vistas de memória em pedaços: já existiam aqui (`SOTC_VIEW_GRANULE_MB`).
- **Ferramenta de teste:** `tools/sotc_bench_shrine.py --replay-after-load` reproduz uma rota gravada com F9 a partir do save de um jogador (rotas a cavalo ainda desviam).
- O perfil de PGO não foi retreinado para esta versão.

## 0.60.6-plus27 — 2026-10-30 (fork Lucas-blip-png)
- **DLSS sem perder FPS só por ter o Reflex (2mg, #5 do fork):** com o DLSS só ampliando a imagem, o Streamline segurava cada quadro com a espera do Reflex: Santuário (1080p de 720p, RTX 5060 Ti) 68,0 FPS com ela, 72,4 sem, igual ao FSR 3.1 (72,7). Agora a espera só roda com a geração de quadros DLSS-G ligada (`SOTC_DLSSG_SLEEP=0` desliga sempre, `=2` força). E com a geração de quadros desligada ao abrir o jogo o Streamline carrega só o plugin do DLSS, sem Reflex nem DLSS-G: é a causa provável da queda com o **Low Latency Mode: Ultra** do driver numa RTX 2080 Ti (48,5 FPS contra 59,1 com Off, teste do 2mg); ainda não confirmado com o Ultra. Com isso, numa RTX 40/50 o F8 só liga o DLSS-G se o jogo abriu com a geração de quadros ligada no lançador (o FSR 3 continua no F8). `SOTC_SL_ALL=1` carrega tudo como antes.
- **Nitidez (RCAS) também no FSR 3.1, no DLSS e no FSR 1 sem ampliação (pedido do 2mg):** a nitidez do F1 e do lançador só agia dentro da ampliação do FSR 1 e não fazia nada com FSR 3.1, DLSS ou FSR 1 com a imagem já no tamanho da saída (1080p → 1080p). Agora vale em todos (não no bilinear). A nitidez padrão (Média) passa a agir no FSR 3.1 e no DLSS também: a imagem fica um pouco mais nítida que antes; "Desligada" volta ao visual anterior. Santuário 1080p com DLSS: 70-72 FPS com e sem ela (ruído).
- **F8 não desliga mais o DLSS/FSR 3.1 depois de passar pelo FSR 3:** sair da geração de quadros FSR 3 liberava as imagens de profundidade que o upscaler também lê, e todo quadro depois disso era a ampliação bilinear (teste do usuário a 4K, 29/10). Teste com 4 ciclos DLSS-G → FSR 3 → desligada: nenhum quadro bilinear depois.
- **4K com DLSS Desempenho sem chão preto e grama em cartões:** o ajuste de nível de textura do upscaler (`log2(render/saída) - 1`) chegava a -2 e esgotava o pool de páginas de textura do jogo. Agora para em -1 (`SOTC_MIP_BIAS` muda).
- **VRAM perto do limite:** a memória fria das arenas de buffer também volta para a placa quando o uso total passa de 85% do orçamento (antes só com as arenas acima de 4 GB). Numa sessão de 53 min a 4K com DLSS-G (placa de 8 GB) o uso chegou a 6,9 de 6,2 GB, a limpeza de texturas rodava sem parar (74 → 35 FPS) e as texturas corromperam. `SOTC_ARENA_EVICT_VRAM_PERCENT=0` desliga.
- **DLSS sem o contexto do FSR 3.1 junto:** com DLSS o jogo criava o contexto e as imagens do FSR 3.1 (64 MB em 1080p, ~200 MB em 4K) sem usar.
- **Lançador:**
  - **"Sugerir para meu PC"** (Imagem e FPS): escolhe saída, upscaler, render e texturas pela placa, VRAM, CPU e tela, e explica por quê.
  - **Preset "Máximo"** (texturas Máxima, grama mais longe e mais densa, 3 cascatas de sombra da geometria virtual, FPS livre; DLSS nativo/DLAA até 1600 linhas de altura, Qualidade acima), para placas com 8 GB de VRAM ou mais (12 GB em 4K). No "Equilibrado" as placas RTX usam DLSS.
  - **Saves em Documentos\My Games\Shadow of the Colossus PC** por padrão: ao trocar ou apagar a pasta do port, os saves ficam. Na primeira abertura os saves de `user\savedata` são copiados para lá (a cópia antiga fica).
  - **Pular a cinemática de abertura** (`SOTC_SKIP_INTRO`): segura o X sozinho até a tela de título.
  - **Geração de quadros:** em placas que não são RTX 40/50 (RTX 20/30, AMD, Intel), DLSS-G e "Automática" ficam na lista em cinza, com o motivo, e não podem ser escolhidas; quem tinha "Automática" passa para FSR 3. Ali o "Automática" carregava o Streamline da NVIDIA à toa.
- **Menu F1:**
  - O Upscaler lista FSR 1, Bilinear, FSR 3.1 e DLSS; o que não pode ser trocado em jogo fica em cinza com "só no lançador" ou "só NVIDIA RTX".
  - A geração de quadros indisponível diz por quê ("ligue no lançador antes de abrir o jogo", "só NVIDIA RTX 40/50", "não suportada nesta placa").
  - **Qualidade das texturas muda só no lançador:** trocar em jogo podia fechar o jogo (Máxima em vistas amplas) e "Desligada" não fazia nada até reiniciar. O F1 mostra o nível em uso.
- **README:** requisitos primeiro, com FPS e VRAM medidos por preset e resolução (Santuário, Ryzen 7 5700X + RTX 5060 Ti 8 GB), depois o roteiro e as novidades.
- Diagnóstico: quando a GPU para ou o dispositivo é perdido, o registro mostra as 32 últimas operações da fila.
- O perfil de PGO foi retreinado em 4K (DLSS Desempenho); sem ganho medível contra o anterior.
- **Não trazido do 0.70 do upstream (ainda):** a correção das quedas de FPS nas trocas de área (AjncSystem, 7fde988) mexe no núcleo de memória, que aqui está diferente (23 conflitos); fica para a próxima versão, com teste próprio.

## 0.60.6-plus26 — 2026-10-29 (fork Lucas-blip-png)
- **CPUs de 4 threads mais rápidas (cochilo das threads de jobs, do fork do MMadmer, c301eef):** as threads de jobs do jogo ficavam testando a fila sem parar enquanto esperavam trabalho (no PS4 cada uma tem um núcleo só seu). Agora, depois de 64 tentativas vazias seguidas, cochilam 100 µs e devolvem o núcleo ao render. Santuário, A/B intercalado: **4 CPUs 61,8 e 60,0 FPS ligado contra 49,3 e 52,8 desligado (+19%)**; 16 CPUs sem diferença (69,5/75,8 contra 69,2/75,7). `SOTC_POLL_NAP_US=0` desliga. Crédito: Madmer (MMadmer), autor do commit.
- **Contagem de CPUs pela máscara de afinidade:** o modo de CPU fraca do processador de comandos e o número de workers de pipeline usavam o total de threads do sistema, não as que o processo pode usar (ideia do `UsableCpuCount` do MMadmer). Em PCs de verdade nada muda; os testes com 4 CPUs agora representam um PC de 4 threads.
- **Texto do HUD sem rastro com FSR 3.1 e DLSS:** os upscalers recebiam o quadro final já com a interface, e o FSR 3.1 misturava as letras com a cena em movimento (dica "MANTÉM PREMIDO … PARA IGNORAR" quebrada e dobrada na cinemática do Santuário). Agora eles ampliam o quadro sem a interface, e a interface é recolocada por cima depois. Medido em gravação de tela, 720p → 1080p: FSR 3.1 Qualidade passa de letras pontilhadas a sólidas; DLSS Qualidade fica parecido com antes, um pouco mais suave. `SOTC_UPSCALER_HUD=0` volta ao antigo.
- **Barras de vida e resistência do tamanho certo com resolução de render reduzida** (FSR 3.1/DLSS em Qualidade e abaixo): os retângulos das barras usavam a largura de render em vez da largura virtual do HUD (1080 × aspecto).
- **Trava de 30 FPS no modo PS4 Pro (AjncSystem, 0.60.6-C3, 2956b7f):** quando o jogo pede "sem limite de flip", o "30" do menu ou do F7 não fazia efeito (42 FPS em "30"). Agora segura 30.
- **DMA desligado por padrão em placas Intel (Oussama268, 9077704):** como já era na AMD; corrige listras na Intel Arc B580 relatadas no fork dele. Na Intel o cache de shaders é refeito na primeira abertura. `BB_DIRECT_MEMORY_ACCESS=1` religa.
- **HDR (tabnul, pelo fork do Oussama268, a21bee0; sem a parte do DLSS):** o jogo escolhe o HDR pelo formato do buffer de flip, o branco de referência é 200 nits (`SOTC_HDR_WHITE=100` volta ao brilho do jogo) e o FSR 1 em HDR não eleva mais a imagem ao quadrado. Não testado em tela HDR aqui.
- **Força da vibração de 0 a 100% (pad do MMadmer, 77e6282, #210; controle deslizante do xeodeo, dfe962c):** página Controles do lançador, "Vibração"; `SOTC_VIBRATION=0..100`. A caixa "Vibração do controle" e o menu do jogo continuam ligando e desligando. Não testado com controle aqui.
- **Lançador:** a roda do mouse sobre uma caixa de opções fechada rola a página em vez de trocar a opção (xeodeo, dfe962c). O texto do "submit periódico" diz que o padrão vale para NVIDIA e AMD, como no código.
- **Diagnóstico para AMD GCN (#7, #8):** o registro mostra quando a GPU recusa um bind esparso (antes era ignorado em silêncio) e avisa quando o wave32 roda fora da especificação do Vulkan nas RX 400/500/Vega. O padrão não mudou: há quedas com ele ligado e desligado em placas de 4 GB, e falta um teste com o registro novo.
- Diagnóstico: o registro mostra a versão do DLSS (Streamline/NGX), o modo e o preset, e a cada 600 quadros quantos passaram pelo FSR 3.1/DLSS e por que os outros foram bilineares.
- O perfil de PGO não foi retreinado para esta versão.

## 0.60.6-plus25 — 2026-10-28 (fork Lucas-blip-png)
- **21:9 e 32:9 sem a cena esticada (#4, amarshall9062; o mesmo sintoma em #327 e #331 do upstream):** desde o plus16 (correção do HUD do C2, #301) o aspecto do objeto de tela ficava em 16:9, e é ele que também define o da cena 3D, então em tela larga o jogo era a imagem 16:9 esticada para os lados. Medido no Santuário e na demonstração em 3176x1320: o Wander ficava mais largo e sem mais cenário nas laterais; agora tem as proporções de 16:9 e mostra mais cenário dos lados. Vale só para saídas mais largas que 16:9; 16:9 não muda.
- **Conhecido, volta com isso:** o pedaço cinza na barra de vida em 21:9 (#301). A barra de resistência continua certa. Para escolher o outro lado (cena esticada, sem o pedaço cinza), coloque `SOTC_WIDE_ASPECT=0` em "Variáveis extras". Procurando a causa do pedaço cinza sem estragar a cena.
- **Biblioteca do lançador** mostra a versão 01.01, a mesma da aba Play (#5).

## 0.60.6-plus24 — 2026-10-28 (fork Lucas-blip-png)
- **Cache de shaders em um arquivo por sessão (journal), escritor em segundo plano e pipelines de compute especulativos (Lukto02, PR #3):** com cache frio, a sessão grava 1 arquivo em vez de ~8,3 mil arquivos pequenos; na próxima abertura as entradas vão para o pack e o journal é apagado. O escritor do cache roda em modo de segundo plano no Windows. Os pipelines de compute das traduções antecipadas são montados já na instalação. Teste aqui (demonstração da tela inicial, cache frio, 4 rodadas com o código ligado e desligado): sem diferença de FPS nem de 1% low (63,0 e 63,4 contra 63,8 e 63,5), sem erros novos; partida seguinte leu 8779 registros do journal e gravou o pack. O ganho que o autor mediu na abertura (cinemática, cache frio) não apareceu na demonstração. Desligar: `SOTC_CACHE_JOURNAL=0`, `SOTC_CACHE_IO_BACKGROUND=0`, `SOTC_COMPUTE_SPECULATE=0`, `SOTC_PREFETCH_PRIORITY=0`. O cache de shaders não é refeito.

## 0.60.6-plus23 — 2026-10-28 (fork Lucas-blip-png)
- **F8 em rajada não derruba mais o driver da NVIDIA:** alternar a geração de quadros várias vezes seguidas (DLSS-G, FSR 3, desligada, duas vezes em 2 s) fechou o jogo dentro de `nvoglv64.dll` 67 ms depois de o FSR 3 liberar suas imagens. A troca acontece no máximo a cada 1,5 s e os toques extras são ignorados. Visto uma vez; depois da correção, cerca de 40 toques seguidos não derrubaram o jogo.
- **Diagnóstico `BB_WRITE_LOG=3` (quedas com leitura de `0xffffffffffffffff`, #88 e #325):** guarda, por página de 4 KiB da memória do jogo, as 4 últimas escritas que a GPU fez (readback de buffer, download de imagem, fence). Numa queda, as linhas `Write trace` do relatório listam as que cobrem o cabeçalho do bloco que o jogo leu, marcadas `OVERLAP`. Para ligar, coloque `BB_WRITE_LOG=3` em "Variáveis extras" do lançador; custa uns 7 a 9% de FPS e reserva cerca de 400 MB de memória virtual. Desligado por padrão. Quem enviar o zip de "Relatar problema" depois de uma queda com isso ligado ajuda a achar a causa (relatos de ta955995-cmd e brxzzi).
- **Conhecido, sem causa ainda:** chão do deserto com ruído colorido e faixas ciano depois de carregar uma área (visto em vídeo no plus22; a demonstração da tela inicial não reproduz). Se acontecer, `SOTC_EXPAND_COPY_STALE=1 SOTC_DMA_OVERLAP=0 SOTC_REUSE_SIZE_CHECK=0` em "Variáveis extras" restauram o cache de texturas anterior ao plus22; diga se o chão volta ao normal.
- O perfil de PGO não foi retreinado para esta versão.

## 0.60.6-plus22 — 2026-10-28 (fork Lucas-blip-png)
- **Notas da atualização em inglês no lançador (#321, pedido de Khimari):** a janela "Procurar atualização" mostra o texto do release no idioma do lançador (seção `## English` do release).
- **Vazamento de memória do processador de comandos corrigido (PR #2, Lukto02 / Luca Zarecki):** os quadros dos buffers indiretos aninhados (~700 por segundo) nunca eram liberados, cerca de 44 MB por minuto (2,6 GB por hora). Agora são liberados, depois que a thread de desenho termina o trabalho enfileirado deles; `SOTC_CP_FREE_TASKS=0` volta ao antigo. Medido aqui no Santuário parado, 5 min: `main` +31 MB/min, com o PR estável; armadilha de falha do autor (`SOTC_TASK_GUARD=1 SOTC_CP_FREE_NOW=1`) sem falta. Deserto A B B A sem regressão.
- **Correções do cache de texturas da 0.60.6-C1 (82229db, AjncSystem), trazidas pelo mesmo PR:** `ExpandImage` só copia a imagem velha se ela tem dados da GPU (upstream shadPS4 #5444), cópias e preenchimentos DMA marcam todas as imagens sobrepostas, e um slot reaproveitado com outro tamanho troca a textura em vez de ver pela velha. `SOTC_EXPAND_COPY_STALE=1`, `SOTC_DMA_OVERLAP=0` e `SOTC_REUSE_SIZE_CHECK=0` voltam ao antigo. Também `SOTC_FRAMETIME_OUT` (tempo de cada quadro em arquivo) e `tools/sotc_frametimes.py`.
- O conserto do alocador de páginas (`0x1a371`) continua ligado por padrão; `SOTC_POOL_FIX=0` o desliga (o PR o desligava, e foi corrigido na revisão).
- **Notas da atualização em inglês (#321, pedido de Khimari):** o texto de um release pode ter uma seção `## English`; a janela de "Procurar atualização" mostra a parte do idioma do lançador. Vale a partir do release seguinte ao que instalar este lançador; releases antigos continuam mostrando o texto inteiro.

## 0.60.6-plus21 — 2026-10-28 (fork Lucas-blip-png)
- **Diagnóstico:** com `SOTC_FPS=1` o relatório de FPS mostra quantas chamadas de proteção de página o lado da GPU faz por segundo e quanto tempo gastam (total e na thread de desenho). Sem efeito fora do diagnóstico.
- **Último recurso de buffer em RAM:** o fallback dos buffers (`buffer.cpp`) pedia dois flags de acesso do host ao mesmo tempo num buffer HostCached (RANDOM mais SEQUENTIAL), o que a VMA recusa; a última tentativa nunca podia dar certo. Agora troca um pelo outro. Visto no #319 (RX 7600, aborto de 0x17 com 6 GB de RAM livres: "Failed allocating HostCached buffer of 64 KB"). Não reproduzi; é uma correção de código sem prova de que era a causa única.
- **Mapeamento fixo sobre memória viva avisa a GPU:** `place()` com `MAP_FIXED` apagava as áreas antigas sem mandar o `HOOK_UNMAP`, então o lado da GPU podia manter buffers e imagens do conteúdo antigo. Agora o aviso sai antes, como no `unmap`. Não consegui provar que isso causava as quedas de #283/#289/#255 (sem reprodução); a rota do deserto (2 passadas, 78 e 86 FPS) e as quedas não mudaram.

## 0.60.6-plus20 — 2026-10-27 (fork Lucas-blip-png)
- **Cache de shaders frio mais curto (tradução à frente de compute):** antes de executar um buffer de comandos, o processador de comandos
  varre os pacotes (seguindo os buffers indiretos), acha os `COMPUTE_PGM` + `DISPATCH` e manda traduzir os shaders de compute novos nos
  workers, em paralelo; o dispatch espera o resultado em vez de traduzir de novo. Nada é descartado nem refeito (a versão assíncrona
  antiga perdia dispatches de execução única, como a névoa do Santuário). Se a previsão errar, só nasce uma permutação a mais. Rota do
  deserto largo com o cache apagado (3840x1080, 5700X): pior quadro de 493-618 ms para 174 ms, tempo de shader no pior quadro de 360-470 ms
  para 6 ms; 150 computes que antes compilavam na thread de comandos passaram para 24. Com o cache quente não muda nada. `SOTC_PREFETCH_COMPUTE=0`
  desliga. O cache de shaders não é invalidado.

## 0.60.6-plus19 — 2026-10-27 (fork Lucas-blip-png)
- **Grama ajustável** (distância, altura, mato, detalhe e fade; menu da grama dentro do jogo; buffers de grama 4x maiores): trabalho de
  tabnul (197754f), AjncSystem (30002ea: o menu mostra o valor do jogo em vez de 0) e Oussama Afassi (teste do lançador, #94), trazidos do
  0.60.6-C1 com a autoria original. Mudança de padrão: sombras da grama em 2 (como no PS4; antes 3) e detalhe do patch 0,25. Testei só que
  o jogo abre e que a rota do deserto (196 s, 3840x1080) roda sem queda; falta olhar a grama de longe e o custo de VRAM em jogo.
- **Predefinições no lançador** (página de imagem): Compatibilidade, Equilibrado e Desempenho ajustam FPS, upscaler, textura, filtro e
  geração de quadros de uma vez; cada opção continua editável depois.
- **Aviso de caminho:** o lançador avisa quando a pasta do port ou do jogo tem acento ou é longa (mais de 150 letras), pois repacks em
  caminhos assim estavam por trás de jogos que fechavam sem erro (#299, #282).
- **Diagnóstico da saída silenciosa (código 0xffffffff depois de "Resource Loading", #244, #291, #299, #61):** o log passa a dizer se o
  processo terminou por `exit()` ou por `abort()`. Sem nenhuma das duas linhas, o processo foi encerrado de fora (antivírus, por exemplo).
- **Memória flexível dividida (`split_at`):** ao dividir um mapeamento flexível (mprotect ou munmap no meio), a metade direita mantinha o
  endereço de backing da esquerda, e leituras, escritas e a liberação seguinte usavam as páginas erradas. Achado ao analisar as quedas por
  leitura de endereço não canônico (#283, #289, #255); não provei que esse caminho seja o que as causa, nenhuma queda foi reproduzida aqui.
- Medições (Santuário, Ryzen 7 5700X, RTX 5060 Ti): Mailbox e FIFO empatam (~70 FPS, mesmo p99); por número de CPUs 4/6/8/16 dão
  56/65/70/73 FPS (4 CPUs eram 34 antes da prioridade de threads); a `GpuDraw` está plana, nenhuma função passa de 3%.

## 0.60.6-plus18 — 2026-10-27 (fork Lucas-blip-png)
- **Campo de visão ajustável (#179):** o lançador ganhou "Campo de visão" (100% original a 140%). A câmera do jogo converte os graus
  em radianos num único ponto (uma constante de 4 bytes, sem outro uso), então o patch multiplica essa constante e vale para todas as
  câmeras; LOD e cascatas de sombra seguem a matriz. `SOTC_FOV_SCALE=<0,5 a 1,6>` faz o mesmo sem o lançador. Achado por engenharia
  reversa do eboot EU 1.01 (nota em `out/fov_re_notes.md`, não vai no repositório). Medido no Santuário a 1080p: 140% mostra bem mais
  cenário, sem artefatos; os comandos por quadro sobem de 5744 para 7302 (mais objetos na tela) e o FPS cai de 74,5 para 69,3. Só
  testei o Santuário parado; câmeras de colossos e cutscenes não foram vistas. O aumento é só vertical, o horizontal acompanha o aspecto.
- **Vibração liga/desliga dentro do jogo** (menu F1), mantida como opção do lançador: trabalho de AjncSystem (0.60.6-C1/C2, 307e024),
  entrou com a autoria dele.
- Desempenho: `SOTC_RB_HOT` conferido de novo no Santuário (0 = 53 FPS, 1 = 74-75, 2 = 72-74): o padrão 1 fica.

## 0.60.6-plus17 — 2026-10-27 (fork Lucas-blip-png)
- **Correção urgente: o lançador fechava ao abrir nas versões plus15 e plus16** (`KeyError: 'fx_sunshadows'`, relato de geosaw616 no #316).
  A opção "Sombras do sol" tinha entrado sem valor padrão. Agora tem, e um teste confere que todo efeito do lançador tem padrão.
  Quem está no plus15 ou no plus16 deve baixar esta versão (o atualizador do lançador não abre nelas).

## 0.60.6-plus16 — 2026-10-27 (fork Lucas-blip-png)
- **Barras de vida e stamina em telas 21:9 e mais largas (#301, #202, #145, #315):** o objeto de tela do HUD mantém o aspecto 16:9
  (pedaço cinza na barra de vida) e os dois retângulos de 1920 de largura da barra de stamina são escalados (a barra amarela parava
  antes do círculo e enchia ao contrário). Trabalho de AjncSystem (0.60.6-C2), confirmado por testes dele e do usuário em 2560x1080;
  os commits entraram com a autoria original. `SOTC_WIDE_ASPECT=1` (teste) e `SOTC_RENDER_KEEP` ajudam a achar outros pontos do HUD.
  Não testado aqui em tela larga de verdade (só a geração dos patches e a rota de 3840x1080 sem queda).
- **Cache de shaders em um arquivo só** (`cache.sotcpack`, `SOTC_CACHE_PACK=0` desliga), também de AjncSystem (82229db): em HD, os
  ~12 mil arquivos pequenos levavam de 30 a 100 s para ler quando o Windows os tirava do cache de arquivos. O arquivo (~200 MB) é
  criado em segundo plano na primeira abertura.
- Correção: `sotc_modes.py` faltava `import os` depois de trazer o commit da C2 (quebraria toda abertura).

## 0.60.6-plus15 — 2026-10-27 (fork Lucas-blip-png)
- **Sombras do sol desligáveis no lançador** (efeitos visuais → "Sombras do sol", `r_enableCascadeShadows=0`): ideia e medição de
  4isaque4 (#294), com repetição intercalada: +34% de FPS a 720p numa Ryzen 7 5700X + RX 6700 XT, sem sombras do sol. Vem ligado.
- **RX 9070 XT, `ErrorOutOfDeviceMemory` em buffers de 24 a 96 MiB (#293, relato e logs de Coriolys):** com 13 GiB de orçamento livre,
  o driver recusava o buffer (memória dedicada com endereço de dispositivo) e o jogo fechava com o código 0x17. Agora, antes de
  abortar, o buffer é tentado sem memória dedicada e depois em memória do sistema (o log mostra `SOTC buffer fallback`).
  Não testado numa placa AMD: depende de quem reportou confirmar.
- **Diagnóstico de stutter:** tradução de shader na thread de comandos aparece no log como `SOTC SYNC PIPELINE shader (compute)`
  (limite em `SOTC_SYNC_PIPELINE_MS`). Com o cache de shaders frio, 386 shaders de compute (cerca de 913 ms) são traduzidos ali, em
  rajadas ao entrar numa área; `SOTC_ASYNC_COMPUTE_TRANSLATE=1` (experimental, desligado) leva o pior quadro de 571 para 188 ms,
  mas descarta um compute de execução única e a névoa do Santuário some. Números e o que não deu certo: `docs/sotc/STUTTER.md`.
- Medido: no deserto com cache aquecido o plus14 teve 3 a 5 quadros acima de 50 ms em 196 s, contra 78 do plus12 (a rota varia).

## 0.60.6-plus14 — 2026-10-26 (fork Lucas-blip-png)
- **Caminhos rápidos do processador de comandos e do despacho ligados por padrão** (`CpFastPaths`, bit 20, e `DispatchFastPath`,
  bit 40, do PR #308 de Lukto02 / Luca Zarecki; ele os deixou desligados porque no teste dele o quadro não mexia). Medimos em
  ABBA na RTX 5060 Ti: Santuário 73,4 → 76,4 FPS (+4%) com a mesma carga e, com o PGO retreinado, 71,7 → 75,5 FPS (+5%) contra o
  plus13. `SOTC_CP_VERIFY=1` e `SOTC_DISPFAST_VERIFY=1` (do autor) deram 0 divergências em mais de 50 milhões de checagens.
  A rota do deserto largo divergiu entre rodadas (de 4250 a 7500 comandos por quadro) e não serve para concluir; nela o lado novo
  ficou igual ou melhor. `SOTC_CP_FAST=0` e `SOTC_DISPFAST=0` desligam. Não testado: AMD/Intel.
- PGO retreinado com os caminhos novos (NVIDIA; veja `docs/sotc/PGO.md` para contribuir com AMD/Intel).

## 0.60.6-plus13 — 2026-10-26 (fork Lucas-blip-png)
- **Desempenho do PR #308 do upstream, de Lukto02 (Luca Zarecki):** caminhos rápidos na thread de desenho (`GpuDraw`),
  DrawReplay, intervalos de leitura de volta, vigia de heap do BPE e ajustes de memória, tudo atrás de bits do `BbToggle`
  (`bbport_toggles.h`) e com variáveis `*_VERIFY=1` do autor. Mesclado com os 5 commits originais, autoria dele mantida.
  A/B intercalado (ABBA) contra o `main` anterior, mesmo perfil PGO antigo: Santuário 62,3 / 62,6 → 67,8 / 67,9 FPS (+8,6%);
  rota do deserto largo (3840x1080) 48,6 / 45,6 → 61,2 / 55,5 FPS (cerca de +24%, a rota varia de uma rodada para outra).
  Sem quedas nem "Jogo parado" nos logs. Não testado: AMD/Intel nem uma sessão longa de jogo.
- **PGO retreinado** com o código novo (`tools/pgo_train.sh`, RTX 5060 Ti): mais 6% no Santuário (68,5 → 72,6 FPS) e 7% no deserto
  (55,0 → 58,9 FPS) contra o perfil antigo de 17/10.
- **Três pacotes por release:** `SotC-PC.zip` (Livre, sem configuração pronta, é o que o atualizador baixa),
  `SotC-PC-Otimizada-NVIDIA.zip` (DLSS Qualidade, 60 FPS) e `SotC-PC-Otimizada-AMD-Intel.zip` (FSR 3.1 Qualidade). O perfil
  PGO só foi treinado em NVIDIA; veja `docs/sotc/PGO.md` para enviar um de AMD ou Intel (quem enviar será creditado).
- Também: tamanho de saída 1600x900 (pedido do usuário 2mg no #306).

## 0.60.6-plus12 — 2026-10-26 (fork Lucas-blip-png)
- **Textura Máxima no modo PS4 Pro (experimental, da 0.60.6-C1):** o launcher e o menu do jogo não trocam mais a Máxima
  por Alta no modo Pro, agora que a correção do alocador de páginas está no fork. `SOTC_MAX_IN_PRO=0` volta ao
  comportamento antigo. Observação: com DLSS ou FSR 3 o modo Pro continua desligado (o upscaler usa o tamanho do jogo).
- **F7 em 3 estágios (da 0.60.6-C1):** 30, 60 e FPS livres (antes só travava e destravava).
- Testes Python: os dois de caminhos Linux/Nix passam a ser pulados no Windows.

## 0.60.6-plus11 — 2026-10-26 (fork Lucas-blip-png)
- **CPUs de 4 threads: prioridades das threads** (#267, ideia primeiro feita no fork do MMadmer): com 4 processadores
  lógicos ou menos, as threads de som sobem para a prioridade mais alta, as `BPE JobWorker` descem para abaixo do normal e
  as de arquivo (`SceFios*`, `Resource Loading`) sobem para acima do normal. `SOTC_JOB_PRIORITY=0` desliga. Santuário com
  o processo limitado a 4 CPUs (uma por núcleo): 34,0 / 33,8 FPS sem, 46,9 / 48,5 FPS com. CPUs maiores não mudam.
- **Correção do alocador de páginas do jogo (queda `0x1a371`, tabnul, PR #288 do upstream, trazida da 0.60.6-C1):** o
  contador de páginas livres do jogo se descolava do mapa real e o alocador lia antes do início do mapa. Agora, quando a
  busca falha, o contador volta a 0, um pedaço novo é separado e a busca roda de novo. Só é aplicada se os bytes do
  executável baterem. Testado até a tela de título e o jogo abrindo sem queda; a correção em si só aparece quando a queda
  aconteceria.
- **Leitura de memória que travava o jogo em CPUs de 4 threads** (fork do MMadmer): uma página marcada como escrita pela
  GPU, sem nenhum dado da GPU por trás, ficava protegida para leitura e a leitura falhava para sempre. Agora a página é
  liberada. `SOTC_READ_FAULT_DROP=0` desliga. Santuário em 4 CPUs: sem regressão e sem travamento. Também do MMadmer: ao
  devolver memória fria de uma arena, o port agora sempre termina (e espera) as draws já gravadas antes do desmapeamento.
- **Lançador: "Multiplicador do DLSS-G"** (2x, 3x, 4x, 6x, dinâmico): 3x a 6x geram 2 a 5 quadros por quadro do jogo na
  NVIDIA RTX 50 (RTX 40 fica em 2x). O dinâmico só vale se o driver disser que suporta; senão vale 6x. Na RTX 5060 Ti: 6
  quadros apresentados por quadro, estado 0. Experimental. Padrão continua 2x.
- **Diagnóstico:** novo "Registro detalhado (diagnóstico)" no lançador (`user/texture_diag.log`, até 400 MB, ~7% de FPS)
  com o motivo de cada textura descartada; aviso nas sombras distantes (podem piscar); o .zip de "Reportar problema" agora
  leva também a sessão anterior e os últimos 8 MB do registro detalhado. A análise da água na captura do RenderDoc está em
  `docs/sotc/AGUA.md` (a posterização aparece na passada de iluminação; causa ainda não provada).

## 0.60.6-plus10 — 2026-10-25 (fork Lucas-blip-png)
- **Texturas menos borradas depois de carregar:** a válvula de LOD da 0.60.6-plus8 passava a mostrar as texturas no nível 5
  (o mais grosso) sempre que o pool de páginas do jogo marcava 0% livre, e depois de cada carregamento o jogo o mantém
  em 0% por um minuto ou mais sem falhar; as texturas ficavam no nível 5 até 40-70 s depois e nunca passavam do 4. Agora
  o nível 5 só vale quando a memória livre está caindo abaixo de 1% e não houve carregamento nos últimos 60 s; fora
  disso o limite é o nível 4, como na plus7. No Santuário: 0% livre ao carregar mostra o nível 4 e, com 10% livre, o 3.
  Soak do deserto a 3840x1080 (3 corridas de 196 s): sem queda `0x1a371`.
- **Diagnóstico:** `tools/sotc_bench_shrine.py --video` também grava durante uma sessão manual (`--record`).

## 0.60.6-plus9 — 2026-10-25 (fork Lucas-blip-png)
- **Lançador: "Vibração do controle"** (Controle): desmarcada, o controle não vibra (`SOTC_RUMBLE=0`). Vale na próxima
  vez que o jogo abre (#292).
- **Lançador: "Geração de quadros sem a interface (experimental)"** (`SOTC_FG_HUDLESS=1`): gera os quadros intermediários
  só com a cena e põe a interface por cima, sem interpolá-la, o que tira o tremor das barras de vida e stamina e dos pelos
  do Agro com a geração de quadros (#206, #210). Pode desbotar o logo na tela de título e mostrar a cena antiga atrás das
  telas de carregamento; por isso vem desligada.
- **Diagnóstico:** a gravação de comandos do controle (`BB_PAD_RECORD`) termina no primeiro F9; um segundo F9 reabria o
  arquivo e apagava a gravação. Duas rotas de areia gravadas em `tools/routes/`. Soak do deserto: 3 corridas de 196 s e
  uma sessão manual de 3 minutos na areia, sem queda `0x1a371`.

## 0.60.6-plus8 — 2026-10-25 (fork Lucas-blip-png)
- **VRAM automática** (nova opção padrão do lançador): o "limite de 6 GB" do overlay é o orçamento que o Windows e o driver
  dão ao jogo (`VK_EXT_memory_budget`), não uma configuração. O port agora escolhe sozinho, por esse orçamento, quando
  começar a liberar texturas e quanto manter de buffers: menos de 5,5 GB usa 50% e 1024 MB; até 10,5 GB usa 75% e 4096 MB;
  acima disso 85% e sem limite de buffers. Quem já tinha salvo "normal" ou outra opção mantém a escolha.
- **Resolução até 1440p** com FSR 3.1 e DLSS (antes 1080p): o 4K nativo continua bloqueado porque perde as texturas finas
  (pedra e roupas ficam visivelmente borradas) e roda a cerca de 43 FPS na RTX 5060 Ti. 1440p nativo: 54 FPS; 1080p para
  4K por FSR 1: 54 a 58 FPS; DLSS 1080p para 4K: 52 a 53 FPS. O caminho simples (FSR 1 e bilinear) continua em 1080p.
- **Pool de texturas (falha em `0x1a371`, #259, #293, #283, #241):** a válvula de LOD confere o pool com mais frequência
  quando já está engrossando mips, ganhou um nível extra (mip +3) abaixo de 1% livre e só solta acima de 2%. A falha de
  origem está no jogo (lê o mapa de páginas com índice -1 quando não há página livre) e só reproduzi com
  `SOTC_LOD_LEVEL=0`; com a válvula nova três corridas de 196 s não falharam. Se continuar acontecendo, mande o `last_run.log`.
- **Diagnóstico:** `SOTC_GPU_TIME=1|2` (com `SOTC_WAIT_MARKER=1`) mede o tempo ocupado da GPU por segundo, carimbos de tempo
  por passe e as esperas do CPU. Achado: a 1080p o jogo já está no teto de 60 FPS do próprio jogo; em 4K o limite é o
  trabalho de GPU (14 a 16 ms por quadro em muitos passes pequenos e dependentes). `tools/sotc_route_test.sh` repete uma
  rota gravada (`tools/routes/deserto_wide.txt`) para benchmark.

## 0.60.6-plus7 — 2026-10-25 (fork Lucas-blip-png)
- **Menu do jogo (F1): aviso sobre o upscaler.** O upscaler só muda na hora entre FSR 1 e bilinear (e a nitidez e a geração
  de quadros também). FSR 3.1 e DLSS Super Resolution definem o tamanho de renderização ao abrir o jogo, então só se
  escolhem no lançador; o menu agora diz isso, e que o "FSR 3" mais abaixo é a geração de quadros, não o upscaler (#246).
- **README:** GIF novo do Santuário sem limite de FPS (Ryzen 7 5700X + RTX 5060 Ti); a legenda avisa que o painel de FPS
  marca cerca de 3 FPS a menos durante a gravação de tela. O `--video` do benchmark passou a trazer a janela do jogo para
  a frente e a gravar pela duplicação de desktop da GPU, a 30 FPS (menos custo para o jogo).

## 0.60.6-plus6 — 2026-10-25 (fork Lucas-blip-png)
- **Cache de pipelines do driver em disco** (`user/cache/CUSA08809_driver.bin`; `SOTC_DRIVER_CACHE=0` desliga,
  `SOTC_DRIVER_CACHE_MAX_MB` limita o tamanho, 512 por padrão): o `VkPipelineCache` começava vazio a cada abertura, então
  o driver recompilava todo pipeline que o cache de shaders já tinha carregado. Agora o arquivo é lido na abertura
  (conferindo fabricante, dispositivo e UUID da GPU e do driver), gravado depois do pré-carregamento, a cada 3 minutos
  quando cresceu e ao fechar. Pré-carga de 2.753 pipelines na RTX 5060 Ti: de 0,7–0,9 s para 0,2–0,3 s (a NVIDIA já guarda
  um cache próprio, então o ganho é maior onde o driver não guarda). Não ajuda com shaders ainda nunca vistos. "Limpar cache
  de shaders" apaga o arquivo junto. Savewalk de 300 s sem falhas.
- **Lançador: tela cheia sem borda** ("Tela cheia sem borda (não trocar o modo do monitor)"): não muda o modo do monitor
  (`BB_FULLSCREEN_HZ=0`). Em monitores de 144 Hz a tela cheia normal troca para um modo exclusivo com um múltiplo de 60 Hz
  para o movimento não tremer; ligada, a troca de janela é instantânea, mas quadros a 30 ou 60 FPS podem alternar entre 2 e
  3 atualizações.
- **Diagnóstico:** `tools/sotc_bench_shrine.py --video arquivo.mp4` grava a janela do jogo durante a medição (ffmpeg, 60 FPS)
  e `tools/sotc_video_to_gif.sh` converte o vídeo no GIF do README.

## 0.60.6-plus5 — 2026-10-25 (fork Lucas-blip-png)
- **Santuário a 60 FPS (59 a 61 sem limite, antes 53 a 54):** cada uso de uma imagem de armazenamento 1920x1080 R32Uint
  (em layout GENERAL) depois de uma escrita gerava uma barreira, e cada barreira dentro de um passe de renderização
  obriga a fechá-lo e abri-lo de novo (cerca de 20 mil vezes por segundo). No GCN as escritas em UAV não são ordenadas
  entre desenhos: o jogo coloca um rótulo de fim de pipe (EOP/ReleaseMem) onde precisa delas visíveis. Agora a barreira
  GENERAL→GENERAL com o mesmo acesso sai uma vez por época de fence (no primeiro uso depois de um rótulo), não antes de
  cada desenho. `SOTC_GENERAL_SYNC=0` volta à barreira em todo uso. Medido em execuções intercaladas: 59,2 / 60,9 FPS
  contra 54,0 / 53,0. Roteiro de save de 400 s e de 600 s sem falhas e imagem sem defeitos visíveis.
- **Profundidade amostrada como alvo:** a imagem de profundidade com escrita de stencil, amostrada ao mesmo tempo, usa
  `DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL` também no descritor (validação do Vulkan; sozinha não muda o FPS).
  `SOTC_DEPTH_RO=0` desliga.
- **Leituras de volta parciais** (`SOTC_RB_PARTIAL`, ligada): uma leitura de volta assíncrona aplica só o trecho que ficou
  sujo em vez de refazer a janela (cerca de +1,3 FPS).
- **Lançador:** botão **Reportar problema** (menu lateral e página do registro): monta um relatório sem nome de usuário
  nem caminhos pessoais (versão, Windows, CPU, RAM, GPU e driver, configurações, final dos logs e dos crashes), que se
  copia, se salva em `user/reports/*.zip` ou abre uma issue no GitHub já iniciada. O lançador não envia nada sozinho.
  Ideia vinda do lançador do iss_pc.
- Medidas desta rodada que **não** viraram mudança: faixa assíncrona de compute em uma segunda fila (o compute é só 10 a 20%
  do tempo da GPU), parâmetros do motor (compute assíncrono, sondas, sombras), afinidade de threads e espera ativa nos
  mutexes (a contenção sumiu e o FPS não mudou).

## 0.60.6-plus4 — 2026-10-24 (fork Lucas-blip-png)
- **Leituras de volta "quentes" ligadas por padrão (`SOTC_RB_HOT`, desligue com `SOTC_RB_HOT=0`):** o que a GPU escreve
  e o jogo logo lê volta pela CPU sem esperar o fim do quadro. Santuário: cerca de +14% (44,0 / 47,4 para 53,3 / 50,3 FPS).
  A 0.60.6 as desligou por causa de ruído colorido e do travamento 0x49500. As correções do Oussama268 resolvem as
  duas causas: o jogo é reconferido logo antes de aplicar a leitura e cada página é gravada sob o lock da sua região.
  Validado aqui: 3 roteiros de save (30 FPS) mais 1 sem limite (49 FPS) com 0 falhas e sem ruído, e 26 minutos de
  demo de atração (média 71,0 FPS, mínimo 39,6, 0 falhas; o travamento antigo vinha aos 23 minutos).
- **Melhorias da GPU vindas do fork do Oussama268:** a fila de comandos sem nada pendente não toma mais o lock;
  arquivos de cache de shader que não existem não são lidos do disco; a posição fica invariante nos pipelines que emulam
  coordenadas baricêntricas (o cache de shaders é refeito na primeira abertura); a remoção de arenas do cache de buffers
  não espera mais a GPU.
- **Lançador:** aviso antes de iniciar quando o Windows não tem memória de commit para o jogo (#95), opção de pular os
  logos, opções de grama (cascatas de sombra, tamanho e desvanecimento) e modo **HDR** (saída HDR10 mais a renderização HDR do jogo).
- **Diagnóstico:** `SOTC_PROFILE_SLOW_MS` perfila só os quadros mais longos que o valor; o bench do Santuário ganhou
  `--turn`/`--turn-pad` e o da demo aceita `BENCH_MODES`.
- Medições desta rodada que **não** viraram mudança: reaproveitar descritores em compute (quase todo dispatch escreve),
  cortar o custo do preenchimento HLE (ganho de 2 a 3%) e reduzir os `Protect` (12 MB por quadro realmente reescritos
  pelo jogo). O Santuário fica em 50 a 53 FPS sem limite.

### Versão americana (CUSA08034, do fork do Oussama268; sem dump americano aqui, só os testes de código)
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

## 0.60.6-plus3 — 2026-10-24 (fork Lucas-blip-png)
- **Reuso de descritores ligado por padrão (`SOTC_FAST_BIND`, desligue com `SOTC_FAST_BIND=0`):** quando um desenho usa o
  mesmo pipeline, as mesmas constantes e os mesmos recursos do anterior, os descritores já enviados são reaproveitados.
  Santuário: de 43,5 para 47,1 FPS (+8%, três rodadas de cada); demo de atração 69,5 FPS. Ficou desligado desde a 0.60.6
  porque na 0.60.5 deixou pontos pretos em grade na areia. Revisão independente do código achou as corridas que
  deixavam isso passar e elas foram corrigidas: o contador de mudanças de memória agora sobe depois das marcações e é lido
  antes do bind, páginas "quentes" e marcações adiantadas também o fazem subir, cópias e HLE de shaders invalidam o
  reuso, e os passes que sobrescrevem os descritores (scroll de sombra, blit, troca de backing MSAA) forçam um novo envio.
  `SOTC_FAST_BIND_VERIFY=1` refaz o bind completo em cada acerto e compara: 0 descritores diferentes em 5,6 milhões de
  acertos simulados, 6 casos de dado novo (corrida com escrita do jogo para o quadro seguinte).
  **A areia do mundo aberto não foi testada** (o roteiro automático só chega ao Santuário): se aparecerem pontos pretos
  no chão, `SOTC_FAST_BIND=0` desliga e uma nota na issue ajuda.

## 0.60.6-plus2 — 2026-10-24 (fork Lucas-blip-png)
- **DLSS Super Resolution (modelo DLSS 4.5), só em placas NVIDIA RTX.** No lançador: Upscaler > DLSS
  Super Resolution; com as DLLs `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll` e `nvngx_dlss.dll`
  (Streamline 2.14.16, `nvngx_dlss.dll` 310.9.16) na pasta `bin`. Usa os presets M (DLAA, Qualidade,
  Equilibrado, Desempenho) e L (Ultra desempenho), a segunda geração do modelo transformer; `SOTC_DLSS_PRESET=k`
  volta ao modelo do DLSS 4 nos três primeiros. Sem as DLLs ou sem RTX, o jogo usa o FSR 3.1 (verificado). Testado
  na RTX 5060 Ti: imagem e cores corretas, sem quedas. O Streamline intercepta as chamadas do Vulkan e custa uns
  5% de FPS nesta CPU (FSR 3.1 49,6 → 46,7 só com ele carregado; o DLSS em si mais ~1 FPS), então o ganho do DLSS é
  de qualidade de imagem; onde a placa de vídeo é o limite (saída 1440p/4K em placas menores) o FPS sobe como no FSR.
- **Modos de qualidade do FSR 3.1 e do DLSS no lançador:** Nativa (DLAA), Qualidade (1,5x), Equilibrado (1,7x),
  Desempenho (2,0x) e Ultra desempenho (3,0x); o modo do DLSS segue o fator. A dica avisa que, com a CPU como
  limite, renderizar menor quase não muda o FPS (Santuário em 4K: 1080p 39 FPS, 720p 47 a 50).
- **Viés de LOD das texturas com o upscaler ligado** (`log2(render/saída) - 1`, o que o FSR e o DLSS pedem ao jogo):
  pedra, roupa e rosto ficam mais nítidos ao renderizar abaixo da saída (720p → 4K: -2,58). Pode reabrir o chiado em
  texturas finas; `SOTC_MIP_BIAS=0` desliga, ou outro valor o troca. Aceito o risco de pontos pretos na areia, a
  corrigir depois.
- **Menu do jogo (F1):** mostra "DLSS Super Resolution" quando é o upscaler e não troca a escolha do lançador por FSR 3.1.
- `SetSrtReadProtectedPages` com uma operação atômica por palavra de 64 páginas (equivalente, dentro do ruído).
- Diagnósticos: `SOTC_RB_STATS=1` marca se os bytes lidos são do GPU e `SOTC_RB_STATS_CODE=1` despeja o código do
  leitor; `SOTC_PROTECT_STATS=1`; `SOTC_STREAMLINE_FORCE=1` (carrega o Streamline com qualquer upscaler).
- Medido e descartado (24/10, sem mudança de código no jogo): trava do cache de texturas com a thread de desenho
  como dona (-5%), leitura sem readback para bytes que o GPU nunca escreveu (60% menos faltas, FPS igual) e
  `SOTC_RB_EARLY=0`. Detalhes no README.
- **Imagens sem alocação de memória de vídeo por quadro (+4% no Santuário, +6% na demo).** O driver
  da NVIDIA pede memória dedicada para imagens, então a cada imagem criada o port chamava
  `vkAllocateMemory` e, quando ela morria, `vkFreeMemory`: 4 de cada por quadro com a cena parada, uns
  5% do tempo da thread de desenho no Windows. Agora as imagens saem de um pool de blocos de 64 MiB que
  não permite alocação dedicada (as maiores que um bloco seguem o caminho antigo). Santuário 40,3 →
  42,0 FPS, demo de atração 71,2 → 75,5; o uso de memória de vídeo ficou igual. `SOTC_IMAGE_POOL=0`
  desliga. Diagnóstico novo: `SOTC_VK_ALLOC_TRACE=1` (com `SOTC_CMD_STATS=1`) mostra quem aloca.
- **Upscaler FSR 3.1 (temporal), desligado por padrão.** O jogo renderiza abaixo da saída (720p, 576p...)
  e o FSR 3.1 da AMD (qualquer placa com Vulkan 1.3) reconstrói a imagem com a profundidade, os vetores
  de movimento, o jitter e a câmera do próprio jogo, as mesmas entradas da geração de quadros. No
  lançador: Upscaler > FSR 3.1 e "Qualidade do FSR 3.1" (nativa, 1,5x, 1,7x, 2,0x, 3,0x); no menu F1 a
  opção aparece quando o jogo abriu com ele e liga/desliga na hora. `SOTC_UPSCALER=fsr3` liga sem o
  lançador (com `SOTC_RENDER_RES` e `SOTC_OUTPUT_RES`); `SOTC_FSR3UP_LOG=1` mostra o estado. No Santuário,
  720p para 4K: 38,7 FPS a 1080p, 46,9 a 720p com FSR 1 e ~48 FPS com FSR 3.1 (o PC de teste é limitado
  pela CPU; o ganho aparece onde a placa de vídeo é o limite). Limitações: a interface (HUD) também passa
  pelo upscaler; desliga o modo PS4 Pro. O cache de shaders não muda.

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
