/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t skse_version, runtime_version, editor_version, is_editor;
    void *(__cdecl *query_interface)(unsigned);
    unsigned (__cdecl *get_plugin_handle)(void);
    unsigned (__cdecl *get_release_index)(void);
} FixtureInterface;

typedef struct {
    uint32_t info_version;
    const char *name;
    uint32_t plugin_version;
} FixtureInfo;

_Static_assert(sizeof(FixtureInterface) == 28, "Classic i386 interface layout");
_Static_assert(sizeof(FixtureInfo) == 12, "Classic i386 plugin info layout");

#ifdef BUILD_FIXTURE
#define FIXTURE_EXPORT __declspec(dllexport)
#else
#define FIXTURE_EXPORT __declspec(dllimport)
#endif
FIXTURE_EXPORT _Bool __cdecl SKSEPlugin_Query(const FixtureInterface *api, FixtureInfo *info);
FIXTURE_EXPORT _Bool __cdecl SKSEPlugin_Load(const FixtureInterface *api);
