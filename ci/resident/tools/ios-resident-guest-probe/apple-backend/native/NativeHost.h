/* Public synthetic Apple host helpers. No title data or guest code. */
#ifndef RA_APPLE_NATIVE_HOST_H
#define RA_APPLE_NATIVE_HOST_H
#include <stddef.h>
#include <stdint.h>
#include "PrivateGuestABI.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Exactly 49 words: size, alignment, offsets for requirements/memory/host,
 * in declaration order. Failure leaves the output untouched. */
int32_t RAAppleHostLayoutV1(uint64_t *out, size_t capacity);
uint32_t RAApplePlatformV1(void); /* 2=device, 7=Simulator, 0=unsupported */
uint32_t RAAppleOSVersionV1(void); /* major << 16 | minor << 8 | patch */
int32_t RAAppleBundlePathV1(char *out, size_t capacity);
/* SDK exposes RTLD_FIRST; Rust validates canonical path and held file hash. */
void *RAAppleOpenFixtureV1(const char *canonical_path);
/* Output objects/buffer are writable, exclusive and mutually disjoint. The
 * caller retains loaded images against unloading during this lookup. */
int32_t RAAppleLocateImageV1(const void *symbol, uintptr_t *header,
                            int64_t *slide, char *path, size_t capacity);
/* Caller owns a correctly aligned/exclusive host object. Only native-provider
 * fields are replaced, and only on complete success. Existing kernel/timebase/
 * memory/context contracts remain the caller's responsibility. Native pointers
 * are direct provider entries, never ordinary setjmp/longjmp wrappers. */
int32_t RAApplePrepareHostV1(PGHostCallbacksV1 *host, uint32_t bytes,
                           const void *fixture_header);
#ifdef __cplusplus
}
#endif
#endif
