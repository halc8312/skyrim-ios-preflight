/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "enb_ini.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static void trim(const char **begin, const char **end) {
    while (*begin < *end && (**begin == ' ' || **begin == '\t')) ++*begin;
    while (*end > *begin && ((*end)[-1] == ' ' || (*end)[-1] == '\t' || (*end)[-1] == '\r')) --*end;
}

static unsigned copy_field(char *out, size_t capacity, const char *begin, const char *end) {
    size_t n = (size_t)(end - begin);
    unsigned issue = 0;
    if (n >= capacity) { n = capacity - 1; issue = ENB_INI_ISSUE_TRUNCATED_LINE; }
    memcpy(out, begin, n);
    out[n] = 0;
    return issue;
}

int enb_ini_parse(const char *text, size_t length, EnbIni *out) {
    if (!out || (length && !text)) return EINVAL;
    *out = (EnbIni){0};
    if (length > ENB_INI_MAX_BYTES) { out->issues |= ENB_INI_ISSUE_TOO_LARGE; length = ENB_INI_MAX_BYTES; }
    if (length >= 3 && !memcmp(text, "\xef\xbb\xbf", 3)) { text += 3; length -= 3; }
    size_t capacity = 256;
    out->entries = malloc(capacity * sizeof *out->entries);
    if (!out->entries) return ENOMEM;
    char section[ENB_INI_NAME] = "";
    for (size_t at = 0; at < length;) {
        const char *line = text + at, *stop = memchr(line, '\n', length - at);
        const char *end = stop ? stop : text + length;
        at = (size_t)(end - text) + (stop ? 1 : 0);
        const char *begin = line;
        trim(&begin, &end);
        if (begin == end || *begin == ';' || *begin == '#' || (end - begin >= 2 && begin[0] == '/' && begin[1] == '/')) continue;
        if (*begin == '[') {
            const char *close = memchr(begin, ']', (size_t)(end - begin));
            if (!close) continue;
            const char *s = begin + 1, *e = close;
            trim(&s, &e);
            out->issues |= copy_field(section, sizeof section, s, e);
            continue;
        }
        const char *eq = memchr(begin, '=', (size_t)(end - begin));
        if (!eq) continue;
        const char *ke = eq, *vs = eq + 1, *ve = end;
        trim(&begin, &ke);
        trim(&vs, &ve);
        if (begin == ke) continue;
        EnbIniEntry entry = {0};
        out->issues |= copy_field(entry.key, sizeof entry.key, begin, ke);
        out->issues |= copy_field(entry.value, sizeof entry.value, vs, ve);
        memcpy(entry.section, section, sizeof entry.section);
        size_t found = out->count;
        for (size_t i = 0; i < out->count; ++i)
            if (!strcasecmp(out->entries[i].section, entry.section) && !strcasecmp(out->entries[i].key, entry.key)) { found = i; break; }
        if (found == out->count) {
            if (out->count == ENB_INI_MAX_ENTRIES) { out->issues |= ENB_INI_ISSUE_TOO_MANY; continue; }
            if (out->count == capacity) {
                capacity *= 2;
                EnbIniEntry *bigger = realloc(out->entries, capacity * sizeof *bigger);
                if (!bigger) { enb_ini_free(out); return ENOMEM; }
                out->entries = bigger;
            }
            ++out->count;
        }
        out->entries[found] = entry;
    }
    return 0;
}

void enb_ini_free(EnbIni *ini) {
    if (!ini) return;
    free(ini->entries);
    *ini = (EnbIni){0};
}

const char *enb_ini_get(const EnbIni *ini, const char *section, const char *key) {
    if (!ini || !section || !key) return NULL;
    for (size_t i = 0; i < ini->count; ++i)
        if (!strcasecmp(ini->entries[i].section, section) && !strcasecmp(ini->entries[i].key, key)) return ini->entries[i].value;
    return NULL;
}

int enb_ini_bool(const EnbIni *ini, const char *section, const char *key, int def) {
    const char *v = enb_ini_get(ini, section, key);
    if (!v) return def;
    if (!strcasecmp(v, "true") || !strcasecmp(v, "1") || !strcasecmp(v, "yes") || !strcasecmp(v, "on")) return 1;
    if (!strcasecmp(v, "false") || !strcasecmp(v, "0") || !strcasecmp(v, "no") || !strcasecmp(v, "off")) return 0;
    return def;
}

float enb_ini_float(const EnbIni *ini, const char *section, const char *key, float def) {
    const char *v = enb_ini_get(ini, section, key);
    if (!v || !*v) return def;
    char *end;
    errno = 0;
    const double d = strtod(v, &end);
    while (*end == ' ' || *end == '\t') ++end;
    return (*end || errno || !isfinite(d) || fabs(d) > 1e9) ? def : (float)d;
}

long enb_ini_int(const EnbIni *ini, const char *section, const char *key, long def) {
    const char *v = enb_ini_get(ini, section, key);
    if (!v || !*v) return def;
    char *end;
    errno = 0;
    const long n = strtol(v, &end, 10);
    while (*end == ' ' || *end == '\t') ++end;
    return (*end || errno) ? def : n;
}

int enb_ini_read_file(const char *path, EnbIni *out) {
    if (!path || !out) return EINVAL;
    *out = (EnbIni){0};
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return EIO; }
    size_t want = st.st_size > ENB_INI_MAX_BYTES ? ENB_INI_MAX_BYTES : (size_t)st.st_size;
    char *buffer = malloc(want ? want : 1);
    if (!buffer) { close(fd); return ENOMEM; }
    size_t have = 0;
    while (have < want) {
        const ssize_t got = read(fd, buffer + have, want - have);
        if (got <= 0) break;
        have += (size_t)got;
    }
    close(fd);
    if (have != want) { free(buffer); return EIO; }
    const int rc = enb_ini_parse(buffer, have, out);
    if (!rc && st.st_size > ENB_INI_MAX_BYTES) out->issues |= ENB_INI_ISSUE_TOO_LARGE;
    free(buffer);
    return rc;
}
