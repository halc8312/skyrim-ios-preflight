/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_X86_CLASSIC_CATALOG_H
#define SKYRIM_EXPERIMENT_X86_CLASSIC_CATALOG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "papyrus_trial.h"

typedef enum {
    SKSE_CLASSIC_CATALOG_ALLOCATE, SKSE_CLASSIC_CATALOG_CONSTRUCT,
    SKSE_CLASSIC_CATALOG_FORM_TYPE, SKSE_CLASSIC_CATALOG_REGISTER,
    SKSE_CLASSIC_CATALOG_FLAGS, SKSE_CLASSIC_CATALOG_OPERATIONS
} SkseClassicCatalogOperation;

enum {
    SKSE_CLASSIC_CATALOG_NAME_BYTES = 64,
    SKSE_CLASSIC_CATALOG_MAX_ARGS = 8,
    SKSE_CLASSIC_CATALOG_MAX_ROWS = 512,
    SKSE_CLASSIC_CATALOG_FORM_TYPES = 256,
    SKSE_CLASSIC_CATALOG_FORM_BYTES = 16,
    SKSE_CLASSIC_CATALOG_FORM = 0x10000,
    SKSE_CLASSIC_CATALOG_FORM_ARRAY = 0x100
};

typedef struct {
    char class_name[SKSE_CLASSIC_CATALOG_NAME_BYTES], name[SKSE_CLASSIC_CATALOG_NAME_BYTES];
    uint32_t owner, object, vtable, entry, run, result, arguments[SKSE_CLASSIC_CATALOG_MAX_ARGS];
    uint32_t class_address, name_address, parameters, live_types[SKSE_CLASSIC_CATALOG_MAX_ARGS];
    uint8_t count, is_static;
    uint32_t flags;
} SkseClassicDeclaration;

/* Hosted registration only. Caller owns the row storage and exact zeroed
 * guest allocations for the registry vtable and synthetic form type refs.
 * Rows are usable only if the whole registration callback succeeded. No
 * pointer from the game or the host process is exported into the image. */
typedef struct {
    SkseClassicDeclaration *rows;
    size_t count, limit;
    uint32_t form_types;
    const SksePapyrusTrial *trial;
    uint32_t epoch;
} SkseClassicCatalog;

static inline bool skse_classic_type_is_form(uint32_t type) {
    return type == 1 || (type >= SKSE_CLASSIC_CATALOG_FORM &&
                         type - SKSE_CLASSIC_CATALOG_FORM < SKSE_CLASSIC_CATALOG_FORM_TYPES);
}

typedef enum {
    SKSE_CLASSIC_FRAME_ARG_OFFSET, SKSE_CLASSIC_FRAME_ARG_GET, SKSE_CLASSIC_FRAME_DESTROY,
    SKSE_CLASSIC_FRAME_STRING_SET, SKSE_CLASSIC_FRAME_STRING_CONSTRUCT,
    SKSE_CLASSIC_FRAME_INVALID_HANDLE, SKSE_CLASSIC_FRAME_HAS_TYPE,
    SKSE_CLASSIC_FRAME_VALID_HANDLE, SKSE_CLASSIC_FRAME_RESOLVE_HANDLE, SKSE_CLASSIC_FRAME_OPERATIONS
} SkseClassicFrameOperation;

enum {
    SKSE_CLASSIC_FRAME_STATE = 8, SKSE_CLASSIC_FRAME_LIST = 0x30, SKSE_CLASSIC_FRAME_ARGS = 0x40,
    SKSE_CLASSIC_FRAME_BYTES = SKSE_CLASSIC_FRAME_ARGS + 8 * SKSE_CLASSIC_CATALOG_MAX_ARGS
};

enum {
    SKSE_CLASSIC_FORM_POLICY_SLOT = 0x01ba7008, SKSE_CLASSIC_IDENTIFIER_BYTES = 0x1c,
    SKSE_CLASSIC_FORM_VIEW_BYTES = 0x14
};

/* One immutable selected form and an interpreter-private VMIdentifier.
 * Addresses and the full 64-bit handle are properties of this call only. */
typedef struct {
    uint32_t argument, policy, identifier, view, form_id;
    uint8_t form_type;
    uint64_t handle;
} SkseClassicFormArgument;

enum { SKSE_CLASSIC_FRAME_STRING_BYTES = 512, SKSE_CLASSIC_FRAME_STRINGS = 4 };
enum { SKSE_CLASSIC_PREPARE_MAX_WORK = 2000000 };

/* A caller-owned string copy for one native invocation. The interpreter sees
 * these bytes through an exact read-only view with a private guest address. */
typedef struct {
    uint32_t address;
    uint16_t lengths[SKSE_CLASSIC_FRAME_STRINGS];
    uint8_t count, selected; // selected is a one-based index. Zero means the result was never assigned.
    uint8_t bytes[SKSE_CLASSIC_FRAME_STRINGS * SKSE_CLASSIC_FRAME_STRING_BYTES];
} SkseClassicStringResult;

/* A private, initialized argument frame. No game-owned addresses escape into
 * the image, including when the caller supplies a bounded form projection. */
typedef struct {
    const SkseClassicDeclaration *function;
    const SkseClassicFormArgument *form;
    SkseClassicStringResult *string_result;
    uint32_t address, arguments[SKSE_CLASSIC_CATALOG_MAX_ARGS];
    /* A host-only preparation call before exposing the native to gameplay.
     * This mode uses a read-only virtual filesystem and no form projection. */
    bool preparing;
} SkseClassicFrame;

enum { SKSE_CLASSIC_LOAD_FORMS_MAX = 1024 };

typedef struct {
    uint32_t form_id, view; // A zero view explicitly represents a verified absent form.
    uint8_t type;
} SkseClassicLoadForm;

/* Sorted, immutable FormID results scoped to one serialization Load. The
 * caller owns both the records and the independent read-only copied values. */
typedef struct {
    uint32_t owner;
    const SkseClassicLoadForm *forms;
    size_t count;
} SkseClassicLoadForms;
#endif
