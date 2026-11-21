/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "classic_mod_layout.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static int skse_classic_file_property(CPU *cpu, unsigned op, uint32_t file,
        uint32_t master_index, uint32_t result) {
    switch (op) {
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

/* The selected TESFile name belongs to DataHandler. Copy the fixed-size field
 * before string interning, which calls back into the guest VM. */
static int skse_classic_copy_mod_name(CPU *cpu, uint32_t handler, uint32_t node,
                                      uint32_t file, uint32_t result) {
    char name[CLASSIC_FILE_NAME_BYTES];
    memcpy(name, str(file + CLASSIC_FILE_NAME), sizeof name);
    if (!memchr(name, 0, sizeof name) || M32(CLASSIC_DATA_HANDLER) != handler || M32(node) != file)
        return 0;
    vm_string(cpu, result, name);
    return 1;
}

/* Called on Papyrus queries only, after the dispatcher has initialized the
 * typed result to zero. No per-frame work and no copy of the game registry. */
static int skse_classic_read_mod_query(CPU *cpu, unsigned op, const uint32_t *args, uint32_t result) {
    enum { FIRST_MAPPED = 0x10000 };
    /* SKSE returns a null BSFixedString for out-of-range GetModName before
     * touching DataHandler. The dispatcher has already set String+null. */
    if (op == S_MOD_NAME && args[0] > UINT8_MAX) return 1;
    uint32_t handler = M32(CLASSIC_DATA_HANDLER);
    if (handler < FIRST_MAPPED || (handler & 3u) || handler > UINT32_MAX - CLASSIC_LOADED_COUNT - 3u) return 0;
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
        if (node < FIRST_MAPPED || (node & 3u) || node > UINT32_MAX - 7u) return 0;
        uint32_t file = M32(node);
        if (!file) return 1;  /* Classic tList's terminating empty item. */
        if (file < FIRST_MAPPED || (file & 3u) || file > UINT32_MAX - CLASSIC_FILE_DESCRIPTION - 3u) return 0;
        if (by_name) {
            const char *name = str(file + CLASSIC_FILE_NAME);
            if (!memchr(name, 0, CLASSIC_FILE_NAME_BYTES)) return 0;
            if (!strcasecmp(name, query)) {
                M32(result + 4) = (uint8_t)position;
                return 1;
            }
        } else if (position == args[0]) {
            /* Classic GetModName indexes modInfoList, including entries that
             * have no assigned active load index. */
            if (op == S_MOD_NAME) return skse_classic_copy_mod_name(cpu, handler, node, file, result);
            return skse_classic_file_property(cpu, op, file, args[1], result);
        }
        node = M32(node + 4);
        if (position & 1u) slow = M32(slow + 4);
        if (node && node == slow) return 0;
    }
    return 1;
}

/* Called from the registered Papyrus native. A handled empty result is valid.
 * Emit each outcome once per query type and process, without game data. */
static int skse_classic_mod_query(CPU *cpu, unsigned op, const uint32_t *args, uint32_t result) {
    static atomic_uint reported;
    if (op < S_MOD_COUNT || op > S_MOD_NTH_MASTER) return 0;
    int handled = skse_classic_read_mod_query(cpu, op, args, result);
    unsigned query = op - S_MOD_COUNT;
    unsigned bit = 1u << (query * 2u + (unsigned)handled);
    if (!(atomic_load_explicit(&reported, memory_order_relaxed) & bit) &&
        !(atomic_fetch_or_explicit(&reported, bit, memory_order_relaxed) & bit))
        fprintf(stderr, "IOS_CLASSIC_QUERY id=%u handled=%d\n", query, handled);
    return handled;
}
