#define BUILD_FIXTURE
#include "fixture_api.h"

_Bool __cdecl SKSEPlugin_Query(const struct ClassicApi *api, struct PluginInfo *info) {
    if (!api || !info) return 0;
    info->info_version = 1;
    info->name = "ClassicProbe";
    info->plugin_version = 17;
    return api->skse_version == 0x01070030u && api->runtime_version == 0x09200000u && !api->is_editor;
}

_Bool __cdecl SKSEPlugin_Load(const struct ClassicApi *api) {
    if (!api || !api->get_release_index || !api->get_plugin_handle) return 0;
    if (api->get_release_index() != 48) return 0;
    return api->get_plugin_handle() != 0xffffffffu;
}
