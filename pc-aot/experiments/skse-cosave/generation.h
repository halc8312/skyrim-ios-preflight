/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SKSE_PAIR_OK, SKSE_PAIR_ALREADY, SKSE_PAIR_ARGUMENT, SKSE_PAIR_MISSING,
    SKSE_PAIR_WRONG_GENERATION, SKSE_PAIR_ESS_CHANGED, SKSE_PAIR_CONFLICT,
    SKSE_PAIR_UNSUPPORTED, SKSE_PAIR_INVALID_DATA, SKSE_PAIR_DUPLICATE_UID,
    SKSE_PAIR_LIMIT, SKSE_PAIR_OWNER_MISSING, SKSE_PAIR_STOPPED,
    SKSE_PAIR_STORAGE, SKSE_PAIR_BUSY, SKSE_PAIR_DAMAGED, SKSE_PAIR_SEALED,
    SKSE_PAIR_UNPINNED
} SksePairResult;

typedef struct {
    uint8_t profile[16], slot[16], event[16];
} SksePairKey;
/* ess_fd must be a stable, read-only regular file used by this completed
 * game event. It is borrowed. Its seek position is never changed. */
typedef struct { SksePairKey key; int ess_fd; } SksePairGeneration;
typedef struct { uint32_t type, version, length; const void *data; } SksePairRecord;
typedef int (*SksePairVisitor)(void *, const SksePairRecord *);
typedef void (*SksePairRevert)(void *);
typedef struct SksePairStore SksePairStore;
typedef struct SksePairStage SksePairStage;

enum {
    SKSE_PAIR_MAX_ESS = 512 * 1024 * 1024, SKSE_PAIR_MAX_COMPANION = 64 * 1024 * 1024,
    SKSE_PAIR_MAX_HEADERS = 100000
};

/* Open a dedicated database in a caller-owned existing private directory.
 * Each store and stage is confined to its calling thread; independent stores
 * on the same directory may be used concurrently. */
SksePairResult skse_pair_open(const char *directory, SksePairStore **out);
/* Open a dedicated read-only connection under the checked directory. The
 * borrowed descriptor must identify directory. An absent database is an
 * empty store. As with the writer, the caller protects this private path. */
SksePairResult skse_pair_open_readonly(const char *directory, int directory_fd, SksePairStore **out);
void skse_pair_close(SksePairStore *store);

/* An explicit NULL base starts empty. A supplied base must be verifiable in
 * this profile; a missing/mismatched base fails rather than starting empty. */
SksePairResult skse_pair_begin(SksePairStore *store, const SksePairKey *target,
                               const SksePairGeneration *base, SksePairStage **out);
/* A fresh in-memory stage needs no storage connection until commit. */
SksePairResult skse_pair_begin_empty(const SksePairKey *target, SksePairStage **out);
/* The Classic wire format has no binding to .ess. Callers of import must
 * independently establish that these original bytes belong to this event. */
SksePairResult skse_pair_import(const SksePairKey *target, const void *data, size_t size,
                                SksePairStage **out);
/* A failed edit poisons the entire stage. Drop it and restart. UID 0 is
 * reserved for Classic itself; a zero count removes only the selected UID. */
SksePairResult skse_pair_replace(SksePairStage *stage, uint32_t uid,
                                 const SksePairRecord *records, size_t count);
/* Pin the finalized, read-only .ess at the authoritative completion boundary.
 * This freezes the records and the observed digest. Publication checks both
 * the caller's event identity and the actual bytes again. */
SksePairResult skse_pair_pin(SksePairStage *stage, const SksePairGeneration *completed);
SksePairResult skse_pair_commit(SksePairStore *store, SksePairStage *stage,
                                const SksePairGeneration *completed);
void skse_pair_discard(SksePairStage *stage);

/* Reset the receiver on entry to a valid request, including missing or
 * mismatched generations. A stopped visitor triggers another reset to drop
 * partial state. Both callbacks must operate on caller-owned reversible state.
 * Record memory is borrowed only for the duration of the visitor. */
SksePairResult skse_pair_restore(SksePairStore *store, const SksePairGeneration *generation,
                                 uint32_t uid, SksePairRevert revert, SksePairVisitor visitor,
                                 void *context);
/* Export an independently owned exact Classic .skse image for a verified
 * generation. Frees no caller buffer; use free() after success. */
SksePairResult skse_pair_export(SksePairStore *store, const SksePairGeneration *generation,
                                void **data, size_t *size);
/* Copy verified records into a different authorized profile. Both generation
 * keys must describe the exact finalized files. The destination must match the
 * authenticated source .ess byte for byte; an existing different image is a
 * conflict. The caller handles the surrounding .ess copy transaction. */
SksePairResult skse_pair_rebind_copy(SksePairStore *source, const SksePairGeneration *original,
                                    SksePairStore *destination, const SksePairGeneration *copy);
/* Check for a row without reading the .ess or returning companion data. */
SksePairResult skse_pair_contains(SksePairStore *store, const SksePairKey *key);
const char *skse_pair_result(SksePairResult result);

#ifdef __cplusplus
}
#endif
