/* SPDX-License-Identifier: GPL-3.0-only */
#include "camera_state.h"
#include <assert.h>
#include <stddef.h>

static const char pinned_guest[] = "d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605";
enum {
    SET_STATE = 0x006533D0, SINGLETON = 0x012E7288, PLAYER = 0x2000,
    ACTOR_GLOBAL = 0x01B2E8E4, ACTOR = 0x10000, LOADED_STATE = 0x11000,
    ARMS_ROOT = 0x12000, BODY_ROOT = 0x13000, PARENT = 0x14000
};

typedef struct {
    uint32_t player, current, states[13], unreadable;
    uint32_t actor, loaded_state, arms_root, body_root, arms_parent, body_parent, arms_flags, body_flags;
    unsigned reads;
} Frame;

static bool read_word(void *guest, uint32_t address, uint32_t *value) {
    Frame *frame = guest;
    ++frame->reads;
    if (address == frame->unreadable) return false;
    if (address == SINGLETON) *value = frame->player;
    else if (address == PLAYER + 0x20) *value = frame->current;
    else if (address >= PLAYER + 0x6C && address <= PLAYER + 0x9C && address % 4 == 0)
        *value = frame->states[(address - PLAYER - 0x6C) / 4];
    else if (address == ACTOR_GLOBAL) *value = frame->actor;
    else if (address == ACTOR + 0x44) *value = frame->loaded_state;
    else if (address == LOADED_STATE + 0x20) *value = frame->body_root;
    else if (address == ACTOR + 0x58C) *value = frame->arms_root;
    else if (address == ARMS_ROOT + 0x18) *value = frame->arms_parent;
    else if (address == ARMS_ROOT + 0x98) *value = frame->arms_flags;
    else if (address == BODY_ROOT + 0x18) *value = frame->body_parent;
    else if (address == BODY_ROOT + 0x98) *value = frame->body_flags;
    else assert(false);
    return true;
}

static ClassicCameraSample paired_sample(Frame *frame) {
    ClassicCameraSample sample;
    assert(classic_camera_sample_after_return(pinned_guest, SET_STATE, PLAYER, read_word, frame, &sample));
    return sample;
}

static void check_body_samples(Frame frame) {
    frame.actor = ACTOR;
    frame.loaded_state = LOADED_STATE;
    frame.arms_root = ARMS_ROOT;
    frame.body_root = BODY_ROOT;
    frame.arms_parent = PARENT;
    frame.body_parent = PARENT;
    frame.arms_flags = 0x200;
    frame.body_flags = 0x201;
    frame.current = frame.states[0];
    ClassicCameraSample sample = paired_sample(&frame);
    assert(sample.slot == 0 && sample.sampled_roots);
    assert(sample.arms == CLASSIC_CAMERA_ROOT_ATTACHED_CLEAR && sample.body == CLASSIC_CAMERA_ROOT_ATTACHED_CULL);

    frame.current = frame.states[8];
    frame.arms_parent = frame.body_parent = 0;
    frame.arms_flags ^= 1;
    frame.body_flags ^= 1;
    sample = paired_sample(&frame);
    assert(sample.slot == 8 && sample.sampled_roots);
    assert(sample.arms == CLASSIC_CAMERA_ROOT_DETACHED_CULL && sample.body == CLASSIC_CAMERA_ROOT_DETACHED_CLEAR);

    frame.current = frame.states[12];
    frame.reads = 0;
    sample = paired_sample(&frame);
    assert(sample.slot == 12 && !sample.sampled_roots && frame.reads == 15);

    frame.current = frame.states[9];
    frame.arms_root = frame.loaded_state = 0;
    sample = paired_sample(&frame);
    assert(sample.slot == 9 && sample.sampled_roots);
    assert(sample.arms == CLASSIC_CAMERA_ROOT_ABSENT && sample.body == CLASSIC_CAMERA_ROOT_ABSENT);

    frame.loaded_state = LOADED_STATE;
    frame.arms_root = BODY_ROOT;
    sample = paired_sample(&frame);
    assert(sample.arms == CLASSIC_CAMERA_ROOT_UNAVAILABLE && sample.body == CLASSIC_CAMERA_ROOT_UNAVAILABLE);

    frame.arms_root = ARMS_ROOT;
    frame.arms_parent = PARENT + 1;
    frame.body_root = UINT32_MAX - 3;
    sample = paired_sample(&frame);
    assert(sample.arms == CLASSIC_CAMERA_ROOT_UNAVAILABLE && sample.body == CLASSIC_CAMERA_ROOT_UNAVAILABLE);

    frame.arms_parent = PARENT;
    frame.body_root = BODY_ROOT;
    frame.unreadable = BODY_ROOT + 0x98;
    sample = paired_sample(&frame);
    assert(sample.arms == CLASSIC_CAMERA_ROOT_ATTACHED_CULL && sample.body == CLASSIC_CAMERA_ROOT_UNAVAILABLE);

    frame.actor = 0;
    sample = paired_sample(&frame);
    assert(sample.arms == CLASSIC_CAMERA_ROOT_UNAVAILABLE && sample.body == CLASSIC_CAMERA_ROOT_UNAVAILABLE);
}

int main(void) {
    Frame frame = {.player = PLAYER};
    for (unsigned slot = 0; slot < sizeof frame.states / sizeof *frame.states; ++slot)
        frame.states[slot] = 0x3000 + 16 * slot;
    check_body_samples(frame);

    /* Model completed guest transitions without calling back into the game. */
    const int slots[] = {0, 9, 10, 6, 12, 0};
    for (size_t i = 0; i < sizeof slots / sizeof *slots; ++i) {
        int slot = slots[i];
        frame.current = frame.states[slot];
        frame.reads = 0;
        assert(classic_camera_state_after_return(pinned_guest, SET_STATE, PLAYER, read_word, &frame) == slot);
        assert(frame.current == frame.states[slot] && frame.reads == (unsigned)slot + 3);
    }

    frame.reads = 0;
    assert(classic_camera_state_after_return("different", SET_STATE, PLAYER, read_word, &frame) == -1);
    assert(classic_camera_state_after_return(pinned_guest, SET_STATE + 0x57, PLAYER, read_word, &frame) == -1);
    assert(classic_camera_state_after_return(pinned_guest, SET_STATE, 0, read_word, &frame) == -1);
    assert(classic_camera_state_after_return(pinned_guest, SET_STATE, UINT32_MAX, read_word, &frame) == -1);
    assert(!frame.reads);
    assert(classic_camera_state_after_return(pinned_guest, SET_STATE, PLAYER + 4, read_word, &frame) == -1);
    assert(frame.reads == 1);

    frame.current = 0;
    frame.states[0] = 0;
    assert(classic_camera_state_after_return(pinned_guest, SET_STATE, PLAYER, read_word, &frame) == -1);
    frame.current = 0x4321;
    assert(classic_camera_state_after_return(pinned_guest, SET_STATE, PLAYER, read_word, &frame) == -1);
    frame.current = frame.states[12];
    frame.unreadable = PLAYER + 0x70;
    assert(classic_camera_state_after_return(pinned_guest, SET_STATE, PLAYER, read_word, &frame) == -1);
    ClassicCameraSample ignored;
    assert(!classic_camera_sample_after_return("different", SET_STATE, PLAYER, read_word, &frame, &ignored));
    assert(!classic_camera_sample_after_return(pinned_guest, SET_STATE, PLAYER, read_word, &frame, NULL));
    return 0;
}
