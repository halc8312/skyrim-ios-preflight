/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { SKYRIM_GAME_ENGLISH, SKYRIM_GAME_JAPANESE, SKYRIM_GAME_ARABIC };
enum { SKYRIM_ARABIC_MOD_OFF, SKYRIM_ARABIC_MOD_READY, SKYRIM_ARABIC_MOD_OVERRIDDEN,
       SKYRIM_ARABIC_MOD_NEEDS_INI, SKYRIM_ARABIC_MOD_UNAVAILABLE };
enum { SKYRIM_GAME_PACK_READY, SKYRIM_GAME_PACK_MISSING,
       SKYRIM_GAME_PACK_INCOMPLETE, SKYRIM_GAME_PACK_UNSUPPORTED };
enum { SKYRIM_GAME_MISSING_DEFAULT_INI = 4 }; /* effective-language reason */
enum { SKYRIM_GAME_INI_PROFILE, SKYRIM_GAME_INI_DEFAULT };
typedef enum {
    SKYRIM_PROFILE_INI_ERROR = -1, SKYRIM_PROFILE_INI_DEFERRED,
    SKYRIM_PROFILE_INI_EXISTING, SKYRIM_PROFILE_INI_FROM_DEFAULT
} SkyrimGameIniSource;
typedef struct {
    unsigned text_status, voices_status;
} SkyrimGameLanguagePacks;
typedef struct {
    unsigned requested_text, requested_voices, text, voices;
    unsigned text_reason, voices_reason;
    unsigned arabic_mod;
} SkyrimGameLanguage;

/* Once at startup. Fixed filenames and size-only installed-pack validation;
 * the private installer verifies content hashes and writes the marker last.
 * This is completeness checking, not an authenticity or same-size-corruption
 * guarantee. No archive contents are read and no files are changed here. */
void skyrim_game_language_probe(const char *game_directory, SkyrimGameLanguagePacks *packs);
SkyrimGameLanguage skyrim_game_language_select(unsigned text, unsigned voices,
                                               SkyrimGameLanguagePacks packs, unsigned arabic_mod);
/* Resolve before writing: the guest requests Skyrim.INI with this exact case.
 * Prefer that existing file, otherwise reuse a single case-insensitive match.
 * Return 1 for an existing regular file, 0 for absent (canonical path output),
 * -1 for errors/ambiguous aliases. Never rename, delete or write here. */
int skyrim_game_language_ini_path(const char *directory, unsigned kind,
                                 char *path, size_t capacity);
/* Select the existing profile or its original template. If both are absent,
 * defer explicit Japanese settings and leave the game's initial profile alone.
 * Only the effective selection changes. Call again on the next launch. */
SkyrimGameIniSource skyrim_game_language_plan_profile(const char *game, const char *folder,
    SkyrimGameLanguage *language, char *target, char *defaults, size_t capacity);
/* Transform only the app profile's language keys. Caller owns returned malloc
 * buffer and must publish it atomically before guest threads start. NULL means
 * invalid/oversized input or allocation failure; it is never partial success. */
char *skyrim_game_language_ini(const char *source, size_t bytes,
                             SkyrimGameLanguage language, size_t *out_bytes);
#ifdef __cplusplus
}
#endif
