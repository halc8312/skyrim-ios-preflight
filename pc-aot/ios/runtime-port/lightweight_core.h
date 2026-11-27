/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
/* Lightweight mode: portable C, no Apple headers.
 *
 * Opt-in (mode 0 = off is the default and changes nothing). When the game reads
 * one of a fixed list of graphics keys from SkyrimPrefs.ini or Skyrim.ini, a
 * value that is heavier than the mode's limit is replaced by the limit (or, for
 * a few keys, a value that is lighter than the mode's floor is raised to it).
 * Nothing is written to any ini file: when the game writes one of these keys
 * back with the value this module handed out, the write is skipped, so turning
 * the mode off brings back exactly the player's own values.
 *
 * The mode and the exemptions are set once at startup, before the game's
 * threads exist. skyrim_light_read / skyrim_light_write_is_echo may then be
 * called from any thread; their counters use relaxed atomics. */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0 off, 1 light, 2 lighter, 3 maximum. Anything else is treated as 0. Startup only. */
void skyrim_light_set_mode(int mode);
int skyrim_light_mode(void);

/* The player chose this key explicitly in the graphics rows: never touch it.
 * Unknown keys are ignored. Startup only. */
void skyrim_light_exempt(const char *section, const char *key);

/* The game read `key` of `section` from `file` (a Windows or host path; only
 * the base name matters, and only SkyrimPrefs.ini and Skyrim.ini are handled).
 * `current` is the value found in the ini (NULL when absent), `fallback` the
 * default the game passed (may be NULL or empty). Returns buf holding the
 * replacement text, or NULL when nothing changes. Always NULL when the mode is
 * off. A value that does not read as a number is left alone. */
const char *skyrim_light_read(const char *file, const char *section, const char *key,
                              const char *current, const char *fallback, char *buf, size_t size);

/* 1 when the game is writing `key` back with the value skyrim_light_read last
 * handed out for it (numerically equal): the caller must then skip the write.
 * Always 0 when the mode is off. */
int skyrim_light_write_is_echo(const char *file, const char *section, const char *key, const char *value);

/* Prints one `IOS_LIGHT summary ...` line to stderr (see diagnostic_schema.h). */
void skyrim_light_log_summary(void);

#ifdef __cplusplus
}
#endif
