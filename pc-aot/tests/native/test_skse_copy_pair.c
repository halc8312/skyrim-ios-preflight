/* SPDX-License-Identifier: GPL-3.0-only */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _GNU_SOURCE 1
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

static ssize_t observed_read(int fd, void *bytes, size_t size, off_t offset);
static ssize_t observed_write(int fd, const void *bytes, size_t size);
static int observed_sync(int fd);
static int observed_mkdir(int dir, const char *name, mode_t mode);
static int observed_rename(int from_dir, const char *from, int to_dir, const char *to, unsigned flags);
#if defined(__APPLE__)
static int observed_setxattr(int fd, const char *name, const void *value, size_t size, uint32_t position, int flags);
#else
static int observed_setxattr(int fd, const char *name, const void *value, size_t size, int flags);
#endif
#define pread observed_read
#define write observed_write
#define fsync observed_sync
#define mkdirat observed_mkdir
#define fsetxattr observed_setxattr
#if defined(__APPLE__)
#define renameatx_np observed_rename
#else
#define renameat2 observed_rename
#endif
#include "../../experiments/skse-cosave/copy_pair.c"
#undef pread
#undef write
#undef fsync
#undef mkdirat
#undef renameatx_np
#undef renameat2
#undef fsetxattr

enum Fault {
    NO_FAULT, SHORT_IO, SCRATCH_ERROR, READ_ERROR, WRITE_ERROR, SYNC_ERROR,
    MODIFY_BASE, REPLACE_BASE, SIDE_PUBLISH_ERROR, BASE_PUBLISH_ERROR,
    BASE_COLLISION, FOREIGN_SIDE_REPLACEMENT, INTERRUPT_BEFORE_BASE, INTERRUPT_WITHOUT_PROOF
};

typedef struct { int source, destination, base, side; } Fixture;
static const char base_name[] = "Case Save.ESS", side_name[] = "case save.SKSE";
static const char other[] = "unchanged foreign content";
static unsigned char base_bytes[85003], side_bytes[42027];
static Fixture *running;
static enum Fault error_mode;
static unsigned reads, writes, syncs, publications;
static int resuming;

static int open_dir(int parent, const char *name) {
    int dir = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    assert(dir >= 0);
    return dir;
}

static void create_data(int dir, const char *name, const void *data, size_t size) {
    int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    assert(fd >= 0);
    assert(write(fd, data, size) == (ssize_t)size);
    assert(close(fd) == 0);
}

static int exists(int dir, const char *name) {
    struct stat file;
    if (!fstatat(dir, name, &file, AT_SYMLINK_NOFOLLOW)) return 1;
    assert(errno == ENOENT);
    return 0;
}

static void expect_bytes(int dir, const char *name, const void *data, size_t size) {
    int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    assert(fd >= 0);
    struct stat st;
    assert(!fstat(fd, &st) && st.st_size == (off_t)size && st.st_nlink == 1);
    unsigned char chunk[4096];
    const unsigned char *expected = data;
    for (size_t offset = 0; offset < size;) {
        ssize_t bytes = read(fd, chunk, sizeof chunk);
        assert(bytes > 0 && (size_t)bytes <= size - offset);
        assert(!memcmp(expected + offset, chunk, (size_t)bytes));
        offset += (size_t)bytes;
    }
    assert(close(fd) == 0);
}

static void expect_entries(int dir, unsigned expected) {
    DIR *reader = fdopendir(open_dir(dir, "."));
    assert(reader);
    unsigned used = 0;
    for (struct dirent *entry; (entry = readdir(reader));)
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) ++used;
    assert(!closedir(reader));
    assert(used == expected);
}

static Fixture new_fixture(int root, const char *name, int include_side) {
    assert(!mkdirat(root, name, 0700));
    int group = open_dir(root, name);
    assert(!mkdirat(group, "source", 0700) && !mkdirat(group, "destination", 0700));
    Fixture fixture = {open_dir(group, "source"), open_dir(group, "destination"), -1, -1};
    assert(close(group) == 0);
    create_data(fixture.source, base_name, base_bytes, sizeof base_bytes);
    fixture.base = openat(fixture.source, base_name, O_RDONLY | O_NOFOLLOW);
    assert(fixture.base >= 0);
    if (include_side) {
        create_data(fixture.source, side_name, side_bytes, sizeof side_bytes);
        fixture.side = openat(fixture.source, side_name, O_RDONLY | O_NOFOLLOW);
        assert(fixture.side >= 0);
    }
    return fixture;
}

static void close_fixture(Fixture f) {
    if (f.side >= 0) assert(close(f.side) == 0);
    assert(close(f.base) == 0 && close(f.source) == 0 && close(f.destination) == 0);
}

static int copy(Fixture *fixture, enum Fault fault) {
    running = fixture;
    error_mode = fault;
    resuming = fixture->side >= 0 && exists(fixture->destination, side_name);
    reads = writes = syncs = publications = 0;
    assert(lseek(fixture->base, 17, SEEK_SET) == 17);
    if (fixture->side >= 0) assert(lseek(fixture->side, 21, SEEK_SET) == 21);
    int result = skse_copy_save_pair(fixture->source, fixture->base, fixture->side,
                                    fixture->destination, "CASE SAVE.ess");
    assert(lseek(fixture->base, 0, SEEK_CUR) == 17);
    if (fixture->side >= 0) assert(lseek(fixture->side, 0, SEEK_CUR) == 21);
    running = NULL;
    return result;
}

static void alter_base(int source) {
    int writer = openat(source, base_name, O_WRONLY | O_NOFOLLOW);
    struct stat before;
    assert(writer >= 0 && !fstat(writer, &before));
    unsigned char value = base_bytes[0] ^ 0xed;
    assert(pwrite(writer, &value, 1, 0) == 1);
#if defined(__APPLE__)
    const struct timespec time[2] = {before.st_atimespec, before.st_mtimespec};
#else
    const struct timespec time[2] = {before.st_atim, before.st_mtim};
#endif
    assert(!futimens(writer, time) && !close(writer));
}

static ssize_t observed_read(int fd, void *bytes, size_t size, off_t offset) {
    if (error_mode == SHORT_IO && reads++ == 0) { errno = EINTR; return -1; }
    if (running && fd == running->side && offset == 0) {
        if (error_mode == READ_ERROR) { errno = EIO; return -1; }
        if (error_mode == MODIFY_BASE) alter_base(running->source);
        if (error_mode == REPLACE_BASE) {
            create_data(running->source, "new-base", base_bytes, sizeof base_bytes);
            assert(!renameat(running->source, "new-base", running->source, base_name));
        }
    }
    return pread(fd, bytes, size, offset);
}

static ssize_t observed_write(int fd, const void *bytes, size_t size) {
    if (error_mode == WRITE_ERROR) { errno = ENOSPC; return -1; }
    if (error_mode == SHORT_IO) {
        if (writes++ == 0) return write(fd, bytes, size / 2);
        if (writes == 2) { errno = EINTR; return -1; }
    }
    return write(fd, bytes, size);
}

static int observed_sync(int fd) {
    if (error_mode == SYNC_ERROR && ++syncs == 2) { errno = EIO; return -1; }
    return fsync(fd);
}

static int observed_mkdir(int dir, const char *name, mode_t mode) {
    if (error_mode == SCRATCH_ERROR) { errno = ENOSPC; return -1; }
    return mkdirat(dir, name, mode);
}

#if defined(__APPLE__)
static int observed_setxattr(int fd, const char *name, const void *value, size_t size, uint32_t position, int flags) {
    if (error_mode == INTERRUPT_WITHOUT_PROOF) { errno = ENOTSUP; return -1; }
    return fsetxattr(fd, name, value, size, position, flags);
}
#else
static int observed_setxattr(int fd, const char *name, const void *value, size_t size, int flags) {
    if (error_mode == INTERRUPT_WITHOUT_PROOF) { errno = ENOTSUP; return -1; }
    return fsetxattr(fd, name, value, size, flags);
}
#endif

static int observed_rename(int from_dir, const char *from, int to_dir, const char *to, unsigned flags) {
    if (running) {
        assert(to_dir == running->destination);
        ++publications;
        assert(!exists(to_dir, base_name));
        if (!strcmp(from, "side")) {
            assert(!strcmp(to, side_name) && publications == 1);
            if (error_mode == SIDE_PUBLISH_ERROR) { errno = EIO; return -1; }
        } else {
            assert(!strcmp(from, "base") && !strcmp(to, base_name));
            assert(publications == (running->side >= 0 && !resuming ? 2u : 1u));
            if (running->side >= 0) expect_bytes(to_dir, side_name, side_bytes, sizeof side_bytes);
            if (error_mode == INTERRUPT_BEFORE_BASE || error_mode == INTERRUPT_WITHOUT_PROOF) _exit(77);
            if (error_mode == BASE_PUBLISH_ERROR) { errno = EACCES; return -1; }
            if (error_mode == BASE_COLLISION) create_data(to_dir, base_name, other, sizeof other);
            if (error_mode == FOREIGN_SIDE_REPLACEMENT) {
                assert(!unlinkat(to_dir, side_name, 0));
                create_data(to_dir, side_name, other, sizeof other);
                errno = EACCES;
                return -1;
            }
        }
    }
#if defined(__APPLE__)
    assert(flags == RENAME_EXCL);
    return renameatx_np(from_dir, from, to_dir, to, flags);
#else
    assert(flags == RENAME_NOREPLACE);
    return renameat2(from_dir, from, to_dir, to, flags);
#endif
}

static void success_cases(int root) {
    Fixture pair = new_fixture(root, "paired", 1);
    char stale[64];
    snprintf(stale, sizeof stale, ".skyrimios-save-copy-%ld-0", (long)getpid());
    assert(!mkdirat(pair.destination, stale, 0700));
    int old = open_dir(pair.destination, stale);
    create_data(old, "base", other, sizeof other);
    create_data(pair.destination, "Existing.ess", other, sizeof other);
    struct stat before_base, before_side, after;
    assert(!fstat(pair.base, &before_base) && !fstat(pair.side, &before_side));
    assert(!copy(&pair, SHORT_IO) && reads > 1 && writes > 2 && publications == 2);
    expect_bytes(pair.destination, base_name, base_bytes, sizeof base_bytes);
    expect_bytes(pair.destination, side_name, side_bytes, sizeof side_bytes);
    expect_bytes(pair.destination, "Existing.ess", other, sizeof other);
    expect_bytes(old, "base", other, sizeof other);
    expect_entries(pair.destination, 4);
    assert(!fstat(pair.base, &after) && skse_save_same_version(&before_base, &after));
    assert(!fstat(pair.side, &after) && skse_save_same_version(&before_side, &after));
    assert(copy(&pair, NO_FAULT) == EEXIST && !publications);
    assert(!close(old));
    close_fixture(pair);

    Fixture base_only = new_fixture(root, "vanilla", 0);
    assert(!copy(&base_only, NO_FAULT) && publications == 1);
    expect_bytes(base_only.destination, base_name, base_bytes, sizeof base_bytes);
    expect_entries(base_only.destination, 1);
    close_fixture(base_only);
}

static void wrong_args_and_collisions(int root) {
    Fixture clean = new_fixture(root, "arguments", 1);
    const char *bad[] = {NULL, "", ".hidden.ess", ".ess", "../Case Save.ess", "a\\b.ess", "a\n.ess", "x.skse"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i)
        assert(skse_copy_save_pair(clean.source, clean.base, clean.side, clean.destination, bad[i]) == EINVAL);
    assert(skse_copy_save_pair(clean.source, clean.base, clean.side, clean.source, base_name) == EINVAL);
    assert(skse_copy_save_pair(clean.source, clean.base, -1, clean.destination, base_name) == EINVAL);
    assert(skse_copy_save_pair(clean.source, clean.side, clean.base, clean.destination, base_name) == ESTALE);
    int wrong_handle = openat(clean.source, base_name, O_RDWR | O_NOFOLLOW);
    assert(wrong_handle >= 0);
    assert(skse_copy_save_pair(clean.source, wrong_handle, clean.side, clean.destination, base_name) == EINVAL);
    assert(!close(wrong_handle));
    expect_entries(clean.destination, 0);
    close_fixture(clean);

    Fixture occupied = new_fixture(root, "occupied-base", 1);
    create_data(occupied.destination, "case SAVE.ess", other, sizeof other);
    assert(copy(&occupied, NO_FAULT) == EEXIST && !publications);
    expect_bytes(occupied.destination, "case SAVE.ess", other, sizeof other);
    expect_entries(occupied.destination, 1);
    close_fixture(occupied);

    Fixture orphan = new_fixture(root, "occupied-side", 0);
    create_data(orphan.destination, "CASE SAVE.skse", other, sizeof other);
    assert(copy(&orphan, NO_FAULT) == EEXIST && !publications);
    expect_bytes(orphan.destination, "CASE SAVE.skse", other, sizeof other);
    expect_entries(orphan.destination, 1);
    close_fixture(orphan);
}

static void fail_cases(int root) {
    const struct { const char *name; enum Fault fault; int expected; unsigned attempts; } cases[] = {
        {"scratch-error", SCRATCH_ERROR, ENOSPC, 0}, {"read-error", READ_ERROR, EIO, 0},
        {"write-error", WRITE_ERROR, ENOSPC, 0}, {"sync-error", SYNC_ERROR, EIO, 0},
        {"write-source", MODIFY_BASE, ESTALE, 0}, {"replace-source", REPLACE_BASE, ESTALE, 0},
        {"side-error", SIDE_PUBLISH_ERROR, EIO, 1}, {"base-error", BASE_PUBLISH_ERROR, EACCES, 2},
        {"base-collision", BASE_COLLISION, EEXIST, 2}, {"side-replaced", FOREIGN_SIDE_REPLACEMENT, EIO, 2},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        Fixture f = new_fixture(root, cases[i].name, 1);
        create_data(f.destination, "Existing.ess", other, sizeof other);
        int result = copy(&f, cases[i].fault);
        if (result != cases[i].expected) fprintf(stderr, "%s result=%d\n", cases[i].name, result);
        assert(result == cases[i].expected && publications == cases[i].attempts);
        expect_bytes(f.destination, "Existing.ess", other, sizeof other);
        if (cases[i].fault == BASE_COLLISION) expect_bytes(f.destination, base_name, other, sizeof other);
        else assert(!exists(f.destination, base_name));
        if (cases[i].fault == FOREIGN_SIDE_REPLACEMENT) expect_bytes(f.destination, side_name, other, sizeof other);
        else assert(!exists(f.destination, side_name));
        expect_entries(f.destination, cases[i].fault == BASE_COLLISION || cases[i].fault == FOREIGN_SIDE_REPLACEMENT ? 2 : 1);
        close_fixture(f);
    }
}

static void interrupt_copy(Fixture *fixture, enum Fault fault) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        (void)copy(fixture, fault);
        _exit(78);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
    assert(!exists(fixture->destination, base_name));
    expect_bytes(fixture->destination, side_name, side_bytes, sizeof side_bytes);
    expect_entries(fixture->destination, 2); /* published companion and hidden scratch */
}

static int open_interrupted_side(const Fixture *fixture, CopyProof *proof) {
    int fd = openat(fixture->destination, side_name, O_RDONLY | O_NOFOLLOW);
    assert(fd >= 0 && read_proof(fd, proof));
    return fd;
}

static void update_proof(int fd, const CopyProof *proof) {
#if defined(__APPLE__)
    assert(!fsetxattr(fd, proof_attribute, proof, sizeof *proof, 0, 0));
#else
    assert(!fsetxattr(fd, proof_attribute, proof, sizeof *proof, 0));
#endif
}

static void expect_interrupted_collision(Fixture *fixture) {
    assert(copy(fixture, NO_FAULT) == EEXIST && !publications);
    assert(!exists(fixture->destination, base_name));
    expect_bytes(fixture->destination, side_name, side_bytes, sizeof side_bytes);
    expect_entries(fixture->destination, 2);
}

static void interrupted_process(int root) {
    Fixture f = new_fixture(root, "interrupted-copy", 1);
    interrupt_copy(&f, INTERRUPT_BEFORE_BASE);
    CopyProof proof;
    int side = open_interrupted_side(&f, &proof);
    struct stat before, after;
    assert(!fstat(side, &before));
    assert(copy(&f, BASE_PUBLISH_ERROR) == EACCES && publications == 1);
    assert(!exists(f.destination, base_name));
    expect_bytes(f.destination, side_name, side_bytes, sizeof side_bytes);
    expect_entries(f.destination, 2);
    assert(!copy(&f, NO_FAULT) && publications == 1);
    assert(!fstatat(f.destination, side_name, &after, AT_SYMLINK_NOFOLLOW) && skse_save_same_version(&before, &after));
    expect_bytes(f.destination, base_name, base_bytes, sizeof base_bytes);
    expect_bytes(f.destination, side_name, side_bytes, sizeof side_bytes);
    expect_entries(f.destination, 2);
    assert(copy(&f, NO_FAULT) == EEXIST && !publications);
    assert(!close(side));
    close_fixture(f);

    f = new_fixture(root, "unsupported-proof", 1);
    interrupt_copy(&f, INTERRUPT_WITHOUT_PROOF);
    expect_interrupted_collision(&f);
    close_fixture(f);

    f = new_fixture(root, "tampered-proof", 1);
    interrupt_copy(&f, INTERRUPT_BEFORE_BASE);
    side = open_interrupted_side(&f, &proof);
    proof.nonce[0] ^= 1;
    update_proof(side, &proof);
    expect_interrupted_collision(&f);
    assert(!close(side));
    close_fixture(f);

    f = new_fixture(root, "foreign-proof", 1);
    interrupt_copy(&f, INTERRUPT_BEFORE_BASE);
    side = open_interrupted_side(&f, &proof);
    assert(!unlinkat(f.destination, side_name, 0));
    create_data(f.destination, side_name, side_bytes, sizeof side_bytes);
    int replacement = openat(f.destination, side_name, O_RDONLY | O_NOFOLLOW);
    assert(replacement >= 0);
    update_proof(replacement, &proof);
    expect_interrupted_collision(&f);
    assert(!close(replacement) && !close(side));
    close_fixture(f);

    f = new_fixture(root, "replace-staged-base", 1);
    interrupt_copy(&f, INTERRUPT_BEFORE_BASE);
    side = open_interrupted_side(&f, &proof);
    int scratch = open_dir(f.destination, proof.directory);
    create_data(scratch, "replacement", base_bytes, sizeof base_bytes);
    assert(!renameat(scratch, "replacement", scratch, "base"));
    expect_interrupted_collision(&f);
    assert(!close(scratch) && !close(side));
    close_fixture(f);

    f = new_fixture(root, "changed-source-proof", 1);
    interrupt_copy(&f, INTERRUPT_BEFORE_BASE);
    Fixture other_source = new_fixture(root, "different-profile", 1);
    assert(skse_copy_save_pair(other_source.source, other_source.base, other_source.side,
                               f.destination, base_name) == EEXIST);
    close_fixture(other_source);
    alter_base(f.source);
    expect_interrupted_collision(&f);
    close_fixture(f);

    f = new_fixture(root, "identical-source-replacement", 1);
    interrupt_copy(&f, INTERRUPT_BEFORE_BASE);
    create_data(f.source, "new-base", base_bytes, sizeof base_bytes);
    assert(!renameat(f.source, "new-base", f.source, base_name));
    int new_base = openat(f.source, base_name, O_RDONLY | O_NOFOLLOW);
    assert(new_base >= 0 && !close(f.base));
    f.base = new_base;
    expect_interrupted_collision(&f);
    close_fixture(f);

    f = new_fixture(root, "moved-staging-and-side", 1);
    interrupt_copy(&f, INTERRUPT_BEFORE_BASE);
    side = open_interrupted_side(&f, &proof);
    Fixture other_destination = new_fixture(root, "different-target", 0);
    assert(!renameat(f.destination, proof.directory, other_destination.destination, proof.directory));
    assert(!renameat(f.destination, side_name, other_destination.destination, side_name));
    assert(skse_copy_save_pair(f.source, f.base, f.side, other_destination.destination, base_name) == EEXIST);
    assert(!exists(other_destination.destination, base_name));
    expect_bytes(other_destination.destination, side_name, side_bytes, sizeof side_bytes);
    expect_entries(other_destination.destination, 2);
    assert(!close(side));
    close_fixture(other_destination);
    close_fixture(f);

    f = new_fixture(root, "interrupted-base-conflict", 1);
    interrupt_copy(&f, INTERRUPT_BEFORE_BASE);
    create_data(f.destination, "CASE SAVE.ess", other, sizeof other);
    assert(copy(&f, NO_FAULT) == EEXIST && !publications);
    expect_bytes(f.destination, "CASE SAVE.ess", other, sizeof other);
    expect_bytes(f.destination, side_name, side_bytes, sizeof side_bytes);
    expect_entries(f.destination, 3);
    close_fixture(f);
}

static void named_profile_paths(int root) {
    Fixture f = new_fixture(root, "named", 1);
    const char *const src = "named/source", *const dst = "named/destination/Documents/My Games/Skyrim/Saves";
    int side = 9;
    assert(!skse_copy_named_save(root, src, dst, base_name, &side) && side == 1);
    int copied = open_dir(root, dst);
    expect_bytes(copied, base_name, base_bytes, sizeof base_bytes);
    expect_bytes(copied, side_name, side_bytes, sizeof side_bytes);
    expect_entries(copied, 2);
    assert(skse_copy_named_save(root, src, dst, base_name, &side) == EEXIST && !side);
    assert(!close(copied));
    assert(skse_copy_named_save(root, src, "named/new/../wrong", base_name, &side) == EINVAL && !side);
    assert(!exists(root, "named/new"));
    assert(skse_copy_named_save(root, src, "/outside", base_name, &side) == EINVAL && !side);
    assert(skse_copy_named_save(root, src, "named/absent", "missing.ess", &side) == ENOENT && !side);
    assert(!exists(root, "named/absent"));
    assert(!symlinkat("destination", root, "named/link"));
    assert(skse_copy_named_save(root, src, "named/link/escaped", base_name, &side) != 0 && !side);
    assert(!exists(f.destination, "escaped"));
    expect_bytes(f.source, base_name, base_bytes, sizeof base_bytes);
    expect_bytes(f.source, side_name, side_bytes, sizeof side_bytes);
    close_fixture(f);

    Fixture plain = new_fixture(root, "named-plain", 0);
    assert(!skse_copy_named_save(root, "named-plain/source", "named-plain/destination", base_name, &side) && !side);
    expect_bytes(plain.destination, base_name, base_bytes, sizeof base_bytes);
    expect_entries(plain.destination, 1);
    close_fixture(plain);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    for (size_t i = 0; i < sizeof base_bytes; ++i) base_bytes[i] = (unsigned char)(i * 13u + i / 17u);
    for (size_t i = 0; i < sizeof side_bytes; ++i) side_bytes[i] = (unsigned char)(i * 53u + i / 31u);
    int root = open_dir(AT_FDCWD, argv[1]);
    success_cases(root);
    wrong_args_and_collisions(root);
    fail_cases(root);
    interrupted_process(root);
    named_profile_paths(root);
    assert(!close(root));
    puts("classic_profile_copy=PASS");
    return 0;
}
