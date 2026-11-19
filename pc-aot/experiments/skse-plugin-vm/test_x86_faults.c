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

static void append_word(uint8_t *code, size_t *used, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) code[(*used)++] = (uint8_t)(value >> (8 * i));
}

static void check_alu(unsigned width, unsigned op, unsigned carry_in, uint32_t a, uint32_t b) {
    uint8_t code[64];
    size_t used = 0;
    code[used++] = 0xb8; // MOV EAX, a
    append_word(code, &used, a);
    code[used++] = 0xb9; // MOV ECX, b
    append_word(code, &used, b);
    code[used++] = carry_in ? 0xf9 : 0xf8;
    if (width == 2) code[used++] = 0x66;
    static const uint8_t opcodes[] = {0x00, 0x10, 0x28, 0x18}; // ADD, ADC, SUB, SBB
    code[used++] = opcodes[op] + (width != 1);
    code[used++] = 0xc8; // Destination EAX or a sub-register, source ECX.
    code[used++] = 0xa3; // MOV [scratch], EAX
    append_word(code, &used, scratch_base);
    static const uint8_t setcc[] = {0x92, 0x90, 0x94, 0x98, 0x9a}; // CF, OF, ZF, SF, PF
    for (unsigned i = 0; i < sizeof setcc; ++i) {
        code[used++] = 0x0f;
        code[used++] = setcc[i];
        code[used++] = 0x05;
        append_word(code, &used, scratch_base + 4 + i);
    }
    code[used++] = 0xc3;
    assert(used <= sizeof code);
    SkseX86Result result = run(code, used, 100, (SkseX86Services){0});
    assert(result.status == SKSE_X86_OK);

    const int64_t modulus = INT64_C(1) << (8 * width);
    const int64_t sign = modulus / 2;
    const int64_t carry = (op & 1) ? carry_in : 0;
    const int64_t left = a >= sign ? (int64_t)a - modulus : (int64_t)a;
    const int64_t right = (b >= sign ? (int64_t)b - modulus : (int64_t)b) + carry;
    const int64_t signed_value = op >= 2 ? left - right : left + right;
    const int64_t unsigned_value = op >= 2 ? (int64_t)a - b - carry : (int64_t)a + b + carry;
    const uint32_t value = (uint32_t)unsigned_value & (uint32_t)(modulus - 1);
    unsigned parity = 0;
    for (unsigned i = 0; i < 8; ++i) parity += (value >> i) & 1;
    assert((result.eax & (uint32_t)(modulus - 1)) == value);
    assert(scratch[4] == (unsigned_value < 0 || unsigned_value >= modulus));
    assert(scratch[5] == (signed_value < -sign || signed_value >= sign));
    assert(scratch[6] == (value == 0));
    assert(scratch[7] == (value >= sign));
    assert(scratch[8] == !(parity & 1));
}

static void check_arithmetic_flags(void) {
    for (unsigned op = 0; op < 4; ++op) {
        for (unsigned carry = 0; carry < 2; ++carry) {
            for (unsigned a = 0; a < 256; ++a)
                for (unsigned b = 0; b < 256; ++b) check_alu(1, op, carry, a, b);
            uint32_t rng = 0x731edaff;
            for (unsigned i = 0; i < 2048; ++i) {
                uint32_t a = rng = rng * 1664525 + 1013904223;
                uint32_t b = rng = rng * 1664525 + 1013904223;
                check_alu(2, op, carry, a & 0xffff, b & 0xffff);
                check_alu(4, op, carry, a, b);
            }
        }
    }
}

static void check_bounded_heap(void) {
    SkseX86Heap heap = {.base = 0x00300000};
    assert(!heap.bytes && !skse_x86_heap_allocate(&heap, SKSE_X86_HEAP_BYTES + 1, false));
    const uint32_t original = skse_x86_heap_allocate(&heap, 3, false);
    assert(original == heap.base && heap.bytes);
    uint8_t *data = skse_x86_heap_memory(&heap, original, 3);
    assert(data && !skse_x86_heap_memory(&heap, original, 4) && !skse_x86_heap_memory(&heap, original + 3, 1));
    memcpy(data, "abc", 3);
    const uint32_t blocker = skse_x86_heap_allocate(&heap, 64, false);
    assert(blocker == original + 16);
    assert(!skse_x86_heap_resize(&heap, original, 48, true, true));
    assert(skse_x86_heap_memory(&heap, original, 3) && !skse_x86_heap_release(&heap, original + 1));
    uint32_t grown = skse_x86_heap_resize(&heap, original, 48, true, false);
    assert(grown != original && grown > blocker);
    assert(!skse_x86_heap_memory(&heap, original, 1));
    data = skse_x86_heap_memory(&heap, grown, 48);
    assert(data && !memcmp(data, "abc", 3));
    for (unsigned i = 3; i < 48; ++i) assert(data[i] == 0);
    assert(skse_x86_heap_release(&heap, blocker) && !skse_x86_heap_release(&heap, blocker));
    assert(skse_x86_heap_resize(&heap, grown, 64, true, true) == grown);
    assert(skse_x86_heap_release(&heap, grown));
    assert(skse_x86_heap_allocate(&heap, SKSE_X86_HEAP_BYTES, true) == original);
    assert(!skse_x86_heap_allocate(&heap, 1, false));
    skse_x86_heap_close(&heap);
    assert(!heap.bytes && !heap.count);
}

static void check_virtual_tls(void) {
    SkseWinThreadProcess process = {0};
    SkseWinThread thread = {.process = &process, .id = 3, .last_error = 0x1234};
    SkseWinThread worker = {.process = &process, .id = 4, .last_error = 0x4321};
    for (unsigned i = 0; i < SKSE_WIN_TLS_SLOTS; ++i)
        assert(skse_win_thread_call(&thread, SKSE_WIN_TLS_ALLOC, 0, 0) == i);
    assert(thread.last_error == 0x1234);
    assert(skse_win_thread_call(&thread, SKSE_WIN_TLS_ALLOC, 0, 0) == UINT32_MAX && thread.last_error == 8);
    assert(skse_win_thread_call(&thread, SKSE_WIN_TLS_GET, 109, 0) == 0 && thread.last_error == 0);
    assert(skse_win_thread_call(&thread, SKSE_WIN_SET_LAST_ERROR, 44, 0) == 0);
    assert(skse_win_thread_call(&thread, SKSE_WIN_TLS_SET, 109, 0x12345678) && thread.last_error == 44);
    assert(skse_win_thread_call(&worker, SKSE_WIN_TLS_GET, 109, 0) == 0 && worker.last_error == 0);
    assert(skse_win_thread_call(&worker, SKSE_WIN_TLS_SET, 109, 0x3400));
    assert(skse_win_thread_call(&worker, SKSE_WIN_SET_LAST_ERROR, 77, 0) == 0);
    assert(thread.last_error == 44);
    assert(skse_win_thread_call(&thread, SKSE_WIN_TLS_GET, 109, 0) == 0x12345678 && thread.last_error == 0);
    assert(worker.last_error == 77 && skse_win_thread_call(&worker, SKSE_WIN_TLS_GET, 109, 0) == 0x3400);
    assert(skse_win_thread_call(&thread, SKSE_WIN_TLS_FREE, 109, 0));
    assert(skse_win_thread_call(&thread, SKSE_WIN_TLS_GET, UINT32_MAX, 0) == 0 && thread.last_error == 87);
    assert(skse_win_thread_call(&thread, SKSE_WIN_TLS_ALLOC, 0, 0) == 109);
    assert(skse_win_thread_call(&thread, SKSE_WIN_TLS_GET, 109, 0) == 0 && thread.last_error == 0);
    assert(skse_win_thread_call(&worker, SKSE_WIN_TLS_GET, 109, 0) == 0 && worker.last_error == 0);
    assert(skse_win_thread_call(&thread, SKSE_WIN_CURRENT_THREAD_ID, 0, 0) == 3);
}

static void check_virtual_critical_sections(void) {
    SkseWinThreadProcess process = {0};
    SkseWinThread thread = {.process = &process, .id = 1, .last_error = 42};
    SkseWinThread worker = {.process = &process, .id = 2, .last_error = 66};
    const uint32_t base = 0x1234, size = SKSE_WIN_CRITICAL_SECTION_BYTES;
    for (unsigned i = 0; i < SKSE_WIN_CRITICAL_SECTIONS; ++i)
        assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_SPIN_INIT, base + i * size) == SKSE_WIN_CRITICAL_ACQUIRED);
    assert(thread.last_error == 42);
    assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_SPIN_INIT, base + SKSE_WIN_CRITICAL_SECTIONS * size) ==
           SKSE_WIN_CRITICAL_INVALID);
    assert(thread.last_error == 8);
    assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_ENTER, base) == SKSE_WIN_CRITICAL_ACQUIRED);
    assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_TRY, base) == SKSE_WIN_CRITICAL_ACQUIRED);
    assert(skse_win_critical_section(&worker, SKSE_WIN_CRITICAL_TRY, base) == SKSE_WIN_CRITICAL_CONTENDED);
    assert(worker.last_error == 66);
    assert(skse_win_critical_section(&worker, SKSE_WIN_CRITICAL_LEAVE, base) == SKSE_WIN_CRITICAL_INVALID);
    assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_LEAVE, base) == SKSE_WIN_CRITICAL_ACQUIRED);
    assert(skse_win_critical_section(&worker, SKSE_WIN_CRITICAL_ENTER, base) == SKSE_WIN_CRITICAL_CONTENDED);
    assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_DELETE, base) == SKSE_WIN_CRITICAL_INVALID);
    assert(thread.last_error == 87);
    assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_LEAVE, base) == SKSE_WIN_CRITICAL_ACQUIRED);
    assert(skse_win_critical_section(&worker, SKSE_WIN_CRITICAL_ENTER, base) == SKSE_WIN_CRITICAL_ACQUIRED);
    assert(skse_win_critical_section(&worker, SKSE_WIN_CRITICAL_LEAVE, base) == SKSE_WIN_CRITICAL_ACQUIRED);
    assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_DELETE, base) == SKSE_WIN_CRITICAL_ACQUIRED);
    assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_ENTER, base) == SKSE_WIN_CRITICAL_INVALID);
    assert(skse_win_critical_section(&thread, SKSE_WIN_CRITICAL_INIT, base) == SKSE_WIN_CRITICAL_ACQUIRED);
}

static void check_static_tls_address(void) {
    SkseX86Services tls = {.static_tls_table = scratch_base + 16, .stack_floor = scratch_base + 256};
    uint8_t direct[] = {0x64, 0xa1, 0x2c, 0, 0, 0, 0xc3};
    uint8_t indirect[] = {0x64, 0x8b, 0x0d, 0x2c, 0, 0, 0, 0x89, 0xc8, 0xc3};
    assert(ARRAY_RUN(direct).status == SKSE_X86_UNSUPPORTED_INSTRUCTION);
    SkseX86Result result = run(direct, sizeof direct, 100, tls);
    assert(result.status == SKSE_X86_OK && result.eax == tls.static_tls_table && result.work == 2);
    result = run(indirect, sizeof indirect, 100, tls);
    assert(result.status == SKSE_X86_OK && result.eax == tls.static_tls_table);
    direct[2] = 0x18;
    assert(run(direct, sizeof direct, 100, tls).status == SKSE_X86_UNSUPPORTED_INSTRUCTION);
    uint8_t store[] = {0x64, 0xa3, 0x2c, 0, 0, 0, 0xc3};
    assert(run(store, sizeof store, 100, tls).status == SKSE_X86_UNSUPPORTED_INSTRUCTION);
    uint8_t underflow[] = {0xbc, 0, 1, 0x20, 0, 0x50, 0xc3};
    assert(run(underflow, sizeof underflow, 100, tls).status == SKSE_X86_MEMORY_FAULT);
    tls.static_tls_table = tls.stack_floor;
    assert(run(indirect, sizeof indirect, 100, tls).status == SKSE_X86_BAD_ARGUMENT);
}

static void check_cached_image_bounds(void) {
    uint8_t bytes[128] = {0};
    // Read one word from a read-only region, then try writing through the
    // populated data cache. The second operation must still see permissions.
    const uint8_t read_then_write[] = {
        0xa1, 0x40, 0, 0x10, 0, 0xa3, 0x40, 0, 0x10, 0, 0xc3
    };
    memcpy(bytes, read_then_write, sizeof read_then_write);
    bytes[0x40] = 91;
    SksePeImage image = {
        .base = image_base, .size = sizeof bytes, .bytes = bytes, .section_count = 2,
        .sections = {{0, 0x20, SKSE_PE_READ | SKSE_PE_EXEC}, {0x40, 8, SKSE_PE_READ}}
    };
    SkseX86Scratch work = {scratch_base, scratch, sizeof scratch};
    SkseX86Result result = skse_x86_call(&image, image_base, work, NULL, 0, 100,
                                        (SkseX86Services){0}, SKSE_X86_CDECL);
    assert(result.status == SKSE_X86_MEMORY_FAULT && bytes[0x40] == 91);
    // Replace the write with a read that crosses this same region's end.
    bytes[5] = 0xa1;
    bytes[6] = 0x46;
    result = skse_x86_call(&image, image_base, work, NULL, 0, 100,
                          (SkseX86Services){0}, SKSE_X86_CDECL);
    assert(result.status == SKSE_X86_MEMORY_FAULT);

    // A failed comparison still requires write permission on the target.
    const uint8_t compare[] = {0x31, 0xc0, 0xf0, 0x0f, 0xb1, 0x0d, 0x40, 0, 0x10, 0, 0xc3};
    memcpy(bytes, compare, sizeof compare);
    result = skse_x86_call(&image, image_base, work, NULL, 0, 100, (SkseX86Services){0}, SKSE_X86_CDECL);
    assert(result.status == SKSE_X86_MEMORY_FAULT && bytes[0x40] == 91);
    uint8_t register_lock[] = {0xf0, 0x0f, 0xc1, 0xc9, 0xc3};
    assert(ARRAY_RUN(register_lock).status == SKSE_X86_UNSUPPORTED_INSTRUCTION);
}

static void check_services_need_live_state(void) {
    SkseWinSync events = {0};
    SkseWinThread thread = {.id = 1};
    uint32_t token = skse_win_event_create(&events, false, false);
    const SkseX86Service binding = {0x00401000, SKSE_WIN_WAIT_ONE, SKSE_X86_WIN_SYNC};
    SkseX86Services services = {.entries = &binding, .count = 1, .sync = &events, .thread = &thread};
    uint8_t wait[18] = {0};
    size_t length = 0;
    wait[length++] = 0x68;
    append_word(wait, &length, UINT32_MAX);
    wait[length++] = 0x68;
    append_word(wait, &length, token);
    wait[length++] = 0xb8;
    append_word(wait, &length, binding.address);
    wait[length++] = 0xff;
    wait[length++] = 0xd0;
    wait[length++] = 0xc3;
    assert(run(wait, length, 50, services).status == SKSE_X86_UNSUPPORTED_API);
    assert(skse_win_event_signal(&events, token, true));
    SkseX86Result result = run(wait, length, 50, services);
    assert(result.status == SKSE_X86_OK && result.eax == 0);

    SkseX86Serialization serial = {.interface_address = scratch_base + 64, .owner_handle = 12};
    const SkseX86Service resolver = {0x00401000, SKSE_SERIAL_RESOLVE_FORM, SKSE_X86_SERIALIZATION};
    services = (SkseX86Services){.entries = &resolver, .count = 1, .serialization = &serial};
    assert(run(wait + 10, length - 10, 50, services).status == SKSE_X86_UNSUPPORTED_API);
    assert(skse_x86_records_count(&serial.records) == 0);
}

static void check_service_index(void) {
    // These two validated tokens have the same initial bucket.
    SkseX86Service bindings[] = {
        {0x00400004, 11, SKSE_X86_SCALAR_RETURN},
        {0x004004c8, 22, SKSE_X86_SCALAR_RETURN}
    };
    SkseX86Services services = {.entries = bindings, .count = 2};
    uint8_t call[] = {0xb8, 0xc8, 0x04, 0x40, 0, 0xff, 0xd0, 0xc3};
    SkseX86Result result = run(call, sizeof call, 20, services);
    assert(result.status == SKSE_X86_OK && result.eax == 22 && result.service_calls[1] == 1);
    bindings[1].address = bindings[0].address;
    assert(run(call, sizeof call, 20, services).status == SKSE_X86_BAD_ARGUMENT);
}

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

    // Aligned SSE encodings require alignment even inside allowed scratch.
    uint8_t unaligned_movaps[] = {0x0f, 0x28, 0x05, 1, 0, 0x20, 0, 0xc3};
    assert(ARRAY_RUN(unaligned_movaps).status == SKSE_X86_MEMORY_FAULT);
    uint8_t unaligned_movups[] = {0x0f, 0x10, 0x05, 1, 0, 0x20, 0, 0xc3};
    assert(ARRAY_RUN(unaligned_movups).status == SKSE_X86_OK);
    uint8_t truncated_movups[] = {0x0f, 0x10, 0x05, 0xf8, 0xf, 0x20, 0, 0xc3};
    assert(ARRAY_RUN(truncated_movups).status == SKSE_X86_MEMORY_FAULT);
    uint8_t vector_to_code[] = {0xf3, 0x0f, 0x7f, 0x05, 0, 0, 0x10, 0, 0xc3};
    assert(ARRAY_RUN(vector_to_code).status == SKSE_X86_MEMORY_FAULT && vector_to_code[0] == 0xf3);

    uint8_t divide_by_zero[] = {0x31, 0xc9, 0xf7, 0xf1, 0xc3};
    assert(ARRAY_RUN(divide_by_zero).status == SKSE_X86_DIVIDE_FAULT);
    // EDX:EAX = INT64_MIN / -1 would overflow even native int64 division.
    uint8_t divide_overflow[] = {0xba, 0, 0, 0, 0x80, 0x31, 0xc0, 0x83, 0xc9, 0xff, 0xf7, 0xf9, 0xc3};
    assert(ARRAY_RUN(divide_overflow).status == SKSE_X86_DIVIDE_FAULT);
    uint8_t byte_divide[] = {0x66, 0xb8, 0xf1, 0xff, 0xb1, 3, 0xf6, 0xf9, 0xc3};
    result = ARRAY_RUN(byte_divide);
    assert(result.status == SKSE_X86_OK && result.eax == 0xfb);

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
    SkseX86Services services = {.entries = &service, .count = 1};
    uint8_t calls_service[] = {0xb8, 0, 0, 0x30, 0, 0xff, 0xd0, 0xc3};
    result = run(calls_service, sizeof calls_service, 100, services);
    assert(result.status == SKSE_X86_OK && result.eax == 0x1234 && result.service_calls[0] == 1);
    calls_service[6] = 0xe0; // The import thunk returns through the original caller.
    result = run(calls_service, sizeof calls_service, 100, services);
    assert(result.status == SKSE_X86_OK && result.eax == 0x1234 && result.service_calls[0] == 1);

    const SkseX86Service win_service = {0x00300000, 0, SKSE_X86_HEAP_ALLOC};
    SkseX86Heap win_heap = {.base = 0x00400000};
    SksePeImage thunk_image = {
        .base = image_base, .size = sizeof calls_service, .bytes = calls_service, .section_count = 1,
        .sections = {{0, sizeof calls_service, SKSE_PE_READ | SKSE_PE_EXEC}}
    };
    const uint32_t heap_args[] = {SKSE_X86_PROCESS_HEAP, 8, 32};
    SkseX86Services win_services = {.entries = &win_service, .count = 1, .heap = &win_heap};
    result = skse_x86_call(&thunk_image, image_base, (SkseX86Scratch){scratch_base, scratch, sizeof scratch},
                           heap_args, 3, 65535, win_services, SKSE_X86_STDCALL);
    assert(result.status == SKSE_X86_OK && result.eax == win_heap.base && result.service_calls[0] == 1);
    skse_x86_heap_close(&win_heap);

    const SkseX86Service aliases_code = {image_base, 0x1234, SKSE_X86_SCALAR_RETURN};
    assert(run(calls_service, sizeof calls_service, 100, (SkseX86Services){.entries = &aliases_code, .count = 1}).status ==
           SKSE_X86_BAD_ARGUMENT);
    check_arithmetic_flags();
    check_bounded_heap();
    check_virtual_tls();
    check_virtual_critical_sections();
    check_static_tls_address();
    check_cached_image_bounds();
    check_services_need_live_state();
    check_service_index();
    return 0;
}
