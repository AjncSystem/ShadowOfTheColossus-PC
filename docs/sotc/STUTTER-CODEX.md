# Melhorias locais de stutter — Codex, 13/10/2026

Trabalho sobre [AjncSystem/ShadowOfTheColossus-PC](https://github.com/AjncSystem/ShadowOfTheColossus-PC),
commit `4ace782ac0c2fcbd4d4f34cfd5d19de3f6910ab5`, branch local
`codex/stutter-cache`. Fonte em `D:\Dev\ShadowOfTheColossus-PC`.
O protótipo anterior em `D:\Dev\sotc_pc` permaneceu parado.

## Executável para testar

A instalação usada normalmente também foi atualizada no segundo lote:

```text
D:\Downloads\SotC-PC (1)\SotC-PC\SotC.exe
```

A cópia separada do primeiro lote continua disponível:

```text
D:\Downloads\SotC-PC (1)\SotC-PC\SotC-Codex\SotC.exe
```

O launcher, runtime e dependências anteriores da instalação normal estão em
`_codex_backup/20261013-214910`, com manifesto de hashes e instruções de restauração. O segundo
lote foi aplicado também à cópia separada. Ambas usam os mesmos caminhos de jogo/update. Os saves
iniciais foram copiados para `SotC-Codex/user/savedata`; progresso novo dessa
cópia fica nessa pasta, salvo se escolher outra pasta de dados no lançador.
Na instalação normal, `user/savedata` foi preservado. As preferências do lançador
continuam no local usado pelo projeto original.
Não foi feito push, publicação ou alteração nos dumps do jogo.

SHA-256 do runtime do segundo lote:
`293c0d185f5d5d9c4e6707d9b20bf250560c1d930aca3163011a5e317a89a9e7`.
Primeiro lote: `4b8e52750d397573626d1c53a310a5a6786cb601be80f776c570f08d99d18b52`.
Original preservado:
`43a4eade3e02fed9861029c909f45cb2bc13e881d5b10635c8c04769f06aa2c7`.

## Correções implementadas

- O `VkPipelineCache` agora é criado **antes** do `WarmUp`. Antes, as pipelines
  pré-carregadas recebiam um handle vazio, e só depois era criado o cache do driver.
- O cache do driver é persistido e restaurado, com identificação de vendor/device,
  UUID, tamanho limitado e checksum do conteúdo. Dados truncados, corrompidos ou
  de outro dispositivo são ignorados; uma rejeição do driver tenta cache vazio.
- Depois do preload há um checkpoint; novos pipelines podem gerar outro depois
  de 30 segundos. Consulta ao driver ocorre na thread GPU; checksum, envelope e
  publicação do arquivo ocorrem no worker de I/O. O CSV mede o custo da consulta.
- Perfil incompatível preserva a pasta antiga como `CUSA08809.incompatible.*`,
  reabre um cache vazio e grava o perfil atual. Antes fechava o cache pelo resto
  da sessão. Se preservar a pasta falhar, o diagnóstico registra a falha.
- A fila de I/O usa uma trava única e drena os pedidos aceitos antes de fechar.
  Foi removida a contagem compartilhada de pedidos acessada sem sincronização.
- Arquivos são publicados completos, com temporários distintos por processo e
  substituição de arquivo existente no Windows. Isso também vale para blobs
  de shaders, metadata, perfil e chaves de pipelines.
- O cache padrão no Windows fica em
  `%LOCALAPPDATA%/ShadowOfTheColossus-PC/cache`, independente da pasta da release.
  Um destino novo recebe uma cópia do cache legado de `user/cache`, sem apagá-lo.
  `SOTC_CACHE_DIR` seleciona outro destino; `SOTC_PORTABLE_CACHE=1` mantém `user/cache`.
  O botão de limpeza do lançador acompanha o destino novo e apaga o título
  CUSA08809, preservando outros diretórios do destino configurado.
- Medição opcional por quadro registra intervalos, compilação de shaders/pipelines,
  readbacks, espera de gravação e consulta de cache. A apresentação conta somente
  novos quadros do jogo cujo `Present` foi bem-sucedido.

As versões de shaders/metadata/chaves não foram aumentadas: essas mudanças não
alteram SPIR-V nem o formato dos blobs existentes. O envelope do driver é separado.
Também foram necessários dois ajustes de compilação com o Clang local:
`<cstdlib>` no sirit e anotação da variável de estado não consultada de CommonDialog.

## Verificação e limites da evidência

### Segundo lote: apresentação e ritmo dos quadros

- `Present` passa para uma fila com worker próprio, em ordem. O relógio de vblank
  e os eventos do jogo continuam enquanto esse worker aguarda gravação, GPU ou
  swapchain. Redesenhos ociosos não acumulam enquanto há trabalho pendente.
  A trava existente de acesso à fila Vulkan foi mantida.
- O produtor de vblank é parado antes de drenar e juntar o worker no destrutor.
- O limitador do host agora é usado: 60 FPS com vblank a 120 Hz, 30 FPS com
  vblank a 60 Hz. `BB_FPS_LIMIT=0` permite desativá-lo explicitamente.
  Flips terminados entre dois vblanks podem ser liberados no prazo do limitador,
  quando `flip_rate=0`, sem aguardar o próximo tick completo.
- A recuperação de atraso tem dívida limitada a um período; evita uma longa
  sequência de vblanks imediatos depois de um stall.
- A espera no Windows reutiliza um timer por thread, tenta alta resolução e
  usa fallback. O relógio é monotônico e o trecho final de espera é curto.
- Preparar um quadro limita o avanço dos comandos da GPU a um quadro por
  padrão. Nenhum draw/readback foi descartado para obter esse limite.
- Não houve alteração de shaders, aumento da versão do cache, relaxamento dos
  readbacks ou reativação do streaming neste lote.

Overrides para diagnóstico:

| Variável | Padrão / efeito |
| --- | --- |
| `SOTC_PRESENT_THREAD=0` | Desliga só o worker separado; padrão ligado. |
| `SOTC_FRAMES_AHEAD=0` | Desliga a espera de avanço da GPU; padrão 1, máximo 3. |
| `BB_FPS_LIMIT=30`, `40`, `60` | Limite explícito do host; não modifica sozinho o modo do jogo. |
| `BB_FPS_LIMIT=0` | Sem limite do host; modo experimental do jogo ainda depende das opções do launcher. |

- Build completo passou; `sotc-present-test` verifica ordem, descarte de redraw
  ocioso, relógio independente de presenter bloqueado, produtores concorrentes,
  drenagem/fechamento e dívida limitada do timer, com asserts ativos.
- `out/pacing/run-1791938520363819600`: SHA do segundo lote acima; save de progresso
  carregado, personagem no campo, movimento por pad file e menu de pausa vistos
  em screenshots. Janela de 26 s de avanço controlado: **51,51 FPS**, p95
  **21,77 ms**, p99 **23,38 ms**, **1 intervalo acima de 40 ms**. Capturas foram
  feitas durante o teste e controles físicos ainda eram aceitos nesta execução;
  portanto é evidência de funcionamento no mapa, não um A/B de rotas idênticas.
  A janela está em `walking-summary.json`, os inputs em `manual-events.jsonl`.
  Fechamento dirigido à janela do próprio teste: exit 0, sem timeout.
- A base do primeiro lote chegou ao campo em `run-1791937469179093000`, mas não
  fez a mesma rota nem usou o mesmo aquecimento de cache. Os outros dois testes
  iniciais ficaram na abertura e foram descartados como benchmark de caminhada.
- Instalação normal: `out/pacing-install.json` registra hashes do binário e do
  launcher, 1.150 arquivos anteriores no backup e saves idênticos antes/depois.
  O `sotc.ini` também recebeu o caminho atual do dump, depois da mudança de pasta.
- Teste do `SotC.exe --run` **já instalado**, modo de 30 FPS:
  `out/installed-pacing-smoke/run-1791939127579468100`. Cache de driver do teste
  do mapa copiado somente para o cache isolado desse teste; restaurou
  **28.913.311 bytes**, pré-carregou **2.015 pipelines em 1.197 ms** e gravou
  **1.200 apresentações** em CSV, com captura da abertura. Log confirmou
  `vblank 60 Hz, frame limit 30 FPS, separate present 1`. Encerramento intencional
  pelo job aos 45 s; não é evidência de caminhada no modo de 30 FPS.
  O primeiro teste isolado, `run-1791938991593559800`, tinha cache de driver vazio
  e atingiu o limite de 55 s durante o preload, antes da primeira apresentação.
  Esse teste não foi contado como sucesso de boot ou benchmark.
- O helper de rotas agora usa replay neutro para bloquear interferência de
  controles físicos, salvo `--allow-host-pad`. Selecionar o menu por tempos
  fixos pode falhar: confirmar o save e as capturas antes de resumir a janela.

**Ainda existem stalls de novos shaders/pipelines.** A compilação desses objetos
continua síncrona. As medidas desta sessão não demonstram a eliminação do stutter
nem 60 FPS constantes na campanha.

Testes locais, a partir da fonte, com CLANG64 no PATH:

```powershell
python -m unittest discover -s tests -p test_sotc_stutter.py -v
& out/gpu/sotc-present-test.exe
& out/gpu/sotc-cache-test.exe out/gpu/cache-smoke.spv out/cache-test-local
```

O segundo argumento do teste de cache é um diretório descartável, sem saves.

### Primeiro lote: cache e instrumentação

- Build nativo completo com MSYS2 CLANG64, sem LTO.
- `sotc-cache-test`: pipeline compute real na RTX 5070; gravação, restauração,
  reutilização, troca de arquivo existente e rejeição por corrupção/UUID/vendor.
- Três testes Python: destino entre releases, overrides e métricas com hitch.
- `run-1791935584976250300`: restaurou **21.740.744 bytes** do driver antes do
  preload; **1.560 pipelines em 707 ms**. 60,97 s de execução, encerrada pelo
  timeout do teste, com 6.120 linhas de quadros gravadas. A captura da abertura
  mostra a águia no cânion; isso não é gameplay controlado no mapa.
- `run-1791935683114203300`: um perfil inválido foi colocado somente no cache
  copiado do teste. O runtime preservou a geração antiga, reabriu o store e
  gravou perfil válido de 64 bytes e novos blobs. 55,88 s até timeout e 5.760
  linhas de quadros. Esses dois testes usam o binário anterior à alteração
  apenas do texto do aviso (`00c2095c...`); o teste empacotado registra o SHA final.
- Relatórios e logs desses testes: `out/stutter-smoke/run-*/`.
- Teste do executável empacotado, preparação e migração do cache:
  `out/packaged-smoke/run-1791935959629210900/`.
  Usa cópia de saves e `%LOCALAPPDATA%` isolado; encerra seu próprio job de
  processos no timeout. A preparação pelo `SotC.exe` funcionou, o cache novo
  recebeu a cópia do legado (preservado), e foram pré-carregadas 1.545 pipelines
  em 2.108 ms, com 3.540 apresentações registradas durante o limite de 75 s.
  Resultado completo em `run.json` e `game.log`.
- Manifesto da cópia instalada: `out/candidate-install.json`.

**Ainda não há comparação de duas rotas iguais de gameplay antes/depois.**
Os números de boot/preload não demonstram aumento de FPS ou eliminação de stutter
durante exploração. Capturar screenshot perturba o quadro correspondente.
Os testes são limitados por tempo, sem confirmação de combate/campanha ou áudio.
Sem módulos de firmware próprios, o caminho de áudio desse projeto permanece mudo.

## Medir a mesma rota

Na página Avançado do lançador, nas variáveis de ambiente, ou no PowerShell antes
de abrir a cópia nova:

```powershell
$env:SOTC_FRAME_STATS='1'
$env:SOTC_FRAME_TIMES='D:\Dev\ShadowOfTheColossus-PC\out\rota-1.csv'
& 'D:\Downloads\SotC-PC (1)\SotC-PC\SotC-Codex\SotC.exe'
```

Use outro nome para a segunda sessão, repita a mesma rota e conserve opções,
save, cache e modo de apresentação. Evite capturas durante a janela comparada.
`SOTC_FRAME_STATS=1` imprime `SOTCSTALL` para intervalos maiores que 40 ms;
o CSV funciona também sem esses prints. Sem ambas as variáveis, os timers ficam
desligados. O diretório pai do CSV precisa existir.

Resumo de uma janela **identificada visualmente**, em segundos após a primeira
apresentação registrada:

```powershell
python tools/sotc_frame_summary.py out/rota-1.csv --start 30 --end 90 --scene 'rota a cavalo, cache aquecido' --runtime out/bb-probe.exe
```

Tempos de apresentação no host não são tempos de GPU nem de scanout. Contadores
de workers são atribuídos quando a operação termina e podem se sobrepor: não
somá-los como custo total de um quadro. A consulta periódica de cache tem custo
mensurável; observar `cache_snapshot_ns` antes de decidir outro intervalo/estratégia.

## Próximos problemas a isolar

1. Medir rota repetida e distinguir compilação de readbacks/esperas/present.
2. Medir os overrides do segundo lote na mesma rota com cache aquecido, incluindo
   limites de 30/40/60 FPS e apresentação do monitor. Não foi transplantado o DrawScan/DrawPrep.
3. Estudar uploads/staging e readbacks sem reduzir sua precisão arbitrariamente.
4. Streaming: `sotc_lod_stats.h` tem `Linger = 0` e decide amostragem usando
   `idle[i] < linger`. Mesmo banco usado acaba com `idle=0`, logo `0 < 0` é falso.
   O streaming esperado pelo comentário fica desativado. **Não foi reativado
   neste lote:** o próprio histórico relata corrupção/quedas quando o pool enche.
   Corrigir e fazer testes longos de carga de save/memória é trabalho separado.
5. Não usar uma execução sem crash, a média de menus ou o contador de FPS como
   prova de fluidez durante campanha. AMD/Intel e sessões longas não foram testados.

## Referências do contrato Vulkan

- [Cabeçalho de cache: 32 bytes, vendor/device/UUID](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCacheHeaderVersionOne.html).
- [Recuperar os dados do cache](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPipelineCacheData.html).
- [Restaurar dados na criação](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCacheCreateInfo.html).
