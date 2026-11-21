/* SPDX-License-Identifier: GPL-3.0-only */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#elif defined(__linux__)
#define _GNU_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "probe_directory.h"
#include "probe_asset_file.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char relative[SKSE_PROBE_DIRECTORY_PATH_BYTES];
    uint8_t *bytes;
    size_t length;
    int fd, parent;
    struct stat snapshot;
    SkseProbeDirectoryFileStatus status;
} Source;

typedef struct {
    Source files[SKSE_PROBE_DIRECTORY_MAX_FILES];
    unsigned count, entries;
    size_t budget_used;
} DirectoryScan;

typedef enum { ROOT, DATA, SKSE, PLUGINS } Location;
typedef struct {
    const char *wanted;
    Location location;
    char found[8]; /* Data, SKSE or Plugins in its original case. */
    struct stat snapshot;
} Child;

static bool same_file(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static bool unchanged_regular(const struct stat *a, const struct stat *b) {
    if (!same_file(a, b) || !S_ISREG(b->st_mode) || b->st_nlink != 1 || a->st_size != b->st_size) return false;
#ifdef __APPLE__
    return a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec && a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
           a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec && a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    return a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}

static bool dll_name(const char *name) {
    size_t length = strlen(name);
    return length > 4 && skse_pe_library_name_equal(name + length - 4, ".dll");
}

static bool join(char *out, const char *prefix, const char *component) {
    int n = snprintf(out, SKSE_PROBE_DIRECTORY_PATH_BYTES, "%s%s%s", prefix, *prefix ? "/" : "", component);
    return n >= 0 && n < SKSE_PROBE_DIRECTORY_PATH_BYTES;
}

static ssize_t read_retry(int fd, void *out, size_t count) {
    ssize_t got;
    do { got = read(fd, out, count); } while (got < 0 && errno == EINTR);
    return got;
}

static bool read_snapshot(int parent, const char *name, int fd, const struct stat *before, Source *source) {
    size_t at = 0;
    while (at < source->length) {
        ssize_t n = read_retry(fd, source->bytes + at, source->length - at);
        if (n <= 0) return false;
        at += (size_t)n;
    }
    uint8_t extra;
    struct stat after;
    return read_retry(fd, &extra, 1) == 0 && fstat(fd, &after) == 0 && unchanged_regular(before, &after) &&
           fstatat(parent, name, &after, AT_SYMLINK_NOFOLLOW) == 0 && unchanged_regular(before, &after);
}

static SkseProbeDirectoryStatus collect(DirectoryScan *scan, int parent, const char *prefix, const char *name) {
    if (scan->count == SKSE_PROBE_DIRECTORY_MAX_FILES) return SKSE_PROBE_DIRECTORY_FILE_LIMIT;
    Source *source = &scan->files[scan->count++];
    source->fd = source->parent = -1;
    if (!join(source->relative, prefix, name)) return SKSE_PROBE_DIRECTORY_INCOMPLETE;
    source->status = SKSE_PROBE_DLL_UNREADABLE;
    struct stat listed, opened;
    if (fstatat(parent, name, &listed, AT_SYMLINK_NOFOLLOW) != 0) return SKSE_PROBE_DIRECTORY_CHECKED;
    if (!S_ISREG(listed.st_mode) || listed.st_nlink != 1) {
        source->status = SKSE_PROBE_DLL_NOT_REGULAR;
        return SKSE_PROBE_DIRECTORY_CHECKED;
    }
    if (listed.st_size <= 0 || listed.st_size > SKSE_PE_MAX_FILE_BYTES) {
        source->status = listed.st_size <= 0 ? SKSE_PROBE_DLL_EMPTY : SKSE_PROBE_DLL_TOO_LARGE;
        return SKSE_PROBE_DIRECTORY_CHECKED;
    }
    size_t length = (size_t)listed.st_size;
    if (length > SKSE_PE_MAX_FILE_BYTES - scan->budget_used) return SKSE_PROBE_DIRECTORY_SIZE_LIMIT;
    int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return SKSE_PROBE_DIRECTORY_CHECKED;
    if (fstat(fd, &opened) != 0 || !unchanged_regular(&listed, &opened)) {
        close(fd);
        return SKSE_PROBE_DIRECTORY_CHECKED;
    }
    source->parent = fcntl(parent, F_DUPFD_CLOEXEC, 0);
    if (source->parent < 0) {
        close(fd);
        return SKSE_PROBE_DIRECTORY_INCOMPLETE;
    }
    scan->budget_used += length;
    source->length = length;
    source->fd = fd;
    source->snapshot = opened;
    return SKSE_PROBE_DIRECTORY_CHECKED;
}

static unsigned child_paths(Location location, Child children[2]) {
    if (location == ROOT) {
        children[0] = (Child){.wanted = "Data", .location = DATA};
        children[1] = (Child){.wanted = "SKSE", .location = SKSE};
        return 2;
    }
    if (location == DATA || location == SKSE) {
        children[0] = location == DATA ? (Child){.wanted = "SKSE", .location = SKSE}
                                       : (Child){.wanted = "Plugins", .location = PLUGINS};
        return 1;
    }
    return 0;
}

/* Takes ownership of fd. Recursion has at most three fixed path components. */
static SkseProbeDirectoryStatus scan_folder(DirectoryScan *scan, int fd, Location location, const char *prefix) {
    DIR *directory = fdopendir(fd);
    if (!directory) {
        close(fd);
        return SKSE_PROBE_DIRECTORY_INCOMPLETE;
    }
    Child children[2] = {0};
    unsigned child_count = child_paths(location, children);
    SkseProbeDirectoryStatus status = SKSE_PROBE_DIRECTORY_CHECKED;
    while (status == SKSE_PROBE_DIRECTORY_CHECKED) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) {
            if (errno) status = SKSE_PROBE_DIRECTORY_INCOMPLETE;
            break;
        }
        const char *name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (++scan->entries > SKSE_PROBE_DIRECTORY_MAX_ENTRIES) {
            status = SKSE_PROBE_DIRECTORY_ENTRY_LIMIT;
            break;
        }
        if ((location == ROOT || location == PLUGINS) && dll_name(name)) {
            status = collect(scan, fd, prefix, name);
            continue;
        }
        for (unsigned i = 0; i < child_count; ++i) {
            Child *child = &children[i];
            if (!skse_pe_library_name_equal(name, child->wanted)) continue;
            struct stat item;
            if (fstatat(fd, name, &item, AT_SYMLINK_NOFOLLOW) != 0) status = SKSE_PROBE_DIRECTORY_INCOMPLETE;
            else if (S_ISDIR(item.st_mode)) {
                if (child->found[0]) status = SKSE_PROBE_DIRECTORY_AMBIGUOUS_LAYOUT;
                else {
                    memcpy(child->found, name, strlen(child->wanted) + 1);
                    child->snapshot = item;
                }
            }
            break;
        }
    }
    for (unsigned i = 0; i < child_count && status == SKSE_PROBE_DIRECTORY_CHECKED; ++i) {
        const Child *child = &children[i];
        if (!child->found[0]) continue;
        char relative[SKSE_PROBE_DIRECTORY_PATH_BYTES];
        int sub = openat(fd, child->found, O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        struct stat opened;
        if (sub < 0 || !join(relative, prefix, child->found) || fstat(sub, &opened) != 0 ||
            !same_file(&opened, &child->snapshot)) {
            if (sub >= 0) close(sub);
            status = SKSE_PROBE_DIRECTORY_INCOMPLETE;
        } else {
            status = scan_folder(scan, sub, child->location, relative);
        }
    }
    if (closedir(directory) != 0 && status == SKSE_PROBE_DIRECTORY_CHECKED) status = SKSE_PROBE_DIRECTORY_INCOMPLETE;
    return status;
}

static int compare_sources(const void *a, const void *b) {
    return strcmp(((const Source *)a)->relative, ((const Source *)b)->relative);
}

static int open_selected(const char *path) {
    const int dir_flags = O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
#if defined(O_SEARCH)
    const int walk_flags = dir_flags | O_SEARCH;
#elif defined(O_PATH)
    const int walk_flags = dir_flags | O_PATH;
#else
    const int walk_flags = dir_flags | O_RDONLY;
#endif
    char components[PATH_MAX];
    size_t length = strnlen(path, sizeof components);
    if (!length || length == sizeof components) return -1;
    memcpy(components, path, length + 1);
    bool absolute = path[0] == '/';
    int parent = open(absolute ? "/" : ".", walk_flags);
    char *state = NULL;
    for (char *part = strtok_r(components, "/", &state); part && parent >= 0; part = strtok_r(NULL, "/", &state)) {
        if (strcmp(part, ".") == 0) continue;
        int next = -1;
        if (strcmp(part, "..") != 0) next = openat(parent, part, walk_flags);
#ifdef __APPLE__
        /* Files can supply /var/mobile or /private/var/mobile. Resolve only the
         * system's fixed top-level aliases by opening their physical components. */
        if (next < 0 && absolute && (strcmp(part, "var") == 0 || strcmp(part, "tmp") == 0)) {
            char target[32];
            ssize_t size = readlinkat(parent, part, target, sizeof target - 1);
            if (size > 0 && (size_t)size < sizeof target - 1) {
                target[size] = 0;
                const char *relative = target[0] == '/' ? target + 1 : target;
                if (strncmp(relative, "private/", 8) == 0 && strcmp(relative + 8, part) == 0) {
                    int physical = openat(parent, "private", walk_flags);
                    if (physical >= 0) {
                        next = openat(physical, part, walk_flags);
                        close(physical);
                    }
                }
            }
        }
#endif
        close(parent);
        parent = next;
        absolute = false;
    }
    if (parent < 0) return -1;
    /* The picker authorizes listing the selected folder. Its parents only
     * need directory-search permission, including on file-provider volumes. */
    int selected = openat(parent, ".", dir_flags | O_RDONLY);
    close(parent);
    return selected;
}

static const char *base_name(const Source *source) {
    const char *slash = strrchr(source->relative, '/');
    return slash ? slash + 1 : source->relative;
}

static void close_source(Source *source) {
    if (source->fd >= 0) close(source->fd);
    if (source->parent >= 0) close(source->parent);
    source->fd = source->parent = -1;
}

/* All directory/size limits are known before the first payload read. Each file
 * is opened just once and the same immutable byte snapshot is used for every pair. */
static SkseProbeDirectoryStatus snapshot_all(DirectoryScan *scan) {
    for (unsigned i = 0; i < scan->count; ++i) {
        Source *source = &scan->files[i];
        if (source->fd < 0) continue;
        source->bytes = malloc(source->length);
        if (!source->bytes) return SKSE_PROBE_DIRECTORY_NO_MEMORY;
        if (read_snapshot(source->parent, base_name(source), source->fd, &source->snapshot, source)) {
            source->status = SKSE_PROBE_DLL_NOT_PLUGIN;
        } else {
            free(source->bytes);
            source->bytes = NULL;
        }
        close_source(source);
    }
    return SKSE_PROBE_DIRECTORY_CHECKED;
}

static unsigned external_names(const Source *source, SksePeReport *imports, char first[SKSE_PE_MAX_IMPORT_NAME], char *second) {
    SksePeStatus status = skse_pe_inspect(source->bytes, source->length, imports);
    if (status != SKSE_PE_OK && status != SKSE_PE_UNSUPPORTED_IMPORTS && status != SKSE_PE_UNSUPPORTED_TLS) return 0;
    unsigned count = 0;
    for (uint16_t i = 0; i < imports->library_count; ++i) {
        const char *name = imports->libraries[i];
        if (skse_trial_system_library(name)) continue;
        uint16_t j = 0;
        while (j < i && !skse_pe_library_name_equal(name, imports->libraries[j])) ++j;
        if (j != i) continue;
        if (count == 0) memcpy(first, name, strlen(name) + 1);
        if (count == 1 && second) memcpy(second, name, strlen(name) + 1);
        ++count;
    }
    return count;
}

static void diagnose(const DirectoryScan *scan, const Source *source, uint32_t uptime_ms, SksePeReport *manifest,
                     SkseWinAssets assets, SkseProbeDirectoryFile *file) {
    memcpy(file->relative_path, source->relative, strlen(source->relative) + 1);
    file->status = source->status;
    file->export_status = SKSE_PE_BAD_ARGUMENT;
    if (!source->bytes) return;
    file->export_status = skse_pe_classic_exports(source->bytes, source->length);
    if (file->export_status != SKSE_PE_OK) return;
    file->is_plugin = 1;
    file->external_library_count = external_names(source, manifest, file->dependency_name, file->other_dependency_name);
    const SkseTrialOptions options = {0x01070030, 0x09200000, 0, 48, 1, 1, 1};
    const SkseTrialRuntime runtime = {
        .uptime_ms = uptime_ms, .core_messages = 1, .static_tls = 1, .serialization = 1, .worker_thread = 1,
        .task_interface = 1, .trace_known_windows_imports = true, .assets = assets, .module_file_name = base_name(source)
    };
    if (!file->external_library_count) {
        file->status = SKSE_PROBE_DLL_STANDALONE;
        file->trial = skse_plugin_trial_with_runtime(source->bytes, source->length, options, runtime);
        file->trial_ran = 1;
        return;
    }
    SkseTrialDependency catalog[SKSE_PROBE_DIRECTORY_MAX_FILES];
    size_t count = 0;
    for (unsigned i = 0; i < scan->count; ++i) {
        const Source *item = &scan->files[i];
        if (item != source) catalog[count++] = (SkseTrialDependency){base_name(item), item->bytes, item->length};
    }
    SkseTrialDependency primary = {base_name(source), source->bytes, source->length};
    file->trial = skse_plugin_trial_with_package(primary, catalog, count, options, runtime);
    const SkseDependencyTrace *trace = &file->trial.dependencies;
    switch (trace->status) {
    case SKSE_DEPENDENCY_READY:
    case SKSE_DEPENDENCY_INVALID_IMAGE:
    case SKSE_DEPENDENCY_NO_MEMORY:
        file->status = SKSE_PROBE_DLL_PAIRED;
        file->trial_ran = 1;
        return;
    case SKSE_DEPENDENCY_MISSING: file->status = SKSE_PROBE_DLL_DEPENDENCY_MISSING; break;
    case SKSE_DEPENDENCY_AMBIGUOUS: file->status = SKSE_PROBE_DLL_DEPENDENCY_AMBIGUOUS; break;
    case SKSE_DEPENDENCY_SELF: file->status = SKSE_PROBE_DLL_DEPENDENCY_SELF; break;
    case SKSE_DEPENDENCY_CYCLE: file->status = SKSE_PROBE_DLL_DEPENDENCY_CYCLE; break;
    case SKSE_DEPENDENCY_UNREADABLE: file->status = SKSE_PROBE_DLL_DEPENDENCY_UNREADABLE; break;
    case SKSE_DEPENDENCY_INVALID_NAME: file->status = SKSE_PROBE_DLL_DEPENDENCY_INVALID; break;
    case SKSE_DEPENDENCY_LIMIT:
    case SKSE_DEPENDENCY_SIZE_LIMIT:
    case SKSE_DEPENDENCY_ADDRESS_SPACE: file->status = SKSE_PROBE_DLL_DEPENDENCY_LIMIT; break;
    }
    memcpy(file->dependency_name, trace->requested, sizeof file->dependency_name);
    memcpy(file->other_dependency_name, trace->requester, sizeof file->other_dependency_name);
}

SkseProbeDirectoryStatus skse_probe_local_directory(const char *path, uint32_t uptime_ms, SkseProbeDirectoryReport *out) {
    if (!out) return SKSE_PROBE_DIRECTORY_UNAVAILABLE;
    memset(out, 0, sizeof *out);
    if (!path || !*path) return SKSE_PROBE_DIRECTORY_UNAVAILABLE;
    int root = open_selected(path);
    if (root < 0) return SKSE_PROBE_DIRECTORY_UNAVAILABLE;
    int listing = openat(root, ".", O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (listing < 0) {
        close(root);
        return SKSE_PROBE_DIRECTORY_INCOMPLETE;
    }
    DirectoryScan scan = {0};
    SkseProbeDirectoryStatus status = scan_folder(&scan, listing, ROOT, "");
    if (status == SKSE_PROBE_DIRECTORY_CHECKED) {
        qsort(scan.files, scan.count, sizeof scan.files[0], compare_sources);
        status = snapshot_all(&scan);
    }
    SksePeReport *manifest = NULL;
    if (status == SKSE_PROBE_DIRECTORY_CHECKED && scan.count) {
        manifest = malloc(sizeof *manifest);
        if (!manifest) status = SKSE_PROBE_DIRECTORY_NO_MEMORY;
    }
    if (status == SKSE_PROBE_DIRECTORY_CHECKED) {
        SkseWinAssets assets = {.read = skse_probe_asset_read, .attributes = skse_probe_asset_attributes, .context = &root};
        for (unsigned i = 0; i < scan.count; ++i) {
            diagnose(&scan, &scan.files[i], uptime_ms, manifest, assets, &out->files[i]);
            out->plugin_count += out->files[i].is_plugin;
        }
        out->file_count = scan.count;
        if (!out->plugin_count) status = SKSE_PROBE_DIRECTORY_NO_PLUGINS;
    }
    free(manifest);
    for (unsigned i = 0; i < scan.count; ++i) {
        close_source(&scan.files[i]);
        free(scan.files[i].bytes);
    }
    close(root);
    return status;
}
