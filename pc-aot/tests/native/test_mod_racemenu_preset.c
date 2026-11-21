/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_racemenu_preset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* argv[1] file; prints status unapplied presets... morph[1] */
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    char *buf = malloc(SKYRIM_RACEMENU_PRESET_LIMIT + 64);
    size_t n = fread(buf, 1, SKYRIM_RACEMENU_PRESET_LIMIT + 64, f);
    fclose(f);
    SkyrimRaceMenuPreset sentinel, out;
    memset(&sentinel, 0x5a, sizeof sentinel);
    out = sentinel;
    int status = skyrim_racemenu_preset_parse(buf, n, &out);
    if (status && memcmp(&out, &sentinel, sizeof out)) return 3; /* outputs untouched on failure */
    printf("%d", status);
    if (!status) {
        printf(" %u %u %u %u %u %.9g %.9g", out.unapplied, out.presets[0], out.presets[1], out.presets[2], out.presets[3],
               (double)out.morphs[SKYRIM_RACEMENU_NOSE_HEIGHT], (double)out.morphs[18]);
    }
    printf("\n");
    free(buf);
    return 0;
}
