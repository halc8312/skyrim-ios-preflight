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
    "dialogueviews", "facegen", "materials", "calientetools", "source", NULL};
static int data_marker(const char *rest) {
    const char *slash = strchr(rest, '/');
    if (slash) {
        size_t n = (size_t)(slash - rest);
        for (int i = 0; k_data_dirs[i]; ++i)
            if (strlen(k_data_dirs[i]) == n && !strncasecmp(rest, k_data_dirs[i], n)) return 1;
        return 0;
    }
    const char *dot = strrchr(rest, '.');
    return dot && (!strcasecmp(dot, ".esp") || !strcasecmp(dot, ".esm") || !strcasecmp(dot, ".bsa"));
}
typedef struct { char *prefix; size_t len; } Candidate;
static int candidate_cmp(const void *a, const void *b) {
    const Candidate *x = a, *y = b;
    int c = strcmp(x->prefix, y->prefix);
    return c ? c : (x->len > y->len) - (x->len < y->len);
}
int skyrim_mod_data_root(const char *const *paths, size_t count, char *prefix, size_t capacity, int *installer) {
    if (installer) *installer = 0;
    if (!prefix || !capacity) return 0;
    prefix[0] = 0;
    Candidate *c = malloc((count ? count : 1) * 5 * sizeof *c);
    if (!c) return 0;
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        const char *p = paths[i];
        const char *fomod = strcasestr(p, "fomod/moduleconfig.xml");
        if (installer && fomod && (fomod == p || fomod[-1] == '/')) *installer = 1;
        const char *rest = p;
        for (int depth = 0; depth < 5 && rest; ++depth) {
            if (data_marker(rest)) { c[n].prefix = (char *)p; c[n].len = (size_t)(rest - p); ++n; break; }
            const char *slash = strchr(rest, '/');
            rest = slash ? slash + 1 : NULL;
        }
    }
    /* Count each distinct prefix (case-sensitive, as stored in the archive). */
    char **keys = malloc((n ? n : 1) * sizeof *keys);
    if (!keys) { free(c); return 0; }
    for (size_t i = 0; i < n; ++i) {
        keys[i] = malloc(c[i].len + 1);
        if (!keys[i]) { for (size_t k = 0; k < i; ++k) free(keys[k]); free(keys); free(c); return 0; }
        memcpy(keys[i], c[i].prefix, c[i].len); keys[i][c[i].len] = 0;
        c[i].prefix = keys[i];
    }
    qsort(c, n, sizeof *c, candidate_cmp);
    size_t best = 0, best_len = 0; const char *best_prefix = NULL;
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && !strcmp(c[j].prefix, c[i].prefix)) ++j;
        size_t score = j - i;
        if (score > best || (score == best && c[i].len < best_len)) { best = score; best_len = c[i].len; best_prefix = c[i].prefix; }
        i = j;
    }
    int ok = 0;
    if (best_prefix && best_len < capacity) { memcpy(prefix, best_prefix, best_len + 1); ok = 1; }
    for (size_t i = 0; i < n; ++i) free(keys[i]);
    free(keys); free(c);
    return ok;
}

/* ---- file helpers ----------------------------------------------------- */
static int copy_bytes(const char *source, const char *destination) {
    int in = open(source, O_RDONLY | O_CLOEXEC);
    if (in < 0) return errno;
    int out = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (out < 0) { int e = errno; close(in); return e; }
    char buf[1 << 16]; ssize_t r; int e = 0;
    while ((r = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)r) != r) { e = errno ? errno : EIO; break; }
    if (r < 0) e = errno;
    close(in);
    if (close(out) && !e) e = errno;
    if (e) unlink(destination);
    return e;
}
int skyrim_clone_file(const char *source, const char *destination) {
#ifdef __APPLE__
    if (!clonefile(source, destination, CLONE_NOFOLLOW)) return 0;
#else
    if (!link(source, destination)) return 0;
#endif
    return copy_bytes(source, destination);
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
    snprintf(path, sizeof path, "%s%s%s", root, len ? "/" : "", relative);
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
        snprintf(full, sizeof full, "%s/%s", root, relative);
        if (lstat(full, &st)) { e = errno; }
        else if (S_ISDIR(st.st_mode)) {
            e = fn(ctx, relative, name, 1, &st);
            if (!e) e = walk(root, relative, child, cap, depth + 1, fn, ctx);
        } else if (S_ISREG(st.st_mode)) e = fn(ctx, relative, name, 0, &st);
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
            if (!s->dir && d->conflicts) fprintf(d->conflicts, "%s\t%d\t%d\n", s->actual, s->owner, d->owner);
            return 0;
        }
        snprintf(dst, sizeof dst, "%s/%s", d->data_dir, actual);
        if (mkdir(dst, 0755) && errno != EEXIST) return errno;
        s->actual = strdup(actual); s->owner = d->owner; s->dir = 1;
        return s->actual ? 0 : ENOMEM;
    }
    char src[8192];
    snprintf(src, sizeof src, "%s/%s", d->package, relative);
    if (s->actual) {
        if (d->conflicts) fprintf(d->conflicts, "%s\t%d\t%d\n", s->actual, s->owner, d->owner);
        if (s->dir || (s->owner == -1 && protected_original(s))) return 0;
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
    if (is_dir) return mkdir(dst, 0755) && errno != EEXIST ? errno : 0;
    return skyrim_clone_file(src, dst);
}
static int find_child(const char *dir, const char *name, char *out, size_t cap) {
    return skyrim_plugin_child_path(dir, name, out, cap);
}

int skyrim_deploy_matches(const char *view_dir, const char *key) {
    char path[4096], line[1024];
    snprintf(path, sizeof path, "%s/.skyrimios-deploy", view_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(line, 1, sizeof line - 1, f);
    fclose(f);
    line[n] = 0;
    size_t k = strlen(key);
    return n == k + 1 && !memcmp(line, key, k) && line[k] == '\n';
}

int skyrim_deploy_build(const char *game_dir, const char *const *package_roots, size_t count,
                        const char *view_dir, const char *conflicts_path, const char *key,
                        char *err, size_t err_cap) {
    char building[4096], old[4096], data[4096];
    if (strlen(view_dir) > 3000 || strlen(game_dir) > 3000) return ENAMETOOLONG;
    if (err && err_cap) err[0] = 0;
    snprintf(building, sizeof building, "%s.building", view_dir);
    snprintf(old, sizeof old, "%s.old", view_dir);
    int e = skyrim_remove_tree(building);
    if (!e) e = skyrim_remove_tree(old);
    if (e) { snprintf(err, err_cap, "cannot clear previous build (%d)", e); return e; }
#ifdef __APPLE__
    if (clonefile(game_dir, building, CLONE_NOFOLLOW)) { e = errno; snprintf(err, err_cap, "cannot clone the game folder (%d)", e); return e; }
#else
    if (mkdir(building, 0755)) { e = errno; snprintf(err, err_cap, "cannot create view (%d)", e); return e; }
    {
        const char *roots[2] = {game_dir, building};
        char rel[4096] = "";
        e = walk(game_dir, rel, 0, sizeof rel, 0, clone_tree_fn, (void *)roots);
        if (e) { snprintf(err, err_cap, "cannot copy the game folder (%d)", e); skyrim_remove_tree(building); return e; }
    }
#endif
    if (find_child(building, "Data", data, sizeof data) < 0) { skyrim_remove_tree(building); snprintf(err, err_cap, "ambiguous Data folder"); return EEXIST; }
    if (mkdir(data, 0755) && errno != EEXIST) { e = errno; skyrim_remove_tree(building); return e; }
    Deploy d = {0};
    d.data_dir = data; d.err = err; d.err_cap = err_cap;
    d.conflicts = conflicts_path ? fopen(conflicts_path, "wb") : NULL;
    char rel[4096] = "";
    e = walk(data, rel, 0, sizeof rel, 0, index_fn, &d);
    for (size_t i = 0; !e && i < count; ++i) {
        d.package = package_roots[i]; d.owner = (int)i;
        rel[0] = 0;
        e = walk(package_roots[i], rel, 0, sizeof rel, 0, overlay_fn, &d);
    }
    if (d.conflicts) fclose(d.conflicts);
    map_free(&d.map);
    if (!e) {
        char marker[8192];
        snprintf(marker, sizeof marker, "%s/.skyrimios-deploy", building);
        FILE *f = fopen(marker, "wb");
        if (!f || fprintf(f, "%s\n", key ? key : "") < 0) e = EIO;
        if (f && fclose(f) && !e) e = EIO;
    }
    if (!e) {
        struct stat st;
        if (!lstat(view_dir, &st) && rename(view_dir, old)) e = errno;
        else if (rename(building, view_dir)) e = errno;
        if (!e) skyrim_remove_tree(old);
    }
    if (e) {
        if (err && !err[0]) snprintf(err, err_cap, "deploy failed (%d)", e);
        skyrim_remove_tree(building);
    }
    return e;
}
