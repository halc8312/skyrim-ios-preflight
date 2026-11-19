/* Authored ABI fixture for the public, 32-bit Classic SKSE plugin entry points. */
#pragma once
#include <stddef.h>

struct ClassicApi {
    unsigned skse_version, runtime_version, editor_version, is_editor;
    void *(__cdecl *query_interface)(unsigned);
    unsigned (__cdecl *get_plugin_handle)(void);
    unsigned (__cdecl *get_release_index)(void);
};
struct PluginInfo {
    unsigned info_version;
    const char *name;
    unsigned plugin_version;
};
_Static_assert(sizeof(struct ClassicApi) == 28 && sizeof(struct PluginInfo) == 12, "Classic i386 ABI");
_Static_assert(offsetof(struct ClassicApi, get_plugin_handle) == 20 &&
               offsetof(struct ClassicApi, get_release_index) == 24, "Classic callbacks");

#ifdef BUILD_FIXTURE
#define FIXTURE_ENTRY __declspec(dllexport)
#else
#define FIXTURE_ENTRY __declspec(dllimport)
#endif
FIXTURE_ENTRY _Bool __cdecl SKSEPlugin_Query(const struct ClassicApi *api, struct PluginInfo *info);
FIXTURE_ENTRY _Bool __cdecl SKSEPlugin_Load(const struct ClassicApi *api);
