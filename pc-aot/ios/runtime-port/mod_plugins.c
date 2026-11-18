/* SPDX-License-Identifier: GPL-3.0-only
 * Classic plugin header reading and load-order selection. Startup only; no
 * game memory, guest addresses or original installation edits. */
#include "mod_plugins.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { TES4_HEADER_BYTES = 24, TES4_COMPRESSED = 0x00040000u };
static const char k_managed[] = "# Managed by SkyrimIOS Settings > MOD. Changes here are replaced at launch.\r\n";

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

size_t skyrim_plugin_header_size(const unsigned char *first8, size_t length) {
    if (!first8 || length < 8 || memcmp(first8, "TES4", 4)) return 0;
    uint32_t data = le32(first8 + 4);
    if (data > SKYRIM_PLUGIN_HEADER_MAX - TES4_HEADER_BYTES) return 0;
    return TES4_HEADER_BYTES + (size_t)data;
}

void skyrim_plugin_header_free(SkyrimPluginHeader *header) {
    if (!header) return;
    free(header->masters);
    memset(header, 0, sizeof *header);
}

const char *skyrim_plugin_master(const SkyrimPluginHeader *header, unsigned index) {
    if (!header || index >= header->master_count) return NULL;
    const char *p = header->masters;
    while (index--) p += strlen(p) + 1;
    return p;
}

int skyrim_plugin_parse_header(const unsigned char *bytes, size_t length,
                               SkyrimPluginHeader *out) {
    if (!out) return SKYRIM_PLUGIN_HEADER_NOT_TES4;
    memset(out, 0, sizeof *out);
    if (!bytes || length < 8 || memcmp(bytes, "TES4", 4)) return SKYRIM_PLUGIN_HEADER_NOT_TES4;
    size_t total = skyrim_plugin_header_size(bytes, length);
    if (!total) return SKYRIM_PLUGIN_HEADER_TOO_LARGE;
    if (length < total) return SKYRIM_PLUGIN_HEADER_TRUNCATED;
    uint32_t flags = le32(bytes + 8);
    if (flags & TES4_COMPRESSED) return SKYRIM_PLUGIN_HEADER_COMPRESSED;
    out->flags = flags;
    out->version = le16(bytes + 20);
    /* Collect masters into one buffer; at most the data size in bytes. */
    size_t data = total - TES4_HEADER_BYTES, used = 0;
    char *names = malloc(data ? data : 1);
    if (!names) return SKYRIM_PLUGIN_HEADER_NO_MEMORY;
    const unsigned char *p = bytes + TES4_HEADER_BYTES, *end = bytes + total;
    uint32_t pending = 0; int has_pending = 0;
    unsigned count = 0;
    int status = SKYRIM_PLUGIN_HEADER_OK;
    while (p < end) {
        if ((size_t)(end - p) < 6) { status = SKYRIM_PLUGIN_HEADER_BAD_SUBRECORD; break; }
        const unsigned char *type = p;
        size_t size = le16(p + 4);
        p += 6;
        if (has_pending) {
            if (size != 0) { status = SKYRIM_PLUGIN_HEADER_BAD_SUBRECORD; break; }
            size = pending; has_pending = 0;
        }
        if ((size_t)(end - p) < size) { status = SKYRIM_PLUGIN_HEADER_BAD_SUBRECORD; break; }
        if (!memcmp(type, "XXXX", 4)) {
            if (size != 4) { status = SKYRIM_PLUGIN_HEADER_BAD_SUBRECORD; break; }
            pending = le32(p); has_pending = 1;
        } else if (!memcmp(type, "MAST", 4)) {
            const unsigned char *nul = size ? memchr(p, 0, size) : NULL;
            size_t n = nul ? (size_t)(nul - p) : 0;
            if (!nul || !n || n >= SKYRIM_PLUGIN_NAME_MAX) { status = SKYRIM_PLUGIN_HEADER_BAD_MASTER; break; }
            memcpy(names + used, p, n + 1);
            used += n + 1;
            ++count;
        }
        p += size;
    }
    if (status == SKYRIM_PLUGIN_HEADER_OK && has_pending) status = SKYRIM_PLUGIN_HEADER_BAD_SUBRECORD;
    if (status != SKYRIM_PLUGIN_HEADER_OK) { free(names); memset(out, 0, sizeof *out); return status; }
    out->masters = names;
    out->master_count = count;
    return SKYRIM_PLUGIN_HEADER_OK;
}

static int has_extension(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcasecmp(name + n - e, ext);
}

int skyrim_plugin_name_valid(const char *name) {
    if (!name) return 0;
    size_t n = strlen(name);
    if (n < 5 || n >= SKYRIM_PLUGIN_NAME_MAX) return 0;
    if (!has_extension(name, ".esp") && !has_extension(name, ".esm")) return 0;
    if (name[0] == '.' || name[0] == ' ' || name[n - 5] == ' ') return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c > 0x7e || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '#') return 0;
    }
    return 1;
}

int skyrim_plugin_is_base(const char *name) {
    return name && (!strcasecmp(name, "Skyrim.esm") || !strcasecmp(name, "Update.esm"));
}

int skyrim_plugin_default_rank(const char *name, int is_master) {
    static const char *const official[] = {
        "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm",
        "HighResTexturePack01.esp", "HighResTexturePack02.esp", "HighResTexturePack03.esp"};
    for (int i = 0; i < (int)(sizeof official / sizeof official[0]); ++i)
        if (name && !strcasecmp(name, official[i])) return i;
    return is_master ? 100 : 200;
}

static int is_master(const SkyrimPluginEntry *e) { return e->header && (e->header->flags & 1u); }

/* Index of the entry named name, or -1. Duplicates are already blocked, so the
 * first case-insensitive match is the only candidate. */
static long find_entry(const SkyrimPluginEntry *entries, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i)
        if (!strcasecmp(entries[i].name, name)) return (long)i;
    return -1;
}

/* Mark entries whose masters are not loadable; repeat until stable. */
static void propagate(const SkyrimPluginEntry *entries, size_t count,
                      SkyrimPluginState *state, int *problem) {
    for (int changed = 1; changed;) {
        changed = 0;
        for (size_t i = 0; i < count; ++i) {
            if (state[i] != SKYRIM_PLUGIN_LOAD) continue;
            const SkyrimPluginHeader *h = entries[i].header;
            for (unsigned m = 0; m < h->master_count; ++m) {
                const char *master = skyrim_plugin_master(h, m);
                if (skyrim_plugin_is_base(master)) continue;
                long j = find_entry(entries, count, master);
                if (j < 0 || (size_t)j == i || state[j] != SKYRIM_PLUGIN_LOAD) {
                    state[i] = SKYRIM_PLUGIN_MISSING_MASTER; problem[i] = (int)m; changed = 1; break;
                }
                if (is_master(&entries[i]) && !is_master(&entries[j])) {
                    state[i] = SKYRIM_PLUGIN_MASTER_AFTER; problem[i] = (int)m; changed = 1; break;
                }
            }
        }
    }
}

size_t skyrim_plugin_order(const SkyrimPluginEntry *entries, size_t count,
                           size_t *order_out, SkyrimPluginState *state, int *problem) {
    if (!entries || !state || !problem || !order_out) return 0;
    for (size_t i = 0; i < count; ++i) {
        problem[i] = -1;
        const SkyrimPluginEntry *e = &entries[i];
        if (!e->enabled) state[i] = SKYRIM_PLUGIN_OFF;
        else if (!skyrim_plugin_name_valid(e->name) || skyrim_plugin_is_base(e->name)) state[i] = SKYRIM_PLUGIN_BAD_NAME;
        else if (e->header_status != SKYRIM_PLUGIN_HEADER_OK || !e->header) state[i] = SKYRIM_PLUGIN_UNREADABLE;
        else state[i] = SKYRIM_PLUGIN_LOAD;
        if (state[i] != SKYRIM_PLUGIN_BAD_NAME)
            for (size_t j = 0; j < i; ++j)
                if (!strcasecmp(entries[j].name, e->name)) {
                    if (e->enabled) state[i] = SKYRIM_PLUGIN_DUPLICATE;
                    if (entries[j].enabled) state[j] = SKYRIM_PLUGIN_DUPLICATE;
                }
    }
    propagate(entries, count, state, problem);

    unsigned char *placed = calloc(count ? count : 1, 1);
    if (!placed) {
        for (size_t i = 0; i < count; ++i) if (state[i] == SKYRIM_PLUGIN_LOAD) state[i] = SKYRIM_PLUGIN_LIMIT;
        return 0;
    }
    size_t loaded = 0;
    for (int group = 1; group >= 0; --group) {     /* masters, then plugins */
        for (;;) {
            long pick = -1;
            int remaining = 0;
            for (size_t i = 0; i < count && pick < 0; ++i) {
                if (state[i] != SKYRIM_PLUGIN_LOAD || placed[i] || is_master(&entries[i]) != group) continue;
                remaining = 1;
                const SkyrimPluginHeader *h = entries[i].header;
                int ready = 1;
                for (unsigned m = 0; m < h->master_count && ready; ++m) {
                    const char *master = skyrim_plugin_master(h, m);
                    if (skyrim_plugin_is_base(master)) continue;
                    long j = find_entry(entries, count, master);
                    ready = j >= 0 && placed[j];
                }
                if (ready) pick = (long)i;
            }
            if (pick >= 0) { placed[pick] = 1; order_out[loaded++] = (size_t)pick; continue; }
            if (!remaining) break;
            /* Nothing in this group can be placed: the rest form or follow a
             * cycle. Block them, then re-check everything that needs them. */
            for (size_t i = 0; i < count; ++i)
                if (state[i] == SKYRIM_PLUGIN_LOAD && !placed[i] && is_master(&entries[i]) == group)
                    state[i] = SKYRIM_PLUGIN_CYCLE;
            propagate(entries, count, state, problem);
            break;
        }
    }
    free(placed);
    /* propagate() after a cycle can only block unplaced entries of the plugin
     * group, which is processed after the masters, so placed entries remain
     * loadable. Apply the engine slot limit in final order: a dependent
     * always follows its masters, so it is cut whenever a master is cut. */
    size_t limit = SKYRIM_PLUGIN_MAX_LOADED - SKYRIM_PLUGIN_BASE_COUNT, kept = 0;
    for (size_t k = 0; k < loaded; ++k) {
        size_t i = order_out[k];
        if (state[i] != SKYRIM_PLUGIN_LOAD) continue;
        if (kept >= limit) { state[i] = SKYRIM_PLUGIN_LIMIT; continue; }
        order_out[kept++] = i;
    }
    return kept;
}

char *skyrim_plugin_list_text(const SkyrimPluginEntry *entries,
                              const size_t *order, size_t loaded, size_t *out_bytes) {
    static const char base[] = "Skyrim.esm\r\nUpdate.esm\r\n";
    size_t bytes = sizeof k_managed - 1 + sizeof base - 1;
    for (size_t k = 0; k < loaded; ++k) bytes += strlen(entries[order[k]].name) + 2;
    char *text = malloc(bytes + 1), *p = text;
    if (!text) return NULL;
    memcpy(p, k_managed, sizeof k_managed - 1); p += sizeof k_managed - 1;
    memcpy(p, base, sizeof base - 1); p += sizeof base - 1;
    for (size_t k = 0; k < loaded; ++k) {
        size_t n = strlen(entries[order[k]].name);
        memcpy(p, entries[order[k]].name, n); p += n;
        *p++ = '\r'; *p++ = '\n';
    }
    *p = 0;
    if (out_bytes) *out_bytes = bytes;
    return text;
}

int skyrim_plugin_list_is_managed(const char *bytes, size_t length) {
    return bytes && length >= sizeof k_managed - 1 && !memcmp(bytes, k_managed, sizeof k_managed - 1);
}

int skyrim_plugin_child_path(const char *directory, const char *name, char *out, size_t capacity) {
    if (!directory || !name || !out || !capacity) return -1;
    DIR *dir = opendir(directory);
    const char *chosen = name;
    char found[SKYRIM_PLUGIN_NAME_MAX];
    int matches = 0;
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (strcasecmp(entry->d_name, name)) continue;
            if (++matches > 1) break;
            if (strlen(entry->d_name) >= sizeof found) { matches = 2; break; }
            strcpy(found, entry->d_name);
        }
        closedir(dir);
        if (matches > 1) return -1;
        if (matches) chosen = found;
    }
    int n = snprintf(out, capacity, "%s/%s", directory, chosen);
    if (n < 0 || (size_t)n >= capacity) return -1;
    return matches ? 1 : 0;
}
