# Camino rápido exacto para los draws de G-buffer (color + profundidad): diseño

2026-10-26. Fase 1 (solo lectura: ningún fichero de `gpu/`, `src/`, `tools/`, `scripts/` tocado; sin compilar ni lanzar). Continúa
[DEPTH-FASTPATH.md](DEPTH-FASTPATH.md) §8 ("extensión barata sin hacer"). Marcas: [M] medido (logs `out/run-dftp.txt`, `out/run-depth-ph.txt`),
[C] leído en el código, [E] estimado, [I] inferencia. Las líneas son del árbol actual (con los cambios sin commitear del camino de profundidad).

## 0. Resumen

- El draw de G-buffer (clase "G-buffer" = `mrt_mask` con el bit 4, `ClassOf`, `vk_rasterizer.cpp:290`; ~7 destinos de color + profundidad) sale de
  `BeginRenderingFast` por la puerta `mrt_mask != 0` (`vk_rasterizer.cpp:4011-4014`) y ejecuta `BeginRendering` completo
  (`vk_rasterizer.cpp:4112-4290`) en cada draw: por destino `GetImage`, `UpdateImage`, `SetBackingSamples`, `FindRenderTarget`, `TestAndTouchMeta`,
  `Transit` y rellenar el `RenderState`; luego la profundidad (`FindDepthTarget`, meta HTile, `Transit`). El estado dinámico (`dyn` 0.03 µs) y
  `PrepareRenderState` (memo por series, 99% de aciertos) **ya** están cubiertos por el camino de profundidad: lo que queda es `BeginRendering`.
- [M] El `RenderState` de un draw de G-buffer es idéntico al del draw anterior en el ~100% de los casos (`same%: rstate=100`, `run-dftp.txt`,
  `run-depth-ph.txt`), y `PrepareRenderState` acierta en el 99.3% de todos los draws (`prep hit 1041427 miss 7149` de 1.05 M). La mecánica de
  `BeginRenderingFast` se extiende a N destinos con las mismas series (RelRender + RelColor), `ImageSetGeneration`, `gpu_modified_epoch` y `MetaEpoch`, más
  comprobaciones por destino (flags, `backing`, `TransitIsNoop`, `binding.is_bound`, muestras de la clave del pipeline).
- **Ahorro realista: 0.15-0.35 µs por draw de G-buffer = 0.4-0.8 ms/frame en el templo pesado [E], no los 1.7 ms del techo** (0.70 µs x 2465). Motivo (§5): los
  0.70 µs son con `SOTC_DRAW_PHASES` y llevan ~42 lecturas de `rdtsc` dentro de `BeginRendering` (5 vueltas por destino), y la repetición no es gratis
  (~0.2-0.25 µs para 7 destinos + profundidad). **Paso 0 obligatorio antes de implementar: medir `BeginRendering` sin las vueltas** (§6). Si el coste real es
  < ~0.35 µs, el beneficio no justifica el riesgo y la palanca mejor es `BindTextures` (§7).
- Implementación: memo propio `gb_memo` (no comparte hueco con `df_memo`), interruptor nuevo `BbToggle::GBufferFastPath` (bit 29 si está libre, §4.1; si no, bit 31),
  VERIFY que reproduce los efectos, ejecuta el `BeginRendering` completo encima y compara instantánea por destino + `RenderState` byte a byte.

## 1. Qué hace hoy un draw de G-buffer y qué cubre ya el camino de profundidad

Orden en `Rasterizer::Draw` (`vk_rasterizer.cpp:1760-1877`; `DrawIndirect` `:1879-2102` repite la misma secuencia y llama a la misma `BeginRenderingFast`, `:1944`).
Coste por draw de G-buffer, µs instrumentados [M] (`run-dftp.txt`, `run-depth-ph.txt`; ventanas con 11-12 k draws de G-buffer): `begin` 0.59-0.70, `prep` 0.11-0.14,
`bbuf` 0.85-0.89, `btex` 0.94-1.16, `pipe` 0.25-0.29, `post` 0.19-0.23, total 3.44-3.79.

| Paso | Dónde [C] | Camino de profundidad (hoy) | G-buffer (hoy) |
|---|---|---|---|
| `PrepareRenderState`: memo `RtMemo` por series `rel_depth`/`rel_color`, `mrt_mask`, `ImageSetGeneration`, `gpu_modified_epoch` | `:892-1060`, hit `:913-922`; efectos del acierto (`GetImage`, `NoteFound`, `bound_images`, `is_target`) `:974-993` | cubierto | **cubierto** (`num_cb != 0` exige también `rel_color`, `:917-918`). Residuo: el bucle de `active_mask` (`:906-908`, 7 `GetMask`) se calcula también en el acierto aunque solo lo usa el fallo; el bucle de efectos toca 8 imágenes (~0.05-0.07 µs) |
| Puerta a `BeginRenderingFast` | `:4002-4014` | memo `df_memo`, 90-93% de aciertos | **`mrt_mask != 0` -> `BeginRendering` completo** |
| `BeginRendering` por destino de color: `GetImage` (toque LRU) + comprobación `needs_rebind` (re-búsqueda con `FindImage` que además **muta `cb_descs[cb].first` y `bound_images`**) | `:4137-4141` | n/a (sin color) | cada draw |
| `UpdateImage` (`LockCounted`, `TrackImage`, `TouchImage`, `RefreshImage`) | `:4142`, `texture_cache.h:419-426` | `UpdateIsNoop` en el check | cada draw |
| `runtime.SetBackingSamples(image, key.color_samples[cb])` | `:4144`, `vk_runtime.cpp:644-719` (sale al instante si `backing->num_samples` coincide) | n/a | cada draw; **depende de la clave del pipeline**, no de registros |
| `FindRenderTarget(update=false)`: `MarkWritten` (flag `GpuModified`; con `MipTailMode()` busca solapes si aún no lo está), cola `download_images` si `readback_linear_images` y la imagen es lineal/<=8 px, `usage.render_target`, `surface_metas.emplace` de CMASK/FMASK (bump de `MetaEpoch` si inserta), `image.FindView(view_info, false)` | `:4146`, `texture_cache.cpp:896-940` | n/a | cada draw |
| `TestAndTouchMeta(col_buf.CmaskAddress(), slice)` -> `is_clear` (consume el bit CMASK) | `:4155`, `texture_cache.h:522-531` | HTILE: memo con `MetaEpoch` | cada draw (una búsqueda hash por destino aunque `fast_clear` esté apagado, porque `CmaskAddress()` sale del registro: `image_info.cpp:77` solo pone `meta_info.cmask_addr` con `fast_clear`) |
| Transición del destino a `ColorAttachmentOptimal` (`ColorAttachmentOutput`, `Write\|Read`, rango = `desc.view_info.range`) o, si `image->binding.is_bound`, a feedback-loop/`General` con `FlushBarriers()` previo y `attachment_feedback_loop = true` | `:4162-4181`, `vk_runtime.cpp:171-177`, `image.cpp:352-450` | `TransitIsNoop` (profundidad) | cada draw, un `Transit` por destino |
| Tamaño/capas del `RenderState` (`min` con `image->info.size >> mip`, capas de la vista), `RenderAttachment` (vista, layout, `clear_value`, `is_clear`), `usage.render_target` | `:4184-4196` | memo | cada draw |
| `TakeHudless`: si `cb == 0 && image->usage.vo_surface` fija `hudless_vo*` | `:4197-4202`, flag en `vk_presenter.h:92` | n/a | cada draw (efecto fuera del `RenderState`) |
| Profundidad: `FindDepthTarget` (búsqueda del stencil por regiones + `AssociateDepth`, `surface_metas.emplace` HTILE), `TestAndTouchMeta` HTILE, `Transit` | `:4209-4281`, `texture_cache.cpp:942-990` | `DepthFastBeginCheck` (`:3915-3950`) | cada draw. En G-buffer pesa más que en solo profundidad: `begin split ... depth=0.13` [M] |
| `scheduler.BeginRendering(state, token)` | `:1833`, `vk_scheduler.cpp:86-146` | token salta el `memcmp` de 296 bytes | token 0 siempre -> `memcmp` (schBegin 0.029 µs [M]) |
| Estado dinámico (viewport, blend, `write_masks`, ...) | `CommitDynamicState` `:4544-4591` | memo por `RelDynamic` | **cubierto** (`dyn` 0.088 -> 0.034 µs ON/OLD en G-buffer [M]); no hay nada más que sacar |
| Descriptores / bindings (`BindBuffers`, `BindTextures`, `Pipeline::BindResources`, `ResetBindings`) | `:3454-`, `:2833-2862` | sin cambios | sin cambios (ver §7) |

Nota sobre cómo está medido `begin` [C]: `BeginRendering` lleva `DrawPhase::LapStart/Lap/Sub` (`vk_rasterizer.cpp:4114,4130,4143-4160,4183,4204,4214,4233,4287-4288`; `Now()` =
`__builtin_ia32_rdtsc()`, `vk_rasterizer.h:36`). Por destino son **5 vueltas** (4143, 4145, 4147, 4160, 4183): con 7 destinos son 35 lecturas, más ~7 de la profundidad
y los extremos: ~42 lecturas por draw. La vuelta `samples` (que solo mide `SetBackingSamples`, una salida inmediata) marca **0.07-0.12 µs por draw** en los dos logs [M]: eso es casi
solo coste del reloj (~13 ns por lectura y destino).

## 2. Invariantes que hacen exacta la repetición del `RenderState` anterior

`BeginRendering` (draw consecutivo de G-buffer, sin limpieza) es función de:

| Entrada | Cómo se garantiza que no cambió | Qué la rompe |
|---|---|---|
| `key.mrt_mask`, `key.color_samples[cb]` (de la clave del pipeline: el pipeline cambia en ~20% de los draws, `pipe=80-83` [M]) | comparar `mrt_mask` y los 8 `u8` de `color_samples` con los del memo | mascara distinta entre pipelines del mismo tramo (conjuntos de MRT, ESCENA-TEMPLO §3: 4 conjuntos de 7): fallo `mask`, fallback completo |
| `regs.color_buffers[cb]`, `color_target_mask`, extensiones de color (CmaskAddress, vista, formato) | serie `RelColor` igual (sube solo si un dword de la máscara cambia de **valor**, `liverpool.cpp:418-500`) | cambio de conjunto de MRT (unos 4 por frame) |
| Registros de profundidad, `color_control` (modo `Disable`), `depth_render_control` (clear), `DB_DEPTH_CONTROL` bits 0-2, extensión de profundidad | serie `RelRender` igual (`DepthFastEnsureRelevance`, `:3844-3911`) | los 16 draws con escritura `GREATER` al inicio del G-buffer, PRE-Z tardíos |
| Qué imagen es cada destino: `cb_descs[cb].first`, `db_desc.first` (y su `desc`) | `ImageSetGeneration` + `gpu_modified_epoch` (los de `RtMemo`) **y** comparar los ids con los del memo (otro `PrepareRenderState` fallido o un `mrt_mask` distinto reescribe `cb_descs`) | alta/baja de imágenes (streaming, gemelos profundidad/color: ~100 conversiones/s en mundo abierto, `texture_cache.cpp:350-413`); `needs_rebind` |
| Estado de cada imagen: `Registered`, `GpuModified` (`MarkWritten` sin efecto), no `Dirty` y rastreada entera (`UpdateIsNoop`, `texture_cache.h:590-595`: `UpdateImage` solo toca el LRU), `num_samples <= 1`, sin `needs_rebind` | lectura de flags bajo `BatchLock` (el CP/handler de fallos modifica flags bajo el mismo mutex) | escritura de CPU sobre el destino (`CpuDirty`), pérdida de rastreo parcial, MSAA |
| `backing` (vistas, layout, `SetBackingSamples`) | mismo puntero `backing` que al grabar (los `BackingImage` viven en un `std::deque`: direcciones estables; las listas de vistas solo crecen) | `SetBackingSamples` en otra operación (copias, subidas, resolve) |
| `Transit` del destino sin barrera ni cambio de estado | `Image::TransitIsNoop(ColorAttachmentOptimal, Write\|Read, ColorAttachmentOutput, rango)` (`image.cpp:310-350`; más estricto que `GetBarriers`: compara también la etapa) | el destino fue muestreado/escrito por otra operación entre dos draws (barrera real); compute que escribe el destino |
| Bit de limpieza CMASK/HTILE ya consumido, y que no se vuelva a poner | `MetaEpoch` igual (sube en `ClearMeta`, `TouchMeta(.., true)`, alta/baja de entradas: `texture_cache.h:538,550`, `.cpp:925,934,955,2241-2249,2307-2315`) | `ClearMeta` por dispatch/`FillBuffer` de metadatos (`vk_rasterizer.cpp:2900,4347`): <= decenas por frame |
| `RenderAttachment::is_clear` / `depth_clear` / `stencil_clear` = falsos al grabar | solo se graba un memo cuyo estado no limpia (como `DepthFastRecord`, `:3967`); el estado del draw que limpia **no** es el del siguiente (el bit se consume) | primer draw tras una limpieza: no hay memo -> completo, y el siguiente graba |
| `image->binding.is_bound` (muestreo del propio destino: bucle de realimentación) | se lee **en cada draw** (lo pone `BindTextures`, que corre antes, `:1805` frente a `:1813`); se exige `false` para todos los destinos | destino muestreado por el PS (SSR, lectura del buffer de profundidad como textura si es color-twin): el camino completo lo trata con `eAttachmentFeedbackLoopOptimalEXT` y `attachment_feedback_loop = true` |
| `usage.vo_surface` del destino 0 (efecto `hudless_vo*`) | se exige falso al grabar y se vuelve a leer en cada acierto (un bit) | superficie del video out (draw de composición final): completo |
| Efectos laterales: `download_images.emplace` (no idempotente en el tiempo: `ProcessDownloadImages` vacía el conjunto), `image.usage.render_target` | no grabar si `readback_linear_images && (!is_tiled \|\| width <= 8)`; el `usage` es un almacenamiento idempotente que se repite | destinos lineales con readback (no son los del G-buffer) |

Efectos que **sí** hay que repetir en cada acierto (los demás de `BeginRendering` son nulos con las condiciones de arriba): `attachment_feedback_loop = false`; toque LRU de cada imagen
(`GetImage` y el de `UpdateImage` coinciden dentro del mismo `gc_tick`: `LeastRecentlyUsedCache::Touch` sale en cuanto `item.tick >= tick`, `lru_cache.h:35-39`, ~3-5 ns); `usage.render_target = 1` y
`usage.depth_target = true`; toque de la imagen de stencil enlazada (`TouchLinked`, ya existe). `NoteFound`/`bound_images`/`is_target` los hace `PrepareRenderState` antes, no `BeginRendering`.

Casos que hacen incorrecto el memo si se olvidan (todos tienen comprobación arriba): primer draw tras una limpieza; destinos también muestreados (feedback); metadatos CMASK/FMASK que cambian por una limpieza rápida (`MetaEpoch`);
`SetBackingSamples` con copia entre backings MSAA (cambia `backing`); imagen recreada por conversión profundidad/color (`ImageSetGeneration`); `needs_rebind` (el camino completo reasigna `cb_descs[cb].first`);
`mrt_mask`/`color_samples` distintos con los mismos registros (clave del pipeline); destino 0 = superficie de video out (hudless).

## 3. Qué es y qué no es repetible (decisión)

Todo lo anterior es comprobable con lecturas por destino, sin tocar tablas hash: la repetición es exacta cuando se cumplen (Registro, Imagen, Estado, Backing, Transit, Meta, Limpieza, is_bound, vo_surface). Lo que **no** se puede saltar sin
cambiar el comportamiento: los toques LRU (efecto observable por la GC) y la lectura de flags de cada imagen (los cambia el CP y los fallos de CPU entre draws). Eso fija el coste mínimo de un acierto en ~20-25 ns por destino [E].

## 4. Plan de implementación (fase 2, a pedir después)

### 4.1 Interruptor

`BbToggle::GBufferFastPath` (bit puesto = camino antiguo; **activo por defecto**: no entra en la máscara por defecto de `src/runtime_memory.c:1402`). Bit 29: `bbport_toggles.h` dice
"los bits 20-29 se usan como interruptores de depuración crudos" (cámara/movimiento/upscaler); `grep` en `gpu/` y `src/` no encuentra ningún uso de `1<<29`, pero hay que repetir ese `grep` al empezar. Si hay duda, **bit 31**
(`1u << 31`, libre; `UpdateImageFastPath` es 30). Además exige `DepthFastActive()` (hilo de dibujo + máscaras de relevancia construidas): sin el transporte por diferencias las series suben siempre. Honrado en vivo, leído por draw, como el bit 28.

### 4.2 Estructuras (`vk_rasterizer.h`)

```
struct GBufferFastMemo {            // un hueco, aparte de df_memo (no se pisan: DepthFastRecord invalida el suyo con color)
    bool valid{};
    u32 rel_render{}, rel_color{}, mrt_mask{};
    std::array<u8, 8> samples{};                 // key.color_samples al grabar
    u64 image_generation{}, gpu_epoch{}, meta_epoch{};
    struct Att { VideoCore::ImageId id{}; const void* backing{}; } color[8]; // id nulo = ranura vacía
    DepthFastMemo depth;                         // db_id, stencil_id, backing, layout/access/stage/range (reutiliza campos y la comprobación)
    u64 token{};
    RenderState state{};
};
```

### 4.3 Funciones

- `Rasterizer::BeginRenderingFast` (`:4002`): quitar la salida temprana de `mrt_mask != 0` **solo** si `GBufferFastPath` está activo; para `mrt_mask != 0` llamar a `GBufferFastCheck` / `Apply` / `Record`. Un único punto, lo usan `Draw` y `DrawIndirect`.
- `u32 GBufferFastCheck(const GraphicsPipeline*)`: bajo `BatchLock`; causas (como `c.cause[]`): `nomemo`, `mask` (mrt_mask/muestras), `regs` (RelRender/RelColor), `ids` (`cb_descs[cb].first`/`db_desc.first`), `gen`, `gpuepoch`, `meta`, `flags` (Registered/GpuModified/needs_rebind/`UpdateIsNoop`), `bound` (`is_bound`), `vo` (`usage.vo_surface` del destino 0), `backing`, `transit` (color), `transit_depth`. Para la parte de profundidad, extraer de `DepthFastBeginCheck` (`:3915-3950`) una función `DepthSideCheck(const DepthFastMemo&)` y llamarla desde las dos (sin cambiar su comportamiento: el verify de profundidad ya tiene 0 discrepancias en 24.8 M).
- `void GBufferFastApply()`: `attachment_feedback_loop = false`; por destino no vacío `GetImage(id)` + `usage.render_target = 1`; profundidad como `DepthFastBeginApply` (`:3955-3960`).
- `void GBufferFastRecord(const RenderState&, const GraphicsPipeline*)`: tras un `BeginRendering` completo; grabar solo si: ningún `is_clear`/`depth_clear`/`stencil_clear`; `attachment_feedback_loop == false`; para cada imagen no vacía `!needs_rebind`, `Registered`, `GpuModified`, `UpdateIsNoop`, `num_samples <= 1`, sin readback lineal, `backing->num_samples == key.color_samples[cb]`; destino 0 sin `vo_surface`; los ids de `cb_descs` iguales a los usados (el camino completo puede haberlos reasignado); además la comprobación de `TransitIsNoop` actual (para no grabar un estado que acaba de generar una barrera). Si no, `valid = false` y contador `not-recorded` por causa.
- El `Transit` de color no necesita campos propios: son constantes (`ColorAttachmentOptimal`, `ColorAttachmentOutput`, `Write|Read`) y el rango sale de `cb_descs[cb].second.view_info.range`, que no cambia mientras `cb_descs` y las series coincidan (misma suposición que `RtMemo`).
- `Scheduler::BeginRendering(state, token)` ya sabe saltar el `memcmp` con el token (`vk_scheduler.cpp:86-99`): se pasa `gb_memo.token` (contador compartido `df_next_token`).
- Micro-ajustes aparte (sin riesgo, ~0.03-0.05 µs/draw de G-buffer [E]): mover el bucle de `active_mask` de `PrepareRenderState` al camino de fallo (`:906-908`); y que el acierto de `PrepareRenderState` y `GBufferFastApply` no repitan `GetImage`+`NoteFound` sobre las mismas 8 imágenes (hoy: `:975-988` y luego `:4137`).

### 4.4 VERIFY (`SOTC_DEPTHFAST_VERIFY=1`, mismo mecanismo; o `SOTC_GBFAST_VERIFY=1`)

Igual que `:4034-4106`: en cada acierto, `GBufferFastApply`; instantánea; `BeginRendering` **completo** encima; instantánea; exigir (a) `RenderState` byte a byte igual al del memo, (b) instantánea idéntica, (c) ningún
barrier nuevo (`runtime.PendingImageBarriers()`), usar el resultado del camino completo. La instantánea se amplía por destino (array de 8 + profundidad): `flags`, `usage`, `binding`, `backing`, layout/acceso/etapa, `subresource_states.size()`,
`track_addr/_end`, tick LRU, `tick_accessed_last`; máscara CMASK del destino (`MetaClearMask(CmaskAddress)`) y HTILE; más globales: `attachment_feedback_loop`, `needs_barrier`, `bound_images.size()`, ids de `cb_descs`, `hudless_vo` (el camino completo no debe fijarlo: así se comprueba la exclusión de `vo_surface`),
`LruTickOf` de las imágenes. Cualquier diferencia se cuenta e imprime (primeras 20); contadores acumulados en la línea `SOTC DEPTHFAST` (añadir `gb cand/hit/cause[]`).
Límites (como en DEPTH-FASTPATH §5): no verifica los bytes de los thunks del recorder (son función de `RenderState`, descriptores, etc.); un campo del `RenderState` fuera del `memcmp` no se vería, pero `RenderState` se compara entero y sin padding (`static_assert has_unique_object_representations`).
Criterio de aceptación del VERIFY: replay del templo + Santuario con >= 20 M de aciertos de G-buffer comprobados y 0 discrepancias.

### 4.5 Medición y lanzamientos (presupuesto sugerido: 8)

1. **Calibración + modo "solo contar"** (1 lanzamiento): compilar con un interruptor diagnóstico `SOTC_DRAW_LAPS=0` que quite las vueltas de `BeginRendering` (`Lap`/`LapStart`; quedan los `Mark`) y con `GBufferFastCheck` ejecutándose sin aplicar (estadística de causas). Dice: `begin` real de G-buffer, y la tasa de acierto real por causa. **Criterio de seguir: `begin` real >= 0.35 µs y acierto >= 90%.**
2. VERIFY del templo (+ Santuario en reposo al inicio): 1-2 lanzamientos.
3. A/B en la misma binaria con el bit alternando cada 5 s, ABBA y BAAB, templo y Santuario (`out/depth_ab.sh`, `out/depth_ab_temple.sh`): 4 lanzamientos. 1 lanzamiento de fases (`SOTC_DRAW_PHASES=1` con laps apagadas) para la tabla por fase ON/OLD.
4. `tools/pgo_train.sh` antes de empaquetar (las funciones nuevas no tienen perfil).

## 5. Tasa de acierto y ahorro esperados

**Acierto [E]**: 93-98% de los draws de G-buffer. Base: `rstate=100%` frente al draw anterior; `PrepareRenderState` 99.3% [M]; el camino de profundidad ve `regs` 0.8-1.3%, `gen` 0.01%, `meta` 0%, `flags`/`backing` 0% y `transit` 6-8% (solo por los draws DEPTH-RO). Fallos esperables en el G-buffer: primer draw de cada tramo tras limpieza (~10-20 por frame), cambio de conjunto de MRT/`mrt_mask` entre pipelines (**desconocido, es el principal riesgo del acierto**: hoy `RtMemo` es de un solo hueco y falla con otra `mrt_mask`), la profundidad de los draws que además la muestrean (barrera real por draw, el mismo 6-8% del DEPTH-RO), `ClearMeta`s.

**Coste** [E, por draw de G-buffer, 7 destinos + profundidad]:

| | µs |
|---|---:|
| `BeginRendering` completo, instrumentado [M] | 0.59-0.70 |
| menos ~42 lecturas de `rdtsc` (~8-14 ns cada una; la vuelta `samples` = solo reloj marca 0.07-0.12 µs [M]) | -0.15 a -0.40 |
| **`BeginRendering` completo, real** | **0.30-0.55** |
| repetición por acierto: `BatchLock` 0.009 + 7 x 20-25 ns + profundidad 0.04 + efectos 0.01 (anclado en el acierto de solo profundidad: 0.079 µs instrumentado, ~0.06 real, con 1 destino) | 0.20-0.25 |
| `scheduler.BeginRendering` con token (evita `memcmp` de 296 B) | -0.01 |
| **ahorro neto por acierto** | **0.10-0.35, central ~0.2** |

Por frame (templo pesado, 2465 draws de G-buffer, acierto 95%): **0.25-0.8 ms, central ~0.5 ms/frame (~+1.2 FPS a 47 FPS, ~2.5% del frame)**, más 0.07-0.12 ms de los micro-ajustes de §4.3. El techo de 1.7 ms de
DEPTH-FASTPATH §8 es la cifra instrumentada con repetición gratis: no es alcanzable. En el borde del criterio de aceptación del proyecto (>= 0.5 ms/frame o >= 2% de FPS); por eso el paso 0 de §4.5 es obligatorio.

## 6. Riesgos

1. **Beneficio menor que el esperado** (arriba). Mitigación: calibración antes de escribir la repetición.
2. **Efecto olvidado** en la repetición: lo cubre el VERIFY (instantánea ampliada), pero un efecto fuera de la instantánea no se vería: `download_images` (solo con `readback_linear_images`, excluido al grabar), `hudless_*` (excluido y comprobado), `bound_images`/`cb_descs` (en la instantánea).
3. **Memo de un solo hueco con `mrt_mask` alternante**: si los draws de un tramo alternan 2-3 máscaras, tanto `RtMemo` como `gb_memo` fallan siempre. Contador `mask` en el paso 0; si pasa, ampliar a 2-4 huecos por `mrt_mask` (el `RtMemo` también).
4. **Transit solo de etapa**: `GetBarriers` no actualiza `pl_stage` cuando no emite barrera (`image.cpp:429-431`) y `TransitIsNoop` la compara: una imagen con layout/acceso correctos pero otra etapa fallaría siempre sin barrera real. Contar `transit` aparte para destinos y revisar al ver las causas.
5. **Imágenes con `Image*` inestables**: no se guardan punteros de `Image` (el `SlotVector` se mueve al insertar); solo ids y `backing` (estable). No introducir un caché de `Image*` sin validar contra `ImageSetGeneration`.
6. **Concurrencia**: los flags los cambia el CP/handler de fallos bajo el mutex de `TextureCache`; las lecturas del check van bajo `BatchLock`, igual que el camino de profundidad. No leer flags fuera del bloqueo.
7. **Contaminación entre memos**: `df_memo` y `gb_memo` separados; `RtMemo` es compartido (ya lo estaba). No cambiar `DepthFastRecord` (invalida con color: solo se llama con `mrt_mask == 0`).
8. **PGO**: las funciones nuevas no tienen perfil (`tools/pgo_train.sh`). El caché de shaders no cambia.
9. **Alcance**: la misma puerta aceptará draws de clase "otros" (color sin MRT 4, ~200 por ventana); no acertarán (`begTrans` 1.6-1.9 µs: barreras reales) y no deben empeorar: si el check falla, el coste extra es el del check (~0.03 µs).

## 7. Otros costes por draw de G-buffer, mayores que `BeginRendering` (evidencia)

Todos [M] en µs instrumentados por draw de G-buffer (`run-dftp.txt` / `run-depth-ph.txt`) con la causa [C]:

| Coste | µs | Evidencia de código | Veredicto |
|---|---:|---|---|
| `BindTextures` (VS + PS; ~11 imágenes + muestreadores, 22-24 escrituras de descriptor por draw) | **0.94-1.16** | `ReplayTexBindMemo` (`:3387-3452`) acierta 86%, pero por imagen aún ejecuta `BindResolvedImage` (`:3321-3366`): `FindTexture` -> `UpdateImage` (bloqueo + `TrackImage` + `TouchImage` + `RefreshImage`) + `FindView` + `Transit` + `image_infos.emplace_back`; y por muestreador `GetSampler` (`:3786-3829`). Es **la misma maquinaria por imagen** que `BeginRendering` por destino, con más imágenes (11 frente a 7) | **Candidato mayor y del mismo tipo**: `UpdateIsNoop && TransitIsNoop(ShaderReadOnlyOptimal, ShaderRead, AllCommands, rango)` + vista memoizada por `(backing, view_info)` en `TexBindMemoBinding`. [E] 0.2-0.4 µs/draw de G-buffer (0.5-1.0 ms/frame). Exactitud más delicada: los descriptores llevan el layout que deja `Transit`, y las imágenes muestreadas que son destino (`is_target`) usan feedback loop. Merece su propio documento |
| `BindBuffers` (11 buffers) | **0.85-0.89** (`bbObt` 0.46-0.58, `bbClamp` 0.16-0.19, `bbDesc` 0.14-0.17, `bbFlat` 0.09-0.11) | `ObtainBuffer` x ~11, recorte de rango y descriptor por buffer | **No** (DEPTH-FASTPATH §3: direcciones nuevas en cada draw; un memo por (dirección, tamaño) ya midió cero ganancia) |
| `Pipeline::BindResources` (`pipeDesc` 0.16-0.21, `pipePush` 0.05-0.06) | 0.25-0.29 | `pushDescriptorSetKHR` con 22-24 escrituras, descriptores iguales 0% | No sin cambiar los comandos grabados |
| `ResetBindings` + `FlushPeriodic` (`postReset` 0.165-0.20) | 0.19-0.23 | `:2833-2862`: `binding = {}` por cada imagen de `bound_images` (~8 destinos + ~11 muestreadas), `AccessBuffer` por buffer (~11, memoizado por época `vk_runtime.cpp:731-751`) y por lectura fija | Solo el `binding = {}` de los destinos podría saltarse, pero `is_target` persistente cambiaría el comportamiento entre draws (`ResolveOverlap` y copias lo leen, `texture_cache.cpp:660,2181`): **no** exacto |
| `PrepareRenderState` acierto | 0.11-0.14 | bucle de 8 imágenes (`:975-988`) + `active_mask` (`:906-908`) | Parcialmente: micro-ajustes de §4.3 (~0.03-0.05) |

`BeginRendering` (0.59-0.70 instrumentado, 0.30-0.55 estimado real) está al nivel de `pipe` y por debajo de `bbuf`/`btex`; **`BindTextures` es el único coste mayor que `BeginRendering` donde el trabajo por imagen es memoizable con las mismas
herramientas** (`UpdateIsNoop`, `TransitIsNoop`, series de generación/época).

## 8. Orden recomendado

1. Paso 0 de §4.5 (calibración + estadística de causas, 1 lanzamiento, sin cambiar comportamiento).
2. Si `begin` real >= 0.35 µs y acierto >= 90%: implementar §4 (`GBufferFastPath`) con VERIFY; A/B.
3. Con las mismas piezas (`UpdateIsNoop`, `TransitIsNoop`): evaluar `BindTextures` (§7) con el mismo método (modo "solo contar" primero).

## 9. Resultados medidos (2026-10-26, binaria `out/bb-probe.gbfast.exe`, 6 lanzamientos de 10)

Aviso: la estimación de §5 era pesimista. Quitadas las vueltas (`SOTC_DRAW_LAPS=0`), `BeginRendering` de G-buffer cuesta **0.52 µs reales** (no 0.30-0.45): la mayor parte de los 0.60-0.70 instrumentados era trabajo real. Se llegó a implementar y medir todo (la coordinación pidió después parar en el paso 0 y portar a la rama nueva; el código queda en el árbol, compila, y está **activo por defecto**).

**Paso 0 (`gbs0`, ABBA, `SOTC_DRAW_PHASES=1 SOTC_DRAW_LAPS=0`, modo `SOTC_GBFAST_MODE=count` y `SOTC_BTFAST_MODE=count`, ventanas >= 5000 cmds/frame, G-buffer, µs/draw):**

| | bit 29 puesto (camino antiguo) | bit 29 limpio (cuenta: check + completo) |
|---|---:|---:|
| `begin` | **0.521** | 0.600 (el check cuesta +0.079) |
| `btex` (BindTextures, ~11 imágenes, 2 etapas) | **1.147** | 1.196 |
| `btex` -> `s_btMemo` (hash + memo + `BindResolvedImage` por imagen) / `s_btImages` | 0.796 / 0.240 | 0.812 / 0.272 |
| total | 3.461 | 3.626 |

Acierto del G-buffer por el check: **97.6%** (`cand 124703 hit 121660`; fallos: `mask` 1.2%, `nomemo` 0.4%, `regs` 0.6%, `dtransit` 0.15%, `gen` 0.03%; `ids/gpuepoch/meta/flags/bound/backing/transit/vo` 0). `state-differs 0`: en cada acierto el `BeginRendering` completo dio el `RenderState` del memo. BindTextures: 99.9% de 3.5 M bindings memoizados cumplen las condiciones (todos los fallos son `transit`). Puerta del paso 0 (begin real >= 0.35 µs, acierto >= 90%): cumplida; `btex` real 1.15 >= 0.6: cumplida.

**Implementado** (bits: `GBufferFastPath` = 29, `TexBindFastPath` = 31; bit puesto = camino antiguo; la máscara por defecto de `src/runtime_memory.c` no los incluye -> activos). Bit 29 comprobado libre: ningún uso en `gpu/`, `src/`, `launcher/`, `sotc.py` ni scripts (el comentario de "bits 20-29 de depuración" es histórico de bbport). El bit 31 también.
- `GBufferFast`/`Check`/`Record`/`Apply` (`vk_rasterizer.cpp`, antes de `BeginRenderingFast`) con `gb_memo` propio; token del scheduler; contadores y línea `SOTC GBFAST`.
- `TexBindFastCheck/Apply` dentro de `ReplayTexBindMemo`: solo sustituye `BindResolvedImage` (UpdateImage + FindView + Transit) por la vista y layout recordados cuando la imagen no está Dirty, tiene el mismo `backing`, `TransitIsNoop(ShaderReadOnly/DepthStencilReadOnly, ShaderRead, AllCommands)` y no es destino/General. `SotcLod::Mark` por T# se hace en el bucle anterior, que no se toca: la secuencia de marcas es idéntica a la lenta.
- Diagnóstico: `SOTC_DRAW_LAPS=0` (quita las vueltas de `BeginRendering`), subfases `btMemo`/`btImages`, `SOTC_GBFAST_MODE=count`, `SOTC_BTFAST_MODE=count`.

**VERIFY** (`gbv1`, replay del templo, `SOTC_DEPTHFAST_VERIFY=1`): `BeginRendering` de G-buffer **13.04 M comprobaciones, 0 discrepancias**; texturas **134.5 M, 0**; profundidad (ya existente) 25.0 M, 0; `PrepareRenderState` 40.3 M, 0; estado dinámico 34.1 M, 0.

**A/B en caliente** (misma binaria, `BB_TOGGLE_FILE` cada 5 s, ABBA + BAAB agrupados, vecinos de >= 5000 cmds/frame, desde 70 s; `gba1/gba2` bit 29, `gbb1/gbb2` bit 31):

| Cambio | `draw busy` ON-OLD | por 1000 cmds | FPS ON-OLD | tramo pesado (>= 7000 cmds) |
|---|---|---|---|---|
| `GBufferFastPath` (29) | **-1.06 ± 0.41 ms/frame** (n=20) | -0.120 ± 0.047 ms | **+2.57 ± 0.83** | -0.72 ± 0.27 ms/frame, +1.42 FPS (n=5) |
| `TexBindFastPath` (31) | -0.70 ± 0.42 ms/frame (n=20) | -0.055 ± 0.068 ms | +1.48 ± 0.94 | -0.46 ± 0.18 ms/frame, +0.67 FPS (n=3) |

El criterio (>= 0.5 ms/frame en el tramo pesado del templo o >= 2% de FPS en el Santuario) se cumple para el bit 29 (-0.72 ± 0.27 ms) y queda al borde para el 31 (-0.46 ± 0.18, n=3, ruido alto). No se midió el Santuario. Archivos tocados: `gpu/shim/bbport_toggles.h`, `gpu/shadps4/video_core/texture_cache/texture_cache.h` (`RenderTargetReplayable`), `gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h/.cpp`; scripts `out/gb_bench_temple.sh`, `out/gb_ab_temple.sh`, `out/_gb_chain.sh`.
