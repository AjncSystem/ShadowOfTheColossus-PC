# SotC Lab

O SotC Lab é um banco de testes de travadas (stutters) e de desempenho que roda dentro do próprio
jogo. Ele serve para repetir a mesma situação com as mesmas posições, trocar `SOTC_*` sem reabrir
o jogo a cada tentativa e medir o resultado em número (percentis de tempo de quadro e contagem de
travadas), em vez de depender de jogar e perceber.

Ele é **experimental e desligado por padrão**. Nada dele muda o jogo quando `SOTC_LAB` não está
definido.

## Para que serve

- **Câmera livre (F3)**: uma câmera de noclip que leva o Wander junto, sem gravidade e sem colisão.
  Como a carga de áreas segue a câmera (e não o jogador), dá para voar pelo mapa e medir o
  streaming como se estivesse jogando.
- **Rotas (F4 e aba Rotas)**: você voa, grava pontos e depois reproduz a mesma trajetória em
  velocidade constante, quantas vezes quiser.
- **Pulo para colosso**: leva o jogo direto a um dos 16 colossos, sem passar pela história.
- **Trocas ao vivo**: parte dos `SOTC_*` muda na hora, sem reinício. O restante pede reinício, e o
  Lab pode fazer esse reinício e voltar ao mesmo ponto.
- **Orquestrador**: roda uma matriz de cenários × voltas × configurações sem intervenção e grava
  os resultados em CSV.

## Como ativar

1. Pelo lançador: aba **Ajustes experimentais** (abaixo de Avançado), opção **SotC Lab**. Ela
   grava `SOTC_LAB=1` nas variáveis de ambiente do jogo.
2. Pelo `sotc.ini` (ou pelo ambiente do sistema), na pasta do jogo:

   ```ini
   SOTC_LAB=1
   ```

Com `SOTC_LAB=1` ligado, o jogo liga os recursos do Lab que têm chave própria (câmera livre,
comandos da engine, canal de controle). Cada um deles continua com a sua chave, que pode ser
desligada separadamente (ver a tabela de chaves abaixo).

## Teclas

| Tecla | Ação |
|---|---|
| **F2** | abre ou fecha o menu do Lab (abas abaixo) |
| **F3** | liga ou desliga a câmera livre |
| **F4** | com a câmera livre ativa, grava um ponto de rota na posição atual |
| **G** | com a câmera livre ativa, desliga a câmera e deixa o Wander onde ela está (teleporte) |

Com a câmera livre ativa, o controle do jogo fica neutro: o Wander não anda, e o teclado e o mouse
são usados só pela câmera. As teclas do menu (F2) não chegam ao jogo.

### Controles da câmera livre

A câmera livre usa a câmera de debug do próprio motor e os comandos `toggleDebugCam`, `SetCameraPos` e
`SetPlayerPos` (com `RetailBuild=0`, que o Lab liga sozinho). O Wander vai junto, 8 m abaixo da câmera
(`SOTC_FREECAM_DROP`), para as áreas carregarem como no jogo normal. Ao desligar com F3 ele volta
ao ponto de onde saiu; com **G** ele fica onde a câmera está.

| Entrada | Efeito |
|---|---|
| **W A S D** | anda para frente, para trás e para os lados (em relação à câmera) |
| **Q / E** | desce / sobe |
| **Mouse** / **setas** | olha em volta |
| **Shift** | multiplica a velocidade por 4 |
| **Ctrl** | multiplica a velocidade por 0,25 (movimento fino) |
| **Roda do mouse** | muda a velocidade base |
| **Analógico esquerdo** | anda |
| **Analógico direito** | olha em volta |
| **L2 / R2** | desce / sobe (como Q/E) |

A velocidade não passa de `SOTC_FREECAM_MAXSPEED` (metros por segundo). Acima de certa velocidade a
carga de áreas não acompanha a câmera, e as travadas medidas viram artefato do teste, não do jogo.
Por isso as rotas devem ficar abaixo desse limite.

## O menu (F2)

O menu tem cinco abas.

### Câmera

- Liga/desliga a câmera livre, ajusta a velocidade base e mostra a posição atual.
- Campos para ir a uma posição digitada (x, y, z, guinada e inclinação).
- Marcadores: guarda posições em `user/lab_bookmarks.json` e volta a elas depois.

### Rotas

- Lista as rotas de `tools/routes/` e de `user/routes/`.
- Grava uma rota nova, adiciona pontos, reproduz, pausa, liga o modo de repetição e ajusta a
  velocidade.
- Mostra o progresso por segmento (cada trecho nomeado da rota).

### Colossos

- Os 16 colossos, na ordem do jogo: 1 Valus, 2 Quadratus, 3 Gaius, 4 Phaedra, 5 Avion, 6 Barba,
  7 Hydrus, 8 Kuromori, 9 Basaran, 10 Dirge, 11 Celosia, 12 Pelagia, 13 Phalanx, 14 Cenobia,
  15 Argus, 16 Malus.
- **Ir lutar** envia o pulo ao colosso escolhido. Modos:
  - **Play**: entra na arena com o colosso vivo. Não muda o save.
  - **Kill**: entra na arena e mata o colosso. **Muda o save.**
  - **Outro**: leva ao desfecho (aftermath no Santuário). **Muda o save.**
- **Cavalo** (opção "horse"): depois do pulo, traz o Agro até o jogador. Fica desligado sozinho nas
  arenas aquáticas 7 (Hydrus) e 12 (Pelagia), onde esse passo trava o jogo.
- Antes de Kill e Outro, o Lab copia a pasta de save para `user/save-backups/<data-hora>/`.
- Se o pulo ao vivo não estiver disponível, o botão mostra **Reabrir no colosso N**, que reinicia o
  jogo já dentro da arena (ver "Reabrir no colosso" abaixo).

### SOTC_*

- Todos os `SOTC_*` de `out/tuning.json`, agrupados como no lançador, com busca e dica ao passar o
  mouse.
- Controles conforme o tipo: caixa de seleção para ligar/desligar, lista para escolha, campo de
  texto para valor.
- **Ao vivo** (selo verde): muda na hora pelo `SotcLive`.
- **Reinício** (selo amarelo): entra numa lista de pendências. O botão **Aplicar e reabrir** grava
  `user/lab_pending_env.json` e reinicia o jogo (pelo orquestrador, se houver um conectado; se não,
  o menu pede para reabrir pelo lançador).
- **Restaurar padrões** volta todos os `SOTC_*` ao valor padrão.

### Testes

- Carrega uma matriz, inicia ou para a execução e mostra o progresso por configuração, volta e
  segmento.
- Tabela ao vivo de p50, p99, máximo e travadas (acima de 33, 50 e 100 ms) do segmento atual.
- Mostra a pasta onde os resultados são gravados.

## Trocas ao vivo e trocas que pedem reinício

Cada `SOTC_*` é de um dos dois tipos. O menu e o lançador mostram o selo:

- **ao vivo**: muda enquanto o jogo roda. Vale a partir do próximo quadro que usa a opção.
- **reinício**: só vale quando o jogo abre. Mudar durante a partida não tem efeito até reabrir.

Uma chave só entra na lista de ao vivo se cumprir **todas** estas regras:

1. não faz parte de uma chave de pipeline ou de shader, nem da saída do recompilador, nem do
   layout dos descritores;
2. não é usada na criação do dispositivo Vulkan nem da cadeia de troca (swapchain);
3. não define tamanho de alocação na inicialização;
4. é lida no caminho de uso (a cada quadro ou a cada operação), não só uma vez.

Por isso, por exemplo, o cache de ligações (`kBindCache`), o cache de estado de pipeline
(`kDedupState`) e as chaves de bindless nunca são ao vivo.

## Reabrir no colosso

Algumas operações pedem reinício (por exemplo, quando o pulo ao vivo não é possível). Nesses casos o
Lab reinicia o jogo e pede que ele abra direto no colosso N. Isso usa:

- `user/lab_pending_env.json` (`{"env": {"SOTC_X": "v"}, "boss": N}`): as variáveis pendentes e o
  colosso. O `sotc.py` e o lançador leem esse arquivo na próxima abertura, aplicam uma vez e o
  apagam.
- Alternativa manual: `SOTC_BOOT_BOSS=N` no `sotc.ini` (ou no ambiente), para abrir direto no
  colosso N.

## Canal de controle (pipe)

Com `SOTC_CTL=1` (ou `SOTC_LAB=1`), o jogo abre o canal `\\.\pipe\sotc-ctl`. Você envia uma linha por
comando e recebe uma linha de resposta. Um cliente por vez; ao desconectar, o canal espera o próximo.
`SOTC_CTL=<nome>` troca o nome do pipe para `\\.\pipe\<nome>`.

Comandos (sensíveis a maiúsculas; campos separados por espaço):

| Comando | Resposta / efeito |
|---|---|
| `ping` | `ok pong` |
| `freecam on` / `freecam off` | `ok`, ou `err <motivo>` |
| `goto x y z [yaw pitch]` | `ok` (move a câmera livre e liga a câmera) |
| `speed <m/s>` | `ok` |
| `path load <arquivo>` | carrega uma rota |
| `path play` / `path pause` / `path stop` | reproduz, pausa, volta ao primeiro ponto |
| `path seek <ponto>` | vai ao ponto indicado |
| `path loop on` / `path loop off` | repetição da rota |
| `path save <arquivo>` | grava a rota atual |
| `path record <nome>` | começa uma rota nova (limpa a atual) |
| `path waypoint [segmento]` | adiciona a posição atual da câmera livre |
| `path stoprecord` | para a gravação |
| `boss <1..16> [play\|kill\|outro] [nohorse]` | `ok`, ou `err <motivo>`; `err live-unavailable` significa que é preciso reabrir |
| `engine <linha de console da engine>` | `ok`, ou `err <motivo>` |
| `set NOME=valor` | `ok`, `err needs-restart` ou `err bad-value` |
| `pending NOME=valor` | `ok` (entra na lista do próximo reinício) |
| `relaunch [boss N]` | `ok` (pede ao orquestrador conectado que reinicie) |
| `mark <segmento>` | `ok` (zera a janela de medição e dá nome a ela) |
| `get state` | JSON com posição, câmera, câmera livre, piloto automático, área, pulo pendente, segmento, janela de medição (p50, p99, máximo, travadas), valores ao vivo, pendências e pedido de reinício |
| `get live` | JSON só com os valores ao vivo |
| `quit` | `ok`; fecha só esta conexão |

Exemplo, com o jogo rodando e o `tools/sotc_ctl.py` (que envia um comando e imprime a resposta):

```
python tools/sotc_ctl.py "get state"
python tools/sotc_ctl.py "boss 3 play"
python tools/sotc_ctl.py "set SOTC_EXEMPLO=1"
```

## Rotas

Uma rota é um arquivo JSON com pontos de passagem. Ela é voada em velocidade constante por uma
curva Catmull-Rom centrípeta, com o avanço medido em comprimento de arco. O avanço usa o `dt` do
próprio jogo, então a mesma rota repete as mesmas posições por quadro do jogo.

```json
{
  "name": "ponte",
  "speed": 20.0,
  "start": { "boss": 3 },
  "points": [
    { "p": [10.0, 2.0, -5.0], "yaw": 0.0, "pitch": 0.0, "seg": "ponte", "dwell_ms": 0 },
    { "p": [40.0, 6.0, 12.0], "yaw": 1.2, "pitch": -0.1, "dwell_ms": 500 }
  ]
}
```

- `name`: nome da rota.
- `speed`: velocidade em metros por segundo.
- `start`: onde começar. `{ "boss": N }` pula para o colosso N (1 a 16); `{ "goto": [x, y, z, yaw,
  pitch] }` vai a uma posição; `{}` começa do estado atual.
- `points`: lista de pontos.
  - `p`: posição `[x, y, z]` em coordenadas do mundo do jogo.
  - `yaw` e `pitch`: direção da câmera, em radianos (guinada 0 aponta para +Z do motor; positiva
    vira à esquerda; inclinação positiva olha para cima).
  - `seg`: nome do segmento que começa neste ponto. Vazio significa o mesmo segmento do ponto
    anterior.
  - `dwell_ms`: tempo parado no ponto, em milissegundos.

As rotas ficam em `tools/routes/` (incluídas no código) ou em `user/routes/` (suas). Ao gravar pelo
F4 ou pelo menu, a rota vai para `user/routes/`.

Ao reproduzir, o Lab escreve no log `SOTC SEG <nome> begin` e `SOTC SEG <nome> end` nas fronteiras
de segmento, e `SOTC LAP <n>` a cada volta. Esses marcadores são usados para separar as medições.

### Rota automática

Para gravar uma rota e medir sem tocar no jogo:

```bash
SOTC_LAB=1 SOTC_CVARS="skipSplash=1 nointro" SOTC_LAB_ROUTE=user/routes/minharota.json python sotc.py
```

O jogo abre sem logos e sem tela de título, espera 8 s depois que o jogo começa
(`SOTC_LAB_ROUTE_DELAY`), liga a câmera livre, toca a rota inteira, escreve no log uma linha
`SOTC LAB ROUTE done: ...` com quadros, p50, p99, pior quadro e travadas, espera 5 s
(`SOTC_LAB_ROUTE_EXIT`) e fecha o jogo. Os segmentos da rota aparecem como `SOTC SEG <nome>
begin/end`. A rota `tools/routes/shrine_test.json` (8 s, saindo do Santuário) serve de teste.

## Orquestrador de matrizes

O `tools/sotc_matrix.py` roda uma matriz completa sem intervenção: para cada cenário, cada
configuração e cada volta, ele reinicia o jogo quando preciso, restaura o save de teste, leva o
jogador ao cenário e mede.

```
python tools/sotc_matrix.py --matrix tools/matrices/example.json
python tools/sotc_matrix.py --matrix tools/matrices/example.json --dry-run
python tools/sotc_matrix.py --attach --matrix tools/matrices/example.json
```

- `--dry-run`: mostra o plano (quantos lançamentos, quais trocas ao vivo e quais reinícios) sem
  rodar nada.
- `--attach`: usa um jogo já aberto pelo pipe, em vez de abrir um. É o modo usado pelo menu, na aba
  **Testes**, que inicia o orquestrador como processo filho com essa opção. O comando usado pelo
  menu vem de `SOTC_LAB_ORCH_CMD` (padrão `python tools/sotc_matrix.py`).

Formato da matriz (JSON):

```json
{
  "scenarios": [ { "route": "ponte" }, { "boss": 3 } ],
  "laps": 3,
  "settle_s": 10,
  "base_env": { "SOTC_FPS": "1" },
  "configs": [
    { "name": "base", "env": {} },
    { "name": "teste", "env": { "SOTC_EXEMPLO": "1" } }
  ]
}
```

- `scenarios`: cenários a medir. Cada um é uma rota (`"route"`, nome do arquivo sem a pasta) ou um
  colosso (`"boss"`, 1 a 16).
- `laps`: voltas por cenário e configuração. Use pelo menos 3.
- `settle_s`: segundos de espera, depois de chegar ao cenário e antes de medir, para o streaming
  ficar ocioso.
- `base_env`: variáveis comuns a todas as configurações.
- `configs`: as configurações comparadas. Cada uma tem `name` e `env` (variáveis adicionais, que
  vencem `base_env`).

Saída, em `out/matrix/<data-hora>/`:

- `results.csv`: uma linha por cenário, configuração, volta e segmento, com p50, p99, máximo e as
  travadas acima de 33, 50 e 100 ms.
- `summary.md`: a mediana das voltas (com pelo menos três voltas) por configuração, com a diferença
  em relação à linha de base.

O orquestrador tem limite de tempo por lançamento e detecta jogo travado. Configurações que só mudam
chaves ao vivo não reiniciam o jogo: o orquestrador envia as trocas pelo pipe entre as voltas.

## Chaves de ambiente

| Chave | Efeito |
|---|---|
| `SOTC_LAB=1` | liga o Lab (menu F2, câmera livre, pulo, canal de controle, ajustes ao vivo). Desligado por padrão |
| `SOTC_FREECAM=1` | liga a câmera livre (F3). Ligada por `SOTC_LAB=1` |
| `SOTC_FREECAM_MAXSPEED=<m/s>` | limite de velocidade da câmera livre. O padrão está em `gpu/sotc/freecam.cpp` |
| `SOTC_ENGINE_CMD=1` | liga a execução de comandos da engine (aba Colossos, `engine` no pipe). Ligada por `SOTC_LAB=1` |
| `SOTC_CTL=1` ou `SOTC_CTL=<nome>` | liga o canal de controle no pipe `\\.\pipe\sotc-ctl` (ou `\\.\pipe\<nome>`) |
| `SOTC_CAM_PROBE=1` | só medição: registra a câmera do jogo e a posição do jogador (para o desenvolvimento) |
| `SOTC_TUNING_TSV=<arquivo>` | caminho do `out/tuning.tsv` que o menu lê para montar a aba SOTC_* (o `sotc.py` preenche) |
| `SOTC_BOOT_BOSS=N` | abre o jogo direto no colosso N (1 a 16) |
| `SOTC_LAB_ROUTE=<arquivo>` | toca essa rota sozinho quando o jogo começa e fecha o jogo depois (veja "Rota automática") |
| `SOTC_LAB_ROUTE_DELAY=<s>` | espera depois do início do jogo antes da rota (padrão 8) |
| `SOTC_LAB_ROUTE_EXIT=<s>` | espera depois do fim da rota antes de fechar o jogo (padrão 5; negativo não fecha) |
| `SOTC_FREECAM_DROP=<m>` | quantos metros abaixo da câmera livre o Wander vai (padrão 8) |
| `SOTC_LAB_ORCH_CMD=<comando>` | comando que o menu usa para iniciar o orquestrador (padrão `python tools/sotc_matrix.py`) |

As chaves acima podem ir no `sotc.ini` ou no ambiente do sistema, como as demais.

## Limitações

- **Experimental.** O Lab mexe em partes do jogo que não foram totalmente confirmadas. Para cada
  recurso que depende de um gancho (hook) de engine, o Lab procura o gancho ao iniciar. Se não
  encontrar, o recurso fica desligado e o menu diz isso. Nada que escreve na memória do jogo é
  ativado sem um padrão de memória conhecido.
- **Limite de velocidade medido.** Acima de certa velocidade o streaming não acompanha a câmera,
  e as travadas medidas nesse trecho são artefato do teste. Mantenha as rotas abaixo de
  `SOTC_FREECAM_MAXSPEED`.
- **Medição não determinística.** A leitura de arquivos varia de uma execução para outra. Por isso
  o Lab reporta a **mediana** das voltas (nunca uma volta só) e espera o streaming ficar ocioso antes
  de medir.
- **Pulo para colosso e saves.** Kill e Outro mudam o save. O Lab copia a pasta antes, mas use
  sempre um save de teste (o modelo em `out/save-template/`, usado pelos benchmarks). O pulo também
  pode disparar scripts do jogo.
- **Pulo em série.** Dois pulos seguidos na mesma sessão podem travar a automação da engine. Se
  isso acontecer, o menu oferece reabrir no colosso.
- **Arenas aquáticas 7 e 12.** O passo do cavalo fica desligado nelas.

## Créditos

O pulo para colosso usa a automação de comandos da engine (`shadow_jumpToBoss`, `killBoss`,
`boss_outro` e o gerenciador de automação, com `automate_loadfile` e `automate_go`), descoberta por
**tabnul** para a PR [AjncSystem/ShadowOfTheColossus-PC#345](https://github.com/AjncSystem/ShadowOfTheColossus-PC/pull/345)
(aberta, ainda não mesclada). O Lab reaproveita essa descoberta e a estende para executar comandos
durante a partida, sem reiniciar o jogo a cada pulo.

---

## Para desenvolvedores

### Mapa de arquivos

Código C++ (`gpu/sotc/`):

- `freecam.h`, `freecam.cpp`: câmera livre (estado, entrada, limite de velocidade).
- `autopilot.h`, `autopilot.cpp`: rotas, curva, gravação e reprodução.
- `engine_cmd.h`, `engine_cmd.cpp`: fila de comandos da engine, executada no thread de atualização.
- `sotc_live.h`, `sotc_live.cpp`: registro das chaves ao vivo (`SotcLive::Int`).
- `lab_commands.h`, `lab_commands.cpp`: analisador único de comandos (pipe e menu usam o mesmo).
- `ctl_server.h`, `ctl_server.cpp`: servidor do pipe.
- `probe_cam.cpp`, `probe_cam.h`: sondas da câmera (`SOTC_CAM_PROBE`).
- `lab_layout.h`: offsets e padrões de memória do jogo, preenchidos a partir da engenharia reversa.
- `hook_handlers.cpp`: registra os manipuladores dos ganchos do Lab.

Menu e integração (`gpu/shim/`):

- `sotc_lab_menu.h`, `sotc_lab_menu.cpp`: menu F2 (ImGui), dentro do quadro do overlay.
- `bbport_overlay.cpp`: teclas F2, F3 e F4, e a chamada de desenho do menu.

Ganchos e scripts (`scripts/`):

- `scripts/hooks.d/enginecmd.py` (`SOTC_ENGINE_CMD`), 
  `cam_probe.py` (`SOTC_CAM_PROBE`): declaram os ganchos no `sotc_hooks.py`.
- `scripts/tuning_export.py`: exporta `TUNING` e `LIVE` de `launcher/sotc_tuning.py` para
  `out/tuning.json`. O menu lê esse arquivo, então a lista de chaves continua em um só lugar (Python).

Lançador e testes:

- `launcher/sotc_tuning.py`: a lista `TUNING` e o dicionário `LIVE`.
- `tools/sotc_ctl.py`: cliente de linha de comando do pipe.
- `tools/sotc_matrix.py`: orquestrador. Rotas em `tools/routes/`, matrizes em `tools/matrices/`.
- `tests/test_sotc_live.py`, `tests/test_ctl_protocol.py`, `tests/test_matrix.py`.

Engenharia reversa (`docs/re-stutters/`):

- `engine-commands.md`: comandos e cvars registrados pela engine, e como são executados.
- `freecam-re.md`: a câmera do jogo, o passo de física do Wander e o vetor que controla o streaming.

### Como migrar uma chave para ao vivo

1. Confira as quatro regras de "Trocas ao vivo" acima. Se uma delas não se aplica com certeza, a
   chave continua de reinício.
2. No código, troque a leitura única por um `SotcLive::Int`, definido no escopo do namespace:

   ```cpp
   // antes: lida uma vez, na inicialização
   static const bool kFoo = [] { const char* v = getenv("SOTC_FOO"); ... }();

   // depois: lida a cada uso, com leitura relaxada
   static SotcLive::Int kFoo{"SOTC_FOO", 0};
   ...
   if (kFoo.get()) { ... }
   ```

3. Em `launcher/sotc_tuning.py`, inclua o nome no dicionário `LIVE` (a forma das tuplas de `TUNING`
   não muda).
4. Rode `python -m unittest discover -s tests -p test_sotc_live.py`. O teste confere que `LIVE` é um
   subconjunto de `TUNING`, que cada chave ao vivo aparece no código, e que `out/tuning.json` está
   coerente.
5. Confirme no jogo: a troca deve aparecer no log como `SOTC LIVE <NOME>=<valor>`, sem erro de
   validação.

`SotcLive::Set` aceita a troca em texto (vazio volta ao padrão) e responde `NotLive` para chaves de
reinício. Chaves de reinício ficam em `user/lab_pending_env.json` até a próxima abertura.

### Testes

```
python -m unittest discover -s tests
ninja -C out/gpu bb-probe
```

Os testes do pipe (`test_ctl_protocol.py`) e do orquestrador (`test_matrix.py`) não precisam do jogo.
Os de ao vivo também rodam sem o jogo, pela lista e pelo código. Para verificar no jogo, use
`python tools/sotc_ctl.py "get state"` com o jogo aberto.
