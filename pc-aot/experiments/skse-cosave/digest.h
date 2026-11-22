/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "generation.h"
struct stat;
typedef struct { uint64_t size; uint8_t hash[32]; } SkseEssDigest;
int skse_ess_same_version(const struct stat *a, const struct stat *b);
SksePairResult skse_ess_digest(int fd, SkseEssDigest *out);
int skse_pair_digest(const SksePairKey *key, const SkseEssDigest *ess,
                     const void *data, size_t size, uint8_t hash[32]);
int skse_pair_named_key(const uint8_t profile[16], const char *leaf, size_t length,
                        const struct stat *version, SksePairKey *out);
int skse_pair_copied_key(const uint8_t profile[16], const char *leaf, size_t length,
                         const struct stat *version, const uint8_t token[16], SksePairKey *out);
