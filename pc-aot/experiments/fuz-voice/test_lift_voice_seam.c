/* SPDX-License-Identifier: GPL-3.0-only */
#include "lift_voice_seam.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {
    SLOT = 64, FRAME = SLOT + SKYRIM_FUZ_LIFT_FRAME_FROM_SLOT, RESPONSE = 512, TEXT = 768,
    MEMORY_BYTES = 1024
};
#define RETURN_ADDRESS UINT32_C(0x67326d)
static const char original[] = "Data\\Sound\\Voice\\Fixture.esp\\Narrator\\Reply.wav";
static const char stock_path[] = "Sound/Voice/OwnerSample/FourSeconds.xwm";
static const char stock_game_path[] = "Data\\Sound\\Voice\\OwnerSample\\FourSeconds.xwm";

typedef union {
    uint8_t bytes[MEMORY_BYTES];
    uint32_t words[MEMORY_BYTES / 4];
} TestMemory;

typedef struct {
    TestMemory guest;
    const SkyrimFuzPolicy *policy;
    unsigned reads, text_reads, replacements, probes, hook_calls, original_calls;
    bool deny_write, deny_text, mutate_frame, mutate_text_header;
    SkyrimFuzAvailability wav, stock;
    const char *configured_stock;
    bool changed;
    char assigned[SKYRIM_FUZ_LIFT_FRAME_BYTES];
} Fixture;

static Fixture *active;

static bool in_guest(uint32_t address, size_t length) {
    return address < MEMORY_BYTES && length <= MEMORY_BYTES - address;
}

static bool read_guest(void *opaque, uint32_t address, void *bytes, size_t length) {
    Fixture *fixture = opaque;
    ++fixture->reads;
    if (address == TEXT) ++fixture->text_reads;
    if (!in_guest(address, length) || (fixture->deny_text && address == TEXT)) return false;
    memcpy(bytes, fixture->guest.bytes + address, length);
    return true;
}

static bool replace_guest(void *opaque, uint32_t address, const void *expected, const void *replacement, size_t length) {
    Fixture *fixture = opaque;
    ++fixture->replacements;
    assert(address == FRAME && length == SKYRIM_FUZ_LIFT_FRAME_BYTES);
    uint8_t *frame = fixture->guest.bytes + address;
    if (fixture->deny_write || memcmp(frame, expected, length)) return false;
    memcpy(frame, replacement, length);
    return true;
}

static SkyrimFuzAvailability lookup_voice(void *opaque, const char *path) {
    Fixture *fixture = opaque;
    const uintptr_t start = (uintptr_t)fixture->guest.bytes, name = (uintptr_t)path;
    assert(name < start || name - start >= sizeof fixture->guest.bytes);
    ++fixture->probes;
    if (!strcmp(path, fixture->configured_stock)) {
        if (fixture->mutate_frame)
            fixture->guest.bytes[FRAME + SKYRIM_FUZ_LIFT_FRAME_BYTES - 1] = 0x79;
        if (fixture->mutate_text_header) --fixture->guest.bytes[RESPONSE + 4];
        return fixture->stock;
    }
    static const char *const candidates[] = {
        "Sound/Voice/Fixture.esp/Narrator/Reply.wav",
        "Sound/Voice/Fixture.esp/Narrator/Reply.fuz",
        "Sound/Voice/Fixture.esp/Narrator/Reply.xwm"
    };
    unsigned index = fixture->probes - 1;
    assert(index < sizeof candidates / sizeof candidates[0] && !strcmp(path, candidates[index]));
    return index == 0 ? fixture->wav : SKYRIM_FUZ_ABSENT;
}

void skyrim_fuz_host_before_voice_assign(uint32_t argument_slot, uint32_t response_object) {
    assert(active && !active->original_calls);
    ++active->hook_calls;
    const SkyrimFuzLiftMemory memory = {read_guest, replace_guest, active};
    active->changed = skyrim_fuz_apply_lifted_path(active->policy, &memory, argument_slot, response_object);
}

/* Run the original stack call with an authored assignment as the callee. The
 * opt-in private-lift test compiles its exact staged line in this function. */
typedef struct { uint32_t stack; } TestCpu;
#define M32(address) (active->guest.words[(address) / 4])
#define SPILL() (c->stack = esp)
#define RELOAD() (esp = c->stack)

static void f_A51210(TestCpu *c) {
    assert(active->hook_calls == 1 && active->original_calls++ == 0);
    assert(c->stack == SLOT - 4 && M32(c->stack) == RETURN_ADDRESS && M32(c->stack + 4) == FRAME);
    const uint8_t *path = active->guest.bytes + FRAME;
    const uint8_t *end = memchr(path, '\0', SKYRIM_FUZ_LIFT_FRAME_BYTES);
    assert(end);
    memcpy(active->assigned, path, (size_t)(end - path) + 1);
    c->stack += 8;
}

static void run_assignment(Fixture *fixture) {
    active = fixture;
    TestCpu registers = {0}, *c = &registers;
    uint32_t esp = SLOT, esi = RESPONSE;
#ifdef SKYRIM_FUZ_STAGED_SITE
#include "reviewed_voice_site.inc"
#else
    skyrim_fuz_host_before_voice_assign(esp, esi);
    esp -= 4;
    M32(esp) = RETURN_ADDRESS;
    SPILL();
    f_A51210(c);
    RELOAD();
#endif
    assert(esp == SLOT + 4);
    active = NULL;
}

static void set_u32(uint8_t *bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
}

static void initialize(Fixture *fixture, const char *response) {
    *fixture = (Fixture){.stock = SKYRIM_FUZ_PRESENT, .configured_stock = stock_path};
    memset(fixture->guest.bytes, 0x5b, sizeof fixture->guest.bytes);
    set_u32(fixture->guest.bytes + SLOT, FRAME);
    memcpy(fixture->guest.bytes + FRAME, original, sizeof original);
    set_u32(fixture->guest.bytes + RESPONSE, TEXT);
    const size_t length = strlen(response);
    assert(length + 1 < 256 && in_guest(TEXT, length + 1));
    set_u32(fixture->guest.bytes + RESPONSE + 4, (uint32_t)(length | ((length + 1) << 16)));
    memcpy(fixture->guest.bytes + TEXT, response, length + 1);
}

static SkyrimFuzPolicy configure(Fixture *fixture) {
    SkyrimFuzPolicy policy = {.lookup = lookup_voice, .context = fixture, .words_per_second = 2,
                              .skip_empty_responses = true};
    policy.stock_paths[3] = fixture->configured_stock;
    return policy;
}

static void check_assignment(Fixture *fixture, bool changed, unsigned probes, unsigned replacements) {
    TestMemory expected = fixture->guest;
    set_u32(expected.bytes + SLOT - 4, RETURN_ADDRESS);
    if (changed) memcpy(expected.bytes + FRAME, stock_game_path, sizeof stock_game_path);
    if (fixture->mutate_frame) expected.bytes[FRAME + SKYRIM_FUZ_LIFT_FRAME_BYTES - 1] = 0x79;
    if (fixture->mutate_text_header) --expected.bytes[RESPONSE + 4];
    run_assignment(fixture);
    assert(fixture->hook_calls == 1 && fixture->original_calls == 1 && fixture->changed == changed);
    assert(fixture->probes == probes && fixture->replacements == replacements);
    assert(!strcmp(fixture->assigned, changed ? stock_game_path : original));
    assert(!memcmp(fixture->guest.bytes, expected.bytes, sizeof expected.bytes));
}

int main(void) {
    const char text[] = "The gate remains closed until sunrise";
    Fixture fixture;
    initialize(&fixture, text);
    check_assignment(&fixture, false, 0, 0);
    assert(!fixture.reads);

    initialize(&fixture, text);
    SkyrimFuzPolicy policy = configure(&fixture);
    fixture.policy = &policy;
    check_assignment(&fixture, true, 4, 1);
    assert(fixture.text_reads == 1);

    initialize(&fixture, text);
    fixture.policy = &policy;
    fixture.wav = SKYRIM_FUZ_PRESENT;
    check_assignment(&fixture, false, 1, 0);
    assert(!fixture.text_reads);

    initialize(&fixture, text);
    fixture.policy = &policy;
    fixture.stock = SKYRIM_FUZ_ABSENT;
    check_assignment(&fixture, false, 4, 0);

    initialize(&fixture, text);
    policy.stock_paths[3] = NULL;
    fixture.policy = &policy;
    check_assignment(&fixture, false, 3, 0);
    policy.stock_paths[3] = stock_path;

    initialize(&fixture, text);
    fixture.policy = &policy;
    fixture.wav = SKYRIM_FUZ_LOOKUP_ERROR;
    check_assignment(&fixture, false, 3, 0);
    assert(!fixture.text_reads);

    initialize(&fixture, text);
    fixture.policy = &policy;
    fixture.deny_write = true;
    check_assignment(&fixture, false, 4, 1);

    initialize(&fixture, text);
    fixture.policy = &policy;
    fixture.mutate_frame = true;
    check_assignment(&fixture, false, 4, 1);

    initialize(&fixture, text);
    fixture.policy = &policy;
    fixture.mutate_text_header = true;
    check_assignment(&fixture, false, 4, 0);

    initialize(&fixture, text);
    fixture.policy = &policy;
    fixture.deny_text = true;
    check_assignment(&fixture, false, 3, 0);
    assert(fixture.text_reads == 1);

    initialize(&fixture, text);
    fixture.policy = &policy;
    fixture.guest.bytes[RESPONSE + 6] = 1;
    check_assignment(&fixture, false, 0, 0);

    initialize(&fixture, text);
    char oversized[300];
    memset(oversized, 'q', sizeof oversized - 1);
    oversized[sizeof oversized - 1] = '\0';
    fixture.configured_stock = oversized;
    policy = configure(&fixture);
    fixture.policy = &policy;
    check_assignment(&fixture, false, 4, 0);

    initialize(&fixture, text);
    const SkyrimFuzLiftMemory memory = {read_guest, replace_guest, &fixture};
    policy = configure(&fixture);
    assert(!skyrim_fuz_apply_lifted_path(&policy, &memory, UINT32_MAX, RESPONSE));
    assert(!fixture.reads);
    set_u32(fixture.guest.bytes + SLOT, FRAME + 1);
    assert(!skyrim_fuz_apply_lifted_path(&policy, &memory, SLOT, RESPONSE));
    assert(fixture.reads == 1 && !fixture.probes && !fixture.replacements);
    puts("Hosted LE voice seam preserved the original reference assignment.");
    return 0;
}
