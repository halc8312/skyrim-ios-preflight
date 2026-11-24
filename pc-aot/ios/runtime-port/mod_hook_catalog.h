/* SPDX-License-Identifier: GPL-3.0-only
 * Plan X5: describe a DLL's code patch request against the game image
 * (recorded by the DLL diagnostic, never applied). Only sites with evidence in
 * this repository are catalogued; everything else is reported as unsupported
 * with its address and the containing function.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SKYRIM_PATCH_OTHER, SKYRIM_PATCH_CALL, SKYRIM_PATCH_JUMP } SkyrimPatchKind;
typedef enum {
    SKYRIM_PATCH_UNSUPPORTED,        /* no iOS replacement */
    SKYRIM_PATCH_EXPERIMENT_ONLY,    /* a reviewed replacement exists only as an experiment */
    SKYRIM_PATCH_NEEDS_GUEST,        /* the replacement needs a guest built with its hook group */
    SKYRIM_PATCH_ADAPTER_AVAILABLE   /* this app and guest carry the replacement */
} SkyrimPatchStatus;

typedef struct {
    uint32_t site, length, target, function; /* function: 0 when unknown */
    SkyrimPatchKind kind;
    SkyrimPatchStatus status;
    int catalog;            /* row index, or -1 */
    uint32_t group;         /* PcGuestModHooksABI.h bit for the replacement, 0 when none */
    const char *adapter;    /* English name of the replacement, NULL when none */
} SkyrimPatchDescription;

/* bytes: the recorded request (length bytes). function_starts: the guest's
 * function table addresses in ascending order (may be NULL). */
void skyrim_mod_patch_describe(uint32_t site, uint32_t length, const uint8_t *bytes, uint32_t compiled_groups,
                               const uint32_t *function_starts, size_t function_count, SkyrimPatchDescription *out);
/* Greatest function start <= address, or 0. */
uint32_t skyrim_mod_patch_function(uint32_t address, const uint32_t *function_starts, size_t count);
/* One line for the DLL diagnostic screen, in English or Japanese. Returns the
 * snprintf length. */
int skyrim_mod_patch_text(const SkyrimPatchDescription *, int japanese, char *out, size_t capacity);
size_t skyrim_mod_patch_catalog_count(void);

#ifdef __cplusplus
}
#endif
