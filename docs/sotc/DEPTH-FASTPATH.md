# Camino rápido exacto para los draws de solo profundidad (hilo de dibujo)

2026-10-26. Continúa [ESCENA-TEMPLO.md](ESCENA-TEMPLO.md) §4 y §9 y cierra [PREPASE-PLAN.md](PREPASE-PLAN.md) (paso 0: NO-GO). Marcas: [M] medido,
[E] estimado, [I] inferencia. 12 de 12 lanzamientos usados (§7).

## 0. Resumen

- **Paso A (limpieza).** El planificador de ensayo del pre-pase se quitó entero (`sotc_prepass_plan.h/.cpp`, `gpu/sotc/prepass_pairs.inc`, ganchos de
  `liverpool.cpp/.h`, `vk_rasterizer.cpp/.h`, `videoout/driver.cpp`, parámetro `top_level` de `ProcessGraphics`; CMake no tenía entradas propias, usa
  `GLOB`). Se **conserva** la corrección real que salió de él: `depth_write` en la clave de `RtMemo` (`PrepareRenderState`), sin el contador
  `NoteStaleRtMemo`. El build pasa.
- **Resultado.** Camino rápido **exacto** para los draws de solo profundidad (sin destino de color): reutiliza el trabajo de estado que no cambió (el memo
  de destinos de `PrepareRenderState`, el `RenderState` de `BeginRendering` y la derivación del estado dinámico) cuando una comprobación barata
  demuestra que sus entradas son las mismas. Interruptor `BbToggle::DepthFastPath` (**bit 28**, `1u << 28`; bit puesto = camino antiguo; **activo por
  defecto**: la máscara por defecto de `src/runtime_memory.c`, bits 39, 59 y 60, no lo incluye).
- **VERIFY: 0 discrepancias.** Replay completo del templo (incluye el Santuario en reposo): >= 40.3 M comprobaciones de `PrepareRenderState`, 24.8 M de
  `BeginRendering` y 34.1 M de estado dinámico, y 135 M de chunks de registros comparados contra el CP (`SOTC_DRAW_OPT_VERIFY`), todo con 0 diferencias (§5).
- **A/B en la misma binaria, ABBA y BAAB.** Santuario (pooled): **+2.16 ± 0.29 FPS (+4.4%)** y **-1.02 ± 0.15 ms de `draw busy` (-5.4%)**. Templo
  (ventanas vecinas de >= 5000 cmds/frame, pooled): **-0.184 ± 0.046 ms de `draw busy` por 1000 cmds (-5.5%) = -1.46 ± 0.38 ms/frame**, +2.7 ± 0.9 FPS; en el
  tramo pesado (>= 7000 cmds, 4 pares) -2.1 ± 0.8 ms/frame. **Cumple el criterio (>= 0.5 ms/frame o >= 2% FPS) en los cuatro A/B.** Se queda.
- **Qué es y qué no es del objetivo.** El interruptor agrupa tres cosas (§4): el transporte de registros por diferencias (necesario para saber "no cambió
  ningún registro de render"), el memo de destinos con comprobación sin comparar registros, y el estado dinámico. Por draw de solo profundidad ahorra
  **~0.19 µs de ~1.63 [M]** (instrumentado), no los 0.8 µs del objetivo: el resto es trabajo por draw con datos que cambian en cada draw (§3). La ganancia
  total es mayor que la de los draws de solo profundidad porque el prefijo de cada op (-0.035 µs) y el memo de destinos y el estado dinámico de los draws de
  G-buffer (-0.12 µs) van bajo el mismo bit.

## 1. Paso A: limpieza del planificador del pre-pase

Quitado: `gpu/shadps4/video_core/amdgpu/sotc_prepass_plan.h/.cpp`, `gpu/sotc/prepass_pairs.inc`, `Liverpool::PreplanScan/PreplanExecDraw/PreplanExecDispatch`,
`preplan_mutex/preplan_queue`, las llamadas `NotePacketIf` (10), `EndDcb`, el reparto de la cola en `SubmitGfx`, `top_level` de `ProcessGraphics`,
`Rasterizer::PreplanFill*/PreplanArgsGpuModified` y el bloque `SOTC PREPLAN` de `driver.cpp`. Los ficheros nuevos no estaban en git (se borraron sin más; ver
PREPASE-PLAN.md P0.8). `SOTC_OP_STATS` (análisis del pre-pase de ESCENA-TEMPLO §9) no se tocó. Conservado: `depth_write` en `RtMemo`.

## 2. Desglose por fase (draws de solo profundidad, templo pesado) [M]

Instrumentación nueva (solo diagnóstico, con `SOTC_DRAW_PHASES=1`): vueltas finas (`Sub`) dentro de las fases de `DrawPhase` (`vk_rasterizer.cpp`; el reloj
del thunk de `Pipeline::BindResources` se marca desde `vk_pipeline_common.cpp`). Las vueltas tardan ~8-10 ns cada una (hay ~28 por draw), así que los totales
instrumentados están ~10-15% por encima del coste real; los repartos relativos valen. `out/depth_table.py` promedia las ventanas. Corrida `depth-ph`
(replay del templo, 184 ventanas de >= 7000 cmds/frame, 4741 draws de solo profundidad por frame):

| Fase (µs/draw, instrumentado) | Base | Qué contiene |
|---|---:|---|
| `pre` | 0.078 | `PopPendingOperations` (0.046, casi todo reloj y `Scope`), filtros de registros (0.023) |
| `prep` | 0.071 | `PrepareRenderState` con RtMemo acertado: bloqueo 0.009, comparación de registros 0.021, toques LRU/`NoteFound` ~0.04 |
| `bbuf` | 0.360 | `BindBuffers`: buffer plano (copia al anillo) 0.036, `ClampRangeSize` 0.066, `ObtainBuffer` 0.169 (~3.5 llamadas), descriptor + `IsBufferAccessed` 0.074 |
| `btex` | 0.290 | `BindTextures`: ~0.02 sin imágenes; el resto son los draws DEPTH-RO con imágenes (~0.8 µs) |
| `brest` | 0.060 | comprobaciones de copia/limpieza por cómputo, datos de usuario, `MakeUserData` |
| `begin` | 0.126 | `BeginRendering`: bloqueo 0.009, `FindDepthTarget` 0.079, meta HTile 0.011, `Transit` 0.022 |
| `refresh` | 0.010 | |
| `vtx` | 0.087 | `GetVertexInputs` + memo 0.026 (sin vertex bindings), `BindIndexBuffer` 0.051 |
| `args` | 0.020 | |
| `pipe` | 0.117 | thunk `pushConstants` 0.039, thunk `pushDescriptorSetKHR` 0.066 |
| `dyn` | 0.081 | derivación del estado dinámico + `Commit` (el memo de bit 60 está apagado por defecto) |
| `sched` | 0.050 | `scheduler.BeginRendering` (compara el `RenderState`, 0.021), `bindPipeline` 0.011, `draw` 0.012 |
| `post` | 0.098 | `ResetBindings` 0.075 (`AccessBuffer` por binding), `FlushPeriodic` 0.015 |
| **Total** | **1.444** | + prefijo de op 0.171 µs/op (copia de 2.79 chunks, 178 dwords) |

Misma corrida, redundancia frente al draw anterior: push constants iguales 100%, `RenderState` igual 99%, **descriptores 0%** (cada draw cambia algún
offset), pipeline 68%. El `RenderState` es el mismo casi siempre, pero el memo de destinos y el estado dinámico no se reutilizaban por completo: el primero
comparaba las estructuras de registros de destinos y llamaba a `FindDepthTarget`/`Transit`/`TestAndTouchMeta` en `BeginRendering` (que no tenía memo), el segundo derivaba
siempre (el memo de bit 60 estaba apagado).

## 3. Qué se puede reducir de forma exacta y qué no

| Parte | µs/draw | Veredicto |
|---|---:|---|
| `begin` (`FindDepthTarget`, meta, `Transit`) | 0.126 | **Reducible**: función de los registros de profundidad, el registro de imágenes y el estado de la imagen. Hecho (§4.3) |
| `dyn` | 0.081 | **Reducible** cuando no cambió ningún registro de entrada. Hecho (§4.4); el 17% de los draws sí cambia (profundidad/estencil/cull) |
| `prep` (comparación de registros) | 0.021 | **Reducible**. Hecho (§4.2); quedan los toques LRU, que son efectos |
| prefijo de op | 0.171 | **Reducible** (copia de chunks enteros): transporte por diferencias, 0.136 µs/op |
| `ObtainBuffer` + recorte + descriptor | 0.30 | **No**: ~3.5 rangos por draw con dirección nueva (offset de constantes, VB, IB); cada uno exige su consulta de páginas CPU-modificadas y su registro en el arena. Un memo por (dirección, tamaño) ya se midió sin ganancia (LOTE-2-RESULTADOS, `SOTC_BUFFER_MEMO`) y aquí las direcciones no se repiten |
| buffer plano (`bbFlat`) | 0.036 | **No**: copia al anillo con offset nuevo en cada draw (cambia el comando) |
| `ResetBindings` | 0.075 | **No**: `AccessBuffer` por binding con rangos distintos cada draw; alimenta las barreras y los readbacks |
| thunks (`pushConstants`, descriptores, `bindPipeline`, `draw`, `bindIndexBuffer`) | ~0.20 | **No** sin cambiar el flujo: los argumentos cambian (offsets de descriptores) o el comando es obligatorio; saltarse `bindPipeline`/`pushConstants` repetidos cambiaría los comandos grabados |
| `PopPendingOperations`, `FlushPeriodic`, filtros | ~0.06 | Ya son early-outs; la mayor parte del coste medido es el reloj |

Por eso el objetivo de 0.5 µs por draw (de 1.3) no es alcanzable con exactitud: de los ~1.2 µs reales solo ~0.3 µs son estado que se puede probar igual
(`prep` + `begin` + `dyn`), y se recupera ~0.19.

## 4. Diseño

### 4.1 Saber cuándo no cambió ningún registro: transporte por diferencias y "series de relevancia"

El hilo de dibujo recibe los registros en cada `BackOp`. Con la máscara por defecto (bit 60 puesto) llegaban chunks de 64 dwords enteros (2.8 por op), así que
"el chunk cambió" no dice nada. Ahora, con el bit 28 limpio, `BackAlloc` envía solo los dwords que cambian de valor (el CP compara contra `back_shadow`;
código y verificación del bit 60, LOTE-2-RESULTADOS Lote 4), y `ExecuteBackOp` mantiene tres series (`Liverpool::DrawRelSerial`): **RelRender** (registros del
lado de profundidad + extensión de profundidad), **RelColor** (`color_buffers`, `color_target_mask` + extensiones de color) y **RelDynamic** (los registros
que leen `Update{ViewportScissor,DepthStencil,Primitive,Rasterization,ColorBlending}State`). Cada serie sube en uno por op que cambió **el valor** de un
dword de su máscara. La máscara se construye una vez con las direcciones de los campos (`DepthFastEnsureRelevance`; la lista dinámica es la de
`CommitDynamicStateOld`, ya verificada 9.4 M veces en el Lote 4). `DB_DEPTH_CONTROL` es parcial para RelRender (`SetRelevancePartial`, bits 0-2: stencil_enable,
depth_enable, depth_write_enable, lo único que llega a los destinos): sin eso `depth_func` cambiaba la serie en ~15% de los draws (log `why-regs`:
`depth_control` 98% de los fallos de registros). Hasta que se fijan las máscaras cada op sube las tres series (conservador). Con el bit puesto (camino
antiguo) y el 60 también, los chunks llegan enteros y las series suben casi siempre: el camino rápido no se activa aunque se fuerce.

### 4.2 `PrepareRenderState` (todas las clases de draw)

El memo ya existente (`RtMemo`) guarda ahora las series (`rel_depth`, `rel_color`). Acierto rápido = memo válido, **misma serie de profundidad** (y de color si el draw
tiene destinos de color), misma máscara de destinos, mismo `ImageSetGeneration`, mismo `gpu_modified_epoch`: se salta la comparación de registros (esa es la parte
que elimina) y repite los efectos de siempre (toques LRU, `NoteFound`, lista `bound_images`, `is_target`). Cualquier otra cosa: el camino antiguo.

### 4.3 `BeginRendering` de un draw de solo profundidad (`BeginRenderingFast`)

`BeginRendering` para `mrt_mask == 0` produce un `RenderState` que depende de: registros de profundidad (RelRender), la imagen de profundidad y su estado, el
registro de imágenes, las metas HTile, y tiene efectos sobre el caché de texturas. Tras un `BeginRendering` completo sin limpieza en el estado
(`depth_clear`/`stencil_clear` falsos, ningún destino de color) se **graba** `df_memo`: serie, id de imagen, `ImageSetGeneration`, `gpu_modified_epoch`, `MetaEpoch`
(nuevo: sube al añadir/quitar una meta o al poner un bit de limpieza), imagen enlazada de estencil (`LastDepthStencil`), puntero del `backing`, argumentos del
`Transit` y el `RenderState`. El siguiente draw lo **reutiliza** si, bajo el bloqueo del caché (BatchLock), se cumple todo (cada condición cuenta su causa):

| Causa de fallo | Comprobación (barata, por draw) |
|---|---|
| nomemo | hay memo (se grabó uno y no se invalidó) |
| regs | `DrawRelSerial(RelRender)` igual: ningún registro de profundidad ni la extensión cambiaron de valor |
| image | `db_desc.first` es la imagen del memo |
| gen / gpuepoch / meta | `ImageSetGeneration`, `gpu_modified_epoch` y `MetaEpoch` iguales: las búsquedas de `FindDepthTarget` dan lo mismo y el bit HTile sigue limpio |
| flags | imagen `Registered` y `GpuModified` (`MarkWritten` no hace nada), sin `needs_rebind` (el `ASSERT`), `UpdateIsNoop` (no `Dirty`, rastreada entera: `UpdateImage` no hace nada salvo el toque) |
| backing | mismo `backing` (las vistas y el layout son los del memo) |
| transit | `Image::TransitIsNoop` (nuevo, espejo de solo lectura de `GetBarriers`, parcial o completo): no habría barrera ni cambio de estado |

Los efectos que sí tiene `BeginRendering` en ese caso se repiten: toque LRU de la imagen (`GetImage`) y de la imagen de estencil enlazada, `usage.depth_target`,
`attachment_feedback_loop = false`. `Scheduler::BeginRendering` recibe un `token` del memo y se salta la comparación de 400 bytes del `RenderState` si el
pase en curso empezó con ese mismo token (se asigna uno nuevo en cada grabación; con otro estado o fuera de un pase el camino es el de siempre).

### 4.4 Estado dinámico (todas las clases)

`CommitDynamicState` acierta si la derivación anterior (de cualquier draw) tuvo la misma serie `RelDynamic`, los mismos `write_masks` del pipeline, el mismo
`attachment_feedback_loop` y el mismo `is_indexed` (el memo del bit 60 no tenía este último). Entonces solo hace `Commit` de lo que el objeto ya tiene. Cualquier
derivación por otro camino (camino antiguo con el bit puesto, hilo sin draw thread) borra el memo (`df_dyn.valid`), para que no sobreviva a valores cambiados.

### 4.5 Lo que no se tocó

`ObtainBuffer`, `BindBuffers`, `BindTextures`, `BindVertexBuffers/IndexBuffer`, `Pipeline::BindResources`, `ResetBindings`, `FlushPeriodic`: se ejecutan
igual con el interruptor activo o no. **Los comandos Vulkan de un draw son función de (RenderState, push constants, descriptores, VB/IB, bits sucios del estado
dinámico, barreras pendientes); el camino rápido solo sustituye la derivación de los tres primeros extremos que no dependen del draw, y solo con igualdad
demostrada.**

## 5. VERIFY (`SOTC_DEPTHFAST_VERIFY=1`)

Mecanismo: en cada acierto, el camino antiguo corre también y se compara.

- **`PrepareRenderState`**: el acierto por serie exige que la comparación antigua (memcmp de `depth_buffer`, `depth_view`, `color_buffers`, extensiones,
  máscaras) también diga acierto; si no, se cuenta, se imprime y se usa el resultado antiguo.
- **`BeginRendering`**: se reproducen los efectos del acierto, se toma una instantánea de la imagen (flags, `usage`, `binding`, `backing`, layout/acceso/etapa,
  subrecursos, rango rastreado, tick LRU de la imagen y de la de estencil, `tick_accessed_last`, máscara HTile, `attachment_feedback_loop`, `needs_barrier`, barreras
  de imagen pendientes), se ejecuta el `BeginRendering` **completo** encima y se exige: instantánea idéntica (el camino completo no cambia nada más), ningún
  barrier nuevo y `RenderState` byte a byte igual al del memo. Se usa el estado del camino completo.
- **Estado dinámico**: en cada acierto se deriva otra vez con todos los bits sucios a cero (como el verify del bit 60): cualquier bit que sube es un valor que
  el memo habría dejado atrás.
- **Transporte** (`SOTC_DRAW_OPT_VERIFY=1`, ya existente): los registros del hilo de dibujo igualan a los del CP en cada chunk sucio.

**No se captura el flujo de thunks del recorder** (pedido original): ejecutar el camino lento y el rápido para el mismo draw en el mismo flujo no es posible (el
`Copy` del buffer plano avanza el anillo, los toques LRU y las marcas de escritura no se deshacen), y los comandos grabados son función de los valores
comparados arriba. Lo que se comprueba son las **entradas del recorder** (y los efectos sobre el caché), no sus bytes. Es la parte más débil de la prueba: un
error en algo que el VERIFY no mira (p. ej. un campo del `RenderState` que no fuera de memcmp) no se vería; `RenderState` se compara entero (memcmp, sin padding).

Resultados (replay completo del templo con `SOTC_DEPTHFAST_VERIFY=1`; contadores acumulados de `SOTC DEPTHFAST ... VERIFY`):

| Corrida | `PrepareRenderState` | `BeginRendering` | Estado dinámico | Chunks de registros | Nota |
|---|---:|---:|---:|---:|---|
| `df-verify` (v1) | 1.7 M, 0 | 0 | 28 M, 0 | | el memo de `BeginRendering` nunca se grababa: `RenderAttachment::is_clear` es una unión con `has_depth` y daba siempre "con limpieza"; serie única demasiado gruesa (acierto 30%) |
| `df-verify2` | 35.3 M, 0 | 21.4 M, 0 | 33.8 M, 0 | | tras separar la serie de color y corregir la unión |
| `df-verify3` | **40.3 M, 0** | **24.8 M, 0** | **34.1 M, 0** | | tras la máscara parcial de `depth_control` (binaria final) |
| `df-verify4` | 38.2 M, 0 | 22.8 M, 0 | 32.1 M, 0 | **135.3 M, 0** | binaria final + `SOTC_DRAW_OPT_VERIFY` |

El replay empieza con ~100 s del Santuario en reposo (5.7 k cmds/frame) y sigue con la ruta del templo (tramo pesado de 7.8-8.4 k cmds/frame), de modo que cubre
las dos escenas. 0 mensajes `SOTC DEPTHFAST VERIFY:` y 0 `register chunk ... differs` en todas.

Tasas de acierto (corridas con la binaria final, todo el replay; `df-verify3` y `df-verify4` coinciden):

| | Acierto | Fallos |
|---|---:|---|
| `PrepareRenderState` (todos los draws) | **98.5%** | 1.5% registros/imágenes (0.02% donde el memo antiguo habría acertado) |
| `BeginRendering`, draws de solo profundidad | **90-91%** | `transit` ~8% (draws DEPTH-RO: el pase de texturas deja la profundidad en otro layout y el `Transit` es real), `regs` 0.8%, `nomemo` 0.3%, `gen` 0.01%; `image`, `gpuepoch`, `meta`, `flags`, `backing`: 0 |
| Estado dinámico (todos los draws) | **83%** | el resto cambia de verdad (profundidad/estencil/cull: se deriva y se hace `Commit`) |

Antes de la máscara parcial (`df-phase2`) eran 84% (`BeginRendering`) y 88% (`PrepareRenderState`).

## 6. A/B en la misma binaria (`out/bb-probe.depthfast.exe`)

`BB_TOGGLE_FILE` cambia tras cada línea `SOTC FPS` (A = camino rápido, bit limpio; B = camino antiguo, bit 28 puesto). Scripts: `out/depth_ab.sh` (Santuario,
`BENCH_MEASURE=150`) y `out/depth_ab_temple.sh` (replay del templo, alternancia cada 5 s), `out/rb_stats.py`, `out/depth_pair2.py`, `out/depth_phase_ab.py`.

### 6.1 Santuario (ventanas desde 100 s, 5 s cada una)

| Corrida | Orden | ON FPS (n) | OLD FPS (n) | ON-OLD FPS | `draw busy` ON / OLD (ms) | ON-OLD busy |
|---|---|---|---|---|---|---|
| `dfs2` | ABBA | 52.46 (14) | 50.53 (13) | +1.93 | 17.59 / 18.51 | **-0.93** |
| `dfs3` | BAAB | 50.98 (13) | 48.72 (14) | +2.26 | 18.40 / 19.46 | **-1.05** |
| Conjunto | | 51.75 (27) | 49.59 (27) | **+2.16 ± 0.29 (+4.4%)** | 17.98 / 19.00 | **-1.02 ± 0.15 ms (-5.4%)** |

(`dfs1`, primera corrida, con la medida por defecto de 40 s: 4 ventanas ON y 2 OLD, +2.8 FPS y -1.12 ms; descartada por pocas muestras, no entra en el conjunto.)

### 6.2 Templo (ventanas desde 70 s; cada ventana ON comparada con la media de sus vecinas OLD, ambas de >= 5000 cmds/frame)

| Corrida | Orden | Pares | `busy` por 1000 cmds ON-OLD | `draw busy` ON-OLD | FPS ON-OLD |
|---|---|---:|---|---|---|
| `dft1` | ABBA | 7 | -0.180 ± 0.105 ms | -1.37 ± 0.68 ms/frame | +3.13 ± 1.75 |
| `dft2` | BAAB | 12 | -0.186 ± 0.043 ms | -1.51 ± 0.48 ms/frame | +2.46 ± 0.99 |
| Conjunto | | 19 | **-0.184 ± 0.046 ms (-5.5% de 3.34)** | **-1.46 ± 0.38 ms/frame** | **+2.71 ± 0.87** |
| Tramo pesado (>= 7000 cmds/frame, ambas corridas) | | 4 | -0.196 ± 0.083 ms | -2.11 ± 0.82 ms/frame | +2.65 ± 1.32 |

Medias sin emparejar (ventanas >= 5000 cmds): ON 47.02 FPS, 20.13 ms; OLD 45.27 FPS, 21.18 ms (23 ventanas cada una, mismos cmds/frame medios 6392). El tramo pesado
dura 5-6 ventanas por corrida: el emparejado es el que resuelve la variación de carga de escena (el A/B de punta a punta sin emparejar no la resuelve,
ESCENA-TEMPLO §8.4).

### 6.3 Por fase, misma corrida (replay del templo con `SOTC_DRAW_PHASES=1` y el bit alternando cada 5 s; ventanas de >= 5000 cmds/frame, ON n=386, OLD n=376) [M]

Instrumentado (µs/draw):

| Fase | Solo profundidad ON | OLD | ON-OLD | G-buffer ON | OLD | ON-OLD |
|---|---:|---:|---:|---:|---:|---:|
| `pre` | 0.103 | 0.111 | -0.008 | 0.132 | 0.129 | +0.003 |
| `prep` | 0.072 | 0.085 | -0.013 | 0.119 | 0.152 | -0.033 |
| `bbuf` | 0.364 | 0.375 | -0.011 | 0.844 | 0.850 | -0.005 |
| `btex` | 0.319 | 0.331 | -0.011 | 0.984 | 1.006 | -0.021 |
| `begin` | 0.079 | 0.171 | **-0.093** | 0.703 | 0.700 | +0.002 |
| `dyn` | 0.055 | 0.098 | **-0.043** | 0.034 | 0.088 | **-0.054** |
| resto (`brest`, `refresh`, `vtx`, `args`, `pipe`, `sched`, `post`) | 0.513 | 0.521 | -0.008 | 0.737 | 0.743 | -0.006 |
| **Total** | **1.446** | **1.632** | **-0.186** | **3.553** | **3.670** | **-0.117** |

Prefijo de op (misma corrida): 0.136 µs/op con 2.07 chunks y 29 dwords por op (ON) frente a 0.171 µs con 2.79 chunks y 178 dwords (OLD): -0.035 µs/op. `BackAlloc` del CP
0.99 frente a 1.03 µs (dentro del ruido).

Por frame del tramo pesado [E]: 4710 draws de solo profundidad x 0.186 + 2465 de G-buffer x 0.117 + ~8900 ops x 0.035 = 0.88 + 0.29 + 0.31 = **-1.47 ms**, de acuerdo con
el -1.46 ± 0.38 ms medido de punta a punta (§6.2). Los draws de solo profundidad son el 60% de la ganancia.

## 7. Lanzamientos (12 de 12)

| # | Qué | Para qué |
|---|---|---|
| 1 | `depth-ph`: replay del templo, `SOTC_DRAW_PHASES=1`, binaria sin camino rápido | desglose base (§2) |
| 2 | `df-verify`: VERIFY, primera versión | encontró el memo de `BeginRendering` sin grabar y la serie demasiado gruesa |
| 3 | `df-verify2`: VERIFY | 0 discrepancias; `BeginRendering` acierta 75-90% |
| 4 | `df-phase2`: fases + `SOTC_DEPTHFAST_WHY=1` | el 98% de los fallos de registros era `depth_control` -> bits parciales |
| 5 | `df-verify3`: VERIFY, binaria final | 0 discrepancias (§5) |
| 6 | `dfs1`: Santuario A/B con 40 s de medida | muestras insuficientes (descartada) |
| 7-8 | `dfs2`, `dfs3`: Santuario ABBA y BAAB, `BENCH_MEASURE=150` | §6.1 |
| 9-10 | `dft1`, `dft2`: templo ABBA y BAAB | §6.2 |
| 11 | `df-verify4`: VERIFY + `SOTC_DRAW_OPT_VERIFY` | transporte de registros por diferencias (135 M chunks, 0) |
| 12 | `dftp`: templo, alternando el bit con `SOTC_DRAW_PHASES=1` | tabla por fase ON/OLD (§6.3) |

No se hizo una corrida VERIFY solo del Santuario: el replay del templo empieza en el Santuario en reposo (ventanas de 50-95 s con 5.7 k cmds/frame) y los contadores son
acumulados; la binaria es la misma.

## 8. Se queda, se retira, lo que falta

- **Se queda todo** (bit 28, activo por defecto): transporte por diferencias con series de relevancia, memo de destinos por serie, `BeginRenderingFast`, memo de estado
  dinámico por serie, `Scheduler::BeginRendering(token)`, `MetaEpoch`, `Image::TransitIsNoop`, `TextureCache::UpdateIsNoop/LastDepthStencil/TouchLinked/LruTickOf/MetaClearMask`.
  Con el bit 60 puesto (por defecto) y el 28 limpio los registros viajan por diferencias: el comportamiento de ese transporte es el del bit 60 (verificado 135 M chunks, 0
  diferencias, y 40.9 M en el Lote 4); `src/runtime_memory.c` documenta que el 60 sigue apagado por defecto y que el 28 lo activa de hecho.
- **Retirado**: el planificador del pre-pase (§1). No se retiró nada del trabajo nuevo.
- **Extensión barata sin hacer** (fuera del encargo, mismo mecanismo): `BeginRendering` de los draws de G-buffer cuesta 0.70 µs por draw (7 destinos: `UpdateImage`, `FindRenderTarget`,
  meta CMask, `Transit` por destino; 2465 draws/frame = 1.7 ms) y no tiene memo. Exige memoizar por destino de color los efectos (`SetBackingSamples`, `download_images`,
  captura `hudless`, limpieza CMask, `is_bound` -> bucle de realimentación) y su propio VERIFY.
- El binario con el cambio no tiene perfil PGO para las funciones nuevas (`BeginRenderingFast`, `DepthFast*`); `tools/pgo_train.sh` antes de empaquetar. El caché de shaders no cambia.
- Los draws DEPTH-RO (15% de los de solo profundidad) no aciertan en `BeginRendering`: el `Transit` de la profundidad es real (barrera) en cada draw.

## 9. Reproducir

- Variables (solo diagnóstico): `SOTC_DEPTHFAST_VERIFY=1` (§5), `SOTC_DEPTHFAST_WHY=1` (qué registro de profundidad movió la serie, línea `SOTC DEPTHFAST`), `SOTC_DRAW_PHASES=1`
  (vueltas finas, `SOTC DRAWPHASE ... sub us/draw`), `SOTC_DRAW_OPT_VERIFY=1` (transporte). Línea de contadores `SOTC DEPTHFAST draws ...` cada ~1 M de draws en `user/last_run.log`.
- Scripts: `out/depth_table.py <log> [min cmds] [clase]` (tabla base), `out/depth_phase_ab.py <bit> <min cmds> <log> [clase]` (ON/OLD por fase), `out/depth_ab.sh` y
  `out/depth_ab_temple.sh <nombre> 268435456 <ABBA|BAAB> [--env ...]`, `out/rb_stats.py <log> <tmin> 268435456`, `out/depth_pair2.py 268435456 <min cmds> <tmin> <logs...>`.
- Código: `gpu/shim/bbport_toggles.h` (bit), `amdgpu/liverpool.h/.cpp` (series, `BackAlloc`, `ExecuteBackOp`), `renderer_vulkan/vk_rasterizer.h/.cpp` (`DepthFast*`,
  `BeginRenderingFast`, `CommitDynamicState`, `PrepareRenderState`), `vk_scheduler.h/.cpp` (token), `vk_runtime.h` (`PendingImageBarriers`), `texture_cache/*` (`MetaEpoch`, etc.),
  `image.h/.cpp` (`TransitIsNoop`). Todo marcado `sotc:`.
