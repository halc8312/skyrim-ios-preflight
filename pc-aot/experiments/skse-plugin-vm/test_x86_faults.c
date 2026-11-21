/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_call.h"
#include "x86_wintext.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static uint8_t scratch[4096];
static const uint32_t image_base = 0x00100000;
static const uint32_t scratch_base = 0x00200000;

static SkseX86Result run(uint8_t *code, size_t length, uint32_t limit, SkseX86Services services) {
    SksePeImage image = {
        .base = image_base, .size = (uint32_t)length, .bytes = code, .section_count = 1,
        .sections = {{0, (uint32_t)length, SKSE_PE_READ | SKSE_PE_EXEC}}
    };
    memset(scratch, 0, sizeof scratch);
    return skse_x86_call(&image, image.base, (SkseX86Scratch){scratch_base, scratch, sizeof scratch},
                         NULL, 0, limit, services, SKSE_X86_CDECL);
}

#define ARRAY_RUN(code) run(code, sizeof code, 100, (SkseX86Services){0})

static void append_word(uint8_t *code, size_t *used, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) code[(*used)++] = (uint8_t)(value >> (8 * i));
}

static SkseX86Result run_wide_scan(bool compact, const SkseX86ReadView *input, uint32_t count, uint32_t work) {
    uint8_t code[128] = {0x56, 0xbe};              /* preserve ESI, then load source */
    size_t used = 2;
    append_word(code, &used, input->base);
    code[used++] = 0xb9;                         /* MOV ECX, count */
    append_word(code, &used, count);
    const uint8_t short_cmp[] = {0x66, 0x83, 0x3e, 0, 0x74, 6, 0x83, 0xc6, 2, 0x49, 0x75, 0xf4};
    const uint8_t long_cmp[] = {0x66, 0x81, 0x3e, 0, 0, 0x74, 6, 0x83, 0xc6, 2, 0x49, 0x75, 0xf3};
    const size_t length = compact ? sizeof short_cmp : sizeof long_cmp;
    memcpy(code + used, compact ? short_cmp : long_cmp, length);
    used += length;
    code[used++] = 0x89;
    code[used++] = 0x0d;                         /* store ECX without changing flags */
    append_word(code, &used, scratch_base);
    code[used++] = 0x89;
    code[used++] = 0xf0;                         /* MOV EAX, ESI */
    const uint8_t flags[] = {0x92, 0x94, 0x98, 0x90, 0x9a};
    for (unsigned i = 0; i < sizeof flags; ++i) {
        code[used++] = 0x0f;
        code[used++] = flags[i];
        code[used++] = 5;
        append_word(code, &used, scratch_base + 4 + i);
    }
    code[used++] = 0x5e;
    code[used++] = 0xc3;
    return run(code, used, work, (SkseX86Services){.read_views = input, .read_view_count = 1});
}

static void check_wide_scan(void) {
    uint8_t words[128];
    const uint32_t address = 0x00400001;          /* unaligned reads are permitted */
    const struct {
        uint32_t count, width, work;
        unsigned zero_index;
        SkseX86Status expected;
    } cases[] = {
        {64, 128, 500, 0, SKSE_X86_OK}, {64, 128, 500, 20, SKSE_X86_OK},
        {64, 128, 500, 63, SKSE_X86_OK}, {64, 128, 500, 64, SKSE_X86_OK},
        {64, 128, 25, 1, SKSE_X86_OK}, {64, 128, 25, 32, SKSE_X86_INSTRUCTION_LIMIT},
        {64, 77, 500, 64, SKSE_X86_MEMORY_FAULT}, {64, 77, 500, 38, SKSE_X86_MEMORY_FAULT},
        {64, 76, 500, 11, SKSE_X86_OK}, {15, 128, 500, 64, SKSE_X86_OK},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        memset(words, 0, sizeof words);
        for (unsigned j = 0; j < sizeof words / 2; ++j)
            if (j != cases[i].zero_index) words[2 * j + j % 2] = 0x80;
        const SkseX86ReadView source = {address, words, cases[i].width};
        const SkseX86Result expected = run_wide_scan(false, &source, cases[i].count, cases[i].work);
        uint8_t output[9];
        memcpy(output, scratch, sizeof output);
        const SkseX86Result actual = run_wide_scan(true, &source, cases[i].count, cases[i].work);
        assert(actual.status == cases[i].expected && actual.status == expected.status);
        assert(actual.eax == expected.eax && actual.fault_address == expected.fault_address);
        assert(actual.instructions == expected.instructions && actual.work == expected.work);
        assert(!memcmp(output, scratch, sizeof output));
    }
}

static uint32_t protected_file_attributes(void *context, const char *path, uint32_t *attributes) {
    (void)context;
    if (strcmp(path, "base.dat")) return 2;
    *attributes = SKSE_WIN_ATTRIBUTE_NORMAL;
    return 0;
}

static void check_virtual_file_limits(void) {
    SkseWinFiles files = {.source.attributes = protected_file_attributes};
    uint32_t error = 0;
    assert(skse_win_file_open(&files, "base.dat", SKSE_WIN_FILE_WRITE, 0, SKSE_WIN_CREATE_ALWAYS, &error) ==
           UINT32_MAX && error == 5);
    assert(skse_win_file_open(&files, "missing/output.log", SKSE_WIN_FILE_WRITE, 0, SKSE_WIN_CREATE_NEW, &error) ==
           UINT32_MAX && error == 3);
    uint16_t absolute[] = {'C', ':', '/', 's', 'k', 'y', 'r', 'i', 'm', '/', 'l', 'o', 'g'};
    char relative[SKSE_WIN_PATH_BYTES];
    assert(skse_win_asset_path(absolute, sizeof absolute / sizeof absolute[0], true, relative));
    assert(!strcmp(relative, "log"));
    absolute[0] = 'D';
    assert(!skse_win_asset_path(absolute, sizeof absolute / sizeof absolute[0], true, relative));
    uint16_t traversal[] = {'.', '.', '\\', 'l', 'o', 'g'};
    assert(!skse_win_asset_path(traversal, sizeof traversal / sizeof traversal[0], true, relative));
    const uint16_t sibling[] = u"C:\\Skyrim\\Data\\SKSE\\Plugins\\StorageUtilData\\..\\WearableLanternsData\\startup_test_file.json";
    assert(skse_win_asset_path(sibling, sizeof sibling / sizeof sibling[0] - 1, true, relative));
    assert(!strcmp(relative, "Data/SKSE/Plugins/WearableLanternsData/startup_test_file.json"));
    const uint16_t alias[] = u".\\Unused\\..\\base.dat";
    assert(skse_win_asset_path(alias, sizeof alias / sizeof alias[0] - 1, true, relative) &&
           !strcmp(relative, "base.dat"));
    assert(skse_win_file_open(&files, relative, SKSE_WIN_FILE_WRITE, 0, SKSE_WIN_CREATE_ALWAYS, &error) ==
           UINT32_MAX && error == 5);
    const uint16_t escape[] = u"C:\\Skyrim\\Data\\..\\..\\base.dat";
    assert(!skse_win_asset_path(escape, sizeof escape / sizeof escape[0] - 1, true, relative));
    const uint16_t root[] = u"Data\\..";
    assert(!skse_win_asset_path(root, sizeof root / sizeof root[0] - 1, true, relative));

    uint32_t token = skse_win_file_open(&files, "log", SKSE_WIN_FILE_WRITE, 0, SKSE_WIN_CREATE_NEW, &error);
    SkseWinFile *file = skse_win_file_lookup(&files, token);
    assert(file && skse_win_output_exists(&files, "LOG"));
    assert(skse_win_file_resize(file, 8) == 0);
    assert(skse_win_file_resize(file, 9) == 0 && file->data->bytes[8] == 0);
    file->data->bytes[8] = 0xf3;
    assert(skse_win_file_resize(file, 8) == 0);
    assert(skse_win_file_resize(file, 9) == 0 && file->data->bytes[8] == 0);
    assert(skse_win_file_resize(file, UINT64_MAX) == 223 && file->data->length == 9);

    uint8_t write_thunk[] = {0xb8, 0, 0, 0x30, 0, 0xff, 0xe0};
    SksePeImage image = {
        .base = image_base, .size = sizeof write_thunk, .bytes = write_thunk, .section_count = 1,
        .sections = {{0, sizeof write_thunk, SKSE_PE_READ | SKSE_PE_EXEC}}
    };
    const SkseX86Service service = {0x00300000, SKSE_WIN_WRITE_FILE, SKSE_X86_WIN_FILES};
    SkseWinThread thread = {0};
    SkseX86Services services = {.entries = &service, .count = 1, .files = &files, .thread = &thread};
    SkseX86Scratch memory = {scratch_base, scratch, sizeof scratch};
    uint32_t args[] = {token, 0x00400000, 1, scratch_base, 0};
    file->offset = 32;
    files.read_only = true;
    assert(skse_win_file_open(&files, "log", SKSE_WIN_FILE_READ, 0, SKSE_WIN_OPEN_ALWAYS, &error) ==
           UINT32_MAX && error == 5);
    assert(skse_win_file_open(&files, "new.log", SKSE_WIN_FILE_WRITE, 0, SKSE_WIN_CREATE_NEW, &error) ==
           UINT32_MAX && error == 5);
    assert(skse_win_directory_create(&files, "new") == 5 && !files.directory_requests);
    SkseX86Result result = skse_x86_call(&image, image.base, memory, args, 5, 100, services, SKSE_X86_STDCALL);
    assert(result.status == SKSE_X86_OK && !result.eax && thread.last_error == 5 &&
           file->data->length == 9 && file->offset == 32);
    files.read_only = false;
    result = skse_x86_call(&image, image.base, memory, args, 5, 100, services, SKSE_X86_STDCALL);
    assert(result.status == SKSE_X86_MEMORY_FAULT && file->data->length == 9 && file->offset == 32);
    args[1] = scratch_base;
    args[2] = 1536;
    args[3] = scratch_base + 2048;
    result = skse_x86_call(&image, image.base, memory, args, 5, 12, services, SKSE_X86_STDCALL);
    assert(result.status == SKSE_X86_INSTRUCTION_LIMIT && file->data->length == 9 && file->offset == 32);
    skse_win_files_close(&files);
    assert(!skse_win_file_lookup(&files, token) && !skse_win_output_exists(&files, "log"));
}

static void check_alu(unsigned width, unsigned op, unsigned carry_in, uint32_t a, uint32_t b) {
    uint8_t code[64];
    size_t used = 0;
    code[used++] = 0xb8; // MOV EAX, a
    append_word(code, &used, a);
    code[used++] = 0xb9; // MOV ECX, b
    append_word(code, &used, b);
    code[used++] = carry_in ? 0xf9 : 0xf8;
    if (width == 2) code[used++] = 0x66;
    static const uint8_t opcodes[] = {0x00, 0x10, 0x28, 0x18}; // ADD, ADC, SUB, SBB
    code[used++] = opcodes[op] + (width != 1);
    code[used++] = 0xc8; // Destination EAX or a sub-register, source ECX.
    code[used++] = 0xa3; // MOV [scratch], EAX
    append_word(code, &used, scratch_base);
    static const uint8_t setcc[] = {0x92, 0x90, 0x94, 0x98, 0x9a}; // CF, OF, ZF, SF, PF
    for (unsigned i = 0; i < sizeof setcc; ++i) {
        code[used++] = 0x0f;
        code[used++] = setcc[i];
        code[used++] = 0x05;
        append_word(code, &used, scratch_base + 4 + i);
    }
    code[used++] = 0xc3;
    assert(used <= sizeof code);
    SkseX86Result result = run(code, used, 100, (SkseX86Services){0});
    assert(result.status == SKSE_X86_OK);

    const int64_t modulus = INT64_C(1) << (8 * width);
    const int64_t sign = modulus / 2;
    const int64_t carry = (op & 1) ? carry_in : 0;
    const int64_t left = a >= sign ? (int64_t)a - modulus : (int64_t)a;
    const int64_t right = (b >= sign ? (int64_t)b - modulus : (int64_t)b) + carry;
    const int64_t signed_value = op >= 2 ? left - right : left + right;
    const int64_t unsigned_value = op >= 2 ? (int64_t)a - b - carry : (int64_t)a + b + carry;
    const uint32_t value = (uint32_t)unsigned_value & (uint32_t)(modulus - 1);
    unsigned parity = 0;
    for (unsigned i = 0; i < 8; ++i) parity += (value >> i) & 1;
    assert((result.eax & (uint32_t)(modulus - 1)) == value);
    assert(scratch[4] == (unsigned_value < 0 || unsigned_value >= modulus));
    assert(scratch[5] == (signed_value < -sign || signed_value >= sign));
    assert(scratch[6] == (value == 0));
    assert(scratch[7] == (value >= sign));
    assert(scratch[8] == !(parity & 1));
}

static void check_default_file_security_attributes(void) {
    uint8_t name[] = "scratch.log";
    const uint32_t name_address = 0x00400000, attributes_address = 0x00400100;
    const uint32_t token_address = 0x00300000;
    uint8_t attributes[12] = {12, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
    SkseX86ReadView views[] = {{name_address, name, sizeof name}, {attributes_address, attributes, sizeof attributes}};
    uint8_t thunk[] = {0xb8, 0, 0, 0x30, 0, 0xff, 0xe0};
    SksePeImage image = {
        .base = image_base, .size = sizeof thunk, .bytes = thunk, .section_count = 1,
        .sections = {{0, sizeof thunk, SKSE_PE_READ | SKSE_PE_EXEC}}
    };
    const SkseX86Service open_file = {token_address, SKSE_WIN_OPEN_FILE_A, SKSE_X86_WIN_FILES};
    SkseWinFiles files = {0};
    SkseWinThread thread = {0};
    const SkseX86Services services = {.entries = &open_file, .count = 1, .files = &files, .thread = &thread,
                                     .read_views = views, .read_view_count = 2};
    const SkseX86Scratch memory = {scratch_base, scratch, sizeof scratch};
    const uint32_t arguments[] = {name_address, UINT32_C(0x40000000), 1, attributes_address, SKSE_WIN_CREATE_ALWAYS,
                                  SKSE_WIN_ATTRIBUTE_NORMAL, 0};
    attributes[4] = 1; // An explicit descriptor cannot change the virtual access policy.
    SkseX86Result result = skse_x86_call(&image, image.base, memory, arguments, 7, 100, services, SKSE_X86_STDCALL);
    assert(result.status == SKSE_X86_OK && result.eax == UINT32_MAX && thread.last_error == 5 && !files.requests);
    attributes[4] = 0;
    views[1].length = sizeof attributes - 1;
    result = skse_x86_call(&image, image.base, memory, arguments, 7, 100, services, SKSE_X86_STDCALL);
    assert(result.status == SKSE_X86_MEMORY_FAULT && result.fault_address == attributes_address && !files.requests);
    views[1].length = sizeof attributes;
    result = skse_x86_call(&image, image.base, memory, arguments, 7, 100, services, SKSE_X86_STDCALL);
    assert(result.status == SKSE_X86_OK && skse_win_file_lookup(&files, result.eax));
    assert(skse_win_output_exists(&files, (const char *)name));
    skse_win_files_close(&files);
}

static uint32_t read_test_ini(void *context, const char *path, size_t limit, uint8_t **bytes, size_t *length) {
    unsigned *reads = context;
    ++*reads;
    if (!skse_win_asset_name_equal(path, "Data/SKSE/Plugins/Test.ini")) return 2;
    static const char input[] = " [ MAIN ] \r\n ;Name=Ignored\r\n Name = \"Value\" \r\nName=Second\r\n"
                                "number = -0x2a suffix\nEmpty=''\n [Main]\nMissing=Duplicate section\n";
    *length = sizeof input - 1;
    assert(*length <= limit);
    *bytes = malloc(*length);
    assert(*bytes);
    memcpy(*bytes, input, *length);
    return 0;
}

static void check_plugin_ini(void) {
    enum { SECTION, NAME, NUMBER, EMPTY, MISSING, DEFAULT, PATH, FILE_MISSING, FOREIGN };
    char input[][64] = {"main", "name", "number", "Empty", "Missing", " keep ",
        "C:\\Skyrim\\Data\\SKSE\\Plugins\\Test.ini", "data\\SKSE\\Plugins\\Missing.ini",
        "data\\outside.ini"};
    const SkseX86ReadView view = {0x400000, (uint8_t *)input, sizeof input};
    uint32_t addresses[sizeof input / sizeof input[0]];
    for (size_t i = 0; i < sizeof addresses / sizeof addresses[0]; ++i)
        addresses[i] = view.base + (uint32_t)(sizeof input[0] * i);
    uint8_t thunk[] = {0xb8, 0, 0, 0x30, 0, 0xff, 0xe0};
    SksePeImage image = {
        .base = image_base, .size = sizeof thunk, .bytes = thunk, .section_count = 1,
        .sections = {{0, sizeof thunk, SKSE_PE_READ | SKSE_PE_EXEC}}
    };
    unsigned reads = 0;
    SkseWinFiles files = {.source = {.read = read_test_ini, .context = &reads}, .read_only = true};
    SkseWinThread thread = {.last_error = 999};
    SkseX86Service service = {0x00300000, SKSE_WIN_GET_PROFILE_STRING_A, SKSE_X86_WIN_FILES};
    const SkseX86Services services = {.entries = &service, .count = 1, .files = &files, .thread = &thread,
        .read_views = &view, .read_view_count = 1};
    const SkseX86Scratch memory = {scratch_base, scratch, sizeof scratch};
    static const struct {
        unsigned key, path, capacity, last_error;
        const char *expected;
    } cases[] = {
        {NAME, PATH, 12, 0, "Value"}, {NAME, PATH, 6, 234, "Value"}, {NAME, PATH, 3, 234, "Va"},
        {NAME, PATH, 0, 234, NULL}, {EMPTY, PATH, 12, 0, ""}, {MISSING, PATH, 12, 2, " keep"},
        {NAME, FILE_MISSING, 2, 2, " "}, {NAME, FOREIGN, 12, SKSE_WIN_TEXT_NOT_SUPPORTED, NULL}
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        memset(scratch, 0x5a, sizeof scratch);
        thread.last_error = 999;
        const uint32_t args[] = {addresses[SECTION], addresses[cases[i].key], addresses[DEFAULT],
                                 scratch_base, cases[i].capacity, addresses[cases[i].path]};
        const SkseX86Result result = skse_x86_call(&image, image.base, memory, args, 6, 1000, services, SKSE_X86_STDCALL);
        assert(result.status == SKSE_X86_OK && thread.last_error == cases[i].last_error);
        const char *expected = cases[i].expected;
        if (expected) {
            assert(result.eax == strlen(expected) && !memcmp(scratch, expected, result.eax + 1));
            assert(scratch[result.eax + 1] == 0x5a);
        } else {
            assert(!result.eax && scratch[0] == 0x5a);
        }
    }
    service.value = SKSE_WIN_GET_PROFILE_INT_A;
    const unsigned keys[] = {NUMBER, MISSING, EMPTY};
    const uint32_t integers[] = {UINT32_C(0xffffffd6), 19, 19};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        thread.last_error = 999;
        const uint32_t args[] = {addresses[SECTION], addresses[keys[i]], 19, addresses[PATH]};
        const SkseX86Result result = skse_x86_call(&image, image.base, memory, args, 4, 1000, services, SKSE_X86_STDCALL);
        assert(result.status == SKSE_X86_OK && result.eax == integers[i] && thread.last_error == (i == 1 ? 2 : 0));
    }
    assert(reads == 10);
    for (size_t i = 0; i < SKSE_WIN_OPEN_FILES; ++i) assert(!files.files[i].token);
    skse_win_files_close(&files);
}

static void check_arithmetic_flags(void) {
    for (unsigned op = 0; op < 4; ++op) {
        for (unsigned carry = 0; carry < 2; ++carry) {
            for (unsigned a = 0; a < 256; ++a)
                for (unsigned b = 0; b < 256; ++b) check_alu(1, op, carry, a, b);
            uint32_t rng = 0x731edaff;
            for (unsigned i = 0; i < 2048; ++i) {
                uint32_t a = rng = rng * 1664525 + 1013904223;
                uint32_t b = rng = rng * 1664525 + 1013904223;
                check_alu(2, op, carry, a & 0xffff, b & 0xffff);
                check_alu(4, op, carry, a, b);
            }
        }
    }
}

static void check_double_shift_left(void) {
    uint8_t shift[] = {0xb8, 0, 0, 0, 0x80, 0xba, 0, 0, 0, 0x80,
                       0x0f, 0xa4, 0xd0, 1, 0x71, 3, 0x73, 1, 0xc3, 0x0f, 0x0b};
    SkseX86Result result = ARRAY_RUN(shift);
    assert(result.status == SKSE_X86_OK && result.eax == 1);
    uint8_t count_register[] = {0xb8, 0x34, 0x12, 0x11, 0x11, 0xba, 0x78, 0x56, 0x22, 0x22,
                                0xb9, 16, 0, 0, 0, 0x66, 0x0f, 0xa5, 0xd0, 0xc3};
    result = ARRAY_RUN(count_register);
    assert(result.status == SKSE_X86_OK && result.eax == 0x11115678);
    count_register[11] = 17;
    assert(ARRAY_RUN(count_register).status == SKSE_X86_UNSUPPORTED_INSTRUCTION);
    uint8_t zero_count[] = {0xb8, 99, 0, 0, 0, 0xf9, 0x0f, 0xa4, 0xd0, 32, 0x73, 1, 0xc3, 0x0f, 0x0b};
    result = ARRAY_RUN(zero_count);
    assert(result.status == SKSE_X86_OK && result.eax == 99);
}

static void check_bounded_heap(void) {
    SkseX86Heap heap = {.base = 0x00300000};
    assert(!heap.bytes && !skse_x86_heap_allocate(&heap, SKSE_X86_HEAP_BYTES + 1, false));
    const uint32_t original = skse_x86_heap_allocate(&heap, 3, false);
    assert(original == heap.base && heap.bytes);
    assert(skse_x86_heap_size(&heap, original) == 3 && skse_x86_heap_size(&heap, original + 1) == UINT32_MAX);
    uint8_t *data = skse_x86_heap_memory(&heap, original, 3, true);
    assert(data && !skse_x86_heap_memory(&heap, original, 4, false) &&
           !skse_x86_heap_memory(&heap, original + 3, 1, false));
    memcpy(data, "abc", 3);
    heap.bytes[3] = 0xde; // Old allocation padding cannot escape.
    SkseX86Services memory = {.heap = &heap};
    uint8_t load_word[] = {0xa1, 0, 0, 0x30, 0, 0xc3};
    SkseX86Result tail = run(load_word, sizeof load_word, 10, memory);
    assert(tail.status == SKSE_X86_OK && tail.eax == 0x00636261);
    load_word[1] = 2; // Unaligned.
    assert(run(load_word, sizeof load_word, 10, memory).status == SKSE_X86_MEMORY_FAULT);
    load_word[1] = 4; // Entirely outside the live logical range.
    assert(run(load_word, sizeof load_word, 10, memory).status == SKSE_X86_MEMORY_FAULT);
    load_word[0] = 0xa3;
    load_word[1] = 0; // Writes retain the logical bounds.
    assert(run(load_word, sizeof load_word, 10, memory).status == SKSE_X86_MEMORY_FAULT);
    assert(!memcmp(data, "abc", 3) && heap.bytes[3] == 0xde && skse_x86_heap_size(&heap, original) == 3);
    const uint32_t blocker = skse_x86_heap_allocate(&heap, 64, false);
    assert(blocker == original + 16);
    assert(!skse_x86_heap_resize(&heap, original, 48, true, true));
    assert(skse_x86_heap_memory(&heap, original, 3, false) && !skse_x86_heap_release(&heap, original + 1));
    uint32_t grown = skse_x86_heap_resize(&heap, original, 48, true, false);
    assert(grown != original && grown > blocker);
    assert(!skse_x86_heap_memory(&heap, original, 1, false));
    data = skse_x86_heap_memory(&heap, grown, 48, true);
    assert(data && !memcmp(data, "abc", 3));
    for (unsigned i = 3; i < 48; ++i) assert(data[i] == 0);
    assert(skse_x86_heap_release(&heap, blocker) && !skse_x86_heap_release(&heap, blocker));
    assert(skse_x86_heap_resize(&heap, grown, 64, true, true) == grown);
    assert(skse_x86_heap_release(&heap, grown));
    assert(skse_x86_heap_allocate(&heap, SKSE_X86_HEAP_BYTES, true) == original);
    assert(!skse_x86_heap_allocate(&heap, 1, false));
    skse_x86_heap_close(&heap);
    assert(!heap.bytes && !heap.count);
}

static void check_heap_metadata_growth(void) {
    SkseX86Heap heap = {.base = 0x00300000};
    uint32_t addresses[SKSE_X86_HEAP_OBJECTS];
    assert(!heap.bytes && !heap.blocks && !heap.capacity);
    addresses[0] = skse_x86_heap_allocate(&heap, 7, false);
    uint8_t *first = skse_x86_heap_memory(&heap, addresses[0], 7, true);
    assert(first);
    memcpy(first, "storage", 7);
    const size_t initial_capacity = heap.capacity;
    for (size_t i = 1; i < SKSE_X86_HEAP_OBJECTS; ++i) {
        addresses[i] = skse_x86_heap_allocate_owned(
            &heap, 13, true, i == 1 ? SKSE_X86_HEAP_ENVIRONMENT : SKSE_X86_HEAP_GENERAL);
        assert(addresses[i] > addresses[i - 1]);
    }
    assert(initial_capacity > 0 && initial_capacity < heap.capacity);
    assert(heap.count == SKSE_X86_HEAP_OBJECTS && heap.capacity == SKSE_X86_HEAP_OBJECTS);
    assert(!skse_x86_heap_allocate(&heap, 1, false));
    assert(first == skse_x86_heap_memory(&heap, addresses[0], 7, true) && !memcmp(first, "storage", 7));
    assert(!skse_x86_heap_release(&heap, addresses[1]));
    assert(!skse_x86_heap_memory(&heap, addresses[1], 1, true));
    assert(skse_x86_heap_release_owned(&heap, addresses[1], SKSE_X86_HEAP_ENVIRONMENT));
    assert(!skse_x86_heap_memory(&heap, addresses[1], 1, false));
    assert(skse_x86_heap_allocate(&heap, 16, true) == addresses[1]);
    assert(skse_x86_heap_size(&heap, addresses[SKSE_X86_HEAP_OBJECTS - 1]) == 13);
    skse_x86_heap_close(&heap);
    assert(!heap.bytes && !heap.blocks && !heap.capacity && !heap.count);
}

static SkseX86Result private_heap_call(SkseX86Heap *heap, SkseWinThread *thread, SkseX86ServiceKind kind,
                                     const uint32_t *args, size_t count, unsigned budget) {
    uint8_t thunk[] = {0xb8, 0, 0, 0x40, 0, 0xff, 0xe0};  /* mov eax, token; jmp eax */
    const SkseX86Service service = {0x00400000, 0, kind};
    SksePeImage image = {
        .base = image_base, .size = sizeof thunk, .bytes = thunk, .section_count = 1,
        .sections = {{0, sizeof thunk, SKSE_PE_READ | SKSE_PE_EXEC}}
    };
    const SkseX86Services services = {.entries = &service, .count = 1, .heap = heap, .thread = thread};
    memset(scratch, 0, sizeof scratch);
    return skse_x86_call(&image, image_base, (SkseX86Scratch){scratch_base, scratch, sizeof scratch},
                         args, count, budget, services, SKSE_X86_STDCALL);
}

static uint32_t heap_value(SkseX86Heap *heap, SkseWinThread *thread, SkseX86ServiceKind kind,
                           const uint32_t *args, size_t count) {
    const SkseX86Result result = private_heap_call(heap, thread, kind, args, count, 65535);
    assert(result.status == SKSE_X86_OK && result.service_calls[0] == 1);
    return result.eax;
}

static void check_private_heaps(void) {
    SkseX86Heap heap = {.base = 0x00300000};
    SkseWinThread thread = {.id = 1};
    const uint32_t create[] = {0, 0x1000, 0}, create_unserialized[] = {1, 0x1000, 0};
    const uint32_t a = heap_value(&heap, &thread, SKSE_X86_HEAP_CREATE, create, 3);
    const uint32_t b = heap_value(&heap, &thread, SKSE_X86_HEAP_CREATE, create_unserialized, 3);
    assert(a && b && a != b && a != SKSE_X86_PROCESS_HEAP && b != SKSE_X86_PROCESS_HEAP && !heap.bytes);

    uint32_t blank[] = {a, 9, 12};
    const uint32_t first = heap_value(&heap, &thread, SKSE_X86_HEAP_ALLOC, blank, 3);
    const uint32_t other = heap_value(&heap, &thread, SKSE_X86_HEAP_ALLOC, (uint32_t[]){b, 0, 16}, 3);
    const uint32_t process = skse_x86_heap_allocate(&heap, 16, false);
    const uint32_t env = skse_x86_heap_allocate_owned(&heap, 16, false, SKSE_X86_HEAP_ENVIRONMENT);
    assert(first && other && process && env);
    uint8_t *bytes = skse_x86_heap_memory(&heap, first, 12, true);
    assert(bytes);
    for (unsigned i = 0; i < 12; ++i) assert(!bytes[i]);
    memcpy(bytes, "test", 4);
    assert(heap_value(&heap, &thread, SKSE_X86_HEAP_SIZE, (uint32_t[]){a, 1, first}, 3) == 12);
    assert(heap_value(&heap, &thread, SKSE_X86_HEAP_SIZE, (uint32_t[]){b, 0, first}, 3) == UINT32_MAX);
    assert(heap_value(&heap, &thread, SKSE_X86_HEAP_SIZE,
                      (uint32_t[]){SKSE_X86_PROCESS_HEAP, 0, first}, 3) == UINT32_MAX);
    assert(!heap_value(&heap, &thread, SKSE_X86_HEAP_FREE, (uint32_t[]){a, 1, env}, 3));
    assert(!heap_value(&heap, &thread, SKSE_X86_HEAP_FREE, (uint32_t[]){b, 0, first}, 3));
    assert(!skse_x86_heap_release(&heap, first));
    assert(!heap_value(&heap, &thread, SKSE_X86_HEAP_REALLOC, (uint32_t[]){b, 0, first, 48}, 4));
    assert(!heap_value(&heap, &thread, SKSE_X86_HEAP_REALLOC, (uint32_t[]){a, 0x10, first, 48}, 4));

    const uint32_t resized = heap_value(&heap, &thread, SKSE_X86_HEAP_REALLOC, (uint32_t[]){a, 8, first, 48}, 4);
    assert(resized && resized != first && !skse_x86_heap_memory(&heap, first, 1, false));
    bytes = skse_x86_heap_memory(&heap, resized, 48, true);
    assert(bytes && !memcmp(bytes, "test", 4));
    for (unsigned i = 4; i < 48; ++i) assert(!bytes[i]);
    const uint32_t second = heap_value(&heap, &thread, SKSE_X86_HEAP_ALLOC, (uint32_t[]){a, 0, 1}, 3);
    assert(second && second != resized);
    assert(!heap_value(&heap, &thread, SKSE_X86_HEAP_DESTROY, (uint32_t[]){SKSE_X86_PROCESS_HEAP}, 1) &&
           thread.last_error == 6 && skse_x86_heap_size(&heap, process) == 16);
    assert(heap_value(&heap, &thread, SKSE_X86_HEAP_DESTROY, &a, 1));
    assert(!skse_x86_heap_memory(&heap, resized, 1, false) && !skse_x86_heap_memory(&heap, second, 1, false));
    assert(heap_value(&heap, &thread, SKSE_X86_HEAP_SIZE, (uint32_t[]){b, 0, other}, 3) == 16);
    assert(skse_x86_heap_size(&heap, process) == 16 && skse_x86_heap_memory(&heap, env, 1, false) &&
           !skse_x86_heap_memory(&heap, env, 1, true));
    assert(!heap_value(&heap, &thread, SKSE_X86_HEAP_DESTROY, &a, 1));

    const uint32_t c = heap_value(&heap, &thread, SKSE_X86_HEAP_CREATE, create, 3);
    assert(c && c != a && c != b);
    blank[0] = c;
    const uint32_t fresh = heap_value(&heap, &thread, SKSE_X86_HEAP_ALLOC, blank, 3);
    assert(fresh && !heap_value(&heap, &thread, SKSE_X86_HEAP_FREE, (uint32_t[]){a, 0, fresh}, 3));
    assert(heap_value(&heap, &thread, SKSE_X86_HEAP_FREE, (uint32_t[]){c, 1, fresh}, 3));
    assert(!heap_value(&heap, &thread, SKSE_X86_HEAP_ALLOC, (uint32_t[]){c, 0, SKSE_X86_HEAP_BYTES}, 3));

    for (unsigned i = 2; i < SKSE_X86_PRIVATE_HEAPS; ++i)
        assert(heap_value(&heap, &thread, SKSE_X86_HEAP_CREATE, create, 3));
    assert(!heap_value(&heap, &thread, SKSE_X86_HEAP_CREATE, create, 3) && thread.last_error == 8);
    assert(private_heap_call(&heap, &thread, SKSE_X86_HEAP_CREATE,
                             (uint32_t[]){0x40000, 0x1000, 0}, 3, 100).status == SKSE_X86_UNSUPPORTED_API);
    assert(private_heap_call(&heap, &thread, SKSE_X86_HEAP_CREATE,
                             (uint32_t[]){0, 0x1000, 0x2000}, 3, 100).status == SKSE_X86_UNSUPPORTED_API);
    skse_x86_heap_close(&heap);
    assert(!heap.bytes && !heap.count && !skse_x86_heap_private_live(&heap, c));
}

static SkseX86Result protect_worker(void *context, uint32_t id, uint32_t entry, uint32_t parameter, uint32_t budget) {
    SksePeImage *image = context;
    uint8_t worker_bytes[4096] = {0};
    const SkseX86Scratch work = {scratch_base + 8192, worker_bytes, sizeof worker_bytes};
    const SkseX86Service protect = {0x00300000, 0, SKSE_X86_PROTECT_IMAGE_DATA};
    SkseWinThread thread = {.id = id};
    const SkseX86Services services = {.entries = &protect, .count = 1, .thread = &thread};
    const uint32_t args[] = {parameter, 1, 2, work.base};
    return skse_x86_call(image, entry, work, args, 4, budget, services, SKSE_X86_STDCALL);
}

static void check_data_page_protection(void) {
    const uint32_t data_address = image_base + 4096;
    const unsigned protect_token = 0x00300000;
    const SkseX86Service protect = {protect_token, 0, SKSE_X86_PROTECT_IMAGE_DATA};
    SkseWinThread thread = {.id = 1, .last_error = 42};
    SkseX86Services services = {.entries = &protect, .count = 1, .thread = &thread};
    SksePeImage image = {
        .base = image_base, .size = 12288, .bytes = calloc(1, 12288), .section_count = 2,
        .sections = {{0, 256, SKSE_PE_READ | SKSE_PE_EXEC}, {4096, 8192, SKSE_PE_READ}}
    };
    assert(image.bytes);
    const SkseX86Scratch work = {scratch_base, scratch, sizeof scratch};
    uint8_t thunk[] = {0xb8, 0, 0, 0x30, 0, 0xff, 0xe0}; // Tail call the stdcall service.
    memcpy(image.bytes, thunk, sizeof thunk);

    const uint32_t make_writable[] = {data_address + 1, 1, 4, scratch_base};
    SkseX86Result result = skse_x86_call(&image, image_base, work, make_writable, 4, 100,
                                       services, SKSE_X86_STDCALL);
    assert(result.status == SKSE_X86_OK && result.eax == 1 && scratch[0] == 2 && thread.last_error == 42);

    const uint32_t mark_code[] = {image_base, 1, 4, scratch_base};
    result = skse_x86_call(&image, image_base, work, mark_code, 4, 100, services, SKSE_X86_STDCALL);
    assert(result.status == SKSE_X86_UNSUPPORTED_API && result.failed_service == protect_token);
    assert(result.protection_address == image_base && result.protection_length == 1 && result.protection_flags == 4);
    const uint32_t request_execution[] = {data_address, 1, 0x20, scratch_base};
    result = skse_x86_call(&image, image_base, work, request_execution, 4, 100, services, SKSE_X86_STDCALL);
    assert(result.status == SKSE_X86_UNSUPPORTED_API && result.failed_service == protect_token);
    assert(result.protection_address == data_address && result.protection_length == 1 && result.protection_flags == 0x20);
    const uint32_t bad_output[] = {data_address, 1, 2, 0};
    result = skse_x86_call(&image, image_base, work, bad_output, 4, 100, services, SKSE_X86_STDCALL);
    assert(result.status == SKSE_X86_OK && !result.eax && thread.last_error == 998);
    const uint32_t bad_range[] = {data_address, UINT32_MAX, 2, scratch_base};
    result = skse_x86_call(&image, image_base, work, bad_range, 4, 100, services, SKSE_X86_STDCALL);
    assert(result.status == SKSE_X86_OK && !result.eax && thread.last_error == 487);

    const uint32_t mark_unreadable[] = {data_address + 4096, 1, 1, scratch_base};
    result = skse_x86_call(&image, image_base, work, mark_unreadable, 4, 100, services, SKSE_X86_STDCALL);
    assert(result.status == SKSE_X86_OK && result.eax == 1 && scratch[0] == 2);
    assert(skse_x86_image_readable(&image, data_address, 4096));
    assert(!skse_x86_image_readable(&image, data_address + 4095, 2));

    // Cache the writable page with an ordinary store, then change its
    // protection and test another store in the same invocation.
    const uint8_t write_protect_write[] = {
        0xb8, 17, 0, 0, 0,                // mov eax, 17
        0xa3, 0, 0x10, 0x10, 0,           // mov [data_address], eax
        0x68, 0, 0, 0x20, 0,              // previous_at
        0x6a, 2, 0x6a, 1,                 // protection and length
        0x68, 0, 0x10, 0x10, 0,           // address
        0xb8, 0, 0, 0x30, 0, 0xff, 0xd0,  // call VirtualProtect
        0xa3, 0, 0x10, 0x10, 0, 0xc3
    };
    memcpy(image.bytes + 32, write_protect_write, sizeof write_protect_write);
    result = skse_x86_call(&image, image_base + 32, work, NULL, 0, 100, services, SKSE_X86_CDECL);
    assert(result.status == SKSE_X86_MEMORY_FAULT && image.bytes[4096] == 17 && scratch[0] == 4);

    result = skse_x86_call(&image, image_base, work, make_writable, 4, 100, services, SKSE_X86_STDCALL);
    assert(result.status == SKSE_X86_OK && result.eax == 1 && scratch[0] == 2);
    // A synchronous worker has a separate VM and stack. Its change to the
    // shared image must invalidate the parent's writable cache.
    const uint8_t write_wait_write[] = {
        0xb8, 19, 0, 0, 0,                // mov eax, 19
        0xa3, 0, 0x10, 0x10, 0,           // warm the writable cache
        0x6a, 0xff,                       // infinite wait
        0x68, 0, 0, 0, 0x50,              // worker handle
        0xb8, 4, 0, 0x30, 0, 0xff, 0xd0,  // call WaitForSingleObject
        0xa3, 0, 0x10, 0x10, 0, 0xc3
    };
    memcpy(image.bytes + 96, write_wait_write, sizeof write_wait_write);
    const SkseX86Service wait = {0x00300004, SKSE_WIN_WAIT_ONE, SKSE_X86_WIN_SYNC};
    SkseWinSync sync = {0};
    SkseX86Worker worker = {.run = protect_worker, .context = &image, .id = 2, .handle = 0x50000000,
                           .entry = image_base, .parameter = data_address, .open = true};
    services = (SkseX86Services){.entries = &wait, .count = 1, .thread = &thread, .sync = &sync,
                                .worker = &worker};
    result = skse_x86_call(&image, image_base + 96, work, NULL, 0, 100, services, SKSE_X86_CDECL);
    assert(worker.complete && !worker.failed && worker.exit_code == 1);
    assert(result.status == SKSE_X86_MEMORY_FAULT && image.bytes[4096] == 19);
    skse_pe_free(&image);
}

static void check_virtual_tls(void) {
    SkseWinThreadProcess process = {0};
    SkseWinThread thread = {.process = &process, .id = 3, .last_error = 0x1234};
    SkseWinThread worker = {.process = &process, .id = 4, .last_error = 0x4321};
    for (unsigned i = 0; i < SKSE_WIN_TLS_SLOTS; ++i)
        assert(skse_win_thread_call(&thread, SKSE_WIN_TLS_ALLOC, 0, 0) == i);
    assert(thread.last_error == 0x1234);
    assert(skse_win_thread_call(&thread, SKSE_WIN_TLS_ALLOC, 0, 0) == UINT32_MAX && thread.last_error == 8);
    assert(skse_win_thread_call(&thread, SKSE_WIN_TLS_GET, 109, 0) == 0 && thread.last_error == 0);
    assert(skse_win_thread_call(&thread, SKSE_WIN_SET_LAST_ERROR, 44, 0) == 0);
    assert(skse_win_thread_call(&thread, SKSE_WIN_TLS_SET, 109, 0x12345678) && thread.last_error == 44);
    assert(skse_win_thread_call(&worker, SKSE_WIN_TLS_GET, 109, 0) == 0 && worker.last_error == 0);
    assert(skse_win_thread_call(&worker, SKSE_WIN_TLS_SET, 109, 0x3400));
    assert(skse_win_thread_call(&worker, SKSE_WIN_SET_LAST_ERROR, 77, 0) == 0);
    assert(thread.last_error == 44);
    assert(skse_win_thread_call(&thread, SKSE_WIN_TLS_GET, 109, 0) == 0x12345678 && thread.last_error == 0);
    assert(worker.last_error == 77 && skse_win_thread_call(&worker, SKSE_WIN_TLS_GET, 109, 0) == 0x3400);
    assert(skse_win_thread_call(&thread, SKSE_WIN_TLS_FREE, 109, 0));
    assert(skse_win_thread_call(&thread, SKSE_WIN_TLS_GET, UINT32_MAX, 0) == 0 && thread.last_error == 87);
    assert(skse_win_thread_call(&thread, SKSE_WIN_TLS_ALLOC, 0, 0) == 109);
    assert(skse_win_thread_call(&thread, SKSE_WIN_TLS_GET, 109, 0) == 0 && thread.last_error == 0);
    assert(skse_win_thread_call(&worker, SKSE_WIN_TLS_GET, 109, 0) == 0 && worker.last_error == 0);
    assert(skse_win_thread_call(&thread, SKSE_WIN_CURRENT_THREAD_ID, 0, 0) == 3);
}

static void check_virtual_critical_sections(void) {
    SkseWinThreadProcess process = {0};
    SkseWinThread thread = {.process = &process, .id = 1, .last_error = 42};
    SkseWinThread worker = {.process = &process, .id = 2, .last_error = 66};
    const uint32_t base = 0x1234, size = SKSE_WIN_CRITICAL_SECTION_BYTES;
    for (unsigned i = 0; i < SKSE_WIN_CRITICAL_SECTIONS; ++i)
        assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_SPIN_INIT, base + i * size) == SKSE_WIN_CRITICAL_ACQUIRED);
    assert(thread.last_error == 42);
    assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_SPIN_INIT, base + SKSE_WIN_CRITICAL_SECTIONS * size) ==
           SKSE_WIN_CRITICAL_INVALID);
    assert(thread.last_error == 8);
    assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_ENTER, base) == SKSE_WIN_CRITICAL_ACQUIRED);
    assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_TRY, base) == SKSE_WIN_CRITICAL_ACQUIRED);
    assert(skse_win_critical_section(&worker, SKSE_WIN_CRITICAL_TRY, base) == SKSE_WIN_CRITICAL_CONTENDED);
    assert(worker.last_error == 66);
    assert(skse_win_critical_section(&worker, SKSE_WIN_CRITICAL_LEAVE, base) == SKSE_WIN_CRITICAL_INVALID);
    assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_LEAVE, base) == SKSE_WIN_CRITICAL_ACQUIRED);
    assert(skse_win_critical_section(&worker, SKSE_WIN_CRITICAL_ENTER, base) == SKSE_WIN_CRITICAL_CONTENDED);
    assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_DELETE, base) == SKSE_WIN_CRITICAL_INVALID);
    assert(thread.last_error == 87);
    assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_LEAVE, base) == SKSE_WIN_CRITICAL_ACQUIRED);
    assert(skse_win_critical_section(&worker, SKSE_WIN_CRITICAL_ENTER, base) == SKSE_WIN_CRITICAL_ACQUIRED);
    assert(skse_win_critical_section(&worker, SKSE_WIN_CRITICAL_LEAVE, base) == SKSE_WIN_CRITICAL_ACQUIRED);
    assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_DELETE, base) == SKSE_WIN_CRITICAL_ACQUIRED);
    assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_ENTER, base) == SKSE_WIN_CRITICAL_INVALID);
    assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_INIT, base) == SKSE_WIN_CRITICAL_ACQUIRED);
}

static void check_environment_block_ownership(void) {
    SkseX86Heap heap = {.base = 0x00300000};
    uint32_t address = skse_x86_heap_allocate_owned(&heap, 4, true, SKSE_X86_HEAP_ENVIRONMENT);
    assert(address && skse_x86_heap_memory(&heap, address, 4, false));
    assert(!skse_x86_heap_memory(&heap, address, 4, true));
    assert(!skse_x86_heap_release(&heap, address));
    assert(!skse_x86_heap_resize(&heap, address, 8, false, false));
    assert(!skse_x86_heap_release_owned(&heap, address + 2, SKSE_X86_HEAP_ENVIRONMENT));
    assert(skse_x86_heap_release_owned(&heap, address, SKSE_X86_HEAP_ENVIRONMENT));
    assert(!skse_x86_heap_memory(&heap, address, 4, false));
    uint32_t reused = skse_x86_heap_allocate(&heap, 8, false);
    assert(reused == address && !skse_x86_heap_release_owned(&heap, reused, SKSE_X86_HEAP_ENVIRONMENT));
    assert(skse_x86_heap_memory(&heap, reused, 8, true) && skse_x86_heap_release(&heap, reused));
    skse_x86_heap_close(&heap);
}

static void check_static_tls_address(void) {
    SkseX86Services tls = {.static_tls_table = scratch_base + 16, .stack_floor = scratch_base + 256};
    uint8_t direct[] = {0x64, 0xa1, 0x2c, 0, 0, 0, 0xc3};
    uint8_t indirect[] = {0x64, 0x8b, 0x0d, 0x2c, 0, 0, 0, 0x89, 0xc8, 0xc3};
    assert(ARRAY_RUN(direct).status == SKSE_X86_UNSUPPORTED_INSTRUCTION);
    SkseX86Result result = run(direct, sizeof direct, 100, tls);
    assert(result.status == SKSE_X86_OK && result.eax == tls.static_tls_table && result.work == 2);
    result = run(indirect, sizeof indirect, 100, tls);
    assert(result.status == SKSE_X86_OK && result.eax == tls.static_tls_table);
    direct[2] = 0x18;
    assert(run(direct, sizeof direct, 100, tls).status == SKSE_X86_UNSUPPORTED_INSTRUCTION);
    uint8_t store[] = {0x64, 0xa3, 0x2c, 0, 0, 0, 0xc3};
    assert(run(store, sizeof store, 100, tls).status == SKSE_X86_UNSUPPORTED_INSTRUCTION);
    uint8_t underflow[] = {0xbc, 0, 1, 0x20, 0, 0x50, 0xc3};
    assert(run(underflow, sizeof underflow, 100, tls).status == SKSE_X86_MEMORY_FAULT);
    tls.static_tls_table = tls.stack_floor;
    assert(run(indirect, sizeof indirect, 100, tls).status == SKSE_X86_BAD_ARGUMENT);
}

static void check_exception_registrations(void) {
    SkseWinThreadProcess process = {0};
    SkseWinThread thread = {.process = &process, .id = 7};
    SkseX86Services services = {.thread = &thread, .stack_floor = scratch_base + 256};
    uint8_t frame[] = {
        0x68, 0, 0, 0, 0,                     // push an executable handler address
        0x64, 0xff, 0x35, 0, 0, 0, 0,         // push fs:[0]
        0x89, 0xe0,                           // mov eax, esp
        0x64, 0xa3, 0, 0, 0, 0,              // mov fs:[0], eax
        0x64, 0x8b, 0x0d, 0, 0, 0, 0,         // mov ecx, fs:[0]
        0x31, 0xc0, 0x39, 0xe1, 0x0f, 0x94, 0xc0, // return whether this frame was installed
        0x64, 0x8f, 0x05, 0, 0, 0, 0,         // pop fs:[0]
        0x83, 0xc4, 4, 0xc3                    // remove the handler and return
    };
    size_t pointer_offset = 1;
    append_word(frame, &pointer_offset, image_base + sizeof frame - 1);
    assert(ARRAY_RUN(frame).status == SKSE_X86_UNSUPPORTED_INSTRUCTION);
    SkseX86Result result = run(frame, sizeof frame, 100, services);
    assert(result.status == SKSE_X86_OK && result.eax == 1);

    uint8_t outside[] = {0xb8, 0, 0, 0, 0, 0x64, 0xa3, 0, 0, 0, 0, 0xc3};
    pointer_offset = 1;
    append_word(outside, &pointer_offset, scratch_base);
    assert(run(outside, sizeof outside, 100, services).status == SKSE_X86_MEMORY_FAULT);
    pointer_offset = 1;
    append_word(frame, &pointer_offset, scratch_base);
    assert(run(frame, sizeof frame, 100, services).status == SKSE_X86_BAD_ARGUMENT);
    pointer_offset = 1;
    append_word(frame, &pointer_offset, image_base + sizeof frame - 1);
    memset(frame + sizeof frame - 11, 0x90, 7); // Leave the link installed.
    frame[sizeof frame - 2] = 8;                // Restore the ordinary stack.
    assert(run(frame, sizeof frame, 100, services).status == SKSE_X86_BAD_CALL_RETURN);
}

static void check_cached_image_bounds(void) {
    uint8_t bytes[128] = {0};
    // Read one word from a read-only region, then try writing through the
    // populated data cache. The second operation must still see permissions.
    const uint8_t read_then_write[] = {
        0xa1, 0x40, 0, 0x10, 0, 0xa3, 0x40, 0, 0x10, 0, 0xc3
    };
    memcpy(bytes, read_then_write, sizeof read_then_write);
    bytes[0x40] = 91;
    SksePeImage image = {
        .base = image_base, .size = sizeof bytes, .bytes = bytes, .section_count = 2,
        .sections = {{0, 0x20, SKSE_PE_READ | SKSE_PE_EXEC}, {0x40, 8, SKSE_PE_READ}}
    };
    SkseX86Scratch work = {scratch_base, scratch, sizeof scratch};
    SkseX86Result result = skse_x86_call(&image, image_base, work, NULL, 0, 100,
                                        (SkseX86Services){0}, SKSE_X86_CDECL);
    assert(result.status == SKSE_X86_MEMORY_FAULT && bytes[0x40] == 91);
    // Replace the write with a read that crosses this same region's end.
    bytes[5] = 0xa1;
    bytes[6] = 0x46;
    result = skse_x86_call(&image, image_base, work, NULL, 0, 100,
                          (SkseX86Services){0}, SKSE_X86_CDECL);
    assert(result.status == SKSE_X86_MEMORY_FAULT);

    // A failed comparison still requires write permission on the target.
    const uint8_t compare[] = {0x31, 0xc0, 0xf0, 0x0f, 0xb1, 0x0d, 0x40, 0, 0x10, 0, 0xc3};
    memcpy(bytes, compare, sizeof compare);
    result = skse_x86_call(&image, image_base, work, NULL, 0, 100, (SkseX86Services){0}, SKSE_X86_CDECL);
    assert(result.status == SKSE_X86_MEMORY_FAULT && bytes[0x40] == 91);
    uint8_t register_lock[] = {0xf0, 0x0f, 0xc1, 0xc9, 0xc3};
    assert(ARRAY_RUN(register_lock).status == SKSE_X86_UNSUPPORTED_INSTRUCTION);
}

static void check_services_need_live_state(void) {
    SkseWinSync events = {0};
    SkseWinThread thread = {.id = 1};
    uint32_t token = skse_win_event_create(&events, false, false);
    const SkseX86Service binding = {0x00401000, SKSE_WIN_WAIT_ONE, SKSE_X86_WIN_SYNC};
    SkseX86Services services = {.entries = &binding, .count = 1, .sync = &events, .thread = &thread};
    uint8_t wait[18] = {0};
    size_t length = 0;
    wait[length++] = 0x68;
    append_word(wait, &length, UINT32_MAX);
    wait[length++] = 0x68;
    append_word(wait, &length, token);
    wait[length++] = 0xb8;
    append_word(wait, &length, binding.address);
    wait[length++] = 0xff;
    wait[length++] = 0xd0;
    wait[length++] = 0xc3;
    SkseX86Result blocked = run(wait, length, 50, services);
    assert(blocked.status == SKSE_X86_UNSUPPORTED_API && blocked.failed_service == binding.address);
    assert(skse_win_event_signal(&events, token, true));
    SkseX86Result result = run(wait, length, 50, services);
    assert(result.status == SKSE_X86_OK && result.eax == 0 && !result.failed_service);

    SkseX86Serialization serial = {.interface_address = scratch_base + 64, .owner_handle = 12};
    const SkseX86Service resolver = {0x00401000, SKSE_SERIAL_RESOLVE_FORM, SKSE_X86_SERIALIZATION};
    services = (SkseX86Services){.entries = &resolver, .count = 1, .serialization = &serial};
    assert(run(wait + 10, length - 10, 50, services).status == SKSE_X86_UNSUPPORTED_API);
    assert(skse_x86_records_count(&serial.records) == 0);
}

static void check_service_index(void) {
    // These two validated tokens have the same initial bucket.
    SkseX86Service bindings[] = {
        {0x00400004, 11, SKSE_X86_SCALAR_RETURN},
        {0x004004c8, 22, SKSE_X86_SCALAR_RETURN}
    };
    SkseX86Services services = {.entries = bindings, .count = 2};
    uint8_t call[] = {0xb8, 0xc8, 0x04, 0x40, 0, 0xff, 0xd0, 0xc3};
    SkseX86Result result = run(call, sizeof call, 20, services);
    assert(result.status == SKSE_X86_OK && result.eax == 22 && result.service_calls[1] == 1);
    bindings[1].address = bindings[0].address;
    assert(run(call, sizeof call, 20, services).status == SKSE_X86_BAD_ARGUMENT);
}

static void hook_protect_request(uint8_t *code, size_t *used, uint32_t site, uint8_t flags) {
    code[(*used)++] = 0x68;
    append_word(code, used, scratch_base);
    code[(*used)++] = 0x6a;
    code[(*used)++] = flags;
    code[(*used)++] = 0x6a;
    code[(*used)++] = 4;
    code[(*used)++] = 0x68;
    append_word(code, used, site);
    code[(*used)++] = 0xb8;
    append_word(code, used, 0x00400000);
    code[(*used)++] = 0xff;
    code[(*used)++] = 0xd0;
}

static void check_hook_observations(void) {
    const uint32_t site = 0x00600000, target = image_base;
    const SkseX86Service protect = {0x00400000, 0, SKSE_X86_PROTECT_IMAGE_DATA};
    SkseWinThread thread = {.id = 1};
    SkseX86Services services = {
        .entries = &protect, .count = 1, .thread = &thread, .hook_probe_sites = &site, .hook_probe_count = 1
    };
    uint8_t code[128];
    size_t used = 0;
    hook_protect_request(code, &used, site, 0x40);
    const size_t opcode_store = used;
    code[used++] = 0xc6; code[used++] = 5;
    append_word(code, &used, site);
    code[used++] = 0xe9;
    hook_protect_request(code, &used, site, 0x20);
    const size_t first_window = used;
    hook_protect_request(code, &used, site + 1, 0x40);
    code[used++] = 0xc7; code[used++] = 5;
    append_word(code, &used, site + 1);
    const size_t operand = used;
    append_word(code, &used, target - site - 5);
    hook_protect_request(code, &used, site + 1, 0x20);
    code[used++] = 0xc3;

    SkseX86Result result = run(code, used, 100, services);
    assert(result.status == SKSE_X86_OK && result.hook_count == 1 &&
           result.hooks[0].site == site && result.hooks[0].target == target);
    services.hook_probe_count = 0;
    result = run(code, used, 100, services);
    assert(result.status == SKSE_X86_UNSUPPORTED_API && !result.hook_count &&
           result.protection_address == site && result.protection_flags == 0x40);
    services.hook_probe_count = 1;

    size_t intermediate = first_window;
    const uint8_t first_next = code[intermediate];
    code[intermediate++] = 0xc3;
    result = run(code, intermediate, 100, services);
    assert(result.status == SKSE_X86_UNSUPPORTED_API && !result.hook_count);
    code[first_window] = first_next;
    size_t displacement = operand;
    append_word(code, &displacement, scratch_base - site - 5);
    result = run(code, used, 100, services);
    assert(result.status == SKSE_X86_UNSUPPORTED_API && !result.hook_count);
    uint8_t raw_store[8];
    memcpy(raw_store, code + opcode_store, sizeof raw_store - 1);
    raw_store[sizeof raw_store - 1] = 0xc3;
    result = run(raw_store, sizeof raw_store, 100, services);
    assert(result.status == SKSE_X86_MEMORY_FAULT && result.fault_address == site && !result.hook_count);

    const uint32_t conflicting[] = {site, site + 1};
    services.hook_probe_sites = conflicting;
    services.hook_probe_count = 2;
    assert(run(code, used, 100, services).status == SKSE_X86_BAD_ARGUMENT);
}

static void check_code_write_audit(void) {
    uint8_t bytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const uint8_t original[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const SkseX86ReadView view = {.base = 0x00600000, .bytes = bytes, .length = sizeof bytes};
    const SkseX86Service protect = {0x00400000, 0, SKSE_X86_PROTECT_IMAGE_DATA};
    SkseWinThread thread = {.id = 1};
    SkseX86CodeAudit audit = {0};
    SkseX86Services services = {.entries = &protect, .count = 1, .thread = &thread,
                                .read_views = &view, .read_view_count = 1, .code_audit = &audit};
    uint8_t code[1024];
    size_t used = 0;
    hook_protect_request(code, &used, view.base + 1, 0x40);
    const size_t store = used;
    code[used++] = 0xc7;
    code[used++] = 5;
    const size_t operand = used;
    append_word(code, &used, view.base + 1);
    append_word(code, &used, 0x11223344);
    const size_t after_store = used;
    hook_protect_request(code, &used, view.base + 1, 0x20);
    // A read after the trial write still sees the original bytes.
    code[used++] = 0xa1;
    append_word(code, &used, view.base + 1);
    code[used++] = 0xc3;
    SkseX86Result result = run(code, used, 100, services);
    assert(result.status == SKSE_X86_OK && result.eax == 0x05040302 && audit.count == 1 && !audit.truncated);
    assert(audit.writes[0].address == view.base + 1 &&
           audit.writes[0].value == 0x11223344 && audit.writes[0].width == 4);
    assert(!memcmp(bytes, original, sizeof bytes));

    size_t changed = operand;
    append_word(code, &changed, view.base + 2);
    result = run(code, used, 100, services);
    assert(result.status == SKSE_X86_MEMORY_FAULT && audit.count == 0);
    changed = operand;
    append_word(code, &changed, view.base + 1);

    code[after_store] = 0xc3;
    result = run(code, after_store + 1, 100, services);
    assert(result.status == SKSE_X86_UNSUPPORTED_API && audit.count == 1 && !audit.truncated);
    used = after_store;
    for (int i = 0; i < SKSE_X86_CODE_WRITE_LIMIT; ++i) {
        const size_t count = after_store - store;
        memcpy(code + used, code + store, count);
        used += count;
    }
    code[used++] = 0xc3;
    result = run(code, used, 300, services);
    assert(result.status == SKSE_X86_UNSUPPORTED_API && audit.count == SKSE_X86_CODE_WRITE_LIMIT && audit.truncated);
    assert(!memcmp(bytes, original, sizeof bytes));

    audit.read_view_index = 1;
    assert(run(code, used, 300, services).status == SKSE_X86_BAD_ARGUMENT);
}

static void check_read_only_views(void) {
    uint8_t bytes[] = {0x78, 0x56, 0x34, 0x12, 0xc3, 0, 0, 0};
    const uint8_t original[] = {0x78, 0x56, 0x34, 0x12, 0xc3, 0, 0, 0};
    SkseX86ReadView view = {.base = 0x00300000, .bytes = bytes, .length = sizeof bytes};
    SkseX86Services services = {.read_views = &view, .read_view_count = 1};
    uint8_t read[] = {0xa1, 0, 0, 0x30, 0, 0xc3};
    SkseX86Result result = run(read, sizeof read, 10, services);
    assert(result.status == SKSE_X86_OK && result.eax == 0x12345678);
    result = ARRAY_RUN(read);
    assert(result.status == SKSE_X86_MEMORY_FAULT && result.fault_address == view.base &&
           result.fault_access == SKSE_PE_READ);
    uint8_t writes_after_read[] = {0xa1, 0, 0, 0x30, 0, 0xa3, 1, 0, 0x30, 0, 0xc3};
    result = run(writes_after_read, sizeof writes_after_read, 10, services);
    assert(result.status == SKSE_X86_MEMORY_FAULT && result.fault_address == view.base + 1 &&
           result.fault_access == SKSE_PE_WRITE && !memcmp(bytes, original, sizeof bytes));
    uint8_t execute[] = {0xb8, 4, 0, 0x30, 0, 0xff, 0xd0, 0xc3};
    result = run(execute, sizeof execute, 10, services);
    assert(result.status == SKSE_X86_MEMORY_FAULT && result.fault_address == view.base + 4 &&
           result.fault_access == SKSE_PE_EXEC);
    read[1] = 6;
    result = run(read, sizeof read, 10, services);
    assert(result.status == SKSE_X86_MEMORY_FAULT && result.fault_address == view.base + 6);
    read[1] = 0;
    SkseX86ReadView overlapping[] = {view, view};
    services.read_views = overlapping;
    services.read_view_count = 2;
    assert(run(read, sizeof read, 10, services).status == SKSE_X86_BAD_ARGUMENT);
    services.read_view_count = 1;
    overlapping[0].base = scratch_base;
    assert(run(read, sizeof read, 10, services).status == SKSE_X86_BAD_ARGUMENT);
    overlapping[0].base = image_base;
    assert(run(read, sizeof read, 10, services).status == SKSE_X86_BAD_ARGUMENT);
    overlapping[0].base = UINT32_MAX - 2;
    assert(run(read, sizeof read, 10, services).status == SKSE_X86_BAD_ARGUMENT);
    overlapping[0].base = view.base;
    SkseX86Service alias = {view.base, 0, SKSE_X86_SCALAR_RETURN};
    services.entries = &alias;
    services.count = 1;
    assert(run(read, sizeof read, 10, services).status == SKSE_X86_BAD_ARGUMENT);
}

static SkseX86Result run_byte_scan(const SkseX86ReadView *view, uint8_t prefix, bool backwards,
                                   uint32_t address, uint32_t count, uint8_t accumulator, uint32_t limit) {
    uint8_t code[48] = {0x57, 0xbf}; // Preserve EDI. MOV EDI, address.
    size_t used = 2;
    append_word(code, &used, address);
    code[used++] = 0xb9; // MOV ECX, count.
    append_word(code, &used, count);
    code[used++] = 0xb0; // MOV AL, accumulator.
    code[used++] = accumulator;
    code[used++] = 0xf9; // STC, giving a recognizable initial flags value.
    if (backwards) code[used++] = 0xfd;
    if (prefix) code[used++] = prefix;
    code[used++] = 0xae; // SCASB.
    code[used++] = 0x89;
    code[used++] = 0x3d; // MOV [scratch], EDI.
    append_word(code, &used, scratch_base);
    code[used++] = 0x89;
    code[used++] = 0x0d; // MOV [scratch + 4], ECX.
    append_word(code, &used, scratch_base + 4);
    const uint8_t tail[] = {0x9c, 0x58, 0xfc, 0x5f, 0xc3}; // Return flags and restore DF and EDI.
    memcpy(code + used, tail, sizeof tail);
    return run(code, used + sizeof tail, limit, (SkseX86Services){.read_views = view, .read_view_count = 1});
}

static void check_byte_scan(void) {
    uint8_t bytes[] = {5, 5, 0};
    const uint32_t base = 0x00400000;
    const SkseX86ReadView view = {base, bytes, sizeof bytes};
    const struct {
        uint8_t prefix, accumulator;
        bool backwards;
        uint32_t address, count, remaining, ending_address, flags;
        unsigned comparisons;
    } cases[] = {
        {0xf2, 0, false, base, UINT32_MAX, UINT32_MAX - 3, base + 3, 0x246, 3},
        {0xf3, 5, false, base, 5, 2, base + 3, 0x206, 3},
        {0xf2, 5, true, base + 2, 3, 1, base, 0x646, 2},
        {0xf2, 3, false, base, 2, 0, base + 2, 0x293, 2},
        {0xf2, 1, false, UINT32_MAX, 0, 0, UINT32_MAX, 0x203, 0},
        {0,    5, false, base, 0, 0, base + 1, 0x246, 0}
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const SkseX86Result result = run_byte_scan(&view, cases[i].prefix, cases[i].backwards,
                                                   cases[i].address, cases[i].count, cases[i].accumulator, 100);
        assert(result.status == SKSE_X86_OK && result.eax == cases[i].flags);
        assert(result.work - result.instructions == cases[i].comparisons);
        uint8_t expected[8];
        size_t offset = 0;
        append_word(expected, &offset, cases[i].ending_address);
        append_word(expected, &offset, cases[i].remaining);
        assert(!memcmp(scratch, expected, sizeof expected));
    }
    const SkseX86Result read_fault = run_byte_scan(&view, 0xf2, false, base + 2, 2, 1, 100);
    assert(read_fault.status == SKSE_X86_MEMORY_FAULT && read_fault.fault_address == base + 3 &&
           read_fault.fault_access == SKSE_PE_READ);
    const SkseX86Result budget = run_byte_scan(&view, 0xf2, false, base, UINT32_MAX, 1, 8);
    assert(budget.status == SKSE_X86_INSTRUCTION_LIMIT && budget.instructions == 6 && budget.work == 8);
}

static void check_win32_guest_flags(void) {
    uint8_t initial[] = {0x9c, 0x58, 0xc3}; // PUSHFD; POP EAX; RET
    SkseX86Result result = ARRAY_RUN(initial);
    assert(result.status == SKSE_X86_OK && result.eax == 0x202);

    uint8_t set_all[] = {0x6a, 0xff, 0x9d, 0x9c, 0x58, 0xfc, 0xc3};
    result = ARRAY_RUN(set_all);
    assert(result.status == SKSE_X86_OK && result.eax == 0xed7);

    uint8_t clear_all[] = {0x6a, 0xff, 0x9d, 0x6a, 0, 0x9d, 0x9c, 0x58, 0xc3};
    result = ARRAY_RUN(clear_all);
    assert(result.status == SKSE_X86_OK && result.eax == 0x202);

    uint8_t carry_and_add[] = {0xb0, 0x0f, 0xf9, 0x14, 0, 0x9c, 0x58, 0xc3};
    result = ARRAY_RUN(carry_and_add);
    assert(result.status == SKSE_X86_OK && result.eax == 0x212);

    uint8_t borrow_and_subtract[] = {0xb0, 0x10, 0xf9, 0x1c, 0, 0x9c, 0x58, 0xc3};
    result = ARRAY_RUN(borrow_and_subtract);
    assert(result.status == SKSE_X86_OK && result.eax == 0x216);

    uint8_t identify[] = {
        0x9c, 0x58, 0x89, 0xc1,             // Read and save EFLAGS.
        0x35, 0, 0, 0x20, 0, 0x50, 0x9d,    // Try to change ID.
        0x9c, 0x5a, 0x29, 0xca, 0x74, 2,    // Compare and branch past CPUID if unchanged.
        0x0f, 0xa2, 0xc3
    };
    result = ARRAY_RUN(identify);
    assert(result.status == SKSE_X86_OK && result.instructions == 11);
    uint8_t cpuid[] = {0x0f, 0xa2, 0xc3};
    result = ARRAY_RUN(cpuid);
    assert(result.status == SKSE_X86_UNSUPPORTED_INSTRUCTION && result.instruction_va == image_base);
    for (uint32_t feature = 0; feature < 64; ++feature)
        assert(skse_win_processor_feature(feature) == (feature == 12));
}

int main(void) {
    check_win32_guest_flags();
    check_byte_scan();
    // CALL forwards to another x86 region and that callee returns into the caller.
    uint8_t nested[] = {0xe8, 1, 0, 0, 0, 0xc3, 0xb8, 42, 0, 0, 0, 0xc3};
    SkseX86Result result = ARRAY_RUN(nested);
    assert(result.status == SKSE_X86_OK && result.eax == 42 && result.instructions == 4);

    uint8_t overflow[] = {0xb8, 0xff, 0xff, 0xff, 0x7f, 0x83, 0xc0, 1, 0x70, 2, 0x31, 0xc0, 0xc3};
    result = ARRAY_RUN(overflow);
    assert(result.status == SKSE_X86_OK && result.eax == UINT32_C(0x80000000));

    uint8_t byte_increment[] = {
        0xb8, 0, 0x7f, 0x34, 0x12, 0xf9, 0xfe, 0xc4, 0x0f, 0x90, 0xc0, 0x73, 1, 0xc3, 0xcc
    };
    result = ARRAY_RUN(byte_increment);
    assert(result.status == SKSE_X86_OK && result.eax == 0x12348001);
    uint8_t byte_decrement[] = {
        0xf8, 0xfe, 0x0d, 0x40, 0, 0x20, 0, 0x72, 8,
        0x0f, 0xb6, 0x05, 0x40, 0, 0x20, 0, 0xc3, 0xcc
    };
    result = ARRAY_RUN(byte_decrement);
    assert(result.status == SKSE_X86_OK && result.eax == 0xff);
    uint8_t invalid_byte_group[] = {0xfe, 0xd0, 0xc3};
    assert(ARRAY_RUN(invalid_byte_group).status == SKSE_X86_UNSUPPORTED_INSTRUCTION);

    uint8_t never_returns[] = {0xeb, 0xfe};
    result = ARRAY_RUN(never_returns);
    assert(result.status == SKSE_X86_INSTRUCTION_LIMIT && result.instructions == 100);

    uint8_t modifies_code[] = {0xc7, 0x05, 0, 0, 0x10, 0, 55, 0, 0, 0, 0xc3};
    result = ARRAY_RUN(modifies_code);
    assert(result.status == SKSE_X86_MEMORY_FAULT && result.fault_access == SKSE_PE_WRITE &&
           result.fault_address == image_base);
    assert(modifies_code[0] == 0xc7);

    uint8_t executes_data[] = {0xb8, 0x30, 0, 0x20, 0, 0xff, 0xe0};
    result = ARRAY_RUN(executes_data);
    assert(result.status == SKSE_X86_MEMORY_FAULT && result.fault_access == SKSE_PE_EXEC &&
           result.fault_address == scratch_base + 0x30);

    uint8_t prefixed_return[] = {0xb8, 29, 0, 0, 0, 0xf3, 0xc3};
    result = ARRAY_RUN(prefixed_return);
    assert(result.status == SKSE_X86_OK && result.eax == 29 && result.instructions == 2);

    uint8_t clear_x87[] = {0xb8, 29, 0, 0, 0, 0xdb, 0xe2, 0xc3};
    result = ARRAY_RUN(clear_x87);
    assert(result.status == SKSE_X86_OK && result.eax == 29 && result.instructions == 3);

    uint8_t incomplete[] = {0xb8, 0, 0};
    result = ARRAY_RUN(incomplete);
    assert(result.status == SKSE_X86_MEMORY_FAULT && result.fault_access == SKSE_PE_EXEC &&
           result.fault_address == image_base + 1);

    uint8_t split_code[] = {0xb8, 0x78, 0x56, 0x34, 0x12, 0xc3};
    SksePeImage split = {
        .base = image_base, .size = sizeof split_code, .bytes = split_code, .section_count = 2,
        .sections = {{0, 5, SKSE_PE_READ | SKSE_PE_EXEC}, {5, 1, SKSE_PE_READ | SKSE_PE_EXEC}}
    };
    const SkseX86Scratch stack = {scratch_base, scratch, sizeof scratch};
    result = skse_x86_call(&split, image_base, stack, NULL, 0, 100, (SkseX86Services){0}, SKSE_X86_CDECL);
    assert(result.status == SKSE_X86_OK && result.eax == UINT32_C(0x12345678) && result.instructions == 2);
    split.sections[0].length = 4;
    split.sections[1] = (SksePeSection){4, 2, SKSE_PE_READ};
    result = skse_x86_call(&split, image_base, stack, NULL, 0, 100, (SkseX86Services){0}, SKSE_X86_CDECL);
    assert(result.status == SKSE_X86_MEMORY_FAULT && result.fault_access == SKSE_PE_EXEC &&
           result.fault_address == image_base + 1);

    uint8_t unknown[] = {0xcc, 0xc3};
    assert(ARRAY_RUN(unknown).status == SKSE_X86_UNSUPPORTED_INSTRUCTION);

    // BT dword ptr [0x200000], ECX. The signed word displacement falls before scratch.
    uint8_t outside_bit_string[] = {0xb9, 0xff, 0xff, 0xff, 0xff, 0x0f, 0xa3, 0x0d, 0, 0, 0x20, 0, 0xc3};
    result = ARRAY_RUN(outside_bit_string);
    assert(result.status == SKSE_X86_MEMORY_FAULT && result.fault_access == SKSE_PE_READ &&
           result.fault_address == scratch_base - 4);
    // BTS modifies the selected dword and CF contains the previous bit.
    uint8_t set_bit_string[] = {
        0x31, 0xc0, 0xb9, 0xff, 0xff, 0xff, 0xff,
        0x0f, 0xab, 0x0d, 4, 0, 0x20, 0, // Set bit 31 in the preceding dword.
        0x72, 11,                           // Initial CF must be clear.
        0x0f, 0xab, 0x0d, 4, 0, 0x20, 0,
        0x0f, 0x92, 0xc0, 0xc3, 0x0f, 0x0b
    };
    result = ARRAY_RUN(set_bit_string);
    assert(result.status == SKSE_X86_OK && result.eax == 1 && scratch[3] == 0x80 && !scratch[4]);
    uint8_t set_bit_code[] = {0x31, 0xc9, 0x0f, 0xab, 0x0d, 0, 0, 0x10, 0, 0xc3};
    result = ARRAY_RUN(set_bit_code);
    assert(result.status == SKSE_X86_MEMORY_FAULT && result.fault_access == SKSE_PE_WRITE &&
           result.fault_address == image_base && set_bit_code[0] == 0x31);
    uint8_t set_bit_register[] = {
        0xb8, 0, 0x80, 0, 0, 0xb9, 17, 0, 0, 0, 0x66, 0x0f, 0xab, 0xc8, 0xc3
    };
    result = ARRAY_RUN(set_bit_register);
    assert(result.status == SKSE_X86_OK && result.eax == 0x8002);
    uint8_t locked_bit_test[] = {0xf0, 0x0f, 0xba, 0x25, 0, 0, 0x20, 0, 1, 0xc3};
    assert(ARRAY_RUN(locked_bit_test).status == SKSE_X86_UNSUPPORTED_INSTRUCTION);

    // A zero repeat count doesn't address memory or change ECX, ESI or EDI.
    uint8_t empty_copy[] = {0x57, 0xbf, 0xfe, 0xff, 0xff, 0xff, 0x31, 0xc9, 0xf3, 0xaa, 0x5f, 0xc3};
    assert(ARRAY_RUN(empty_copy).status == SKSE_X86_OK);
    uint8_t copy_over_budget[] = {0xbf, 0, 0, 0x20, 0, 0xb9, 0xff, 0xff, 0xff, 0xff, 0xf3, 0xaa, 0xc3};
    assert(ARRAY_RUN(copy_over_budget).status == SKSE_X86_INSTRUCTION_LIMIT && scratch[0] == 0);
    uint8_t crosses_scratch[] = {0xbf, 0xff, 0x0f, 0x20, 0, 0xb9, 2, 0, 0, 0, 0xb0, 0x33, 0xf3, 0xaa, 0xc3};
    assert(ARRAY_RUN(crosses_scratch).status == SKSE_X86_MEMORY_FAULT && scratch[sizeof scratch - 1] == 0);
    uint8_t leaves_direction_flag[] = {0xfd, 0xc3};
    assert(ARRAY_RUN(leaves_direction_flag).status == SKSE_X86_BAD_CALL_RETURN);

    // Aligned SSE encodings require alignment even inside allowed scratch.
    uint8_t unaligned_movaps[] = {0x0f, 0x28, 0x05, 1, 0, 0x20, 0, 0xc3};
    assert(ARRAY_RUN(unaligned_movaps).status == SKSE_X86_MEMORY_FAULT);
    uint8_t unaligned_movups[] = {0x0f, 0x10, 0x05, 1, 0, 0x20, 0, 0xc3};
    assert(ARRAY_RUN(unaligned_movups).status == SKSE_X86_OK);
    uint8_t truncated_movups[] = {0x0f, 0x10, 0x05, 0xf8, 0xf, 0x20, 0, 0xc3};
    assert(ARRAY_RUN(truncated_movups).status == SKSE_X86_MEMORY_FAULT);
    uint8_t vector_to_code[] = {0xf3, 0x0f, 0x7f, 0x05, 0, 0, 0x10, 0, 0xc3};
    assert(ARRAY_RUN(vector_to_code).status == SKSE_X86_MEMORY_FAULT && vector_to_code[0] == 0xf3);
    uint8_t low_double[] = {
        0x66, 0x0f, 0x74, 0xc0,                     // fill xmm0 with FF
        0x66, 0x0f, 0x13, 0x05, 0x68, 0, 0x20, 0, // seed the low store range
        0xc6, 0x05, 0x70, 0, 0x20, 0, 0xa5,       // neighboring canary
        0x66, 0x0f, 0x12, 0x05, 0x40, 0, 0x20, 0, // load low 8 zero bytes
        0x66, 0x0f, 0x7f, 0x05, 0x50, 0, 0x20, 0, // store all lanes
        0x66, 0x0f, 0x13, 0x05, 0x68, 0, 0x20, 0, // replace half of next sentinel
        0xc3
    };
    result = ARRAY_RUN(low_double);
    assert(result.status == SKSE_X86_OK);
    for (unsigned i = 0; i < 16; ++i) assert(scratch[0x50 + i] == (i < 8 ? 0 : 0xff));
    for (unsigned i = 0; i < 8; ++i) assert(!scratch[0x68 + i]);
    assert(scratch[0x70] == 0xa5);
    uint8_t invalid_low_double[] = {0x66, 0x0f, 0x13, 0xc1, 0xc3};
    assert(ARRAY_RUN(invalid_low_double).status == SKSE_X86_UNSUPPORTED_INSTRUCTION);

    uint8_t divide_by_zero[] = {0x31, 0xc9, 0xf7, 0xf1, 0xc3};
    assert(ARRAY_RUN(divide_by_zero).status == SKSE_X86_DIVIDE_FAULT);
    // EDX:EAX = INT64_MIN / -1 would overflow even native int64 division.
    uint8_t divide_overflow[] = {0xba, 0, 0, 0, 0x80, 0x31, 0xc0, 0x83, 0xc9, 0xff, 0xf7, 0xf9, 0xc3};
    assert(ARRAY_RUN(divide_overflow).status == SKSE_X86_DIVIDE_FAULT);
    uint8_t byte_divide[] = {0x66, 0xb8, 0xf1, 0xff, 0xb1, 3, 0xf6, 0xf9, 0xc3};
    result = ARRAY_RUN(byte_divide);
    assert(result.status == SKSE_X86_OK && result.eax == 0xfb);

    uint8_t clobbers_ebx[] = {0x31, 0xdb, 0xc3};
    assert(ARRAY_RUN(clobbers_ebx).status == SKSE_X86_BAD_CALL_RETURN);

    uint8_t fake_return[] = {0x6a, 0, 0xc3};
    assert(ARRAY_RUN(fake_return).status == SKSE_X86_BAD_CALL_RETURN);

    uint8_t stdcall_return[] = {0xc2, 8, 0};
    assert(ARRAY_RUN(stdcall_return).status == SKSE_X86_BAD_CALL_RETURN);
    SksePeImage stdcall_image = {
        .base = image_base, .size = sizeof stdcall_return, .bytes = stdcall_return, .section_count = 1,
        .sections = {{0, sizeof stdcall_return, SKSE_PE_READ | SKSE_PE_EXEC}}
    };
    const uint32_t stdcall_args[] = {12, 34};
    result = skse_x86_call(&stdcall_image, image_base, (SkseX86Scratch){scratch_base, scratch, sizeof scratch},
                           stdcall_args, 2, 100, (SkseX86Services){0}, SKSE_X86_STDCALL);
    assert(result.status == SKSE_X86_OK);

    const SkseX86Service service = {0x00300000, 0x1234, SKSE_X86_SCALAR_RETURN};
    SkseX86Services services = {.entries = &service, .count = 1};
    uint8_t calls_service[] = {0xb8, 0, 0, 0x30, 0, 0xff, 0xd0, 0xc3};
    result = run(calls_service, sizeof calls_service, 100, services);
    assert(result.status == SKSE_X86_OK && result.eax == 0x1234 && result.service_calls[0] == 1);
    calls_service[6] = 0xe0; // The import thunk returns through the original caller.
    result = run(calls_service, sizeof calls_service, 100, services);
    assert(result.status == SKSE_X86_OK && result.eax == 0x1234 && result.service_calls[0] == 1);

    const SkseX86Service win_service = {0x00300000, 0, SKSE_X86_HEAP_ALLOC};
    SkseX86Heap win_heap = {.base = 0x00400000};
    SksePeImage thunk_image = {
        .base = image_base, .size = sizeof calls_service, .bytes = calls_service, .section_count = 1,
        .sections = {{0, sizeof calls_service, SKSE_PE_READ | SKSE_PE_EXEC}}
    };
    const uint32_t heap_args[] = {SKSE_X86_PROCESS_HEAP, 8, 32};
    SkseX86Services win_services = {.entries = &win_service, .count = 1, .heap = &win_heap};
    result = skse_x86_call(&thunk_image, image_base, (SkseX86Scratch){scratch_base, scratch, sizeof scratch},
                           heap_args, 3, 65535, win_services, SKSE_X86_STDCALL);
    assert(result.status == SKSE_X86_OK && result.eax == win_heap.base && result.service_calls[0] == 1);
    skse_x86_heap_close(&win_heap);

    const SkseX86Service aliases_code = {image_base, 0x1234, SKSE_X86_SCALAR_RETURN};
    assert(run(calls_service, sizeof calls_service, 100, (SkseX86Services){.entries = &aliases_code, .count = 1}).status ==
           SKSE_X86_BAD_ARGUMENT);
    check_arithmetic_flags();
    check_wide_scan();
    check_double_shift_left();
    check_bounded_heap();
    check_heap_metadata_growth();
    check_private_heaps();
    check_read_only_views();
    check_hook_observations();
    check_code_write_audit();
    check_environment_block_ownership();
    check_virtual_file_limits();
    check_default_file_security_attributes();
    check_plugin_ini();
    check_data_page_protection();
    check_virtual_tls();
    check_virtual_critical_sections();
    check_static_tls_address();
    check_exception_registrations();
    check_cached_image_bounds();
    check_services_need_live_state();
    check_service_index();
    return 0;
}
