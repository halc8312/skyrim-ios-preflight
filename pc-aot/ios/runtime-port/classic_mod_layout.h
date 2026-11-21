/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/* Public 32-bit TESV 1.9.32 layout described by the Classic 1.7.3 SDK.
 * The active pointer array is indexed by assigned load index. The linked
 * list can also include files that have no assigned index. */
enum {
    CLASSIC_DATA_HANDLER = 0x012e2cb8,
    CLASSIC_MOD_LIST = 0x694,
    CLASSIC_LOADED_COUNT = 0x69c,
    CLASSIC_LOADED_MODS = 0x6a0,
    CLASSIC_FILE_NAME = 0x20,
    CLASSIC_FILE_NAME_BYTES = 0x104,
    CLASSIC_FILE_MASTER_COUNT = 0x3fc,
    CLASSIC_FILE_MASTERS = 0x400,
    CLASSIC_FILE_INDEX = 0x40c,
    CLASSIC_FILE_AUTHOR = 0x410,
    CLASSIC_FILE_DESCRIPTION = 0x418
};
