/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Automatic, package-wide conversion at import. Runs only on the app's own
 * staged copy of a MOD, never on original game files.
 *
 * Textures (one file):
 *  - mip chain larger than max_size: top levels are dropped (no re-encode);
 *  - DX10-header BC1/BC2/BC3 and RGBA8: header rewritten for the classic
 *    D3D9 engine (no re-encode);
 *  - BC7, BC5, BC4 or uncompressed >= 1024 px, or no mips above 256 px:
 *    decoded, reduced to max_size, full mips rebuilt, encoded BC1 (opaque)
 *    or BC3 (alpha). BC5 normal maps get their blue channel reconstructed.
 *  - cube maps, volumes, BC6H and unknown formats are left unchanged.
 * max_size 0 keeps the resolution. */
enum { SKYRIM_TEX_KEPT, SKYRIM_TEX_TRIMMED, SKYRIM_TEX_REHEADERED, SKYRIM_TEX_TRANSCODED,
       SKYRIM_TEX_UNSUPPORTED, SKYRIM_TEX_ERROR };
int skyrim_texture_optimize(const char *path, unsigned max_size);
/* In-memory form for tests: returns a malloc'd new DDS or NULL when kept. */
unsigned char *skyrim_texture_convert(const unsigned char *dds, size_t length, unsigned max_size,
                                      size_t *out_length, int *action);

/* Special Edition BSA (v105, LZ4) to loose files under data_dir. Existing
 * loose files win (they would override the archive in the game too). The
 * archive itself is left for the caller to delete. 0 on success. */
int skyrim_bsa_extract_se(const char *bsa_path, const char *data_dir, unsigned *files);
/* LZ4 frame decoder used above; returns decoded bytes or -1. */
long skyrim_lz4_frame_decode(const unsigned char *src, size_t n, unsigned char *dst, size_t cap);
#ifdef __cplusplus
}
#endif
