/* SPDX-License-Identifier: GPL-3.0-only */
/* Hosted check of the game resource lookup glue with authored stand-ins for
 * the BSIStream constructor and destructor. The private resource test runs
 * the same provider through the reviewed lifted routines. */
#include "funcs_exe.h"
#include "lift_menu_seam.h"
#include "lift_prelude.h"
#include "lift_resource_lookup.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {
    GUEST_BYTES = 0x20000, STACK_LIMIT = 0x18000, LIVE = 0x1f000,
    FRAME = LIVE + SKYRIM_FUZ_LIFT_FRAME_FROM_SLOT,
    TITLE_A = 0x1f400, TITLE_B = 0x1f440, RESPONSE_A = 0x1f500, RESPONSE_B = 0x1f540, RESPONSE_C = 0x1f580,
    VOICE_A = 0x1f600, VOICE_STOCK = 0x1f700, VOICE_C = 0x1f800, TLS_BASE = 0x7ffde000,
    BSISTREAM_VTABLE = 0x0111eaf8, STREAM = 0x5000, SAVED_EBX = 0x12345678
};

static _Alignas(16) uint8_t guest[GUEST_BYTES];
uint8_t *MEM = guest;

static const char line[] = "The gate remains closed until sunrise";
static const char voiced[] = "Data\\Sound\\Voice\\Fixture.esp\\Narrator\\Voiced.wav";
static const char silent[] = "Data\\Sound\\Voice\\Fixture.esp\\Narrator\\Silent.wav";
static const char stock[] = "Sound/Voice/OwnerSample/Stock_4.xwm";
static const char game_stock[] = "Data\\Sound\\Voice\\OwnerSample\\Stock_4.xwm";

typedef struct {
    const char *path;
    bool unreadable;
} Entry;

static struct {
    const Entry *entries;
    size_t count;
    char probes[8][SKYRIM_FUZ_LIFT_FRAME_BYTES];
    unsigned probe_count, live_destroys;
    uint32_t frame;
    bool check_reentry, unknown_vtable;
} catalog;

static const CPU caller = {.fs_base = TLS_BASE, .r = {[3] = SAVED_EBX}};
static const SkyrimFuzGuestResources resources = {
    .caller = &caller, .live_stack = LIVE,
    .stack_limit = STACK_LIMIT, .stack_base = GUEST_BYTES, .nested_bytes = 0x4000
};

static uint32_t argument(const CPU *cpu, unsigned index) {
    return M32(cpu->r[4] + 4 * (index + 1));
}

/* Model of the reviewed observations: vtable, stream, valid byte, name. */
void f_B08630(CPU *cpu) {
    const uint32_t object = cpu->r[1], name = argument(cpu, 0);
    assert(cpu->fs_base == TLS_BASE && cpu->r[3] == SAVED_EBX &&
           !argument(cpu, 1) && cpu->r[4] + 12 <= object && object < name);
    assert(name + SKYRIM_FUZ_GUEST_LOOKUP_NAME <= LIVE && catalog.probe_count < 8);
    const char *requested = (const char *)MEM + name;
    strcpy(catalog.probes[catalog.probe_count++], requested);
    if (catalog.check_reentry) {
        SkyrimFuzGuestResources reentry = resources;
        const unsigned probes = catalog.probe_count;
        assert(skyrim_fuz_guest_resource_lookup(&reentry, stock) == SKYRIM_FUZ_LOOKUP_ERROR);
        assert(catalog.probe_count == probes && argument(cpu, 0) == name);
    }
    M32(object) = catalog.unknown_vtable ? 0 : BSISTREAM_VTABLE;
    M32(object + 4) = 0;
    M8(object + 8) = 0;
    M32(object + 12) = name;
    for (size_t i = 0; i < catalog.count; ++i) {
        if (strcmp(catalog.entries[i].path, requested)) continue;
        M32(object + 4) = STREAM;
        M8(object + 8) = !catalog.entries[i].unreadable;
    }
    cpu->r[0] = object;
    cpu->r[4] += 12;
}

void f_B08970(CPU *cpu) {
    const uint32_t object = cpu->r[1];
    assert(cpu->fs_base == TLS_BASE && cpu->r[3] == SAVED_EBX &&
           argument(cpu, 0) == 0 && M32(object) == BSISTREAM_VTABLE);
    memset(MEM + object, 0, 16);
    ++catalog.live_destroys;
    cpu->r[0] = object;
    cpu->r[4] += 8;
}

static bool read_guest(void *context, uint32_t address, void *bytes, size_t length) {
    (void)context;
    if (address > GUEST_BYTES || length > GUEST_BYTES - address) return false;
    memcpy(bytes, guest + address, length);
    return true;
}

static bool replace_guest(void *context, uint32_t address, const void *expected, const void *replacement,
                          size_t length) {
    (void)context;
    if (address > GUEST_BYTES || length > GUEST_BYTES - address || memcmp(guest + address, expected, length))
        return false;
    memcpy(guest + address, replacement, length);
    return true;
}

static const SkyrimFuzLiftMemory access = {read_guest, replace_guest, NULL};

static void set_response(uint32_t response, uint32_t title) {
    M32(response) = title;
    M32(response + 4) = (uint32_t)(sizeof line - 1) | ((uint32_t)sizeof line << 16);
    M32(response + 0x10) = 0;
}

/* One pass of the pinned constructor's hook, original assignment and note. */
static bool assign(SkyrimFuzMenuTracker *tracker, const SkyrimFuzPolicy *policy, uint32_t response,
                   const char *path, uint32_t voice) {
    M32(LIVE) = FRAME;
    memset(MEM + FRAME, 0x5b, SKYRIM_FUZ_LIFT_FRAME_BYTES);
    strcpy((char *)MEM + FRAME, path);
    uint8_t above[0x400];
    memcpy(above, MEM + LIVE, sizeof above);
    catalog.probe_count = catalog.live_destroys = 0;

    SkyrimFuzLiftChoice choice;
    const bool changed = skyrim_fuz_choose_with_guest_resources(policy, &access, &resources, response, &choice);
    assert(catalog.live_destroys == catalog.probe_count);
    if (changed) memcpy(above + SKYRIM_FUZ_LIFT_FRAME_FROM_SLOT, game_stock, sizeof game_stock);
    assert(!memcmp(above, MEM + LIVE, sizeof above)); // Only the accepted path changes.
    // The original reference assignment interns the stack path.
    strcpy((char *)MEM + voice, (const char *)MEM + FRAME);
    M32(response + 0x10) = voice;
    skyrim_fuz_menu_assigned(tracker, &access, response, &choice);
    return changed;
}

static void check_probes(const char *const *expected, unsigned count) {
    assert(catalog.probe_count == count);
    for (unsigned i = 0; i < count; ++i) assert(!strcmp(catalog.probes[i], expected[i]));
}

int main(void) {
    SkyrimFuzPolicy policy = {.words_per_second = 2, .skip_empty_responses = true};
    policy.stock_paths[3] = stock;
    strcpy((char *)MEM + TITLE_A, line);
    strcpy((char *)MEM + TITLE_B, line);
    set_response(RESPONSE_A, TITLE_A);
    set_response(RESPONSE_B, TITLE_B);
    set_response(RESPONSE_C, TITLE_A);

    static const char *const voiced_probes[] = {
        "Sound/Voice/Fixture.esp/Narrator/Voiced.wav", "Sound/Voice/Fixture.esp/Narrator/Voiced.fuz",
    };
    static const char *const silent_probes[] = {
        "Sound/Voice/Fixture.esp/Narrator/Silent.wav", "Sound/Voice/Fixture.esp/Narrator/Silent.fuz",
        "Sound/Voice/Fixture.esp/Narrator/Silent.xwm", "Sound/Voice/OwnerSample/Stock_4.xwm",
    };
    const Entry installed[] = {{voiced_probes[1], false}, {stock, false}};
    catalog.entries = installed;
    catalog.count = 2;

    // A voiced and an unvoiced response with identical text. The original
    // FUZ wins without probing stock. Only the unvoiced one is marked.
    SkyrimFuzMenuTracker tracker = {0};
    assert(!assign(&tracker, &policy, RESPONSE_A, voiced, VOICE_A));
    check_probes(voiced_probes, 2);
    assert(assign(&tracker, &policy, RESPONSE_B, silent, VOICE_STOCK));
    check_probes(silent_probes, 4);
    assert(!strcmp((const char *)MEM + VOICE_STOCK, game_stock));
    assert(!skyrim_fuz_substitution_serial(&tracker, &access, RESPONSE_A, VOICE_A, TITLE_A));
    assert(skyrim_fuz_substitution_serial(&tracker, &access, RESPONSE_B, VOICE_STOCK, TITLE_B) == 1);

    // A third, voiced response shares the text and the stock string itself.
    const Entry stock_as_voice[] = {{"Sound/Voice/Fixture.esp/Narrator/Shared.wav", false}, {stock, false}};
    catalog.entries = stock_as_voice;
    assert(!assign(&tracker, &policy, RESPONSE_C, "Data\\Sound\\Voice\\Fixture.esp\\Narrator\\Shared.wav",
                   VOICE_C));
    M32(RESPONSE_C + 0x10) = VOICE_STOCK;
    assert(!skyrim_fuz_substitution_serial(&tracker, &access, RESPONSE_C, VOICE_STOCK, TITLE_A));
    assert(skyrim_fuz_substitution_serial(&tracker, &access, RESPONSE_B, VOICE_STOCK, TITLE_B) == 1);

    // Missing stock or an unreadable stock falls back to the original path.
    catalog.entries = NULL;
    catalog.count = 0;
    assert(!assign(&tracker, &policy, RESPONSE_B, silent, VOICE_STOCK));
    check_probes(silent_probes, 4);
    assert(!skyrim_fuz_substitution_serial(&tracker, &access, RESPONSE_B, VOICE_STOCK, TITLE_B));
    const Entry unreadable_stock[] = {{stock, true}};
    catalog.entries = unreadable_stock;
    catalog.count = 1;
    assert(!assign(&tracker, &policy, RESPONSE_B, silent, VOICE_STOCK));
    check_probes(silent_probes, 4);

    // An unreadable original XWM is not replaced.
    const Entry unreadable_original[] = {{silent_probes[2], true}, {stock, false}};
    catalog.entries = unreadable_original;
    catalog.count = 2;
    assert(!assign(&tracker, &policy, RESPONSE_B, silent, VOICE_STOCK));
    check_probes(silent_probes, 3);
    assert(!tracker.count);

    // A numerically high ESP can still have insufficient owner-local room.
    SkyrimFuzGuestResources low = resources;
    low.stack_limit = LIVE - 0x100;
    catalog.probe_count = 0;
    assert(skyrim_fuz_guest_resource_lookup(&low, stock) == SKYRIM_FUZ_LOOKUP_ERROR && !catalog.probe_count);
    SkyrimFuzGuestResources missing = resources;
    missing.caller = NULL;
    assert(skyrim_fuz_guest_resource_lookup(&missing, stock) == SKYRIM_FUZ_LOOKUP_ERROR && !catalog.probe_count);
    SkyrimFuzGuestResources shallow = resources;
    shallow.nested_bytes = LIVE - STACK_LIMIT;
    assert(skyrim_fuz_guest_resource_lookup(&shallow, stock) == SKYRIM_FUZ_LOOKUP_ERROR && !catalog.probe_count);
    SkyrimFuzGuestResources narrow = resources;
    narrow.stack_base = LIVE + 32;
    SkyrimFuzLiftChoice denied = {.assigned_path = "previous", .force_subtitle = true};
    assert(!skyrim_fuz_choose_with_guest_resources(&policy, &access, &narrow, RESPONSE_B, &denied));
    assert(!catalog.probe_count && !denied.assigned_path[0] && !denied.force_subtitle);

    SkyrimFuzGuestResources valid = resources;
    catalog.check_reentry = true;
    assert(skyrim_fuz_guest_resource_lookup(&valid, stock) == SKYRIM_FUZ_PRESENT && catalog.probe_count == 1);
    catalog.check_reentry = false;
    catalog.unknown_vtable = true;
    const unsigned destroyed = catalog.live_destroys;
    assert(skyrim_fuz_guest_resource_lookup(&valid, stock) == SKYRIM_FUZ_LOOKUP_ERROR);
    assert(catalog.probe_count == 2 && catalog.live_destroys == destroyed);
    catalog.unknown_vtable = false;
    assert(skyrim_fuz_guest_resource_lookup(&valid, stock) == SKYRIM_FUZ_PRESENT);

    puts("Hosted game-lookup glue kept original voices and marked only the substituted response.");
    return 0;
}
