/* SPDX-License-Identifier: GPL-3.0-only */
#include "registration_activation.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    SkseRegistrationFamily family;
    SkseRegistrationKey key;
    SkseRegistrationName callback;
    uint64_t handle, key_token;
    uint32_t ordinal;
} Candidate;

struct SkseVmRegistrations {
    SkseVmRegistrationPolicy policy;
    SkseVmEpoch epoch;
    Candidate *rows;
    uint8_t *names;
    size_t held;
    int busy, active;
};

static int valid_epoch(const SkseVmEpoch *epoch) {
    static const SkseVmEpoch zero = {{0}};
    return epoch && memcmp(epoch->bytes, zero.bytes, sizeof zero.bytes);
}

static SkseVmRegistrationResult bind_load(SkseVmRegistrations *s, const SkseGameEvent *requested) {
    SkseGameEvent loaded = {0};
    SkseVmEpoch epoch = {{0}};
    if (!s->policy.loaded(s->policy.context, &epoch, &loaded) || !valid_epoch(&epoch)) return SKSE_VM_REG_UNAVAILABLE;
    if (loaded.ticket != requested->ticket || memcmp(&loaded.saved, &requested->saved, sizeof loaded.saved) ||
        memcmp(loaded.scope.launch, requested->scope.launch, sizeof loaded.scope.launch) ||
        memcmp(loaded.scope.profile, requested->scope.profile, sizeof loaded.scope.profile) ||
        loaded.scope.text != requested->scope.text || loaded.scope.voices != requested->scope.voices)
        return SKSE_VM_REG_WRONG_LOAD;
    s->epoch = epoch;
    return SKSE_VM_REG_OK;
}

static SkseVmRegistrationResult current(const SkseVmRegistrations *s) {
    SkseVmWorld world = s->policy.world(s->policy.context, &s->epoch);
    if (world == SKSE_VM_CURRENT) return SKSE_VM_REG_OK;
    return world == SKSE_VM_RETIRED ? SKSE_VM_REG_RETIRED : SKSE_VM_REG_UNAVAILABLE;
}

static void forget(SkseVmRegistrations *s) {
    free(s->rows);
    free(s->names);
    s->rows = NULL;
    s->names = NULL;
    s->held = 0;
    s->active = 0;
    s->epoch = (SkseVmEpoch){{0}};
}

static SkseVmRegistrationResult clear_owned(SkseVmRegistrations *s) {
    s->active = 0;
    while (s->held) {
        SkseVmRegistrationResult state = current(s);
        if (state == SKSE_VM_REG_RETIRED) break;
        if (state != SKSE_VM_REG_OK) return state;
        s->policy.release(s->policy.context, &s->epoch, s->rows[--s->held].handle);
    }
    forget(s);
    return SKSE_VM_REG_OK;
}

SkseVmRegistrationResult skse_vm_registrations_create(const SkseVmRegistrationPolicy *policy, SkseVmRegistrations **out) {
    if (!out) return SKSE_VM_REG_ARGUMENT;
    *out = NULL;
    if (!policy || !policy->loaded || !policy->world || !policy->name_token || !policy->retain || !policy->release)
        return SKSE_VM_REG_ARGUMENT;
    SkseVmRegistrations *state = calloc(1, sizeof *state);
    if (!state) return SKSE_VM_REG_MEMORY;
    state->policy = *policy;
    *out = state;
    return SKSE_VM_REG_OK;
}

SkseVmRegistrationResult skse_vm_registrations_clear(SkseVmRegistrations *state) {
    if (!state) return SKSE_VM_REG_ARGUMENT;
    if (state->busy) return SKSE_VM_REG_BUSY;
    state->busy = 1;
    SkseVmRegistrationResult result = clear_owned(state);
    state->busy = 0;
    return result;
}

SkseVmRegistrationResult skse_vm_registrations_close(SkseVmRegistrations **state) {
    if (!state) return SKSE_VM_REG_ARGUMENT;
    if (!*state) return SKSE_VM_REG_OK;
    SkseVmRegistrationResult result = skse_vm_registrations_clear(*state);
    if (result == SKSE_VM_REG_OK) {
        free(*state);
        *state = NULL;
    }
    return result;
}

typedef struct {
    Candidate *rows;
    size_t count;
    SkseRegistrationFamily family;
} Collection;
static void collect_row(void *ctx, SkseRegistrationKey key, SkseRegistrationName callback, uint64_t saved,
                        SkseModResolution resolution, const uint64_t *mapped) {
    (void)saved;
    (void)resolution;
    Collection *c = ctx;
    if (mapped) {
        c->rows[c->count] = (Candidate){.family = c->family, .key = key, .callback = callback,
                                       .handle = *mapped, .ordinal = (uint32_t)c->count};
        ++c->count;
    }
}

static SkseVmRegistrationResult read_candidates(SkseVmRegistrations *s, const SkseCoreSnapshot *source,
                                                size_t *count, SkseVmRegistrationReport *report) {
    /* NINU as emitted by the stock writer is deliberately absent here. The
     * reader recognizes NiNU. Both tags stay visible in the inspection API. */
    static const SkseRegistrationFamily families[] = {
        SKSE_REG_MENU_EVENTS, SKSE_REG_INPUT_KEYS, SKSE_REG_INPUT_CONTROLS, SKSE_REG_MOD_CALLBACKS,
        SKSE_REG_CROSSHAIR, SKSE_REG_CAMERA, SKSE_REG_ACTOR_ACTIONS, SKSE_REG_NODE_READER
    };
    enum { FAMILIES = sizeof families / sizeof families[0] };
    uint32_t present = 0;
    for (unsigned i = 0; i < FAMILIES; ++i) {
        SkseRegistrationSummary sum;
        SkseRegistrationResult format = skse_registration_view_read(source, families[i], &sum, NULL, NULL);
        if (format == SKSE_REG_MISSING) continue;
        if (format != SKSE_REG_OK) { report->format = format; return SKSE_VM_REG_FORMAT; }
        if (sum.handles > SKSE_REG_MAX_HANDLES - report->rows) return SKSE_VM_REG_LIMIT;
        present |= 1u << i;
        report->rows += sum.handles;
        report->missing_mod += sum.handles - sum.resolutions[SKSE_MOD_RESOLVED] - sum.resolutions[SKSE_MOD_DYNAMIC];
    }
    size_t available = report->rows - report->missing_mod;
    if (!available) { *count = 0; return SKSE_VM_REG_OK; }
    s->rows = calloc(available, sizeof *s->rows);
    if (!s->rows) return SKSE_VM_REG_MEMORY;
    Collection collected = {.rows = s->rows};
    for (unsigned i = 0; i < FAMILIES; ++i) {
        if (!(present & (1u << i))) continue;
        collected.family = families[i];
        SkseRegistrationSummary sum;
        SkseRegistrationResult format = skse_registration_view_read(source, families[i], &sum, collect_row, &collected);
        if (format != SKSE_REG_OK) { report->format = format; return SKSE_VM_REG_FORMAT; }
    }
    *count = collected.count;
    return SKSE_VM_REG_OK;
}

static int compare_key(const Candidate *a, const Candidate *b) {
    if (a->family != b->family) return a->family < b->family ? -1 : 1;
    if (a->key_token != b->key_token) return a->key_token < b->key_token ? -1 : 1;
    return (a->handle > b->handle) - (a->handle < b->handle);
}
static int compare_order(const void *a, const void *b) {
    const Candidate *left = a, *right = b;
    return (left->ordinal > right->ordinal) - (left->ordinal < right->ordinal);
}
static int sort_registrations(const void *a, const void *b) {
    int result = compare_key(a, b);
    return result ? result : compare_order(a, b);
}

static SkseVmRegistrationResult unique_rows(SkseVmRegistrations *s, size_t *count, SkseVmRegistrationReport *report) {
    for (size_t i = 0; i < *count; ++i) {
        Candidate *c = &s->rows[i];
        if (c->key.type == SKSE_REG_NUMERIC_KEY) c->key_token = c->key.number;
        if (c->key.type != SKSE_REG_NAMED_KEY) continue;
        SkseVmRegistrationResult result = current(s);
        if (result != SKSE_VM_REG_OK) return result;
        if (!s->policy.name_token(s->policy.context, &s->epoch, c->key.name, &c->key_token)) return SKSE_VM_REG_POLICY;
    }
    if (*count < 2) return SKSE_VM_REG_OK;
    qsort(s->rows, *count, sizeof *s->rows, sort_registrations);
    size_t unique = 1;
    for (size_t i = 1; i < *count; ++i) {
        if (compare_key(&s->rows[unique - 1], &s->rows[i]) == 0) ++report->duplicates;
        else s->rows[unique++] = s->rows[i];
    }
    qsort(s->rows, unique, sizeof *s->rows, compare_order);
    *count = unique;
    return SKSE_VM_REG_OK;
}

static SkseVmRegistrationResult acquire(SkseVmRegistrations *s, size_t count, SkseVmRegistrationReport *report) {
    for (size_t i = 0; i < count; ++i) {
        SkseVmRegistrationResult result = current(s);
        if (result != SKSE_VM_REG_OK) return result;
        Candidate row = s->rows[i];
        SkseVmRetain retained = s->policy.retain(s->policy.context, &s->epoch, row.handle);
        if (retained == SKSE_VM_HANDLE_ABSENT) ++report->missing_handle;
        else if (retained == SKSE_VM_RETAINED) s->rows[s->held++] = row;
        else return SKSE_VM_REG_POLICY;
    }
    return current(s);
}

#if defined(SKSE_REGISTRATION_TEST_ALLOC)
static _Thread_local int fail_name_allocation;
void skse_vm_test_fail_name_allocation(void) { fail_name_allocation = 1; }
#endif

/* The shared name of a keyed REGS group is a single slice in the Classic
 * image. unique_rows restores the input order, so its handles remain adjacent.
 * Keep only these bounded names when dropping the entire companion image. */
static SkseVmRegistrationResult copy_names(SkseVmRegistrations *s, size_t count, size_t *bytes) {
    size_t size = 0;
    const uint8_t *previous = NULL;
    for (size_t i = 0; i < count; ++i) {
        const Candidate *c = &s->rows[i];
        if (c->key.name.data != previous) size += c->key.name.size;
        previous = c->key.name.data;
        size += c->callback.size;
    }
    *bytes = size;
    if (!size) return SKSE_VM_REG_OK;
#if defined(SKSE_REGISTRATION_TEST_ALLOC)
    if (fail_name_allocation) { fail_name_allocation = 0; return SKSE_VM_REG_MEMORY; }
#endif
    s->names = malloc(size);
    if (!s->names) return SKSE_VM_REG_MEMORY;
    uint8_t *next = s->names, *key = NULL;
    previous = NULL;
    for (size_t i = 0; i < count; ++i) {
        Candidate *c = &s->rows[i];
        if (c->key.name.size) {
            if (c->key.name.data != previous) {
                key = next;
                memcpy(next, c->key.name.data, c->key.name.size);
                next += c->key.name.size;
            }
            previous = c->key.name.data;
            c->key.name.data = key;
        } else previous = NULL;
        if (c->callback.size) {
            memcpy(next, c->callback.data, c->callback.size);
            c->callback.data = next;
            next += c->callback.size;
        }
    }
    return SKSE_VM_REG_OK;
}

static size_t compact_live_rows(SkseVmRegistrations *s, size_t capacity) {
    if (!s->held) { free(s->rows); s->rows = NULL; return 0; }
    if (s->held <= capacity / 2) {
        Candidate *smaller = realloc(s->rows, s->held * sizeof *s->rows);
        if (smaller) { s->rows = smaller; return s->held; }
    }
    return capacity;
}

SkseVmRegistrationResult skse_bridge_load_registrations(SkseEventBridge *bridge, const SkseGameEvent *requested,
    const SkseEventTrace *trace, const SkseBaseEvidence *base, const SkseLoadedMod *current_mods, size_t count,
    SkseVmRegistrations *s, SkseVmRegistrationReport *out) {
    SkseVmRegistrationReport scratch;
    SkseVmRegistrationReport *r = out ? out : &scratch;
    *r = (SkseVmRegistrationReport){.map = SKSE_MAP_SOURCE_ERROR};
    if (!s) return SKSE_VM_REG_ARGUMENT;
    if (s->busy) return SKSE_VM_REG_BUSY;
    if (s->active || s->held) return SKSE_VM_REG_OCCUPIED;
    s->busy = 1;
    SkseCoreSnapshot *source = NULL;
    r->bridge = skse_bridge_open_loaded_core(bridge, requested, trace, base, current_mods, count,
                                              &source, &r->map, &r->pair);
    SkseVmRegistrationResult result;
    if (r->bridge != SKSE_BRIDGE_OK) {
        result = r->bridge == SKSE_BRIDGE_CORE ? SKSE_VM_REG_CORE : SKSE_VM_REG_BRIDGE;
    } else {
        size_t n = 0;
        result = read_candidates(s, source, &n, r);
        if (result == SKSE_VM_REG_OK) result = bind_load(s, requested);
        if (result == SKSE_VM_REG_OK) result = current(s);
        if (result == SKSE_VM_REG_OK) result = unique_rows(s, &n, r);
        if (result == SKSE_VM_REG_OK) result = acquire(s, n, r);
        size_t names = 0;
        if (result == SKSE_VM_REG_OK) result = copy_names(s, s->held, &names);
        skse_core_snapshot_close(source);
        if (result == SKSE_VM_REG_OK) {
            s->active = 1;
            r->retained = (uint32_t)s->held;
            size_t capacity = compact_live_rows(s, r->rows - r->missing_mod);
            r->retained_bytes = (uint32_t)(sizeof *s + capacity * sizeof *s->rows + names);
        } else {
            SkseVmRegistrationResult cleared = clear_owned(s);
            if (cleared != SKSE_VM_REG_OK) result = cleared;
        }
    }
    s->busy = 0;
    return result;
}

SkseVmRegistrationResult skse_vm_registrations_visit(SkseVmRegistrations *s, SkseActiveRegistration observe, void *ctx) {
    if (!s || !observe) return SKSE_VM_REG_ARGUMENT;
    if (s->busy) return SKSE_VM_REG_BUSY;
    if (!s->active) return SKSE_VM_REG_INACTIVE;
    s->busy = 1;
    SkseVmRegistrationResult result = current(s);
    if (result == SKSE_VM_REG_OK) {
        for (size_t i = 0; i < s->held; ++i) {
            const Candidate *c = &s->rows[i];
            observe(ctx, c->family, c->key, c->callback, c->handle);
        }
    }
    s->busy = 0;
    return result;
}
