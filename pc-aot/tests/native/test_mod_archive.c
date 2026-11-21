/* Command driver for tests/test_mod_archive.py; authored archives only. */
#include "mod_archive.h"
#include "mod_deploy.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    if (!strcmp(argv[1], "list") || !strcmp(argv[1], "extract") || !strcmp(argv[1], "root")) {
        SkyrimZip zip;
        int s = skyrim_zip_open(argv[2], &zip);
        printf("status %s\n", skyrim_zip_error_name(s));
        if (s) return 0;
        printf("skipped %zu\n", zip.skipped);
        const char **paths = malloc((zip.count ? zip.count : 1) * sizeof *paths);
        size_t n = 0;
        for (size_t i = 0; i < zip.count; ++i) if (!zip.entries[i].is_dir) paths[n++] = zip.entries[i].path;
        char prefix[1024]; int installer = 0;
        int found = skyrim_mod_data_root(paths, n, prefix, sizeof prefix, &installer);
        if (!strcmp(argv[1], "root")) printf("root %d [%s] installer %d\n", found, prefix, installer);
        if (!strcmp(argv[1], "list")) for (size_t i = 0; i < n; ++i) printf("file %s\n", paths[i]);
        if (!strcmp(argv[1], "extract")) {
            for (size_t i = 0; i < zip.count; ++i) {
                SkyrimZipEntry *e = &zip.entries[i];
                const char *rel = skyrim_archive_relative_path(e->path, prefix);
                if (e->is_dir || !rel) continue;
                int r = skyrim_zip_extract(&zip, i, argv[3], rel);
                printf("extract %s %s\n", rel, skyrim_zip_error_name(r));
            }
        }
        free(paths);
        skyrim_zip_close(&zip);
        return 0;
    }
    if (!strcmp(argv[1], "preset") && argc == 5) {
        printf("preset %d\n", skyrim_mod_stage_racemenu_preset(argv[2], argv[3], argv[4]));
        return 0;
    }
    if (!strcmp(argv[1], "ece-slot") && argc == 5) {
        int profile = open(argv[4], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        unsigned slot = 0;
        int status = profile < 0 ? errno : skyrim_mod_publish_ece_slot(profile, argv[2], argv[3], &slot);
        if (profile >= 0) close(profile);
        printf("ece-slot %d %u\n", status, slot);
        return 0;
    }
    if (!strcmp(argv[1], "matches") && argc == 5) {
        printf("matches %d\n", skyrim_deploy_matches(argv[2], argv[3], argv[4]));
        return 0;
    }
    if (!strcmp(argv[1], "deploy") && argc >= 6) {
        char err[512];
        int r = skyrim_deploy_build(argv[2], (const char *const *)argv + 6, (size_t)(argc - 6), argv[3], argv[4], argv[5], err, sizeof err);
        printf("deploy %d %s matches %d\n", r, err, skyrim_deploy_matches(argv[3], argv[4], argv[5]));
        uint64_t a = 0, b = 0;
        skyrim_tree_signature(argv[2], &a); skyrim_tree_signature(argv[2], &b);
        printf("signature-stable %d\n", a == b && a != 0);
        return 0;
    }
    return 2;
}
