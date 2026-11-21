/* SPDX-License-Identifier: GPL-3.0-only */
/* Build against the actual staged Kernel32 shim with its registration entry
 * removed. The guest memory and calls below are authored for this host test. */
#include "kernel32.c"
#include <assert.h>

enum { TEB_ADDRESS = 0x1000, STACK = 0x2000, NAME = 0x3000, BYTES_READ = 0x4000, BUFFER = 0x5000, CHUNK = 65536 };
static uint8_t memory[BUFFER + CHUNK];
uint8_t *MEM = memory;
int g_log_level;
char g_game_dir[1024];
char g_save_dir[1024];

void rt_fatal(const char *fmt, ...) {
    fputs(fmt, stderr);
    abort();
}

static uint32_t invoke(CPU *cpu, ShimFn shim, size_t argc, const uint32_t *argv) {
    cpu->r[4] = STACK;
    for (size_t i = 0; i < argc; ++i) M32(STACK + 4 + i * 4) = argv[i];
    shim(cpu);
    assert(cpu->r[4] == STACK + 4 + 4 * argc);
    return cpu->r[0];
}

int main(int argc, char **argv) {
    assert(argc == 3);
    assert(strlen(argv[1]) < sizeof g_game_dir && strlen(argv[2]) < BYTES_READ - NAME);
    strcpy(g_game_dir, argv[1]);
    assert(chdir(g_game_dir) == 0);
    strcpy(GSTR(NAME), argv[2]);
    CPU cpu = {.fs_base = TEB_ADDRESS};
    const uint32_t open_args[] = {NAME, 0x80000000u, 1, 0, 3, 0, 0};
    const uint32_t handle = invoke(&cpu, shim_CreateFileA, 7, open_args);
    if (handle == 0xffffffffu) return rt_get_last_error(&cpu) == ERROR_FILE_NOT_FOUND ? 3 : 2;
    const uint32_t read_args[] = {handle, BUFFER, CHUNK, BYTES_READ, 0};
    size_t size = 0;
    for (;;) {
        assert(invoke(&cpu, shim_ReadFile, 5, read_args) == 1);
        const uint32_t length = M32(BYTES_READ);
        assert(length <= CHUNK && size + length <= 2 * 1024 * 1024);
        if (length == 0) break;
        assert(fwrite(MEM + BUFFER, 1, length, stdout) == length);
        size += length;
    }
    assert(invoke(&cpu, shim_CloseHandle, 1, &handle) == 1);
    return 0;
}
