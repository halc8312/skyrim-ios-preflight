/* SPDX-License-Identifier: GPL-3.0-only */
#include "plugin_trial.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum {
    SCRATCH_SIZE = 32 * 1024,
    INFO_OFFSET = 64,
    MESSAGING_INTERFACE_OFFSET = 128,
    MESSAGE_OFFSET = 160,
    SENDER_OFFSET = 176,
    SERIALIZATION_INTERFACE_OFFSET = 192,
    TASK_INTERFACE_OFFSET = 244,
    TLS_TABLE_OFFSET = 256,
    TLS_SCRATCH_LIMIT = 64 * 1024,
    TLS_MAX_ALIGNMENT = 8192,
    RELEASE_CALLBACK_OFFSET = 16,
    HANDLE_CALLBACK_OFFSET = RELEASE_CALLBACK_OFFSET + 4,
    QUERY_INTERFACE_OFFSET = HANDLE_CALLBACK_OFFSET + 4,
    REGISTER_CALLBACK_OFFSET = QUERY_INTERFACE_OFFSET + 4,
    UNAVAILABLE_CALLBACK_OFFSET = REGISTER_CALLBACK_OFFSET + 4,
    SERIALIZATION_CALLBACK_OFFSET = UNAVAILABLE_CALLBACK_OFFSET + 4,
    TASK_CALLBACK_OFFSET = SERIALIZATION_CALLBACK_OFFSET + SKSE_SERIAL_METHOD_COUNT * 4,
    CALLBACK_REGION_END = TASK_CALLBACK_OFFSET + SKSE_X86_TASK_TYPES * 4,
    CALL_BUDGET = 100000,
    TRIAL_BUDGET = 1000000
};
static const struct {
    SksePeImportBinding binding;
    SkseX86ServiceKind kind;
    uint32_t value;
} windows_services[] = {
    {{"KERNEL32.dll", "GetTickCount", 0xe0001000}, SKSE_X86_SCALAR_RETURN, 0},
    {{"msvcrt.dll", "memcpy", 0xe0001010}, SKSE_X86_MEMCPY, 0},
    {{"msvcrt.dll", "memmove", 0xe0001014}, SKSE_X86_MEMMOVE, 0},
    {{"msvcrt.dll", "memset", 0xe0001018}, SKSE_X86_MEMSET, 0},
    {{"msvcrt.dll", "malloc", 0xe0001020}, SKSE_X86_MALLOC, 0},
    {{"msvcrt.dll", "calloc", 0xe0001024}, SKSE_X86_CALLOC, 0},
    {{"msvcrt.dll", "realloc", 0xe0001028}, SKSE_X86_REALLOC, 0},
    {{"msvcrt.dll", "free", 0xe000102c}, SKSE_X86_FREE, 0},
    {{"KERNEL32.dll", "GetProcessHeap", 0xe0001030}, SKSE_X86_SCALAR_RETURN, SKSE_X86_PROCESS_HEAP},
    {{"KERNEL32.dll", "HeapAlloc", 0xe0001034}, SKSE_X86_HEAP_ALLOC, 0},
    {{"KERNEL32.dll", "HeapReAlloc", 0xe0001038}, SKSE_X86_HEAP_REALLOC, 0},
    {{"KERNEL32.dll", "HeapFree", 0xe000103c}, SKSE_X86_HEAP_FREE, 0},
    {{"KERNEL32.dll", "TlsAlloc", 0xe0001040}, SKSE_X86_WIN_THREAD, SKSE_WIN_TLS_ALLOC},
    {{"KERNEL32.dll", "TlsFree", 0xe0001044}, SKSE_X86_WIN_THREAD, SKSE_WIN_TLS_FREE},
    {{"KERNEL32.dll", "TlsGetValue", 0xe0001048}, SKSE_X86_WIN_THREAD, SKSE_WIN_TLS_GET},
    {{"KERNEL32.dll", "TlsSetValue", 0xe000104c}, SKSE_X86_WIN_THREAD, SKSE_WIN_TLS_SET},
    {{"KERNEL32.dll", "GetLastError", 0xe0001050}, SKSE_X86_WIN_THREAD, SKSE_WIN_GET_LAST_ERROR},
    {{"KERNEL32.dll", "SetLastError", 0xe0001054}, SKSE_X86_WIN_THREAD, SKSE_WIN_SET_LAST_ERROR},
    {{"KERNEL32.dll", "GetCurrentThreadId", 0xe0001058}, SKSE_X86_WIN_THREAD, SKSE_WIN_CURRENT_THREAD_ID},
    {{"KERNEL32.dll", "InitializeCriticalSection", 0xe000105c}, SKSE_X86_WIN_THREAD, SKSE_WIN_CRITICAL_INIT},
    {{"KERNEL32.dll", "InitializeCriticalSectionAndSpinCount", 0xe0001060}, SKSE_X86_WIN_THREAD, SKSE_WIN_CRITICAL_SPIN_INIT},
    {{"KERNEL32.dll", "EnterCriticalSection", 0xe0001064}, SKSE_X86_WIN_THREAD, SKSE_WIN_CRITICAL_ENTER},
    {{"KERNEL32.dll", "LeaveCriticalSection", 0xe0001068}, SKSE_X86_WIN_THREAD, SKSE_WIN_CRITICAL_LEAVE},
    {{"KERNEL32.dll", "TryEnterCriticalSection", 0xe000106c}, SKSE_X86_WIN_THREAD, SKSE_WIN_CRITICAL_TRY},
    {{"KERNEL32.dll", "DeleteCriticalSection", 0xe0001070}, SKSE_X86_WIN_THREAD, SKSE_WIN_CRITICAL_DELETE},
    {{"KERNEL32.dll", "InterlockedIncrement", 0xe0001074}, SKSE_X86_INTERLOCKED, SKSE_X86_ATOMIC_INC},
    {{"KERNEL32.dll", "InterlockedDecrement", 0xe0001078}, SKSE_X86_INTERLOCKED, SKSE_X86_ATOMIC_DEC},
    {{"KERNEL32.dll", "InterlockedExchange", 0xe000107c}, SKSE_X86_INTERLOCKED, SKSE_X86_ATOMIC_EXCHANGE},
    {{"KERNEL32.dll", "InterlockedExchangeAdd", 0xe0001080}, SKSE_X86_INTERLOCKED, SKSE_X86_ATOMIC_ADD},
    {{"KERNEL32.dll", "InterlockedCompareExchange", 0xe0001084}, SKSE_X86_INTERLOCKED, SKSE_X86_ATOMIC_COMPARE_EXCHANGE},
    {{"KERNEL32.dll", "GetModuleHandleA", 0xe0001088}, SKSE_X86_MODULE_LOOKUP, SKSE_X86_MODULE_ANSI},
    {{"KERNEL32.dll", "GetModuleHandleW", 0xe000108c}, SKSE_X86_MODULE_LOOKUP, SKSE_X86_MODULE_WIDE},
    {{"KERNEL32.dll", "GetProcAddress", 0xe0001090}, SKSE_X86_MODULE_LOOKUP, SKSE_X86_PROCEDURE},
    {{"KERNEL32.dll", "CreateFileA", 0xe0001094}, SKSE_X86_READ_ONLY_FILES, SKSE_WIN_OPEN_FILE_A},
    {{"KERNEL32.dll", "CreateFileW", 0xe0001098}, SKSE_X86_READ_ONLY_FILES, SKSE_WIN_OPEN_FILE_W},
    {{"KERNEL32.dll", "ReadFile", 0xe000109c}, SKSE_X86_READ_ONLY_FILES, SKSE_WIN_READ_FILE},
    {{"KERNEL32.dll", "GetFileSize", 0xe00010a0}, SKSE_X86_READ_ONLY_FILES, SKSE_WIN_FILE_SIZE},
    {{"KERNEL32.dll", "GetFileSizeEx", 0xe00010a4}, SKSE_X86_READ_ONLY_FILES, SKSE_WIN_FILE_SIZE_EX},
    {{"KERNEL32.dll", "CloseHandle", 0xe00010a8}, SKSE_X86_READ_ONLY_FILES, SKSE_WIN_CLOSE_FILE},
    {{"KERNEL32.dll", "CreateEventA", 0xe00010ac}, SKSE_X86_WIN_SYNC, SKSE_WIN_EVENT_CREATE},
    {{"KERNEL32.dll", "CreateEventW", 0xe00010b0}, SKSE_X86_WIN_SYNC, SKSE_WIN_EVENT_CREATE},
    {{"KERNEL32.dll", "SetEvent", 0xe00010b4}, SKSE_X86_WIN_SYNC, SKSE_WIN_EVENT_SET},
    {{"KERNEL32.dll", "ResetEvent", 0xe00010b8}, SKSE_X86_WIN_SYNC, SKSE_WIN_EVENT_RESET},
    {{"KERNEL32.dll", "WaitForSingleObject", 0xe00010bc}, SKSE_X86_WIN_SYNC, SKSE_WIN_WAIT_ONE},
    {{"KERNEL32.dll", "WaitForSingleObjectEx", 0xe00010c0}, SKSE_X86_WIN_SYNC, SKSE_WIN_WAIT_ONE_EX},
    {{"KERNEL32.dll", "WaitForMultipleObjects", 0xe00010c4}, SKSE_X86_WIN_SYNC, SKSE_WIN_WAIT_MANY},
    {{"KERNEL32.dll", "WaitForMultipleObjectsEx", 0xe00010c8}, SKSE_X86_WIN_SYNC, SKSE_WIN_WAIT_MANY_EX},
    {{"KERNEL32.dll", "SetFilePointerEx", 0xe00010cc}, SKSE_X86_READ_ONLY_FILES, SKSE_WIN_SEEK_FILE_EX},
    {{"KERNEL32.dll", "CreateSemaphoreA", 0xe00010d0}, SKSE_X86_WIN_SYNC, SKSE_WIN_SEMAPHORE_CREATE},
    {{"KERNEL32.dll", "CreateSemaphoreW", 0xe00010d4}, SKSE_X86_WIN_SYNC, SKSE_WIN_SEMAPHORE_CREATE},
    {{"KERNEL32.dll", "ReleaseSemaphore", 0xe00010d8}, SKSE_X86_WIN_SYNC, SKSE_WIN_SEMAPHORE_RELEASE},
    {{"KERNEL32.dll", "GetFileType", 0xe00010dc}, SKSE_X86_READ_ONLY_FILES, SKSE_WIN_FILE_TYPE},
    {{"KERNEL32.dll", "CreateThread", 0xe00010e0}, SKSE_X86_WIN_WORKER, SKSE_WIN_WORKER_CREATE},
    {{"KERNEL32.dll", "ResumeThread", 0xe00010e4}, SKSE_X86_WIN_WORKER, SKSE_WIN_WORKER_RESUME},
    {{"KERNEL32.dll", "GetExitCodeThread", 0xe00010e8}, SKSE_X86_WIN_WORKER, SKSE_WIN_WORKER_EXIT_CODE},
    {{"KERNEL32.dll", "GetFileAttributesA", 0xe00010ec}, SKSE_X86_READ_ONLY_FILES, SKSE_WIN_ATTRIBUTES_A},
    {{"KERNEL32.dll", "GetFileAttributesW", 0xe00010f0}, SKSE_X86_READ_ONLY_FILES, SKSE_WIN_ATTRIBUTES_W},
    {{"KERNEL32.dll", "SetFilePointer", 0xe00010f4}, SKSE_X86_READ_ONLY_FILES, SKSE_WIN_SEEK_FILE},
    {{"KERNEL32.dll", "InitializeCriticalSectionEx", 0xe00010f8}, SKSE_X86_WIN_THREAD, SKSE_WIN_CRITICAL_INIT_EX}
};
enum { WINDOWS_SERVICE_COUNT = sizeof windows_services / sizeof windows_services[0] };
_Static_assert(WINDOWS_SERVICE_COUNT + 5 + SKSE_SERIAL_METHOD_COUNT + SKSE_X86_TASK_TYPES <= SKSE_X86_MAX_SERVICES,
               "trial service count");
_Static_assert(SERIALIZATION_INTERFACE_OFFSET + 52 <= TASK_INTERFACE_OFFSET &&
               TASK_INTERFACE_OFFSET + 12 <= TLS_TABLE_OFFSET, "trial scratch ABI");

const char *skse_trial_windows_service_name(uint32_t token) {
    for (size_t i = 0; i < WINDOWS_SERVICE_COUNT; ++i)
        if (windows_services[i].binding.service_token == token) return windows_services[i].binding.symbol;
    return NULL;
}

typedef struct {
    SksePeImage *images[SKSE_DEPENDENCY_MAX_MODULES];
    size_t count;
    SkseX86Scratch scratch;
    SkseX86Services services;
    uint32_t remaining_work;
} TrialVm;

static uint32_t read_word(const uint8_t *memory) {
    return (uint32_t)memory[0] | (uint32_t)memory[1] << 8 |
           (uint32_t)memory[2] << 16 | (uint32_t)memory[3] << 24;
}

static void write_word(uint8_t *memory, uint32_t word) {
    for (unsigned i = 0; i < 4; ++i) memory[i] = (uint8_t)(word >> (8 * i));
}

static bool overlaps(uint32_t a, uint32_t a_size, uint32_t b, uint32_t b_size) {
    return a_size && b_size && (uint64_t)a + a_size > b && (uint64_t)b + b_size > a;
}

_Static_assert((unsigned)SKSE_DEPENDENCY_MAX_MODULES <= (unsigned)SKSE_X86_MAX_IMAGES, "module capacity");

static uint32_t free_region(const SkseDependencySpace *space, const uint32_t *choices, size_t count, uint32_t extent) {
    for (size_t i = 0; i < count; ++i) {
        unsigned j = 0;
        while (j < space->count && !overlaps(choices[i], extent, space->images[j].base, space->images[j].size)) ++j;
        if (j == space->count) return choices[i];
    }
    return 0;
}

static bool read_plugin_info(const SkseDependencySpace *space, const uint8_t *info, SkseTrialResult *result) {
    uint32_t version = read_word(info);
    uint32_t address = read_word(info + 4);
    if (version != 1 || !address) return false;
    const SksePeImage *image = NULL;
    for (unsigned i = 0; i < space->count; ++i) {
        if (skse_pe_allows(&space->images[i], address, 1, SKSE_PE_READ)) { image = &space->images[i]; break; }
    }
    if (!image) return false;
    for (size_t i = 0; i < sizeof result->name; ++i) {
        if (i > UINT32_MAX - address ||
            !skse_pe_allows(image, address + (uint32_t)i, 1, SKSE_PE_READ)) return false;
        uint8_t byte = image->bytes[(address - image->base) + i];
        if (!byte) {
            if (i == 0) return false;
            result->info_version = version;
            result->plugin_version = read_word(info + 8);
            return true;
        }
        if (byte < 0x20 || byte > 0x7e || i + 1 == sizeof result->name) return false;
        result->name[i] = (char)byte;
    }
    return false;
}

static SkseX86Result call_trial(TrialVm *vm, uint32_t entry, const uint32_t *args,
                                size_t count, SkseX86Convention convention) {
    if (!vm->remaining_work) return (SkseX86Result){.status = SKSE_X86_INSTRUCTION_LIMIT, .instruction_va = entry};
    uint32_t limit = vm->remaining_work < CALL_BUDGET ? vm->remaining_work : CALL_BUDGET;
    SkseX86Result result = skse_x86_call_in_space(vm->images, vm->count, entry, vm->scratch, args, count, limit,
                                               vm->services, convention);
    vm->remaining_work -= result.work;
    return result;
}

static bool layout_static_tls(const SkseDependencySpace *space, uint32_t offsets[], uint32_t *extra_scratch) {
    uint32_t end = TLS_TABLE_OFFSET + SKSE_DEPENDENCY_MAX_MODULES * 4;
    for (unsigned i = 0; i < space->count; ++i) {
        const SksePeTlsInfo *tls = &space->tls[i];
        if (!tls->present) continue;
        uint32_t alignment = tls->alignment ? tls->alignment : 4;
        if (alignment > TLS_MAX_ALIGNMENT || (alignment & (alignment - 1))) return false;
        uint32_t start = (end + alignment - 1) & ~(alignment - 1);
        uint64_t length = (uint64_t)tls->template_bytes + tls->zero_fill;
        if (!length) length = 1;
        if (start >= TLS_SCRATCH_LIMIT || length > TLS_SCRATCH_LIMIT - start) return false;
        offsets[i] = start;
        end = start + (uint32_t)length;
        *extra_scratch = (end + 15) & ~UINT32_C(15);
    }
    return true;
}

static void install_static_tls(SkseDependencySpace *space, const uint32_t offsets[], SkseX86Scratch scratch, bool assign_indices) {
    for (unsigned i = 0; i < space->count; ++i) {
        if (!offsets[i]) continue;
        const SksePeTlsInfo *tls = &space->tls[i];
        SksePeImage *image = &space->images[i];
        if (tls->template_bytes)
            memcpy(scratch.bytes + offsets[i], image->bytes + tls->template_rva, tls->template_bytes);
        write_word(scratch.bytes + TLS_TABLE_OFFSET + 4 * i, scratch.base + offsets[i]);
        if (assign_indices) write_word(image->bytes + tls->index_rva, i);
    }
}

static bool run_tls_callbacks(TrialVm *vm, const SkseDependencySpace *space, unsigned index, uint32_t reason,
                              SkseTrialResult *result) {
    const SksePeTlsInfo *tls = &space->tls[index];
    const uint32_t base = space->images[index].base;
    const uint32_t parameters[] = {base, reason, 0};
    for (unsigned i = 0; i < tls->callback_count; ++i) {
        SkseX86Result call = call_trial(vm, base + tls->callback_rvas[i], parameters, 3, SKSE_X86_STDCALL);
        if (call.status != SKSE_X86_OK) {
            if (result->tls_fault.status == SKSE_X86_OK) result->tls_fault = call;
            return false;
        }
        ++result->tls_callbacks;
    }
    return true;
}

typedef struct {
    TrialVm *parent;
    SkseDependencySpace *space;
    SkseTrialResult *result;
    const uint32_t *tls_offsets;
    uint32_t stack_offset, tls_scratch;
} TrialWorkerContext;

struct SksePluginSession {
    SkseTrialResult result;
    SkseDependencySpace space;
    SksePeImportBinding imports[WINDOWS_SERVICE_COUNT];
    SkseX86Service entries[SKSE_X86_MAX_SERVICES];
    uint32_t tls_offsets[SKSE_DEPENDENCY_MAX_MODULES];
    SkseX86CoreMessaging core;
    SkseX86Serialization serial;
    SkseX86Tasks tasks;
    SkseX86Heap heap;
    SkseWinThreadProcess process;
    SkseWinThread thread;
    SkseWinFiles files;
    SkseWinSync sync;
    TrialVm vm;
    SkseX86Worker worker;
    TrialWorkerContext worker_context;
    unsigned startup_events;
    bool detach_primary, helpers_ready, detach_helper[SKSE_DEPENDENCY_MAX_HELPERS];
};

static SkseX86Result run_worker(void *opaque, uint32_t id, uint32_t entry, uint32_t parameter, uint32_t budget) {
    TrialWorkerContext *context = opaque;
    SkseTrialResult *result = context->result;
    TrialVm child = *context->parent;
    SkseWinThread thread = {.process = child.services.thread->process, .id = id};
    SkseX86Scratch worker_area = {
        .base = child.scratch.base + context->stack_offset,
        .bytes = child.scratch.bytes + context->stack_offset,
        .length = child.scratch.length - context->stack_offset
    };
    child.services.thread = &thread;
    child.services.stack_floor = worker_area.base + context->tls_scratch;
    child.services.stack_top = worker_area.base + (uint32_t)worker_area.length;
    child.remaining_work = budget;
    if (context->tls_scratch) {
        memset(worker_area.bytes, 0, context->tls_scratch);
        install_static_tls(context->space, context->tls_offsets, worker_area, false);
        child.services.static_tls_table = worker_area.base + TLS_TABLE_OFFSET;
    }
    uint32_t notifications[] = {context->space->images[0].base, 2, 0}; // DLL_THREAD_ATTACH.
    SkseX86Result call;
    uint32_t exit_code = 0, dll_main = context->space->images[0].entry_va;
    if (!run_tls_callbacks(&child, context->space, 0, notifications[1], result)) {
        call = result->tls_fault;
        goto done;
    }
    if (dll_main) {
        call = call_trial(&child, dll_main, notifications, 3, SKSE_X86_STDCALL);
        if (call.status != SKSE_X86_OK) goto done;
    }
    call = call_trial(&child, entry, &parameter, 1, SKSE_X86_STDCALL);
    if (call.status != SKSE_X86_OK) goto done;
    exit_code = call.eax;
    notifications[1] = 3; // DLL_THREAD_DETACH.
    if (!run_tls_callbacks(&child, context->space, 0, notifications[1], result)) {
        call = result->tls_fault;
        goto done;
    }
    if (dll_main) call = call_trial(&child, dll_main, notifications, 3, SKSE_X86_STDCALL);
done:
    call.work = budget - child.remaining_work;
    if (call.status == SKSE_X86_OK) {
        call.eax = exit_code;
        ++result->workers_completed;
    } else {
        result->worker_fault = call;
    }
    return call;
}

static void exercise_serialization(TrialVm *vm, SkseX86Serialization *serial, SkseTrialResult *result) {
    if (!serial->uid || !serial->callbacks[SKSE_SERIAL_SAVE - SKSE_SERIAL_REVERT] ||
        !serial->callbacks[SKSE_SERIAL_LOAD - SKSE_SERIAL_REVERT]) return;

    const SkseSerialMethod schedule[] = {SKSE_SERIAL_SAVE, SKSE_SERIAL_REVERT, SKSE_SERIAL_LOAD};
    const SkseSerialPhase phases[] = {SKSE_SERIAL_WRITING, SKSE_SERIAL_IDLE, SKSE_SERIAL_READING};
    const uint32_t parameters[] = {serial->interface_address};
    for (unsigned i = 0; i < sizeof schedule / sizeof schedule[0]; ++i) {
        const uint32_t callback = serial->callbacks[schedule[i] - SKSE_SERIAL_REVERT];
        if (!callback) continue;
        serial->phase = phases[i];
        if (serial->phase == SKSE_SERIAL_READING && !skse_x86_records_reset_reading(&serial->records)) {
            result->serialization_fault.status = SKSE_X86_BAD_ARGUMENT;
            result->status = SKSE_TRIAL_SERIALIZATION_FAULT;
            break;
        }
        SkseX86Result call = call_trial(vm, callback, parameters, 1, SKSE_X86_CDECL);
        if (call.status != SKSE_X86_OK) {
            result->serialization_fault = call;
            result->status = SKSE_TRIAL_SERIALIZATION_FAULT;
            break;
        }
        ++result->serialization_callbacks;
    }
    serial->phase = SKSE_SERIAL_IDLE;
    result->serialization_records = skse_x86_records_count(&serial->records);
}

static void run_task_queue(TrialVm *vm, SkseX86TaskQueue *queue, SkseTrialResult *result, SkseX86TaskType type) {
    while (queue->count) {
        uint32_t delegate = queue->delegates[queue->head];
        queue->head = (queue->head + 1) % SKSE_X86_TASK_LIMIT;
        --queue->count;
        for (unsigned slot = 0; slot < 2; ++slot) {
            const uint32_t limit = vm->remaining_work < CALL_BUDGET ? vm->remaining_work : CALL_BUDGET;
            SkseX86Result call = limit
                ? skse_x86_call_virtual(vm->images, vm->count, delegate, slot, vm->scratch, limit, vm->services)
                : (SkseX86Result){.status = SKSE_X86_INSTRUCTION_LIMIT};
            vm->remaining_work -= call.work;
            if (call.status != SKSE_X86_OK) {
                result->task_fault = call;
                result->status = SKSE_TRIAL_TASK_FAULT;
                return;
            }
        }
        ++result->tasks_completed[type];
    }
}

typedef enum { NO_CATALOG, PACKAGE_CATALOG, EXPLICIT_HELPER } CatalogPolicy;

static void identify_unavailable_import(SkseTrialDependency primary, SkseDependencyTrace *trace) {
    SksePeReport *report = malloc(sizeof *report);
    if (!report) return;
    skse_pe_inspect(primary.bytes, primary.length, report);
    for (uint16_t i = 0; i < report->symbol_count; ++i) {
        const SksePeImportSymbol *symbol = &report->symbols[i];
        if (symbol->by_ordinal || symbol->library_index >= report->library_count) continue;
        const char *library = report->libraries[symbol->library_index];
        size_t service = 0;
        for (; service < WINDOWS_SERVICE_COUNT; ++service) {
            const SksePeImportBinding *binding = &windows_services[service].binding;
            if (skse_pe_library_name_equal(library, binding->library) && !strcmp(symbol->name, binding->symbol)) break;
        }
        if (service == WINDOWS_SERVICE_COUNT) {
            memcpy(trace->requested, library, sizeof trace->requested);
            memcpy(trace->import_symbol, symbol->name, sizeof trace->import_symbol);
            break;
        }
    }
    free(report);
}

static bool map_session(SksePluginSession *session, SkseTrialDependency primary, const SkseTrialRuntime *runtime,
                        const SkseTrialDependency *catalog, size_t count, CatalogPolicy policy) {
    SkseTrialResult *result = &session->result;
    SkseDependencySpace *space = &session->space;
    SksePeImage *image = &space->images[0];
    SksePeImportBinding *imports = session->imports;
    if (runtime) {
        for (size_t i = 0; i < WINDOWS_SERVICE_COUNT; ++i) imports[i] = windows_services[i].binding;
        if (policy != NO_CATALOG) {
            if (policy == EXPLICIT_HELPER) {
                bool too_large = primary.length > SKSE_PE_MAX_FILE_BYTES || catalog[0].length > SKSE_PE_MAX_FILE_BYTES - primary.length;
                if (too_large || !skse_dependency_local_name_valid(catalog[0].file_name) || !catalog[0].bytes || !catalog[0].length) {
                    result->status = SKSE_TRIAL_DEPENDENCY_REFUSED;
                    result->dependency_status = too_large ? SKSE_PE_SIZE_LIMIT : SKSE_PE_BAD_ARGUMENT;
                    return false;
                }
            }
            SkseDependencyStatus status = skse_dependency_map(primary, catalog, count, runtime->guest_base,
                                        runtime->static_tls, imports, WINDOWS_SERVICE_COUNT, space, &result->dependencies);
            if (status != SKSE_DEPENDENCY_READY || (policy == EXPLICIT_HELPER && space->count != 2)) {
                result->dependency_status = status == SKSE_DEPENDENCY_READY ? SKSE_PE_MISSING_DEPENDENCY_EXPORT
                                                                           : result->dependencies.image_status;
                if (status == SKSE_DEPENDENCY_NO_MEMORY) result->status = SKSE_TRIAL_NO_MEMORY;
                else if (result->dependencies.root_image_error && policy != EXPLICIT_HELPER)
                    result->image_status = result->dependency_status;
                else result->status = SKSE_TRIAL_DEPENDENCY_REFUSED;
                return false;
            }
        } else if (runtime->static_tls) {
            result->image_status = skse_pe_map_tls(primary.bytes, primary.length, runtime->guest_base,
                                 imports, WINDOWS_SERVICE_COUNT, SKSE_PE_ALLOW_ENTRY, NULL, 0, image, &space->tls[0]);
        } else {
            result->image_status = runtime->guest_base
                ? skse_pe_map_at_base(primary.bytes, primary.length, runtime->guest_base, imports, WINDOWS_SERVICE_COUNT,
                                      SKSE_PE_ALLOW_ENTRY, image)
                : skse_pe_map_with_imports(primary.bytes, primary.length, imports, WINDOWS_SERVICE_COUNT, SKSE_PE_ALLOW_ENTRY, image);
        }
    } else {
        result->image_status = skse_pe_map(primary.bytes, primary.length, image);
    }
    if (result->image_status != SKSE_PE_OK) {
        if (result->image_status == SKSE_PE_UNAPPROVED_IMPORT) identify_unavailable_import(primary, &result->dependencies);
        return false;
    }
    if (policy == NO_CATALOG) space->count = 1;
    return true;
}

static bool prepare_vm(SksePluginSession *session, SkseTrialOptions options, const SkseTrialRuntime *runtime) {
    SkseDependencySpace *space = &session->space;

    uint32_t tls_scratch = 0;
    if (!layout_static_tls(space, session->tls_offsets, &tls_scratch)) {
        session->result.image_status = SKSE_PE_SIZE_LIMIT;
        return false;
    }
    uint32_t primary_scratch = SCRATCH_SIZE + tls_scratch;
    bool worker_supported = runtime && runtime->worker_thread && space->count == 1;
    uint32_t scratch_size = worker_supported ? 2 * primary_scratch : primary_scratch;
    static const uint32_t scratch_choices[] = {0x60000000, 0x30000000, 0x68000000, 0x70000000, 0x78000000};
    static const uint32_t heap_choices[] = {0x18000000, 0x28000000, 0x38000000, 0x48000000, 0x58000000};
    SkseX86Scratch scratch = {
        .base = free_region(space, scratch_choices, sizeof scratch_choices / sizeof scratch_choices[0],
                            scratch_size + CALLBACK_REGION_END),
        .bytes = calloc(scratch_size, 1), .length = scratch_size
    };
    if (!scratch.base || !scratch.bytes) {
        session->result.status = SKSE_TRIAL_NO_MEMORY;
        free(scratch.bytes);
        return false;
    }

    uint32_t callbacks_base = scratch.base + scratch_size;
    SkseX86Service *entries = session->entries;
    entries[0] = (SkseX86Service){callbacks_base + RELEASE_CALLBACK_OFFSET, options.release, SKSE_X86_SCALAR_RETURN};
    entries[1] = (SkseX86Service){callbacks_base + HANDLE_CALLBACK_OFFSET, options.plugin_handle, SKSE_X86_SCALAR_RETURN};
    bool enable_messages = runtime && runtime->core_messages && !options.editor;
    bool enable_serialization = runtime && runtime->serialization && !options.editor;
    bool enable_tasks = runtime && runtime->task_interface && !options.editor;
    session->core = (SkseX86CoreMessaging){
        .interface_address = enable_messages ? scratch.base + MESSAGING_INTERFACE_OFFSET : 0,
        .owner_handle = options.plugin_handle
    };
    session->serial = (SkseX86Serialization){
        .interface_address = scratch.base + SERIALIZATION_INTERFACE_OFFSET,
        .owner_handle = options.plugin_handle
    };
    session->tasks.interface_address = scratch.base + TASK_INTERFACE_OFFSET;
    session->heap.base = free_region(space, heap_choices, sizeof heap_choices / sizeof heap_choices[0], SKSE_X86_HEAP_BYTES);
    session->thread = (SkseWinThread){.process = &session->process, .id = 1};
    SkseX86Services services = {.entries = entries, .count = 2};
    if (tls_scratch) {
        services.static_tls_table = scratch.base + TLS_TABLE_OFFSET;
        services.stack_floor = scratch.base + tls_scratch;
        install_static_tls(space, session->tls_offsets, scratch, true);
    }
    if (worker_supported) services.stack_top = scratch.base + primary_scratch;
    if (runtime) {
        services.heap = &session->heap;
        services.thread = &session->thread;
        session->files.source = runtime->assets;
        services.files = &session->files;
        services.sync = &session->sync;
        services.system_exports = session->imports;
        services.system_export_count = WINDOWS_SERVICE_COUNT;
        for (size_t i = 0; i < WINDOWS_SERVICE_COUNT; ++i) {
            entries[services.count++] = (SkseX86Service){
                windows_services[i].binding.service_token, windows_services[i].value, windows_services[i].kind
            };
        }
        entries[2].value = runtime->uptime_ms;
    }
    // Classic SKSE's public 32-bit interface. Callback fields are virtual
    // addresses owned by the VM.
    write_word(scratch.bytes, options.skse_version);
    write_word(scratch.bytes + 4, options.runtime_version);
    write_word(scratch.bytes + 12, options.editor);
    if (options.expose_handle) write_word(scratch.bytes + 20, entries[1].address);
    if (options.expose_release) write_word(scratch.bytes + 24, entries[0].address);
    if (enable_messages || enable_serialization || enable_tasks) {
        services.core_messaging = &session->core;
        uint32_t query_address = callbacks_base + QUERY_INTERFACE_OFFSET;
        entries[services.count++] = (SkseX86Service){query_address, 0, SKSE_X86_QUERY_CLASSIC};
        write_word(scratch.bytes + 16, query_address);
    }
    if (enable_messages) {
        uint32_t register_address = callbacks_base + REGISTER_CALLBACK_OFFSET;
        uint32_t unavailable_address = callbacks_base + UNAVAILABLE_CALLBACK_OFFSET;
        entries[services.count++] = (SkseX86Service){register_address, 0, SKSE_X86_REGISTER_CORE_LISTENER};
        entries[services.count++] = (SkseX86Service){unavailable_address, 0, SKSE_X86_UNAVAILABLE};
        write_word(scratch.bytes + MESSAGING_INTERFACE_OFFSET, 2);
        write_word(scratch.bytes + MESSAGING_INTERFACE_OFFSET + 4, register_address);
        write_word(scratch.bytes + MESSAGING_INTERFACE_OFFSET + 8, unavailable_address);
        write_word(scratch.bytes + MESSAGING_INTERFACE_OFFSET + 12, unavailable_address);
    }
    if (enable_serialization) {
        services.serialization = &session->serial;
        uint8_t *interface = scratch.bytes + SERIALIZATION_INTERFACE_OFFSET;
        write_word(interface, 4);
        for (unsigned i = 0; i < SKSE_SERIAL_METHOD_COUNT; ++i) {
            uint32_t address = callbacks_base + SERIALIZATION_CALLBACK_OFFSET + 4 * i;
            entries[services.count++] = (SkseX86Service){address, i, SKSE_X86_SERIALIZATION};
            write_word(interface + 4 * (i + 1), address);
        }
    }
    if (enable_tasks) {
        services.tasks = &session->tasks;
        uint8_t *interface = scratch.bytes + TASK_INTERFACE_OFFSET;
        write_word(interface, 2);
        for (unsigned i = 0; i < SKSE_X86_TASK_TYPES; ++i) {
            uint32_t address = callbacks_base + TASK_CALLBACK_OFFSET + 4 * i;
            entries[services.count++] = (SkseX86Service){address, i, SKSE_X86_ENQUEUE_TASK};
            write_word(interface + 4 * (i + 1), address);
        }
    }

    session->vm = (TrialVm){.count = space->count, .scratch = scratch, .services = services, .remaining_work = TRIAL_BUDGET};
    session->worker_context = (TrialWorkerContext){
        .parent = &session->vm, .space = space, .result = &session->result, .tls_offsets = session->tls_offsets,
        .stack_offset = primary_scratch, .tls_scratch = tls_scratch
    };
    session->worker = (SkseX86Worker){.run = run_worker, .context = &session->worker_context, .loader_locked = true};
    if (worker_supported) session->vm.services.worker = &session->worker;
    for (unsigned i = 0; i < space->count; ++i) session->vm.images[i] = &space->images[i];
    return true;
}

static void initialize_session(SksePluginSession *session, SkseTrialDependency primary, SkseTrialOptions options,
                                const SkseTrialRuntime *runtime, const SkseTrialDependency *catalog,
                                size_t count, CatalogPolicy policy) {
    SkseTrialResult *result = &session->result;
    result->status = SKSE_TRIAL_IMAGE_REFUSED;
    if (!map_session(session, primary, runtime, catalog, count, policy) || !prepare_vm(session, options, runtime)) return;
    SkseDependencySpace *space = &session->space;
    SksePeImage *image = &space->images[0];
    TrialVm *vm = &session->vm;
    SkseX86Scratch scratch = vm->scratch;
    for (unsigned i = 0; i < result->dependencies.helper_count; ++i) {
        unsigned index = space->helper_order[i];
        const SksePeImage *helper = &space->images[index];
        if (!helper->entry_va && !space->tls[index].callback_count) continue;
        if (!run_tls_callbacks(vm, space, index, 1, result)) {
            result->status = SKSE_TRIAL_TLS_ATTACH_FAULT;
            memcpy(result->failing_helper, result->dependencies.load_order[i], sizeof result->failing_helper);
            return;
        }
        const uint32_t args[] = {helper->base, 1, 0};
        result->dependency_attach = helper->entry_va ? call_trial(vm, helper->entry_va, args, 3, SKSE_X86_STDCALL)
                                                    : (SkseX86Result){.eax = 1};
        if (result->dependency_attach.status != SKSE_X86_OK) {
            result->status = SKSE_TRIAL_DEPENDENCY_ATTACH_FAULT;
            memcpy(result->failing_helper, result->dependencies.load_order[i], sizeof result->failing_helper);
            return;
        }
        session->detach_helper[i] = true;
        if (!result->dependency_attach.eax) {
            result->status = SKSE_TRIAL_DEPENDENCY_ATTACH_DECLINED;
            memcpy(result->failing_helper, result->dependencies.load_order[i], sizeof result->failing_helper);
            return;
        }
        ++result->helpers_attached;
    }
    session->helpers_ready = true;
    const uint32_t entry_args[] = {image->base, 1, 0}; // Dynamic DLL_PROCESS_ATTACH.
    if (!run_tls_callbacks(vm, space, 0, 1, result)) {
        result->status = SKSE_TRIAL_TLS_ATTACH_FAULT;
        return;
    }
    if (image->entry_va) {
        result->attach = call_trial(vm, image->entry_va, entry_args, 3, SKSE_X86_STDCALL);
        if (result->attach.status != SKSE_X86_OK) {
            result->status = SKSE_TRIAL_ATTACH_FAULT;
            return;
        }
    }
    session->detach_primary = true; // Also notify a DLL which declined its attach.
    if (image->entry_va && !result->attach.eax) {
        result->status = SKSE_TRIAL_ATTACH_DECLINED;
        return;
    }

    session->worker.loader_locked = false;
    const uint32_t args[] = {scratch.base, scratch.base + INFO_OFFSET};
    result->query = call_trial(vm, image->query_va, args, 2, SKSE_X86_CDECL);
    if (result->query.status != SKSE_X86_OK) {
        result->status = SKSE_TRIAL_QUERY_FAULT;
    } else if (!(result->query.eax & 0xff)) {
        result->status = SKSE_TRIAL_QUERY_DECLINED;
    } else if (!read_plugin_info(space, scratch.bytes + INFO_OFFSET, result)) {
        result->status = SKSE_TRIAL_INVALID_INFO;
    } else {
        result->load = call_trial(vm, image->load_va, args, 1, SKSE_X86_CDECL);
        if (result->load.status != SKSE_X86_OK) result->status = SKSE_TRIAL_LOAD_FAULT;
        else result->status = (result->load.eax & 0xff) ? SKSE_TRIAL_COMPLETED : SKSE_TRIAL_LOAD_DECLINED;
    }
}

static void send_startup_message(SksePluginSession *session, uint32_t type) {
    if (!session->core.listener) return;
    TrialVm *vm = &session->vm;
    const SkseX86Scratch scratch = vm->scratch;
    uint8_t *message = scratch.bytes + MESSAGE_OFFSET;
    const uint32_t parameters[] = {scratch.base + MESSAGE_OFFSET};
    memcpy(scratch.bytes + SENDER_OFFSET, "SKSE", 5);
    write_word(message, scratch.base + SENDER_OFFSET);
    write_word(message + 4, type); // PostLoad, followed by PostPostLoad.
    write_word(message + 8, 0);
    write_word(message + 12, 0);
    session->entries[1].kind = SKSE_X86_UNAVAILABLE; // GetPluginHandle is scoped to Query/Load.
    SkseTrialResult *result = &session->result;
    result->message = call_trial(vm, session->core.listener, parameters, 1, SKSE_X86_CDECL);
    session->entries[1].kind = SKSE_X86_SCALAR_RETURN;
    if (result->message.status != SKSE_X86_OK) result->status = SKSE_TRIAL_MESSAGE_FAULT;
    else ++result->messages_delivered;
}

static void run_session_tasks(SksePluginSession *session, SkseX86TaskType type) {
    session->entries[1].kind = SKSE_X86_UNAVAILABLE;
    run_task_queue(&session->vm, &session->tasks.queues[type], &session->result, type);
    session->entries[1].kind = SKSE_X86_SCALAR_RETURN;
}

static void finish_session_worker(SksePluginSession *session) {
    SkseX86Worker *worker = &session->worker;
    if (!worker->id || worker->complete) return;
    TrialVm *vm = &session->vm;
    SkseX86Result call = skse_x86_finish_worker(worker, vm->remaining_work);
    vm->remaining_work -= call.work;
    if (call.status != SKSE_X86_OK) {
        session->result.worker_fault = call;
        session->result.status = SKSE_TRIAL_WORKER_FAULT;
    }
}

static void destroy_session_state(SksePluginSession *session) {
    SkseTrialResult *result = &session->result;
    SkseDependencySpace *space = &session->space;
    TrialVm *vm = &session->vm;
    session->worker.loader_locked = true;
    session->worker.tearing_down = true;
    if (session->detach_primary) {
        const SksePeImage *image = &space->images[0];
        if (!run_tls_callbacks(vm, space, 0, 0, result)) {
            if (result->status == SKSE_TRIAL_COMPLETED) result->status = SKSE_TRIAL_TLS_DETACH_FAULT;
        } else if (image->entry_va) {
            const uint32_t args[] = {image->base, 0, 0}; // DLL_PROCESS_DETACH.
            result->detach = call_trial(vm, image->entry_va, args, 3, SKSE_X86_STDCALL);
            if (result->detach.status != SKSE_X86_OK && result->status == SKSE_TRIAL_COMPLETED)
                result->status = SKSE_TRIAL_DETACH_FAULT;
        }
    }
    const unsigned helpers = result->dependencies.helper_count;
    for (unsigned step = 0; step < helpers; ++step) {
        unsigned i = session->helpers_ready ? space->detach_steps[step] : helpers - 1 - step;
        if (!session->detach_helper[i]) continue;
        unsigned index = space->helper_order[i];
        const SksePeImage *helper = &space->images[index];
        if (!run_tls_callbacks(vm, space, index, 0, result)) {
            if (result->status == SKSE_TRIAL_COMPLETED) {
                result->status = SKSE_TRIAL_TLS_DETACH_FAULT;
                memcpy(result->failing_helper, result->dependencies.load_order[i], sizeof result->failing_helper);
            }
            continue;
        }
        const uint32_t args[] = {helper->base, 0, 0};
        SkseX86Result detached = helper->entry_va ? call_trial(vm, helper->entry_va, args, 3, SKSE_X86_STDCALL)
                                                : (SkseX86Result){0};
        if (result->dependency_detach.status == SKSE_X86_OK) result->dependency_detach = detached;
        if (detached.status == SKSE_X86_OK) ++result->helpers_detached;
        else if (result->status == SKSE_TRIAL_COMPLETED) {
            result->status = SKSE_TRIAL_DEPENDENCY_DETACH_FAULT;
            memcpy(result->failing_helper, result->dependencies.load_order[i], sizeof result->failing_helper);
        }
    }
    result->unavailable_interface = session->core.unavailable_id;
    skse_x86_records_close(&session->serial.records);
    skse_win_files_close(&session->files);
    free(vm->scratch.bytes);
    skse_x86_heap_close(&session->heap);
    skse_dependency_close(space);
}

static SkseTrialResult run_trial(SkseTrialDependency primary, SkseTrialOptions options, const SkseTrialRuntime *runtime,
                                 const SkseTrialDependency *catalog, size_t count, CatalogPolicy policy) {
    SksePluginSession session = {0};
    initialize_session(&session, primary, options, runtime, catalog, count, policy);
    SkseTrialResult *result = &session.result;
    for (uint32_t type = 0; result->status == SKSE_TRIAL_COMPLETED && type < 2; ++type)
        send_startup_message(&session, type);
    if (result->status == SKSE_TRIAL_COMPLETED && session.vm.services.serialization) {
        session.entries[1].kind = SKSE_X86_UNAVAILABLE;
        exercise_serialization(&session.vm, &session.serial, result);
        session.entries[1].kind = SKSE_X86_SCALAR_RETURN;
    }
    while (result->status == SKSE_TRIAL_COMPLETED) {
        if (session.tasks.queues[SKSE_X86_GAME_TASK].count || session.tasks.queues[SKSE_X86_UI_TASK].count) {
            run_session_tasks(&session, SKSE_X86_GAME_TASK);
            if (result->status == SKSE_TRIAL_COMPLETED) run_session_tasks(&session, SKSE_X86_UI_TASK);
        } else if (session.worker.id && !session.worker.complete) {
            finish_session_worker(&session);
        } else {
            break;
        }
    }
    destroy_session_state(&session);
    return session.result;
}

SkseTrialResult skse_plugin_trial(const uint8_t *file, size_t length, SkseTrialOptions options) {
    return run_trial((SkseTrialDependency){NULL, file, length}, options, NULL, NULL, 0, NO_CATALOG);
}

SkseTrialResult skse_plugin_trial_with_runtime(const uint8_t *file, size_t length,
                                               SkseTrialOptions options, SkseTrialRuntime runtime) {
    return run_trial((SkseTrialDependency){NULL, file, length}, options, &runtime, NULL, 0, NO_CATALOG);
}

SkseTrialResult skse_plugin_trial_with_dependency(const uint8_t *file, size_t length,
                                                  SkseTrialOptions options, SkseTrialRuntime runtime,
                                                  SkseTrialDependency dependency) {
    return run_trial((SkseTrialDependency){NULL, file, length}, options, &runtime, &dependency, 1, EXPLICIT_HELPER);
}

SkseTrialResult skse_plugin_trial_with_package(SkseTrialDependency primary, const SkseTrialDependency *catalog,
                                              size_t count, SkseTrialOptions options, SkseTrialRuntime runtime) {
    return run_trial(primary, options, &runtime, catalog, count, PACKAGE_CATALOG);
}

SksePluginSession *skse_plugin_session_open(SkseTrialDependency primary, const SkseTrialDependency *catalog,
                                            size_t count, SkseTrialOptions options, SkseTrialRuntime runtime,
                                            SkseTrialResult *report) {
    SksePluginSession *session = calloc(1, sizeof *session);
    if (!session) {
        if (report) *report = (SkseTrialResult){.status = SKSE_TRIAL_NO_MEMORY};
        return NULL;
    }
    initialize_session(session, primary, options, &runtime, catalog, count, PACKAGE_CATALOG);
    if (session->result.status != SKSE_TRIAL_COMPLETED) {
        destroy_session_state(session);
        if (report) *report = session->result;
        free(session);
        return NULL;
    }
    session->result.unavailable_interface = session->core.unavailable_id;
    if (report) *report = session->result;
    return session;
}

SkseSessionResult skse_plugin_session_step(SksePluginSession *session, SkseSessionEvent event,
                                         uint32_t work_limit, uint32_t uptime_ms) {
    if (!session || !work_limit || work_limit > TRIAL_BUDGET || (unsigned)event > SKSE_SESSION_WORKER)
        return SKSE_SESSION_OUT_OF_ORDER;
    if (session->result.status != SKSE_TRIAL_COMPLETED) return SKSE_SESSION_FAULTED;
    const bool startup = event <= SKSE_SESSION_POST_POST_LOAD;
    if (startup ? (unsigned)event != session->startup_events : session->startup_events != 2)
        return SKSE_SESSION_OUT_OF_ORDER;

    session->vm.remaining_work = work_limit;
    session->entries[2].value = uptime_ms; // GetTickCount from this host event's uptime sample.
    switch (event) {
    case SKSE_SESSION_POST_LOAD:
    case SKSE_SESSION_POST_POST_LOAD:
        send_startup_message(session, (uint32_t)event);
        ++session->startup_events;
        break;
    case SKSE_SESSION_GAME_TASKS:
        run_session_tasks(session, SKSE_X86_GAME_TASK);
        break;
    case SKSE_SESSION_UI_TASKS:
        run_session_tasks(session, SKSE_X86_UI_TASK);
        break;
    case SKSE_SESSION_WORKER:
        finish_session_worker(session);
        break;
    }
    session->result.unavailable_interface = session->core.unavailable_id;
    return session->result.status == SKSE_TRIAL_COMPLETED ? SKSE_SESSION_OK : SKSE_SESSION_FAULTED;
}

const SkseTrialResult *skse_plugin_session_report(const SksePluginSession *session) {
    return session ? &session->result : NULL;
}

void skse_plugin_session_close(SksePluginSession *session, SkseTrialResult *report) {
    if (!session) return;
    session->vm.remaining_work = TRIAL_BUDGET;
    destroy_session_state(session);
    if (report) *report = session->result;
    free(session);
}
