/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_features.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* argv: root... ; prints "unreadable key:providers:winner ..." */
int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "--nioverride")) {
        const char *const *roots = (const char *const *)argv + 2;
        SkyrimFeatureScan scan;
        assert(!skyrim_features_scan(roots, (size_t)(argc - 2), &scan));
        SkyrimFeatureState ni = scan.features[SKYRIM_FEATURE_NIOVERRIDE];
        printf("%u %d", ni.providers, ni.winner);
        uint16_t version[4];
        if (ni.winner >= 0 && skyrim_nioverride_file_version(roots[ni.winner], version))
            printf(" %u.%u.%u.%u", (unsigned)version[0], (unsigned)version[1],
                   (unsigned)version[2], (unsigned)version[3]);
        puts("");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--precache-path")) {
        char path[4096];
        int found = skyrim_precache_killer_path(argv[2], path, sizeof path);
        printf("%d\n", found);
        if (found > 0) puts(path);
        return 0;
    }
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
    return 0;
}
