/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_wintext.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static void test_utf8(void) {
    const uint8_t utf8[] = {'A', 0, 0xc3, 0xa4, 0xf0, 0x9f, 0x98, 0x83};
    const uint8_t utf16le[] = {'A', 0, 0, 0, 0xe4, 0, 0x3d, 0xd8, 0x03, 0xde};
    uint8_t out[20];
    memset(out, 0xcc, sizeof out);
    size_t count;
    assert(!skse_win_multi_to_wide(SKSE_WIN_CP_UTF8, 0, utf8, sizeof utf8, NULL, 0, &count));
    assert(count == sizeof utf16le / 2);
    assert(!skse_win_multi_to_wide(SKSE_WIN_CP_UTF8, 0, utf8, sizeof utf8, out, 5, &count));
    assert(count == 5 && !memcmp(out, utf16le, sizeof utf16le) && out[10] == 0xcc);
    assert(!skse_win_wide_to_multi(SKSE_WIN_CP_UTF8, 0, utf16le, 5, NULL, 0, NULL, NULL, &count));
    assert(count == sizeof utf8);
    assert(!skse_win_wide_to_multi(SKSE_WIN_CP_UTF8, 0, utf16le, 5, out, sizeof out, NULL, NULL, &count));
    assert(count == sizeof utf8 && !memcmp(out, utf8, sizeof utf8));

    const uint8_t invalid[] = {0xe0, 0x80, 0x80, 'a', 0xc2, 0xe2, 0x82};
    const uint8_t repaired[] = {0xfd, 0xff, 0xfd, 0xff, 'a', 0, 0xfd, 0xff, 0xfd, 0xff};
    assert(skse_win_multi_to_wide(SKSE_WIN_CP_UTF8, 8, invalid, sizeof invalid, NULL, 0, &count) ==
           SKSE_WIN_TEXT_NO_UNICODE_TRANSLATION && count == 0);
    assert(!skse_win_multi_to_wide(SKSE_WIN_CP_UTF8, 0, invalid, sizeof invalid, out, 5, &count));
    assert(count == 5 && !memcmp(out, repaired, sizeof repaired));
    memset(out, 0xcc, sizeof out);
    assert(skse_win_multi_to_wide(SKSE_WIN_CP_UTF8, 0, utf8, sizeof utf8, out, 4, &count) ==
           SKSE_WIN_TEXT_INSUFFICIENT_BUFFER && count == 0);
    assert(!memcmp(out, utf16le, 8) && out[8] == 0xcc);
    memset(out, 0xcc, sizeof out);
    assert(skse_win_wide_to_multi(SKSE_WIN_CP_UTF8, 0, utf16le, 5, out, 7, NULL, NULL, &count) ==
           SKSE_WIN_TEXT_INSUFFICIENT_BUFFER && count == 0);
    assert(!memcmp(out, utf8, 4) && out[4] == 0xcc);

    const uint8_t unpaired[] = {0x00, 0xd8, 'a', 0, 0x00, 0xdc};
    const uint8_t expected[] = {0xef, 0xbf, 0xbd, 'a', 0xef, 0xbf, 0xbd};
    assert(skse_win_wide_to_multi(SKSE_WIN_CP_UTF8, 0x80, unpaired, 3, NULL, 0, NULL, NULL, &count) ==
           SKSE_WIN_TEXT_NO_UNICODE_TRANSLATION && count == 0);
    assert(!skse_win_wide_to_multi(SKSE_WIN_CP_UTF8, 0, unpaired, 3, out, sizeof out, NULL, NULL, &count));
    assert(count == sizeof expected && !memcmp(out, expected, sizeof expected));
}

static void test_cp1252(void) {
    const uint8_t source[] = {0, 0x81, 0xc4, 0x80, 0xff};
    const uint8_t precomposed[] = {0, 0, 0x81, 0, 0xc4, 0, 0xac, 0x20, 0xff, 0};
    const uint8_t decomposed[] = {0, 0, 0x81, 0, 'A', 0, 0x08, 0x03, 0xac, 0x20, 'y', 0, 0x08, 0x03};
    uint8_t out[32];
    size_t count;
    assert(!skse_win_multi_to_wide(SKSE_WIN_CP_ACP, 0, source, sizeof source, out, 16, &count));
    assert(count == 5 && !memcmp(out, precomposed, sizeof precomposed));
    assert(!skse_win_multi_to_wide(SKSE_WIN_CP_1252, 2, source, sizeof source, out, 16, &count));
    assert(count == 7 && !memcmp(out, decomposed, sizeof decomposed));
    assert(!skse_win_multi_to_wide(SKSE_WIN_CP_1252, 2, source, sizeof source, out, 6, &count));
    assert(count == 6 && !memcmp(out, decomposed, 12));
    bool used = true;
    assert(!skse_win_wide_to_multi(SKSE_WIN_CP_1252, 0x400, precomposed, 5, out, 16, NULL, &used, &count));
    assert(count == sizeof source && !used && !memcmp(out, source, sizeof source));

    const uint8_t find_best_fit[] = {0x1e, 0x22, 0x21, 0xff, 0x03, 0x26, 0x3d, 0xd8, 0x03, 0xde};
    const uint8_t fallback = '@';
    assert(!skse_win_wide_to_multi(SKSE_WIN_CP_ACP, 0, find_best_fit, 5, out, 16, &fallback, &used, &count));
    assert(count == 5 && used && !memcmp(out, "8A@@@", 5));
    assert(!skse_win_wide_to_multi(SKSE_WIN_CP_1252, 0x400, find_best_fit, 5, out, 16, &fallback, &used, &count));
    assert(count == 5 && used && !memcmp(out, "@@@@@", 5));
    used = false;
    assert(!skse_win_wide_to_multi(SKSE_WIN_CP_1252, 0, find_best_fit, 5, NULL, 0, &fallback, &used, &count));
    assert(count == 5 && used);
    assert(skse_win_wide_to_multi(SKSE_WIN_CP_1252, 0, find_best_fit, 5, out, 2, &fallback, &used, &count) ==
           SKSE_WIN_TEXT_INSUFFICIENT_BUFFER && count == 0 && !used && !memcmp(out, "8A", 2));
    assert(skse_win_wide_to_multi(SKSE_WIN_CP_1252, 0, find_best_fit, 5, out, 3, &fallback, &used, &count) ==
           SKSE_WIN_TEXT_INSUFFICIENT_BUFFER && count == 0 && used && !memcmp(out, "8A@", 3));
}

static void test_arguments(void) {
    const uint8_t input[] = {'x', 0};
    uint8_t out[] = {0xcc, 0xcc};
    bool used = true;
    size_t count = 777;
    assert(skse_win_wide_to_multi(SKSE_WIN_CP_UTF8, 0, input, 0, out, 1, NULL, NULL, &count) ==
           SKSE_WIN_TEXT_INVALID_PARAMETER && count == 0);
    assert(skse_win_wide_to_multi(SKSE_WIN_CP_UTF8, 0, input, 1, out, 1, NULL, &used, &count) ==
           SKSE_WIN_TEXT_INVALID_PARAMETER && count == 0);
    assert(skse_win_wide_to_multi(SKSE_WIN_CP_UTF8, 1, input, 1, out, 1, NULL, NULL, &count) ==
           SKSE_WIN_TEXT_INVALID_FLAGS && count == 0);
    assert(skse_win_wide_to_multi(SKSE_WIN_CP_1252, 0x200, input, 1, out, 1, NULL, &used, &count) ==
           SKSE_WIN_TEXT_NOT_SUPPORTED && count == 0);
    assert(skse_win_wide_to_multi(SKSE_WIN_CP_1252, 0x80, input, 1, out, 1, NULL, &used, &count) ==
           SKSE_WIN_TEXT_INVALID_FLAGS && count == 0);
    assert(skse_win_wide_to_multi(932, 0, input, 1, out, 1, NULL, &used, &count) ==
           SKSE_WIN_TEXT_NOT_SUPPORTED && count == 0);
    assert(skse_win_multi_to_wide(SKSE_WIN_CP_1252, 3, input, 1, out, 1, &count) ==
           SKSE_WIN_TEXT_INVALID_FLAGS && count == 0);
    assert(skse_win_multi_to_wide(SKSE_WIN_CP_UTF8, 0, input, 1, NULL, 1, &count) ==
           SKSE_WIN_TEXT_INVALID_PARAMETER && count == 0);
    assert(skse_win_multi_to_wide(SKSE_WIN_CP_UTF8, 0, out, 1, out, 1, &count) ==
           SKSE_WIN_TEXT_INVALID_PARAMETER && count == 0);
    assert(out[0] == 0xcc && out[1] == 0xcc && used);
}

static void test_string_type(void) {
    const uint8_t source[] = {'A', 0, 0, 0, 0xac, 0x20, 0x3d, 0xd8, 0x00, 0xde};
    const uint8_t ctype1[] = {0x81, 3, 0x20, 2, 0x00, 2, 0, 2, 0, 2};
    const uint8_t ctype2[] = {1, 0, 0, 0, 5, 0, 1, 0, 1, 0};
    const uint8_t ctype3[] = {0x40, 0x80, 0, 0, 8, 0, 0, 8, 0, 0x10};
    uint8_t types[12];
    memset(types, 0xcc, sizeof types);
    assert(!skse_win_string_type(1, source, 5, types));
    assert(!memcmp(types, ctype1, sizeof ctype1) && types[10] == 0xcc);
    assert(!skse_win_string_type(2, source, 5, types));
    assert(!memcmp(types, ctype2, sizeof ctype2) && types[10] == 0xcc);
    assert(!skse_win_string_type(4, source, 5, types));
    assert(!memcmp(types, ctype3, sizeof ctype3) && types[10] == 0xcc);
    assert(skse_win_string_type(3, source, 1, types) == SKSE_WIN_TEXT_INVALID_FLAGS);
    assert(skse_win_string_type(3, source, 0, types) == SKSE_WIN_TEXT_INVALID_PARAMETER);
    assert(skse_win_string_type(1, types, 1, types) == SKSE_WIN_TEXT_INVALID_PARAMETER);
    assert(skse_win_string_type(1, NULL, 1, types) == SKSE_WIN_TEXT_INVALID_PARAMETER);
    assert(skse_win_string_type(1, source, 1, NULL) == SKSE_WIN_TEXT_INVALID_PARAMETER);
    assert(!memcmp(types, ctype3, sizeof ctype3));

    const uint8_t unsupported[] = {'A', 0, 0x42, 0x30};
    assert(skse_win_string_type(1, unsupported, 2, types) == SKSE_WIN_TEXT_NOT_SUPPORTED);
    assert(!memcmp(types, ctype3, sizeof ctype3));
}

static void test_locale_info(void) {
    const uint32_t required_number = UINT32_C(0x20000000);
    uint8_t output[24];
    size_t units = 0;
    memset(output, 0xcc, sizeof output);
    assert(!skse_win_locale_info(SKSE_WIN_GUEST_DEFAULT_LCID, 0x5c, NULL, 0, &units) && units == 6);
    assert(!skse_win_locale_info(SKSE_WIN_LOCALE_INVARIANT, 0x5c, output, 1, &units) &&
           units == 1 && output[0] == 0 && output[1] == 0 && output[2] == 0xcc);
    assert(!skse_win_locale_info(SKSE_WIN_LOCALE_INVARIANT, required_number | 0x1004, NULL, 0, &units) &&
           units == 2);
    const uint8_t acp[] = {0xe4, 0x04, 0, 0};
    assert(!skse_win_locale_info(SKSE_WIN_LOCALE_INVARIANT, required_number | 0x1004, output, 2, &units) &&
           units == 2 && !memcmp(output, acp, sizeof acp) && output[4] == 0xcc);
    const uint8_t id[] = {0x09, 0x04, 0, 0};
    assert(!skse_win_locale_info(SKSE_WIN_GUEST_DEFAULT_LCID, required_number | 1, output, 2, &units) &&
           units == 2 && !memcmp(output, id, sizeof id));
    const uint8_t currency[] = {0xa4, 0, 0, 0};
    assert(!skse_win_locale_info(SKSE_WIN_LOCALE_INVARIANT, 0x14, output, 2, &units) &&
           units == 2 && !memcmp(output, currency, sizeof currency));

    const uint8_t name[] = {'e', 0, 'n', 0, '-', 0, 'U', 0, 'S', 0, 0, 0};
    const uint32_t aliases[] = {0, 0x0400, 0x0800};
    for (unsigned i = 0; i < sizeof aliases / sizeof aliases[0]; ++i) {
        assert(!skse_win_locale_info(aliases[i], UINT32_C(0x8000005c), output, 6, &units) &&
               units == 6 && !memcmp(output, name, sizeof name));
        assert(!skse_win_is_valid_locale(aliases[i], 1));
    }
    memset(output, 0xcc, sizeof output);
    assert(skse_win_locale_info(SKSE_WIN_GUEST_DEFAULT_LCID, 0x5c, output, 5, &units) ==
           SKSE_WIN_TEXT_INSUFFICIENT_BUFFER && units == 0);
    assert(skse_win_locale_info(SKSE_WIN_LOCALE_INVARIANT, required_number | 0x1004, output, 1, &units) ==
           SKSE_WIN_TEXT_INSUFFICIENT_BUFFER && units == 0);
    assert(skse_win_locale_info(SKSE_WIN_GUEST_DEFAULT_LCID, required_number | 0x5c, output, 2, &units) ==
           SKSE_WIN_TEXT_INVALID_FLAGS && units == 0);
    assert(skse_win_locale_info(SKSE_WIN_GUEST_DEFAULT_LCID, UINT32_C(0x0800005c), output, 2, &units) ==
           SKSE_WIN_TEXT_INVALID_FLAGS && units == 0);
    assert(skse_win_locale_info(SKSE_WIN_GUEST_DEFAULT_LCID, 0x5c, NULL, 6, &units) ==
           SKSE_WIN_TEXT_INVALID_PARAMETER && units == 0);
    assert(skse_win_locale_info(UINT32_MAX, 0x5c, output, 6, &units) ==
           SKSE_WIN_TEXT_INVALID_PARAMETER && units == 0);
    assert(skse_win_locale_info(0x0411, 0x5c, output, 6, &units) ==
           SKSE_WIN_TEXT_NOT_SUPPORTED && units == 0);
    assert(skse_win_locale_info(SKSE_WIN_GUEST_DEFAULT_LCID, 0x58, output, 6, &units) ==
           SKSE_WIN_TEXT_NOT_SUPPORTED && units == 0);
    assert(skse_win_locale_info(SKSE_WIN_GUEST_DEFAULT_LCID, 2, output, 6, &units) ==
           SKSE_WIN_TEXT_NOT_SUPPORTED && units == 0);
    for (unsigned i = 0; i < sizeof output; ++i) assert(output[i] == 0xcc);
    assert(skse_win_is_valid_locale(SKSE_WIN_GUEST_DEFAULT_LCID, 0));
    assert(skse_win_is_valid_locale(SKSE_WIN_LOCALE_INVARIANT, 2));
    assert(!skse_win_is_valid_locale(SKSE_WIN_GUEST_DEFAULT_LCID, 3));
    assert(!skse_win_is_valid_locale(SKSE_WIN_GUEST_DEFAULT_LCID, 0x39));
    assert(!skse_win_is_valid_locale(0x0411, 2));
}

static void test_string_mapping(void) {
    const uint8_t source[] = {'a', 0, 0, 0, 0xe0, 0, 0xdf, 0, 0xff, 0, 0x53, 1,
                              0x7d, 1, 0xb5, 0, 0x92, 1, 0x22, 0x21};
    const uint8_t upper[] = {'A', 0, 0, 0, 0xc0, 0, 0xdf, 0, 0x78, 1, 0x52, 1,
                             0x7d, 1, 0xb5, 0, 0x91, 1, 0x22, 0x21};
    const size_t count = sizeof source / 2;
    uint8_t output[sizeof source + 2];
    size_t written;
    memset(output, 0xcc, sizeof output);
    assert(!skse_win_map_string(0, 0x200, source, count, NULL, 0, &written) && written == count);
    assert(skse_win_map_string(0, 0x200, source, count, output, count - 1, &written) ==
           SKSE_WIN_TEXT_INSUFFICIENT_BUFFER && written == 0);
    for (size_t i = 0; i < sizeof output; ++i) assert(output[i] == 0xcc);
    assert(!skse_win_map_string(0, 0x200, source, count, output, count + 1, &written));
    assert(written == count && !memcmp(output, upper, sizeof upper) && output[sizeof upper] == 0xcc);

    memcpy(output, source, sizeof source);
    assert(!skse_win_map_string(SKSE_WIN_LOCALE_INVARIANT, 0x01000200, output, count, output, count, &written));
    assert(written == count && output[14] == 0x9c && output[15] == 3);
    assert(!memcmp(output, upper, 14) && !memcmp(output + 16, upper + 16, sizeof upper - 16));
    assert(!skse_win_map_string(0x0800, 0x100, source, count, output, count, &written));
    assert(written == count && output[12] == 0x7e && output[13] == 1);
    assert(!memcmp(output, source, 12) && !memcmp(output + 14, source + 14, sizeof source - 14));

    const uint8_t outside[] = {'A', 0, 0x42, 0x30};
    memset(output, 0xcc, sizeof output);
    assert(skse_win_map_string(0, 0x100, outside, 2, output, 2, &written) ==
           SKSE_WIN_TEXT_NOT_SUPPORTED && written == 0);
    assert(skse_win_map_string(0, 0x100, outside, 2, NULL, 0, &written) ==
           SKSE_WIN_TEXT_NOT_SUPPORTED && written == 0);
    assert(skse_win_map_string(0, 0x400, source, count, NULL, 0, &written) ==
           SKSE_WIN_TEXT_NOT_SUPPORTED && written == 0);
    assert(skse_win_map_string(0x041f, 0x100, source, count, output, count, &written) ==
           SKSE_WIN_TEXT_NOT_SUPPORTED && written == 0);
    assert(skse_win_map_string(0, 0, source, count, output, count, &written) ==
           SKSE_WIN_TEXT_INVALID_FLAGS && written == 0);
    assert(skse_win_map_string(0, 0x01000000, source, count, output, count, &written) ==
           SKSE_WIN_TEXT_INVALID_FLAGS && written == 0);
    assert(skse_win_map_string(0, 0x100, source, 0, output, count, &written) ==
           SKSE_WIN_TEXT_INVALID_PARAMETER && written == 0);
    assert(skse_win_map_string(0, 0x100, source, count, NULL, count, &written) ==
           SKSE_WIN_TEXT_INVALID_PARAMETER && written == 0);
    for (size_t i = 0; i < sizeof output; ++i) assert(output[i] == 0xcc);
}

static void test_string_comparison(void) {
    const uint8_t az[] = {'a', 0, 0, 0, 'Z', 0};
    const uint8_t aa[] = {'A', 0, 'a', 0};
    const uint8_t a_lower[] = {'a', 0, 'z', 0};
    const uint8_t punctuation[] = {'?', 0, '<', 0, '-', 0, ' ', 0};
    uint32_t relation;
    assert(!skse_win_compare_string(0, 0, az, 3, aa, 2, &relation) && relation == 3);
    assert(!skse_win_compare_string(0x0400, 0, az, 3, a_lower, 2, &relation) && relation == 3);
    assert(!skse_win_compare_string(0x0800, 1, az, 3, a_lower, 2, &relation) && relation == 2);
    assert(!skse_win_compare_string(0, 0, az, 0, aa, 0, &relation) && relation == 2);
    assert(!skse_win_compare_string(0, 0, az + 2, 1, aa, 0, &relation) && relation == 2);
    assert(!skse_win_compare_string(0, 0, az, 1, aa, 1, &relation) && relation == 1);
    assert(!skse_win_compare_string(SKSE_WIN_LOCALE_INVARIANT, 0, punctuation, 1,
                                    punctuation + 2, 1, &relation) && relation == 1);
    assert(skse_win_compare_string(0, 0, punctuation + 4, 1, a_lower, 2, &relation) ==
           SKSE_WIN_TEXT_NOT_SUPPORTED && relation == 0);
    assert(skse_win_compare_string(0, 0x1000, punctuation + 4, 1, a_lower, 2, &relation) ==
           SKSE_WIN_TEXT_NOT_SUPPORTED && relation == 0);
    assert(!skse_win_compare_string(0, 0x1001, punctuation + 4, 1, punctuation + 6, 1, &relation) &&
           relation == 1);
    const uint8_t outside[] = {'z', 0, 0xdf, 0};
    assert(skse_win_compare_string(0, 0, az, 3, outside, 2, &relation) ==
           SKSE_WIN_TEXT_NOT_SUPPORTED && relation == 0);
    assert(skse_win_compare_string(0, 0x8, az, 3, aa, 2, &relation) ==
           SKSE_WIN_TEXT_NOT_SUPPORTED && relation == 0);
    assert(skse_win_compare_string(0, 0x80000000, az, 3, aa, 2, &relation) ==
           SKSE_WIN_TEXT_INVALID_FLAGS && relation == 0);
    assert(skse_win_compare_string(0x0411, 0, az, 3, aa, 2, &relation) ==
           SKSE_WIN_TEXT_NOT_SUPPORTED && relation == 0);
    assert(skse_win_compare_string(0, 0, NULL, 0, aa, 2, &relation) ==
           SKSE_WIN_TEXT_INVALID_PARAMETER && relation == 0);
}

#ifdef _WIN32
static void compare_encode(uint32_t code_page, uint32_t flags, const WCHAR *source,
                           int length, int capacity) {
    const char fallback = '@';
    BOOL win_used = 7;
    uint8_t win_out[64], guest_out[64];
    memset(win_out, 0xcc, sizeof win_out);
    memset(guest_out, 0xcc, sizeof guest_out);
    const bool utf8 = code_page == SKSE_WIN_CP_UTF8;
    SetLastError(777);
    int win_count = WideCharToMultiByte(code_page, flags, source, length, (char *)win_out, capacity,
                                       utf8 ? NULL : &fallback, utf8 ? NULL : &win_used);
    uint32_t win_status = win_count ? 0 : GetLastError();
    if (win_count) assert(GetLastError() == 777);
    size_t guest_count;
    bool guest_used = false;
    uint32_t guest_status = skse_win_wide_to_multi(code_page, flags, (const uint8_t *)source, (size_t)length,
                                                 guest_out, (size_t)capacity, utf8 ? NULL : (const uint8_t *)&fallback,
                                                 utf8 ? NULL : &guest_used, &guest_count);
    if (win_status != guest_status || (size_t)win_count != guest_count ||
        (!utf8 && (win_used != 0) != guest_used) || memcmp(win_out, guest_out, sizeof win_out)) {
        fprintf(stderr, "encode mismatch page=%u flags=%u capacity=%d win=%u/%d guest=%u/%zu;",
                code_page, flags, capacity, win_status, win_count, guest_status, guest_count);
        for (int i = 0; i < length; ++i) fprintf(stderr, " %04x", (unsigned)source[i]);
        fputc('\n', stderr);
        assert(false);
    }
}

static void compare_decode(uint32_t code_page, uint32_t flags, const uint8_t *source,
                           int length, int capacity) {
    WCHAR win_out[600], guest_out[600];
    memset(win_out, 0xcc, sizeof win_out);
    memset(guest_out, 0xcc, sizeof guest_out);
    SetLastError(777);
    int win_count = MultiByteToWideChar(code_page, flags, (const char *)source, length, win_out, capacity);
    uint32_t win_status = win_count ? 0 : GetLastError();
    if (win_count) assert(GetLastError() == 777);
    size_t guest_count;
    uint32_t guest_status = skse_win_multi_to_wide(code_page, flags, source, (size_t)length,
                                                 (uint8_t *)guest_out, (size_t)capacity, &guest_count);
    if (win_status != guest_status || (size_t)win_count != guest_count ||
        memcmp(win_out, guest_out, sizeof win_out)) {
        fprintf(stderr, "decode mismatch page=%u flags=%u capacity=%d win=%u/%d guest=%u/%zu;",
                code_page, flags, capacity, win_status, win_count, guest_status, guest_count);
        for (int i = 0; i < length; ++i) fprintf(stderr, " %02x", (unsigned)source[i]);
        fputc('\n', stderr);
        assert(false);
    }
}

static uint32_t next_random(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

static void test_native_string_type(void) {
    const uint32_t levels[] = {CT_CTYPE1, CT_CTYPE2, CT_CTYPE3};
    uint8_t cp1252_bytes[32];
    WCHAR cp1252_units[32];
    for (unsigned i = 0; i < 32; ++i) cp1252_bytes[i] = (uint8_t)(0x80 + i);
    assert(MultiByteToWideChar(1252, 0, (const char *)cp1252_bytes, 32, cp1252_units, 32) == 32);
    size_t compared = 0;
    for (unsigned value = 0; value <= 0xffff; ++value) {
        bool supported = value < 256 || (value >= 0xd800 && value <= 0xdfff);
        for (unsigned i = 0; !supported && i < 32; ++i) supported = value == cp1252_units[i];
        WCHAR unit = (WCHAR)value;
        for (unsigned i = 0; i < sizeof levels / sizeof levels[0]; ++i) {
            uint8_t flags[4] = {0xcc, 0xcc, 0xcc, 0xcc};
            uint32_t guest_status = skse_win_string_type(levels[i], (const uint8_t *)&unit, 1, flags);
            if (supported) {
                WORD expected = 0xcccc;
                SetLastError(777);
                assert(GetStringTypeW(levels[i], &unit, 1, &expected) && GetLastError() == 777);
                assert(guest_status == 0 && flags[0] == (uint8_t)expected && flags[1] == (uint8_t)(expected >> 8));
                ++compared;
            } else {
                assert(guest_status == SKSE_WIN_TEXT_NOT_SUPPORTED && flags[0] == 0xcc && flags[1] == 0xcc);
            }
            assert(flags[2] == 0xcc && flags[3] == 0xcc);
        }
    }

    const WCHAR terminated[] = {'A', 0x0000, 0xd83d, 0xde00, 'C', 0};
    for (unsigned i = 0; i < sizeof levels / sizeof levels[0]; ++i) {
        WORD windows_flags[7], guest_flags[7];
        memset(windows_flags, 0xcc, sizeof windows_flags);
        memset(guest_flags, 0xcc, sizeof guest_flags);
        SetLastError(777);
        assert(GetStringTypeW(levels[i], terminated, -7, windows_flags) && GetLastError() == 777);
        assert(!skse_win_string_type(levels[i], (const uint8_t *)terminated, 2, (uint8_t *)guest_flags));
        assert(!memcmp(windows_flags, guest_flags, sizeof windows_flags));
        SetLastError(777);
        assert(GetStringTypeW(levels[i], terminated, 6, windows_flags) && GetLastError() == 777);
        assert(!skse_win_string_type(levels[i], (const uint8_t *)terminated, 6, (uint8_t *)guest_flags));
        assert(!memcmp(windows_flags, guest_flags, sizeof windows_flags));
        assert(!GetStringTypeW(levels[i], terminated, 0, windows_flags) &&
               GetLastError() == SKSE_WIN_TEXT_INVALID_PARAMETER);
    }
    WORD in_place = 'A';
    assert(!GetStringTypeW(CT_CTYPE1, &in_place, 1, &in_place) &&
           GetLastError() == SKSE_WIN_TEXT_INVALID_PARAMETER && in_place == 'A');
    assert(!GetStringTypeW(CT_CTYPE1 | CT_CTYPE2, terminated, 1, &in_place) &&
           GetLastError() == SKSE_WIN_TEXT_INVALID_FLAGS && in_place == 'A');
    printf("Native Windows character type comparisons passed for %zu flags.\n", compared);
}

static int compare_locale_query(uint32_t lcid, uint32_t type, int capacity) {
    WCHAR win_out[64], guest_out[64];
    memset(win_out, 0xcc, sizeof win_out);
    memset(guest_out, 0xcc, sizeof guest_out);
    SetLastError(777);
    int win_count = GetLocaleInfoW(lcid, type, win_out, capacity);
    uint32_t win_status = win_count ? 0 : GetLastError();
    if (win_count) assert(GetLastError() == 777);
    size_t guest_count;
    uint32_t guest_status = skse_win_locale_info(lcid, type, (uint8_t *)guest_out, (size_t)capacity, &guest_count);
    if (win_status != guest_status || (size_t)win_count != guest_count ||
        memcmp(win_out, guest_out, sizeof win_out)) {
        fprintf(stderr, "locale mismatch lcid=%u type=0x%08x capacity=%d win=%u/%d guest=%u/%zu\n",
                lcid, type, capacity, win_status, win_count, guest_status, guest_count);
        assert(false);
    }
    return win_count;
}

static void test_native_locale_info(void) {
    const uint32_t locales[] = {SKSE_WIN_GUEST_DEFAULT_LCID, SKSE_WIN_LOCALE_INVARIANT};
    for (unsigned i = 0; i < sizeof locales / sizeof locales[0]; ++i) {
        size_t fields = 0;
        for (uint32_t field = 1; field <= 0x1014; ++field) {
            size_t units;
            uint32_t status = skse_win_locale_info(locales[i], field, NULL, 0, &units);
            if (status == SKSE_WIN_TEXT_NOT_SUPPORTED) continue;
            assert(status == 0 && units > 0);
            ++fields;
            for (uint32_t flags = 0; flags < 8; ++flags) {
                uint32_t type = field | LOCALE_NOUSEROVERRIDE | (flags << 28);
                int needed = compare_locale_query(locales[i], type, 0);
                assert(needed <= 64);
                compare_locale_query(locales[i], type, needed ? needed : 2);
                if (needed > 1) compare_locale_query(locales[i], type, needed - 1);
            }
        }
        assert(fields == 129);
        compare_locale_query(locales[i], UINT32_C(0x0800005c), 6);
    }
    uint32_t lcid_samples[] = {0, 0x0400, 0x0800, SKSE_WIN_GUEST_DEFAULT_LCID, SKSE_WIN_LOCALE_INVARIANT, 0xffff};
    const uint32_t flags[] = {0, LCID_INSTALLED, LCID_SUPPORTED, LCID_INSTALLED | LCID_SUPPORTED, 0x39, UINT32_MAX};
    for (unsigned i = 0; i < sizeof lcid_samples / sizeof lcid_samples[0]; ++i) {
        for (unsigned j = 0; j < sizeof flags / sizeof flags[0]; ++j) {
            SetLastError(777);
            BOOL win = IsValidLocale(lcid_samples[i], flags[j]);
            assert(GetLastError() == 777 && (win != 0) == skse_win_is_valid_locale(lcid_samples[i], flags[j]));
        }
    }
    puts("Native Windows locale comparisons passed for 129 fields in two locales.");
}

static void compare_case_map(uint32_t lcid, uint32_t flags, const WCHAR *source,
                             int win_count, size_t count, int capacity, bool in_place) {
    WCHAR win_out[258], guest_out[258];
    memset(win_out, 0xcc, sizeof win_out);
    memset(guest_out, 0xcc, sizeof guest_out);
    if (in_place) {
        memcpy(win_out, source, count * sizeof(WCHAR));
        memcpy(guest_out, source, count * sizeof(WCHAR));
    }
    const WCHAR *win_source = in_place ? win_out : source;
    const WCHAR *guest_source = in_place ? guest_out : source;
    SetLastError(777);
    int expected_count = LCMapStringW(lcid, flags, win_source, win_count, win_out, capacity);
    uint32_t expected_status = expected_count ? 0 : GetLastError();
    size_t guest_count;
    uint32_t guest_status = skse_win_map_string(lcid, flags, (const uint8_t *)guest_source, count,
                                               (uint8_t *)guest_out, (size_t)capacity, &guest_count);
    if (guest_status != expected_status || guest_count != (size_t)expected_count ||
        memcmp(win_out, guest_out, sizeof win_out)) {
        fprintf(stderr, "case map mismatch lcid=0x%04x flags=0x%08x count=%d capacity=%d win=%u/%d guest=%u/%zu\n",
                lcid, flags, win_count, capacity, expected_status, expected_count, guest_status, guest_count);
        assert(false);
    }
}

static void test_native_string_mapping(void) {
    uint8_t every_byte[256];
    WCHAR cp1252_units[256];
    for (unsigned i = 0; i < 256; ++i) every_byte[i] = (uint8_t)i;
    assert(MultiByteToWideChar(1252, 0, (const char *)every_byte, 256, cp1252_units, 256) == 256);
    const uint32_t locales[] = {SKSE_WIN_GUEST_DEFAULT_LCID, SKSE_WIN_LOCALE_INVARIANT};
    const uint32_t flags[] = {LCMAP_LOWERCASE, LCMAP_UPPERCASE,
                             LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING,
                             LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING};
    size_t tested = 0;
    for (size_t i = 0; i < sizeof locales / sizeof locales[0]; ++i) {
        for (size_t j = 0; j < sizeof flags / sizeof flags[0]; ++j) {
            for (unsigned value = 0; value < 256; ++value) {
                WCHAR latin1 = (WCHAR)value;
                compare_case_map(locales[i], flags[j], &latin1, 1, 1, 1, false);
                compare_case_map(locales[i], flags[j], cp1252_units + value, 1, 1, 1, false);
                tested += 2;
            }
            compare_case_map(locales[i], flags[j], cp1252_units, 256, 256, 0, false);
            compare_case_map(locales[i], flags[j], cp1252_units, 256, 256, 255, false);
            compare_case_map(locales[i], flags[j], cp1252_units, 256, 256, 256, false);
            compare_case_map(locales[i], flags[j], cp1252_units, 256, 256, 256, true);
            const WCHAR terminated[] = {'a', 0x0161, 0, 'Z', 0};
            compare_case_map(locales[i], flags[j], terminated, -7, 3, 0, false);
            compare_case_map(locales[i], flags[j], terminated, -7, 3, 3, false);
            compare_case_map(locales[i], flags[j], terminated, 5, 5, 5, false);
            tested += 7;
        }
    }
    compare_case_map(0, LCMAP_LOWERCASE, cp1252_units, 256, 256, 0, false);
    compare_case_map(0, LCMAP_LOWERCASE, cp1252_units, 256, 256, 256, false);
    printf("Native Windows casing comparisons passed for %zu calls.\n", tested + 2);
}

static void compare_collation(uint32_t lcid, uint32_t flags, const WCHAR *left, size_t left_count,
                              const WCHAR *right, size_t right_count) {
    int expected = CompareStringW(lcid, flags, left, (int)left_count, right, (int)right_count);
    uint32_t relation;
    uint32_t status = skse_win_compare_string(lcid, flags, (const uint8_t *)left, left_count,
                                               (const uint8_t *)right, right_count, &relation);
    if (!expected || status || relation != (uint32_t)expected) {
        fprintf(stderr, "collation mismatch lcid=0x%04x flags=0x%08x win=%d guest=%u/%u left:",
                lcid, flags, expected, status, relation);
        for (size_t i = 0; i < left_count; ++i) fprintf(stderr, " %04x", (unsigned)left[i]);
        fputs(" right:", stderr);
        for (size_t i = 0; i < right_count; ++i) fprintf(stderr, " %04x", (unsigned)right[i]);
        fputc('\n', stderr);
        assert(false);
    }
}

static void test_native_string_comparison(void) {
    const uint32_t locales[] = {SKSE_WIN_GUEST_DEFAULT_LCID, SKSE_WIN_LOCALE_INVARIANT};
    const uint32_t flags[] = {0, NORM_IGNORECASE, SORT_STRINGSORT, NORM_IGNORECASE | SORT_STRINGSORT};
    const WCHAR embedded[] = {'a', 0, 'b', 0};
    uint32_t seed = 0xcbde741u;
    size_t tested = 0;
    for (size_t i = 0; i < sizeof locales / sizeof locales[0]; ++i) {
        for (size_t j = 0; j < sizeof flags / sizeof flags[0]; ++j) {
            WCHAR alphabet[96] = {0};
            size_t alphabet_size = 1;
            for (WCHAR unit = ' '; unit <= '~'; ++unit) {
                if (flags[j] != (SORT_STRINGSORT | NORM_IGNORECASE) && (unit == '\'' || unit == '-')) continue;
                alphabet[alphabet_size++] = unit;
            }
            for (size_t a = 0; a < alphabet_size; ++a) {
                for (size_t b = 0; b < alphabet_size; ++b) {
                    WCHAR left[] = {'A', 0, alphabet[a]}, right[] = {'a', alphabet[b]};
                    compare_collation(locales[i], flags[j], alphabet + a, 1, alphabet + b, 1);
                    compare_collation(locales[i], flags[j], left, 3, right, 2);
                    tested += 2;
                }
            }
            for (unsigned run = 0; run < 2048; ++run) {
                WCHAR left[12], right[12];
                size_t left_count = next_random(&seed) % 13, right_count = next_random(&seed) % 13;
                for (size_t n = 0; n < left_count; ++n) left[n] = alphabet[next_random(&seed) % alphabet_size];
                for (size_t n = 0; n < right_count; ++n) right[n] = alphabet[next_random(&seed) % alphabet_size];
                compare_collation(locales[i], flags[j], left, left_count, right, right_count);
                ++tested;
            }
            int expected = CompareStringW(locales[i], flags[j], embedded, -7, embedded + 2, -1);
            uint32_t relation;
            assert(expected && !skse_win_compare_string(locales[i], flags[j], (const uint8_t *)embedded, 1,
                                                        (const uint8_t *)(embedded + 2), 1, &relation) &&
                   relation == (uint32_t)expected);
            ++tested;
        }
    }
    printf("Native Windows collation comparisons passed for %zu calls.\n", tested);
}

static void test_native_windows(void) {
    _Static_assert(sizeof(WCHAR) == sizeof(uint16_t), "Windows WCHAR units");
    uint8_t every_byte[256];
    for (unsigned byte = 0; byte < 256; ++byte) every_byte[byte] = (uint8_t)byte;
    static const uint32_t flags[] = {0, 1, 2, 4, 8, 2 | 4 | 8};
    const int capacities[] = {0, 1, 129, 231, 255, 256, 257, 315, 316, 600};
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; ++i)
        for (size_t j = 0; j < sizeof capacities / sizeof capacities[0]; ++j)
            compare_decode(SKSE_WIN_CP_1252, flags[i], every_byte, 256, capacities[j]);
    for (unsigned code = 0; code < 0x10000; ++code) {
        WCHAR unit = (WCHAR)code;
        compare_encode(SKSE_WIN_CP_1252, 0, &unit, 1, 1);
        compare_encode(SKSE_WIN_CP_1252, 0x400, &unit, 1, 1);
        compare_encode(SKSE_WIN_CP_UTF8, 0, &unit, 1, 4);
        compare_encode(SKSE_WIN_CP_UTF8, 0x80, &unit, 1, 4);
        const uint8_t pair[] = {(uint8_t)(code >> 8), (uint8_t)code};
        compare_decode(SKSE_WIN_CP_UTF8, 0, pair, 2, 3);
        compare_decode(SKSE_WIN_CP_UTF8, 8, pair, 2, 3);
    }
    uint32_t seed = 0x91be634u;
    for (unsigned run = 0; run < 20000; ++run) {
        uint8_t bytes[15];
        WCHAR wide[15];
        int length = 1 + (int)(next_random(&seed) % 15);
        int capacity = (int)(next_random(&seed) % 32);
        for (int i = 0; i < length; ++i) {
            bytes[i] = (uint8_t)next_random(&seed);
            wide[i] = (WCHAR)next_random(&seed);
        }
        compare_decode(SKSE_WIN_CP_UTF8, (run & 1) ? 8u : 0u, bytes, length, capacity);
        compare_encode(SKSE_WIN_CP_UTF8, (run & 1) ? 0x80u : 0u, wide, length, capacity);
        compare_encode(SKSE_WIN_CP_1252, (run & 1) ? 0x400u : 0u, wide, length, capacity);
    }
    puts("Native Windows UTF-8 and Windows-1252 comparisons passed.");
    test_native_string_type();
    test_native_locale_info();
    test_native_string_mapping();
    test_native_string_comparison();
}
#endif

int main(void) {
    test_utf8();
    test_cp1252();
    test_arguments();
    test_string_type();
    test_locale_info();
    test_string_mapping();
    test_string_comparison();
#ifdef _WIN32
    test_native_windows();
#endif
    puts("Portable Win32 text conversion tests passed.");
    return 0;
}
