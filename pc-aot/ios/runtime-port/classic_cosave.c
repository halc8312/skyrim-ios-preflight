/* SPDX-License-Identifier: GPL-3.0-only */
#include "classic_cosave.h"
#include "mod_analyze.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

enum { HEADER = 20, BLOCK = 12 };
typedef struct {
    unsigned char *bytes;
    size_t size, first, end;
    uint32_t plugins, records, owned_records;
} Snapshot;

static int cosave_path(const char *path) {
    if (!path) return 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *extension = strrchr(base, '.');
    return extension && extension > base && !strcasecmp(extension, ".skse");
}

static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(unsigned char *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i));
}
static int order_uid(const void *lhs, const void *rhs) {
    uint32_t a = *(const uint32_t *)lhs, b = *(const uint32_t *)rhs;
    return (a > b) - (a < b);
}

static SkyrimClassicResult scan_sections(Snapshot *s, uint32_t uid, uint32_t *ids) {
    size_t pos = HEADER;
    s->first = s->end = s->size;
    for (uint32_t i = 0; i < s->plugins; ++i) {
        if (s->size - pos < BLOCK) return SKYRIM_CLASSIC_TRUNCATED;
        size_t start = pos;
        ids[i] = u32(s->bytes + pos);
        uint32_t count = u32(s->bytes + pos + 4), length = u32(s->bytes + pos + 8);
        pos += BLOCK;
        if (length > s->size - pos) return SKYRIM_CLASSIC_TRUNCATED;
        if (count > length / BLOCK) return SKYRIM_CLASSIC_MALFORMED;
        if (count > SKYRIM_COSAVE_HEADER_LIMIT - s->plugins - s->records) return SKYRIM_CLASSIC_LIMIT;
        size_t end = pos + length;
        for (uint32_t j = 0; j < count; ++j) {
            if (end - pos < BLOCK) return SKYRIM_CLASSIC_MALFORMED;
            uint32_t payload = u32(s->bytes + pos + 8);
            pos += BLOCK;
            if (payload > end - pos) return SKYRIM_CLASSIC_MALFORMED;
            pos += payload;
        }
        if (pos != end) return SKYRIM_CLASSIC_MALFORMED;
        s->records += count;
        if (ids[i] == uid) { s->first = start; s->end = end; s->owned_records = count; }
    }
    if (pos != s->size) return SKYRIM_CLASSIC_MALFORMED;
    if (s->plugins > 1) {
        qsort(ids, s->plugins, sizeof *ids, order_uid);
        for (uint32_t i = 1; i < s->plugins; ++i)
            if (ids[i] == ids[i - 1]) return SKYRIM_CLASSIC_DUPLICATE_UID;
    }
    return SKYRIM_CLASSIC_OK;
}

static SkyrimClassicResult validate(Snapshot *s, uint32_t uid) {
    if (s->size < HEADER) return SKYRIM_CLASSIC_TRUNCATED;
    if (memcmp(s->bytes, "SKSE", 4) || !u32(s->bytes + 4)) return SKYRIM_CLASSIC_MALFORMED;
    if (u32(s->bytes + 4) != 1 || u32(s->bytes + 12) != 0x09200000u) return SKYRIM_CLASSIC_UNSUPPORTED;
    s->plugins = u32(s->bytes + 16);
    if (s->plugins > (s->size - HEADER) / BLOCK) return SKYRIM_CLASSIC_TRUNCATED;
    if (s->plugins > SKYRIM_COSAVE_HEADER_LIMIT) return SKYRIM_CLASSIC_LIMIT;
    uint32_t *ids = s->plugins ? malloc(sizeof *ids * s->plugins) : NULL;
    if (s->plugins && !ids) return SKYRIM_CLASSIC_IO;
    SkyrimClassicResult result = scan_sections(s, uid, ids);
    free(ids);
    return result;
}

static int same_version(const struct stat *a, const struct stat *b) {
#if defined(__APPLE__)
    struct timespec am = a->st_mtimespec, bm = b->st_mtimespec, ac = a->st_ctimespec, bc = b->st_ctimespec;
#else
    struct timespec am = a->st_mtim, bm = b->st_mtim, ac = a->st_ctim, bc = b->st_ctim;
#endif
    return a->st_size == b->st_size && am.tv_sec == bm.tv_sec && am.tv_nsec == bm.tv_nsec &&
           ac.tv_sec == bc.tv_sec && ac.tv_nsec == bc.tv_nsec;
}

static SkyrimClassicResult load(Snapshot *s, const char *source, uint32_t uid) {
    if (!source) {
        s->bytes = calloc(1, HEADER);
        if (!s->bytes) return SKYRIM_CLASSIC_IO;
        s->first = s->end = s->size = HEADER;
        memcpy(s->bytes, "SKSE", 4);
        put32(s->bytes + 4, 1);
        put32(s->bytes + 8, 0x01070030u);
        put32(s->bytes + 12, 0x09200000u);
        return SKYRIM_CLASSIC_OK;
    }
    int fd = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? SKYRIM_CLASSIC_MISSING : SKYRIM_CLASSIC_IO;
    SkyrimClassicResult result = SKYRIM_CLASSIC_IO;
    struct stat before, after;
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size < 0) goto done;
    if ((uint64_t)before.st_size > SKYRIM_CLASSIC_COSAVE_MAX_BYTES) { result = SKYRIM_CLASSIC_LIMIT; goto done; }
    if (before.st_size < HEADER) { result = SKYRIM_CLASSIC_TRUNCATED; goto done; }
    s->size = (size_t)before.st_size;
    s->bytes = malloc(s->size);
    if (!s->bytes) goto done;
    for (size_t pos = 0; pos < s->size;) {
        ssize_t n = read(fd, s->bytes + pos, s->size - pos);
        if (n > 0) pos += (size_t)n;
        else if (n == 0 || errno != EINTR) goto done;
    }
    if (fstat(fd, &after) || !same_version(&before, &after)) goto done;
    result = validate(s, uid);
done:
    close(fd);
    return result;
}

SkyrimClassicResult skyrim_classic_cosave_visit(const char *source, uint32_t uid,
                                                SkyrimClassicVisitor visitor, void *context) {
    if (!cosave_path(source) || !visitor) return SKYRIM_CLASSIC_ARGUMENT;
    Snapshot s = {0};
    SkyrimClassicResult result = load(&s, source, uid);
    if (result == SKYRIM_CLASSIC_OK && s.first == s.size) result = SKYRIM_CLASSIC_UID_MISSING;
    if (result == SKYRIM_CLASSIC_OK) {
        size_t pos = s.first + BLOCK;
        for (uint32_t i = 0; i < s.owned_records; ++i) {
            const unsigned char *p = s.bytes + pos;
            SkyrimClassicRecord r = {u32(p), u32(p + 4), u32(p + 8), p + BLOCK};
            if (!visitor(context, &r)) { result = SKYRIM_CLASSIC_STOPPED; break; }
            pos += BLOCK + r.length;
        }
    }
    free(s.bytes);
    return result;
}

static int write_all(int fd, const void *data, size_t n) {
    const unsigned char *p = data;
    while (n) {
        ssize_t written = write(fd, p, n);
        if (written > 0) { p += written; n -= (size_t)written; }
        else if (written == 0 || errno != EINTR) return 0;
    }
    return 1;
}
static int write_block(int fd, uint32_t id, uint32_t version_or_count, uint32_t length) {
    unsigned char block[BLOCK];
    put32(block, id); put32(block + 4, version_or_count); put32(block + 8, length);
    return write_all(fd, block, sizeof block);
}

static SkyrimClassicResult publish(const char *destination, const Snapshot *s, uint32_t uid,
                                   const SkyrimClassicRecord *records, size_t count, size_t section_size) {
    char temporary[PATH_MAX];
    const char *base = strrchr(destination, '/');
    size_t parent = base ? (size_t)(base - destination + 1) : 0;
    if (!destination[parent]) return SKYRIM_CLASSIC_ARGUMENT;
    static const char pattern[] = ".classic-cosave-XXXXXX";
    if (parent + sizeof pattern > sizeof temporary) return SKYRIM_CLASSIC_ARGUMENT;
    memcpy(temporary, destination, parent);
    memcpy(temporary + parent, pattern, sizeof pattern);
    int fd = mkstemp(temporary);
    if (fd < 0) return SKYRIM_CLASSIC_IO;
    unsigned char header[HEADER];
    memcpy(header, s->bytes, HEADER);
    put32(header + 16, s->plugins - (s->first < s->size) + (count != 0));
    int ok = write_all(fd, header, HEADER) && write_all(fd, s->bytes + HEADER, s->first - HEADER);
    if (ok && count) {
        ok = write_block(fd, uid, (uint32_t)count, (uint32_t)(section_size - BLOCK));
        for (size_t i = 0; ok && i < count; ++i)
            ok = write_block(fd, records[i].type, records[i].version, records[i].length) &&
                 write_all(fd, records[i].data, records[i].length);
    }
    if (ok) ok = write_all(fd, s->bytes + s->end, s->size - s->end) && !fsync(fd);
    if (close(fd)) ok = 0;
    SkyrimClassicResult result = SKYRIM_CLASSIC_IO;
    /* link creates a complete destination or fails atomically if someone
     * else has published that name. Both names live on the same volume. */
    if (ok) {
        if (!link(temporary, destination)) result = SKYRIM_CLASSIC_OK;
        else if (errno == EEXIST) result = SKYRIM_CLASSIC_EXISTS;
    }
    unlink(temporary);
    return result;
}

SkyrimClassicResult skyrim_classic_cosave_replace(const char *source, const char *destination,
                                                  uint32_t uid, const SkyrimClassicRecord *records,
                                                  size_t count) {
    if (!uid || !cosave_path(destination) || (source && !cosave_path(source)) || (count && !records))
        return SKYRIM_CLASSIC_ARGUMENT;
    if (count > SKYRIM_COSAVE_HEADER_LIMIT - 1) return SKYRIM_CLASSIC_LIMIT;
    size_t section = count ? BLOCK : 0;
    for (size_t i = 0; i < count; ++i) {
        if (records[i].length && !records[i].data) return SKYRIM_CLASSIC_ARGUMENT;
        if (section + BLOCK + (uint64_t)records[i].length > SKYRIM_CLASSIC_COSAVE_MAX_BYTES) return SKYRIM_CLASSIC_LIMIT;
        section += BLOCK + records[i].length;
    }
    struct stat existing;
    if (!lstat(destination, &existing)) return SKYRIM_CLASSIC_EXISTS;
    if (errno != ENOENT) return SKYRIM_CLASSIC_IO;
    Snapshot s = {0};
    SkyrimClassicResult result = load(&s, source, uid);
    if (result == SKYRIM_CLASSIC_OK) {
        size_t headers = s.plugins + s.records - (s.first < s.size) - s.owned_records + (count != 0) + count;
        if (s.size - (s.end - s.first) + section > SKYRIM_CLASSIC_COSAVE_MAX_BYTES || headers > SKYRIM_COSAVE_HEADER_LIMIT)
            result = SKYRIM_CLASSIC_LIMIT;
        else result = publish(destination, &s, uid, records, count, section);
    }
    free(s.bytes);
    return result;
}

const char *skyrim_classic_cosave_result(SkyrimClassicResult result) {
    static const char *const names[] = {"ok", "invalid argument", "source missing", "UID missing",
        "destination exists", "unsupported", "truncated", "malformed", "duplicate UID", "limit",
        "I/O error", "stopped"};
    return (unsigned)result < sizeof names / sizeof *names ? names[result] : "unknown result";
}
