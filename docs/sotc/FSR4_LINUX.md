# Native FSR4 on Linux (experimental)

The Linux implementation uses the vendored FSR-Vulkan source-v07 INT8/DOT4 provider.
It does not load AMD's Windows DLLs and is not FSR 4.1/4.1.1. Windows keeps the SDK
loader and its existing preset list.

In the launcher, select **AMD FSR 4 (native Vulkan, experimental)**. Turn **PS4 Pro
off** to choose a render preset:

| Preset | Render size at 2560x1440 output | v07 model |
|---|---|---|
| Quality | 1664x936 | quality |
| Performance | 1280x720 | performance |
| Native AA | 2560x1440 | native |

Native AA applies temporal reconstruction at 1:1; it does not reduce the render
resolution. Frame generation is independent: Off, or FSR3 for Linux. DLSS-G requires
Windows. The shared saved setting `dlss_mode=dlaa` represents Native AA for FSR4.
Balanced and Ultra Performance are hidden for Linux FSR4 because the game's rounded
render sizes can select other models; old saved values normalize to Quality.
The size preview shows the requested render size and the model selected by its ratio.
At 720p Quality, for example, the rounded 896x504 input selects the native network.

With PS4 Pro on, the game chooses its render size and the preset control is disabled.
The backend requires shaderInt8, shaderFloat16, 16-bit storage, integer dot products
and VK_KHR_compute_shader_derivatives with computeDerivativeGroupLinear enabled;
the RX 9070 XT/RADV is the tested GPU. Other GPUs have not been validated.
Missing assets, unsupported features or context errors fall back to FSR1. Outputs
above **3,686,400 pixels** also fall back: larger outputs previously hung the GPU in
game. `SOTC_FSR4_MAX_PIXELS=0` removes that guard for developer experiments only.

## Build and package

From a checkout with the submodules initialized and the usual Linux build dependencies:

```sh
bash tools/fetch_fsr4_assets.sh
bash build.sh
bash packaging/linux/build_appimage.sh
```

The existing fetch script pins the asset source to FireBurn/Q2RTX commit
`ae8d628fae208813172446d1e49ed94150b04658`. These are the source-v07 assets, not models
extracted from the proprietary FSR4.1 Windows DLL. The bundle includes its AMD MIT
license notice. No game or console firmware files are added.

The packager requires the complete 1080/2160 asset tiers and copies `fsr4_shaders/`
beside `bin/` inside the AppImage. The backend searches `SOTC_FSR4_ASSETS`, beside the
executable, one level above it (the AppImage layout), then
`~/.local/share/sotc-pc/fsr4_shaders`. No environment override is needed for a package.

`build.sh` applies patches 0003/0004 to the existing FSR-Vulkan submodule: imported
depth-image barriers use their declared aspect, and external-image records can be
evicted when the renderer cycles image views. The submodule pointer is unchanged.
The source-v07 provider and its caller are excluded from LTO so their FidelityFX ABI
cannot merge with FSR3 frame generation's different SDK structures.

Successful logs include both:

```text
SOTC FSR: v07 asset bundle .../bin/../fsr4_shaders
SOTC FSR: FSR 4 v07 upscaling 1664x936 -> 2560x1440 (quality preset, 2160 tier, depth inverted)
```

## Related work

This ports the locally tested Linux v0.74 implementation onto the Linux v0.75 release
(`eccb8d9`). The Linux port originates with FlaipyTheHost's upstream PR #39 and is
maintained by Gabriel-TwoBits. The provider and shader bundle come from
[FireBurn/FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and
[FireBurn/Q2RTX](https://github.com/FireBurn/Q2RTX), with AMD's source-v07 model.

[mrgroeff's upstream PR #409](https://github.com/AjncSystem/ShadowOfTheColossus-PC/pull/409)
fixed the separate heap-backing read crash and is already incorporated in v0.75.
This FSR4 patch does not duplicate that fix or carry unrelated performance experiments.

## Shader variants and existing caches

Each translated shader variant keeps the resource metadata that generated its SPIR-V.
The pipeline layout, draw bindings and saved metadata use that same variant, including
when the cache is reloaded. FMask constants remain specialization dependencies: changing
a bound surface from FMask to an ordinary texture must select a different variant.

The cache metadata generation is 23 and uses its own cache folder (`*_meta23*`), so
old incompatible shader/metadata pairs are not preloaded. The first visit recompiles
shaders; later starts use the rebuilt cache. Shader compilation switches, including
`SOTC_WAVE64_ABSENT_LANES`, have separate cache keys.

No wave64 override is required for the tested presets. Remove
`SOTC_WAVE64_ABSENT_LANES=0` if it was added solely for the earlier glow investigation;
that was not established as a general fix. The existing GCN absent-lane repair is retained.
