/* SPDX-License-Identifier: GPL-3.0-only */
/* Include the private trial implementation only in this host test binary.
 * No source from the original DLL or game is embedded in the fixture. */
#define _POSIX_C_SOURCE 200809L
#define SKYRIM_TEST_CLASSIC_REGISTRY 1
#include "plugin_trial.c"
#include "owned_storage.h"
#include "classic_pex.h"
#include "classic_scalar.h"
#include "../../skse-cosave/records.h"
#include "../../skse-profile-json/profile_overlay.h"
#ifdef SKYRIM_TEST_CLASSIC_VALUES_ENGINE
#include "classic_values_engine.h"
#endif
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>

enum { OwnedDllBytes = 978944, JsonCapacity = 4096, JsonWorkLimit = 500000, WarmValueWorkLimit = 30000 };

static const char settings_root[] = "Data/SKSE/Plugins/WearableLanternsData";
static const char settings_name[] = "Data/SKSE/Plugins/WearableLanternsData/startup_test_file.json";
static const char profile_name[] = "Data/SKSE/Plugins/WearableLanternsData/profile1.json";
static const char common_name[] = "Data/SKSE/Plugins/WearableLanternsData/common.json";
typedef enum {
    WRITE_SETTINGS, UPDATE_SETTINGS, READ_SETTINGS, ISOLATED_PROFILE, WRITE_RECORDS, READ_RECORDS, PEX_CONTRACT,
    WRITE_PROFILE, READ_PROFILE, PREPARE_PROFILE, PREPARE_DISABLED
} Scenario;
typedef enum { NORMAL_CALL, WARM_CALL } CallMode;

static bool lantern_profiles(Scenario scenario) {
    return scenario == WRITE_PROFILE || scenario == READ_PROFILE || scenario == PREPARE_PROFILE || scenario == PREPARE_DISABLED;
}

static uint32_t base_attributes(void *context, const char *path, uint32_t *attributes) {
    (void)context;
    static const char *const directories[] = {
        "Data", "Data/SKSE", "Data/SKSE/Plugins", "Data/SKSE/Plugins/StorageUtilData"
    };
    for (size_t i = 0; i < sizeof directories / sizeof directories[0]; ++i) {
        if (skse_win_asset_name_equal(path, directories[i])) {
            *attributes = SKSE_WIN_ATTRIBUTE_DIRECTORY;
            return 0;
        }
    }
    return 2;
}

static void capture_registry(SksePluginSession *session, SkseClassicCatalog *catalog) {
    SkseX86Result callback = {0};
    const bool registered = capture_classic_registry(session, catalog, &callback);
    if (!registered)
        fprintf(stderr, "Classic registration call=%u ip=%08x address=%08x service=%08x work=%u rows=%zu\n",
                callback.status, callback.instruction_va, callback.fault_address,
                callback.failed_service, callback.work, catalog->count);
    assert(registered);
}

static const SkseClassicDeclaration *declaration(const SkseClassicCatalog *catalog, const char *group,
                                                  const char *name, uint32_t result,
                                                  const uint32_t *arguments, size_t count) {
    for (size_t i = 0; i < catalog->count; ++i) {
        const SkseClassicDeclaration *row = &catalog->rows[i];
        if (strcmp(row->class_name, group) || strcmp(row->name, name)) continue;
        assert(row->owner == 0 && row->is_static && row->result == result && row->count == count);
        for (size_t j = 0; j < count; ++j)
            assert(row->arguments[j] == arguments[j] ||
                   (arguments[j] == SKSE_CLASSIC_CATALOG_FORM && skse_classic_type_is_form(row->arguments[j])));
        return row;
    }
    fprintf(stderr, "Missing verified declaration %s.%s\n", group, name);
    abort();
}

static uint8_t *read_owned_pex(const char *root, const char *script, size_t *length) {
    char path[4096];
    const int chars = snprintf(path, sizeof path, "%s/%s.pex", root, script);
    if (chars <= 0 || (size_t)chars >= sizeof path) return NULL;
    FILE *input = fopen(path, "rb");
    if (!input) return NULL;
    const long size = fseek(input, 0, SEEK_END) == 0 ? ftell(input) : -1;
    uint8_t *bytes = size > 0 && size <= SKSE_PEX_MAX_BYTES && fseek(input, 0, SEEK_SET) == 0
        ? malloc((size_t)size) : NULL;
    const bool finished = bytes && fread(bytes, 1, (size_t)size, input) == (size_t)size && fgetc(input) == EOF && !ferror(input);
    if (fclose(input) || !finished) {
        free(bytes);
        return NULL;
    }
    *length = (size_t)size;
    return bytes;
}

static bool verify_profile_methods(const SkseClassicCatalog *catalog, const char *root,
                                   const SkseClassicDeclaration *const *methods, size_t count) {
    size_t length = 0;
    uint8_t *bytes = read_owned_pex(root, "JsonUtil", &length);
    SksePexNative *rows = calloc(SKSE_PEX_MAX_ROWS, sizeof *rows);
    SksePexInventory inventory = {.natives = rows, .native_capacity = SKSE_PEX_MAX_ROWS};
    bool accepted = bytes && rows && skse_pex_inspect(bytes, length, &inventory) == SKSE_PEX_OK;
    for (size_t i = 0; accepted && i < count; ++i) {
        const SkseClassicDeclaration *method = methods[i];
        const SksePexStaticCall reference = {
            .script = {(const uint8_t *)method->class_name, (uint16_t)strlen(method->class_name)},
            .name = {(const uint8_t *)method->name, (uint16_t)strlen(method->name)}, .count = method->count
        };
        const SkseClassicDeclaration *matched = NULL;
        accepted = skse_pex_match_call(&reference, rows, inventory.native_count, catalog, &matched) == SKSE_PEX_MATCH &&
                   matched == method;
    }
    free(rows);
    free(bytes);
    return accepted;
}

static const SkseClassicDeclaration *check_owned_pex(const SkseClassicCatalog *catalog, const char *root) {
    static const struct { const char *script; size_t natives; } scripts[] = {
        {"ActorUtil", 5}, {"JsonUtil", 128}, {"MiscUtil", 14}, {"ObjectUtil", 6},
        {"PapyrusUtil", 54}, {"StorageUtil", 188}
    };
    SksePexNative *rows = calloc(SKSE_PEX_MAX_ROWS, sizeof *rows);
    SksePexStaticCall *calls = calloc(SKSE_PEX_MAX_CALLS, sizeof *calls);
    SksePexNative *json_rows = calloc(SKSE_PEX_MAX_ROWS, sizeof *json_rows);
    assert(rows && calls && json_rows);
    SksePexInventory inventory = {.natives = rows, .native_capacity = SKSE_PEX_MAX_ROWS,
                                  .calls = calls, .call_capacity = SKSE_PEX_MAX_CALLS};
    bool seen[SKSE_CLASSIC_CATALOG_MAX_ROWS] = {false};
    const SkseClassicDeclaration *resolved = NULL;
    uint8_t *json_input = NULL;
    size_t json_count = 0;
    size_t matched = 0;
    for (size_t i = 0; i < sizeof scripts / sizeof scripts[0]; ++i) {
        size_t length;
        uint8_t *bytes = read_owned_pex(root, scripts[i].script, &length);
        assert(bytes && skse_pex_inspect(bytes, length, &inventory) == SKSE_PEX_OK);
        assert(inventory.native_count == scripts[i].natives);
        if (!strcmp(scripts[i].script, "StorageUtil")) {
            for (size_t j = 0; j < inventory.call_count; ++j) {
                const SksePexStaticCall *site = &calls[j];
                if (!skse_pex_text_equal(site->script, "JsonUtil") || !skse_pex_text_equal(site->name, "IntListSet"))
                    continue;
                const SkseClassicDeclaration *method;
                assert(json_input && skse_pex_match_call(site, json_rows, json_count, catalog, &method) == SKSE_PEX_MATCH);
                assert(!resolved || method == resolved);
                resolved = method;
            }
        }
        for (size_t j = 0; j < inventory.native_count; ++j) {
            const SksePexNative *native = &rows[j];
            assert(skse_pex_text_equal(native->script, scripts[i].script));
            const SkseClassicDeclaration *declaration;
            const SksePexMatch match = skse_pex_match(native, catalog, &declaration);
            if (match == SKSE_PEX_MATCH) {
                const size_t index = (size_t)(declaration - catalog->rows);
                assert(index < catalog->count && !seen[index]);
                seen[index] = true;
                ++matched;
            } else {
                printf("pex mismatch=%u %s.%.*s\n", match, scripts[i].script,
                       native->name.length, (const char *)native->name.bytes);
            }
        }
        if (!strcmp(scripts[i].script, "JsonUtil")) {
            json_input = bytes;
            json_count = inventory.native_count;
            memcpy(json_rows, rows, json_count * sizeof json_rows[0]);
        } else {
            free(bytes);
        }
    }
    assert(resolved);
    free(json_input);
    free(json_rows);
    free(calls);
    free(rows);
    for (size_t i = 0; i < catalog->count; ++i)
        if (!seen[i]) printf("dll only %s.%s\n", catalog->rows[i].class_name, catalog->rows[i].name);
    printf("native signatures matched=%zu dll_declarations=%zu\n", matched, catalog->count);
    return resolved;
}

typedef struct {
    uint32_t id;
    uint8_t type;
    uint64_t handle;
} FixtureForm;

static const char actor_saved_text[] = u8"夜のランタン";
static const char actor_default_text[] = "No actor setting";
typedef struct {
    char key[sizeof "classicvalue"], saved[sizeof actor_saved_text], missing[sizeof actor_default_text];
} StorageStrings;

static uint32_t call_native_in_frame(SksePluginSession *session, const SkseClassicDeclaration *entry,
                                     const uint32_t *args, size_t count, const FixtureForm *borrowed,
                                     char *string_out, CallMode mode) {
    static const SkseX86Service helpers[] = {
        {0x00c3a620, SKSE_CLASSIC_FRAME_ARG_OFFSET, SKSE_X86_CLASSIC_FRAME},
        {0x00c3ac40, SKSE_CLASSIC_FRAME_ARG_GET, SKSE_X86_CLASSIC_FRAME},
        {0x00c328e0, SKSE_CLASSIC_FRAME_DESTROY, SKSE_X86_CLASSIC_FRAME},
        {0x00a51210, SKSE_CLASSIC_FRAME_STRING_SET, SKSE_X86_CLASSIC_FRAME},
        {0x00a511c0, SKSE_CLASSIC_FRAME_STRING_CONSTRUCT, SKSE_X86_CLASSIC_FRAME},
        {0xe0010660, SKSE_CLASSIC_FRAME_INVALID_HANDLE, SKSE_X86_CLASSIC_FRAME},
        {0xe0010664, SKSE_CLASSIC_FRAME_HAS_TYPE, SKSE_X86_CLASSIC_FRAME},
        {0xe0010668, SKSE_CLASSIC_FRAME_VALID_HANDLE, SKSE_X86_CLASSIC_FRAME},
        {0xe001066c, SKSE_CLASSIC_FRAME_RESOLVE_HANDLE, SKSE_X86_CLASSIC_FRAME}
    };
    const SksePeImage *image = &session->space.images[entry->owner];
    assert(entry->is_static && count == (size_t)entry->count + 1 && !args[0]);
    assert(skse_pe_allows(image, entry->run, 1, SKSE_PE_EXEC));
    assert(entry->result >= 2 && entry->result <= 5 && (entry->result == 2) == (string_out != NULL));

    TrialVm *vm = &session->vm;
    const SkseX86ReadView *prior_views = vm->services.read_views;
    const size_t prior_count = vm->services.read_view_count;
    SkseX86ReadView views[8];
    size_t view_count = prior_count;
    SkseClassicFormArgument form = {0};
    SkseClassicStringResult output = {.address = 0x1400a000};
    uint8_t policy_root[4], policy_bytes[0x60] = {0};
    if (borrowed || string_out) {
        assert(prior_count + (borrowed ? 2u : 0u) + (string_out ? 1u : 0u) <= sizeof views / sizeof views[0]);
        memcpy(views, prior_views, prior_count * sizeof views[0]);
    }
    if (borrowed) {
        assert(skse_classic_type_is_form(entry->arguments[0]));
        form = (SkseClassicFormArgument){
            .policy = 0x14008000, .view = 0x14008040, .form_id = borrowed->id,
            .form_type = borrowed->type, .handle = borrowed->handle,
            .identifier = skse_x86_heap_allocate(&session->heap, SKSE_CLASSIC_IDENTIFIER_BYTES, true)
        };
        uint8_t *identifier = skse_x86_heap_memory(&session->heap, form.identifier, SKSE_CLASSIC_IDENTIFIER_BYTES, true);
        assert(identifier);
        write_word(identifier + 0x10, (uint32_t)form.handle);
        write_word(identifier + 0x14, (uint32_t)(form.handle >> 32));
        write_word(identifier + 0x18, 1);
        write_word(policy_root, form.policy);
        write_word(policy_bytes, form.policy + 8);
        write_word(policy_bytes + 8 + 3 * 4, helpers[SKSE_CLASSIC_FRAME_INVALID_HANDLE].address);
        write_word(policy_bytes + 8 + 1 * 4, helpers[SKSE_CLASSIC_FRAME_HAS_TYPE].address);
        write_word(policy_bytes + 8 + 2 * 4, helpers[SKSE_CLASSIC_FRAME_VALID_HANDLE].address);
        write_word(policy_bytes + 8 + 8 * 4, helpers[SKSE_CLASSIC_FRAME_RESOLVE_HANDLE].address);
        write_word(policy_bytes + 0x40 + 0x0c, form.form_id);
        policy_bytes[0x40 + 0x12] = form.form_type;
        views[view_count++] = (SkseX86ReadView){SKSE_CLASSIC_FORM_POLICY_SLOT, policy_root, sizeof policy_root};
        views[view_count++] = (SkseX86ReadView){form.policy, policy_bytes, sizeof policy_bytes};
    }
    if (string_out)
        views[view_count++] = (SkseX86ReadView){output.address, output.bytes, sizeof output.bytes};
    if (view_count != prior_count) {
        vm->services.read_views = views;
        vm->services.read_view_count = view_count;
    }

    SkseClassicFrame frame = {.function = entry, .form = borrowed ? &form : NULL,
        .string_result = string_out ? &output : NULL,
        .address = skse_x86_heap_allocate(&session->heap, SKSE_CLASSIC_FRAME_BYTES, true)};
    uint8_t *bytes = skse_x86_heap_memory(&session->heap, frame.address, SKSE_CLASSIC_FRAME_BYTES, true);
    assert(bytes);
    write_word(bytes + SKSE_CLASSIC_FRAME_STATE, frame.address + SKSE_CLASSIC_FRAME_LIST);
    write_word(bytes + SKSE_CLASSIC_FRAME_STATE + 0x1c, entry->count);
    for (uint32_t i = 0; i < entry->count; ++i) {
        const uint32_t type = entry->arguments[i];
        assert((type >= 2 && type <= 5) || skse_classic_type_is_form(type));
        assert(type != 5 || args[i + 1] <= 1);
        assert(!skse_classic_type_is_form(type) || !args[i + 1]);
        const bool has_form = borrowed && form.argument == i;
        const uint32_t live_type = has_form ? 1 : skse_classic_type_is_form(type) ? 0 : type;
        const uint32_t bits = has_form ? form.identifier : args[i + 1];
        write_word(bytes + SKSE_CLASSIC_FRAME_ARGS + i * 8, live_type);
        write_word(bytes + SKSE_CLASSIC_FRAME_ARGS + i * 8 + 4, bits);
        frame.arguments[i] = bits;
    }

    const uint32_t arguments[] = {0, session->papyrus.registry, PAPYRUS_FIXTURE_STACK_ID,
                                 frame.address, frame.address + SKSE_CLASSIC_FRAME_STATE};
    const size_t service_count = vm->services.count;
    const size_t helpers_used = borrowed ? SKSE_CLASSIC_FRAME_OPERATIONS : SKSE_CLASSIC_FRAME_INVALID_HANDLE;
    assert(service_count + helpers_used <= SKSE_X86_MAX_SERVICES);
    assert(session->papyrus.phase == SKSE_PAPYRUS_REGISTERED);
    memcpy(session->entries + service_count, helpers, helpers_used * sizeof helpers[0]);
    vm->services.count += helpers_used;
    vm->services.classic_frame = &frame;
    session->papyrus.phase = SKSE_PAPYRUS_RUNNING;
    const uint32_t per_call = mode == WARM_CALL ? WarmValueWorkLimit : JsonWorkLimit;
    const uint32_t limit = vm->remaining_work < per_call ? vm->remaining_work : per_call;
    if (borrowed) {
        // Matching low bits alone must not allow another handle into the wrapper.
        form.handle ^= UINT64_C(1) << 32;
        const SkseX86Result refused = skse_x86_call_thiscall(vm->images, vm->count, entry->run, entry->object,
            vm->scratch, arguments, 5, limit, vm->services);
        assert(refused.status == SKSE_X86_BAD_ARGUMENT && !refused.work);
        form.handle ^= UINT64_C(1) << 32;
    }
    const SkseX86Result r = skse_x86_call_thiscall(vm->images, vm->count, entry->run, entry->object, vm->scratch,
                                                  arguments, 5, limit, vm->services);
    session->papyrus.phase = SKSE_PAPYRUS_REGISTERED;
    vm->services.classic_frame = NULL;
    vm->services.count = service_count;
    vm->services.read_views = prior_views;
    vm->services.read_view_count = prior_count;
    if (borrowed) assert(skse_x86_heap_release(&session->heap, form.identifier));
    vm->remaining_work -= r.work;
    if (r.status != SKSE_X86_OK) {
        fprintf(stderr, "%s.%s status=%u ip=%08x memory=%08x service=%08x work=%u\n",
                entry->class_name, entry->name, r.status, r.instruction_va, r.fault_address, r.failed_service, r.work);
    }
    assert(r.status == SKSE_X86_OK && (uint8_t)r.eax == 1 && read_word(bytes) == entry->result);
    const uint32_t value = read_word(bytes + 4);
    assert(entry->result != 5 || value <= 1);
    if (string_out) {
        assert(output.selected && output.selected <= output.count);
        const unsigned slot = output.selected - 1;
        const unsigned offset = slot * SKSE_CLASSIC_FRAME_STRING_BYTES;
        const uint16_t length = output.lengths[slot];
        assert(value == output.address + offset && length && length <= SKSE_CLASSIC_FRAME_STRING_BYTES &&
               !output.bytes[offset + length - 1]);
        memcpy(string_out, output.bytes + offset, length);
    }
    assert(skse_x86_heap_release(&session->heap, frame.address));
    return string_out ? 0 : value;
}

static uint32_t call_native_with_form(SksePluginSession *session, const SkseClassicDeclaration *entry,
                                      const uint32_t *args, size_t count, const FixtureForm *borrowed) {
    return call_native_in_frame(session, entry, args, count, borrowed, NULL, NORMAL_CALL);
}

static uint32_t call_native(SksePluginSession *session, const SkseClassicDeclaration *entry,
                            const uint32_t *args, size_t count) {
    return call_native_with_form(session, entry, args, count, NULL);
}

static SkseClassicScalarScope scalar_scope(SksePluginSession *session, const SkseClassicCatalog *catalog) {
    const TrialVm *vm = &session->vm;
    return (SkseClassicScalarScope){
        .catalog = catalog, .images = vm->images, .image_count = vm->count,
        .scratch = vm->scratch, .services = &vm->services, .epoch = session->papyrus.epoch,
        .stack_id = PAPYRUS_FIXTURE_STACK_ID, .arguments_base = 0x1400c000, .text_base = 0x1400a000
    };
}

#ifdef SKYRIM_TEST_CLASSIC_VALUES_ENGINE
typedef struct { SksePluginSession *session; const SkseClassicCatalog *catalog; CallMode mode; } ComposedCall;

static void discard_composed_output(void *context) {
    ComposedCall *call = context;
    call->session->files.profile_aborted = true;
}

static bool invoke_composed_native(void *context, const SkseClassicDeclaration *row,
                                   const uint32_t *bits, const char *const *strings, uint32_t *out,
                                   char *result_text, size_t result_capacity) {
    const ComposedCall *call = context;
    SksePluginSession *session = call->session;
    TrialVm *vm = &session->vm;
    const SkseClassicScalarScope scope = scalar_scope(session, call->catalog);
    SkseClassicScalar values[SKSE_CLASSIC_CATALOG_MAX_ARGS] = {0};
    for (size_t i = 0; i < row->count; ++i) {
        values[i] = (SkseClassicScalar){.type = row->arguments[i], .bits = bits[i], .text = strings[i],
                                       .text_size = strings[i] ? strlen(strings[i]) + 1 : 0};
    }
    assert((row->result == 2) == (result_text != NULL));
    assert(!result_text || result_capacity >= SKSE_CLASSIC_FRAME_STRING_BYTES);
    const uint32_t budget = call->mode == WARM_CALL ? WarmValueWorkLimit : JsonWorkLimit;
    const uint32_t limit = vm->remaining_work < budget ? vm->remaining_work : budget;
    SkseX86Result trace;
    SkseClassicScalarResult result;
    const SkseClassicScalarStatus status = skse_classic_scalar_call(&scope, row, values, row->count, limit, false,
                                                                    &result, &trace);
    vm->remaining_work -= trace.work;
    if (status != SKSE_SCALAR_OK) {
        fprintf(stderr, "Composed %s.%s result=%u x86=%u at=%08x service=%08x work=%u\n",
                row->class_name, row->name, status, trace.status, trace.instruction_va, trace.failed_service, trace.work);
        return false;
    }
    if (result_text) memcpy(result_text, result.text, sizeof result.text);
    *out = result.bits;
    return true;
}
#endif

static SkseX86Result load_owned_form_records(SksePluginSession *session, const FixtureForm *reloaded,
                                             size_t count) {
    enum { LookupFormByID = 0x00451a30, FormView = 0x14009000 };
    TrialVm *vm = &session->vm;
    const SkseX86ReadView *prior = vm->services.read_views;
    const size_t prior_count = vm->services.read_view_count, prior_services = vm->services.count;
    assert(prior_count == 5 && prior_services < SKSE_X86_MAX_SERVICES);
    SkseX86ReadView views[6];
    memcpy(views, prior, prior_count * sizeof views[0]);
    OwnedStorageCatalog active;
    owned_storage_catalog(&active, false, views + 3);
    uint8_t body[SKSE_CLASSIC_FORM_VIEW_BYTES] = {0};
    write_word(body + 0x0c, reloaded->id);
    body[0x12] = reloaded->type;
    views[5] = (SkseX86ReadView){FormView, body, sizeof body};
    const SkseClassicLoadForm form = {.form_id = reloaded->id, .view = FormView, .type = reloaded->type};
    const SkseClassicLoadForms forms = {.owner = 0, .forms = &form, .count = count};
    vm->services.read_views = views;
    vm->services.read_view_count = sizeof views / sizeof views[0];
    vm->services.classic_load_forms = &forms;
    session->entries[vm->services.count++] = (SkseX86Service){LookupFormByID, 0, SKSE_X86_CLASSIC_LOOKUP_FORM};
    const SkseX86Result restored = invoke_serialization(vm, &session->serial, SKSE_SERIAL_LOAD, 250000);
    vm->services.read_views = prior;
    vm->services.read_view_count = prior_count;
    vm->services.classic_load_forms = NULL;
    vm->services.count = prior_services;
    return restored;
}

static int restore_record(void *context, const SksePairRecord *record) {
    return skse_x86_records_write(context, record->type, record->version, record->data, record->length);
}

enum { OwnedStorageRecordCount = 3 };

static bool read_fixture_companion(SkseX86Serialization *serial, const char *path) {
    uint8_t bytes[512];
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    const size_t size = fread(bytes, 1, sizeof bytes, file);
    const bool complete = size < sizeof bytes && feof(file) && !ferror(file);
    if (fclose(file) || !complete) return false;
    const SksePairImage image = {bytes, size};
    SkseX86RecordStream staged = {0};
    if (skse_records_visit(&image, serial->uid, restore_record, &staged) != SKSE_PAIR_OK ||
        skse_x86_records_count(&staged) != OwnedStorageRecordCount) {
        skse_x86_records_close(&staged);
        return false;
    }
    skse_x86_records_close(&serial->records);
    /* Move the complete stream into the session, the sole owner from here. */
    serial->records = staged;
    return true;
}

static void check_actor_text(SksePluginSession *session, const SkseClassicDeclaration *method,
                             const FixtureForm *actor, uint32_t key, size_t source, const char *expected) {
    char text[SKSE_CLASSIC_FRAME_STRING_BYTES];
    const uint32_t args[] = {0, 0, key, key + (uint32_t)source};
    call_native_in_frame(session, method, args, 4, actor, text, NORMAL_CALL);
    assert(!strcmp(text, expected));
}

static void write_owned_native_records(SksePluginSession *session, const SkseClassicCatalog *catalog, uint32_t key,
                                       const FixtureForm *actor, const char *path) {
    const uint32_t types[] = {SKSE_CLASSIC_CATALOG_FORM, 2, 3};
    const SkseClassicDeclaration *set = declaration(catalog, "StorageUtil", "SetIntValue", 3, types, 3);
    const SkseClassicDeclaration *get = declaration(catalog, "StorageUtil", "GetIntValue", 3, types, 3);
    const uint32_t set_args[] = {0, 0, key, (uint32_t)-731};
    const uint32_t get_args[] = {0, 0, key, (uint32_t)-99};
    assert(call_native(session, get, get_args, 4) == (uint32_t)-99);
    assert(call_native(session, set, set_args, 4) == (uint32_t)-731);
    assert(call_native(session, get, get_args, 4) == (uint32_t)-731);
    SkseTrialRecordProbe probe = {.callback_work_limit = 250000};
    probe_serialization(session, &probe);
    assert(probe.status == SKSE_SESSION_OK && session->result.serialization_callbacks == 3);
    owned_storage_expect(probe.output, true, true);
    skse_plugin_record_set_close(probe.output);
    assert(call_native(session, get, get_args, 4) == (uint32_t)-731);
    FixtureForm other = *actor;
    other.id++;
    other.handle++;
    const uint32_t actor_args[] = {0, 0, key, (uint32_t)-382};
    assert(call_native_with_form(session, get, get_args, 4, actor) == (uint32_t)-99);
    assert(call_native_with_form(session, set, actor_args, 4, actor) == (uint32_t)-382);
    assert(call_native_with_form(session, get, get_args, 4, actor) == (uint32_t)-382);
    assert(call_native_with_form(session, get, get_args, 4, &other) == (uint32_t)-99);
    assert(call_native(session, get, get_args, 4) == (uint32_t)-731);

    const uint32_t string_types[] = {SKSE_CLASSIC_CATALOG_FORM, 2, 2};
    const SkseClassicDeclaration *get_text = declaration(catalog, "StorageUtil", "GetStringValue", 2, string_types, 3);
    const SkseClassicDeclaration *set_text = declaration(catalog, "StorageUtil", "SetStringValue", 2, string_types, 3);
    check_actor_text(session, get_text, actor, key, offsetof(StorageStrings, missing), actor_default_text);
    check_actor_text(session, set_text, actor, key, offsetof(StorageStrings, saved), actor_saved_text);
    check_actor_text(session, get_text, actor, key, offsetof(StorageStrings, missing), actor_saved_text);
    check_actor_text(session, get_text, &other, key, offsetof(StorageStrings, missing), actor_default_text);

    SkseX86Serialization *serial = &session->serial;
    const SkseX86Result saved = invoke_serialization(&session->vm, serial, SKSE_SERIAL_SAVE, 250000);
    assert(saved.status == SKSE_X86_OK && skse_x86_records_count(&serial->records) == OwnedStorageRecordCount);
    assert(skse_x86_records_reset_reading(&serial->records));
    uint8_t encoded[OwnedStorageRecordCount][128] = {{0}};
    SksePairRecord rows[OwnedStorageRecordCount] = {{0}};
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
        SksePairRecord *row = &rows[i];
        assert(skse_x86_records_next(&serial->records, &row->type, &row->version, &row->length));
        assert(row->length < sizeof encoded[i] && skse_x86_records_read(&serial->records, encoded[i], row->length) == row->length);
        row->data = encoded[i];
    }
    assert(rows[0].type == 0x4d4f4453 && rows[0].version == 0 && rows[1].type == 0x494e5456 && rows[1].version == 1);
    const uint64_t form_key = (uint64_t)actor->type << 32 | actor->id;
    char expected[128];
    const int length = snprintf(expected, sizeof expected, "2 0 1 classicvalue -731 %" PRIu64 " 1 classicvalue -382", form_key);
    assert(length == (int)rows[1].length && !memcmp(encoded[1], expected, rows[1].length));
    SksePairImage image = {0};
    assert(skse_records_empty(&image) == SKSE_PAIR_OK);
    assert(skse_records_replace(&image, serial->uid, rows, sizeof rows / sizeof rows[0]) == SKSE_PAIR_OK);
    FILE *file = fopen(path, "wbx");
    assert(file && fwrite(image.data, 1, image.size, file) == image.size && fclose(file) == 0);
    free(image.data);
}

static bool reload_owned_native_records(SksePluginSession *session, const SkseClassicCatalog *catalog, uint32_t key,
                                        const FixtureForm *actor, const char *path) {
    const uint32_t types[] = {SKSE_CLASSIC_CATALOG_FORM, 2, 3};
    const SkseClassicDeclaration *get = declaration(catalog, "StorageUtil", "GetIntValue", 3, types, 3);
    const uint32_t string_types[] = {SKSE_CLASSIC_CATALOG_FORM, 2, 2};
    const SkseClassicDeclaration *get_text = declaration(catalog, "StorageUtil", "GetStringValue", 2, string_types, 3);
    const uint32_t get_args[] = {0, 0, key, (uint32_t)-99};
    FixtureForm moved = *actor;
    moved.id &= 0x00ffffff;
    ++moved.handle;
    assert(call_native(session, get, get_args, 4) == (uint32_t)-99);
    assert(call_native_with_form(session, get, get_args, 4, &moved) == (uint32_t)-99);
    check_actor_text(session, get_text, &moved, key, offsetof(StorageStrings, missing), actor_default_text);
    SkseX86Serialization *serial = &session->serial;
    if (!read_fixture_companion(serial, path)) {
        assert(!skse_x86_records_count(&serial->records) && !session->result.serialization_callbacks);
        return false;
    }
    const SkseX86Result unverified = load_owned_form_records(session, &moved, 0);
    assert(unverified.status == SKSE_X86_UNSUPPORTED_API && unverified.failed_service == 0x00451a30);
    assert(invoke_serialization(&session->vm, serial, SKSE_SERIAL_REVERT, 250000).status == SKSE_X86_OK);
    const SkseX86Result restored = load_owned_form_records(session, &moved, 1);
    if (restored.status != SKSE_X86_OK)
        fprintf(stderr, "Form record restore status=%u ip=%08x address=%08x service=%08x work=%u\n",
                restored.status, restored.instruction_va, restored.fault_address, restored.failed_service, restored.work);
    assert(restored.status == SKSE_X86_OK);
    assert(call_native_with_form(session, get, get_args, 4, &moved) == (uint32_t)-382);
    assert(call_native_with_form(session, get, get_args, 4, actor) == (uint32_t)-99);
    assert(call_native(session, get, get_args, 4) == (uint32_t)-731);
    check_actor_text(session, get_text, &moved, key, offsetof(StorageStrings, missing), actor_saved_text);
    check_actor_text(session, get_text, actor, key, offsetof(StorageStrings, missing), actor_default_text);
    return true;
}

static const char lantern_profile_text[] = u8"ランタンの明かり";
static const char lantern_profile_fallback[] = "Fallback label only";
static const char lantern_common[] = "../WearableLanternsData/common";
static const char auto_load_key[] = "auto_load", last_profile_key[] = "last_profile";
enum { AdditionalIntegers = 22, AdditionalFloats = 10, ProfileKeyBytes = 32 };
static const float additional_floats[AdditionalFloats] = {
    0.01f, -0.00004f, 19.875f, 289.50f, 123.88f, 40.1001f, 2.1f, 120.0f, 10.3f, 720.15f
};

typedef struct {
    uint8_t key[sizeof "profile_name"], text[sizeof lantern_profile_text], fallback[sizeof lantern_profile_fallback],
            absent[sizeof "never_set"];
    uint8_t common[sizeof lantern_common], auto_load[sizeof auto_load_key], last_profile[sizeof last_profile_key], empty[1];
    char integer_keys[AdditionalIntegers][ProfileKeyBytes], float_keys[AdditionalFloats][ProfileKeyBytes];
} ProfileStrings;

static uint32_t call_profile_method(SksePluginSession *session, const SkseClassicDeclaration *row,
                                     unsigned index, const uint32_t *args, char *text, CallMode mode) {
#ifdef SKYRIM_TEST_CLASSIC_VALUES_ENGINE
    (void)mode;
    const SkseX86Services *services = &session->vm.services;
    const char *strings[SKSE_CLASSIC_CATALOG_MAX_ARGS] = {0};
    for (unsigned i = 0; i < row->count; ++i) {
        if (row->arguments[i] != 2) continue;
        for (size_t j = 0; j < services->read_view_count; ++j) {
            const SkseX86ReadView *view = &services->read_views[j];
            const uint32_t offset = args[i + 1] - view->base;
            if (offset >= view->length) continue;
            const uint8_t *pointer = view->bytes + offset;
            assert(memchr(pointer, 0, view->length - offset));
            strings[i] = (const char *)pointer;
            break;
        }
        assert(strings[i]);
    }
    uint32_t result;
    assert(classic_value_fixture_run(index, args + 1, strings, &result, text,
                                     text ? SKSE_CLASSIC_FRAME_STRING_BYTES : 0));
    return result;
#else
    (void)index;
    return call_native_in_frame(session, row, args, row->count + 1, NULL, text, mode);
#endif
}

static void expect_scalar_refusal(const SkseClassicScalarScope *scope, const SkseClassicDeclaration *row,
                                  const SkseClassicScalar *values, size_t count, uint32_t limit) {
    SkseClassicScalarResult output = {.type = 2, .bits = 31, .text = "previous result"};
    SkseX86Result trace = {.status = SKSE_X86_OK, .work = 7};
    assert(skse_classic_scalar_call(scope, row, values, count, limit, true, &output, &trace) == SKSE_SCALAR_ARGUMENT);
    assert(trace.status == SKSE_X86_BAD_ARGUMENT && !trace.work);
    assert(output.type == 2 && output.bits == 31 && !strcmp(output.text, "previous result"));
}

static void prepare_profile(SksePluginSession *session, const SkseClassicCatalog *catalog,
                            const SkseClassicDeclaration *get_int) {
    static const char relative_path[] = "../WearableLanternsData/profile1", key[] = "brightness";
    const SkseClassicScalar values[] = {
        {.type = 2, .text = relative_path, .text_size = sizeof relative_path},
        {.type = 2, .text = key, .text_size = sizeof key},
        {.type = 3, .bits = UINT32_MAX}
    };
    SkseClassicScalarScope scope = scalar_scope(session, catalog);
    const uint32_t budget = SKSE_CLASSIC_PREPARE_MAX_WORK;
    const unsigned opened = session->files.requests;
    const size_t initial_blocks = session->heap.count;
    const uint32_t remaining = session->vm.remaining_work;
    assert(!session->files.read_only);
    scope.prepare_method = get_int;
    expect_scalar_refusal(&scope, get_int, values, 3, budget);
    session->files.read_only = true;

    session->files.profile_aborted = true;
    expect_scalar_refusal(&scope, get_int, values, 3, budget);
    session->files.profile_aborted = false;

    SkseClassicScalarScope unapproved = scope;
    unapproved.prepare_method = NULL;
    expect_scalar_refusal(&unapproved, get_int, values, 3, budget);
    SkseClassicScalarScope expired = scope;
    ++expired.epoch;
    expect_scalar_refusal(&expired, get_int, values, 3, budget);
    SksePapyrusTrial other_trial = session->papyrus;
    SkseX86Services other_services = *scope.services;
    other_services.papyrus = &other_trial;
    SkseClassicScalarScope foreign = scope;
    foreign.services = &other_services;
    expect_scalar_refusal(&foreign, get_int, values, 3, budget);
    SkseClassicDeclaration unregistered = *get_int;
    SkseClassicScalarScope copied = scope;
    copied.prepare_method = &unregistered;
    expect_scalar_refusal(&copied, &unregistered, values, 3, budget);
    expect_scalar_refusal(&scope, get_int, values, 2, budget);
    expect_scalar_refusal(&scope, get_int, values, 3, budget + 1);

    SkseClassicScalar malformed[3];
    memcpy(malformed, values, sizeof malformed);
    malformed[2].type = 4;
    expect_scalar_refusal(&scope, get_int, malformed, 3, budget);
    malformed[2] = values[2];
    --malformed[0].text_size;
    expect_scalar_refusal(&scope, get_int, malformed, 3, budget);

    uint8_t *live_type = skse_x86_heap_memory(&session->heap, get_int->parameters + 4, 4, true);
    assert(live_type && read_word(live_type) == get_int->live_types[0]);
    write_word(live_type, 3);
    expect_scalar_refusal(&scope, get_int, values, 3, budget);
    write_word(live_type, get_int->live_types[0]);
    assert(session->files.requests == opened && session->heap.count == initial_blocks &&
           session->papyrus.phase == SKSE_PAPYRUS_REGISTERED && !session->vm.services.classic_frame);

    SkseClassicScalarResult output;
    SkseX86Result trace;
    assert(skse_classic_scalar_call(&scope, get_int, values, 3, budget, true, &output, &trace) == SKSE_SCALAR_OK);
    assert(output.type == 3 && output.bits == 3 && trace.status == SKSE_X86_OK && trace.work > JsonWorkLimit);
    assert(session->files.requests == opened + 1 && session->vm.remaining_work == remaining);
    session->files.read_only = false;
}

static uint32_t prepare_common(SksePluginSession *session, const SkseClassicCatalog *catalog,
                               const SkseClassicDeclaration *get_int) {
    SkseClassicScalarScope scope = scalar_scope(session, catalog);
    scope.prepare_method = get_int;
    SkseClassicScalar arguments[] = {
        {.type = 2, .text = lantern_common, .text_size = sizeof lantern_common},
        {.type = 2, .text = auto_load_key, .text_size = sizeof auto_load_key}, {.type = 3}
    };
    const unsigned opened = session->files.requests;
    session->files.read_only = true;
    SkseClassicScalarResult result;
    assert(skse_classic_scalar_call(&scope, get_int, arguments, 3, JsonWorkLimit, true, &result, NULL) == SKSE_SCALAR_OK);
    assert(result.type == 3 && session->files.requests == opened + 1);
    const uint32_t autoload = result.bits;
    if (autoload == 2) {
        arguments[1] = (SkseClassicScalar){.type = 2, .text = last_profile_key, .text_size = sizeof last_profile_key};
        assert(skse_classic_scalar_call(&scope, get_int, arguments, 3, WarmValueWorkLimit, true, &result, NULL) == SKSE_SCALAR_OK);
        assert(result.type == 3 && result.bits == 1 && session->files.requests == opened + 1);
    }
    session->files.read_only = false;
    return autoload;
}

static bool check_profile_scalars(SksePluginSession *session, const SkseClassicCatalog *catalog, uint32_t filename,
                                  uint32_t integer_key, uint32_t float_key, uint32_t strings, Scenario scenario,
                                  const char *scripts) {
    const bool writing = scenario == WRITE_PROFILE;
    const CallMode mode = scenario == PREPARE_PROFILE ? WARM_CALL : NORMAL_CALL;
    const uint32_t int_types[] = {2, 2, 3}, float_types[] = {2, 2, 4}, text_types[] = {2, 2, 2}, save_types[] = {2, 5};
    const SkseClassicDeclaration *get_int = declaration(catalog, "JsonUtil", "GetIntValue", 3, int_types, 3);
    const SkseClassicDeclaration *get_float = declaration(catalog, "JsonUtil", "GetFloatValue", 4, float_types, 3);
    const SkseClassicDeclaration *get_text = declaration(catalog, "JsonUtil", "GetStringValue", 2, text_types, 3);
    const SkseClassicDeclaration *set_int = declaration(catalog, "JsonUtil", "SetIntValue", 3, int_types, 3);
    const SkseClassicDeclaration *set_float = declaration(catalog, "JsonUtil", "SetFloatValue", 4, float_types, 3);
    const SkseClassicDeclaration *set_text = declaration(catalog, "JsonUtil", "SetStringValue", 2, text_types, 3);
    const SkseClassicDeclaration *save = declaration(catalog, "JsonUtil", "Save", 5, save_types, 2);
    const SkseClassicDeclaration *const rows[] = {get_int, get_float, get_text, set_int, set_float, set_text, save};
    if (!verify_profile_methods(catalog, scripts, rows, sizeof rows / sizeof rows[0])) return false;
    const uint32_t get_int_args[] = {0, filename, integer_key, UINT32_C(0xffffffff)};
    if (scenario == PREPARE_DISABLED) {
        assert(prepare_common(session, catalog, get_int) == 1);
        return true;
    }
    // Use the same lazy file read as a normal lookup. This avoids asking the
    // DLL to reload a file after its first lookup already populated the cache.
    if (mode == WARM_CALL) {
        assert(prepare_common(session, catalog, get_int) == 2);
        prepare_profile(session, catalog, get_int);
        /* Each gameplay getter has its own cap. The runner starts a test
         * allowance for the two common, five profile and 32 extra reads. */
        session->vm.remaining_work = (7 + AdditionalIntegers + AdditionalFloats) * WarmValueWorkLimit;
    }
    const unsigned opened = session->files.requests;
#ifdef SKYRIM_TEST_CLASSIC_VALUES_ENGINE
    ComposedCall context = {session, catalog, mode};
    classic_value_fixture_bind(rows, sizeof rows / sizeof rows[0], invoke_composed_native, discard_composed_output, &context);
#endif
    const uint32_t common = strings + offsetof(ProfileStrings, common);
    const uint32_t auto_key = strings + offsetof(ProfileStrings, auto_load);
    const uint32_t last_key = strings + offsetof(ProfileStrings, last_profile);
    const uint32_t auto_args[] = {0, common, auto_key, 0}, last_args[] = {0, common, last_key, 0};
    assert(call_profile_method(session, get_int, 0, auto_args, NULL, mode) == (writing ? 0u : 2u));
    assert(call_profile_method(session, get_int, 0, last_args, NULL, mode) == (writing ? 0u : 1u));
    if (writing) {
        const uint32_t set_auto[] = {0, common, auto_key, 2}, set_last[] = {0, common, last_key, 1};
        const uint32_t save_common[] = {0, common, 0};
        assert(call_profile_method(session, set_int, 3, set_auto, NULL, mode) == 2);
        assert(call_profile_method(session, set_int, 3, set_last, NULL, mode) == 1);
        assert(call_profile_method(session, save, 6, save_common, NULL, mode) == 1);
        assert(call_profile_method(session, set_int, 3, set_last, NULL, mode) == 1);
        assert(call_profile_method(session, save, 6, save_common, NULL, mode) == 1);
    }
    const uint32_t get_float_args[] = {0, filename, float_key, UINT32_C(0xbf800000)};
    assert(call_profile_method(session, get_int, 0, get_int_args, NULL, mode) == (writing ? UINT32_C(0xffffffff) : 3));
    assert(call_profile_method(session, get_float, 1, get_float_args, NULL, mode) ==
           (writing ? UINT32_C(0xbf800000) : UINT32_C(0x3fd00000)));
    const uint32_t text_key = strings + offsetof(ProfileStrings, key);
    const uint32_t fallback = strings + offsetof(ProfileStrings, fallback);
    const uint32_t text_args[] = {0, filename, text_key, fallback};
    char returned[SKSE_CLASSIC_FRAME_STRING_BYTES];
    const uint32_t selection[] = {0, filename, text_key, strings + offsetof(ProfileStrings, empty)};
    call_profile_method(session, get_text, 2, selection, returned, mode);
    assert(!strcmp(returned, writing ? "" : lantern_profile_text));
    call_profile_method(session, get_text, 2, text_args, returned, mode);
    assert(!strcmp(returned, writing ? lantern_profile_fallback : lantern_profile_text));
    const uint32_t absent_args[] = {0, filename, strings + offsetof(ProfileStrings, absent), fallback};
    call_profile_method(session, get_text, 2, absent_args, returned, mode);
    assert(!strcmp(returned, lantern_profile_fallback));
    if (mode == WARM_CALL) {
        for (unsigned i = 0; i < AdditionalIntegers; ++i) {
            const uint32_t key = strings + offsetof(ProfileStrings, integer_keys) + i * ProfileKeyBytes;
            const uint32_t args[] = {0, filename, key, UINT32_MAX};
            assert(call_profile_method(session, get_int, 0, args, NULL, mode) == i + 1);
        }
        for (unsigned i = 0; i < AdditionalFloats; ++i) {
            const uint32_t key = strings + offsetof(ProfileStrings, float_keys) + i * ProfileKeyBytes;
            const uint32_t args[] = {0, filename, key, UINT32_C(0xbf800000)};
            uint32_t bits;
            memcpy(&bits, &additional_floats[i], sizeof bits);
            assert(call_profile_method(session, get_float, 1, args, NULL, mode) == bits);
        }
        assert(session->files.requests == opened);
    }
    if (writing) {
        const uint32_t set_int_args[] = {0, filename, integer_key, 3};
        const uint32_t set_float_args[] = {0, filename, float_key, UINT32_C(0x3fd00000)};  /* 1.625f */
        const uint32_t set_text_args[] = {0, filename, text_key, strings + offsetof(ProfileStrings, text)};
        const uint32_t save_args[] = {0, filename, 0};
        assert(call_profile_method(session, set_int, 3, set_int_args, NULL, mode) == 3);
        assert(call_profile_method(session, set_float, 4, set_float_args, NULL, mode) == UINT32_C(0x3fd00000));
        call_profile_method(session, set_text, 5, set_text_args, returned, mode);
        assert(!strcmp(returned, lantern_profile_text));
        assert(call_profile_method(session, save, 6, save_args, NULL, mode) == 1);
    }
#ifdef SKYRIM_TEST_CLASSIC_VALUES_ENGINE
    classic_value_fixture_retire();
#endif
    return true;
}

static bool check_list_settings(SksePluginSession *session, const SkseClassicCatalog *catalog,
                                uint32_t file_address, uint32_t key_address, Scenario scenario,
                                const uint8_t *filename_bytes, const uint8_t *key_bytes, const char *scripts) {
    const bool save_settings = scenario == WRITE_SETTINGS || scenario == UPDATE_SETTINGS;
#ifdef SKYRIM_TEST_CLASSIC_VALUES_ENGINE
    const char *filename = (const char *)filename_bytes, *key = (const char *)key_bytes;
#else
    (void)filename_bytes;
    (void)key_bytes;
#endif
    const uint32_t str_int4[] = {2, 2, 3, 3}, str_int3[] = {2, 2, 3}, strings[] = {2, 2};
    const uint32_t path_bool[] = {2, 5};
    const SkseClassicDeclaration *version = declaration(catalog, "PapyrusUtil", "GetVersion", 3, NULL, 0);
    const SkseClassicDeclaration *resize = declaration(catalog, "JsonUtil", "IntListResize", 3, str_int4, 4);
    const SkseClassicDeclaration *set = declaration(catalog, "JsonUtil", "IntListSet", 3, str_int4, 4);
    const SkseClassicDeclaration *get = declaration(catalog, "JsonUtil", "IntListGet", 3, str_int3, 3);
    const SkseClassicDeclaration *clear = declaration(catalog, "JsonUtil", "IntListClear", 3, strings, 2);
    const SkseClassicDeclaration *load = declaration(catalog, "JsonUtil", "Load", 5, path_bool, 1);
    const SkseClassicDeclaration *save = declaration(catalog, "JsonUtil", "Save", 5, path_bool, 2);
    const SkseClassicDeclaration *const methods[] = {resize, set, get, clear, load, save};
    if (!verify_profile_methods(catalog, scripts, methods, sizeof methods / sizeof methods[0])) return false;
    const uint32_t static_arg = 0;
    assert(call_native(session, version, &static_arg, 1) == 33);

    const uint32_t resize_args[] = {0, file_address, key_address, 7, 0};
    const uint32_t saved_value = scenario == WRITE_SETTINGS ? (uint32_t)-731 : (uint32_t)-382;
    const uint32_t set_args[] = {0, file_address, key_address, 4, saved_value};
    const uint32_t get_args[] = {0, file_address, key_address, 4};
    const uint32_t clear_args[] = {0, file_address, key_address};
    const uint32_t load_args[] = {0, file_address};
    const uint32_t save_args[] = {0, file_address, 0};
    if (scenario == WRITE_SETTINGS) {
        assert(call_native(session, resize, resize_args, 5) == 7);
    } else {
        const bool loaded = call_native(session, load, load_args, 2) != 0;
        assert(loaded == (scenario != ISOLATED_PROFILE));
        const uint32_t expected = scenario == UPDATE_SETTINGS ? (uint32_t)-731 :
                                  scenario == READ_SETTINGS ? saved_value : 0;
        assert(call_native(session, get, get_args, 4) == expected);
    }
    if (save_settings) {
#ifdef SKYRIM_TEST_CLASSIC_VALUES_ENGINE
        const SkseClassicDeclaration *const rows[] = {set, save};
        ComposedCall context = {session, catalog, NORMAL_CALL};
        classic_value_fixture_bind(rows, sizeof rows / sizeof rows[0], invoke_composed_native, discard_composed_output, &context);
        const char *const set_strings[] = {filename, key, NULL, NULL};
        const char *const save_strings[] = {filename, NULL};
        uint32_t result = 0;
        assert(classic_value_fixture_run(0, set_args + 1, set_strings, &result, NULL, 0) && result == saved_value);
        assert(classic_value_fixture_run(1, save_args + 1, save_strings, &result, NULL, 0) && result == 1);
        classic_value_fixture_retire();
#else
        assert(call_native(session, set, set_args, 5) == saved_value);
        assert(call_native(session, save, save_args, 3) == 1);
#endif
    } else if (scenario == READ_SETTINGS) {
        assert(call_native(session, clear, clear_args, 3) == 7);
        assert(call_native(session, get, get_args, 4) == 0);
    }
    return true;
}

static bool run_json_trial(const uint8_t library[OwnedDllBytes], uint8_t initializers[8],
                           SkseProfileJsonStore *store, const SkseProfileJsonIdentity *identity,
                           Scenario scenario, const char *scripts, const char *companion) {
    const bool profile_values = lantern_profiles(scenario);
    const bool storage_records = scenario == WRITE_RECORDS || scenario == READ_RECORDS;
    uint8_t filename[] = "../WearableLanternsData/startup_test_file";
    if (profile_values) memcpy(filename, "../WearableLanternsData/profile1", sizeof "../WearableLanternsData/profile1");
    if (scenario == PEX_CONTRACT) memcpy(filename, "../WearableLanternsData/contract_test", sizeof "../WearableLanternsData/contract_test");
    uint8_t key[] = "skyrimios-probe";
    uint8_t integer_key[] = "brightness", float_key[] = "hold_activate_toggle_duration";
    const uint32_t file_address = 0x14006000, key_address = 0x14006100, float_address = 0x14006200,
                   strings_address = 0x14006400;
    ProfileStrings profile_strings = {.key = "profile_name", .absent = "never_set"};
    SkseX86ReadView views[6] = {
        {0x0124166c, initializers, 4}, {0x012b5b80, initializers + 4, 4},
        {file_address, filename, sizeof filename}, {key_address, key, sizeof key}
    };
    OwnedStorageCatalog game_data;
    StorageStrings actor_text = {.key = "classicvalue"};
    if (storage_records) {
        memcpy(actor_text.saved, actor_saved_text, sizeof actor_text.saved);
        memcpy(actor_text.missing, actor_default_text, sizeof actor_text.missing);
        views[2] = (SkseX86ReadView){key_address, (uint8_t *)&actor_text, sizeof actor_text};
        owned_storage_catalog(&game_data, scenario == WRITE_RECORDS, views + 3);
    }
    if (profile_values) {
        memcpy(profile_strings.text, lantern_profile_text, sizeof profile_strings.text);
        memcpy(profile_strings.fallback, lantern_profile_fallback, sizeof profile_strings.fallback);
        memcpy(profile_strings.common, lantern_common, sizeof profile_strings.common);
        memcpy(profile_strings.auto_load, auto_load_key, sizeof profile_strings.auto_load);
        memcpy(profile_strings.last_profile, last_profile_key, sizeof profile_strings.last_profile);
        if (scenario == PREPARE_PROFILE) {
            for (unsigned i = 0; i < AdditionalIntegers; ++i)
                snprintf(profile_strings.integer_keys[i], ProfileKeyBytes, "synthetic_integer_%u", i);
            for (unsigned i = 0; i < AdditionalFloats; ++i)
                snprintf(profile_strings.float_keys[i], ProfileKeyBytes, "synthetic_float_%u", i);
        }
        views[3] = (SkseX86ReadView){key_address, integer_key, sizeof integer_key};
        views[4] = (SkseX86ReadView){float_address, float_key, sizeof float_key};
        views[5] = (SkseX86ReadView){strings_address, (uint8_t *)&profile_strings, sizeof profile_strings};
    }
    const uint32_t hook_sites[] = {0x006a9e90, 0x008cab54, 0x006f02ff};
    const SkseTrialOptions options = {0x01070030, 0x09200000, 0, 48, 1, 1, 1};
    const uint32_t placement = scenario == WRITE_SETTINGS || scenario == WRITE_RECORDS ? 0 : 0x12000000;
    const SkseTrialRuntime runtime = {
        .uptime_ms = 4427, .guest_base = placement, .static_tls = 1, .serialization = 1, .module_file_name = "StorageUtil.dll",
        .read_views = views, .read_view_count = storage_records ? 5 : profile_values ? 6 : 4,
        .capture_papyrus_startup = true, .trace_known_windows_imports = true,
        .hook_probe_sites = hook_sites, .hook_probe_count = sizeof hook_sites / sizeof hook_sites[0],
        .assets.attributes = base_attributes
    };
    SkseProfileJsonOverlay *overlay = NULL;
    assert(skse_profile_json_overlay_open(store, identity, &overlay) == SKSE_PROFILE_JSON_OK);
    const SkseTrialDependency source = {runtime.module_file_name, library, OwnedDllBytes};
    SksePluginSession session = {0};
    // The ordinary trial retries only known stop-only imports, and never binds
    // truly unknown symbols. Mirror that gate for this optional test.
    initialize_session(&session, source, options, &runtime, NULL, 0, NO_CATALOG);
    if (is_traceable_import(&session.result, false)) {
        destroy_session_state(&session);
        session = (SksePluginSession){.trace_windows_imports = true};
        initialize_session(&session, source, options, &runtime, NULL, 0, NO_CATALOG);
    }
    assert(session.result.status == SKSE_TRIAL_ENGINE_HOOKS_OBSERVED && session.papyrus.count == 1);
    assert(session.result.load.hook_count == 3 && session.result.load.status == SKSE_X86_OK);
    assert(session.space.images[0].base == (placement ? placement : 0x10000000));
    session.files.profile = skse_profile_json_overlay_provider(overlay);

    SkseClassicCatalog catalog = {.limit = SKSE_CLASSIC_CATALOG_MAX_ROWS};
    catalog.rows = calloc(catalog.limit, sizeof *catalog.rows);
    assert(catalog.rows);
    capture_registry(&session, &catalog);
    if (storage_records || scenario == PEX_CONTRACT) {
        bool restored = true;
        if (storage_records) {
            const FixtureForm actor = {.id = 0x01001234, .type = 0x3e, .handle = UINT64_C(0x1234abbb00003214)};
            if (scenario == WRITE_RECORDS) write_owned_native_records(&session, &catalog, key_address, &actor, companion);
            else restored = reload_owned_native_records(&session, &catalog, key_address, &actor, companion);
        } else {
            const SkseClassicDeclaration *set = check_owned_pex(&catalog, scripts);
            const uint32_t types[] = {2, 2, 3, 3};
            const SkseClassicDeclaration *resize = declaration(&catalog, "JsonUtil", "IntListResize", 3, types, 4);
            const uint32_t resize_args[] = {0, file_address, key_address, 4, 0};
            assert(call_native(&session, resize, resize_args, 5) == 4);
            const SkseClassicScalar values[] = {
                {.type = 2, .text = (const char *)filename, .text_size = strlen((const char *)filename) + 1},
                {.type = 2, .text = (const char *)key, .text_size = sizeof key},
                {.type = 3, .bits = 2}, {.type = 3, .bits = 17}
            };
            const SkseClassicScalarScope scope = scalar_scope(&session, &catalog);
            SkseClassicScalarResult output;
            assert(skse_classic_scalar_call(&scope, set, values, 4, JsonWorkLimit, false, &output, NULL) == SKSE_SCALAR_OK);
            assert(output.type == 3 && output.bits == 17);
            SkseX86Result trace;
            assert(skse_classic_scalar_call(&scope, set, values, 4, 1, false, &output, &trace) == SKSE_SCALAR_EXECUTION);
            assert(trace.status == SKSE_X86_INSTRUCTION_LIMIT && trace.work && output.type == 3 && output.bits == 17);
            assert(session.papyrus.phase == SKSE_PAPYRUS_SEALED && session.files.profile_aborted);
            assert(skse_classic_scalar_call(&scope, set, values, 4, JsonWorkLimit, false, &output, &trace) == SKSE_SCALAR_ARGUMENT);
            assert(!trace.work && output.bits == 17);
        }
        free(catalog.rows);
        assert(destroy_session_state(&session));
        skse_profile_json_overlay_close(overlay);
        return restored;
    }
    const bool save_settings = scenario == WRITE_SETTINGS || scenario == UPDATE_SETTINGS || scenario == WRITE_PROFILE;
    const unsigned opened = session.files.requests;
    const bool checked = profile_values
        ? check_profile_scalars(&session, &catalog, file_address, key_address, float_address, strings_address, scenario, scripts)
        : check_list_settings(&session, &catalog, file_address, key_address, scenario, filename, key, scripts);
    if (!checked) {
        assert(session.files.requests == opened);
        free(catalog.rows);
        assert(destroy_session_state(&session));
        skse_profile_json_overlay_close(overlay);
        return false;
    }
    SkseProfileJsonBatch batch = {0};
    const SkseProfileJsonCaptureResult capture = skse_profile_json_overlay_capture(overlay, &session.files, true, &batch);
    const size_t expected_writes = scenario == WRITE_PROFILE ? 2 : save_settings ? 1 : 0;
    assert(!capture.guest_error && capture.storage_error == SKSE_PROFILE_JSON_OK && batch.count == expected_writes);
    free(catalog.rows);
    assert(destroy_session_state(&session));
    skse_profile_json_overlay_close(overlay);

    // Output capture owns an immutable copy. File publication follows teardown
    // of the interpreted image and file handles, outside the VM call scope.
    for (size_t i = 0; i < batch.count; ++i)
        assert(skse_profile_json_publish(store, batch.entries[i]) == SKSE_PROFILE_JSON_OK);
    skse_profile_json_batch_discard(&batch);
    return true;
}

int main(int argc, char **argv) {
    assert(argc == 6 || argc == 7);
    Scenario scenario = ISOLATED_PROFILE;
    if (!strcmp(argv[4], "write")) scenario = WRITE_SETTINGS;
    else if (!strcmp(argv[4], "update")) scenario = UPDATE_SETTINGS;
    else if (!strcmp(argv[4], "read")) scenario = READ_SETTINGS;
    else if (!strcmp(argv[4], "write-records")) scenario = WRITE_RECORDS;
    else if (!strcmp(argv[4], "read-records")) scenario = READ_RECORDS;
    else if (!strcmp(argv[4], "contract")) scenario = PEX_CONTRACT;
    else if (!strcmp(argv[4], "write-profile")) scenario = WRITE_PROFILE;
    else if (!strcmp(argv[4], "read-profile")) scenario = READ_PROFILE;
    else if (!strcmp(argv[4], "prepare-profile")) scenario = PREPARE_PROFILE;
    else if (!strcmp(argv[4], "prepare-disabled")) scenario = PREPARE_DISABLED;
    else assert(!strcmp(argv[4], "isolation"));
    const bool storage_records = scenario == WRITE_RECORDS || scenario == READ_RECORDS;
    assert(storage_records == (argc == 7));
    uint8_t initializers[8];
    assert(owned_storage_read_initializers(argv[2], initializers));
    uint8_t *library = malloc(OwnedDllBytes);
    assert(library);
    FILE *input = fopen(argv[1], "rb");
    assert(input && fread(library, 1, OwnedDllBytes, input) == OwnedDllBytes);
    assert(fgetc(input) == EOF && !ferror(input) && fclose(input) == 0);
    const int directory = open(argv[3], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    assert(directory >= 0);
    const SkseProfileJsonGrant grant = {
        .identity = {
            .profile = {scenario == ISOLATED_PROFILE ? 2 : 1},
            .owner = {0x51, 0xe4, 0x0d, 0x27, 0xbc, 0x63, 0x56, 0x64, 0xd7, 0x47, 0xa7, 0x81, 0x17, 0x96, 0x73, 0xd9,
                      0xcb, 0xda, 0x8c, 0x22, 0xd1, 0x76, 0xf5, 0x6f, 0xa6, 0x59, 0x8f, 0xcc, 0xe4, 0xa0, 0xb5, 0xe9}
        },
        .virtual_prefix = settings_root, .file_bytes = JsonCapacity, .total_bytes = JsonCapacity * 2, .files = 2
    };
    SkseProfileJsonStore *store = NULL;
    assert(skse_profile_json_open(directory, &grant, &store) == SKSE_PROFILE_JSON_OK);
    assert(close(directory) == 0);
    const bool checked = run_json_trial(library, initializers, store, &grant.identity, scenario, argv[5],
                                        storage_records ? argv[6] : NULL);
    free(library);
    if (!checked) {
        skse_profile_json_close(store);
        fputs(storage_records ? "The Classic companion does not contain complete StorageUtil records.\n"
                              : "Selected JsonUtil PEX does not match the captured native methods.\n", stderr);
        return 2;
    }
    uint8_t *bytes = NULL;
    size_t length = 0;
    const bool profile_values = lantern_profiles(scenario);
    const char *output_name = scenario == PREPARE_DISABLED ? common_name : profile_values ? profile_name : settings_name;
    const SkseProfileJsonResult read = skse_profile_json_read(
        store, &grant.identity, output_name, JsonCapacity, &bytes, &length);
    skse_profile_json_close(store);
    if (scenario == ISOLATED_PROFILE) {
        assert(read == SKSE_PROFILE_JSON_MISSING && !bytes && !length);
        return 0;
    }
    if (storage_records || scenario == PEX_CONTRACT) {
        free(bytes);
        return 0;
    }
    assert(read == SKSE_PROFILE_JSON_OK && bytes && length);
    const bool complete = fwrite(bytes, 1, length, stdout) == length;
    free(bytes);
    return complete ? 0 : 1;
}
