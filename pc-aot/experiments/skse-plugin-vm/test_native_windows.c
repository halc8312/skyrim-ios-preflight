/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <wchar.h>
#include <windows.h>
#include "test_fixture.h"
#include "test_trial_cases.h"

static uint32_t release_value, handle_value;
static unsigned release_calls, handle_calls;
static unsigned __cdecl get_release(void) { ++release_calls; return release_value; }
static unsigned __cdecl get_handle(void) { ++handle_calls; return handle_value; }

int main(void) {
    // Match the interpreter's dynamic-load entry contract. This loader is
    // used only for the fixture built next to this test executable.
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return 2;
    wchar_t *filename = wcsrchr(path, L'\\');
    if (!filename || wcscpy_s(filename + 1, MAX_PATH - (size_t)(filename + 1 - path), L"fixture.dll"))
        return 2;
    HMODULE dll = LoadLibraryW(path);
    if (!dll) return 3;
    typedef _Bool (__cdecl *Query)(const FixtureInterface *, FixtureInfo *);
    typedef _Bool (__cdecl *Load)(const FixtureInterface *);
    Query query_export = (Query)GetProcAddress(dll, "SKSEPlugin_Query");
    Load load_export = (Load)GetProcAddress(dll, "SKSEPlugin_Load");
    if (!query_export || !load_export) return 4;
    for (size_t i = 0; i < sizeof trial_cases / sizeof trial_cases[0]; ++i) {
        const SkseTrialOptions *options = &trial_cases[i];
        const FixtureInterface api = {
            options->skse_version, options->runtime_version, 0, options->editor, NULL,
            options->expose_handle ? get_handle : NULL,
            options->expose_release ? get_release : NULL
        };
        FixtureInfo info = {0};
        release_value = options->release;
        handle_value = options->plugin_handle;
        release_calls = handle_calls = 0;
        unsigned query = query_export(&api, &info);
        unsigned load = query && load_export(&api);
        print_trial(i, query, load, query ? info.plugin_version : 0, query ? info.name : "",
                    release_calls, handle_calls);
    }
    if (!FreeLibrary(dll)) return 5;
    return 0;
}
