/* Command driver for tests/test_mod_libarchive.py; authored archives only. */
#include "mod_archive.h"
#include "mod_deploy.h"
#include "mod_libarchive.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    SkyrimArchiveList list;
    int s = skyrim_archive_list(argv[2], &list);
    printf("status %s count %zu\n", skyrim_zip_error_name(s), s ? 0 : list.count);
    if (s) return 0;
    char prefix[1024]; int installer = 0;
    int found = skyrim_mod_data_root((const char *const *)list.paths, list.count, prefix, sizeof prefix, &installer);
    printf("root %d [%s] installer %d\n", found, prefix, installer);
    skyrim_archive_list_free(&list);
    if (!strcmp(argv[1], "extract") && argc == 4) {
        size_t files; uint64_t bytes;
        int r = skyrim_archive_extract(argv[2], prefix, 1, argv[3], &files, &bytes);
        printf("extract %s files %zu bytes %llu\n", skyrim_zip_error_name(r), files, (unsigned long long)bytes);
    }
    return 0;
}
