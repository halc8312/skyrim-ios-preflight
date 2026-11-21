/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE 1
#include "fresh_load.h"
#include "rt.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

uint8_t *MEM;
void classic_runtime_fixture_attach(CPU *);

enum { STREAM = 0x22000, SELECTED_OPEN_RETURN = 0x682225, PATH_OFFSET = 0x14 };

static void expect_rejected(const CPU *cpu, uint32_t path, const char *directory) {
    char canonical[CLASSIC_FRESH_PATH_BYTES] = "stale";
    errno = ERANGE;
    assert(!classic_fresh_resolve_selected_guest_path(cpu, path, directory, canonical));
    assert(!canonical[0] && errno == ERANGE);
}

static void write_fixture(int dir, const char *leaf) {
    const int fd = openat(dir, leaf, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    assert(fd >= 0 && write(fd, "fixture", 7) == 7 && !close(fd));
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const size_t mapped = GUEST_TEB_BASE + 0x2000;
    MEM = mmap(NULL, mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(MEM != MAP_FAILED);
    const int saved = snprintf(g_save_dir, sizeof g_save_dir, "%s", argv[1]);
    assert(saved > 0 && (size_t)saved < sizeof g_save_dir);
    const int root = open(g_save_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    assert(root >= 0 && !mkdirat(root, "Saves", 0700) && !mkdirat(root, "SavesOther", 0700));
    const int dir = openat(root, "Saves", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    assert(dir >= 0 && !mkdirat(dir, "Other.ess", 0700) && !mkdirat(dir, "nested", 0700));
    const int nested = openat(dir, "nested", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    const int sibling = openat(root, "SavesOther", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    assert(nested >= 0 && sibling >= 0);
    write_fixture(dir, "Fixture.ESS");
    write_fixture(dir, "Fixture.skse");
    write_fixture(nested, "Nested.ess");
    write_fixture(sibling, "Outside.ess");
    assert(!symlinkat("../SavesOther/Outside.ess", dir, "Link.ess"));
    struct stat link;
    assert(!fstatat(dir, "Link.ess", &link, 0) && S_ISREG(link.st_mode));
    assert(!fstatat(dir, "Link.ess", &link, AT_SYMLINK_NOFOLLOW) && S_ISLNK(link.st_mode));
    char directory[CLASSIC_FRESH_PATH_BYTES], expected[CLASSIC_FRESH_PATH_BYTES];
    const int prefix = snprintf(directory, sizeof directory, "%s/Saves", g_save_dir);
    const int size = snprintf(expected, sizeof expected, "%s/Fixture.ESS", directory);
    assert(prefix > 0 && (size_t)prefix < sizeof directory && size > 0 && (size_t)size < sizeof expected);

    CPU cpu = {0};
    classic_runtime_fixture_attach(&cpu);
    const uint32_t stack = cpu.r[4] - 512;
    cpu.r[1] = cpu.r[6] = STREAM;
    cpu.r[4] = stack;
    M32(stack) = SELECTED_OPEN_RETURN;
    const uint32_t path = stack + PATH_OFFSET;
    strcpy(GSTR(path), "C:\\Users\\Player\\saves\\fixture.ess");
    char canonical[CLASSIC_FRESH_PATH_BYTES];
    errno = ERANGE;
    assert(classic_fresh_resolve_selected_guest_path(&cpu, path, directory, canonical));
    assert(!strcmp(canonical, expected) && errno == ERANGE);

    CPU foreign = cpu;
    expect_rejected(&foreign, path, directory);
    expect_rejected(&cpu, path + 1, directory);
    cpu.r[1] = 0;
    expect_rejected(&cpu, path, directory);
    cpu.r[1] = STREAM;
    ++cpu.fs_base;
    expect_rejected(&cpu, path, directory);
    --cpu.fs_base;
    --M32(cpu.fs_base + 4);
    expect_rejected(&cpu, path, directory);
    ++M32(cpu.fs_base + 4);
    cpu.r[4] = M32(cpu.fs_base + 4) - 4;
    expect_rejected(&cpu, cpu.r[4] + PATH_OFFSET, directory);
    cpu.r[4] = stack;
    M32(stack) = 0x68224b; /* The later check is a different call site. */
    expect_rejected(&cpu, path, directory);
    M32(stack) = SELECTED_OPEN_RETURN;

    static const char *const refused_paths[] = {
        "C:\\Users\\Player\\Saves\\Missing.ess", "C:\\Users\\Player\\Saves\\Other.ess",
        "C:\\Users\\Player\\Saves\\Fixture.skse", "C:\\Users\\Player\\Saves\\Link.ess",
        "C:\\Users\\Player\\SavesOther\\Outside.ess", "C:\\Users\\Player\\Saves\\nested\\Nested.ess",
        "C:\\Users\\Player\\SavesOther\\..\\Saves\\Fixture.ESS",
    };
    for (size_t i = 0; i < sizeof refused_paths / sizeof refused_paths[0]; ++i) {
        strcpy(GSTR(path), refused_paths[i]);
        expect_rejected(&cpu, path, directory);
    }
    memset(GSTR(path), 'x', 260);
    expect_rejected(&cpu, path, directory);
    assert(!close(sibling) && !close(nested) && !close(dir) && !close(root));
    assert(!munmap(MEM, mapped));
    puts("PASS selected load path, active guest frame and case-preserving profile mapping");
    return 0;
}
