/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_fuz.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* argv[1] "parse" <text> | "read" roots... ; prints words skip issues found */
int main(int argc, char **argv) {
    SkyrimFuzSettings s;
    assert(!strcmp(skyrim_fuz_stock_path(1), "Sound/Voice/Fuz Ro Doh/Stock_1.xwm"));
    assert(!strcmp(skyrim_fuz_stock_path(10), "Sound/Voice/Fuz Ro Doh/Stock_10.xwm"));
    assert(!skyrim_fuz_stock_path(0) && !skyrim_fuz_stock_path(11));
    assert(skyrim_fuz_read_settings(NULL, 1, &s) && skyrim_fuz_read_settings(NULL, 0, NULL));
    assert(argc >= 2);
    if (!strcmp(argv[1], "parse")) {
        size_t length = argc > 3 ? (size_t)atoi(argv[3]) : strlen(argv[2]);
        skyrim_fuz_parse_settings(argv[2], length, &s);
    } else {
        assert(!skyrim_fuz_read_settings((const char *const *)argv + 2, (size_t)(argc - 2), &s));
    }
    printf("%d %d %u %d\n", s.words_per_second, s.skip_empty_responses, s.issues, s.found_file);
    return 0;
}
