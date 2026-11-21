/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE 1
#include "rt.h"
#include "runtime_load.h"
#include "runtime_save.h"
#include "save_paths.h"

#include <assert.h>
#include <sys/mman.h>

uint8_t *MEM;
void classic_runtime_fixture_attach(CPU *);

enum {
    SELECTED_RETURN = 0x682225, CRT_OPEN_RETURN = 0xf6bf24, CRT_READ_RETURN = 0xf6adf1,
    CRT_CLOSE_RETURN = 0xf67fc8, STREAM = 0x22000, BUFFER = 0x24000,
    READ_COUNT = BUFFER + 0x200, FILE_INFO_BYTES = 52,
};
static const char guest_path[] = "C:\\Users\\Player\\Saves\\Fixture.ess";
static const char original[] = "immutable authored save bytes";
static const char changed[] = "changed source bytes";
static unsigned clone_calls;
static int clone_error;

/* Emulate Darwin's named clone with this small authored file. The build
 * runs the real capture's independent open, unlink and identity checks. */
int skse_test_fclonefileat(int source, int dir, const char *name, unsigned flags) {
    assert(flags == 0 && ++clone_calls);
    if (clone_error) { errno = clone_error; return -1; }
    assert((fcntl(source, F_GETFL) & O_ACCMODE) == O_RDONLY);
    const int writer = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    assert(writer >= 0);
    char buffer[sizeof original];
    const ssize_t bytes = pread(source, buffer, sizeof buffer, 0);
    assert(bytes > 0 && write(writer, buffer, (size_t)bytes) == bytes && !close(writer));
    return 0;
}

static void write_named(int dir, const char *name, const char *bytes) {
    const int fd = openat(dir, name, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, bytes, strlen(bytes)) == (ssize_t)strlen(bytes) && !close(fd));
}

static void set_call(CPU *cpu, uint32_t stack, uint32_t ret, const uint32_t *args, size_t count) {
    cpu->r[4] = stack;
    M32(stack) = ret;
    for (size_t i = 0; i < count; ++i) M32(stack + 4 + i * 4) = args[i];
}

static uint32_t open_save(CPU *cpu, uint32_t stack, int *observed_errno) {
    strcpy(GSTR(stack + 32), guest_path);
    const uint32_t args[] = {stack + 32, 0x80000000u, 1, 0, 3, 0x80, 0};
    set_call(cpu, stack, CRT_OPEN_RETURN, args, sizeof args / sizeof *args);
    rt_set_last_error(cpu, 0xcafe);
    errno = E2BIG;
    classic_runtime_file_call(cpu, ClassicCreateFile);
    *observed_errno = errno;
    assert(cpu->r[0] && cpu->r[0] != UINT32_MAX && cpu->r[4] == stack + 32 && !rt_get_last_error(cpu));
    return cpu->r[0];
}

static void close_guest(CPU *cpu, uint32_t stack, uint32_t handle) {
    const uint32_t args[] = {handle};
    set_call(cpu, stack, CRT_CLOSE_RETURN, args, sizeof args / sizeof *args);
    rt_set_last_error(cpu, 0xabcd);
    classic_runtime_file_call(cpu, ClassicCloseHandle);
    assert(cpu->r[0] == 1 && cpu->r[4] == stack + 8 && rt_get_last_error(cpu) == 0xabcd);
}

static void read_guest(CPU *cpu, uint32_t stack, uint32_t handle, const char *expected) {
    const uint32_t args[] = {handle, BUFFER, sizeof original, READ_COUNT, 0};
    set_call(cpu, stack, CRT_READ_RETURN, args, sizeof args / sizeof *args);
    memset(MEM + BUFFER, 0x30, sizeof original);
    M32(READ_COUNT) = UINT32_MAX;
    rt_set_last_error(cpu, 0xaebe);
    classic_runtime_load_file_call(cpu, ClassicLoadRead);
    assert(cpu->r[0] == 1 && cpu->r[4] == stack + 24 && rt_get_last_error(cpu) == 0xaebe);
    assert(M32(READ_COUNT) == strlen(expected) && !memcmp(MEM + BUFFER, expected, strlen(expected)));
}

static void check_position(CPU *cpu, uint32_t stack, uint32_t handle, size_t expected) {
    const uint32_t args[] = {handle, 0, 0, 1};
    set_call(cpu, stack, 0, args, sizeof args / sizeof *args);
    classic_runtime_load_file_call(cpu, ClassicLoadSeek);
    assert(cpu->r[0] == expected);
}

static void check_size(CPU *cpu, uint32_t stack, uint32_t handle, size_t expected) {
    const uint32_t args[] = {handle, READ_COUNT};
    set_call(cpu, stack, 0, args, sizeof args / sizeof *args);
    M32(READ_COUNT) = UINT32_MAX;
    classic_runtime_load_file_call(cpu, ClassicLoadSize);
    assert(cpu->r[0] == expected && !M32(READ_COUNT));
}

static void get_information(CPU *cpu, uint32_t stack, uint32_t handle, void *bytes) {
    const uint32_t args[] = {handle, BUFFER};
    set_call(cpu, stack, 0, args, sizeof args / sizeof *args);
    classic_runtime_load_file_call(cpu, ClassicLoadInformation);
    assert(cpu->r[0] == 1 && cpu->r[4] == stack + 12);
    memcpy(bytes, MEM + BUFFER, FILE_INFO_BYTES);
}

static void begin(CPU *cpu, uint32_t stack, int directory, const char *path, ClassicRuntimeLoad *load) {
    cpu->r[4] = stack;
    cpu->r[1] = cpu->r[6] = STREAM;
    M32(stack) = SELECTED_RETURN;
    strcpy(GSTR(stack + 0x14), guest_path);
    assert(classic_runtime_load_begin(load, cpu, STREAM, stack + 0x14, directory, path));
}

static void assert_closed(int fd) {
    assert(fcntl(fd, F_GETFD) < 0 && errno == EBADF);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const size_t map_size = GUEST_TEB_BASE + 0x4000;
    MEM = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(MEM != MAP_FAILED);
    const int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(root >= 0 && !mkdirat(root, "Saves", 0700) && !mkdirat(root, "Clone", 0700));
    const int directory = openat(root, "Saves", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    const int scratch = openat(root, "Clone", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    assert(directory >= 0 && scratch >= 0);
    char mapped[CLASSIC_FRESH_PATH_BYTES];
    const int root_length = snprintf(g_save_dir, sizeof g_save_dir, "%s", argv[1]);
    const int length = snprintf(mapped, sizeof mapped, "%s/Saves", argv[1]);
    assert(root_length > 0 && (size_t)root_length < sizeof g_save_dir && length > 0 && (size_t)length < sizeof mapped);
    write_named(directory, "Fixture.ess", original);

    SkseEventBridge *bridge = NULL;
    const SkseGameScope running = {.launch = {1}, .profile = {2}};
    assert(skse_bridge_open(argv[1], &running, &bridge, NULL) == SKSE_BRIDGE_OK && bridge);
    CPU cpu = {0};
    classic_runtime_fixture_attach(&cpu);
    const uint32_t stack = cpu.r[4] - 512, api = stack - 512;

    int ordinary_errno, capture_errno;
    uint8_t base_information[FILE_INFO_BYTES], info[FILE_INFO_BYTES];
    uint32_t handle = open_save(&cpu, api, &ordinary_errno);
    get_information(&cpu, api, handle, base_information);
    read_guest(&cpu, api, handle, original);
    close_guest(&cpu, api, handle);
    assert(!clone_calls);

    for (unsigned mode = 0; mode < 2; ++mode) {
        ClassicRuntimeLoad load;
        SkyrimLifecycleScope lifecycle;
        if (mode) assert(skyrim_lifecycle_scope_open(&lifecycle));
        begin(&cpu, stack, directory, mapped, &load);
        assert(classic_runtime_load_enable_capture(&load, bridge, scratch));
        clone_error = EOPNOTSUPP;
        handle = open_save(&cpu, api, &capture_errno);
        classic_runtime_load_open_leave(&load, true);
        assert(capture_errno == ordinary_errno && clone_calls == mode &&
               !load.pending_input && load.source_fd == -1 && classic_runtime_load_generation(&cpu, STREAM, handle));
        read_guest(&cpu, api, handle, original);
        get_information(&cpu, api, handle, info);
        assert(!memcmp(base_information, info, sizeof info));
        close_guest(&cpu, api, handle);
        classic_runtime_load_end(&load);
        if (mode) assert(skyrim_lifecycle_scope_close(&lifecycle));
    }

    ClassicRuntimeLoad load;
    SkyrimLifecycleScope lifecycle;
    assert(skyrim_lifecycle_scope_open(&lifecycle));
    begin(&cpu, stack, directory, mapped, &load);
    assert(classic_runtime_load_enable_capture(&load, bridge, scratch));
    clone_error = 0;
    handle = open_save(&cpu, api, &capture_errno);
    classic_runtime_load_open_leave(&load, true);
    assert(capture_errno == ordinary_errno && clone_calls == 2 &&
           load.pending_input && load.source_fd >= 0 && load.fd >= 0 && load.fd != load.source_fd);
    const int loader_fd = load.fd, source_fd = load.source_fd;
    struct stat source, loader, named;
    assert(!fstat(source_fd, &source) && !fstat(loader_fd, &loader) &&
           !fstatat(directory, "Fixture.ess", &named, AT_SYMLINK_NOFOLLOW));
    assert(!skse_save_same_file(&source, &loader) && skse_save_same_version(&source, &named));
    assert(loader.st_nlink == 0 && (fcntl(loader_fd, F_GETFL) & O_ACCMODE) == O_RDONLY &&
           (fcntl(loader_fd, F_GETFD) & FD_CLOEXEC) && lseek(source_fd, 0, SEEK_CUR) == 0);
    assert(fstatat(scratch, "base.clone", &named, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT);
    check_position(&cpu, api, handle, 0);

    assert(!renameat(directory, "Fixture.ess", directory, "Previous.ess"));
    write_named(directory, "Previous.ess", changed);
    write_named(directory, "Fixture.ess", changed);
    assert(classic_runtime_load_generation(&cpu, STREAM, handle));
    get_information(&cpu, api, handle, info);
    assert(!memcmp(base_information, info, sizeof info));
    check_size(&cpu, api, handle, sizeof original - 1);
    read_guest(&cpu, api, handle, original);
    check_position(&cpu, api, handle, sizeof original - 1);

    /* The application-owned attempt may finish before the game closes its
     * handle. The original stream still uses the snapshot until that close. */
    classic_runtime_load_end(&load);
    assert(skyrim_lifecycle_scope_close(&lifecycle));
    check_size(&cpu, api, handle, sizeof original - 1);
    close_guest(&cpu, api, handle);
    assert_closed(loader_fd);
    assert_closed(source_fd);
    const uint32_t reused = open_save(&cpu, api, &capture_errno);
    assert(reused == handle && clone_calls == 2);
    read_guest(&cpu, api, reused, changed);
    close_guest(&cpu, api, reused);
    skse_bridge_close(bridge);
    assert(!close(scratch) && !close(directory) && !close(root) && !munmap(MEM, map_size));
    puts("PASS selected GFile reads immutable captured bytes and retains original guest metadata");
    return 0;
}
