/* Command driver for tests/test_mod_optimize.py; authored files only. */
#include "mod_analyze.h"
#include "mod_optimize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    long size = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    unsigned char *bytes = malloc((size_t)size + 1);
    if (bytes && fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        free(bytes);
        bytes = NULL;
    }
    fclose(file);
    *length = (size_t)size;
    return bytes;
}
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    if (!strcmp(argv[1], "convert") && argc == 5) {
        size_t n, on = 0; int action;
        unsigned char *b = slurp(argv[2], &n);
        if (!b) return 2;
        unsigned char *o = skyrim_texture_convert(b, n, (unsigned)atoi(argv[4]), &on, &action);
        printf("action %d\n", action);
        if (o) {
            FILE *f = fopen(argv[3], "wb"); fwrite(o, 1, on, f); fclose(f);
            SkyrimDDSInfo info; skyrim_dds_info(o, on, &info);
            size_t chain = 0;
            for (unsigned i = 0; i < info.mips; ++i) {
                unsigned w = info.width >> i, h = info.height >> i;
                chain += skyrim_dds_level_bytes(info.format, w ? w : 1, h ? h : 1);
            }
            printf("out %ux%u mips %u format %u dx10 %u exact %d\n", info.width, info.height, info.mips, info.format, info.dx10, 128 + chain == on);
            free(o);
        }
        free(b);
        return 0;
    }
    if (!strcmp(argv[1], "file") && argc == 4) {
        printf("action %d\n", skyrim_texture_optimize(argv[2], (unsigned)atoi(argv[3])));
        return 0;
    }
    if (!strcmp(argv[1], "bsa") && argc == 4) {
        unsigned files = 0;
        int r = skyrim_bsa_extract_se(argv[2], argv[3], &files);
        printf("bsa %d files %u\n", r, files);
        return 0;
    }
    if (!strcmp(argv[1], "analyze")) {
        SkyrimModReport r;
        skyrim_mod_analyze(argv[2], &r);
        printf("plugins %u se %u esl %u bsas %u se_bsas %u textures %u dx10 %u bc7 %u 4k %u nomips %u max %u meshes %u se_meshes %u scripts %u skse %u mcm %u dlls %u bytes %llu\nmissing [%s]\n",
               r.plugins, r.se_plugins, r.esl, r.bsas, r.se_bsas, r.textures, r.dx10_textures, r.bc7_textures, r.textures_4k,
               r.textures_no_mips, r.max_texture, r.meshes, r.se_meshes, r.scripts, r.skse_scripts, r.mcm_scripts, r.skse_dlls,
               (unsigned long long)r.texture_bytes, r.skse_missing);
        return 0;
    }
    if (!strcmp(argv[1], "save")) {
        char *names; unsigned count;
        int r = skyrim_save_plugins(argv[2], &names, &count);
        printf("save %d count %u\n", r, count);
        for (unsigned i = 0, o = 0; i < count; ++i) { printf("plugin %s\n", names + o); o += (unsigned)strlen(names + o) + 1; }
        free(names);
        return 0;
    }
    return 2;
}
