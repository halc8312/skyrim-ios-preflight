/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/* Public 32-bit TESV 1.9.32 layout described by the Classic 1.7.3 SDK.
 * The loaded count, linked-list position, and a master's assigned load index
 * are separate values; a profile's requested order cannot supply these. */
enum {
    CLASSIC_DATA_HANDLER = 0x012e2cb8,
    CLASSIC_MOD_LIST = 0x694,
    CLASSIC_LOADED_COUNT = 0x69c,
    CLASSIC_FILE_NAME = 0x20,
    CLASSIC_FILE_NAME_BYTES = 0x104,
    CLASSIC_FILE_MASTER_COUNT = 0x3fc,
    CLASSIC_FILE_MASTERS = 0x400,
    CLASSIC_FILE_INDEX = 0x40c,
    CLASSIC_FILE_AUTHOR = 0x410,
    CLASSIC_FILE_DESCRIPTION = 0x418
};

static int skse_classic_file_property(CPU *cpu, unsigned op, uint32_t file,
        uint32_t master_index, uint32_t result) {
    switch (op) {
    case S_MOD_NAME: {
        const char *name = str(file + CLASSIC_FILE_NAME);
        if (!memchr(name, 0, CLASSIC_FILE_NAME_BYTES)) return 0;
        vm_string(cpu, result, name);
        return 1;
    }
    case S_MOD_AUTHOR:
    case S_MOD_DESCRIPTION: {
        unsigned field = op == S_MOD_AUTHOR ? CLASSIC_FILE_AUTHOR : CLASSIC_FILE_DESCRIPTION;
        /* An existing plugin with unset metadata returns an empty VM string. */
        vm_string(cpu, result, str(M32(file + field)));
        return 1;
    }
    case S_MOD_MASTER_COUNT:
        M32(result + 4) = M32(file + CLASSIC_FILE_MASTER_COUNT);
        return 1;
    case S_MOD_NTH_MASTER: {
        if (master_index >= M32(file + CLASSIC_FILE_MASTER_COUNT)) return 1;
        uint32_t masters = M32(file + CLASSIC_FILE_MASTERS);
        uint64_t entry = (uint64_t)masters + (uint64_t)master_index * 4;
        if (!masters || entry > UINT32_MAX - 3u) return 0;
        uint32_t master = M32((uint32_t)entry);
        if (!master || master > UINT32_MAX - CLASSIC_FILE_INDEX) return 0;
        M32(result + 4) = M8(master + CLASSIC_FILE_INDEX);
        return 1;
    }
    default:
        return 0;
    }
}

/* Called on Papyrus queries only, after the dispatcher has initialized the
 * typed result to zero. No per-frame work and no copy of the game registry. */
static int skse_classic_mod_query(CPU *cpu, unsigned op, const uint32_t *args, uint32_t result) {
    uint32_t handler = M32(CLASSIC_DATA_HANDLER);
    if (!handler || handler > UINT32_MAX - CLASSIC_LOADED_COUNT - 3u) return 0;
    if (op == S_MOD_COUNT) {
        M32(result + 4) = M32(handler + CLASSIC_LOADED_COUNT);
        return 1;
    }
    int by_name = op == S_MOD_FIND;
    if (by_name) M32(result + 4) = UINT8_MAX;
    if ((by_name && !args[0]) || (!by_name && args[0] > UINT8_MAX)) return 1;
    const char *query = by_name ? str(args[0]) : NULL;
    uint32_t node = handler + CLASSIC_MOD_LIST;
    uint32_t slow = node;
    for (uint32_t position = 0; node; ++position) {
        if (node > UINT32_MAX - 7u) return 0;
        uint32_t file = M32(node);
        if (!file) return 1;  /* Classic tList's terminating empty item. */
        if (file > UINT32_MAX - CLASSIC_FILE_DESCRIPTION - 3u) return 0;
        if (by_name) {
            const char *name = str(file + CLASSIC_FILE_NAME);
            if (!memchr(name, 0, CLASSIC_FILE_NAME_BYTES)) return 0;
            if (!strcasecmp(name, query)) {
                M32(result + 4) = (uint8_t)position;
                return 1;
            }
        } else if (position == args[0]) {
            return skse_classic_file_property(cpu, op, file, args[1], result);
        }
        node = M32(node + 4);
        if (position & 1u) slow = M32(slow + 4);
        if (node && node == slow) return 0;
    }
    return 1;
}
