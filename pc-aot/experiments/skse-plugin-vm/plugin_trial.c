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

static uint32_t read_word(const uint8_t *memory) {
    return (uint32_t)memory[0] | (uint32_t)memory[1] << 8 |
           (uint32_t)memory[2] << 16 | (uint32_t)memory[3] << 24;
}

static void write_word(uint8_t *memory, uint32_t word) {
    for (unsigned i = 0; i < 4; ++i) memory[i] = (uint8_t)(word >> (8 * i));
}

static uint32_t scratch_base(const SksePeImage *image) {
    // These reservations are far enough apart that one validated PE image
    // cannot cover both. The call boundary also rejects overlaps.
    const uint32_t primary = UINT32_C(0x60000000);
    uint64_t image_end = (uint64_t)image->base + image->size;
    if (image_end > primary && image->base <= primary + UNAVAILABLE_CALLBACK_OFFSET)
        return UINT32_C(0x30000000);
    return primary;
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

static SkseTrialResult run_trial(const uint8_t *file, size_t length, SkseTrialOptions options,
                                  const SkseTrialRuntime *runtime) {
    SkseTrialResult result = {.status = SKSE_TRIAL_IMAGE_REFUSED};
    SksePeImage image = {0};
    if (runtime) {
        const SksePeImportBinding imports[] = {
            {"KERNEL32.dll", "GetTickCount", clock_token},
            {"msvcrt.dll", "memcpy", copy_token},
            {"msvcrt.dll", "memmove", move_token},
            {"msvcrt.dll", "memset", set_token}
        };
        size_t count = sizeof imports / sizeof imports[0];
        result.image_status = runtime->guest_base
            ? skse_pe_map_at_base(file, length, runtime->guest_base, imports, count, SKSE_PE_ALLOW_ENTRY, &image)
            : skse_pe_map_with_imports(file, length, imports, count, SKSE_PE_ALLOW_ENTRY, &image);
    } else {
        result.image_status = skse_pe_map(file, length, &image);
    }
    if (result.image_status != SKSE_PE_OK) return result;

    SkseX86Scratch scratch = {
        .base = scratch_base(&image), .bytes = calloc(SCRATCH_SIZE, 1), .length = SCRATCH_SIZE
    };
    if (!scratch.bytes) {
        result.status = SKSE_TRIAL_NO_MEMORY;
        skse_pe_free(&image);
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

    uint32_t entry_args[] = {image.base, 1, 0}; // Dynamic DLL_PROCESS_ATTACH.
    if (image.entry_va) {
        result.attach = skse_x86_call(&image, image.entry_va, scratch, entry_args, 3,
                                      CALL_BUDGET, services, SKSE_X86_STDCALL);
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
    result.query = skse_x86_call(&image, image.query_va, scratch, args, 2,
                                 CALL_BUDGET, services, SKSE_X86_CDECL);
    if (result.query.status != SKSE_X86_OK) {
        result.status = SKSE_TRIAL_QUERY_FAULT;
    } else if (!(result.query.eax & 0xff)) {
        result.status = SKSE_TRIAL_QUERY_DECLINED;
    } else if (!read_plugin_info(&image, scratch.bytes + INFO_OFFSET, &result)) {
        result.status = SKSE_TRIAL_INVALID_INFO;
    } else {
        result.load = skse_x86_call(&image, image.load_va, scratch, args, 1,
                                    CALL_BUDGET, services, SKSE_X86_CDECL);
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
            result.message = skse_x86_call(&image, core.listener, scratch, parameters, 1,
                                            CALL_BUDGET, services, SKSE_X86_CDECL);
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
        result.detach = skse_x86_call(&image, image.entry_va, scratch, entry_args, 3,
                                      CALL_BUDGET, services, SKSE_X86_STDCALL);
        if (result.detach.status != SKSE_X86_OK && result.status == SKSE_TRIAL_COMPLETED)
            result.status = SKSE_TRIAL_DETACH_FAULT;
    }
cleanup:
    result.unavailable_interface = core.unavailable_id;
    free(scratch.bytes);
    skse_pe_free(&image);
    return result;
}

SkseTrialResult skse_plugin_trial(const uint8_t *file, size_t length, SkseTrialOptions options) {
    return run_trial(file, length, options, NULL);
}

SkseTrialResult skse_plugin_trial_with_runtime(const uint8_t *file, size_t length,
                                               SkseTrialOptions options, SkseTrialRuntime runtime) {
    return run_trial(file, length, options, &runtime);
}
