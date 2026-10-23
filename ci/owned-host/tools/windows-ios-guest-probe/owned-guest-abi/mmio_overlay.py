"""Reviewed public XMA hook, applied only after the existing atomic overlay.

Every replacement is anchored. Unrelated generated-header content is preserved;
the caller must recompile every translation unit with the explicit bridge flag.
"""
from audit_private_guest_symbols import require

MMIO_FLAG = '-DSKYRT_XMA_MMIO_BRIDGE=1'

HOOK = '''// Isolated default-OFF XMA MMIO candidate. Enabling this requires rebuilding
// every affected native object and binding the new callback symbols; the
// standalone iOS adapter has no such callback ABI yet.
#if defined(SKYRT_XMA_MMIO_BRIDGE) && SKYRT_XMA_MMIO_BRIDGE
extern "C" bool skyrt_xma_mmio_read_u32(uint32_t address, uint32_t* out_device_word);
extern "C" bool skyrt_xma_mmio_write_u32(uint32_t address, uint32_t device_word);

static constexpr bool skyrt_is_xma_mmio_u32(uint32_t ea)
{
    return (ea & 3u) == 0 && ea >= 0x7FEA0000u && ea <= 0x7FEAFFFCu;
}

static inline __attribute__((always_inline)) uint32_t skyrt_xma_load_u32(uint8_t* base, uint32_t ea)
{
    SKYRT_CHECK_FRAMES(ea, 4);
    uint32_t device_word;
    if (skyrt_is_xma_mmio_u32(ea) && skyrt_xma_mmio_read_u32(ea, &device_word))
        return __builtin_bswap32(device_word);
    return __builtin_bswap32(*(volatile uint32_t*)PPC_EA(ea));
}

static inline __attribute__((always_inline)) void skyrt_xma_store_u32(uint8_t* base, uint32_t ea, uint32_t value)
{
    SKYRT_CHECK_FRAMES(ea, 4);
    // Pass the actual instruction operand, even for repeated equal stores.
    // Callbacks receive the logical little-endian device register value.
    if (!(skyrt_is_xma_mmio_u32(ea) && skyrt_xma_mmio_write_u32(ea, __builtin_bswap32(value))))
        *(volatile uint32_t*)PPC_EA(ea) = __builtin_bswap32(value);
    PPC_MARK_WRITTEN(ea, 4);
}
#endif

'''

LOAD = '''#ifndef PPC_LOAD_U32
#define PPC_LOAD_U32(x) (SKYRT_CHECK_FRAMES(x, 4), __builtin_bswap32(*(volatile uint32_t*)PPC_EA(x)))
#endif'''

STORE = r'''#ifndef PPC_STORE_U32
#define PPC_STORE_U32(x, y) \
    do { uint32_t skyrt_store_ea = (uint32_t)(x); SKYRT_CHECK_FRAMES(skyrt_store_ea, 4); \
         *(volatile uint32_t*)PPC_EA(skyrt_store_ea) = __builtin_bswap32(y); \
         PPC_MARK_WRITTEN(skyrt_store_ea, 4); } while (0)
#endif'''


def apply_mmio_overlay(text):
    require('SKYRT_XMA_MMIO_BRIDGE' not in text, 'mmio_overlay_already_present')
    changes = [
        ('#ifndef PPC_LOAD_U8\n', HOOK + '#ifndef PPC_LOAD_U8\n'),
        (LOAD, '#ifndef PPC_LOAD_U32\n#if defined(SKYRT_XMA_MMIO_BRIDGE) && SKYRT_XMA_MMIO_BRIDGE\n'
         '#define PPC_LOAD_U32(x) skyrt_xma_load_u32(base, (uint32_t)(x))\n#else\n'
         + LOAD.split('\n', 1)[1] + '\n#endif'),
        (STORE, '#ifndef PPC_STORE_U32\n#if defined(SKYRT_XMA_MMIO_BRIDGE) && SKYRT_XMA_MMIO_BRIDGE\n'
         '#define PPC_STORE_U32(x, y) skyrt_xma_store_u32(base, (uint32_t)(x), (uint32_t)(y))\n#else\n'
         + STORE.split('\n', 1)[1] + '\n#endif'),
    ]
    for old, new in changes:
        require(text.count(old) == 1, 'mmio_overlay_anchor_mismatch')
        text = text.replace(old, new)
    return text
