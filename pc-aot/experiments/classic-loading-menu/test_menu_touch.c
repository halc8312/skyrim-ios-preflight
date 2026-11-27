/* SPDX-License-Identifier: GPL-3.0-only */
#include "loading_menu.h"
#include "mod_loading_menu_host.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

enum { MANAGER = 0x12000, HOLDER = 0x12100, EVENT = 0x12300 };
uint8_t *MEM;
uint64_t skyrim_perf_now_ns(void) { return 0; }
int skyrim_device_memory_pressure(uint64_t now) { (void)now; return 0; }

static void word(unsigned address, uint32_t value) { memcpy(MEM+address,&value,sizeof(value)); }
static void dispatch(unsigned name_offset, bool opening) {
    unsigned name=0x20000+name_offset;
    word(HOLDER+name_offset,name);
    word(EVENT,name);
    word(EVENT+4,opening);
    skyrim_loading_menu_app_observe(opening ? CLASSIC_LOADING_MENU_OPEN_SITE : CLASSIC_LOADING_MENU_CLOSE_SITE,
                                    MANAGER+4,EVENT);
}

int main(void) {
    MEM=calloc(1,0x12e4000); assert(MEM);
    word(0x12e3548,MANAGER); word(0x12e35e0,HOLDER);
    skyrim_loading_menu_app_configure("unverified",false);
    dispatch(0x44,true); assert(!skyrim_loading_menu_touch_state().menus);
    skyrim_loading_menu_app_configure("d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605",false);
    const unsigned lists[]={0x44,0x64,0x7c,0x80,0x98,0xb4};
    for (unsigned i=0;i<sizeof(lists)/sizeof(lists[0]);i++) {
        dispatch(lists[i],true);
        SkyrimTouchMenu s=skyrim_loading_menu_touch_state(); assert(s.menus && s.list_navigation);
        dispatch(lists[i],true); assert(skyrim_loading_menu_touch_state().menus==s.menus);
        for (unsigned k=0;k<2;k++) {
            unsigned modal=k ? 0x90 : 0x58;
            dispatch(modal,true);
            assert(skyrim_loading_menu_touch_state().menus!=s.menus && !skyrim_loading_menu_touch_state().list_navigation);
            dispatch(modal,false); assert(skyrim_loading_menu_touch_state().list_navigation);
        }
        dispatch(0x88,true); assert(!skyrim_loading_menu_touch_state().menus);
        dispatch(0x88,false); assert(skyrim_loading_menu_touch_state().menus==s.menus);
        dispatch(0x74,true); assert(!skyrim_loading_menu_touch_state().menus && skyrim_safety_load_app_bypass());
        dispatch(0xb0,true); assert(!skyrim_safety_load_app_bypass());
        dispatch(0xb0,false); dispatch(0x74,false);
        assert(skyrim_loading_menu_touch_state().menus==s.menus);
        dispatch(lists[i],false); assert(!skyrim_loading_menu_touch_state().menus);
    }
    const unsigned pages[]={0xa8,0x4c,0xb8,0xa4,0xac};
    for (unsigned i=0;i<sizeof(pages)/sizeof(pages[0]);i++) {
        dispatch(pages[i],true);
        assert(skyrim_loading_menu_touch_state().menus && !skyrim_loading_menu_touch_state().list_navigation);
        dispatch(pages[i],false); assert(!skyrim_loading_menu_touch_state().menus);
    }
    dispatch(0x44,true); dispatch(0x64,true); assert(!skyrim_loading_menu_touch_state().list_navigation);
    dispatch(0x44,false); assert(skyrim_loading_menu_touch_state().list_navigation);
    dispatch(0x64,false); assert(!skyrim_loading_menu_touch_state().menus);
    dispatch(0x78,true); assert(!skyrim_loading_menu_touch_state().menus);
    free(MEM);
}
