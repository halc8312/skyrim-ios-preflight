/* SPDX-License-Identifier: GPL-3.0-only */
#include "PcGuestModHooksABI.h"
#include "x86.h"

#include <assert.h>
#include <pthread.h>

enum { FRAME_ABSENT, FRAME_PENDING, FRAME_WAITED, FIRST_FS = 0x7400, OTHER_FS = 0x8100 };

extern const PcGuestModHooksGuest *skyrim_guest_bind_mod_hooks_v1(uint32_t, uint32_t);

static bool scene_notification(const CPU *cpu, enum SkyrimHighHeelSceneNotice site,
                               uint32_t first, uint32_t second, uint32_t third) {
    (void)cpu; (void)site; (void)first; (void)second; (void)third;
    assert(!"A frame receipt must remain local until a scene is observed");
    return false;
}

static void *check_other_thread(void *context) {
    assert(skyrim_highheel_scene_guest_frame_receipt(context, FIRST_FS) == FRAME_ABSENT);
    skyrim_highheel_scene_guest_frame_begin(context, FIRST_FS);
    assert(skyrim_highheel_scene_guest_frame_receipt(context, FIRST_FS) == FRAME_PENDING);
    skyrim_highheel_scene_guest_frame_end(context);
    return NULL;
}

static void check_frames(void) {
    CPU first = {.fs_base = FIRST_FS}, other = {.fs_base = OTHER_FS};
    assert(skyrim_highheel_scene_guest_state == 0);
    skyrim_highheel_scene_guest_frame_wait(&first, FIRST_FS, 0);
    assert(skyrim_highheel_scene_guest_frame_receipt(&first, FIRST_FS) == FRAME_ABSENT);
    skyrim_highheel_scene_guest_frame_end(&first);

    const uint32_t group = PC_GUEST_MOD_HOOK_HIGHHEEL_SCENE_OBSERVE;
    const PcGuestModHooks hooks = {.version = PC_GUEST_MOD_HOOKS_ABI, .bytes = sizeof hooks,
                                  .groups = group, .highheel_scene = {scene_notification}};
    const PcGuestModHooksGuest *guest = skyrim_guest_bind_mod_hooks_v1(hooks.version, hooks.bytes);
    assert(guest && (guest->compiled_groups & group) && guest->select(&hooks) == 0);

    skyrim_highheel_scene_guest_frame_begin(&first, FIRST_FS);
    assert(skyrim_highheel_scene_guest_frame_receipt(&first, FIRST_FS) == FRAME_PENDING);
    skyrim_highheel_scene_guest_frame_wait(&other, FIRST_FS, 0);
    skyrim_highheel_scene_guest_frame_wait(&first, OTHER_FS, 0);
    assert(skyrim_highheel_scene_guest_frame_receipt(&first, FIRST_FS) == FRAME_PENDING);
    skyrim_highheel_scene_guest_frame_wait(&first, FIRST_FS, 0);
    assert(skyrim_highheel_scene_guest_frame_receipt(&first, FIRST_FS) == FRAME_WAITED);
    assert(skyrim_highheel_scene_guest_frame_receipt(&first, OTHER_FS) == FRAME_ABSENT);
    assert(skyrim_highheel_scene_guest_frame_receipt(&other, FIRST_FS) == FRAME_ABSENT);
    skyrim_highheel_scene_guest_frame_end(&other);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, check_other_thread, &first));
    assert(!pthread_join(thread, NULL));
    assert(skyrim_highheel_scene_guest_frame_receipt(&first, FIRST_FS) == FRAME_WAITED);
    first.fs_base = OTHER_FS;
    skyrim_highheel_scene_guest_frame_end(&first);
    assert(skyrim_highheel_scene_guest_frame_receipt(&first, FIRST_FS) == FRAME_ABSENT);
    first.fs_base = FIRST_FS;

    skyrim_highheel_scene_guest_frame_begin(&first, FIRST_FS);
    skyrim_highheel_scene_guest_frame_begin(&first, FIRST_FS);
    skyrim_highheel_scene_guest_frame_wait(&first, FIRST_FS, 0);
    assert(skyrim_highheel_scene_guest_frame_receipt(&first, FIRST_FS) != FRAME_WAITED);
    skyrim_highheel_scene_guest_frame_end(&first);
    skyrim_highheel_scene_guest_frame_end(&first);
    assert(skyrim_highheel_scene_guest_frame_receipt(&first, FIRST_FS) == FRAME_ABSENT);
}

#ifdef SKYRIM_TEST_OWNED_FRAME_WAIT
#include "guest_memory.h"
#include "lift_prelude.h"

#include <string.h>

enum {
    STACK = 0x2fc0, POOL = 0x2100, QUEUE = 0x2200, SEMAPHORE = 91,
    WAIT_IMPORT = 0x106b158, COUNT_IMPORT = 0x106b154, WAIT = 1, COUNT = 2,
    WAITER_RETURN = 0x6a0902, COUNTER_RETURN = 0x6a0910, FRAME_RETURN = 0x69477f,
    PHASE_RETURN = 0x6f3c84, COUNTER_RESULT = 37, POOL_SINGLETON = 0x1b38308,
    CAMERA_SINGLETON = 0x12e32e8, CAMERA = 0x2150
};

uint8_t *MEM;
static uint32_t wait_result;
static unsigned waits, increments, camera_receipt;

void f_6A08F0(CPU *cpu);
void fixture_original_wait(CPU *cpu);
void fixture_frame_prefix(CPU *cpu);

void f_6F3AB0(CPU *cpu) {
    const uint32_t stack = cpu->r[4];
    assert(stack == STACK - 12 && M32(stack) == 0x694788 && cpu->r[1] == STACK - 1);
    cpu->r[4] += 4;
}

void f_A58FF0(CPU *cpu) {
    const uint32_t stack = cpu->r[4];
    assert(stack == STACK - 16 && M32(stack) == 0x69478f && M32(stack + 4) == 0x13);
    cpu->r[4] += 4;
}

void f_75CFB0(CPU *cpu) {
    const uint32_t stack = cpu->r[4];
    assert(stack == STACK - 12 && M32(stack) == 0x69479d && cpu->r[1] == CAMERA);
    camera_receipt = skyrim_highheel_scene_guest_frame_receipt(cpu, cpu->fs_base);
    cpu->r[4] += 4;
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    (void)cache;
    const uint32_t stack = cpu->r[4];
    if (target == WAIT) {
        assert(M32(stack) == WAITER_RETURN && M32(stack + 4) == SEMAPHORE);
        assert(M32(stack + 8) == UINT32_MAX);
        ++waits;
        cpu->r[0] = wait_result;
        cpu->r[4] += 12;
    } else {
        assert(target == COUNT && M32(stack) == COUNTER_RETURN && M32(stack + 4) == QUEUE + 0x18);
        ++increments;
        ++M32(QUEUE + 0x18);
        cpu->r[0] = COUNTER_RESULT;
        cpu->r[4] += 8;
    }
}

typedef struct {
    uint32_t registers[8];
    Flags flags;
    uint8_t stack[48];
} WaitState;

static WaitState run_wait(void (*wait)(CPU *), uint32_t result, uint32_t caller, unsigned expected_receipt) {
    CPU cpu = {.r = {1, POOL, 3, 4, STACK, 5, 6, 7}, .fs_base = FIRST_FS};
    memset(MEM + STACK - 32, 0, 48);
    M32(STACK) = caller;
    M32(STACK + 12) = PHASE_RETURN;
    M32(QUEUE + 0x18) = 0;
    wait_result = result;
    waits = increments = 0;
    skyrim_highheel_scene_guest_frame_begin(&cpu, cpu.fs_base);
    wait(&cpu);
    assert(skyrim_highheel_scene_guest_frame_receipt(&cpu, cpu.fs_base) == expected_receipt);
    skyrim_highheel_scene_guest_frame_end(&cpu);
    assert(waits == 1 && increments == (result == 0) && M32(QUEUE + 0x18) == increments);
    assert(cpu.r[0] == (result == 0 ? COUNTER_RESULT : result));
    assert(cpu.r[4] == STACK + 4 && cpu.r[6] == 6);
    WaitState state = {.flags = cpu.flags};
    memcpy(state.registers, cpu.r, sizeof state.registers);
    memcpy(state.stack, MEM + STACK - 32, sizeof state.stack);
    return state;
}

static void compare_wait(uint32_t result, uint32_t caller, unsigned receipt) {
    const WaitState original = run_wait(fixture_original_wait, result, caller, FRAME_PENDING);
    const WaitState observed = run_wait(f_6A08F0, result, caller, receipt);
    assert(!memcmp(original.registers, observed.registers, sizeof observed.registers));
    assert(!memcmp(&original.flags, &observed.flags, sizeof observed.flags));
    assert(!memcmp(original.stack, observed.stack, sizeof observed.stack));
}

static void check_frame_prefix(uint32_t result, uint32_t caller, unsigned receipt) {
    CPU cpu = {.r = {1, 2, 3, 4, STACK, 5, 6, 7}, .fs_base = FIRST_FS};
    M32(STACK) = caller;
    wait_result = result;
    waits = increments = 0;
    camera_receipt = UINT32_MAX;
    fixture_frame_prefix(&cpu);
    assert(waits == 1 && increments == (result == 0));
    assert(camera_receipt == receipt && cpu.r[4] == STACK - 8);
    assert(skyrim_highheel_scene_guest_frame_receipt(&cpu, cpu.fs_base) == FRAME_ABSENT);
}

static void check_owned_wait(void) {
    gm_space memory = {0};
    assert(gm_reserve(&memory, NULL));
    const uint32_t addresses[] = {STACK, POOL, WAIT_IMPORT, POOL_SINGLETON, CAMERA_SINGLETON};
    for (unsigned i = 0; i < sizeof addresses / sizeof *addresses; ++i)
        if (!gm_pointer(&memory, addresses[i], 4))
            assert(gm_commit_page(&memory, addresses[i] / memory.page_size * memory.page_size, NULL));
    MEM = memory.base;
    M32(POOL) = QUEUE;
    M32(QUEUE + 0x14) = SEMAPHORE;
    M32(WAIT_IMPORT) = WAIT;
    M32(COUNT_IMPORT) = COUNT;
    M32(POOL_SINGLETON) = POOL;
    M32(CAMERA_SINGLETON) = CAMERA;
    compare_wait(0, FRAME_RETURN, FRAME_WAITED);
    compare_wait(UINT32_MAX, FRAME_RETURN, FRAME_PENDING);
    compare_wait(258, FRAME_RETURN, FRAME_PENDING);
    compare_wait(0, 0x12345, FRAME_PENDING);
    check_frame_prefix(0, PHASE_RETURN, FRAME_WAITED);
    check_frame_prefix(UINT32_MAX, PHASE_RETURN, FRAME_PENDING);
    check_frame_prefix(0, 0x12345, FRAME_ABSENT);
    assert(gm_release(&memory, NULL));
}
#endif

int main(void) {
    check_frames();
#ifdef SKYRIM_TEST_OWNED_FRAME_WAIT
    check_owned_wait();
#endif
    return 0;
}
