/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "core_snapshot.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SKSE_OBJECT_OK, SKSE_OBJECT_ARGUMENT, SKSE_OBJECT_MISSING, SKSE_OBJECT_UNSUPPORTED_VERSION,
    SKSE_OBJECT_UNKNOWN_CLASS, SKSE_OBJECT_UNKNOWN_ARGUMENT, SKSE_OBJECT_INVALID,
    SKSE_OBJECT_DUPLICATE, SKSE_OBJECT_LIMIT, SKSE_OBJECT_OUT_OF_ORDER
} SkseObjectResult;
enum { SKSE_OBJECT_MAX_SLOTS = 65536, SKSE_OBJECT_MAX_ARGUMENTS = 100000 };
typedef enum {
    SKSE_ARG_NONE, SKSE_ARG_INT, SKSE_ARG_FLOAT, SKSE_ARG_BOOL, SKSE_ARG_PROCESS_LOCAL_STRING, SKSE_ARG_FORM
} SkseStoredArgumentKind;
typedef struct {
    uint32_t slots, filled, arguments, process_local_strings, unresolved_forms;
} SkseObjectSummary;
typedef struct { uint32_t index, owning_stack, argument_count; SkseCoreName event_name; } SkseStoredModEvent;
typedef struct {
    SkseStoredArgumentKind kind;
    /* Raw scalar bits or saved form ID. Process-local string args report zero.
     * mapped_form applies only to RESOLVED/DYNAMIC form arguments. */
    uint32_t bits, mapped_form;
    SkseModResolution form_resolution;
} SkseStoredArgument;
typedef void (*SkseObjectObserver)(void *context, const SkseStoredModEvent *event);
typedef void (*SkseArgumentObserver)(void *context, const SkseStoredModEvent *event,
                                     uint32_t argument_index, const SkseStoredArgument *argument);
/* Only the 1.7.3 OBMG representation of PapyrusModEvent v1 is decoded.
 * Unknown classes and arguments produce a specific failure with no callbacks.
 * The opaque data of any class remains intact in the generation store. */
SkseObjectResult skse_mod_event_objects(const SkseCoreSnapshot *snapshot, SkseObjectSummary *summary,
    SkseObjectObserver object_observer, SkseArgumentObserver argument_observer, void *context);

/* UICallback.Create stores a pending UIInvokeDelegate until Send or Release.
 * Its Win32 v1 arguments use Scaleform's actual scalar type numbers. */
typedef enum { SKSE_UI_BOOL = 2, SKSE_UI_NUMBER = 3, SKSE_UI_PROCESS_LOCAL_STRING = 4 } SkseUIArgumentKind;
typedef struct {
    uint32_t slots, filled, callbacks, mod_events, ui_arguments, process_local_strings;
} SkseUIObjectSummary;
typedef struct {
    uint32_t index, owning_stack, argument_count;
    SkseCoreName menu, target;
} SkseStoredUICallback;
typedef struct {
    SkseUIArgumentKind kind;
    /* Boolean or the exact IEEE-754 double bits. Process-local strings
     * report zero. Reading a record never invokes a Scaleform function. */
    uint64_t bits;
} SkseStoredUIArgument;
typedef void (*SkseUICallbackObserver)(void *, const SkseStoredUICallback *);
typedef void (*SkseUIArgumentObserver)(void *, const SkseStoredUICallback *, uint32_t, const SkseStoredUIArgument *);
SkseObjectResult skse_ui_callback_objects(const SkseCoreSnapshot *snapshot, SkseUIObjectSummary *summary,
    SkseUICallbackObserver callback_observer, SkseUIArgumentObserver argument_observer, void *context);

#ifdef __cplusplus
}
#endif
