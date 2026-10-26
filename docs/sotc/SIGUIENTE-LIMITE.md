# Siguiente límite en el tramo pesado del templo (2026-10-26)

Árbol `sotc-port` (nuestro trabajo sobre 0.60.6-plus10), exe base `out/snapshots/port-2026-10-26/bb-probe.portpgo.exe`
(PGO). Escena: ruta grabada del templo (`out/bench_port.sh temple`), ventanas de 5 s con `SOTC_FPS=1`. 12 lanzamientos
de 12 usados, todos con `out/game.lock`. Marcas: [M] medido, [E] estimado.

## 1. Veredicto

En las ventanas de 7500 cmds/frame o más **el límite sigue siendo el hilo de dibujo (`GpuDraw`), no el CP ni la GPU**:

| Hilo (ventanas >= 7500 cmds, 6 + 7 ventanas de dos corridas con `SOTC_HASH_STATS`) | ms/frame | % del frame (20.5 ms, 48.7 FPS) |
|---|---:|---:|
| `GpuDraw` ocupado (frame menos espera de trabajo) | 19.1-19.3 | 93-94% |
| CP, tiempo dentro de `resume` (`command parse`) | 12.8-13.0 | 63% |
| CP, trabajo propio (parse menos esperas de anillo/drenado/etiqueta) | 11.3 | 55% |
| Recorder | 6.4 | 31% |
| GPU ocupada (`SOTC_GPU_TIME=1`, `SOTC_WAIT_MARKER=1`, una ventana de 8033 cmds, 52.9 FPS) | 10.1-10.6 | 53-56% |

- El `GpuDraw` espera trabajo solo 1.4 ms por frame; el CP espera el anillo 0.5-1.5 ms por frame (las dos holguras
  se reparten por fases: hay tramos limitados por el CP y tramos limitados por el dibujo). El CP trabaja 1.35 µs por cmd
  (8.4 k cmds) y no llega al hilo de dibujo: aún tendría que subir ~8 ms.
- La GPU no limita (53%). El proceso usa ~4.9 núcleos de 16 hilos lógicos: no hay contención de CPU.
- **Las ventanas pesadas no se repiten**: la ruta diverge entre corridas (en dos de las ocho corridas solo llegaron 1-2 ventanas de >= 7000
  cmds, 220 s). Los A/B dentro de la corrida reparten los estados en ABBA/BAAB y comparan con ANCOVA por cmds
  (`out/ab_analyze.py`), y aun así solo se juntan 9 + 9 ventanas pesadas.

### Dónde va el tiempo del hilo de dibujo (ventana pesada, ~8.4 k cmds, 48.5 FPS) [M]

`SOTC_HASH_STATS` (cubre el 80-82% de la ocupación; relojes incluidos):

| Tipo de operación | ops/frame | µs/op | ms/frame |
|---|---:|---:|---:|
| `DrawIndirect` | 6240 | 1.16 | 7.2 |
| `Draw` | 944 | 2.17 | 2.05 |
| `Dispatch` | 966 | 4.35 | 4.2 |
| `DispatchIndirect` | 361 | 5.26 | 1.9 |
| `Func` | 365 | 1.07 | 0.4 |
| **Operaciones** | | | **15.8** |
| **Comandos de hilos del juego (`ProcessCommands`: terminar lecturas de vuelta, guarda BPE, invalidaciones)** | | | **2.6-3.2** |
| Resto (bucle, anillo, relojes) | | | ~0.5 |

Los draws ya están a 1.16 µs por el trabajo anterior (memos al 99%: objetivo de render 99%, G-buffer 99%, bind de texturas 99.7%).
Lo que NO estaba medido y es nuevo: **los comandos de los hilos del juego son 2.6-3.2 ms por frame (14-16% del hilo)** en
el tramo pesado (1.2-1.6 ms en los tramos ligeros). Incluyen la telemetría de `SOTC_FPS=1` (bucle O(n^2) de lecturas en
vuelo en `BeginAsyncReadback`; unos 0.3 ms), así que en juego son algo menos.

Perfil de muestreo de `GpuDraw` en frames de > 19 ms (`SOTC_PROFILE=GpuDraw`, `SOTC_PROFILE_SLOW_MS=19`, 8043 muestras):
`ExecuteBackOp` 50% (`DrawIndirect` 27%, `Draw` 8%, `DispatchDirect` 22%, `DispatchIndirect` 11%), `ProcessCommands`
12.5% (`FinishAsyncReadback` 8%, `ApplyDownloads` 2.8%, guarda BPE ~1%), `ObtainBuffer` 19% (`SynchronizeMemory` 11%:
`CollectUploads` 7.7%, de ello **`host_protect` (VirtualProtect) 5.3%, ~1 ms por frame**, ~370 llamadas), replicador HLE
6% (1.15 ms), `GuestBytesEqual` 0.3%.

### Dónde va el tiempo del CP (mismo método, `GpuCommandProcessor`, 9190 muestras) [M]

26% en búsqueda de pipeline (`GetGraphicsPipeline` 19.7%, cómputo 6.6%; dentro, `RefreshFlatBuf` 11.8% = 2.4 ms por
frame, `StageSpecialization` 3.7%, `GetParams` 3.3%), `BackAlloc` 4.3%, `CaptureBackOp` 2.4%, caminata de SRT 3.2% con
`ReadClean` 2.5% (`RangeSet::Intersects` 2.2%), 35% esperando (submit/anillo). La tasa del memo de clave es 52%; las
reconstrucciones cuestan 1.44 µs (de ellas 38% dan la clave idéntica).

## 2. Candidatos y por qué no pasan 0.5 ms exactos en el hilo limitante

| Candidato | ms/frame (hilo) | Estado |
|---|---:|---|
| Memo de clave de pipeline más fino (ignorar los registros de parámetros de draw) | CP 0.1 | **Medido: solo +0.9 puntos de aciertos (52.8 -> 53.7%)**: casi todos los draws que fallan escriben registros de usuario o de contexto reales. El CP-PLAN §2.2 (0.3-0.7) estaba sobreestimado. |
| Memo de especialización por etapa (CP-PLAN §2.1) | CP 0.7-1.0 [E] | No implementado: máscara de bits de los sharps que lee `StageSpecialization`; no es exacta sin auditar cada campo, y el CP no limita. |
| Atajo de `Nop` y guarda por paquete | CP 0.4 [M] | Hecho (abajo). |
| Subidas dentro de dispatches (~259/frame, 1.3 ms) | dibujo 1.3 | La mitad es `VirtualProtect` (2.7 µs por llamada, ~370/frame). El agrupado de protecciones ya existe (`SOTC_PROTECT_BATCH`) y no ahorra llamadas: los rangos no son contiguos. Sin camino exacto. |
| Replicador HLE (1.6 ms/frame, de ellos 1.1 ms en tablas de 4 y 8 MiB) | dibujo 0.3-0.4 si se paraleliza | La comparación está limitada por DRAM (24.8 GB/s de lectura); repartida en hilos subiría a ~36 GB/s: ahorro 0.35 ms, bajo el umbral, y el comparador paralelo se apagó por un bloqueo de arranque. Quitar la lectura exige vigilancia de escritura por página (generaciones de página incluidas las escrituras del emulador: lecturas de vuelta, `NoteHostWrite`); riesgo medio-alto. Es el único candidato con > 1 ms. |
| Guarda BPE (1 construcción por frame, ~1 ms según el reloj, ~0.5 ms según el perfil) | dibujo 0.5-1 | Sin camino exacto: la guarda se construye "fresca por lote" a propósito (CRASH-2026-10-23). |

## 3. Cambio hecho: `BbToggle::CpFastPaths` (bit 20, **apagado por defecto**)

Bit puesto = ruta antigua. Tres piezas, todas del CP, exactas, honradas en vivo:

1. **Atajo de `Nop` (`liverpool.cpp`, `IsInertNop`)**: una racha de paquetes `Nop` sin carga o con carga que
   `ProcessGraphics` ignoraba (todo salvo `PatchedFlip`, y salvo los marcadores de depuración cuando el CP los atiende
   sin hilo de dibujo) se salta sin `ProcessCommands()` ni el `switch`. Los comandos ordenados que `ProcessCommands`
   habría dejado pasar se toman en el siguiente paquete que hace algo: ninguna operación entre medias, mismo orden.
2. **Guarda `PollCommands()`**: con hilo de dibujo, el CP solo atiende `num_ordered` en `ProcessCommands`; sin comandos
   ordenados la llamada por paquete no se hace.
3. **Memo de clave "fino"**: `Liverpool::KeyDirty()` (se levanta con cualquier `MarkRegs` salvo los registros que escribe
   el propio paquete de draw: tipo de índice, cuentas, base, iniciador, instancias, marcados con `MarkParamRegs`; se
   baja al reconstruir la clave). Sustituye a "algún trozo de registros escrito desde la última operación".

### Verificación (`SOTC_CP_VERIFY=1`)

En cada acierto del memo de clave reconstruye la clave, la compara con la del pipeline devuelto y restaura el estado que
la reconstrucción toca (clave, tablas de etapas, `RuntimeInfo`, datos por draw del CP). Corrida de 150 s con el bit
cambiando cada 5 s (ABBA): **26.2 M aciertos comprobados, 0 discrepancias con la regla fina; 2 con la regla antigua**
(`pipeline key memo hit (old rule, chunks dirty 0) but the key differs`). La regla antigua tiene un agujero conocido: un
dispatch entre dos draws consume los trozos sucios en `BackAlloc` y el draw siguiente acierta con un pipeline viejo
(1 caso por ~6 M de aciertos). La regla fina no lo tiene (`KeyDirty` no lo borra el dispatch): con el bit encendido el
resultado puede diferir del antiguo solo en esos casos, y a favor de la corrección. El atajo de `Nop` y la guarda no
necesitan verificador: no cambian ningún estado observable (salvo cuándo se toman los comandos ordenados, sin
operaciones entre medias).

### A/B dentro de la corrida (mismo binario, ruta del templo; A = encendido, B = bit puesto)

ANCOVA por cmds/frame (`out/ab_analyze.py`), efecto de A frente a B:

| Corridas | Ventanas | FPS | `GpuDraw` ocupado (ms) | CP trabajo propio (ms) |
|---|---|---:|---:|---:|
| ABBA + BAAB, `Nop` + memo fino | >= 7000: 9; >= 5000: 57 | +1.36 +- 0.95; +1.32 +- 1.01 | -0.51 +- 0.55; -0.41 +- 0.25 | -0.37 +- 0.19; -0.30 +- 0.12 |
| ABBA + BAAB, + `PollCommands` | >= 7000: 9; >= 5000: 56 | +0.49 +- 0.74; -0.34 +- 0.83 | +0.08 +- 0.37; +0.18 +- 0.19 | -0.38 +- 0.12; -0.24 +- 0.11 |
| Las cuatro, >= 7000 | 18 | **+1.14 +- 0.74** | -0.35 +- 0.38 | **-0.39 +- 0.11** |

Aciertos del memo de clave: 53.7% (A) frente a 52.8% (B).

**Resultado**: el CP ahorra 0.39 +- 0.11 ms por frame (3.5% de su trabajo), pero el hilo de dibujo, que limita, no se
mueve (-0.35 +- 0.38 ms) y el FPS sube +1.1 +- 0.7 en las ventanas pesadas (no distinguible del ruido; el segundo par,
con el binario final, +0.5 +- 0.7). **No cumple el criterio (>= 0.5 ms en el hilo limitante y ganancia de FPS visible)**:
queda **apagado por defecto** (`runtime_memory.c`, bit 20 en la máscara por defecto). Se enciende con `SOTC_CP_FAST=1` o con
una máscara de `BB_TOGGLE_FILE` sin el bit 20 (las dos líneas `Runtime: disabled optimizations mask=...` lo muestran).
Con la máscara por defecto el humo de la ruta del templo da 50.5-53.5 FPS en las 6 ventanas pesadas, sin errores.

Nota de PGO: las funciones cambiadas (`ProcessGraphics`, `GetGraphicsPipeline`, `BackLoop`, `BackAlloc`...) pierden su
perfil en esta compilación incremental; los dos lados del A/B comparten ese efecto. El exe recomendado sigue siendo el
de `out/snapshots/port-2026-10-26` hasta reentrenar con `tools/pgo_train.sh`.

## 4. Diagnósticos nuevos (solo con `SOTC_FPS=1`)

- `SOTC CP own work X ms per frame (parse minus waits: ring, drain, label)`: el trabajo propio del CP.
- `SOTC draw thread guest commands X ms per frame`: tiempo de `ProcessCommands` en el hilo de dibujo.
- `SOTC CPU time per frame: draw thread, command processor, process (cores)`: tiempo de CPU (`GetThreadTimes`). Los hilos
  giran cuando esperan, así que el CPU del dibujo y del CP es ~100% del frame: solo el total del proceso (4.9 núcleos)
  informa de contención.
- `SOTC pipeline key memo: N% of M draws (K% of the draws only with the fine rule)`.
- Scripts: `out/ab_temple.sh <nombre> <ABBA|BAAB> <bit>...` (acepta `AB_EXTRA="--env X=Y"`), `out/ab_analyze.py MIN_CMDS
  nombre:patrón ...`, `out/pair.sh` (empareja las líneas de hilos con la de FPS: la línea de hilos va ANTES de la de FPS).

## 5. Qué sigue

1. El mayor trozo exacto restante del hilo limitante es la lectura del replicador (1.1 ms) y las protecciones de
   páginas (~1 ms), ambos de seguimiento de memoria. Exigen generaciones por página que incluyan las escrituras del
   propio emulador, con verificador que compare y cuente.
2. Mover trabajo del hilo de dibujo (2.6-3.2 ms de comandos de hilos del juego: terminar lecturas de vuelta y guarda BPE)
   a otro hilo exige demostrar el orden con los draws: sin diseño aún.
3. Reentrenar PGO después de cualquier cambio de `liverpool.cpp`.
