/* SPDX-License-Identifier: GPL-3.0-only */
#include "ece_head_geometry.h"

enum {
    FIRST_OBJECT = 0x10000, FACE_VTABLE = 0x10bea8c,
    NAME_LOOKUP = 0x9c, TRI_BASED_CAST = 0x20,
    NODE_LOOKUP = 0xaae1d0, LEAF_LOOKUP = 0xaaf450,
    CAST_SELF = 0xc707b0, CAST_NULL = 0x5eadd0,
    NAME = 8, PARENT = 0x18, CHILDREN = 0xac, CHILD_ARRAY_SIZES = 0xb0,
    MAX_DEPTH = 32, MAX_CHILD_SLOTS = 256
};

typedef struct {
    EceNoseRead read;
    void *context;
    uint32_t name, geometry;
    bool found;
    unsigned slots;
} EceGeometrySearch;

static bool word(const EceGeometrySearch *search, uint32_t object, uint32_t offset, uint32_t *out) {
    uint8_t bytes[4];
    if (object < FIRST_OBJECT || (object & 3) || object > UINT32_MAX - offset - 3 ||
        !search->read(search->context, object + offset, bytes, sizeof bytes)) return false;
    *out = (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
    return true;
}

static bool find(EceGeometrySearch *search, uint32_t node, uint32_t parent, unsigned depth) {
    uint32_t vtable, lookup, name, backlink;
    if (depth >= MAX_DEPTH || !word(search, node, 0, &vtable) ||
        (!depth && vtable != FACE_VTABLE) || !word(search, vtable, NAME_LOOKUP, &lookup) ||
        (lookup != NODE_LOOKUP && lookup != LEAF_LOOKUP) ||
        !word(search, node, NAME, &name) ||
        (depth && (!word(search, node, PARENT, &backlink) || backlink != parent))) return false;
    if (name == search->name) {
        uint32_t cast;
        if (!word(search, vtable, TRI_BASED_CAST, &cast) ||
            (cast != CAST_SELF && cast != CAST_NULL)) return false;
        /* The stock search stops even if its subsequent cast returns null. */
        search->found = true;
        if (cast == CAST_SELF) search->geometry = node;
        return true;
    }
    if (lookup == LEAF_LOOKUP) return true;

    uint32_t sizes, children;
    if (!word(search, node, CHILD_ARRAY_SIZES, &sizes)) return false;
    const unsigned count = sizes >> 16;  /* Span including holes, as in the original NiNode visitor. */
    if (!count) return true;
    if (count > (sizes & 0xffff) || count > search->slots || !word(search, node, CHILDREN, &children))
        return false;
    search->slots -= count;
    for (unsigned index = 0; index < count && !search->found; ++index) {
        uint32_t child;
        if (!word(search, children, index * 4, &child) || (child && !find(search, child, node, depth + 1)))
            return false;
    }
    return true;
}

bool ece_head_geometry(EceNoseRead read, void *context, uint32_t face, uint32_t name, uint32_t *out) {
    if (!read || !name || !out) return false;
    EceGeometrySearch search = {.read = read, .context = context, .name = name, .slots = MAX_CHILD_SLOTS};
    if (!find(&search, face, 0, 0)) return false;
    *out = search.geometry;
    return true;
}

static bool string_length(const EceGeometrySearch *search, uint32_t address, unsigned *out) {
    *out = 0;
    if (!address) return true;
    uint32_t header;
    if (address < FIRST_OBJECT + 4 || !word(search, address - 4, 0, &header)) return false;
    *out = header & 0xffffff;
    return true;
}

bool ece_head_layout(EceNoseRead read, void *context, uint32_t face, uint32_t list,
                     uint32_t count, EceHeadLayout *out) {
    enum {
        TYPE = 0x12, HEAD_PART = 0x0c,
        PART_MODEL = 0x20, PART_TRI_MODEL = 0x78, PART_TRI_PATH = 0x7c, PART_NODE_NAME = 0x94,
        TRI_MODEL_VTABLE = 0x10a2e08, MODEL_PATH = 0x10, GET_MODEL_PATH = 0x4f8ca0,
        GEOMETRY_MODEL_DATA = 0xb0, VERTEX_COUNT = 8
    };
    if (!read || !out || count > ECE_HEAD_PART_LIMIT) return false;
    EceGeometrySearch search = {.read = read, .context = context, .slots = MAX_CHILD_SLOTS};
    EceHeadLayout result = {0};
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t part, type_word, model, tri_model, path_method, tri, name, model_data, vertices;
        unsigned length;
        if (!word(&search, list, i * 4, &part)) return false;
        if (!part) continue;
        if (!word(&search, part, TYPE & ~3u, &type_word) || ((type_word >> 16) & 0xff) != HEAD_PART ||
            !word(&search, part, PART_MODEL, &model) || !string_length(&search, model, &length))
            return false;
        if (!length) continue;
        if (!word(&search, part, PART_TRI_MODEL, &tri_model) || tri_model != TRI_MODEL_VTABLE ||
            !word(&search, tri_model, MODEL_PATH, &path_method) || path_method != GET_MODEL_PATH ||
            !word(&search, part, PART_TRI_PATH, &tri) || !string_length(&search, tri, &length))
            return false;
        if (!length) continue;
        uint32_t geometry;
        if (!word(&search, part, PART_NODE_NAME, &name) || !name) return false;
        search.name = name;
        search.geometry = 0;
        search.found = false;
        if (!find(&search, face, 0, 0) || !(geometry = search.geometry) ||
            !word(&search, geometry, GEOMETRY_MODEL_DATA, &model_data) ||
            !word(&search, model_data, VERTEX_COUNT, &vertices) || !(uint16_t)vertices)
            return false;
        for (unsigned seen = 0; seen < result.count; ++seen) {
            if (result.parts[seen].part == part || result.parts[seen].geometry == geometry) return false;
        }
        result.parts[result.count++] = (EceHeadPart){part, geometry, model_data, tri, (uint16_t)vertices};
    }
    *out = result;
    return true;
}
