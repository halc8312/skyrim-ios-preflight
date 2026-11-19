/* SPDX-License-Identifier: GPL-3.0-only */
#include "serialization_files.h"
#include <stddef.h>

static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(unsigned char *p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (i * 8));
}

#ifndef _WIN32
#include "../../ios/runtime-port/classic_cosave.h"

int fixture_store_record(const char *previous, const char *destination, uint32_t value) {
    unsigned char bytes[4];
    put32(bytes, value);
    SkyrimClassicRecord r = {FIXTURE_SERIAL_TYPE, FIXTURE_SERIAL_VERSION, sizeof bytes, bytes};
    return skyrim_classic_cosave_replace(previous, destination, FIXTURE_SERIAL_UID, &r, 1) == SKYRIM_CLASSIC_OK;
}
typedef struct { unsigned count; uint32_t value; } Value;
static int read_value(void *context, const SkyrimClassicRecord *r) {
    Value *v = context;
    if (v->count || r->type != FIXTURE_SERIAL_TYPE || r->version != FIXTURE_SERIAL_VERSION || r->length != 4) return 0;
    v->value = get32(r->data);
    ++v->count;
    return 1;
}
int fixture_load_record(const char *source, uint32_t *value) {
    Value v = {0};
    if (!value || skyrim_classic_cosave_visit(source, FIXTURE_SERIAL_UID, read_value, &v) != SKYRIM_CLASSIC_OK || v.count != 1)
        return 0;
    *value = v.value;
    return 1;
}

#else
/* Independent reference for the small synthetic Windows fixtures. The app
 * uses the production owner-scoped reader/writer above. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HEADER = 20, BLOCK = 12, OWN_SIZE = 2 * BLOCK + 4, MAX_INPUT = 1024 * 1024, MAX_UIDS = 256 };
typedef struct {
    unsigned char *bytes;
    size_t size, first, end;
    uint32_t plugins;
} FixtureFile;

static int cosave_name(const char *path) {
    if (!path || !*path) return 0;
    const char *base = path;
    for (const char *p = path; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    const char *extension = strrchr(base, '.');
    return extension && extension > base && !_stricmp(extension, ".skse");
}
static int parse(FixtureFile *f) {
    const unsigned char *b = f->bytes;
    if (f->size < HEADER || memcmp(b, "SKSE", 4) || get32(b + 4) != 1 || get32(b + 12) != 0x09200000u) return 0;
    f->plugins = get32(b + 16);
    if (f->plugins > MAX_UIDS) return 0;
    uint32_t seen[MAX_UIDS];
    size_t pos = HEADER;
    f->first = f->end = f->size;
    for (uint32_t i = 0; i < f->plugins; ++i) {
        if (f->size - pos < BLOCK) return 0;
        size_t start = pos;
        seen[i] = get32(b + pos);
        for (uint32_t j = 0; j < i; ++j) if (seen[j] == seen[i]) return 0;
        uint32_t count = get32(b + pos + 4), length = get32(b + pos + 8);
        pos += BLOCK;
        if (length > f->size - pos || count > length / BLOCK) return 0;
        size_t end = pos + length;
        for (uint32_t j = 0; j < count; ++j) {
            if (end - pos < BLOCK) return 0;
            uint32_t bytes = get32(b + pos + 8);
            pos += BLOCK;
            if (bytes > end - pos) return 0;
            pos += bytes;
        }
        if (pos != end) return 0;
        if (seen[i] == FIXTURE_SERIAL_UID) { f->first = start; f->end = end; }
    }
    return pos == f->size;
}
static int load(const char *name, FixtureFile *f) {
    if (!name) {
        f->bytes = calloc(1, HEADER);
        if (!f->bytes) return 0;
        f->size = HEADER;
        memcpy(f->bytes, "SKSE", 4);
        put32(f->bytes + 4, 1);
        put32(f->bytes + 8, 0x01070030u);
        put32(f->bytes + 12, 0x09200000u);
        return parse(f);
    }
    if (!cosave_name(name)) return 0;
    HANDLE file = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION st;
    DWORD read_bytes = 0;
    int ok = GetFileInformationByHandle(file, &st) && GetFileType(file) == FILE_TYPE_DISK &&
             !(st.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
             !st.nFileSizeHigh && st.nFileSizeLow >= HEADER && st.nFileSizeLow <= MAX_INPUT;
    if (ok) {
        f->size = st.nFileSizeLow;
        f->bytes = malloc(f->size);
        ok = f->bytes && ReadFile(file, f->bytes, (DWORD)f->size, &read_bytes, NULL) && read_bytes == f->size;
    }
    CloseHandle(file);
    return ok && parse(f);
}
static int publish(const char *destination, const unsigned char *bytes, size_t size) {
    char temporary[MAX_PATH];
    static LONG sequence;
    int n = snprintf(temporary, sizeof temporary, "%s.fixture-%lu-%ld.tmp", destination,
                     GetCurrentProcessId(), InterlockedIncrement(&sequence));
    if (n < 0 || (size_t)n >= sizeof temporary) return 0;
    HANDLE file = CreateFileA(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    int ok = WriteFile(file, bytes, (DWORD)size, &written, NULL) && written == size && FlushFileBuffers(file);
    if (!CloseHandle(file)) ok = 0;
    if (ok && MoveFileExA(temporary, destination, MOVEFILE_WRITE_THROUGH)) return 1;
    DeleteFileA(temporary);
    return 0;
}
int fixture_store_record(const char *previous, const char *destination, uint32_t value) {
    if (!cosave_name(destination)) return 0;
    FixtureFile f = {0};
    int ok = load(previous, &f);
    if (ok) {
        size_t size = f.size - (f.end - f.first) + OWN_SIZE;
        uint32_t plugins = f.plugins + (f.first == f.size);
        unsigned char *out = size <= MAX_INPUT && plugins <= MAX_UIDS ? malloc(size) : NULL;
        ok = 0;
        if (out) {
            memcpy(out, f.bytes, f.first);
            put32(out + 16, plugins);
            unsigned char *owner = out + f.first;
            put32(owner, FIXTURE_SERIAL_UID); put32(owner + 4, 1); put32(owner + 8, BLOCK + 4);
            put32(owner + 12, FIXTURE_SERIAL_TYPE); put32(owner + 16, FIXTURE_SERIAL_VERSION);
            put32(owner + 20, 4); put32(owner + 24, value);
            memcpy(owner + OWN_SIZE, f.bytes + f.end, f.size - f.end);
            ok = publish(destination, out, size);
            free(out);
        }
    }
    free(f.bytes);
    return ok;
}
int fixture_load_record(const char *source, uint32_t *value) {
    if (!source || !value) return 0;
    FixtureFile f = {0};
    int ok = load(source, &f);
    if (ok) {
        const unsigned char *p = f.bytes + f.first;
        ok = f.end - f.first == OWN_SIZE && get32(p + 4) == 1 &&
             get32(p + 12) == FIXTURE_SERIAL_TYPE && get32(p + 16) == FIXTURE_SERIAL_VERSION && get32(p + 20) == 4;
        if (ok) *value = get32(p + 24);
    }
    free(f.bytes);
    return ok;
}
#endif
