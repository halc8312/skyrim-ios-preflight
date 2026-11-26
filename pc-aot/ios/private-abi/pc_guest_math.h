/* Force-included before every private guest translation unit by scripts/build_title_guest.py.
 *
 * The guest is compiled with -fno-builtin, which also turns the C math functions into calls into libSystem. The
 * translated x87/SSE code reaches sqrt, fabs, floor, ceil, trunc and nearbyint (and their float forms) for fsqrt,
 * fabs, frndint, fist, cvt*2si and sqrtss/sqrtsd, so each of those was an external call. These names map to the
 * compiler's own implementations, which are the single AArch64 instructions fsqrt, fabs, frintm, frintp, frintz and
 * frinti. They are exactly rounded IEEE operations (nearbyint honours the current rounding mode like the library
 * does), so results are bit-identical. Every other function keeps its libSystem call and -fno-builtin is untouched.
 *
 * <math.h> is included first: its guard keeps the later include inside x86.h from redefining anything, and the
 * macros are function-like so declarations are not rewritten.
 */
#pragma once
#include <math.h>

#define sqrt(x) __builtin_sqrt(x)
#define sqrtf(x) __builtin_sqrtf(x)
#define fabs(x) __builtin_fabs(x)
#define fabsf(x) __builtin_fabsf(x)
#define floor(x) __builtin_floor(x)
#define floorf(x) __builtin_floorf(x)
#define ceil(x) __builtin_ceil(x)
#define ceilf(x) __builtin_ceilf(x)
#define trunc(x) __builtin_trunc(x)
#define truncf(x) __builtin_truncf(x)
#define nearbyint(x) __builtin_nearbyint(x)
#define nearbyintf(x) __builtin_nearbyintf(x)
