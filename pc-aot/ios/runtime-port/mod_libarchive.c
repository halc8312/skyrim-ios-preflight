/* SPDX-License-Identifier: GPL-3.0-only
 * 7z / RAR (and other libarchive formats) for MOD import, with the same path
 * rules and limits as the zip reader. Uses libarchive (BSD-2-Clause). */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "mod_libarchive.h"
#include "mod_archive.h"
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static struct archive *open_reader(const char *path) {
    struct archive *a = archive_read_new();
    if (!a) return NULL;
    archive_read_support_format_7zip(a);
    archive_read_support_format_rar(a);
    archive_read_support_format_rar5(a);
    archive_read_support_format_zip(a);
    archive_read_support_format_tar(a);
    archive_read_support_filter_all(a);
    if (archive_read_open_filename(a, path, 1 << 16) != ARCHIVE_OK) { archive_read_free(a); return NULL; }
    return a;
}

static int entry_path(struct archive_entry *e, char *out, size_t cap) {
    const char *name = archive_entry_pathname_utf8(e);
    if (!name) name = archive_entry_pathname(e);
    if (!name) return 0;
    return skyrim_zip_normalize(name, strlen(name), 1, out, cap);
}

static int casecmp_paths(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

int skyrim_archive_list(const char *path, SkyrimArchiveList *list) {
    memset(list, 0, sizeof *list);
    struct archive *a = open_reader(path);
    if (!a) return SKYRIM_ZIP_NOT_ZIP;
    struct archive_entry *e;
    int status = SKYRIM_ZIP_OK, r;
    size_t cap = 0;
    char name[1024];
    while (status == SKYRIM_ZIP_OK && (r = archive_read_next_header(a, &e)) == ARCHIVE_OK) {
        mode_t type = archive_entry_filetype(e);
        if (type == AE_IFDIR) continue;
        if (type == AE_IFLNK || archive_entry_hardlink(e)) { status = SKYRIM_ZIP_LINK; break; }
        if (type != AE_IFREG) continue;
        if (archive_entry_is_encrypted(e)) { status = SKYRIM_ZIP_ENCRYPTED; break; }
        int norm = entry_path(e, name, sizeof name);
        if (norm < 0) { status = SKYRIM_ZIP_UNSAFE_PATH; break; }
        if (!norm) { ++list->skipped; continue; }
        int64_t size = archive_entry_size_is_set(e) ? archive_entry_size(e) : 0;
        if (size < 0) size = 0;
        list->total += (uint64_t)size;
        if (list->count >= SKYRIM_ZIP_MAX_ENTRIES || list->total > SKYRIM_ZIP_MAX_TOTAL) { status = SKYRIM_ZIP_TOO_LARGE; break; }
        if (list->count == cap) {
            cap = cap ? cap * 2 : 256;
            char **grown = realloc(list->paths, cap * sizeof *grown);
            if (!grown) { status = SKYRIM_ZIP_NO_MEMORY; break; }
            list->paths = grown;
        }
        if (!(list->paths[list->count] = strdup(name))) { status = SKYRIM_ZIP_NO_MEMORY; break; }
        ++list->count;
    }
    if (status == SKYRIM_ZIP_OK && r != ARCHIVE_EOF) status = SKYRIM_ZIP_CORRUPT;
    archive_read_free(a);
    if (status == SKYRIM_ZIP_OK && list->count > 1) {
        char **sorted = malloc(list->count * sizeof *sorted);
        if (!sorted) status = SKYRIM_ZIP_NO_MEMORY;
        else {
            memcpy(sorted, list->paths, list->count * sizeof *sorted);
            qsort(sorted, list->count, sizeof *sorted, casecmp_paths);
            for (size_t i = 1; i < list->count && status == SKYRIM_ZIP_OK; ++i)
                if (!strcasecmp(sorted[i - 1], sorted[i])) status = SKYRIM_ZIP_DUPLICATE;
            free(sorted);
        }
    }
    if (status != SKYRIM_ZIP_OK) skyrim_archive_list_free(list);
    return status;
}

void skyrim_archive_list_free(SkyrimArchiveList *list) {
    for (size_t i = 0; i < list->count; ++i) free(list->paths[i]);
    free(list->paths);
    memset(list, 0, sizeof *list);
}

int skyrim_archive_extract(const char *path, const char *prefix, int skip_fomod, const char *dest_root,
                           size_t *files_out, uint64_t *bytes_out) {
    *files_out = 0; *bytes_out = 0;
    struct archive *a = open_reader(path);
    if (!a) return SKYRIM_ZIP_NOT_ZIP;
    size_t plen = prefix ? strlen(prefix) : 0;
    struct archive_entry *e;
    int status = SKYRIM_ZIP_OK, r;
    char name[1024], out[4096];
    void *buf = malloc(1 << 18);
    if (!buf) { archive_read_free(a); return SKYRIM_ZIP_NO_MEMORY; }
    while (status == SKYRIM_ZIP_OK && (r = archive_read_next_header(a, &e)) == ARCHIVE_OK) {
        if (archive_entry_filetype(e) != AE_IFREG || archive_entry_hardlink(e)) continue;
        if (archive_entry_is_encrypted(e)) { status = SKYRIM_ZIP_ENCRYPTED; break; }
        int norm = entry_path(e, name, sizeof name);
        if (norm < 0) { status = SKYRIM_ZIP_UNSAFE_PATH; break; }
        if (!norm || strncmp(name, prefix ? prefix : "", plen)) continue;
        const char *rel = name + plen;
        if (!*rel || (skip_fomod && !strncasecmp(rel, "fomod/", 6))) continue;
        if (skyrim_make_parents(dest_root, rel)) { status = SKYRIM_ZIP_WRITE; break; }
        if ((size_t)snprintf(out, sizeof out, "%s/%s", dest_root, rel) >= sizeof out) { status = SKYRIM_ZIP_WRITE; break; }
        int fd = open(out, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd < 0) { status = SKYRIM_ZIP_WRITE; break; }
        la_ssize_t n;
        while ((n = archive_read_data(a, buf, 1 << 18)) > 0) {
            if (write(fd, buf, (size_t)n) != n) { status = SKYRIM_ZIP_WRITE; break; }
            *bytes_out += (uint64_t)n;
            if (*bytes_out > SKYRIM_ZIP_MAX_TOTAL) { status = SKYRIM_ZIP_TOO_LARGE; break; }
        }
        if (n < 0 && status == SKYRIM_ZIP_OK) status = SKYRIM_ZIP_CORRUPT;
        if (close(fd) && status == SKYRIM_ZIP_OK) status = SKYRIM_ZIP_WRITE;
        if (status != SKYRIM_ZIP_OK) { unlink(out); break; }
        ++*files_out;
    }
    if (status == SKYRIM_ZIP_OK && r != ARCHIVE_EOF) status = SKYRIM_ZIP_CORRUPT;
    free(buf);
    archive_read_free(a);
    return status;
}
