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
    SELECTED_RETURN = 0x682225, CRT_OPEN_RETURN = 0xf6bf24, CRT_CLOSE_RETURN = 0xf67fc8,
    POSITION = 0x14, STREAM = 0x22000,
};
static const uint32_t generic_read = UINT32_C(0x80000000);
static const char fixture[] = "C:\\Users\\Player\\saves\\fixture.ess";
static const char other[] = "C:\\Users\\Player\\Saves\\Other.ess";
static const char replace[] = "C:\\Users\\Player\\Saves\\Replace.ess";

static void write_named(int dir, const char *name) {
    const int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    assert(fd >= 0 && write(fd, "test", 4) == 4 && !close(fd));
}

static uint32_t selected_path(CPU *cpu, uint32_t stack, const char *name) {
    cpu->r[4] = stack;
    cpu->r[1] = cpu->r[6] = STREAM;
    M32(stack) = SELECTED_RETURN;
    strcpy(GSTR(stack + POSITION), name);
    return stack + POSITION;
}

static uint32_t open_with(CPU *cpu, uint32_t stack, const char *name, uint32_t ret,
                         uint32_t access, uint32_t share, uint32_t disposition) {
    strcpy(GSTR(stack + 32), name);
    cpu->r[4] = stack;
    M32(stack) = ret;
    const uint32_t args[] = {stack + 32, access, share, 0, disposition, 0x80, 0};
    for (size_t i = 0; i < sizeof args / sizeof *args; ++i) M32(stack + 4 + i * 4) = args[i];
    classic_runtime_file_call(cpu, ClassicCreateFile);
    assert(cpu->r[4] == stack + 32 && M32(stack) == ret);
    assert(cpu->r[0] && cpu->r[0] != UINT32_MAX);
    assert(rt_get_last_error(cpu) == (disposition == 4 ? 183 : 0));
    return cpu->r[0];
}

static uint32_t open_readonly(CPU *cpu, uint32_t stack, const char *name) {
    return open_with(cpu, stack, name, CRT_OPEN_RETURN, generic_read, 1, 3);
}

static void close_file(CPU *cpu, uint32_t stack, uint32_t handle) {
    cpu->r[4] = stack;
    M32(stack) = CRT_CLOSE_RETURN;
    M32(stack + 4) = handle;
    classic_runtime_file_call(cpu, ClassicCloseHandle);
    assert(cpu->r[0] == 1 && cpu->r[4] == stack + 8 && M32(stack) == CRT_CLOSE_RETURN);
}

static void *close_from_another_thread(void *context) {
    CPU cpu = {0};
    classic_runtime_fixture_attach(&cpu);
    close_file(&cpu, cpu.r[4] - 128, *(const uint32_t *)context);
    return NULL;
}

static void check_rejected_opens(CPU *cpu, uint32_t stack, int dir, const char *directory) {
    const struct {
        const char *path;
        uint32_t ret, access, share, disposition;
    } cases[] = {
        {fixture, CRT_OPEN_RETURN + 1, generic_read, 1, 3},
        {fixture, CRT_OPEN_RETURN, 0, 1, 3},
        {fixture, CRT_OPEN_RETURN, generic_read, 2, 3},
        {fixture, CRT_OPEN_RETURN, generic_read, 1, 4},
        {other, CRT_OPEN_RETURN, generic_read, 1, 3},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        const uint32_t path = selected_path(cpu, stack, fixture);
        ClassicRuntimeLoad load;
        assert(classic_runtime_load_begin(&load, cpu, STREAM, path, dir, directory));
        const uint32_t handle = open_with(cpu, stack - 512, cases[i].path, cases[i].ret,
                                          cases[i].access, cases[i].share, cases[i].disposition);
        classic_runtime_load_open_leave(&load, true);
        assert(!classic_runtime_load_generation(cpu, STREAM, handle));
        close_file(cpu, stack - 512, handle);
        classic_runtime_load_end(&load);
    }
}

static void check_replaced_selection(CPU *cpu, uint32_t stack, int dir, const char *directory) {
    ClassicRuntimeLoad load;
    write_named(dir, "Replace.ess");
    write_named(dir, "Replacement.ess");
    assert(classic_runtime_load_begin(&load, cpu, STREAM, selected_path(cpu, stack, replace), dir, directory));
    assert(!renameat(dir, "Replace.ess", dir, "Previous.ess"));
    assert(!renameat(dir, "Replacement.ess", dir, "Replace.ess"));
    uint32_t handle = open_readonly(cpu, stack - 512, replace);
    classic_runtime_load_open_leave(&load, true);
    assert(!classic_runtime_load_generation(cpu, STREAM, handle));
    close_file(cpu, stack - 512, handle);
    classic_runtime_load_end(&load);

    assert(classic_runtime_load_begin(&load, cpu, STREAM, selected_path(cpu, stack, replace), dir, directory));
    handle = open_readonly(cpu, stack - 512, replace);
    classic_runtime_load_open_leave(&load, true);
    assert(classic_runtime_load_generation(cpu, STREAM, handle));
    assert(!renameat(dir, "Replace.ess", dir, "Replacement.ess"));
    assert(!renameat(dir, "Previous.ess", dir, "Replace.ess"));
    assert(!classic_runtime_load_generation(cpu, STREAM, handle));
    close_file(cpu, stack - 512, handle);
    classic_runtime_load_end(&load);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const size_t map_size = GUEST_TEB_BASE + 0x4000;
    MEM = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(MEM != MAP_FAILED);
    const int saved = snprintf(g_save_dir, sizeof g_save_dir, "%s", argv[1]);
    char directory[CLASSIC_FRESH_PATH_BYTES];
    const int size = snprintf(directory, sizeof directory, "%s/Saves", g_save_dir);
    assert(saved > 0 && (size_t)saved < sizeof g_save_dir && size > 0 && (size_t)size < sizeof directory);
    const int root = open(g_save_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(root >= 0 && !mkdirat(root, "Saves", 0700));
    const int dir = openat(root, "Saves", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    assert(dir >= 0);
    write_named(dir, "Fixture.ESS");
    write_named(dir, "Other.ess");
    struct stat before, after;
    assert(!fstatat(dir, "Fixture.ESS", &before, AT_SYMLINK_NOFOLLOW));

    CPU cpu = {0};
    classic_runtime_fixture_attach(&cpu);
    const uint32_t stack = cpu.r[4] - 512;
    const uint32_t api = stack - 512;
    ClassicRuntimeLoad load;
    const uint32_t path = selected_path(&cpu, stack, fixture);
    assert(!classic_runtime_load_begin(&load, &cpu, STREAM + 4, path, dir, directory));
    assert(!classic_runtime_load_begin(&load, &cpu, STREAM, path, root, directory));
    assert(classic_runtime_load_begin(&load, &cpu, STREAM, path, dir, directory));
    uint32_t handle = open_readonly(&cpu, api, fixture);
    assert(!classic_runtime_load_generation(&cpu, STREAM, handle));
    classic_runtime_load_open_leave(&load, true);

    errno = E2BIG;
    const uint64_t generation = classic_runtime_load_generation(&cpu, STREAM, handle);
    assert(generation && errno == E2BIG);
    CPU foreign = cpu;
    assert(!classic_runtime_load_generation(&foreign, STREAM, handle));
    assert(!classic_runtime_load_generation(&cpu, STREAM + 4, handle));
    assert(!classic_runtime_load_generation(&cpu, STREAM, handle + 4));
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, close_from_another_thread, &handle));
    assert(!pthread_join(worker, NULL));
    assert(!classic_runtime_load_generation(&cpu, STREAM, handle));
    const uint32_t reused = open_readonly(&cpu, api, fixture);
    assert(reused == handle && !classic_runtime_load_generation(&cpu, STREAM, reused));
    close_file(&cpu, api, reused);
    classic_runtime_load_end(&load);

    assert(classic_runtime_load_begin(&load, &cpu, STREAM, selected_path(&cpu, stack, fixture), dir, directory));
    handle = open_readonly(&cpu, api, fixture);
    classic_runtime_load_open_leave(&load, true);
    const uint64_t new_generation = classic_runtime_load_generation(&cpu, STREAM, handle);
    assert(new_generation > generation);
    const int other_fd = openat(dir, "Other.ess", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    assert(other_fd >= 0 && dup2(other_fd, load.fd) == load.fd && !close(other_fd));
    assert(!classic_runtime_load_generation(&cpu, STREAM, handle));
    close_file(&cpu, api, handle);
    classic_runtime_load_end(&load);

    check_rejected_opens(&cpu, stack, dir, directory);
    check_replaced_selection(&cpu, stack, dir, directory);
    assert(!fstatat(dir, "Fixture.ESS", &after, AT_SYMLINK_NOFOLLOW));
    assert(skse_save_same_version(&before, &after));
    char bytes[5] = {0};
    const int selected = openat(dir, "Fixture.ESS", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    assert(selected >= 0 && read(selected, bytes, sizeof bytes) == 4 && !memcmp(bytes, "test", 4));
    assert(!close(selected) && !close(dir) && !close(root));
    assert(!munmap(MEM, map_size));
    puts("PASS live selected-file witness, native handle identity, close and reuse");
    return 0;
}
