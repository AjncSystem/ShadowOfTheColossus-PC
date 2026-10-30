/* sotc: C interface of the CPU patches (gpu/sotc/sotc_patches.cpp), called by src/probe.c. */
#ifndef SOTC_PATCHES_H
#define SOTC_PATCHES_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint64_t offset, size; } SotcCodeSegment; /* image offsets */
typedef struct {
    void *image;              /* the mapped eboot; offsets below are relative to it */
    uint64_t module_size;     /* eboot part of the image */
    void *trampoline;         /* within +-2 GiB of the image, executable */
    uint64_t trampoline_size;
    uint64_t eh_frame_hdr, eh_frame_hdr_size; /* PT_GNU_EH_FRAME: function starts */
    const SotcCodeSegment *segments;
    uint32_t segment_count;
    const uint64_t *red_zone_functions; /* function offsets to protect */
    uint32_t red_zone_count;
    void *tsc_clock;          /* non-null: rdtsc reads this host function (cores disagree on the TSC) */
    /* sotc: out/hooks.bin (SOTCHOOK1, scripts/sotc_hooks.py) or NULL: engine hooks
     * (docs/HOOK-DESIGN.md, gpu/sotc/hooks.h), installed after every other CPU patch. */
    const char *hooks_path;
} SotcPatchRequest;
typedef struct {
    int needs_sse4a;
    uint64_t function_starts, short_sse4a, red_zone_functions, red_zone_instructions;
    uint64_t rdtsc, rdtsc_unpatched;
    uint64_t hooks_installed, hooks_off; /* sotc: engine hooks (see the SOTC HOOK manifest) */
    uint64_t trampoline_used;            /* sotc: trampoline bytes used by all CPU patches */
} SotcPatchResult;
int sotc_patch_eboot(const SotcPatchRequest *request, SotcPatchResult *result);
int sotc_handle_illegal(void *exception_pointers);
#ifdef __cplusplus
}
#endif
#endif
