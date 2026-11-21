/* SPDX-License-Identifier: GPL-3.0-only */
#include "guest_memory.h"
#include "lift_resource_lookup.h"
#include "lift_prelude.h"
#include "funcs_exe.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {
    STREAM = 0x2140, STREAM_VTABLE = 0x2200, CALLER_OFFSET = 0x2800, RESOLVE_OUTPUT = 0x2804,
    DEFAULT_ROOT = 0x2900, PATH_CAPACITY = 260,
    STACK_LIMIT = 0x10000, STACK = 0x17000, STACK_BASE = 0x18000,
    // lift_resource_lookup.c places the name and object below the live stack.
    PATH = STACK - SKYRIM_FUZ_GUEST_LOOKUP_GAP - SKYRIM_FUZ_GUEST_LOOKUP_NAME,
    OBJECT = PATH - SKYRIM_FUZ_GUEST_LOOKUP_OBJECT,
    // A response whose stack path frame begins at STACK + 0x14.
    RESPONSE = 0x3200, RESPONSE_TEXT = 0x3240,
    COMPARE_EXCHANGE_SLOT = 0x106b14c, ROOT_VTABLE = 0x110cc68, DEFAULT_ROOT_SLOT = 0x1b41100,
    INPUT_STREAM_VTABLE = 0x111eaf8, HEAP = 0x1b418b0,
    STREAM_REFERENCE = 0x1000, INITIAL_STREAM_FLAGS = 0x100, FIXTURE_CURSOR = 0x4321,
    TEST_COMPARE_EXCHANGE = 0x300, TEST_RESOLVE = 0x304, TEST_OPEN = 0x308, TEST_CLOSE = 0x30c,
    TEST_DISPOSE = 0x310,
    // Authored backend statuses. The lift uses zero/nonzero, not these meanings.
    FIXTURE_NOT_FOUND = 17, FIXTURE_RESOLVER_FAILURE = 19, FIXTURE_OPEN_FAILURE = 21
};

typedef struct {
    const char *path;
    uint32_t lookup_status, open_status;
} Resource;

typedef struct {
    const Resource *entries;
    size_t count;
    unsigned probes, opens, closes, disposals, frees, names;
    const Resource *resolved;
    uint32_t lookup_status;
} Fixture;

uint8_t *MEM;
static Fixture *active;
static gm_space space;
static const CPU caller = {0}; // The reviewed routines run with the fixture's zero TLS base.
static const SkyrimFuzGuestResources resources = {
    .caller = &caller, .live_stack = STACK,
    .stack_limit = STACK_LIMIT, .stack_base = STACK_BASE, .nested_bytes = 0x4000
};

static uint32_t argument(CPU *cpu, unsigned index) {
    return M32(cpu->r[4] + 4 * (index + 1));
}

static void return_guest(CPU *cpu, unsigned popped_arguments, uint32_t value) {
    cpu->r[0] = value;
    cpu->r[4] += 4 * (popped_arguments + 1);
}

/* The authored runtime models name references and a cache miss. Those
 * methods do not implement the engine hash table or the FormHeap. */
void f_A511C0(CPU *cpu) {
    assert(argument(cpu, 0) == PATH && active->names == 0);
    M32(cpu->r[1]) = PATH;
    ++active->names;
    return_guest(cpu, 1, cpu->r[1]);
}

void f_A50D50(CPU *cpu) {
    assert(cpu->r[1] == OBJECT + 12 && M32(argument(cpu, 0)) == PATH && active->names == 1);
    M32(cpu->r[1]) = PATH;
    ++active->names;
    return_guest(cpu, 1, cpu->r[1]);
}

void f_A511B0(CPU *cpu) {
    assert(M32(cpu->r[1]) == PATH && active->names);
    M32(cpu->r[1]) = 0;
    --active->names;
    return_guest(cpu, 0, 0);
}

void f_A42320(CPU *cpu) {
    assert(argument(cpu, 0) == PATH);
    memset(MEM + cpu->r[1], 0, 12);
    return_guest(cpu, 1, cpu->r[1]);
}

void f_A3F780(CPU *cpu) {
    assert(M32(argument(cpu, 0)) == 0 && M32(argument(cpu, 1)) == 0);
    return_guest(cpu, 0, 1);
}

/* The stack-owned stream object is destroyed with flag 0, never freed. */
void f_A487B0(CPU *cpu) {
    (void)cpu;
    ++active->frees;
    assert(!"stack-owned BSIStream reached the heap release");
}

static void resolve_from_catalog(CPU *cpu) {
    uint32_t wrapper = cpu->r[1], output = argument(cpu, 1);
    assert(M32(wrapper) == ROOT_VTABLE && M32(wrapper + 8) == 0 && M32(wrapper + 12) == DEFAULT_ROOT);
    uint32_t auxiliary = argument(cpu, 2);
    assert(argument(cpu, 0) == PATH && auxiliary == wrapper + 0x1c &&
           !M32(auxiliary) && !argument(cpu, 3));
    assert(!M32(output) && memchr(MEM + PATH, 0, PATH_CAPACITY));
    ++active->probes;
    active->resolved = NULL;
    active->lookup_status = FIXTURE_NOT_FOUND;
    for (size_t i = 0; i < active->count; ++i) {
        const Resource *resource = &active->entries[i];
        if (!strcmp(resource->path, (const char *)MEM + PATH)) {
            active->resolved = resource;
            active->lookup_status = resource->lookup_status;
            if (!resource->lookup_status) {
                M32(STREAM) = STREAM_VTABLE;
                M32(STREAM + 8) = STREAM_REFERENCE | INITIAL_STREAM_FLAGS;
                M32(STREAM + 16) = FIXTURE_CURSOR;
                M32(output) = STREAM;
            }
            break;
        }
    }
    return_guest(cpu, 4, active->lookup_status);
}

void rt_call(CPU *cpu, uint32_t target) {
    switch (target) {
    case TEST_COMPARE_EXCHANGE: {
        uint32_t address = argument(cpu, 0);
        assert(address == STREAM + 8);
        uint32_t previous = M32(address);
        if (previous == argument(cpu, 2)) M32(address) = argument(cpu, 1);
        return_guest(cpu, 3, previous);
        return;
    }
    case TEST_RESOLVE:
        resolve_from_catalog(cpu);
        return;
    case TEST_OPEN:
        assert(cpu->r[1] == STREAM && active->resolved);
        assert(M32(STREAM + 8) == (STREAM_REFERENCE | INITIAL_STREAM_FLAGS | 0xa));
        assert(M32(STREAM + 16) == FIXTURE_CURSOR);
        ++active->opens;
        return_guest(cpu, 0, active->resolved->open_status);
        return;
    case TEST_CLOSE:
        assert(cpu->r[1] == STREAM && M32(STREAM + 8) == (STREAM_REFERENCE | INITIAL_STREAM_FLAGS));
        assert(M32(STREAM + 16) == FIXTURE_CURSOR);
        ++active->closes;
        return_guest(cpu, 0, 0);
        return;
    case TEST_DISPOSE:
        assert(cpu->r[1] == STREAM && argument(cpu, 0) == 1 && M32(STREAM + 8) == INITIAL_STREAM_FLAGS);
        assert(M32(STREAM + 16) == FIXTURE_CURSOR);
        ++active->disposals;
        return_guest(cpu, 1, STREAM);
        return;
    default:
        assert(false); // An unreviewed read, seek or other target fails the fixture.
    }
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    (void)cache;
    rt_call(cpu, target);
}

/* These private routines are compiled from the pinned lift in scratch.
 * Every address supplied here refers to an authored, owned guest range. The
 * policy reaches them only through lift_resource_lookup.c. */
static SkyrimFuzAvailability lookup_lifted(void *context, const char *relative) {
    Fixture *fixture = context;
    assert(active == fixture && !fixture->names);
    const unsigned probes = fixture->probes, streams = fixture->disposals;
    SkyrimFuzGuestResources call = resources;
    const SkyrimFuzAvailability found = skyrim_fuz_guest_resource_lookup(&call, relative);
    assert(!strcmp((const char *)MEM + PATH, relative));
    assert(fixture->probes == probes + 1 && !fixture->names && !fixture->frees);
    assert(M32(CALLER_OFFSET) == FIXTURE_CURSOR);
    const bool opened = fixture->resolved && !fixture->lookup_status;
    assert(fixture->disposals == streams + opened);
    if (found == SKYRIM_FUZ_PRESENT) assert(opened && !fixture->resolved->open_status);
    // A known resolver failure without a stream is ABSENT, as in LE 6.1.
    if (found == SKYRIM_FUZ_ABSENT) assert(!opened);
    if (found == SKYRIM_FUZ_LOOKUP_ERROR) assert(opened && fixture->resolved->open_status);
    return found;
}

static Fixture *begin(Fixture *fixture, const Resource *entries, size_t count) {
    *fixture = (Fixture){.entries = entries, .count = count};
    active = fixture;
    return fixture;
}

static const char *const original[] = {
    "Sound/Voice/Fixture.esp/Narrator/Test.wav",
    "Sound/Voice/Fixture.esp/Narrator/Test.fuz",
    "Sound/Voice/Fixture.esp/Narrator/Test.xwm"
};
static const char stock[] = "Sound/Voice/OwnerSample/Stock_4.xwm";
static const char packed[] = "Sound/Voice/OwnerSample/Stock_4.fuz";
static const char response[] = "The gate remains closed until sunrise";

static void check_lifetime(const Fixture *fixture, unsigned probes, unsigned streams) {
    assert(fixture->probes == probes && !fixture->frees && !fixture->names);
    assert(fixture->opens == streams && fixture->closes == streams && fixture->disposals == streams);
}

static uint32_t probe_resolver_failure(Fixture *fixture, const char *path) {
    const size_t length = strlen(path);
    assert(length < PATH_CAPACITY && active == fixture);
    memcpy(MEM + PATH, path, length + 1);
    CPU cpu = {0};
    cpu.r[4] = STACK;
    M32(STACK) = 0;
    M32(STACK + 4) = PATH;
    M32(STACK + 8) = RESOLVE_OUTPUT;
    M32(STACK + 12) = M32(STACK + 16) = M32(RESOLVE_OUTPUT) = 0;
    f_A3F810(&cpu);
    assert(cpu.r[4] == STACK + 4 && !M32(RESOLVE_OUTPUT) && !fixture->opens && !fixture->frees);
    return cpu.r[0];
}

static bool read_guest(void *context, uint32_t address, void *bytes, size_t length) {
    const void *data = gm_pointer(context, address, length);
    if (data) memcpy(bytes, data, length);
    return data != NULL;
}

static bool replace_guest(void *context, uint32_t address, const void *expected, const void *replacement,
                          size_t length) {
    void *data = gm_pointer(context, address, length);
    if (!data || memcmp(data, expected, length)) return false;
    memcpy(data, replacement, length);
    return true;
}

static const char game_original[] = "Data\\Sound\\Voice\\Fixture.esp\\Narrator\\Test.wav";
static const char game_stock[] = "Data\\Sound\\Voice\\OwnerSample\\Stock_4.xwm";

/* The pinned constructor's stack argument slot, path frame and response. */
static bool choose_in_game(const SkyrimFuzPolicy *policy, SkyrimFuzLiftChoice *choice) {
    const uint32_t frame = STACK + SKYRIM_FUZ_LIFT_FRAME_FROM_SLOT;
    M32(STACK) = frame;
    memset(MEM + frame, 0, SKYRIM_FUZ_LIFT_FRAME_BYTES);
    memcpy(MEM + frame, game_original, sizeof game_original);
    memcpy(MEM + RESPONSE_TEXT, response, sizeof response);
    M32(RESPONSE) = RESPONSE_TEXT;
    M32(RESPONSE + 4) = (uint32_t)(sizeof response - 1) | ((uint32_t)sizeof response << 16);
    const SkyrimFuzLiftMemory access = {read_guest, replace_guest, &space};
    const bool changed = skyrim_fuz_choose_with_guest_resources(policy, &access, &resources, RESPONSE, choice);
    assert(M32(STACK) == frame && !memcmp(MEM + RESPONSE_TEXT, response, sizeof response));
    const char *expected = changed ? game_stock : game_original;
    assert(!strcmp((const char *)MEM + frame, expected));
    if (changed) assert(!strcmp(choice->assigned_path, game_stock) && choice->force_subtitle);
    else assert(!choice->assigned_path[0] && !choice->force_subtitle);
    return changed;
}

int main(void) {
    assert(gm_reserve(&space, NULL));
    for (uint64_t page = STACK_LIMIT; page < STACK_BASE; page += space.page_size)
        assert(gm_commit_page(&space, page, NULL));
    const uint32_t addresses[] = {STREAM, RESPONSE, COMPARE_EXCHANGE_SLOT, ROOT_VTABLE, DEFAULT_ROOT_SLOT};
    for (size_t i = 0; i < sizeof addresses / sizeof addresses[0]; ++i) {
        const uint64_t page = (addresses[i] / space.page_size) * space.page_size;
        if (!gm_pointer(&space, addresses[i], 1)) assert(gm_commit_page(&space, page, NULL));
    }
    MEM = space.base;
    SkyrimFuzGuestLookupLayout layout;
    assert(skyrim_fuz_guest_lookup_layout(&resources, &layout) && layout.name == PATH && layout.object == OBJECT);
    M32(COMPARE_EXCHANGE_SLOT) = TEST_COMPARE_EXCHANGE;
    M32(ROOT_VTABLE + 12) = TEST_RESOLVE;
    M32(DEFAULT_ROOT_SLOT) = DEFAULT_ROOT;
    M32(STREAM_VTABLE) = TEST_DISPOSE;
    M32(STREAM_VTABLE + 4) = TEST_OPEN;
    M32(STREAM_VTABLE + 8) = TEST_CLOSE;
    M32(CALLER_OFFSET) = FIXTURE_CURSOR;

    Resource resource = {0};
    Fixture fixture = {0};
    SkyrimFuzPolicy policy = {.lookup = lookup_lifted, .context = &fixture, .words_per_second = 2,
                              .skip_empty_responses = true};
    policy.stock_paths[3] = stock;
    for (unsigned i = 0; i < 3; ++i) {
        resource = (Resource){.path = original[i]};
        begin(&fixture, &resource, 1);
        SkyrimFuzVoice voice = skyrim_fuz_choose_voice(&policy, original[0], response, false);
        assert(voice.decision == SKYRIM_FUZ_KEEP_VOICE && !voice.stock_path && !voice.force_subtitle);
        check_lifetime(&fixture, i + 1, 1);
    }

    resource = (Resource){.path = original[0], .lookup_status = FIXTURE_RESOLVER_FAILURE};
    begin(&fixture, &resource, 1);
    assert(probe_resolver_failure(&fixture, original[0]) == FIXTURE_RESOLVER_FAILURE);
    begin(&fixture, NULL, 0);
    assert(probe_resolver_failure(&fixture, original[0]) == FIXTURE_NOT_FOUND);
    // BSIStream drops the resolver status. LE 6.1 also reads this as missing.
    begin(&fixture, &resource, 1);
    assert(lookup_lifted(&fixture, original[0]) == SKYRIM_FUZ_ABSENT);
    assert(fixture.lookup_status == FIXTURE_RESOLVER_FAILURE);
    check_lifetime(&fixture, 1, 0);

    resource = (Resource){.path = original[0], .open_status = FIXTURE_OPEN_FAILURE};
    begin(&fixture, &resource, 1);
    assert(lookup_lifted(&fixture, original[0]) == SKYRIM_FUZ_LOOKUP_ERROR);
    check_lifetime(&fixture, 1, 1);

    resource = (Resource){.path = stock};
    begin(&fixture, &resource, 1);
    SkyrimFuzVoice voice = skyrim_fuz_choose_voice(&policy, original[0], response, false);
    assert(voice.decision == SKYRIM_FUZ_USE_STOCK && voice.stock_path == stock && voice.seconds == 4);
    check_lifetime(&fixture, 4, 1);

    resource = (Resource){.path = packed};
    begin(&fixture, &resource, 1);
    assert(lookup_lifted(&fixture, packed) == SKYRIM_FUZ_PRESENT);
    assert(lookup_lifted(&fixture, stock) == SKYRIM_FUZ_ABSENT);
    assert(fixture.lookup_status == FIXTURE_NOT_FOUND);
    check_lifetime(&fixture, 2, 1);
    begin(&fixture, &resource, 1);
    voice = skyrim_fuz_choose_voice(&policy, original[0], response, false);
    assert(voice.decision == SKYRIM_FUZ_STOCK_UNAVAILABLE && !voice.force_subtitle);
    check_lifetime(&fixture, 4, 0);
    policy.allow_packed_stock = true;
    begin(&fixture, &resource, 1);
    voice = skyrim_fuz_choose_voice(&policy, original[0], response, false);
    assert(voice.decision == SKYRIM_FUZ_USE_STOCK && voice.stock_path == stock && voice.force_subtitle);
    check_lifetime(&fixture, 5, 1);
    resource.open_status = FIXTURE_OPEN_FAILURE;
    begin(&fixture, &resource, 1);
    voice = skyrim_fuz_choose_voice(&policy, original[0], response, false);
    assert(voice.decision == SKYRIM_FUZ_RESOURCE_FAILURE && !voice.force_subtitle);
    check_lifetime(&fixture, 5, 1);
    resource.open_status = 0;
    policy.allow_packed_stock = false;
    begin(&fixture, &resource, 1);
    policy.stock_paths[3] = packed;
    voice = skyrim_fuz_choose_voice(&policy, original[0], response, false);
    assert(voice.decision == SKYRIM_FUZ_USE_STOCK && voice.stock_path == packed);
    check_lifetime(&fixture, 4, 1);
    policy.stock_paths[3] = stock;

    // The pinned assignment site through the game's own lookup on this thread.
    SkyrimFuzLiftChoice choice;
    const Resource catalog[] = {{.path = original[1]}, {.path = stock}};
    begin(&fixture, catalog, 2);
    assert(!choose_in_game(&policy, &choice)); // An existing FUZ keeps the original path.
    check_lifetime(&fixture, 2, 1);
    begin(&fixture, catalog + 1, 1);
    assert(choose_in_game(&policy, &choice));
    check_lifetime(&fixture, 4, 1);
    begin(&fixture, NULL, 0);
    assert(!choose_in_game(&policy, &choice)); // Missing stock: the normal path.
    check_lifetime(&fixture, 4, 0);
    resource = (Resource){.path = original[2], .open_status = FIXTURE_OPEN_FAILURE};
    begin(&fixture, &resource, 1);
    assert(!choose_in_game(&policy, &choice)); // An unreadable XWM is not replaced.
    check_lifetime(&fixture, 3, 1);
    active = NULL;

    assert(gm_release(&space, NULL));
    puts("Reviewed LE resource lookup selected only missing voices through the stack-owned stream.");
    return 0;
}
