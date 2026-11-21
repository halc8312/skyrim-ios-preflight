/* SPDX-License-Identifier: GPL-3.0-only */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "profile_overlay.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define PREFIX "Data/SKSE/Plugins/OwnerSettings"
#define SETTINGS PREFIX "/Nested/Options.json"
#define SETTINGS_LOWER "data/skse/plugins/ownersettings/nested/options.json"
#define EXTRA PREFIX "/Another.json"
#define PENDING ".skyrimios-json.pending"

static const char initial[] = "{\"values\":[-731,2]}";
static const char updated[] = "{\"values\":[42]}";
static const char other_profile[] = "{\"locale\":\"ja\"}";
static const char other_owner[] = "{\"locale\":\"en\"}";
static const char form[] = "{}";

static SkseProfileJsonGrant grant(unsigned profile, unsigned owner, unsigned files, size_t bytes) {
    SkseProfileJsonGrant result = {.virtual_prefix = PREFIX, .file_bytes = 32, .total_bytes = bytes, .files = files};
    result.identity.profile[15] = (uint8_t)profile;
    result.identity.owner[31] = (uint8_t)owner;
    return result;
}

static SkseProfileJsonStore *open_store(int root, const SkseProfileJsonGrant *policy) {
    SkseProfileJsonStore *store = NULL;
    assert(skse_profile_json_open(root, policy, &store) == SKSE_PROFILE_JSON_OK && store);
    return store;
}

static SkseProfileJsonSnapshot *capture(const SkseProfileJsonStore *store, const SkseProfileJsonGrant *policy,
                                       const char *name, const char *text) {
    SkseProfileJsonSnapshot *snapshot = NULL;
    assert(skse_profile_json_capture(store, &policy->identity, name, (const uint8_t *)text, strlen(text), &snapshot) ==
           SKSE_PROFILE_JSON_OK && snapshot);
    return snapshot;
}

static void write_json(const SkseProfileJsonStore *store, const SkseProfileJsonGrant *policy,
                       const char *name, const char *text, SkseProfileJsonResult expect) {
    SkseProfileJsonSnapshot *snapshot = capture(store, policy, name, text);
    assert(skse_profile_json_publish(store, snapshot) == expect);
    skse_profile_json_discard(snapshot);
}

static void reads(const SkseProfileJsonStore *store, const SkseProfileJsonGrant *policy, const char *name,
                  const char *expected) {
    uint8_t *bytes = NULL;
    size_t length = 0;
    assert(skse_profile_json_read(store, &policy->identity, name, SIZE_MAX, &bytes, &length) == SKSE_PROFILE_JSON_OK);
    assert(bytes && length == strlen(expected) && !memcmp(bytes, expected, length));
    free(bytes);
}

static void read_error(const SkseProfileJsonStore *store, const SkseProfileJsonGrant *policy, const char *name,
                       size_t limit, SkseProfileJsonResult expect) {
    uint8_t unexpected = 0, *bytes = &unexpected;
    size_t length = SIZE_MAX;
    assert(skse_profile_json_read(store, &policy->identity, name, limit, &bytes, &length) == expect);
    assert(!bytes && !length);
}

static void write_file(int dir, const char *name, const char *text) {
    int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    assert(fd >= 0);
    size_t n = strlen(text);
    assert(write(fd, text, n) == (ssize_t)n && !close(fd));
}

static void check_fd(int fd, const char *text) {
    char bytes[48];
    size_t n = strlen(text);
    assert(n < sizeof bytes && read(fd, bytes, sizeof bytes) == (ssize_t)n && !memcmp(bytes, text, n));
    assert(!close(fd));
}

static void check_file(int dir, const char *name, const char *text) {
    int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    assert(fd >= 0);
    check_fd(fd, text);
}

static int parent_of_settings(int root, unsigned profile, unsigned owner) {
    char path[180];
    int n = snprintf(path, sizeof path, "p-%032x/o-%064x/data/skse/plugins/ownersettings/nested", profile, owner);
    assert(n > 0 && (size_t)n < sizeof path);
    int fd = openat(root, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    assert(fd >= 0);
    return fd;
}

static void no_pending(int dir) {
    struct stat st;
    assert(fstatat(dir, PENDING, &st, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT);
}

static void test_scope_and_atomic_replacement(int root) {
    const SkseProfileJsonGrant a = grant(0, 1, 4, 96);
    const SkseProfileJsonGrant b = grant(1, 1, 4, 96);
    const SkseProfileJsonGrant c = grant(0, 2, 4, 96);
    SkseProfileJsonStore *one = open_store(root, &a);
    SkseProfileJsonStore *two = open_store(root, &b);
    SkseProfileJsonStore *three = open_store(root, &c);
    char editable[sizeof initial];
    memcpy(editable, initial, sizeof initial);
    SkseProfileJsonSnapshot *snapshot = capture(one, &a, SETTINGS, editable);
    memset(editable, '!', sizeof editable);
    assert(skse_profile_json_publish(two, snapshot) == SKSE_PROFILE_JSON_SCOPE);
    assert(skse_profile_json_publish(three, snapshot) == SKSE_PROFILE_JSON_SCOPE);

    SkseProfileJsonGrant narrow = a;
    narrow.virtual_prefix = PREFIX "/Nested";
    SkseProfileJsonStore *wrong_grant = open_store(root, &narrow);
    assert(skse_profile_json_publish(wrong_grant, snapshot) == SKSE_PROFILE_JSON_SCOPE);
    read_error(one, &a, SETTINGS, SIZE_MAX, SKSE_PROFILE_JSON_MISSING);
    assert(skse_profile_json_publish(one, snapshot) == SKSE_PROFILE_JSON_OK);
    assert(skse_profile_json_publish(one, snapshot) == SKSE_PROFILE_JSON_ALREADY_PUBLISHED);
    reads(one, &a, SETTINGS_LOWER, initial);
    reads(wrong_grant, &narrow, SETTINGS, initial); // Same owner explicitly authorizes overlapping grants.
    read_error(one, &b, SETTINGS, SIZE_MAX, SKSE_PROFILE_JSON_SCOPE);
    read_error(one, &c, SETTINGS, SIZE_MAX, SKSE_PROFILE_JSON_SCOPE);
    read_error(two, &b, SETTINGS, SIZE_MAX, SKSE_PROFILE_JSON_MISSING);
    read_error(three, &c, SETTINGS, SIZE_MAX, SKSE_PROFILE_JSON_MISSING);
    write_json(two, &b, SETTINGS, other_profile, SKSE_PROFILE_JSON_OK);
    write_json(three, &c, SETTINGS, other_owner, SKSE_PROFILE_JSON_OK);
    int parent = parent_of_settings(root, 0, 1);
    int old = openat(parent, "options.json", O_RDONLY | O_CLOEXEC);
    assert(old >= 0);

    write_json(one, &a, SETTINGS, updated, SKSE_PROFILE_JSON_OK);
    check_fd(old, initial); // Open readers see their original inode after atomic replacement.
    reads(one, &a, SETTINGS, updated);
    reads(two, &b, SETTINGS, other_profile);
    reads(three, &c, SETTINGS, other_owner);
    read_error(one, &a, SETTINGS, 3, SKSE_PROFILE_JSON_LIMIT);
    no_pending(parent);
    assert(!close(parent));
    skse_profile_json_discard(snapshot);

    snapshot = capture(one, &a, SETTINGS, initial);
    skse_profile_json_close(one);
    one = open_store(root, &a);
    assert(skse_profile_json_publish(one, snapshot) == SKSE_PROFILE_JSON_SCOPE);
    reads(one, &a, SETTINGS, updated);
    skse_profile_json_discard(snapshot);
    skse_profile_json_close(one);
    skse_profile_json_close(two);
    skse_profile_json_close(three);
    skse_profile_json_close(wrong_grant);
}

static void test_path_and_links(int root, int outside) {
    const SkseProfileJsonGrant policy = grant(2, 2, 4, 96);
    SkseProfileJsonStore *store = open_store(root, &policy);
    const char *bad_paths[] = {
        PREFIX "/../Options.json", PREFIX "/./Options.json", "/" SETTINGS, "C:" SETTINGS,
        PREFIX "//Options.json", PREFIX "\\Options.json", PREFIX "/Hidden.", PREFIX "/a /Options.json",
        PREFIX "/.secret.json", PREFIX "/file.txt", PREFIX "/X\xe3\x81\x84.json",
    };
    for (size_t i = 0; i < sizeof bad_paths / sizeof bad_paths[0]; ++i) {
        SkseProfileJsonSnapshot *snapshot = NULL;
        assert(skse_profile_json_capture(store, &policy.identity, bad_paths[i], (const uint8_t *)form, strlen(form),
                                         &snapshot) == SKSE_PROFILE_JSON_ARGUMENT && !snapshot);
        read_error(store, &policy, bad_paths[i], SIZE_MAX, SKSE_PROFILE_JSON_ARGUMENT);
    }
    read_error(store, &policy, "Data/SKSE/Plugins/OwnerSettingsOther/file.json", SIZE_MAX, SKSE_PROFILE_JSON_SCOPE);
    read_error(store, &policy, "Data/SKSE/Plugins/Other/file.json", SIZE_MAX, SKSE_PROFILE_JSON_SCOPE);
    SkseProfileJsonIdentity wrong = policy.identity;
    wrong.owner[0] ^= 1;
    SkseProfileJsonSnapshot *snapshot = NULL;
    assert(skse_profile_json_capture(store, &wrong, SETTINGS, (const uint8_t *)form, strlen(form), &snapshot) ==
           SKSE_PROFILE_JSON_SCOPE && !snapshot);

    write_json(store, &policy, SETTINGS, initial, SKSE_PROFILE_JSON_OK);
    int parent = parent_of_settings(root, 2, 2);
    write_file(outside, "foreign.json", other_owner);
    assert(!linkat(outside, "foreign.json", parent, "hard.json", 0));
    read_error(store, &policy, PREFIX "/Nested/hard.json", SIZE_MAX, SKSE_PROFILE_JSON_CONFLICT);
    write_json(store, &policy, PREFIX "/Nested/hard.json", form, SKSE_PROFILE_JSON_CONFLICT);
    assert(!unlinkat(parent, "hard.json", 0));
    assert(!symlinkat("/nonexistent/a-file", parent, "soft.json"));
    read_error(store, &policy, PREFIX "/Nested/soft.json", SIZE_MAX, SKSE_PROFILE_JSON_CONFLICT);
    write_json(store, &policy, PREFIX "/Nested/soft.json", form, SKSE_PROFILE_JSON_CONFLICT);
    assert(!unlinkat(parent, "soft.json", 0));
    assert(!symlinkat("/nonexistent/a-directory", parent, "corner"));
    read_error(store, &policy, PREFIX "/Nested/corner/file.json", SIZE_MAX, SKSE_PROFILE_JSON_CONFLICT);
    write_json(store, &policy, PREFIX "/Nested/corner/file.json", form, SKSE_PROFILE_JSON_CONFLICT);
    assert(!unlinkat(parent, "corner", 0));
    assert(!mkfifoat(parent, "pipe.json", 0600));
    read_error(store, &policy, PREFIX "/Nested/pipe.json", SIZE_MAX, SKSE_PROFILE_JSON_CONFLICT);
    assert(!unlinkat(parent, "pipe.json", 0));
    assert(!symlinkat("/nonexistent/pending-path", parent, PENDING));
    write_json(store, &policy, SETTINGS, form, SKSE_PROFILE_JSON_CONFLICT);
    struct stat pending;
    assert(!fstatat(parent, PENDING, &pending, AT_SYMLINK_NOFOLLOW) && S_ISLNK(pending.st_mode));
    assert(!unlinkat(parent, PENDING, 0));

    reads(store, &policy, SETTINGS, initial);
    check_file(outside, "foreign.json", other_owner);
    assert(!close(parent));
    skse_profile_json_close(store);
}

static void test_case_and_limits(int root) {
    const SkseProfileJsonGrant policy = grant(3, 2, 2, 35);
    SkseProfileJsonStore *store = open_store(root, &policy);
    write_json(store, &policy, SETTINGS, initial, SKSE_PROFILE_JSON_OK);
    int parent = parent_of_settings(root, 3, 2);
    assert(!renameat(parent, "options.json", parent, "Options.JSON"));
    reads(store, &policy, SETTINGS_LOWER, initial);
    write_json(store, &policy, SETTINGS_LOWER, updated, SKSE_PROFILE_JSON_OK);
    check_file(parent, "Options.JSON", updated);

    int duplicate = openat(parent, "options.json", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (duplicate >= 0) {
        assert(!close(duplicate)); // The POSIX directory supports independent case aliases.
        read_error(store, &policy, SETTINGS_LOWER, SIZE_MAX, SKSE_PROFILE_JSON_CONFLICT);
        write_json(store, &policy, SETTINGS_LOWER, initial, SKSE_PROFILE_JSON_CONFLICT);
        assert(!unlinkat(parent, "options.json", 0));
        int above = openat(parent, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        assert(above >= 0 && !mkdirat(above, "NESTED", 0700));
        read_error(store, &policy, SETTINGS, SIZE_MAX, SKSE_PROFILE_JSON_CONFLICT);
        write_json(store, &policy, SETTINGS, initial, SKSE_PROFILE_JSON_CONFLICT);
        assert(!unlinkat(above, "NESTED", AT_REMOVEDIR) && !close(above));
    } else assert(errno == EEXIST);
    check_file(parent, "Options.JSON", updated);

    char big[34] = "{1234567890123456789012345678901}";
    big[32] = '}';
    big[33] = 0;
    SkseProfileJsonSnapshot *snapshot = NULL;
    assert(skse_profile_json_capture(store, &policy.identity, EXTRA, (const uint8_t *)big, 33, &snapshot) ==
           SKSE_PROFILE_JSON_LIMIT && !snapshot);
    write_json(store, &policy, EXTRA, initial, SKSE_PROFILE_JSON_OK);
    write_json(store, &policy, PREFIX "/Third.json", form, SKSE_PROFILE_JSON_LIMIT);
    // Replacement subtracts the prior bytes; a new over-quota payload preserves them.
    write_json(store, &policy, SETTINGS, initial, SKSE_PROFILE_JSON_LIMIT);
    reads(store, &policy, SETTINGS, updated);
    write_json(store, &policy, EXTRA, "", SKSE_PROFILE_JSON_OK);
    reads(store, &policy, EXTRA, "");
    write_json(store, &policy, SETTINGS, initial, SKSE_PROFILE_JSON_OK);

    no_pending(parent);
    assert(!close(parent));
    skse_profile_json_close(store);
}

typedef struct {
    SkseProfileJsonStore *store;
    SkseProfileJsonSnapshot *snapshot;
    SkseProfileJsonResult result;
} Writer;

static void *publish_thread(void *opaque) {
    Writer *writer = opaque;
    writer->result = skse_profile_json_publish(writer->store, writer->snapshot);
    return NULL;
}

static void test_concurrent_quota(int root) {
    const SkseProfileJsonGrant policy = grant(4, 1, 1, 32);
    Writer first = {.store = open_store(root, &policy)};
    Writer next = {.store = open_store(root, &policy)};
    first.snapshot = capture(first.store, &policy, SETTINGS, initial);
    next.snapshot = capture(next.store, &policy, EXTRA, updated);
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, publish_thread, &first));
    assert(!pthread_create(&b, NULL, publish_thread, &next));
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    bool first_won = first.result == SKSE_PROFILE_JSON_OK;
    assert(first_won ? next.result == SKSE_PROFILE_JSON_LIMIT :
                       (first.result == SKSE_PROFILE_JSON_LIMIT && next.result == SKSE_PROFILE_JSON_OK));
    reads(first.store, &policy, first_won ? SETTINGS : EXTRA, first_won ? initial : updated);
    read_error(next.store, &policy, first_won ? EXTRA : SETTINGS, SIZE_MAX, SKSE_PROFILE_JSON_MISSING);
    skse_profile_json_discard(first.snapshot);
    skse_profile_json_discard(next.snapshot);
    skse_profile_json_close(first.store);
    skse_profile_json_close(next.store);
}

#ifdef SKSE_PROFILE_TEST_FAULTS
typedef enum { NO_FAULT, SHORT_WRITE, FILE_SYNC, RENAME_FAIL, DIR_SYNC, CRASH, DETACH } Fault;
static Fault fault;
static unsigned attempted_writes, renamed;
static int detach_parent, detach_destination;
ssize_t __real_write(int, const void *, size_t);
int __real_fsync(int);
int __real_renameat(int, const char *, int, const char *);

ssize_t __wrap_write(int fd, const void *bytes, size_t sizeof_bytes) {
    if (fault == SHORT_WRITE) {
        if (!attempted_writes++) return __real_write(fd, bytes, 1);
        errno = ENOSPC;
        return -1;
    }
    return __real_write(fd, bytes, sizeof_bytes);
}

int __wrap_fsync(int fd) {
    struct stat info;
    assert(!fstat(fd, &info));
    if ((fault == FILE_SYNC && S_ISREG(info.st_mode)) || (fault == DIR_SYNC && renamed && S_ISDIR(info.st_mode))) {
        errno = EIO;
        return -1;
    }
    if (fault == DETACH && S_ISREG(info.st_mode))
        assert(!__real_renameat(detach_parent, "nested", detach_destination, "separated"));
    return __real_fsync(fd);
}

int __wrap_renameat(int old_parent, const char *old_leaf, int new_parent, const char *new_leaf) {
    if (fault == CRASH) _exit(70);
    assert(fault == NO_FAULT || old_parent == new_parent);
    if (fault == RENAME_FAIL) { errno = EACCES; return -1; }
    int result = __real_renameat(old_parent, old_leaf, new_parent, new_leaf);
    if (!result) ++renamed;
    return result;
}

static void test_failure_and_recovery(int root, int outside) {
    const SkseProfileJsonGrant policy = grant(5, 2, 4, 96);
    SkseProfileJsonStore *store = open_store(root, &policy);
    write_json(store, &policy, SETTINGS, initial, SKSE_PROFILE_JSON_OK);
    int parent = parent_of_settings(root, 5, 2);
    SkseProfileJsonSnapshot *snapshot = capture(store, &policy, SETTINGS, updated);
    const Fault pre_rename_faults[] = {SHORT_WRITE, FILE_SYNC, RENAME_FAIL};
    for (size_t i = 0; i < sizeof pre_rename_faults / sizeof pre_rename_faults[0]; ++i) {
        fault = pre_rename_faults[i];
        attempted_writes = renamed = 0;
        assert(skse_profile_json_publish(store, snapshot) == SKSE_PROFILE_JSON_IO);
        assert(!renamed && (fault != SHORT_WRITE || attempted_writes == 2));
        fault = NO_FAULT;
        no_pending(parent);
        reads(store, &policy, SETTINGS, initial);
    }

    pid_t writer = fork();
    assert(writer >= 0);
    if (writer == 0) {
        fault = CRASH;
        (void)skse_profile_json_publish(store, snapshot);
        _exit(71);
    }
    int child_status = 0;
    assert(waitpid(writer, &child_status, 0) == writer && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 70);
    check_file(parent, PENDING, updated);
    reads(store, &policy, SETTINGS, initial);
    fault = DIR_SYNC;
    renamed = 0;
    assert(skse_profile_json_publish(store, snapshot) == SKSE_PROFILE_JSON_PUBLISHED_UNSYNCED);
    assert(renamed == 1);
    fault = NO_FAULT;
    assert(skse_profile_json_publish(store, snapshot) == SKSE_PROFILE_JSON_ALREADY_PUBLISHED);
    reads(store, &policy, SETTINGS, updated);
    no_pending(parent);
    skse_profile_json_discard(snapshot);

    detach_parent = openat(parent, "..", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    assert(detach_parent >= 0);
    detach_destination = outside;
    fault = DETACH;
    write_json(store, &policy, SETTINGS, initial, SKSE_PROFILE_JSON_CHANGED);
    fault = NO_FAULT;
    assert(!close(detach_parent));
    no_pending(parent);
    check_file(parent, "options.json", updated);
    read_error(store, &policy, SETTINGS, SIZE_MAX, SKSE_PROFILE_JSON_MISSING);
    assert(!close(parent));
    skse_profile_json_close(store);
}
#endif

typedef struct {
    bool collide, blocked_ancestor;
    unsigned reads;
} ImmutableBase;

static bool base_path(const ImmutableBase *base, const char *path) {
    return skse_win_asset_name_equal(path, "Data/base.json") ||
        (base->collide && skse_win_asset_name_equal(path, SETTINGS)) ||
        (base->blocked_ancestor && skse_win_asset_name_equal(path, PREFIX));
}

static uint32_t immutable_read(void *opaque, const char *path, size_t limit, uint8_t **bytes, size_t *length) {
    ImmutableBase *base = opaque;
    if (!base_path(base, path)) return 2;
    size_t count = strlen(other_owner);
    if (limit < count) return 223;
    uint8_t *data = malloc(count);
    if (!data) return 8;
    memcpy(data, other_owner, count);
    *bytes = data;
    *length = count;
    ++base->reads;
    return 0;
}

static uint32_t immutable_attributes(void *opaque, const char *path, uint32_t *attributes) {
    const ImmutableBase *base = opaque;
    if (!base_path(base, path)) return 2;
    *attributes = 1; // FILE_ATTRIBUTE_READONLY.
    return 0;
}

typedef struct {
    SkseProfileJsonStore *store;
    SkseProfileJsonOverlay *binding;
    SkseWinFiles files;
} ProfileVfs;

static ProfileVfs vfs_open(int root, const SkseProfileJsonGrant *policy, ImmutableBase *base) {
    ProfileVfs vfs = {.store = open_store(root, policy)};
    assert(skse_profile_json_overlay_open(vfs.store, &policy->identity, &vfs.binding) == SKSE_PROFILE_JSON_OK);
    vfs.files.source = (SkseWinAssets){immutable_read, immutable_attributes, base};
    vfs.files.profile = skse_profile_json_overlay_provider(vfs.binding);
    return vfs;
}

static void vfs_close(ProfileVfs *vfs) {
    skse_win_files_close(&vfs->files);
    skse_profile_json_overlay_close(vfs->binding);
    skse_profile_json_close(vfs->store);
}

static SkseWinFile *vfs_file(ProfileVfs *vfs, const char *path, unsigned access, unsigned share,
                            SkseWinFileDisposition disposition, uint32_t old_error, uint32_t expected) {
    uint32_t error = old_error;
    uint32_t token = skse_win_file_open(&vfs->files, path, access, share, disposition, &error);
    assert(token != UINT32_MAX && error == expected);
    SkseWinFile *file = skse_win_file_lookup(&vfs->files, token);
    assert(file);
    return file;
}

static void vfs_open_error(ProfileVfs *vfs, const char *path, unsigned access, unsigned share,
                           SkseWinFileDisposition disposition, uint32_t expected) {
    uint32_t error = UINT32_MAX;
    assert(skse_win_file_open(&vfs->files, path, access, share, disposition, &error) == UINT32_MAX);
    assert(error == expected);
}

static void vfs_fill(SkseWinFile *file, const char *text) {
    size_t n = strlen(text);
    assert(skse_win_file_resize(file, n) == 0);
    if (n) {
        memcpy(file->data->bytes, text, n);
        skse_win_file_note_write(file, true);
    }
}

static void vfs_check(SkseWinFile *file, const char *text) {
    size_t n = strlen(text);
    assert(file->data->length == n && (!n || !memcmp(file->data->bytes, text, n)));
}

static SkseProfileJsonBatch vfs_stage(ProfileVfs *vfs, bool guest_ok, uint32_t guest_error,
                                     SkseProfileJsonResult storage_error, size_t count) {
    SkseProfileJsonBatch batch = {0};
    SkseProfileJsonCaptureResult result = skse_profile_json_overlay_capture(vfs->binding, &vfs->files, guest_ok, &batch);
    assert(result.guest_error == guest_error && result.storage_error == storage_error && batch.count == count);
    for (size_t i = count; i < SKSE_WIN_OUTPUT_FILES; ++i) assert(!batch.entries[i]);
    return batch;
}

static void test_profile_vfs_sessions(int root) {
    enum { READ = SKSE_WIN_FILE_READ, WRITE = SKSE_WIN_FILE_WRITE, BOTH = READ | WRITE };
    const SkseProfileJsonGrant policy = grant(7, 1, 4, 96);
    ImmutableBase base = {0};
    ProfileVfs first = vfs_open(root, &policy, &base);
    uint32_t attributes, error;
    assert(skse_win_file_attributes(&first.files, PREFIX, &attributes, &error) &&
           !error && attributes == SKSE_WIN_ATTRIBUTE_DIRECTORY && first.files.attribute_requests == 1);
    assert(skse_win_directory_create(&first.files, PREFIX "/Nested") == 0);
    assert(first.files.attribute_requests == 1); // Internal stat doesn't consume an explicit guest query.

    static const char filename[] = "C:\\Skyrim\\Data\\SKSE\\Plugins\\Temporary\\..\\OwnerSettings\\Nested\\Options.json";
    uint16_t units[sizeof filename];
    for (size_t i = 0; i < sizeof filename; ++i) units[i] = (uint8_t)filename[i];
    char path[SKSE_WIN_PATH_BYTES];
    assert(skse_win_asset_path(units, sizeof filename - 1, true, path) && skse_win_asset_name_equal(path, SETTINGS));
    const uint16_t outside_root[] = {'.', '.', '/', 'e', 's', 'c'};
    char denied[SKSE_WIN_PATH_BYTES];
    assert(!skse_win_asset_path(outside_root, sizeof outside_root / sizeof outside_root[0], true, denied));
    SkseWinFile *writer = vfs_file(&first, path, BOTH, BOTH, SKSE_WIN_CREATE_ALWAYS, UINT32_MAX, 0);
    vfs_fill(writer, initial);
    (void)vfs_stage(&first, true, 32, SKSE_PROFILE_JSON_OK, 0); // Output still held open.
    skse_win_file_close(writer);

    SkseWinFile *log = vfs_file(&first, "Data/debug.log", BOTH, BOTH, SKSE_WIN_CREATE_ALWAYS, UINT32_MAX, 0);
    vfs_fill(log, other_owner); // Legacy volatile output remains outside the profile grant.
    SkseWinFile *unrelated = vfs_file(&first, "Data/base.json", READ, BOTH, SKSE_WIN_OPEN_EXISTING, 17, 17);
    vfs_check(unrelated, other_owner);
    SkseProfileJsonBatch staged = vfs_stage(&first, true, 0, SKSE_PROFILE_JSON_OK, 1);
    read_error(first.store, &policy, SETTINGS, SIZE_MAX, SKSE_PROFILE_JSON_MISSING);
    skse_win_file_close(log);
    skse_win_file_close(unrelated);
    skse_win_files_close(&first.files);
    assert(skse_profile_json_publish(first.store, staged.entries[0]) == SKSE_PROFILE_JSON_OK);
    skse_profile_json_batch_discard(&staged);
    vfs_close(&first);

    ProfileVfs second = vfs_open(root, &policy, &base);
    SkseProfileJsonIdentity wrong = policy.identity;
    wrong.owner[31] ^= 1;
    SkseProfileJsonOverlay *binding = NULL;
    assert(skse_profile_json_overlay_open(second.store, &wrong, &binding) == SKSE_PROFILE_JSON_SCOPE && !binding);
    assert(skse_profile_json_overlay_open(second.store, &policy.identity, &binding) == SKSE_PROFILE_JSON_OK);
    SkseProfileJsonCaptureResult mismatch = skse_profile_json_overlay_capture(binding, &second.files, true, &staged);
    assert(mismatch.storage_error == SKSE_PROFILE_JSON_SCOPE && !mismatch.guest_error && !staged.count);
    skse_profile_json_overlay_close(binding);
    assert(skse_win_file_attributes(&second.files, SETTINGS_LOWER, &attributes, &error) &&
           !error && attributes == SKSE_WIN_ATTRIBUTE_NORMAL);
    assert(skse_win_directory_create(&second.files, PREFIX "/Nested") == 183);
    SkseWinFile *reader = vfs_file(&second, SETTINGS_LOWER, READ, READ, SKSE_WIN_OPEN_EXISTING, 17, 17);
    vfs_check(reader, initial);
    vfs_open_error(&second, SETTINGS, BOTH, BOTH, SKSE_WIN_OPEN_EXISTING, 32);
    skse_win_file_close(reader);
    reader = vfs_file(&second, SETTINGS_LOWER, READ, BOTH, SKSE_WIN_OPEN_EXISTING, 17, 17);
    vfs_open_error(&second, SETTINGS, BOTH, BOTH, SKSE_WIN_CREATE_NEW, 80);
    writer = vfs_file(&second, SETTINGS, BOTH, BOTH, SKSE_WIN_OPEN_ALWAYS, 17, 183);
    assert(writer->data == reader->data);
    vfs_check(writer, initial);
    skse_win_file_close(writer);
    skse_win_file_close(reader);
    (void)vfs_stage(&second, true, 0, SKSE_PROFILE_JSON_OK, 0); // COW open alone isn't a mutation.

    writer = vfs_file(&second, SETTINGS, WRITE, BOTH, SKSE_WIN_OPEN_EXISTING, 17, 17);
    char same_length[sizeof initial];
    memcpy(same_length, initial, sizeof initial);
    same_length[12] = '8'; // Existing digit, no resize.
    memcpy(writer->data->bytes, same_length, sizeof initial - 1);
    skse_win_file_note_write(writer, true);
    skse_win_file_close(writer);
    staged = vfs_stage(&second, true, 0, SKSE_PROFILE_JSON_OK, 1);
    reads(second.store, &policy, SETTINGS, initial);
    assert(skse_profile_json_publish(second.store, staged.entries[0]) == SKSE_PROFILE_JSON_OK);
    skse_profile_json_batch_discard(&staged);
    reads(second.store, &policy, SETTINGS, same_length);
    vfs_close(&second);

    ProfileVfs third = vfs_open(root, &policy, &base);
    writer = vfs_file(&third, SETTINGS, BOTH, BOTH, SKSE_WIN_CREATE_ALWAYS, 17, 183);
    vfs_check(writer, "");
    vfs_fill(writer, updated);
    skse_win_file_close(writer);
    staged = vfs_stage(&third, true, 0, SKSE_PROFILE_JSON_OK, 1);
#ifdef SKSE_PROFILE_TEST_FAULTS
    fault = RENAME_FAIL;
    assert(skse_profile_json_publish(third.store, staged.entries[0]) == SKSE_PROFILE_JSON_IO);
    fault = NO_FAULT;
    reads(third.store, &policy, SETTINGS, same_length);
#endif
    assert(skse_profile_json_publish(third.store, staged.entries[0]) == SKSE_PROFILE_JSON_OK);
    skse_profile_json_batch_discard(&staged);
    vfs_close(&third);

    ProfileVfs final = vfs_open(root, &policy, &base);
    reader = vfs_file(&final, SETTINGS_LOWER, READ, BOTH, SKSE_WIN_OPEN_EXISTING, 17, 17);
    vfs_check(reader, updated);
    skse_win_file_close(reader);
    final.files.attribute_requests = SKSE_WIN_ATTRIBUTE_REQUEST_LIMIT - 1;
    assert(skse_win_file_attributes(&final.files, SETTINGS_LOWER, &attributes, &error) && !error);
    assert(!skse_win_file_attributes(&final.files, SETTINGS_LOWER, &attributes, &error));

    final.files.attribute_requests = 0;
    base.blocked_ancestor = true;
    assert(skse_win_file_attributes(&final.files, SETTINGS_LOWER, &attributes, &error) && error == 3 && !attributes);
    vfs_open_error(&final, SETTINGS_LOWER, READ, BOTH, SKSE_WIN_OPEN_EXISTING, 3);
    vfs_open_error(&final, SETTINGS_LOWER, BOTH, BOTH, SKSE_WIN_CREATE_ALWAYS, 3);
    base.blocked_ancestor = false;
    base.collide = true;
    reader = vfs_file(&final, SETTINGS_LOWER, READ, BOTH, SKSE_WIN_OPEN_EXISTING, 17, 17);
    vfs_check(reader, other_owner); // Result comes from the immutable base.
    skse_win_file_close(reader);
    for (SkseWinFileDisposition disposition = SKSE_WIN_CREATE_NEW; disposition <= SKSE_WIN_TRUNCATE_EXISTING;
         ++disposition)
        vfs_open_error(&final, SETTINGS_LOWER, BOTH, BOTH, disposition, disposition == SKSE_WIN_CREATE_NEW ? 80u : 5u);
    final.files.source.attributes = NULL;
    vfs_open_error(&final, EXTRA, BOTH, BOTH, SKSE_WIN_CREATE_ALWAYS, 5); // Base visibility is unknown.
    reads(final.store, &policy, SETTINGS, updated);
    vfs_close(&final);
}

static void test_profile_vfs_rejects_failed_outputs(int root) {
    enum { WRITE = SKSE_WIN_FILE_WRITE, BOTH = SKSE_WIN_FILE_READ | WRITE };
    const SkseProfileJsonGrant policy = grant(8, 1, 4, 96);
    ImmutableBase base = {0};
    ProfileVfs vfs = vfs_open(root, &policy, &base);
    write_json(vfs.store, &policy, SETTINGS, initial, SKSE_PROFILE_JSON_OK);
    SkseWinFile *writer = vfs_file(&vfs, SETTINGS, WRITE, BOTH, SKSE_WIN_CREATE_ALWAYS, 17, 183);
    vfs_fill(writer, updated);
    skse_win_file_close(writer);
    (void)vfs_stage(&vfs, false, 995, SKSE_PROFILE_JSON_OK, 0);
    (void)vfs_stage(&vfs, true, 995, SKSE_PROFILE_JSON_OK, 0);
    reads(vfs.store, &policy, SETTINGS, initial);
    vfs_close(&vfs);

    vfs = vfs_open(root, &policy, &base);
    writer = vfs_file(&vfs, SETTINGS, WRITE, BOTH, SKSE_WIN_CREATE_ALWAYS, 17, 183);
    assert(skse_win_file_resize(writer, SKSE_WIN_FILE_MAX_BYTES + 1u) == 223);
    skse_win_file_note_write(writer, false);
    vfs_fill(writer, updated);
    skse_win_file_close(writer);
    (void)vfs_stage(&vfs, true, 223, SKSE_PROFILE_JSON_OK, 0);
    reads(vfs.store, &policy, SETTINGS, initial);
    vfs_close(&vfs);

    vfs = vfs_open(root, &policy, &base);
    writer = vfs_file(&vfs, SETTINGS, WRITE, BOTH, SKSE_WIN_CREATE_ALWAYS, 17, 183);
    vfs_fill(writer, updated);
    skse_win_file_close(writer);
    writer = vfs_file(&vfs, EXTRA, WRITE, BOTH, SKSE_WIN_CREATE_ALWAYS, 17, 0);
    assert(skse_win_file_resize(writer, policy.file_bytes + 1) == 0);
    skse_win_file_close(writer);
    (void)vfs_stage(&vfs, true, 0, SKSE_PROFILE_JSON_LIMIT, 0); // Discard the already staged first file.
    reads(vfs.store, &policy, SETTINGS, initial);
    read_error(vfs.store, &policy, EXTRA, SIZE_MAX, SKSE_PROFILE_JSON_MISSING);
    vfs_close(&vfs);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    int scratch = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    assert(scratch >= 0 && !mkdirat(scratch, "app", 0700) && !mkdirat(scratch, "other", 0700));
    int root = openat(scratch, "app", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int outside = openat(scratch, "other", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    assert(root >= 0 && outside >= 0);
    test_scope_and_atomic_replacement(root);
    test_path_and_links(root, outside);
    test_case_and_limits(root);
    test_concurrent_quota(root);
    test_profile_vfs_sessions(root);
    test_profile_vfs_rejects_failed_outputs(root);
#ifdef SKSE_PROFILE_TEST_FAULTS
    test_failure_and_recovery(root, outside);
#endif
    assert(!close(root) && !close(outside) && !close(scratch));
    puts("Private JSON snapshots, per-profile grants, no-follow traversal, quota, and atomic publication passed.");
    return 0;
}
