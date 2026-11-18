// Authored diagnostic experiment. Compilation MUST fail at these assertions.
// The stringification output is not a proof of expression type or value.
#if !defined(U64_MAX) || !defined(U64_HIGH) || !defined(I64_MIN) || !defined(U32_MAX) || !defined(F32_NEG_ZERO) || !defined(F32_NEXT) || !defined(BOOL_VALUE) || !defined(TOKEN_STRING)
#error Missing authored macro input
#endif

#define TOKEN_STRINGIFY_INNER(value) #value
#define TOKEN_STRINGIFY(value) TOKEN_STRINGIFY_INNER(value)

static_assert(false, "TOKEN_BEGIN_U64_MAX__" TOKEN_STRINGIFY(U64_MAX) "__TOKEN_END");
static_assert(false, "TOKEN_BEGIN_U64_HIGH__" TOKEN_STRINGIFY(U64_HIGH) "__TOKEN_END");
static_assert(false, "TOKEN_BEGIN_I64_MIN__" TOKEN_STRINGIFY(I64_MIN) "__TOKEN_END");
static_assert(false, "TOKEN_BEGIN_U32_MAX__" TOKEN_STRINGIFY(U32_MAX) "__TOKEN_END");
static_assert(false, "TOKEN_BEGIN_F32_NEG_ZERO__" TOKEN_STRINGIFY(F32_NEG_ZERO) "__TOKEN_END");
static_assert(false, "TOKEN_BEGIN_F32_NEXT__" TOKEN_STRINGIFY(F32_NEXT) "__TOKEN_END");
static_assert(false, "TOKEN_BEGIN_BOOL_VALUE__" TOKEN_STRINGIFY(BOOL_VALUE) "__TOKEN_END");
static_assert(false, "TOKEN_BEGIN_TOKEN_STRING__" TOKEN_STRINGIFY(TOKEN_STRING) "__TOKEN_END");
