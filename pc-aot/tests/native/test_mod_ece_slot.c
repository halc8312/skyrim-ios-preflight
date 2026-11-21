/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_ece_slot.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    assert(argc == 2 || (argc == 3 && !strcmp(argv[2], "stock")));
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    char *bytes = malloc(SKYRIM_ECE_SLOT_LIMIT + 1);
    if (!bytes) { fclose(f); return 2; }
    size_t length = fread(bytes, 1, SKYRIM_ECE_SLOT_LIMIT + 1, f);
    bool failed = ferror(f) != 0;
    fclose(f);
    if (failed) { free(bytes); return 2; }
    float value = 123.f;
    SkyrimEceSlotStatus status = argc == 3 ? skyrim_ece_slot_stock_nose_height(bytes, length, &value)
                                           : skyrim_ece_slot_read_nose_height(bytes, length, &value);
    free(bytes);
    assert(status == SKYRIM_ECE_SLOT_OK || value == 123.f);
    printf("%u %.9g\n", (unsigned)status, value);
    return 0;
}
