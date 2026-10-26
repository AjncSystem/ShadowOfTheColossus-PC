# Dispatches en el hilo de dibujo: desglose y atajos exactos (2026-10-26)

Árbol `sotc-port`, ruta grabada del templo (`out/bench_port.sh temple`), ventanas de 5 s con `SOTC_FPS=1`. Exe de partida:
`out/snapshots/port-2026-10-26/bb-probe.portpgo.exe`; exe con el cambio: `out/bb-probe.dfast4.exe` (sin perfil PGO en las
funciones tocadas). Marcas: [M] medido, [E] estimado. **11 lanzamientos de 12 usados** (todos con `out/game.lock`; uno se perdió
en el arranque: `no free 256 MiB range below 2 GiB for the eboot`, ASLR de Windows, y se repitió).

## 1. Veredicto

`BbToggle::DispatchFastPath` (**bit 40, apagado por defecto**) es exacto (VERIFY: 0 discrepancias en cientos de millones de
comprobaciones) pero **no cumple el criterio de aceptación**: baja `GpuDraw` ocupado unos 0.3 ms por frame (rango
-0.30 +- 0.17 con 45 + 45 ventanas de >= 5000 cmds; -0.45 +- 0.18 en las 10 ventanas de >= 7000 de las corridas con relojes, pero
0.0 +- 0.26 con las 22 de las cuatro corridas) y la ganancia de FPS (+0.6 +- 0.8) no se distingue del ruido. El mecanismo sí está
medido y es limpio: el coste por operación de `DispatchIndirect` baja 0.46-0.50 +- 0.04 us (4.77 -> 4.31, -10%, 0.16 ms por frame)
y el de `Dispatch` 0.14 +- 0.13 us (0.17 ms por frame). Queda con el bit puesto (= ruta antigua); `SOTC_DISPFAST=1` o una máscara
de `BB_TOGGLE_FILE` sin el bit 40 lo enciende (las dos líneas `Runtime: disabled optimizations mask=...` lo muestran).

**No hay un trozo grande exacto que quitar en el camino de los dispatches**: el tiempo está repartido en muchas piezas de
0.1-0.3 us y las dos mayores (subidas de páginas escritas por el juego y el replicador HLE) son trabajo real (copias,
`VirtualProtect`, lecturas de DRAM) o exigen seguimiento de memoria nuevo (ver §6).

## 2. Desglose de un dispatch (templo, ventanas de >= 5000 cmds, `SOTC_DISP_PHASES=1`) [M]

Relojes TSC por fase (`renderer_vulkan/sotc_dispphase.h`; inflan ~0.3-0.5 ms por frame en total). Ruta antigua = `out/run-disp1.txt`
(10 ventanas), con el bit encendido = `out/run-disp3.txt` (18 ventanas); rutas distintas, misma escena.

| Clase | por frame | us/op antes | us/op con el bit | ms/frame antes -> con el bit |
|---|---:|---:|---:|---:|
| Directo (sin HLE) | 851 / 848 | 3.37 | 3.25 | 2.87 -> 2.76 |
| Indirecto | 357 / 355 | 4.84 | 4.64 | 1.73 -> 1.65 |
| Replicador `50784801` por HLE | 91 / 100 | 9.88 | 11.46 | 0.90 -> 1.15 (tablas de 4 y 8 MiB, ancho de banda de DRAM) |

us por dispatch y fase (antes -> con el bit):

| Fase | Directo | Indirecto | Qué es |
|---|---:|---:|---|
| `ObtainBuffer`, binding escrito | 1.03 -> 0.98 | 1.47 -> 1.16 | arena 0.037 -> 0.020, sync 0.19, **marca de escritura 0.269 -> 0.217 por llamada** |
| `ObtainBuffer`, binding de lectura | 0.85 -> 0.78 | 1.34 -> 1.34 | arena + sync; 3.7% de las llamadas suben datos (6 us cada una) |
| `BindTextures` (primer y segundo pase) | 0.31 + 0.13 | 0.03 | casi todo en 3 shaders de post-proceso con 4-6 imágenes (60-80 us cada dispatch) |
| `FlushPeriodic` | 0.21 | 0.22 -> 0.30 | envíos periódicos (trabajo real) |
| `ClampRangeSize` y lectura del V# | 0.15 -> 0.17 | 0.42 -> 0.46 | no bajó con la memoización (ver §4) |
| `ResetBindings` (`AccessBuffer` por binding) | 0.14 | 0.33 -> 0.31 | la memo de accesos muere en cada `FlushBarriers` (42% de los dispatches) |
| resto de `BindResources`, `Pipeline::BindResources`, buffer plano | 0.11 + 0.08 + 0.07 | 0.30 + 0.15 + 0.10 | |
| `bindPipeline` + `dispatch`, barrera, `EndRendering` | < 0.1 en conjunto | < 0.1 | no hay coste fijo que repartir |

Forma media de un dispatch: 5.2 buffers (2.3 escritos), 0.35 imágenes, barrera en 42%. Contadores nuevos de `SOTC_DISP_PHASES`:

- **Subidas dentro de dispatches** (por llamada que sube): lectura 4.4-4.6 us de "recoger" (`CollectUploads`: de ellos ~2.7 us son
  el `VirtualProtect` de las páginas recién subidas, más el cerrojo de cada página) + 1.25-1.55 us de "enviar" (`CommitUploads`:
  staging, copia, `CopyBuffer`), 1.07 rangos, 30-33 KB; escritura 3.0-3.6 + 0.85-1.1 us, 1.0 rangos, 28-78 KB. Unas 157 + 65 subidas
  por frame = **~1.2 ms por frame, inherente** (el juego reescribe esas páginas cada frame; hay que protegerlas otra vez).
- **`TextureCache::FindImage` dentro de dispatches**: memo 0.07 us (44k por ventana), perfecta 0.24 us (15k), por solapamiento
  1.6 us (32k), imagen nueva 13.6 us (190); 170-180 `ExpandImage` y 410-460 `FreeImage` por ventana. Total ~0.2 ms por frame:
  **no** es lo que cuesta los 60-80 us de los tres shaders de post-proceso (eso es `UpdateImage`/`FindView`/transiciones de sus
  imágenes, que no se memoizan por ser "impuras" para `TexBindMemo`).
- Perfil de muestreo de `GpuDraw` en frames de > 19 ms (`out/prof-disp1.txt`, 4697 muestras): `DispatchDirect` 27.5% + `DispatchIndirect`
  13.9% inclusivos, `ObtainBuffer` 20.2% (`SynchronizeMemory` 11.4%, `host_protect` 6.1%), `MarkWrittenBinding` 4.8% (`RangeSet::Add`
  2.9%, `RecordCurrentWrite` 2.0%), `ResetBindings` 3.4%, `ClampRangeSize` 1.5%, replicador 7.3%; fuera del programa: `ntdll` 24%
  (`VirtualProtect`) y `ucrtbase` 13% (`memcpy`/`memcmp`).

## 3. Qué se hizo (bit 40 `DispatchFastPath`; bit puesto = ruta antigua, honrado en vivo, leído por llamada)

1. **`current_writes` perezoso** (`buffer_cache.cpp`, `RecordCurrentWrite`, `CurrentWritesIntersects`): cada binding escrito insertaba
   su rango en un `RangeSet` (búsqueda en árbol y fusiones, 3 400 por frame) y solo se le pregunta `Intersects` (dos lecturas de
   vuelta por frame). Ahora el rango se añade a un vector (`pending_writes`, sin duplicados consecutivos, 1024 como máximo y luego se
   vuelca al conjunto) y `Intersects` mira el conjunto y el vector. La intersección con una unión es la intersección con un miembro:
   mismas respuestas.
2. **Memo de la marca de escritura, parte A, por secuencia**: el memo (`WriteMarkMemo`) caía con cualquier lectura de vuelta que
   entraba en la lista en vuelo (`inflight_gen`). Ahora, si entraron lecturas después del memo, solo cae cuando el rango toca la
   región o las carreras de páginas GPU de alguna de ellas (`InflightHitSince`; `MarkInflightReadbacksDirty` no cambia nada en una
   que no toca).
3. **Cierre convexo de cada lectura de vuelta** (`hull_lo/hull_hi`, fijo una vez en vuelo): `MarkInflightReadbacksDirty` e
   `InflightHitSince` descartan sin recorrer los ítems las lecturas cuyo cierre no toca el rango.
4. **`ClampRangeSize` memoizado por generación de la tabla de VMA** (`Rasterizer::ClampBindingSize`, 512 ranuras por (dirección,
   tamaño)): la respuesta solo depende de la tabla, y `runtime_memory_generation()` es par y no cambia mientras la tabla no cambia.
5. **Tablas de memos más anchas**, cada entrada válida por su propio contenido (con el bit puesto se usan las ranuras antiguas):
   `resident_cache` de `EnsureResident` 64 -> 1024 (`arena` 0.037 -> 0.020 us por llamada), memo de marca de escritura 256 -> 2048
   (aciertos de la parte A 54% -> 75%), memo de accesos de barrera (`AccessBuffer`) 64 -> 256.

Sin el bit (no se pueden alternar en vivo, son exactos por construcción):

- `table_generation` (`runtime_memory.c`) en una línea de caché propia: la lee el hilo de dibujo por cada búsqueda de región, y el
  cerrojo de al lado lo escriben los hilos del juego. Solo distribución en memoria.
- **Telemetría `SOTC_FPS=1`**: `BeginAsyncReadback` recorría todas las lecturas en vuelo por cada copia (O(n^2): 2.2% de las muestras
  de `GpuDraw` en frames lentos, ~0.4 ms por frame). Ahora ordena y fusiona los rangos de las otras una vez y busca por bisección
  (O(n log n)). Cambia la cifra "bytes de solape" de la telemetría (ahora la unión de los demás, antes la suma con tope), nada más.
  Esto es **coste de medición, no de juego normal**: no baja el tiempo de ningún usuario sin `SOTC_FPS=1`.
- `SOTC_DISP_PHASES`: subidas (recoger / enviar / rangos / bytes), `FindImage` por resultado, `ResetBindings` aparte de `FlushPeriodic`.

## 4. Verificación (`SOTC_DISPFAST_VERIFY=1` + `SOTC_WRITEMARK_VERIFY=1`, bit encendido, ruta del templo, dos corridas de 230 s)

| Comprobación | Corrida 1 (sin cierre convexo) | Corrida 2 (versión final) |
|---|---:|---:|
| `current_writes`: respuesta del vector sola frente al conjunto solo (cada pregunta) | 0 de >= 229 k | 0 de >= 229 k |
| `ClampRangeSize`: acierto del memo frente a la recomputación (generación sin cambios) | 0 de 111.1 M | 0 de 126.9 M |
| cierre convexo: lectura saltada que en realidad tocaba | (no existía) | 0 de 11.5 M |
| memo de la marca de escritura (incluye la regla A por secuencia): estado de todas las lecturas en vuelo igual antes y después de repetir la marca | 0 de 35.7 M | 0 de 32.2 M |

En verify, `current_writes` guarda cada rango en el vector y en el conjunto, la respuesta que se usa es la del bit y se cuentan las
discrepancias entre las dos; la regla A usa el verificador antiguo (`SOTC_WRITEMARK_VERIFY`) que ya repite `AddGpuModified` en cada
acierto y compara el estado de cada lectura en vuelo.

Honestidad sobre el reloj del clamp: la memoización de `ClampRangeSize` es exacta pero **no ahorró nada medible** (fase `clamp`
0.15 -> 0.17 us por dispatch directo, 0.42 -> 0.46 indirecto; el 1.0% de muestras propias de `ClampBindingSize` es la carga de la
generación) y el aislar `table_generation` tampoco la movió. Se deja porque es exacta y barata, pero no cuenta en el balance.

## 5. A/B dentro de la corrida (mismo exe, `BB_TOGGLE_FILE`, A = bit 40 limpio, B = puesto; `out/ab_disp.sh`, `out/ab_analyze.py`, `out/ab_ops.py`)

Máscara de A 1729382806667132928, de B 1729383906178760704 (el resto igual a la de por defecto: bit 20 puesto). ANCOVA por cmds/frame
(`metrica = a + b * cmds + c * [A]`), efecto de A frente a B. Las corridas 3 y 4 llevan `SOTC_HASH_STATS=1` (relojes por clase de
operación, un poco de coste igual en los dos estados).

| Corrida (orden) | Ventanas A/B | FPS | `GpuDraw` ocupado (ms) | Comandos de hilos del juego (ms) |
|---|---|---:|---:|---:|
| `dab1` (ABBA), >= 5000 | 7 / 10 | +2.66 +- 1.70 | -0.68 +- 0.34 | -0.04 +- 0.20 |
| `dab2` (BAAB), >= 7000 | 5 / 6 | -0.70 +- 0.59 | +0.33 +- 0.34 | +0.30 +- 0.26 |
| `dab2` (BAAB), >= 5000 | 12 / 12 | -0.74 +- 1.71 | +0.07 +- 0.43 | +0.19 +- 0.21 |
| `dab3` (ABBA, con relojes), >= 7000 | 3 / 5 | -0.37 +- 0.37 | -0.65 +- 0.18 | -0.27 +- 0.10 |
| `dab4` (BAAB, con relojes), >= 6000 | 5 / 4 | +2.41 +- 1.61 | -0.51 +- 0.36 | +0.07 +- 0.10 |
| `dab3 + dab4`, >= 7000 | 5 / 5 | -0.22 +- 0.26 | **-0.45 +- 0.18** | -0.14 +- 0.16 |
| `dab3 + dab4`, >= 6000 | 11 / 12 | +1.41 +- 0.83 | -0.63 +- 0.20 | +0.02 +- 0.09 |
| `dab3 + dab4`, >= 5000 | 26 / 23 | +0.66 +- 0.98 | -0.37 +- 0.18 | +0.01 +- 0.10 |
| **Las cuatro, >= 7000** | 10 / 12 | -0.54 +- 0.54 | **+0.01 +- 0.26** | +0.17 +- 0.19 |
| **Las cuatro, >= 6000** | 19 / 23 | +0.85 +- 0.87 | -0.38 +- 0.25 | +0.06 +- 0.13 |
| **Las cuatro, >= 5000** | 45 / 45 | +0.60 +- 0.77 | **-0.30 +- 0.17** | +0.05 +- 0.09 |

Por clase de operación (`dab3 + dab4`, ANCOVA, relojes de `SOTC_HASH_STATS`):

| Clase | us/op (>= 5000) | ms/frame (>= 5000) | ms/frame (>= 7000, 5/5) |
|---|---:|---:|---:|
| `DispatchIndirect` | **-0.458 +- 0.037** (4.77 -> 4.31) | **-0.159 +- 0.015** | -0.188 +- 0.022 |
| `Dispatch` | -0.139 +- 0.129 (3.66 -> 3.51) | -0.167 +- 0.090 | +0.063 +- 0.169 |
| `DrawIndirect` | +0.006 +- 0.023 | +0.022 +- 0.115 | -0.215 +- 0.046 |
| `Draw`, `Func` | ~0 | ~0 | ~0 |

Lectura: el efecto sobre dispatches indirectos es real y firme; sobre dispatches directos es del orden de 0.1-0.2 ms y no se
resuelve; el total del hilo queda en -0.3 +- 0.17 ms con las 90 ventanas de >= 5000 y no se puede distinguir de 0 en las de >= 7000
(la ruta diverge entre corridas: solo 2-5 ventanas de >= 7000 por corrida, 22 en total). Con un ahorro de 0.3 ms a 19 ms por
frame la ganancia esperada es ~+0.8 FPS a 50 FPS: bajo el ruido de las ventanas (+- 0.8).

## 6. Decisión y lo que sigue

- **Criterio** (>= 0.5 ms menos de `GpuDraw` ocupado **y** ganancia visible de FPS en ventanas de >= 7000 cmds, VERIFY a 0): **no se
  cumple**. El bit 40 queda **puesto por defecto** (`runtime_memory.c`, máscara por defecto con el bit 40; `SOTC_DISPFAST=1/0` lo fija).
  Es seguro encenderlo (exacto, verificado), pero la ganancia esperada es de ~0.3 ms.
- El cambio de la telemetría y el diseño de `table_generation` van siempre (exactos, sin bit).
- Cualquier cambio de `buffer_cache.cpp`, `vk_rasterizer.cpp`, `vk_runtime.cpp` y `runtime_memory.c` pierde su perfil PGO en esta
  compilación; hay que reentrenar con `tools/pgo_train.sh` antes de empaquetar. El caché de shaders no cambia.
- Lo que queda en los dispatches y por qué no se tocó:
  - **Subidas de páginas escritas por el juego** (~1.2 ms): `VirtualProtect` 2.7 us por llamada + cerrojos de página; hace falta
    para detectar la próxima escritura. Las páginas que el juego reescribe una vez por frame nunca llegan a ser "calientes"
    (`FaultGrace` pide dos fallos en 1-2 ms); una ventana de gracia más larga evitaría el `VirtualProtect` a cambio de volver a
    subir las mismas páginas en cada binding: es una política de seguimiento, no una memoización, y no se probó.
  - **Replicador HLE** (~1 ms, 70% en las tablas de 4 y 8 MiB, comparación de DRAM a ~25 GB/s): solo no leer (generaciones por página
    que incluyan las escrituras del propio emulador) o repartir entre hilos (apagado por el bloqueo de arranque).
  - **`BindTextures` de los tres shaders de post-proceso** (~0.5 ms): `FindImage` solo es 0.2 ms; el resto es `UpdateImage`, vistas y
    transiciones de imágenes impuras para `TexBindMemo`.
  - **Memo de accesos de barrera** (`ResetBindings`, 0.3-0.6 ms): cae en cada `FlushBarriers`; ampliarlo no ayuda.
- **Comandos de hilos del juego** (prioridad 2): `FinishAsyncReadback` + `ApplyDownloads` son copias y comparaciones reales
  (`memcpy` 13% de las muestras del hilo, la guarda BPE ~1-3%, `CollectFreeTree`); lo único que se pudo quitar es la telemetría de
  `SOTC_FPS=1` (arriba). Mover trabajo a otro hilo exige demostrar el orden con los draws: sin diseño.
- Reproducir: `out/ab_disp.sh <nombre> <ABBA|BAAB> 40` (acepta `AB_EXTRA="--env SOTC_HASH_STATS=1"`), `out/ab_ops.py MIN_CMDS nombre:ABBA ...`,
  `out/ab_analyze.py`; verificador: `SOTC_DISPFAST=1 SOTC_DISPFAST_VERIFY=1 SOTC_WRITEMARK_VERIFY=1`; relojes: `SOTC_DISP_PHASES=1`.
