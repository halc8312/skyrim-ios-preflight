/* SPDX-License-Identifier: GPL-3.0-only */
#include "camera_state.h"
#include <assert.h>
#include <stddef.h>

static const char pinned_guest[] = "d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605";
enum { SET_STATE = 0x006533D0, SINGLETON = 0x012E7288, PLAYER = 0x2000 };

typedef struct {
    uint32_t player, current, states[13], unreadable;
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
    else assert(false);
    return true;
}

int main(void) {
    Frame frame = {.player = PLAYER};
    for (unsigned slot = 0; slot < sizeof frame.states / sizeof *frame.states; ++slot)
        frame.states[slot] = 0x3000 + 16 * slot;

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
    return 0;
}
