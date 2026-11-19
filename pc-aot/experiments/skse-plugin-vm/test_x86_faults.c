/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_call.h"
#include <assert.h>
#include <string.h>

static uint8_t scratch[4096];
static const uint32_t image_base = 0x00100000;
static const uint32_t scratch_base = 0x00200000;

static SkseX86Result run(uint8_t *code, size_t length, uint32_t limit, SkseX86Services services) {
    SksePeImage image = {
        .base = image_base, .size = (uint32_t)length, .bytes = code, .section_count = 1,
        .sections = {{0, (uint32_t)length, SKSE_PE_READ | SKSE_PE_EXEC}}
    };
    memset(scratch, 0, sizeof scratch);
    return skse_x86_call(&image, image.base, (SkseX86Scratch){scratch_base, scratch, sizeof scratch},
                         NULL, 0, limit, services, SKSE_X86_CDECL);
}

#define ARRAY_RUN(code) run(code, sizeof code, 100, (SkseX86Services){0})

int main(void) {
    // CALL forwards to another x86 region and that callee returns into the caller.
    uint8_t nested[] = {0xe8, 1, 0, 0, 0, 0xc3, 0xb8, 42, 0, 0, 0, 0xc3};
    SkseX86Result result = ARRAY_RUN(nested);
    assert(result.status == SKSE_X86_OK && result.eax == 42 && result.instructions == 4);

    uint8_t overflow[] = {0xb8, 0xff, 0xff, 0xff, 0x7f, 0x83, 0xc0, 1, 0x70, 2, 0x31, 0xc0, 0xc3};
    result = ARRAY_RUN(overflow);
    assert(result.status == SKSE_X86_OK && result.eax == UINT32_C(0x80000000));

    uint8_t never_returns[] = {0xeb, 0xfe};
    result = ARRAY_RUN(never_returns);
    assert(result.status == SKSE_X86_INSTRUCTION_LIMIT && result.instructions == 100);

    uint8_t modifies_code[] = {0xc7, 0x05, 0, 0, 0x10, 0, 55, 0, 0, 0, 0xc3};
    assert(ARRAY_RUN(modifies_code).status == SKSE_X86_MEMORY_FAULT);
    assert(modifies_code[0] == 0xc7);

    uint8_t executes_data[] = {0xb8, 0x30, 0, 0x20, 0, 0xff, 0xe0};
    assert(ARRAY_RUN(executes_data).status == SKSE_X86_MEMORY_FAULT);

    uint8_t incomplete[] = {0xb8, 0, 0};
    assert(ARRAY_RUN(incomplete).status == SKSE_X86_MEMORY_FAULT);

    uint8_t unknown[] = {0xcc, 0xc3};
    assert(ARRAY_RUN(unknown).status == SKSE_X86_UNSUPPORTED_INSTRUCTION);

    // A zero repeat count doesn't address memory or change ECX, ESI or EDI.
    uint8_t empty_copy[] = {0x57, 0xbf, 0xfe, 0xff, 0xff, 0xff, 0x31, 0xc9, 0xf3, 0xaa, 0x5f, 0xc3};
    assert(ARRAY_RUN(empty_copy).status == SKSE_X86_OK);
    uint8_t copy_over_budget[] = {0xbf, 0, 0, 0x20, 0, 0xb9, 0xff, 0xff, 0xff, 0xff, 0xf3, 0xaa, 0xc3};
    assert(ARRAY_RUN(copy_over_budget).status == SKSE_X86_INSTRUCTION_LIMIT && scratch[0] == 0);
    uint8_t crosses_scratch[] = {0xbf, 0xff, 0x0f, 0x20, 0, 0xb9, 2, 0, 0, 0, 0xb0, 0x33, 0xf3, 0xaa, 0xc3};
    assert(ARRAY_RUN(crosses_scratch).status == SKSE_X86_MEMORY_FAULT && scratch[sizeof scratch - 1] == 0);
    uint8_t leaves_direction_flag[] = {0xfd, 0xc3};
    assert(ARRAY_RUN(leaves_direction_flag).status == SKSE_X86_BAD_CALL_RETURN);

    uint8_t clobbers_ebx[] = {0x31, 0xdb, 0xc3};
    assert(ARRAY_RUN(clobbers_ebx).status == SKSE_X86_BAD_CALL_RETURN);

    uint8_t fake_return[] = {0x6a, 0, 0xc3};
    assert(ARRAY_RUN(fake_return).status == SKSE_X86_BAD_CALL_RETURN);

    uint8_t stdcall_return[] = {0xc2, 8, 0};
    assert(ARRAY_RUN(stdcall_return).status == SKSE_X86_BAD_CALL_RETURN);
    SksePeImage stdcall_image = {
        .base = image_base, .size = sizeof stdcall_return, .bytes = stdcall_return, .section_count = 1,
        .sections = {{0, sizeof stdcall_return, SKSE_PE_READ | SKSE_PE_EXEC}}
    };
    const uint32_t stdcall_args[] = {12, 34};
    result = skse_x86_call(&stdcall_image, image_base, (SkseX86Scratch){scratch_base, scratch, sizeof scratch},
                           stdcall_args, 2, 100, (SkseX86Services){0}, SKSE_X86_STDCALL);
    assert(result.status == SKSE_X86_OK);

    const SkseX86Service service = {0x00300000, 0x1234, SKSE_X86_SCALAR_RETURN};
    SkseX86Services services = {&service, 1};
    uint8_t calls_service[] = {0xb8, 0, 0, 0x30, 0, 0xff, 0xd0, 0xc3};
    result = run(calls_service, sizeof calls_service, 100, services);
    assert(result.status == SKSE_X86_OK && result.eax == 0x1234 && result.service_calls[0] == 1);
    calls_service[6] = 0xe0; // JMP cannot invoke a service token.
    result = run(calls_service, sizeof calls_service, 100, services);
    assert(result.status == SKSE_X86_MEMORY_FAULT && result.service_calls[0] == 0);

    const SkseX86Service aliases_code = {image_base, 0x1234, SKSE_X86_SCALAR_RETURN};
    assert(run(calls_service, sizeof calls_service, 100, (SkseX86Services){&aliases_code, 1}).status ==
           SKSE_X86_BAD_ARGUMENT);
    return 0;
}
