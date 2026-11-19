/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Read-only checks of a MOD package before use. Heuristics, not proofs:
 * a "likely Special Edition" or "uses SKSE" result is a warning shown to the
 * user, never an automatic refusal. Only headers are read. */

enum { SKYRIM_DDS_UNKNOWN, SKYRIM_DDS_BC1, SKYRIM_DDS_BC2, SKYRIM_DDS_BC3, SKYRIM_DDS_BC4,
       SKYRIM_DDS_BC5, SKYRIM_DDS_BC6H, SKYRIM_DDS_BC7, SKYRIM_DDS_RGBA8, SKYRIM_DDS_BGRA8,
       SKYRIM_DDS_BGRX8, SKYRIM_DDS_BGR8, SKYRIM_DDS_OTHER };
typedef struct {
    unsigned width, height, mips, format;
    unsigned dx10;           /* has a DX10 header: unreadable by the classic D3D9 engine */
    unsigned cube_or_volume; /* left untouched by the optimizer */
    size_t data_offset;
} SkyrimDDSInfo;
/* 0 on success. */
int skyrim_dds_info(const unsigned char *bytes, size_t length, SkyrimDDSInfo *out);
/* Bytes of one mip level, 0 for unknown formats. */
size_t skyrim_dds_level_bytes(unsigned format, unsigned width, unsigned height);

/* NIF user version 2 (83 classic, 100 Special Edition), or -1. */
long skyrim_nif_stream_version(const unsigned char *bytes, size_t length);
/* BSA version (103 Oblivion, 104 classic, 105 Special Edition), or -1. */
long skyrim_bsa_version(const unsigned char *bytes, size_t length);

typedef struct {
    unsigned plugins, se_plugins, esl;
    unsigned bsas, se_bsas, foreign_bsas;
    unsigned textures, dx10_textures, bc7_textures, textures_4k, textures_no_mips, textures_unknown;
    unsigned max_texture;
    uint64_t texture_bytes;         /* rough GPU memory, mips included */
    unsigned meshes, se_meshes, foreign_meshes;
    unsigned scripts, skse_scripts, mcm_scripts;
    unsigned skse_dlls;
    char skse_missing[512];         /* SKSE functions this port does not provide */
} SkyrimModReport;
/* Walk a package's Data folder. 0 on success. */
int skyrim_mod_analyze(const char *data_dir, SkyrimModReport *report);

/* Plugins named in a classic .ess save header. *names receives count
 * NUL-terminated names back to back (free with free()). 0 on success. */
int skyrim_save_plugins(const char *path, char **names, unsigned *count);

typedef enum {
    SKYRIM_COSAVE_OK,
    SKYRIM_COSAVE_ABSENT,
    SKYRIM_COSAVE_UNSUPPORTED,
    SKYRIM_COSAVE_TRUNCATED,
    SKYRIM_COSAVE_MALFORMED,
    SKYRIM_COSAVE_UNREADABLE,
    SKYRIM_COSAVE_LIMIT
} SkyrimCoSaveStatus;

/* An inspection budget, not a restriction of the file format. */
enum { SKYRIM_COSAVE_HEADER_LIMIT = 100000 };
typedef struct {
    uint32_t format_version, skse_version, runtime_version;
    uint32_t plugin_count, record_count;   /* available only on OK */
} SkyrimCoSaveInfo;
/* Inspect the classic .skse companion of an .ess in this save directory.
 * Checks container boundaries without interpreting plugin data or changing files. */
SkyrimCoSaveStatus skyrim_save_cosave(const char *directory, const char *ess_name, SkyrimCoSaveInfo *info);
#ifdef __cplusplus
}
#endif
