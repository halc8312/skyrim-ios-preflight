/* SPDX-License-Identifier: GPL-3.0-only */
/* Build against the actual staged Kernel32 shim with its registration entry
 * removed. The guest memory and calls below are authored for this host test. */
#include "kernel32.c"
#include <assert.h>

enum { TEB_ADDRESS = 0x1000, STACK = 0x2000, NAME = 0x3000, BYTES_READ = 0x4000,
       BUFFER = 0x5000, CHUNK = 65536, ECE_PATH = 0x18000, ECE_STREAM = 0x19000 };
#ifdef SKYRIM_IOS_MOD_HOOKS
#include "classic_vm_epoch.h"
enum { ECE_PLAYER = 0x1B2E8E4 };
static uint8_t memory[ECE_PLAYER + 4];
#else
static uint8_t memory[ECE_STREAM + 64];
#endif
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

static bool replay_pex(const void *bytes, size_t size, uint8_t digest[32]) {
    if (size != 5875) return false;
    memcpy(digest, bytes, 32);
    return true;
}

static void check_highheel_read(CPU *cpu) {
    enum { HEEL_LOADER = 0x19100, HEEL_STORE = 0x19200, HEEL_STREAM = 0x19300, HEEL_NAME = 0x19400 };
    PcGuestModHooks table = {0};
    skyrim_highheel_pex_configure(replay_pex);
    skyrim_highheel_pex_fill(&table);
    M32(HEEL_LOADER) = 0x1149B98;
    M32(HEEL_LOADER + 8) = HEEL_STORE;
    M32(HEEL_STORE) = 0x10EA8C8;
    M32(HEEL_STREAM) = 0x110CD68;
    strcpy(GSTR(HEEL_NAME), "hdtHighHeelEffect");
    t_cpu = cpu;
    skyrim_vm_epoch_registered(0x50000);
    assert(!table.highheel_pex.begin(cpu, HEEL_NAME + 1, HEEL_LOADER));
    M8(HEEL_STORE + 0x20) = 1;
    assert(!table.highheel_pex.begin(cpu, HEEL_NAME, HEEL_LOADER));
    M8(HEEL_STORE + 0x20) = 0;
    uint64_t prior = 0;
    for (unsigned run = 0; run < 3; ++run) {
        strcpy(GSTR(NAME), run == 1 ? "Data\\OtherEffect.pex" : "Data\\Effect.pex");
        const uint32_t open_args[] = {NAME, 0x80000000u, 1, 0, 3, 0, 0};
        const uint32_t handle = invoke(cpu, shim_CreateFileA, 7, open_args);
        assert(handle != UINT32_MAX);
        assert(table.highheel_pex.begin(cpu, HEEL_NAME, HEEL_LOADER) && skyrim_highheel_pex_read_pending);
        M32(HEEL_STORE + 0x18) = HEEL_STREAM;
        M8(HEEL_STORE + 0x20) = 1;
        M32(HEEL_STREAM + 0x1c) = handle;
        M32(STACK) = run ? 0xA580E2 : 0xA58152;
        const uint32_t read_args[] = {handle, BUFFER, 32, BYTES_READ, 0};
        assert(invoke(cpu, shim_ReadFile, 5, read_args) == 1 && M32(BYTES_READ) == 32);
        assert(skyrim_highheel_pex_read_pending == (run == 0));
        assert(invoke(cpu, shim_CloseHandle, 1, &handle) == 1);
        const uint64_t epoch = table.highheel_pex.end(cpu, true);
        assert((epoch != 0) == (run == 2) && !skyrim_highheel_pex_read_pending);
        if (epoch) {
            table.highheel_pex.linked(cpu, epoch, true);
            prior = epoch;
        }
        M8(HEEL_STORE + 0x20) = 0;
        M32(HEEL_STORE + 0x18) = 0;
    }
    skyrim_vm_epoch_before_revert(1);
    skyrim_vm_epoch_after_load(1);
    table.highheel_pex.linked(cpu, prior, false);
    t_cpu = NULL;
    M32(STACK) = 0;
}

static void check_ece_nose(CPU *cpu, uint32_t handle, uint32_t other) {
    enum { ACTOR = 0x20000, BASE = 0x22000, RACE = 0x24000, MENU = 0x26000, ROWS = 0x28000,
           HEAD = 0x2A000, FACE = 0x2C000, GEOMETRY = 0x2E000, ENTRY = 0x34000, TRI = 0x35000,
           NOSE_NAME = 0x37000, RELATIVE_PATH = 0x38000 };
    M32(ECE_PLAYER) = ACTOR;
    M32(ACTOR) = 0x10D201C;
    M32(ACTOR + 0x24) = BASE;
    M32(ACTOR + 0x130) = RACE;
    M32(BASE + 0xC4) = RACE;
    M8(BASE + 0x12) = 0x2B;
    M8(BASE + 0x24) = 1;
    M8(RACE + 0x12) = 0x0E;
    M32(RACE + 0x0C) = 0x13746;
    M32(MENU) = 0x10E7404;
    M8(MENU + 0x10) = 0x40;
    M32(MENU + 0xF4) = 1;
    M32(MENU + 0xCC) = ROWS;
    M32(MENU + 0xD4) = 1;
    M32(ROWS) = RACE;
    M8(HEAD + 0x12) = 0x0C;
    M32(HEAD + 0x0C) = 0x51623;
    M32(HEAD + 0x3C) = 1;
    M32(ENTRY + 0x2C) = TRI;
    M32(TRI) = 0x10BE9D4;
    strcpy(GSTR(NOSE_NAME), "NoseUp");
    strcpy(GSTR(RELATIVE_PATH), "Actors\\Character\\Character Assets\\FemaleHeadCharGen.tri");
#define NOSE(event, a, b, c, d, receipt) \
    skyrim_ece_nose_app_observe(cpu, SkyrimEceNose##event, a, b, c, d, receipt)

    assert(!skyrim_ece_nose_app_observe(NULL, SkyrimEceNoseMenu, 0xA5CE2B, MENU, 0, 0, 0));
    assert(!NOSE(Menu, 0xA5CE2C, MENU, 0, 0, 0));
    for (int idle = 0; idle < 80; ++idle) {
        assert(NOSE(Menu, 0xA5CE2B, MENU, 0, 0, 0));
        assert(!NOSE(Begin, MENU, BASE, 0, 0, 0));
        NOSE(MenuEnd, 0, 0, 0, 0, 0);
    }
    for (int test = 0; test < 5; ++test) {
        assert(NOSE(Menu, 0xA5CE2B, MENU, 0, 0, 0));
        assert(NOSE(Begin, MENU, BASE, RACE, 0, 0));
        NOSE(Face, 0x885611, MENU, ACTOR, BASE, FACE);
        assert(NOSE(Head, 0x5A48D2, FACE, HEAD, GEOMETRY, 0));
        assert(NOSE(Lookup, RELATIVE_PATH, 0, 0, 0, 0));
        if (test != 2) {
            const uint32_t selected = test == 1 ? other : handle;
            M32(ECE_STREAM + 0x1C) = selected;
            const uint64_t legacy = test == 4 ? skyrim_ece_tri_app_begin(cpu, ECE_PATH, ECE_STREAM) : 0;
            assert((legacy != 0) == (test == 4));
            const uint64_t ticket = NOSE(Parse, RELATIVE_PATH, ECE_STREAM, 0, 0, 0);
            assert((ticket != 0) == !legacy && skyrim_ece_tri_read_pending);
            if (test == 3) {
                assert(!NOSE(Menu, 0xA5CE2B, MENU, 0, 0, 0));
                assert(!NOSE(Parse, ECE_PATH, ECE_STREAM, 0, 0, 0));
            }
            const uint32_t read_args[] = {selected, BUFFER, 32, BYTES_READ, BYTES_READ + 8};
            M32(STACK) = 0xA580E2;
            assert(invoke(cpu, shim_ReadFile, 5, read_args) == 1 && M32(BYTES_READ) == 32);
            const bool fresh = NOSE(Parsed, 1, 0, 0, 0, ticket);
            assert(fresh == (test == 0) && !skyrim_ece_tri_read_pending);
            if (legacy) skyrim_ece_tri_app_finish(cpu, legacy, 1);
            if (fresh) NOSE(Constructed, ENTRY, 0, 0, 0, 0);
        }
        NOSE(LookupEnd, ENTRY, 0, 0, 0, 0);
        assert(NOSE(Morph, TRI, GEOMETRY, 0, 0, 0));
        NOSE(MorphRecord, NOSE_NAME, 1, 0, 0, 0);
        NOSE(HeadEnd, 0, 0, 0, 0, 0);
        NOSE(End, 0, 0, 0, 0, 0);
        NOSE(MenuEnd, 0, 0, 0, 0, 0);
    }
#undef NOSE
}

static void check_ece_read(CPU *cpu, uint32_t handle) {
    int data = open("Data", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    assert(data >= 0);
    assert(!skyrim_ece_tri_configure(data, replay));
    assert(!skyrim_ece_nose_app_observe(cpu, SkyrimEceNoseMenu, 0xA5CE2B, 0x26000, 0, 0, 0));
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
    M32(STACK) = 0xA58152;  // The real WriteFile return must not witness this read.
    assert(invoke(cpu, shim_ReadFile, 5, read_args) == 1 && M32(BYTES_READ) == 32);
    assert(skyrim_ece_tri_read_pending); // Wrong guest return address cannot witness the read.
    M32(STACK) = 0xA580E2;
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
    M32(ECE_STREAM + 0x30) = 1;
    const uint64_t backed = skyrim_ece_tri_app_begin(cpu, ECE_PATH, ECE_STREAM);
    assert(backed && !skyrim_ece_tri_read_pending);
    skyrim_ece_tri_app_finish(cpu, backed, 1);
    M32(ECE_STREAM + 0x30) = 0;
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
    M32(STACK) = 0xA580E2;
    assert(repeated && invoke(cpu, shim_ReadFile, 5, other_read) == 1 && M32(BYTES_READ) == 32);
    assert(!skyrim_ece_tri_read_pending);
    skyrim_ece_tri_app_finish(cpu, repeated, 1);
    check_ece_nose(cpu, handle, other);
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
    check_highheel_read(&cpu);
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
