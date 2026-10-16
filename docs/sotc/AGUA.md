# SotC water in the port: what was wrong and how it was found

Record of the water fixes (October 2026), so the investigation does not need to be repeated.

## Symptoms

- The water was smooth like a mirror, with no waves. It only rippled very close to Wander.
- The lake near the waterfall reflected only nearby objects. Farther away, the surface was dark and smooth.
- The open-world lakes and rivers below the waterfalls were completely still.

## Two types of water

1. **Simulated water** (lake near the waterfall, shader `fs 8356` in the lake capture).
   The CPU updates a 512×512 simulation texture around Wander, and the vertex shader
   derives the normal from it. Outside this square, waves come from normal maps.
2. **Flow-map water** (open-world lakes and rivers, `fs 0xc65f86f1`). It has no
   simulation: waves are BC5 normal maps offset by a flow map and a clock
   (`srt[69]`, time × speed `srt[184]`, with two `fract` phases).

## Fix 1: DMA enabled by default (`294b68e`)

The screen-space reflection shader (compute) reads a 224-byte *probe* record for each ray,
using a runtime-computed index. The step that "flattens" shader memory reads cannot resolve
this index. Without DMA, the read fell back to `srt_flatbuf.data[0]`, so every pixel used the
same record.

Pink-shadPS4 enables `direct_memory_access_enabled` in `CUSA08809.json`. With DMA, these reads
become direct accesses to game memory (BDA page table). This is now the port default
(`BB_DIRECT_MEMORY_ACCESS`, in `gpu/shim/core/emulator_settings.h`). Only about 4 shaders start
using DMA, and the measured cost is negligible.

## Fix 2: `S_WQM_B64` (`debb0c4`)

At the start of the open-world water shader:

```
s_mov_b32  vcc_hi, 0x384        ; the game uses vcc_hi as a general-purpose register
...
s_wqm_b64  vcc, exec            ; VCC = active-thread mask ("whole quad" mode)
...
v_cmp_nle_f32 s[12:13], v9, s87 ; s87 = 0.05
s_and_saveexec_b64 s[88:89], vcc
```

The recompiler treated `S_WQM_B64` as a "no-op," which is only safe for
`s_wqm_b64 exec, exec`. Here the destination is VCC: the lower half of VCC was undefined and the
upper half was 0x384 (900). On an NVIDIA GPU (32-wide subgroups), only the undefined half matters,
so the branch that applies the flow map never ran. The shader always wrote the same straight-up
normal (`0x16ffb801` in the G-buffer).

Fix in `shader_recompiler/frontend/translate/scalar_alu.cpp`: when the destination is not
EXEC, `S_WQM_B64` writes the mask expanded to whole quads (a 2×2 quad with any bit set gets all
four bits set) and sets SCC. Pink has the same issue.

- The first version only copied the mask. The water looked correct, but distant treetops gained
  "colored garbage." Foliage uses `s_wqm_b64` in the transparency test to keep neighboring quad
  lanes active, which the GPU needs to calculate the texture level of detail. Without expansion,
  it read garbage mips.
- Expansion is performed per thread (each pixel gathers the 4 bits of its quad with
  `QuadBroadcast` and then `Ballot`), not with shifts on the 64-bit mask. The `InverseBallot`
  elimination pass treats masks as one boolean per thread and aborts on a constant such as
  `0x1111111111111111`.
- Only 9 game pixel shaders use this form (`s_wqm_b64 vcc, exec` or `vcc, vcc`).
  `SOTC_WQM_COPY=0` restores the old behavior for comparison.

## Method (scripts in `tools/rdoc`; run with `qrenderdoc.exe --python <script>` and the `RD_*` variables)

1. **Capture in RenderDoc:** `SOTC_WRAP` (JSON with `renderdoccmd capture`) +
   `SOTC_RDOC_SUBMITS=8` + `SOTC_RECORD_THREAD=0`, then press F12 in the game. Opening the capture
   requires the game to be closed because there is not enough VRAM left. The first frame has
   garbage indirect arguments; use the following frames.
2. **Find the draw:** list draws by shader and textures (`rd_findps.py`). The G-buffer pixel
   history (`rd_hist.py`) shows which draw wrote each pixel.
3. **Check the inputs:** views and mips (`rd_views.py`, `rd_mipsminmax.py`), constants
   (`rd_flatdump.py`), vertex buffers (`rd_vsbufs.py`), and outputs (`rd_raw.py`, which saves the
   G-buffer bytes for decoding in Python).
4. **Instrument the shader:** extract SPIR-V (`rd_spv.py`), disassemble with `spirv-dis`, replace
   the value sent to `frag_color0` with intermediate values (`spv_probe.py`), assemble with
   `spirv-as`, replace it in the capture (`BuildTargetShader` + `ReplaceResource`), and read the pixel
   (`rd_probe.py`). This revealed the straight-up normal and the branch with `InverseBallot` on
   `(Undef, 900)`.
5. **Inspect the GCN code:** `BB_DUMP_SHADERS=1` saves the IR and binary for each compiled shader
   in `user/shader/dumps`. `tools/rdoc/gcnscan.py` is a minimal GCN decoder that marks
   instructions that touch VCC.
