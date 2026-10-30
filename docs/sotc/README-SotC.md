# Shadow of the Colossus — PC (port nativo)

Port nativo do **Shadow of the Colossus** de PS4 (CUSA08809, versão EU 1.01, ou CUSA08034,
versão US 1.00) para Windows.
O código do jogo roda direto na CPU. Isto não é um emulador completo: há um carregador e as
funções de sistema de que o jogo precisa. A GPU usa o núcleo de vídeo Vulkan do fork
shadPS4 SotC. A base é o [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) (GPL-2.0).

Este pacote **não contém o jogo nem firmware da Sony**. Você precisa ter os seus.

## Requisitos

- Windows 10/11 64-bit e GPU com Vulkan 1.3.
- Python não é necessário para o `SotC.exe`; só o `SotC.bat` precisa de
  [Python 3](https://www.python.org/downloads/).
- O **seu dump** do jogo CUSA08809 ou CUSA08034 (pasta com `eboot.bin`, `sce_module`, `sce_sys`...).
- Para ter som, `libSceNgs2.sprx` e `libSceUlt.sprx` do **seu próprio console**, decriptados.
  São os mesmos arquivos da pasta `user/sys_modules` do shadPS4; num PS4 com GoldHEN ficam em
  `/system/common/lib/` (FTP na porta 2121). Sem eles o jogo funciona, mas sem som.

## Como jogar

1. Abra `SotC.exe`, o lançador.
2. Na primeira vez ele **procura sozinho** nos discos do PC:
   - a pasta do jogo (CUSA08809 ou CUSA08034);
   - os dois `.sprx` de som de um shadPS4 instalado (pasta `user/sys_modules`), que ele copia
     para a pasta de dados do port;
   - um save do jogo no shadPS4, que ele oferece importar;
   - as DLCs do jogo (pasta `addcont` do shadPS4 ou uma pasta com uma subpasta por DLC).
3. Confira em **Jogo e saves**. A **biblioteca de jogos** funciona como no shadPS4: adicione as
   pastas onde ficam os seus jogos e escolha o jogo na lista (duplo clique). **Procurar no PC**
   repete a busca automática.
4. Clique em **JOGAR**.

A tela **Jogar** confere tudo antes: a versão do jogo, os módulos de som, os saves e a placa de vídeo.
Na primeira vez a imagem do jogo é preparada, o que leva alguns segundos. Isso só se repete
quando o jogo ou os módulos mudam. Os saves e o cache de shaders ficam em `user/`.

Sem o lançador, `SotC.bat` faz o mesmo lendo `sotc.ini`. `SotC.exe --play` abre o jogo direto
com as últimas opções do lançador, o que serve para atalhos e para o Steam.

## Imagem e desempenho

Tudo isto está no lançador, na página **Imagem e FPS** (e os principais em **Jogar**):

- **Saída** 720p, 1080p, 1440p ou 4K. O jogo renderiza em 1080p, como no PS4, e a imagem é
  ampliada para a saída com **FSR 1** + nitidez (RCAS) ou bilinear. A janela nunca passa de 90%
  da tela; a tela cheia usa o monitor inteiro.
- **60 FPS** (experimental): tira o limite de 30 FPS do jogo (patch GoldHEN de illusion), com
  vblank a 120 Hz. Mediu 38 FPS no campo aberto e ~25 no templo.
- **Reutilização de binds** (desligada por padrão desde a 0.60.6; `SOTC_FAST_BIND=1` liga): pula
  a remontagem dos descritores quando o desenho seguinte usa o mesmo pipeline, recursos e
  uniforms. Medido no Santuário: ~25 → 30 FPS. Na 0.60.5 deixava pontos pretos em grade no chão de
  areia; a correção aguarda teste.
- **Texturas nítidas**: filtro anisotrópico 16x, quase sem custo.
- **Modo PS4 Pro**: o jogo se vê num PS4 Pro e oferece no próprio menu (Opções > Apresentação)
  o Modo Grafismo **Cinematográfico** (1440p, mais detalhe) ou **Desempenho**. É mais pesado:
  ~25 FPS no campo aberto.
- **Efeitos**: vinheta, névoa atmosférica e oclusão ambiente (SSAO) podem ser desligados.
  O motion blur, o brilho e os filtros ficam no menu do próprio jogo.
- **Cvars do motor** (Avançado): variáveis do motor Bluepoint, `NOME=valor`, para quem quer
  experimentar.

Os modos de patch valem para a versão EU 1.01 e para a US 1.00 (o executável é o mesmo); em
outra versão eles não são aplicados. Cada versão tem os seus saves (`user/savedata/1/CUSA08809`
ou `CUSA08034`) e só usa DLCs da mesma região.

## Controles

**Mouse:** clique na janela do jogo para capturar o mouse, que passa a girar a câmera; Esc ou
trocar de janela solta o mouse. Botão esquerdo = R1 (agarrar), direito = □ (atacar),
meio = L1. Sensibilidade e inversão ficam em **Controles** no lançador. Para remapear os botões
do mouse, use `mouse.<entrada>=left,right,middle,x1,x2`.

Um controle (DualShock 4, DualSense, Xbox...) é usado automaticamente. Com vários conectados,
escolha um em **Controles** no lançador (ou `BB_GAMEPAD=` no `sotc.ini`). O teclado funciona
junto com o controle:

| Teclado | PS4 |
|---|---|
| W A S D | analógico esquerdo |
| setas | analógico direito (câmera) |
| Espaço | ✕ |
| Shift esquerdo | ○ |
| E | □ |
| Q | △ |
| 1 / 3 | L1 / R1 |
| R / F | L2 / R2 |
| Z / C | L3 / R3 |
| Enter | Options |
| I J K L | direcional |
| Tab / Backspace | touchpad (esquerda / direita) |

As teclas podem ser trocadas em **Controles** no lançador: clique na ação e aperte a nova tecla.
O controle e o mouse também podem ser remapeados no `sotc.ini`, por exemplo `pad.circle=b`.
As entradas são:

- botões: `cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad touchpad_right`;
- direcional: `up down left right`;
- no teclado, os analógicos: `move_up` `move_down` `move_left` `move_right` e
  `look_up` `look_down` `look_left` `look_right`.

## Estado

Funcionam: logos, abertura, menus com texto, carregar save, exploração a cavalo,
som (com os módulos do usuário).

Desempenho medido no demo da tela de título, modo 60 FPS, i9-13900K + RTX 2060 SUPER:
média de ~67 FPS; o trecho mais pesado (a ponte do cânion) fica em 29–34 FPS. No modo
normal o jogo roda travado em 30 FPS, como no PS4.

## Perguntas comuns

- **DirectX 11/12?** Não. O núcleo de vídeo (shadPS4) traduz a GPU do PS4 para Vulkan; os
  shaders do jogo viram SPIR-V. Toda placa de vídeo com DirectX 12 recente também tem Vulkan
  1.3: atualize o driver. Um modo DirectX exigiria reescrever o núcleo de vídeo inteiro.
- **Placas AMD e Intel?** As otimizações valem para qualquer placa, menos duas que só ligam
  sozinhas na NVIDIA (o envio periódico de trabalho à GPU e as leituras antecipadas).
  Em **Imagem e FPS > Envio de trabalho à GPU**, escolha **Ligado** para testá-las na AMD ou
  Intel; se a tela ficar preta, volte para **Automático**.
- **Por que o som precisa de arquivos meus?** `libSceNgs2.sprx` e `libSceUlt.sprx` são firmware
  da Sony e não podem ser distribuídos. Eles saem do seu PS4 (ou do shadPS4 que você já usa).

## Créditos

- [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) (bbport, deadinside28 e forks): carregador,
  runtime nativo e lançador, base deste port.
- [shadPS4](https://github.com/shadps4-emu/shadPS4) e o fork shadPS4 SotC: núcleo de vídeo Vulkan.
- [Pink-shadPS4](https://github.com/luizgustavs/Pink-shadPS4) (luizgustavs): as cvars de culling, a proteção
  do heap do jogo contra leituras da GPU (`bpe_heap_guard`), o flush periódico, as leituras antecipadas,
  o marcador de espera e o gravador de comandos Vulkan em thread própria (`cp_record_thread`).

## SotC Lab

Banco de testes de travadas dentro do jogo (experimental, desligado por padrão): câmera livre
(F3) com o Wander junto, rotas gravadas (F4), pulo para qualquer um dos 16 colossos, `SOTC_*` ao
vivo ou com reinício, e um orquestrador que roda matrizes de configurações e grava os resultados
em CSV. Para ligar, use **Ajustes experimentais** no lançador (`SOTC_LAB=1`). Teclas: **F2** abre o
menu, **F3** liga a câmera livre, **F4** grava um ponto de rota. O guia completo, com os controles,
o formato das rotas, o canal de controle e as limitações, está em [LAB.md](LAB.md).

## Diagnóstico

Estas variáveis de ambiente ajudam a investigar problemas:

| Variável | Efeito |
|---|---|
| `SOTC_FPS=1` | taxa de quadros a cada 5 s |
| `SOTC_FPS_LEAN=1` | com `SOTC_FPS`: mantém o resumo e as contagens, sem as leituras de relógio por operação (para jogar medindo) |
| `SOTC_STALL_DETAIL=1` | com `SOTC_FPS`: linhas `SOTCSTALL` com ritmo de flip, CPU das threads do jogo e usuário/kernel do upload |
| `SOTC_OPSTAT=1` | operações da thread de desenho por tipo, a cada 5 s |
| `SOTC_UNTRACK_LOG=1` | com `SOTC_FPS`: blocos do heap por quadro RenderSingleFrameGPU do jogo |
| `SOTC_TRACE=1/2` | gravador de linha do tempo (trace automático, versão 2): ver [Trace automático](#trace-automático-sotc_trace) |
| `BB_FAULT_RING=arquivo` | grava as últimas exceções (sobrevive a quedas) |
| `BB_STACKS_TRIGGER=arquivo` | criar o arquivo despeja as pilhas das threads |
| `BB_READBACKS=0/1/2` | leituras da GPU; o padrão 2 é o exigido pelo jogo |
| `SOTC_FAST_BIND=1` | reaproveita binds de descritores entre desenhos iguais (padrão: desligado desde a 0.60.6) |
| `SOTC_RECORD_THREAD=0` | grava os comandos Vulkan na própria thread de comandos (padrão: thread própria) |
| `SOTC_INDIRECT_DIRECT=0` | mantém indiretos os draws cujos argumentos estão na CPU |
| `SOTC_STAGING_CACHED=1` | experimental: a área de cópia (staging) dos buffers e das texturas que a thread de desenho envia à GPU fica em memória com cache, em vez de write-combined, para a cópia da CPU andar mais rápido nas cargas de área (ignorado sem esse tipo de memória ou com `SOTC_REBAR_UPLOAD`) |
| `SOTC_COPY_GUEST_VIEW=1` | experimental: os envios de memória do jogo para a GPU leem pelo endereço do próprio jogo, protegidos contra falha (se a página estiver bloqueada, a cópia volta para a segunda visão da memória); evita a falta de página da segunda visão, que segurava a cópia em ~3 GB/s |
| `SOTC_BACKING_WARM=1` | experimental: uma thread de baixa prioridade toca, na segunda visão da memória, as páginas que o jogo começa a escrever, para os envios à GPU não pagarem a falta de página na thread de desenho (medido em 30/10: sem ganho; prefira `SOTC_COPY_GUEST_VIEW`) |
| `SOTC_FILL_HLE_GPU=1` | experimental: as tabelas que o shader de preenchimento (HLE) escreve chegam à GPU por preenchimento ou cópia na própria GPU, em vez de serem copiadas de novo pela memória do jogo na varredura DMA (uma tabela de 8 MiB, ~33 vezes por carga de área) |
| `SOTC_WAIT_MARKER=1/2` | esperas pela GPU consultam um marcador escrito por ela (experimental) |
| `SOTC_CMD_STATS=1/2` | draws e dispatches por quadro (2: shaders de compute mais usados) |
| `SOTC_TSC=qpc/native` | relógio do jogo: `qpc` sempre usa o QPC do Windows, `native` sempre o TSC do processador (padrão: QPC só quando os núcleos discordam no TSC) |
| `SOTC_UI_HIRES=0` | com DLSS, a interface (textos, logos, HUD) volta a ser desenhada na resolução de renderização e ampliada, em vez de na resolução de saída |
| `SOTC_LAB=1` | liga o SotC Lab (menu F2, câmera livre, pulo para colosso, canal de controle). Ver [LAB.md](LAB.md) |
| `SOTC_FREECAM=1` | câmera livre (F3); ligada por `SOTC_LAB=1` |
| `SOTC_FREECAM_MAXSPEED=<m/s>` | velocidade máxima da câmera livre, acima da qual o streaming não acompanha |
| `SOTC_ENGINE_CMD=1` | executa comandos do console da engine (pulo para colosso); ligada por `SOTC_LAB=1` |
| `SOTC_CTL=1` ou `SOTC_CTL=<nome>` | canal de controle em `\\.\pipe\sotc-ctl` (ou `\\.\pipe\<nome>`), para `tools/sotc_ctl.py` |
| `SOTC_CAM_PROBE=1` | só medição: registra a câmera do jogo e a posição do jogador |
| `SOTC_TUNING_JSON=<arquivo>` | `out/tuning.json` lido pelo menu do SotC Lab para montar a aba SOTC_* |
| `SOTC_BOOT_BOSS=N` | abre o jogo direto no colosso N (1 a 16) |
| `SOTC_LAB_ORCH_CMD=<comando>` | comando que o menu usa para iniciar o orquestrador (padrão `python tools/sotc_matrix.py`) |

### Trace automático (SOTC_TRACE)

Gravador de linha do tempo da versão 2, experimental e desligado por padrão. Registra, com um
relógio só, o que as threads do jogo e do motor fizeram, e grava arquivos `.sotctrace` em
`user/trace/<sessão>/`. Formato em [TRACE-FORMAT.md](TRACE-FORMAT.md).

- `SOTC_TRACE=1`: grava a janela ao redor de cada quadro acima de `SOTC_TRACE_MS` (padrão 60), com
  `SOTC_TRACE_PRE_MS` (150) antes e `SOTC_TRACE_POST_MS` (40) depois, até `SOTC_TRACE_MAX` arquivos
  por sessão (padrão 40).
- `SOTC_TRACE=2`: grava a sessão inteira, em pedaços comprimidos, enquanto o jogo roda.
- Os dois custam CPU e ligam o `SOTC_FPS` (contadores no registro).

Interruptores que acompanham o `SOTC_TRACE` (todos desligados por padrão):

| Variável | Efeito |
|---|---|
| `SOTC_TRACE_DIR=<pasta>` | pasta dos arquivos (padrão `user/trace`) |
| `SOTC_TRACE_RING=<n>` | multiplicador do tamanho dos anéis de eventos de cada thread |
| `SOTC_TRACE_ZONES=1` | as zonas de medição do shadPS4 (Tracy) viram trechos; mínimo `SOTC_TRACE_ZONE_MIN_US` (padrão 20 µs) |
| `SOTC_TRACE_GPU=1` | tempo real da GPU por envio, com marcas de tempo da Vulkan (usa a calibração do relógio da placa, quando ela tem a extensão) |
| `SOTC_TRACE_IMPORTS=1/2` | chamadas ao sistema PS4: 1 conta chamadas e tempo; 2 também grava os trechos maiores que `SOTC_TRACE_IMPORT_MIN_US` (padrão 50 µs) |
| `SOTC_TRACE_SCOPES=1` | os escopos de medição do próprio motor do jogo viram trechos com os seus nomes |
| `SOTC_TRACE_SAMPLE=<Hz>` | amostragem contínua das pilhas de todas as threads; substitui o `SOTC_PROFILE` (o nome antigo ainda vale) |
| `SOTC_TRACE_FUNCS=<arquivo>` | cronometragem exata das funções do eboot listadas no arquivo, gerado por `tools/trace/sotc_trace.py autofuncs` |

Os arquivos são lidos com `tools/trace/sotc_trace.py` (biblioteca padrão do Python, sem instalação):

- `hitch <arquivos> [--md relatório.md]`: por que cada quadro lento foi lento.
- `top <arquivos>`: tempo total por nome e por thread.
- `json <arquivo>`: JSON do Chrome, para abrir em ui.perfetto.dev.
- `diff <pasta A> <pasta B> --md relatório.md`: diferença de tempo entre duas execuções.

No `tools/sotc_matrix.py`, a chave de topo `"trace": 1` ou `2` (e `"trace_funcs"`, um arquivo para
`SOTC_TRACE_FUNCS`) liga o rastreamento em cada execução. Cada volta do teste também envia
`trace mark <configuração>-<cenário>-lapN`, e o `summary.md` ganha a seção "Onde o tempo foi": a
diferença de cada configuração contra a primeira, com links para as pastas de rastreamento e os
relatórios de travada.
