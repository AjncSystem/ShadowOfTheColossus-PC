# Modelo de memória do PC (opção C) — plano por etapas

Pedido do usuário em 27/10/2026, depois de medir as quedas ao chegar em áreas novas (DMA 83–127 ms,
envio 70–106 ms, espera de leituras da GPU 47–59 ms por janela de 5 s; VRAM 6,4–7,1 GB de 8 GB;
thread de desenho 27–36 ms por quadro a 30 FPS, modo Pro + Máxima + DLSS).

## A ideia

Hoje a memória do jogo e a da GPU são separadas: o que o jogo escreve é copiado para a VRAM
(sincronização DMA, envios), e o que a GPU escreve e o jogo lê volta por cópia (leituras da GPU,
com proteção de páginas e espera). No "modelo de memória do PC" a GPU usa a memória do jogo
**onde ela está** (RAM importada com `VK_EXT_external_memory_host`), e só o que é lido sempre
fica espelhado na VRAM.

Referência: port do Bloodborne (deadinside28, 0.5–0.6, `docs/NVIDIA_MEMORY_MODEL.ru.md` em
`ref/deadinside28_bloodborne_pc`). Lá o modelo virou padrão em todas as placas na 0.6 (19/10),
mas eles reescreveram o núcleo de GPU inteiro. O nosso usa o núcleo do shadPS4: a ideia é
portada, o código não. Lições deles que valem aqui:

- a cópia na VRAM **divergir** da memória do jogo deu explosão de vértices e texturas com lixo;
  toda escrita da CPU que não passa pelo port precisa ser vista (armadilhas de escrita);
- a GPU só deve escrever na VRAM em blocos que a CPU não tocou desde a alocação;
- importar memória do host fixa páginas na RAM: importar por pedaços, sob demanda, não 9 GB.

O protótipo `tools/host_import_test.c` (08/10) mostrou na RTX 2060 SUPER: import de 256 MiB a
5 GiB funciona, coerente nos dois sentidos, GPU lê a 5,5 GB/s.

## Etapas (cada uma medida antes da próxima; tudo atrás de `SOTC_PC_MEMORY`, desligado por padrão)

| # | Etapa | Entrega | Medição | Risco |
|---|---|---|---|---|
| 1 | Importar a memória do jogo na GPU | extensão ativada; pedaços de 64 MiB da visão `backing` importados sob demanda como `VkBuffer`; teste de coerência na inicialização | log: importou, tempo, tipo de memória; nada muda no jogo | baixo |
| 2 | Leituras da GPU direto na memória do jogo | `BeginAsyncReadback` copia da VRAM para o pedaço importado, sem área temporária nem `memcpy` | ms/s de leituras, FPS no demo e no save do usuário | baixo |
| 3 | Rótulos de fim de trabalho "honestos" | o rótulo (EOP) que o jogo espera passa a ser escrito quando a GPU termina, só para os casos medidos | FPS, nenhum travamento | médio (já tentado em 12/10 como `SOTC_EOP_DEFER`: FPS caiu à metade; precisa ser seletivo) |
| 4 | Regiões lidas todo quadro escritas no lugar | o culling por visibilidade (bitmask do XPR, ~0x1078c10000) e outras "leituras quentes" passam a ser escritas pela GPU direto na RAM importada: sem proteção, sem leitura de volta | esperas de leitura → ~0; FPS na ponte e no final do demo | médio/alto |
| 5 | Dados que a CPU escreve lidos no lugar | constantes e vértices dinâmicos lidos da RAM importada em vez de sincronizados por DMA; espelho na VRAM só do que é lido sempre | custo de DMA (hoje ~47 ms/s), quedas ao trocar de área | alto |

**Restrição achada na etapa 1 (27/10):** as leituras de volta aplicam os dados pela
`ApplyDownloads`, que contém a guarda dos heaps do jogo (`BpeHeapGuard`, #146, a queda 0x49500 do
final): bytes velhos da GPU não podem cair em blocos que o jogo já liberou. Uma cópia da GPU direto
na memória do jogo pula essa guarda. Por isso a etapa 2 escreve direto **só fora dos heaps
guardados**; dentro deles continua o caminho atual. O mesmo vale para as etapas 4 e 5.

Critérios de corte: imagem igual nas capturas do demo, nenhum bloco verde (templo do 6º colosso),
final do jogo sem queda (POOL_FIX, guardas de heap e regra do pool intocados).

## Estado

- 27/10: plano escrito.
- 27/10: **etapa 1 pronta** (`SOTC_PC_MEMORY=1`, desligada por padrão). `VK_EXT_external_memory_host`
  pedida só com a chave; `renderer_vulkan/sotc_guest_memory.*` importa a visão `backing`
  (`runtime_memory_backing_view`) em pedaços de 64 MiB sob demanda. A visão só existe depois que o
  jogo mapeia memória, então ela é procurada a cada envio (`Rasterizer::OnSubmit`) até achar. Na
  RTX 2060 SUPER: `10176 MiB of game memory, 159 chunks ... chunk 0 in 24.2 ms` (tipo de memória
  3), demo a 60 FPS, sem erros. A lista de extensões do dispositivo passou de 32 para 48 lugares
  (33 pedidas).
- 27/10: **etapa 2 descartada** (pedido do usuário: o mais seguro, protegendo o final). A guarda
  dos heaps decide na hora de aplicar quais blocos o jogo acabou de liberar (heaps do mundo crescem
  em pedaços de 1 MiB, área refeita a cada 250 ms–2 s); uma cópia da GPU direto na memória cai fora
  desse controle. Ganho também pequeno (~3–5% da thread de desenho; a espera continua).
- 27/10, medição (`SOTC_RB_STATS=1`, save do usuário, gameplay parado, 60 FPS, Pro + DLSS):
  leituras de volta por instrução do jogo em ~3 min: `eboot+0x1a6d60` (culling por visibilidade)
  ~50 mil em `0x109f83…–0x109f8a…`; `eboot+0x101a98` ~22 mil espalhadas em `0x13…`;
  `eboot+0x937fd` ~2,6 mil em `0x1245620000`. O buffer do culling **não é fixo** (14/10 estava em
  `0x1078c1…`): é alocado no **pool de páginas de 64 KiB** (`0x10…`–`0x123C…`, o do `POOL_FIX`),
  não num heap (o heap guardado começa em `0x123c000020`, 150 MB). O pool libera e reaproveita
  páginas: escrita direta só é segura com o aviso de fim de trabalho honesto.
- **Nova ordem:** (a) EOP honesto só para o trabalho do culling (risco: lentidão, nenhum dado muda
  de lugar); (b) depois, escrita direta só nas páginas que o culling usa, conferindo a cada quadro
  que continuam dele. Tudo desligado por padrão; final do jogo testado pelo usuário antes de virar
  padrão.
