/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE 1
#include "lifted_backend.h"
#include "rt.h"

#include <assert.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

enum { NAME_1 = 0x41000, NAME_2 = 0x41400, DATA = 0x41800, COUNT = 0x41c00, STACK = 0x42000 };
static ClassicRuntimeSave save;

static uint32_t shim(CPU *cpu, enum ClassicFileCall operation, uint32_t ret,
                     size_t count, const uint32_t *args) {
    const uint32_t stack = cpu->r[4], eax = cpu->r[0];
    cpu->r[4] = STACK;
    M32(STACK) = ret;
    for (size_t i = 0; i < count; ++i) M32(STACK + 4 + i * 4) = args[i];
    classic_runtime_file_call(cpu, operation);
    assert(cpu->r[4] == STACK + 4 + count * 4 && M32(STACK) == ret);
    const uint32_t result = cpu->r[0];
    cpu->r[4] = stack;
    cpu->r[0] = eax;
    return result;
}

static void path_for(uint32_t address, const char *leaf) {
    const int length = snprintf(GSTR(address), 1024, "C:\\Users\\Player\\%s", leaf);
    assert(length > 0 && length < 1024);
}

void fixture_runtime_begin(CPU *cpu, int dir, const char *parent, const char *child, const char *leaf) {
    const int length = snprintf(g_save_dir, sizeof g_save_dir, "%s/%s", parent, child);
    assert(length > 0 && (size_t)length < sizeof g_save_dir);
    assert(signal(SIGXFSZ, SIG_IGN) != SIG_ERR);
    assert(classic_runtime_save_begin(&save, cpu, dir, g_save_dir, leaf));
}

int fixture_runtime_open(CPU *cpu, const char *temp) {
    path_for(NAME_1, temp);
    const uint32_t existing[] = {NAME_1, 0xc0000000, 1, 0, 3, 0, 0};
    const uint32_t create[] = {NAME_1, 0x40000000, 1, 0, 2, 0, 0};
    assert(shim(cpu, ClassicCreateFile, 0xf6bf24, 7, existing) == UINT32_MAX);
    assert(shim(cpu, ClassicCreateFile, 0xf6bf24, 7, create) == save.guest_handle && save.guest_handle);
    return save.file.writer;
}

ssize_t fixture_runtime_write(CPU *cpu, const char *bytes, size_t count, bool short_write) {
    assert(count && count < COUNT - DATA);
    memcpy(MEM + DATA, bytes, count);
    return fixture_runtime_write_buffer(cpu, DATA, count, short_write);
}

ssize_t fixture_runtime_write_buffer(CPU *cpu, uint32_t buffer, size_t count, bool short_write) {
    assert(count && count <= UINT32_MAX);
    struct rlimit original;
    if (short_write) {
        const off_t position = lseek(save.file.writer, 0, SEEK_CUR);
        assert(position >= 0 && !getrlimit(RLIMIT_FSIZE, &original));
        struct rlimit limit = original;
        limit.rlim_cur = (rlim_t)position + count - 1;
        assert(!setrlimit(RLIMIT_FSIZE, &limit));
    }
    const uint32_t args[] = {save.guest_handle, buffer, (uint32_t)count, COUNT, 0};
    /* Use the fixed CRT return site for binary WriteFile, with no text conversion. */
    const uint32_t result = shim(cpu, ClassicWriteFile, 0xf6a97c, 5, args);
    if (short_write) assert(!setrlimit(RLIMIT_FSIZE, &original));
    assert(result == 1 && save.last_requested == count && save.last_written == (ssize_t)M32(COUNT));
    return save.last_written;
}

int fixture_runtime_close(CPU *cpu) {
    assert(shim(cpu, ClassicCloseHandle, 0xf67fc8, 1, &save.guest_handle) == 1 && save.os_close_seen);
    return save.close_result;
}

int fixture_runtime_rename(CPU *cpu, const char *from, const char *to) {
    path_for(NAME_1, from);
    path_for(NAME_2, to);
    const uint32_t args[] = {NAME_1, NAME_2};
    return shim(cpu, ClassicMoveFile, 0xf53f17, 2, args) == 1 ? 0 : -1;
}

uint32_t fixture_runtime_last_error(CPU *cpu) { return rt_get_last_error(cpu); }
int fixture_runtime_reader(void) { return classic_runtime_save_reader(&save); }

void fixture_runtime_end(int expected_reader) {
    const int actual = classic_runtime_save_reader(&save);
    assert((actual >= 0) == (expected_reader >= 0));
    if (actual >= 0) {
        struct stat a, b;
        assert(!fstat(actual, &a) && !fstat(expected_reader, &b));
        assert(a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size);
    }
    classic_runtime_save_end(&save);
}
