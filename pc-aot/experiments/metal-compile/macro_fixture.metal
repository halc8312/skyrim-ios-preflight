// Authored negative fixture: Windows Metal 6.2 rejects host_name strings that
// contain the negative/fractional tokens below. -E stringification succeeds,
// but this is not a usable complete runtime token-capture method.
// No game input, GPU dispatch, runtime token result or value-equivalence proof.
#if !defined(U64_MAX) || !defined(U64_HIGH) || !defined(I64_MIN) || !defined(U32_MAX) || !defined(F32_NEG_ZERO) || !defined(F32_NEXT) || !defined(BOOL_VALUE) || !defined(TOKEN_STRING)
#error Missing authored macro input
#endif

#define TOKEN_STRINGIFY_INNER(value) #value
#define TOKEN_STRINGIFY(value) TOKEN_STRINGIFY_INNER(value)

[[host_name("macro_U64_MAX_" TOKEN_STRINGIFY(U64_MAX))]]
kernel void observe_u64_max() {}
[[host_name("macro_U64_HIGH_" TOKEN_STRINGIFY(U64_HIGH))]]
kernel void observe_u64_high() {}
[[host_name("macro_I64_MIN_" TOKEN_STRINGIFY(I64_MIN))]]
kernel void observe_i64_min() {}
[[host_name("macro_U32_MAX_" TOKEN_STRINGIFY(U32_MAX))]]
kernel void observe_u32_max() {}
[[host_name("macro_F32_NEG_ZERO_" TOKEN_STRINGIFY(F32_NEG_ZERO))]]
kernel void observe_f32_negative_zero() {}
[[host_name("macro_F32_NEXT_" TOKEN_STRINGIFY(F32_NEXT))]]
kernel void observe_f32_next() {}
[[host_name("macro_BOOL_VALUE_" TOKEN_STRINGIFY(BOOL_VALUE))]]
kernel void observe_bool() {}
[[host_name("macro_TOKEN_STRING_" TOKEN_STRINGIFY(TOKEN_STRING))]]
kernel void observe_string_token() {}
