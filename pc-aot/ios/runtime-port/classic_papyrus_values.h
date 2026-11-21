/* SPDX-License-Identifier: GPL-3.0-only
 * Types of the staged Classic value publication (classic_papyrus_values.c.in,
 * compiled inside the staged skse_compat.c). A provider in another file uses
 * them with skse_classic_values_request and skse_classic_values_retire.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Include after the runtime's x86.h, which declares CPU. */

enum { CLASSIC_VALUES_FUNCTIONS = 16, CLASSIC_VALUES_ARGS = 4, CLASSIC_VALUES_STRING_BYTES = 512 };
enum { CLASSIC_VALUES_FORM = 1, CLASSIC_VALUES_STRING = 2, CLASSIC_VALUES_INT = 3,
       CLASSIC_VALUES_FLOAT = 4, CLASSIC_VALUES_BOOL = 5 };
/* Non-latent NativeFunctionBase::Invoke in the pinned 1.9.32 image. */
enum { CLASSIC_VALUES_RUN_RETURN = 0xc46e61, CLASSIC_VALUES_RESULT_OFFSET = 0x2c,
       CLASSIC_VALUES_BASE_OFFSET = 0x38 };
enum { CLASSIC_VALUES_STRING_DATA_OFFSET = 0x0c };

typedef struct {
    const char *class_name, *name;
    uint32_t id;
    uint8_t result, count, arguments[CLASSIC_VALUES_ARGS];
} ClassicValueFunction;

typedef struct {
    uint32_t type, bits;
    const char *string;
} ClassicValue;

typedef struct {
    uint32_t object, sp, base, registry, stack_id, result, state, argument_list;
} ClassicValueFrame;

static inline bool classic_value_same_frame(const ClassicValueFrame *a, const ClassicValueFrame *b) {
    return a->object == b->object && a->sp == b->sp && a->base == b->base && a->registry == b->registry &&
           a->stack_id == b->stack_id && a->result == b->result && a->state == b->state &&
           a->argument_list == b->argument_list;
}

typedef struct {
    uint64_t epoch;
    const ClassicValueFunction *functions;
    size_t count;
    bool (*read_guest)(void *, CPU *, uint32_t address, void *, size_t);
    bool (*copy_string)(void *, CPU *, uint32_t address, char *, size_t);
    /* An allowlisted Form provider copies a nonzero identity in the current
     * invocation. The returned scalar never carries a live guest pointer. */
    bool (*copy_form_id)(void *, CPU *, const ClassicValueFrame *, uint32_t value_address, uint32_t *identity);
    /* Attest the exact frame and its VM-owned storage. The provider keeps
     * this scope alive and exclusively owned until Run returns. */
    bool (*owns_frame)(void *, CPU *, uint64_t epoch, const ClassicValueFrame *);
    bool (*invoke)(void *, uint64_t epoch, uint32_t function, const ClassicValue *, size_t, ClassicValue *,
                   char *result_text, size_t result_capacity);
    /* A false acknowledgement leaves host effects unpublished. A commit
     * must return with the attested frame still owned and cannot reenter Run. */
    bool (*commit)(void *, uint64_t epoch, uint32_t function);
    void (*discard)(void *, uint64_t epoch); /* Invoked after an attempted call whose result cannot be published. */
    /* Optional. Invoked once after every Run that matched a published row,
     * after the result was written or discarded, before Run returns. A
     * provider that holds a lock from commit releases it here. */
    void (*finished)(void *, uint64_t epoch);
    void *context;
} ClassicValueRequest;

/* Configure before the pinned registration hook; retire after the guest returns.
 * Calls are serialized: a Run that finds another call in progress returns 0. */
bool skse_classic_values_request(const ClassicValueRequest *request);
bool skse_classic_values_retire(uint64_t epoch);
/* During copy_form_id, check the typed argument against the original registry's
 * entry for a static FormID already authenticated by the provider. The result
 * only compares the engine's two references while the same frame is current. */
bool skse_classic_values_matches_static_form(CPU *, const ClassicValueFrame *, uint32_t value_address,
                                             uint32_t expected);
/* Nonblocking serialized access for the registered provider's save/load
 * records. The operation must return without entering a Papyrus native. */
bool skse_classic_values_with_provider(uint64_t epoch, uint32_t registry,
                                       bool (*operation)(void *), void *context);
