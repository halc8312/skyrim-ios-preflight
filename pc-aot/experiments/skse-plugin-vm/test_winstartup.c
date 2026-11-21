/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_winthread.h"

#include <stddef.h>
#include <string.h>

#if defined(SKSE_WIN_STARTUP_NATIVE_CHILD)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

_Static_assert(sizeof(STARTUPINFOW) == SKSE_WIN_STARTUPINFO_BYTES, "PE32 STARTUPINFOW size");
_Static_assert(offsetof(STARTUPINFOW, cb) == 0, "PE32 cb");
_Static_assert(offsetof(STARTUPINFOW, dwFlags) == 44, "PE32 dwFlags");
_Static_assert(offsetof(STARTUPINFOW, wShowWindow) == 48, "PE32 wShowWindow");
_Static_assert(offsetof(STARTUPINFOW, cbReserved2) == 50, "PE32 cbReserved2");
_Static_assert(offsetof(STARTUPINFOW, lpReserved2) == 52, "PE32 lpReserved2");
_Static_assert(offsetof(STARTUPINFOW, hStdInput) == 56, "PE32 hStdInput");
_Static_assert(offsetof(STARTUPINFOW, hStdOutput) == 60, "PE32 hStdOutput");
_Static_assert(offsetof(STARTUPINFOW, hStdError) == 64, "PE32 hStdError");
_Static_assert(PF_NX_ENABLED == 12, "Windows NX feature ID");

static int test_detached_console(SkseWinThread *thread) {
    WCHAR path[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"SKYRIM_WINCONSOLE_TEST_FILE", path, MAX_PATH);
    if (!length || length >= MAX_PATH) return 14;
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE pipe_read, pipe_write;
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE semaphore = CreateSemaphoreW(NULL, 0, 1, NULL);
    if (file == INVALID_HANDLE_VALUE || !event || !semaphore ||
        !CreatePipe(&pipe_read, &pipe_write, NULL, 0)) return 15;

    const struct { HANDLE handle; DWORD read_error; } cases[] = {
        {NULL, ERROR_INVALID_HANDLE},
        {INVALID_HANDLE_VALUE, ERROR_INVALID_HANDLE},
        {(HANDLE)(uintptr_t)0xe3000004, ERROR_INVALID_HANDLE},
        {file, ERROR_INVALID_PARAMETER},
        {pipe_read, ERROR_INVALID_FUNCTION},
        {pipe_write, ERROR_INVALID_FUNCTION},
        {event, ERROR_INVALID_HANDLE},
        {semaphore, ERROR_INVALID_HANDLE},
        {GetCurrentThread(), ERROR_INVALID_HANDLE},
    };
    const SkseWinThreadOperation operations[] = {
        SKSE_WIN_GET_CONSOLE_MODE, SKSE_WIN_READ_CONSOLE_W, SKSE_WIN_WRITE_CONSOLE_W,
    };
    const WCHAR marker[] = {0x0153, 0x0a0b, 0xfffb};
    const DWORD sentinel = 0x1234abcd;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        HANDLE handle = cases[i].handle;
        for (size_t op = 0; op < sizeof operations / sizeof operations[0]; ++op) {
            const bool reading = operations[op] == SKSE_WIN_READ_CONSOLE_W;
            const DWORD expected = reading ? cases[i].read_error : ERROR_INVALID_HANDLE;
            for (unsigned variant = 0; variant < 6; ++variant) {
                WCHAR buffer[sizeof marker / sizeof marker[0]];
                memcpy(buffer, marker, sizeof buffer);
                DWORD word = sentinel;
                DWORD *output = variant == 1 ? NULL : variant == 2 ? (DWORD *)(uintptr_t)1 : &word;
                void *text = variant == 3 ? NULL : variant == 4 ? (void *)(uintptr_t)1 : buffer;
                DWORD count = variant == 5 ? 0 : 2;

                SetLastError(777);
                BOOL success;
                switch (operations[op]) {
                case SKSE_WIN_GET_CONSOLE_MODE: success = GetConsoleMode(handle, output); break;
                case SKSE_WIN_READ_CONSOLE_W: success = ReadConsoleW(handle, text, count, output, NULL); break;
                case SKSE_WIN_WRITE_CONSOLE_W: success = WriteConsoleW(handle, text, count, output, NULL); break;
                default: return 16;
                }
                DWORD error = GetLastError();
                if (success || error != expected || word != sentinel || memcmp(buffer, marker, sizeof buffer))
                    return 17 + (int)op;

                /* Pipe errors are measured for future reference. No guest
                 * pipe handle kind is modeled in the current runtime. */
                if (cases[i].read_error == ERROR_INVALID_FUNCTION) continue;
                const bool disk_file = cases[i].read_error == ERROR_INVALID_PARAMETER;
                thread->last_error = 777;
                if (skse_win_console_call(thread, operations[op], disk_file) || thread->last_error != error)
                    return 20 + (int)op;
            }
        }
    }

    LARGE_INTEGER file_size;
    DWORD bytes_ready = UINT32_MAX;
    if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart ||
        !PeekNamedPipe(pipe_read, NULL, 0, NULL, &bytes_ready, NULL) || bytes_ready)
        return 23;
    CloseHandle(pipe_read);
    CloseHandle(pipe_write);
    CloseHandle(file);
    CloseHandle(event);
    CloseHandle(semaphore);

    /* The optional input control is a separate case. Windows reads it
     * before testing the handle, unlike the transfer's output pointers. */
    const PCONSOLE_READCONSOLE_CONTROL unreadable = (PCONSOLE_READCONSOLE_CONTROL)(uintptr_t)1;
    WCHAR input = marker[0];
    DWORD read = sentinel;
    SetLastError(777);
    if (ReadConsoleW(NULL, &input, 1, &read, unreadable) || GetLastError() != ERROR_NOACCESS ||
        input != marker[0] || read != sentinel)
        return 24;
    return 0;
}

int WINAPI WinMain(HINSTANCE module, HINSTANCE previous, LPSTR arguments, int show) {
    (void)module;
    (void)previous;
    (void)arguments;
    (void)show;
    const DWORD sentinel = 777;
    STARTUPINFOW native;
    uint8_t guest[SKSE_WIN_STARTUPINFO_BYTES + 4];
    memset(&native, 0xcc, sizeof native);
    memset(guest, 0xcc, sizeof guest);

    SetLastError(sentinel);
    GetStartupInfoW(&native);
    if (GetLastError() != sentinel || native.cb != SKSE_WIN_STARTUPINFO_BYTES) return 1;
    if (native.dwFlags || native.cbReserved2 || native.lpReserved2) return 2;
    /* The guest has no Windows shell, station or title metadata. Compare
     * the native record after clearing the three optional host pointers. */
    native.lpReserved = NULL;
    native.lpDesktop = NULL;
    native.lpTitle = NULL;
    skse_win_startup_info(guest);
    if (memcmp(guest, &native, sizeof native) || guest[sizeof native] != 0xcc) return 3;

    SkseWinThread thread = {0};
    static const uint32_t selectors[] = {
        STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE, 0, UINT32_MAX, (uint32_t)-13
    };
    for (unsigned i = 0; i < sizeof selectors / sizeof selectors[0]; ++i) {
        SetLastError(sentinel);
        HANDLE handle = GetStdHandle(selectors[i]);
        DWORD error = GetLastError();
        thread.last_error = sentinel;
        uint32_t value = skse_win_std_handle(&thread, selectors[i]);
        if ((uintptr_t)handle != value || thread.last_error != error) return 4 + (int)i;
    }

    if (GetConsoleWindow()) return 10;
    SetLastError(sentinel);
    UINT native_cp = GetConsoleCP();
    DWORD native_error = GetLastError();
    thread.last_error = sentinel;
    if (native_cp != skse_win_console_cp(&thread) || native_error != thread.last_error) return 11;

    SetLastError(sentinel);
    if ((uint32_t)!!IsDebuggerPresent() != skse_win_debugger_present() || GetLastError() != sentinel) return 12;
    SetLastError(sentinel);
    if ((uint32_t)!!IsProcessorFeaturePresent(UINT32_MAX) != skse_win_processor_feature(UINT32_MAX) ||
        GetLastError() != sentinel) return 13;
    return test_detached_console(&thread);
}
#else
#include <assert.h>
#include <stdio.h>

int main(void) {
    uint8_t bytes[SKSE_WIN_STARTUPINFO_BYTES + 4];
    memset(bytes, 0xa5, sizeof bytes);
    skse_win_startup_info(bytes);
    assert(bytes[0] == SKSE_WIN_STARTUPINFO_BYTES);
    for (unsigned i = 1; i < 56; ++i) assert(bytes[i] == 0);
    for (unsigned i = 56; i < sizeof bytes; ++i) assert(bytes[i] == 0xa5);

    SkseWinThread thread = {.last_error = 777};
    assert(!skse_win_std_handle(&thread, (uint32_t)-10) && thread.last_error == 777);
    assert(!skse_win_std_handle(&thread, (uint32_t)-11) && thread.last_error == 777);
    assert(!skse_win_std_handle(&thread, (uint32_t)-12) && thread.last_error == 777);
    assert(skse_win_std_handle(&thread, (uint32_t)-13) == UINT32_MAX && thread.last_error == 6);
    thread.last_error = 777;
    assert(skse_win_std_handle(&thread, 0) == UINT32_MAX && thread.last_error == 6);
    thread.last_error = 777;
    assert(!skse_win_console_cp(&thread) && thread.last_error == 6);
    const SkseWinThreadOperation operations[] = {
        SKSE_WIN_GET_CONSOLE_MODE, SKSE_WIN_READ_CONSOLE_W, SKSE_WIN_WRITE_CONSOLE_W,
    };
    for (unsigned i = 0; i < sizeof operations / sizeof operations[0]; ++i) {
        assert(skse_win_thread_argument_count(operations[i]) == (i ? 5u : 2u));
        thread.last_error = 777;
        assert(!skse_win_console_call(&thread, operations[i], false) && thread.last_error == 6);
        thread.last_error = 777;
        assert(!skse_win_console_call(&thread, operations[i], true) &&
               thread.last_error == (operations[i] == SKSE_WIN_READ_CONSOLE_W ? 87u : 6u));
    }

    assert(!skse_win_debugger_present());
    for (uint32_t feature = 0; feature < 64; ++feature)
        assert(skse_win_processor_feature(feature) == (feature == 12));
    assert(!skse_win_processor_feature(UINT32_MAX));
    puts("Guest Win32 GUI startup metadata tests passed.");
    return 0;
}
#endif
