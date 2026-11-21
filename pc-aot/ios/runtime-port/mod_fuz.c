/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "mod_fuz.h"
#include "mod_plugins.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

enum { INI_LIMIT = 64 * 1024, LINE_LIMIT = 1024 };

const char *skyrim_fuz_stock_path(unsigned seconds) {
    static const char *const paths[SKYRIM_FUZ_STOCK_CLIPS] = {
        "Sound/Voice/Fuz Ro Doh/Stock_1.xwm", "Sound/Voice/Fuz Ro Doh/Stock_2.xwm",
        "Sound/Voice/Fuz Ro Doh/Stock_3.xwm", "Sound/Voice/Fuz Ro Doh/Stock_4.xwm",
        "Sound/Voice/Fuz Ro Doh/Stock_5.xwm", "Sound/Voice/Fuz Ro Doh/Stock_6.xwm",
        "Sound/Voice/Fuz Ro Doh/Stock_7.xwm", "Sound/Voice/Fuz Ro Doh/Stock_8.xwm",
        "Sound/Voice/Fuz Ro Doh/Stock_9.xwm", "Sound/Voice/Fuz Ro Doh/Stock_10.xwm"};
    return seconds >= 1 && seconds <= SKYRIM_FUZ_STOCK_CLIPS ? paths[seconds - 1] : NULL;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') ++s;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = 0;
    return s;
}

/* Whole-string decimal integer. */
static int parse_int(const char *text, long min, long max, int *out) {
    if (!*text) return 0;
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || *end || value < min || value > max) return 0;
    *out = (int)value;
    return 1;
}

void skyrim_fuz_parse_settings(const char *text, size_t length, SkyrimFuzSettings *out) {
    out->words_per_second = SKYRIM_FUZ_DEFAULT_WORDS_PER_SECOND;
    out->skip_empty_responses = SKYRIM_FUZ_DEFAULT_SKIP_EMPTY;
    out->issues = 0;
    out->found_file = 0;
    if (length >= 3 && !memcmp(text, "\xef\xbb\xbf", 3)) { text += 3; length -= 3; }
    int in_general = 0;
    size_t i = 0;
    while (i < length) {
        char line[LINE_LIMIT];
        size_t n = 0;
        int too_long = 0;
        while (i < length && text[i] != '\n') {
            if (n + 1 < sizeof line) line[n++] = text[i]; else too_long = 1;
            ++i;
        }
        ++i;
        line[n] = 0;
        if (memchr(line, 0, n)) continue; /* a NUL inside a line makes it unusable */
        line[n] = 0;
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close) { in_general = 0; continue; }
            *close = 0;
            in_general = !strcasecmp(trim(s + 1), "General");
            continue;
        }
        if (!in_general || too_long) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s), *value = trim(eq + 1);
        if (!strcasecmp(key, "WordsPerSecondSilence")) {
            if (!parse_int(value, 1, 2147483647L, &out->words_per_second)) {
                out->words_per_second = SKYRIM_FUZ_DEFAULT_WORDS_PER_SECOND;
                out->issues |= SKYRIM_FUZ_ISSUE_WORDS_VALUE;
            } else out->issues &= ~(unsigned)SKYRIM_FUZ_ISSUE_WORDS_VALUE;
        } else if (!strcasecmp(key, "SkipEmptyResponses")) {
            int v;
            if (!parse_int(value, -2147483647L, 2147483647L, &v)) {
                out->skip_empty_responses = SKYRIM_FUZ_DEFAULT_SKIP_EMPTY;
                out->issues |= SKYRIM_FUZ_ISSUE_SKIP_VALUE;
            } else { out->skip_empty_responses = v != 0; out->issues &= ~(unsigned)SKYRIM_FUZ_ISSUE_SKIP_VALUE; }
        }
    }
}

/* 1 found regular file (path filled), 0 absent, -1 link, alias or error. */
static int resolve(const char *root, char *out, size_t capacity) {
    static const char *const parts[] = {"SKSE", "Plugins", "Fuz Ro D'oh.ini"};
    char current[4096];
    if (snprintf(current, sizeof current, "%s", root) >= (int)sizeof current) return -1;
    for (int i = 0; i < 3; ++i) {
        char next[4096];
        int found = skyrim_plugin_child_path(current, parts[i], next, sizeof next);
        if (found <= 0) return found;
        struct stat st;
        if (lstat(next, &st)) return errno == ENOENT ? 0 : -1;
        if (S_ISLNK(st.st_mode) || (i == 2 ? !S_ISREG(st.st_mode) : !S_ISDIR(st.st_mode))) return -1;
        memcpy(current, next, strlen(next) + 1);
    }
    if (strlen(current) >= capacity) return -1;
    memcpy(out, current, strlen(current) + 1);
    return 1;
}

int skyrim_fuz_read_settings(const char *const *roots, size_t count, SkyrimFuzSettings *out) {
    if (!out || (count && !roots)) return EINVAL;
    *out = (SkyrimFuzSettings){.words_per_second = SKYRIM_FUZ_DEFAULT_WORDS_PER_SECOND,
                               .skip_empty_responses = SKYRIM_FUZ_DEFAULT_SKIP_EMPTY};
    char path[4096];
    int found = 0;
    for (size_t i = count; i-- > 0 && !found;) {
        found = resolve(roots[i], path, sizeof path);
        if (found < 0) { out->issues |= SKYRIM_FUZ_ISSUE_UNREADABLE; return 0; }
    }
    if (!found) return 0;
    FILE *f = fopen(path, "rb");
    char *text = f ? malloc(INI_LIMIT + 1) : NULL;
    size_t got = text ? fread(text, 1, INI_LIMIT + 1, f) : 0;
    int bad = !text || ferror(f) || got > INI_LIMIT;
    if (f) fclose(f);
    if (bad) { free(text); out->issues |= SKYRIM_FUZ_ISSUE_UNREADABLE; return 0; }
    skyrim_fuz_parse_settings(text, got, out);
    free(text);
    out->found_file = 1;
    return 0;
}
