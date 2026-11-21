/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_winenv.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#endif

#define LENGTH(array) (sizeof(array) / sizeof((array)[0]))

static uint32_t set_variable(SkseWinEnv *env, const char *name, const char *value) {
    return skse_win_env_set(env, (const uint8_t *)name, strlen(name), (const uint8_t *)value,
                           value ? strlen(value) : 0);
}

static size_t query_size(const SkseWinEnv *env) {
    size_t needed;
    assert(!skse_win_env_snapshot_w(env, NULL, 0, &needed));
    return needed;
}

static void expect_snapshot(const SkseWinEnv *env, const uint16_t *expected, size_t units) {
    uint8_t bytes[160];
    assert(units * 2 + 2 <= sizeof bytes && query_size(env) == units);
    memset(bytes, 0xc7, sizeof bytes);
    size_t written;
    assert(!skse_win_env_snapshot_w(env, bytes + 1, units, &written));
    assert(written == units && bytes[0] == 0xc7 && bytes[units * 2 + 1] == 0xc7);
    for (size_t i = 0; i < units; ++i) {
        assert(bytes[2 * i + 1] == (uint8_t)expected[i]);
        assert(bytes[2 * i + 2] == (uint8_t)(expected[i] >> 8));
    }
}

static void test_snapshot(void) {
    SkseWinEnv env = {0}, other = {0};
    const uint16_t empty[] = {0, 0};
    expect_snapshot(&env, empty, LENGTH(empty));
    assert(!set_variable(&env, "bEtA", "old"));
    assert(!set_variable(&env, "_a", "u"));
    assert(!set_variable(&env, "A", ""));
    assert(!set_variable(&env, "=C:", "\\"));
    assert(!set_variable(&env, "BETA", "x=\x80\x81"));
    const uint16_t expected[] = {
        '=', 'C', ':', '=', '\\', 0, 'A', '=', 0,
        'b', 'E', 't', 'A', '=', 'x', '=', 0x20ac, 0x81, 0, '_', 'a', '=', 'u', 0, 0
    };
    expect_snapshot(&env, expected, LENGTH(expected));
    expect_snapshot(&other, empty, LENGTH(empty));

    size_t written;
    uint8_t snapshot[2 * LENGTH(expected)];
    assert(!skse_win_env_snapshot_w(&env, snapshot, LENGTH(expected), &written));
    assert(written == LENGTH(expected));
    assert(!set_variable(&env, "bEtA", NULL));
    assert(!set_variable(&env, "beta", NULL));
    const uint16_t removed[] = {'=', 'C', ':', '=', '\\', 0, 'A', '=', 0, '_', 'a', '=', 'u', 0, 0};
    expect_snapshot(&env, removed, LENGTH(removed));
    assert(snapshot[2 * 9] == 'b' && snapshot[2 * 16] == 0xac);
    assert(!set_variable(&env, "_A", NULL));
    assert(!set_variable(&env, "a", NULL));
    assert(!set_variable(&env, "=c:", NULL));
    expect_snapshot(&env, empty, LENGTH(empty));
    assert(!set_variable(&env, "A", "still living"));
    skse_win_env_close(&env);
    skse_win_env_close(&env);
    expect_snapshot(&env, empty, LENGTH(empty));
}

static void test_lengths_and_limits(void) {
    SkseWinEnv env = {0};
    const uint8_t name[] = {'a', 'b', 0, 'c'};
    const uint8_t value[] = {'v', 0, 'w'};
    assert(set_variable(&env, "", "") == SKSE_WIN_ENV_INVALID_PARAMETER);
    assert(set_variable(&env, "bad=name", "") == SKSE_WIN_ENV_INVALID_PARAMETER);
    assert(set_variable(&env, "==", "") == SKSE_WIN_ENV_INVALID_PARAMETER);
    assert(set_variable(&env, "\xc9", "") == SKSE_WIN_ENV_NOT_SUPPORTED);
    assert(skse_win_env_set(&env, name, sizeof name, value, 1) == SKSE_WIN_ENV_INVALID_PARAMETER);
    assert(skse_win_env_set(&env, name, 1, value, sizeof value) == SKSE_WIN_ENV_INVALID_PARAMETER);
    assert(skse_win_env_set(&env, name, 1, NULL, 1) == SKSE_WIN_ENV_INVALID_PARAMETER);
    assert(skse_win_env_set(&env, name, SIZE_MAX, value, 1) == SKSE_WIN_ENV_NOT_SUPPORTED);
    assert(skse_win_env_set(&env, name, 1, value, SIZE_MAX) == SKSE_WIN_ENV_NOT_SUPPORTED);
    assert(query_size(&env) == 2);

    uint8_t empty[] = {0xaa, 0xaa, 0xaa, 0xaa};
    size_t written = 99;
    assert(skse_win_env_snapshot_w(&env, empty, 1, &written) == SKSE_WIN_ENV_INSUFFICIENT_BUFFER);
    assert(written == 0 && !memcmp(empty, "\xaa\xaa\xaa\xaa", sizeof empty));
    assert(skse_win_env_snapshot_w(&env, NULL, 2, &written) == SKSE_WIN_ENV_INVALID_PARAMETER);
    assert(!written && skse_win_env_snapshot_w(&env, empty, 2, NULL) == SKSE_WIN_ENV_INVALID_PARAMETER);

    uint8_t long_name[SKSE_WIN_ENV_MAX_NAME_BYTES + 1];
    memset(long_name, 'n', sizeof long_name);
    assert(!skse_win_env_set(&env, long_name, SKSE_WIN_ENV_MAX_NAME_BYTES, value, 1));
    assert(skse_win_env_set(&env, long_name, sizeof long_name, value, 1) == SKSE_WIN_ENV_NOT_SUPPORTED);
    assert(query_size(&env) == SKSE_WIN_ENV_MAX_NAME_BYTES + 4);
    skse_win_env_close(&env);

    for (unsigned i = 0; i < SKSE_WIN_ENV_MAX_ENTRIES; ++i) {
        char key[8];
        assert(snprintf(key, sizeof key, "a%02u", i) == 3);
        assert(!set_variable(&env, key, ""));
    }
    assert(set_variable(&env, "zz", "") == SKSE_WIN_ENV_NO_MEMORY);
    assert(query_size(&env) == 1 + 5 * SKSE_WIN_ENV_MAX_ENTRIES);
    assert(!set_variable(&env, "A00", "longer value"));
    assert(!set_variable(&env, "a01", NULL));
    assert(!set_variable(&env, "zz", ""));
    skse_win_env_close(&env);

    uint8_t long_value[SKSE_WIN_ENV_MAX_VALUE_BYTES];
    memset(long_value, 'x', sizeof long_value);
    const size_t second_length = SKSE_WIN_ENV_MAX_BLOCK_UNITS - 1 - (5 + sizeof long_value + 2) - (4 + 2);
    assert(!skse_win_env_set(&env, (const uint8_t *)"alpha", 5, long_value, sizeof long_value));
    assert(!skse_win_env_set(&env, (const uint8_t *)"beta", 4, long_value, second_length));
    assert(query_size(&env) == SKSE_WIN_ENV_MAX_BLOCK_UNITS);
    assert(skse_win_env_set(&env, (const uint8_t *)"BETA", 4, long_value, second_length + 1) == SKSE_WIN_ENV_NO_MEMORY);
    assert(query_size(&env) == SKSE_WIN_ENV_MAX_BLOCK_UNITS);
    assert(!set_variable(&env, "ALPHA", NULL));
    assert(!skse_win_env_set(&env, (const uint8_t *)"BETA", 4, long_value, second_length + 1));
    skse_win_env_close(&env);
}

#ifdef _WIN32
typedef struct {
    NTSTATUS (NTAPI *create)(BOOLEAN, PVOID *);
    NTSTATUS (NTAPI *destroy)(PVOID);
    NTSTATUS (NTAPI *set)(PVOID *, PUNICODE_STRING, PUNICODE_STRING);
    ULONG (NTAPI *to_win32)(NTSTATUS);
} NativeEnv;

static void load_native(HMODULE module, const char *name, void *destination, size_t size) {
    FARPROC address = GetProcAddress(module, name);
    assert(address && size == sizeof address);
    memcpy(destination, &address, size);
}

static void native_string(const char *text, WCHAR out[64], UNICODE_STRING *unicode) {
    const int units = MultiByteToWideChar(1252, 0, text, -1, out, 64);
    assert(units > 0);
    *unicode = (UNICODE_STRING){.Length = (USHORT)((size_t)(units - 1) * sizeof *out),
                                .MaximumLength = (USHORT)((size_t)units * sizeof *out), .Buffer = out};
}

/* Use Windows' private environment-block routines without touching this
 * process's environment. Convert the fixture bytes using the same guest ACP. */
static void test_native_windows(void) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    assert(ntdll);
    NativeEnv native;
    load_native(ntdll, "RtlCreateEnvironment", &native.create, sizeof native.create);
    load_native(ntdll, "RtlDestroyEnvironment", &native.destroy, sizeof native.destroy);
    load_native(ntdll, "RtlSetEnvironmentVariable", &native.set, sizeof native.set);
    load_native(ntdll, "RtlNtStatusToDosError", &native.to_win32, sizeof native.to_win32);
    void *block = NULL;
    assert(native.create(FALSE, &block) == 0 && block);
    assert(!memcmp(block, "\0\0\0\0", 4));
    const struct { const char *name, *value; uint32_t error; } steps[] = {
        {"bEtA", "old", 0}, {"_a", "underline", 0}, {"Alpha", "\x80", 0},
        {"beta", "\xc4=\x81", 0}, {"ALPHA", "", 0}, {"missing", NULL, 0},
        {"", "value", 87}, {"BAD=KEY", "value", 87}, {"==", "value", 87},
        {"=C:", "C:\\", 0}, {"=", "hidden", 0}, {" a", "spaced", 0}, {"a", "prefix", 0},
        {"Alpha", NULL, 0}, {"Alpha", NULL, 0}, {"bEtA", NULL, 0}, {"=c:", NULL, 0}, {"_a", NULL, 0}
    };
    SkseWinEnv model = {0};
    for (size_t i = 0; i < LENGTH(steps); ++i) {
        WCHAR wide_name[64], wide_value[64];
        UNICODE_STRING name, value;
        native_string(steps[i].name, wide_name, &name);
        if (steps[i].value) native_string(steps[i].value, wide_value, &value);
        NTSTATUS status = native.set(&block, &name, steps[i].value ? &value : NULL);
        assert(native.to_win32(status) == steps[i].error);
        assert(set_variable(&model, steps[i].name, steps[i].value) == steps[i].error);
        uint8_t bytes[512];
        size_t units;
        assert(!skse_win_env_snapshot_w(&model, bytes, sizeof bytes / 2, &units));
        assert(!memcmp(block, bytes, units * 2));
    }
    skse_win_env_close(&model);
    assert(native.destroy(block) == 0);
}
#endif

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    if (!strcmp(argv[1], "snapshot")) test_snapshot();
    else if (!strcmp(argv[1], "limits")) test_lengths_and_limits();
#ifdef _WIN32
    else if (!strcmp(argv[1], "windows")) test_native_windows();
#endif
    else return 1;
    return 0;
}
