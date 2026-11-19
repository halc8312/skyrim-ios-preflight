/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_X86_CALL_H
#define SKYRIM_EXPERIMENT_SKSE_X86_CALL_H

#include "pe_image.h"
#include "x86_heap.h"
#include "x86_winthread.h"
#include "x86_winfile.h"
#include "x86_winsync.h"
#include "x86_skse_records.h"

typedef enum {
    SKSE_X86_OK,
    SKSE_X86_BAD_ARGUMENT,
    SKSE_X86_MEMORY_FAULT,
    SKSE_X86_UNSUPPORTED_INSTRUCTION,
    SKSE_X86_INSTRUCTION_LIMIT,
    SKSE_X86_BAD_CALL_RETURN,
    SKSE_X86_UNSUPPORTED_API,
    SKSE_X86_DIVIDE_FAULT
} SkseX86Status;

typedef enum { SKSE_X86_CDECL, SKSE_X86_STDCALL } SkseX86Convention;

typedef struct {
    uint32_t base;
    uint8_t *bytes;
    size_t length;
} SkseX86Scratch;

enum {
    SKSE_X86_MAX_SERVICES = 96, SKSE_X86_MAX_IMAGES = 4,
    SKSE_CLASSIC_SERIALIZATION_ID = 3, SKSE_CLASSIC_TASK_ID = 4, SKSE_CLASSIC_MESSAGING_ID = 5
};
#define SKSE_X86_PROCESS_HEAP UINT32_C(0xe1000000)

typedef enum {
    SKSE_X86_SCALAR_RETURN, SKSE_X86_MEMCPY, SKSE_X86_MEMMOVE, SKSE_X86_MEMSET,
    SKSE_X86_QUERY_CLASSIC, SKSE_X86_REGISTER_CORE_LISTENER, SKSE_X86_UNAVAILABLE,
    SKSE_X86_MALLOC, SKSE_X86_CALLOC, SKSE_X86_REALLOC, SKSE_X86_FREE,
    SKSE_X86_HEAP_ALLOC, SKSE_X86_HEAP_REALLOC, SKSE_X86_HEAP_FREE, SKSE_X86_WIN_THREAD,
    SKSE_X86_INTERLOCKED, SKSE_X86_MODULE_LOOKUP, SKSE_X86_READ_ONLY_FILES, SKSE_X86_WIN_SYNC,
    SKSE_X86_SERIALIZATION, SKSE_X86_WIN_WORKER, SKSE_X86_ENQUEUE_TASK
} SkseX86ServiceKind;

typedef enum {
    SKSE_X86_ATOMIC_INC, SKSE_X86_ATOMIC_DEC, SKSE_X86_ATOMIC_EXCHANGE,
    SKSE_X86_ATOMIC_ADD, SKSE_X86_ATOMIC_COMPARE_EXCHANGE, SKSE_X86_ATOMIC_COUNT
} SkseX86Interlocked;

typedef enum {
    SKSE_X86_MODULE_ANSI, SKSE_X86_MODULE_WIDE, SKSE_X86_PROCEDURE, SKSE_X86_MODULE_LOOKUP_COUNT
} SkseX86ModuleLookup;

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

typedef enum {
    SKSE_SERIAL_UID, SKSE_SERIAL_REVERT, SKSE_SERIAL_SAVE, SKSE_SERIAL_LOAD, SKSE_SERIAL_FORM_DELETE,
    SKSE_SERIAL_WRITE, SKSE_SERIAL_OPEN, SKSE_SERIAL_APPEND, SKSE_SERIAL_NEXT, SKSE_SERIAL_READ,
    SKSE_SERIAL_RESOLVE_HANDLE, SKSE_SERIAL_RESOLVE_FORM, SKSE_SERIAL_METHOD_COUNT
} SkseSerialMethod;

typedef enum {
    SKSE_SERIAL_IDLE, SKSE_SERIAL_WRITING, SKSE_SERIAL_READING
} SkseSerialPhase;

typedef struct {
    uint32_t interface_address, owner_handle, uid;
    uint32_t callbacks[4];
    SkseSerialPhase phase;
    SkseX86RecordStream records;
} SkseX86Serialization;

enum { SKSE_X86_TASK_LIMIT = 16 };
typedef enum { SKSE_X86_GAME_TASK, SKSE_X86_UI_TASK, SKSE_X86_TASK_TYPES } SkseX86TaskType;
typedef struct {
    uint32_t delegates[SKSE_X86_TASK_LIMIT];
    unsigned head, count;
} SkseX86TaskQueue;
typedef struct {
    uint32_t interface_address;
    SkseX86TaskQueue queues[SKSE_X86_TASK_TYPES];
} SkseX86Tasks;

typedef struct {
    SkseX86Status status;
    uint32_t eax, instructions, work, instruction_va, failed_service;
    uint32_t service_calls[SKSE_X86_MAX_SERVICES];
} SkseX86Result;

typedef enum {
    SKSE_WIN_WORKER_CREATE, SKSE_WIN_WORKER_RESUME, SKSE_WIN_WORKER_EXIT_CODE, SKSE_WIN_WORKER_OPERATION_COUNT
} SkseWinWorkerOperation;

typedef SkseX86Result (*SkseX86RunWorker)(void *context, uint32_t id, uint32_t entry, uint32_t parameter, uint32_t budget);

/* One joinable diagnostic worker. Closing the handle leaves the worker
 * eligible to run until the owning trial is torn down. */
typedef struct {
    SkseX86RunWorker run;
    void *context;
    uint32_t id, handle, entry, parameter, exit_code;
    bool open, suspended, running, complete, failed, loader_locked, tearing_down;
} SkseX86Worker;

typedef struct {
    const SkseX86Service *entries;
    size_t count;
    SkseX86CoreMessaging *core_messaging;
    SkseX86Serialization *serialization;
    SkseX86Tasks *tasks;
    SkseX86Heap *heap;
    SkseWinThread *thread;
    SkseWinFiles *files;
    SkseWinSync *sync;
    SkseX86Worker *worker;
    /* Optional export names for resolving symbols in the emulated system DLLs.
     * The result must also be present in entries as a service token. */
    const SksePeImportBinding *system_exports;
    size_t system_export_count;
    /* The static TLS table, when present, is below the stack. A zero stack
     * bound uses the corresponding end of the full scratch allocation. */
    uint32_t static_tls_table, stack_floor, stack_top;
} SkseX86Services;

SkseX86Result skse_x86_finish_worker(SkseX86Worker *worker, uint32_t budget);

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
/* A parameterless MSVC i386 virtual method receives its object in ECX.
 * Find that slot in the current checked guest memory on each call. */
SkseX86Result skse_x86_call_virtual(SksePeImage *const *images, size_t image_count,
                                    uint32_t object, unsigned slot, SkseX86Scratch scratch,
                                    uint32_t instruction_limit, SkseX86Services services);

const char *skse_x86_status_text(SkseX86Status status);
#endif
