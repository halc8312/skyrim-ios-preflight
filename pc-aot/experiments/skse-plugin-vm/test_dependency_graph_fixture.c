/* SPDX-License-Identifier: GPL-3.0-only */
/* Four authored, separately linked i386 DLLs. Both branches import the leaf. */
#include <stdint.h>
#define EXPORT __declspec(dllexport)
#define IMPORT __declspec(dllimport)

#if GRAPH_MODULE == 1
static volatile uint32_t events, state;
static uint32_t *observer;
EXPORT uint32_t __cdecl PlanLeafMark(uint32_t value) { events = events * 10 + value; return events; }
EXPORT void __cdecl PlanObserve(uint32_t *value) { observer = value; }
EXPORT uint32_t __cdecl PlanLeafA(void) { return state == 11 ? 13 : 0; }
EXPORT uint32_t __cdecl PlanLeafB(void) { return state == 11 ? 29 : 0; }
EXPORT const char *__cdecl PlanLeafName(void) { return "Package-local diamond"; }
int __stdcall GraphEntry(void *image, unsigned reason, void *reserved) {
    if (!image || reserved) return 0;
    if (reason == 1) { events = 1; state = 11; }
    if (reason == 0) {
        state = 0;
        uint32_t answer = PlanLeafMark(8);
        if (observer) *observer = answer;
        return (int)answer;
    }
    return 1;
}
#elif GRAPH_MODULE == 2
IMPORT uint32_t __cdecl PlanLeafMark(uint32_t);
IMPORT uint32_t __cdecl PlanLeafA(void);
IMPORT const char *__cdecl PlanLeafName(void);
static volatile uint32_t state;
EXPORT uint32_t __cdecl PlanA(void) { return state; }
EXPORT uint32_t __cdecl PlanMarkThroughA(uint32_t n) { return PlanLeafMark(n); }
EXPORT const char *__cdecl PlanNameThroughA(void) { return PlanLeafName(); }
int __stdcall GraphEntry(void *image, unsigned reason, void *reserved) {
    if (!image || reserved) return 0;
    if (reason == 1) { state = PlanLeafA(); PlanLeafMark(2); return state == 13; }
    if (reason == 0) { state = 0; PlanLeafMark(7); }
    return 1;
}
#elif GRAPH_MODULE == 3
IMPORT uint32_t __cdecl PlanLeafMark(uint32_t);
IMPORT uint32_t __cdecl PlanLeafB(void);
#ifdef GRAPH_CHAIN
IMPORT uint32_t __cdecl PlanA(void);
#endif
static volatile uint32_t state;
EXPORT uint32_t __cdecl PlanB(void) { return state; }
int __stdcall GraphEntry(void *image, unsigned reason, void *reserved) {
    if (!image || reserved) return 0;
    if (reason == 1) {
#ifdef GRAPH_CHAIN
        if (PlanA() != 13) return 0;
#endif
        state = PlanLeafB(); PlanLeafMark(3);
#ifdef DECLINE_GRAPH_BRANCH
        return 0;
#else
        return state == 29;
#endif
    }
    if (reason == 0) { state = 0; PlanLeafMark(6); }
    return 1;
}
#elif GRAPH_MODULE == 4
#define BUILD_FIXTURE
#include "test_fixture.h"
IMPORT uint32_t __cdecl PlanA(void);
IMPORT uint32_t __cdecl PlanB(void);
IMPORT uint32_t __cdecl PlanMarkThroughA(uint32_t);
IMPORT const char *__cdecl PlanNameThroughA(void);
static volatile uint32_t version;
int __stdcall GraphEntry(void *image, unsigned reason, void *reserved) {
    if (!image || reserved) return 0;
    if (reason == 1) { version = 100 * PlanA() + PlanB(); PlanMarkThroughA(4); return version == 1329; }
    if (reason == 0) { PlanMarkThroughA(5); version = 0; }
    return 1;
}
_Bool __cdecl SKSEPlugin_Query(const FixtureInterface *api, FixtureInfo *info) {
    if (!api || !info || api->skse_version != 0x01070030 || api->runtime_version != 0x09200000 || api->is_editor) return 0;
    info->info_version = 1;
    info->plugin_version = version;
    info->name = PlanNameThroughA(); /* Readable string lives in the transitive leaf's R section. */
    return 1;
}
_Bool __cdecl SKSEPlugin_Load(const FixtureInterface *api) {
    return api && api->get_release_index && api->get_release_index() == 48 &&
           api->get_plugin_handle && api->get_plugin_handle() != UINT32_MAX;
}
#else
#error Select one graph module (1, 2, 3 or 4)
#endif
