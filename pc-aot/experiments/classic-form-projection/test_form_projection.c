/* SPDX-License-Identifier: GPL-3.0-only */
#if defined(SKYRIM_TEST_PROJECT_FORM)
#define MEM classic_form_projection_memory
#define rt_call_ic classic_form_projection_call_ic
#endif
#include "lift_prelude.h"
#include "form_view.h"
#include "registration_activation.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #cond); exit(1); } \
} while (0)

enum {
    FORM_PAGE = 0x10000,
    ACTOR = FORM_PAGE + 0x100,
    STACK = 0x20000,
    MAP = 0x30000,
    BUCKETS = MAP + 0x100,
    CHAIN = MAP + 0x200,
    END = MAP + 0x300,
    GUEST_BYTES = 0x1300000,
    FORM_TABLE = 0x12e59d4,
    FORM_LOCK = 0x12e59e4,
    CRC_TABLE = 0x110d240,
    THREAD_ID = 0x106b20c,
    COMPARE_EXCHANGE = 0x106b14c,
    DECREMENT = 0x106b154,
    GET_FORM_RET = 0x8f0c1a,
    FIXTURE_RET = 0x400020,
    ACTOR_ID = 0x1234567,
};

typedef struct { uint32_t form_id; uint8_t type; } ProjectedForm;
typedef struct { SkseGameEvent event; SkseVmEpoch epoch; } LoadedWorld;
typedef struct {
    const CPU *cpu;
    uint32_t requested;
    ProjectedForm copy;
} LockedCopy;

uint8_t *MEM;
static size_t page_size;
static unsigned lookups, field_reads;
static bool hide_form_at_unlock, change_world_at_unlock, owner_available;
static LoadedWorld loaded_world;
static _Thread_local LockedCopy *copy_scope;

void f_8F0C10(CPU *);
void f_451A30(CPU *);

static void protect(uint32_t address, int mode) {
    CHECK(!mprotect(MEM + address / page_size * page_size, page_size, mode));
}

/* These are the only host services the extracted lock bodies need when one
 * owner performs the lookup. Acquires/releases are the original lifted calls. */
void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    CHECK(cache && !*cache);
    const uint32_t stack = cpu->r[4];
    const uint32_t ret = M32(stack);
    switch (target) {
    case THREAD_ID:
        CHECK(ret == 0xa4aee9 && !M32(FORM_LOCK));
        ++lookups;
        cpu->r[0] = 1;
        cpu->r[4] += 4;
        break;
    case COMPARE_EXCHANGE:
        CHECK(ret == 0xa4af0f && M32(stack + 4) == FORM_LOCK);
        CHECK(M32(stack + 8) == 1 && M32(stack + 12) == 0);
        CHECK(!M32(FORM_LOCK));
        M32(FORM_LOCK) = 1;
        cpu->r[0] = 0;
        cpu->r[4] += 16;
        break;
    case DECREMENT:
        CHECK(ret == 0xa4b0f7 && M32(stack + 4) == FORM_LOCK);
        CHECK(M32(FORM_LOCK) == 1);
        M32(FORM_LOCK) = 0;
        if (hide_form_at_unlock) protect(FORM_PAGE, PROT_NONE);
        if (change_world_at_unlock) ++loaded_world.epoch.bytes[0];
        cpu->r[0] = 0;
        cpu->r[4] += 8;
        break;
    default:
        CHECK(false);
    }
}

bool classic_engine_read_data(const CPU *cpu, uint32_t address, uint8_t *bytes, size_t count) {
    CHECK(copy_scope && cpu == copy_scope->cpu && M32(FORM_LOCK) == 1);
    ++field_reads;
    if (address != ACTOR + 0x0c || count != 7) return false;
    memcpy(bytes, MEM + address, count);
    return true;
}

/* The argument includes a borrowed guest address. Only the trusted host
 * reads it under the original registry lock. The staged result has no address. */
void classic_test_copy_actor(const CPU *cpu, uint32_t caller, uint32_t id, uint32_t address) {
    if (!copy_scope || copy_scope->cpu != cpu || caller != GET_FORM_RET ||
        copy_scope->requested != id || copy_scope->copy.form_id) return;
    uint32_t actual;
    if (classic_read_form_id(cpu, address, CLASSIC_CHARACTER_FORM, &actual) && actual == id)
        copy_scope->copy = (ProjectedForm){actual, CLASSIC_CHARACTER_FORM};
}

static bool current_world(const LoadedWorld *candidate) {
    CHECK(!M32(FORM_LOCK));
    const SkseGameEvent *expected = &candidate->event, *current = &loaded_world.event;
    return owner_available && expected->ticket == current->ticket &&
           expected->scope.text == current->scope.text && expected->scope.voices == current->scope.voices &&
           !memcmp(expected->scope.launch, current->scope.launch, sizeof current->scope.launch) &&
           !memcmp(expected->scope.profile, current->scope.profile, sizeof current->scope.profile) &&
           !memcmp(&expected->saved, &current->saved, sizeof current->saved) &&
           !memcmp(&candidate->epoch, &loaded_world.epoch, sizeof loaded_world.epoch);
}

static bool project_with_trace(const LoadedWorld *request, const SkseEventTrace *trace,
                               uint32_t id, bool get_form, uint32_t expected_engine_result, ProjectedForm *out) {
    if (!id || copy_scope || !current_world(request) ||
        skse_bridge_check_call(&request->event, SkyrimLoadSlot, trace) != SKSE_BRIDGE_OK) return false;

    const uint32_t stack = STACK + 0x800;
    M32(stack) = FIXTURE_RET;
    M32(stack + (get_form ? 16 : 4)) = id;
    CPU cpu = {0};
    cpu.r[4] = stack;
    cpu.r[6] = 0xabc8;
    cpu.r[7] = 0xabc9;
    LockedCopy scope = {.cpu = &cpu, .requested = id};
    copy_scope = &scope;
    if (get_form) f_8F0C10(&cpu);
    else f_451A30(&cpu);
    copy_scope = NULL;
    CHECK(cpu.r[0] == expected_engine_result && cpu.r[4] == stack + 4 && M32(stack) == FIXTURE_RET);
    CHECK(cpu.r[6] == 0xabc8 && cpu.r[7] == 0xabc9 && !M32(FORM_LOCK));
    if (!scope.copy.form_id || !current_world(request)) return false;
    *out = scope.copy;
    return true;
}

#if !defined(SKYRIM_TEST_PROJECT_FORM)
static bool project(const LoadedWorld *request, uint32_t id, bool get_form,
                    uint32_t expected_engine_result, ProjectedForm *out) {
    SkyrimLifecycleRow rows[SkyrimLifecycleCapacity];
    SkseEventTrace trace = {.rows = rows};
    trace.count = skyrim_lifecycle_snapshot(rows, SkyrimLifecycleCapacity, &trace.dropped);
    memcpy(trace.launch, loaded_world.event.scope.launch, sizeof trace.launch);
    return project_with_trace(request, &trace, id, get_form, expected_engine_result, out);
}

/* The load acceptance and epoch for these authored loads come from the fixture owner.
 * The existing bridge trace validator independently confirms their ordering. */
static LoadedWorld record_load(bool revert, bool successful, bool base_verified) {
    SkyrimLifecycleCall slot = skyrim_lifecycle_begin(SkyrimLoadSlot);
    if (revert) {
        SkyrimLifecycleCall reset = skyrim_lifecycle_begin(SkyrimRevertForLoad);
        skyrim_lifecycle_end(reset, SkyrimNoResult);
    }
    SkyrimLifecycleCall load = skyrim_lifecycle_begin(SkyrimLoadVM);
    skyrim_lifecycle_end(load, successful ? SkyrimReturnedTrue : SkyrimReturnedFalse);
    skyrim_lifecycle_end(slot, SkyrimReturnedTrue);
    loaded_world = (LoadedWorld){
        .event = {.scope = {{0x11}, {0x22}, 1, 1},
                  .saved = {{0x22}, {0x33}, {0x44}}, .ticket = slot.ticket},
        .epoch = {{(uint8_t)slot.ticket}},
    };
    owner_available = base_verified;
    return loaded_world;
}
#endif

static uint32_t form_bucket(uint32_t id) {
    uint32_t hash = 0;
    for (unsigned i = 0; i < 4; ++i, id >>= 8)
        hash = (hash >> 8) ^ M32(CRC_TABLE + ((id ^ hash) & 255u) * 4);
    return BUCKETS + (hash & 3u) * 12;
}

static void map_form(uint32_t id, uint32_t address) {
    const uint32_t bucket = form_bucket(id);
    uint32_t other = id + 1;
    while (form_bucket(other) != bucket) ++other;
    memset(MEM + BUCKETS, 0, 4 * 12);
    M32(bucket) = other;
    M32(bucket + 8) = CHAIN;
    M32(CHAIN) = id;
    M32(CHAIN + 4) = address;
    M32(CHAIN + 8) = END;
}

static void form_fields(uint32_t id, uint8_t type) {
    protect(FORM_PAGE, PROT_READ | PROT_WRITE);
    M32(ACTOR + 0x0c) = id;
    M8(ACTOR + 0x12) = type;
    protect(FORM_PAGE, PROT_READ);
}

static void set_up(void) {
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    CHECK(page_size >= 4096 && GUEST_BYTES % page_size == 0 && FORM_PAGE % page_size == 0);
    MEM = mmap(NULL, GUEST_BYTES, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(MEM != MAP_FAILED);
    protect(STACK, PROT_READ | PROT_WRITE);
    protect(MAP, PROT_READ | PROT_WRITE);
    protect(FORM_LOCK, PROT_READ | PROT_WRITE);
    protect(CRC_TABLE, PROT_READ | PROT_WRITE);
    protect(THREAD_ID, PROT_READ | PROT_WRITE);

    M32(THREAD_ID) = THREAD_ID;
    M32(COMPARE_EXCHANGE) = COMPARE_EXCHANGE;
    M32(DECREMENT) = DECREMENT;
    protect(THREAD_ID, PROT_READ);
    M32(FORM_TABLE) = MAP;
    M32(MAP + 8) = 4;
    M32(MAP + 0x14) = END;
    M32(MAP + 0x1c) = BUCKETS;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t value = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1u) ? UINT32_C(0xedb88320) : 0);
        M32(CRC_TABLE + i * 4) = value;
    }
    protect(CRC_TABLE, PROT_READ);
    map_form(ACTOR_ID, ACTOR);
    form_fields(ACTOR_ID, CLASSIC_CHARACTER_FORM);
}

#if !defined(SKYRIM_TEST_PROJECT_FORM)
int main(void) {
    set_up();
    LoadedWorld first = record_load(true, true, true);
    ProjectedForm out = {0};
    hide_form_at_unlock = true;
    CHECK(project(&first, ACTOR_ID, true, ACTOR, &out));
    CHECK(out.form_id == ACTOR_ID && out.type == CLASSIC_CHARACTER_FORM);
    CHECK(lookups == 1 && field_reads == 1);
    protect(FORM_PAGE, PROT_READ);
    hide_form_at_unlock = false;

    out = (ProjectedForm){0xffffffffu, 0xff};
    CHECK(!project(&first, ACTOR_ID, false, ACTOR, &out));
    CHECK(lookups == 2 && field_reads == 1);
    form_fields(ACTOR_ID, CLASSIC_REFERENCE_FORM);
    CHECK(!project(&first, ACTOR_ID, true, ACTOR, &out));
    form_fields(ACTOR_ID + 1, CLASSIC_CHARACTER_FORM);
    CHECK(!project(&first, ACTOR_ID, true, ACTOR, &out));
    form_fields(ACTOR_ID, CLASSIC_CHARACTER_FORM);
    map_form(ACTOR_ID, ACTOR + 1);
    CHECK(!project(&first, ACTOR_ID, true, ACTOR + 1, &out));
    map_form(ACTOR_ID, 0x50000);
    CHECK(!project(&first, ACTOR_ID, true, 0x50000, &out));
    map_form(ACTOR_ID + 32, ACTOR);
    CHECK(!project(&first, ACTOR_ID, true, 0, &out));
    CHECK(lookups == 7 && field_reads == 4);
    map_form(ACTOR_ID, ACTOR);

    LoadedWorld wrong = first;
    wrong.event.saved.event[0] ^= 1;
    CHECK(!project(&wrong, ACTOR_ID, true, 0, &out));
    wrong = first;
    wrong.epoch.bytes[0] ^= 1;
    CHECK(!project(&wrong, ACTOR_ID, true, 0, &out));
    LoadedWorld no_revert = record_load(false, true, true);
    CHECK(!project(&no_revert, ACTOR_ID, true, 0, &out));
    LoadedWorld vm_failed = record_load(true, false, true);
    CHECK(!project(&vm_failed, ACTOR_ID, true, 0, &out));
    LoadedWorld no_base = record_load(true, true, false);
    CHECK(!project(&no_base, ACTOR_ID, true, 0, &out));

    LoadedWorld newer = record_load(true, true, true);
    CHECK(!project(&first, ACTOR_ID, true, 0, &out));
    CHECK(lookups == 7 && field_reads == 4);
    change_world_at_unlock = true;
    CHECK(!project(&newer, ACTOR_ID, true, ACTOR, &out));
    CHECK(lookups == 8 && field_reads == 5);
    CHECK(out.form_id == 0xffffffffu && out.type == 0xff);
    CHECK(!munmap(MEM, GUEST_BYTES));
    puts("PASS stock Game.GetForm lookup, copied actor, and same-load exclusions");
    return 0;
}
#else
void classic_test_managed_after_projection(uint32_t, uint8_t, unsigned);

/* Called only from the hosted roundtrip after the engine's scoped immutable
 * input and the persisted companion have passed the existing bridge. */
void classic_test_projection_after_load(const SkseGameEvent *event, const SkseEventTrace *trace,
                                        SkseBridgeResult result) {
    set_up();
    const LoadedWorld request = {.event = *event, .epoch = {{0x52}}};
    owner_available = result == SKSE_BRIDGE_OK;
    loaded_world = request;
    ProjectedForm out = {0};
    CHECK(project_with_trace(&request, trace, ACTOR_ID, true, ACTOR, &out) == owner_available);
    if (owner_available) {
        CHECK(out.form_id == ACTOR_ID && out.type == CLASSIC_CHARACTER_FORM);
        CHECK(lookups == 1 && field_reads == 1);
        classic_test_managed_after_projection(out.form_id, out.type, request.epoch.bytes[0]);
        ++loaded_world.epoch.bytes[0];
        CHECK(!project_with_trace(&request, trace, ACTOR_ID, true, ACTOR, &out));
        CHECK(lookups == 1 && field_reads == 1);
        puts("PASS copied actor after exact companion generation");
    } else {
        CHECK(!out.form_id && !lookups && !field_reads);
        puts("PASS actor lookup suppressed without accepted companion generation");
    }
    CHECK(!munmap(MEM, GUEST_BYTES));
}
#endif
