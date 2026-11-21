/* SPDX-License-Identifier: GPL-3.0-only */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../skse-plugin-vm/x86_winfile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const char file_name[] = "Data/Options.json";
static const char previous[] = "{\"value\":3}";
static const char changed[] = "{\"value\":7}";

static uint32_t input_attributes(void *context, const char *path, uint32_t *attributes) {
    if (skse_win_asset_name_equal(path, "Data")) {
        *attributes = SKSE_WIN_ATTRIBUTE_DIRECTORY;
        return 0;
    }
    if (*(const bool *)context && skse_win_asset_name_equal(path, file_name)) {
        *attributes = SKSE_WIN_ATTRIBUTE_NORMAL;
        return 0;
    }
    return ERROR_FILE_NOT_FOUND;
}

static uint32_t input_read(void *context, const char *path, size_t limit, uint8_t **bytes, size_t *length) {
    if (!*(const bool *)context || !skse_win_asset_name_equal(path, file_name)) return ERROR_FILE_NOT_FOUND;
    if (limit < sizeof(previous) - 1) return ERROR_FILE_TOO_LARGE;
    *bytes = malloc(sizeof(previous) - 1);
    if (!*bytes) return ERROR_NOT_ENOUGH_MEMORY;
    memcpy(*bytes, previous, sizeof(previous) - 1);
    *length = sizeof(previous) - 1;
    return 0;
}

static bool allow(void *context, const char *path) {
    (void)context;
    return skse_win_asset_name_equal(path, file_name);
}

static void fill(HANDLE file, const char *bytes, DWORD length) {
    DWORD written = 0;
    assert(WriteFile(file, bytes, length, &written, NULL) && written == length);
}

static HANDLE open_file(const WCHAR *path, DWORD access, DWORD share, DWORD disposition) {
    return CreateFileW(path, access, share, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void compare_creation_dispositions(const WCHAR *path) {
    for (unsigned had_input = 0; had_input < 2; ++had_input) {
        for (DWORD disposition = CREATE_NEW; disposition <= TRUNCATE_EXISTING; ++disposition) {
            bool exists = had_input != 0;
            if (exists) {
                HANDLE initial = open_file(path, GENERIC_WRITE, 0, CREATE_NEW);
                assert(initial != INVALID_HANDLE_VALUE);
                fill(initial, previous, sizeof(previous) - 1);
                assert(CloseHandle(initial));
            }
            SkseWinFiles files = {.profile = {{input_read, input_attributes, &exists}, allow}};
            const unsigned rw = SKSE_WIN_FILE_READ | SKSE_WIN_FILE_WRITE;
            SetLastError(321);
            HANDLE native = open_file(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, disposition);
            const DWORD windows_error = GetLastError();
            uint32_t guest_error = 321;
            uint32_t remote = skse_win_file_open(&files, file_name, rw, rw, (SkseWinFileDisposition)disposition, &guest_error);
            assert((remote == UINT32_MAX) == (native == INVALID_HANDLE_VALUE));
            if (remote == UINT32_MAX) {
                assert(guest_error == windows_error);
            } else {
                if (disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS) assert(guest_error == windows_error);
                SkseWinFile *file = skse_win_file_lookup(&files, remote);
                char bytes[sizeof previous];
                DWORD read = 0;
                assert(ReadFile(native, bytes, sizeof bytes, &read, NULL));
                assert(file && file->data->length == read && (!read || !memcmp(file->data->bytes, bytes, read)));
                assert(CloseHandle(native));
                skse_win_file_close(file);
                SkseWinProfileOutputs out;
                assert(!skse_win_profile_closed_outputs(&files, true, &out));
                assert(out.count == (!exists || disposition == CREATE_ALWAYS || disposition == TRUNCATE_EXISTING ? 1u : 0u));
            }
            if (native != INVALID_HANDLE_VALUE || exists) assert(DeleteFileW(path));
            skse_win_files_close(&files);
        }
    }
}

static void compare_shared_handles(const WCHAR *path) {
    HANDLE prior = open_file(path, GENERIC_WRITE, 0, CREATE_NEW);
    assert(prior != INVALID_HANDLE_VALUE);
    fill(prior, previous, sizeof(previous) - 1);
    assert(CloseHandle(prior));
    bool exists = true;
    SkseWinFiles files = {.profile = {{input_read, input_attributes, &exists}, allow}};
    const unsigned rw = SKSE_WIN_FILE_READ | SKSE_WIN_FILE_WRITE;
    HANDLE reader = open_file(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    assert(reader != INVALID_HANDLE_VALUE);
    uint32_t error = 0;
    uint32_t input = skse_win_file_open(&files, file_name, SKSE_WIN_FILE_READ, SKSE_WIN_FILE_READ, SKSE_WIN_OPEN_EXISTING, &error);
    assert(input != UINT32_MAX);
    assert(open_file(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING) == INVALID_HANDLE_VALUE);
    DWORD expected = GetLastError();
    assert(skse_win_file_open(&files, "data/options.JSON", SKSE_WIN_FILE_WRITE, rw, SKSE_WIN_OPEN_EXISTING, &error) == UINT32_MAX &&
           error == expected);
    assert(CloseHandle(reader));
    skse_win_file_close(skse_win_file_lookup(&files, input));

    reader = open_file(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING);
    HANDLE writer = open_file(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING);
    assert(reader != INVALID_HANDLE_VALUE && writer != INVALID_HANDLE_VALUE);
    input = skse_win_file_open(&files, file_name, SKSE_WIN_FILE_READ, rw, SKSE_WIN_OPEN_EXISTING, &error);
    uint32_t output = skse_win_file_open(&files, "data/options.JSON", SKSE_WIN_FILE_WRITE, rw, SKSE_WIN_OPEN_EXISTING, &error);
    SkseWinFile *virtual_reader = skse_win_file_lookup(&files, input);
    SkseWinFile *virtual_writer = skse_win_file_lookup(&files, output);
    assert(virtual_reader && virtual_writer);

    fill(writer, changed, sizeof(changed) - 1);
    memcpy(virtual_writer->data->bytes, changed, sizeof(changed) - 1);
    skse_win_file_note_write(virtual_writer, true);
    char bytes[sizeof changed];
    DWORD read = 0;
    assert(ReadFile(reader, bytes, sizeof bytes, &read, NULL) && read == sizeof(changed) - 1);
    assert(virtual_reader->data->length == read && !memcmp(virtual_reader->data->bytes, bytes, read));
    assert(CloseHandle(writer));
    skse_win_file_close(virtual_writer);
    SkseWinProfileOutputs out;
    assert(skse_win_profile_closed_outputs(&files, true, &out) == ERROR_SHARING_VIOLATION && !out.count);
    assert(CloseHandle(reader) && DeleteFileW(path));
    skse_win_file_close(virtual_reader);
    assert(!skse_win_profile_closed_outputs(&files, true, &out) && out.count == 1);
    skse_win_files_close(&files);
}

int main(void) {
    WCHAR file[MAX_PATH];
    DWORD root = GetEnvironmentVariableW(L"SKYRIM_PROFILE_WIN_TEST_ROOT", file, MAX_PATH);
    static const WCHAR leaf[] = L"\\profile-output.json";
    assert(root && root + sizeof(leaf) / sizeof(*leaf) <= MAX_PATH);
    memcpy(file + root, leaf, sizeof leaf);
    compare_creation_dispositions(file);
    compare_shared_handles(file);
    puts("Win32 writable-overlay dispositions and in-place shared write matched native files.");
    return 0;
}
