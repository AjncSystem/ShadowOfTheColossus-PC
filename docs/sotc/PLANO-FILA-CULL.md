# Plano: visibilidade (cull) numa segunda fila da GPU (17/11/2026)

Etapa 5 de `PLANO-TROCA-DE-AREA.md`, agora com a prova que ela pedia. Objetivo: os trechos densos do
templo (44–48 FPS) e as trocas de área, onde o jogo espera o mapa de visibilidade que a GPU calcula no
mesmo quadro. Regra do usuário: sem perda de imagem nem de FPS; tudo atrás de `SOTC_CULL_QUEUE` (0 = como
hoje).

## A prova (partida do usuário, 22:46, `SOTC_GPU_TIMES=1`)

- Por quadro de ~21 ms de GPU nos trechos a 45–48 FPS: ~10 ms de desenhos/dispatches, ~3–4 ms de
  barreiras/cópias, ~6–9 ms de GPU parada entre envios (o jogo esperando a visibilidade).
- Esperas de leitura do jogo: 20–27 ms por quadro, quase tudo "gpu" (a GPU terminando o que está na frente;
  fila do port ~1 ms, envio 0,3 ms).
- **Cada dispatch do `0x50784801` (30–86 por quadro) começa ~10 ms de GPU depois de suas entradas estarem
  prontas** (tempo entre a última escrita da GPU nas páginas que ele lê e o início dele); o maior de cada
  quadro 14–24 ms. É tempo de fila atrás de trabalho sem relação.
- `SOTC_IDLE_FLUSH` (enviar quando a thread de desenho fica sem trabalho) não mudou nada: a GPU para porque
  o jogo ainda não mandou o trabalho.

## Recursos dos programas de cull (`SOTC CULL RESOURCES`)

| Programa | Buffers (escritos) | Imagens | Observação |
|---|---|---|---|
| `0x50784801` | 4 (1) | 0 | o que mais espera (~10 ms por dispatch); só buffers: **primeiro alvo** |
| `0xc54081d1` | 4 (1) | 0 | ~200/quadro, folga menor |
| `0x4defcf7f` | 7 (2) | 0 | sombra |
| `0x2e0f474b`, `0x93aec24b`, `0xf94d23d4`, `0x729c541e` | 2–9 | 1 lida | Hi-Z: estado de layout de imagem entre filas; depois |

Leitores do resultado na GPU: muitos programas, alguns a 1–4 operações (`0x9404c951` dispatch-indirect a 1).
A marcação é por página de 64 KiB (o jogo junta outros dados nas mesmas páginas): parte é falso positivo,
mas há leitores reais. Então a fila gráfica precisa esperar a segunda fila antes do primeiro leitor.

## Desenho (MVP: só `0x50784801`, só buffers)

1. **Fila**: pedir uma segunda fila da família gráfica (NVIDIA: 16 na família 0; mesma família = sem
   transferência de posse). `SOTC_CULL_QUEUE_PRIORITY` (prioridade da fila maior que a principal).
2. **Agendador próprio** (`CullScheduler`): pool, buffers de comando e semáforo de linha do tempo próprios,
   gravado direto na thread de desenho (sem a thread de gravação).
3. **Dependência de entrada**: cada página de 64 KiB escrita pela GPU guarda o tick gráfico de quem
   escreveu. O dispatch de cull espera (semáforo) o tick gráfico mais novo entre o que ele lê; se for o
   buffer gráfico ainda não enviado, envia o gráfico antes (custo de um envio).
4. **Envios de dados da CPU** (`SynchronizeMemory`) dos buffers do cull vão para o buffer de comando da fila 2
   (redirecionamento no `Scheduler::CommandBuffer()` enquanto grava o bind e o dispatch); barreiras idem.
5. **Saída**: as faixas escritas guardam "escrito na fila 2, tick T2". Um bind gráfico que lê essas faixas
   (faixa exata, não página) envia o gráfico até ali e o próximo envio gráfico espera T2.
6. **Leituras do jogo** (readback) de faixa escrita na fila 2: a cópia vai na fila 2 e o jogo espera T2
   (não o gráfico). Mesmo caminho de aplicação com a guarda do heap.
7. **Conflito de escrita** (a fila 2 escrever onde um trabalho gráfico ainda pendente lê): antes de
   redirecionar, conferir que nenhum op gráfico depois do tick de dependência leu/escreveu as faixas que o
   cull escreve; se houver, cai no caminho normal.
8. **Rede de segurança**: qualquer caso não coberto (imagens, GDS, faixas em conflito) usa o caminho de hoje.

## Medição e aprovação

- `SOTC_GPU_TIMES`: folga dos culls cai para perto de 0; GPU parada entre envios cai.
- FPS nos trechos do templo (44–48 hoje) e entradas de área (`tools/entry_report.py`).
- Imagem: capturas iguais (grama, cavalo, capa, escadas: os sintomas de máscara errada do fork HiddenUserHere).
- `SOTC_CULL_QUEUE_VERIFY=1`: roda o cull nas duas filas e compara a máscara byte a byte (0 diferenças).
- Estabilidade: rota do usuário + final do jogo até os créditos sem queda antes de virar padrão.

## Riscos

- Ordem de memória entre filas (a lição do `SOTC_VIS_DIRECT`: rótulos de fim de pipe saindo cedo deixaram
  o jogo reaproveitar memória que a GPU ainda escrevia). Aqui os rótulos não mudam; a fila 2 só adianta.
- Rastreamento de barreiras do `Runtime` assume uma fila: os comandos da fila 2 começam depois do semáforo
  (tudo antes visível); os gráficos gravados depois da dependência e antes do cull rodam em paralelo — o
  passo 7 evita que toquem as mesmas faixas.
- Custo: um envio a mais por lote de cull, semáforos por envio.
