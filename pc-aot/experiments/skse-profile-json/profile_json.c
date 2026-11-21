/* SPDX-License-Identifier: GPL-3.0-only */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include "profile_json.h"
#include "../skse-cosave/save_paths.h"
#include "../skse-plugin-vm/x86_winfile.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

enum { PATH_BYTES = SKSE_WIN_PATH_UNITS, MAX_DEPTH = 12, MAX_DIRS = 64, MAX_VISITS = 8192 };
enum { DIRECTORY_FLAGS = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW };
static const char pending_name[] = ".skyrimios-json.pending";
static pthread_mutex_t store_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t next_lease = 1;

typedef SkseProfileJsonResult Result;

struct SkseProfileJsonStore {
    int root;
    struct stat root_info;
    SkseProfileJsonIdentity identity;
    char profile[35], owner[67], prefix[PATH_BYTES];
    uint64_t lease;
    size_t file_bytes, total_bytes;
    unsigned files;
};

struct SkseProfileJsonSnapshot {
    uint64_t lease;
    size_t length;
    bool published;
    char path[PATH_BYTES];
    uint8_t bytes[];
};

typedef struct {
    struct stat info;
    char name[NAME_MAX + 1];
} Entry;

typedef struct {
    size_t bytes;
    unsigned files, dirs;
} Usage;

static bool json_name(const char *name) {
    size_t n = strlen(name);
    return n > 5 && skse_win_asset_name_equal(name + n - 5, ".json");
}

static bool skipped_entry(const char *name) {
    return !strcmp(name, ".") || !strcmp(name, "..");
}

static bool normalize(const char *input, char output[PATH_BYTES]) {
    if (!input) return false;
    size_t n = strnlen(input, PATH_BYTES);
    if (!n || n == PATH_BYTES) return false;
    unsigned parts = 0;
    size_t start = 0;
    for (size_t i = 0; i <= n; ++i) {
        unsigned c = (unsigned char)input[i];
        if (c == '/' || !c) {
            size_t length = i - start;
            if (!length || length > NAME_MAX || input[start] == '.' || input[i-1] == '.' ||
                input[i-1] == ' ' || ++parts > MAX_DEPTH) return false;
            start = i + 1;
        } else if (c < 0x20 || c > 0x7e || strchr("\\:\"<>|?*", (int)c)) {
            return false;
        }
        output[i] = (char)skse_win_ascii_lower(c);
    }
    return true;
}

bool skse_profile_json_matches(const SkseProfileJsonStore *store, const SkseProfileJsonIdentity *identity) {
    return store && identity &&
        !memcmp(store->identity.profile, identity->profile, sizeof identity->profile) &&
        !memcmp(store->identity.owner, identity->owner, sizeof identity->owner);
}

static Result path_in_grant(const SkseProfileJsonStore *store, const SkseProfileJsonIdentity *identity,
                            const char *path, char normalized[PATH_BYTES]) {
    if (!store || !identity) return SKSE_PROFILE_JSON_ARGUMENT;
    if (!skse_profile_json_matches(store, identity)) return SKSE_PROFILE_JSON_SCOPE;
    if (!normalize(path, normalized)) return SKSE_PROFILE_JSON_ARGUMENT;
    const char *slash = strrchr(normalized, '/');
    if (!json_name(slash ? slash + 1 : normalized)) return SKSE_PROFILE_JSON_ARGUMENT;
    size_t prefix = strlen(store->prefix);
    return !strncmp(normalized, store->prefix, prefix) && normalized[prefix] == '/' ?
        SKSE_PROFILE_JSON_OK : SKSE_PROFILE_JSON_SCOPE;
}

bool skse_profile_json_allows(const SkseProfileJsonStore *store, const SkseProfileJsonIdentity *identity, const char *path) {
    char normalized[PATH_BYTES];
    return path_in_grant(store, identity, path, normalized) == SKSE_PROFILE_JSON_OK;
}

static void id_name(char *out, char type, const uint8_t *bytes, size_t length) {
    const char digits[] = "0123456789abcdef";
    *out++ = type;
    *out++ = '-';
    for (size_t i = 0; i < length; ++i) {
        *out++ = digits[bytes[i] >> 4];
        *out++ = digits[bytes[i] & 0xf];
    }
    *out = 0;
}

static bool private_object(const struct stat *s, const struct stat *root) {
    return s->st_dev == root->st_dev && s->st_uid == root->st_uid && !(s->st_mode & (S_IRWXG | S_IRWXO));
}

static bool private_directory(const struct stat *s, const struct stat *root) {
    return S_ISDIR(s->st_mode) && private_object(s, root);
}

static bool private_file(const struct stat *s, const struct stat *root) {
    return S_ISREG(s->st_mode) && s->st_nlink == 1 && s->st_size >= 0 && private_object(s, root);
}

SkseProfileJsonResult skse_profile_json_open(int app_dir, const SkseProfileJsonGrant *grant, SkseProfileJsonStore **out) {
    if (!out) return SKSE_PROFILE_JSON_ARGUMENT;
    *out = NULL;
    struct stat root;
    char prefix[PATH_BYTES];
    int flags = fcntl(app_dir, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY || fstat(app_dir, &root) || root.st_uid != geteuid() ||
        !private_directory(&root, &root) || !grant || !normalize(grant->virtual_prefix, prefix) ||
        !grant->file_bytes || grant->file_bytes > SKSE_PROFILE_JSON_MAX_FILE ||
        grant->total_bytes < grant->file_bytes || grant->total_bytes > SKSE_PROFILE_JSON_MAX_TOTAL ||
        !grant->files || grant->files > SKSE_PROFILE_JSON_MAX_FILES) return SKSE_PROFILE_JSON_ARGUMENT;
    SkseProfileJsonStore *store = calloc(1, sizeof(*store));
    if (!store) return SKSE_PROFILE_JSON_NO_MEMORY;
    store->root = fcntl(app_dir, F_DUPFD_CLOEXEC, 0);
    if (store->root < 0) {
        free(store);
        return SKSE_PROFILE_JSON_IO;
    }
    pthread_mutex_lock(&store_lock);
    if (next_lease) store->lease = next_lease++;
    pthread_mutex_unlock(&store_lock);
    if (!store->lease) {
        skse_profile_json_close(store);
        return SKSE_PROFILE_JSON_LIMIT;
    }
    store->root_info = root;
    store->identity = grant->identity;
    store->file_bytes = grant->file_bytes;
    store->total_bytes = grant->total_bytes;
    store->files = grant->files;
    id_name(store->profile, 'p', store->identity.profile, sizeof store->identity.profile);
    id_name(store->owner, 'o', store->identity.owner, sizeof store->identity.owner);
    memcpy(store->prefix, prefix, strlen(prefix) + 1);
    *out = store;
    return SKSE_PROFILE_JSON_OK;
}

void skse_profile_json_close(SkseProfileJsonStore *store) {
    if (!store) return;
    close(store->root);
    free(store);
}

SkseProfileJsonResult skse_profile_json_capture(const SkseProfileJsonStore *store,
                                                const SkseProfileJsonIdentity *identity, const char *path,
                                                const uint8_t *bytes, size_t length, SkseProfileJsonSnapshot **out) {
    if (!out) return SKSE_PROFILE_JSON_ARGUMENT;
    *out = NULL;
    char canonical[PATH_BYTES];
    Result result = path_in_grant(store, identity, path, canonical);
    if (result) return result;
    if (length && !bytes) return SKSE_PROFILE_JSON_ARGUMENT;
    if (length > store->file_bytes) return SKSE_PROFILE_JSON_LIMIT;
    SkseProfileJsonSnapshot *snapshot = malloc(sizeof(*snapshot) + length);
    if (!snapshot) return SKSE_PROFILE_JSON_NO_MEMORY;
    snapshot->lease = store->lease;
    snapshot->length = length;
    snapshot->published = false;
    memcpy(snapshot->path, canonical, strlen(canonical) + 1);
    if (length) memcpy(snapshot->bytes, bytes, length);
    *out = snapshot;
    return SKSE_PROFILE_JSON_OK;
}

void skse_profile_json_discard(SkseProfileJsonSnapshot *snapshot) {
    free(snapshot);
}

/* An independent open file description keeps the caller's offset intact. */
static DIR *open_scan(int dir) {
    int scan = openat(dir, ".", DIRECTORY_FLAGS);
    if (scan < 0) return NULL;
    DIR *stream = fdopendir(scan);
    if (!stream) close(scan);
    return stream;
}

static Result next_entry(DIR *stream, unsigned *remaining, const char **name) {
    *name = NULL;
    for (;;) {
        errno = 0;
        struct dirent *item = readdir(stream);
        if (!item) return errno ? SKSE_PROFILE_JSON_IO : SKSE_PROFILE_JSON_OK;
        if (!*remaining) return SKSE_PROFILE_JSON_LIMIT;
        --*remaining;
        if (skipped_entry(item->d_name)) continue;
        *name = item->d_name;
        return SKSE_PROFILE_JSON_OK;
    }
}

static Result lookup(int dir, const char *folded, Entry *entry, unsigned *remaining) {
    entry->name[0] = 0;
    DIR *stream = open_scan(dir);
    if (!stream) return SKSE_PROFILE_JSON_IO;
    Result result = SKSE_PROFILE_JSON_OK;
    const char *name;
    while (!(result = next_entry(stream, remaining, &name)) && name) {
        if (!skse_win_asset_name_equal(name, folded)) continue;
        if (*entry->name) {
            result = SKSE_PROFILE_JSON_CONFLICT;
            break;
        }
        /* A case match is the same length as the previously bounded component. */
        memcpy(entry->name, name, strlen(folded) + 1);
    }
    if (closedir(stream) && !result) result = SKSE_PROFILE_JSON_IO;
    if (result) return result;
    if (!*entry->name) return SKSE_PROFILE_JSON_MISSING;
    return !fstatat(dir, entry->name, &entry->info, AT_SYMLINK_NOFOLLOW) ? SKSE_PROFILE_JSON_OK :
        (errno == ENOENT ? SKSE_PROFILE_JSON_CHANGED : SKSE_PROFILE_JSON_IO);
}

static int sync_file(int fd) {
    int result;
    do { result = fsync(fd); } while (result && errno == EINTR);
    return result;
}

static Result open_directory(int parent, const Entry *entry, const struct stat *root, int *out) {
    if (!private_directory(&entry->info, root)) return SKSE_PROFILE_JSON_CONFLICT;
    int next = openat(parent, entry->name, DIRECTORY_FLAGS);
    if (next < 0) return errno == ENOENT || errno == ENOTDIR || errno == ELOOP ?
        SKSE_PROFILE_JSON_CHANGED : SKSE_PROFILE_JSON_IO;
    struct stat opened;
    if (fstat(next, &opened) || !private_directory(&opened, root) || !skse_save_same_file(&entry->info, &opened)) {
        close(next);
        return SKSE_PROFILE_JSON_CHANGED;
    }
    *out = next;
    return SKSE_PROFILE_JSON_OK;
}

static Result child_directory(int parent, const char *component, const struct stat *root,
                              bool create, unsigned *directories, unsigned *remaining, int *out) {
    Entry entry;
    Result result = lookup(parent, component, &entry, remaining);
    if (result == SKSE_PROFILE_JSON_MISSING && create) {
        if (directories && *directories >= MAX_DIRS) return SKSE_PROFILE_JSON_LIMIT;
        if (mkdirat(parent, component, 0700)) return errno == EEXIST ? SKSE_PROFILE_JSON_CHANGED : SKSE_PROFILE_JSON_IO;
        if (directories) ++*directories;
        result = lookup(parent, component, &entry, remaining);
        if (!result && sync_file(parent)) {
            struct stat current;
            if (!fstatat(parent, entry.name, &current, AT_SYMLINK_NOFOLLOW) &&
                private_directory(&current, root) && skse_save_same_file(&entry.info, &current))
                (void)unlinkat(parent, entry.name, AT_REMOVEDIR);
            return SKSE_PROFILE_JSON_IO;
        }
    }
    return result ? result : open_directory(parent, &entry, root, out);
}

static Result owner_directory(const SkseProfileJsonStore *store, bool create, unsigned *remaining, int *out) {
    int profile;
    Result result = child_directory(store->root, store->profile, &store->root_info, create, NULL, remaining, &profile);
    if (result) return result;
    result = child_directory(profile, store->owner, &store->root_info, create, NULL, remaining, out);
    close(profile);
    return result;
}

static Result open_parent(const SkseProfileJsonStore *store, int owner, const char *path,
                          Usage *usage, unsigned *remaining, int *parent, const char **leaf) {
    int fd = fcntl(owner, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return SKSE_PROFILE_JSON_IO;
    for (const char *slash; (slash = strchr(path, '/')); path = slash + 1) {
        char part[NAME_MAX + 1];
        size_t n = (size_t)(slash - path);
        memcpy(part, path, n);
        part[n] = 0;
        int child;
        Result result = child_directory(fd, part, &store->root_info, usage != NULL, usage ? &usage->dirs : NULL,
                                        remaining, &child);
        close(fd);
        if (result) return result;
        fd = child;
    }
    *parent = fd;
    *leaf = path;
    return SKSE_PROFILE_JSON_OK;
}

static Result stored_attributes(const SkseProfileJsonStore *store, const char *path, int *is_directory) {
    unsigned remaining = MAX_VISITS;
    int owner, parent;
    const char *leaf;
    Result result = owner_directory(store, false, &remaining, &owner);
    if (result) return result;
    result = open_parent(store, owner, path, NULL, &remaining, &parent, &leaf);
    close(owner);
    if (result) return result;
    Entry entry;
    result = lookup(parent, leaf, &entry, &remaining);
    if (!result) {
        if (private_directory(&entry.info, &store->root_info)) *is_directory = 1;
        else if (!json_name(entry.name) || !private_file(&entry.info, &store->root_info))
            result = SKSE_PROFILE_JSON_CONFLICT;
    }
    close(parent);
    return result;
}

SkseProfileJsonResult skse_profile_json_attributes(const SkseProfileJsonStore *store,
                                                  const SkseProfileJsonIdentity *identity,
                                                  const char *path, int *is_directory) {
    if (!store || !identity || !is_directory) return SKSE_PROFILE_JSON_ARGUMENT;
    *is_directory = 0;
    if (!skse_profile_json_matches(store, identity)) return SKSE_PROFILE_JSON_SCOPE;
    char normalized[PATH_BYTES];
    if (!normalize(path, normalized)) return SKSE_PROFILE_JSON_ARGUMENT;
    size_t n = strlen(normalized), prefix = strlen(store->prefix);
    if (n <= prefix && !strncmp(normalized, store->prefix, n) && (!store->prefix[n] || store->prefix[n] == '/')) {
        *is_directory = 1;
        return SKSE_PROFILE_JSON_OK;
    }
    if (n <= prefix || strncmp(normalized, store->prefix, prefix) || normalized[prefix] != '/') return SKSE_PROFILE_JSON_SCOPE;
    pthread_mutex_lock(&store_lock);
    Result result = stored_attributes(store, normalized, is_directory);
    pthread_mutex_unlock(&store_lock);
    return result;
}

static Result owner_usage(int dir, const struct stat *root, unsigned depth, Usage *usage, unsigned *remaining) {
    DIR *stream = open_scan(dir);
    if (!stream) return SKSE_PROFILE_JSON_IO;
    Result result = SKSE_PROFILE_JSON_OK;
    const char *name;
    while (!(result = next_entry(stream, remaining, &name)) && name) {
        Entry entry;
        size_t length = strnlen(name, sizeof entry.name);
        if (length == sizeof entry.name) { result = SKSE_PROFILE_JSON_CONFLICT; break; }
        if (fstatat(dir, name, &entry.info, AT_SYMLINK_NOFOLLOW)) { result = SKSE_PROFILE_JSON_CHANGED; break; }
        if (S_ISDIR(entry.info.st_mode)) {
            if (depth + 1 >= MAX_DEPTH || ++usage->dirs > MAX_DIRS) { result = SKSE_PROFILE_JSON_LIMIT; break; }
            memcpy(entry.name, name, length + 1);
            int child;
            result = open_directory(dir, &entry, root, &child);
            if (result) break;
            result = owner_usage(child, root, depth + 1, usage, remaining);
            close(child);
            if (result) break;
            continue;
        }
        if (!private_file(&entry.info, root)) { result = SKSE_PROFILE_JSON_CONFLICT; break; }
        uintmax_t size = (uintmax_t)entry.info.st_size;
        if (size > SKSE_PROFILE_JSON_MAX_FILE) { result = SKSE_PROFILE_JSON_LIMIT; break; }
        if (!strcmp(name, pending_name)) continue;
        if (!json_name(name)) { result = SKSE_PROFILE_JSON_CONFLICT; break; }
        if (++usage->files > SKSE_PROFILE_JSON_MAX_FILES || size > SKSE_PROFILE_JSON_MAX_TOTAL - usage->bytes) {
            result = SKSE_PROFILE_JSON_LIMIT;
            break;
        }
        usage->bytes += (size_t)size;
    }
    if (closedir(stream) && !result) result = SKSE_PROFILE_JSON_IO;
    return result;
}

static Result current_parent(const SkseProfileJsonStore *store, const char *path, int parent, unsigned *remaining) {
    int owner;
    Result result = owner_directory(store, false, remaining, &owner);
    if (result) return result == SKSE_PROFILE_JSON_MISSING ? SKSE_PROFILE_JSON_CHANGED : result;
    int reopened;
    const char *leaf;
    result = open_parent(store, owner, path, NULL, remaining, &reopened, &leaf);
    close(owner);
    if (result) return result == SKSE_PROFILE_JSON_MISSING ? SKSE_PROFILE_JSON_CHANGED : result;
    struct stat a, b;
    result = fstat(parent, &a) || fstat(reopened, &b) || !skse_save_same_file(&a, &b) ?
        SKSE_PROFILE_JSON_CHANGED : SKSE_PROFILE_JSON_OK;
    close(reopened);
    return result;
}

static Result current_entry(int parent, const char *leaf, const Entry *before, unsigned *remaining) {
    Entry now;
    Result result = lookup(parent, leaf, &now, remaining);
    if (result != SKSE_PROFILE_JSON_OK && result != SKSE_PROFILE_JSON_MISSING) return result;
    if (!*before->name) return result == SKSE_PROFILE_JSON_MISSING ? SKSE_PROFILE_JSON_OK : SKSE_PROFILE_JSON_CHANGED;
    return result == SKSE_PROFILE_JSON_OK && !strcmp(before->name, now.name) &&
        skse_save_same_version(&before->info, &now.info) ? SKSE_PROFILE_JSON_OK : SKSE_PROFILE_JSON_CHANGED;
}

static Result clear_pending(int parent, const struct stat *root, unsigned *remaining) {
    Entry entry;
    Result result = lookup(parent, pending_name, &entry, remaining);
    if (result == SKSE_PROFILE_JSON_MISSING) return SKSE_PROFILE_JSON_OK;
    if (result) return result;
    /* Only the app's reserved, exact private staging name can be recycled. */
    if (strcmp(entry.name, pending_name) || !private_file(&entry.info, root))
        return SKSE_PROFILE_JSON_CONFLICT;
    return unlinkat(parent, pending_name, 0) ? SKSE_PROFILE_JSON_IO : SKSE_PROFILE_JSON_OK;
}

static Result write_pending(int parent, const uint8_t *bytes, size_t length, struct stat *created, bool *owned) {
    int fd = openat(parent, pending_name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return errno == EEXIST ? SKSE_PROFILE_JSON_CHANGED : SKSE_PROFILE_JSON_IO;
    *owned = fstat(fd, created) == 0;
    Result result = *owned ? SKSE_PROFILE_JSON_OK : SKSE_PROFILE_JSON_IO;
    for (size_t at = 0; !result && at < length;) {
        ssize_t n = write(fd, bytes + at, length - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { result = SKSE_PROFILE_JSON_IO; break; }
        at += (size_t)n;
    }
    if (!result && sync_file(fd)) result = SKSE_PROFILE_JSON_IO;
    if (!result) {
        struct stat written;
        if (fstat(fd, &written) || !skse_save_same_file(created, &written) || (uintmax_t)written.st_size != length)
            result = SKSE_PROFILE_JSON_CHANGED;
        else *created = written;
    }
    if (close(fd) && !result) result = SKSE_PROFILE_JSON_IO;
    return result;
}

static Result remove_pending(int parent, const struct stat *created) {
    struct stat now;
    if (fstatat(parent, pending_name, &now, AT_SYMLINK_NOFOLLOW))
        return errno == ENOENT ? SKSE_PROFILE_JSON_OK : SKSE_PROFILE_JSON_IO;
    if (!skse_save_same_file(created, &now)) return SKSE_PROFILE_JSON_CHANGED;
    return unlinkat(parent, pending_name, 0) ? SKSE_PROFILE_JSON_IO : SKSE_PROFILE_JSON_OK;
}

static Result publish_bytes(const SkseProfileJsonStore *store, SkseProfileJsonSnapshot *snapshot) {
    unsigned remaining = MAX_VISITS;
    int owner, parent;
    const char *leaf;
    Usage usage = {0};
    Result result = owner_directory(store, true, &remaining, &owner);
    if (result) return result;
    result = owner_usage(owner, &store->root_info, 0, &usage, &remaining);
    if (!result) result = open_parent(store, owner, snapshot->path, &usage, &remaining, &parent, &leaf);
    close(owner);
    if (result) return result;

    Entry before;
    result = lookup(parent, leaf, &before, &remaining);
    if (result == SKSE_PROFILE_JSON_MISSING) result = SKSE_PROFILE_JSON_OK;
    if (!result && *before.name) {
        if (!private_file(&before.info, &store->root_info)) result = SKSE_PROFILE_JSON_CONFLICT;
        else if (!usage.files || (uintmax_t)before.info.st_size > usage.bytes) result = SKSE_PROFILE_JSON_CHANGED;
        else {
            --usage.files;
            usage.bytes -= (size_t)before.info.st_size;
        }
    }
    if (!result && (usage.files >= store->files || usage.bytes > store->total_bytes ||
                    snapshot->length > store->total_bytes - usage.bytes)) result = SKSE_PROFILE_JSON_LIMIT;
    if (!result) result = clear_pending(parent, &store->root_info, &remaining);

    struct stat written;
    bool owned = false;
    if (!result) result = write_pending(parent, snapshot->bytes, snapshot->length, &written, &owned);
    if (!result) result = current_parent(store, snapshot->path, parent, &remaining);
    if (!result) result = current_entry(parent, leaf, &before, &remaining);
    if (!result) {
        struct stat named;
        if (fstatat(parent, pending_name, &named, AT_SYMLINK_NOFOLLOW) ||
            !private_file(&named, &store->root_info) || !skse_save_same_version(&written, &named))
            result = SKSE_PROFILE_JSON_CHANGED;
    }
    if (!result) {
        const char *target = *before.name ? before.name : leaf;
        if (renameat(parent, pending_name, parent, target)) result = SKSE_PROFILE_JSON_IO;
        else {
            owned = false;
            snapshot->published = true;
            if (sync_file(parent)) result = SKSE_PROFILE_JSON_PUBLISHED_UNSYNCED;
        }
    }
    if (owned) {
        Result cleanup = remove_pending(parent, &written);
        if (cleanup) result = cleanup;
    }
    close(parent);
    return result;
}

SkseProfileJsonResult skse_profile_json_publish(const SkseProfileJsonStore *store, SkseProfileJsonSnapshot *snapshot) {
    if (!store || !snapshot) return SKSE_PROFILE_JSON_ARGUMENT;
    if (store->lease != snapshot->lease) return SKSE_PROFILE_JSON_SCOPE;
    pthread_mutex_lock(&store_lock);
    Result result = snapshot->published ? SKSE_PROFILE_JSON_ALREADY_PUBLISHED : publish_bytes(store, snapshot);
    pthread_mutex_unlock(&store_lock);
    return result;
}

static ssize_t read_retry(int fd, uint8_t *bytes, size_t count) {
    ssize_t result;
    do { result = read(fd, bytes, count); } while (result < 0 && errno == EINTR);
    return result;
}

static Result read_bytes(const SkseProfileJsonStore *store, const char *path, size_t limit, uint8_t **bytes, size_t *length) {
    unsigned remaining = MAX_VISITS;
    int owner, parent;
    const char *leaf;
    Result result = owner_directory(store, false, &remaining, &owner);
    if (result) return result;
    result = open_parent(store, owner, path, NULL, &remaining, &parent, &leaf);
    close(owner);
    if (result) return result;
    Entry entry;
    result = lookup(parent, leaf, &entry, &remaining);
    if (!result && !private_file(&entry.info, &store->root_info)) result = SKSE_PROFILE_JSON_CONFLICT;
    if (!result && (uintmax_t)entry.info.st_size > limit) result = SKSE_PROFILE_JSON_LIMIT;

    int fd = -1;
    size_t count = 0;
    uint8_t *buffer = NULL;
    if (!result) {
        fd = openat(parent, entry.name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        struct stat opened;
        if (fd < 0 || fstat(fd, &opened) || !skse_save_same_version(&entry.info, &opened)) result = SKSE_PROFILE_JSON_CHANGED;
    }
    if (!result) {
        count = (size_t)entry.info.st_size;
        buffer = malloc(count ? count : 1);
        if (!buffer) result = SKSE_PROFILE_JSON_NO_MEMORY;
        for (size_t at = 0; !result && at < count;) {
            ssize_t n = read_retry(fd, buffer + at, count - at);
            if (n <= 0) { result = SKSE_PROFILE_JSON_CHANGED; break; }
            at += (size_t)n;
        }
        uint8_t extra;
        if (!result && read_retry(fd, &extra, 1)) result = SKSE_PROFILE_JSON_CHANGED;
    }
    if (!result) result = current_parent(store, path, parent, &remaining);
    if (!result) result = current_entry(parent, leaf, &entry, &remaining);
    if (fd >= 0 && close(fd) && !result) result = SKSE_PROFILE_JSON_IO;
    close(parent);
    if (result) free(buffer);
    else {
        *bytes = buffer;
        *length = count;
    }
    return result;
}

SkseProfileJsonResult skse_profile_json_read(const SkseProfileJsonStore *store, const SkseProfileJsonIdentity *identity,
                                             const char *path, size_t limit, uint8_t **bytes, size_t *length) {
    if (bytes) *bytes = NULL;
    if (length) *length = 0;
    if (!bytes || !length) return SKSE_PROFILE_JSON_ARGUMENT;
    char canonical[PATH_BYTES];
    Result result = path_in_grant(store, identity, path, canonical);
    if (result) return result;
    limit = limit < store->file_bytes ? limit : store->file_bytes;
    pthread_mutex_lock(&store_lock);
    result = read_bytes(store, canonical, limit, bytes, length);
    pthread_mutex_unlock(&store_lock);
    return result;
}
