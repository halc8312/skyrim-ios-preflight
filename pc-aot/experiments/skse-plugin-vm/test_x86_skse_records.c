/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_skse_records.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned allocations;
static int fail_realloc;
void *skse_test_realloc(void *p, size_t size) {
    allocations++;
    return fail_realloc ? NULL : realloc(p, size);
}

static void trace(void) {
    SkseX86RecordStream s = {0};
    const uint8_t first[] = {0, 0x11, 0x22, 0x33, 0x44}, third[] = {0x80, 0, 0x61, 0xff};
    assert(skse_x86_records_open(&s, 0, 0));
    assert(skse_x86_records_append(&s, first, 2));
    assert(skse_x86_records_append(&s, first + 2, 3));
    assert(skse_x86_records_open(&s, 0, 0));
    assert(skse_x86_records_append(&s, NULL, 0));
    assert(skse_x86_records_write(&s, UINT32_MAX, 7, third, 4));
    assert(skse_x86_records_write(&s, UINT32_MAX, 7, "QZ", 2));
    assert(skse_x86_records_write(&s, 0x40000001, 0xfedcba98, NULL, 0));
    assert(skse_x86_records_reset_reading(&s));
    uint32_t type = 0, version = 0, length = 0, index = 0;
    while (skse_x86_records_next(&s, &type, &version, &length)) {
        uint8_t b[16] = {0};
        uint32_t n = skse_x86_records_read(&s, b, index == 0 ? 2 : 1);
        if (index == 2) n += skse_x86_records_read(&s, b + n, (uint32_t)sizeof(b) - n);
        printf("%" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " ",
               index, type, version, length, n);
        for (uint32_t i = 0; i < n; ++i) printf("%02x", (unsigned)b[i]);
        putchar('\n');
        index++;
    }
    assert(index == 5 && type == 0x40000001 && version == 0xfedcba98 && length == 0);
    skse_x86_records_close(&s);
}

static void phases(void) {
    SkseX86RecordStream s = {0};
    uint8_t byte = 0;
    uint32_t type = 10, version = 20, length = 30;
    assert(!skse_x86_records_count(NULL));
    assert(!skse_x86_records_reset_reading(NULL));
    assert(!skse_x86_records_open(NULL, 1, 1));
    assert(!skse_x86_records_append(&s, NULL, 0));
    assert(!skse_x86_records_read(&s, &byte, 1));
    assert(!skse_x86_records_next(&s, &type, &version, &length));
    assert(skse_x86_records_reset_reading(&s));
    assert(!skse_x86_records_next(&s, &type, &version, &length));
    assert(type == 10 && version == 20 && length == 30);
    assert(!skse_x86_records_write(&s, 9, 7, "a", 1));
    assert(!skse_x86_records_open(&s, 9, 7));
    assert(!skse_x86_records_append(&s, "a", 1));
    skse_x86_records_close(&s);
    assert(skse_x86_records_write(&s, 8, 9, "abc", 3));
    assert(!skse_x86_records_write(&s, 33, 44, NULL, 1));
    assert(skse_x86_records_count(&s) == 1);
    assert(skse_x86_records_reset_reading(&s));
    assert(!skse_x86_records_read(&s, &byte, 1));
    assert(skse_x86_records_next(&s, &type, &version, &length));
    assert(type == 8 && version == 9 && length == 3);
    assert(skse_x86_records_read(&s, &byte, 1) == 1 && byte == 'a');
    assert(!skse_x86_records_next(&s, NULL, &version, &length));
    assert(!skse_x86_records_read(&s, NULL, 1));
    assert(!skse_x86_records_read(&s, NULL, 0));
    assert(skse_x86_records_read(&s, &byte, 1) == 1 && byte == 'b');
    assert(!skse_x86_records_next(&s, &type, &version, &length));
    assert(type == 8 && version == 9 && length == 3);
    assert(!skse_x86_records_read(&s, &byte, 1));
    assert(skse_x86_records_reset_reading(&s));
    assert(skse_x86_records_next(&s, &type, &version, &length));
    uint8_t again[5] = {0, 0, 0, 17, 18};
    assert(skse_x86_records_read(&s, again, UINT32_MAX) == 3);
    assert(memcmp(again, "abc\021\022", 5) == 0);
    skse_x86_records_close(&s);
    skse_x86_records_close(&s);
    skse_x86_records_close(NULL);
    assert(!skse_x86_records_count(&s));
}

static void record_limit(void) {
    SkseX86RecordStream s = {0};
    for (uint32_t i = 0; i < SKSE_X86_RECORD_LIMIT; ++i)
        assert(skse_x86_records_open(&s, i, UINT32_MAX - i));
    assert(allocations == 0);
    assert(!skse_x86_records_open(&s, 1234, 1234));
    assert(!skse_x86_records_write(&s, 4321, 4321, "X", 1));
    assert(skse_x86_records_append(&s, "Y", 1));
    assert(skse_x86_records_count(&s) == 16 && skse_x86_records_reset_reading(&s));
    uint32_t type, version, length;
    for (uint32_t i = 0; i < 16; ++i) {
        assert(skse_x86_records_next(&s, &type, &version, &length));
        assert(type == i && version == UINT32_MAX - i && length == (uint32_t)(i == 15));
    }
    char byte;
    assert(skse_x86_records_read(&s, &byte, 1) == 1 && byte == 'Y');
    skse_x86_records_close(&s);
}

static void byte_limit(void) {
    SkseX86RecordStream s = {0};
    uint8_t input[SKSE_X86_RECORD_BYTES], output[SKSE_X86_RECORD_BYTES];
    for (uint32_t i = 0; i < sizeof(input); ++i) input[i] = (uint8_t)(i ^ (i >> 8));
    assert(!skse_x86_records_write(&s, 11, 11, input, UINT32_MAX));
    assert(!skse_x86_records_count(&s) && allocations == 0);
    assert(skse_x86_records_write(&s, 1, 0, input, 123));
    assert(skse_x86_records_write(&s, 2, 0, input + 123, 65536 - 123));
    assert(!skse_x86_records_append(&s, input, 1));
    assert(!skse_x86_records_write(&s, 9, 9, input, 1));
    assert(skse_x86_records_write(&s, 3, 0, NULL, 0));
    assert(skse_x86_records_count(&s) == 3 && skse_x86_records_reset_reading(&s));
    uint32_t type, version, length;
    assert(skse_x86_records_next(&s, &type, &version, &length) && type == 1 && length == 123);
    assert(skse_x86_records_read(&s, output, sizeof(output)) == 123);
    assert(skse_x86_records_next(&s, &type, &version, &length) && type == 2 && length == 65536 - 123);
    assert(skse_x86_records_read(&s, output + 123, length) == length);
    assert(memcmp(input, output, sizeof(input)) == 0);
    assert(skse_x86_records_next(&s, &type, &version, &length) && type == 3 && !length);
    skse_x86_records_close(&s);
}

static void alloc_failure(void) {
    SkseX86RecordStream s = {0};
    uint8_t large[1024] = {42}, saved[2];
    fail_realloc = 1;
    assert(!skse_x86_records_write(&s, 99, 99, "x", 1));
    assert(!skse_x86_records_count(&s));
    fail_realloc = 0;
    assert(skse_x86_records_write(&s, 1, 1, "a", 1));
    fail_realloc = 1;
    assert(!skse_x86_records_append(&s, large, sizeof(large)));
    assert(!skse_x86_records_write(&s, 99, 99, large, sizeof(large)));
    assert(skse_x86_records_count(&s) == 1);
    assert(skse_x86_records_append(&s, "b", 1));
    fail_realloc = 0;
    assert(skse_x86_records_write(&s, 2, 0, large, sizeof(large)));
    uint32_t type, version, length;
    assert(skse_x86_records_reset_reading(&s));
    assert(skse_x86_records_next(&s, &type, &version, &length) && type == 1 && length == 2);
    assert(skse_x86_records_read(&s, saved, 2) == 2 && !memcmp(saved, "ab", 2));
    assert(skse_x86_records_next(&s, &type, &version, &length) && type == 2 && length == 1024);
    assert(skse_x86_records_read(&s, saved, 1) == 1 && saved[0] == 42);
    skse_x86_records_close(&s);
}

static void separate_owners(void) {
    SkseX86RecordStream a = {0}, b = {0};
    uint32_t type, version, length;
    uint8_t out[4];
    assert(skse_x86_records_write(&a, 77, 9, "one", 3));
    assert(skse_x86_records_write(&b, 77, 9, "two", 3));
    assert(skse_x86_records_reset_reading(&a));
    assert(skse_x86_records_next(&a, &type, &version, &length) && length == 3);
    assert(skse_x86_records_read(&a, out, 1) == 1 && out[0] == 'o');
    assert(skse_x86_records_append(&b, "!", 1));
    skse_x86_records_close(&a);
    assert(skse_x86_records_reset_reading(&b));
    assert(skse_x86_records_next(&b, &type, &version, &length) && length == 4);
    assert(skse_x86_records_read(&b, out, 4) == 4 && !memcmp(out, "two!", 4));
    assert(!skse_x86_records_count(&a) && skse_x86_records_count(&b) == 1);
    skse_x86_records_close(&b);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "trace")) trace();
    else if (!strcmp(argv[1], "phases")) phases();
    else if (!strcmp(argv[1], "record-limit")) record_limit();
    else if (!strcmp(argv[1], "byte-limit")) byte_limit();
    else if (!strcmp(argv[1], "oom")) alloc_failure();
    else if (!strcmp(argv[1], "owners")) separate_owners();
    else return 2;
    return 0;
}
