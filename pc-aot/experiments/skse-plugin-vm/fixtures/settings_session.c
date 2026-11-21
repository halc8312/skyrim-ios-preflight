/* SPDX-License-Identifier: GPL-3.0-only */
#include "classic_settings.h"
#include "owned_storage.h"
#include "mod_papyrus_inputs.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef SKYRIM_TEST_CLASSIC_VALUES_ENGINE
#include "classic_values_engine.h"
#endif

static const char profile_path[] = "../WearableLanternsData/profile1";
static const char title[] = "ランタンの明かり";

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long end = ftell(file);
    assert(end > 0 && end <= 1024 * 1024 && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = malloc((size_t)end);
    assert(bytes && fread(bytes, 1, (size_t)end, file) == (size_t)end && fclose(file) == 0);
    *size = (size_t)end;
    return bytes;
}

static SkseClassicScalar string(const char *value) {
    return (SkseClassicScalar){.type = 2, .text = value, .text_size = strlen(value) + 1};
}

static SkseClassicScalar scalar(uint32_t type, uint32_t bits) {
    return (SkseClassicScalar){.type = type, .bits = bits};
}

static SkseClassicSettings *open_settings(const SkseSettingsInputs *inputs) {
    SkseClassicSettings *settings = NULL;
    SkseSettingsStatus status = skse_classic_settings_open(inputs, &settings);
    if (status) fprintf(stderr, "open failed: %u\n", status);
    assert(status == SKSE_SETTINGS_OK && settings);
    return settings;
}

static SkseClassicScalarResult invoke(SkseClassicSettings *settings, uint64_t epoch, SkseSettingsMethod method,
                                     const SkseClassicScalar *arguments, size_t count, uint64_t *ticket) {
    SkseClassicScalarResult result;
    SkseX86Result trace;
    SkseSettingsStatus status = skse_classic_settings_invoke(settings, epoch, method, arguments, count, 4510,
        &result, ticket, &trace);
    if (status) fprintf(stderr, "method=%u status=%u vm=%u ip=%08x address=%08x work=%u\n",
                        method, status, trace.status, trace.instruction_va, trace.fault_address, trace.work);
    assert(status == SKSE_SETTINGS_OK && trace.work && *ticket);
    return result;
}

static SkseClassicScalarResult call(SkseClassicSettings *settings, uint64_t epoch, SkseSettingsMethod method,
                                   const SkseClassicScalar *arguments, size_t count) {
    uint64_t ticket;
    SkseClassicScalarResult result = invoke(settings, epoch, method, arguments, count, &ticket);
    assert(skse_classic_settings_commit(settings, epoch, ticket) == SKSE_SETTINGS_OK);
    assert(skse_classic_settings_commit(settings, epoch, ticket) == SKSE_SETTINGS_ARGUMENT);
    return result;
}

static uint32_t reopen_brightness(const SkseSettingsInputs *inputs) {
    SkseClassicSettings *reader = open_settings(inputs);
    const SkseClassicScalar args[] = {string(profile_path), string("brightness"), scalar(3, UINT32_MAX)};
    SkseClassicScalarResult value;
    assert(skse_classic_settings_prepare(reader, inputs->epoch, args, 4350, &value, NULL) == SKSE_SETTINGS_OK);
    skse_classic_settings_close(reader);
    assert(value.type == 3);
    return value.bits;
}

static void check_storage_records(const SkseSettingsInputs *inputs) {
    static const SkseLoadedMod current[] = {
        {(const uint8_t *)"Skyrim.esm", sizeof "Skyrim.esm" - 1, 0, SKSE_LOADED_CLASSIC},
        {(const uint8_t *)"OwnedFixture.esp", sizeof "OwnedFixture.esp" - 1, 1, SKSE_LOADED_CLASSIC}
    };
    SkseClassicSettings *settings = open_settings(inputs);
    SksePluginRecordSet *captured = NULL;
    assert(skse_classic_storage_capture(settings, inputs->epoch + 1, current, 2, 4510, &captured) ==
           SKSE_SETTINGS_ARGUMENT && !captured);
    SkseSettingsStatus status = skse_classic_storage_capture(settings, inputs->epoch, current, 2, 4510, &captured);
    if (status) fprintf(stderr, "storage capture status=%u\n", status);
    assert(status == SKSE_SETTINGS_OK);
    owned_storage_expect(captured, false, false);
    SksePluginRecordSet *seeded = owned_storage_add_integer(captured);
    skse_plugin_record_set_close(captured);
    captured = NULL;
    skse_classic_settings_close(settings);

    settings = open_settings(inputs);
    assert(skse_classic_storage_restore(settings, inputs->epoch, current, 2, 4520, seeded, NULL) == SKSE_SETTINGS_OK);
    assert(skse_classic_storage_capture(settings, inputs->epoch, current, 2, 4530, &captured) == SKSE_SETTINGS_OK);
    owned_storage_expect(captured, false, true);
    skse_plugin_record_set_close(captured);
    captured = NULL;
    assert(skse_classic_storage_reset(settings, inputs->epoch + 1, current, 2, 4540) == SKSE_SETTINGS_ARGUMENT);
    assert(skse_classic_storage_reset(settings, inputs->epoch, current, 2, 4550) == SKSE_SETTINGS_OK);
    assert(skse_classic_storage_capture(settings, inputs->epoch, current, 2, 4560, &captured) == SKSE_SETTINGS_OK);
    owned_storage_expect(captured, false, false);
    skse_plugin_record_set_close(captured);
    skse_plugin_record_set_close(seeded);
    skse_classic_settings_close(settings);
}

#ifdef SKYRIM_TEST_CLASSIC_VALUES_ENGINE
typedef struct {
    SkseClassicSettings *writer;
    SkseSettingsInputs reader;
    uint64_t epoch, ticket;
    unsigned rejected;
} ValuesContext;

static bool stage_value(void *opaque, const SkseClassicDeclaration *row, const uint32_t *bits,
                        const char *const *strings, uint32_t *out, char *text, size_t capacity) {
    (void)text;
    (void)capacity;
    ValuesContext *context = opaque;
    SkseSettingsMethod method = !strcmp(row->name, "Save") ? SKSE_SETTINGS_SAVE : SKSE_SETTINGS_SET_INT;
    SkseClassicScalar arguments[3];
    for (size_t i = 0; i < row->count; ++i)
        arguments[i] = row->arguments[i] == 2 ? string(strings[i]) : scalar(row->arguments[i], bits[i]);
    SkseClassicScalarResult result = invoke(context->writer, context->epoch, method, arguments, row->count, &context->ticket);
    *out = result.bits;
    return result.type == row->result;
}

static bool acknowledge_value(void *opaque, const SkseClassicDeclaration *row) {
    ValuesContext *context = opaque;
    if (!strcmp(row->name, "Save")) {
        /* VM asks the provider to publish only after the new value is valid.
         * An independent reader still sees the previous committed file here. */
        assert(reopen_brightness(&context->reader) == 39);
        return false;
    }
    return skse_classic_settings_commit(context->writer, context->epoch, context->ticket) == SKSE_SETTINGS_OK;
}

static void discard_value(void *opaque) {
    ValuesContext *context = opaque;
    ++context->rejected;
    skse_classic_settings_discard(context->writer, context->epoch, context->ticket);
}

static void check_acknowledged_discard(SkseClassicSettings *writer, const SkseSettingsInputs *reader, uint64_t epoch) {
    static const SkseClassicDeclaration set_int = {
        .class_name = "JsonUtil", .name = "SetIntValue", .result = 3, .count = 3, .is_static = 1,
        .arguments = {2, 2, 3}
    }, save = {
        .class_name = "JsonUtil", .name = "Save", .result = 5, .count = 2, .is_static = 1, .arguments = {2, 5}
    };
    const SkseClassicDeclaration *const rows[] = {&set_int, &save};
    ValuesContext context = {.writer = writer, .reader = *reader, .epoch = epoch};
    classic_value_fixture_bind_acknowledged(rows, 2, stage_value, acknowledge_value, discard_value, &context);
    const uint32_t set_bits[] = {0, 0, 77}, save_bits[] = {0, 0};
    const char *const set_strings[] = {profile_path, "brightness", NULL}, *const save_strings[] = {profile_path, NULL};
    uint32_t result;
    assert(classic_value_fixture_run(0, set_bits, set_strings, &result, NULL, 0) && result == 77);
    result = 42;
    assert(!classic_value_fixture_run(1, save_bits, save_strings, &result, NULL, 0) && result == 42);
    assert(context.rejected == 1);
    classic_value_fixture_retire();
}
#endif

static void save_check(SkseSettingsInputs inputs) {
    SkseClassicSettings *writer = open_settings(&inputs);
    const uint64_t epoch = inputs.epoch;
    SkseClassicScalar brightness[] = {string(profile_path), string("brightness"), scalar(3, 3)};
    const SkseClassicScalar duration[] = {string(profile_path), string("duration"), scalar(4, 0x3fd00000)};
    const SkseClassicScalar profile_name[] = {string(profile_path), string("profile_name"), string(title)};
    const SkseClassicScalar save[] = {string(profile_path), scalar(5, 0)};
    const SkseClassicScalar invalid[] = {string("../WearableLanternsData/../../bad"), scalar(5, 0)};
    uint64_t ticket = 123;
    SkseClassicScalarResult value = {.type = 32, .bits = 452, .text = "original"};
    assert(skse_classic_settings_invoke(writer, epoch, SKSE_SETTINGS_SAVE, invalid, 2, 0, &value, &ticket, NULL)
        == SKSE_SETTINGS_ARGUMENT);
    assert(skse_classic_settings_invoke(writer, epoch + 1, SKSE_SETTINGS_SET_INT, brightness, 3, 0, &value, &ticket, NULL)
        == SKSE_SETTINGS_ARGUMENT);
    assert(ticket == 123 && value.type == 32 && value.bits == 452 && !strcmp(value.text, "original"));
    assert(call(writer, epoch, SKSE_SETTINGS_SET_INT, brightness, 3).bits == 3);
    assert(call(writer, epoch, SKSE_SETTINGS_SET_FLOAT, duration, 3).bits == 0x3fd00000);
    value = call(writer, epoch, SKSE_SETTINGS_SET_STRING, profile_name, 3);
    assert(value.type == 2 && !strcmp(value.text, title));
    value = invoke(writer, epoch, SKSE_SETTINGS_SAVE, save, 2, &ticket);
    assert(value.type == 5 && value.bits == 1);
    assert(skse_classic_settings_commit(writer, epoch + 1, ticket) == SKSE_SETTINGS_ARGUMENT);
    assert(skse_classic_settings_commit(writer, epoch, ticket - 1) == SKSE_SETTINGS_ARGUMENT);
    SkseSettingsInputs reader = inputs;
    ++reader.epoch;
    assert(reopen_brightness(&reader) == UINT32_MAX);
    assert(skse_classic_settings_commit(writer, epoch, ticket) == SKSE_SETTINGS_OK);
    assert(reopen_brightness(&reader) == 3);

    /* New writes reuse the closed virtual-file slot. More than 32 saves
     * exercises the refreshed callback budget with a real DLL. */
    for (uint32_t i = 4; i < 40; ++i) {
        brightness[2].bits = i;
        assert(call(writer, epoch, SKSE_SETTINGS_SET_INT, brightness, 3).bits == i);
        assert(call(writer, epoch, SKSE_SETTINGS_SAVE, save, 2).bits == 1);
    }
#ifdef SKYRIM_TEST_CLASSIC_VALUES_ENGINE
    check_acknowledged_discard(writer, &reader, epoch);
#else
    brightness[2].bits = 77;
    assert(call(writer, epoch, SKSE_SETTINGS_SET_INT, brightness, 3).bits == 77);
    assert(invoke(writer, epoch, SKSE_SETTINGS_SAVE, save, 2, &ticket).bits == 1);
    skse_classic_settings_discard(writer, epoch, ticket);
#endif
    assert(skse_classic_settings_commit(writer, epoch, ticket) == SKSE_SETTINGS_ARGUMENT);
    assert(skse_classic_settings_invoke(writer, epoch, SKSE_SETTINGS_GET_INT, brightness, 3, 0, &value, &ticket, NULL)
        == SKSE_SETTINGS_RETIRED);
    skse_classic_settings_close(writer);
    assert(reopen_brightness(&reader) == 39);
}

static void read_check(SkseSettingsInputs inputs) {
    assert(reopen_brightness(&inputs) == 39);
    SkseClassicSettings *reader = open_settings(&inputs);
    const SkseClassicScalar duration[] = {string(profile_path), string("duration"), scalar(4, 0)};
    const SkseClassicScalar profile_name[] = {string(profile_path), string("profile_name"), string("fallback")};
    uint64_t epoch = inputs.epoch;
    assert(call(reader, epoch, SKSE_SETTINGS_GET_FLOAT, duration, 3).bits == 0x3fd00000);
    SkseClassicScalarResult name = call(reader, epoch, SKSE_SETTINGS_GET_STRING, profile_name, 3);
    assert(name.type == 2 && !strcmp(name.text, title));
    skse_classic_settings_close(reader);
    inputs.profile[0] = 2;
    ++inputs.epoch;
    assert(reopen_brightness(&inputs) == UINT32_MAX);
}

static void base_file_check(const SkseSettingsInputs *inputs) {
    assert(reopen_brightness(inputs) == 208);
    SkseClassicSettings *writer = open_settings(inputs);
    const SkseClassicScalar brightness[] = {string(profile_path), string("brightness"), scalar(3, 77)};
    const SkseClassicScalar save[] = {string(profile_path), scalar(5, 0)};
    assert(call(writer, inputs->epoch, SKSE_SETTINGS_SET_INT, brightness, 3).bits == 77);
    uint64_t ticket = 51;
    SkseClassicScalarResult old_value = {.type = 3, .bits = 13};
    assert(skse_classic_settings_invoke(writer, inputs->epoch, SKSE_SETTINGS_SAVE, save, 2, 0,
        &old_value, &ticket, NULL) == SKSE_SETTINGS_STORAGE);
    assert(old_value.type == 3 && old_value.bits == 13 && ticket == 51);
    skse_classic_settings_close(writer);
}

static void selected_view_check(SkseSettingsInputs inputs) {
    static const char profile[] = "10000000-0000-4000-8000-000000000000";
    static const char *const packages[] = {"20000000-0000-4000-8000-000000000000"};
    SkyrimPapyrusInputs selected;
    assert(!skyrim_papyrus_inputs_open(inputs.game_fd, profile, packages, 1, &selected));
    inputs.dll = selected.dll;
    inputs.pex = selected.pex;
    inputs.dll_size = selected.dll_size;
    inputs.pex_size = selected.pex_size;
    inputs.game_fd = selected.game_fd;
    inputs.settings_fd = selected.settings_fd;
    memcpy(inputs.profile, selected.profile, sizeof inputs.profile);
    SkseClassicSettings *settings = open_settings(&inputs);
    skyrim_mods_release_papyrus_inputs(&selected);

    const SkseClassicScalar arguments[] = {string("../WearableLanternsData/common"), string("auto_load"), scalar(3, 0)};
    SkseClassicScalarResult value;
    assert(skse_classic_settings_prepare(settings, inputs.epoch, arguments, 4410, &value, NULL) == SKSE_SETTINGS_OK);
    assert(value.type == 3 && value.bits == 2);
    skse_classic_settings_close(settings);
}

static void check_bootstrap(const char *path, const char *sha, const uint8_t expected[8]) {
    uint8_t digest[32];
    assert(strlen(sha) == sizeof digest * 2);
    for (size_t i = 0; i < sizeof digest; ++i) {
        const char byte[] = {sha[i * 2], sha[i * 2 + 1], 0};
        digest[i] = (uint8_t)strtoul(byte, NULL, 16);
    }
    FILE *executable = fopen(path, "rb");
    assert(executable);
    SkseSettingsBootstrap state = {0};
    uint8_t actual[8] = {0}, block[16384];
    assert(!skse_classic_settings_copy_bootstrap(&state, digest, actual));
    uint64_t offset = 0;
    for (size_t size; (size = fread(block, 1, sizeof block, executable)); offset += size)
        skse_classic_settings_scan_bootstrap(&state, offset, block, size);
    assert(!ferror(executable) && fclose(executable) == 0);
    digest[0] ^= 1;
    assert(!skse_classic_settings_copy_bootstrap(&state, digest, actual));
    digest[0] ^= 1;
    assert(skse_classic_settings_copy_bootstrap(&state, digest, actual) && !memcmp(actual, expected, sizeof actual));
}

int main(int argc, char **argv) {
    assert(argc == 9);
    SkseSettingsInputs inputs = {.epoch = 1, .profile = {1}, .uptime_ms = 4300};
    uint8_t *dll = read_file(argv[1], &inputs.dll_size);
    uint8_t *pex = read_file(argv[2], &inputs.pex_size);
    inputs.dll = dll;
    inputs.pex = pex;
    size_t size;
    uint8_t *boot = read_file(argv[3], &size);
    assert(size == sizeof inputs.bootstrap);
    memcpy(inputs.bootstrap, boot, size);
    free(boot);
    inputs.game_fd = open(argv[4], O_RDONLY | O_DIRECTORY);
    inputs.settings_fd = open(argv[5], O_RDONLY | O_DIRECTORY);
    assert(inputs.game_fd >= 0 && inputs.settings_fd >= 0);

    if (!strcmp(argv[6], "write")) {
        SkseClassicSettings *settings = NULL;
        check_bootstrap(argv[7], argv[8], inputs.bootstrap);
        SkseSettingsInputs invalid = inputs;
        invalid.settings_fd = invalid.game_fd;
        assert(skse_classic_settings_open(&invalid, &settings) == SKSE_SETTINGS_STORAGE && !settings);
        pex[10] ^= 1;
        assert(skse_classic_settings_open(&inputs, &settings) == SKSE_SETTINGS_VERSION && !settings);
        pex[10] ^= 1;
        dll[10] ^= 1;
        assert(skse_classic_settings_open(&inputs, &settings) == SKSE_SETTINGS_VERSION && !settings);
        dll[10] ^= 1;
        save_check(inputs);
    } else if (!strcmp(argv[6], "read")) {
        read_check(inputs);
    } else if (!strcmp(argv[6], "records")) {
        check_storage_records(&inputs);
    } else if (!strcmp(argv[6], "view")) {
        selected_view_check(inputs);
    } else {
        assert(!strcmp(argv[6], "base"));
        base_file_check(&inputs);
    }
    close(inputs.settings_fd);
    close(inputs.game_fd);
    free(pex);
    free(dll);
    return 0;
}
