/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_X86_CALL_H
#define SKYRIM_EXPERIMENT_SKSE_X86_CALL_H

#include "pe_image.h"
#include "x86_heap.h"
#include "x86_winthread.h"
#include "x86_winfile.h"
#include "x86_winenv.h"
#include "papyrus_trial.h"
#include "x86_winsync.h"
#include "x86_skse_records.h"
#include "x86_classic_catalog.h"

typedef enum {
    SKSE_X86_OK,
    SKSE_X86_BAD_ARGUMENT,
    SKSE_X86_MEMORY_FAULT,
    SKSE_X86_UNSUPPORTED_INSTRUCTION,
    SKSE_X86_INSTRUCTION_LIMIT,
    SKSE_X86_BAD_CALL_RETURN,
    SKSE_X86_UNSUPPORTED_API,
    SKSE_X86_DIVIDE_FAULT,
    SKSE_X86_RECORD_WRITE_FAILED
} SkseX86Status;

typedef enum { SKSE_X86_CDECL, SKSE_X86_STDCALL } SkseX86Convention;

typedef struct {
    uint32_t base;
    uint8_t *bytes;
    size_t length;
} SkseX86Scratch;

/* A host-supplied immutable snapshot at a 32-bit guest address. The guest
 * can only read bytes inside one view and cannot branch into it. */
typedef struct {
    uint32_t base;
    uint8_t *bytes;
    size_t length;
} SkseX86ReadView;

enum { SKSE_X86_CODE_WRITE_LIMIT = 64 };
typedef struct {
    uint32_t address, value;
    unsigned width;
} SkseX86CodeWrite;

/* A diagnostic caller identifies one verified immutable code view. The
 * interpreter records scalar stores within its open protection window.
 * Reads still see the original bytes, and execution in the view is refused. */
typedef struct {
    size_t read_view_index, count;
    bool truncated;
    SkseX86CodeWrite writes[SKSE_X86_CODE_WRITE_LIMIT];
} SkseX86CodeAudit;

enum {
    SKSE_X86_MAX_SERVICES = 160, SKSE_X86_MAX_IMAGES = 4, SKSE_X86_MAX_HOOK_SITES = 8,
    SKSE_CLASSIC_SERIALIZATION_ID = 3, SKSE_CLASSIC_TASK_ID = 4, SKSE_CLASSIC_MESSAGING_ID = 5
};

typedef enum {
    SKSE_X86_SCALAR_RETURN, SKSE_X86_MEMCPY, SKSE_X86_MEMMOVE, SKSE_X86_MEMSET,
    SKSE_X86_QUERY_CLASSIC, SKSE_X86_REGISTER_CORE_LISTENER, SKSE_X86_UNAVAILABLE,
    SKSE_X86_MALLOC, SKSE_X86_CALLOC, SKSE_X86_REALLOC, SKSE_X86_FREE,
    SKSE_X86_HEAP_ALLOC, SKSE_X86_HEAP_REALLOC, SKSE_X86_HEAP_FREE, SKSE_X86_WIN_THREAD,
    SKSE_X86_INTERLOCKED, SKSE_X86_MODULE_LOOKUP, SKSE_X86_WIN_FILES, SKSE_X86_WIN_SYNC,
    SKSE_X86_SERIALIZATION, SKSE_X86_WIN_WORKER, SKSE_X86_ENQUEUE_TASK,
    SKSE_X86_PROTECT_IMAGE_DATA, SKSE_X86_WIN_TEXT, SKSE_X86_REGISTER_PAPYRUS, SKSE_X86_WIN_ENV,
    SKSE_X86_REGISTER_SHADOW_FUNCTION, SKSE_X86_HEAP_SIZE, SKSE_X86_CLASSIC_CATALOG, SKSE_X86_CLASSIC_FRAME,
    SKSE_X86_CLASSIC_LOOKUP_FORM, SKSE_X86_HEAP_CREATE, SKSE_X86_HEAP_DESTROY
} SkseX86ServiceKind;

typedef enum {
    SKSE_X86_ATOMIC_INC, SKSE_X86_ATOMIC_DEC, SKSE_X86_ATOMIC_EXCHANGE,
    SKSE_X86_ATOMIC_ADD, SKSE_X86_ATOMIC_COMPARE_EXCHANGE, SKSE_X86_ATOMIC_COUNT
} SkseX86Interlocked;

typedef enum {
    SKSE_X86_MODULE_ANSI, SKSE_X86_MODULE_WIDE, SKSE_X86_PROCEDURE,
    SKSE_X86_MODULE_PATH_ANSI, SKSE_X86_MODULE_PATH_WIDE, SKSE_X86_MODULE_LOOKUP_COUNT
} SkseX86ModuleLookup;

typedef enum {
    SKSE_WIN_WIDE_TO_MULTI, SKSE_WIN_MULTI_TO_WIDE, SKSE_WIN_GET_ACP, SKSE_WIN_IS_VALID_CODE_PAGE,
    SKSE_WIN_GET_CP_INFO, SKSE_WIN_GET_STRING_TYPE, SKSE_WIN_GET_LOCALE_INFO,
    SKSE_WIN_IS_VALID_LOCALE, SKSE_WIN_GET_USER_DEFAULT_LCID, SKSE_WIN_MAP_STRING,
    SKSE_WIN_COMPARE_STRING, SKSE_WIN_TEXT_OPERATION_COUNT
} SkseWinTextOperation;

typedef enum { SKSE_WIN_GET_ENV_W, SKSE_WIN_FREE_ENV_W, SKSE_WIN_SET_ENV_A, SKSE_WIN_ENV_OPERATION_COUNT } SkseWinEnvOperation;

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
    SKSE_SERIAL_IDLE, SKSE_SERIAL_WRITING, SKSE_SERIAL_READING, SKSE_SERIAL_REVERTING
} SkseSerialPhase;

/* The host supplies a load-scoped, verified catalog. All inputs are values.
 * The result pointer is ordinary private host scratch for this one call. */
typedef struct {
    const void *context;
    bool (*form_id)(const void *context, uint32_t saved, uint32_t *current);
    bool (*handle)(const void *context, uint64_t saved, uint64_t *current);
} SkseSerialResolvers;

typedef struct {
    uint32_t interface_address, owner_handle, uid;
    uint32_t callbacks[4];
    SkseSerialPhase phase;
    bool write_failed;
    const SkseSerialResolvers *resolvers;
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
    uint32_t site, target;
} SkseX86HookObservation;

typedef struct {
    SkseX86Status status;
    uint32_t eax, instructions, work, instruction_va, failed_service;
    uint32_t fault_address;
    unsigned fault_access; // SKSE_PE_READ, SKSE_PE_WRITE and/or SKSE_PE_EXEC, or zero if unavailable.
    uint32_t protection_address, protection_length, protection_flags; // Rejected VirtualProtect request.
    unsigned hook_count;
    SkseX86HookObservation hooks[SKSE_X86_MAX_HOOK_SITES];
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

typedef bool (*SkseX86RegisterShadowFunction)(void *context, uint32_t owner,
                                              uint32_t registry, uint32_t object);

typedef struct {
    const SkseX86Service *entries;
    size_t count;
    SkseX86CoreMessaging *core_messaging;
    SksePapyrusTrial *papyrus;
    uint32_t papyrus_interface;
    SkseX86RegisterShadowFunction register_shadow_function;
    SkseClassicCatalog *classic_catalog;
    const SkseClassicFrame *classic_frame;
    const SkseClassicLoadForms *classic_load_forms;
    SkseX86Serialization *serialization;
    SkseX86Tasks *tasks;
    SkseX86Heap *heap;
    SkseWinEnv *environment;
    SkseWinThread *thread;
    SkseWinFiles *files;
    SkseWinSync *sync;
    SkseX86Worker *worker;
    /* Optional export names for resolving symbols in the emulated system DLLs.
     * The result must also be present in entries as a service token. */
    const SksePeImportBinding *system_exports;
    size_t system_export_count;
    /* Optional validated basenames in the same order as the mapped images. */
    const char (*module_names)[SKSE_PE_MAX_IMPORT_NAME];
    const SkseX86ReadView *read_views;
    size_t read_view_count;
    /* Hosted-only observation of five-byte near-jump proposals. Sites are
     * virtual numeric identifiers, with no mapped memory behind them. */
    const uint32_t *hook_probe_sites;
    size_t hook_probe_count;
    SkseX86CodeAudit *code_audit; /* Optional, one call; never used by the resident VM. */
    /* The static TLS table, when present, is below the stack. A zero stack
     * bound uses the corresponding end of the full scratch allocation. */
    uint32_t static_tls_table, stack_floor, stack_top;
} SkseX86Services;

SkseX86Result skse_x86_finish_worker(SkseX86Worker *worker, uint32_t budget);

/* Check a data read against both the PE layout and the active guest page
 * protections. Host copies of guest data use the same policy as instructions. */
bool skse_x86_image_readable(const SksePeImage *image, uint32_t address, size_t length);

/* Invoke one 32-bit function using a mapped PE and private data storage.
 * Scratch at low addresses can hold argument structures. The stack begins at
 * its aligned high end. Discard the instance after a failed call, which may
 * have modified its writable PE sections or scratch. */
SkseX86Result skse_x86_call(SksePeImage *image, uint32_t entry,
                            SkseX86Scratch scratch, const uint32_t *args,
                            size_t arg_count, uint32_t instruction_limit,
                            SkseX86Services services, SkseX86Convention convention);
/* Images share guest addresses for one invocation. All ranges must be disjoint.
 * Data page protections belong to each mapping. Serialize uses of a mapping
 * and release its page table with skse_pe_free alongside the image. */
SkseX86Result skse_x86_call_in_space(SksePeImage *const *images, size_t image_count, uint32_t entry,
                                     SkseX86Scratch scratch, const uint32_t *args,
                                     size_t arg_count, uint32_t instruction_limit,
                                     SkseX86Services services, SkseX86Convention convention);
/* A parameterless MSVC i386 virtual method receives its object in ECX.
 * Find that slot in the current checked guest memory on each call. */
SkseX86Result skse_x86_call_virtual(SksePeImage *const *images, size_t image_count,
                                    uint32_t object, unsigned slot, SkseX86Scratch scratch,
                                    uint32_t instruction_limit, SkseX86Services services);

/* Invoke an already selected i386 member function. The object is passed
 * in ECX and the callee must pop all explicit arguments. */
SkseX86Result skse_x86_call_thiscall(SksePeImage *const *images, size_t image_count, uint32_t entry,
                                     uint32_t object, SkseX86Scratch scratch, const uint32_t *args,
                                     size_t arg_count, uint32_t instruction_limit, SkseX86Services services);

const char *skse_x86_status_text(SkseX86Status status);
#endif
