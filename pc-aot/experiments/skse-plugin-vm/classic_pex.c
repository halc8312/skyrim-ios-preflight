/* SPDX-License-Identifier: GPL-3.0-only */
#include "classic_pex.h"
#include <stdlib.h>
#include <string.h>

_Static_assert((unsigned)SKSE_PEX_MAX_ARGS == (unsigned)SKSE_CLASSIC_CATALOG_MAX_ARGS,
               "PEX and Classic registry arity");

typedef struct {
    const uint8_t *next;
    size_t left;
    SksePexStatus status;
    SksePexText *strings;
    uint16_t string_count;
    SksePexInventory inventory;
} Pex;

static void refuse(Pex *pex, SksePexStatus status) {
    if (pex->status == SKSE_PEX_OK) pex->status = status;
}

static const uint8_t *take(Pex *pex, size_t bytes) {
    if (pex->status != SKSE_PEX_OK) return NULL;
    if (bytes > pex->left) {
        refuse(pex, SKSE_PEX_MALFORMED);
        return NULL;
    }
    const uint8_t *result = pex->next;
    pex->next += bytes;
    pex->left -= bytes;
    return result;
}

static uint8_t u8(Pex *pex) {
    const uint8_t *p = take(pex, 1);
    return p ? p[0] : 0;
}

static uint16_t u16(Pex *pex) {
    const uint8_t *p = take(pex, 2);
    return p ? (uint16_t)((unsigned)p[0] << 8 | p[1]) : 0;
}

static uint32_t u32(Pex *pex) {
    const uint8_t *p = take(pex, 4);
    return p ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3] : 0;
}

static SksePexText string(Pex *pex) {
    const uint16_t length = u16(pex);
    return (SksePexText){take(pex, length), length};
}

static SksePexText reference(Pex *pex) {
    const uint16_t index = u16(pex);
    if (pex->status == SKSE_PEX_OK && index < pex->string_count) return pex->strings[index];
    refuse(pex, SKSE_PEX_MALFORMED);
    return (SksePexText){0};
}

static uint8_t fold(uint8_t value) {
    return value >= 'A' && value <= 'Z' ? (uint8_t)(value + ('a' - 'A')) : value;
}

static bool equal_text(SksePexText a, SksePexText b) {
    if (a.length != b.length || (a.length && (!a.bytes || !b.bytes))) return false;
    for (size_t i = 0; i < a.length; ++i)
        if (fold(a.bytes[i]) != fold(b.bytes[i])) return false;
    return true;
}

bool skse_pex_text_equal(SksePexText text, const char *ascii) {
    if (!ascii || strlen(ascii) != text.length || (text.length && !text.bytes)) return false;
    return equal_text(text, (SksePexText){(const uint8_t *)ascii, text.length});
}

static bool nonempty_text(SksePexText text) {
    return text.length && text.bytes && !memchr(text.bytes, 0, text.length);
}

static uint32_t value(Pex *pex, unsigned type) {
    switch (type) {
    case 0: return 0;                              /* None */
    case 1: case 2: reference(pex); return 0;     /* identifier or string */
    case 3: case 4: return u32(pex);              /* integer or float */
    case 5: return u8(pex);                       /* boolean */
    default: refuse(pex, SKSE_PEX_MALFORMED); return 0;
    }
}

static void code(Pex *pex, uint16_t count) {
    /* The Skyrim opcode set ends with ARRAY_RFIND. These are operand counts
     * for structural traversal only. Branches and calls never execute. */
    static const uint8_t fixed[] = {
        0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3,
        1, 2, 2, 3, 2, 3, 1, 3, 3, 3, 2, 2, 3, 3, 4, 4
    };
    for (unsigned i = 0; i < count && pex->status == SKSE_PEX_OK; ++i) {
        const uint8_t opcode = u8(pex);
        if (opcode >= sizeof fixed) {
            refuse(pex, SKSE_PEX_UNSUPPORTED);
            return;
        }
        SksePexStaticCall site = {0};
        const bool capture = opcode == 25 && pex->inventory.calls;
        for (unsigned j = 0; j < fixed[opcode]; ++j) {
            const uint8_t type = u8(pex);
            if (capture && j < 2) {
                if (type != 1) {
                    refuse(pex, SKSE_PEX_MALFORMED);
                    return;
                }
                if (!j) site.script = reference(pex);
                else site.name = reference(pex);
            } else {
                value(pex, type);
            }
        }
        if (opcode < 23 || opcode > 25) continue;   /* callmethod, callparent, callstatic */
        if (u8(pex) != 3) {
            refuse(pex, SKSE_PEX_MALFORMED);
            return;
        }
        const uint32_t arguments = u32(pex);
        if (arguments > UINT16_MAX || arguments > pex->left) {
            refuse(pex, SKSE_PEX_MALFORMED);
            return;
        }
        for (uint32_t j = 0; j < arguments && pex->status == SKSE_PEX_OK; ++j) value(pex, u8(pex));
        if (capture && pex->status == SKSE_PEX_OK) {
            if (!nonempty_text(site.script) || !nonempty_text(site.name)) {
                refuse(pex, SKSE_PEX_MALFORMED);
                return;
            }
            if (pex->inventory.call_count == pex->inventory.call_capacity) {
                refuse(pex, SKSE_PEX_LIMIT);
                return;
            }
            site.count = (uint16_t)arguments;
            pex->inventory.calls[pex->inventory.call_count++] = site;
        }
    }
}

static void function(Pex *pex, SksePexNative row) {
    row.result = reference(pex);
    reference(pex);                               /* documentation */
    take(pex, 4);                                 /* user flags */
    const uint8_t flags = u8(pex);
    const bool native = (flags & 2) != 0;
    if (flags & ~3u) refuse(pex, SKSE_PEX_UNSUPPORTED);
    const uint16_t params = u16(pex);
    if (native && pex->inventory.natives && params > SKSE_PEX_MAX_ARGS) refuse(pex, SKSE_PEX_LIMIT);
    for (unsigned i = 0; i < params && pex->status == SKSE_PEX_OK; ++i) {
        reference(pex);                           /* parameter name */
        const SksePexText type = reference(pex);
        if (native && pex->inventory.natives) row.arguments[i] = type;
    }
    const uint16_t locals = u16(pex);
    if (native && locals) refuse(pex, SKSE_PEX_MALFORMED);
    for (unsigned i = 0; i < locals && pex->status == SKSE_PEX_OK; ++i) {
        reference(pex);
        reference(pex);
    }
    const uint16_t instructions = u16(pex);
    if (!native) {
        code(pex, instructions);
        return;
    }
    if (instructions || !nonempty_text(row.name) || !nonempty_text(row.result) ||
        (row.state.length && !nonempty_text(row.state))) refuse(pex, SKSE_PEX_MALFORMED);
    SksePexInventory *inventory = &pex->inventory;
    if (!inventory->natives) return;
    if (inventory->native_count == inventory->native_capacity) refuse(pex, SKSE_PEX_LIMIT);
    if (pex->status != SKSE_PEX_OK) return;
    for (unsigned i = 0; i < params; ++i)
        if (!nonempty_text(row.arguments[i])) refuse(pex, SKSE_PEX_MALFORMED);
    for (size_t i = 0; i < inventory->native_count; ++i)
        if (equal_text(row.name, inventory->natives[i].name) && equal_text(row.state, inventory->natives[i].state))
            refuse(pex, SKSE_PEX_MALFORMED);
    if (pex->status != SKSE_PEX_OK) return;
    row.count = (uint8_t)params;
    row.global = (flags & 1) != 0;
    inventory->natives[inventory->native_count++] = row;
}

static void object(Pex *pex) {
    const SksePexText class = reference(pex);
    const uint32_t size = u32(pex);
    /* Bethesda's compiler counts the length word. Caprica counts the payload. */
    if (!nonempty_text(class) || ((size_t)size != pex->left && (size_t)size != pex->left + 4))
        refuse(pex, SKSE_PEX_MALFORMED);
    reference(pex);                               /* parent */
    reference(pex);                               /* documentation */
    take(pex, 4);                                 /* user flags */
    reference(pex);                               /* default state */
    const uint16_t variables = u16(pex);
    for (unsigned i = 0; i < variables && pex->status == SKSE_PEX_OK; ++i) {
        reference(pex);
        reference(pex);
        take(pex, 4);
        value(pex, u8(pex));
    }
    const uint16_t properties = u16(pex);
    for (unsigned i = 0; i < properties && pex->status == SKSE_PEX_OK; ++i) {
        const SksePexText name = reference(pex);
        reference(pex);                           /* type */
        reference(pex);
        take(pex, 4);
        const uint8_t flags = u8(pex);
        if (flags & ~7u) refuse(pex, SKSE_PEX_UNSUPPORTED);
        if (flags & 4) {
            reference(pex);
        } else {
            /* Accessor names alone cannot identify a registry entry. Validate
             * their bodies without exposing them in the native inventory. */
            const size_t before = pex->inventory.native_count;
            const SksePexNative accessor = {.script = class, .name = name};
            if (flags & 1) function(pex, accessor);
            if (flags & 2) function(pex, accessor);
            if (before != pex->inventory.native_count) refuse(pex, SKSE_PEX_UNSUPPORTED);
        }
    }
    const uint16_t states = u16(pex);
    for (unsigned i = 0; i < states && pex->status == SKSE_PEX_OK; ++i) {
        const SksePexText state = reference(pex);
        const uint16_t functions = u16(pex);
        for (unsigned j = 0; j < functions && pex->status == SKSE_PEX_OK; ++j) {
            const SksePexText name = reference(pex);
            function(pex, (SksePexNative){.script = class, .state = state, .name = name});
        }
    }
}

static void debug_info(Pex *pex) {
    const uint8_t flag = u8(pex);
    if (flag > 1) refuse(pex, SKSE_PEX_MALFORMED);
    if (!flag) return;
    take(pex, 8);
    const uint16_t count = u16(pex);
    for (unsigned i = 0; i < count && pex->status == SKSE_PEX_OK; ++i) {
        reference(pex);                           /* object */
        reference(pex);                           /* state */
        reference(pex);                           /* function */
        u8(pex);                                  /* function or property accessor */
        const uint16_t lines = u16(pex);
        take(pex, (size_t)lines * 2);
    }
}

SksePexStatus skse_pex_inspect(const uint8_t *bytes, size_t length, SksePexInventory *inventory) {
    if (!inventory) return SKSE_PEX_MALFORMED;
    inventory->native_count = 0;
    inventory->call_count = 0;
    if (!bytes || (inventory->native_capacity && !inventory->natives) ||
        (inventory->call_capacity && !inventory->calls) || length < 16) return SKSE_PEX_MALFORMED;
    if (length > SKSE_PEX_MAX_BYTES || inventory->native_capacity > SKSE_PEX_MAX_ROWS ||
        inventory->call_capacity > SKSE_PEX_MAX_CALLS) return SKSE_PEX_LIMIT;
    Pex pex = {.next = bytes, .left = length, .inventory = *inventory};
    if (u32(&pex) != UINT32_C(0xfa57c0de)) return SKSE_PEX_UNSUPPORTED;
    const uint8_t major = u8(&pex);
    const uint8_t minor = u8(&pex);
    if (major != 3 || (minor != 1 && minor != 2) || u16(&pex) != 1) return SKSE_PEX_UNSUPPORTED;
    take(&pex, 8);
    for (unsigned i = 0; i < 3; ++i) string(&pex);
    pex.string_count = u16(&pex);
    if (pex.status != SKSE_PEX_OK || pex.string_count < 1 || pex.string_count > pex.left / 2)
        return SKSE_PEX_MALFORMED;
    pex.strings = calloc(pex.string_count, sizeof *pex.strings);
    if (!pex.strings) return SKSE_PEX_NOMEM;
    for (unsigned i = 0; i < pex.string_count && pex.status == SKSE_PEX_OK; ++i) pex.strings[i] = string(&pex);
    debug_info(&pex);
    const uint16_t flags = u16(&pex);
    for (unsigned i = 0; i < flags && pex.status == SKSE_PEX_OK; ++i) {
        reference(&pex);
        if (u8(&pex) >= 32) refuse(&pex, SKSE_PEX_MALFORMED);
    }
    if (u16(&pex) != 1) refuse(&pex, SKSE_PEX_UNSUPPORTED);
    object(&pex);
    if (pex.left) refuse(&pex, SKSE_PEX_MALFORMED);
    free(pex.strings);
    if (pex.status == SKSE_PEX_OK) *inventory = pex.inventory;
    return pex.status;
}

static uint32_t classic_type(SksePexText text) {
    static const char *const literals[] = {"None", NULL, "String", "Int", "Float", "Bool"};
    /* Their numeric class IDs are from Classic 1.7.3's GameForms.h. A class
     * outside the observed set remains unknown even if it inherits Form. */
    static const struct { const char *name; uint8_t id; } classes[] = {
        {"Form", 0}, {"Keyword", 0x04}, {"Faction", 0x0b}, {"Race", 0x0e},
        {"ObjectReference", 0x3d}, {"Actor", 0x3e}, {"Idle", 0x4e}, {"Package", 0x4f}, {"Alias", 0x88}
    };
    if (!text.bytes) return UINT32_MAX;
    const bool array = text.length >= 2 && text.bytes[text.length - 2] == '[' && text.bytes[text.length - 1] == ']';
    if (array) text.length -= 2;
    for (uint32_t i = array ? 2 : 0; i < sizeof literals / sizeof literals[0]; ++i)
        if (literals[i] && skse_pex_text_equal(text, literals[i])) return array ? i + 10 : i;
    for (size_t i = 0; i < sizeof classes / sizeof classes[0]; ++i)
        if (skse_pex_text_equal(text, classes[i].name))
            return SKSE_CLASSIC_CATALOG_FORM | classes[i].id | (array ? SKSE_CLASSIC_CATALOG_FORM_ARRAY : 0);
    return UINT32_MAX;
}

SksePexMatch skse_pex_match(const SksePexNative *native, const SkseClassicCatalog *catalog,
                            const SkseClassicDeclaration **matched) {
    if (matched) *matched = NULL;
    if (!native || !catalog || !catalog->rows || catalog->count > SKSE_CLASSIC_CATALOG_MAX_ROWS)
        return SKSE_PEX_MISSING;
    if (native->state.length) return SKSE_PEX_NAMED_STATE;
    const SkseClassicDeclaration *row = NULL;
    for (size_t i = 0; i < catalog->count; ++i) {
        const SkseClassicDeclaration *candidate = &catalog->rows[i];
        if (!memchr(candidate->class_name, 0, sizeof candidate->class_name) ||
            !memchr(candidate->name, 0, sizeof candidate->name) ||
            !skse_pex_text_equal(native->script, candidate->class_name) ||
            !skse_pex_text_equal(native->name, candidate->name)) continue;
        if (row) return SKSE_PEX_AMBIGUOUS;
        row = candidate;
    }
    if (!row) return SKSE_PEX_MISSING;
    if (native->global != row->is_static || native->count != row->count || native->count > SKSE_PEX_MAX_ARGS)
        return SKSE_PEX_DIFFERENT_SIGNATURE;
    const uint32_t result = classic_type(native->result);
    if (result == UINT32_MAX) return SKSE_PEX_UNMODELED_TYPE;
    if (result != row->result) return SKSE_PEX_DIFFERENT_SIGNATURE;
    for (unsigned i = 0; i < native->count; ++i) {
        const uint32_t argument = classic_type(native->arguments[i]);
        if (argument == UINT32_MAX) return SKSE_PEX_UNMODELED_TYPE;
        if (argument != row->arguments[i]) return SKSE_PEX_DIFFERENT_SIGNATURE;
    }
    if (matched) *matched = row;
    return SKSE_PEX_MATCH;
}

SksePexMatch skse_pex_match_call(const SksePexStaticCall *call, const SksePexNative *declarations,
                                 size_t count, const SkseClassicCatalog *catalog,
                                 const SkseClassicDeclaration **matched) {
    if (matched) *matched = NULL;
    if (!call || !declarations || count > SKSE_PEX_MAX_ROWS) return SKSE_PEX_MISSING;
    const SksePexNative *candidate = NULL;
    for (size_t i = 0; i < count; ++i) {
        const SksePexNative *native = &declarations[i];
        if (!equal_text(native->script, call->script) || !equal_text(native->name, call->name)) continue;
        if (candidate) return SKSE_PEX_AMBIGUOUS;
        candidate = native;
    }
    if (!candidate) return SKSE_PEX_MISSING;
    if (!candidate->global || candidate->count != call->count) return SKSE_PEX_DIFFERENT_SIGNATURE;
    return skse_pex_match(candidate, catalog, matched);
}
