/* SPDX-License-Identifier: GPL-3.0-only */
#include "guest_memory.h"
#include "voice_fallback.h"
#include "lift_prelude.h"
#include "funcs_exe.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {
    OBJECT = 0x2100, STREAM = 0x2140, STREAM_VTABLE = 0x2200, PATH = 0x2600, CALLER_OFFSET = 0x2800,
    RESOLVE_OUTPUT = 0x2804,
    DEFAULT_ROOT = 0x2900, STACK = 0x2fe0, PATH_CAPACITY = 260,
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
    const char *requested;
    unsigned probes, opens, closes, disposals, frees, names;
    const Resource *resolved;
    uint32_t lookup_status;
    bool owns_object;
} Fixture;

uint8_t *MEM;
static Fixture *active;

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

void f_A487B0(CPU *cpu) {
    assert(cpu->r[1] == HEAP && argument(cpu, 0) == OBJECT && !argument(cpu, 1));
    assert(active->owns_object && !active->names);
    active->owns_object = false;
    ++active->frees;
    return_guest(cpu, 2, 0);
}

static void resolve_from_catalog(CPU *cpu) {
    uint32_t wrapper = cpu->r[1], output = argument(cpu, 1);
    assert(M32(wrapper) == ROOT_VTABLE && M32(wrapper + 8) == 0 && M32(wrapper + 12) == DEFAULT_ROOT);
    uint32_t auxiliary = argument(cpu, 2);
    assert(argument(cpu, 0) == PATH && auxiliary == wrapper + 0x1c &&
           !M32(auxiliary) && !argument(cpu, 3));
    assert(!M32(output) && !strcmp((const char *)MEM + PATH, active->requested));
    ++active->probes;
    for (size_t i = 0; i < active->count; ++i) {
        const Resource *resource = &active->entries[i];
        if (!strcmp(resource->path, active->requested)) {
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
 * Every address supplied here refers to an authored, owned guest range. */
static SkyrimFuzAvailability lookup_lifted(void *context, const char *relative) {
    Fixture *fixture = context;
    assert(!active && !fixture->owns_object && !fixture->names);
    const size_t length = strlen(relative);
    if (length >= PATH_CAPACITY) return SKYRIM_FUZ_LOOKUP_ERROR;
    memcpy(MEM + PATH, relative, length + 1);
    fixture->requested = relative;
    fixture->resolved = NULL;
    fixture->lookup_status = FIXTURE_NOT_FOUND;
    fixture->owns_object = true;
    active = fixture;

    CPU cpu = {0};
    cpu.r[1] = OBJECT;
    cpu.r[4] = STACK;
    M32(STACK) = 0;
    M32(STACK + 4) = PATH;
    M32(STACK + 8) = 0;
    f_B08630(&cpu);
    assert(cpu.r[0] == OBJECT && cpu.r[4] == STACK + 12);
    assert(M32(OBJECT) == INPUT_STREAM_VTABLE && M32(OBJECT + 12) == PATH && fixture->names == 1);
    const bool opened = fixture->resolved && !fixture->lookup_status;
    const bool valid = opened && !fixture->resolved->open_status;
    assert(M8(OBJECT + 8) == valid && M32(OBJECT + 4) == (opened ? STREAM : 0));

    cpu.r[1] = OBJECT;
    cpu.r[4] = STACK;
    M32(STACK + 4) = 1;
    f_B08970(&cpu); // The verified first slot of INPUT_STREAM_VTABLE.
    assert(cpu.r[0] == OBJECT && cpu.r[4] == STACK + 8);
    assert(!fixture->owns_object && !fixture->names && M32(CALLER_OFFSET) == FIXTURE_CURSOR);
    fixture->requested = NULL;
    active = NULL;
    if (valid) return SKYRIM_FUZ_PRESENT;
    // Real BSIStream loses the reason. Only this known fixture miss is ABSENT.
    return fixture->resolved ? SKYRIM_FUZ_LOOKUP_ERROR : SKYRIM_FUZ_ABSENT;
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
    assert(fixture->probes == probes && fixture->frees == probes);
    assert(fixture->opens == streams && fixture->closes == streams && fixture->disposals == streams);
}

static uint32_t probe_resolver_failure(Fixture *fixture, const char *path) {
    const size_t length = strlen(path);
    assert(length < PATH_CAPACITY && !active);
    memcpy(MEM + PATH, path, length + 1);
    fixture->requested = path;
    fixture->lookup_status = FIXTURE_NOT_FOUND;
    active = fixture;
    CPU cpu = {0};
    cpu.r[4] = STACK;
    M32(STACK) = 0;
    M32(STACK + 4) = PATH;
    M32(STACK + 8) = RESOLVE_OUTPUT;
    M32(STACK + 12) = M32(STACK + 16) = M32(RESOLVE_OUTPUT) = 0;
    f_A3F810(&cpu);
    assert(cpu.r[4] == STACK + 4 && !M32(RESOLVE_OUTPUT) && !fixture->opens && !fixture->frees);
    fixture->requested = NULL;
    active = NULL;
    return cpu.r[0];
}

int main(void) {
    gm_space memory = {0};
    assert(gm_reserve(&memory, NULL));
    const uint32_t addresses[] = {OBJECT, COMPARE_EXCHANGE_SLOT, ROOT_VTABLE, DEFAULT_ROOT_SLOT};
    for (size_t i = 0; i < sizeof addresses / sizeof addresses[0]; ++i)
        assert(gm_commit_page(&memory, (addresses[i] / memory.page_size) * memory.page_size, NULL));
    MEM = memory.base;
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
        fixture = (Fixture){.entries = &resource, .count = 1};
        SkyrimFuzVoice voice = skyrim_fuz_choose_voice(&policy, original[0], response, false);
        assert(voice.decision == SKYRIM_FUZ_KEEP_VOICE && !voice.stock_path && !voice.force_subtitle);
        check_lifetime(&fixture, i + 1, 1);
    }

    resource = (Resource){.path = original[0], .lookup_status = FIXTURE_RESOLVER_FAILURE};
    fixture = (Fixture){.entries = &resource, .count = 1};
    assert(probe_resolver_failure(&fixture, original[0]) == FIXTURE_RESOLVER_FAILURE);
    fixture = (Fixture){0};
    assert(probe_resolver_failure(&fixture, original[0]) == FIXTURE_NOT_FOUND);
    fixture = (Fixture){.entries = &resource, .count = 1};
    assert(lookup_lifted(&fixture, original[0]) == SKYRIM_FUZ_LOOKUP_ERROR);
    assert(fixture.lookup_status == FIXTURE_RESOLVER_FAILURE);
    check_lifetime(&fixture, 1, 0);

    resource = (Resource){.path = original[0], .open_status = FIXTURE_OPEN_FAILURE};
    fixture = (Fixture){.entries = &resource, .count = 1};
    assert(lookup_lifted(&fixture, original[0]) == SKYRIM_FUZ_LOOKUP_ERROR);
    check_lifetime(&fixture, 1, 1);

    resource = (Resource){.path = stock};
    fixture = (Fixture){.entries = &resource, .count = 1};
    SkyrimFuzVoice voice = skyrim_fuz_choose_voice(&policy, original[0], response, false);
    assert(voice.decision == SKYRIM_FUZ_USE_STOCK && voice.stock_path == stock && voice.seconds == 4);
    check_lifetime(&fixture, 4, 1);

    resource = (Resource){.path = packed};
    fixture = (Fixture){.entries = &resource, .count = 1};
    assert(lookup_lifted(&fixture, packed) == SKYRIM_FUZ_PRESENT);
    assert(lookup_lifted(&fixture, stock) == SKYRIM_FUZ_ABSENT);
    assert(fixture.lookup_status == FIXTURE_NOT_FOUND);
    check_lifetime(&fixture, 2, 1);
    fixture = (Fixture){.entries = &resource, .count = 1};
    voice = skyrim_fuz_choose_voice(&policy, original[0], response, false);
    assert(voice.decision == SKYRIM_FUZ_STOCK_UNAVAILABLE && !voice.force_subtitle);
    check_lifetime(&fixture, 4, 0);
    fixture = (Fixture){.entries = &resource, .count = 1};
    policy.stock_paths[3] = packed;
    voice = skyrim_fuz_choose_voice(&policy, original[0], response, false);
    assert(voice.decision == SKYRIM_FUZ_USE_STOCK && voice.stock_path == packed);
    check_lifetime(&fixture, 4, 1);

    assert(gm_release(&memory, NULL));
    puts("Reviewed LE resource stream construction and disposal passed with an authored catalog.");
    return 0;
}
