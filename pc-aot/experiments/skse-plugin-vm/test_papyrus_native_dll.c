/* SPDX-License-Identifier: GPL-3.0-only */
#define BUILD_FIXTURE
#include "test_fixture.h"
#include "papyrus_trial.h"

typedef struct FixtureIFunction FixtureIFunction;
typedef struct FixtureRegistry FixtureRegistry;

typedef struct {
    uint32_t type, bits;
} FixtureVMValue;

typedef _Bool (__thiscall *FixtureRun)(const FixtureIFunction *, const FixtureVMValue *,
                                      const FixtureRegistry *, uint32_t, FixtureVMValue *, const uint32_t *);
typedef void (__thiscall *FixtureRegisterFunction)(const FixtureRegistry *, const FixtureIFunction *);

typedef struct {
    uint32_t unused[SKSE_CLASSIC_PAPYRUS_RUN_SLOT];
    FixtureRun run;
} FixtureFunctionVtable;

typedef struct {
    uint32_t unused[SKSE_CLASSIC_PAPYRUS_REGISTER_SLOT];
    FixtureRegisterFunction register_function;
} FixtureRegistryVtable;

struct FixtureIFunction { const FixtureFunctionVtable *vtable; };
struct FixtureRegistry { const FixtureRegistryVtable *vtable; };

_Static_assert(sizeof(FixtureVMValue) == 8 && offsetof(FixtureVMValue, bits) == 4, "Classic i386 VMValue");
_Static_assert(offsetof(FixtureFunctionVtable, run) == 22 * 4, "Classic i386 IFunction::Run");
_Static_assert(offsetof(FixtureRegistryVtable, register_function) == 0x17 * 4, "Classic i386 RegisterFunction");

static _Bool __thiscall run(const FixtureIFunction *self, const FixtureVMValue *base,
                            const FixtureRegistry *registry, uint32_t stack_id, FixtureVMValue *result,
                            const uint32_t *state);

static const FixtureFunctionVtable function_vtable = {.run = run};
static const FixtureIFunction function = {&function_vtable};
static const FixtureRegistry *registered_registry;

static _Bool __thiscall run(const FixtureIFunction *self, const FixtureVMValue *base,
                            const FixtureRegistry *registry, uint32_t stack_id, FixtureVMValue *result,
                            const uint32_t *state) {
    if (self != &function || base || !registered_registry || registry != registered_registry ||
        stack_id != UINT32_C(0x0edab123) || !result || !state || (((uintptr_t)result | (uintptr_t)state) & 3))
        return 0;
    /* The shadow state starts with an authored marker. It is not a VMState. */
    if (result->type || result->bits || *state != UINT32_C(0x5a711e01)) return 0;
#ifdef PAPYRUS_NATIVE_FIXTURE_BAD_RESULT
    result->type = 4;
#else
    result->type = SKSE_CLASSIC_PAPYRUS_INT;
#endif
    result->bits = UINT32_C(0xfe123456);
    return 1;
}

static _Bool __cdecl register_papyrus(void *context) {
    const FixtureRegistry *registry = context;
    if (!registry || registered_registry || !registry->vtable || !registry->vtable->register_function)
        return 0;
    registry->vtable->register_function(registry, &function);
    registered_registry = registry;
    return 1;
}

_Bool __cdecl SKSEPlugin_Query(const FixtureInterface *api, FixtureInfo *info) {
    if (!api || !info || api->is_editor || api->runtime_version != UINT32_C(0x09200000) ||
        api->skse_version != UINT32_C(0x01070030)) return 0;
    info->info_version = 1;
    info->name = "OwnedPapyrusFixture";
    info->plugin_version = 1;
    return 1;
}

_Bool __cdecl SKSEPlugin_Load(const FixtureInterface *api) {
    if (!api || !api->query_interface) return 0;
    const FixturePapyrus *papyrus = api->query_interface(SKSE_CLASSIC_PAPYRUS_ID);
    return papyrus && papyrus->version == SKSE_CLASSIC_PAPYRUS_VERSION && papyrus->register_callback &&
           papyrus->register_callback(register_papyrus);
}
