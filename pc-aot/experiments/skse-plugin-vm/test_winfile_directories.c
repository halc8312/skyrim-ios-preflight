/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_winfile.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

enum { FILE_MISSING = 2, PATH_MISSING = 3, QUOTA = 4, DENIED = 5,
       EXISTS = 183, READ_ONLY = 1 };

static uint32_t asset_attributes(void *context, const char *path, uint32_t *attributes) {
    ++*(unsigned *)context;
    if (skse_win_asset_name_equal(path, "Data") || skse_win_asset_name_equal(path, "Data/Stable"))
        *attributes = SKSE_WIN_ATTRIBUTE_DIRECTORY;
    else if (skse_win_asset_name_equal(path, "Data/Existing.json"))
        *attributes = READ_ONLY;
    else if (!strncmp(path, "Data/Locked", sizeof "Data/Locked" - 1))
        return DENIED;
    else
        return FILE_MISSING;
    return 0;
}

static uint32_t unexpected_read(void *context, const char *path, size_t limit, uint8_t **bytes, size_t *length) {
    (void)context;
    (void)path;
    (void)limit;
    (void)bytes;
    (void)length;
    assert(false);
    return DENIED;
}

static void check_local(SkseWinFiles *files, const char *path, uint32_t expected_error,
                         uint32_t expected_attributes) {
    uint32_t attributes = UINT32_MAX, error = UINT32_MAX;
    assert(skse_win_local_attributes(files, path, &attributes, &error));
    assert(attributes == expected_attributes && error == expected_error);
}

static void check_unknown(SkseWinFiles *files, const char *path) {
    uint32_t attributes = UINT32_MAX, error = UINT32_MAX;
    assert(!skse_win_local_attributes(files, path, &attributes, &error));
    assert(attributes == UINT32_MAX && error == UINT32_MAX);
}

static void test_virtual_directories(void) {
    unsigned metadata_calls = 0;
    SkseWinFiles files = {.source = {unexpected_read, asset_attributes, &metadata_calls}};
    assert(skse_win_directory_create(&files, "Data") == EXISTS);
    assert(skse_win_directory_create(&files, "data/existing.JSON") == EXISTS);
    assert(skse_win_directory_create(&files, "Data/Existing.json/child") == PATH_MISSING);
    assert(skse_win_directory_create(&files, "Data/Absent/child") == PATH_MISSING);
    assert(skse_win_directory_create(&files, "Data/Locked/child") == DENIED);
    check_unknown(&files, "Data/Absent");
    check_unknown(&files, "Data/Existing.json");

    char name[] = "Data/Config";
    assert(!skse_win_directory_create(&files, name));
    name[0] = 'X';
    unsigned before = metadata_calls;
    check_local(&files, "data/config", 0, SKSE_WIN_ATTRIBUTE_DIRECTORY);
    check_local(&files, "Data/Config/Missing", FILE_MISSING, 0);
    check_local(&files, "Data/Config/Missing/file", PATH_MISSING, 0);
    check_unknown(&files, "Data/Configuration");
    assert(skse_win_directory_create(&files, "data/config") == EXISTS);

    const uint16_t wide_path[] = {'c', ':', '/', 's', 'K', 'Y', 'r', 'I', 'm', '\\',
                                  'D', 'a', 't', 'a', '\\', 'C', 'o', 'n', 'f', 'i', 'g', '/',
                                  0x8a2d, 0x5b9a};
    char normalized[SKSE_WIN_PATH_BYTES];
    assert(skse_win_asset_path(wide_path, sizeof wide_path / sizeof wide_path[0], true, normalized));
    assert(!strcmp(normalized, "Data/Config/\xe8\xa8\xad\xe5\xae\x9a"));
    assert(!skse_win_directory_create(&files, normalized));
    check_local(&files, normalized, 0, SKSE_WIN_ATTRIBUTE_DIRECTORY);
    assert(metadata_calls == before);

    uint32_t error = UINT32_MAX;
    unsigned access = SKSE_WIN_FILE_READ | SKSE_WIN_FILE_WRITE;
    assert(skse_win_file_open(&files, "data/config", access, access, SKSE_WIN_CREATE_NEW, &error) ==
           UINT32_MAX && error == DENIED);
    assert(skse_win_file_open(&files, "Data/Config/Gone/read.json", SKSE_WIN_FILE_READ,
                              access, SKSE_WIN_OPEN_EXISTING, &error) == UINT32_MAX && error == PATH_MISSING);
    assert(skse_win_file_open(&files, "Data/Config/Gone/create.json", access, access,
                              SKSE_WIN_CREATE_NEW, &error) == UINT32_MAX && error == PATH_MISSING);
    const char *json = "Data/Config/note.json";
    uint32_t token = skse_win_file_open(&files, json, access, access, SKSE_WIN_OPEN_ALWAYS, &error);
    assert(token != UINT32_MAX && !error);
    SkseWinFile *file = skse_win_file_lookup(&files, token);
    assert(file && !skse_win_file_resize(file, 2));
    memcpy(file->data->bytes, "{}", 2);
    skse_win_file_close(file);
    check_local(&files, "DATA/config/NOTE.JSON", 0, SKSE_WIN_ATTRIBUTE_NORMAL);
    check_local(&files, "Data/Config/note.json/child", PATH_MISSING, 0);
    assert(skse_win_directory_create(&files, "data/config/NOTE.JSON") == EXISTS);
    assert(skse_win_directory_create(&files, "Data/Config/note.json/child") == PATH_MISSING);

    token = skse_win_file_open(&files, "data/config/NOTE.JSON", SKSE_WIN_FILE_READ,
                               access, SKSE_WIN_OPEN_EXISTING, &error);
    file = skse_win_file_lookup(&files, token);
    assert(file && file->data->length == 2 && !memcmp(file->data->bytes, "{}", 2));
    assert(metadata_calls == before);

    SkseWinFiles another = {0};
    check_unknown(&another, "Data/Config");
    assert(!skse_win_directory_create(&another, "Local"));
    assert(!skse_win_directory_create(&another, "Local/Child"));
    check_local(&another, "local/child", 0, SKSE_WIN_ATTRIBUTE_DIRECTORY);
    check_unknown(&files, "Local");
    skse_win_files_close(&another);
    skse_win_files_close(&files);
    check_unknown(&files, "Data/Config");
    check_unknown(&files, json);
    assert(!files.directory_count && !skse_win_file_lookup(&files, token));
    skse_win_files_close(&files);
}

static void test_resource_bounds(void) {
    SkseWinFiles files = {0};
    char name[20];
    for (unsigned i = 0; i < SKSE_WIN_OUTPUT_DIRECTORIES; ++i) {
        assert(snprintf(name, sizeof name, "Folder%u", i) > 0);
        assert(!skse_win_directory_create(&files, name));
    }
    assert(skse_win_directory_create(&files, "Folder0") == EXISTS);
    assert(skse_win_directory_create(&files, "Overflow") == QUOTA);
    check_unknown(&files, "Overflow");
    skse_win_files_close(&files);

    unsigned metadata_calls = 0;
    files = (SkseWinFiles){.source = {unexpected_read, asset_attributes, &metadata_calls}};
    for (unsigned i = 0; i < 32; ++i)
        assert(skse_win_directory_create(&files, "Data/Absent/child") == PATH_MISSING);
    assert(!files.directory_count && metadata_calls == 64);
    assert(skse_win_directory_create(&files, "NeverSent") == QUOTA);
    assert(metadata_calls == 64);
    skse_win_files_close(&files);

    files = (SkseWinFiles){.source.read = unexpected_read};
    assert(skse_win_directory_create(&files, "WithoutMetadata") == DENIED);
    check_unknown(&files, "WithoutMetadata");
    skse_win_files_close(&files);
}

#ifdef _WIN32
static void native_test_path(const WCHAR *leaf, WCHAR path[MAX_PATH]) {
    size_t root = GetEnvironmentVariableW(L"SKYRIM_WINFILE_TEST_ROOT", path, MAX_PATH);
    size_t length = wcslen(leaf);
    assert(root && root < MAX_PATH && root + length + 2 <= MAX_PATH);
    path[root] = L'\\';
    memcpy(path + root + 1, leaf, (length + 1) * sizeof *path);
}

static void native_mkdir(SkseWinFiles *files, const WCHAR *native, const char *local, uint32_t expected) {
    WCHAR path[MAX_PATH];
    native_test_path(native, path);
    uint32_t status = CreateDirectoryW(path, NULL) ? 0 : GetLastError();
    assert(status == expected && skse_win_directory_create(files, local) == status);
}

static void native_attributes(SkseWinFiles *files, const WCHAR *native, const char *local) {
    WCHAR path[MAX_PATH];
    native_test_path(native, path);
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) check_local(files, local, GetLastError(), 0);
    else check_local(files, local, 0, attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static void test_windows_reference(void) {
    unsigned metadata_calls = 0;
    SkseWinFiles files = {.source = {unexpected_read, asset_attributes, &metadata_calls}};
    WCHAR path[MAX_PATH];
    native_test_path(L"Data", path);
    assert(CreateDirectoryW(path, NULL));
    native_test_path(L"Data\\Existing.json", path);
    HANDLE asset = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(asset != INVALID_HANDLE_VALUE && CloseHandle(asset));
    assert(SetFileAttributesW(path, FILE_ATTRIBUTE_READONLY));

    native_mkdir(&files, L"Data", "Data", EXISTS);
    native_mkdir(&files, L"Data\\Existing.json", "Data/Existing.json", EXISTS);
    native_mkdir(&files, L"Data\\Existing.json\\child", "Data/Existing.json/child", PATH_MISSING);
    native_mkdir(&files, L"Data\\Never\\child", "Data/Never/child", PATH_MISSING);
    native_mkdir(&files, L"Data\\Config", "Data/Config", 0);
    native_mkdir(&files, L"DATA\\CONFIG", "DATA/CONFIG", EXISTS);
    native_mkdir(&files, L"Data\\Config\\\x8a2d\x5b9a", "Data/Config/\xe8\xa8\xad\xe5\xae\x9a", 0);
    native_attributes(&files, L"data\\CONFIG", "data/CONFIG");
    native_attributes(&files, L"Data\\Config\\\x8a2d\x5b9a", "Data/Config/\xe8\xa8\xad\xe5\xae\x9a");
    native_attributes(&files, L"Data\\Config\\missing", "Data/Config/missing");
    native_attributes(&files, L"Data\\Config\\missing\\file", "Data/Config/missing/file");

    native_test_path(L"Data\\Config", path);
    for (DWORD disposition = CREATE_NEW; disposition <= TRUNCATE_EXISTING; ++disposition) {
        HANDLE handle = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                    disposition, FILE_ATTRIBUTE_NORMAL, NULL);
        assert(handle == INVALID_HANDLE_VALUE);
        DWORD expected = GetLastError();
        uint32_t error;
        assert(skse_win_file_open(&files, "Data/Config", SKSE_WIN_FILE_WRITE,
                                  SKSE_WIN_FILE_READ | SKSE_WIN_FILE_WRITE,
                                  (SkseWinFileDisposition)disposition, &error) == UINT32_MAX);
        assert(error == expected);
    }
    native_test_path(L"Data\\Existing.json", path);
    assert(SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL));
    skse_win_files_close(&files);
    puts("Windows reference directory creation, attributes and file-open collisions passed.");
}
#endif

int main(void) {
    test_virtual_directories();
    test_resource_bounds();
#ifdef _WIN32
    test_windows_reference();
#endif
    puts("Guest virtual directory tests passed.");
    return 0;
}
