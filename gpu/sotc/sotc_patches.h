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
    uint64_t module_size;     /* patched part of the image (eboot and linked modules) */
    void *trampoline;         /* within +-2 GiB of the image, executable */
    uint64_t trampoline_size;
    uint64_t eh_frame_hdr, eh_frame_hdr_size; /* PT_GNU_EH_FRAME: function starts */
    const SotcCodeSegment *segments;
    uint32_t segment_count;
    const uint64_t *red_zone_functions; /* function offsets to protect */
    uint32_t red_zone_count;
    const SotcCodeSegment *module_tables; /* linked modules' PT_GNU_EH_FRAME (offset, size) */
    uint32_t module_table_count;
    uint64_t eboot_size;      /* segments past it (modules) are only patched for BMI emulation */
    void *tsc_clock;          /* non-null: rdtsc reads this host function (cores disagree on the TSC) */
} SotcPatchRequest;
typedef struct {
    int needs_sse4a, needs_bmi;
    uint64_t function_starts, short_sse4a, red_zone_functions, red_zone_instructions;
    uint64_t rdtsc, rdtsc_unpatched;
} SotcPatchResult;
int sotc_patch_eboot(const SotcPatchRequest *request, SotcPatchResult *result);
int sotc_handle_illegal(void *exception_pointers);
#ifdef __cplusplus
}
#endif
#endif
