# Memória de testes — quedas de FPS e travadas na troca de área

Meta do usuário (23/11): **60 FPS estável e nenhuma travada na troca de área** (no PS5 não parece travar).

Este arquivo é a regra. **Antes de qualquer teste novo:**
1. Ler a tabela inteira e conferir que o teste **não repete** uma linha (mesma chave, mesma hipótese).
2. Escrever a linha nova com hipótese, chave e a medida de aprovação **antes** de rodar.
3. Mandar a proposta para a **IA parceira** (um subagente revisor) com esta tabela: ela confere os
   cálculos, aponta repetição e propõe alternativa. Registrar a opinião dela na coluna "Parceira".
4. Depois da partida: preencher números e veredito na mesma hora. Teste sem número não conta.

Sintomas (não misturar):
- **Travada** = quadro de 100 ms ou mais (imagem parada). Medida: pior quadro e quadros > 50 ms (`SOTC FRAMES`).
- **Mergulho** = segundos com quadros de 20–55 ms. Medida: menor valor de `SOTC SCREEN FPS` no trecho e quantos segundos abaixo de 50.
- **Média** = FPS no resto do caminho (janelas `SOTC FPS` de 5 s com 3.000+ comandos/quadro).

Validade de uma partida (senão não entra na tabela):
- Partida do **usuário**, no trecho (carga do save + troca de área da floresta). Harness só para estabilidade e trancos de shader, nunca para FPS.
- `md5` do `bin/bb-probe.exe` = o exe instalado; a chave aparece ativa no log; 3.000+ comandos/quadro no trecho.
- Como terminou: "Queda em"/"O jogo caiu" = queda; "Jogo parado pelo lançador" = usuário fechou.
- **Medições ligadas pesam** (`SOTC_DRAW_SPLIT`, `SOTC_SLOW_OPS`, `SOTC_GPU_TIMES`, `SOTC_TICKWATCH`): anotar na coluna "Medições"; só comparar partidas com as mesmas medições.
- Uma mudança por partida. Editar `launcher_settings.json` só com o lançador fechado.

**Correção do usuário (23/11 10:55): "voltou para 60 FPS porque pausei o jogo".** Em todas as partidas de 23/11, logo
depois do mergulho da floresta o log cai para ~535 comandos/quadro = **tela de pausa**. O "volta a 60 em ~3 s" era a
pausa: **a duração real do mergulho nunca foi medida**. Regra nova: na troca de área da floresta, o usuário **continua
jogando ~30 s** antes de pausar; só valem segundos com 3.000+ comandos/quadro (a pausa e o menu não contam). O
argumento "o mergulho dura mais que as cópias" do #41 continua de pé (as cópias acabam antes da pausa), mas a duração
do mergulho em todas as linhas de 23/11 está truncada pela pausa.

Vereditos: ✅ ganho (fica) · ➖ sem ganho (desligado, não repetir) · ❌ piorou ou quebrou · 🔬 só medição · ⏳ falta partida.

## Tabela

### Hipótese H1 — o jogo espera a GPU devolver o mapa de visibilidade (leituras de volta)

| # | Data | Mudança (chave) | Resultado medido | Veredito |
|---|---|---|---|---|
| 1 | 03/11 | `SOTC_RB_AGE=1` | 84% da espera é dado escrito no quadro atual | 🔬 |
| 2 | 03/11 | `SOTC_CLEAN_GUEST_READS=1` | quadros > 50 ms 14/10 → 7/13 por minuto (ruído alto) | ➖ |
| 3 | 03/11 | `SOTC_STALE_READS` (valor de 100 ms atrás) | cena inteira piscou, 11/13 | ❌ |
| 4 | 05/11 | `SOTC_RB_STREAM=1` (leituras em sequência) | esperas iguais | ➖ |
| 5 | 05/11 | envio tardio / `SOTC_RB_EARLY` | já envia a cada ~1 ms | ➖ |
| 6 | 05/11 | Mailbox no lugar de FIFO | esperas iguais | ➖ |
| 7 | 05/11 | `SOTC_RB_HOT=2` | 400–900 leituras/s iguais | ➖ |
| 8 | 05/11 | `SOTC_VIS_DIRECT` v1 | floresta > 50 ms 31 → 14, mas quedas no final (`0x499cc`) | ❌ |
| 9 | 05/11 | `SOTC_VIS_DIRECT` v2 (todo rótulo espera a GPU) | 58,8 → 41,8 FPS | ❌ |
| 10 | 18/11 | `SOTC_CULL_QUEUE=1` (fila separada) | troca de área 39–41 (ref. 41–47) | ➖ |
| 11 | 18/11 | `SOTC_RB_GROUP` (etapa 1) | troca 42–44; até 213 MB/5 s a mais | ➖ |
| 12 | 18/11 | `SOTC_RB_PUSH=1` (etapa 2) | troca 45; 92% das cópias velhas | ➖ |
| 13 | 18/11 | `SOTC_RB_SAFEPOINT=1` (etapa 3) | troca 40–49 | ➖ |
| 14 | 18/11 | `SOTC_CULL_QUEUE` + `_PAGES` | troca 38–42 | ➖ |
| 15 | 19/11 | `SOTC_RB_NARROW=256` | troca 37–45 | ➖ |
| 16 | 23/11 10:26 | `SOTC_VIS_DIRECT=1 SOTC_VIS_DIRECT_ORDER=1` (v3) | espera de leitura por quadro ~14,5 → ~5,5 ms no caminho e 19,7 → 6,4 no mergulho; **processador de comandos 12 → 16–18 ms/quadro** (o custo mudou de lugar); após a carga 41–52 por ~17 s (#36: 43–59); resto 50–55 em 44–58 s, depois 53–60; mergulho 37 (igual), pior 50,1 ms | ❌ |

**Conclusão H1:** 16 linhas, nenhuma tirou o mergulho. Encurtar ou adiantar a leitura não muda o mergulho. **Não abrir teste novo em H1** sem um dado novo que mude a causa.

### Hipótese H2 — o trabalho do port na CPU (thread de desenho, processador de comandos)

| # | Data | Mudança (chave) | Resultado medido | Veredito |
|---|---|---|---|---|
| 17 | 28–29/10 | travas de memória (`SOTC_LOCK_WAKE`, `VIEW_CHUNK_MB`, `REGION_*`, `PROTECT_FAST`, `WRITE_FAST`, `BATCH_SPLIT`, `GUARD_DEDUP`, `UPLOAD_BUDGET_MB`) | pior quadro 534 → 58–75 ms, sem tela de carregamento forçada | ✅ |
| 18 | 29/10 | `SOTC_COPY_LOCK_ONCE=1` | cópias lentas iguais | ➖ |
| 19 | 29/10 | etapa 1 do plano da thread (envio de texturas) | sem ganho | ➖ |
| 20 | 05/11 | `SOTC_CLAMP_GAP` | clamp 8–10 → 5–7 ms nos quadros lentos | ✅ |
| 21 | 05/11 | `SOTC_TEX_DIRECT` + `SOTC_PC_MEMORY_PREIMPORT` | floresta > 50 ms 14 → 9 | ✅ |
| 22 | 05/11 | `SOTC_RELEASE_DEFER=1` | liberação 68 → 1,6 ms nos quadros lentos | ✅ |
| 23 | 05/11 | `SOTC_IMAGE_DEDICATED=1` | VRAM acima do limite 18/99 janelas → 0 | ✅ |
| 24 | 17/11 | `SOTC_TEX_GATHER=1` | entrada de área 7–9 quadros > 50 ms → 1 | ✅ |
| 25 | 17/11 | `SOTC_IDLE_FLUSH` | 46–55 FPS (igual) | ➖ |
| 26 | 18/11 | `SOTC_FLIP_QUEUE=2` | troca 41–47 (ref. 40–45) | ➖ |
| 27 | 18/11 | perfil da thread nos quadros lentos | ~16% em `Protect`/`VirtualProtect`, ~20% esperando o flip | 🔬 |
| 28 | 23/11 | `SOTC_DRAW_SPLIT` no pior quadro do mergulho | thread trabalha 16,6 de 55,4 ms ("draw waits gpu 0"): **não é o limite**. A espera de leitura (48–68 ms) é soma de várias threads e tem a mesma proporção do quadro a 60 FPS (0,84 × 0,87): **não é causa própria do mergulho** (revisão da parceira) | 🔬 |

**Nunca testado, com pista medida:** juntar as chamadas de proteção de páginas (#27).

### Hipótese H3 — o que o port põe em cima do jogo (configuração, recursos)

| # | Data | Mudança (chave) | Resultado medido | Veredito |
|---|---|---|---|---|
| 29 | 17/11 | `SOTC_COMPUTE_PREFETCH` (padrão) | passeio frio > 50 ms 94 → 67; não mexe no mergulho | ✅ |
| 30 | 17/11 | lançador mantém o cache de shaders (`SHADER_CACHE_VERSION`) | sem retradução a cada atualização | ✅ |
| 31 | 18/11 | tirar as medições do lançador | resto 54–57 → 59–60; troca 40–47 → 49–55 | ✅ |
| 32 | 18/11 | sem modo PS4 Pro (só para medir) | troca 40,5 (igual) | ➖ |
| 33 | 19/11 | FPS dinâmico desligado | sem travar em 48; mergulho ~2 s igual | 🔬 |
| 34 | 19/11 | FPS dinâmico ajustado (5 s / 4 s) | nenhuma trava de 10–15 s em 48 | ✅ |
| 35 | 23/11 10:14 | `SOTC_PREFETCH=0` (leitura antecipada desligada) | após a carga: mínimo 30 → 43, pior 217–320 → **102 ms**; floresta 37 · 37 · 43 | ✅ |
| 41 | 23/11 10:47 | `SOTC_WATCH_PROGRAMS=0xa446aeb5,0x57bd9f27` (exe d89ddb1a, só essa medição) | 2ª partida (10:48): `0xa446aeb5` 3.772 dispatches (média 0,38 ms, máx 6,71), `0x57bd9f27` 8.387 (média 0,50, máx 4,60). O custo é a varredura de DMA: cada dispatch caro copia **10–33 MB** de memória do jogo (2–7 ms). Dispatches ≥ 2 ms: **22 no mergulho (82–83 s)**, mas **14 aos 13 s sem queda nenhuma (60 FPS)**. O de 6,71 ms caiu num quadro com o processador de comandos esperando só 7,3 ms (não é pico). Mergulho 40 · 37 · 39 · 46. Critério (> 5 ms junto de um pico de espera) ✗ | ➖ **as cópias não são a causa**. Parceira: a janela dos 13 s **não vale** (670 comandos/quadro, ainda carga; tela 53); o relógio do WATCH está ~2 s atrás do SCREEN. O argumento certo: as cópias se concentram no início do mergulho (~104 ms somados num segundo) e caem para ~0,4 ms/quadro no segundo seguinte, enquanto a tela segue em 37–46 por mais ~3 s: **o mergulho dura mais que as cópias**. Correção: `0xa446aeb5` roda todo quadro depois da troca (~0,37 ms), não é exclusivo do mergulho. Linha encerrada. |
| 39 | 23/11 10:42 | partida limpa (variáveis extras vazias; exe fa5edfc3) | após a carga 40 · 45 · 58 · 51 · 47 · 49, um quadro de 145 ms; resto 58–60; **floresta 40 · 43 · 50 · 49 (~2–3 s abaixo de 50), 1 quadro > 50 ms (65,8: dma 8,5, processador de comandos esperando o jogo 43,9 ms)** — contra #36 43 · 37 · 38 · 45, pior 55. Critérios: ≥ 48 ✗, ≤ 1 s abaixo de 50 ✗, pior < 34 ✗, resto ≥ 58 ✓ | ➖ (dentro da variação: menor da floresta hoje 40/37/37/37/40). Parceira: confere; **as medições não explicam o mergulho**; o quadro de 145 ms após a carga é buffer 36,8 + alloc 35,6 + readback 24 (390 MB copiados) |
| 36 | 23/11 10:20 | leitura antecipada só em HD (exe fa5edfc3, commit a0328659) | **mesma configuração do #35 neste PC** (log: "prefetch: off, SSD"); confirma: após a carga mínimo 43, um quadro de 95 ms; floresta 43 · 37 · 38 · 45, pior 55,4 ms | ✅ (confirmação) |

### Estabilidade (harness, menu de debug)

| # | Data | Mudança | Resultado | Veredito |
|---|---|---|---|---|
| 37 | 23/11 | final pelo harness, sem mudança (`base1`) | queda `0x1a2c0` aos 2:42 | o harness não serve para o final |
| 38 | 23/11 | final com VIS_DIRECT v3 (`order1`, `order2`) | queda `0x1a2c0` (igual ao base1) | inconclusivo |

### Quedas do jogo nas partidas do usuário (23/11)
| Partida | Exe / leitura antecipada | Queda |
|---|---|---|
| 09:33 | 0.77 b5f07a67, ligada | não |
| 10:13 | b5f07a67, `SOTC_PREFETCH=0` | **`0x14cf3f`** (escrita em 0x40), 18 s após a carga |
| 10:14, 10:20, 10:26, 10:42 | fa5edfc3 (desligada no SSD) | não |
| 10:47 | d89ddb1a (desligada no SSD + WATCH) | **`0x8c2b6`** (divisão por zero, 0xc0000094) aos 73 s, a 60 FPS |
| 10:48 | d89ddb1a | não |
2 quedas em 6 partidas com a leitura antecipada desligada, 0 em 1 com ela ligada. **Parceira: ruído** (com 1/3 de quedas, 1 partida limpa tem 67% de chance); quedas de tipos diferentes, ambas já jogando (46,8 s e 73,6 s, tela 58–60), nenhuma em carga nem com leitura de arquivo pendente; pilhas só em comum no laço genérico da thread (`0x39a73`/`0x38ad2`/`0x33586`). Para virar sinal: ~5 partidas com `SOTC_PREFETCH=1` sem queda.

### Hipótese H4 — o trabalho do próprio jogo na troca de área é em série e depende da velocidade de um núcleo (proposta pela parceira, 23/11)
O 13900K tem núcleos P (lógicos 0–15) e E (16–31, ~metade da velocidade). Sinais: 16+ mudanças do port não mexem no mergulho; thread de desenho e GPU não cheias; as threads do jogo esperam umas às outras em série (perfil 18/11); quadro de 65,8 ms com 43,9 ms do processador de comandos esperando o jogo.

| # | Data | Mudança | Resultado | Veredito |
|---|---|---|---|---|
| 43 ⏳ | 23/11 | jogo preso aos núcleos P (afinidade 0xFFFF do processo `bb-probe`), nada mais | aprovação: **2 partidas** com menor `SCREEN FPS` na floresta ≥ 45, ≤ 1 s abaixo de 50, resto ≥ 58. Se ➖: sobra o trabalho do jogo na troca de área, que o port não corta | **não aplicado** (10:53): o vigia externo levou "Acesso negado" (meu shell roda num contêiner sem acesso ao processo do jogo). Precisa ser feito pelo próprio exe (`SetProcessAffinityMask` no início, atrás de chave) |
| 44 | 23/11 10:53 | partida limpa (exe d89ddb1a sem chaves), **continuando a jogar na floresta sem pausar** | troca de área aos 85 s e depois **44–50 FPS sem parar até o fim (98 s)**, 5.800–8.400 comandos/quadro; só 1 quadro > 50 ms (52); resto 58–60 | 🔬 **descoberta: não é um mergulho curto, é o trecho denso da floresta a ~47 FPS contínuo** (o mesmo "trechos densos 44–48" de 17/11: GPU ocupada 71–76%, jogo esperando a GPU dentro do quadro). O "volta a 60" das outras partidas era a pausa |

## Reavaliação depois da descoberta da pausa (parceira, 23/11 11:00)
O problema é o **trecho denso da floresta preso em ~48 FPS** (janelas 5,8–8,4 mil comandos: 48,8 · 47,8 · 50,5 · 52,2;
mediana 19–21 ms; leituras 1.400–1.500/s contra 850–1.000 a 60). Conta: GPU ~15 ms ocupada + 5–7 ms parada esperando a
ida e volta do mapa de visibilidade ≈ 20 ms. Para 16,7 ms: tirar a parada **e** caber na GPU.
- **"H1 esgotada" caiu**: foi julgada pelo mínimo dos 2–4 s após a troca, com a pausa no fim.
- **#16 (VIS_DIRECT v3) a rejulgar**: cortou ~9 ms/quadro de espera (14,5 → 5,5), exatamente a parada da GPU, mas pôs +5 ms no processador de comandos; o trecho denso nunca foi medido com ele.
- #33/#34: o "→ 60" também era pausa (a correção da trava em 48 vale; a duração do mergulho não).
- #35/#36/#39 falam só da carga do save e do início da troca.
- De pé: #10–#15 (julgados pela média de 5 s com 6 mil+ comandos), #31, #32.
**Métrica daqui em diante:** média das janelas `SOTC FPS` com ≥ 6 mil comandos/quadro e mediana do quadro (`SOTC FRAMES` p50), jogando ≥ 20 s seguidos depois da troca, sem pausar.

| # | Mudança | Aprovação | Estado |
|---|---|---|---|
| 45 ✔ 10:57 | **resultado:** sem queda, 0 rótulos pendurados; resto 58–60 ✓ (o #16 tinha caído a 50–55: agora não); troca de área aos 82 s 38 · 38 · 45, depois a tela fica **48 cravado** (48·48·48·48·48·49·50·48) = o FPS dinâmico baixou para 48, então o teto ficou em 48. Janelas ≥ 6 mil comandos: 48,7 · 51,3 (média 50,0; ref. #44 47,8 · 50,5 = 49,2). Critério ≥ 53 ✗ | ➖. Parceira: **a leitura do "48 cravado" estava errada**: a janela dos 90 s (que cobre os 85–90 s "cravados") teve 257 quadros = 51,3 FPS, acima do teto de 48; a tela mostra 48 porque é média de ~20 quadros de ~20 ms; o log não tem linha do FPS dinâmico baixando. Janelas ≥ 6 mil: 48,7 · 51,3 · 50,4 (média 50,1; #44 49,8), mediana 19–20 ms (#44 19–21): **igual**. Conclusão: cortar ~9 ms/quadro de espera não subiu o FPS denso → **no trecho denso a espera do mapa de visibilidade não é o limite** (a parada muda de lugar). O v3 não pesa mais no resto, mas não rende: desligado |
| 46 | partida limpa com `fps_dynamic: false` | média ≥ 53 nas janelas ≥ 6 mil → o FPS dinâmico mascarava (refazer #45); ≤ 51 → sem trava, assunto fechado | proposto (parceira) |
| 47 ✔ 11:01 | **resultado** (amostra de 23–28 quadros a cada 5 s). Caminho a 60 FPS (4–5 mil comandos): GPU span 16,3–17,2 ms = ops 8,0–8,5 + entre ops no mesmo envio 2,6–3,6 + **parada entre envios 4,7–6,1** (15–19 envios). **Floresta densa (6,3–9 mil comandos, 50–52 FPS):** span 17,2–19,4 = ops 9,5–10,6 (indiretos 2 → **4,2–4,6 ms**, 4,9–6,9 mil por quadro) + entre ops 3,3–3,7 + **parada entre envios 4,4–5,6**. Trabalho total 13–14,3 ms; a parada fica igual à do trecho a 60; o que cresce no denso são os **desenhos indiretos (+2,3 ms)** | 🔬 Parceira: a parada a 60 FPS **inclui a folga do vsync** (tela de título: 2,8 ms de ops e 9,5 de "parada"), então não compara; no denso não há vsync a esperar e os 4,4–5,6 ms são parada real. Trabalho 13,6–14,3 (fronteira, pende para envio). **Os dois definem o quadro:** o trabalho subiu ~2,8 ms (11,4 → 14,3, dos quais 2,3 são indiretos) e a parada ficou ~5 ms; maiores pedaços da parada: antes do `0x50784801` (visibilidade) 1,6–2,4 ms/quadro, antes do `0x79704cba` 1,2–1,5. Como o #45 não tirou a parada antes do `0x50784801`, o alvo mais barato são os indiretos |
| 48 | juntar desenhos indiretos seguidos (mesmo pipeline/estado num `vkCmdDrawIndirect` com count > 1 ou multi-draw); 4,9–6,9 mil indiretos/quadro (~0,65 µs cada); `0x190ff49d` sozinho 670–990/quadro, 0,4–1,2 ms | A/B com as medições do #47: indiretos no denso 4,3–4,6 → ≤ 2,5 ms/quadro, ops + entre ≤ 12,5 ms, média ≥ 6 mil comandos +2 sobre o #47 (50,3–52,0), sem erro de imagem; depois partida limpa com média ≥ 54 | proposto (parceira); **viabilidade a confirmar no código** (desenhos seguidos precisam ter o mesmo estado/descritores) |
| 48a ⏳ | **contagem de viabilidade** (`SOTC_INDIRECT_MERGE_STATS=1`, linha `SOTC INDIRECT MERGE` a cada 5 s). Código (23/11): os indiretos com argumentos da CPU **já viram desenhos diretos** (`SOTC_INDIRECT_DIRECT`); os 5–7 mil que sobram têm argumentos escritos pela GPU (culling). Juntar exige: mesmo pipeline e command buffer, `fast_bind_hit` (mesmos descritores e dados de usuário), mesmo estado de renderização, sem barreira entre eles, argumentos logo depois dos anteriores no mesmo buffer | seguir para o #48 só se ≥ 50% dos indiretos do denso puderem juntar-se ao anterior; se a razão principal for "bindings" (dados por objeto), o #48 não é viável assim | **11:09: 0 de ~60 mil a cada 5 s podem se juntar** (maior sequência 0). No denso: ~55% trocam de pipeline em relação ao anterior (shaders intercalados) e ~45% têm o mesmo pipeline com descritores/dados de objeto diferentes; estado e argumentos nunca foram o problema. Os indiretos com argumentos da GPU são só ~240/quadro no denso (~12 mil/s); os 5–7 mil "draw-indirect" do `SOTC_GPU_TIMES` são em maioria pacotes indiretos com argumentos da CPU, que já viram desenhos diretos (e têm o mesmo intercalamento). **➖ #48 fechado**: o jogo manda um desenho por objeto, com dados próprios; o número de desenhos é o do jogo. Parceira: concorda; os 4,3–4,6 ms de "draw-indirect" são sombreamento real, não custo por chamada |
| 49 | tirar os envios de textura e as cópias de imagem do meio do quadro (fila de transferência ou antes do 1º desenho). No denso, o "entre ops" de 3,3–3,7 ms = barreira+upload+bufcopy 0,9–1,2, barreira+texupload+imagecopy 0,65–1,14 (0,34 a 60 FPS), 230–350 barreiras soltas 0,4–0,66 | com as medições do #47, 20 s+ após a troca: "entre ops" no denso ≤ 2,5 ms, média ≥ 6 mil comandos +1,5 FPS sobre o #47, sem textura errada/piscando | **código escrito 23/11 (não compilado/testado ao fim da sessão)**: `SOTC_UPLOAD_PREPASS=1` — envio de textura nunca usada (estado Undefined) com bytes já no host (staging da CPU ou memória do jogo direta; sem gather nem detile na GPU) vai para o `UploadCommandBuffer` (antes do primário), sem `EndRendering`; linha `SOTC UPLOAD PREPASS`. Arquivos: vk_runtime.cpp/.h, buffer_cache.cpp/.h, texture_cache.cpp, sotc_stats.h, driver.cpp. Testar com `SOTC_UPLOAD_PREPASS=1 SOTC_GPU_TIMES=1 SOTC_GPU_TIMES_EVERY=10` |
| 49 ✔ 15:41 | exe ef3d6054, `SOTC_UPLOAD_PREPASS=1` + medições do #47 | **não agiu: 0 envios antecipados**; a cada 5 s ~700–740 envios de imagem **já usada** (recargas de texturas existentes, não imagens novas) e 190–350 com origem fora do host (gather/detile/arena da GPU). Floresta densa (6–8,5 mil comandos): 50,5 · 53,1 · 52,9 (média 52,2; #47 51,1: ruído); "entre ops" 3,2–3,5 ms (igual); tela 40 · 40 · 45 · 46 depois 50–53 | ➖ (a condição "imagem nunca usada" não acontece no jogo). Parceira: **v2 não vale**: as ~700 recargas/5 s acontecem igual a 60 FPS (~2,7 por quadro, custo de sempre); máximo teórico 1,2–1,5 ms (→ ~55 FPS) e real 0,3–0,6 ms (+1–1,5 FPS, dentro do ruído 50,5–53,1), com risco de textura velha |

### Consolidação (23/11 15:50)
**Teto do trecho denso da floresta: ~52–54 FPS** (o jogo faz 17–18 envios em série com ida e volta CPU→GPU→CPU, mais ~14 ms
de GPU). Ganhos que ficam: leitura antecipada só em HD (travadas após a carga), FPS dinâmico ajustado, cache de shaders,
pré-tradução de compute, medições fora do lançador. Ficam no código, desligados, como ferramentas: `SOTC_VIS_DIRECT_ORDER`,
`SOTC_WATCH_PROGRAMS`, `SOTC_INDIRECT_MERGE_STATS`, `SOTC_UPLOAD_PREPASS`. Linhas fechadas: H1 (16+#45), cópias (#41),
junção de desenhos (#48), envio antecipado de texturas (#49).

### Conclusão da parceira (23/11 11:15) — o teto
Não há caminho barato para 60 FPS no trecho denso da floresta: a GPU trabalha ~14,3 ms e fica ~5 ms parada pela cadeia do
próprio jogo (17–18 envios por quadro; o maior pedaço antes do cálculo de visibilidade). Envio tardio (#5), IDLE_FLUSH
(#25), FLIP_QUEUE (#26) e H1 mostraram que o port não adianta essa cadeia. O #49 é o último ganho barato visto: na
melhor hipótese ~19,6 → ~18,5 ms (**53–54 FPS**). Passar disso exige quebrar a dependência do jogo em ler o resultado
da GPU no mesmo quadro, o que a H1 mostrou não ser possível sem errar a imagem. **Teto real = o jogo + o custo de
emular a GPU.**
| 47 | GPU no trecho denso: `SOTC_GPU_TIMES=1 SOTC_GPU_TIMES_EVERY=10`, nada mais, 20 s+ após a troca | trabalho da GPU ≥ 17 ms/quadro → o limite é a GPU (alvo: custo de emulação, barreiras/cópias 3–4 ms); ≤ 14 ms com parada ≥ 5 ms → o limite é o envio do port | proposto (parceira) |
| 45 | `SOTC_VIS_DIRECT=1 SOTC_VIS_DIRECT_ORDER=1`, sem medições, julgado no trecho denso | média ≥ 53 nas janelas ≥ 6 mil comandos (ref. 47,8–52,2), mediana ≤ 18 ms, 0 quadros > 50 ms após a troca, resto ≥ 57 (se cair a 50–55 como em #16: ❌). Risco: estabilidade no final do jogo não provada | proposto (parceira) |
| 43b | núcleos P pelo próprio exe (`SOTC_PCORES=1`) | média ≥ 51 nas janelas ≥ 6 mil comandos (ref. ~49,8), mediana ≤ 18,5 ms | segunda opção (ganho esperado pequeno) |

## Estado agora (23/11, 10:45)
- E:: exe `fa5edfc3`; lançador com variáveis extras vazias (estado do jogador comum).
- Sobra o **mergulho da floresta**: 40–50 FPS por ~2–3 s, 1 quadro de 66 ms (sem medições, #39); e um quadro de ~100–145 ms depois da carga do save.

## Próximos testes (propostos, ainda não rodados)
| # | Mudança | Hipótese | Aprovação | Parceira |
|---|---|---|---|---|
| ~~39~~ (rodado, ver H3) | partida limpa: só o padrão do lançador, sem medições nem VIS_DIRECT | H3: as medições pesam no mergulho também (é o #31 no exe novo: ponto de partida) | menor `SCREEN FPS` na floresta ≥ 48, no máximo 1 s abaixo de 50, pior quadro < 34 ms, 0 quadros > 50 ms, resto ≥ 58 (a floresta variou 37–40 entre partidas iguais) | conferir antes que `SCREEN FPS`/`FRAMES` saem no log sem as medições |
| 40 | juntar as chamadas de proteção de páginas | H2 #27 | trabalho real da thread no mergulho (hoje 10,5 ms/quadro na janela) −2 ms **e** menor `SCREEN FPS` ≥ 42, com as mesmas medições da partida das 10:20 | provável ➖: ~2,7 ms numa thread que não está no caminho crítico |
| 41 ⏳ | medir as cópias dos shaders que só aparecem no mergulho (`SOTC_WATCH_PROGRAMS=0xa446aeb5,0x57bd9f27`, linha `SOTC WATCH`; sem outras medições) | novo: `0xa446aeb5` aparece só no mergulho nas 4 partidas (custo quase todo "dma" 5,4 ms; pior quadro dma 8,0 contra 1,3 de média); `0x57bd9f27` 3–6 ms num dispatch | bytes, faltas de página e proteção dessas cópias por quadro | **escolhido pela parceira (23/11 10:50)**: medição mínima = uma linha de log só nos dispatches `0xa446aeb5`/`0x57bd9f27` (ms de dma, bytes, faltas de página, se o jogo leu de volta no mesmo quadro), sem DRAW_SPLIT/SLOW_OPS/GPU_TIMES. Seguir só se a cópia passar de ~5 ms no mesmo quadro de um pico de idle do processador de comandos; senão parar a linha |
| 42 | tempos de GPU só no mergulho (disparados abaixo de 50 FPS) | ver se a GPU fica parada enquanto a thread faz essas cópias (aí a cópia atrasa o envio e estica a ida e volta) | GPU parada × cópia no mesmo quadro | parceira: **repete em boa parte o 18/11 22:28** (GPU ~13 ms de trabalho, 5–7,5 parada); só vale se pegar o pior quadro exato |

Nota da parceira: "recorder cpu" acompanha o tempo do quadro (16,5 a 60 FPS, 54,7 no pior quadro): não indica thread cheia.
