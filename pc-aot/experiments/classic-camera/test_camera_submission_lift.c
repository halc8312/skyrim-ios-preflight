/* SPDX-License-Identifier: GPL-3.0-only */
#include "funcs_exe.h"
#include "lift_prelude.h"
#ifdef SKYRIM_CLASSIC_CAMERA_CULL_TRIAL
#include "camera_cull_trial.h"
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

enum {
    INCREMENT_IMPORT = 0x0106B150, DECREMENT_IMPORT = 0x0106B154,
    RENDER_READY = 0x01B913F0, WORLD_ROOT = 0x01BA7680,
    FRAME = 0x20000, STACK = 0x4000, BODY = 0x24000, PARENT = 0x25000,
    CHILDREN = 0x7000, WORLD = 0x8000, WORLD_CHILDREN = 0x9000,
    CONTEXT = 0xA000, CONTEXT_VTABLE = 0xB000, WRAPPER = 0xC000,
    WRAPPER_VTABLE = 0xD000, RESOURCE = 0xE000, CAMERA = 0xF000,
    BODY_VTABLE = 0x10000, PARENT_VTABLE = 0x11000, LIST_STORAGE = 0x12000,
    FIRST_LIST = FRAME + 0xC8, SECOND_LIST = FRAME + 0xD4, THIRD_LIST = 0x13000,
    INCREMENT = 1, DECREMENT, FINALIZE, LEAF,
    UNCULLED = 0x800, CULLED = UNCULLED | 1
};

uint8_t *MEM;
static unsigned first_dispatches, leaf_visits;
static uint32_t first_entry;

void classic_camera_collect_first_root(CPU *cpu);

void rt_call(CPU *cpu, uint32_t target) {
    // The stock application-cull gate tail-calls the context's +0x48.
    assert(target == 0xB07C80 && cpu->r[1] == CONTEXT);
    f_B07C80(cpu);
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    (void)cache;
    const uint32_t argument = M32(cpu->r[4] + 4);
    switch (target) {
    case INCREMENT:
    case DECREMENT:
        assert(argument == BODY + 4 || argument == PARENT + 4);
        assert(M32(argument) != 0);
        cpu->r[0] = (target == INCREMENT) ? ++M32(argument) : --M32(argument);
        cpu->r[4] += 8;
        break;
    case 0xB07E70: // BSCullingProcess +0x4C, the first entry in each list.
        assert(cpu->r[1] == CONTEXT && argument == CAMERA);
        ++first_dispatches;
        first_entry = M32(cpu->r[4] + 8);
        assert(M32(first_entry + 4) == 2);
        f_B07E70(cpu);
        break;
    case 0xAAE130: // NiNode +0xC4, including its real child application-cull gate.
        assert(cpu->r[1] == PARENT && argument == CONTEXT && M32(PARENT + 4) == 2);
        f_AAE130(cpu);
        break;
    case LEAF:
        // Stop at the authored body's render callback before any GPU work.
        assert(cpu->r[1] == BODY && argument == CONTEXT && M32(BODY + 4) > 0);
        ++leaf_visits;
        cpu->r[4] += 8;
        break;
    case FINALIZE:
        assert(cpu->r[1] == WRAPPER);
        cpu->r[4] += 8;
        break;
    default:
        abort();
    }
}

// Small context shims keep the fixture on the stock BSCullingProcess route
// through +0x4C/+0x48, with no real render state initialization.
void f_B1F880(CPU *cpu) { cpu->r[4] += 4; }
void f_ACE0C0(CPU *cpu) { cpu->r[4] += 8; }
void f_633580(CPU *cpu) { (void)cpu; abort(); }
void f_ACDE80(CPU *cpu) { (void)cpu; abort(); }
void f_ACE100(CPU *cpu) { (void)cpu; abort(); }
void f_B19BA0(CPU *cpu) { (void)cpu; abort(); }
void f_B19BE0(CPU *cpu) { (void)cpu; abort(); }
void f_B19EB0(CPU *cpu) { (void)cpu; abort(); }
void f_B080C0(CPU *cpu) { (void)cpu; abort(); }
void f_B07F40(CPU *cpu) { (void)cpu; abort(); }

static void init_list(uint32_t list, unsigned slot) {
    M32(list) = LIST_STORAGE + slot * 16;
    M32(list + 4) = 2;
    M32(list + 8) = 0;
}

static void collect_first_root(void) {
    CPU cpu = {0};
    cpu.r[3] = FIRST_LIST;
    cpu.r[4] = STACK;
    M32(STACK) = 0x698866;
    classic_camera_collect_first_root(&cpu);
    assert(cpu.r[4] == STACK);
}

static void retain(uint32_t list, uint32_t root) {
    CPU cpu = {0};
    cpu.r[1] = list;
    cpu.r[4] = STACK;
    M32(STACK + 4) = root;
    f_7BB370(&cpu);
    assert(cpu.r[4] == STACK + 8 && M32(list + 8) == 1);
}

static void release(uint32_t list) {
    CPU cpu = {0};
    cpu.r[1] = list;
    cpu.r[4] = STACK;
    M32(STACK + 4) = 0;
    M32(STACK + 8) = M32(list + 8);
    f_45F5E0(&cpu);
    assert(cpu.r[4] == STACK + 12);
    M32(list + 8) = 0; // What the frame owner does after dropping this list.
}

static void check_handoff(uint32_t list, uint32_t entry, unsigned expected_first, unsigned expected_leaf) {
    CPU cpu = {0};
    cpu.r[1] = WRAPPER;
    cpu.r[4] = STACK;
    M32(STACK) = 0x69AB11;
    M32(STACK + 4) = 0;
    M32(STACK + 8) = FIRST_LIST;
    M32(STACK + 12) = SECOND_LIST;
    M32(STACK + 16) = THIRD_LIST;
    first_dispatches = leaf_visits = first_entry = 0;
    const uint32_t body_flags = M32(BODY + 0x98);
    const uint32_t parent_flags = M32(PARENT + 0x98);
    assert(!entry || (M32(list + 8) == 1 && M32(M32(list)) == entry));
    f_CB9E20(&cpu);
    assert(cpu.r[4] == STACK + 20);
    assert(first_dispatches == expected_first && leaf_visits == expected_leaf);
    assert(first_entry == (expected_first ? entry : 0));
    assert(M32(BODY + 0x98) == body_flags && M32(PARENT + 0x98) == parent_flags);
    release(list);
    assert(M32(BODY + 4) == 1 && M32(PARENT + 4) == 1);
}

#ifdef SKYRIM_CLASSIC_CAMERA_CULL_TRIAL
enum {
    PLAYER_CAMERA_SINGLETON = 0x012E7288,
    PLAYER_ACTOR_SINGLETON = 0x01B2E8E4,
    PLAYER_CAMERA = 0x26000, FIRST_STATE = 0x27000, ACTOR = 0x28000, LOADED = 0x29000
};

static ClassicCameraCullTrialReport latest_report;
static unsigned reports;

static bool trial_read(void *guest, uint32_t address, uint32_t *value) {
    assert(guest == MEM && address >= 0x10000 && address <= WORLD_ROOT);
    // The app-cull flags are read by the stock paths. The observer itself
    // must not read any field of a candidate node.
    assert(!(address >= BODY && address < BODY + 0x100));
    assert(!(address >= PARENT && address < PARENT + 0x100));
    *value = M32(address);
    return true;
}

static void trial_report(void *context, const ClassicCameraCullTrialReport *report) {
    assert(context == MEM);
    ++reports;
    latest_report = *report;
}

static void begin_trial(void) {
    const char *image = "d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605";
    reports = 0;
    assert(classic_camera_cull_arm(image, trial_read, MEM, trial_report, MEM));
    assert(skyrim_camera_cull_prepare(FRAME));
}

static void check_counts(unsigned phase, unsigned checked, unsigned clear, unsigned culled) {
    assert(latest_report.stages & (1u << phase));
    const ClassicCameraCullCounts got = latest_report.gates[phase];
    assert(got.checked == checked && got.clear == clear && got.culled == culled);
}

static void check_cull_trial(void) {
    M32(PLAYER_CAMERA_SINGLETON) = PLAYER_CAMERA;
    M32(PLAYER_CAMERA + 0x20) = M32(PLAYER_CAMERA + 0x6C) = FIRST_STATE;
    M32(PLAYER_ACTOR_SINGLETON) = ACTOR;
    M32(ACTOR + 0x44) = LOADED;
    M32(LOADED + 0x20) = BODY;

    M32(WORLD_CHILDREN) = BODY;
    M32(BODY + 0x98) = CULLED;
    begin_trial();
    collect_first_root();
    skyrim_camera_cull_prepared(true);
    check_handoff(FIRST_LIST, 0, 0, 0);
    skyrim_camera_cull_finish(true);
    assert(reports == 1 && latest_report.stages == 7);
    check_counts(CLASSIC_CULL_PRODUCER, 1, 0, 1);
    check_counts(CLASSIC_CULL_FIRST, 0, 0, 0);

    M32(BODY + 0x98) = UNCULLED;
    begin_trial();
    collect_first_root();
    skyrim_camera_cull_prepared(true);
    M32(BODY + 0x98) = CULLED;
    check_handoff(FIRST_LIST, BODY, 1, 0);
    skyrim_camera_cull_finish(true);
    assert(reports == 1);
    check_counts(CLASSIC_CULL_PRODUCER, 1, 1, 0);
    check_counts(CLASSIC_CULL_FIRST, 1, 0, 1);
    check_counts(CLASSIC_CULL_SECOND, 0, 0, 0);

    M32(WORLD_CHILDREN) = PARENT;
    begin_trial();
    collect_first_root();
    skyrim_camera_cull_prepared(true);
    check_handoff(FIRST_LIST, PARENT, 1, 0);
    skyrim_camera_cull_finish(true);
    assert(reports == 1);
    check_counts(CLASSIC_CULL_PRODUCER, 1, 0, 0);
    check_counts(CLASSIC_CULL_FIRST, 2, 0, 1);

    begin_trial();
    skyrim_camera_cull_prepared(true);
    retain(SECOND_LIST, BODY);
    check_handoff(SECOND_LIST, BODY, 1, 0);
    skyrim_camera_cull_finish(true);
    assert(reports == 1);
    check_counts(CLASSIC_CULL_FIRST, 0, 0, 0);
    check_counts(CLASSIC_CULL_SECOND, 1, 0, 1);

    M32(BODY + 0x98) = UNCULLED;
    begin_trial();
    skyrim_camera_cull_prepared(true);
    retain(THIRD_LIST, BODY);
    check_handoff(THIRD_LIST, BODY, 1, 1);
    skyrim_camera_cull_finish(true);
    assert(reports == 1);
    check_counts(CLASSIC_CULL_FIRST, 0, 0, 0);
    check_counts(CLASSIC_CULL_SECOND, 0, 0, 0);

    begin_trial();
    skyrim_camera_cull_producer(0, BODY, true);
    for (unsigned i = 0; i < CLASSIC_CULL_PROBE_LIMIT + 20; ++i)
        skyrim_camera_cull_producer(0x698866, BODY, true);
    skyrim_camera_cull_prepared(true);
    assert(!skyrim_camera_cull_dispatch(0, FIRST_LIST, CLASSIC_CULL_FIRST));
    assert(!skyrim_camera_cull_dispatch(0x69AB11, THIRD_LIST, CLASSIC_CULL_FIRST));
    skyrim_camera_cull_finish(true);
    assert(reports == 1 && latest_report.stages == 1);
    check_counts(CLASSIC_CULL_PRODUCER, CLASSIC_CULL_PROBE_LIMIT + 1, CLASSIC_CULL_PROBE_LIMIT, 0);

    begin_trial();
    skyrim_camera_cull_prepared(true);
    M32(LOADED + 0x20) = PARENT;
    skyrim_camera_cull_finish(true);
    assert(reports == 0);
    M32(LOADED + 0x20) = BODY;

    begin_trial();
    skyrim_camera_cull_prepared(true);
    assert(!skyrim_camera_cull_prepare(FRAME));
    skyrim_camera_cull_finish(true);
    assert(reports == 0);
    assert(!skyrim_camera_cull_prepare(FRAME));
}
#endif

int main(void) {
    MEM = calloc(1, WORLD_ROOT + 4);
    assert(MEM);
    M32(INCREMENT_IMPORT) = INCREMENT;
    M32(DECREMENT_IMPORT) = DECREMENT;
    M32(RENDER_READY) = 1;
    M32(WORLD_ROOT) = WORLD;
    M32(WORLD + 0xAC) = WORLD_CHILDREN;
    M16(WORLD + 0xB2) = 1;

    M32(CONTEXT) = CONTEXT_VTABLE;
    M32(CONTEXT + 8) = 1;
    M8(CONTEXT + 0x16C) = 1;
    M32(CONTEXT_VTABLE + 0x48) = 0xB07C80;
    M32(CONTEXT_VTABLE + 0x4C) = 0xB07E70;
    M32(WRAPPER) = WRAPPER_VTABLE;
    M32(WRAPPER + 0xD8) = RESOURCE;
    M32(RESOURCE + 0xD0) = RESOURCE;
    M32(WRAPPER + 0x128) = CAMERA;
    M32(WRAPPER + 0x12C) = CONTEXT;
    M32(WRAPPER_VTABLE + 0x20) = FINALIZE;

    M32(BODY) = BODY_VTABLE;
    M32(PARENT) = PARENT_VTABLE;
    M32(BODY_VTABLE + 0xC4) = LEAF;
    M32(PARENT_VTABLE + 0xC4) = 0xAAE130;
    M32(BODY + 4) = M32(PARENT + 4) = 1;
    M32(WORLD_CHILDREN) = BODY;

    init_list(FIRST_LIST, 0);
    init_list(SECOND_LIST, 1);
    init_list(THIRD_LIST, 2);
    M32(BODY + 0x98) = CULLED;
    collect_first_root();
    assert(M32(FIRST_LIST + 8) == 0 && M32(BODY + 4) == 1);
    check_handoff(FIRST_LIST, 0, 0, 0);

    M32(BODY + 0x98) = UNCULLED;
    collect_first_root();
    check_handoff(FIRST_LIST, BODY, 1, 1);

    // Even if the node is already retained, the later stock gate can skip it.
    collect_first_root();
    M32(BODY + 0x98) = CULLED;
    check_handoff(FIRST_LIST, BODY, 1, 0);
    retain(SECOND_LIST, BODY);
    check_handoff(SECOND_LIST, BODY, 1, 0);
    retain(THIRD_LIST, BODY);
    check_handoff(THIRD_LIST, BODY, 0, 0);
    M32(BODY + 0x98) = UNCULLED;
    retain(THIRD_LIST, BODY);
    check_handoff(THIRD_LIST, BODY, 1, 1);

    // A retained ancestor keeps its own reference but tests each child.
    M32(WORLD_CHILDREN) = PARENT;
    M32(PARENT + 0x98) = UNCULLED;
    M32(PARENT + 0xAC) = CHILDREN;
    M16(PARENT + 0xB2) = 1;
    M32(CHILDREN) = BODY;
    for (unsigned app_cull = 0; app_cull < 2; ++app_cull) {
        M32(BODY + 0x98) = UNCULLED | app_cull;
        collect_first_root();
        assert(M32(BODY + 4) == 1);
        check_handoff(FIRST_LIST, PARENT, 1, app_cull == 0);
    }

#ifdef SKYRIM_CLASSIC_CAMERA_CULL_TRIAL
    check_cull_trial();
#endif
    free(MEM);
    return 0;
}
