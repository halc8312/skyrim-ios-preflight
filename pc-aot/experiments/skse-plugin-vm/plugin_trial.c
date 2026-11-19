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
    TLS_TABLE_OFFSET = 256,
    TLS_SCRATCH_LIMIT = 64 * 1024,
    TLS_MAX_ALIGNMENT = 8192,
    RELEASE_CALLBACK_OFFSET = 16,
    HANDLE_CALLBACK_OFFSET = RELEASE_CALLBACK_OFFSET + 4,
    QUERY_INTERFACE_OFFSET = HANDLE_CALLBACK_OFFSET + 4,
    REGISTER_CALLBACK_OFFSET = QUERY_INTERFACE_OFFSET + 4,
    UNAVAILABLE_CALLBACK_OFFSET = REGISTER_CALLBACK_OFFSET + 4,
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
    {{"KERNEL32.dll", "CloseHandle", 0xe00010a8}, SKSE_X86_READ_ONLY_FILES, SKSE_WIN_CLOSE_FILE}
};
enum { WINDOWS_SERVICE_COUNT = sizeof windows_services / sizeof windows_services[0] };
_Static_assert(WINDOWS_SERVICE_COUNT + 5 <= SKSE_X86_MAX_SERVICES, "trial service count");

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

static void install_static_tls(SkseDependencySpace *space, const uint32_t offsets[], SkseX86Scratch scratch) {
    for (unsigned i = 0; i < space->count; ++i) {
        if (!offsets[i]) continue;
        const SksePeTlsInfo *tls = &space->tls[i];
        SksePeImage *image = &space->images[i];
        if (tls->template_bytes)
            memcpy(scratch.bytes + offsets[i], image->bytes + tls->template_rva, tls->template_bytes);
        write_word(scratch.bytes + TLS_TABLE_OFFSET + 4 * i, scratch.base + offsets[i]);
        write_word(image->bytes + tls->index_rva, i);
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

static SkseTrialResult run_trial(SkseTrialDependency primary, SkseTrialOptions options, const SkseTrialRuntime *runtime,
                                 const SkseTrialDependency *catalog, size_t count, CatalogPolicy policy) {
    SkseTrialResult result = {.status = SKSE_TRIAL_IMAGE_REFUSED};
    SkseDependencySpace space = {0};
    SksePeImage *image = &space.images[0];
    SksePeImportBinding imports[WINDOWS_SERVICE_COUNT];
    if (runtime) {
        for (size_t i = 0; i < WINDOWS_SERVICE_COUNT; ++i) imports[i] = windows_services[i].binding;
        if (policy != NO_CATALOG) {
            if (policy == EXPLICIT_HELPER) {
                bool too_large = primary.length > SKSE_PE_MAX_FILE_BYTES || catalog[0].length > SKSE_PE_MAX_FILE_BYTES - primary.length;
                if (too_large || !skse_dependency_local_name_valid(catalog[0].file_name) || !catalog[0].bytes || !catalog[0].length) {
                    result.status = SKSE_TRIAL_DEPENDENCY_REFUSED;
                    result.dependency_status = too_large ? SKSE_PE_SIZE_LIMIT : SKSE_PE_BAD_ARGUMENT;
                    return result;
                }
            }
            SkseDependencyStatus status = skse_dependency_map(primary, catalog, count, runtime->guest_base,
                                        runtime->static_tls, imports, WINDOWS_SERVICE_COUNT, &space, &result.dependencies);
            if (status != SKSE_DEPENDENCY_READY || (policy == EXPLICIT_HELPER && space.count != 2)) {
                result.dependency_status = status == SKSE_DEPENDENCY_READY ? SKSE_PE_MISSING_DEPENDENCY_EXPORT
                                                                          : result.dependencies.image_status;
                if (status == SKSE_DEPENDENCY_NO_MEMORY) result.status = SKSE_TRIAL_NO_MEMORY;
                else if (result.dependencies.root_image_error && policy != EXPLICIT_HELPER)
                    result.image_status = result.dependency_status;
                else result.status = SKSE_TRIAL_DEPENDENCY_REFUSED;
                skse_dependency_close(&space);
                return result;
            }
        } else if (runtime->static_tls) {
            result.image_status = skse_pe_map_tls(primary.bytes, primary.length, runtime->guest_base,
                               imports, WINDOWS_SERVICE_COUNT, SKSE_PE_ALLOW_ENTRY, NULL, 0, image, &space.tls[0]);
        } else {
            result.image_status = runtime->guest_base
                ? skse_pe_map_at_base(primary.bytes, primary.length, runtime->guest_base, imports, WINDOWS_SERVICE_COUNT,
                                      SKSE_PE_ALLOW_ENTRY, image)
                : skse_pe_map_with_imports(primary.bytes, primary.length, imports, WINDOWS_SERVICE_COUNT, SKSE_PE_ALLOW_ENTRY, image);
        }
    } else {
        result.image_status = skse_pe_map(primary.bytes, primary.length, image);
    }
    if (result.image_status != SKSE_PE_OK) {
        if (result.image_status == SKSE_PE_UNAPPROVED_IMPORT) identify_unavailable_import(primary, &result.dependencies);
        return result;
    }
    if (policy == NO_CATALOG) space.count = 1;

    uint32_t tls_offsets[SKSE_DEPENDENCY_MAX_MODULES] = {0};
    uint32_t tls_scratch = 0;
    if (!layout_static_tls(&space, tls_offsets, &tls_scratch)) {
        result.image_status = SKSE_PE_SIZE_LIMIT;
        skse_dependency_close(&space);
        return result;
    }
    uint32_t scratch_size = SCRATCH_SIZE + tls_scratch;
    static const uint32_t scratch_choices[] = {0x60000000, 0x30000000, 0x68000000, 0x70000000, 0x78000000};
    static const uint32_t heap_choices[] = {0x18000000, 0x28000000, 0x38000000, 0x48000000, 0x58000000};
    SkseX86Scratch scratch = {
        .base = free_region(&space, scratch_choices, sizeof scratch_choices / sizeof scratch_choices[0],
                            scratch_size + UNAVAILABLE_CALLBACK_OFFSET + 1),
        .bytes = calloc(scratch_size, 1), .length = scratch_size
    };
    if (!scratch.base || !scratch.bytes) {
        result.status = SKSE_TRIAL_NO_MEMORY;
        free(scratch.bytes);
        skse_dependency_close(&space);
        return result;
    }

    uint32_t callbacks_base = scratch.base + scratch_size;
    SkseX86Service entries[SKSE_X86_MAX_SERVICES] = {
        {callbacks_base + RELEASE_CALLBACK_OFFSET, options.release, SKSE_X86_SCALAR_RETURN},
        {callbacks_base + HANDLE_CALLBACK_OFFSET, options.plugin_handle, SKSE_X86_SCALAR_RETURN},
    };
    bool enable_messages = runtime && runtime->core_messages && !options.editor;
    SkseX86CoreMessaging core = {
        .interface_address = scratch.base + MESSAGING_INTERFACE_OFFSET,
        .owner_handle = options.plugin_handle
    };
    SkseX86Heap heap = {.base = free_region(&space, heap_choices, sizeof heap_choices / sizeof heap_choices[0], SKSE_X86_HEAP_BYTES)};
    SkseWinThread thread = {.id = 1};
    SkseWinFiles files = {0};
    SkseX86Services services = {.entries = entries, .count = 2};
    if (tls_scratch) {
        services.static_tls_table = scratch.base + TLS_TABLE_OFFSET;
        services.stack_floor = scratch.base + tls_scratch;
        install_static_tls(&space, tls_offsets, scratch);
    }
    if (runtime) {
        services.heap = &heap;
        services.thread = &thread;
        files.source = runtime->assets;
        services.files = &files;
        services.system_exports = imports;
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
    if (enable_messages) {
        services.core_messaging = &core;
        uint32_t query_address = callbacks_base + QUERY_INTERFACE_OFFSET;
        uint32_t register_address = callbacks_base + REGISTER_CALLBACK_OFFSET;
        uint32_t unavailable_address = callbacks_base + UNAVAILABLE_CALLBACK_OFFSET;
        entries[services.count++] = (SkseX86Service){query_address, 0, SKSE_X86_QUERY_CLASSIC};
        entries[services.count++] = (SkseX86Service){register_address, 0, SKSE_X86_REGISTER_CORE_LISTENER};
        entries[services.count++] = (SkseX86Service){unavailable_address, 0, SKSE_X86_UNAVAILABLE};
        write_word(scratch.bytes + 16, query_address);
        write_word(scratch.bytes + MESSAGING_INTERFACE_OFFSET, 2);
        write_word(scratch.bytes + MESSAGING_INTERFACE_OFFSET + 4, register_address);
        write_word(scratch.bytes + MESSAGING_INTERFACE_OFFSET + 8, unavailable_address);
        write_word(scratch.bytes + MESSAGING_INTERFACE_OFFSET + 12, unavailable_address);
    }

    TrialVm vm = {.count = space.count, .scratch = scratch, .services = services, .remaining_work = TRIAL_BUDGET};
    for (unsigned i = 0; i < space.count; ++i) vm.images[i] = &space.images[i];
    bool detach_helper[SKSE_DEPENDENCY_MAX_HELPERS] = {false};
    bool helpers_ready = false;
    for (unsigned i = 0; i < result.dependencies.helper_count; ++i) {
        unsigned index = space.helper_order[i];
        const SksePeImage *helper = &space.images[index];
        if (!helper->entry_va && !space.tls[index].callback_count) continue;
        if (!run_tls_callbacks(&vm, &space, index, 1, &result)) {
            result.status = SKSE_TRIAL_TLS_ATTACH_FAULT;
            memcpy(result.failing_helper, result.dependencies.load_order[i], sizeof result.failing_helper);
            goto cleanup;
        }
        const uint32_t args[] = {helper->base, 1, 0};
        result.dependency_attach = helper->entry_va ? call_trial(&vm, helper->entry_va, args, 3, SKSE_X86_STDCALL)
                                                   : (SkseX86Result){.eax = 1};
        if (result.dependency_attach.status != SKSE_X86_OK) {
            result.status = SKSE_TRIAL_DEPENDENCY_ATTACH_FAULT;
            memcpy(result.failing_helper, result.dependencies.load_order[i], sizeof result.failing_helper);
            goto cleanup;
        }
        detach_helper[i] = true;
        if (!result.dependency_attach.eax) {
            result.status = SKSE_TRIAL_DEPENDENCY_ATTACH_DECLINED;
            memcpy(result.failing_helper, result.dependencies.load_order[i], sizeof result.failing_helper);
            goto cleanup;
        }
        ++result.helpers_attached;
    }
    helpers_ready = true;
    uint32_t entry_args[] = {image->base, 1, 0}; // Dynamic DLL_PROCESS_ATTACH.
    if (!run_tls_callbacks(&vm, &space, 0, 1, &result)) {
        result.status = SKSE_TRIAL_TLS_ATTACH_FAULT;
        goto cleanup;
    }
    if (image->entry_va) {
        result.attach = call_trial(&vm, image->entry_va, entry_args, 3, SKSE_X86_STDCALL);
        if (result.attach.status != SKSE_X86_OK) {
            result.status = SKSE_TRIAL_ATTACH_FAULT;
            goto cleanup;
        }
        if (!result.attach.eax) {
            result.status = SKSE_TRIAL_ATTACH_DECLINED;
            goto detach;
        }
    }

    const uint32_t args[] = {scratch.base, scratch.base + INFO_OFFSET};
    result.query = call_trial(&vm, image->query_va, args, 2, SKSE_X86_CDECL);
    if (result.query.status != SKSE_X86_OK) {
        result.status = SKSE_TRIAL_QUERY_FAULT;
    } else if (!(result.query.eax & 0xff)) {
        result.status = SKSE_TRIAL_QUERY_DECLINED;
    } else if (!read_plugin_info(&space, scratch.bytes + INFO_OFFSET, &result)) {
        result.status = SKSE_TRIAL_INVALID_INFO;
    } else {
        result.load = call_trial(&vm, image->load_va, args, 1, SKSE_X86_CDECL);
        if (result.load.status != SKSE_X86_OK) result.status = SKSE_TRIAL_LOAD_FAULT;
        else result.status = (result.load.eax & 0xff) ? SKSE_TRIAL_COMPLETED : SKSE_TRIAL_LOAD_DECLINED;
    }
    if (result.status == SKSE_TRIAL_COMPLETED && core.listener) {
        uint8_t *message = scratch.bytes + MESSAGE_OFFSET;
        const uint32_t parameters[] = {scratch.base + MESSAGE_OFFSET};
        entries[1].kind = SKSE_X86_UNAVAILABLE; // GetPluginHandle is scoped to Query/Load.
        for (uint32_t type = 0; type < 2; ++type) {
            memcpy(scratch.bytes + SENDER_OFFSET, "SKSE", 5);
            write_word(message, scratch.base + SENDER_OFFSET);
            write_word(message + 4, type); // PostLoad, followed by PostPostLoad.
            write_word(message + 8, 0);
            write_word(message + 12, 0);
            result.message = call_trial(&vm, core.listener, parameters, 1, SKSE_X86_CDECL);
            if (result.message.status != SKSE_X86_OK) {
                result.status = SKSE_TRIAL_MESSAGE_FAULT;
                break;
            }
            ++result.messages_delivered;
        }
        entries[1].kind = SKSE_X86_SCALAR_RETURN;
    }

detach:
    if (!run_tls_callbacks(&vm, &space, 0, 0, &result)) {
        if (result.status == SKSE_TRIAL_COMPLETED) result.status = SKSE_TRIAL_TLS_DETACH_FAULT;
    } else if (image->entry_va) {
        entry_args[1] = 0; // DLL_PROCESS_DETACH, including a declined attach.
        result.detach = call_trial(&vm, image->entry_va, entry_args, 3, SKSE_X86_STDCALL);
        if (result.detach.status != SKSE_X86_OK && result.status == SKSE_TRIAL_COMPLETED)
            result.status = SKSE_TRIAL_DETACH_FAULT;
    }
cleanup:
    for (unsigned step = 0; step < result.dependencies.helper_count; ++step) {
        unsigned i = helpers_ready ? space.detach_steps[step] : result.dependencies.helper_count - 1 - step;
        if (!detach_helper[i]) continue;
        unsigned index = space.helper_order[i];
        const SksePeImage *helper = &space.images[index];
        if (!run_tls_callbacks(&vm, &space, index, 0, &result)) {
            if (result.status == SKSE_TRIAL_COMPLETED) {
                result.status = SKSE_TRIAL_TLS_DETACH_FAULT;
                memcpy(result.failing_helper, result.dependencies.load_order[i], sizeof result.failing_helper);
            }
            continue;
        }
        uint32_t args[] = {helper->base, 0, 0};
        SkseX86Result detached = helper->entry_va ? call_trial(&vm, helper->entry_va, args, 3, SKSE_X86_STDCALL)
                                                : (SkseX86Result){0};
        if (result.dependency_detach.status == SKSE_X86_OK) result.dependency_detach = detached;
        if (detached.status == SKSE_X86_OK) ++result.helpers_detached;
        else if (result.status == SKSE_TRIAL_COMPLETED) {
            result.status = SKSE_TRIAL_DEPENDENCY_DETACH_FAULT;
            memcpy(result.failing_helper, result.dependencies.load_order[i], sizeof result.failing_helper);
        }
    }
    result.unavailable_interface = core.unavailable_id;
    skse_win_files_close(&files);
    free(scratch.bytes);
    skse_x86_heap_close(&heap);
    skse_dependency_close(&space);
    return result;
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
