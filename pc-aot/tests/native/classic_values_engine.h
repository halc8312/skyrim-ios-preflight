/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_TEST_CLASSIC_VALUES_ENGINE_H
#define SKYRIM_TEST_CLASSIC_VALUES_ENGINE_H
#include "x86_classic_catalog.h"
#include <stdbool.h>

typedef bool (*ClassicValueFixtureInvoke)(void *context, const SkseClassicDeclaration *,
                                          const uint32_t *bits, const char *const *strings, uint32_t *out,
                                          char *result_text, size_t result_capacity);
typedef bool (*ClassicValueFixtureCommit)(void *context, const SkseClassicDeclaration *);

void classic_value_fixture_bind(const SkseClassicDeclaration *const *rows, unsigned count,
                                ClassicValueFixtureInvoke invoke, void (*discard)(void *), void *context);
void classic_value_fixture_bind_acknowledged(const SkseClassicDeclaration *const *rows, unsigned count,
                                             ClassicValueFixtureInvoke invoke, ClassicValueFixtureCommit commit,
                                             void (*discard)(void *), void *context);
bool classic_value_fixture_run(unsigned row, const uint32_t *bits, const char *const *strings, uint32_t *out,
                               char *result_text, size_t result_capacity);
void classic_value_fixture_retire(void);
#endif
