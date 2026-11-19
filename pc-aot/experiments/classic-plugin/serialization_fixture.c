#define BUILD_FIXTURE
#include "fixture_api.h"
#include "serialization_abi.h"
#include "serialization_files.h"

static unsigned state;

static void __cdecl reset(struct SerialApi *api) {
    (void)api;
    state = 0;
}
static void __cdecl store(struct SerialApi *api) {
    unsigned next = state + 5;
    if (api->write_record(FIXTURE_SERIAL_TYPE, FIXTURE_SERIAL_VERSION, &next, sizeof next)) state = next;
}
static void __cdecl restore(struct SerialApi *api) {
    unsigned type, version, length, value;
    while (api->next_record(&type, &version, &length)) {
        if (type == FIXTURE_SERIAL_TYPE && version == FIXTURE_SERIAL_VERSION && length == sizeof value &&
            api->read_data(&value, sizeof value) == sizeof value) state = value;
    }
}

_Bool __cdecl SKSEPlugin_Query(const struct ClassicApi *api, struct PluginInfo *info) {
    if (!api || !info) return 0;
    info->info_version = 1;
    info->name = "SerialProbe";
    info->plugin_version = 2;
    return api->skse_version == 0x01070030u && api->runtime_version == 0x09200000u && !api->is_editor;
}

_Bool __cdecl SKSEPlugin_Load(const struct ClassicApi *api) {
    if (!api || !api->query_interface || !api->get_plugin_handle) return 0;
    struct SerialApi *serial = api->query_interface(3);
    if (!serial || serial->version != 4) return 0;
    unsigned handle = api->get_plugin_handle();
    if (handle == 0xffffffffu) return 0;
    state = handle;
    serial->unique_id(handle, FIXTURE_SERIAL_UID);
    serial->on_revert(handle, reset);
    serial->on_save(handle, store);
    serial->on_load(handle, restore);
    return 1;
}
