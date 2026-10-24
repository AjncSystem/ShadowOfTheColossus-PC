// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the FSR 3.1 upscaler's shader blob accessors. Its pipelines come from the fsr-vulkan bridge
// by name (the same stubs as third_party/fsr-vulkan/tests/fsr3_3_1_5_bridge_test.cpp), so the SDK's
// own blob tables are not linked.

#include <cstdint>
#include <cstring>

#include "ffx_fsr3upscaler.h"
#include "ffx_interface.h"

extern "C" FfxErrorCode fsr3UpscalerGetPermutationBlobByIndex(FfxFsr3UpscalerPass, uint32_t,
                                                              FfxShaderBlob* outBlob) {
    if (!outBlob) {
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_POINTER);
    }
    std::memset(outBlob, 0, sizeof(*outBlob));
    return FFX_OK;
}

extern "C" FfxErrorCode fsr3UpscalerIsWave64(uint32_t, bool& isWave64) {
    isWave64 = false;
    return FFX_OK;
}
