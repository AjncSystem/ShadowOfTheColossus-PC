# Changelog

## Em desenvolvimento

**Experimental (desligado por padrão)**
- Liberar memória sem espera (`SOTC_DROP_FAST=1`, aba de ajustes experimentais do lançador):
  medido em 29/10 numa rota de carga (carregar o save, cavalgar, entrar na arena de um colosso),
  contra a mesma rota sem a opção. As esperas ao liberar memória caíram de ~9,3 s para 25–44 ms por
  sessão, o pior quadro de 417 ms para 125–185 ms e o p99 de 42 para ~34,5 ms. **Em 1 de 4 sessões
  o jogo fechou durante o carregamento do save** (o processador de comandos leu um comando
  zerado). A opção agora espera quando a memória liberada contém um command buffer que ainda está
  sendo lido, mas essa proteção não disparou nas sessões seguintes, então a causa não está
  confirmada. Por isso continua desligada por padrão. Se a queda voltar, o log diz se o comando
  veio de memória liberada sem espera (`sotc: bad PM4 packet …`).
- **SotC Lab** (`SOTC_LAB=1`, aba Ajustes experimentais do lançador): banco de testes de travadas
  dentro do jogo. F3 liga uma câmera livre (noclip) que leva o Wander junto, para a carga de áreas
  acompanhar a câmera como no jogo; F4 grava pontos de rota, que são reproduzidos em velocidade
  constante; F2 abre o menu, com as abas Câmera, Rotas, Colossos, SOTC_* (trocas ao vivo quando a
  chave permite, senão no reinício) e Testes. O pulo para os 16 colossos tem os modos Play, Kill e
  Outro; Kill e Outro copiam o save para `user/save-backups/` antes de mexer nele. Canal de controle
  em `\\.\pipe\sotc-ctl` (`SOTC_CTL=1`) e orquestrador `tools/sotc_matrix.py`, que roda matrizes de
  configurações e grava CSV e resumo. Recursos cujos ganchos não são encontrados ficam desligados, e
  o menu diz isso. O pulo para colosso usa a automação de comandos da engine descoberta por tabnul na
  PR AjncSystem/ShadowOfTheColossus-PC#345 (ainda não mesclada). Guia: `docs/sotc/LAB.md`.

## 0.70.1 — 2026-10-29
Correções e melhorias por cima da 0.70, que vem inteira (trocas de área, queda `0x49800`, DLSS no
modo Pro, HUD com geração de quadros).

**Correções**
- O logo da Bluepoint e o aviso de salvamento automático não apareciam: o limite de 32 MB de
  texturas novas por quadro da 0.70 podia adiar a mesma textura para sempre. Agora uma textura
  espera no máximo 2 quadros.
- Manchas coloridas nas malhas: as texturas pequenas agora são comparadas inteiras.
- Cache de shaders que não se atualizava: ~1.850 pipelines eram recusados e ~2.100 shaders
  traduzidos de novo a cada sessão. **O cache de shaders é refeito na primeira abertura.**
- Uploads pequenos que podiam chegar velhos à GPU.
- Uma textura ampliada podia ficar preta para sempre com o limite de 32 MB por quadro da 0.70.
- Sombras que piscavam ao carregar uma área: os passes de sombra passam na frente na fila de
  compilação, então a piscada é mais curta.
- Uma condição de corrida na marcação das páginas lidas pelos shaders.

**Melhorias**
- Menos travadas ao andar pelo mapa: o timer do Windows passa a 1 ms, e o ritmo de quadros usa um
  timer de alta resolução.
- Menos trabalho por desenho: o formato dos vértices só é reenviado quando muda, e chamadas
  vizinhas de proteção de memória são juntadas.
- O `bb-probe.exe` passa a levar a libc++ embutida (a thread de desenho não chama mais o
  `libc++.dll`), e o caminho quente da thread de desenho ficou mais leve. O `libc++.dll`
  continua no pacote, porque os codecs do FFmpeg precisam dele.
- Lançador: nova aba **Ajustes experimentais**, abaixo de Avançado. Ela traz as opções criadas
  contra as travadas, cada uma com uma descrição curta e já no valor padrão.

**Experimental (desligado por padrão)**
- Dezenas de chaves para a thread de desenho (novo caminho de bind, cache de binds, memória dos
  alvos de render), para os readbacks, para a proteção de memória e para os uploads, além de
  diagnósticos (`SOTC_TRACE`, `SOTC_OP_PHASES`, `SOTC_FPS_LEAN`). Cada uma tem os seus resultados
  medidos nos detalhes abaixo; a maioria não deu ganho sozinha.

<details>
<summary>Detalhes técnicos: medições e chaves experimentais</summary>

- **Tudo da 0.70 incluído** (trocas de área, correção da queda `0x49800`, DLSS no modo Pro, HUD
  com geração de quadros, LOD feedback, `SOTC_DRAW_SPLIT`, `SOTC_PC_MEMORY`), junto com o
  trabalho desta versão. Os padrões da 0.70 valem: as correções de troca de área vêm ligadas
  (`=0` desliga) e `SOTC_VIEW_CHUNK_MB` passa a ser 2 MiB sem a chave (`=0` volta a uma visão
  por mapeamento; `SOTC_VIEW_CHUNK_KB` continua tendo prioridade). Correções feitas na junção:
  - uma textura ampliada (`ExpandImage`) que o limite de 32 MB por quadro adiasse ficava preta
    fora da parte copiada, para sempre; agora ela é enviada inteira, como antes do limite;
  - uma imagem reaproveitada do estacionamento voltava sem zerar o estado do limite;
  - com `SOTC_PC_MEMORY=1` e `SOTC_HOST_IMPORT=1` juntos, a extensão de memória do host era
    pedida duas vezes ao driver;
  - o envio de bytes do readback pelo caminho rápido da 0.70 (`SOTC_WRITE_FAST`) agora espera os
    uploads assíncronos que ainda leem esses bytes (`SOTC_ASYNC_UPLOAD`), como o caminho normal.
  O cache de shaders do LOD feedback é refeito na primeira abertura.
- **Novo caminho de bind da thread de desenho (experimentais, todos desligados; ainda não
  medidos no jogo).** Projeto em `docs/BIND-PATH-DESIGN.md`. Cada opção tem um modo de
  verificação que refaz o caminho antigo, compara e usa o resultado antigo:
  - `SOTC_OP_PHASES=1` (diagnóstico): a cada 5 s, o tempo de cada operação da thread de desenho
    por fase (pipeline, render state, bind, texturas, vértices, descritores, estado dinâmico,
    HLE, reset), os buffers limpos/sujos/escritos, uploads, chamadas de proteção de página e
    readbacks percorridos. `=2` deixa de fora a medição por buffer (pesa menos: com `=1` o
    próprio contador custa ~2 ms por quadro).
  - `SOTC_WRITE_DIET=1` (`=2` verifica): cada readback em andamento guarda a faixa que cobre, e
    um buffer escrito pula os readbacks fora dela; sem readbacks em andamento o percurso não roda;
    a pré-busca deixa de ser marcada duas vezes pela mesma escrita.
  - `SOTC_SYNC_PIPE=1` (`=2` verifica; precisa de `SOTC_SYNC_FAST`): uma faixa já limpa é
    varrida uma vez só na sincronização (antes eram duas); os bits de página do SRT mudam uma
    palavra de 64 bits por vez. Conta as chamadas de proteção por operação ("SOTC SYNC PIPE"),
    para decidir se vale juntar as proteções de uma operação inteira.
  - `SOTC_TEX_BLOCK=1` (`=2` verifica): o bloco de texturas de um estágio (T#, S# e as imagens
    achadas) é reaproveitado enquanto as imagens, as visões e o estado delas não mudaram; a
    checagem de sujeira e as transições continuam rodando em cada draw. Baseado no PR #69.
    Também vale quando o `SOTC_BIND_CACHE` está gravando uma entrada nova: o acerto deixa na
    entrada as mesmas marcas de LOD, imagens e transições que o bind completo deixaria (antes ele
    recusava esses binds, ~50% do total, e só ~90 por quadro chegavam a ele). O `=2` também
    compara essas marcas.
  - `SOTC_RS_MEMO=2` (`=3` verifica): a memória dos alvos de render (`SOTC_RS_MEMO=1`, que não
    muda) também vale entre um envio à GPU e o seguinte, com as transições rodando quando não for
    provado que não fazem nada. Baseado no G-buffer do PR #308, sem os seriais de registrador.
  - `SOTC_HLE_SKIP=1` (`=2` verifica): as cópias do shader de cópia emulado marcam o destino uma
    vez por faixa juntada, em vez de uma vez por cópia. Pular cópias inteiras não foi feito: falta
    um sinal exato de "não mudou" para as escritas da GPU.
  - Correção pequena: `MarkSrtCleanPages` agora liga os bits de página com uma operação atômica
    (uma proteção em outra thread podia perder o bit desligado).
- **Medido (29 out): os dispatches não têm bind para reaproveitar.** `SOTC_CS_STATS=1`
  (diagnóstico, desligado) imprime a cada 5 s quantos dispatches repetem o anterior e quanto o bind
  deles custa. São ~1.100–1.200 dispatches por quadro, todos escrevendo em memória, com bind de
  125–190 ms por segundo (~4,5 ms por quadro). Nenhum repete o estado completo do anterior; só
  0,5–2% repetem os mesmos recursos, e nenhum deles sem mudança de memória no meio. ~28% usam os
  mesmos buffers com outro deslocamento, mas esses já são baratos (~1,4 µs contra ~5,8 µs dos que
  trocam de recurso; ~9% do tempo). Um atalho de bind para compute não compensa: o custo está em
  achar e preparar cada recurso novo.
- **Medido (29 out): o jogo não manda draws vazios.** `SOTC_CMD_STATS=1` agora também conta os
  draws diretos com 0 índices ou 0 instâncias. Numa sessão, os ~320–1.080 draws diretos e os
  ~2.100–4.600 indiretos com argumentos na CPU por quadro tiveram 0 vazios (um único indireto vazio
  na sessão toda). O culling do jogo funciona; pular draws vazios não ganha nada.
- **Medido (29 out): nova base enxuta e a variação entre sessões.** Sem nenhuma mudança, a thread
  de desenho ficou em 18,1 ms por quadro (mediana), com 5 quadros acima de 60 ms, contra 21–22 ms
  nas sessões de teste anteriores. Dentro de uma mesma sessão ela varia de 14 a 25 ms a cada 5 s.
  Diferenças de 1–3 ms entre sessões em trajetos diferentes são ruído: os testes A/B precisam
  repetir o mesmo save e o mesmo trajeto.
- **Medidos (29 out), todos desligados:**
  - `SOTC_BIND_DIFF=1` (diagnóstico): diz por que cada draw elegível perdeu o atalho do bind. Só
    ~1–2% perdem apenas pela época de memória; a maioria muda o pipeline e o hash (30–50%), o
    atalho já estava inválido (25–50%) ou só o hash muda (20–25%).
  - `SOTC_BUFFER_MEMO=1`: a memória de V# do `SOTC_BIND_CACHE_FRAME` sozinha. Acerta 96–99% dos
    buffers, mas a thread de desenho não melhorou: os binds que ela pula já eram baratos.
  - `SOTC_CEILING=sync` / `=tex` (só medição, quebra o jogo): pulam a sincronização de memória
    dos buffers ou a atualização das texturas, para medir o teto do ganho. `sync` derruba o jogo
    (o alocador de command buffers do jogo devolve nulo); `tex` piorou (as imagens ficam sujas).
  - `SOTC_FAULT_GRANULE` de 4 KB (sem granularidade): 22,2 ms por quadro e 12% mais faltas; sem
    ganho.
- **Medido (29 out): `SOTC_UNTRACK_AUTO` não reduz as faltas.** Depois de duas correções (a leitura
  da lista de blocos aceitava só endereços abaixo de 2 GiB, e o ponteiro para o próximo nó fica em
  +8, não em +0), o port achou o heap por quadro RenderSingleFrameGPU: um bloco de 32 MiB, liberado
  até o teto de 16 MiB, sem nenhuma falta nas faixas liberadas. Mas as faltas de escrita ficaram
  iguais (~13.500 a cada 5 s, contra ~13.200 na base), as páginas liberadas passaram a ser reenviadas
  200–450 mil vezes por segundo e a thread de desenho subiu de 19,2 para 22,1 ms por quadro. As
  faltas de escrita não vêm desse heap. Fica desligado.
- **ETW (29 out, WPR + `tools/etw_offcpu.py`):** a thread de desenho fica na CPU ~60% do tempo; ~8%
  dele ela espera dentro do kernel sem syscall (provável disputa do lock de memória do processo) e o
  resto é preempção ou espera por trabalho; a CPU tem pouca folga (o port ocupa ~7 threads pesadas, e
  havia programas em segundo plano). `SOTC_THREAD_IDS=1` agora também lista as threads do port, para
  cruzar com o ETW.
- **Medido (29 out): a segunda rodada não melhorou.** Os cortes do anel de operações
  (`SOTC_BACK_HEAD_CACHE`, `SOTC_BACK_PF`, `SOTC_REG_SAME`) sozinhos: thread de desenho 21,5 ms
  por quadro contra 19,2 ms da base enxuta (sessão mais pesada; nenhum sinal de ganho). O
  `SOTC_OPSTAT` mostrou o porquê: o custo fixo de cada operação no anel é ~0,12–0,2 µs de ~5 µs; o
  resto é o próprio draw. `SOTC_PR_RESOLVE` divergiu do cálculo normal 12 vezes numa sessão
  (`SOTC_PR_VERIFY` desligou o atalho nesses draws): não usar até ser investigado.
- **`SOTC_MDI_STATS=1` (diagnóstico, desligado):** conta quantos draws indiretos seguidos poderiam
  virar um multi-draw (mesmo estado, argumentos contíguos, nada entre eles). Medido: ~3.000 draws
  indiretos por quadro, só ~5% com argumentos escritos pela GPU; ~85–90% formam sequências de 1;
  só 3–12% dariam para juntar. Juntar draws não compensa neste jogo.
- **Medido (29 out): a configuração enxuta é o que melhorou.** Sem `SOTC_PROFILE`, `SOTC_TRACE` e
  `SOTC_HRR=dry`, e com `SOTC_FPS_LEAN=1`, a thread de desenho caiu de 21–24 para ~19 ms de CPU por
  quadro e os quadros acima de 60 ms de 12–17 para 6–8 numa sessão de ~70 s. Os cinco switches da
  rodada anterior (`SOTC_SYNC_PRECHECK`, `SOTC_GPUMOD_CONTAINS`, `SOTC_HASH_EPOCH`,
  `SOTC_BIND_MARK_ONCE`, `SOTC_INVAL_INDEX`) não mudaram nada mensurável (19,0 contra 19,2 ms).
- **Segunda rodada para a thread de desenho (experimentais, todos desligados; ainda não medidos):**
  - `SOTC_BACK_HEAD_CACHE=1`: a thread de desenho só relê o início do anel de operações quando
    alcança o valor que já tinha; `SOTC_BACK_PF=1` (com o anterior) pré-carrega os registradores da
    próxima operação. O início e o fim do anel agora ficam em linhas de cache separadas (só layout).
  - `SOTC_REG_SAME=1`: uma escrita de registrador que não muda o valor não marca o bloco como
    alterado (o bloco não é copiado de novo para a operação seguinte).
  - `SOTC_PR_RESOLVE=1` (com `SOTC_PARALLEL_RECORD`): as threads de preparação já calculam os
    descritores de buffer e textura de cada draw (leitura dos V#/T#, limites de faixa); a thread
    de desenho só confere e usa. `SOTC_PR_VERIFY=1` refaz a conta e desliga o atalho se divergir.
  - `SOTC_UNTRACK_AUTO=1`: as faixas do heap por quadro RenderSingleFrameGPU do jogo deixam de ser
    protegidas e passam a ser enviadas a cada bind de leitura, achadas sozinhas a cada 30 quadros
    (até `SOTC_UNTRACK_MAX_MB`, padrão 16); desliga sozinho se aparecer uma falta numa delas.
  - `SOTC_ASYNC_JOB_MB=<n>`: uploads assíncronos maiores que n MiB viram vários jobs menores.
    `SOTC_ASYNC_STAGE_CACHED=1`: staging dos uploads em memória cacheada (ignorado com
    `SOTC_REBAR_UPLOAD`). `SOTC_IMAGE_PREFETCH_MAX=<n>`: mais imagens criadas antecipadamente.
  - Diagnóstico: `SOTC_STALL_DETAIL=1` (com `SOTC_FPS`) acrescenta às linhas `SOTCSTALL` o tempo de
    ritmo de flip, a CPU somada das threads do jogo e a divisão usuário/kernel das threads de
    upload; `SOTC_OPSTAT=1` imprime a cada 5 s as operações da thread de desenho por tipo.
  - Analisados e não implementados: aplicar os readbacks na própria thread do jogo (ganho estimado
    de 1,5–3%, exige mais uma ida e volta à thread de desenho) e dividir a thread de desenho em
    várias (~1,1–1,3x com 8 núcleos: a maior parte do trabalho mexe nos caches e precisa ser em
    série).
- **O que o gravador de linha do tempo mostrou sobre as travadas (29 out):** a thread de desenho do
  port não tem folga. Nos quadros normais perto de uma travada ela já fica ocupada o quadro inteiro
  (~5.700 operações, 6–10 µs cada) com a GPU ~50% ocupada; nos lentos o jogo pede ~14% mais
  operações e cada uma custa ~21% mais, e o quadro dobra. Os readbacks do jogo (culling XPR,
  partículas) só esperam por ela. Não há um culpado único: binds de buffer, operações curtas,
  aplicação de readbacks, uploads e binds de texturas dividem o tempo. Atenção para medições
  antigas: o `SOTC_PROFILE` suspende a thread de desenho ~1 vez por ms, e os timers do `SOTC_FPS` e
  do `SOTC_TRACE` somam ~10% dela.
- **Switches novos para dar folga à thread de desenho (experimentais, todos desligados; ainda não
  medidos):**
  - `SOTC_FPS_LEAN=1`: o `SOTC_FPS` mantém o resumo e as contagens, sem as leituras de relógio por
    operação nem as marcas de escrita por shader (as colunas de tempo detalhado saem zeradas).
  - `SOTC_SYNC_PRECHECK=1`: a sincronização de um buffer confere antes, sem locks, se ele está
    limpo, e só então faz a varredura completa.
  - `SOTC_GPUMOD_CONTAINS=1`: um bind escrito numa faixa já marcada como escrita pela GPU sai com
    um lock compartilhado, sem o exclusivo.
  - `SOTC_HASH_EPOCH=1`: com `SOTC_HASH_SMALL`, pula o mutex dos hashes quando a faixa está fora
    de todos os uploads com hash.
  - `SOTC_BIND_MARK_ONCE=1`: tira uma marcação de escrita repetida por bind escrito.
  - `SOTC_INVAL_INDEX=1`: um bind escrito só procura imagens a invalidar quando há alguma imagem
    nas páginas dele (contagem por página que o cache de texturas já mantém).
  - `SOTC_UNTRACK_RANGES=<endereço:tamanho,...>`: faixas que nunca são protegidas e são enviadas
    de novo a cada bind de leitura (desligado com `SOTC_CP_AHEAD`); `SOTC_UNTRACK_LOG=1` imprime
    os blocos do heap por quadro RenderSingleFrameGPU do jogo, para escolher as faixas.
  - Sem switch, sem mudança de resultado: o HRR em modo dry sai sem lock quando não há nada
    rastreado, e leituras de relógio que só alimentavam relatórios deixaram de rodar com o
    `SOTC_FPS` desligado.
- **`SOTC_TRACE=1`: gravador de linha do tempo para os quadros lentos (diagnóstico, desligado).**
  Cada thread que participa de um quadro grava o que fez num buffer circular próprio, sem lock: as
  threads do jogo (esperas em event flag, semáforo, cond, mutex disputado, equeue, sleep; leitura de
  arquivo; espera de upload antes de o runtime escrever na memória do jogo; faltas de leitura e
  escrita, com o endereço e a instrução do jogo; readbacks; submits e flips), o processador
  de comandos (parse por fila, bloqueio na thread de desenho, sono), a thread de desenho (cada
  operação acima de 50 µs e toda escrita de label, as outras curtas agrupadas; esperas por GPU,
  recorder, upload e lock de
  texturas; ociosidade), a thread de gravação (blocos, espera de upload, vkQueueSubmit), a GPU (quando
  cada submit terminou de fato, por fila) e o presenter. Quando um quadro passa de `SOTC_TRACE_MS`
  (padrão 60), salva a janela em volta dele (`SOTC_TRACE_PRE_MS` antes, padrão 150; `SOTC_TRACE_POST_MS`
  depois, padrão 40) em `user/trace/<sessão>/`, no formato do Chrome, que o ui.perfetto.dev abre no
  navegador; no máximo `SOTC_TRACE_MAX` arquivos por sessão (padrão 40). Cada thread é uma faixa, e o
  endereço de retorno do jogo aparece como offset do eboot. Serve para ver qual cadeia de esperas
  segurou o flip em cada travada, em vez das somas do `SOTCSTALL`. Sem a variável, nada muda.
- **`SOTC_HOOK_PSTATS=1` (experimental, desligado; resultado medido: não reduz as faltas):** tira uma escrita do jogo em
  `ParticleSystemPool::ProcessStats` (eboot `0x101ae4`, troca 3 bytes por um `nop`). A cada quadro,
  ela gravava a hora atual num array de "último quadro vivo" de cada sistema de partículas (heap
  padrão, `pool+0x188`), que o jogo não lê em lugar nenhum encontrado. As páginas desse array caem
  nos mesmos blocos de 64 KiB que dados da GPU, então essa escrita sozinha era 53% de todas as
  faltas de escrita do jogo (~1.400/s numa sessão de 5.000 quadros). A gravação na criação do sistema
  e o tempo de vida das partículas (contado a partir da criação) não mudam. Os bytes originais são
  conferidos antes; em outra versão do jogo nada é alterado. Medido (~70 s, 29 out): a instrução
  some das faltas, mas o total fica igual (~2.870 faltas de escrita/s antes e depois) e os quadros
  lentos também (13; base 12–16). Cada bloco de 64 KiB é protegido de novo a cada quadro e a
  primeira escrita da CPU nele, de qualquer instrução, é que paga a falta: tirada essa, a próxima
  escrita do jogo no mesmo bloco faz o mesmo papel. Os "53%" vinham da lista das 12 maiores por
  região, que pesava demais essa instrução.
- **`SOTC_RB_EXACT=publish` (experimental, desligado; resultado medido: pior):** versão enxuta do
  `SOTC_RB_EXACT`, sem o mapa de quem escreveu cada byte. Adia só o label do XPR (EOP, evento
  `0x28` BOTTOM_OF_PIPE_TS, 32 bits, valor 1) que vem depois de escritas da GPU, copia em bloco o que
  a GPU escreveu desde o label anterior e só escreve o label depois de entregar esses bytes na
  memória do jogo; os outros labels saem cedo. Páginas que o jogo também escreveu ficam protegidas
  com os bytes da GPU pendentes (a cópia informa exatamente quais pulou). Linha `SOTC RB publish`
  com a latência em quatro etapas. Medido (~70 s): só com janelas aprendidas, ~3 labels/quadro a
  ~2,4 ms cada e 34 quadros lentos (base 12–16), a cópia não pegava os bitsets; contando toda
  escrita pequena, ~20 labels/quadro, ~100 MB copiados a cada 5 s e 19–24 FPS. Neste jogo esperar a
  GPU em cada label custa mais do que deixar as faltas acontecerem.
- **Resultado medido dos switches novos (sessões de ~70 s nas mesmas áreas, 29–30 out):** a
  configuração estável (`SOTC_RB_TICK`, `SOTC_RB_GROW`, `SOTC_RB_MATERIALIZE` sobre a base) teve 12
  quadros acima de 60 ms. Cada switch novo, ligado sozinho por cima dela, piorou ou não agiu:
  `SOTC_RB_EXACT` 25 (a espera total de readback cai ~15%, mas a thread de desenho sobe de 22 para
  35 ms/quadro); `SOTC_HOT_PAGES` + granule 4 KiB + write-ahead adaptativo 22 (faltas 30% menos, mas
  o dobro do custo cada); `SOTC_BIND_CACHE_FRAME` dobrou a thread de desenho (o cache é esvaziado
  ~15 mil vezes por segundo); `SOTC_RB_GROUP` não agiu (cada bind do jogo escreve um bitset só);
  `SOTC_HLE_SHADOW_CULL` não agiu (as entradas do shader são escritas pela GPU);
  `SOTC_WRITE_FAULT_NARROW` dobrou os readbacks. Todos ficam desligados por padrão; servem de base
  para medições futuras. `SOTC_CULL_EXPRESS` (fila prioritária para o culling) está incompleto.
- **Correção: `SOTC_BIND_CACHE_XTICK` e `SOTC_BIND_CACHE_FRAME` nunca ligavam:** liam outro switch
  durante a inicialização, numa ordem não garantida; agora leem as variáveis diretamente. Quem tinha
  `SOTC_BIND_CACHE_XTICK=1` no launcher passa a tê-lo ligado de verdade (nunca foi medido sozinho).
- **Correção: uploads pequenos podiam ficar velhos na GPU (`SOTC_HASH_SMALL`).** O port copiava os
  bytes para o upload e só depois calculava o hash relendo a memória do jogo; uma escrita do jogo no
  meio guardava o hash dos bytes novos enquanto a GPU recebia os velhos, e o próximo upload era
  pulado como "igual". Agora o hash e o upload usam a mesma cópia. Também: um upload com uma página
  que o jogo está escrevendo não vai mais inteiro para a thread de desenho — só essa página.
- **`SOTC_RB_EXACT`, segunda versão:** a queda para ~20 FPS vinha de avisos adiados em cascata
  (6.170 a cada 5 s, quando só ~150 tinham dado quente). Agora só é adiado o aviso que vem depois de
  escritas quentes, e só no endereço em que adiar custou menos que deixar o aviso sair cedo — o port
  mede as duas coisas por endereço. Com `SOTC_FPS=1` a linha `SOTC RB exact` ganha a parte
  `adaptive`.
- **Faltas de escrita sem readback inteiro (`SOTC_WRITE_FAULT_NARROW=1`, desligado por padrão):**
  uma escrita do jogo numa página escrita pela GPU lia de volta uma janela de 512 KiB; agora só as
  páginas da falta, pela fila de transferência quando a GPU já terminou (com `SOTC_RB_TICK=1`).
- **Bind cache que dura o quadro inteiro (`SOTC_BIND_CACHE_FRAME=1`, desligado por padrão; precisa
  de `SOTC_BIND_CACHE_XTICK=1`):** o cache era zerado ~30 vezes por quadro (troca de render target,
  envio, qualquer imagem nova). Agora os render targets fazem parte da chave, a tabela tem 4096
  entradas, e cada entrada só cai quando algo nas faixas que ela usa mudou. Linha `SOTC BIND FRAME`.
- **Menos chamadas de proteção de página (`SOTC_PROTECT_COALESCE=1`, desligado por padrão):** numa
  falta de escrita, as liberações de páginas vizinhas viram uma chamada só; nova linha
  `SOTC FAULT SPLIT` mostra onde vai o tempo de cada falta. `SOTC_HOT_PAGES`: tempo de vida sem
  "precipício", contagem certa e limite que cresce sozinho (`SOTC_HOT_PAGES_MAX_CEIL`).
- **Leituras da GPU exatas e sem espera desnecessária (`SOTC_RB_EXACT=1`, experimental, desligado
  por padrão; precisa de `SOTC_RB_OWNER=1` e do processador de comandos em threads):** a análise do
  código do jogo mostrou que 92% das leituras de volta vêm de duas funções. A de 63% (`eboot+0x1a6d60`)
  lê o bitset de visibilidade do XPR deste quadro depois de esperar o label `CS_DONE`; o port
  escrevia esse label assim que o lia, antes da GPU rodar o trabalho, e o jogo travava na leitura.
  A de 29% (`eboot+0x101a98`) lê um anel de 3 buffers, o de 2 quadros atrás, já pronto; o port
  esperava mesmo assim, porque a página é a mesma do buffer que a GPU escreve agora. Com o switch:
  os labels que vêm depois de escritas nessas faixas são escritos quando a GPU termina de verdade,
  em ordem, com o trabalho enviado na hora e os dados já na memória do jogo; e uma leitura espera só
  por quem escreveu os bytes pedidos — vizinhos que a GPU ainda escreve, e que nenhum label anunciou,
  são entregues quando ela termina. É o mesmo resultado do PS4, sem dado de quadro anterior. Desliga
  `SOTC_RB_PREV`. Com `SOTC_FPS=1` a linha `SOTC RB exact` mostra leituras servidas sem espera, as
  que caíram no caminho antigo e por quê, labels adiados e a latência deles.
- **Máscaras do XPR com um quadro de atraso (`SOTC_RB_PREV=1`, experimental, desligado por
  padrão; precisa de `SOTC_RB_OWNER=1`):** depois de `SOTC_RB_MATERIALIZE`, o que sobra das
  travadas são leituras da máscara do quadro atual enquanto a GPU ainda a produz; nenhum truque de
  memória tira essa espera, porque o dado ainda não existe. Com o switch, o jogo lê a versão que já
  está na memória dele (a última entregue, de no máximo `SOTC_RB_PREV_FRAMES` quadros, padrão 2, e
  150 ms atrás) em vez de esperar; a próxima escrita da GPU traz o quadro seguinte. Nada é gravado
  na memória do jogo. Só vale para leituras (nunca escritas), de janelas lidas e reescritas pela
  GPU em todos os últimos 3 quadros, e só para bytes entregues recentemente; memória desmapeada ou
  liberada perde esse histórico. Risco aceito: um objeto que acabou de ficar visível pode sumir por
  1-2 quadros ao girar a câmera. Com `SOTC_FPS=1` a linha `SOTC RB previous` mostra quantas leituras
  foram servidas sem espera e por que as outras foram recusadas.
- **Máscaras do XPR entregues antes da leitura (`SOTC_RB_MATERIALIZE=1`, desligado por padrão;
  liga junto as cópias antecipadas de `SOTC_RB_PREFETCH` e precisa de `SOTC_RB_OWNER=1`):** as
  máscaras de oclusão do XPR são ~80% das leituras de volta da GPU (com `r_EnableXPR=0` caem de
  ~900 para ~200 por segundo, mas os draws dobram e o jogo vai a 8-17 FPS, então elas ficam), e em
  78-89% dessas leituras a GPU já tinha terminado de escrever quando o jogo leu. Agora a cópia
  antecipada de uma janela lida todo quadro é aplicada na memória do jogo assim que termina, sem
  esperar a falta de página: o jogo lê sem falta, sem ida à thread de desenho e sem espera. O dado
  é o mesmo que uma leitura naquele momento copiaria (mesmas regras de descarte das cópias
  antecipadas). Duas proteções contra o problema do antigo `SOTC_RB_HOT` (ruído arco-íris,
  `0x49500`): só entram bytes que a GPU escreveu neste quadro ou no anterior, e há no máximo 100 ms (memória esquecida
  e reaproveitada pelo jogo fica para a falta normal), e páginas que o jogo escreveu nesse meio
  tempo ficam com os bytes do jogo. `SOTC_RB_MATERIALIZE_US` (padrão 100) é o intervalo mínimo
  entre verificações. Com `SOTC_FPS=1` a linha `SOTC RB materialize` mostra quantas foram
  aplicadas sem falta (e MiB) e quantas cópias não se qualificaram (`not fresh`).
- **Faixas residentes no host, passo 1: seleção e simulação (`SOTC_HRR=dry`, desligado por padrão):**
  aprende, pelo caminho das leituras do jogo, quais faixas de 64 KiB são lidas de volta pelo menos
  `SOTC_HRR_MIN_RB` (padrão 30) vezes por segundo e as junta em faixas contíguas, até
  `SOTC_HRR_MB` (padrão 16) MiB. Anota o que impediria a promoção: imagem do cache de texturas
  sobre a faixa, bind que ultrapassa a borda, nenhum bind visto, faixa sem visão de backing. Com
  `SOTC_FPS=1` a linha `SOTC HRR` mostra faixas, KiB, faltas nelas (sem espera / com espera, pelo
  tick do escritor) e quanto essas leituras custam hoje; as linhas `SOTCSTALL` ganham `hrr N`.
  Nada é promovido: `SOTC_HRR=1` ainda se comporta como `dry` (a importação com
  `VK_EXT_external_memory_host` e o redirecionamento da arena não estão feitos). Sem a variável
  nada muda.
- **Cache de binds que atravessa o tick de envio (`SOTC_BIND_CACHE_XTICK=1`, desligado por padrão;
  exige `SOTC_BIND_CACHE=1` e `SOTC_BIND_CACHE_RANGES=1`):** o cache de binds era esvaziado a cada
  envio (~20–30 por quadro) e a cada faixa nova tornada residente. Com a variável, (1) o tick deixa
  de esvaziar o cache: o primeiro acerto de cada entrada num tick novo repete o que um bind
  completo faz uma vez por tick e um acerto pulava (carimbos de acesso e LRU das imagens, toque no
  LRU dos samplers, conferido por devolver o mesmo handle e slot, carimbos de uso dos blocos da
  arena usados por `MaybeEvictCold`, uma vez por segundo, e a conferência de que o mapeamento do
  guest sob cada V# continua o mesmo); descritores, dados de push, transições de imagem e
  barreiras de buffer já eram refeitos a cada acerto; (2) o cache de buffers ganha `BindVersion`,
  que só sobe quando uma faixa residente é desfeita (liberar memória, evicção, troca de arena) e
  não quando mais memória vira residente; o bind rápido e o cache usam esse número, então os
  quadros de streaming deixam de esvaziá-los; (3) faixa de buffer modificada pela CPU não recusa
  mais o acerto: é sincronizada como o bind completo faz e os descritores guardados continuam
  valendo (imagens sujas continuam recusando). Com `SOTC_FPS=1` a linha `SOTC BIND CACHE` ganha
  `xtick hits` (acertos de entradas feitas em tick anterior), `repaired` (acertos que sincronizaram
  uma faixa), `replays` (primeiros acertos num tick novo) e as chaves já vistas no quadro anterior.
  Sem a variável nada muda.
- **Leituras do jogo na fila de transferência, sem esperar o trabalho novo (`SOTC_RB_TICK=1`,
  desligado por padrão):** o cache de buffers guarda os conjuntos de escrita dos últimos 8 ticks
  (quem escreveu cada faixa e quando). Com `SOTC_FPS=1` a linha `SOTC RB writer` mostra, a cada 5 s,
  quantas leituras do jogo tinham o escritor já terminado na GPU (`done`), enviado mas não terminado
  (`submitted`) ou ainda em gravação (`unsubmitted`), e a idade média em ticks. Com
  `SOTC_RB_TICK=1`, quando o escritor já foi enviado, a cópia vai para a fila só de transferência
  (criada sem desviar uploads; os buffers ficam concorrentes entre as duas famílias de filas),
  esperando apenas o tick do escritor; a thread do jogo espera o valor da fila de transferência,
  consultando o contador por `SOTC_RB_SPIN_US` (padrão 150) antes de bloquear. O próximo envio
  gráfico só espera essa cópia se uma escrita gravada depois cobrir os mesmos bytes (escrita depois
  de leitura); desfazer mapeamentos e `Finish` também esperam por ela. Escritor desconhecido ou em
  gravação, binds esparsos pendentes, sem fila de transferência ou sem slot livre seguem o caminho
  de antes. A linha `SOTC RB tick` mostra cópias, espera média e recuos (sem fila, desconhecido,
  binds). Sem a variável nada muda.
- **Latência das faltas de escrita e janela de escrita adaptativa (`SOTC_WRITE_AHEAD_ADAPT=1`,
  desligado por padrão):** com `SOTC_FPS=1` a linha `SOTC FAULTS` mostra as faltas de escrita das
  threads do jogo por janela: quantas, quantas em rajada sequencial, latência média (do início ao
  fim do tratador) separada em espera do mutex da região, espera da cerca do upload assíncrono e
  tempo em `VirtualProtect`, p99, páginas marcadas à frente (e quantas voltaram idênticas no
  upload) e as páginas do grânulo que não precisaram subir; as linhas `SOTCSTALL` ganham
  `faults N (X ms)`. Com `SOTC_WRITE_AHEAD_ADAPT=1`, uma thread que escreve em sequência (falta
  logo após a extensão liberada pela anterior, em até `SOTC_WRITE_AHEAD_ADAPT_US`, padrão 1000)
  dobra a janela de escrita à frente (ou atrás, se escreve para baixo) a cada falta, de 16 páginas
  até `SOTC_WRITE_AHEAD_MAX_PAGES` (padrão 512 = 2 MiB), e volta a 16 numa falta não sequencial. As
  páginas marcadas ganham antes um instantâneo (hash) do conteúdo, e no upload as que o jogo não
  escreveu são descartadas, como no grânulo; páginas da GPU, com imagens por perto, de pilha ou já
  sujas não são marcadas. Sem a variável nada muda. Soltar o mutex da região durante o
  `VirtualProtect` foi avaliado e não implementado: sem o mutex a ordem entre o rastreador de
  páginas e os bits poderia estourar a contagem de observadores.
- **Processador de comandos sem ping-pong com a thread de desenho (`SOTC_WD_FUSE=1`, desligado
  por padrão):** os pacotes WRITE_DATA pequenos (até 64 bytes, 4 pacotes) que escrevem os
  argumentos de um DRAW_INDIRECT/DISPATCH_INDIRECT seguinte viajam dentro do próprio draw em vez de
  virar uma função na fila; assim o `WaitBackWrites` não espera mais por eles e o processador de
  comandos pode correr à frente da thread de desenho. Os closures pequenos da fila também ficam
  num buffer interno, sem alocação (`SOTC_INLINE_FUNC=1` liga só isso). Com `SOTC_FPS` as linhas
  `SOTC CP ops` mostram funções por quadro, WRITE_DATA fundidos, pares WRITE_DATA→draw indireto e
  as esperas do `WaitBackWrites`.
- **Escritas da GPU por shader (diagnóstico) e extensão de escrita (`SOTC_WRITE_EXTENT=1`, desligado
  por padrão):** com `SOTC_FPS=1` a linha `SOTC GPU writes by shader` lista, a cada 5 s, os 8
  pares (hash do programa, índice do binding) com mais leituras da CPU que exigiram ida à GPU, com
  os MB marcados como escritos, o tamanho do binding, as threads do dispatch e os offsets lidos.
  Com `SOTC_WRITE_EXTENT=1`, bindings de armazenamento escritos por dispatches diretos listados em
  `user/write_extent.txt` (`hash binding [offset [fixo [por_thread [por_grupo]]]]`, em bytes) são
  marcados como modificados pela GPU só na janela `offset + fixo + por_thread*threads +
  por_grupo*grupos`, em vez do range inteiro; o resto do binding é sincronizado como leitura. Quem
  lista um shader garante que ele não escreve fora da janela (janela maior é sempre segura; menor
  deixa a CPU ler dados velhos). Gráficos, dispatches indiretos e buffers com formato mantêm o range
  inteiro. O cache de shaders não muda.
- **Menos travadas ao andar pelo mapa:** o timer do Windows passa a 1 ms (`SOTC_TIMER_RES=0`
  desliga), liberar memória ainda espera a thread da GPU por padrão (`SOTC_DROP_ASYNC=1`, experimental
  e desligado, deixa de esperar), os workers de
  compilação rodam abaixo do normal (`SOTC_WORKER_LOW_PRIO=0`) e o sleep do ritmo de quadros usa um
  timer de alta resolução.
- **Sombras piscando logo após carregar:** os passes de sombra (só profundidade) compilam na hora
  em vez de serem pulados (`SOTC_DEPTH_SYNC_PIPE=0` desliga).
- **Manchas coloridas nas malhas:** texturas pequenas são comparadas inteiras, não só os primeiros
  8×8 texels (`SOTC_MAYBE_DIRTY_HASH=0` re-envia sempre).
- Menos chamadas por desenho: o formato dos vértices não é reenviado quando não mudou
  (`SOTC_DEDUP_STATE=0`); chamadas de `VirtualProtect` vizinhas são juntadas.
- O SRT walker resolve instruções `Phi` simples (`SOTC_SRT_PHI=0` desliga). O cache de shaders é
  refeito na primeira abertura.
- **Experimentais, desligados por padrão** (`=1` liga): `SOTC_UNBIND_GPU_WAIT`,
  `SOTC_VIEW_CHUNK_MB=2`, `SOTC_GPU_THREAD_HIGH`, `SOTC_DEDUP_PUSH`, `SOTC_FASTBIND_WIDE`,
  `SOTC_DESC_BUFFER`, `SOTC_ARENA_TICK_FREE`, `SOTC_CPUSETS`, `SOTC_WAIT_NOSPIN`,
  `SOTC_PRESENT_WAIT`, `SOTC_CP_AHEAD`, `SOTC_FAULT_GRANULE=65536`, `SOTC_EOP_TICK`,
  `SOTC_HASH_SMALL=16384`, `SOTC_REBAR_UPLOAD` (placas com Resizable BAR; `SOTC_REBAR_MB` limita),
  `SOTC_XFER_QUEUE` (fila de transferência, se a placa tiver), `SOTC_HOST_IMAGE_COPY`,
  `SOTC_LOW_LATENCY` (Reflex na NVIDIA, Anti-Lag na AMD, limitador genérico nas outras),
  `SOTC_STAGE_BINDINGS` (refaz o cache de shaders), `SOTC_LAYOUT_CACHE`, `SOTC_PASS_SEC`
  (passes grandes gravados numa thread à parte; `SOTC_PASS_SEC_MIN` = desenhos mínimos, 64),
  `SOTC_BINDLESS_SAMP` (samplers num heap global; refaz o cache de shaders; não combina com
  `SOTC_STAGE_BINDINGS` nem `SOTC_DESC_BUFFER`). Todos caem para o
  caminho atual quando a placa ou o driver não têm o recurso.
- Diagnóstico das travadas: com `SOTC_FPS=1` o log mostra o tempo de CPU por quadro de cada
  thread da GPU (`SOTC cpu/frame`: command processor, draws, gravação, `PassSecondary`, cópias),
  o que a thread de draws esperou (`SOTC draw waits/frame`: GPU, gravação, uploads, lock do cache
  de texturas), o tempo que o command processor ficou bloqueado na thread de draws
  (`SOTC cp/frame`) e o da thread de gravação (`SOTC recorder/frame`). Cada quadro acima de
  `SOTC_STALL_MS` (70 ms) ganha uma linha `SOTCSTALL` com esses números só daquele quadro, sem
  precisar de `SOTC_FRAME_LOG`. Sem `SOTC_FPS` nada é medido.
- Diagnóstico dos readbacks (`SOTC_FPS=1`): a linha `SOTC RB pattern` mostra, a cada 5 s, quantos
  pedidos de readback das threads do jogo caem logo depois (ou antes, até 64 KiB) da janela
  anterior da mesma thread (e quantos em até 1 ms), quantos caem dentro dela, quantos repetem uma
  janela do quadro anterior, o tamanho médio pedido/janela/baixado e as 5 janelas mais pedidas. A
  linha `SOTCSTALL` ganha `rb adj` (pedidos vizinhos naquele quadro).
- `SOTC_RB_GROW=1` (experimental, desligado): quando uma thread do jogo lê a memória escrita pela
  GPU em sequência (cada pedido vizinho à janela anterior em até `SOTC_RB_GROW_US`, 1000 µs), a
  janela do readback dobra a cada pedido, na direção da leitura, até `SOTC_RB_GROW_MAX_KB`
  (1024); qualquer outro pedido volta à janela normal (`SOTC_RB_NARROW`, 64 KiB). Só vale com a
  janela estreita. Com `SOTC_FPS=1`, linha `SOTC RB grow` (janelas aumentadas, quantas batem em
  escritas do envio atual, readbacks economizados no máximo).
- `SOTC_BIND_CACHE=1` (experimental, desligado): os recursos dos últimos 32 desenhos que só leem
  buffers e texturas ficam guardados; um desenho com o mesmo estado de um anterior no mesmo envio
  à GPU (não só o imediatamente anterior, como no `SOTC_FAST_BIND`) não refaz as buscas no cache
  de buffers e de texturas, só reenvia os descritores. Tudo é descartado quando muda o envio, a
  memória do jogo, os buffers/texturas/samplers residentes ou os render targets, ou quando um
  desenho, dispatch ou DMA escreve na memória da GPU. Linha `SOTC BIND CACHE` com `SOTC_FPS=1`.
- `SOTC_ADAPTIVE_LOAD=1` (experimental, desligado): controle de carga dinâmico. Quando dois
  quadros seguidos passam de `SOTC_ADAPTIVE_HIGH_MS` (36 ms, ~27,8 FPS no limite de 30), o port
  alivia o próprio trabalho até os quadros voltarem ao ritmo (todos até `SOTC_ADAPTIVE_LOW_MS`,
  34,5 ms) por `SOTC_ADAPTIVE_HOLD_MS` (1000 ms; dobra, até 8×, se voltar a pesar logo depois).
  No modo 60 FPS os limites acompanham o alvo (18 / 17,25 ms); sem limite de FPS ele não age.
  Enquanto alivia: as duas cascatas distantes da sombra do sol são desenhadas em quadros
  alternados e deslocadas nos outros, como `SOTC_SHADOW_REUSE=1` (`SOTC_ADAPTIVE_SHADOW=0`
  desliga; onde o deslocamento não serve, a cascata é desenhada, nunca pulada); os envios à GPU
  ficam a cada `SOTC_ADAPTIVE_FLUSH_US` (4000 µs em vez de 1000); a limpeza rotineira de texturas
  e a verificação de memória fria esperam (`SOTC_ADAPTIVE_DEFER=0` desliga; sob pressão de
  memória tudo roda como antes). **Troca visual:** só enquanto o jogo está abaixo de ~27 FPS, as
  sombras distantes podem atrasar um quadro ou piscar brevemente (o mesmo pisca-pisca da
  reutilização de sombras). `SOTC_SHADOW_REUSE=1` e a opção do menu no jogo continuam valendo
  como antes. Linha `SOTC ADAPTIVE` com `SOTC_FPS=1`.
- `SOTC_BIND_CACHE_RANGES=1` (experimental, desligado; só vale junto com `SOTC_BIND_CACHE=1`):
  um dispatch, desenho ou DMA que escreve na memória da GPU descarta só as entradas que leem o que
  ele escreve (faixas de buffers e texturas que se sobrepõem, ou a mesma textura), em vez do cache
  inteiro; dispatches que só leem não descartam nada. Mudanças na memória do jogo também deixam de
  esvaziar o cache: cada acerto já recusa faixas modificadas pela CPU e texturas sujas. As regras
  de envio, de recursos residentes e de render targets continuam. Desenhos que leem um texel
  buffer no endereço de uma textura, ou uma cópia no stream buffer, não são guardados. A linha
  `SOTC BIND CACHE` ganha `invalidated by ranges N entries (writes M)`.
- `SOTC_RS_MEMO=1` (experimental, desligado): um desenho nos mesmos render targets do desenho
  anterior, no mesmo envio à GPU e no render pass que ele deixou aberto, reaproveita o estado de
  renderização dele em vez de procurar os alvos de novo no cache de texturas. Não vale quando
  algum alvo foi recriado, sujo pela CPU ou também é lido como textura, nem em passes com clear
  (CMASK/HTILE ou registrador) ou com feedback loop. Linha `SOTC RS MEMO` com `SOTC_FPS=1`
  (acertos e motivos das falhas).
- `SOTC_PARALLEL_RECORD=1` (experimental, desligado; precisa da thread de draws e de mais de 6
  threads de CPU): 1 ou 2 threads `GpuPrep` (`SOTC_PARALLEL_RECORD_WORKERS`) seguem a fila de
  desenhos e calculam antes da thread de draws o que só depende dos registradores (estado
  dinâmico, push constants, a chave do fast bind/bind cache, os offsets de vértice) e fazem o
  `SOTC_ASYNC_IMAGE_CREATE` no lugar do command processor. As decisões continuam na thread de
  draws, na ordem; um desenho sem o cálculo pronto é feito como hoje. `SOTC_PR_VERIFY=N` confere
  um a cada N desenhos com o cálculo normal e registra as diferenças. Linha `SOTC PR prep/frame`
  com `SOTC_FPS=1`.
- `SOTC_SYNC_FAST=1` (experimental, desligado): cada buffer de cada desenho ou dispatch não varre
  mais as páginas rastreadas quando nada mudou desde a última sincronização daquele trecho. Um
  contador por bloco de 2 MiB sobe sempre que a CPU suja uma página (ou um dado escrito pela GPU
  deixa de valer); um trecho que ficou limpo e cujos blocos não mudaram é pulado. Páginas que
  ficam liberadas para escrita (fault grace, `SOTC_HOT_PAGES`) seguem pelo caminho normal. Linha
  `SOTC SYNC FAST` com `SOTC_FPS=1` (pulados/consultados).
- `SOTC_PROTECT_BATCH=1` (experimental, desligado): as re-proteções de páginas de um upload de
  buffer (páginas que a CPU sujou e que voltam a ser protegidas contra escrita) são juntadas e
  aplicadas de uma vez antes de o upload ler a memória do jogo, em vez de uma chamada de
  `VirtualProtect` por bloco rastreado de 16 MiB; páginas cujo estado mudou nesse meio-tempo são
  puladas. Páginas com dados escritos pela GPU e as desproteções (readbacks, faults) continuam
  imediatas. Linha `SOTC PROTECT BATCH` com `SOTC_FPS=1` (chamadas e páginas, pedidos juntados,
  páginas puladas).
- `SOTC_ASYNC_COMPUTE_XLATE=1` (experimental, desligado): quando um dispatch precisa de um shader
  de compute novo, o resto do command buffer é varrido e os outros shaders de compute novos vão
  para os workers, traduzidos em paralelo; os dispatches continuam publicados em ordem. Linha
  `SOTC COMPUTE PREFETCH` com `SOTC_FPS=1`.
- `SOTC_DROP_FAST=1` (experimental, desligado): liberar memória sem dados escritos pela GPU (o
  caso comum ao trocar de área) não espera mais a thread de draws; no boot e no carregamento do
  save essa espera somava 2,5-3 s a cada 5 s, com as outras threads do jogo paradas no lock da
  memória. Linha `SOTC drop fast` com `SOTC_FPS=1`.
- `SOTC_FLIP_SERVICE=1` (experimental, desligado): enquanto a thread de draws espera um quadro
  livre para apresentar (~20% do tempo dela), ela atende os pedidos das threads do jogo
  (readbacks, liberar memória) em vez de deixá-los para depois.
- `SOTC_CP_FLIP_PACE=1` (experimental, desligado): a espera por um quadro livre para apresentar
  passa da thread de draws para o command processor, antes de enfileirar o flip. A thread de
  draws não bloqueia mais ali e atende na hora os pedidos das threads do jogo; o FPS é o mesmo.
  Linha `SOTC CP flip pacing` com `SOTC_FPS=1`.
- **Cache de shaders que não se atualizava:** um arquivo do cache regravado depois do pacote
  (`cache.sotcpack`) perdia para a cópia antiga do pacote, então ~1850 pipelines eram recusados
  em todo boot e ~2100 estágios de shader eram traduzidos de novo em toda sessão (com ~6900
  desenhos pulados). Agora o arquivo mais novo vence e o pacote é refeito com ele. A versão dos
  shaders sobe junto (binários antigos com o mesmo número davam vértices errados e árvores
  escuras): **o cache de shaders é refeito na primeira abertura.**
- `SOTC_VIEW_SPLIT_FINE=1` (experimental, desligado): quando o jogo libera parte de uma visão de
  memória, as partes que ficam viram visões de 64 KiB; as próximas liberações ali não param mais
  todas as threads do jogo (49 ms parados numa janela de 5 s ao trocar de área).
- `SOTC_RB_OWNER=1` (experimental, desligado): várias threads do jogo leem a mesma memória
  escrita pela GPU ao mesmo tempo, e cada readback copiava tudo de novo; o primeiro a terminar
  deixava os outros "velhos" e eles eram refeitos esperando a GPU. Agora as páginas que um
  readback em andamento está baixando são dele: o seguinte não as copia e, para a parte que pediu,
  espera e termina junto com o dono. `SOTC_RB_BATCH=1` (experimental, desligado): os readbacks
  pedidos juntos vão para a GPU num só envio. Linha `SOTC readbacks sharing pages` com
  `SOTC_FPS=1`.
- `SOTC_RB_KEEP_LATER=1` (experimental, desligado): um readback em andamento cujos bytes a GPU
  escreve de novo por um comando posterior era refeito na hora, com a thread de draws esperando a
  GPU (~15 mil numa sessão, concentrados ao virar a câmera em áreas densas). Agora o dado lido é
  aplicado (é o que a memória tinha quando o jogo pediu) e só os trechos da escrita posterior
  continuam marcados como da GPU, para a próxima leitura buscar o dado novo. Linha
  `SOTC readbacks made stale by` com o motivo de cada um, com `SOTC_FPS=1`.
- `SOTC_RB_ASYNC_AHEAD=1` (experimental, desligado): quando uma thread do jogo lê memória escrita
  pela GPU, a cópia ia no envio do trabalho que estava sendo gravado e a thread esperava tudo até
  ali na GPU, inclusive as cópias de upload daquele envio (até ~190 ms de CPU nos quadros
  pesados). Se os bytes pedidos só foram escritos por trabalho já enviado, a cópia agora vai num
  envio pequeno à parte, logo atrás dele, sem fechar o envio atual nem esperar os uploads dele, e a
  thread do jogo espera só essa cópia. Nos outros casos nada muda. Com `SOTC_FPS=1` o log ganha a
  linha `SOTC RB guest wait` (quantos readbacks a thread do jogo esperou, o total, a média, o
  maior, e quanto foi esperando o envio e a GPU), a linha `SOTC RB ahead-async` (quantos poderiam
  ir na frente, quantos foram e a espera média de cada caminho) e o campo `rb wait` nas linhas
  `SOTCSTALL`; essas medidas valem mesmo com o switch desligado.
- `SOTC_RB_PREFETCH=1` (experimental, desligado; só age junto com `SOTC_RB_OWNER=1`): nos quadros
  travados as threads do jogo passavam 60–90 ms esperando readbacks (até ~126 por quadro), quase
  sempre das mesmas janelas a cada quadro. Uma janela lida em pelo menos 2 dos 3 quadros
  anteriores passa a ser "recorrente"; quando a GPU escreve nela, a cópia dos trechos escritos
  vai para a memória de staging logo depois do comando, no mesmo envio, e fica como dona dessas
  páginas. Quando o jogo lê a janela e a cópia já terminou, ela é aplicada na hora, sem esperar a
  GPU. Nada é escrito na memória do jogo antes dessa leitura e nenhuma proteção de página muda:
  qualquer escrita posterior nesses bytes (GPU, upload, liberação de memória, outro readback)
  descarta a cópia, e uma cópia não pedida em `SOTC_RB_PREFETCH_TICKS` envios (8) é descartada;
  nos dois casos a leitura segue o caminho normal. Limites: `SOTC_RB_PREFETCH_MAX` cópias ao mesmo
  tempo (32) e `SOTC_RB_PREFETCH_MB` de staging (4). Com `SOTC_FPS=1`, linha `SOTC RB prefetch`
  (emitidas, usadas e a espera economizada estimada, expiradas, descartadas, puladas pelos
  limites, ainda na GPU na hora da leitura) e o campo `rb pf` nas linhas `SOTCSTALL`.
- `SOTC_HEAP_GUARD_RUNS=1` (experimental, desligado): a proteção do heap do jogo durante os
  readbacks é refeita ~100 vezes por segundo ao carregar áreas; cada vez ela localizava de novo
  onde fica cada pedaço da memória que lê. Agora reaproveita isso enquanto o mapa de memória não
  muda. Diagnóstico: linha `SOTC readbacks redone synchronously` (readbacks que ficaram velhos e
  foram refeitos esperando a GPU) com `SOTC_FPS=1`.
- `SOTC_CARVE_NOSTOP=1` (experimental, desligado): ao entrar em área nova o jogo libera pedaços
  de 64 KiB-1,4 MiB dentro dos heaps grandes, e cada liberação dividia uma visão de memória
  parando todas as threads do jogo (738 vezes numa sessão, até 218 ms parados a cada 5 s). Quando
  a parte da visão que fica não tem páginas bloqueadas para leitura (dado da GPU) nem pilha de
  fibra, a
  divisão acontece com o jogo rodando: uma thread que toque ali no meio espera e repete o acesso.
  Páginas protegidas só contra escrita voltam graváveis e são marcadas como escritas pela CPU
  (reenviadas no próximo uso). `SOTC_CARVE_LOG=1` mostra por que cada divisão ainda parou.
  As outras ainda param, mas a suspensão das threads agora é feita em paralelo. Linha
  `SOTC view splits without stopping the game` com `SOTC_FPS=1`.
- `SOTC_VIEW_CHUNK_KB=64` (experimental, desligado): a memória do jogo é mapeada em visões de 64 KiB;
  ao entrar em área nova o jogo remapeia páginas de 64 KiB dentro dos heaps grandes
  (`sceKernelBatchMap`), e com visões de 2 MiB cada primeiro remapeamento dividia uma visão e
  parava todas as threads do jogo (até 87 ms a cada 5 s). `SOTC_CARVE_LOG=1` registra cada divisão
  que ainda acontecer (quem pediu, endereço, alinhamento e tempo parado).
  **Não recomendado:** com ~150 mil visões cada mudança de proteção vira dezenas de chamadas,
  e o jogo caiu para 2-4 FPS no teste.
- `SOTC_ASYNC_TEXTURE_UPLOAD=1` (experimental, desligado): a cópia da memória do jogo para o
  upload de texturas usa o mesmo pool de cópia e a mesma cerca de escrita do `SOTC_ASYNC_UPLOAD`.
- `SOTC_IMAGE_POOL_MB=<n>` (desligado): reserva n MiB de memória de vídeo para texturas no boot,
  e as texturas novas não esperam mais `vkAllocateMemory` ao carregar uma área.
- `SOTC_ASYNC_IMAGE_CREATE=1` (experimental, desligado): o command processor lê as texturas de
  cada desenho antes da thread de draws e, para as que ainda não existem, dois workers criam a
  imagem Vulkan com a memória dela (~1 ms cada ao carregar uma área) enquanto isso; a thread de
  draws pega a imagem pronta. Sem reservar memória de vídeo: só dentro do orçamento, no máximo
  128 imagens/384 MiB adiantadas, e as que ninguém usa em 2 s são liberadas. Linha
  `SOTC async image create` com `SOTC_FPS=1`.
- `SOTC_HOT_PAGES=1` (experimental, desligado): páginas que o jogo reescreve a cada frame ficam
  sem proteção por 2 s (`SOTC_HOT_PAGES_MS`) em vez de gerar uma falta de página por frame; são
  reenviadas nos syncs (o `SOTC_HASH_SMALL` pula as que não mudaram). No máximo 1024 páginas
  (`SOTC_HOT_PAGES_MAX`). Linha `SOTC hot pages` com `SOTC_FPS=1`. **Não recomendado:** no teste
  reenviou 0,6-1,7 GB a cada 5 s e copiou buffers de vértices no meio da escrita do jogo
  (vértices piscando, mais travadas).
- Build: libc++ ligada estaticamente no `bb-probe.exe` (o `libc++.dll` fica só para os codecs do FFmpeg), xxhash compilado inline, e três custos
  tirados do caminho quente da thread de draws (nomes de depuração de imagens montados com os
  nomes desligados, a checagem do nível de log e a do `SOTC_OPT_OFF`).
- `SOTC_STAGING_LARGE_POW2=1` (experimental, desligado): buffers de staging grandes em classes de
  potência de dois, reaproveitados até 2x o pedido, e até `SOTC_STAGING_LARGE_KEEP_MB` (256)
  mantidos ociosos em vez de alocados de novo a cada rajada de streaming.
- `SOTC_ASYNC_UPLOAD=1` (experimental, desligado): a cópia da CPU de uploads grandes de buffer
  (a partir de `SOTC_ASYNC_UPLOAD_MIN_KB`, 256) sai da thread de draws e vai para um pool de
  threads de cópia (`SOTC_ASYNC_UPLOAD_THREADS`, 3); a submissão para a GPU espera as cópias do
  seu tick. Uma cerca de escrita faz escritas do jogo, unmaps, remapeamentos e a memória liberada
  esperarem a cópia pendente daqueles bytes, então os dados são os mesmos de antes. Páginas
  quentes (que ficam sem proteção) e pilhas das threads ficam na thread de draws. Linha
  `SOTC async upload` com `SOTC_FPS=1`.
- `SOTC_FENCE_PIECES=1` (experimental, desligado; só vale junto com `SOTC_ASYNC_UPLOAD=1` ou
  `SOTC_ASYNC_TEXTURE_UPLOAD=1`): a cerca de escrita espera só os pedaços da cópia (até 512 KiB)
  que leem os bytes escritos, não o upload inteiro (até 16 MiB num sweep de DMA juntado), e a
  thread do jogo copia ela mesma os pedaços que nenhuma thread de cópia começou. Leituras do jogo
  não esperam mais a cerca (não mudam o que a cópia lê). Linha `SOTC FENCE` com `SOTC_FPS=1`
  (esperas e tempo, pedaços copiados pela thread do jogo, leituras que passaram sem esperar); as
  linhas `SOTCSTALL` ganham o tempo da cerca no quadro (`fence ... ms`).
- `SOTC_HOST_IMPORT=1` (experimental, desligado): uploads grandes de buffer (a partir de
  `SOTC_HOST_IMPORT_MIN_KB`, 256) são copiados pelo motor de cópia da GPU direto da memória do
  jogo, importada com `VK_EXT_external_memory_host` em pedaços de 64 MiB sob demanda (no máximo
  `SOTC_HOST_IMPORT_MB`, 512, travados na RAM), em vez de copiados pela CPU. Precisa de uma fila
  só de transferência; sem ela, sem a extensão ou se o driver recusar a memória, o upload segue
  pela CPU. Linha `SOTC host import` com `SOTC_FPS=1`.
- `SOTC_CPUSETS` põe as threads da GPU nos núcleos mais rápidos e, em processadores híbridos
  (núcleos P/E), os workers de compilação nos núcleos E.
- Diagnóstico (`SOTC_FPS=1`): linhas `SOTC unmap`, `SOTC FASTBIND`, `SOTC arena tick waits` e o
  motivo de cada `SOTC SYNC PIPELINE`; `SOTC_CMD_STATS=1` conta os pacotes `SET_PREDICATION`.
- `SOTC_CP_IDLE_US=<µs>` (experimental, desligado; sugerido 50): quando nenhuma fila do command
  processor avança por esse tempo (todas esperando um label, um semáforo ou a CPU do jogo), a
  thread dorme até ser avisada ou por no máximo `SOTC_CP_IDLE_TIMEOUT_US` (200 µs), em vez de
  girar a 100% de um núcleo com `yield` (nos quadros de travada eram ~140 ms de CPU em 150 ms).
  Labels, cercas e dados escritos pela thread de draws, labels de flip, novos envios e comandos
  acordam a thread na hora; o que só a CPU do jogo escreve é visto no fim do timeout. Antes de
  dormir ela continua girando como hoje, então esperas curtas não mudam.
- Diagnóstico (`SOTC_FPS=1`): linha `SOTC CP spin` (tempo por quadro em que o command processor
  só re-testou esperas, rodadas sem progresso por tipo de espera, sonos, timeouts e a latência
  média do aviso até acordar) e `spin ... ms` nas linhas `SOTCSTALL`. O tempo de `parse` passa a
  contar só as retomadas que avançaram (antes incluía o giro) e o giro não lê mais o relógio a
  cada retomada, o que custava ~5% da thread com `SOTC_FPS`.
- Arquitetura-alvo e plano de migração: `docs/ARCHITECTURE-TARGET.md`.

</details>

## 0.70 — 2026-10-29
> ⚠️ **Atenção: instale o jogo num SSD (de preferência NVMe).** Cada área nova lê centenas de MB
> de texturas e modelos da pasta do jogo; num HD as trocas de área travam muito mais e podem
> parar por segundos. Coloque a pasta do jogo (o dump) e, se puder, a pasta do port num SSD.

- **Trocas de área sem o FPS cair perto de zero.** Antes, cada troca de área derrubava o FPS e às
  vezes forçava telas de carregamento de 1–3 s; o pior quadro passava de meio segundo (534 ms).
  Agora o pior fica em 58–75 ms, sem tela de carregamento forçada, e o final do jogo vai até os
  créditos. Ainda se perdem 1–6 quadros em cada troca: é o próximo trabalho. As causas eram a
  trava da tabela de memória do jogo, cópias de memória remapeando gigabytes, a busca de regiões
  da GPU refeita a cada escrita, a proteção da memória do jogo percorrendo o mesmo trecho
  milhares de vezes e muitas texturas novas num quadro só. Agora entram no máximo 32 MB de
  texturas novas grandes por quadro, e as que ficam para o quadro seguinte aparecem pretas por
  um instante. Cada correção tem uma chave para desligar (`SOTC_LOCK_WAKE=0`,
  `SOTC_VIEW_CHUNK_MB=0`, `SOTC_REGION_LOG=0`, `SOTC_REGION_FAST=0`, `SOTC_PROTECT_FAST=0`,
  `SOTC_WRITE_FAST=0`, `SOTC_BATCH_SPLIT=0`, `SOTC_GUARD_DEDUP=0`, `SOTC_UPLOAD_BUDGET_MB=0`).
- **Queda rara no alocador do jogo depois de uma carga** (`0x49800`): dois atalhos da proteção da
  memória do jogo podiam deixar a GPU escrever num bloco que o jogo acabava de liberar. Saem
  desligados (`SOTC_GUARD_STALE=1` e `SOTC_GUARD_USER_POINTER=1` ligam). Uma sessão de 36 min com
  6 carregamentos sem queda; se cair, mande o log.
- **DLSS no modo PS4 Pro a 30 FPS sem a imagem tremer:** o quadro repetido mostrava a imagem sem
  DLSS.
- **Geração de quadros (F8) com FSR 1: o HUD fica parado.**
- **LOD feedback** (`SOTC_LOD_FEEDBACK=1`, experimental) não custa mais ~7 FPS.
- Diagnóstico: `SOTC_DRAW_SPLIT=1` (com o FPS na tela) mostra no log onde vai o tempo da thread
  de desenho.

## 0.60.6-C3 — 2026-10-27
- **Barra de stamina com DLSS (correção da C2):** a área da barra passou a usar a largura de uma
  tela de 1080 linhas na proporção da saída (1080 × L/A) em vez da largura de renderização. Com
  DLSS a renderização é menor (1704x720 em 2560x1080) e a barra amarela voltava a parar longe do
  círculo; em 16:9 com DLSS sem o modo PS4 Pro a C2 também a quebrava.
- **Limite de 30 FPS:** "30" (menu F1 ou F7) segura 30 FPS mesmo quando o jogo pede para não
  limitar (modo PS4 Pro); antes ficava em ~42.

## 0.60.6-C2 — 2026-10-26
- **Tela larga (21:9, 32:9): barras de vida e stamina certas (#301, #202).** A barra de vida
  tinha um pedaço cinza a mais e a amarela da stamina parava longe do círculo e enchia ao
  contrário. O aspecto do objeto de tela do jogo fica 16:9 (o jogo desenha a barra de vida com
  ele) e dois retângulos de 1920 de largura da interface passam a usar a largura da tela.
- **Vibração do controle pode ser desligada (#292, Lucas-blip-png):** no lançador (Controles) e
  no menu F1 (seção Controle), na hora.
- **Mais FPS no Santuário (Lucas-blip-png):** a barreira de imagens de armazenamento GENERAL é
  feita uma vez por fence em vez de antes de cada desenho (cada uma encerrava o passe de
  renderização, ~20 mil por segundo), e o buffer de profundidade lido enquanto o stencil é escrito
  fica num layout só. `SOTC_GENERAL_SYNC=0` / `SOTC_DEPTH_RO=0` desligam.
- **Geração de quadros sem a interface (opção do Lucas-blip-png, experimental):** Imagem e FPS →
  "Geração de quadros sem a interface". As letras e as barras não tremem mais com o FSR 3
  (funciona também no modo PS4 Pro). Pode desbotar o logo da tela de título.
- **DLSS Ultra Desempenho (3×) (#306)**; o DLSS só amplia 2× + FSR 1 quando nenhum modo aceita a
  escala (modo Pro em 4K).
- Testes: `SOTC_RENDER_KEEP`, `SOTC_RENDER_EXTRA` e `SOTC_WIDE_ASPECT` (achar valores de tamanho
  de tela do jogo).

## 0.60.6-C1 — 2026-10-26
Versão da comunidade: correções e recursos do tabnul, do Oussama268, do NathanMBR e do
JihedMrouki, testados com texturas **Máxima + modo PS4 Pro + 60 FPS** (RTX 2060 SUPER).

- **Correção do alocador de páginas do jogo (tabnul, PR #288 / fork #112):** o contador de
  páginas livres do jogo se descolava do mapa real e o jogo lia antes do início do mapa (queda
  `0x1a371`). Com a correção e a reserva de memória de texturas em 0 (também no modo PS4 Pro), o
  streamer não derruba mais todas as texturas para o nível 3 a cada segundo. Resultado: **uma
  hora em Máxima + Pro sem queda** e quase todo o lixo colorido depois dos carregamentos sumiu.
- **Textura Máxima no modo PS4 Pro (experimental):** voltou a valer (o lançador trocava por Alta).
  `SOTC_MAX_IN_PRO=0` volta ao comportamento antigo.
- **NVIDIA DLSS (RTX 20 ou mais nova), do tabnul (fork PR #81):** o jogo renderiza menor e o
  DLSS amplia; a interface é desenhada na resolução de saída, TAA do jogo desligado, vinheta
  depois da ampliação. Novo aqui: **funciona no modo PS4 Pro** (a composição final do Pro tem a
  cena e a interface em outra ordem e tamanho) e **em 4K** (o DLSS amplia 2x e o FSR 1 completa;
  antes a tela ficava preta). No menu F1 o upscaler aparece como "NVIDIA DLSS" e só troca no
  lançador (antes gravava "fsr1" por cima). Arquivos `sl.dlss.dll` e `nvngx_dlss.dll` (NVIDIA
  Streamline 2.14.16) incluídos.
- **Grama (tabnul):** distância, altura e detalhe da grama e do mato mudam na hora no menu F1;
  sombras e esmaecimento da grama no lançador; buffers de grama 4x maiores (a grama distante não
  some em faixas).
- **Mais FPS: leituras rápidas da GPU (experimental),** nova opção nos Ajustes rápidos, com a
  checagem final e o travamento por página do Oussama268 (`SOTC_RB_HOT=1`). Desligada por padrão.
- **Reutilização de binds** ligada de novo por padrão (Oussama268; a correção do chão de areia
  foi conferida). Do Oussama268 também: o processador de comandos não espera filas vazias e a
  memória fria das arenas volta sem esperar a GPU.
- **Versão americana CUSA08034 (NathanMBR, PR #270)** — detalhes abaixo.
- **CPUs fracas e GTX 10xx (JihedMrouki, PR #258):** posição invariante nos pipelines com
  emulação de baricêntricas (diferença de textura/profundidade no chão) e o cache de shaders não lê
  do disco arquivos que não existem. O cache de shaders é refeito na primeira abertura.
- **F7** passa por **30 → 60 → ilimitado** (antes só ligava/desligava o limite de 30).
- **Aba Debug do lançador:** menu de debug da Bluepoint (**F6 ou Start+Select** no controle; o
  Select sozinho continua abrindo o mapa), **pular os logotipos de abertura**, **ir direto para o
  jogo** (`+nointro`, pula também a tela Start), vigia da memória de texturas, F5 (captura das
  texturas do quadro, limitada a 1 GB) e F9 (recarrega as texturas; com "Comparar DLSS" ligado,
  troca DLSS / FSR 1).
- Menu F1: sombras e esmaecimento da grama mostram o valor do jogo quando ainda não foram salvos.

### Versão americana (CUSA08034), NathanMBR
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
