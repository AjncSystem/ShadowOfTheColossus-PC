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
