#ifndef OWNED_HOST_GRAPHICS_PRESENTATION_PROBE_H
#define OWNED_HOST_GRAPHICS_PRESENTATION_PROBE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
// Public simulator test only; not part of OwnedHost.h/C96 or PrivateGuestABI.
typedef struct OHGraphicsPresentationProbeV1 {
    uint32_t version;
    uint32_t bytes;
    uint32_t backend;
    uint32_t reserved;
    uint64_t checks;
    uint64_t rendered_cases;
} OHGraphicsPresentationProbeV1;
// Complete/aligned/writable result, exact sizeof(struct), no output on failure.
// Native driver calls cannot be interrupted here: test enforces process timeout.
int32_t OHRunPublicGraphicsProbeV1(OHGraphicsPresentationProbeV1 *out, size_t bytes);
#ifdef __cplusplus
}
static_assert(sizeof(OHGraphicsPresentationProbeV1)==32);
#else
_Static_assert(sizeof(OHGraphicsPresentationProbeV1)==32,"public probe layout");
#endif
#endif
