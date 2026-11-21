/* SPDX-License-Identifier: GPL-3.0-only */
/* Build against the actual staged Kernel32 shim with its registration entry
 * removed. The guest memory and calls below are authored for this host test. */
#include "kernel32.c"
#include <assert.h>

enum { TEB_ADDRESS = 0x1000, STACK = 0x2000, NAME = 0x3000, BYTES_READ = 0x4000,
       BUFFER = 0x5000, CHUNK = 65536, ECE_PATH = 0x18000, ECE_STREAM = 0x19000 };
static uint8_t memory[ECE_STREAM + 64];
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

#ifdef SKYRIM_IOS_MOD_HOOKS
static bool replay(int fd, size_t length, uint8_t digest[32]) {
    return length >= 32 && read(fd, digest, 32) == 32;
}

static void check_ece_read(CPU *cpu, uint32_t handle) {
    int data = open("Data", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    assert(data >= 0);
    assert(!skyrim_ece_tri_configure(data, replay));
    skyrim_ece_tri_allow_input("d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605", true);
    assert(skyrim_ece_tri_configure(data, replay));
    close(data);
    t_cpu = cpu;
    strcpy(GSTR(ECE_PATH), "Meshes\\Actors\\Character\\Character Assets\\FemaleHeadCharGen.tri");
    M32(ECE_STREAM) = 0x110CD68;
    M32(ECE_STREAM + 0x1C) = handle;
    assert(!skyrim_ece_tri_app_begin(cpu, ECE_PATH + 7, ECE_STREAM));
    assert(!skyrim_ece_tri_app_begin(NULL, ECE_PATH, ECE_STREAM));
    const uint64_t ticket = skyrim_ece_tri_app_begin(cpu, ECE_PATH, ECE_STREAM);
    assert(ticket && skyrim_ece_tri_read_pending);
    const uint32_t overlapped = BYTES_READ + 8;
    const uint32_t read_args[] = {handle, BUFFER, 32, BYTES_READ, overlapped};
    assert(invoke(cpu, shim_ReadFile, 5, read_args) == 1 && M32(BYTES_READ) == 32);
    assert(skyrim_ece_tri_read_pending); // Wrong guest return address cannot witness the read.
    M32(STACK) = 0xA58152;
    assert(invoke(cpu, shim_ReadFile, 5, read_args) == 1 && M32(BYTES_READ) == 32);
    assert(!skyrim_ece_tri_read_pending);
    skyrim_ece_tri_app_finish(cpu, ticket + 1, 1);
    skyrim_ece_tri_app_finish(cpu, ticket, 1);
    skyrim_ece_tri_app_finish(cpu, ticket, 0);

    M32(ECE_STREAM) = 0x110CBE4;
    uint64_t archive = skyrim_ece_tri_app_begin(cpu, ECE_PATH, ECE_STREAM);
    assert(archive && !skyrim_ece_tri_read_pending);
    skyrim_ece_tri_app_finish(cpu, archive, 1);
    archive = skyrim_ece_tri_app_begin(cpu, ECE_PATH, ECE_STREAM);
    skyrim_ece_tri_app_finish(cpu, archive, 0);

    M32(ECE_STREAM) = 0x110CD68;
    uint64_t repeated = skyrim_ece_tri_app_begin(cpu, ECE_PATH, ECE_STREAM);
    assert(repeated && !skyrim_ece_tri_app_begin(cpu, ECE_PATH, ECE_STREAM));
    assert(!skyrim_ece_tri_read_pending);
    skyrim_ece_tri_app_finish(cpu, repeated, 1);

    // Use the same Win32 wrappers and bytes through another inode. Neither it
    // nor a follow-up read of that handle may prove the audited loose winner.
    strcpy(GSTR(NAME), "Data\\other.tri");
    const uint32_t other_args[] = {NAME, 0x80000000u, 1, 0, 3, 0, 0};
    const uint32_t other = invoke(cpu, shim_CreateFileA, 7, other_args);
    assert(other != UINT32_MAX);
    M32(ECE_STREAM + 0x1C) = other;
    repeated = skyrim_ece_tri_app_begin(cpu, ECE_PATH, ECE_STREAM);
    const uint32_t other_read[] = {other, BUFFER, 32, BYTES_READ, overlapped};
    M32(STACK) = 0xA58152;
    assert(repeated && invoke(cpu, shim_ReadFile, 5, other_read) == 1 && M32(BYTES_READ) == 32);
    assert(!skyrim_ece_tri_read_pending);
    skyrim_ece_tri_app_finish(cpu, repeated, 1);
    assert(invoke(cpu, shim_CloseHandle, 1, &other) == 1);
    skyrim_ece_tri_retire();
    t_cpu = NULL;
    M32(STACK) = 0;
}
#endif

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
#ifdef SKYRIM_IOS_MOD_HOOKS
    check_ece_read(&cpu, handle);
#endif
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
