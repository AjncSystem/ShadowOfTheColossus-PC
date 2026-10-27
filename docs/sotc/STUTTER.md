# Stutters: o que foi medido (26/10, plus14, RTX 5060 Ti + Ryzen 7 5700X)

Método: rota do deserto largo (`tools/routes/deserto_wide.txt`, 196 s) com `SOTC_FPS=1`; `tools/frame_report.py`
dá os quadros acima de 50 ms e a causa de cada um. A rota diverge entre execuções: os números são indicativos.

| Situação | Quadros > 50 ms | Pior quadro |
|---|---|---|
| plus12, cache aquecido | 78 | 232 ms |
| plus14, cache aquecido (3 passadas) | 5, 3, 5 | 88–113 ms (sempre aos ~55 s, entrada numa área nova: cópia de ~200 MB) |
| plus14, cache de shaders **frio** | 12–18 | 571–651 ms (449–521 ms só de shader) |

## Causa do pior caso: shaders de compute traduzidos na thread de comandos
Com o cache frio, 386 shaders de compute foram traduzidos na `GpuCommandProcessor` (~913 ms no total, em rajadas
ao entrar numa área). Os shaders gráficos já são traduzidos em workers (`SOTC_ASYNC_TRANSLATE`); os de compute não.
O log `SOTC SYNC PIPELINE shader (compute) N ms` (limite em `SOTC_SYNC_PIPELINE_MS`) lista cada um.

## Experimento: `SOTC_ASYNC_COMPUTE_TRANSLATE=1` (desligado por padrão)
Traduz o compute num worker e descarta o dispatch até ficar pronto. Resultado no deserto frio: pior quadro de
571 para 188 ms e tempo de shader de 795 para 32 ms. **Mas** no Santuário com cache frio o céu e a névoa saíram
diferentes (sem a neblina da versão aquecida): algum compute que roda uma vez foi descartado e nunca refeito.
Por isso fica desligado. Para ligar com segurança seria preciso reexecutar o dispatch descartado depois, ou
esperar o worker só nos computes de execução única.

## Não encontrado
- Pipelines gráficos síncronos: só "tiny draw" (passes de tela cheia), 1–7 ms cada, um de 76 ms na primeira vez.
- Evicção de arenas e GC: 0 ms nas rotas medidas (8 GB de VRAM não chegam ao limite).
- Os 1% lows de ~39 FPS do deserto são cenas pesadas (> 5000 comandos por quadro), não engasgos.

## Medições de 27/10 (0.60.6-plus18/19, Santuário parado, Ryzen 7 5700X, RTX 5060 Ti)

- **Frame pacing:** Mailbox 70,5 e 71,7 FPS contra FIFO 69,4 e 71,7 (ordem M F F M); p50 16,2 ms e p99 19 a 26 ms nos dois. Sem diferença: Mailbox continua o padrão.
- **Escala por CPUs** (afinidade 0x55, 0x555, 0x5555, 0xffff, um núcleo por CPU): 4 CPUs 56 FPS, 6 CPUs 65, 8 CPUs 70, 16 CPUs 73. Com 4 CPUs eram 34 FPS antes da prioridade de threads.
- **`SOTC_RB_HOT`:** 0 = 53 FPS, 1 = 74 a 75,5, 2 = 72,5 a 74,1. O padrão 1 fica.
- **Perfil da `GpuDraw`:** plano. `BindResources` 17,5% (soma de vários trechos inlined), `BackLoop` 11,8% (espera), `ObtainBuffer` 7%, nenhum trecho único acima de 3%. Draw ocupada 11,5 ms de 13,5 ms por quadro (85%), comando 8,2 ms, gravação 5,8 ms.
- **Rota do deserto a 3840x1080 (196 s, cache quente):** 0 quadros acima de 50 ms, VRAM 3,4 de 6,2 GB com texturas Alta. Textura Máxima não foi medida.
- Cache frio e compilação assíncrona de compute: continua como na seção anterior (a versão assíncrona perdia despachos de uma vez só e fica desligada).

## Cache frio, segunda análise (27/10)
- Rota do deserto largo com o cache de `CUSA08809` apagado: 4 quadros acima de 50 ms; o pior, 493 ms aos 60 s, tem 368 ms de shader. Só uma tradução passou de 30 ms (a de `b48a3d66`, 30 ms); o resto da rajada é uma centena de computes de 2 a 5 ms, ao entrar numa área nova.
- A thread de comandos passa quase todo o tempo esperando: o perfil dela mostra `GetProgram` com 0,7%. O custo é concentrado em rajadas, não espalhado.
- Por que não mexi no código: a tradução assíncrona de compute descarta o dispatch até o shader ficar pronto, e uma rajada de entrada de área é feita justamente de computes de execução única (inicializações), que não voltam. Esperar o worker no lugar de descartar é o mesmo que traduzir na thread de comandos. Reexecutar o dispatch depois não é seguro: ele lê memória do jogo (constantes, SRT) que muda até lá.
- O único caminho sem risco que sobra é traduzir à frente: varrer o buffer de comandos antes de executá-lo, achar os pares `COMPUTE_PGM` + `DISPATCH` e traduzir todos em paralelo nos workers, esperando o resultado no dispatch. Exige reproduzir o estado dos registradores e dos dados de usuário que a tradução lê; fica como trabalho futuro.
- O cache só é frio na primeira abertura, depois de uma mudança do recompilador (a versão do cache subiu duas vezes em 10 dias) ou da placa/driver.

## Traduzir à frente (28/10, plus20)
`Liverpool::PrefetchComputePrograms` varre cada buffer de comandos (graphics e ASC) ao começar a executá-lo, segue os `IndirectBuffer`
(até 2 níveis, só endereços mapeados, no máximo 65536 dwords) e mantém uma cópia dos registradores de compute a partir dos `SetShReg`.
Em cada `Dispatch*` chama `PipelineCache::PrefetchComputeProgram`, que enfileira a tradução nos workers (até 512 pendentes). Em
`RefreshComputeKey`, se o programa está pendente, o dispatch espera o worker (no máximo 2 s) em vez de traduzir.
- Rota do deserto largo, cache apagado: pior quadro 493-618 ms (shader 360-470) para 174 ms (shader 6); computes compilados na thread de
  comandos 178 para 24; 5 quadros acima de 50 ms (antes 4-9, os restantes são cópia de ~250 MB e outros custos, não shader).
- Com o limite de 64 pendentes a rajada estourava (150 computes ficavam de fora); 512 cobre a rajada medida (586 programas).
- Santuário com cache frio e quente: mesmo céu e mesma névoa (luminância média 86,4 e 85,2).
- Um quadro de 346 ms de `readback` no run quente (rota do deserto, `RepairStuckWriteFault` no log) não tem relação: o cache quente
  não enfileira nada.
