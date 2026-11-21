/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_X86_WINTEXT_H
#define SKYRIM_EXPERIMENT_SKSE_X86_WINTEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    SKSE_WIN_CP_ACP = 0, SKSE_WIN_CP_1252 = 1252, SKSE_WIN_CP_UTF8 = 65001,
    SKSE_WIN_TEXT_NOT_SUPPORTED = 50, SKSE_WIN_TEXT_INVALID_PARAMETER = 87,
    SKSE_WIN_TEXT_INSUFFICIENT_BUFFER = 122, SKSE_WIN_TEXT_INVALID_FLAGS = 1004,
    SKSE_WIN_TEXT_NO_UNICODE_TRANSLATION = 1113,
    SKSE_WIN_GUEST_DEFAULT_LCID = 0x0409, SKSE_WIN_LOCALE_INVARIANT = 0x007f
};

/*
 * The caller validates and charges work for the byte ranges. Counts are
 * positive for the source and exact, including a terminator if desired.
 * A zero destination capacity queries the number of output units.
 * A nonempty destination must be disjoint from the source and controls.
 * The guest ACP is fixed to 1252. UTF-8 requires both optional controls to
 * be null. Wide-to-1252 supports direct and best-fit mappings without
 * WC_COMPOSITECHECK.
 *
 * The return value is a Win32 error code (zero on success). Written is
 * measured in destination units and set to zero on errors. A conversion
 * error can leave an output prefix and report whether fallback was used,
 * as in Win32. Parameter and unsupported-feature errors leave the
 * destination and optional fallback report untouched.
 */
uint16_t skse_win_decode_cp1252(uint8_t byte);
uint32_t skse_win_wide_to_multi(uint32_t code_page, uint32_t flags,
                              const uint8_t *utf16le, size_t source_units,
                              uint8_t *bytes, size_t capacity_bytes,
                              const uint8_t *default_byte, bool *used_default,
                              size_t *written);
uint32_t skse_win_multi_to_wide(uint32_t code_page, uint32_t flags,
                              const uint8_t *bytes, size_t source_bytes,
                              uint8_t *utf16le, size_t capacity_units,
                              size_t *written);

/* Classify one UTF-16 unit at a time as Windows character type 1, 2 or 3.
 * The caller validates disjoint ranges of source_units * 2 bytes and
 * includes the terminator when it is part of the input. Latin-1, the
 * additional Windows-1252 characters and surrogate code units use the
 * reference Windows classifications. Other units return NOT_SUPPORTED.
 * The result is one little-endian 16-bit value per source unit. */
uint32_t skse_win_string_type(uint32_t info_type, const uint8_t *utf16le, size_t source_units,
                              uint8_t *types_le);

/* Query immutable en-US or invariant locale metadata. Neutral, user and
 * system-default pseudo LCIDs resolve to en-US for this query. Output is
 * UTF-16LE with a terminator, or one LE DWORD for LOCALE_RETURN_NUMBER.
 * The caller validates the writable range. Capacity and written count
 * 16-bit units (also for numbers). Capacity zero is a size query.
 * Unsupported fields and locales return NOT_SUPPORTED. Other errors and
 * short buffers leave the destination untouched and written zero. */
uint32_t skse_win_locale_info(uint32_t lcid, uint32_t locale_type,
                              uint8_t *output_le, size_t capacity_units, size_t *written);

/* IsValidLocale queries the explicit guest locale catalog, preserving
 * the caller's last error. The two locales accept flags 0, 1 and 2. */
bool skse_win_is_valid_locale(uint32_t lcid, uint32_t flags);

/* Bounded CompareStringW for en-US and invariant, with the same default
 * aliases as skse_win_locale_info. Supports the printable ASCII repertoire
 * and ignored NUL units. Hyphen and apostrophe require both SORT_STRINGSORT
 * and NORM_IGNORECASE because their other collation modes use extra weights.
 * Zero, either flag, and their combination are valid.
 * Counts may be zero, and both pointers must be valid even for empty
 * strings. For negative Win32 counts, the caller measures the text before
 * the first NUL. On success, relation is CSTR_LESS_THAN, CSTR_EQUAL, or
 * CSTR_GREATER_THAN. On failure it is zero. */
uint32_t skse_win_compare_string(uint32_t lcid, uint32_t flags,
                                  const uint8_t *left_le, size_t left_units,
                                  const uint8_t *right_le, size_t right_units, uint32_t *relation);

/* Bounded LCMapStringW casing for the same locales. Supports Latin-1 and
 * Windows-1252 units with LCMAP_LOWERCASE or LCMAP_UPPERCASE and optional
 * LCMAP_LINGUISTIC_CASING. Every supported input maps to one UTF-16 unit.
 * The caller validates the byte ranges, including a terminator for
 * negative Win32 source counts. Capacity zero queries the size. The
 * destination can equal the source, or the ranges must be disjoint.
 * The output is unchanged and written is zero on failure. Sort keys and
 * other mapping modes return NOT_SUPPORTED. */
uint32_t skse_win_map_string(uint32_t lcid, uint32_t flags,
                              const uint8_t *source_le, size_t source_units,
                              uint8_t *output_le, size_t capacity_units, size_t *written);
#endif
