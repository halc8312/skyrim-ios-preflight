/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { SKYRIM_GAME_ENGLISH, SKYRIM_GAME_JAPANESE };
enum { SKYRIM_GAME_PACK_READY, SKYRIM_GAME_PACK_MISSING,
       SKYRIM_GAME_PACK_INCOMPLETE, SKYRIM_GAME_PACK_UNSUPPORTED };
typedef struct {
    unsigned text_status, voices_status;
} SkyrimGameLanguagePacks;
typedef struct {
    unsigned requested_text, requested_voices, text, voices;
    unsigned text_reason, voices_reason;
} SkyrimGameLanguage;

/* Once at startup. Fixed filenames and size-only installed-pack validation;
 * the private installer verifies content hashes and writes the marker last.
 * This is completeness checking, not an authenticity or same-size-corruption
 * guarantee. No archive contents are read and no files are changed here. */
void skyrim_game_language_probe(const char *game_directory, SkyrimGameLanguagePacks *packs);
SkyrimGameLanguage skyrim_game_language_select(unsigned text, unsigned voices,
                                               SkyrimGameLanguagePacks packs);
/* Transform only the app profile's language keys. Caller owns returned malloc
 * buffer and must publish it atomically before guest threads start. NULL means
 * invalid/oversized input or allocation failure; it is never partial success. */
char *skyrim_game_language_ini(const char *source, size_t bytes,
                             SkyrimGameLanguage language, size_t *out_bytes);
#ifdef __cplusplus
}
#endif
