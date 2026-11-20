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
- **Som:** desde a 0.75 o port tem um **mixer de som embutido**, e o jogo tem som sem nada do
  console. Opcional: `libSceNgs2.sprx` e `libSceUlt.sprx` do **seu próprio console**, decriptados
  (os mesmos da pasta `user/sys_modules` do shadPS4; num PS4 com GoldHEN ficam em
  `/system/common/lib/`, FTP na porta 2121). Com eles, o som é o do firmware.

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
com as últimas opções do lançador, o que serve para atalhos e para o Steam. O botão "Atalho para
jogar direto" do lançador (Avançado) cria esse atalho na área de trabalho, que pode ser adicionado
ao Steam como jogo não-Steam.

O lançador procura versões novas no GitHub ao abrir. Se a conexão com o GitHub não puder ser
verificada (erro de certificado, comum com antivírus ou proxy que inspecionam HTTPS, ou com o
Windows sem os certificados raiz atualizados), ele avisa e mostra o endereço para baixar a versão
nova manualmente: https://github.com/AjncSystem/ShadowOfTheColossus-PC/releases

Ao procurar atualização, o lançador mostra as notas da versão do GitHub. Quem publica uma versão
pode incluir uma seção `## English` no texto: o lançador em português mostra só o que vem antes dela,
e em outros idiomas mostra só essa seção (até um `## Português`, se houver). Sem essa seção, o texto
aparece inteiro.

## Imagem e desempenho

Tudo isto está no lançador, na página **Imagem e FPS** (e os principais em **Jogar**):

- **Saída** 720p, 1080p, 1440p ou 4K. O jogo renderiza em 1080p, como no PS4, e a imagem é
  ampliada para a saída com **FSR 1** + nitidez (RCAS) ou bilinear. A janela nunca passa de 90%
  da tela; a tela cheia usa o monitor inteiro.
- **60 FPS** (experimental): tira o limite de 30 FPS do jogo (patch GoldHEN de illusion), com
  vblank a 120 Hz. Mediu 38 FPS no campo aberto e ~25 no templo.
- **Reutilização de binds** (ligada por padrão; `SOTC_FAST_BIND=0` desliga): pula
  a remontagem dos descritores quando o desenho seguinte usa o mesmo pipeline, recursos e
  uniforms. Medido no Santuário: ~25 → 30 FPS. Na 0.60.5 deixava pontos pretos em grade no chão de
  areia; com a correção o chão de areia do demo ficou limpo.
- **Texturas nítidas**: filtro anisotrópico 16x, quase sem custo.
- **HDR**: saída HDR10 com a renderização HDR do próprio jogo; o HDR do Windows precisa estar
  ligado. O branco fica em 200 nits (`SOTC_HDR_WHITE` muda). Funciona com FSR 1 e DLSS. Os realces
  descem suavemente até o pico da tela (lido do Windows) em vez de estourar; a interface
  tem brilho próprio (com FSR 1 e DLSS). Controles de imagem no estilo RenoDX: exposição, realces,
  sombras, contraste, saturação e desbotar realces, neutros por padrão e só no mundo (não na
  interface); contraste, realces e sombras giram em torno do brilho médio da cena, medido a cada
  quadro. Também saturação dos realces, compensação de véu (pretos mais fundos) e correção de gama
  (a parte escura como numa tela SDR de gama 2.2). Curva de tons: **Original** (a do jogo),
  **Original+** (os mesmos tons médios e escuros, mas os realces continuam subindo até o pico da
  tela em vez de achatar), **Original+ (cor)** (a mesma curva aplicada ao brilho e não a cada cor:
  as cores claras não puxam para o amarelo e o branco; a correção de matiz devolve aos poucos o
  tom do jogo) ou **Nenhuma**; e a força
  da correção de cor do jogo (o tom de cada lugar), de 0 a 100%. Funcionam com FSR 1 e DLSS. Tudo
  no menu F1 > Imagem; "HDR: restaurar imagem" volta tudo (e os efeitos) ao original. O pico
  escolhido vai para a tela como metadado HDR10, para ela não aplicar o próprio tone mapping por
  cima. O pico automático vem do Windows: calibre com o app Calibração de HDR do Windows.
- **Modo PS4 Pro**: o jogo se vê num PS4 Pro e oferece no próprio menu (Opções > Apresentação)
  o Modo Grafismo **Cinematográfico** (1440p, mais detalhe) ou **Desempenho**. É mais pesado:
  ~25 FPS no campo aberto.
- **Efeitos**: vinheta, névoa atmosférica e oclusão ambiente (SSAO) podem ser desligados.
  A força do brilho difuso (bloom), da vinheta, da névoa e do borrão de movimento vai de 0 a 200%
  ao vivo (F1 > Imagem, em SDR e HDR); a névoa em 0% deixa a distância nítida, em 200% ela junta o
  dobro com a distância. O bloom do jogo é um véu largo que clareia as sombras: no deserto, 400% sobe a parte mais
  escura da imagem em ~30%; 0% deixa a imagem mais nítida e as sombras mais fundas.
  A nitidez depois do DLSS / FSR 4 também muda ao vivo (F1 > Imagem, seis níveis).
  O motion blur, o brilho e os filtros ficam no menu do próprio jogo.
- **Comparar com o original (F3)**: com o menu F1 fechado, F3 mostra a imagem como o jogo a faz
  (curva de tons e cores do jogo, controles de imagem do HDR neutros, efeitos em 100%), com
  "ORIGINAL (F3)" no canto; F3 de novo volta aos seus ajustes. O pico e o brilho do HDR ficam.
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

**Remapear (`sotc.ini`):** uma linha por entrada, com várias opções separadas por vírgula (vazio =
sem tecla). `key.<entrada>=` troca as teclas (nomes do SDL, como `Q`, `Left Shift`, `Space`) e
`mouse.<entrada>=` os botões do mouse. Entradas: `cross circle square triangle l1 r1 l2 r2 l3 r3
options touchpad touchpad_right up down left right move_up move_down move_left move_right look_up
look_down look_left look_right`. Exemplos: atacar com o botão esquerdo e o raio da espada com o
direito: `mouse.square=left` e `mouse.l1=right` (e `mouse.r1=` para soltar o R1); L1/R1 em outras
teclas: `key.l1=Q` e `key.r1=E`.

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
- **Preciso de arquivos do console para ter som?** Não, desde a 0.75: o mixer embutido do port
  toca o som. `libSceNgs2.sprx` e `libSceUlt.sprx` (firmware da Sony, que não pode ser
  distribuído) continuam opcionais; com eles, o som é o do firmware.

## Créditos

- [bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) (bbport, deadinside28 e forks): carregador,
  runtime nativo e lançador, base deste port.
- [shadPS4](https://github.com/shadps4-emu/shadPS4) e o fork shadPS4 SotC: núcleo de vídeo Vulkan.
- [Pink-shadPS4](https://github.com/luizgustavs/Pink-shadPS4) (luizgustavs): as cvars de culling, a proteção
  do heap do jogo contra leituras da GPU (`bpe_heap_guard`), o flush periódico, as leituras antecipadas,
  o marcador de espera e o gravador de comandos Vulkan em thread própria (`cp_record_thread`).

## Diagnóstico

Estas variáveis de ambiente ajudam a investigar problemas:

| Variável | Efeito |
|---|---|
| `SOTC_FPS=1` | taxa de quadros a cada 5 s |
| `BB_FAULT_RING=arquivo` | grava as últimas exceções (sobrevive a quedas) |
| `BB_STACKS_TRIGGER=arquivo` | criar o arquivo despeja as pilhas das threads |
| `BB_READBACKS=0/1/2` | leituras da GPU; o padrão 2 é o exigido pelo jogo |
| `SOTC_FAST_BIND=0` | desliga o reaproveitamento de binds de descritores entre desenhos iguais (padrão: ligado) |
| `SOTC_RECORD_THREAD=0` | grava os comandos Vulkan na própria thread de comandos (padrão: thread própria) |
| `SOTC_INDIRECT_DIRECT=0` | mantém indiretos os draws cujos argumentos estão na CPU |
| `SOTC_WAIT_MARKER=1/2` | esperas pela GPU consultam um marcador escrito por ela (experimental) |
| `SOTC_CMD_STATS=1/2` | draws e dispatches por quadro (2: shaders de compute mais usados) |
| `SOTC_TSC=qpc/native` | relógio do jogo: `qpc` sempre usa o QPC do Windows, `native` sempre o TSC do processador (padrão: QPC só quando os núcleos discordam no TSC) |
| `SOTC_HEAP_TRACK=arquivo` | só em builds com `-DBB_HEAP_TRACK=ON`: a cada minuto grava as pilhas de chamada que mais seguram memória do heap (vazamentos); `tools/heap_track_report.py` compara dois relatórios |
| `SOTC_WS_DIAG=1` | linha `SOTC WS` a cada 5 s: quanto as cópias de buffers da GPU copiaram, quantas páginas de origem/staging estavam fora do working set (falhas de página), falhas de página do processo, heap de memória do host do Vulkan e memória zerada ao liberar |
| `SOTC_AMD_WAVE32=1` | só AMD, para testar nas GCN (RX 470-590, Vega, integradas): os shaders são convertidos para 32 lanes, como faz a camada SotC-AMD-Fix (personagens piscando em branco). O cache de shaders é refeito; o log mostra `GPU: AMD wave32 on`. De Lucas-blip-png |
| `SOTC_SLOW_BUILD_MS=500` | o log mostra `SOTC SLOW BUILD` para cada pipeline construído em segundo plano (ou esperado pela gravação de comandos) que levou esse tempo ou mais; `SOTC SYNC PIPELINE` mostra os construídos na thread de comandos acima de 30 ms (`SOTC_SYNC_PIPELINE_MS`). Para as travadas longas nas placas Intel Arc |
| `SOTC_HDR_WHITE=nits` | com HDR: o branco do jogo em nits (padrão 200; a saída do próprio jogo tem ~100) |
| `SOTC_HDR_PEAK=nits` | com HDR: o pico da tela; os realces descem suavemente até ele (BT.2390). Padrão `auto` (o valor do Windows, 1000 sem ele); `off` corta como antes |
| `SOTC_HDR_UI=nits` | com HDR: o brilho da interface (menus, legendas, HUD), separado do brilho do jogo. Padrão: igual ao `SOTC_HDR_WHITE` |
| `SOTC_HDR_EXPOSURE=EV` | em HDR e SDR: exposição do mundo em pontos, -3 a 3 (padrão 0) |
| `SOTC_HDR_CONTRAST=0..100` | em HDR e SDR: contraste em torno do brilho médio da cena (padrão 50 = original) |
| `SOTC_HDR_SHADOWS=0..100` | em HDR e SDR: acima de 50 clareia as partes escuras, abaixo aprofunda (padrão 50) |
| `SOTC_HDR_HIGHLIGHTS=0..100` | em HDR e SDR: brilho das partes claras (padrão 50) |
| `SOTC_HDR_SATURATION=0..100` | em HDR e SDR: força das cores; 0 é preto e branco (padrão 50) |
| `SOTC_HDR_BLOWOUT=0..100` | em HDR e SDR: os realces mais fortes perdem a cor e vão para o branco perto do pico (padrão 0) |
| `SOTC_HDR_HIGHLIGHT_SATURATION=0..100` | em HDR e SDR: a força das cores só nas partes claras (padrão 50 = original) |
| `SOTC_HDR_FLARE=0..100` | em HDR e SDR: compensação de véu, pretos mais fundos (padrão 0) |
| `SOTC_HDR_GAMMA_CORRECTION=0\|100\|200` | com HDR: a parte escura decodificada como gama 2.2 (100) ou 2.4, BT.1886 (200), em vez de sRGB (padrão 0, desligado) |
| `SOTC_HDR_GRAIN=0..100` | em HDR e SDR: granulação de filme fina e em movimento, mais nos tons médios (padrão 0, desligada; o jogo não tem) |
| `SOTC_HDR_GAMUT_EXPANSION=0..100` | com HDR: leva as cores já saturadas além do sRGB, rumo ao P3 (padrão 0) |
| `SOTC_DITHER=0` | tira o pontilhado (dithering) de um passo antes dos 8 bits (SDR) ou 10 bits (HDR) da tela, que evita faixas no céu e na névoa (padrão ligado) |
| `SOTC_HDR_HUE_CORRECTION=0..100` | com HDR e as curvas `vanilla_plus_hue` e `renodrt`: quanto o tom dos realces volta ao do jogo (padrão 100) |
| `SOTC_HDR_TONEMAP=vanilla\|vanilla_plus\|vanilla_plus_hue\|renodrt\|none` | com HDR: a curva de tons do jogo (padrão `vanilla`); `vanilla_plus` mantém os tons médios e deixa os realces subirem até o pico; `vanilla_plus_hue` faz o mesmo no brilho, mantendo a cor; `renodrt` (como os mods RenoDX) mantém as sombras e os tons médios do jogo, tira a curva dos realces e desce o brilho suavemente até o pico mantendo cor e saturação; `none` tira a curva |
| `SOTC_FSR1_SPLIT=0` | com HDR e FSR 1: volta a ampliar a imagem do jogo já com a interface (sem brilho próprio da interface; os controles de imagem valem para a interface também) |
| `SOTC_HDR_GRADING=0..100` | em HDR e SDR: a força da correção de cor do jogo em porcentagem (padrão 100); 0 tira o tom de cor de cada lugar e mantém o brilho |
| `SOTC_TONEMAP_LOG=1` | diagnóstico: as constantes do tonemap do jogo quando mudam e a curva aplicada |
| `SOTC_HDR_TONEMAP_CYCLE=s` | teste: alterna as curvas de tons a cada `s` segundos (comparar capturas da mesma cena) |
| `SOTC_COMPARE_CYCLE=s` | teste: liga e desliga a imagem original (F3) a cada `s` segundos; com `SOTC_SHOT_OVERLAY=1` as capturas comparam os ajustes com o original na mesma cena |
| `SOTC_SHOT_RAW=1` | diagnóstico: com HDR, cada captura também grava os pixels PQ como foram mostrados (`<png>.raw`: `SOTCRAW1`, largura, altura e formato em u32, depois os pixels de 10 bits), para medir os nits dos realces que o PNG corta |
| `SOTC_FX_BLOOM=0..400` | a força do bloom em porcentagem (padrão 100 = o do jogo) |
| `SOTC_FX_VIGNETTE=0..400` | a força da vinheta em porcentagem (padrão 100); com a vinheta desligada não faz nada |
| `SOTC_FX_FOG=0..400` | a força da névoa atmosférica em porcentagem (padrão 100); com a névoa desligada não faz nada |
| `SOTC_FX_MOTION_BLUR=0..400` | o comprimento do borrão de movimento em porcentagem (padrão 100); com o borrão desligado não faz nada |
| `SOTC_FLAT_DUMP=0x<hash>,...` | diagnóstico: as constantes (flatbuf) desses shaders como floats, a cada 300 quadros |
| `SOTC_FX_CYCLE=s` | teste: os efeitos em 0%, 100% e 300% a cada `s` segundos |
| `SOTC_FX_CYCLE_WHAT=bloom,vignette,fog,motion` | teste: quais efeitos o `SOTC_FX_CYCLE` alterna (padrão todos) |
| `SOTC_FX_TRACE=1\|n` | diagnóstico: a cada 600 quadros (ou a cada `n`), quatro quadros seguidos: os shaders em ordem, com as texturas lidas e escritas e os formatos (`dados.número`; o do alvo de cor como `dados*100+número`) |
| `SOTC_EVICT_ASYNC=0` | a devolução de memória fria das arenas espera a GPU no processador de comandos, como antes (~44 ms por vez) |
| `SOTC_UI_HIRES=0` | com DLSS, a interface (textos, logos, HUD) volta a ser desenhada na resolução de renderização e ampliada, em vez de na resolução de saída |
