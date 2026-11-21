/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include "test_fixture.h"
#include "test_trial_cases.h"

static uint32_t release_value, handle_value;
static unsigned release_calls, handle_calls;
static unsigned __cdecl get_release(void) { ++release_calls; return release_value; }
static unsigned __cdecl get_handle(void) { ++handle_calls; return handle_value; }

static FixtureMessageHandler core_listener;
static unsigned callback_failed, unavailable;
static int serialization_selected, papyrus_selected;
static FixturePapyrusCallback papyrus_registration;
static uint32_t serialization_uid;
static FixtureSaveCallback saver, loader, reverter;
static SkseX86RecordStream stream;
static void __cdecl set_uid(uint32_t handle, uint32_t uid) {
    if (handle == handle_value) serialization_uid = uid;
}
static void __cdecl set_reverter(uint32_t handle, FixtureSaveCallback cb) {
    if (handle == handle_value) reverter = cb;
}
static void __cdecl set_saver(uint32_t handle, FixtureSaveCallback cb) {
    if (handle == handle_value) saver = cb;
}
static void __cdecl set_loader(uint32_t handle, FixtureSaveCallback cb) {
    if (handle == handle_value) loader = cb;
}
static void __cdecl set_delete(uint32_t handle, FixtureDeleteCallback cb) {
    (void)handle;
    (void)cb; // The test does not delete a game object.
}
static _Bool __cdecl write_record(uint32_t type, uint32_t version, const void *bytes, uint32_t length) {
    return skse_x86_records_write(&stream, type, version, bytes, length);
}
static _Bool __cdecl open_record(uint32_t type, uint32_t version) {
    return skse_x86_records_open(&stream, type, version);
}
static _Bool __cdecl write_data(const void *bytes, uint32_t length) {
    return skse_x86_records_append(&stream, bytes, length);
}
static _Bool __cdecl next_record(uint32_t *type, uint32_t *version, uint32_t *length) {
    return skse_x86_records_next(&stream, type, version, length);
}
static uint32_t __cdecl read_data(void *bytes, uint32_t length) {
    return skse_x86_records_read(&stream, bytes, length);
}
static FixtureSerialization serial = {
    4, set_uid, set_reverter, set_saver, set_loader, set_delete,
    write_record, open_record, write_data, next_record, read_data, NULL, NULL
};

static _Bool __cdecl register_listener(unsigned handle, const char *sender, FixtureMessageHandler callback) {
    if (handle != handle_value || handle == 0xffffffffu || !callback) return 0;
    if (sender) {
        if (_stricmp(sender, "SKSE")) return 0;
        if (!core_listener) core_listener = callback;
    }
    return 1;
}
static void *__cdecl get_dispatcher(unsigned id) {
    (void)id;
    callback_failed = 1;
    return NULL;
}
static _Bool __cdecl register_papyrus_callback(FixturePapyrusCallback callback) {
    if (!callback || (papyrus_registration && papyrus_registration != callback)) return 0;
    papyrus_registration = callback;
    return 1;
}

static void *__cdecl query_interface(unsigned id) {
    static FixtureMessaging messaging = {2, register_listener, NULL, get_dispatcher};
    static FixturePapyrus papyrus = {1, register_papyrus_callback};
    if (id == 5) return &messaging;
    if (id == 3 && serialization_selected) return &serial;
    if (id == 2 && papyrus_selected) return &papyrus;
    if (id && !unavailable) unavailable = id;
    return NULL;
}

typedef void (__thiscall *NativeRegistration)(const void *, const void *);
typedef _Bool (__thiscall *NativeRun)(const void *, const void *, const void *, uint32_t, uint32_t *, const uint32_t *);

static const void *native_fixture;
static void __thiscall capture_native(const void *registry, const void *function) {
    if (!registry || native_fixture) callback_failed = 1;
    else native_fixture = function;
}

static int run_native_fixture(void) {
    const struct { uint32_t reserved[23]; NativeRegistration register_function; } table = {
        .register_function = capture_native
    };
    const void *registry = &table;
    if (!papyrus_registration || !papyrus_registration(&registry) || callback_failed || !native_fixture)
        return 6;
    const uint8_t *vtable;
    memcpy(&vtable, native_fixture, sizeof vtable);
    NativeRun run;
    memcpy(&run, vtable + 22 * 4, sizeof run);
    uint32_t vm_result[2] = {0, 0};
    const uint32_t state = UINT32_C(0x5a711e01);
    const unsigned returned = run(native_fixture, NULL, &registry, UINT32_C(0x0edab123), vm_result, &state);
    const unsigned integer_returned = returned && vm_result[0] == 3;
    printf("papyrus-int,%u,%08x\n", integer_returned,
           integer_returned ? vm_result[1] : UINT32_C(0x55aa33cc));
    return 0;
}

int main(int argc, char **argv) {
    int do_messages = argc == 2 && strcmp(argv[1], "messages") == 0;
    serialization_selected = argc == 2 && strcmp(argv[1], "serialization") == 0;
    int papyrus_native = argc == 2 && (!strcmp(argv[1], "papyrus-int") || !strcmp(argv[1], "papyrus-int-bad"));
    papyrus_selected = papyrus_native || (argc == 2 && strcmp(argv[1], "papyrus-registration") == 0);
    // Match the interpreter's dynamic-load entry contract. This loader is
    // used only for the fixture built next to this test executable.
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return 2;
    wchar_t *filename = wcsrchr(path, L'\\');
    if (!filename || wcscpy_s(filename + 1, MAX_PATH - (size_t)(filename + 1 - path), L"fixture.dll"))
        return 2;
    HMODULE dll = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) {
        fprintf(stderr, "Could not load the authored DLL (Win32 error %lu).\n", (unsigned long)GetLastError());
        return 3;
    }
    typedef _Bool (__cdecl *Query)(const FixtureInterface *, FixtureInfo *);
    typedef _Bool (__cdecl *Load)(const FixtureInterface *);
    Query query_export = (Query)GetProcAddress(dll, "SKSEPlugin_Query");
    Load load_export = (Load)GetProcAddress(dll, "SKSEPlugin_Load");
    if (!query_export || !load_export) return 4;
    for (size_t i = 0; i < sizeof trial_cases / sizeof trial_cases[0]; ++i) {
        const SkseTrialOptions *options = &trial_cases[i];
        const FixtureInterface api = {
            options->skse_version, options->runtime_version, 0, options->editor,
            do_messages || serialization_selected || papyrus_selected ? query_interface : NULL,
            options->expose_handle ? get_handle : NULL,
            options->expose_release ? get_release : NULL
        };
        FixtureInfo info = {0};
        release_value = options->release;
        handle_value = options->plugin_handle;
        release_calls = handle_calls = 0;
        core_listener = NULL;
        papyrus_registration = NULL;
        callback_failed = unavailable = 0;
        unsigned query = query_export(&api, &info);
        unsigned load = query && load_export(&api);
        if (papyrus_native) {
            int result = load ? run_native_fixture() : 7;
            return FreeLibrary(dll) ? result : 5;
        }
        print_trial(i, query, load, query ? info.plugin_version : 0, query ? info.name : "",
                    release_calls, handle_calls);
        if (do_messages) {
            unsigned delivered = 0;
            if (load && core_listener) {
                for (unsigned type = 0; type < 2; ++type) {
                    FixtureMessage message = {"SKSE", type, 0, NULL};
                    core_listener(&message);
                    if (callback_failed) break;
                    ++delivered;
                }
            }
            printf("messages,%u,%u,%u\n", delivered, callback_failed, unavailable);
        }
        if (serialization_selected) {
            unsigned invoked = 0;
            if (load && serialization_uid && saver && loader) {
                saver(&serial);
                ++invoked;
                if (reverter) {
                    reverter(&serial);
                    ++invoked;
                }
                if (!skse_x86_records_reset_reading(&stream)) return 6;
                loader(&serial);
                ++invoked;
            }
            printf("serialization,%u,%u\n", invoked, skse_x86_records_count(&stream));
            skse_x86_records_close(&stream);
            saver = loader = reverter = NULL;
            serialization_uid = 0;
        }
        if (papyrus_selected) {
            uint32_t shadow_registry = 0;
            const unsigned attempted = load && papyrus_registration;
            printf("papyrus,%u,%u\n", attempted, attempted && papyrus_registration(&shadow_registry));
        }
    }
    if (!FreeLibrary(dll)) return 5;
    return 0;
}
