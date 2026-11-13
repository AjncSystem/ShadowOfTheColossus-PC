# Local changes to gpu/third_party/fsr-vulkan

The submodule points at upstream FireBurn/FSR-Vulkan (`c64f093`). `build.sh` applies the
patches here to its working tree when they are not applied yet:

- `0001-...`: `BB_FSR4_PROFILE` (GPU time per FSR 4 pass) and `BB_FSR4_STATS` (driver
  statistics of each pass) in the FSR 4 v07 provider.
- `0002-...`: `#include <mutex>` in the FFX Vulkan backend; libc++ (the Windows MSYS2 CLANG64
  build) does not include it transitively.
- `0003-...`: the FSR 4 v07 backend transitions imported images on the aspects their host declares
  (`FfxFsr4VkExternalImageState::aspectMask`) instead of always on colour, which is invalid for
  the depth import and leaves that aspect in a layout the descriptor set does not describe.
- `0004-...`: the FSR 4 v07 backend's external image state table holds 64 records and evicts the
  least recently registered one when full, instead of holding 8 and failing every registration
  from the ninth on for the rest of the session. A renderer ping-pongs depth and motion vectors
  across its frame ring and recreates them on resize, so 8 was reached within the first frames.
