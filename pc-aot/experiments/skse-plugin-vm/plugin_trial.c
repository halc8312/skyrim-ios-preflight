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
    RELEASE_CALLBACK_OFFSET = SCRATCH_SIZE + 16,
    HANDLE_CALLBACK_OFFSET = RELEASE_CALLBACK_OFFSET + 4,
    QUERY_INTERFACE_OFFSET = HANDLE_CALLBACK_OFFSET + 4,
    REGISTER_CALLBACK_OFFSET = QUERY_INTERFACE_OFFSET + 4,
    UNAVAILABLE_CALLBACK_OFFSET = REGISTER_CALLBACK_OFFSET + 4,
    CALL_BUDGET = 100000
};
static const uint32_t clock_token = UINT32_C(0xe0001000);
static const uint32_t copy_token = UINT32_C(0xe0001010);
static const uint32_t move_token = UINT32_C(0xe0001014);
static const uint32_t set_token = UINT32_C(0xe0001018);

typedef struct {
    SksePeImage *images[2];
    size_t count;
    SkseX86Scratch scratch;
    SkseX86Services services;
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

static uint32_t scratch_base(const SksePeImage *image, const SksePeImage *dependency) {
    static const uint32_t addresses[] = {0x60000000, 0x30000000, 0x68000000};
    const uint32_t extent = UNAVAILABLE_CALLBACK_OFFSET + 1;
    for (size_t i = 0; i < sizeof addresses / sizeof addresses[0]; ++i) {
        uint32_t candidate = addresses[i];
        if (!overlaps(candidate, extent, image->base, image->size) &&
            !overlaps(candidate, extent, dependency->base, dependency->size)) return candidate;
    }
    return 0;
}

static bool read_plugin_info(const SksePeImage *image, const uint8_t *info, SkseTrialResult *result) {
    uint32_t version = read_word(info);
    uint32_t address = read_word(info + 4);
    if (version != 1 || !address) return false;
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

static bool dependency_name_valid(const char *name) {
    if (!name) return false;
    size_t i = 0;
    for (; i < SKSE_PE_MAX_IMPORT_NAME && name[i]; ++i) {
        unsigned c = (uint8_t)name[i];
        if (c < 0x20 || c >= 0x7f || c == '/' || c == '\\' || c == ':') return false;
    }
    return i >= 5 && i < SKSE_PE_MAX_IMPORT_NAME && skse_pe_library_name_equal(name + i - 4, ".dll") &&
           !skse_pe_library_name_equal(name, "kernel32.dll") && !skse_pe_library_name_equal(name, "msvcrt.dll");
}

static SksePeStatus map_dependency(const uint8_t *file, size_t length, uint32_t guest_base,
                                   const SkseTrialDependency *dependency, SksePeImportBinding *bindings,
                                   size_t *binding_count, SksePeReport *primary, SksePeImage *image) {
    const char *name = dependency->file_name;
    if (!dependency_name_valid(name) || !dependency->bytes || !dependency->length) return SKSE_PE_BAD_ARGUMENT;
    if (length > SKSE_PE_MAX_FILE_BYTES || dependency->length > SKSE_PE_MAX_FILE_BYTES - length)
        return SKSE_PE_SIZE_LIMIT;
    SksePeStatus status = skse_pe_inspect(file, length, primary);
    if (status != SKSE_PE_UNSUPPORTED_IMPORTS && status != SKSE_PE_OK) return status;
    SksePeRequiredExport required[SKSE_PE_MAX_IMPORT_SYMBOLS] = {0};
    size_t count = 0;
    for (uint16_t i = 0; i < primary->symbol_count; ++i) {
        const SksePeImportSymbol *symbol = &primary->symbols[i];
        if (!skse_pe_library_name_equal(name, primary->libraries[symbol->library_index])) continue;
        if (symbol->by_ordinal) return SKSE_PE_ORDINAL_IMPORT;
        size_t j = 0;
        while (j < count && strcmp(required[j].name, symbol->name)) ++j;
        if (j == count) required[count++].name = symbol->name;
    }
    if (!count) return SKSE_PE_MISSING_DEPENDENCY_EXPORT;
    if (count > SKSE_PE_MAX_IMPORT_SYMBOLS - *binding_count) return SKSE_PE_SIZE_LIMIT;

    SksePeReport helper_layout;
    skse_pe_inspect(dependency->bytes, dependency->length, &helper_layout);
    uint32_t primary_base = guest_base ? guest_base : primary->preferred_base;
    uint32_t placement = 0;
    if (overlaps(primary_base, primary->image_size, helper_layout.preferred_base, helper_layout.image_size)) {
        placement = 0x40000000;
        if (overlaps(primary_base, primary->image_size, placement, helper_layout.image_size)) placement = 0x50000000;
    }
    status = skse_pe_map_library(dependency->bytes, dependency->length, placement, bindings, *binding_count,
                                  SKSE_PE_ALLOW_ENTRY, required, count, image);
    if (status != SKSE_PE_OK) return status;
    for (size_t i = 0; i < count; ++i) {
        // required[i].name points to the caller's live import manifest.
        bindings[*binding_count + i] = (SksePeImportBinding){name, required[i].name, required[i].address};
    }
    *binding_count += count;
    return SKSE_PE_OK;
}

static SkseX86Result call_trial(TrialVm *vm, uint32_t entry, const uint32_t *args,
                                size_t count, SkseX86Convention convention) {
    return skse_x86_call_in_space(vm->images, vm->count, entry, vm->scratch, args, count, CALL_BUDGET,
                                 vm->services, convention);
}

static SkseTrialResult run_trial(const uint8_t *file, size_t length, SkseTrialOptions options,
                                  const SkseTrialRuntime *runtime, const SkseTrialDependency *dependency) {
    SkseTrialResult result = {.status = SKSE_TRIAL_IMAGE_REFUSED};
    SksePeImage image = {0};
    SksePeImage helper = {0};
    if (runtime) {
        SksePeImportBinding imports[SKSE_PE_MAX_IMPORT_SYMBOLS] = {
            {"KERNEL32.dll", "GetTickCount", clock_token},
            {"msvcrt.dll", "memcpy", copy_token},
            {"msvcrt.dll", "memmove", move_token},
            {"msvcrt.dll", "memset", set_token}
        };
        size_t count = 4;
        SksePeReport primary;
        if (dependency) {
            result.dependency_status = map_dependency(file, length, runtime->guest_base, dependency, imports, &count,
                                                      &primary, &helper);
            if (result.dependency_status != SKSE_PE_OK) {
                result.status = SKSE_TRIAL_DEPENDENCY_REFUSED;
                return result;
            }
        }
        result.image_status = runtime->guest_base
            ? skse_pe_map_at_base(file, length, runtime->guest_base, imports, count, SKSE_PE_ALLOW_ENTRY, &image)
            : skse_pe_map_with_imports(file, length, imports, count, SKSE_PE_ALLOW_ENTRY, &image);
    } else {
        result.image_status = skse_pe_map(file, length, &image);
    }
    if (result.image_status != SKSE_PE_OK) {
        skse_pe_free(&helper);
        return result;
    }

    SkseX86Scratch scratch = {
        .base = scratch_base(&image, &helper), .bytes = calloc(SCRATCH_SIZE, 1), .length = SCRATCH_SIZE
    };
    if (!scratch.bytes) {
        result.status = SKSE_TRIAL_NO_MEMORY;
        skse_pe_free(&image);
        skse_pe_free(&helper);
        return result;
    }

    SkseX86Service entries[] = {
        {scratch.base + RELEASE_CALLBACK_OFFSET, options.release, SKSE_X86_SCALAR_RETURN},
        {scratch.base + HANDLE_CALLBACK_OFFSET, options.plugin_handle, SKSE_X86_SCALAR_RETURN},
        {clock_token, runtime ? runtime->uptime_ms : 0, SKSE_X86_SCALAR_RETURN},
        {copy_token, 0, SKSE_X86_MEMCPY},
        {move_token, 0, SKSE_X86_MEMMOVE},
        {set_token, 0, SKSE_X86_MEMSET},
        {scratch.base + QUERY_INTERFACE_OFFSET, 0, SKSE_X86_QUERY_CLASSIC},
        {scratch.base + REGISTER_CALLBACK_OFFSET, 0, SKSE_X86_REGISTER_CORE_LISTENER},
        {scratch.base + UNAVAILABLE_CALLBACK_OFFSET, 0, SKSE_X86_UNAVAILABLE}
    };
    bool enable_messages = runtime && runtime->core_messages && !options.editor;
    SkseX86CoreMessaging core = {
        .interface_address = scratch.base + MESSAGING_INTERFACE_OFFSET,
        .owner_handle = options.plugin_handle
    };
    SkseX86Services services = {.entries = entries, .count = runtime ? 6u : 2u};
    // Classic SKSE's public 32-bit interface. Callback fields are virtual
    // addresses owned by the VM.
    write_word(scratch.bytes, options.skse_version);
    write_word(scratch.bytes + 4, options.runtime_version);
    write_word(scratch.bytes + 12, options.editor);
    if (options.expose_handle) write_word(scratch.bytes + 20, entries[1].address);
    if (options.expose_release) write_word(scratch.bytes + 24, entries[0].address);
    if (enable_messages) {
        services.count = sizeof entries / sizeof entries[0];
        services.core_messaging = &core;
        write_word(scratch.bytes + 16, entries[6].address);
        write_word(scratch.bytes + MESSAGING_INTERFACE_OFFSET, 2);
        write_word(scratch.bytes + MESSAGING_INTERFACE_OFFSET + 4, entries[7].address);
        write_word(scratch.bytes + MESSAGING_INTERFACE_OFFSET + 8, entries[8].address);
        write_word(scratch.bytes + MESSAGING_INTERFACE_OFFSET + 12, entries[8].address);
    }

    TrialVm vm = {.images = {&image, &helper}, .count = helper.bytes ? 2u : 1u, .scratch = scratch, .services = services};
    bool helper_attached = false;
    uint32_t helper_args[] = {helper.base, 1, 0};
    if (helper.entry_va) {
        result.dependency_attach = call_trial(&vm, helper.entry_va, helper_args, 3, SKSE_X86_STDCALL);
        if (result.dependency_attach.status != SKSE_X86_OK) {
            result.status = SKSE_TRIAL_DEPENDENCY_ATTACH_FAULT;
            goto cleanup;
        }
        helper_attached = true;
        if (!result.dependency_attach.eax) {
            result.status = SKSE_TRIAL_DEPENDENCY_ATTACH_DECLINED;
            goto cleanup;
        }
    }
    uint32_t entry_args[] = {image.base, 1, 0}; // Dynamic DLL_PROCESS_ATTACH.
    if (image.entry_va) {
        result.attach = call_trial(&vm, image.entry_va, entry_args, 3, SKSE_X86_STDCALL);
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
    result.query = call_trial(&vm, image.query_va, args, 2, SKSE_X86_CDECL);
    if (result.query.status != SKSE_X86_OK) {
        result.status = SKSE_TRIAL_QUERY_FAULT;
    } else if (!(result.query.eax & 0xff)) {
        result.status = SKSE_TRIAL_QUERY_DECLINED;
    } else if (!read_plugin_info(&image, scratch.bytes + INFO_OFFSET, &result)) {
        result.status = SKSE_TRIAL_INVALID_INFO;
    } else {
        result.load = call_trial(&vm, image.load_va, args, 1, SKSE_X86_CDECL);
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
    if (image.entry_va) {
        entry_args[1] = 0; // DLL_PROCESS_DETACH, including a declined attach.
        result.detach = call_trial(&vm, image.entry_va, entry_args, 3, SKSE_X86_STDCALL);
        if (result.detach.status != SKSE_X86_OK && result.status == SKSE_TRIAL_COMPLETED)
            result.status = SKSE_TRIAL_DETACH_FAULT;
    }
cleanup:
    if (helper_attached) {
        helper_args[1] = 0;
        result.dependency_detach = call_trial(&vm, helper.entry_va, helper_args, 3, SKSE_X86_STDCALL);
        if (result.dependency_detach.status != SKSE_X86_OK && result.status == SKSE_TRIAL_COMPLETED)
            result.status = SKSE_TRIAL_DEPENDENCY_DETACH_FAULT;
    }
    result.unavailable_interface = core.unavailable_id;
    free(scratch.bytes);
    skse_pe_free(&image);
    skse_pe_free(&helper);
    return result;
}

SkseTrialResult skse_plugin_trial(const uint8_t *file, size_t length, SkseTrialOptions options) {
    return run_trial(file, length, options, NULL, NULL);
}

SkseTrialResult skse_plugin_trial_with_runtime(const uint8_t *file, size_t length,
                                               SkseTrialOptions options, SkseTrialRuntime runtime) {
    return run_trial(file, length, options, &runtime, NULL);
}

SkseTrialResult skse_plugin_trial_with_dependency(const uint8_t *file, size_t length,
                                                  SkseTrialOptions options, SkseTrialRuntime runtime,
                                                  SkseTrialDependency dependency) {
    return run_trial(file, length, options, &runtime, &dependency);
}
