/* SPDX-License-Identifier: GPL-3.0-only */
#define _XOPEN_SOURCE 700
#include "generation.h"
#include "records.h"
#include "digest.h"
#include "immutable_ess.h"
#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct SksePairStore { sqlite3 *db; };
struct SksePairStage {
    SksePairKey target;
    SksePairImage image;
    SksePairResult failure;
    SkseEssDigest completed_ess;
    int pinned, editing;
};

_Static_assert(sizeof(SksePairKey) == 48, "the three opaque IDs must contain exactly 48 bytes");
static int valid_key(const SksePairKey *key) {
    static const uint8_t empty[16] = {0};
    return key && memcmp(key->profile, empty, 16) && memcmp(key->slot, empty, 16) && memcmp(key->event, empty, 16);
}
static SksePairResult db_result(int rc) {
    switch (rc & 255) {
        case SQLITE_OK: return SKSE_PAIR_OK;
        case SQLITE_BUSY: case SQLITE_LOCKED: return SKSE_PAIR_BUSY;
        case SQLITE_CORRUPT: case SQLITE_NOTADB: return SKSE_PAIR_DAMAGED;
        case SQLITE_TOOBIG: return SKSE_PAIR_LIMIT;
        default: return SKSE_PAIR_STORAGE;
    }
}
static int execute(sqlite3 *db, const char *sql) { return sqlite3_exec(db, sql, NULL, NULL, NULL); }

static int integer(sqlite3 *db, const char *sql, int *out) {
    sqlite3_stmt *statement = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(statement);
        if (rc == SQLITE_ROW) { *out = sqlite3_column_int(statement, 0); rc = SQLITE_OK; }
    }
    sqlite3_finalize(statement);
    return rc;
}
static SksePairResult initialize(sqlite3 *db) {
    enum { APPLICATION_ID = 0x53435031 };
    int rc = execute(db, "PRAGMA synchronous=FULL; PRAGMA fullfsync=ON; BEGIN IMMEDIATE");
    if (rc != SQLITE_OK) return db_result(rc);
    int id = 0, version = 0, objects = 0;
    rc = integer(db, "PRAGMA application_id", &id);
    if (rc == SQLITE_OK) rc = integer(db, "PRAGMA user_version", &version);
    SksePairResult result = db_result(rc);
    if (result == SKSE_PAIR_OK && !id && !version) {
        rc = integer(db, "SELECT count(*) FROM sqlite_master", &objects);
        if (rc != SQLITE_OK) result = db_result(rc);
        else if (objects) result = SKSE_PAIR_UNSUPPORTED;
        else result = db_result(execute(db,
            "CREATE TABLE generations("
            "identity BLOB PRIMARY KEY CHECK(length(identity)=48),ess_size INTEGER NOT NULL,"
            "ess_hash BLOB NOT NULL CHECK(length(ess_hash)=32),pair_hash BLOB NOT NULL CHECK(length(pair_hash)=32),"
            "companion BLOB NOT NULL) WITHOUT ROWID;"
            "PRAGMA application_id=0x53435031; PRAGMA user_version=1"));
    } else if (result == SKSE_PAIR_OK && (id != APPLICATION_ID || version != 1)) {
        result = SKSE_PAIR_UNSUPPORTED;
    }
    if (result == SKSE_PAIR_OK) result = db_result(execute(db, "COMMIT"));
    if (!sqlite3_get_autocommit(db)) execute(db, "ROLLBACK");
    return result;
}

SksePairResult skse_pair_open(const char *directory, SksePairStore **out) {
    if (!out) return SKSE_PAIR_ARGUMENT;
    *out = NULL;
    struct stat info, resolved;
    if (!directory || !*directory || lstat(directory, &info) || !S_ISDIR(info.st_mode)) return SKSE_PAIR_ARGUMENT;
    /* `/var` can legitimately alias an Apple container's ancestors. Give
     * SQLite the same checked directory under its canonical spelling so
     * SQLITE_OPEN_NOFOLLOW still rejects any alias of the database itself. */
    char canonical[PATH_MAX];
    if (!realpath(directory, canonical) || lstat(canonical, &resolved) ||
        !S_ISDIR(resolved.st_mode) || info.st_dev != resolved.st_dev || info.st_ino != resolved.st_ino)
        return SKSE_PAIR_STORAGE;
    SksePairStore *store = calloc(1, sizeof *store);
    char *path = sqlite3_mprintf("%s/classic-generations.sqlite3", canonical);
    if (!path || !store) { free(store); sqlite3_free(path); return SKSE_PAIR_STORAGE; }
    /* A private store may not be an alias of another file. The directory is
     * owned by the caller and must be protected from concurrent path changes. */
    if (!lstat(path, &info)) {
        if (!S_ISREG(info.st_mode) || info.st_nlink != 1) {
            free(store); sqlite3_free(path); return SKSE_PAIR_STORAGE;
        }
    } else if (errno != ENOENT) {
        free(store); sqlite3_free(path); return SKSE_PAIR_STORAGE;
    }
    int rc = sqlite3_open_v2(path, &store->db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOFOLLOW, NULL);
    sqlite3_free(path);
    SksePairResult result = db_result(rc);
    if (result == SKSE_PAIR_OK) {
        sqlite3_limit(store->db, SQLITE_LIMIT_LENGTH, SKSE_PAIR_MAX_COMPANION + 4096);
        rc = sqlite3_busy_timeout(store->db, 2000);
        result = rc == SQLITE_OK ? initialize(store->db) : db_result(rc);
    }
    if (result == SKSE_PAIR_OK) *out = store;
    else skse_pair_close(store);
    return result;
}
void skse_pair_close(SksePairStore *store) {
    if (store) { sqlite3_close(store->db); free(store); }
}

static int select_generation(sqlite3 *db, const SksePairKey *key, sqlite3_stmt **out) {
    int rc = sqlite3_prepare_v2(db,
        "SELECT ess_size, ess_hash, pair_hash, companion FROM generations WHERE identity=?1", -1, out, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(*out, 1, key, sizeof *key, SQLITE_STATIC);
    return rc == SQLITE_OK ? sqlite3_step(*out) : rc;
}
static const void *blob(sqlite3_stmt *row, int col, int length) {
    if (sqlite3_column_type(row, col) != SQLITE_BLOB || sqlite3_column_bytes(row, col) != length) return NULL;
    return sqlite3_column_blob(row, col);
}
static SksePairResult expected_ess(sqlite3 *db, const SksePairKey *key, SkseEssDigest *ess) {
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT ess_size,ess_hash FROM generations WHERE identity=?1", -1, &s, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(s, 1, key, sizeof *key, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(s);
    SksePairResult result;
    if (rc == SQLITE_ROW) {
        const void *hash = blob(s, 1, 32);
        sqlite3_int64 size = sqlite3_column_int64(s, 0);
        if (!hash || sqlite3_column_type(s, 0) != SQLITE_INTEGER || size < 1 || size > SKSE_PAIR_MAX_ESS)
            result = SKSE_PAIR_DAMAGED;
        else {
            ess->size = (uint64_t)size;
            memcpy(ess->hash, hash, 32);
            result = SKSE_PAIR_OK;
        }
    } else result = rc == SQLITE_DONE ? SKSE_PAIR_MISSING : db_result(rc);
    sqlite3_finalize(s);
    return result;
}
static SksePairResult read_image(sqlite3 *db, const SksePairGeneration *g, SksePairImage *out) {
    if (!g || !valid_key(&g->key) || g->ess_fd < 0) return SKSE_PAIR_ARGUMENT;
    /* A generation with no companion costs one small lookup, with no ESS read.
     * Release the metadata statement before streaming the .ess. */
    SkseEssDigest expected, ess;
    SksePairResult result = expected_ess(db, &g->key, &expected);
    if (result != SKSE_PAIR_OK) return result;
    result = skse_ess_digest(g->ess_fd, &ess);
    if (result != SKSE_PAIR_OK) return result;
    if (ess.size != expected.size || memcmp(ess.hash, expected.hash, 32)) return SKSE_PAIR_WRONG_GENERATION;
    sqlite3_stmt *row = NULL;
    int rc = select_generation(db, &g->key, &row);
    if (rc != SQLITE_ROW) { result = rc == SQLITE_DONE ? SKSE_PAIR_MISSING : db_result(rc); goto done; }
    const void *ess_hash = blob(row, 1, 32), *image_hash = blob(row, 2, 32);
    if (sqlite3_column_type(row, 0) != SQLITE_INTEGER || !ess_hash || !image_hash) { result = SKSE_PAIR_DAMAGED; goto done; }
    if (sqlite3_column_int64(row, 0) != (sqlite3_int64)ess.size || memcmp(ess_hash, ess.hash, 32)) {
        result = SKSE_PAIR_WRONG_GENERATION; goto done;
    }
    int bytes = sqlite3_column_bytes(row, 3);
    if (bytes < 20 || bytes > SKSE_PAIR_MAX_COMPANION) { result = SKSE_PAIR_DAMAGED; goto done; }
    const void *image = blob(row, 3, bytes);
    uint8_t digest[32];
    if (!image || !skse_pair_digest(&g->key, &ess, image, (size_t)bytes, digest)) { result = SKSE_PAIR_STORAGE; goto done; }
    if (memcmp(digest, image_hash, 32)) { result = SKSE_PAIR_DAMAGED; goto done; }
    result = skse_records_validate(image, (size_t)bytes);
    if (result == SKSE_PAIR_OK) {
        out->data = malloc((size_t)bytes);
        if (!out->data) { result = SKSE_PAIR_STORAGE; goto done; }
        memcpy(out->data, image, (size_t)bytes);
        out->size = (size_t)bytes;
    }
done:
    sqlite3_finalize(row);
    return result;
}

static SksePairResult begin_stage(SksePairStore *store, const SksePairKey *target,
                                  const SksePairGeneration *base, SksePairStage **out) {
    SksePairStage *stage = calloc(1, sizeof *stage);
    if (!stage) return SKSE_PAIR_STORAGE;
    stage->target = *target;
    SksePairResult result = base ? read_image(store->db, base, &stage->image) : skse_records_empty(&stage->image);
    if (result == SKSE_PAIR_OK) *out = stage;
    else skse_pair_discard(stage);
    return result;
}

SksePairResult skse_pair_begin(SksePairStore *store, const SksePairKey *target,
                               const SksePairGeneration *base, SksePairStage **out) {
    if (!out) return SKSE_PAIR_ARGUMENT;
    *out = NULL;
    if (!store || !valid_key(target)) return SKSE_PAIR_ARGUMENT;
    if (base && memcmp(base->key.profile, target->profile, sizeof target->profile)) return SKSE_PAIR_WRONG_GENERATION;
    return begin_stage(store, target, base, out);
}

SksePairResult skse_pair_begin_empty(const SksePairKey *target, SksePairStage **out) {
    if (!out) return SKSE_PAIR_ARGUMENT;
    *out = NULL;
    if (!valid_key(target)) return SKSE_PAIR_ARGUMENT;
    return begin_stage(NULL, target, NULL, out);
}
SksePairResult skse_pair_import(const SksePairKey *target, const void *data, size_t size, SksePairStage **out) {
    if (!out) return SKSE_PAIR_ARGUMENT;
    *out = NULL;
    if (!valid_key(target)) return SKSE_PAIR_ARGUMENT;
    SksePairResult result = skse_records_validate(data, size);
    if (result != SKSE_PAIR_OK) return result;
    SksePairStage *stage = calloc(1, sizeof *stage);
    if (!stage) return SKSE_PAIR_STORAGE;
    stage->image.data = malloc(size);
    if (!stage->image.data) { skse_pair_discard(stage); return SKSE_PAIR_STORAGE; }
    memcpy(stage->image.data, data, size);
    stage->image.size = size;
    stage->target = *target;
    *out = stage;
    return SKSE_PAIR_OK;
}
SksePairResult skse_pair_replace(SksePairStage *stage, uint32_t uid, const SksePairRecord *records, size_t count) {
    if (!stage) return SKSE_PAIR_ARGUMENT;
    if (stage->editing) return SKSE_PAIR_BUSY;
    if (stage->pinned) return SKSE_PAIR_SEALED;
    if (stage->failure == SKSE_PAIR_OK) stage->failure = skse_records_replace(&stage->image, uid, records, count);
    return stage->failure;
}
SksePairResult skse_pair_edit_image(SksePairStage *stage, SkseStageEditor editor, void *context) {
    if (!stage || !editor) return SKSE_PAIR_ARGUMENT;
    if (stage->editing) return SKSE_PAIR_BUSY;
    if (stage->pinned) return SKSE_PAIR_SEALED;
    if (stage->failure == SKSE_PAIR_OK) {
        stage->editing = 1;
        stage->failure = editor(&stage->image, context);
        stage->editing = 0;
    }
    return stage->failure;
}
void skse_pair_discard(SksePairStage *stage) {
    if (stage) { free(stage->image.data); free(stage); }
}

/* Each sqlite3_bind_blob has a stable source until the statement is finalized. */
static int insert_image(sqlite3 *db, const SksePairStage *stage, const SkseEssDigest *ess, const uint8_t hash[32]) {
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO generations VALUES(?1,?2,?3,?4,?5) ON CONFLICT(identity) DO NOTHING", -1, &s, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(s, 1, &stage->target, sizeof stage->target, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 2, (sqlite3_int64)ess->size);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(s, 3, ess->hash, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(s, 4, hash, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(s, 5, stage->image.data, (int)stage->image.size, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}
static SksePairResult compare_existing(sqlite3 *db, const SksePairStage *stage,
                                       const SkseEssDigest *ess, const uint8_t hash[32]) {
    sqlite3_stmt *s = NULL;
    int rc = select_generation(db, &stage->target, &s);
    SksePairResult result = SKSE_PAIR_CONFLICT;
    if (rc == SQLITE_ROW) {
        const void *old_ess = blob(s, 1, 32), *old_hash = blob(s, 2, 32);
        const void *data = blob(s, 3, (int)stage->image.size);
        if (!old_ess || !old_hash || sqlite3_column_type(s, 0) != SQLITE_INTEGER) result = SKSE_PAIR_DAMAGED;
        else if (data && sqlite3_column_int64(s, 0) == (sqlite3_int64)ess->size &&
                 !memcmp(ess->hash, old_ess, 32) && !memcmp(hash, old_hash, 32) &&
                 !memcmp(data, stage->image.data, stage->image.size)) result = SKSE_PAIR_ALREADY;
    } else result = rc == SQLITE_DONE ? SKSE_PAIR_DAMAGED : db_result(rc);
    sqlite3_finalize(s);
    return result;
}

#ifdef SKSE_PAIR_TEST_INTERRUPTION
extern void skse_pair_test_interruption(sqlite3 *db, int committed);
#endif
static SksePairResult prove_completed(SksePairStage *stage, const SksePairGeneration *completed, SkseEssDigest *ess) {
    if (!stage || !completed || !valid_key(&completed->key)) return SKSE_PAIR_ARGUMENT;
    if (stage->editing) return SKSE_PAIR_BUSY;
    if (stage->failure != SKSE_PAIR_OK) return stage->failure;
    if (memcmp(&stage->target, &completed->key, sizeof stage->target)) return SKSE_PAIR_WRONG_GENERATION;
    SksePairResult result = skse_ess_digest(completed->ess_fd, ess);
    if (result == SKSE_PAIR_OK && stage->pinned &&
        (ess->size != stage->completed_ess.size || memcmp(ess->hash, stage->completed_ess.hash, 32)))
        return SKSE_PAIR_WRONG_GENERATION;
    return result;
}
SksePairResult skse_pair_pin(SksePairStage *stage, const SksePairGeneration *completed) {
    SkseEssDigest ess;
    SksePairResult result = prove_completed(stage, completed, &ess);
    if (result != SKSE_PAIR_OK) return result;
    if (stage->pinned) return SKSE_PAIR_ALREADY;
    stage->completed_ess = ess;
    stage->pinned = 1;
    return SKSE_PAIR_OK;
}
static SksePairResult commit_image(SksePairStore *store, SksePairStage *stage, const SkseEssDigest *ess) {
    uint8_t hash[32];
    if (!skse_pair_digest(&stage->target, ess, stage->image.data, stage->image.size, hash)) return SKSE_PAIR_STORAGE;
    sqlite3 *db = store->db;
    int rc = execute(db, "BEGIN IMMEDIATE");
    if (rc != SQLITE_OK) return db_result(rc);
    rc = insert_image(db, stage, ess, hash);
    SksePairResult result = db_result(rc);
    if (result == SKSE_PAIR_OK && !sqlite3_changes(db)) result = compare_existing(db, stage, ess, hash);
    if (result == SKSE_PAIR_OK || result == SKSE_PAIR_ALREADY) {
#ifdef SKSE_PAIR_TEST_INTERRUPTION
        skse_pair_test_interruption(db, 0);
#endif
        rc = execute(db, "COMMIT");
        if (rc != SQLITE_OK) result = db_result(rc);
#ifdef SKSE_PAIR_TEST_INTERRUPTION
        else {
            skse_pair_test_interruption(db, 1);
        }
#endif
    }
    if (!sqlite3_get_autocommit(db)) execute(db, "ROLLBACK");
    return result;
}

SksePairResult skse_pair_commit(SksePairStore *store, SksePairStage *stage, const SksePairGeneration *completed) {
    if (!store || !stage || !completed) return SKSE_PAIR_ARGUMENT;
    if (stage->editing) return SKSE_PAIR_BUSY;
    if (stage->failure != SKSE_PAIR_OK) return stage->failure;
    if (!stage->pinned) return SKSE_PAIR_UNPINNED;
    SkseEssDigest ess;
    SksePairResult result = prove_completed(stage, completed, &ess);
    return result == SKSE_PAIR_OK ? commit_image(store, stage, &ess) : result;
}

SksePairResult skse_pair_commit_immutable(SksePairStore *store, SksePairStage *stage,
                                         const SksePairKey *expected, SkseImmutableEss *snapshot) {
    if (!store || !stage || !expected || !snapshot) return SKSE_PAIR_ARGUMENT;
    if (stage->editing) return SKSE_PAIR_BUSY;
    if (stage->failure != SKSE_PAIR_OK) return stage->failure;
    if (memcmp(&stage->target, expected, sizeof *expected)) return SKSE_PAIR_WRONG_GENERATION;
    SkseEssDigest ess;
    SksePairResult result = skse_ess_read_immutable(snapshot, &ess);
    if (result != SKSE_PAIR_OK) return result;
    if (stage->pinned && (ess.size != stage->completed_ess.size ||
                          memcmp(ess.hash, stage->completed_ess.hash, sizeof ess.hash)))
        return SKSE_PAIR_WRONG_GENERATION;
    stage->completed_ess = ess;
    stage->pinned = 1;
    return commit_image(store, stage, &ess);
}

SksePairResult skse_pair_export(SksePairStore *store, const SksePairGeneration *generation, void **data, size_t *size) {
    if (!data || !size) return SKSE_PAIR_ARGUMENT;
    *data = NULL; *size = 0;
    if (!store) return SKSE_PAIR_ARGUMENT;
    SksePairImage image = {0};
    SksePairResult result = read_image(store->db, generation, &image);
    if (result == SKSE_PAIR_OK) { *data = image.data; *size = image.size; }
    return result;
}
SksePairResult skse_pair_restore(SksePairStore *store, const SksePairGeneration *generation, uint32_t uid,
                                 SksePairRevert revert, SksePairVisitor visitor, void *context) {
    if (!store || !generation || !revert || !visitor) return SKSE_PAIR_ARGUMENT;
    revert(context);
    SksePairImage image = {0};
    SksePairResult result = read_image(store->db, generation, &image);
    if (result == SKSE_PAIR_OK) result = skse_records_visit(&image, uid, visitor, context);
    free(image.data);
    if (result == SKSE_PAIR_STOPPED) revert(context);
    return result;
}
const char *skse_pair_result(SksePairResult result) {
    static const char *const names[] = {"ok", "already committed", "invalid argument", "generation missing",
        "wrong generation", "ESS changed", "generation conflict", "unsupported", "invalid Classic data",
        "duplicate UID", "limit", "owner missing", "receiver stopped", "storage failure",
        "store busy", "stored data damaged", "stage is frozen", "ESS generation not pinned"};
    return (unsigned)result < sizeof names / sizeof *names ? names[result] : "unknown";
}
