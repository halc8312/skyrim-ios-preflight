/* Apple SDK host helpers. No title data or generated guest code. */
#ifndef OH_NATIVE_HOST_H
#define OH_NATIVE_HOST_H
#include <stddef.h>
#include <stdint.h>
#include "PrivateGuestABI.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Exactly 49 words: size, alignment, offsets for requirements/memory/host,
 * in declaration order. Failure leaves the output untouched. */
int32_t OHNativeHostLayoutV1(uint64_t *out, size_t capacity);
uint32_t OHNativePlatformV1(void); /* 2=device, 7=Simulator, 0=unsupported */
uint32_t OHNativeOSVersionV1(void); /* major << 16 | minor << 8 | patch */
int32_t OHNativeBundlePathV1(char *out, size_t capacity);
/* Copies NSHomeDirectory including its terminator; returns strlen or -1. */
int32_t OHNativeSandboxPathV1(char *out, size_t capacity);
/* SDK exposes RTLD_FIRST; Rust validates canonical path and held file hash. */
void *OHNativeOpenGuestV1(const char *canonical_path);
/* Output objects/buffer are writable, exclusive and mutually disjoint. The
 * caller retains loaded images against unloading during this lookup. */
int32_t OHNativeLocateImageV1(const void *symbol, uintptr_t *header,
                            int64_t *slide, char *path, size_t capacity);
/* Caller owns a correctly aligned/exclusive host object. Only native-provider
 * fields are replaced, and only on complete success. Existing kernel/timebase/
 * memory/context contracts remain the caller's responsibility. Native pointers
 * are direct provider entries, never ordinary setjmp/longjmp wrappers. */
int32_t OHNativePrepareHostV1(PGHostCallbacksV1 *host, uint32_t bytes,
                           const void *guest_header);
#ifdef __cplusplus
}
#endif
#endif
