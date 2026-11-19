/* SPDX-License-Identifier: GPL-3.0-only
 * cc -std=gnu11 -Iios/runtime-port ios/runtime-port/classic_cosave.c \
 *    scripts/classic_cosave_tool.c -o classic-cosave
 */
#include "classic_cosave.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int number(const char *s, uint32_t *out) {
    if (!*s || *s == '-' || *s == '+') return 0;
    errno = 0;
    char *end;
    int base = s[0] == '0' && (s[1] == 'x' || s[1] == 'X') ? 16 : 10;
    uintmax_t n = strtoumax(s, &end, base);
    if (errno || *end || end == s || n > UINT32_MAX) return 0;
    *out = (uint32_t)n;
    return 1;
}
static int show(void *context, const SkyrimClassicRecord *r) {
    (void)context;
    return printf("0x%08" PRIx32 " %" PRIu32 " %" PRIu32 "\n", r->type, r->version, r->length) >= 0;
}
static SkyrimClassicResult payload(const char *name, size_t *remaining, SkyrimClassicRecord *r) {
    int fd = open(name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return SKYRIM_CLASSIC_IO;
    struct stat st;
    SkyrimClassicResult result = SKYRIM_CLASSIC_IO;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) goto done;
    if ((uint64_t)st.st_size > *remaining) { result = SKYRIM_CLASSIC_LIMIT; goto done; }
    r->length = (uint32_t)st.st_size;
    unsigned char *data = r->length ? malloc(r->length) : NULL;
    if (r->length && !data) goto done;
    r->data = data;
    for (size_t pos = 0; pos < r->length;) {
        ssize_t n = read(fd, data + pos, r->length - pos);
        if (n > 0) pos += (size_t)n;
        else if (n == 0 || errno != EINTR) goto done;
    }
    *remaining -= r->length;
    result = SKYRIM_CLASSIC_OK;
done:
    close(fd);
    return result;
}
static SkyrimClassicResult replace(int argc, char **argv) {
    uint32_t uid;
    if (!number(argv[4], &uid) || !uid) return SKYRIM_CLASSIC_ARGUMENT;
    size_t count = (size_t)(argc - 5) / 3;
    SkyrimClassicRecord *records = count ? calloc(count, sizeof *records) : NULL;
    if (count && !records) return SKYRIM_CLASSIC_IO;
    size_t remaining = SKYRIM_CLASSIC_COSAVE_MAX_BYTES;
    SkyrimClassicResult result = SKYRIM_CLASSIC_OK;
    for (size_t i = 0; i < count && result == SKYRIM_CLASSIC_OK; ++i) {
        char **p = argv + 5 + 3 * i;
        if (!number(p[0], &records[i].type) || !number(p[1], &records[i].version))
            result = SKYRIM_CLASSIC_ARGUMENT;
        else result = payload(p[2], &remaining, &records[i]);
    }
    if (result == SKYRIM_CLASSIC_OK)
        result = skyrim_classic_cosave_replace(!strcmp(argv[2], "-") ? NULL : argv[2], argv[3], uid, records, count);
    for (size_t i = 0; i < count; ++i) free((void *)records[i].data);
    free(records);
    return result;
}
int main(int argc, char **argv) {
    SkyrimClassicResult result;
    uint32_t uid;
    if (argc == 4 && !strcmp(argv[1], "list") && number(argv[3], &uid))
        result = skyrim_classic_cosave_visit(argv[2], uid, show, NULL);
    else if (argc >= 5 && !strcmp(argv[1], "replace") && (argc - 5) % 3 == 0)
        result = replace(argc, argv);
    else {
        fputs("Usage: classic-cosave list FILE UID\n"
              "       classic-cosave replace SOURCE|- NEW_FILE UID [TYPE VERSION PAYLOAD_FILE]...\n", stderr);
        return 2;
    }
    if (result != SKYRIM_CLASSIC_OK) fprintf(stderr, "%s\n", skyrim_classic_cosave_result(result));
    return result == SKYRIM_CLASSIC_OK ? 0 : 1;
}
