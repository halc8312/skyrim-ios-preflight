/* SPDX-License-Identifier: GPL-3.0-only */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _GNU_SOURCE 1
#endif
#include "copy_pair.h"
#include "save_paths.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(__APPLE__) || defined(__linux__)
#include <sys/xattr.h>
#endif
#if defined(__linux__)
#include <sys/random.h>
#endif

static int inspect_source(int dir, const char *name, int fd, struct stat *info) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return errno;
    if ((flags & O_ACCMODE) != O_RDONLY) return EINVAL;
    struct stat named;
    if (fstat(fd, info) || fstatat(dir, name, &named, AT_SYMLINK_NOFOLLOW)) return errno;
    if (!S_ISREG(info->st_mode) || info->st_size <= 0 || info->st_nlink != 1) return EINVAL;
    return skse_save_same_version(info, &named) ? 0 : ESTALE;
}

static int verify_source(int dir, const char *name, int fd, const struct stat *before) {
    struct stat descriptor, named;
    if (fstat(fd, &descriptor)) return errno;
    if (fstatat(dir, name, &named, AT_SYMLINK_NOFOLLOW)) return errno == ENOENT ? ESTALE : errno;
    return skse_save_same_version(before, &descriptor) && skse_save_same_version(before, &named) ? 0 : ESTALE;
}

typedef struct {
    int dir, base_fd, side_fd;
    struct stat directory, base, side;
    SkseSaveNames names;
} CopySource;

static int verify_copy_source(const CopySource *source) {
    struct stat current;
    if (fstat(source->dir, &current)) return errno;
    if (!skse_save_same_version(&source->directory, &current)) return ESTALE;
    int error = verify_source(source->dir, source->names.base, source->base_fd, &source->base);
    if (!error && source->side_fd >= 0)
        error = verify_source(source->dir, source->names.side, source->side_fd, &source->side);
    return error;
}

static int check_destination(int dir, const char *name, size_t stem) {
    SkseSaveNames collision;
    int error = skse_save_find_names(dir, name, stem, &collision);
    if (error) return error;
    return collision.base[0] || collision.side[0] ? EEXIST : 0;
}

static int remove_owned(int dir, const char *name, const struct stat *owned, int flags) {
    struct stat current;
    if (fstatat(dir, name, &current, AT_SYMLINK_NOFOLLOW)) return errno == ENOENT ? 0 : errno;
    if (!skse_save_same_file(owned, &current)) return ESTALE;
    return unlinkat(dir, name, flags) == 0 ? 0 : errno;
}

typedef struct {
    char name[64];
    int fd;
    struct stat identity;
} Scratch;
static const char scratch_prefix[] = ".skyrimios-save-copy-";

static int create_scratch(int dir, Scratch *scratch) {
    for (unsigned attempt = 0; attempt <= SKSE_SAVE_DIRECTORY_LIMIT; ++attempt) {
        snprintf(scratch->name, sizeof scratch->name, "%s%ld-%u", scratch_prefix, (long)getpid(), attempt);
        if (mkdirat(dir, scratch->name, 0700)) {
            if (errno == EEXIST) continue;
            return errno;
        }
        if (fstatat(dir, scratch->name, &scratch->identity, AT_SYMLINK_NOFOLLOW)) return EIO;
        int fd = openat(dir, scratch->name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        struct stat opened;
        int error = fd < 0 || fstat(fd, &opened) ? errno : 0;
        if (!error && !skse_save_same_file(&scratch->identity, &opened)) error = ESTALE;
        if (!error) {
            scratch->fd = fd;
            return 0;
        }
        if (fd >= 0) close(fd);
        return remove_owned(dir, scratch->name, &scratch->identity, AT_REMOVEDIR) ? EIO : error;
    }
    return EEXIST;
}

static int clear_scratch(int dir, const Scratch *scratch) {
    int error = 0;
    if (unlinkat(scratch->fd, "base", 0) && errno != ENOENT) error = errno;
    if (unlinkat(scratch->fd, "side", 0) && errno != ENOENT) error = errno;
    int removed = remove_owned(dir, scratch->name, &scratch->identity, AT_REMOVEDIR);
    close(scratch->fd);
    return error ? error : removed;
}

static int write_all(int fd, const unsigned char *data, size_t size) {
    while (size) {
        ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n < 0 ? errno : EIO;
        data += n;
        size -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, unsigned char *data, size_t size, off_t offset) {
    while (size) {
        ssize_t n = pread(fd, data, size, offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n < 0 ? errno : ESTALE;
        data += n;
        size -= (size_t)n;
        offset += n;
    }
    return 0;
}

static int copy_file(int scratch, const char *name, int source, off_t size) {
    int fd = openat(scratch, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return errno;
    unsigned char buffer[32 * 1024];
    int error = 0;
    for (off_t offset = 0; offset < size;) {
        off_t remaining = size - offset;
        size_t chunk = remaining < (off_t)sizeof buffer ? (size_t)remaining : sizeof buffer;
        error = read_all(source, buffer, chunk, offset);
        if (!error) error = write_all(fd, buffer, chunk);
        if (error) break;
        offset += (off_t)chunk;
    }
    if (!error && fsync(fd)) error = errno;
    if (close(fd) && !error) error = errno;
    return error;
}

static int publish_file(int scratch, const char *staged, int target, const char *name) {
#if defined(__APPLE__)
    return renameatx_np(scratch, staged, target, name, RENAME_EXCL) == 0 ? 0 : errno;
#elif defined(__linux__)
    return renameat2(scratch, staged, target, name, RENAME_NOREPLACE) == 0 ? 0 : errno;
#else
    (void)scratch; (void)staged; (void)target; (void)name;
    return ENOTSUP;
#endif
}

#if defined(__APPLE__) || defined(__linux__)
typedef struct { uint64_t device, inode; } CopyIdentity;
typedef struct {
    CopyIdentity source_dir, source_base, source_side, destination_dir;
    CopyIdentity staging, staged_base, copied_side;
    unsigned char nonce[16];
    char directory[sizeof ((Scratch *)0)->name];
    SkseSaveNames names;
} CopyProof;

#if defined(__APPLE__)
static const char proof_attribute[] = "org.skyrimios.save-copy.v1";
#else
static const char proof_attribute[] = "user.org.skyrimios.save-copy.v1";
#endif

static CopyIdentity copy_identity(const struct stat *info) {
    return (CopyIdentity){(uint64_t)info->st_dev, (uint64_t)info->st_ino};
}

static int copy_has_identity(const struct stat *info, CopyIdentity identity) {
    return (uint64_t)info->st_dev == identity.device && (uint64_t)info->st_ino == identity.inode;
}

static int write_proof(int fd, const CopyProof *proof) {
#if defined(__APPLE__)
    return fsetxattr(fd, proof_attribute, proof, sizeof *proof, 0, XATTR_CREATE);
#else
    return fsetxattr(fd, proof_attribute, proof, sizeof *proof, XATTR_CREATE);
#endif
}

static int read_proof(int fd, CopyProof *proof) {
#if defined(__APPLE__)
    ssize_t size = fgetxattr(fd, proof_attribute, proof, sizeof *proof, 0, 0);
#else
    ssize_t size = fgetxattr(fd, proof_attribute, proof, sizeof *proof);
#endif
    return size == (ssize_t)sizeof *proof;
}

static void record_proof(const Scratch *scratch, int side_fd, const struct stat *side,
                         const CopySource *source, const struct stat *destination) {
    struct stat base;
    if (fstatat(scratch->fd, "base", &base, AT_SYMLINK_NOFOLLOW) || !S_ISREG(base.st_mode)) return;
    CopyProof proof;
    memset(&proof, 0, sizeof proof);
#if defined(__APPLE__)
    arc4random_buf(proof.nonce, sizeof proof.nonce);
#else
    if (getentropy(proof.nonce, sizeof proof.nonce)) return;
#endif
    proof.source_dir = copy_identity(&source->directory);
    proof.source_base = copy_identity(&source->base);
    proof.source_side = copy_identity(&source->side);
    proof.destination_dir = copy_identity(destination);
    proof.staging = copy_identity(&scratch->identity);
    proof.staged_base = copy_identity(&base);
    proof.copied_side = copy_identity(side);
    strcpy(proof.directory, scratch->name);
    proof.names = source->names;
    /* Both attributes are needed for a retry. Unsupported attributes leave the
     * ordinary copy and its collision behavior available. */
    if (write_proof(scratch->fd, &proof) == 0) (void)write_proof(side_fd, &proof);
}

static int valid_proof_directory(const CopyProof *proof) {
    size_t length = strnlen(proof->directory, sizeof proof->directory);
    size_t prefix = sizeof scratch_prefix - 1;
    return length > prefix && length < sizeof proof->directory &&
           !strncmp(proof->directory, scratch_prefix, prefix) &&
           strspn(proof->directory + prefix, "0123456789-") == length - prefix;
}

static int contents_match(int source, const struct stat *original, int copied, const struct stat *copy) {
    if (original->st_size != copy->st_size) return 0;
    unsigned char first[32 * 1024], second[sizeof first];
    for (off_t offset = 0; offset < original->st_size;) {
        off_t remaining = original->st_size - offset;
        size_t chunk = remaining < (off_t)sizeof first ? (size_t)remaining : sizeof first;
        if (read_all(source, first, chunk, offset) || read_all(copied, second, chunk, offset) ||
            memcmp(first, second, chunk)) return 0;
        offset += (off_t)chunk;
    }
    return 1;
}

/* An explicit repeat may publish only the previously staged base. Preserve
 * every existing destination file if any proof or file has changed. */
static int recover_copy(int destination, const struct stat *destination_info,
                        const CopySource *source, const char *side_name, size_t stem) {
    const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
    int side = openat(destination, side_name, flags | O_NONBLOCK);
    if (side < 0) return EEXIST;
    int staging = -1, base = -1, error = EEXIST;
    struct stat side_info, stage_info, base_info;
    CopyProof proof, stage_proof;
    if (inspect_source(destination, side_name, side, &side_info) || side_info.st_uid != geteuid() ||
        !read_proof(side, &proof) || !valid_proof_directory(&proof) ||
        !copy_has_identity(&side_info, proof.copied_side) ||
        !copy_has_identity(destination_info, proof.destination_dir) ||
        !copy_has_identity(&source->directory, proof.source_dir) ||
        !copy_has_identity(&source->base, proof.source_base) ||
        !copy_has_identity(&source->side, proof.source_side) ||
        strncmp(proof.names.base, source->names.base, sizeof proof.names.base) ||
        strncmp(proof.names.side, source->names.side, sizeof proof.names.side) || strcmp(side_name, source->names.side))
        goto done;
    staging = openat(destination, proof.directory, flags | O_DIRECTORY);
    if (staging < 0 || fstat(staging, &stage_info) || stage_info.st_uid != geteuid() ||
        (stage_info.st_mode & (S_IRWXG | S_IRWXO)) || !copy_has_identity(&stage_info, proof.staging) ||
        !read_proof(staging, &stage_proof) || memcmp(&proof, &stage_proof, sizeof proof)) goto done;
    base = openat(staging, "base", flags | O_NONBLOCK);
    if (base < 0 || inspect_source(staging, "base", base, &base_info) ||
        base_info.st_uid != geteuid() || !copy_has_identity(&base_info, proof.staged_base)) goto done;
    if (!contents_match(source->base_fd, &source->base, base, &base_info) ||
        !contents_match(source->side_fd, &source->side, side, &side_info)) goto done;

    SkseSaveNames current;
    if (skse_save_find_names(destination, source->names.base, stem, &current) ||
        current.base[0] || strcmp(current.side, side_name) || verify_copy_source(source) ||
        verify_source(destination, side_name, side, &side_info) ||
        verify_source(destination, proof.directory, staging, &stage_info) ||
        verify_source(staging, "base", base, &base_info)) goto done;

    error = publish_file(staging, "base", destination, source->names.base);
    if (!error) (void)remove_owned(destination, proof.directory, &stage_info, AT_REMOVEDIR);
done:
    if (base >= 0) close(base);
    if (staging >= 0) close(staging);
    close(side);
    return error;
}
#endif

int skse_copy_save_pair(int source_dir, int base_fd, int side_fd,
                        int destination_dir, const char *selected_ess) {
    size_t stem = skse_save_stem(selected_ess);
    if (!stem || side_fd < -1) return EINVAL;
    CopySource source = {.dir = source_dir, .base_fd = base_fd, .side_fd = side_fd};
    struct stat destination;
    if (fstat(source_dir, &source.directory) || fstat(destination_dir, &destination)) return errno;
    if (!S_ISDIR(source.directory.st_mode) || !S_ISDIR(destination.st_mode) || skse_save_same_file(&source.directory, &destination))
        return EINVAL;

    const SkseSaveNames *names = &source.names;
    int error = skse_save_find_names(source_dir, selected_ess, stem, &source.names);
    if (error) return error;
    if (!names->base[0]) return ENOENT;
    if ((side_fd >= 0) != (names->side[0] != 0)) return EINVAL;
    error = inspect_source(source_dir, names->base, base_fd, &source.base);
    if (!error && side_fd >= 0) error = inspect_source(source_dir, names->side, side_fd, &source.side);
    if (error) return error;

    SkseSaveNames existing;
    error = skse_save_find_names(destination_dir, names->base, stem, &existing);
    if (error) return error;
    if (existing.base[0]) return EEXIST;
    if (existing.side[0]) {
#if defined(__APPLE__) || defined(__linux__)
        if (side_fd >= 0) return recover_copy(destination_dir, &destination, &source, existing.side, stem);
#endif
        return EEXIST;
    }

    Scratch scratch;
    error = create_scratch(destination_dir, &scratch);
    if (error) return error;
    struct stat staged_side;
    int published_side = 0, side_guard = -1;
    error = copy_file(scratch.fd, "base", base_fd, source.base.st_size);
    if (!error && side_fd >= 0) error = copy_file(scratch.fd, "side", side_fd, source.side.st_size);
    if (!error) error = check_destination(destination_dir, names->base, stem);
    if (!error) error = verify_copy_source(&source);
    if (!error && side_fd >= 0) {
        /* Keep the staged inode allocated until any rollback has finished. */
        side_guard = openat(scratch.fd, "side", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (side_guard < 0 || fstat(side_guard, &staged_side)) error = errno;
        else {
#if defined(__APPLE__) || defined(__linux__)
            record_proof(&scratch, side_guard, &staged_side, &source, &destination);
#endif
            error = publish_file(scratch.fd, "side", destination_dir, names->side);
            published_side = !error;
        }
    }
    if (!error) error = publish_file(scratch.fd, "base", destination_dir, names->base);
    if (error && published_side && remove_owned(destination_dir, names->side, &staged_side, 0)) error = EIO;
    if (side_guard >= 0) close(side_guard);
    int cleanup = clear_scratch(destination_dir, &scratch);
    if (error && cleanup) error = EIO;
    /* After the base is visible, the copy succeeded even if the empty scratch
     * directory could not be removed. It is never reused by another copy. */
    return error;
}

static int valid_relative_directory(const char *path) {
    if (!path || !*path || strnlen(path, PATH_MAX) == PATH_MAX) return 0;
    for (const char *part = path;;) {
        size_t n = strcspn(part, "/");
        if (!n || n > NAME_MAX || (n == 1 && part[0] == '.') ||
            (n == 2 && part[0] == '.' && part[1] == '.')) return 0;
        if (!part[n]) return 1;
        part += n + 1;
    }
}

static int open_save_directory(int root, const char *path, int create) {
    const int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    int dir = openat(root, ".", flags);
    if (dir < 0) return -1;
    for (const char *part = path;;) {
        size_t n = strcspn(part, "/");
        char name[NAME_MAX + 1];
        memcpy(name, part, n);
        name[n] = 0;
        int next = openat(dir, name, flags);
        if (next < 0 && errno == ENOENT && create &&
            (!mkdirat(dir, name, 0700) || errno == EEXIST))
            next = openat(dir, name, flags);
        int error = errno;
        close(dir);
        if (next < 0) { errno = error; return -1; }
        if (!part[n]) return next;
        dir = next;
        part += n + 1;
    }
}

int skse_copy_named_save(int profile_root, const char *source_path, const char *destination_path,
                         const char *selected_ess, int *copied_side) {
    if (!copied_side) return EINVAL;
    *copied_side = 0;
    size_t stem = skse_save_stem(selected_ess);
    if (!stem || !valid_relative_directory(source_path) || !valid_relative_directory(destination_path))
        return EINVAL;
    int source = open_save_directory(profile_root, source_path, 0);
    if (source < 0) return errno;
    SkseSaveNames names;
    int error = skse_save_find_names(source, selected_ess, stem, &names);
    if (!error && !names.base[0]) error = ENOENT;
    int base = -1, side = -1, destination = -1;
    const int read_flags = O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW;
    struct stat info;
    if (!error) {
        base = openat(source, names.base, read_flags);
        error = base < 0 ? errno : inspect_source(source, names.base, base, &info);
    }
    if (!error && names.side[0]) {
        side = openat(source, names.side, read_flags);
        error = side < 0 ? errno : inspect_source(source, names.side, side, &info);
    }
    if (!error) {
        destination = open_save_directory(profile_root, destination_path, 1);
        error = destination < 0 ? errno : skse_copy_save_pair(source, base, side, destination, selected_ess);
    }
    if (!error) *copied_side = side >= 0;
    if (destination >= 0) close(destination);
    if (side >= 0) close(side);
    if (base >= 0) close(base);
    close(source);
    return error;
}
