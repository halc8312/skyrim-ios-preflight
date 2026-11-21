/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ZIP reading for MOD import. Pure C + zlib. Every name is normalized before
 * use: '\' becomes '/', and absolute paths, drive letters, "..", control
 * characters, symlinks, encrypted entries and unsupported methods are refused
 * or skipped explicitly. Extraction only creates new files under a fresh
 * destination directory chosen by the caller. */

enum { SKYRIM_ZIP_MAX_ENTRIES = 200000 };
#define SKYRIM_ZIP_MAX_TOTAL (64ull << 30)   /* uncompressed bytes per archive */

enum {
    SKYRIM_ZIP_OK = 0,
    SKYRIM_ZIP_IO,              /* cannot open/read the archive */
    SKYRIM_ZIP_NOT_ZIP,
    SKYRIM_ZIP_CORRUPT,
    SKYRIM_ZIP_TOO_LARGE,       /* entry count or total size over the limits */
    SKYRIM_ZIP_UNSAFE_PATH,     /* traversal / absolute / drive path */
    SKYRIM_ZIP_DUPLICATE,       /* two entries differ only by case */
    SKYRIM_ZIP_ENCRYPTED,
    SKYRIM_ZIP_UNSUPPORTED,     /* compression other than stored/deflate */
    SKYRIM_ZIP_LINK,            /* symbolic link entry */
    SKYRIM_ZIP_WRITE,           /* cannot create an output file */
    SKYRIM_ZIP_CRC,
    SKYRIM_ZIP_NO_MEMORY
};

typedef struct {
    char *path;          /* normalized relative path, '/' separated */
    uint64_t size, compressed, local_offset;
    uint32_t crc;
    uint16_t method;
    unsigned char is_dir, skipped;   /* skipped: name not representable */
} SkyrimZipEntry;

typedef struct {
    int fd;
    uint64_t file_size, total;
    size_t count, skipped;
    SkyrimZipEntry *entries;
} SkyrimZip;

int skyrim_zip_open(const char *path, SkyrimZip *zip);
/* Normalize one stored name; 1 ok, 0 skip (empty / non-UTF-8 non-ASCII when
 * utf8_flag is 0), -1 unsafe. out receives at most capacity bytes. */
int skyrim_zip_normalize(const char *name, size_t length, int utf8_flag, char *out, size_t capacity);
/* Select a normalized archive or folder path under the case-insensitive
 * prefix returned by skyrim_mod_data_root. NULL or "" selects any path. */
const char *skyrim_archive_relative_path(const char *path, const char *prefix);
/* Extract entry index to dest_root/relative (relative must already be a
 * normalized path from this archive, optionally with a prefix removed).
 * Parent directories are created; existing files are never replaced. */
int skyrim_zip_extract(SkyrimZip *zip, size_t index, const char *dest_root, const char *relative);
void skyrim_zip_close(SkyrimZip *zip);
const char *skyrim_zip_error_name(int status);

/* Create every missing directory of root/relative's parent (not following
 * symlinks it did not create). 0 on success. Shared with the deploy code. */
int skyrim_make_parents(const char *root, const char *relative);
#ifdef __cplusplus
}
#endif
