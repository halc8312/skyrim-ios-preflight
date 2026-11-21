/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include "skse_compat.c"
#ifdef SKYRIM_TEST_CLASSIC_VALUES_ENGINE
#include "classic_values_engine.h"
#endif
#ifdef CLASSIC_INT0_TEST_INTERPRETER
#include "plugin_trial.h"
#endif

enum {
    REGISTRY = 0x200, REGISTRY_VTABLE = 0x280, NATIVE_VTABLE = 0x400,
    REGISTRY_METHOD = 0x50, FLAGS_METHOD = 0x60, DELETE_METHOD = 0xC46FD0,
    STACK = 0x600, RESULT = STACK + 0x2c, BASE = STACK + 0x38,
    STATE = 0x780, LIST = 0x7c0, ARGS = 0x800, FIXTURE_RUN_RETURN = 0xc46e61,
    FILE_TEXT = 0x900, KEY_TEXT = 0x980, INPUT_TEXT_BYTES = 128, GUEST_HEAP = 0x1000,
    STRING_POOL = 0x3000, STRING_SLOTS = 4, STRING_BYTES = 512, FIXTURE_FUNCTIONS = 8,
};
#define THUNK_BASE UINT32_C(0xff000000)

typedef struct {
    uint32_t address, parameters;
    bool form, live;
    char group[64], name[64];
} Allocation;

static CPU cpu;
static uint32_t memory[(STRING_POOL + STRING_SLOTS * STRING_BYTES) / 4];
static uint32_t next_address = GUEST_HEAP;
static Allocation allocations[64];
static unsigned allocated, registrations, guest_attempts, result_deletions, form_deletions;
static unsigned fail_guest;
static bool fail_form;
static unsigned fail_parameter_row;
static uint32_t objects[FIXTURE_FUNCTIONS + 1];
static uint32_t fixture_return_token = FIXTURE_RUN_RETURN, fixture_stack_id = 1;
static unsigned string_refs[STRING_SLOTS], scratch_deletions;
static bool refuse_string_intern, lose_frame_during_intern, frame_available = true;
static char class_names[FIXTURE_FUNCTIONS + 1][64], function_names[FIXTURE_FUNCTIONS + 1][64];
static ShimFn thunks[8];
static unsigned thunk_count;

void *fixture_span(uint32_t address) {
    assert(address && address <= sizeof memory - 4);
    return (uint8_t *)memory + address;
}

uint32_t *fixture_word(uint32_t address) {
    assert(!(address & 3));
    return fixture_span(address);
}

CPU *rt_current_cpu(void) { return &cpu; }

static uint32_t allocate(uint32_t bytes, bool form) {
    assert(allocated < sizeof allocations / sizeof allocations[0]);
    assert(next_address + bytes <= STRING_POOL);
    const uint32_t address = next_address;
    next_address += (bytes + 3) & ~UINT32_C(3);
    allocations[allocated++] = (Allocation){.address = address, .form = form, .live = true};
    return address;
}

static Allocation *allocation_at(uint32_t address, bool form) {
    for (unsigned i = 0; i < allocated; ++i) {
        if (allocations[i].address != address) continue;
        assert(allocations[i].form == form && allocations[i].live);
        return &allocations[i];
    }
    abort();
}

static void release(uint32_t address, bool form) {
    allocation_at(address, form)->live = false;
}

static unsigned live_allocations(bool form) {
    unsigned count = 0;
    for (unsigned i = 0; i < allocated; ++i) {
        if (allocations[i].live && allocations[i].form == form) ++count;
    }
    return count;
}

static bool is_fixture_string(uint32_t address) {
    return address >= STRING_POOL && address < STRING_POOL + STRING_SLOTS * STRING_BYTES &&
           !((address - STRING_POOL) % STRING_BYTES);
}

static uint32_t intern_fixture_string(const char *text) {
    const size_t length = strlen(text);
    assert(length < STRING_BYTES);
    unsigned free_slot = STRING_SLOTS;
    for (unsigned i = 0; i < STRING_SLOTS; ++i) {
        const uint32_t address = STRING_POOL + i * STRING_BYTES;
        if (!string_refs[i]) {
            if (free_slot == STRING_SLOTS) free_slot = i;
        } else if (!strcmp(GSTR(address), text)) {
            ++string_refs[i];
            return address;
        }
    }
    assert(free_slot < STRING_SLOTS);
    const uint32_t address = STRING_POOL + free_slot * STRING_BYTES;
    memcpy(GPTR(address), text, length + 1);
    string_refs[free_slot] = 1;
    return address;
}

uint32_t gmalloc(uint32_t bytes) {
    if (registrations && ++guest_attempts == fail_guest) return 0;
    return allocate(bytes, false);
}

void gfree(uint32_t address) {
    if (address) release(address, false);
}

uint32_t gstrdup(const char *text) {
    const uint32_t length = (uint32_t)strlen(text) + 1;
    const uint32_t address = gmalloc(length);
    assert(address);
    memcpy(GPTR(address), text, length);
    return address;
}

uint32_t rt_thunk(ShimFn function, const char *name) {
    assert(function && name && thunk_count < sizeof thunks / sizeof thunks[0]);
    thunks[thunk_count] = function;
    return THUNK_BASE + 4 * thunk_count++;
}

uint32_t rt_invoke(CPU *c, uint32_t address, int argc, const uint32_t *args) {
    assert(c == &cpu);
    const uint32_t self = c->r[1];
    c->r[7] ^= UINT32_C(0xa6a6a6a6);
    switch (address) {
    case HEAP_ALLOC:
        assert(self == FORM_HEAP && argc == 3 && args[0] == 0x30 && !args[1] && !args[2]);
        if (registrations && fail_form) return 0;
        return allocate(args[0], true);
    case VM_NATIVE_CTOR: {
        assert(argc == 4 && args[2] == 1 && args[3] <= 4 && !M32(self));
        Allocation *object = allocation_at(self, true);
        assert(strlen(GSTR(args[0])) < sizeof object->name && strlen(GSTR(args[1])) < sizeof object->group);
        strcpy(object->name, GSTR(args[0]));
        strcpy(object->group, GSTR(args[1]));
        if (args[3] && (!fail_parameter_row || strcmp(object->name, "Save")))
            object->parameters = allocate(args[3] * 8, false);
        M32(self + 0x18) = object->parameters;
        M32(self) = NATIVE_VTABLE;
        return self;
    }
    case REGISTRY_METHOD: {
        assert(self == REGISTRY && argc == 1 && registrations < sizeof objects / sizeof objects[0]);
        assert(M32(args[0] + 0x14) >= 2 && M32(args[0] + 0x14) <= 5);
        const Allocation *object = allocation_at(args[0], true);
        strcpy(function_names[registrations], object->name);
        strcpy(class_names[registrations], object->group);
        objects[registrations++] = args[0];
        return 0;
    }
    case VM_ARG_OFFSET:
        assert(self == LIST && argc == 1 && args[0] == STATE);
        return 2;
    case VM_ARG_GET:
        assert(self == LIST && argc == 3 && args[0] == STATE && args[1] < 4 && args[2] == 2);
        return ARGS + 8 * args[1];
    case FLAGS_METHOD:
        assert(self == REGISTRY && argc == 3 && registrations == 1 && args[2] == 1);
        return 0;
    case VM_DESTROY:
        assert(!argc);
        if (self == RESULT) ++result_deletions;
        else {
            assert(allocation_at(self, false) && M32(self) == 2);
            ++scratch_deletions;
        }
        if (M32(self) == 2 && is_fixture_string(M32(self + 4))) {
            const uint32_t address = M32(self + 4);
            unsigned *references = &string_refs[(address - STRING_POOL) / STRING_BYTES];
            assert(*references);
            if (!--*references) memset(GPTR(address), 0xdd, STRING_BYTES);
        }
        M32(self) = 0;
        M32(self + 4) = 0;
        return 0;
    case STRING_SET: {
        assert(argc == 1 && M32(self - 4) == 2 && !M32(self));
        assert(self != RESULT + 4 && allocation_at(self - 4, false) && args[0] == self + 4);
        M32(self) = refuse_string_intern ? 0 : intern_fixture_string(GSTR(args[0]));
        if (lose_frame_during_intern) frame_available = false;
        return self;
    }
    case DELETE_METHOD: {
        assert(argc == 1 && args[0] == 1 && self != objects[0]);
        const Allocation *object = allocation_at(self, true);
        if (object->parameters) release(object->parameters, false);
        ++form_deletions;
        release(self, true);
        return self;
    }
    default:
        assert(0 && "unexpected engine method");
        return 0;
    }
}

const SkseNative *skse_native_table(unsigned *count) {
    static const SkseNative builtin = {"SKSE", "GetVersionRelease", 3, 0, {0}, 0};
    *count = 1;
    return &builtin;
}

int skse_native_eval(const SkseNative *fn, const uint32_t args[4], const char *strings[4],
                     uint32_t *value, char **text) {
    assert(!strcmp(fn->name, "GetVersionRelease") && !args[0] && !strings[0]);
    *value = 7;
    *text = NULL;
    return 1;
}

int skse_system_run(CPU *c, const SkseNative *fn, uint32_t base, const uint32_t *args,
                    uint32_t registry, uint32_t result) {
    (void)c; (void)fn; (void)base; (void)args; (void)registry; (void)result;
    assert(0 && "unexpected system native");
    return 0;
}

static uint32_t run_thunk(CPU *owner, uint32_t thunk, uint32_t self,
                          uint32_t base, uint32_t registry, uint32_t result) {
    assert(thunk >= THUNK_BASE && (thunk - THUNK_BASE) / 4 < thunk_count);
    owner->r[1] = self;
    owner->r[4] = STACK;
    M32(STACK) = fixture_return_token;
    M32(STACK + 4) = base;
    M32(STACK + 8) = registry;
    M32(STACK + 12) = fixture_stack_id;
    M32(STACK + 16) = result;
    M32(STACK + 20) = STATE;
    thunks[(thunk - THUNK_BASE) / 4](owner);
    assert(owner->r[4] == STACK + 24);
    return owner->r[0];
}

static void register_functions(void) {
    M32(REGISTRY) = REGISTRY_VTABLE;
    M32(REGISTRY_VTABLE + REGISTER_SLOT * 4) = REGISTRY_METHOD;
    M32(REGISTRY_VTABLE + FLAGS_SLOT * 4) = FLAGS_METHOD;
    M32(NATIVE_VTABLE - 4) = 0x1010;
    M32(NATIVE_VTABLE) = DELETE_METHOD;
    const CPU saved = cpu;
    skse_register_natives(&cpu, REGISTRY);
    assert(!memcmp(&cpu, &saved, sizeof cpu));
    assert(!strcmp(class_names[0], "SKSE") && !strcmp(function_names[0], "GetVersionRelease"));
}

static void check_builtin(void) {
    const uint32_t builtin_run = M32(M32(objects[0]) + RUN_SLOT * 4);
    assert(run_thunk(&cpu, builtin_run, objects[0], 0, REGISTRY, RESULT));
    assert(M32(RESULT) == 3 && M32(RESULT + 4) == 48);
}

#ifdef SKYRIM_IOS_CLASSIC_INT0_PROBE
#define FIXTURE_EPOCH UINT64_C(0x1012345678)
#define FIXTURE_FUNCTION UINT32_C(0xb97a)
#define FIXTURE_BITS UINT32_C(0xf0123456)

typedef struct { unsigned calls; bool fail; } TestProvider;

static bool invoke_int0(void *context, uint64_t epoch, uint32_t function_id, uint32_t *bits) {
    TestProvider *provider = context;
    assert(epoch == FIXTURE_EPOCH && function_id == FIXTURE_FUNCTION && bits);
    ++provider->calls;
    assert(!skse_classic_int0_retire(epoch));
    *bits = FIXTURE_BITS;
    return !provider->fail;
}

typedef struct {
    unsigned calls, commits, discards;
    bool bad_type, invalidate, result_live, reject_commit, other_text;
} ValueProvider;

static bool read_values(void *context, CPU *owner, uint32_t address, void *out, size_t length) {
    assert(context && owner == &cpu);
    if ((address != STACK || length != 24) && (address != BASE || length != 8) &&
        (address != STATE || length != 4) &&
        (address < ARGS || address > ARGS + 24 || length != 8 || (address - ARGS) % 8)) return false;
    memcpy(out, GPTR(address), length);
    return true;
}

static bool copy_value_string(void *context, CPU *owner, uint32_t address, char *out, size_t limit) {
    assert(context && owner == &cpu);
    if (address < FILE_TEXT || address >= FILE_TEXT + 4 * INPUT_TEXT_BYTES ||
        (address - FILE_TEXT) % INPUT_TEXT_BYTES) return false;
    const char *text = GSTR(address), *end = memchr(text, 0, INPUT_TEXT_BYTES);
    if (!end || (size_t)(end - text) + 1 > limit) return false;
    memcpy(out, text, (size_t)(end - text) + 1);
    return true;
}

static bool owns_value_frame(void *context, CPU *owner, uint64_t epoch, const ClassicValueFrame *frame) {
    const ValueProvider *provider = context;
    if (owner != &cpu || epoch != FIXTURE_EPOCH || !provider->result_live || !frame_available ||
        frame->sp != STACK || frame->base != BASE || frame->registry != REGISTRY || frame->stack_id != 1 ||
        frame->result != RESULT || frame->state != STATE ||
        (frame->argument_list && frame->argument_list != LIST)) return false;
    for (unsigned i = 1; i < registrations; ++i) if (frame->object == objects[i]) return true;
    return false;
}

static bool invoke_values(void *context, uint64_t epoch, uint32_t method, const ClassicValue *args,
                          size_t count, ClassicValue *out, char *result_text, size_t result_capacity) {
    ValueProvider *provider = context;
    assert(epoch == FIXTURE_EPOCH && (method == 6 || method == 7) && args && out);
    assert(!result_text && !result_capacity);
    assert(!skse_classic_values_retire(epoch));
    assert(args[0].type == 2 && !args[0].bits && args[0].string != GSTR(FILE_TEXT));
    assert(!strcmp(args[0].string, "../WearableLanternsData/startup_test_file"));
    ++provider->calls;
    if (method == 6) {
        assert(count == 4 && args[1].type == 2 && args[1].string != GSTR(KEY_TEXT));
        assert(!strcmp(args[1].string, "SkyrimIOS"));
        assert(args[2].type == 3 && args[2].bits == 4 && args[3].type == 3 && args[3].bits == (uint32_t)-731);
        *out = (ClassicValue){.type = provider->bad_type ? 5u : 3u, .bits = args[3].bits};
    } else {
        assert(count == 2 && args[1].type == 5 && !args[1].bits);
        *out = (ClassicValue){.type = 5, .bits = provider->bad_type ? 2u : 1u};
    }
    if (provider->invalidate) provider->result_live = false;
    return true;
}

static void argument(unsigned index, uint32_t type, uint32_t bits) {
    M32(ARGS + index * 8) = type;
    M32(ARGS + index * 8 + 4) = bits;
}

static bool invoke_text_result(void *context, uint64_t epoch, uint32_t method, const ClassicValue *args,
                                size_t count, ClassicValue *out, char *text, size_t capacity) {
    ValueProvider *provider = context;
    static const char label[] = u8"ランタン";
    assert(epoch == FIXTURE_EPOCH && method == 8 && args && !count && out && text &&
           capacity == CLASSIC_VALUES_STRING_BYTES);
    ++provider->calls;
    *out = (ClassicValue){.type = CLASSIC_VALUES_STRING};
    if (provider->bad_type) memset(text, 'x', capacity);
    else if (provider->other_text) memcpy(text, "Pollen", sizeof "Pollen");
    else memcpy(text, label, sizeof label);
    if (provider->invalidate) provider->result_live = false;
    return true;
}

static void discard_value_result(void *context, uint64_t epoch) {
    ValueProvider *provider = context;
    assert(epoch == FIXTURE_EPOCH);
    ++provider->discards;
}

static bool commit_value_result(void *context, uint64_t epoch, uint32_t function) {
    ValueProvider *provider = context;
    assert(epoch == FIXTURE_EPOCH && function == 8 && provider->result_live);
    assert(!skse_classic_values_retire(epoch));
    ++provider->commits;
    return !provider->reject_commit;
}

static uint32_t run_values(uint32_t thunk, uint32_t self, uint32_t registry, uint32_t result) {
    return run_thunk(&cpu, thunk, self, BASE, registry, result);
}

static void test_string_result(void) {
    ValueProvider provider = {.result_live = true};
    const ClassicValueFunction method = {"JsonUtil", "ProbeProfileName", 8, CLASSIC_VALUES_STRING, 0, {0}};
    const ClassicValueRequest request = {
        .epoch = FIXTURE_EPOCH, .functions = &method, .count = 1, .read_guest = read_values,
        .owns_frame = owns_value_frame, .invoke = invoke_text_result, .commit = commit_value_result,
        .discard = discard_value_result, .context = &provider
    };
    assert(skse_classic_values_request(&request));
    register_functions();
    check_builtin();
    const uint32_t object = objects[1], run = M32(M32(object) + RUN_SLOT * 4);
    const unsigned destroyed = result_deletions;
    M32(RESULT) = CLASSIC_VALUES_INT;
    M32(RESULT + 4) = 7;
    provider.bad_type = true;
    assert(!run_values(run, object, REGISTRY, RESULT) && result_deletions == destroyed);
    assert(M32(RESULT) == CLASSIC_VALUES_INT && M32(RESULT + 4) == 7);
    provider.bad_type = false;
    provider.invalidate = true;
    assert(!run_values(run, object, REGISTRY, RESULT) && result_deletions == destroyed && !provider.commits);
    assert(M32(RESULT) == CLASSIC_VALUES_INT && M32(RESULT + 4) == 7);
    provider.result_live = true;
    provider.invalidate = false;
    fail_guest = guest_attempts + 1;
    assert(!run_values(run, object, REGISTRY, RESULT) && result_deletions == destroyed);
    assert(M32(RESULT) == CLASSIC_VALUES_INT && M32(RESULT + 4) == 7);
    const unsigned live = live_allocations(false);
    provider.reject_commit = true;
    assert(!run_values(run, object, REGISTRY, RESULT) && result_deletions == destroyed);
    assert(M32(RESULT) == CLASSIC_VALUES_INT && M32(RESULT + 4) == 7);
    assert(provider.calls == 3 && provider.commits == 1 && provider.discards == 3 && scratch_deletions == 1);
    assert(!string_refs[0] && live_allocations(false) == live);

    provider.reject_commit = false;
    assert(run_values(run, object, REGISTRY, RESULT) && result_deletions == destroyed + 1);
    const uint32_t installed = M32(RESULT + 4);
    assert(M32(RESULT) == CLASSIC_VALUES_STRING && is_fixture_string(installed) &&
           !strcmp(GSTR(installed), u8"ランタン") && live_allocations(false) == live);

    provider.reject_commit = true;
    provider.other_text = true;
    assert(!run_values(run, object, REGISTRY, RESULT) && result_deletions == destroyed + 1);
    assert(M32(RESULT) == CLASSIC_VALUES_STRING && M32(RESULT + 4) == installed &&
           !strcmp(GSTR(installed), u8"ランタン") && string_refs[0] == 1 && !string_refs[1]);
    assert(provider.commits == 3 && scratch_deletions == 2);

    refuse_string_intern = true;
    provider.reject_commit = false;
    assert(!run_values(run, object, REGISTRY, RESULT) && result_deletions == destroyed + 1);
    assert(M32(RESULT + 4) == installed && !strcmp(GSTR(installed), u8"ランタン") && provider.commits == 3);
    refuse_string_intern = false;
    lose_frame_during_intern = true;
    assert(!run_values(run, object, REGISTRY, RESULT) && result_deletions == destroyed + 1);
    assert(!frame_available && M32(RESULT + 4) == installed && !strcmp(GSTR(installed), u8"ランタン"));
    assert(provider.commits == 3 && scratch_deletions == 3 && string_refs[0] == 1 && !string_refs[1]);
    frame_available = true;
    assert(skse_classic_values_retire(FIXTURE_EPOCH));
    assert(!values_publication.request.owns_frame && !values_publication.request.commit);
    assert(!run_values(run, object, REGISTRY, RESULT) && provider.calls == 7 && provider.discards == 6 &&
           result_deletions == destroyed + 1);
}

#ifdef SKYRIM_TEST_CLASSIC_VALUES_ENGINE
static struct {
    ValueProvider provider;
    const SkseClassicDeclaration *rows[FIXTURE_FUNCTIONS];
    unsigned count;
    ClassicValueFixtureInvoke invoke;
    ClassicValueFixtureCommit commit;
    void (*discard)(void *);
    void *context;
} composed;

static bool call_composed_classic_value(void *context, uint64_t epoch, uint32_t method,
                                        const ClassicValue *args, size_t count, ClassicValue *out,
                                        char *result_text, size_t result_capacity) {
    assert(context == &composed.provider && epoch == FIXTURE_EPOCH && method >= 1 && method <= composed.count);
    const SkseClassicDeclaration *row = composed.rows[method - 1];
    assert(row && row->count == count);
    uint32_t bits[CLASSIC_VALUES_ARGS] = {0}, result;
    const char *strings[CLASSIC_VALUES_ARGS] = {0};
    for (size_t i = 0; i < count; ++i) {
        assert(args[i].type == row->arguments[i]);
        bits[i] = args[i].bits;
        strings[i] = args[i].string;
    }
    if (!composed.invoke(composed.context, row, bits, strings, &result, result_text, result_capacity)) return false;
    *out = (ClassicValue){.type = row->result, .bits = result};
    return true;
}

static void discard_composed(void *context, uint64_t epoch) {
    assert(context == &composed.provider && epoch == FIXTURE_EPOCH);
    if (composed.discard) composed.discard(composed.context);
}

static bool commit_composed(void *context, uint64_t epoch, uint32_t method) {
    assert(context == &composed.provider && epoch == FIXTURE_EPOCH && method >= 1 && method <= composed.count);
    return composed.commit(composed.context, composed.rows[method - 1]);
}

void classic_value_fixture_bind_acknowledged(const SkseClassicDeclaration *const *rows, unsigned count,
                                             ClassicValueFixtureInvoke invoke, ClassicValueFixtureCommit commit,
                                             void (*discard)(void *), void *context) {
    assert(rows && count && count <= FIXTURE_FUNCTIONS && invoke && !composed.invoke);
    composed.provider.result_live = true;
    memcpy(composed.rows, rows, count * sizeof rows[0]);
    composed.count = count;
    composed.invoke = invoke;
    composed.commit = commit;
    composed.discard = discard;
    composed.context = context;
    ClassicValueFunction methods[FIXTURE_FUNCTIONS] = {0};
    for (unsigned i = 0; i < count; ++i) {
        const SkseClassicDeclaration *row = composed.rows[i];
        assert(row->is_static && row->count <= CLASSIC_VALUES_ARGS);
        methods[i] = (ClassicValueFunction){row->class_name, row->name, i + 1, row->result, row->count, {0}};
        for (unsigned j = 0; j < row->count; ++j) methods[i].arguments[j] = (uint8_t)row->arguments[j];
    }
    const ClassicValueRequest request = {
        .epoch = FIXTURE_EPOCH, .functions = methods, .count = count, .read_guest = read_values,
        .copy_string = copy_value_string, .owns_frame = owns_value_frame, .invoke = call_composed_classic_value,
        .commit = commit ? commit_composed : NULL, .discard = discard_composed,
        .context = &composed.provider
    };
    assert(skse_classic_values_request(&request));
    register_functions();
    assert(registrations == count + 1 && thunk_count == 3);
    M32(STATE) = LIST;
}

void classic_value_fixture_bind(const SkseClassicDeclaration *const *rows, unsigned count,
                                ClassicValueFixtureInvoke invoke, void (*discard)(void *), void *context) {
    classic_value_fixture_bind_acknowledged(rows, count, invoke, NULL, discard, context);
}

bool classic_value_fixture_run(unsigned index, const uint32_t *bits, const char *const *strings, uint32_t *out,
                               char *result_text, size_t result_capacity) {
    assert(index < composed.count && bits && strings && out && composed.invoke);
    const SkseClassicDeclaration *row = composed.rows[index];
    unsigned text_index = 0;
    for (unsigned i = 0; i < row->count; ++i) {
        uint32_t value = bits[i];
        if (row->arguments[i] == 2) {
            assert(text_index < 4 && strings[i]);
            value = FILE_TEXT + INPUT_TEXT_BYTES * text_index++;
            const size_t length = strlen(strings[i]);
            assert(length < INPUT_TEXT_BYTES);
            memcpy(GPTR(value), strings[i], length + 1);
        }
        argument(i, row->arguments[i], value);
    }
    const uint32_t object = objects[index + 1], thunk = M32(M32(object) + RUN_SLOT * 4);
    if (!run_values(thunk, object, REGISTRY, RESULT) || M32(RESULT) != row->result)
        return false;
    if (row->result == 2) {
        const uint32_t value = M32(RESULT + 4);
        assert(result_text && is_fixture_string(value));
        const size_t length = strlen(GSTR(value)) + 1;
        if (length > result_capacity) return false;
        memcpy(result_text, GSTR(value), length);
        *out = 0;
        return true;
    }
    *out = M32(RESULT + 4);
    return true;
}

void classic_value_fixture_retire(void) {
    assert(composed.invoke && skse_classic_values_retire(FIXTURE_EPOCH));
    memset(&composed, 0, sizeof composed);
}
#endif

static void test_values(bool fail_registration) {
    ValueProvider provider = {.result_live = true};
    ClassicValueFunction methods[] = {
        {"JsonUtil", "IntListSet", 6, 3, 4, {2, 2, 3, 3}},
        {"JsonUtil", "Save", 7, 5, 2, {2, 5}}
    };
    ClassicValueRequest request = {
        .epoch = FIXTURE_EPOCH, .functions = methods, .count = 2, .read_guest = read_values,
        .copy_string = copy_value_string, .owns_frame = owns_value_frame, .invoke = invoke_values,
        .context = &provider
    };
    const ClassicValueFunction second = methods[1];
    methods[1].id = 6;
    assert(!skse_classic_values_request(&request));
    methods[1] = (ClassicValueFunction){"skse", "GetVersionRelease", 7, 3, 0, {0}};
    assert(!skse_classic_values_request(&request));
    methods[1] = second;
    request.copy_string = NULL;
    assert(!skse_classic_values_request(&request));
    request.copy_string = copy_value_string;
    request.owns_frame = NULL;
    assert(!skse_classic_values_request(&request));
    request.owns_frame = owns_value_frame;
    assert(skse_classic_values_request(&request));
    assert(!skse_classic_values_request(&request));
    const ClassicInt0Request conflict = {"jsonutil", "INTLISTSET", FIXTURE_EPOCH, FIXTURE_FUNCTION, invoke_int0, NULL};
    assert(!skse_classic_int0_request(&conflict));
    methods[0].name = "Altered";
    methods[0].arguments[0] = 5;
    fail_parameter_row = fail_registration;
    register_functions();
    check_builtin();
    if (fail_registration) {
        assert(registrations == 1 && thunk_count == 2 && form_deletions == 2);
        assert(live_allocations(false) == 1 && live_allocations(true) == 1);
        assert(!values_publication.registry && skse_classic_values_retire(FIXTURE_EPOCH));
        return;
    }
    assert(registrations == 3 && thunk_count == 3 && !form_deletions);
    assert(!strcmp(class_names[1], "JsonUtil") && !strcmp(function_names[1], "IntListSet"));
    assert(!strcmp(class_names[2], "JsonUtil") && !strcmp(function_names[2], "Save"));
    assert(M32(M32(objects[1] + 0x18) + 4) == 2 && M32(M32(objects[2] + 0x18) + 12) == 5);
    const uint32_t table = M32(objects[1]);
    assert(table == M32(objects[2]) && M32(table - 4) == 0x1010 && M32(table) == DELETE_METHOD);
    const uint32_t run = M32(table + RUN_SLOT * 4);
    M32(STATE) = LIST;
    memcpy(GPTR(FILE_TEXT), "../WearableLanternsData/startup_test_file", sizeof "../WearableLanternsData/startup_test_file");
    memcpy(GPTR(KEY_TEXT), "SkyrimIOS", sizeof "SkyrimIOS");
    argument(0, 2, FILE_TEXT);
    argument(1, 2, KEY_TEXT);
    argument(2, 3, 4);
    argument(3, 3, (uint32_t)-731);
    assert(run_values(run, objects[1], REGISTRY, RESULT));
    assert(M32(RESULT) == 3 && M32(RESULT + 4) == (uint32_t)-731 && provider.calls == 1);

    argument(1, 2, KEY_TEXT + 1);
    assert(!run_values(run, objects[1], REGISTRY, RESULT) && provider.calls == 1);
    argument(1, 3, 1);
    assert(!run_values(run, objects[1], REGISTRY, RESULT) && provider.calls == 1);
    argument(1, 5, 2);
    assert(!run_values(run, objects[2], REGISTRY, RESULT) && provider.calls == 1);
    argument(1, 5, 0);
    assert(!run_values(run, objects[2], REGISTRY + 4, RESULT) && provider.calls == 1);
    assert(!run_values(run, objects[2], REGISTRY, RESULT + 4) && provider.calls == 1);
    assert(!run_thunk(&cpu, run, objects[2], 0, REGISTRY, RESULT) && provider.calls == 1);
    fixture_return_token = FIXTURE_RUN_RETURN + 1;
    assert(!run_values(run, objects[2], REGISTRY, RESULT) && provider.calls == 1);
    fixture_return_token = FIXTURE_RUN_RETURN;
    fixture_stack_id = 2;
    assert(!run_values(run, objects[2], REGISTRY, RESULT) && provider.calls == 1);
    fixture_stack_id = 1;
    M32(BASE) = CLASSIC_VALUES_INT;
    assert(!run_values(run, objects[2], REGISTRY, RESULT) && provider.calls == 1);
    M32(BASE) = 0;
    M32(STATE) = LIST + 4;
    assert(!run_values(run, objects[2], REGISTRY, RESULT) && provider.calls == 1);
    M32(STATE) = LIST;

    provider.bad_type = true;
    assert(!run_values(run, objects[2], REGISTRY, RESULT) && provider.calls == 2);
    assert(M32(RESULT) == 3 && M32(RESULT + 4) == (uint32_t)-731 && result_deletions == 2);
    provider.bad_type = false;
    provider.invalidate = true;
    assert(!run_values(run, objects[2], REGISTRY, RESULT) && provider.calls == 3);
    assert(M32(RESULT) == 3 && M32(RESULT + 4) == (uint32_t)-731 && result_deletions == 2);
    provider.result_live = true;
    provider.invalidate = false;
    argument(1, 5, UINT32_C(0x91827300));
    assert(run_values(run, objects[2], REGISTRY, RESULT) && provider.calls == 4);
    assert(M32(RESULT) == 5 && M32(RESULT + 4) == 1 && result_deletions == 3);
    assert(skse_classic_values_retire(FIXTURE_EPOCH));
    assert(!run_values(run, objects[2], REGISTRY, RESULT) && provider.calls == 4);
}

static void test_probe(const char *mode) {
    char type[] = "ClassicLabFixture", name[] = "IntegerProbe";
    TestProvider provider = {0};
    ClassicInt0Request request = {type, "invalid-name", FIXTURE_EPOCH, FIXTURE_FUNCTION, invoke_int0, &provider};
    assert(!skse_classic_int0_request(&request));
    char overlong[CLASSIC_INT0_NAME_BYTES + 1];
    memset(overlong, 'A', sizeof overlong - 1);
    overlong[sizeof overlong - 1] = 0;
    request.function_name = overlong;
    assert(!skse_classic_int0_request(&request));
    request.class_name = "skse";
    request.function_name = "getversionrelease";
    assert(!skse_classic_int0_request(&request));
    request.class_name = type;
    request.function_name = name;
    assert(skse_classic_int0_request(&request));
    assert(!skse_classic_int0_request(&request));
    type[0] = name[0] = 'X';

    if (!strcmp(mode, "names")) fail_guest = 1;
    if (!strcmp(mode, "table")) fail_guest = 2;
    if (!strcmp(mode, "object")) fail_form = true;
    register_functions();
    check_builtin();
    if (fail_guest || fail_form) {
        assert(registrations == 1 && thunk_count == 2 && provider.calls == 0);
        assert(live_allocations(false) == 1 && live_allocations(true) == 1);
        assert(!int0_publication.object && skse_classic_int0_retire(FIXTURE_EPOCH));
        return;
    }

    assert(registrations == 2 && thunk_count == 3);
    assert(!strcmp(class_names[1], "ClassicLabFixture") && !strcmp(function_names[1], "IntegerProbe"));
    assert(live_allocations(false) == 2 && live_allocations(true) == 2);
    const uint32_t object = objects[1], vtable = M32(object), run = M32(vtable + RUN_SLOT * 4);
    assert(M32(vtable - 4) == 0x1010 && M32(vtable) == DELETE_METHOD);
    assert(!skse_classic_int0_retire(FIXTURE_EPOCH + 1));
    M32(RESULT) = 2;
    M32(RESULT + 4) = 0x11223344;
    CPU other = {0};
    assert(!run_thunk(&other, run, object, 0, REGISTRY, RESULT));
    assert(!run_thunk(&cpu, run, UINT32_MAX, 0, REGISTRY, RESULT));
    assert(!run_thunk(&cpu, run, object, 0, REGISTRY + 4, RESULT));
    assert(!run_thunk(&cpu, run, object, 2, REGISTRY, RESULT));
    assert(!run_thunk(&cpu, run, object, 0, REGISTRY, UINT32_MAX - 3));
    M32(object + 0x2c) ^= 1;
    assert(!run_thunk(&cpu, run, object, 0, REGISTRY, RESULT));
    M32(object + 0x2c) ^= 1;
    assert(provider.calls == 0 && result_deletions == 1 && M32(RESULT + 4) == 0x11223344);

    provider.fail = true;
    assert(!run_thunk(&cpu, run, object, 0, REGISTRY, RESULT));
    assert(provider.calls == 1 && result_deletions == 1 && M32(RESULT) == 2 && M32(RESULT + 4) == 0x11223344);
    provider.fail = false;
    assert(run_thunk(&cpu, run, object, 0, REGISTRY, RESULT));
    assert(provider.calls == 2 && result_deletions == 2 && M32(RESULT) == 3 && M32(RESULT + 4) == FIXTURE_BITS);

    assert(skse_classic_int0_retire(FIXTURE_EPOCH));
    assert(!skse_classic_int0_retire(FIXTURE_EPOCH) && !skse_classic_int0_request(&request));
    assert(!run_thunk(&cpu, run, object, 0, REGISTRY, RESULT));
    assert(provider.calls == 2 && result_deletions == 2 && M32(RESULT + 4) == FIXTURE_BITS);
    assert(form_deletions == 0 && live_allocations(true) == 2);
    const uint32_t delete_flag = 1;
    skse_virtual(&cpu, object, 0, 1, &delete_flag);
    assert(form_deletions == 1 && live_allocations(true) == 1);
}
#endif

#ifdef CLASSIC_INT0_TEST_INTERPRETER
enum { INTERPRETED_EPOCH = 41, INTERPRETED_FUNCTION = 9, INTERPRETED_WORK = 1000 };

typedef struct {
    SksePluginSession *session;
    SksePapyrusTrialStatus status;
    SkseX86Result call;
    unsigned calls;
} InterpretedProvider;

static bool invoke_interpreted_int0(void *context, uint64_t epoch, uint32_t function_id, uint32_t *bits) {
    InterpretedProvider *provider = context;
    assert(provider->session && epoch == INTERPRETED_EPOCH && function_id == INTERPRETED_FUNCTION);
    ++provider->calls;
    provider->status = skse_plugin_session_run_papyrus_fixture(provider->session, INTERPRETED_WORK, 17,
                                                               bits, &provider->call);
    return provider->status == SKSE_PAPYRUS_TRIAL_OK;
}

static SksePluginSession *open_interpreted_fixture(const char *path) {
    uint8_t bytes[64 * 1024];
    FILE *file = fopen(path, "rb");
    assert(file);
    const size_t size = fread(bytes, 1, sizeof bytes, file);
    assert(size && size < sizeof bytes && feof(file) && !ferror(file));
    assert(!fclose(file));
    const SkseTrialDependency primary = {"Fixture.dll", bytes, size};
    const SkseTrialOptions options = {
        .skse_version = 0x01070030, .runtime_version = 0x09200000,
        .release = 48, .plugin_handle = 1, .expose_release = 1, .expose_handle = 1
    };
    const SkseTrialRuntime runtime = {.papyrus_probe_epoch = INTERPRETED_EPOCH, .papyrus_fixture_int0 = true};
    SkseTrialResult report;
    SksePluginSession *session = skse_plugin_session_open(primary, NULL, 0, options, runtime, &report);
    if (!session)
        fprintf(stderr, "Int0 fixture open: trial=%u PE=%u Load=%u\n",
                (unsigned)report.status, (unsigned)report.image_status, (unsigned)report.load.status);
    assert(session && !strcmp(report.name, "OwnedPapyrusFixture"));
    assert(skse_plugin_session_step(session, SKSE_SESSION_POST_LOAD, INTERPRETED_WORK, 11) == SKSE_SESSION_OK);
    assert(skse_plugin_session_step(session, SKSE_SESSION_POST_POST_LOAD, INTERPRETED_WORK, 12) == SKSE_SESSION_OK);
    SkseX86Result callback;
    const SksePapyrusTrialReport registered = skse_plugin_session_probe_papyrus(
        session, INTERPRETED_EPOCH, INTERPRETED_WORK, 13, &callback);
    assert(registered.status == SKSE_PAPYRUS_TRIAL_OK && registered.attempted == 1);
    assert(callback.status == SKSE_X86_OK && (uint8_t)callback.eax == 1 && callback.work > 0);
    return session;
}

static void test_interpreted_int0(const char *path, bool bad_type) {
    InterpretedProvider provider = {.session = open_interpreted_fixture(path)};
    const ClassicInt0Request request = {"ClassicLabFixture", "InterpretedInteger", INTERPRETED_EPOCH,
                                       INTERPRETED_FUNCTION, invoke_interpreted_int0, &provider};
    assert(skse_classic_int0_request(&request));
    register_functions();
    check_builtin();
    assert(registrations == 2 && provider.calls == 0 && result_deletions == 1);
    assert(!strcmp(function_names[1], "InterpretedInteger"));

    const uint32_t sentinel = UINT32_C(0x55aa33cc);
    M32(RESULT) = 2;
    M32(RESULT + 4) = sentinel;
    const uint32_t object = objects[1], run = M32(M32(object) + RUN_SLOT * 4);
    const bool returned = run_thunk(&cpu, run, object, 0, REGISTRY, RESULT) != 0;
    assert(provider.calls == 1 && provider.call.status == SKSE_X86_OK && provider.call.work > 0);
    assert((uint8_t)provider.call.eax == 1);
    if (bad_type) {
        assert(!returned && provider.status == SKSE_PAPYRUS_TRIAL_RESULT_REFUSED);
        assert(result_deletions == 1 && M32(RESULT) == 2 && M32(RESULT + 4) == sentinel);
        assert(skse_plugin_session_report(provider.session)->status == SKSE_TRIAL_PAPYRUS_FAULT);
    } else {
        assert(returned && provider.status == SKSE_PAPYRUS_TRIAL_OK);
        assert(result_deletions == 2 && M32(RESULT) == 3 && M32(RESULT + 4) == UINT32_C(0xfe123456));
    }

    assert(skse_classic_int0_retire(INTERPRETED_EPOCH));
    skse_plugin_session_close(provider.session, NULL);
    provider.session = NULL;
    const uint32_t last_type = M32(RESULT), last_bits = M32(RESULT + 4);
    assert(!run_thunk(&cpu, run, object, 0, REGISTRY, RESULT));
    assert(provider.calls == 1 && M32(RESULT) == last_type && M32(RESULT + 4) == last_bits);
    const uint32_t delete_flag = 1;
    skse_virtual(&cpu, object, 0, 1, &delete_flag);
    assert(form_deletions == 1);
}
typedef struct {
    uint8_t leaf[6], run[0x50];
    uint32_t leaf_rva, run_rva, base;
    bool unexpected_arguments;
    unsigned calls;
} OwnedIntegerMethod;

static uint32_t read_fixture_rva(FILE *file) {
    uint8_t bytes[4];
    assert(fread(bytes, 1, sizeof bytes, file) == sizeof bytes);
    return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static bool invoke_owned_method(void *context, uint64_t epoch, uint32_t function_id, uint32_t *bits) {
    OwnedIntegerMethod *method = context;
    assert(epoch == INTERPRETED_EPOCH && function_id == INTERPRETED_FUNCTION);
    ++method->calls;
    /* The exact DLL Run method calls two engine routines. This private frame
     * models their simple zero-argument and None-result cases. The fixtures
     * trap if either precondition changes. It never runs the live routines. */
    uint8_t empty_arg_list[] = {0x83, 0x79, 0x28, 0, 0x75, 5, 0x31, 0xc0, 0xc2, 4, 0, 0xcc};
    uint8_t none_result[] = {0x83, 0x39, 0, 0x75, 1, 0xc3, 0xcc};
    const uint32_t leaf = method->base + method->leaf_rva, run = method->base + method->run_rva;
    const unsigned rx = SKSE_PE_READ | SKSE_PE_EXEC;
    SksePeImage images[] = {
        {.base = leaf, .size = sizeof method->leaf, .bytes = method->leaf, .section_count = 1,
         .sections = {{0, sizeof method->leaf, rx}}},
        {.base = run, .size = sizeof method->run, .bytes = method->run, .section_count = 1,
         .sections = {{0, sizeof method->run, rx}}},
        {.base = 0xc3a620, .size = sizeof empty_arg_list, .bytes = empty_arg_list, .section_count = 1,
         .sections = {{0, sizeof empty_arg_list, rx}}},
        {.base = 0xc328e0, .size = sizeof none_result, .bytes = none_result, .section_count = 1,
         .sections = {{0, sizeof none_result, rx}}}
    };
    SksePeImage *space[] = {images, images + 1, images + 2, images + 3};
    enum { Shadow = 0x20000000, Value = 0x40, Frame = 0x50, Arguments = 0x60 };
    uint32_t scratch[128] = {0};
    scratch[0x2c / 4] = leaf;
    scratch[Frame / 4] = Shadow + Arguments;
    scratch[(Arguments + 0x28) / 4] = method->unexpected_arguments;
    const uint32_t args[] = {0, 0, 1, Shadow + Value, Shadow + Frame};
    const SkseX86Result result = skse_x86_call_thiscall(space, sizeof space / sizeof space[0], run, Shadow,
        (SkseX86Scratch){Shadow, (uint8_t *)scratch, sizeof scratch}, args, sizeof args / sizeof args[0], 128,
        (SkseX86Services){.stack_floor = Shadow + 0xa0});
    if (result.status != SKSE_X86_OK || (uint8_t)result.eax != 1 || scratch[Value / 4] != SKSE_CLASSIC_PAPYRUS_INT)
        return false;
    *bits = scratch[Value / 4 + 1];
    return true;
}

static void test_owned_method(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file);
    OwnedIntegerMethod method = {.base = 0x10000000};
    method.leaf_rva = read_fixture_rva(file);
    assert(fread(method.leaf, 1, sizeof method.leaf, file) == sizeof method.leaf);
    method.run_rva = read_fixture_rva(file);
    assert(fread(method.run, 1, sizeof method.run, file) == sizeof method.run);
    assert(fgetc(file) == EOF && !ferror(file));
    assert(!fclose(file));
    assert(method.leaf_rva < 0x1000000 && method.run_rva < 0x1000000);
    assert(method.leaf[0] == 0xb8 && method.leaf[5] == 0xc3);
    const ClassicInt0Request request = {"PapyrusUtil", "GetVersion", INTERPRETED_EPOCH, INTERPRETED_FUNCTION,
                                        invoke_owned_method, &method};
    assert(skse_classic_int0_request(&request));
    register_functions();
    check_builtin();
    assert(registrations == 2 && !strcmp(function_names[1], "GetVersion") && method.calls == 0);
    const uint32_t object = objects[1], run = M32(M32(object) + RUN_SLOT * 4);
    M32(RESULT) = 2;
    M32(RESULT + 4) = 80;
    assert(run_thunk(&cpu, run, object, 0, REGISTRY, RESULT));
    assert(M32(RESULT) == 3 && M32(RESULT + 4) == 33 && method.calls == 1 && result_deletions == 2);
    method.base = 0x12000000;
    assert(run_thunk(&cpu, run, object, 0, REGISTRY, RESULT));
    assert(M32(RESULT) == 3 && M32(RESULT + 4) == 33 && method.calls == 2 && result_deletions == 3);
    method.unexpected_arguments = true;
    assert(!run_thunk(&cpu, run, object, 0, REGISTRY, RESULT));
    assert(M32(RESULT) == 3 && M32(RESULT + 4) == 33 && method.calls == 3 && result_deletions == 3);
    assert(skse_classic_int0_retire(INTERPRETED_EPOCH));
    assert(!run_thunk(&cpu, run, object, 0, REGISTRY, RESULT));
    assert(method.calls == 3 && M32(RESULT) == 3 && M32(RESULT + 4) == 33);
    const uint32_t delete_flag = 1;
    skse_virtual(&cpu, object, 0, 1, &delete_flag);
    assert(form_deletions == 1);
}
#endif

#ifdef SKYRIM_TEST_CLASSIC_VALUES_ENGINE
int classic_engine_fixture_standalone_modes(int argc, char **argv) {
#else
int main(int argc, char **argv) {
#endif
#ifdef CLASSIC_INT0_TEST_INTERPRETER
    if (argc == 3 && !strcmp(argv[1], "owned-wrapper")) {
        test_owned_method(argv[2]);
        return 0;
    }
    if (argc == 3 && (!strcmp(argv[1], "interpreter") || !strcmp(argv[1], "interpreter-bad"))) {
        test_interpreted_int0(argv[2], !strcmp(argv[1], "interpreter-bad"));
        return 0;
    }
#endif
    assert(argc == 2);
    if (!strcmp(argv[1], "default")) {
        register_functions();
        check_builtin();
        assert(registrations == 1 && thunk_count == 2);
        assert(live_allocations(false) == 1 && live_allocations(true) == 1);
        return 0;
    }
#ifdef SKYRIM_IOS_CLASSIC_INT0_PROBE
    if (!strcmp(argv[1], "values-string"))
        test_string_result();
    else if (!strcmp(argv[1], "values") || !strcmp(argv[1], "values-rollback"))
        test_values(!strcmp(argv[1], "values-rollback"));
    else test_probe(argv[1]);
    return 0;
#else
    return 1;
#endif
}
