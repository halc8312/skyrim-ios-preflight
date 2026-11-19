#include <stdio.h>
#include "fixture_api.h"
#include "fixture_cases.h"

static struct Arguments current;
static unsigned release_calls, handle_calls;
static unsigned __cdecl get_release(void) { ++release_calls; return current.release; }
static unsigned __cdecl get_handle(void) { ++handle_calls; return current.handle; }

int main(void) {
    for (enum Case id = GAME; id < CASE_COUNT; ++id) {
        current = arguments(id);
        release_calls = handle_calls = 0;
        struct ClassicApi api = {current.skse_version, current.runtime_version, 0, current.editor,
            NULL, id == NO_HANDLE_CALLBACK ? NULL : get_handle, id == NO_RELEASE_CALLBACK ? NULL : get_release};
        struct PluginInfo info = {0};
        const struct ClassicApi *provided = id == NULL_API ? NULL : &api;
        int query = SKSEPlugin_Query(provided, id == NULL_INFO ? NULL : &info);
        int loaded = query ? SKSEPlugin_Load(provided) : -1;
        printf("%u %d %d %u %u %s %u %u\n", (unsigned)id, query, loaded,
               info.info_version, info.plugin_version, info.name ? info.name : "-", release_calls, handle_calls);
    }
    return 0;
}
