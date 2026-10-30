"""Tuning switches of the streaming-stutter work (SOTC_* environment variables), for the
launcher's "Display & FPS" page. Each row: (name, kind, group, (label pt, en), (hint pt, en),
default text of a typed value, choice options).

kind: "flag" off unless checked (sets NAME=1); "flag_on" on unless unchecked (sets NAME=0);
"choice" one of the options, the first being the default (unset); "value" typed text, shown
filled with its default (empty when the default has no single value). A switch at its default
(or a value left empty) sets nothing, so the game sees the build's own default.
The launcher's "Extra variables" field is applied after these and wins.
"""

GROUPS = [
    ('bind', ('Bind e desenho', 'Bind and draw')),
    ('memory', ('Memória e sincronização', 'Memory and sync')),
    ('readback', ('Leituras da GPU (readbacks)', 'GPU readbacks')),
    ('upload', ('Envios e texturas', 'Uploads and textures')),
    ('threads', ('Threads e CPU', 'Threads and CPU')),
    ('present', ('Apresentação e latência', 'Presentation and latency')),
    ('diag', ('Diagnóstico (só medição)', 'Diagnostics (measuring only)')),
]

TUNING = [('SOTC_ADAPTIVE_SHADOW',
  'flag_on',
  'bind',
  ('Sombras distantes alternadas durante o alívio', 'Alternate distant shadows during relief'),
  ('Ligado por padrão. Com “Controle de carga adaptativo (experimental)”, enquanto o alívio está '
   'ativo, as cascatas distantes do sol são desenhadas em quadros alternados. Desligue para '
   'desenhá-las todo quadro.',
   'On by default. With "Adaptive load control (experimental)", while relief is active the distant '
   'sun shadow cascades are drawn on alternate frames. Turn off to draw them every frame.'),
  '',
  ()),
 ('SOTC_BINDLESS_SAMP',
  'flag',
  'bind',
  ('Samplers em heap global (bindless)', 'Bindless samplers (global heap)'),
  ('Desligado por padrão. Põe os samplers num heap global do Vulkan, se a placa suportar; senão '
   'segue o caminho normal. Refaz o cache de shaders. Não combina com “Vínculos de recursos por '
   'estágio” nem “Descritores com buffer (Vulkan)”.',
   'Off by default. Puts samplers in one global Vulkan heap (if the GPU supports it; otherwise the '
   'normal path). Rebuilds the shader cache. Not combined with "Per-stage resource bindings" or '
   '"Descriptor buffers (Vulkan)".'),
  '',
  ()),
 ('SOTC_BIND_CACHE',
  'flag',
  'bind',
  ('Cache de binds para desenhos de leitura', 'Bind cache for read-only draws'),
  ('Desligado por padrão (experimental). Guarda os recursos dos últimos 32 desenhos que só leem '
   'buffers e texturas; um desenho com o mesmo estado no mesmo envio reaproveita as buscas. Sem '
   'medição isolada.',
   'Off by default (experimental). Keeps the resources of the last 32 draws that only read buffers '
   'and textures; a draw with the same state in the same submit reuses the lookups. No separate '
   'measurement.'),
  '',
  ()),
 ('SOTC_BIND_CACHE_FRAME',
  'flag',
  'bind',
  ('Cache de binds que dura o quadro inteiro', 'Bind cache that lasts the whole frame'),
  ('Desligado por padrão. Mantém o cache de binds pelo quadro inteiro. Precisa de “Cache de binds '
   'que atravessa o envio” (com “Cache de binds para desenhos de leitura” e “Descartar binds só '
   'pelas faixas escritas”). Medido: dobrou o custo da thread de desenho.',
   'Off by default. Keeps the bind cache for the whole frame. Needs "Bind cache that crosses '
   'submits" (with "Bind cache for read-only draws" and "Drop binds only for written ranges"). '
   'Measured: doubled the draw thread cost.'),
  '',
  ()),
 ('SOTC_BIND_CACHE_RANGES',
  'flag',
  'bind',
  ('Descartar binds só pelas faixas escritas', 'Drop binds only for written ranges'),
  ('Desligado por padrão. Só vale com “Cache de binds para desenhos de leitura”. Quando a GPU '
   'escreve memória, descarta só as entradas que leem essa faixa, em vez de todo o cache. Sem '
   'medição isolada.',
   'Off by default. Only with "Bind cache for read-only draws". When the GPU writes memory, it '
   'drops only the entries that read that range, not the whole cache. No separate measurement.'),
  '',
  ()),
 ('SOTC_BIND_CACHE_XTICK',
  'flag',
  'bind',
  ('Cache de binds que atravessa o envio', 'Bind cache that crosses submits'),
  ('Desligado por padrão. Exige “Cache de binds para desenhos de leitura” e “Descartar binds só '
   'pelas faixas escritas”. O cache deixa de ser zerado a cada envio à GPU. Nunca foi medido '
   'sozinho.',
   'Off by default. Requires "Bind cache for read-only draws" and "Drop binds only for written '
   'ranges". The cache is no longer cleared at every submit to the GPU. Never measured on its '
   'own.'),
  '',
  ()),
 ('SOTC_BIND_MARK_ONCE',
  'flag',
  'bind',
  ('Marcar escrita de bind uma só vez', 'Mark bind writes only once'),
  ('Desligado por padrão. Tira uma marcação de escrita repetida por bind escrito. Medido junto com '
   'outros switches: sem mudança mensurável (19,0 contra 19,2 ms).',
   'Off by default. Removes one repeated write mark per written bind. Measured with other '
   'switches: no measurable change (19.0 vs 19.2 ms).'),
  '',
  ()),
 ('SOTC_BUFFER_MEMO',
  'flag',
  'bind',
  ('Memória de buffers do cache de binds', 'Buffer memo for the bind cache'),
  ('Desligado por padrão. É a parte de buffers do “Cache de binds que dura o quadro inteiro”; '
   'exige “Cache de binds para desenhos de leitura”. Acerta 96–99% dos buffers, mas a thread de '
   'desenho não melhorou: os binds pulados já eram baratos.',
   'Off by default. The buffer part of "Bind cache that lasts the whole frame"; needs "Bind cache '
   'for read-only draws". Hits 96-99% of buffers, but the draw thread did not improve: the skipped '
   'binds were already cheap.'),
  '',
  ()),
 ('SOTC_DEDUP_PUSH',
  'flag',
  'bind',
  ('Não reenviar constantes repetidas', 'Skip repeated push constants'),
  ('Experimental, desligado: pula o envio de constantes de desenho que não mudaram. Também fica '
   'desligado se “Não reenviar o formato dos vértices” estiver desmarcado. Sem medição registrada '
   'no changelog.',
   'Experimental, off: skips sending draw constants that did not change. Also off when "Skip '
   'resending the vertex format" is unchecked. No measurement recorded in the changelog.'),
  '',
  ()),
 ('SOTC_DEDUP_STATE',
  'flag_on',
  'bind',
  ('Não reenviar o formato dos vértices', 'Skip resending the vertex format'),
  ('Padrão: ligado. O formato dos vértices não é reenviado quando não mudou. Com 0, é enviado em '
   'todo desenho, e o envio de constantes repetidas também desliga.',
   'Default on: the vertex format is not resent when unchanged. With 0 it is sent on every draw, '
   'and repeated constant skipping is also off.'),
  '',
  ()),
 ('SOTC_DEPTH_SYNC_PIPE',
  'flag',
  'bind',
  ('Compilar pipelines de sombra na hora', 'Build shadow pipelines on the spot'),
  ('Desligado desde 27/10: compilar na hora causava travadas de 0,4 a 3,8 s (cerca de 36 ms por '
   'compilação). Ligado, os passes de sombra nunca são pulados.',
   'Off since 27 Oct: building on the spot caused hitches of 0.4 to 3.8 s (about 36 ms per build). '
   'On: shadow passes are never skipped.'),
  '',
  ()),
 ('SOTC_DESC_BUFFER',
  'flag',
  'bind',
  ('Descritores com buffer (Vulkan)', 'Descriptor buffers (Vulkan)'),
  ('Experimental, desligado: usa a extensão de buffers de descritores da Vulkan. Se o driver não '
   'tiver a extensão ou o recurso, o caminho atual continua em uso.',
   'Experimental, off: uses the Vulkan descriptor buffer extension. If the driver lacks the '
   'extension or feature, the current path stays in use.'),
  '',
  ()),
 ('SOTC_FASTBIND_WIDE',
  'flag',
  'bind',
  ('Reusar bind também em envios posteriores', 'Reuse bind in later submits too'),
  ('Experimental, desligado: um bind reaproveitado também vale num envio posterior, pulando as '
   'buscas. Desliga “Enviar texturas pela cópia do host”. Sem medição registrada no changelog.',
   'Experimental, off: a reused bind also counts in a later submit, skipping lookups. Turns off '
   '"Upload textures by host copy". No measurement recorded in the changelog.'),
  '',
  ()),
 ('SOTC_HLE_SHADOW_CULL',
  'flag',
  'bind',
  ('Cull de sombras calculado na CPU', 'Shadow cull computed on the CPU'),
  ('Experimental, desligado: o cull de sombras roda na CPU, no ponto do fluxo de comandos, em vez '
   'da GPU. Medido: não agiu, porque as entradas do shader são escritas pela GPU.',
   'Experimental, off: the shadow cull runs on the CPU at its place in the command stream, not on '
   'the GPU. Measured: no effect, because the shader inputs are written by the GPU.'),
  '',
  ()),
 ('SOTC_INVAL_INDEX',
  'flag',
  'bind',
  ('Invalidação de texturas por página', 'Texture invalidation by page'),
  ('Ao escrever um bind, só procura texturas a invalidar quando há alguma nas páginas dele. '
   'Desligado por padrão; junto com outros, não mudou nada medido (19,0 contra 19,2 ms).',
   'On a written bind, only looks for textures to invalidate when some are on its pages. Off by '
   'default; with four others, no measurable change (19.0 vs 19.2 ms).'),
  '',
  ()),
 ('SOTC_LAYOUT_CACHE',
  'flag',
  'bind',
  ('Cache de layouts de descritores', 'Descriptor layout cache'),
  ('Compartilha os layouts de descritores entre pipelines. Também liga com “Vínculos de recursos '
   'por estágio”. Desligado por padrão; experimental, sem medição registrada.',
   'Shares descriptor set layouts between pipelines. Also turns on with "Per-stage resource '
   'bindings". Off by default; experimental, no measurement recorded.'),
  '',
  ()),
 ('SOTC_PR_RESOLVE',
  'flag',
  'bind',
  ('Descritores calculados antes da thread de desenho',
   'Descriptors resolved before the draw thread'),
  ('Experimental: as threads de preparação já calculam os descritores de buffer e textura dos '
   'desenhos. Precisa de “Preparação de desenhos em paralelo”. Medido: divergiu 12 vezes numa '
   'sessão; não usar.',
   'Experimental: the preparation threads already compute the buffer and texture descriptors of '
   'each draw. Needs "Parallel draw preparation". Measured: diverged 12 times in one session; do '
   'not use.'),
  '',
  ()),
 ('SOTC_REG_SAME',
  'flag',
  'bind',
  ('Ignorar escrita de registrador repetida', 'Skip repeated register writes'),
  ('Uma escrita de registrador que não muda o valor deixa de marcar o bloco como alterado, e o '
   'bloco não é copiado de novo. Desligado por padrão. Medido com outros cortes do anel: sem '
   'ganho.',
   'A register write that does not change the value no longer marks its block as changed, so the '
   'block is not copied again. Off by default. Measured together with other ring cuts: no gain.'),
  '',
  ()),
 ('SOTC_RS_MEMO',
  'choice',
  'bind',
  ('Reaproveitar alvos de render', 'Reuse render targets'),
  ('Reaproveita o estado de render do desenho anterior quando ele usa os mesmos alvos. Desligado '
   'por padrão, experimental. O modo 2 vale entre envios; o modo 3 confere cada caso com o caminho '
   'completo, custando tempo.',
   "Reuses the previous draw's render state when it uses the same render targets. Off by default, "
   'experimental. Mode 2 also works across submits; mode 3 checks each case against the full path, '
   'costing time.'),
  '',
  (('', ('Desligado', 'Off')),
   ('1', ('Ligado: mesmo envio à GPU', 'On: same GPU submit')),
   ('2', ('Ligado: também entre envios', 'On: also across submits')),
   ('3', ('Verifica: compara com o caminho completo', 'Verify: compare with the full path')))),
 ('SOTC_SRT_PHI',
  'flag_on',
  'bind',
  ('Resolver valores de ramos nos shaders', 'Resolve branch values in shaders'),
  ('Deixa o tradutor de shaders resolver valores de ramos simples ao localizar os recursos de um '
   'shader. Ligado por padrão; com 0 volta ao caminho antigo. Mudar refaz o cache de shaders.',
   "Lets the shader translator resolve simple branch values when locating a shader's resources. On "
   'by default; 0 goes back to the old path. Changing it rebuilds the shader cache.'),
  '',
  ()),
 ('SOTC_STAGE_BINDINGS',
  'flag',
  'bind',
  ('Vínculos de recursos por estágio', 'Per-stage resource bindings'),
  ('Muda como as texturas e os buffers de cada estágio do shader são vinculados. Desligado por '
   'padrão, experimental. Refaz o cache de shaders; não combina com “Samplers em heap global '
   '(bindless)”.',
   'Changes how textures and buffers of each shader stage are bound. Off by default, experimental. '
   'Rebuilds the shader cache; does not combine with "Bindless samplers (global heap)".'),
  '',
  ()),
 ('SOTC_TEX_BLOCK',
  'choice',
  'bind',
  ('Reaproveitar blocos de texturas', 'Reuse texture blocks'),
  ('Reaproveita as texturas de cada estágio enquanto as imagens e as visões não mudaram. Desligado '
   'por padrão, experimental. O modo 2 roda também o caminho completo e compara, custando tempo.',
   "Reuses each stage's textures while the images and views have not changed. Off by default, "
   'experimental. Mode 2 also runs the full path and compares, costing time.'),
  '',
  (('', ('Desligado', 'Off')),
   ('1', ('Ligado: reaproveita blocos de texturas', 'On: reuse texture blocks')),
   ('2', ('Verifica: compara com o caminho completo', 'Verify: compare with the full path')))),
 ('SOTC_ARENA_TICK_FREE',
  'flag',
  'memory',
  ('Reaproveitar memória de vídeo só depois da GPU', 'Reuse video memory only after the GPU'),
  ('Desligado por padrão (experimental). Blocos de memória de vídeo liberados só são '
   'reaproveitados depois que a GPU já passou pelo ponto em que foram soltos. Também liga com '
   '“Desvincular memória sem esperar a GPU”.',
   'Off by default (experimental). Freed video memory blocks are reused only after the GPU has '
   'passed the point where they were released. Also on with "Unbind memory without waiting for the '
   'GPU".'),
  '',
  ()),
 ('SOTC_CARVE_NOSTOP',
  'flag',
  'memory',
  ('Dividir memória sem parar o jogo', 'Split memory without stopping the game'),
  ('Desligado por padrão (experimental). Ao entrar em área nova, o jogo divide memória sem parar '
   'as outras threads quando é seguro. Sem resultado medido registrado. Veja “Diagnóstico: '
   'registrar divisões de memória” para as divisões que ainda param.',
   'Off by default (experimental). When entering a new area, the game splits memory without '
   'stopping other threads when it is safe. No measured result recorded. See "Diagnostic: log '
   'memory view splits" for splits that still stop.'),
  '',
  ()),
 ('SOTC_DROP_ASYNC',
  'flag',
  'memory',
  ('Liberar memória sem esperar a thread de desenho',
   'Release memory without waiting for the draw thread'),
  ('Experimental, desligado: ao liberar memória, o jogo espera a thread de desenho. Ligado, não '
   'espera, mas uma leitura antiga pode copiar bytes velhos sobre o que o jogo escreveu, deixando '
   'vértices quebrados.',
   'Experimental, off: releasing memory waits for the draw thread. On: no wait, but an old '
   'readback can copy stale bytes over what the game wrote, leaving broken vertices.'),
  '',
  ()),
 ('SOTC_DROP_FAST',
  'flag',
  'memory',
  ('Liberar memória que a GPU só leu, sem espera', 'Fast release of memory the GPU only read'),
  ('Experimental, desligado: liberar memória que a GPU só leu não espera a thread de desenho. '
   'Medido: menos travadas nas cargas (o pior quadro caiu de 417 ms para 125–185 ms, e as esperas '
   'ao liberar memória de ~9 s para menos de 50 ms por sessão). ATENÇÃO: em 1 de 4 testes o jogo '
   'fechou sozinho ao carregar o save; a causa ainda não foi encontrada.',
   'Experimental, off: releasing memory the GPU only read does not wait for the draw thread. '
   'Measured: fewer hitches while loading (worst frame from 417 ms to 125-185 ms, memory-release '
   'waits from ~9 s to under 50 ms per session). WARNING: in 1 of 4 tests the game closed on its '
   'own while loading the save; the cause is not found yet.'),
  '',
  ()),
 ('SOTC_FAULT_GRANULE',
  'choice',
  'memory',
  ('Bloco invalidado por falha de escrita', 'Block invalidated per write fault'),
  ('Uma falha de escrita também invalida as outras páginas do bloco alinhado, que então não falham '
   'de novo. Padrão: uma página (4 KiB). Os blocos maiores são experimentais.',
   'A write fault also invalidates the other pages of the aligned block, so later writes there do '
   'not fault again. Default: one page (4 KiB). Larger blocks are experimental.'),
  '',
  (('', ('4 KiB (uma página, padrão)', '4 KiB (one page, default)')),
   ('16384', ('16 KiB', '16 KiB')),
   ('65536', ('64 KiB', '64 KiB')))),
 ('SOTC_GPUMOD_CONTAINS',
  'flag',
  'memory',
  ('Checagem sem trava para faixa já escrita', 'Lock-free check for an already written range'),
  ('Experimental, desligado: um bind numa faixa já marcada como escrita pela GPU usa trava '
   'compartilhada, não exclusiva. Medido junto com outros ajustes: sem mudança mensurável.',
   'Experimental, off: a bind in a range already marked as GPU-written takes a shared lock, not an '
   'exclusive one. Measured with other tweaks: no measurable change.'),
  '',
  ()),
 ('SOTC_HEAP_GUARD_RUNS',
  'flag',
  'memory',
  ('Reaproveitar localizações do heap no readback', 'Reuse heap locations during readbacks'),
  ('Experimental, desligado: a proteção do heap do jogo durante os readbacks reaproveita onde fica '
   'cada pedaço de memória enquanto o mapa não muda. Evita refazer isso ~100 vezes por segundo.',
   'Experimental, off: the game heap guard during readbacks reuses where each memory piece lives '
   'while the memory map does not change. Avoids redoing this about 100 times per second.'),
  '',
  ()),
 ('SOTC_HLE_SKIP',
  'choice',
  'memory',
  ('Marcas de cópias de shader emulado', 'Marks of emulated shader copies'),
  ('Experimental: as cópias do shader de cópia emulado marcam o destino uma vez por faixa juntada, '
   'não uma vez por cópia. Com 2, confere se o resultado bate. Pular as cópias inteiras não foi '
   'feito.',
   'Experimental: the emulated copy shader marks its destination once per merged range, not once '
   'per copy. With 2 it checks the result. Skipping whole copies was not done.'),
  '',
  (('', ('Desligado (padrão)', 'Off (default)')),
   ('1', ('Juntar marcas por faixa', 'Merge marks per range')),
   ('2', ('Juntar e verificar', 'Merge and verify')))),
 ('SOTC_HOT_PAGES',
  'flag',
  'memory',
  ('Páginas quentes sem proteção por um tempo', 'Hot pages briefly left unprotected'),
  ('Experimental, desligado: páginas que o jogo reescreve a cada quadro ficam sem proteção por um '
   'tempo, em vez de gerar uma falha por quadro. Não recomendado: no teste piorou as travadas.',
   'Experimental, off: pages the game rewrites every frame stay unprotected for a while, instead '
   'of faulting each frame. Not recommended: in testing it worsened hitches.'),
  '',
  ()),
 ('SOTC_HOT_PAGES_ADAPT',
  'flag_on',
  'memory',
  ('Limite de páginas quentes automático', 'Automatic hot page limit'),
  ('Padrão: ligado. O limite de páginas quentes sobe quando falta espaço e desce quando sobra, até '
   'o teto. Desligado, fica fixo no valor de “Número máximo de páginas quentes”. Precisa de '
   '“Páginas quentes sem proteção por um tempo”.',
   'Default on: the hot page limit grows when space runs out and shrinks when unused, up to the '
   'ceiling. Off: fixed at "Maximum number of hot pages". Needs "Hot pages briefly left '
   'unprotected".'),
  '',
  ()),
 ('SOTC_HOT_PAGES_MAX',
  'value',
  'memory',
  ('Número máximo de páginas quentes', 'Maximum number of hot pages'),
  ('Quantas páginas quentes podem existir ao mesmo tempo, em todas as regiões de memória. Padrão: '
   '1024. Com o ajuste automático, o limite cresce até o teto de “Teto do limite de páginas '
   'quentes”.',
   'How many hot pages may exist at once across all memory regions. Default: 1024. With automatic '
   'adjustment the limit grows up to "Hot page limit ceiling".'),
  '1024',
  ()),
 ('SOTC_HOT_PAGES_MAX_CEIL',
  'value',
  'memory',
  ('Teto do limite de páginas quentes', 'Hot page limit ceiling'),
  ('Maior valor que o limite automático de páginas quentes pode atingir. Padrão: 16384 (64 MiB). '
   'Nunca fica abaixo de “Número máximo de páginas quentes”.',
   'Highest value the automatic hot page limit can reach. Default: 16384 (64 MiB). Never below '
   '"Maximum number of hot pages".'),
  '16384',
  ()),
 ('SOTC_HOT_PAGES_MS',
  'value',
  'memory',
  ('Tempo de vida de página quente (ms)', 'Hot page lifetime (ms)'),
  ('Tempo, em milissegundos, que uma página quente fica sem proteção (de 100 a 60000). Padrão: '
   '2000. Só tem efeito com “Páginas quentes sem proteção por um tempo” ligado.',
   'Milliseconds a hot page stays unprotected (100 to 60000). Default: 2000. Only has effect with '
   '"Hot pages briefly left unprotected" on.'),
  '2000',
  ()),
 ('SOTC_HOT_PAGES_WINDOW_MS',
  'value',
  'memory',
  ('Janela para marcar página quente (ms)', 'Window to mark a hot page (ms)'),
  ('Se uma página sofre nova falha de escrita dentro desta janela (de 5 a 1000 ms), ela vira '
   'quente. Padrão: 50. Só tem efeito com “Páginas quentes sem proteção por um tempo” ligado.',
   'If a page takes another write fault within this window (5 to 1000 ms), it becomes hot. '
   'Default: 50. Only has effect with "Hot pages briefly left unprotected" on.'),
  '50',
  ()),
 ('SOTC_PROTECT_BATCH',
  'flag',
  'memory',
  ('Re-proteções de páginas em lote', 'Batched page re-protection'),
  ('Experimental: junta as re-proteções de páginas de um upload e as aplica de uma vez, antes de o '
   'upload ler a memória do jogo. Desligado por padrão; sem medição registrada.',
   'Experimental: batches the page re-protections of one upload and applies them at once, before '
   'the upload reads game memory. Off by default; no measurement recorded.'),
  '',
  ()),
 ('SOTC_PROTECT_COALESCE',
  'flag',
  'memory',
  ('Menos chamadas de proteção de página', 'Fewer page protection calls'),
  ('Numa falta de escrita, junta as liberações de páginas vizinhas numa chamada só, e pode '
   'atravessar pequenos intervalos (ver “Intervalo de páginas atravessado”). Desligado por padrão; '
   'sem medição registrada.',
   'On a write fault, merges neighboring page releases into one call, and can bridge small gaps '
   '(see "Page gap bridged"). Off by default; no measurement recorded.'),
  '',
  ()),
 ('SOTC_PROTECT_GAP',
  'value',
  'memory',
  ('Intervalo de páginas atravessado', 'Page gap bridged'),
  ('Com “Menos chamadas de proteção de página” ligado, quantas páginas de intervalo entre duas '
   'proteções iguais podem ser juntadas numa chamada (0 a 64). Padrão 8; sem efeito sem o outro '
   'switch.',
   'With "Fewer page protection calls" on, how many pages of gap between two equal protections can '
   'be bridged in one call (0 to 64). Default 8; no effect without the other switch.'),
  '8',
  ()),
 ('SOTC_SYNC_FAST',
  'flag',
  'memory',
  ('Pular varredura de memória sem mudanças', 'Skip scanning memory that has not changed'),
  ('Antes de sincronizar cada buffer, pula a varredura de um trecho de memória que não mudou desde '
   'a última vez. Desligado por padrão, experimental. Contadores com “FPS no registro” (aba '
   'Avançado).',
   'Before syncing each buffer, skips scanning a memory range that has not changed since the last '
   'sync. Off by default, experimental. Counters with "FPS in the log" (Advanced page).'),
  '',
  ()),
 ('SOTC_SYNC_PIPE',
  'choice',
  'memory',
  ('Sincronização de memória em uma passada', 'Single-pass memory sync'),
  ('Varre cada trecho limpo uma só vez na sincronização e atualiza os bits de leitura de shader '
   'por palavra. Desligado por padrão. Precisa de “Pular varredura de memória sem mudanças”. O '
   'modo 2 confere e conta divergências.',
   'Scans each clean range once during sync and updates shader read-page bits one word at a time. '
   'Off by default. Needs "Skip scanning memory that has not changed". Mode 2 checks results and '
   'counts mismatches.'),
  '',
  (('', ('Desligado', 'Off')),
   ('1', ('Ligado', 'On')),
   ('2', ('Verifica os resultados', 'Verify results')))),
 ('SOTC_SYNC_PRECHECK',
  'flag',
  'memory',
  ('Checagem rápida antes da sincronização', 'Quick check before memory sync'),
  ('Confere sem bloquear se um buffer já está limpo, antes da varredura completa. Desligado por '
   'padrão. Precisa de “Pular varredura de memória sem mudanças”. Medido: sem diferença (thread de '
   'desenho 19,0 contra 19,2 ms).',
   'Checks without locking whether a buffer is already clean, before the full scan. Off by '
   'default. Needs "Skip scanning memory that has not changed". Measured: no difference (draw '
   'thread 19.0 vs 19.2 ms).'),
  '',
  ()),
 ('SOTC_UNBIND_GPU_WAIT',
  'flag',
  'memory',
  ('Desvincular memória sem esperar a GPU', 'Unbind memory without waiting for the GPU'),
  ('Ao liberar memória de vídeo, não espera a GPU terminar antes de desvincular; a ordem fica a '
   'cargo da própria GPU. Desligado por padrão, experimental.',
   'When freeing video memory, does not wait for the GPU to finish before unbinding; ordering is '
   'left to the GPU. Off by default, experimental.'),
  '',
  ()),
 ('SOTC_UNTRACK_AUTO',
  'flag',
  'memory',
  ('Deixar sem proteção a memória temporária', 'Leave temporary memory unprotected'),
  ('Deixa sem proteção os blocos de memória temporária do jogo, achados sozinhos. Desligado por '
   'padrão. Medido (29/10): não reduziu as faltas de escrita e a thread de desenho foi de 19,2 '
   'para 22,1 ms.',
   "Leaves the game's temporary memory blocks unprotected, found automatically. Off by default. "
   'Measured (Oct 29): did not reduce write faults and the draw thread went from 19.2 to 22.1 ms.'),
  '',
  ()),
 ('SOTC_UNTRACK_MAX_MB',
  'value',
  'memory',
  ('Limite de memória sem proteção (MB)', 'Unprotected memory cap (MB)'),
  ('Tamanho máximo, em MB, das faixas que “Deixar sem proteção a memória temporária” deixa sem '
   'proteção. Padrão 16. Só age com “Deixar sem proteção a memória temporária”.',
   'Maximum size in MB of the ranges that "Leave temporary memory unprotected" leaves unprotected. '
   'Default 16. Only has effect with "Leave temporary memory unprotected".'),
  '16',
  ()),
 ('SOTC_UNTRACK_RANGES',
  'value',
  'memory',
  ('Faixas de memória sem proteção', 'Unprotected memory ranges'),
  ('Faixas fixas, em hexadecimal, no formato endereço:tamanho separadas por vírgula, que nunca são '
   'protegidas. Vazio não usa nenhuma. Desligado quando “Deixar o processador de comandos '
   'adiantar” está ligado.',
   'Fixed ranges in hex, written address:size and separated by commas, that are never protected. '
   'Empty uses none. Turned off when "Let the command processor run ahead" is set.'),
  '',
  ()),
 ('SOTC_VIEW_CHUNK_KB',
  'value',
  'memory',
  ('Tamanho das visões de memória (KiB)', 'Memory view size (KiB)'),
  ('Tamanho, em KiB, das visões em que a memória do jogo é mapeada, mínimo 64. 0 (padrão) usa 2 '
   'MiB. Experimental: com 2 MiB, a troca de área parava o jogo por até 87 ms.',
   "Size in KiB of the views the game's memory is mapped in, minimum 64. 0 (default) uses 2 MiB. "
   'Experimental: with 2 MiB, area changes stopped the game for up to 87 ms.'),
  '0',
  ()),
 ('SOTC_VIEW_SPLIT_FINE',
  'flag',
  'memory',
  ('Dividir visões de memória em partes finas', 'Split memory views into fine pieces'),
  ('Quando o jogo libera parte de uma área de memória, as partes restantes viram visões de 64 KiB, '
   'e as próximas liberações não param todas as threads do jogo. Desligado por padrão, '
   'experimental.',
   'When the game frees part of a memory area, the remaining parts become 64 KiB views, so later '
   'frees no longer stop every game thread. Off by default, experimental.'),
  '',
  ()),
 ('SOTC_WRITE_AHEAD_ADAPT',
  'flag',
  'memory',
  ('Janela de escrita adaptativa', 'Adaptive write-ahead window'),
  ('Quando uma thread do jogo escreve memória em sequência, aumenta a janela de páginas marcadas à '
   'frente, até o limite. Desligado por padrão. Medido com outros switches: 30% menos faltas, '
   'custo por falta dobrado.',
   'When a game thread writes memory in sequence, grows the window of pages marked ahead, up to '
   'the cap. Off by default. Measured with other switches: 30% fewer faults, double cost per '
   'fault.'),
  '',
  ()),
 ('SOTC_WRITE_AHEAD_ADAPT_US',
  'value',
  'memory',
  ('Intervalo para escrita em sequência (µs)', 'Sequential write interval (µs)'),
  ('Tempo máximo, em microssegundos, entre o fim de uma falta de escrita e a seguinte para contar '
   'como sequencial. Padrão 1000. Só age com “Janela de escrita adaptativa”.',
   'Longest time, in microseconds, between the end of one write fault and the next to count as '
   'sequential. Default 1000. Only has effect with "Adaptive write-ahead window".'),
  '1000',
  ()),
 ('SOTC_WRITE_AHEAD_MAX_PAGES',
  'value',
  'memory',
  ('Janela máxima de escrita (páginas)', 'Maximum write-ahead window (pages)'),
  ('Limite, em páginas de 4 KB, da janela de escrita adaptativa. Padrão 512 (2 MiB); aceita de 16 '
   'a 4096. Só age com “Janela de escrita adaptativa”.',
   'Limit, in 4 KB pages, of the adaptive write window. Default 512 (2 MiB); accepts 16 to 4096. '
   'Only has effect with "Adaptive write-ahead window".'),
  '512',
  ()),
 ('SOTC_WRITE_EXTENT',
  'flag',
  'memory',
  ('Estreitar faixa escrita por shaders listados', 'Narrow written range for listed shaders'),
  ('Reduz a faixa de memória marcada como escrita pela GPU para os shaders listados no arquivo de '
   'regras. Desligado por padrão, experimental. Precisa do arquivo de regras (“Arquivo de regras '
   'de escrita da GPU”).',
   'Narrows the memory range marked as GPU-written for the shaders listed in the rules file. Off '
   'by default, experimental. Needs the rules file ("GPU write rules file").'),
  '',
  ()),
 ('SOTC_WRITE_EXTENT_FILE',
  'value',
  'memory',
  ('Arquivo de regras de escrita da GPU', 'GPU write rules file'),
  ('Arquivo com as regras de “Estreitar faixa escrita por shaders listados”, uma por linha, com # '
   'para comentário. Vazio usa user/write_extent.txt. Só age com “Estreitar faixa escrita por '
   'shaders listados”.',
   'File with the "Narrow written range for listed shaders" rules, one per line, # starts a '
   'comment. Empty uses user/write_extent.txt. Only has effect with "Narrow written range for '
   'listed shaders".'),
  '',
  ()),
 ('SOTC_WRITE_FAULT_NARROW',
  'flag',
  'memory',
  ('Falta de escrita lê só a página tocada', 'Write fault reads back only the touched page'),
  ('Ao escrever numa página que a GPU alterou, o jogo lê de volta só a página da falta, e não uma '
   'janela de 512 KiB. Desligado por padrão. Medido em 29/10, antes da reescrita: dobrou as '
   'leituras.',
   'When the game writes to a page the GPU changed, only the faulting page is read back, not a 512 '
   'KiB window. Off by default. Measured Oct 29, before the rewrite: doubled the readbacks.'),
  '',
  ()),
 ('SOTC_HRR',
  'choice',
  'readback',
  ('Faixas residentes no host', 'Host-resident ranges'),
  ('Aprende as faixas de memória que o jogo relê e pode movê-las para memória visível ao host. '
   "'dry' só simula e mede; '1' promove de verdade, ainda sem resultado medido. Desligado por "
   'padrão.',
   "Learns which memory ranges the game reads back and can move them to host-visible memory. 'dry' "
   "only simulates and measures; '1' promotes for real, with no measured result yet. Off by "
   'default.'),
  '',
  (('', ('Desligado', 'Off')),
   ('dry', ('Só simulação (diagnóstico)', 'Dry run only (diagnostic)')),
   ('1', ('Promoção real (experimental)', 'Real promotion (experimental)')))),
 ('SOTC_HRR_MB',
  'value',
  'readback',
  ('Limite de faixas residentes (MiB)', 'Resident ranges limit (MiB)'),
  ('Quanto, em MiB, as faixas escolhidas podem somar. Só tem efeito com “Faixas residentes no '
   'host” ligado. Padrão 16.',
   'How much, in MiB, the chosen ranges can add up to. Only has an effect with "Host-resident '
   'ranges" on. Default 16.'),
  '16',
  ()),
 ('SOTC_HRR_MIN_RB',
  'value',
  'readback',
  ('Leituras por segundo para escolher faixa', 'Readbacks per second to pick a range'),
  ('Quantas vezes por segundo uma faixa de 64 KiB precisa ser lida de volta para virar candidata. '
   'Só tem efeito com “Faixas residentes no host” ligado. Padrão 30.',
   'How many times per second a 64 KiB range must be read back to become a candidate. Only has an '
   'effect with "Host-resident ranges" on. Default 30.'),
  '30',
  ()),
 ('SOTC_RB_ASYNC_AHEAD',
  'flag',
  'readback',
  ('Leitura da GPU sem esperar o envio atual', 'GPU read without waiting for the current submit'),
  ('Experimental: quando o jogo lê memória escrita pela GPU, a cópia vai num envio à parte, sem '
   'esperar os uploads do envio atual. Desligado por padrão; sem medição registrada.',
   'Experimental: when the game reads GPU-written memory, the copy goes in a separate submit, '
   "without waiting for the current submit's uploads. Off by default; no measurement recorded."),
  '',
  ()),
 ('SOTC_RB_BATCH',
  'flag',
  'readback',
  ('Leituras pedidas juntas num envio', 'Readbacks batched in one submit'),
  ('Experimental: as leituras pedidas juntas vão para a GPU num só envio. Desligado por padrão; '
   'sem medição registrada.',
   'Experimental: readbacks requested together go to the GPU in a single submit. Off by default; '
   'no measurement recorded.'),
  '',
  ()),
 ('SOTC_RB_EXACT',
  'choice',
  'readback',
  ('Leituras exatas da GPU', 'Exact GPU readbacks'),
  ('Experimental: as leituras da GPU entregam só o que ela já escreveu, sem espera extra. Precisa '
   'de “Dono único das páginas em leitura”. Medido: piorou (thread de desenho de 22 para 35 ms por '
   'quadro).',
   'Experimental: GPU readbacks deliver only what the GPU has already written, with no extra wait. '
   'Needs "Single owner for pages being read". Measured: worse (draw thread from 22 to 35 ms per '
   'frame).'),
  '',
  (('', ('Desligado', 'Off')),
   ('1', ('Ligado (experimental)', 'On (experimental)')),
   ('publish', ('Versão enxuta (publish)', 'Lean version (publish)')))),
 ('SOTC_RB_GROUP',
  'flag',
  'readback',
  ('Leituras em grupo antecipadas', 'Early group readbacks'),
  ('Experimental: as leituras em grupo viram cópias antecipadas. Precisa de “Dono único das '
   'páginas em leitura”. Medido: não agiu, cada bind do jogo escreve um bitset só. Desligado por '
   'padrão.',
   'Experimental: group readbacks become early copies. Needs "Single owner for pages being read". '
   'Measured: no effect, each game bind writes a single bitset. Off by default.'),
  '',
  ()),
 ('SOTC_RB_GROW',
  'flag',
  'readback',
  ('Janela de leitura que cresce', 'Growing readback window'),
  ('Experimental: quando o jogo lê memória da GPU em sequência, a janela de leitura dobra a cada '
   'pedido, até o limite de “Janela máxima de leitura (KiB)”. Desligado por padrão.',
   'Experimental: when the game reads GPU memory in sequence, the readback window doubles each '
   'request, up to the "Max readback window (KiB)" limit. Off by default.'),
  '',
  ()),
 ('SOTC_RB_GROW_MAX_KB',
  'value',
  'readback',
  ('Janela máxima de leitura (KiB)', 'Max readback window (KiB)'),
  ('Maior janela, em KiB, que “Janela de leitura que cresce” pode montar. Valor em potência de 2, '
   'entre 8 e 4096. Padrão 1024.',
   'Largest window, in KiB, that "Growing readback window" can build. A power of 2 between 8 and '
   '4096. Default 1024.'),
  '1024',
  ()),
 ('SOTC_RB_GROW_US',
  'value',
  'readback',
  ('Intervalo para a janela crescer (µs)', 'Window growth interval (µs)'),
  ('Tempo máximo, em microssegundos, entre dois pedidos vizinhos para a janela de “Janela de '
   'leitura que cresce” crescer. Padrão 1000.',
   'Maximum time, in microseconds, between two neighboring requests for the "Growing readback '
   'window" window to grow. Default 1000.'),
  '1000',
  ()),
 ('SOTC_RB_KEEP_LATER',
  'flag',
  'readback',
  ('Manter leitura ao ser reescrita pela GPU', 'Keep a readback when the GPU rewrites it'),
  ('Experimental: uma leitura em andamento que a GPU escreve de novo depois aplica o dado lido, em '
   'vez de refazê-la esperando a GPU. Desligado por padrão.',
   'Experimental: a readback in progress that the GPU writes again later applies the data it read, '
   'instead of redoing it while waiting for the GPU. Off by default.'),
  '',
  ()),
 ('SOTC_RB_MATERIALIZE',
  'flag',
  'readback',
  ('Máscaras de oclusão antecipadas', 'Early occlusion masks'),
  ('Aplica na memória do jogo as máscaras de oclusão assim que a GPU termina, sem falta de página. '
   'Precisa de “Dono único das páginas em leitura”. Desligado por padrão; faz parte da '
   'configuração estável medida em 29-30 out.',
   'Applies the occlusion masks to game memory as soon as the GPU finishes, with no page fault. '
   'Needs "Single owner for pages being read". Off by default; part of the stable setup measured '
   'on Oct 29-30.'),
  '',
  ()),
 ('SOTC_RB_MATERIALIZE_US',
  'value',
  'readback',
  ('Intervalo de verificação das máscaras (µs)', 'Mask check interval (µs)'),
  ('Intervalo mínimo, em microssegundos, entre as verificações de “Máscaras de oclusão '
   'antecipadas”, que consultam a GPU a cada vez. Padrão 100.',
   'Minimum interval, in microseconds, between "Early occlusion masks" checks, each of which '
   'queries the GPU. Default 100.'),
  '100',
  ()),
 ('SOTC_RB_OWNER',
  'flag',
  'readback',
  ('Dono único das páginas em leitura', 'Single owner for pages being read'),
  ('Experimental: várias threads do jogo que leem a mesma memória da GPU não copiam de novo; as '
   'páginas ficam com a leitura que começou. Pré-requisito de vários switches de leitura. '
   'Desligado por padrão.',
   'Experimental: several game threads reading the same GPU memory do not copy it again; the pages '
   'stay with the readback that started. Prerequisite for several readback switches. Off by '
   'default.'),
  '',
  ()),
 ('SOTC_RB_PREFETCH',
  'flag',
  'readback',
  ('Cópias antecipadas de janelas recorrentes', 'Early copies of recurring windows'),
  ('Experimental: janelas lidas em pelo menos 2 dos 3 quadros anteriores são copiadas assim que a '
   'GPU as escreve, e entregues na leitura sem espera. Precisa de “Dono único das páginas em '
   'leitura”. Desligado por padrão.',
   'Experimental: windows read in at least 2 of the previous 3 frames are copied as soon as the '
   'GPU writes them, and delivered on read with no wait. Needs "Single owner for pages being '
   'read". Off by default.'),
  '',
  ()),
 ('SOTC_RB_PREFETCH_MAX',
  'value',
  'readback',
  ('Cópias antecipadas ao mesmo tempo', 'Early copies at the same time'),
  ('Quantas cópias antecipadas podem estar em andamento ao mesmo tempo (1 a 256), com “Cópias '
   'antecipadas de janelas recorrentes”. Padrão 32.',
   'How many early copies can be in flight at once (1 to 256), with "Early copies of recurring '
   'windows". Default 32.'),
  '32',
  ()),
 ('SOTC_RB_PREFETCH_MB',
  'value',
  'readback',
  ('Memória de staging das cópias (MiB)', 'Copy staging memory (MiB)'),
  ('Memória de staging, em MiB, que as cópias antecipadas em andamento podem ocupar, com “Cópias '
   'antecipadas de janelas recorrentes”. Padrão 4.',
   'Staging memory, in MiB, that early copies in flight can use, with "Early copies of recurring '
   'windows". Default 4.'),
  '4',
  ()),
 ('SOTC_RB_PREFETCH_TICKS',
  'value',
  'readback',
  ('Envios até descartar cópia não usada', 'Submits before dropping an unused copy'),
  ('Quantos envios uma cópia antecipada que ninguém pediu espera antes de ser descartada, com '
   '“Cópias antecipadas de janelas recorrentes”. Padrão 8.',
   'How many submits an early copy nobody asked for waits before it is dropped, with "Early copies '
   'of recurring windows". Default 8.'),
  '8',
  ()),
 ('SOTC_RB_PREV',
  'flag',
  'readback',
  ('Máscaras do quadro anterior', 'Previous frame masks'),
  ('Experimental: lê a versão de máscara já entregue em quadro anterior, em vez de esperar a GPU; '
   'objetos podem sumir por 1 a 2 quadros. Precisa de “Dono único das páginas em leitura”; não '
   'combina com “Leituras exatas da GPU”.',
   'Experimental: reads the mask version already delivered from a previous frame, instead of '
   'waiting for the GPU; objects may vanish for 1 to 2 frames. Needs "Single owner for pages being '
   'read"; does not combine with "Exact GPU readbacks".'),
  '',
  ()),
 ('SOTC_RB_PREV_FRAMES',
  'value',
  'readback',
  ('Idade máxima da versão anterior (quadros)', 'Max age of the previous version (frames)'),
  ('Quantos quadros, de 1 a 4, a versão servida por “Máscaras do quadro anterior” pode ter. Padrão '
   '2; versões com mais de 150 ms também expiram.',
   'How many frames, from 1 to 4, the version served by "Previous frame masks" can be. Default 2; '
   'versions older than 150 ms also expire.'),
  '2',
  ()),
 ('SOTC_RB_SPIN_US',
  'value',
  'readback',
  ('Espera ativa antes de bloquear (µs)', 'Busy wait before blocking (µs)'),
  ('Com “Leituras pela fila de transferência” ligado, a thread do jogo confere o contador por este '
   'tempo, em microssegundos, antes de bloquear. Padrão 150.',
   'With "Reads on the transfer queue" on, the game thread polls the counter for this long, in '
   'microseconds, before blocking. Default 150.'),
  '150',
  ()),
 ('SOTC_RB_TICK',
  'flag',
  'readback',
  ('Leituras pela fila de transferência', 'Reads on the transfer queue'),
  ('As leituras do jogo vão à fila de transferência quando a escrita já foi enviada, sem esperar o '
   'trabalho novo da GPU. Precisa de fila de transferência na placa. Faz parte da configuração '
   'estável medida em 29-30 out.',
   'Game reads go to the transfer queue when the write was already submitted, without waiting for '
   'new GPU work. Needs a transfer queue on the GPU. Part of the stable setup measured on Oct '
   '29-30.'),
  '',
  ()),
 ('SOTC_RB_WAIT_US',
  'value',
  'readback',
  ('Espera antes de enviar leituras adiadas (µs)', 'Wait before sending deferred readbacks (µs)'),
  ('Tempo máximo, em microssegundos, que um aviso da GPU adiado espera antes de o lote pendente '
   'ser enviado. Padrão 300. Só age com “Leituras exatas da GPU” em “Ligado (experimental)”.',
   'Longest time, in microseconds, a deferred GPU notice waits before the pending batch is sent. '
   'Default 300. Only works with "Exact GPU readbacks" set to "On (experimental)".'),
  '300',
  ()),
 ('SOTC_WRITE_DIET',
  'choice',
  'readback',
  ('Leituras só da faixa escrita', 'Readbacks only for the written range'),
  ('Cada leitura da GPU em andamento guarda só a faixa que cobre, e buffers escritos pulam as '
   'leituras fora dela. Desligado por padrão, experimental. O modo 2 confere e conta divergências.',
   'Each GPU readback in progress keeps only the range it covers, and written buffers skip '
   'readbacks outside it. Off by default, experimental. Mode 2 checks and counts mismatches.'),
  '',
  (('', ('Desligado', 'Off')),
   ('1', ('Ligado', 'On')),
   ('2', ('Verifica os resultados', 'Verify results')))),
 ('SOTC_ASYNC_IMAGE_CREATE',
  'flag',
  'upload',
  ('Criar texturas antecipadamente em paralelo', 'Create textures ahead in parallel'),
  ('Desligado por padrão (experimental). Dois workers criam antecipadamente as imagens das '
   'texturas que os próximos desenhos vão usar, dentro de um limite de memória. Sem resultado '
   'medido registrado.',
   'Off by default (experimental). Two workers create ahead of time the textures that upcoming '
   'draws will use, within a memory limit. No measured result recorded.'),
  '',
  ()),
 ('SOTC_ASYNC_JOB_MB',
  'value',
  'upload',
  ('Tamanho máximo de job de upload (MiB)', 'Max upload job size (MiB)'),
  ('0 (padrão) = sem divisão. Uploads assíncronos maiores que este número de MiB viram vários jobs '
   'menores. Só tem efeito com “Cópia assíncrona para uploads de buffer” ou “Cópia assíncrona para '
   'uploads de textura”.',
   '0 (default) = no splitting. Async uploads larger than this many MiB become several smaller '
   'jobs. Only has effect with "Async copy for buffer uploads" or "Async copy for texture '
   'uploads".'),
  '0',
  ()),
 ('SOTC_ASYNC_STAGE_CACHED',
  'flag',
  'upload',
  ('Preparo de upload em memória com cache', 'Upload staging in cached memory'),
  ('Desligado por padrão (experimental). Os uploads de buffer são copiados para memória de preparo '
   'com cache no host. Ignorado quando “Uploads pela memória Resizable BAR” está ligado. Sem '
   'resultado medido registrado.',
   'Off by default (experimental). Buffer uploads are copied into host-cached staging memory. '
   'Ignored when "Uploads through Resizable BAR memory" is on. No measured result recorded.'),
  '',
  ()),
 ('SOTC_ASYNC_TEXTURE_UPLOAD',
  'flag',
  'upload',
  ('Cópia assíncrona para uploads de textura', 'Async copy for texture uploads'),
  ('Desligado por padrão (experimental). A cópia da memória do jogo para o upload de texturas sai '
   'da thread de desenho e usa o mesmo pool de cópia do upload de buffers.',
   'Off by default (experimental). Copying game memory for texture uploads moves off the draw '
   'thread and uses the same copy pool as buffer uploads.'),
  '',
  ()),
 ('SOTC_ASYNC_UPLOAD',
  'flag',
  'upload',
  ('Cópia assíncrona para uploads de buffer', 'Async copy for buffer uploads'),
  ('Desligado por padrão (experimental). A cópia de uploads grandes de buffer sai da thread de '
   'desenho e vai para um pool de threads de cópia; a GPU só recebe quando elas terminam.',
   'Off by default (experimental). Copying large buffer uploads moves off the draw thread to a '
   'pool of copy threads; the GPU receives them only when the copies finish.'),
  '',
  ()),
 ('SOTC_ASYNC_UPLOAD_MIN_KB',
  'value',
  'upload',
  ('Tamanho mínimo para cópia assíncrona (KiB)', 'Minimum size for async copy (KiB)'),
  ('Padrão 256 KiB. Uploads menores ficam na thread de desenho. Só tem efeito com “Cópia '
   'assíncrona para uploads de buffer” ou “Cópia assíncrona para uploads de textura”.',
   'Default 256 KiB. Smaller uploads stay on the draw thread. Only has effect with "Async copy for '
   'buffer uploads" or "Async copy for texture uploads".'),
  '256',
  ()),
 ('SOTC_ASYNC_UPLOAD_THREADS',
  'value',
  'upload',
  ('Threads de cópia do upload assíncrono', 'Async upload copy threads'),
  ('Padrão 3 (aceita de 1 a 8). Número de threads que copiam os uploads assíncronos. Só tem efeito '
   'com “Cópia assíncrona para uploads de buffer” ou “Cópia assíncrona para uploads de textura”.',
   'Default 3 (accepts 1 to 8). Number of threads copying async uploads. Only has effect with '
   '"Async copy for buffer uploads" or "Async copy for texture uploads".'),
  '3',
  ()),
 ('SOTC_FENCE_PIECES',
  'flag',
  'upload',
  ('Cerca de escrita espera só os pedaços', 'Write fence waits only for needed pieces'),
  ('Experimental, desligado: a cerca de escrita espera só os pedaços da cópia que leem os bytes '
   'escritos. Só vale com “Cópia assíncrona para uploads de buffer” ou “Cópia assíncrona para '
   'uploads de textura”.',
   'Experimental, off: the write fence waits only for the copy pieces that read the written bytes. '
   'Only works with "Async copy for buffer uploads" or "Async copy for texture uploads".'),
  '',
  ()),
 ('SOTC_HASH_EPOCH',
  'flag',
  'upload',
  ('Pular trava dos hashes fora da faixa', 'Skip hash lock outside the range'),
  ('Experimental, desligado: com “Pular upload de buffer pequeno igual”, evita a trava dos hashes '
   'quando a faixa está fora de todos os uploads com hash. Medido junto com outros ajustes: sem '
   'mudança mensurável.',
   'Experimental, off: with "Skip unchanged small buffer upload", skips the hash lock when the '
   'range is outside every hashed upload. Measured with other tweaks: no measurable change.'),
  '',
  ()),
 ('SOTC_HASH_SMALL',
  'value',
  'upload',
  ('Pular upload de buffer pequeno igual', 'Skip unchanged small buffer upload'),
  ('Tamanho máximo, em bytes, de buffers de leitura comparados por hash antes do upload; 0 '
   'desliga. Padrão: 0. Valor citado no changelog: 16384. Experimental.',
   'Largest read-only buffer size, in bytes, compared by hash before upload; 0 turns it off. '
   'Default: 0. Changelog example: 16384. Experimental.'),
  '0',
  ()),
 ('SOTC_HOST_IMAGE_COPY',
  'flag',
  'upload',
  ('Enviar texturas pela cópia do host', 'Upload textures by host copy'),
  ('Experimental, desligado: texturas lineares que a GPU não escreveu são enviadas pelo host, sem '
   'cópia em command buffer. Fica desligado com “Reusar bind também em envios posteriores”, a '
   'extração de texturas ou os ícones de botões do Xbox.',
   'Experimental, off: linear textures the GPU did not write are sent by the host, not by a copy '
   'in a command buffer. Off with "Reuse bind in later submits too", texture dumping or the Xbox '
   'button icons.'),
  '',
  ()),
 ('SOTC_HOST_IMPORT',
  'flag',
  'upload',
  ('Copiar uploads grandes direto da memória do jogo',
   'Copy large uploads straight from game memory'),
  ('Experimental, desligado: uploads grandes de buffer são copiados pela GPU direto da memória do '
   'jogo, na fila de transferência. Sem essa fila ou a extensão, segue pela CPU.',
   'Experimental, off: large buffer uploads are copied by the GPU straight from game memory, on '
   'the transfer queue. Without that queue or the extension, they stay on the CPU.'),
  '',
  ()),
 ('SOTC_HOST_IMPORT_MB',
  'value',
  'upload',
  ('Limite de memória importada (MiB)', 'Imported memory limit (MiB)'),
  ('Quanto da memória do jogo pode ficar travada na RAM para importação, em MiB (de 64 a 8192). '
   'Padrão: 512. Só tem efeito com “Copiar uploads grandes direto da memória do jogo” ligado.',
   'How much game memory may stay locked in RAM for import, in MiB (64 to 8192). Default: 512. '
   'Only has effect with "Copy large uploads straight from game memory" on.'),
  '512',
  ()),
 ('SOTC_HOST_IMPORT_MIN_KB',
  'value',
  'upload',
  ('Tamanho mínimo para importar (KiB)', 'Minimum size to import (KiB)'),
  ('Uploads menores que isso, em KiB, continuam pela CPU quando “Copiar uploads grandes direto da '
   'memória do jogo” está ligado. Padrão: 256.',
   'Uploads smaller than this, in KiB, stay on the CPU when "Copy large uploads straight from game '
   'memory" is on. Default: 256.'),
  '256',
  ()),
 ('SOTC_IMAGE_POOL_MB',
  'value',
  'upload',
  ('Reserva de memória para texturas (MiB)', 'Texture memory pool (MiB)'),
  ('Reserva esse tanto de memória de vídeo para texturas na abertura do jogo, para uma área nova '
   'não esperar a alocação. 0 é desligado (padrão).',
   'Reserves this much video memory for textures at game start, so a new area does not wait for '
   'allocation. 0 is off (default).'),
  '0',
  ()),
 ('SOTC_IMAGE_PREFETCH_MAX',
  'value',
  'upload',
  ('Imagens criadas antecipadamente', 'Images created ahead of time'),
  ('Quantas texturas podem ser criadas antecipadamente ao mesmo tempo (1 a 4096). Padrão 128; o '
   'limite de bytes cresce na mesma proporção.',
   'How many textures can be created ahead of time at once (1 to 4096). Default 128; the byte '
   'limit grows in the same proportion.'),
  '128',
  ()),
 ('SOTC_REBAR_MB',
  'value',
  'upload',
  ('Limite de memória Resizable BAR (MB)', 'Resizable BAR memory limit (MB)'),
  ('Quanto da memória de vídeo visível para a CPU (Resizable BAR) pode ser usado para uploads, em '
   'MB. Padrão 512. Só age com “Uploads pela memória Resizable BAR”.',
   'How many MB of CPU-visible video memory (Resizable BAR) uploads may use. Default 512. Only has '
   'effect with "Uploads through Resizable BAR memory".'),
  '512',
  ()),
 ('SOTC_REBAR_UPLOAD',
  'flag',
  'upload',
  ('Uploads pela memória Resizable BAR', 'Uploads through Resizable BAR memory'),
  ('Envia dados à GPU pela memória de vídeo visível para a CPU, quando a placa tem Resizable BAR. '
   'Desligado por padrão, experimental. Limite de tamanho: “Limite de memória Resizable BAR (MB)”.',
   'Sends data to the GPU through CPU-visible video memory when the card has Resizable BAR. Off by '
   'default, experimental. Size cap: "Resizable BAR memory limit (MB)".'),
  '',
  ()),
 ('SOTC_STAGING_LARGE_KEEP_MB',
  'value',
  'upload',
  ('Memória ociosa de envio grande (MB)', 'Idle large upload memory (MB)'),
  ('Quanto de buffers grandes de envio à GPU fica guardado sem uso, em MB, por tipo de memória. '
   'Padrão 256. Só age com “Reaproveitar buffers grandes de envio”.',
   'How much idle large upload buffer memory is kept, in MB per memory type. Default 256. Only has '
   'effect with "Reuse large upload buffers".'),
  '256',
  ()),
 ('SOTC_STAGING_LARGE_POW2',
  'flag',
  'upload',
  ('Reaproveitar buffers grandes de envio', 'Reuse large upload buffers'),
  ('Agrupa os buffers grandes de envio à GPU em tamanhos fixos, para reaproveitá-los e não alocar '
   'de novo durante o carregamento de áreas. Desligado por padrão, experimental.',
   'Groups large GPU upload buffers into fixed sizes so they can be reused instead of allocated '
   'again while areas load. Off by default, experimental.'),
  '',
  ()),
 ('SOTC_XFER_QUEUE',
  'flag',
  'upload',
  ('Fila de transferência para uploads', 'Transfer queue for uploads'),
  ('Usa uma fila de transferência separada da placa, quando ela existe, para enviar buffers à GPU. '
   'Desligado por padrão, experimental. Sem essa fila, nada muda.',
   'Uses a separate transfer queue on the card, when one exists, to send buffers to the GPU. Off '
   'by default, experimental. Without such a queue, nothing changes.'),
  '',
  ()),
 ('SOTC_ADAPTIVE_DEFER',
  'flag_on',
  'threads',
  ('Adiar trabalho de fundo durante o alívio', 'Defer background work during relief'),
  ('Ligado por padrão; só age com “Controle de carga adaptativo (experimental)”. Enquanto o alívio '
   'está ativo, a limpeza de texturas e a verificação de memória fria esperam. Desligue para '
   'manter esse trabalho.',
   'On by default; only acts with "Adaptive load control (experimental)". While relief is active, '
   'texture cleanup and the cold memory check wait. Turn it off to keep that work running.'),
  '',
  ()),
 ('SOTC_ADAPTIVE_FLUSH_US',
  'value',
  'threads',
  ('Intervalo de envio durante o alívio (µs)', 'Submit interval during relief (us)'),
  ('Padrão 4000 µs (o normal é 1000). Só vale com “Controle de carga adaptativo (experimental)”: '
   'enquanto o alívio está ativo, os envios periódicos à GPU ficam mais espaçados.',
   'Default 4000 us (normal is 1000). Only with "Adaptive load control (experimental)": while '
   'relief is active, the periodic submits to the GPU are spaced further apart.'),
  '4000',
  ()),
 ('SOTC_ASC_RPTR',
  'choice',
  'threads',
  ('Modo de leitura da fila de compute', 'Compute queue read pointer mode'),
  ('Padrão desligado. Controla quando a fila de compute devolve ao jogo o espaço já lido do anel '
   'de comandos: no modo 1, ao buscar o buffer indireto; no modo 2, logo após copiar os dados. Sem '
   'medição registrada.',
   'Default off. Controls when the compute queue hands the already-read command ring space back to '
   'the game: mode 1 when the indirect buffer is fetched, mode 2 right after copying the data. No '
   'measurement recorded.'),
  '',
  (('', ('Desligado', 'Off')), ('1', ('Modo 1', 'Mode 1')), ('2', ('Modo 2', 'Mode 2')))),
 ('SOTC_ASYNC_COMPUTE_XLATE',
  'flag',
  'threads',
  ('Traduzir shaders de compute em paralelo', 'Translate compute shaders in parallel'),
  ('Desligado por padrão (experimental). Quando um dispatch precisa de um shader de compute novo, '
   'os outros shaders novos do mesmo buffer são traduzidos em paralelo. Sem resultado medido '
   'registrado.',
   'Off by default (experimental). When a dispatch needs a new compute shader, other new compute '
   'shaders in the same buffer are translated in parallel. No measured result recorded.'),
  '',
  ()),
 ('SOTC_BACK_HEAD_CACHE',
  'flag',
  'threads',
  ('Cache do início do anel de operações', 'Cache the start of the operation ring'),
  ('Desligado por padrão. A thread de desenho só relê o início do anel de operações quando alcança '
   'o valor que já tinha. Medido junto com “Pré-carregar a próxima operação do anel” e “Ignorar '
   'escrita de registrador repetida”: sem ganho (21,5 ms contra 19,2 ms).',
   'Off by default. The draw thread re-reads the start of the operation ring only when it reaches '
   'the value it already had. Measured with "Preload the next ring operation" and "Skip repeated '
   'register writes": no gain (21.5 ms vs 19.2 ms).'),
  '',
  ()),
 ('SOTC_BACK_PF',
  'flag',
  'threads',
  ('Pré-carregar a próxima operação do anel', 'Preload the next ring operation'),
  ('Desligado por padrão. Usado junto com “Cache do início do anel de operações”: pré-carrega os '
   'valores de registro da próxima operação do anel. Medido junto com os outros cortes: sem ganho.',
   'Off by default. Used with "Cache the start of the operation ring": preloads the register '
   'values of the next operation in the ring. Measured with the other cuts: no gain.'),
  '',
  ()),
 ('SOTC_CPUSETS',
  'flag',
  'threads',
  ('Fixar threads da GPU nos núcleos rápidos', 'Pin GPU threads to fast cores'),
  ('Desligado por padrão. Põe as threads da GPU nos núcleos mais rápidos; em processadores '
   'híbridos, os workers de compilação vão para os núcleos menos potentes.',
   'Off by default. Puts the GPU threads on the fastest cores; on hybrid CPUs, shader compile '
   'workers go to the efficiency cores.'),
  '',
  ()),
 ('SOTC_CP_AHEAD',
  'choice',
  'threads',
  ('Deixar o processador de comandos adiantar', 'Let the command processor run ahead'),
  ('Desligado por padrão. Modo 1: o envio espera só o processador de comandos ler os buffers, não '
   'a GPU ficar ociosa. Modo 2: marcas por quadro, com limite em “Quadros fechados pendentes (modo '
   '2)”. Sem medição registrada.',
   'Off by default. Mode 1: a submit waits only until the command processor has read the buffers, '
   'not for the GPU to idle. Mode 2: per-frame marks, limited by "Pending closed frames (mode 2)". '
   'No measurement recorded.'),
  '',
  (('', ('Desligado', 'Off')),
   ('1', ('Modo 1: espera só a leitura', 'Mode 1: wait for read only')),
   ('2', ('Modo 2: marcas por quadro', 'Mode 2: per-frame marks')))),
 ('SOTC_CP_AHEAD_FRAMES',
  'value',
  'threads',
  ('Quadros fechados pendentes (modo 2)', 'Pending closed frames (mode 2)'),
  ('Padrão 1 (aceita de 0 a 3). Só com “Deixar o processador de comandos adiantar” em “Modo 2: '
   'marcas por quadro”: quantos quadros fechados podem ficar pendentes antes de um envio esperar a '
   'thread de desenho.',
   'Default 1 (accepts 0 to 3). Only with "Let the command processor run ahead" set to "Mode 2: '
   'per-frame marks": how many closed frames may stay pending before a submit waits for the draw '
   'thread.'),
  '1',
  ()),
 ('SOTC_CP_IDLE_TIMEOUT_US',
  'value',
  'threads',
  ('Tempo máximo de sono do processador de comandos (µs)', 'Max command processor sleep (us)'),
  ('Padrão 200 µs. Só tem efeito com “Dormir quando o processador de comandos espera (µs)”: é o '
   'tempo máximo que a thread dorme sem progresso antes de voltar a checar as filas.',
   'Default 200 us. Only has effect with "Sleep when the command processor waits (us)": the '
   'longest the thread sleeps without progress before checking the queues again.'),
  '200',
  ()),
 ('SOTC_CP_IDLE_US',
  'value',
  'threads',
  ('Dormir quando o processador de comandos espera (µs)',
   'Sleep when the command processor waits (us)'),
  ('0 (padrão) = desligado. Com um número (sugerido: 50), a thread dorme em vez de girar a 100% de '
   'um núcleo quando nenhuma fila avança por esse tempo. Sem resultado de FPS registrado.',
   '0 (default) = off. With a number (suggested: 50), the thread sleeps instead of spinning a full '
   'core when no queue advances for that time. No FPS result recorded.'),
  '0',
  ()),
 ('SOTC_CULL_EXPRESS',
  'flag',
  'threads',
  ('Fila prioritária para o culling (incompleto)', 'Priority queue for culling (incomplete)'),
  ('Desligado por padrão e incompleto. Precisa da thread de desenho e de “Preparação de desenhos '
   'em paralelo” para as pegadas de memória. Adianta a passagem de oclusão à frente de operações '
   'ainda na fila.',
   'Off by default and incomplete. Needs the draw thread and "Parallel draw preparation" for '
   'memory footprints. Runs the occlusion pass ahead of older queued operations.'),
  '',
  ()),
 ('SOTC_CULL_EXPRESS_ADDR',
  'value',
  'threads',
  ('Endereço forçado de rótulo (cull expresso)', 'Forced label address (express cull)'),
  ('Endereço (hex, com 0x) de um rótulo que sempre entra no segmento prioritário; 0 desliga. Só '
   'para teste. Precisa de “Fila prioritária para o culling (incompleto)”, que está incompleto.',
   'Address (hex, with 0x) of a label that always joins the priority segment; 0 turns it off. For '
   'testing only. Needs "Priority queue for culling (incomplete)", which is incomplete.'),
  '0',
  ()),
 ('SOTC_CULL_EXPRESS_MARKS',
  'flag_on',
  'threads',
  ('Marcas de quadro no cull expresso', 'Frame marks in express cull'),
  ('Padrão: ligado, as marcas de quadro puladas não bloqueiam o segmento prioritário. Desligado, '
   'elas bloqueiam. Precisa de “Fila prioritária para o culling (incompleto)”, que está '
   'incompleto.',
   'Default on: skipped frame marks do not block the priority segment. Off: they block it. Needs '
   '"Priority queue for culling (incomplete)", which is incomplete.'),
  '',
  ()),
 ('SOTC_CULL_EXPRESS_PATTERN',
  'flag_on',
  'threads',
  ('Reconhecer fim de dispatch no cull expresso', 'Recognize dispatch end in express cull'),
  ('Padrão: ligado. Publica um segmento prioritário quando um marcador de fim de pipeline fecha um '
   'dispatch. Desligado, só vale o endereço forçado. Precisa de “Fila prioritária para o culling '
   '(incompleto)”.',
   'Default on. Publishes a priority segment when an end-of-pipe marker closes a dispatch. Off: '
   'only the forced address counts. Needs "Priority queue for culling (incomplete)".'),
  '',
  ()),
 ('SOTC_CULL_EXPRESS_SEG',
  'value',
  'threads',
  ('Operações por segmento expresso', 'Operations per express segment'),
  ('Máximo de operações num segmento da fila prioritária, de 1 a 480. Padrão: 256. Só tem efeito '
   'com “Fila prioritária para o culling (incompleto)”, que está incompleto.',
   'Maximum operations in one priority-queue segment, 1 to 480. Default: 256. Only with "Priority '
   'queue for culling (incomplete)", which is incomplete.'),
  '256',
  ()),
 ('SOTC_CULL_EXPRESS_SIGNALS',
  'flag_on',
  'threads',
  ('Sinais de rótulo no cull expresso', 'Label signals in express cull'),
  ('Padrão: ligado. Desligado, os rótulos e flips pulados passam a bloquear o segmento '
   'prioritário. Precisa de “Fila prioritária para o culling (incompleto)”, que está incompleto.',
   'Default on. Off: skipped labels and flips block the priority segment. Needs "Priority queue '
   'for culling (incomplete)", which is incomplete.'),
  '',
  ()),
 ('SOTC_CULL_EXPRESS_SKIP',
  'value',
  'threads',
  ('Operações que podem ser adiantadas', 'Operations that may run ahead'),
  ('Quantas operações o cull expresso pode adiantar à frente da fila de comandos, até o tamanho '
   'dela. Padrão: 480. Precisa de “Fila prioritária para o culling (incompleto)”.',
   'How many operations the express cull may run ahead of the command queue, up to its size. '
   'Default: 480. Needs "Priority queue for culling (incomplete)".'),
  '480',
  ()),
 ('SOTC_CULL_EXPRESS_US',
  'value',
  'threads',
  ('Tempo máximo de decisão do cull (µs)', 'Max cull decision time (us)'),
  ('Tempo máximo, em microssegundos, que o cull expresso gasta decidindo a cada rodada. Padrão: '
   '500. Precisa de “Fila prioritária para o culling (incompleto)”, que está incompleto.',
   'Maximum microseconds the express cull spends deciding in each round. Default: 500. Needs '
   '"Priority queue for culling (incomplete)", which is incomplete.'),
  '500',
  ()),
 ('SOTC_GPU_THREAD_HIGH',
  'flag',
  'threads',
  ('Threads da GPU com prioridade alta', 'GPU threads at high priority'),
  ('Experimental, desligado: o processador de comandos e a thread de desenho rodam em prioridade '
   'alta, para o streaming não os atrasar. Pode tirar CPU das threads normais do jogo.',
   'Experimental, off: the command processor and draw thread run at high priority so streaming '
   "does not delay them. It can starve the game's normal-priority threads."),
  '',
  ()),
 ('SOTC_INLINE_FUNC',
  'flag',
  'threads',
  ('Blocos pequenos da fila sem alocação', 'Small queue blocks without allocation'),
  ('Pequenos blocos da fila de comandos ficam num buffer interno, sem alocar memória. Precisa da '
   'thread de desenho. Desligado por padrão; “Pacotes pequenos junto com o desenho” também o liga.',
   'Small command queue blocks are kept in an internal buffer, with no memory allocation. Needs '
   'the draw thread. Off by default; "Small write packets ride along with draws" also turns it '
   'on.'),
  '',
  ()),
 ('SOTC_PARALLEL_RECORD',
  'flag',
  'threads',
  ('Preparação de desenhos em paralelo', 'Parallel draw preparation'),
  ('Experimental: 1 ou 2 threads extras calculam antes da thread de desenho o que só depende dos '
   'registradores. Precisa de mais de 6 threads de CPU. Desligado por padrão.',
   'Experimental: 1 or 2 extra threads compute ahead of the draw thread what only depends on '
   'registers. Needs more than 6 CPU threads. Off by default.'),
  '',
  ()),
 ('SOTC_PARALLEL_RECORD_WORKERS',
  'value',
  'threads',
  ('Threads de preparação', 'Preparation threads'),
  ('Quantas threads de preparação usar com “Preparação de desenhos em paralelo” (1 ou 2). Vazio '
   'usa um quarto dos processadores, de 1 a 2.',
   'How many preparation threads to use with "Parallel draw preparation" (1 or 2). Empty uses a '
   'quarter of the processors, from 1 to 2.'),
  '',
  ()),
 ('SOTC_PASS_SEC',
  'flag',
  'threads',
  ('Passes grandes em thread própria', 'Large passes on their own thread'),
  ('Grava passes de renderização grandes numa thread à parte, em buffer de comandos secundário. '
   'Experimental, desligado por padrão; sem medição registrada.',
   'Records large render passes on a separate thread, into a secondary command buffer. '
   'Experimental, off by default; no measurement recorded.'),
  '',
  ()),
 ('SOTC_PASS_SEC_MIN',
  'value',
  'threads',
  ('Desenhos mínimos para thread própria', 'Minimum draws for own thread'),
  ('Com “Passes grandes em thread própria” ligado, passes com mais desenhos que este número vão '
   'para a thread à parte. Padrão 64.',
   'With "Large passes on their own thread" on, passes with more draws than this number go to the '
   'separate thread. Default 64.'),
  '64',
  ()),
 ('SOTC_TIMER_RES',
  'flag_on',
  'threads',
  ('Temporizador do Windows em 1 ms', 'Windows timer at 1 ms'),
  ('Mantém o temporizador do Windows em 1 ms enquanto o jogo roda, o que reduz travadas ao andar '
   'pelo mapa. Ligado por padrão; com 0 desliga.',
   'Keeps the Windows timer at 1 ms while the game runs, which reduces stutter when walking around '
   'the map. On by default; 0 turns it off.'),
  '',
  ()),
 ('SOTC_WAIT_NOSPIN',
  'flag',
  'threads',
  ('Espera sem girar a CPU', 'Wait without spinning the CPU'),
  ('Nas esperas entre threads, gira a CPU por cerca de 20 microssegundos e depois bloqueia, '
   'liberando o processador para outras threads. Desligado por padrão, experimental.',
   'In thread waits, spins the CPU for about 20 microseconds, then blocks, freeing the processor '
   'for other threads. Off by default, experimental.'),
  '',
  ()),
 ('SOTC_WD_FUSE',
  'flag',
  'threads',
  ('Pacotes pequenos junto com o desenho', 'Small write packets ride along with draws'),
  ('Pacotes pequenos de escrita que preparam o próximo desenho viajam junto com ele, e o '
   'processamento de comandos pode andar à frente da thread de desenho. Desligado por padrão, '
   'experimental.',
   'Small write packets that set up the next draw travel with it, and command processing can run '
   'ahead of the draw thread. Off by default, experimental.'),
  '',
  ()),
 ('SOTC_WORKER_LOW_PRIO',
  'flag',
  'threads',
  ('Compilar pipelines com prioridade baixa', 'Compile pipelines at low priority'),
  ('Faz as threads que compilam pipelines de shader rodarem com prioridade baixa. Desligado por '
   'padrão: na revisão de 27/10, deixá-las com pouca CPU causava travadas durante o carregamento '
   'de áreas.',
   'Runs the shader pipeline compile threads at low priority. Off by default: in the Oct 27 '
   'review, starving them of CPU caused stutter while areas loaded.'),
  '',
  ()),
 ('SOTC_ADAPTIVE_HIGH_MS',
  'value',
  'present',
  ('Tempo de quadro lento para iniciar o alívio (ms)', 'Slow frame time that starts relief (ms)'),
  ('Padrão 36 ms (cerca de 27,8 FPS). Com “Controle de carga adaptativo (experimental)”, dois '
   'quadros seguidos acima desse tempo ativam o alívio de carga.',
   'Default 36 ms (about 27.8 FPS). With "Adaptive load control (experimental)", two frames in a '
   'row above this time turn on load relief.'),
  '36',
  ()),
 ('SOTC_ADAPTIVE_HOLD_MS',
  'value',
  'present',
  ('Espera antes de encerrar o alívio (ms)', 'Wait before ending relief (ms)'),
  ('Padrão 1000 ms. Com “Controle de carga adaptativo (experimental)”, o alívio só termina depois '
   'de o jogo ficar no ritmo por esse tempo; se pesar de novo logo depois, a espera dobra.',
   'Default 1000 ms. With "Adaptive load control (experimental)", relief ends only after the game '
   'stays on pace for this long; if it slows again soon after, the wait doubles.'),
  '1000',
  ()),
 ('SOTC_ADAPTIVE_LOAD',
  'flag',
  'present',
  ('Controle de carga adaptativo (experimental)', 'Adaptive load control (experimental)'),
  ('Desligado por padrão (experimental). Quando dois quadros seguidos passam de cerca de 36 ms (27 '
   'FPS), o port alivia o próprio trabalho: sombras distantes alternadas, envios mais espaçados. '
   'Pode piscar sombras.',
   'Off by default (experimental). When two frames in a row exceed about 36 ms (27 FPS), the port '
   'eases its own work: distant shadows alternate, submits spread out. Shadows may flicker.'),
  '',
  ()),
 ('SOTC_ADAPTIVE_LOW_MS',
  'value',
  'present',
  ('Tempo de quadro para encerrar o alívio (ms)', 'Frame time to end relief (ms)'),
  ('Padrão 34,5 ms. Com “Controle de carga adaptativo (experimental)”, o alívio só termina quando '
   'todos os quadros ficam nesse tempo ou menos durante a espera.',
   'Default 34.5 ms. With "Adaptive load control (experimental)", relief ends only when every '
   'frame is at or under this time for the wait period.'),
  '34.5',
  ()),
 ('SOTC_CP_FLIP_PACE',
  'flag',
  'present',
  ('Espera de quadro livre no processador de comandos', 'Free-frame wait on the command processor'),
  ('Desligado por padrão (experimental). Passa a espera por um quadro livre para apresentar da '
   'thread de desenho para o processador de comandos, que atende pedidos do jogo nesse tempo. O '
   'FPS é o mesmo.',
   'Off by default (experimental). Moves the wait for a free frame to present from the draw thread '
   'to the command processor, which serves game requests meanwhile. The FPS stays the same.'),
  '',
  ()),
 ('SOTC_FLIP_SERVICE',
  'flag',
  'present',
  ('Atender o jogo enquanto espera o quadro', 'Serve the game while waiting for a frame'),
  ('Experimental, desligado: enquanto a thread de desenho espera um quadro livre para apresentar, '
   'atende os pedidos do jogo (leituras, liberar memória) em vez de deixá-los para depois.',
   'Experimental, off: while the draw thread waits for a free frame to present, it serves the '
   "game's requests (readbacks, releasing memory) instead of delaying them."),
  '',
  ()),
 ('SOTC_LOW_LATENCY',
  'flag',
  'present',
  ('Baixa latência (Reflex / Anti-Lag)', 'Low latency (Reflex / Anti-Lag)'),
  ('Usa Reflex na NVIDIA, Anti-Lag na AMD, ou um limitador de CPU nas outras placas, para reduzir '
   'a latência de entrada. Desligado por padrão; sem medição registrada.',
   'Uses Reflex on NVIDIA, Anti-Lag on AMD, or a CPU limiter on other GPUs, to cut input latency. '
   'Off by default; no measurement recorded.'),
  '',
  ()),
 ('SOTC_PRESENT_WAIT',
  'flag',
  'present',
  ('Ritmo dos flips pela tela', 'Flip pacing by the display'),
  ('Ajusta os flips ao ritmo da tela, com a espera de apresentação do Vulkan; sem essa extensão '
   'usa o temporizador. Experimental, desligado por padrão; sem medição registrada.',
   'Paces the flips to the display using Vulkan present wait; without that extension it uses the '
   'timer. Experimental, off by default; no measurement recorded.'),
  '',
  ()),
 ('SOTC_REFLEX',
  'flag',
  'present',
  ('Baixa latência (Reflex / Anti-Lag)', 'Low latency (Reflex / Anti-Lag)'),
  ('Reduz o atraso entre o comando e a imagem: usa a baixa latência da NVIDIA ou da AMD, ou um '
   'limitador genérico nas outras placas. Desligado por padrão. Igual a “Baixa latência (Reflex / '
   'Anti-Lag)”.',
   'Cuts the delay between input and the picture: uses NVIDIA or AMD low latency, or a generic '
   'frame limiter on other cards. Off by default. Same as "Low latency (Reflex / Anti-Lag)".'),
  '',
  ()),
 ('SOTC_BIND_DIFF',
  'flag',
  'diag',
  ('Diagnóstico: motivo de perder o atalho de bind', 'Diagnostic: why the bind shortcut misses'),
  ('Diagnóstico, desligado. A cada 5 s mostra o motivo de cada desenho elegível que perdeu o '
   'atalho de bind. Medido: a maioria perde por troca de pipeline ou hash (30–50%); só 1–2% pela '
   'época de memória.',
   'Diagnostic, off. Every 5 s shows why each eligible draw missed the bind shortcut. Measured: '
   'most miss on a pipeline or hash change (30-50%); only 1-2% on the memory epoch.'),
  '',
  ()),
 ('SOTC_CARVE_LOG',
  'flag',
  'diag',
  ('Diagnóstico: registrar divisões de memória', 'Diagnostic: log memory view splits'),
  ('Diagnóstico, desligado. Registra cada divisão de visão de memória: quem pediu, o endereço e '
   'quanto tempo as outras threads do jogo ficaram paradas. Mostra por que a divisão ainda para o '
   'jogo.',
   "Diagnostic, off. Logs each memory view split: who asked, the address, and how long the game's "
   'other threads stayed stopped. Shows why a split still stops the game.'),
  '',
  ()),
 ('SOTC_CEILING',
  'choice',
  'diag',
  ('Teto de medição: pular trabalho da thread', 'Measurement ceiling: skip draw-thread work'),
  ("Só medição: a imagem fica errada e serve para ver o teto de ganho. Medido: 'sync' derruba o "
   "jogo; 'tex' piorou (imagens sujas). Não use para jogar. A combinação 'sync,tex' não foi "
   'medida.',
   "Measurement only: the picture is wrong and shows the gain ceiling. Measured: 'sync' crashes "
   "the game; 'tex' got worse (dirty images). Do not play with it. The 'sync,tex' combination was "
   'not measured.'),
  '',
  (('', ('Desligado', 'Off')),
   ('sync', ('Pular sincronização de buffers', 'Skip buffer sync')),
   ('tex', ('Pular upload de texturas', 'Skip texture uploads')),
   ('sync,tex', ('Pular os dois', 'Skip both')))),
 ('SOTC_CS_STATS',
  'flag',
  'diag',
  ('Diagnóstico: dispatches de compute repetidos', 'Diagnostic: repeated compute dispatches'),
  ('Diagnóstico, desligado. A cada 5 s conta quantos dispatches de compute repetem o anterior e '
   'quanto custa o bind deles. Medido: quase nenhum repete recursos; um atalho para compute não '
   'compensaria.',
   'Diagnostic, off. Every 5 s counts how many compute dispatches repeat the previous one and what '
   'their bind costs. Measured: almost none repeat resources; a compute bind shortcut would not '
   'pay off.'),
  '',
  ()),
 ('SOTC_EOP_TICK',
  'flag',
  'diag',
  ('Verificar rótulos de fim de pipeline (diagnóstico)', 'Check end-of-pipe labels (diagnostic)'),
  ('Diagnóstico, desligado: a thread de desenho confere os rótulos de fim de pipeline, com uma ida '
   'à GPU por rótulo. Medido: 3 a 7 FPS. Só para testar corrupção de streaming.',
   'Diagnostic, off: the draw thread checks end-of-pipe labels, one GPU round trip each. Measured: '
   '3 to 7 FPS. Only to test streaming corruption.'),
  '',
  ()),
 ('SOTC_EOP_TICK_US',
  'value',
  'diag',
  ('Espera máxima do rótulo (µs, diagnóstico)', 'Max label wait (us, diagnostic)'),
  ('Diagnóstico: quanto tempo, em microssegundos, um rótulo espera antes de o comando aberto ser '
   'enviado. Padrão: 2000. Com 0, o quadro caiu de ~300 para ~133 ms. Só com “Verificar rótulos de '
   'fim de pipeline (diagnóstico)” ligado.',
   'Diagnostic: microseconds a label waits before the open command buffer is submitted. Default: '
   '2000. At 0 the frame dropped from about 300 to 133 ms. Only with "Check end-of-pipe labels '
   '(diagnostic)" on.'),
  '2000',
  ()),
 ('SOTC_FAULT_OSCOST',
  'flag',
  'diag',
  ('Medir custo do sistema em cada falta', 'Measure system cost per fault'),
  ('Diagnóstico: só mede quanto o sistema gasta em cada falta de página, na linha de divisão das '
   'faltas do log de FPS. Precisa de “FPS no registro” (aba Avançado).',
   'Diagnostic: only measures the time the system spends per page fault, on the fault split line '
   'of the FPS log. Needs "FPS in the log" (Advanced page).'),
  '',
  ()),
 ('SOTC_FPS_LEAN',
  'flag',
  'diag',
  ('Resumo de FPS mais leve', 'Lighter FPS summary'),
  ('Diagnóstico, desligado: o resumo de FPS fica, sem os relógios por operação e as marcas de '
   'escrita por shader. Precisa de “FPS no registro” (aba Avançado). Medido com outros '
   'diagnósticos desligados: de 21-24 para ~19 ms por quadro.',
   'Diagnostic, off: the FPS summary stays, without per-operation clock reads and per-shader write '
   'marks. Needs "FPS in the log" (Advanced page). Measured with other diagnostics off: 21-24 to '
   'about 19 ms per frame.'),
  '',
  ()),
 ('SOTC_IMAGE_BASE',
  'value',
  'diag',
  ('Endereço-base da imagem (hex)', 'Game image base address (hex)'),
  ('Diagnóstico: endereço-base da imagem do jogo, em hexadecimal, usado só no relatório de FPS. '
   'Vazio usa o valor que o carregador informa.',
   'Diagnostic: the game image base address, in hex, used only by the FPS report. Empty uses the '
   'value the loader reports.'),
  '',
  ()),
 ('SOTC_MDI_STATS',
  'flag',
  'diag',
  ('Contar desenhos indiretos juntáveis', 'Count mergeable indirect draws'),
  ('Diagnóstico: conta quantos desenhos indiretos seguidos poderiam virar um só. Medido: juntar '
   'desenhos não compensa neste jogo. Desligado por padrão.',
   'Diagnostic: counts how many consecutive indirect draws could merge into one. Measured: merging '
   'draws does not pay off in this game. Off by default.'),
  '',
  ()),
 ('SOTC_OPSTAT',
  'flag',
  'diag',
  ('Estatística das operações da thread de desenho', 'Draw thread operation stats'),
  ('Diagnóstico: a cada 5 s imprime as operações da thread de desenho por tipo. Medido: o custo '
   'fixo de cada operação é pequeno (0,12 a 0,2 µs de ~5 µs). Desligado por padrão.',
   'Diagnostic: every 5 s prints the draw thread operations by type. Measured: the fixed cost of '
   'each operation is small (0.12 to 0.2 us of ~5 us). Off by default.'),
  '',
  ()),
 ('SOTC_OP_PHASES',
  'choice',
  'diag',
  ('Tempo por fase da thread de desenho', 'Draw thread time per phase'),
  ('Diagnóstico: a cada 5 s mostra o tempo de cada fase da thread de desenho. A opção 2 deixa de '
   'fora a medição por buffer, que pesa menos. Desligado por padrão.',
   'Diagnostic: every 5 s shows the time of each draw thread phase. Option 2 leaves out the '
   'per-buffer timing, which costs less. Off by default.'),
  '',
  (('', ('Desligado', 'Off')),
   ('1', ('Completo (inclui ligações de buffer)', 'Full (includes buffer bindings)')),
   ('2', ('Sem medição por buffer (menos custo)', 'No per-buffer timing (less overhead)')))),
 ('SOTC_PR_VERIFY',
  'value',
  'diag',
  ('Conferir desenhos preparados (a cada N)', 'Verify prepared draws (every N)'),
  ('Diagnóstico: a cada N desenhos preparados, refaz o cálculo normal e registra as diferenças (0 '
   'é desligado). Usado para investigar “Descritores calculados antes da thread de desenho”. '
   'Precisa de “Preparação de desenhos em paralelo”.',
   'Diagnostic: every N prepared draws, recomputes the normal way and logs any differences (0 is '
   'off). Used to investigate "Descriptors resolved before the draw thread". Needs "Parallel draw '
   'preparation".'),
  '0',
  ()),
 ('SOTC_STALL_DETAIL',
  'flag',
  'diag',
  ('Detalhes das travadas no log', 'Stall details in the log'),
  ('Acrescenta à linha de travadas o ritmo de exibição, a CPU das threads do jogo e o uso de CPU '
   'dos uploads. Só aparece com “FPS no registro” (aba Avançado). Desligado por padrão; só para '
   'diagnóstico.',
   'Adds display pacing, game thread CPU and upload thread CPU to the stall log line. Only shown '
   'with "FPS in the log" (Advanced page). Off by default; diagnostics only.'),
  '',
  ()),
 ('SOTC_TRACE',
  'choice',
  'diag',
  ('Gravar linha do tempo (rastreamento)', 'Record timeline (tracing)'),
  ('Desligado por padrão. 1: grava a janela ao redor de cada quadro lento (SOTC_TRACE_MS) em '
   'user/trace. 2: grava a sessão inteira, em pedaços comprimidos. Custa CPU e liga “FPS no '
   'registro” (aba Avançado). Os arquivos são lidos com tools/trace/sotc_trace.py. Só diagnóstico.',
   'Off by default. 1: records the window around each slow frame (SOTC_TRACE_MS) in user/trace. '
   '2: records the whole session in compressed chunks. Costs CPU and turns on "FPS in the log" '
   '(Advanced page). Read the files with tools/trace/sotc_trace.py. Diagnostics only.'),
  '',
  (('', ('Desligado', 'Off')),
   ('1', ('Quadros lentos (janela ao redor)', 'Slow frames (window around)')),
   ('2', ('Sessão inteira (fluxo contínuo)', 'Whole session (continuous stream)')))),
 ('SOTC_TRACE_DIR',
  'value',
  'diag',
  ('Pasta da linha do tempo', 'Timeline folder'),
  ('Pasta onde os arquivos de rastreamento são gravados, com “Gravar linha do tempo (rastreamento)”. '
   'Vazio usa a pasta user/trace. Só para diagnóstico.',
   'Folder where the trace files are written, with "Record timeline (tracing)". Empty uses the '
   'user/trace folder. Diagnostics only.'),
  '',
  ()),
 ('SOTC_TRACE_MAX',
  'value',
  'diag',
  ('Máximo de arquivos de linha do tempo', 'Maximum timeline files'),
  ('Quantos arquivos de rastreamento são gravados por sessão, com “Gravar linha do tempo '
   '(rastreamento)” em 1. Padrão 40. Só para diagnóstico.',
   'How many trace files are written per session, with "Record timeline (tracing)" on 1. Default '
   '40. Diagnostics only.'),
  '40',
  ()),
 ('SOTC_TRACE_MS',
  'value',
  'diag',
  ('Limite de quadro lento (ms)', 'Slow frame threshold (ms)'),
  ('A partir de quantos milissegundos um quadro é considerado lento e gera uma janela de '
   'rastreamento, com “Gravar linha do tempo (rastreamento)” em 1 ou 2. Padrão 60. Só para diagnóstico.',
   'Frame time in milliseconds above which a frame counts as slow and gets a trace window, with '
   '"Record timeline (tracing)" on 1 or 2. Default 60. Diagnostics only.'),
  '60',
  ()),
 ('SOTC_TRACE_POST_MS',
  'value',
  'diag',
  ('Tempo gravado depois do quadro lento (ms)', 'Time recorded after a slow frame (ms)'),
  ('Quanto tempo, em milissegundos, é gravado depois de um quadro lento. Padrão 40. Só com '
   '“Gravar linha do tempo (rastreamento)” em 1; diagnóstico.',
   'How long, in milliseconds, is recorded after a slow frame. Default 40. Only with "Record '
   'timeline (tracing)" on 1; diagnostics.'),
  '40',
  ()),
 ('SOTC_TRACE_PRE_MS',
  'value',
  'diag',
  ('Tempo gravado antes do quadro lento (ms)', 'Time recorded before a slow frame (ms)'),
  ('Quanto tempo, em milissegundos, é gravado antes de um quadro lento. Padrão 150. Só com '
   '“Gravar linha do tempo (rastreamento)” em 1; diagnóstico.',
   'How long, in milliseconds, is recorded before a slow frame. Default 150. Only with "Record '
   'timeline (tracing)" on 1; diagnostics.'),
  '150',
  ()),
 ('SOTC_TRACE_RING',
  'value',
  'diag',
  ('Tamanho dos anéis de eventos (multiplicador)', 'Event ring size (multiplier)'),
  ('Multiplica o tamanho dos anéis de eventos de cada papel de thread. Vazio usa o padrão do build. '
   'Aumente se o contador de descartes mostrar perda dentro da janela de um quadro lento. Só com '
   '“Gravar linha do tempo (rastreamento)” ligado.',
   'Multiplies the event ring size of each thread role. Empty uses the build default. Raise it if the '
   'drop counter shows loss inside a slow-frame window. Only with "Record timeline (tracing)" on.'),
  '',
  ()),
 ('SOTC_TRACE_ZONES',
  'flag',
  'diag',
  ('Zonas do shadPS4 na linha do tempo', 'shadPS4 zones in the timeline'),
  ('Desligado por padrão. As zonas de medição do shadPS4 (Tracy) entram na linha do tempo como '
   'trechos. Só as de pelo menos SOTC_TRACE_ZONE_MIN_US (padrão 20 µs). Só diagnóstico.',
   'Off by default. The shadPS4 measurement zones (Tracy) enter the timeline as spans. Only zones of '
   'at least SOTC_TRACE_ZONE_MIN_US (default 20 µs). Diagnostics only.'),
  '',
  ()),
 ('SOTC_TRACE_GPU',
  'flag',
  'diag',
  ('Tempos reais da GPU na linha do tempo', 'Real GPU times in the timeline'),
  ('Desligado por padrão. Grava o tempo que a GPU gastou em cada envio, com marcas de tempo da '
   'Vulkan. Mostra se um quadro lento esperou a GPU ou a CPU. Usa a calibração de relógio da placa, '
   'quando ela tem a extensão; sem ela, não há efeito. Só diagnóstico.',
   'Off by default. Records the GPU time of each submit, with Vulkan timestamps. Shows whether a slow '
   'frame waited for the GPU or the CPU. Uses the GPU clock calibration when the card has the '
   'extension; without it, no effect. Diagnostics only.'),
  '',
  ()),
 ('SOTC_TRACE_IMPORTS',
  'choice',
  'diag',
  ('Chamadas ao sistema PS4 (importações)', 'PS4 system calls (imports)'),
  ('Desligado por padrão. 1: conta as chamadas e o tempo de cada função do sistema PS4 que o jogo '
   'importa. 2: também grava os trechos mais longos que SOTC_TRACE_IMPORT_MIN_US (padrão 50 µs). '
   'Só diagnóstico.',
   'Off by default. 1: counts the calls and the time of each PS4 system function the game imports. '
   '2: also records the spans longer than SOTC_TRACE_IMPORT_MIN_US (default 50 µs). Diagnostics only.'),
  '',
  (('', ('Desligado (padrão)', 'Off (default)')),
   ('1', ('Contadores por importação', 'Counters per import')),
   ('2', ('Contadores e trechos longos', 'Counters and long spans')))),
 ('SOTC_TRACE_SCOPES',
  'flag',
  'diag',
  ('Escopos de medição do motor como trechos', 'Engine measurement scopes as spans'),
  ('Desligado por padrão. Os escopos de medição do próprio motor do jogo entram na linha do tempo '
   'com os seus nomes. Só diagnóstico.',
   "Off by default. The game engine's own measurement scopes enter the timeline under their names. "
   'Diagnostics only.'),
  '',
  ()),
 ('SOTC_TRACE_SAMPLE',
  'value',
  'diag',
  ('Amostragem de pilhas (Hz)', 'Stack sampling (Hz)'),
  ('Vazio desliga. Amostra a pilha de todas as threads nesta frequência e põe as pilhas na linha do '
   'tempo e em tools/trace top. Substitui SOTC_PROFILE (o nome antigo ainda vale). Custa CPU: cada '
   'amostra suspende a thread medida por pouco tempo. Só diagnóstico.',
   'Empty turns it off. Samples the stack of every thread at this rate and puts the stacks in the '
   'timeline and in tools/trace top. Replaces SOTC_PROFILE (the old name still works). Costs CPU: each '
   'sample suspends the measured thread briefly. Diagnostics only.'),
  '',
  ()),
 ('SOTC_TRACE_FUNCS',
  'value',
  'diag',
  ('Funções do eboot cronometradas (arquivo)', 'Timed eboot functions (file)'),
  ('Arquivo com a lista de funções do eboot a cronometrar com exatidão, gerado por '
   'tools/trace/sotc_trace.py autofuncs. Vazio não usa nenhum. Só diagnóstico.',
   'File with the list of eboot functions to time exactly, generated by tools/trace/sotc_trace.py '
   'autofuncs. Empty uses none. Diagnostics only.'),
  '',
  ()),
 ('SOTC_UNTRACK_LOG',
  'flag',
  'diag',
  ('Registrar blocos de memória do quadro', 'Log per-frame memory blocks'),
  ('Mostra no log, com “FPS no registro” (aba Avançado), os blocos de memória temporária do jogo a '
   'cada quadro, para preencher “Faixas de memória sem proteção”. Só para diagnóstico.',
   'Logs the game\'s per-frame temporary memory blocks, with "FPS in the log" (Advanced page), to '
   'help choose "Unprotected memory ranges". Diagnostics only.'),
  '',
  ()),
 ('SOTC_LAB',
  'flag',
  'diag',
  ('SotC Lab: menu F2, câmera livre F3, colossos, testes automáticos',
   'SotC Lab: menu F2, free-cam F3, colossi, automated tests'),
  ('Desligado por padrão. Liga as ferramentas de medição: menu F2, câmera livre (F3), rotas, pulo '
   'direto aos colossos e testes automáticos. Só para medição; não afeta o jogo quando desligado.',
   'Off by default. Turns on the measurement tools: F2 menu, free-cam (F3), routes, direct jumps to '
   'the colossi and automated tests. For measurement only; the game is unaffected when off.'),
  '',
  ()),
 ('SOTC_FREECAM_MAXSPEED',
  'value',
  'diag',
  ('Velocidade máxima da câmera livre (m/s)', 'Free-cam maximum speed (m/s)'),
  ('Limite de velocidade da câmera livre do SotC Lab, em metros por segundo. Padrão 60. Só com '
   '“SotC Lab” ligado.',
   'Speed cap of the SotC Lab free-cam, in metres per second. Default 60. Only with "SotC Lab" on.'),
  '60',
  ()),
 ('SOTC_CTL',
  'value',
  'diag',
  ('Canal de controle do SotC Lab (nome do pipe)', 'SotC Lab control channel (pipe name)'),
  ('Nome do canal de controle (named pipe) do SotC Lab, para scripts externos. Vazio deixa o canal '
   'desligado; só tem efeito com “SotC Lab” ligado.',
   'Name of the SotC Lab control channel (named pipe) for external scripts. Empty keeps it off; it '
   'only has an effect with "SotC Lab" on.'),
  '',
  ())]


# Switches that can change while the game runs (SotC Lab, gpu/sotc/sotc_live.h). Every name is in
# TUNING and is read on its use path through SotcLive::Int, so a change applies at once.
# Migrating a switch means meeting the live-safe rules in sotc_live.h, not just listing it here.
LIVE = {
    'SOTC_MDI_STATS': True,
    'SOTC_CS_STATS': True,
    'SOTC_BIND_DIFF': True,
    'SOTC_EOP_TICK_US': True,
    'SOTC_RB_WAIT_US': True,
    'SOTC_RB_SPIN_US': True,
    'SOTC_UNTRACK_MAX_MB': True,
}


def is_live(name):
    """True when the switch can change while the game runs (see LIVE)."""
    return bool(LIVE.get(name))


def setting_key(name):
    """The launcher settings key of a switch."""
    return 'tune_' + name.lower()


def defaults():
    """Settings defaults: every switch at the build's own default (a value field holds it)."""
    return {setting_key(name): kind == 'flag_on' if kind in ('flag', 'flag_on') else
            (default if kind == 'value' else '')
            for name, kind, _group, _title, _hint, default, _options in TUNING}


def fill_defaults(settings):
    """A value field saved empty (before the fields showed their default) gets the default."""
    for name, kind, _group, _title, _hint, default, _options in TUNING:
        key = setting_key(name)
        if kind == 'value' and default and not str(settings.get(key) or '').strip():
            settings[key] = default


def environment(settings):
    """The variables of the switches that are not at their default."""
    env = {}
    for name, kind, _group, _title, _hint, default, _options in TUNING:
        value = settings.get(setting_key(name))
        if kind == 'flag':
            if value is True:
                env[name] = '1'
        elif kind == 'flag_on':
            if value is False:
                env[name] = '0'
        else:
            value = str(value or '').strip()
            if value and value != default:
                env[name] = value
    return env
