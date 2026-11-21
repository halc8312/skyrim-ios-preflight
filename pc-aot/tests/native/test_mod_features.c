/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_features.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

/* argv: root... ; prints "unreadable key:providers:winner ..." */
int main(int argc, char **argv) {
    assert(skyrim_feature_from_key("fuz") == SKYRIM_FEATURE_FUZ && skyrim_feature_from_key("nope") < 0);
    assert(skyrim_feature_from_key(NULL) < 0 && !skyrim_feature_key(SKYRIM_FEATURE_COUNT));
    for (int i = 0; i < SKYRIM_FEATURE_COUNT; ++i) assert(skyrim_feature_from_key(skyrim_feature_key(i)) == i);
    SkyrimFeatureScan scan;
    assert(skyrim_features_scan(NULL, 1, &scan) && skyrim_features_scan(NULL, 0, NULL));
    assert(!skyrim_features_scan(NULL, 0, &scan) && !scan.features[0].providers && scan.features[0].winner == -1);
    assert(!skyrim_features_scan((const char *const *)argv + 1, (size_t)(argc - 1), &scan));
    printf("%u", scan.unreadable_roots);
    for (int i = 0; i < SKYRIM_FEATURE_COUNT; ++i)
        if (scan.features[i].providers) printf(" %s:%u:%d", skyrim_feature_key(i), scan.features[i].providers, scan.features[i].winner);
    printf("\n");
    /* FEATURE_VERSION=<key>: the DLL version in each root. */
    const char *key = getenv("FEATURE_VERSION");
    if (key) {
        const int feature = skyrim_feature_from_key(key);
        uint16_t v[4];
        assert(feature >= 0 && skyrim_feature_dll_version(argv[1], (unsigned)feature, NULL) == EINVAL);
        for (int i = 1; i < argc; ++i) {
            const int rc = skyrim_feature_dll_version(argv[i], (unsigned)feature, v);
            printf("%d:%u.%u.%u.%u\n", rc, v[0], v[1], v[2], v[3]);
        }
    }
    return 0;
}
