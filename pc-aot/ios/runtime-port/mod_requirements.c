/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "mod_requirements.h"
#include "mod_plugins.h"
#include "classic_pex.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum { PATH_MAX_BYTES = 4096, PEX_LIMIT = 8 << 20 };

/* 1 regular file found (path filled), 0 absent, -1 link, alias or error. */
static int resolve(const char *root, const char *relative, char *out, size_t capacity) {
    char current[PATH_MAX_BYTES], part[256];
    if (snprintf(current, sizeof current, "%s", root) >= (int)sizeof current) return -1;
    while (*relative) {
        size_t n = strcspn(relative, "/");
        if (!n || n >= sizeof part) return -1;
        memcpy(part, relative, n);
        part[n] = 0;
        char next[PATH_MAX_BYTES];
        int found = skyrim_plugin_child_path(current, part, next, sizeof next);
        if (found < 0) return -1;
        if (!found) return 0;
        struct stat st;
        if (lstat(next, &st)) return errno == ENOENT ? 0 : -1;
        relative += n;
        const int last = !*relative;
        if (S_ISLNK(st.st_mode) || (last ? !S_ISREG(st.st_mode) : !S_ISDIR(st.st_mode))) return -1;
        if (!last) ++relative;
        memcpy(current, next, strlen(next) + 1);
    }
    if (strlen(current) >= capacity) return -1;
    memcpy(out, current, strlen(current) + 1);
    return 1;
}

/* The last root holding the file wins, as in the launch view. */
static int winner(const char *const *roots, size_t count, const char *relative, char *out, size_t capacity) {
    for (size_t i = count; i-- > 0;) {
        int r = resolve(roots[i], relative, out, capacity);
        if (r) return r;
    }
    return 0;
}

static unsigned char *read_file(const char *path, size_t limit, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char *bytes = malloc(limit + 1);
    size_t got = bytes ? fread(bytes, 1, limit + 1, f) : 0;
    int bad = !bytes || ferror(f) || got > limit;
    fclose(f);
    if (bad) { free(bytes); return NULL; }
    *size = got;
    return bytes;
}

static SkyrimSkyUIRequirement check_skse_script(const char *path) {
    size_t size = 0;
    unsigned char *bytes = read_file(path, PEX_LIMIT, &size);
    if (!bytes) return SKYRIM_SKYUI_REQ_SKSE_SCRIPT_INVALID;
    SksePexNative *natives = calloc(SKSE_PEX_MAX_ROWS, sizeof *natives);
    SkyrimSkyUIRequirement result = SKYRIM_SKYUI_REQ_SKSE_SCRIPT_INVALID;
    if (natives) {
        SksePexInventory inventory = {.natives = natives, .native_capacity = SKSE_PEX_MAX_ROWS};
        if (skse_pex_inspect(bytes, size, &inventory) == SKSE_PEX_OK) {
            for (size_t i = 0; i < inventory.native_count; ++i)
                if (natives[i].global && skse_pex_text_equal(natives[i].script, "skse") &&
                    skse_pex_text_equal(natives[i].name, "getversionrelease")) {
                    result = SKYRIM_SKYUI_REQ_OK;
                    break;
                }
        }
    }
    free(natives);
    free(bytes);
    return result;
}

int skyrim_skyui_requirements(const char *const *roots, size_t count, SkyrimSkyUIRequirements *out) {
    if (!out || (count && !roots)) return EINVAL;
    *out = (SkyrimSkyUIRequirements){.status = SKYRIM_SKYUI_REQ_SKYUI_ABSENT,
                                     .support_expected = SKYRIM_SKYUI_SUPPORT_SCRIPTS};
    char path[PATH_MAX_BYTES];
    int found = winner(roots, count, "SkyUI.esp", path, sizeof path);
    if (found < 0) { out->status = SKYRIM_SKYUI_REQ_UNREADABLE; return 0; }
    if (!found) return 0;

    found = winner(roots, count, "SkyUI.bsa", path, sizeof path);
    if (found < 0) { out->status = SKYRIM_SKYUI_REQ_UNREADABLE; return 0; }
    unsigned char header[8] = {0};
    FILE *f = found ? fopen(path, "rb") : NULL;
    size_t got = f ? fread(header, 1, sizeof header, f) : 0;
    if (f) fclose(f);
    uint32_t version = (uint32_t)header[4] | (uint32_t)header[5] << 8 | (uint32_t)header[6] << 16 |
                       (uint32_t)header[7] << 24;
    if (!found || got < sizeof header || memcmp(header, "BSA\0", 4) || version != 104) {
        out->status = SKYRIM_SKYUI_REQ_SKYUI_ARCHIVE_UNSUPPORTED;
        return 0;
    }

    static const char *const support[] = {"Scripts/UI.pex", "Scripts/UICallback.pex",
                                          "Scripts/Input.pex", "Scripts/StringUtil.pex"};
    for (size_t i = 0; i < sizeof support / sizeof *support; ++i)
        if (winner(roots, count, support[i], path, sizeof path) > 0) ++out->support_present;

    found = winner(roots, count, "Scripts/SKSE.pex", path, sizeof path);
    out->status = found < 0 ? SKYRIM_SKYUI_REQ_UNREADABLE
                : !found ? SKYRIM_SKYUI_REQ_SKSE_SCRIPT_MISSING : check_skse_script(path);
    return 0;
}
