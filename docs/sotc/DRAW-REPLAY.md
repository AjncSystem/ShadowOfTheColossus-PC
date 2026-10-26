# Repetición de draws con el mismo estado: desglose, fracción repetible y qué se quitó (2026-10-26)

Árbol `sotc-port`, ruta grabada del templo (`out/bench_port.sh temple`), ventanas de 5 s con `SOTC_FPS=1`. Exe de partida:
`out/snapshots/port-2026-10-26/bb-probe.portpgo.exe`; exe final: `out/bb-probe.dreplay5.exe` (sin perfil PGO en las funciones tocadas).
Marcas: [M] medido, [E] estimado. **11 de 12 lanzamientos usados** (§8), todos con `out/game.lock`.
Continúa [SIGUIENTE-LIMITE.md](SIGUIENTE-LIMITE.md) y [DISPATCH-FASTPATH.md](DISPATCH-FASTPATH.md): el límite del tramo pesado es `GpuDraw`
(19.2 ms por frame, 93-94% ocupado) y la memoización por operación estaba agotada.

## 1. Veredicto

- **Fracción repetible grande, trozo repetible pequeño.** En las ventanas de >= 7000 cmds el **43.9% de los `DrawIndirect`** (2483 de 5651 por
  frame; 38% de todos los draws) tiene exactamente el mismo estado ligado que el draw anterior (mismo pipeline, push, datos de usuario, descriptores,
  `RenderState`, vértices, índices, series de registros, sin barrera, sin cambios de memoria): solo cambian los argumentos. Los `Draw` no
  indirectos: 0%. Pero esos draws ya iban por el camino de "bind reutilizable" (`SOTC_FAST_BIND`, 58% de los indirectos) y lo que les queda
  de coste **no** es repetición de trabajo: son efectos que hay que reproducir (toques LRU, accesos de barrera, marcas de LOD, envío periódico,
  hilos del recorder) y comprobaciones.
- **Lo que sí era trabajo fijo repetido y exacto de quitar** (y se quitó, tras el interruptor nuevo `BbToggle::DrawReplay`, **bit 41**):
  1. el **hash XXH3 de la clave del bind reutilizable** (`FastBindHash`: ~15 llamadas a XXH3 por draw, 0.165 us instrumentados por indirecto);
  2. la **copia por operación del buffer plano** (`rt.flattened_ud_buf.assign`, 2 etapas por op, 0.04-0.05 us por op);
  3. el recorrido de elegibilidad del bind (`FastBindEligible`) y dos contadores con `lock xadd` por draw que solo lee la línea `SOTC FASTBIND`.
- **Resultado (A/B dentro de la corrida, mismo exe, ABBA + BAAB)**: en las ventanas de >= 7000 cmds `GpuDraw` ocupado **-1.20 +- 0.33 ms** y FPS
  **+2.74 +- 0.76** (n = 8 + 9); en las de >= 5000 **-0.82 +- 0.19 ms** y **+1.89 +- 0.85** (n = 26 + 25). **Cumple el criterio**
  (>= 0.5 ms menos de `GpuDraw` ocupado y ganancia de FPS visible) con VERIFY = 0 diferencias. **El bit 41 queda LIMPIO por defecto (activado).**
- No se hizo la repetición "directa al grabado" (saltar preparación de destino, `BeginRendering`, vértices, estado dinámico) ni el multi-draw:
  §6 explica con números por qué el trozo restante es pequeño (~0.2 ms [E]) y arriesgado.

## 2. Desglose de un `DrawIndirect` en `GpuDraw` (templo pesado, >= 7000 cmds, `SOTC_DRAW_PHASES=1` + `SOTC_DRAW_REPLAY_PROBE=1`) [M]

10 ventanas, 1971 frames, exe base (bit 41 puesto = camino antiguo). Relojes TSC por fase (`DrawPhase`): inflan ~7% (1.24 instrumentados frente a
1.16 us reales por `SOTC_HASH_STATS`). `DrawIndirect` con argumentos en CPU (la ruta "directa" de `SOTC_INDIRECT_DIRECT`): 5651 por frame,
7.02 ms/frame instrumentados; los 169 por frame con argumentos en GPU no entran. `Draw`: 884 por frame, 2.05 ms/frame.

| Parte (como la pidió el encargo) | Fase / sub-fase | `DrawIndirect` us/draw | `Draw` us/draw |
|---|---|---:|---:|
| decodificar la operación (copia de trozos de registros, series de relevancia, copia del buffer plano) | prefijo de `ExecuteBackOp` (por op) | 0.089 | 0.130 |
| comprobaciones de registros/estado | `pre` (`PopPendingOperations` 0.025, filtros 0.030) + `dyn` 0.018 | 0.083 | 0.200 |
| preparación del destino de render | `prep` (cierre `RtMemo` por series) | 0.068 | 0.119 |
| memo de `BeginRendering` | `begin` 0.052 + `refresh` 0.012 | 0.064 | 0.141 |
| `BindTextures` | `btex` (memo 0.121, pasadas 0.073) | 0.220 | 0.823 |
| `BindBuffers` / `ObtainBuffer` | `bbuf`: plano 0.025, recorte 0.044, `ObtainBuffer` 0.131, descriptor 0.040 | 0.244 | 0.458 |
| vértices e índices | `vtx`: `GetVertexInputs` 0.030, `BindIndexBuffer` (+`ObtainBuffer`) 0.055 | 0.095 | 0.111 |
| argumentos indirectos + barrera | `args` (lectura 0.019, el resto es la barrera cuando toca) | 0.056 | 0.014 |
| clave del bind reutilizable (hash) y prólogo de `BindResources` | `brest` (hash 0.165) | **0.200** | 0.173 |
| push constants y descriptores | `pipe` (push 0.006, descriptores 0.043) | 0.059 | 0.121 |
| enlace del pipeline, traspaso al recorder y el draw en sí | `sched`: `scheduler.BeginRendering` 0.013, `bindPipeline` 0.009, `drawIndexed` 0.011 (los thunks del recorder están dentro de cada fase) | 0.039 | 0.056 |
| `ResetBindings` (`AccessBuffer` por binding) + envío periódico | `post`: 0.090 + 0.016 | 0.114 | 0.107 |
| **Total (sin prefijo)** | | **1.242** | **2.323** |

Cuando todo acierta (clase `fast-bind hit` + estado idéntico al anterior, 2483 draws por frame, 1.36 ms por frame + 0.22 ms de prefijo), el coste
es **0.548 us + 0.089 us de prefijo** instrumentados, de los cuales el draw en sí (`drawIndexed` + `bindPipeline` + lectura de argumentos) son
0.034 us: **~94% es coste fijo por draw que corre aunque todo acierte**. Su reparto: hash de la clave 0.150, `pre` 0.059, `prep` 0.059,
`begin` 0.048, vértices/índices 0.082, `post` 0.037, `args` 0.037, `sched` 0.035, `dyn` 0.012, `refresh` 0.008, `pipe` 0.006.
Por clase (templo pesado, ms por frame instrumentados): `fast-bind` acierta + estado igual 1.36; acierta pero el estado difiere (dinámico,
barrera...) 0.49; no acierta 5.04 (2275 draws con datos de usuario nuevos, 2.21 us cada uno: `ObtainBuffer` 0.32, `BindTextures` 0.54, hash 0.18...).

Diagnóstico nuevo (solo con las dos variables): `SOTC_DRAW_PHASES=1` + `SOTC_DRAW_REPLAY_PROBE=1` imprime por ventana, antes de la línea FPS,
las líneas `SOTC DRAWREPLAY` (fracciones, motivos, longitudes de racha, fases por clase = tipo x `fast-bind` x repetibilidad, prefijo por tipo).
Scripts: `out/rp_table.py`, `out/rp_fraction.py`, `out/rp_ab.py`.

## 3. Fracción repetible y longitudes de racha [M]

Un draw es "mismo estado" si, tras grabarse, lo siguiente coincide con el draw anterior: operaciones adyacentes (`back_serial`), pipeline, push data,
datos de usuario y buffer plano, descriptores (con y sin los desplazamientos del anillo de stream), `RenderState` completo, buffers de vértices
(identificador, desplazamiento, tamaño, paso, descripciones), buffer/tipo de índices, series `RelRender/RelColor/RelDynamic`, sin barrera.
"Estricto" añade: misma época de memoria (`mem_epoch`), sin subidas en el draw, mismas generaciones de buffer/textura/stream y mismo `tick`.
Solo pueden cambiar los argumentos (y el desplazamiento de índices, que en la práctica nunca cambia).

| | `DrawIndirect` >= 7000 | `DrawIndirect` >= 5000 | `Draw` |
|---|---:|---:|---:|
| draws por frame (media) | 5651 | 4222 | 884 (>= 7000), 812 (>= 5000) |
| `fast-bind` acierta | 58.2% (3287) | 62.3% | 0% |
| mismo estado | 45.4% (2566) | 50.9% | 0% |
| estricto | **43.9% (2483)** | **49.2% (2077)** | 0% |
| mismo estado ignorando la adyacencia | 45.4% | 50.9% | 0% |

(10 ventanas de >= 7000 y 28 de >= 5000 de la corrida `rp1`.) Motivos por los que un draw (de `Draw` + `DrawIndirect`) NO es repetible, en %
de los draws, "cualquiera / único" (>= 7000): datos de usuario 47.4/1.6, descriptores 56.1/12.4 (casi todos por los datos de usuario nuevos: solo el
1.6% de los draws difiere únicamente por el desplazamiento del anillo), pipeline 30.0/0.0, estado dinámico 13.8/0.2, época de memoria 3.9/1.2, barrera 1.4,
índices 1.5, `RenderState` 0.7, vértices 0.0 (estos draws no tienen buffers de vértices), no adyacente 0.8, subidas 0.1, generaciones 0.2.
`push` 0.2. Los argumentos **nunca** están en la misma dirección que los del draw anterior (0.0%) y están en direcciones contiguas con paso
fijo en el 60% de los repetibles (56% en >= 5000).

Rachas de draws repetibles consecutivos (longitud = cuántos draws seguidos son repetibles respecto al anterior; >= 7000, por frame):

| Longitud | 1 | 2 | 3 | 4 | 5-8 | 9-16 | 17-32 | 33-64 | 65+ |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| rachas, estado igual | 174 | 123 | 88 | 46 | 81 | 84 | 12 | 1.9 | 0.2 |
| % de los draws repetibles | 7 | 10 | 10 | 7 | 19 | 33 | 10 | 3 | 1 |
| rachas con argumentos contiguos (estricto) | 341 | 148 | 93 | 41 | 52 | 6.4 | 1.6 | 0 | 0 |

Media de la racha 4.4 (2.2 con argumentos contiguos): 1690 draws por frame están en rachas de >= 5; 413 por frame en rachas de >= 5 con
argumentos contiguos. Es el patrón de una malla con las mismas constantes dibujada N veces (9-16 es la longitud típica), no de miles de repeticiones.

## 4. Qué se hizo (`BbToggle::DrawReplay`, bit 41; bit puesto = camino antiguo; **limpio por defecto**)

Leído en vivo por operación y por draw. `SOTC_DRAWREPLAY=0/1` lo fija; `BB_TOGGLE_FILE` con el bit 41 puesto lo apaga. Marcas `sotc:` en el código.

1. **Cambio de datos de usuario/buffer plano visto por el command processor** (`CaptureBackOp`, `liverpool.cpp`): compara los datos de usuario y el
   buffer plano de la operación con los de la última operación capturada con el mismo shader (`InfoRuntime::cap_ud/cap_flat`, copia 0) y guarda
   `BackStage::changed`. Se calcula siempre, también con el bit puesto (así la respuesta es relativa a la captura anterior si el bit cambia en la
   corrida).
2. **Prefijo de la operación** (`ExecuteBackOp`): en lugar de `rt.flattened_ud_buf.assign(...)` (memcpy de todo el buffer plano por etapa y por op),
   hace `swap` con el vector de la operación (el contenido de `rt` queda idéntico al de antes; el vector de la op toma los bytes viejos y el CP lo
   reasigna antes de reutilizarla) y `InfoRuntime::ud_gen += changed` (copia 1). Si cambió, pide con `prefetch` el primer KiB (los binds lo leen
   unos cientos de ns después). Un draw que acierta ya no lee esos bytes en el hilo de dibujo.
3. **Clave del bind reutilizable comparada, no hasheada** (`BindResources`): `FastBindKey` = los mismos campos que hasheaba `FastBindHash` (puntero del
   pipeline, push data, 8 direcciones de color + la de profundidad, y por etapa el contenido de datos de usuario y buffer plano ahora como `ud_gen`)
   guardada tal cual en el `CommitFastBind` anterior y comparada campo a campo. Misma igualdad, sin hash (y sin posibilidad de colisión).
   Las condiciones restantes del acierto (pipeline, `tick`, generaciones, `mem_epoch`, `push_clobbers`) no cambian.
4. **Elegibilidad del bind** (`FastBindEligible`): la parte que solo depende de los `Info` de las etapas (inmutables) se recuerda en el pipeline
   (`Pipeline::fast_bind_static`); el resto (registros, bucle de realimentación) se evalúa por draw. Las tres comprobaciones `IsComputeImage*` solo
   se llaman para pipelines de cómputo.
5. **Contadores `FASTBIND`** (`fast_bind_eligible/hits`): un `lock xadd` por draw que solo imprime el informe de `SOTC_FPS`. Con el bit limpio, carga y
   almacén relajados (los escribe un solo hilo, como `SotcLod::Mark`) y solo con `SOTC_FPS=1`.

Nada de esto cambia la salida del recompilador de shaders: el caché de shaders no se rehace. Cualquier cambio de `liverpool.cpp`,
`vk_rasterizer.cpp` y `info.h` pierde su perfil PGO en esta compilación; hay que reentrenar con `tools/pgo_train.sh` antes de empaquetar.

Exactitud (argumento): (a) tras el `swap`, `rt.flattened_ud_buf` tiene exactamente el contenido de la operación, como con `assign`; (b) `ud_gen`
sube justo cuando la operación difiere de la anterior capturada con ese shader, y todas las operaciones capturadas llegan en orden al hilo de
dibujo, así que igualdad de `ud_gen` entre dos binds = igualdad de datos entre ellos (transitividad); (c) la clave compara los mismos campos que
el hash; (d) la memoria de elegibilidad es una función de datos inmutables; (e) los contadores son diagnóstico. La tasa de aciertos de `FASTBIND` es
la misma con el bit limpio y puesto en las cuatro corridas A/B (54.7% frente a 54.6% de media, ventanas >= 5000).

## 5. VERIFY (`SOTC_DRAWREPLAY_VERIFY=1`, `SOTC_FAST_BIND_VERIFY=1`, ruta del templo, ~230 s)

Con `SOTC_DRAWREPLAY_VERIFY=1`, en cada draw elegible con el mismo pipeline que el bind anterior se calculan la clave nueva **y** el hash antiguo y
se cuentan los casos en que no coinciden (igual por clave y distinto por hash, o al revés); la memoria de elegibilidad se recalcula en cada draw
y se compara. `SOTC_FAST_BIND_VERIFY=1` (ya existente) hace además el bind completo en cada acierto posible y compara los descriptores.

| Corrida (exe) | Comparaciones clave/hash | Diferencias | Comprobaciones de elegibilidad | Diferencias | Aciertos posibles de `FAST_BIND_VERIFY` | Obsoletos |
|---|---:|---:|---:|---:|---:|---:|
| `dv1` (`dreplay1`, antes de pasar la comparación al CP) | >= 29.4 M | 0 | (aún sin memoria) | | 22.5 M | 0 |
| `dv2` (`dreplay3`) | >= 31.5 M | 0 | (sin contador: 0 mensajes) | 0 | 23.5 M | 0 |
| `dv3` (**exe final `dreplay5`, máscara por defecto**) | >= 29.4 M | 0 | **>= 44.0 M** | **0** | **22.3 M** | **0** |

("Con datos nuevos" 2-5: los mismos casos de `FAST_BIND_VERIFY` de siempre, descriptores iguales con bytes nuevos en el mismo sitio, que cubre la
época de memoria.) 0 discrepancias en las tres corridas. La corrida `dv3` usa la máscara por defecto: `Runtime: disabled optimizations
mask=1729383906178760704` (bit 41 limpio).

## 6. Por qué no se hizo la repetición directa al grabado (ni el multi-draw)

- **Lo que podría saltarse un draw repetible**: tras el cambio del §4, un draw estricto cuesta ~0.46 us (instrumentados) más 0.05 de prefijo. Un
  camino "directo al grabado" conserva `pre` 0.06 (`PopPendingOperations` y los filtros con estado), la lectura de argumentos y la barrera
  0.037, `bindPipeline` + `draw` 0.035, `post` 0.037, la comprobación de la clave y las marcas de LOD: ~0.19 us. Lo saltable (`prep` 0.059, `begin`
  0.048, `refresh` 0.008, vértices/índices 0.082, `dyn` 0.012: 0.21 us) se reduce a ~0.1 us reales porque `PrepareRenderState` y
  `BeginRendering` ya son solo comparaciones de series y toques LRU (efectos que hay que repetir), y los vértices/índices piden `ObtainBuffer` y
  `AccessBuffer`. Estimación: 2483 draws por frame x ~0.1 us = **~0.25 ms por frame [E]**, a cambio de demostrar que cada efecto (LRU de las
  imágenes, accesos de barrera de vértices/índices, `vi_memo`, `DynamicState::Invalidate`, `push_clobbers`) es idempotente entre dos draws
  adyacentes. Con el criterio ya cumplido y sin un VERIFY que vea los efectos (solo ve entradas y salidas), no compensa el riesgo.
- **Multi-draw indirecto**: hay rachas con argumentos contiguos y paso fijo (60% de los repetibles), pero la longitud media es 2.2 y solo 413 draws por
  frame están en rachas contiguas de >= 5. Con los argumentos en CPU (la ruta directa lee 20 bytes por draw), un `vkCmdDrawIndexedIndirect` con
  `drawCount > 1` exigiría subir y enlazar el rango de argumentos (el `ObtainBuffer` + barrera que la ruta directa evita) para ahorrar un thunk de
  0.011 us por draw: pierde. No se consultó `multiDrawIndirect` porque no se implementó. Lo que sí ahorraría ~0.45 us por draw seguido es
  fusionar las operaciones en el command processor (una sola `BackOp` con lista de argumentos), pero la igualdad de la época de memoria solo se
  conoce en el hilo de dibujo y los efectos por draw (envío periódico por tiempo, LRU, `AccessBuffer`) cambiarían de cadencia: cota superior
  ~0.7-1.1 ms por frame [E] con rediseño del CP.

## 7. A/B dentro de la corrida (mismo exe, `BB_TOGGLE_FILE`, `out/ab_replay.sh`, `out/ab_analyze.py`)

A = bit 41 limpio (camino nuevo), B = puesto (antiguo); el resto de la máscara es la de por defecto (bits 20, 39, 40, 59 y 60 puestos; máscaras
1729383906178760704 y 1729386105202016256). El estado cambia al imprimirse cada línea `SOTC FPS` (ABBA / BAAB sobre ventanas de 5 s). ANCOVA por cmds/frame
(`metrica = a + b * cmds + c * [A]`), efecto de A frente a B, +- error estándar. "draw" = `GpuDraw` ocupado en ms por frame (la línea
`SOTC threads per frame`), "parse" = el CP.

| Corrida (exe) | Ventanas A / B | FPS | `GpuDraw` ocupado (ms) | CP, trabajo propio (ms) |
|---|---|---:|---:|---:|
| `rab1` ABBA + `rab2` BAAB (`dreplay3`), >= 7000 | 6 / 5 | +2.05 +- 1.58 | -0.77 +- 0.60 | -0.13 +- 0.19 |
| `rab1` + `rab2`, >= 5000 | 28 / 28 | +1.47 +- 1.10 | -0.52 +- 0.27 | -0.13 +- 0.14 |
| `rab3` ABBA (`dreplay4`), >= 7000 | 3 / 3 | +2.79 +- 0.54 | -1.30 +- 0.19 | -0.11 +- 0.12 |
| `rab4` BAAB (`dreplay4`), >= 7000 | 5 / 6 | +2.75 +- 0.68 | -1.19 +- 0.36 | -0.27 +- 0.23 |
| **`rab3` + `rab4` (exe final), >= 7000** | **8 / 9** | **+2.74 +- 0.76** | **-1.20 +- 0.33** | -0.22 +- 0.20 |
| **`rab3` + `rab4` (exe final), >= 5000** | **26 / 25** | **+1.89 +- 0.85** | **-0.82 +- 0.19** | -0.07 +- 0.11 |
| las cuatro, >= 7000 | 14 / 14 | +2.38 +- 0.69 | -1.02 +- 0.28 | -0.16 +- 0.13 |
| las cuatro, >= 5000 | 54 / 53 | +1.66 +- 0.70 | -0.66 +- 0.17 | -0.10 +- 0.09 |

(`dreplay3` = sin el `prefetch` ni los contadores sin `lock`; `dreplay4` = el exe final salvo el valor por defecto del bit, `dreplay5` = `dreplay4` con el
bit limpio por defecto y los comentarios.) Medias sin emparejar del exe final (>= 7000): A 52.88 FPS, 16.96 ms; B 50.62 FPS, 18.01 ms (8 y 9 ventanas,
8134 y 7984 cmds por frame). El CP no empeora (su trabajo propio baja, dentro del ruido).
La ruta diverge entre corridas: solo 3-6 ventanas pesadas por corrida, por eso se muestran también las de >= 5000 y las cuatro corridas juntas.

Mecanismo, por fase (corrida `rpB`, exe `dreplay2`, `SOTC_DRAW_PHASES=1`, estado alternando cada 5 s, >= 7000, `DrawIndirect`, us por draw instrumentados;
A = nuevo / B = antiguo): hash de la clave 0.080 / 0.169, prefijo de la operación 0.050 / 0.098 us por op (el `assign` se llevó el 0.048), `bbuf`
0.270 / 0.238 (los bytes del buffer plano se leen ahora al enlazar, no en el prefijo: de ahí el `prefetch` posterior), total 1.178 / 1.235; con el
prefijo, -0.10 us por draw indirecto = -0.6 ms por frame instrumentados. La primera versión (`rpA`, `dreplay1`, comparaba el contenido en el prefijo) tenía
el prefijo en 0.139 / 0.106 (peor que el `assign`): por eso la comparación pasó al command processor.

Humo de la máscara por defecto con el exe final (`smoke1`, ruta del templo, sin variables): 64.3 FPS de media en 30 muestras (mínimo 54.3),
tramo pesado de 8.2-8.7 k cmds por frame a 55.9-57.5 FPS, sin errores nuevos. (Las corridas de la sesión anterior daban 50.5-53.5 FPS en el mismo tramo.)

## 8. Lanzamientos (11 de 12)

| # | Qué | Para qué |
|---|---|---|
| 1 | `rp1`: sonda + fases, exe de la sesión (`rprobe`) | desglose y fracción repetible (§2, §3) |
| 2 | `dv1`: VERIFY, primera versión (`dreplay1`) | clave frente a hash |
| 3 | `rpA`: A/B con fases (`dreplay1`) | encontró el prefijo peor que el `assign` |
| 4 | `rpB`: A/B con fases (`dreplay2`) | mecanismo (§7) |
| 5 | `dv2`: VERIFY (`dreplay3`) | + elegibilidad |
| 6-7 | `rab1` ABBA, `rab2` BAAB (`dreplay3`) | A/B limpio |
| 8-9 | `rab3` ABBA, `rab4` BAAB (`dreplay4`, exe final salvo el valor por defecto) | A/B limpio, criterio (§7) |
| 10 | `dv3`: VERIFY con el exe final y la máscara por defecto | §5 |
| 11 | `smoke1`: ruta del templo, máscara por defecto | humo |

Reproducir: `out/ab_replay.sh <nombre> <ABBA|BAAB> 41` (acepta `AB_EXTRA="--env X=Y"`), `out/ab_analyze.py MIN_CMDS nombre:patrón ...`,
`out/rp_ab.py`, `out/rp_table.py`, `out/rp_fraction.py`; verificador: `SOTC_DRAWREPLAY_VERIFY=1 SOTC_FAST_BIND_VERIFY=1`; sonda:
`SOTC_DRAW_PHASES=1 SOTC_DRAW_REPLAY_PROBE=1`.

## 9. Decisión y lo que sigue

- **Se queda activado** (bit 41 limpio en `src/runtime_memory.c`, comentarios al día en `gpu/shim/bbport_toggles.h`): criterio cumplido y VERIFY a 0.
  `SOTC_DRAWREPLAY=0` o un `BB_TOGGLE_FILE` con el bit 41 puesto restauran el camino antiguo.
- Pierden su perfil PGO `liverpool.cpp`, `vk_rasterizer.cpp` y las funciones que incluyen `info.h`: reentrenar con `tools/pgo_train.sh` antes de empaquetar.
- Trozos del hilo de dibujo que siguen sin camino exacto sencillo (números de [SIGUIENTE-LIMITE.md](SIGUIENTE-LIMITE.md) §1): los comandos de hilos del
  juego (`ProcessCommands`, 2.4 ms por frame, de ellos `FinishAsyncReadback` ~1.5 ms de memcpy y comparaciones: moverlo exige demostrar el orden con los
  draws), el replicador HLE (~1 ms), `VirtualProtect` de las subidas (~1 ms) y, de los draws, los 2275 indirectos por frame con datos nuevos (5 ms:
  `ObtainBuffer`, `BindTextures`, `AccessBuffer`), que no son repetición.
- Candidato con cota alta pero rediseño: fusionar en el command processor las operaciones repetibles consecutivas (§6, ~0.7-1.1 ms [E]).
