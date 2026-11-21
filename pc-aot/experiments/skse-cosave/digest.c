/* SPDX-License-Identifier: GPL-3.0-only */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "digest.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
typedef CC_SHA256_CTX Digest;
static int start(Digest *ctx) { return CC_SHA256_Init(ctx); }
static int update(Digest *ctx, const void *data, size_t size) { return CC_SHA256_Update(ctx, data, (CC_LONG)size); }
static int finish(Digest *ctx, uint8_t hash[32]) { return CC_SHA256_Final(hash, ctx); }
static void release(Digest *ctx) { (void)ctx; }
#else
#include <openssl/evp.h>
typedef EVP_MD_CTX *Digest;
static int start(Digest *ctx) {
    *ctx = EVP_MD_CTX_new();
    return *ctx && EVP_DigestInit_ex(*ctx, EVP_sha256(), NULL) == 1;
}
static int update(Digest *ctx, const void *data, size_t size) { return EVP_DigestUpdate(*ctx, data, size) == 1; }
static int finish(Digest *ctx, uint8_t hash[32]) {
    unsigned bytes;
    return EVP_DigestFinal_ex(*ctx, hash, &bytes) == 1 && bytes == 32;
}
static void release(Digest *ctx) { EVP_MD_CTX_free(*ctx); }
#endif

int skse_pair_digest(const SksePairKey *key, const SkseEssDigest *ess,
                     const void *data, size_t size, uint8_t hash[32]) {
    static const char domain[] = "Classic co-save generation v1";
    uint8_t length[8];
    for (unsigned i = 0; i < sizeof length; ++i) length[i] = (uint8_t)(ess->size >> (8 * i));
    Digest ctx;
    int ok = start(&ctx) && update(&ctx, domain, sizeof domain) && update(&ctx, key, sizeof *key) &&
             update(&ctx, length, sizeof length) && update(&ctx, ess->hash, sizeof ess->hash) &&
             update(&ctx, data, size) && finish(&ctx, hash);
    release(&ctx);
    return ok;
}

int skse_pair_named_key(const uint8_t profile[16], const char *leaf, size_t length,
                        const struct stat *version, SksePairKey *out) {
    static const char slot_domain[] = "Classic named save slot v1";
    static const char event_domain[] = "Classic named save event v1";
#if defined(__APPLE__)
    const struct timespec changed = version->st_ctimespec, modified = version->st_mtimespec;
#else
    const struct timespec changed = version->st_ctim, modified = version->st_mtim;
#endif
    const uint64_t fields[] = {
        (uint64_t)version->st_dev, (uint64_t)version->st_ino, (uint64_t)version->st_size,
        (uint64_t)changed.tv_sec, (uint64_t)changed.tv_nsec, (uint64_t)modified.tv_sec, (uint64_t)modified.tv_nsec,
    };
    uint8_t wire[sizeof fields], hash[32];
    for (size_t n = 0; n < sizeof fields / sizeof *fields; ++n)
        for (unsigned byte = 0; byte < 8; ++byte)
            wire[n * 8 + byte] = (uint8_t)(fields[n] >> (byte * 8));

    SksePairKey key = {0};
    memcpy(key.profile, profile, sizeof key.profile);
    Digest ctx;
    int ok = start(&ctx) && update(&ctx, slot_domain, sizeof slot_domain) &&
             update(&ctx, key.profile, sizeof key.profile) && update(&ctx, leaf, length) && finish(&ctx, hash);
    release(&ctx);
    if (!ok) return 0;
    memcpy(key.slot, hash, sizeof key.slot);
    ok = start(&ctx) && update(&ctx, event_domain, sizeof event_domain) &&
         update(&ctx, key.slot, sizeof key.slot) && update(&ctx, wire, sizeof wire) && finish(&ctx, hash);
    release(&ctx);
    if (!ok) return 0;
    memcpy(key.event, hash, sizeof key.event);
    *out = key;
    return 1;
}

int skse_ess_same_version(const struct stat *a, const struct stat *b) {
#if defined(__APPLE__)
    struct timespec am = a->st_mtimespec, bm = b->st_mtimespec, ac = a->st_ctimespec, bc = b->st_ctimespec;
#else
    struct timespec am = a->st_mtim, bm = b->st_mtim, ac = a->st_ctim, bc = b->st_ctim;
#endif
    return a->st_size == b->st_size && a->st_ino == b->st_ino && a->st_dev == b->st_dev &&
           am.tv_sec == bm.tv_sec && am.tv_nsec == bm.tv_nsec && ac.tv_sec == bc.tv_sec && ac.tv_nsec == bc.tv_nsec;
}

#ifdef SKSE_PAIR_TEST_INTERRUPTION
extern void skse_pair_test_ess_progress(void);
#endif
SksePairResult skse_ess_digest(int fd, SkseEssDigest *out) {
    int flags = fcntl(fd, F_GETFL);
    struct stat before, after;
    if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY || fstat(fd, &before) ||
        !S_ISREG(before.st_mode) || before.st_size <= 0) return SKSE_PAIR_ARGUMENT;
    if ((uint64_t)before.st_size > SKSE_PAIR_MAX_ESS) return SKSE_PAIR_LIMIT;
    Digest ctx;
    if (!start(&ctx)) { release(&ctx); return SKSE_PAIR_STORAGE; }
    uint8_t buffer[32 * 1024];
    off_t offset = 0;
    SksePairResult result = SKSE_PAIR_OK;
    while (offset < before.st_size) {
        size_t size = sizeof buffer;
        if (before.st_size - offset < (off_t)size) size = (size_t)(before.st_size - offset);
        ssize_t n = pread(fd, buffer, size, offset);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) { result = SKSE_PAIR_ESS_CHANGED; break; }
        if (n < 0 || !update(&ctx, buffer, (size_t)n)) { result = SKSE_PAIR_STORAGE; break; }
#ifdef SKSE_PAIR_TEST_INTERRUPTION
        if (!offset) skse_pair_test_ess_progress();
#endif
        offset += n;
    }
    if (result == SKSE_PAIR_OK) {
        if (fstat(fd, &after)) result = SKSE_PAIR_STORAGE;
        else if (!skse_ess_same_version(&before, &after)) result = SKSE_PAIR_ESS_CHANGED;
        else if (!finish(&ctx, out->hash)) result = SKSE_PAIR_STORAGE;
        else out->size = (uint64_t)before.st_size;
    }
    release(&ctx);
    return result;
}
