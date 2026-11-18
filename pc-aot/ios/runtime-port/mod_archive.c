/* SPDX-License-Identifier: GPL-3.0-only
 * ZIP reader for MOD import: stored/deflate, ZIP64, strict path rules. */
#include "mod_archive.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const unsigned char *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static uint64_t rd64(const unsigned char *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static int read_at(int fd, uint64_t offset, void *buffer, size_t size) {
    unsigned char *p = buffer;
    while (size) {
        ssize_t n = pread(fd, p, size, (off_t)offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; offset += (uint64_t)n; size -= (size_t)n;
    }
    return 0;
}

const char *skyrim_zip_error_name(int status) {
    static const char *const names[] = {"ok", "io", "not-zip", "corrupt", "too-large", "unsafe-path",
        "duplicate", "encrypted", "unsupported-method", "link", "write", "crc", "no-memory"};
    return status >= 0 && status < (int)(sizeof names / sizeof names[0]) ? names[status] : "unknown";
}

static int valid_utf8(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i];
        size_t extra = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 9;
        if (extra == 9 || (extra == 1 && c < 0xc2) || i + extra >= n + (extra ? 0 : 1)) return 0;
        for (size_t k = 1; k <= extra; ++k) if ((s[i + k] & 0xc0) != 0x80) return 0;
        i += extra + 1;
    }
    return 1;
}

int skyrim_zip_normalize(const char *name, size_t length, int utf8_flag, char *out, size_t capacity) {
    if (!name || !out || capacity < 2) return -1;
    int ascii = 1;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f) return -1;
        if (c >= 0x80) ascii = 0;
    }
    if (!ascii && (!utf8_flag || !valid_utf8((const unsigned char *)name, length))) return 0;
    if (length && (name[0] == '/' || name[0] == '\\')) return -1;
    if (length >= 2 && name[1] == ':') return -1;
    size_t o = 0, i = 0;
    while (i < length) {
        size_t start = i;
        while (i < length && name[i] != '/' && name[i] != '\\') ++i;
        size_t n = i - start;
        if (i < length) ++i;
        if (!n || (n == 1 && name[start] == '.')) continue;
        if (n == 2 && name[start] == '.' && name[start + 1] == '.') return -1;
        if (name[start + n - 1] == ' ' || name[start + n - 1] == '.') return -1; /* Windows-trimmed aliases */
        for (size_t k = start; k < start + n; ++k)
            if (strchr(":*?\"<>|", name[k])) return -1;
        if (o + n + 2 > capacity) return -1;
        if (o) out[o++] = '/';
        memcpy(out + o, name + start, n); o += n;
    }
    out[o] = 0;
    return o ? 1 : 0;
}

static int casecmp_paths(const void *a, const void *b) {
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

void skyrim_zip_close(SkyrimZip *zip) {
    if (!zip) return;
    if (zip->entries) for (size_t i = 0; i < zip->count; ++i) free(zip->entries[i].path);
    free(zip->entries);
    if (zip->fd >= 0) close(zip->fd);
    memset(zip, 0, sizeof *zip);
    zip->fd = -1;
}

int skyrim_zip_open(const char *path, SkyrimZip *zip) {
    memset(zip, 0, sizeof *zip);
    zip->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (zip->fd < 0) return SKYRIM_ZIP_IO;
    struct stat st;
    if (fstat(zip->fd, &st) || !S_ISREG(st.st_mode)) { skyrim_zip_close(zip); return SKYRIM_ZIP_IO; }
    zip->file_size = (uint64_t)st.st_size;
    if (zip->file_size < 22) { skyrim_zip_close(zip); return SKYRIM_ZIP_NOT_ZIP; }
    size_t tail = zip->file_size < 65557 ? (size_t)zip->file_size : 65557;
    unsigned char *buf = malloc(tail);
    if (!buf) { skyrim_zip_close(zip); return SKYRIM_ZIP_NO_MEMORY; }
    if (read_at(zip->fd, zip->file_size - tail, buf, tail)) { free(buf); skyrim_zip_close(zip); return SKYRIM_ZIP_IO; }
    long eocd = -1;
    for (long i = (long)tail - 22; i >= 0; --i)
        if (rd32(buf + i) == 0x06054b50u) { eocd = i; break; }
    if (eocd < 0) { free(buf); skyrim_zip_close(zip); return SKYRIM_ZIP_NOT_ZIP; }
    uint64_t count = rd16(buf + eocd + 10), cd_size = rd32(buf + eocd + 12), cd_offset = rd32(buf + eocd + 16);
    if (count == 0xffff || cd_size == 0xffffffffu || cd_offset == 0xffffffffu) {
        if (eocd < 20 || rd32(buf + eocd - 20) != 0x07064b50u) { free(buf); skyrim_zip_close(zip); return SKYRIM_ZIP_CORRUPT; }
        uint64_t z64 = rd64(buf + eocd - 20 + 8);
        unsigned char rec[56];
        if (z64 > zip->file_size - 56 || read_at(zip->fd, z64, rec, sizeof rec) || rd32(rec) != 0x06064b50u) {
            free(buf); skyrim_zip_close(zip); return SKYRIM_ZIP_CORRUPT;
        }
        count = rd64(rec + 32); cd_size = rd64(rec + 40); cd_offset = rd64(rec + 48);
    }
    free(buf);
    if (count > SKYRIM_ZIP_MAX_ENTRIES) { skyrim_zip_close(zip); return SKYRIM_ZIP_TOO_LARGE; }
    if (cd_offset > zip->file_size || cd_size > zip->file_size - cd_offset || cd_size > (256u << 20)) {
        skyrim_zip_close(zip); return SKYRIM_ZIP_CORRUPT;
    }
    unsigned char *cd = malloc(cd_size ? (size_t)cd_size : 1);
    zip->entries = calloc(count ? (size_t)count : 1, sizeof *zip->entries);
    if (!cd || !zip->entries) { free(cd); skyrim_zip_close(zip); return SKYRIM_ZIP_NO_MEMORY; }
    if (read_at(zip->fd, cd_offset, cd, (size_t)cd_size)) { free(cd); skyrim_zip_close(zip); return SKYRIM_ZIP_IO; }
    int status = SKYRIM_ZIP_OK;
    size_t pos = 0;
    char name[1024];
    for (uint64_t k = 0; k < count && status == SKYRIM_ZIP_OK; ++k) {
        if (pos + 46 > cd_size || rd32(cd + pos) != 0x02014b50u) { status = SKYRIM_ZIP_CORRUPT; break; }
        const unsigned char *h = cd + pos;
        uint16_t made_by = rd16(h + 4), flags = rd16(h + 8), method = rd16(h + 10);
        uint32_t crc = rd32(h + 16);
        uint64_t csize = rd32(h + 20), usize = rd32(h + 24), offset = rd32(h + 42);
        size_t nlen = rd16(h + 28), xlen = rd16(h + 30), clen = rd16(h + 32);
        uint32_t external = rd32(h + 38);
        if (pos + 46 + nlen + xlen + clen > cd_size) { status = SKYRIM_ZIP_CORRUPT; break; }
        const unsigned char *x = h + 46 + nlen, *xend = x + xlen;
        while (x + 4 <= xend) {
            uint16_t id = rd16(x), sz = rd16(x + 2);
            const unsigned char *d = x + 4;
            if (d + sz > xend) break;
            if (id == 0x0001) {
                const unsigned char *q = d, *qend = d + sz;
                if (usize == 0xffffffffu && q + 8 <= qend) { usize = rd64(q); q += 8; }
                if (csize == 0xffffffffu && q + 8 <= qend) { csize = rd64(q); q += 8; }
                if (offset == 0xffffffffu && q + 8 <= qend) { offset = rd64(q); q += 8; }
            }
            x = d + sz;
        }
        SkyrimZipEntry *e = &zip->entries[zip->count];
        int norm = skyrim_zip_normalize((const char *)h + 46, nlen, flags & 0x800, name, sizeof name);
        int is_dir = nlen && (h[46 + nlen - 1] == '/' || h[46 + nlen - 1] == '\\');
        if (norm < 0) status = SKYRIM_ZIP_UNSAFE_PATH;
        else if ((made_by >> 8) == 3 && ((external >> 16) & 0170000) == 0120000) status = SKYRIM_ZIP_LINK;
        else if (!is_dir && (flags & 1)) status = SKYRIM_ZIP_ENCRYPTED;
        else if (!is_dir && method != 0 && method != 8) status = SKYRIM_ZIP_UNSUPPORTED;
        else if (offset >= zip->file_size || (!is_dir && csize > zip->file_size)) status = SKYRIM_ZIP_CORRUPT;
        else if (norm == 0) { ++zip->skipped; }
        else {
            e->path = strdup(name);
            if (!e->path) { status = SKYRIM_ZIP_NO_MEMORY; break; }
            e->size = usize; e->compressed = csize; e->local_offset = offset;
            e->crc = crc; e->method = method; e->is_dir = (unsigned char)is_dir;
            if (!is_dir) {
                zip->total += usize;
                if (usize > SKYRIM_ZIP_MAX_TOTAL || zip->total > SKYRIM_ZIP_MAX_TOTAL) status = SKYRIM_ZIP_TOO_LARGE;
            }
            ++zip->count;
        }
        pos += 46 + nlen + xlen + clen;
    }
    free(cd);
    if (status == SKYRIM_ZIP_OK) {
        char **files = malloc((zip->count ? zip->count : 1) * sizeof *files);
        size_t n = 0;
        if (!files) status = SKYRIM_ZIP_NO_MEMORY;
        else {
            for (size_t i = 0; i < zip->count; ++i) if (!zip->entries[i].is_dir) files[n++] = zip->entries[i].path;
            qsort(files, n, sizeof *files, casecmp_paths);
            for (size_t i = 1; i < n && status == SKYRIM_ZIP_OK; ++i)
                if (!strcasecmp(files[i - 1], files[i])) status = SKYRIM_ZIP_DUPLICATE;
            free(files);
        }
    }
    if (status != SKYRIM_ZIP_OK) skyrim_zip_close(zip);
    return status;
}

int skyrim_make_parents(const char *root, const char *relative) {
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/%s", root, relative);
    if (n < 0 || (size_t)n >= sizeof path) return -1;
    char *slash = path + strlen(root) + 1;
    while ((slash = strchr(slash, '/'))) {
        *slash = 0;
        if (mkdir(path, 0755) && errno != EEXIST) return -1;
        struct stat st;
        if (lstat(path, &st) || !S_ISDIR(st.st_mode)) return -1;
        *slash++ = '/';
    }
    return 0;
}

int skyrim_zip_extract(SkyrimZip *zip, size_t index, const char *dest_root, const char *relative) {
    if (!zip || index >= zip->count || zip->entries[index].is_dir) return SKYRIM_ZIP_CORRUPT;
    const SkyrimZipEntry *e = &zip->entries[index];
    unsigned char local[30];
    if (read_at(zip->fd, e->local_offset, local, sizeof local) || rd32(local) != 0x04034b50u) return SKYRIM_ZIP_CORRUPT;
    uint64_t data = e->local_offset + 30 + rd16(local + 26) + rd16(local + 28);
    if (data > zip->file_size || e->compressed > zip->file_size - data) return SKYRIM_ZIP_CORRUPT;
    if (skyrim_make_parents(dest_root, relative)) return SKYRIM_ZIP_WRITE;
    char out_path[4096];
    int pn = snprintf(out_path, sizeof out_path, "%s/%s", dest_root, relative);
    if (pn < 0 || (size_t)pn >= sizeof out_path) return SKYRIM_ZIP_WRITE;
    int out = open(out_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (out < 0) return SKYRIM_ZIP_WRITE;
    enum { CHUNK = 1 << 18 };
    unsigned char *in = malloc(CHUNK), *buf = malloc(CHUNK);
    int status = in && buf ? SKYRIM_ZIP_OK : SKYRIM_ZIP_NO_MEMORY;
    z_stream z; memset(&z, 0, sizeof z);
    int inflating = 0;
    if (status == SKYRIM_ZIP_OK && e->method == 8) {
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK) status = SKYRIM_ZIP_NO_MEMORY; else inflating = 1;
    }
    uint64_t remaining = e->compressed, offset = data, written = 0;
    uLong crc = crc32(0, Z_NULL, 0);
    int done = 0;
    while (status == SKYRIM_ZIP_OK && !done) {
        size_t want = remaining < CHUNK ? (size_t)remaining : CHUNK;
        if (want && read_at(zip->fd, offset, in, want)) { status = SKYRIM_ZIP_IO; break; }
        offset += want; remaining -= want;
        if (e->method == 0) {
            if (written + want > e->size) { status = SKYRIM_ZIP_CORRUPT; break; }
            if (want && write(out, in, want) != (ssize_t)want) { status = SKYRIM_ZIP_WRITE; break; }
            crc = crc32(crc, in, (uInt)want); written += want;
            done = remaining == 0;
            continue;
        }
        z.next_in = in; z.avail_in = (uInt)want;
        do {
            z.next_out = buf; z.avail_out = CHUNK;
            int r = inflate(&z, Z_NO_FLUSH);
            if (r != Z_OK && r != Z_STREAM_END && !(r == Z_BUF_ERROR && !want)) { status = SKYRIM_ZIP_CORRUPT; break; }
            size_t got = CHUNK - z.avail_out;
            if (written + got > e->size) { status = SKYRIM_ZIP_CORRUPT; break; }
            if (got && write(out, buf, got) != (ssize_t)got) { status = SKYRIM_ZIP_WRITE; break; }
            crc = crc32(crc, buf, (uInt)got); written += got;
            if (r == Z_STREAM_END) { done = 1; break; }
            if (r == Z_BUF_ERROR) { status = SKYRIM_ZIP_CORRUPT; break; }
        } while (z.avail_out == 0 || z.avail_in);
        if (!done && !remaining && status == SKYRIM_ZIP_OK) status = SKYRIM_ZIP_CORRUPT;
    }
    if (inflating) inflateEnd(&z);
    free(in); free(buf);
    if (status == SKYRIM_ZIP_OK && written != e->size) status = SKYRIM_ZIP_CORRUPT;
    if (status == SKYRIM_ZIP_OK && (uint32_t)crc != e->crc) status = SKYRIM_ZIP_CRC;
    if (close(out) && status == SKYRIM_ZIP_OK) status = SKYRIM_ZIP_WRITE;
    if (status != SKYRIM_ZIP_OK) unlink(out_path);
    return status;
}
