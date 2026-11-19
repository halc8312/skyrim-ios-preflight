/* SPDX-License-Identifier: GPL-3.0-only
 * Included only in the staged Classic importer, after its resource functions.
 * All state below is protected by its translation_lock. Each call originates
 * from creation of a Scaleform movie, never the frame or gameplay tick. */
#pragma once
#include "mobile_ui.h"
#include <time.h>

enum { TRANSLATION_MAX_ATTEMPTS = 7 };
static uint32_t translation_retry_loader;
static unsigned translation_failures;
static uint64_t translation_due_ms;

static uint64_t translation_clock_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return UINT64_MAX;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static int translation_attempt_due(uint32_t loader) {
    if (translation_retry_loader != loader) {
        translated_loader = 0;
        translation_retry_loader = loader;
        translation_failures = 0;
        translation_due_ms = 0;
    }
    if (!translation_due_ms) return 1;
    uint64_t now = translation_clock_ms();
    return now >= translation_due_ms;
}

static int translation_last_attempt(void) {
    return translation_failures + 1 >= TRANSLATION_MAX_ATTEMPTS;
}

static int translation_defer(void) {
    if (translation_last_attempt()) return 0;
    uint64_t now = translation_clock_ms();
    translation_due_ms = now == UINT64_MAX ? 0 : now + (UINT64_C(1000) << translation_failures);
    ++translation_failures;
    return 1;
}

static char *translation_plugin_name(char *line) {
    line[strcspn(line, "\r\n")] = '\0';
    while (*line == ' ' || *line == '\t' || *line == '*') ++line;
    if (!*line || *line == '#' || strchr(line, '/') || strchr(line, '\\')) return NULL;
    char *ext = strrchr(line, '.');
    if (!ext || ext == line || (strcasecmp(ext, ".esp") && strcasecmp(ext, ".esm"))) return NULL;
    *ext = '\0';
    return line;
}

static int import_plugin_translations(CPU *c, uint32_t translator) {
    const char *language = skyrim_mobile_classic_text_language();
    if (!language) {
        uint32_t setting = skse_ini_setting(c, "sLanguage:General");
        language = setting && M32(setting + 4) ? GSTR(M32(setting + 4)) : NULL;
        if (!language || !*language) return 0;
    }
    FILE *list = skyrim_skse_open_plugin_list(g_save_dir);
    if (!list) return 0;
    uint32_t reserved = 0;
    char line[512], path[1024];
    /* The selected LE trial ships both of these dictionaries. Most plugins
     * ship no dictionary at all, so missing optional resources must not keep
     * the loader pending. Hold the one required stream across this pass. */
    int needs_skyui = skse_skyui_enabled() &&
        (!strcasecmp(language, "JAPANESE") || !strcasecmp(language, "ENGLISH"));
    if (needs_skyui) {
        fpos_t start;
        if (fgetpos(list, &start) != 0) { fclose(list); return 0; }
        while (fgets(line, sizeof line, list)) {
            const char *name = translation_plugin_name(line);
            if (name && !strcasecmp(name, "SkyUI")) {
                int length = snprintf(path, sizeof path, "Interface\\Translations\\%s_%s.txt", name, language);
                if (length >= 0 && (size_t)length < sizeof path) reserved = open_translation_stream(c, path);
                break;
            }
        }
        if ((reserved == 0 && !translation_last_attempt()) || ferror(list) || fsetpos(list, &start) != 0) {
            if (reserved) close_translation_stream(c, reserved);
            fclose(list);
            return 0;
        }
    }
    while (fgets(line, sizeof line, list)) {
        const char *name = translation_plugin_name(line);
        if (!name) continue;
        int length = snprintf(path, sizeof path, "Interface\\Translations\\%s_%s.txt", name, language);
        if (length < 0 || (size_t)length >= sizeof path) continue;
        if (needs_skyui && !strcasecmp(name, "SkyUI")) {
            if (reserved) import_file(c, translator, path, reserved);
            reserved = 0;
        } else {
            import_file(c, translator, path, 0);
        }
    }
    if (reserved) close_translation_stream(c, reserved);
    fclose(list);
    return 1;
}
