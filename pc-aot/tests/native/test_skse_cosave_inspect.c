/* SPDX-License-Identifier: GPL-3.0-only */
/* Build this file with experiments/skse-cosave/records.c. Including the exact
 * inspector here substitutes only pread, so changes during a read and I/O
 * volume can be tested deterministically on both macOS and Linux. */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static ssize_t fixture_pread(int fd, void *out, size_t n, off_t pos);
#define pread fixture_pread
#include "../../experiments/skse-cosave/inspect.c"
#undef pread

static int directory;
static const char *root;
static size_t calls, bytes;
enum Fault { NONE, INTERRUPT, SHORT, IO_ERROR, REPLACE_BASE, SHRINK_SIDE, ADD_ALIAS };
static enum Fault fault;
static const char *base = "QuickSave.ess", *side = "quicksave.SKSE";
static void put_file(const char *name, const uint8_t *data, size_t n) {
    int fd = openat(directory, name, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);
    assert(fd >= 0 && (!n || write(fd, data, n) == (ssize_t)n));
    assert(close(fd) == 0);
}
static void remove_name(const char *name) {
    assert(!unlinkat(directory, name, 0));
}
static ssize_t fixture_pread(int fd, void *out, size_t n, off_t pos) {
    calls++;
    if (fault == INTERRUPT) {
        fault = NONE;
        errno = EINTR;
        return -1;
    }
    if (fault == SHORT && n > 2) n = 2;
    if (fault == IO_ERROR) {
        errno = EIO;
        return -1;
    }
    if (fault == REPLACE_BASE) {
        fault = NONE;
        put_file("replacement", NULL, 0);
        assert(!renameat(directory, "replacement", directory, base));
    } else if (fault == SHRINK_SIDE) {
        fault = NONE;
        put_file(side, NULL, 0);
    } else if (fault == ADD_ALIAS) {
        fault = NONE;
        put_file("QUICKSAVE.skse", NULL, 0);
    }
    ssize_t used = pread(fd, out, n, pos);
    if (used > 0) bytes += (size_t)used;
    return used;
}
static void put_u32(uint8_t *out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        out[i] = (uint8_t)(value >> (i * 8));
}
static void header(uint8_t *out, uint32_t owners) {
    memcpy(out, "SKSE", 4);
    put_u32(out + 4, 1);
    put_u32(out + 8, 0x01070030);
    put_u32(out + 12, 0x09200000);
    put_u32(out + 16, owners);
}
static void block(uint8_t *out, uint32_t a, uint32_t b, uint32_t c) {
    put_u32(out, a);
    put_u32(out + 4, b);
    put_u32(out + 8, c);
}
static void sample(uint8_t data[61]) {
    header(data, 2);
    block(data + 20, 0, 1, 12);
    block(data + 32, 0x1111, 7, 0);
    block(data + 44, 0x6789, 0, 0); /* The second owner is allowed to be empty. */
}
typedef struct {
    const uint8_t *data;
    size_t length;
    unsigned calls, fail_at;
} AuthoredReader;
static int authored_header(void *context, size_t offset, void *buffer, size_t length) {
    AuthoredReader *reader = context;
    assert(length <= 20 && offset <= reader->length && length <= reader->length - offset);
    if (++reader->calls == reader->fail_at) return 0;
    memcpy(buffer, reader->data + offset, length);
    return 1;
}
static void shared_validator_results(void) {
    uint8_t data[61] = {0};
    sample(data);
    SkseRecordsSummary summary;
    for (size_t offset = 0; offset < 56; ++offset) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            data[offset] ^= (uint8_t)(1u << bit);
            AuthoredReader input = {.data = data, .length = 56};
            assert(skse_records_inspect(56, authored_header, &input, &summary) ==
                   skse_records_validate(data, 56));
            data[offset] ^= (uint8_t)(1u << bit);
        }
    }
    for (unsigned fail = 1; fail <= 4; ++fail) {
        AuthoredReader input = {.data = data, .length = 56, .fail_at = fail};
        assert(skse_records_inspect(56, authored_header, &input, &summary) == SKSE_PAIR_STORAGE);
        assert(!summary.format_version && !summary.skse_version && !summary.runtime_version &&
               !summary.owners && !summary.records);
    }
}
static SkseInspectInfo check(const char *name, SkseInspectStatus expected) {
    SkseInspectInfo out;
    memset(&out, 0xcc, sizeof out);
    calls = bytes = 0;
    SkseInspectStatus status = skse_inspect_companion(root, name, &out);
    if (status != expected)
        fprintf(stderr, "status=%u expected=%u\n", (unsigned)status, (unsigned)expected);
    assert(status == expected);
    if (status != SKSE_INSPECT_OK && status != SKSE_INSPECT_UNSUPPORTED)
        assert(!out.size && !out.format_version && !out.skse_version && !out.runtime_version &&
               !out.owners && !out.records);
    return out;
}

static void normal_metadata_and_wire_errors(void) {
    uint8_t data[61] = {0};
    sample(data);
    put_file(side, data, 56);
    SkseInspectInfo info = check("QUICKSAVE.EsS", SKSE_INSPECT_OK);
    assert(info.size == 56 && info.format_version == 1 && info.skse_version == 0x01070030 &&
           info.runtime_version == 0x09200000 && info.owners == 2 && info.records == 1 && bytes == 56 &&
           calls == 1);
    fault = INTERRUPT;
    check(base, SKSE_INSPECT_OK);
    assert(calls == 2);
    fault = SHORT;
    check(base, SKSE_INSPECT_OK);
    assert(calls == 28);
    fault = NONE;
    fault = IO_ERROR;
    check(base, SKSE_INSPECT_IO);
    fault = NONE;
    for (size_t cut = 0; cut < 56; ++cut) {
        put_file(side, data, cut);
        check(base, SKSE_INSPECT_INVALID);
    }
    put_u32(data + 44, 0);
    put_file(side, data, 56);
    check(base, SKSE_INSPECT_INVALID);
    sample(data);
    put_u32(data + 40, 1);
    put_file(side, data, 56);
    check(base, SKSE_INSPECT_INVALID);
    sample(data);
    put_file(side, data, 57);
    check(base, SKSE_INSPECT_INVALID);
    sample(data);
    put_u32(data + 4, 2);
    put_file(side, data, 56);
    info = check(base, SKSE_INSPECT_UNSUPPORTED);
    assert(info.size == 56 && !info.owners && !info.records && info.format_version == 2);
    sample(data);
    put_u32(data + 12, 9);
    put_file(side, data, 56);
    info = check(base, SKSE_INSPECT_UNSUPPORTED);
    assert(info.runtime_version == 9);
    memset(data, 0, sizeof data);
    header(data, 0);
    put_file(side, data, 20);
    info = check(base, SKSE_INSPECT_OK);
    assert(!info.owners && !info.records);
    data[0] = 'T';
    put_file(side, data, 20);
    check(base, SKSE_INSPECT_INVALID);
    remove_name(side);
}

static void names_and_races(void) {
    uint8_t data[61] = {0};
    sample(data);
    check(base, SKSE_INSPECT_MISSING);
    check(".hidden.ess", SKSE_INSPECT_ARGUMENT);
    check("a/slot.ess", SKSE_INSPECT_ARGUMENT);
    check("a\\slot.ess", SKSE_INSPECT_ARGUMENT);
    check("a\n.ess", SKSE_INSPECT_ARGUMENT);
    check(".ess", SKSE_INSPECT_ARGUMENT);
    check("slot.ess.bak", SKSE_INSPECT_ARGUMENT);
    check("missing.ess", SKSE_INSPECT_BASE_MISSING);
    put_file("other.skse", data, 56);
    check(base, SKSE_INSPECT_MISSING);
    remove_name("other.skse");
    assert(!symlinkat(base, directory, side));
    check(base, SKSE_INSPECT_INVALID);
    remove_name(side);
    assert(!mkfifoat(directory, side, 0600));
    check(base, SKSE_INSPECT_INVALID);
    remove_name(side);
    assert(!mkdirat(directory, side, 0700));
    check(base, SKSE_INSPECT_INVALID);
    assert(!unlinkat(directory, side, AT_REMOVEDIR));
    put_file("foreign", data, 56);
    assert(!linkat(directory, "foreign", directory, side, 0));
    check(base, SKSE_INSPECT_INVALID);
    remove_name(side);
    remove_name("foreign");
    put_file(side, data, 56);
    assert(!symlinkat(".", directory, "link"));
    char linked[PATH_MAX];
    SkseInspectInfo info;
    assert(skse_inspect_companion(root, base, NULL) == SKSE_INSPECT_ARGUMENT);
    assert(skse_inspect_companion("relative", base, &info) == SKSE_INSPECT_ARGUMENT);
    snprintf(linked, sizeof linked, "%s/./", root);
    assert(skse_inspect_companion(linked, base, &info) == SKSE_INSPECT_ARGUMENT);
    snprintf(linked, sizeof linked, "%s/../", root);
    assert(skse_inspect_companion(linked, base, &info) == SKSE_INSPECT_ARGUMENT);
    snprintf(linked, sizeof linked, "%s//child", root);
    assert(skse_inspect_companion(linked, base, &info) == SKSE_INSPECT_ARGUMENT);
    snprintf(linked, sizeof linked, "%s/link", root);
    assert(skse_inspect_companion(linked, base, &info) == SKSE_INSPECT_IO);
    snprintf(linked, sizeof linked, "%s/link/subdirectory", root);
    assert(skse_inspect_companion(linked, base, &info) == SKSE_INSPECT_IO);
    remove_name("link");
    int alias = openat(directory, "QUICKSAVE.skse", O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (alias >= 0) {
        assert(!close(alias));
        check(base, SKSE_INSPECT_AMBIGUOUS);
        remove_name("QUICKSAVE.skse");
        put_file("QUICKSAVE.ESS", NULL, 0);
        check(base, SKSE_INSPECT_AMBIGUOUS);
        remove_name("QUICKSAVE.ESS");
        fault = ADD_ALIAS;
        check(base, SKSE_INSPECT_UNSTABLE);
        remove_name("QUICKSAVE.skse");
    } else
        assert(errno == EEXIST); /* Case-insensitive filesystem. */
    fault = REPLACE_BASE;
    check(base, SKSE_INSPECT_UNSTABLE);
    fault = SHRINK_SIDE;
    check(base, SKSE_INSPECT_UNSTABLE);
    remove_name(side);
    remove_name(base);
    assert(!symlinkat("nowhere", directory, base));
    check(base, SKSE_INSPECT_INVALID);
    remove_name(base);
    put_file(base, NULL, 0);
    put_file("雪.ess", NULL, 0);
    put_file("雪.skse", data, 56);
    check("雪.ESS", SKSE_INSPECT_OK);
    remove_name("雪.ess");
    remove_name("雪.skse");
}

static void limits_and_sparse_reads(void) {
    uint8_t b[44] = {0};
    const uint32_t cap = SKSE_PAIR_MAX_COMPANION;
    header(b, 1);
    block(b + 20, 5, 1, cap - 32);
    block(b + 32, 8, 1, cap - 44);
    put_file(side, b, sizeof b);
    int fd = openat(directory, side, O_WRONLY);
    assert(fd >= 0 && !ftruncate(fd, cap));
    SkseInspectInfo out = check(base, SKSE_INSPECT_OK);
    assert(out.size == cap && out.owners == 1 && out.records == 1 && calls == 1 && bytes == 4096);
    printf("sparse_size=%u pread_calls=%zu pread_bytes=%zu\n", cap, calls, bytes);
    assert(!ftruncate(fd, cap + 1));
    check(base, SKSE_INSPECT_LIMIT);
    assert(!calls);
    assert(!close(fd));
    header(b, 1);
    block(b + 20, 5, 100000, 1200000);
    put_file(side, b, 32);
    fd = openat(directory, side, O_WRONLY);
    assert(fd >= 0 && !ftruncate(fd, 1200032));
    assert(!close(fd));
    check(base, SKSE_INSPECT_LIMIT);
    uint32_t count = 10000;
    size_t size = 32u + 12u * count;
    uint8_t *dense = calloc(1, size);
    assert(dense);
    header(dense, 1);
    block(dense + 20, 17, count, 12u * count);
    put_file(side, dense, size);
    free(dense);
    out = check(base, SKSE_INSPECT_OK);
    assert(out.records == count && calls <= 31 && bytes <= size + 31 * 11);
    printf("dense_records=%u pread_calls=%zu pread_bytes=%zu\n", out.records, calls, bytes);
    remove_name(side);
}
int main(void) {
    char temp[] = "/tmp/skse-companion-inspect-XXXXXX", absolute[PATH_MAX];
    assert(mkdtemp(temp) && realpath(temp, absolute));
    root = absolute;
    directory = open(root, O_RDONLY | O_DIRECTORY);
    assert(directory >= 0);
    put_file(base, (const uint8_t *)"base", 4);
    shared_validator_results();
    normal_metadata_and_wire_errors();
    names_and_races();
    limits_and_sparse_reads();
    remove_name(base);
    assert(!close(directory));
    assert(!rmdir(temp));
    puts("classic_companion=PASS");
    return 0;
}
