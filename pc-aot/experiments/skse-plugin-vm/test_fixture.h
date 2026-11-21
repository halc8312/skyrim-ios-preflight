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

typedef struct {
    const char *sender;
    uint32_t type, length;
    void *data;
} FixtureMessage;

typedef void (__cdecl *FixtureMessageHandler)(FixtureMessage *);
typedef struct {
    uint32_t version;
    _Bool (__cdecl *register_listener)(unsigned, const char *, FixtureMessageHandler);
    _Bool (__cdecl *dispatch)(unsigned, unsigned, void *, unsigned, const char *);
    void *(__cdecl *get_dispatcher)(unsigned);
} FixtureMessaging;

typedef _Bool (__cdecl *FixturePapyrusCallback)(void *);
typedef struct {
    uint32_t version;
    _Bool (__cdecl *register_callback)(FixturePapyrusCallback);
} FixturePapyrus;

typedef struct FixtureSerialization FixtureSerialization;
typedef void (__cdecl *FixtureSaveCallback)(const FixtureSerialization *);
typedef void (__cdecl *FixtureDeleteCallback)(uint64_t);
struct FixtureSerialization {
    uint32_t version;
    void (__cdecl *set_uid)(uint32_t, uint32_t);
    void (__cdecl *set_revert)(uint32_t, FixtureSaveCallback);
    void (__cdecl *set_save)(uint32_t, FixtureSaveCallback);
    void (__cdecl *set_load)(uint32_t, FixtureSaveCallback);
    void (__cdecl *set_deleted)(uint32_t, FixtureDeleteCallback);
    _Bool (__cdecl *write)(uint32_t, uint32_t, const void *, uint32_t);
    _Bool (__cdecl *open)(uint32_t, uint32_t);
    _Bool (__cdecl *append)(const void *, uint32_t);
    _Bool (__cdecl *next)(uint32_t *, uint32_t *, uint32_t *);
    uint32_t (__cdecl *read)(void *, uint32_t);
    _Bool (__cdecl *resolve_handle)(uint64_t, uint64_t *);
    _Bool (__cdecl *resolve_form)(uint32_t, uint32_t *);
};

_Static_assert(sizeof(FixtureInterface) == 28, "Classic i386 interface layout");
_Static_assert(sizeof(FixtureInfo) == 12, "Classic i386 plugin info layout");
_Static_assert(sizeof(FixtureMessage) == 16, "Classic i386 message layout");
_Static_assert(sizeof(FixtureMessaging) == 16, "Classic i386 messaging v2 layout");
_Static_assert(sizeof(FixturePapyrus) == 8, "Classic i386 Papyrus v1 layout");
_Static_assert(sizeof(FixtureSerialization) == 52, "Classic i386 serialization v4 layout");

#ifdef BUILD_FIXTURE
#define FIXTURE_EXPORT __declspec(dllexport)
#else
#define FIXTURE_EXPORT __declspec(dllimport)
#endif
FIXTURE_EXPORT _Bool __cdecl SKSEPlugin_Query(const FixtureInterface *api, FixtureInfo *info);
FIXTURE_EXPORT _Bool __cdecl SKSEPlugin_Load(const FixtureInterface *api);
