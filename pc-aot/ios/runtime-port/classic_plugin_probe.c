/* SPDX-License-Identifier: GPL-3.0-only */
#include "classic_plugin_probe.h"
#include "serialization_session.h"
#include "serialization_files.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

enum { fixture_bytes = 83 };
static pthread_mutex_t guest_lock = PTHREAD_MUTEX_INITIALIZER;

/* The Windows writer independently produces these two Classic plugin sections. */
static unsigned char *put_words(unsigned char *out, const uint32_t *words, size_t count) {
    for (size_t i = 0; i < count; ++i)
        for (unsigned b = 0; b < 4; ++b) *out++ = (unsigned char)(words[i] >> (8 * b));
    return out;
}
static void reference_record(unsigned char out[fixture_bytes], uint32_t value) {
    static const unsigned char opaque[] = {'o','p','a','q','u','e',0,0xff,0x80,1,'!'};
    static const uint32_t headers[] = {1, 0x01070030, 0x09200000, 2,
        0xf3e2d1c0, 1, 12 + sizeof opaque, 0xdeca1234, 9, sizeof opaque};
    const uint32_t record[] = {FIXTURE_SERIAL_UID, 1, 16, FIXTURE_SERIAL_TYPE, FIXTURE_SERIAL_VERSION, 4, value};
    _Static_assert(4 + sizeof headers + sizeof opaque + sizeof record == fixture_bytes, "Windows sample layout");
    memcpy(out, "SKSE", 4);
    unsigned char *section = put_words(out + 4, headers, sizeof headers / sizeof *headers);
    memcpy(section, opaque, sizeof opaque);
    put_words(section + sizeof opaque, record, sizeof record / sizeof *record);
}
static int cosave_name(const char *path) {
    const char *dot = path ? strrchr(path, '.') : NULL;
    return dot && !strcasecmp(dot, ".skse");
}
static int read_reference(const char *path, uint32_t value, unsigned char data[fixture_bytes]) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return 0;
    struct stat info;
    int ok = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size == fixture_bytes;
    size_t offset = 0;
    while (ok && offset < fixture_bytes) {
        ssize_t size = read(fd, data + offset, fixture_bytes - offset);
        if (size < 0 && errno == EINTR) continue;
        if (size <= 0) ok = 0;
        else offset += (size_t)size;
    }
    unsigned char last;
    ssize_t remaining;
    do { remaining = ok ? read(fd, &last, 1) : -1; } while (ok && remaining < 0 && errno == EINTR);
    if (close(fd) || remaining != 0) return 0;
    unsigned char expected[fixture_bytes];
    reference_record(expected, value);
    return !memcmp(data, expected, fixture_bytes);
}

static SkyrimClassicFixtureStatus execute_probe(const char *verified_input, const char *private_output,
                                                SkyrimClassicFixtureResult *result) {
    unsigned input;
    if (!fixture_load_record(verified_input, &input) || input != 16) return SKYRIM_CLASSIC_FIXTURE_INPUT;
    int acquired = pthread_mutex_trylock(&guest_lock);
    if (acquired) return acquired == EBUSY ? SKYRIM_CLASSIC_FIXTURE_BUSY : SKYRIM_CLASSIC_FIXTURE_IO;
    FixtureSessionValues values = {0};
    int allocated = skyrim_serial_i386_allocate();
    int passed = allocated && skyrim_serial_i386_session(77, &input, &values);
    skyrim_serial_i386_release();
    pthread_mutex_unlock(&guest_lock);
    if (!allocated) return SKYRIM_CLASSIC_FIXTURE_MEMORY;
    if (!passed || values.reverted != 5 || values.outgoing != 21 || values.next_calls != 2 ||
        values.reads != 1 || values.events != 7) return SKYRIM_CLASSIC_FIXTURE_CALLBACK;
    if (!fixture_store_record(verified_input, private_output, values.outgoing)) return SKYRIM_CLASSIC_FIXTURE_IO;
    unsigned restored;
    unsigned char on_disk[fixture_bytes];
    if (!fixture_load_record(private_output, &restored) || restored != values.outgoing ||
        !read_reference(private_output, restored, on_disk)) return SKYRIM_CLASSIC_FIXTURE_OUTPUT;
    *result = (SkyrimClassicFixtureResult){input, values.reverted, restored, values.reads, values.events};
    return SKYRIM_CLASSIC_FIXTURE_OK;
}

SkyrimClassicFixtureStatus skyrim_classic_fixture_run(const char *input, const char *output,
                                                     SkyrimClassicFixtureResult *result) {
    if (!result) return SKYRIM_CLASSIC_FIXTURE_ARGUMENT;
    *result = (SkyrimClassicFixtureResult){0};
    if (!cosave_name(input) || !cosave_name(output)) return SKYRIM_CLASSIC_FIXTURE_ARGUMENT;
    struct stat existing;
    if (!lstat(output, &existing)) return SKYRIM_CLASSIC_FIXTURE_EXISTS;
    if (errno != ENOENT) return SKYRIM_CLASSIC_FIXTURE_IO;
    unsigned char data[fixture_bytes];
    if (!read_reference(input, 16, data)) return SKYRIM_CLASSIC_FIXTURE_INPUT;

    /* The document-picker file is never reopened. Parse and transform only
     * our verified 83-byte snapshot in a private directory on the output volume. */
    const char *slash = strrchr(output, '/');
    size_t parent = slash ? (size_t)(slash - output + 1) : 0;
    char dir[PATH_MAX], source[PATH_MAX], target[PATH_MAX];
    static const char pattern[] = ".classic-probe-XXXXXX";
    if (parent + sizeof pattern + sizeof "/output.skse" > sizeof dir) return SKYRIM_CLASSIC_FIXTURE_ARGUMENT;
    memcpy(dir, output, parent);
    memcpy(dir + parent, pattern, sizeof pattern);
    if (!mkdtemp(dir)) return SKYRIM_CLASSIC_FIXTURE_IO;
    size_t len = strlen(dir);
    memcpy(source, dir, len);
    memcpy(source + len, "/input.skse", sizeof "/input.skse");
    memcpy(target, dir, len);
    memcpy(target + len, "/output.skse", sizeof "/output.skse");
    SkyrimClassicFixtureStatus status = SKYRIM_CLASSIC_FIXTURE_IO;
    FILE *copy = fopen(source, "wb");
    if (copy) {
        int wrote = fwrite(data, 1, sizeof data, copy) == sizeof data;
        if (fclose(copy) == 0 && wrote) {
            SkyrimClassicFixtureResult observed;
            status = execute_probe(source, target, &observed);
            if (status == SKYRIM_CLASSIC_FIXTURE_OK) {
                if (link(target, output) == 0) *result = observed;
                else status = errno == EEXIST ? SKYRIM_CLASSIC_FIXTURE_EXISTS : SKYRIM_CLASSIC_FIXTURE_IO;
            }
        }
    }
    unlink(target);
    unlink(source);
    rmdir(dir);
    return status;
}

const char *skyrim_classic_fixture_message(SkyrimClassicFixtureStatus status) {
    static const char *const messages[] = {"ok", "invalid-argument", "unexpected-windows-input",
        "destination-exists", "already-running", "insufficient-memory", "callback-failed", "io-error",
        "output-mismatch"};
    return (unsigned)status < sizeof messages / sizeof *messages ? messages[status] : "unknown-status";
}
