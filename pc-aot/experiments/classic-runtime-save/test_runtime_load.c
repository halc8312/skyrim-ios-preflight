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

#if SKYRIM_TEST_PINNED_CRT
#include "lift_prelude.h"
#include "funcs_exe.h"

enum {
    CRT_STREAM = GUEST_HEAP_BASE + 0x1000, CRT_BSFILE = GUEST_HEAP_BASE + 0x2000,
    CRT_FILE = 0x12de588 + 4 * 0x20, CRT_FILE_TABLE = GUEST_HEAP_BASE + 0x3000,
    CRT_DESCRIPTOR_PAGE = GUEST_HEAP_BASE + 0x4000, CRT_BUFFER = GUEST_HEAP_BASE + 0x5000,
    CRT_FILENO = 9, CRT_ENTRY = CRT_DESCRIPTOR_PAGE + CRT_FILENO * 0x40,
    CRT_REQUEST = CLASSIC_FRESH_SLOT_READ_BYTES,
};
static uint64_t slot_result;
static bool replace_descriptor_during_copy;

void classic_runtime_fixture_crt_read(CPU *);

void classic_runtime_fixture_slot_result(uint64_t generation) { slot_result = generation; }

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    (void)cache;
    if (target == 0xA57260u) f_A57260(cpu);
    else if (target == 0xA57E70u) f_A57E70(cpu);
    else abort();
}

/* The pinned logical read goes through the actual BSFile buffer branch.
 * Keep copying and file-refill services distinct in the fixture. */
void f_F52E30(CPU *cpu) {
    const uint32_t stack = cpu->r[4], destination = M32(stack + 4);
    assert(M32(stack) == 0xb29ab1u && M32(stack + 8) == CRT_BUFFER && M32(stack + 12) == CRT_REQUEST);
    memcpy(MEM + destination, MEM + CRT_BUFFER, CRT_REQUEST);
    if (replace_descriptor_during_copy) M32(CRT_ENTRY) += 1;
    cpu->r[0] = destination;
    cpu->r[4] += 4;
}
void f_B29850(CPU *cpu) { (void)cpu; abort(); }
void f_F584BE(CPU *cpu) { (void)cpu; abort(); }
void f_F53EBD(CPU *cpu) { (void)cpu; abort(); }
void f_F549DC(CPU *cpu) { (void)cpu; abort(); }

static void prepare_crt(uint32_t handle) {
    M32(CRT_STREAM + 0x104) = CRT_BSFILE;
    M32(CRT_BSFILE) = 0x110dba8u;
    M32(0x110dba8u + 0x44) = 0xa57e70u;
    M32(CRT_BSFILE + 8) = 0xa57260u;
    M32(CRT_BSFILE + 0x14) = CRT_REQUEST;
    M32(CRT_BSFILE + 0x18) = 0;
    M32(CRT_BSFILE + 0x20) = CRT_BUFFER;
    M32(CRT_BSFILE + 0x24) = CRT_FILE;
    M8(CRT_BSFILE + 0x2c) = 1u;
    M32(0x1bc046cu) = CRT_FILE_TABLE;
    M32(0x1bc1480u) = 20;
    M32(CRT_FILE_TABLE + 4 * 4) = CRT_FILE;
    M32(CRT_FILE + 0xc) = 1;
    M32(CRT_FILE + 0x10) = CRT_FILENO;
    M32(0x1bc0320u) = 32;
    M32(0x1bc0340u) = CRT_DESCRIPTOR_PAGE;
    M32(CRT_ENTRY) = handle;
    M8(CRT_ENTRY + 4) = 1;
    memset(MEM + CRT_BUFFER, 0x57, CRT_REQUEST);
}

static uint32_t open_for_crt(CPU *cpu, uint32_t stack, int dir, const char *directory, ClassicRuntimeLoad *load) {
    const uint32_t path = selected_path(cpu, stack, fixture);
    cpu->r[1] = cpu->r[6] = CRT_STREAM;
    assert(classic_runtime_load_begin(load, cpu, CRT_STREAM, path, dir, directory));
    const uint32_t handle = open_readonly(cpu, stack - 512, fixture);
    classic_runtime_load_open_leave(load, true);
    prepare_crt(handle);
    return handle;
}

static uint64_t call_crt_slot(CPU *cpu, uint32_t stack, uint32_t parent_return) {
    cpu->r[4] = stack;
    cpu->r[6] = CRT_STREAM;
    M32(stack + 0x348) = parent_return;
    M32(stack + 0x34c) = CRT_STREAM;
    const uint32_t destination = stack + 0x98;
    memset(MEM + destination, 0, CRT_REQUEST);
    M32(CRT_BSFILE + 0x18) = 0;
    slot_result = UINT64_MAX;
    classic_runtime_fixture_crt_read(cpu);
    assert(cpu->r[0] == CRT_REQUEST && cpu->r[4] == stack && cpu->r[6] == CRT_STREAM);
    assert(M32(CRT_BSFILE + 0x18) == CRT_REQUEST && !memcmp(MEM + destination, MEM + CRT_BUFFER, CRT_REQUEST));
    return slot_result;
}

static void check_crt_link(CPU *cpu, uint32_t stack, int dir, const char *directory) {
    ClassicRuntimeLoad load;
    const uint32_t api = stack - 512, slot = stack - 1024;
    uint32_t handle = open_for_crt(cpu, stack, dir, directory, &load);
    M32(api) = 0xF5835Au;
    M32(api + 4) = CRT_FILE;
    cpu->r[4] = api;
    f_F680D9(cpu);
    assert(cpu->r[0] == CRT_FILENO && cpu->r[4] == api + 4);

    assert(!classic_runtime_load_bind_crt(cpu, CRT_STREAM + 4));
    assert(classic_runtime_load_bind_crt(cpu, CRT_STREAM));
    assert(!classic_runtime_load_bind_crt(cpu, CRT_STREAM));
    const uint64_t generation = classic_runtime_load_generation(cpu, CRT_STREAM, handle);
    assert(generation && call_crt_slot(cpu, slot, 0x68227au) == generation);
    assert(!call_crt_slot(cpu, slot, 0x68227bu)); /* the fixed read still returns */
    const uint32_t foreign = open_readonly(cpu, api, other);
    assert(foreign != handle);
    M32(CRT_ENTRY) = foreign;
    assert(!call_crt_slot(cpu, slot, 0x68227au));
    M32(CRT_ENTRY) = handle;
    assert(!call_crt_slot(cpu, slot, 0x68227au)); /* restored numbers can't refresh evidence */
    close_file(cpu, api, foreign);
    close_file(cpu, api, handle);
    classic_runtime_load_end(&load);

    handle = open_for_crt(cpu, stack, dir, directory, &load);
    assert(classic_runtime_load_bind_crt(cpu, CRT_STREAM));
    close_file(cpu, api, handle);
    const uint32_t reused = open_readonly(cpu, api, fixture);
    assert(reused == handle && !call_crt_slot(cpu, slot, 0x68227au));
    close_file(cpu, api, reused);
    classic_runtime_load_end(&load);

    handle = open_for_crt(cpu, stack, dir, directory, &load);
    assert(classic_runtime_load_bind_crt(cpu, CRT_STREAM));
    replace_descriptor_during_copy = true;
    assert(!call_crt_slot(cpu, slot, 0x68227au));
    replace_descriptor_during_copy = false;
    close_file(cpu, api, handle);
    classic_runtime_load_end(&load);

    handle = open_for_crt(cpu, stack, dir, directory, &load);
    assert(classic_runtime_load_bind_crt(cpu, CRT_STREAM));
    const uint32_t other_file = CRT_FILE + 0x20;
    M32(CRT_FILE_TABLE + 5 * 4) = other_file;
    M32(other_file + 0xc) = 1;
    M32(other_file + 0x10) = CRT_FILENO;
    M32(CRT_BSFILE + 0x24) = other_file;
    assert(!call_crt_slot(cpu, slot, 0x68227au)); /* same OS handle, different CRT object */
    close_file(cpu, api, handle);
    classic_runtime_load_end(&load);

    handle = open_for_crt(cpu, stack, dir, directory, &load);
    M32(0x1bc0320u) = 2080;
    assert(!classic_runtime_load_bind_crt(cpu, CRT_STREAM));
    M32(0x1bc0320u) = 32;
    assert(!classic_runtime_load_bind_crt(cpu, CRT_STREAM));
    close_file(cpu, api, handle);
    classic_runtime_load_end(&load);
    puts("PASS pinned buffered read with the selected CRT and native file identities");
}
#endif


int main(int argc, char **argv) {
    assert(argc == 2);
#if SKYRIM_TEST_PINNED_CRT
    const size_t map_size = GUEST_HEAP_BASE + 0x6000;
#else
    const size_t map_size = GUEST_TEB_BASE + 0x4000;
#endif
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

#if SKYRIM_TEST_PINNED_CRT
    check_crt_link(&cpu, stack, dir, directory);
#endif
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
