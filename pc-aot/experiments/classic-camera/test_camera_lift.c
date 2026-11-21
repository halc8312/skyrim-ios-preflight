/* SPDX-License-Identifier: GPL-3.0-only */
#include "camera_state.h"
#include "guest_memory.h"
#include "lift_prelude.h"

#include <assert.h>
#include <stdio.h>

enum {
    CAMERA_ENTRY = 0x6533D0, SINGLETON = 0x012E7288,
    ADD_REF_IMPORT = 0x106B150, RELEASE_IMPORT = 0x106B154,
    CAMERA = 0x2100, OTHER_CAMERA = 0x2200, FIRST = 0x2400, THIRD = 0x2500,
    STATE_VTABLE = 0x2600, STACK = 0x2FC0,
    ON_END = 1, ADD_REF, RELEASE, ON_DISPOSE, ON_START
};

uint8_t *MEM;
static gm_space memory;
static const char pinned_image[] = "d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605";
static CPU *active_cpu;
static uint32_t expected_receiver, expected_state;
static int last_slot;
static unsigned observations, events[5], event_count;

void f_6533D0(CPU *cpu);
void unobserved_camera_transition(CPU *cpu);

static bool read_word(void *guest, uint32_t address, uint32_t *value) {
    void *bytes = gm_pointer(guest, address, sizeof *value);
    if (!bytes) return false;
    memcpy(value, bytes, sizeof *value);
    return true;
}

void skyrim_camera_observe_return(uint32_t receiver) {
    assert(receiver == expected_receiver);
    assert(active_cpu->r[4] == STACK + 8);
    assert(M32(receiver + 0x20) == expected_state);
    last_slot = classic_camera_state_after_return(pinned_image, CAMERA_ENTRY, receiver, read_word, &memory);
    ++observations;
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    (void)cache;
    assert(event_count < sizeof events / sizeof *events);
    unsigned args = 0;
    switch (target) {
    case ON_END:
        assert(cpu->r[1] == M32(expected_receiver + 0x20));
        break;
    case ADD_REF:
    case RELEASE: {
        uint32_t ref = M32(cpu->r[4] + 4);
        assert(ref == FIRST + 4 || ref == THIRD + 4);
        assert(target == ADD_REF || M32(ref) != 0);
        cpu->r[0] = target == ADD_REF ? ++M32(ref) : --M32(ref);
        args = 1;
        break;
    }
    case ON_DISPOSE:
        assert(M32(cpu->r[1] + 4) == 0 && M32(cpu->r[4] + 4) == 1);
        args = 1;
        break;
    case ON_START:
        assert(cpu->r[1] == M32(expected_receiver + 0x20));
        break;
    default: assert(false);
    }
    events[event_count++] = target;
    cpu->r[4] += 4 * (args + 1);
}

struct Result {
    uint32_t r[8], current, refs[2];
    unsigned calls[5], count;
};

static struct Result run(void (*fn)(CPU *), uint32_t receiver, uint32_t before, uint32_t after) {
    CPU cpu = {0};
    cpu.r[1] = receiver;
    cpu.r[3] = 3;
    cpu.r[4] = STACK;
    cpu.r[5] = 5;
    cpu.r[6] = 6;
    cpu.r[7] = 7;
    M32(receiver + 0x20) = before;
    M32(FIRST + 4) = before == FIRST;
    M32(THIRD + 4) = before == THIRD;
    M32(STACK) = 0x100;
    M32(STACK + 4) = after;
    active_cpu = &cpu;
    expected_receiver = receiver;
    expected_state = after;
    event_count = 0;
    observations = 0;
    last_slot = -2;
    fn(&cpu);
    active_cpu = NULL;
    assert(cpu.r[4] == STACK + 8 && cpu.r[3] == 3 && cpu.r[5] == 5 && cpu.r[6] == 6 && cpu.r[7] == 7);
    struct Result result = {.current = M32(receiver + 0x20),
                            .refs = {M32(FIRST + 4), M32(THIRD + 4)}, .count = event_count};
    memcpy(result.r, cpu.r, sizeof result.r);
    memcpy(result.calls, events, event_count * sizeof *events);
    return result;
}

int main(void) {
    assert(gm_reserve(&memory, NULL));
    const uint32_t pages[] = {CAMERA, STACK, SINGLETON, ADD_REF_IMPORT};
    for (unsigned i = 0; i < sizeof pages / sizeof *pages; ++i)
        if (!gm_pointer(&memory, pages[i], 4))
            assert(gm_commit_page(&memory, pages[i] / memory.page_size * memory.page_size, NULL));
    MEM = memory.base;
    M32(SINGLETON) = CAMERA;
    M32(CAMERA + 0x6c) = FIRST;
    M32(CAMERA + 0x6c + 9 * 4) = THIRD;
    M32(FIRST) = M32(THIRD) = STATE_VTABLE;
    M32(STATE_VTABLE) = ON_DISPOSE;
    M32(STATE_VTABLE + 4) = ON_START;
    M32(STATE_VTABLE + 8) = ON_END;
    M32(ADD_REF_IMPORT) = ADD_REF;
    M32(RELEASE_IMPORT) = RELEASE;

    const struct {
        uint32_t receiver, from, to;
        int slot;
    } cases[] = {
        {CAMERA, 0, FIRST, 0}, {CAMERA, FIRST, THIRD, 9}, {CAMERA, THIRD, THIRD, 9},
        {CAMERA, FIRST, 0, -1}, {CAMERA, 0, 0, -1}, {OTHER_CAMERA, FIRST, THIRD, -1}
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; ++i) {
        struct Result base = run(unobserved_camera_transition, cases[i].receiver, cases[i].from, cases[i].to);
        assert(!observations && base.current == cases[i].to);
        struct Result observed = run(f_6533D0, cases[i].receiver, cases[i].from, cases[i].to);
        assert(observations == 1 && last_slot == cases[i].slot);
        assert(base.count == observed.count && base.current == observed.current);
        assert(!memcmp(base.r, observed.r, sizeof base.r) && !memcmp(base.refs, observed.refs, sizeof base.refs));
        assert(!memcmp(base.calls, observed.calls, base.count * sizeof *base.calls));
        if (i == 1) {
            const unsigned transition[] = {ON_END, ADD_REF, RELEASE, ON_DISPOSE, ON_START};
            assert(observed.count == sizeof transition / sizeof *transition);
            assert(!memcmp(observed.calls, transition, sizeof transition));
        }
    }
    assert(gm_release(&memory, NULL));
    puts("Camera observer preserved the reviewed transition and stack");
    return 0;
}
