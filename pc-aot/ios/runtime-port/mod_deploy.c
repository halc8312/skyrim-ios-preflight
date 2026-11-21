/* SPDX-License-Identifier: GPL-3.0-only
 * MOD data-root detection and launch-view deployment. Startup/UI only; never
 * runs while guest threads are using the view. */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include "mod_deploy.h"
#include "mod_archive.h"
#include "mod_plugins.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/clonefile.h>
#endif

/* ---- data root -------------------------------------------------------- */
static const char *const k_data_dirs[] = {"textures", "meshes", "scripts", "sound", "music",
    "interface", "strings", "seq", "shadersfx", "video", "lodsettings", "grass", "skse",
    "dialogueviews", "facegen", "materials", NULL};
typedef enum { NO_MARKER, GAME_DATA, TOOL_DATA } DataMarker;
static DataMarker data_marker(const char *rest) {
    const char *slash = strchr(rest, '/');
    if (slash) {
        size_t n = (size_t)(slash - rest);
        for (int i = 0; k_data_dirs[i]; ++i)
            if (strlen(k_data_dirs[i]) == n && !strncasecmp(rest, k_data_dirs[i], n)) return GAME_DATA;
        if ((n == 13 && !strncasecmp(rest, "calientetools", n)) ||
            (n == 6 && !strncasecmp(rest, "source", n))) return TOOL_DATA;
        return NO_MARKER;
    }
    const char *dot = strrchr(rest, '.');
    return dot && (!strcasecmp(dot, ".esp") || !strcasecmp(dot, ".esm") || !strcasecmp(dot, ".bsa"))
        ? GAME_DATA : NO_MARKER;
}
typedef struct { const char *path; size_t len; DataMarker marker; } Candidate;
static int compare_prefix(const Candidate *x, const Candidate *y) {
    size_t common = x->len < y->len ? x->len : y->len;
    int c = strncasecmp(x->path, y->path, common);
    return c ? c : (x->len > y->len) - (x->len < y->len);
}
static int candidate_cmp(const void *a, const void *b) {
    const Candidate *x = a, *y = b;
    int c = compare_prefix(x, y);
    return c ? c : strncmp(x->path, y->path, x->len);
}
static int is_fomod_config(const char *path) {
    static const char config[] = "fomod/ModuleConfig.xml";
    size_t length = strlen(path), suffix = sizeof config - 1;
    if (length < suffix) return 0;
    const char *component = path + length - suffix;
    return (component == path || component[-1] == '/') && !strcasecmp(component, config);
}
int skyrim_mod_data_root(const char *const *paths, size_t count, char *prefix, size_t capacity, int *installer) {
    if (installer) *installer = 0;
    if (!prefix || !capacity) return 0;
    prefix[0] = 0;
    if (!paths || !count || count > SIZE_MAX / sizeof(Candidate)) return 0;
    Candidate *c = malloc(count * sizeof *c);
    if (!c) return 0;
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        const char *p = paths[i];
        if (!p) continue;
        if (installer && !*installer && is_fomod_config(p)) *installer = 1;
        const char *rest = p;
        for (int depth = 0; depth < 5 && rest; ++depth) {
            DataMarker marker = data_marker(rest);
            if (marker) {
                c[n++] = (Candidate){p, (size_t)(rest - p), marker};
                break;
            }
            const char *slash = strchr(rest, '/');
            rest = slash ? slash + 1 : NULL;
        }
    }
    /* Windows packages can spell the same wrapper folder with different case. */
    qsort(c, n, sizeof *c, candidate_cmp);
    const Candidate *best = NULL;
    size_t best_score = 0;
    int best_has_game_data = 0;
    for (size_t i = 0; i < n;) {
        size_t j = i;
        int has_game_data = 0;
        while (j < n && !compare_prefix(&c[j], &c[i])) {
            has_game_data |= c[j].marker == GAME_DATA;
            ++j;
        }
        size_t score = j - i;
        if (score > best_score || (score == best_score && c[i].len < best->len)) {
            best = &c[i];
            best_score = score;
            best_has_game_data = has_game_data;
        }
        i = j;
    }
    /* Packages can ship a source project beside Data. Its files can outnumber
     * the runtime files and have similar names, such as src/skse/. When a
     * wrapper only scores for tool files, also check for explicit child Data. */
    if (best) {
        size_t length = best->len;
        int source_root = length >= 4 && (length == 4 || best->path[length - 5] == '/') &&
            !strncasecmp(best->path + length - 4, "src/", 4);
        if (source_root || !best_has_game_data) {
            size_t parent_len = source_root ? length - 4 : length;
            for (size_t i = 0; i < n; ++i) {
                if ((source_root || c[i].marker == GAME_DATA) && c[i].len == parent_len + 5 &&
                    !strncasecmp(c[i].path, best->path, parent_len) &&
                    !strncasecmp(c[i].path + parent_len, "data/", 5)) {
                    best = &c[i];
                    break;
                }
            }
        }
    }
    int ok = best && best->len < capacity;
    if (ok) {
        memcpy(prefix, best->path, best->len);
        prefix[best->len] = 0;
    }
    free(c);
    return ok;
}

/* ---- file helpers ----------------------------------------------------- */
static int copy_bytes(int in, const char *destination) {
    int out = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (out < 0) return errno;
    char buf[1 << 16]; ssize_t r; int e = 0;
    while (!e) {
        r = read(in, buf, sizeof buf);
        if (r == 0) break;
        if (r < 0) { if (errno != EINTR) e = errno; continue; }
        for (ssize_t pos = 0; pos < r;) {
            ssize_t n = write(out, buf + pos, (size_t)(r - pos));
            if (n > 0) pos += n;
            else if (n == 0 || errno != EINTR) { e = n == 0 ? EIO : errno; break; }
        }
    }
    if (close(out) && !e) e = errno;
    if (e) unlink(destination);
    return e;
}
static int clone_regular(const char *source, const char *destination, int game_input) {
    int fd = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno;
    struct stat st;
    int e = 0;
    if (fstat(fd, &st)) e = errno;
    else if (!S_ISREG(st.st_mode)) e = EINVAL;
    else {
#ifdef __APPLE__
        if (fclonefileat(fd, AT_FDCWD, destination, 0)) {
            e = errno;
            /* Never silently copy an entire game on a phone without COW. */
            if (!game_input && (e == ENOTSUP || e == EXDEV)) e = copy_bytes(fd, destination);
        }
#else
        (void)game_input;
        e = copy_bytes(fd, destination);
#endif
    }
    close(fd);
    return e;
}
int skyrim_clone_file(const char *source, const char *destination) {
    return clone_regular(source, destination, 0);
}
int skyrim_mod_stage_racemenu_preset(const char *source, const char *name, const char *data_dir) {
    if (!source || !name || !data_dir) return EINVAL;
    size_t length = strnlen(name, NAME_MAX + 1);
    if (!length || length > NAME_MAX || strchr(name, '/') || strchr(name, '\\')) return EINVAL;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || strcasecmp(dot, ".jslot")) return EINVAL;
    char safe[NAME_MAX + 2];
    if (skyrim_zip_normalize(name, length, 1, safe, sizeof safe) != 1) return EINVAL;
    static const char folder[] = "SKSE/Plugins/CharGen/Presets/";
    char relative[sizeof folder + sizeof safe - 1];
    snprintf(relative, sizeof relative, "%s%s", folder, safe);
    char target[4096];
    if ((size_t)snprintf(target, sizeof target, "%s/%s", data_dir, relative) >= sizeof target) return ENAMETOOLONG;
    int error = skyrim_make_parents(data_dir, relative);
    return error ? error : skyrim_clone_file(source, target);
}
static unsigned ece_slot_number(const char *name) {
    if (!name || name[0] != '_' || name[1] < '1' || name[1] > '9') return 0;
    unsigned number = (unsigned)(name[1] - '0');
    const char *suffix = name + 2;
    if (*suffix >= '0' && *suffix <= '9') number = number * 10 + (unsigned)(*suffix++ - '0');
    return number <= 40 && !strcasecmp(suffix, "_commonTriRaces.slot") ? number : 0;
}

static int ece_find_name(int dir, const char *name, char found[NAME_MAX + 1]) {
    found[0] = 0;
    int fd = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return errno;
    DIR *scan = fdopendir(fd);
    if (!scan) {
        int error = errno;
        close(fd);
        return error;
    }
    int error = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(scan);
        if (!entry) { error = errno; break; }
        if (strcasecmp(entry->d_name, name)) continue;
        if (found[0]) { error = EEXIST; break; }
        memcpy(found, entry->d_name, strlen(name) + 1);
    }
    if (closedir(scan) && !error) error = errno;
    return error;
}

static int ece_open_slot_directory(int profile) {
    const int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    static const char *const folders[] = {"Documents", "My Games", "Skyrim", "CME_save"};
    int current = openat(profile, ".", flags);
    if (current < 0) return -1;
    for (size_t i = 0; i < sizeof folders / sizeof folders[0]; ++i) {
        const char *name = folders[i];
        char found[NAME_MAX + 1];
        int error = ece_find_name(current, name, found);
        int next = -1;
        if (error == EEXIST) error = EINVAL; /* Ambiguous folder, not an occupied slot. */
        if (!error) {
            if (found[0]) name = found;
            else if (mkdirat(current, name, 0700) && errno != EEXIST) error = errno;
        }
        if (!error) {
            next = openat(current, name, flags);
            if (next < 0) error = errno;
        }
        close(current);
        if (error) { errno = error; return -1; }
        current = next;
    }
    return current;
}

int skyrim_mod_publish_ece_slot(int profile, const char *staged, const char *name, unsigned *slot) {
    if (!slot) return EINVAL;
    const unsigned number = ece_slot_number(name);
    *slot = number;
    if (!number || profile < 0 || !staged || !*staged) return EINVAL;

    struct stat source;
    if (lstat(staged, &source)) return errno;
    if (!S_ISREG(source.st_mode) || source.st_nlink != 1 || source.st_size <= 0) return EINVAL;
    char filename[sizeof "_40_commonTriRaces.slot"];
    snprintf(filename, sizeof filename, "_%u_commonTriRaces.slot", number);
    int dir = ece_open_slot_directory(profile);
    if (dir < 0) return errno;
    char found[NAME_MAX + 1];
    int error = ece_find_name(dir, filename, found);
    if (!error && found[0]) error = EEXIST;
    // The app-owned Inbox file is complete, on the same volume, and removed by the caller.
    if (!error && linkat(AT_FDCWD, staged, dir, filename, 0)) error = errno;
    close(dir);
    return error;
}

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw;
    return remove(path) && errno != ENOENT ? -1 : 0;
}
int skyrim_remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st)) return errno == ENOENT ? 0 : errno;
    if (!S_ISDIR(st.st_mode)) return unlink(path) ? errno : 0;
    return nftw(path, remove_entry, 32, FTW_DEPTH | FTW_PHYS) ? (errno ? errno : EIO) : 0;
}

typedef int (*WalkFn)(void *ctx, const char *relative, const char *name, int is_dir, const struct stat *st);
static int walk(const char *root, char *relative, size_t len, size_t cap, int depth, WalkFn fn, void *ctx) {
    if (depth > 64) return ELOOP;
    char path[4096];
    if (snprintf(path, sizeof path, "%s%s%s", root, len ? "/" : "", relative) >= (int)sizeof path) return ENAMETOOLONG;
    struct stat dir_st;
    if (lstat(path, &dir_st)) return errno;
    if (!S_ISDIR(dir_st.st_mode)) return ENOTDIR;
    DIR *dir = opendir(path);
    if (!dir) return errno;
    struct dirent *entry; int e = 0;
    while (!e && (entry = readdir(dir))) {
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        size_t nlen = strlen(name);
        if (len + nlen + 2 > cap) { e = ENAMETOOLONG; break; }
        if (len) relative[len] = '/';
        memcpy(relative + len + (len ? 1 : 0), name, nlen + 1);
        size_t child = len + (len ? 1 : 0) + nlen;
        char full[4096]; struct stat st;
        if (snprintf(full, sizeof full, "%s/%s", root, relative) >= (int)sizeof full) { e = ENAMETOOLONG; break; }
        if (lstat(full, &st)) { e = errno; }
        else if (S_ISDIR(st.st_mode)) {
            e = fn(ctx, relative, name, 1, &st);
            if (!e) e = walk(root, relative, child, cap, depth + 1, fn, ctx);
        } else if (S_ISREG(st.st_mode)) e = fn(ctx, relative, name, 0, &st);
        else e = EINVAL;
        relative[len] = 0;
    }
    closedir(dir);
    return e;
}

/* ---- signature -------------------------------------------------------- */
static uint64_t fnv(const void *data, size_t n, uint64_t h) {
    const unsigned char *p = data;
    while (n--) { h ^= *p++; h *= 0x100000001b3ull; }
    return h;
}
static uint64_t mix(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; return x; }
static int signature_fn(void *ctx, const char *relative, const char *name, int is_dir, const struct stat *st) {
    (void)name;
    uint64_t h = fnv(relative, strlen(relative), 0xcbf29ce484222325ull);
    if (!is_dir) {
        int64_t v[3] = {(int64_t)st->st_size, (int64_t)st->st_mtime, (int64_t)st->st_ino};
        h = fnv(v, sizeof v, h);
    }
    *(uint64_t *)ctx += mix(h);
    return 0;
}
int skyrim_tree_signature(const char *dir, uint64_t *signature) {
    char rel[4096] = "";
    *signature = 0;
    return walk(dir, rel, 0, sizeof rel, 0, signature_fn, signature);
}

/* ---- case-insensitive map of the view's Data tree ---------------------- */
typedef struct { char *key, *actual; int owner; unsigned char dir; } Slot;
typedef struct { Slot *s; size_t cap, n; } Map;
static uint64_t fold_hash(const char *k) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *k; ++k) { unsigned char c = (unsigned char)*k; if (c >= 'A' && c <= 'Z') c += 32; h = (h ^ c) * 0x100000001b3ull; }
    return h;
}
static Slot *map_find(Map *m, const char *key, int create) {
    if (create && (m->n + 1) * 2 > m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 4096;
        Slot *s = calloc(cap, sizeof *s);
        if (!s) return NULL;
        for (size_t i = 0; i < m->cap; ++i) if (m->s[i].key) {
            size_t j = fold_hash(m->s[i].key) & (cap - 1);
            while (s[j].key) j = (j + 1) & (cap - 1);
            s[j] = m->s[i];
        }
        free(m->s); m->s = s; m->cap = cap;
    }
    if (!m->cap) return NULL;
    size_t j = fold_hash(key) & (m->cap - 1);
    while (m->s[j].key) {
        if (!strcasecmp(m->s[j].key, key)) return &m->s[j];
        j = (j + 1) & (m->cap - 1);
    }
    if (!create) return NULL;
    m->s[j].key = strdup(key);
    if (!m->s[j].key) return NULL;
    ++m->n;
    return &m->s[j];
}
static void map_free(Map *m) {
    for (size_t i = 0; i < m->cap; ++i) { free(m->s[i].key); free(m->s[i].actual); }
    free(m->s);
}

typedef struct {
    Map map; const char *data_dir, *package; int owner; FILE *conflicts;
    char *err; size_t err_cap;
} Deploy;
static int index_fn(void *ctx, const char *relative, const char *name, int is_dir, const struct stat *st) {
    (void)name; (void)st;
    Deploy *d = ctx;
    Slot *s = map_find(&d->map, relative, 1);
    if (!s) return ENOMEM;
    if (s->actual && strcmp(s->actual, relative)) return EEXIST;
    if (!s->actual) { s->actual = strdup(relative); s->owner = -1; s->dir = (unsigned char)is_dir; }
    return s->actual ? 0 : ENOMEM;
}
static int protected_original(const Slot *s) {
    const char *dot = strrchr(s->key, '.');
    if (!dot) return 0;
    if (!strcasecmp(dot, ".exe") || !strcasecmp(dot, ".dll")) return 1;
    return !strchr(s->key, '/') && (!strcasecmp(dot, ".esm") || !strcasecmp(dot, ".bsa"));
}
static int overlay_fn(void *ctx, const char *relative, const char *name, int is_dir, const struct stat *st) {
    (void)st;
    Deploy *d = ctx;
    /* Actual parent path in the view keeps the first casing ever seen. */
    char parent_actual[4096] = "";
    const char *slash = strrchr(relative, '/');
    if (slash) {
        char parent[4096];
        size_t n = (size_t)(slash - relative);
        memcpy(parent, relative, n); parent[n] = 0;
        Slot *p = map_find(&d->map, parent, 0);
        if (!p || !p->dir) return 0;      /* parent was refused (file in the way) */
        snprintf(parent_actual, sizeof parent_actual, "%s", p->actual);
    }
    char actual[4096];
    snprintf(actual, sizeof actual, "%s%s%s", parent_actual, *parent_actual ? "/" : "", name);
    Slot *s = map_find(&d->map, relative, 1);
    if (!s) return ENOMEM;
    char dst[8192];
    if (is_dir) {
        if (s->actual) {
            if (!s->dir && d->conflicts) fprintf(d->conflicts, "%s\t%d\t%d\n", s->actual, d->owner, s->owner);
            return 0;
        }
        snprintf(dst, sizeof dst, "%s/%s", d->data_dir, actual);
        if (mkdir(dst, 0755)) return errno;
        s->actual = strdup(actual); s->owner = d->owner; s->dir = 1;
        return s->actual ? 0 : ENOMEM;
    }
    char src[8192];
    snprintf(src, sizeof src, "%s/%s", d->package, relative);
    if (s->actual) {
        if (s->dir || (s->owner == -1 && protected_original(s))) {
            if (d->conflicts) fprintf(d->conflicts, "%s\t%d\t%d\n", s->actual, d->owner, s->owner);
            return 0;
        }
        if (d->conflicts) fprintf(d->conflicts, "%s\t%d\t%d\n", s->actual, s->owner, d->owner);
        snprintf(dst, sizeof dst, "%s/%s", d->data_dir, s->actual);
        if (unlink(dst) && errno != ENOENT) return errno;
        s->owner = d->owner;
    } else {
        s->actual = strdup(actual); s->owner = d->owner; s->dir = 0;
        if (!s->actual) return ENOMEM;
        snprintf(dst, sizeof dst, "%s/%s", d->data_dir, actual);
    }
    int e = skyrim_clone_file(src, dst);
    if (e) snprintf(d->err, d->err_cap, "cannot place %s (%d)", relative, e);
    return e;
}
static int clone_tree_fn(void *ctx, const char *relative, const char *name, int is_dir, const struct stat *st) {
    (void)name; (void)st;
    const char *const *roots = ctx;
    char src[8192], dst[8192];
    snprintf(src, sizeof src, "%s/%s", roots[0], relative);
    snprintf(dst, sizeof dst, "%s/%s", roots[1], relative);
    if (is_dir) return mkdir(dst, 0755) ? errno : 0;
    return clone_regular(src, dst, 1);
}
static int find_child(const char *dir, const char *name, char *out, size_t cap) {
    return skyrim_plugin_child_path(dir, name, out, cap);
}

#define CONFLICT_KEY_PREFIX "# SkyrimIOS deployment "

int skyrim_deploy_matches(const char *view_dir, const char *conflicts_path, const char *key) {
    char path[4096], marker[1024];
    int n = snprintf(path, sizeof path, "%s/.skyrimios-deploy", view_dir);
    if (n < 0 || (size_t)n >= sizeof path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int complete = fgets(marker, sizeof marker, f) && strchr(marker, '\n') &&
                   fgetc(f) == EOF && !ferror(f);
    fclose(f);
    if (!complete) return 0;
    if (key) {
        size_t k = strlen(key);
        if (strlen(marker) != k + 1 || memcmp(marker, key, k)) return 0;
    }
    if (!conflicts_path) return 1;

    f = fopen(conflicts_path, "rb");
    if (!f) return 0;
    char header[sizeof CONFLICT_KEY_PREFIX - 1 + sizeof marker];
    size_t prefix_length = sizeof CONFLICT_KEY_PREFIX - 1;
    complete = fgets(header, sizeof header, f) &&
               !strncmp(header, CONFLICT_KEY_PREFIX, prefix_length) &&
               !strcmp(header + prefix_length, marker);
    fclose(f);
    return complete;
}

int skyrim_deploy_build(const char *game_dir, const char *const *package_roots, size_t count,
                        const char *view_dir, const char *conflicts_path, const char *key,
                        char *err, size_t err_cap) {
    char building[4096], old[4096], data[4096], pending_conflicts[4096] = "";
    if (strlen(view_dir) > 3000 || strlen(game_dir) > 3000) return ENAMETOOLONG;
    if (err && err_cap) err[0] = 0;
    snprintf(building, sizeof building, "%s.building", view_dir);
    snprintf(old, sizeof old, "%s.old", view_dir);
    int e = skyrim_remove_tree(building);
    struct stat previous;
    if (!e && !lstat(old, &previous)) {
        if (lstat(view_dir, &previous)) {
            if (errno != ENOENT || rename(old, view_dir)) e = errno;
        } else if (!conflicts_path || skyrim_deploy_matches(view_dir, conflicts_path, NULL)) {
            e = skyrim_remove_tree(old);
        } else {
            /* The new view became visible before its report. Keep the old
             * generation even when this invocation cannot build a replacement. */
            if (rename(view_dir, building) || rename(old, view_dir)) e = errno;
            else e = skyrim_remove_tree(building);
        }
    }
    if (e) { snprintf(err, err_cap, "cannot clear previous build (%d)", e); return e; }
    if (mkdir(building, 0755)) { e = errno; snprintf(err, err_cap, "cannot create view (%d)", e); return e; }
    {
        const char *roots[2] = {game_dir, building};
        char rel[4096] = "";
        e = walk(game_dir, rel, 0, sizeof rel, 0, clone_tree_fn, (void *)roots);
        if (e) { snprintf(err, err_cap, "cannot copy the game folder (%d)", e); skyrim_remove_tree(building); return e; }
    }
    if (find_child(building, "Data", data, sizeof data) < 0) { skyrim_remove_tree(building); snprintf(err, err_cap, "ambiguous Data folder"); return EEXIST; }
    if (mkdir(data, 0755) && errno != EEXIST) { e = errno; skyrim_remove_tree(building); return e; }
    Deploy d = {0};
    d.data_dir = data; d.err = err; d.err_cap = err_cap;
    if (conflicts_path) {
        snprintf(pending_conflicts, sizeof pending_conflicts, "%s.conflicts.XXXXXX", view_dir);
        int fd = mkstemp(pending_conflicts);
        if (fd < 0) e = errno;
        else if (!(d.conflicts = fdopen(fd, "w"))) { e = errno; close(fd); }
        else if (fprintf(d.conflicts, CONFLICT_KEY_PREFIX "%s\n", key ? key : "") < 0) e = EIO;
    }
    char rel[4096] = "";
    if (!e) e = walk(data, rel, 0, sizeof rel, 0, index_fn, &d);
    for (size_t i = 0; !e && i < count; ++i) {
        d.package = package_roots[i]; d.owner = (int)i;
        rel[0] = 0;
        e = walk(package_roots[i], rel, 0, sizeof rel, 0, overlay_fn, &d);
    }
    if (d.conflicts) {
        if (ferror(d.conflicts) && !e) e = EIO;
        if (fclose(d.conflicts) && !e) e = errno;
    }
    map_free(&d.map);
    if (!e) {
        char marker[8192];
        snprintf(marker, sizeof marker, "%s/.skyrimios-deploy", building);
        FILE *f = fopen(marker, "wbx");
        if (!f || fprintf(f, "%s\n", key ? key : "") < 0) e = EIO;
        if (f && fclose(f) && !e) e = EIO;
    }
    if (!e) {
        struct stat st;
        int had_view = !lstat(view_dir, &st);
        if (!had_view && errno != ENOENT) e = errno;
        else if (had_view && rename(view_dir, old)) e = errno;
        else if (rename(building, view_dir)) {
            e = errno;
            if (had_view && rename(old, view_dir)) e = errno;
        }
        else if (conflicts_path && rename(pending_conflicts, conflicts_path)) {
            e = errno;
            if (rename(view_dir, building) || (had_view && rename(old, view_dir))) e = errno;
        }
        if (!e) skyrim_remove_tree(old);
    }
    if (e) {
        if (*pending_conflicts) unlink(pending_conflicts);
        if (err && !err[0]) snprintf(err, err_cap, "deploy failed (%d)", e);
        skyrim_remove_tree(building);
    }
    return e;
}
