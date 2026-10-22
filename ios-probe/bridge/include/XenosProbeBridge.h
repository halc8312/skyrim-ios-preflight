#ifndef XENOS_PROBE_BRIDGE_H
#define XENOS_PROBE_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t probe_abi_version(void); /* 1 */
uint32_t probe_width(void);       /* 4 texels */
uint32_t probe_height(void);      /* 4 texels */
size_t probe_rgba_byte_len(void); /* 64 bytes */

/*
 * Decode a hand-authored BC1 fixture to 4 x 4 row-major RGBA8 texels.
 * Each row is red, blue, (170,0,85,255), (85,0,170,255).
 * No game data is used. This is a texture/FFI probe, not a game runtime.
 *
 * Returns: 64 success; -1 null output; -2 capacity < 64; -3 decoder panic.
 * Failure writes nothing. Success writes exactly 64 bytes and retains no pointer.
 * If non-null with capacity >= 64, out must refer to 64 writable bytes that
 * no other thread accesses during this call. Capacity is measured in bytes.
 */
int32_t probe_decode_rgba(uint8_t *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
