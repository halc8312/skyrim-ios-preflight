/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_X86_CALL_H
#define SKYRIM_EXPERIMENT_SKSE_X86_CALL_H

#include "pe_image.h"
#include "x86_heap.h"

typedef enum {
    SKSE_X86_OK,
    SKSE_X86_BAD_ARGUMENT,
    SKSE_X86_MEMORY_FAULT,
    SKSE_X86_UNSUPPORTED_INSTRUCTION,
    SKSE_X86_INSTRUCTION_LIMIT,
    SKSE_X86_BAD_CALL_RETURN,
    SKSE_X86_UNSUPPORTED_API
} SkseX86Status;

typedef enum { SKSE_X86_CDECL, SKSE_X86_STDCALL } SkseX86Convention;

typedef struct {
    uint32_t base;
    uint8_t *bytes;
    size_t length;
} SkseX86Scratch;

enum { SKSE_X86_MAX_SERVICES = 20, SKSE_X86_MAX_IMAGES = 4, SKSE_CLASSIC_MESSAGING_ID = 5 };
#define SKSE_X86_PROCESS_HEAP UINT32_C(0xe1000000)

typedef enum {
    SKSE_X86_SCALAR_RETURN, SKSE_X86_MEMCPY, SKSE_X86_MEMMOVE, SKSE_X86_MEMSET,
    SKSE_X86_QUERY_CLASSIC, SKSE_X86_REGISTER_CORE_LISTENER, SKSE_X86_UNAVAILABLE,
    SKSE_X86_MALLOC, SKSE_X86_CALLOC, SKSE_X86_REALLOC, SKSE_X86_FREE,
    SKSE_X86_HEAP_ALLOC, SKSE_X86_HEAP_REALLOC, SKSE_X86_HEAP_FREE
} SkseX86ServiceKind;

/* A service address is a guest token outside the image and scratch. The
 * recognized operations use scalar results or checked guest ranges. */
typedef struct {
    uint32_t address, value;
    SkseX86ServiceKind kind;
} SkseX86Service;

typedef struct {
    uint32_t interface_address, owner_handle;
    uint32_t listener, unavailable_id;
} SkseX86CoreMessaging;

typedef struct {
    const SkseX86Service *entries;
    size_t count;
    SkseX86CoreMessaging *core_messaging;
    SkseX86Heap *heap;
} SkseX86Services;

typedef struct {
    SkseX86Status status;
    uint32_t eax, instructions, instruction_va;
    uint32_t service_calls[SKSE_X86_MAX_SERVICES];
} SkseX86Result;

/* Invoke one 32-bit function using a mapped PE and private data storage.
 * Scratch at low addresses can hold argument structures. The stack begins at
 * its aligned high end. Discard the instance after a failed call, which may
 * have modified its writable PE sections or scratch. */
SkseX86Result skse_x86_call(SksePeImage *image, uint32_t entry,
                            SkseX86Scratch scratch, const uint32_t *args,
                            size_t arg_count, uint32_t instruction_limit,
                            SkseX86Services services, SkseX86Convention convention);
/* The images share guest addresses for one invocation. Each image has
 * independent storage and unchanged permissions. All ranges must be disjoint. */
SkseX86Result skse_x86_call_in_space(SksePeImage *const *images, size_t image_count, uint32_t entry,
                                     SkseX86Scratch scratch, const uint32_t *args,
                                     size_t arg_count, uint32_t instruction_limit,
                                     SkseX86Services services, SkseX86Convention convention);

const char *skse_x86_status_text(SkseX86Status status);
#endif
