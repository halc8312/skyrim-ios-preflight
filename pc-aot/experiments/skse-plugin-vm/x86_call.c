/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_call.h"
#include <stdbool.h>
#include <string.h>

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };
enum { CF = 1, PF = 4, ZF = 0x40, SF = 0x80, DF = 0x400, OF = 0x800 };
enum { ADD, OR, ADC, SBB, AND, SUB, XOR, CMP };
enum { MAX_SCRATCH = 256 * 1024, MAX_ARGUMENTS = 16, MAX_STEPS = 1000000 };

typedef struct {
    SksePeImage *const *images;
    size_t image_count;
    SksePeImage *recent_image;
    SkseX86Scratch scratch;
    SkseX86Services services;
    SkseX86Result result;
    uint32_t registers[8], ip, flags, return_slot;
    uint8_t xmm[8][16];
    uint32_t return_cleanup, limit, bulk_work, stack_floor;
    uint64_t code_start, code_end;
    const uint8_t *code_bytes;
    bool finished;
} Vm;

typedef struct {
    bool is_register;
    uint32_t value;
} Operand;

static uint32_t little_endian(const uint8_t *data, unsigned width) {
    uint32_t word = 0;
    for (unsigned i = 0; i < width; ++i)
        word |= (uint32_t)data[i] << (8 * i);
    return word;
}

static uint32_t width_mask(unsigned width) {
    return UINT32_MAX >> (32 - 8 * width);
}

static uint32_t sign_mask(unsigned width) {
    return UINT32_C(1) << (8 * width - 1);
}

static uint32_t sign_extend(uint32_t value, unsigned width) {
    uint32_t sign = sign_mask(width);
    return ((value & width_mask(width)) ^ sign) - sign;
}

static int64_t signed_word(uint32_t value, unsigned width) {
    value &= width_mask(width);
    return (value & sign_mask(width)) ? (int64_t)value - (INT64_C(1) << (8 * width)) : value;
}

static bool in_range(uint64_t start, uint64_t end, uint32_t address, size_t count) {
    return address >= start && address < end && count <= end - address;
}

static bool in_image(const SksePeImage *image, uint32_t address) {
    return address >= image->base && address - image->base < image->size;
}

static SksePeImage *image_at(Vm *vm, uint32_t address) {
    if (in_image(vm->recent_image, address)) return vm->recent_image;
    for (size_t i = 0; i < vm->image_count; ++i) {
        SksePeImage *image = vm->images[i];
        if (in_image(image, address)) {
            vm->recent_image = image;
            return image;
        }
    }
    return NULL;
}

static void fail(Vm *vm, SkseX86Status status) {
    if (vm->result.status == SKSE_X86_OK) vm->result.status = status;
}

static uint32_t fetch(Vm *vm, unsigned width) {
    if (vm->result.status != SKSE_X86_OK) return 0;
    if (!in_range(vm->code_start, vm->code_end, vm->ip, width)) {
        const SksePeImage *image = image_at(vm, vm->ip);
        if (!skse_pe_allows(image, vm->ip, width, SKSE_PE_EXEC)) {
            fail(vm, SKSE_X86_MEMORY_FAULT);
            return 0;
        }
        // Cache the current immutable code section so subsequent bytes skip
        // section lookup. The loader rejects sections that are writable code.
        uint32_t rva = vm->ip - image->base;
        for (uint16_t i = 0; i < image->section_count; ++i) {
            const SksePeSection *section = &image->sections[i];
            if ((section->permissions & SKSE_PE_EXEC) && rva >= section->rva &&
                rva - section->rva < section->length) {
                vm->code_start = (uint64_t)image->base + section->rva;
                vm->code_end = vm->code_start + section->length;
                vm->code_bytes = image->bytes + section->rva;
                break;
            }
        }
    }
    uint32_t value = little_endian(vm->code_bytes + (vm->ip - vm->code_start), width);
    vm->ip += width;
    return value;
}

static uint8_t *memory(Vm *vm, uint32_t address, size_t width, unsigned permissions) {
    if (vm->result.status != SKSE_X86_OK) return NULL;
    if (in_range(vm->scratch.base, (uint64_t)vm->scratch.base + vm->scratch.length, address, width))
        return vm->scratch.bytes + (address - vm->scratch.base);
    SkseX86Heap *heap = vm->services.heap;
    if (heap && heap->bytes && address >= heap->base && address - heap->base < SKSE_X86_HEAP_BYTES) {
        uint8_t *range = skse_x86_heap_memory(heap, address, width);
        if (range) return range;
        fail(vm, SKSE_X86_MEMORY_FAULT);
        return NULL;
    }
    SksePeImage *image = image_at(vm, address);
    if (skse_pe_allows(image, address, width, permissions))
        return image->bytes + (address - image->base);
    fail(vm, SKSE_X86_MEMORY_FAULT);
    return NULL;
}

static uint32_t read_memory(Vm *vm, uint32_t address, unsigned width) {
    const uint8_t *bytes = memory(vm, address, width, SKSE_PE_READ);
    return bytes ? little_endian(bytes, width) : 0;
}

static void write_memory(Vm *vm, uint32_t address, unsigned width, uint32_t value) {
    uint8_t *bytes = memory(vm, address, width, SKSE_PE_WRITE);
    if (bytes) {
        for (unsigned i = 0; i < width; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
    }
}

static Operand reg(unsigned index) { return (Operand){true, index}; }

static uint32_t read_operand(Vm *vm, Operand operand, unsigned width) {
    if (!operand.is_register) return read_memory(vm, operand.value, width);
    if (width == 1 && operand.value >= 4)
        return (vm->registers[operand.value - 4] >> 8) & 0xff;
    return vm->registers[operand.value] & width_mask(width);
}

static void write_operand(Vm *vm, Operand operand, unsigned width, uint32_t value) {
    if (!operand.is_register) {
        write_memory(vm, operand.value, width, value);
        return;
    }
    if (vm->result.status != SKSE_X86_OK) return;
    uint32_t mask = width_mask(width);
    unsigned index = operand.value;
    if (width == 1 && index >= 4) {
        index -= 4;
        mask <<= 8;
        value <<= 8;
    }
    vm->registers[index] = (vm->registers[index] & ~mask) | (value & mask);
}

static Operand decode_rm(Vm *vm, uint32_t modrm) {
    unsigned mode = modrm >> 6;
    unsigned base = modrm & 7;
    if (mode == 3) return reg(base);
    uint32_t address = 0;
    if (base == ESP) {
        uint32_t sib = fetch(vm, 1);
        unsigned index = (sib >> 3) & 7;
        base = sib & 7;
        if (index != ESP) address = vm->registers[index] << (sib >> 6);
    }
    if (mode == 0 && base == EBP)
        address += fetch(vm, 4);
    else
        address += vm->registers[base];
    if (mode == 1) address += sign_extend(fetch(vm, 1), 1);
    if (mode == 2) address += fetch(vm, 4);
    return (Operand){false, address};
}

static void push(Vm *vm, uint32_t value) {
    uint32_t address = vm->registers[ESP] - 4;
    if (!in_range(vm->stack_floor, (uint64_t)vm->scratch.base + vm->scratch.length, address, 4)) {
        fail(vm, SKSE_X86_MEMORY_FAULT);
        return;
    }
    write_memory(vm, address, 4, value);
    vm->registers[ESP] = address;
}

static uint32_t pop(Vm *vm) {
    uint32_t address = vm->registers[ESP];
    if (!in_range(vm->stack_floor, (uint64_t)vm->scratch.base + vm->scratch.length, address, 4)) {
        fail(vm, SKSE_X86_MEMORY_FAULT);
        return 0;
    }
    uint32_t value = read_memory(vm, address, 4);
    vm->registers[ESP] += 4;
    return value;
}

static void set_flags(Vm *vm, uint32_t value, unsigned width, bool carry, bool overflow) {
    value &= width_mask(width);
    unsigned parity_nibble = (value ^ (value >> 4)) & 15;
    bool even_parity = ((0x9669u >> parity_nibble) & 1) != 0;
    vm->flags = (vm->flags & DF) | (carry ? CF : 0) | (overflow ? OF : 0) |
                ((value & sign_mask(width)) ? SF : 0) | (value ? 0 : ZF) |
                (even_parity ? PF : 0);
}

static uint32_t alu(Vm *vm, unsigned op, unsigned width, uint32_t a, uint32_t b) {
    a &= width_mask(width);
    b &= width_mask(width);
    if (op == AND || op == OR || op == XOR) {
        uint32_t value = op == AND ? a & b : op == OR ? a | b : a ^ b;
        set_flags(vm, value, width, false, false);
        return value;
    }
    unsigned carry = (op == ADC || op == SBB) && (vm->flags & CF);
    bool subtract = op == SUB || op == SBB || op == CMP;
    uint64_t right = (uint64_t)b + carry;
    uint64_t wide = subtract ? a - right : a + right;
    // Overflow changes the sign of the left operand. For addition it
    // requires equal operand signs. For subtraction it requires different signs.
    uint32_t operand_signs = subtract ? a ^ b : ~(a ^ b);
    bool overflow = ((a ^ (uint32_t)wide) & operand_signs & sign_mask(width)) != 0;
    set_flags(vm, (uint32_t)wide, width, subtract ? a < right : wide > width_mask(width), overflow);
    return (uint32_t)wide;
}

static void update_operand(Vm *vm, unsigned op, Operand destination, unsigned width, uint32_t value) {
    uint32_t original = read_operand(vm, destination, width);
    uint32_t updated = alu(vm, op, width, original, value);
    if (op != CMP) write_operand(vm, destination, width, updated);
}

static void increment(Vm *vm, Operand destination, unsigned width, bool decrement) {
    uint32_t carry = vm->flags & CF;
    update_operand(vm, decrement ? SUB : ADD, destination, width, 1);
    vm->flags = (vm->flags & ~(uint32_t)CF) | carry;
}

static void multiply(Vm *vm, Operand destination, unsigned width, uint32_t a, uint32_t b) {
    int64_t product = signed_word(a, width) * signed_word(b, width);
    uint32_t value = (uint32_t)product & width_mask(width);
    vm->flags &= ~(uint32_t)(CF | OF);
    if (signed_word(value, width) != product) vm->flags |= CF | OF;
    write_operand(vm, destination, width, value);
}

static void shift_operand(Vm *vm, Operand destination, unsigned group, unsigned width, unsigned count) {
    count &= 31;
    if (!count) return;
    const unsigned bits = width * 8;
    const uint32_t value = read_operand(vm, destination, width);
    const uint32_t sign = sign_mask(width);
    const bool negative = (value & sign) != 0;
    uint32_t output;
    bool carry;
    bool overflow = (vm->flags & OF) != 0;
    if (group == 4) { // SHL
        output = value << count;
        carry = count <= bits && ((value >> (bits - count)) & 1);
        if (count == 1) overflow = ((output ^ value) & sign) != 0;
    } else {
        carry = count <= bits ? ((value >> (count - 1)) & 1) != 0 : group == 7 && negative;
        output = value >> count;
        if (group == 7 && negative) output |= UINT32_MAX << (count >= bits ? 0 : bits - count);
        if (count == 1) overflow = group == 5 && negative;
    }
    write_operand(vm, destination, width, output);
    set_flags(vm, output, width, carry, overflow);
}

static void write_double_width(Vm *vm, unsigned width, uint32_t low, uint32_t high) {
    if (width == 1) {
        write_operand(vm, reg(EAX), 2, (low & 255) | ((high & 255) << 8));
    } else {
        write_operand(vm, reg(EAX), width, low);
        write_operand(vm, reg(EDX), width, high);
    }
}

static void accumulator_multiply(Vm *vm, unsigned width, uint32_t value, bool is_signed) {
    const uint32_t a = read_operand(vm, reg(EAX), width);
    const int64_t product = is_signed ? signed_word(a, width) * signed_word(value, width) : 0;
    const uint64_t wide = is_signed ? (uint64_t)product : (uint64_t)a * value;
    const uint32_t low = (uint32_t)wide & width_mask(width);
    const uint32_t high = (uint32_t)(wide >> (width * 8)) & width_mask(width);
    const bool overflow = is_signed ? product != signed_word(low, width) : high != 0;
    vm->flags = (vm->flags & ~(uint32_t)(CF | OF)) | (overflow ? CF | OF : 0);
    write_double_width(vm, width, low, high);
}

static int64_t signed_pair(uint64_t raw, unsigned bits) {
    if (bits < 64) {
        uint64_t sign = UINT64_C(1) << (bits - 1);
        return (int64_t)(raw ^ sign) - (int64_t)sign;
    }
    return raw <= INT64_MAX ? (int64_t)raw : -(int64_t)~raw - 1;
}

static void accumulator_divide(Vm *vm, unsigned width, uint32_t divisor, bool is_signed) {
    if (vm->result.status != SKSE_X86_OK) return;
    if (!divisor) {
        fail(vm, SKSE_X86_DIVIDE_FAULT);
        return;
    }
    unsigned bits = width * 8;
    uint64_t raw = width == 1 ? read_operand(vm, reg(EAX), 2) : read_operand(vm, reg(EAX), width) |
                   (uint64_t)read_operand(vm, reg(EDX), width) << bits;
    if (is_signed) {
        int64_t dividend = signed_pair(raw, bits * 2);
        int64_t denom = signed_word(divisor, width);
        if (dividend == INT64_MIN && denom == -1) {
            fail(vm, SKSE_X86_DIVIDE_FAULT);
            return;
        }
        int64_t quotient = dividend / denom, remainder = dividend % denom;
        int64_t limit = sign_mask(width);
        if (quotient < -limit || quotient >= limit) {
            fail(vm, SKSE_X86_DIVIDE_FAULT);
            return;
        }
        write_double_width(vm, width, (uint32_t)quotient, (uint32_t)remainder);
    } else {
        uint64_t quotient = raw / divisor;
        if (quotient > width_mask(width)) {
            fail(vm, SKSE_X86_DIVIDE_FAULT);
            return;
        }
        write_double_width(vm, width, (uint32_t)quotient, (uint32_t)(raw % divisor));
    }
}

static bool condition(const Vm *vm, unsigned condition_code) {
    bool carry = vm->flags & CF, zero = vm->flags & ZF;
    bool sign = vm->flags & SF, overflow = vm->flags & OF;
    bool result;
    switch ((condition_code & 15) >> 1) {
    case 0: result = overflow; break;
    case 1: result = carry; break;
    case 2: result = zero; break;
    case 3: result = carry || zero; break;
    case 4: result = sign; break;
    case 5: result = (vm->flags & PF) != 0; break;
    case 6: result = sign != overflow; break;
    default: result = zero || sign != overflow; break;
    }
    return (condition_code & 1) ? !result : result;
}

static void return_from_call(Vm *vm, unsigned discard) {
    uint32_t slot = vm->registers[ESP];
    uint32_t target = pop(vm);
    if (vm->result.status != SKSE_X86_OK) return;
    if (target == 0) {
        // Only the outer call can consume the sentinel at its original slot.
        if (slot != vm->return_slot || discard != vm->return_cleanup)
            fail(vm, SKSE_X86_BAD_CALL_RETURN);
        else
            vm->finished = true;
        return;
    }
    vm->registers[ESP] += discard;
    vm->ip = target;
}

static bool reserve_bulk(Vm *vm, uint64_t cost) {
    if (cost > vm->limit - vm->result.instructions - vm->bulk_work) {
        fail(vm, SKSE_X86_INSTRUCTION_LIMIT);
        return false;
    }
    vm->bulk_work += (uint32_t)cost;
    return true;
}

static const uint8_t *service_parameters(Vm *vm, size_t size) {
    uint32_t stack = vm->registers[ESP];
    if (!in_range(vm->stack_floor, (uint64_t)vm->scratch.base + vm->scratch.length, stack, size)) {
        fail(vm, SKSE_X86_MEMORY_FAULT);
        return NULL;
    }
    return vm->scratch.bytes + (stack - vm->scratch.base);
}

static void memory_service(Vm *vm, SkseX86ServiceKind kind) {
    const uint8_t *parameters = service_parameters(vm, 12);
    if (!parameters) return;
    uint32_t destination = little_endian(parameters, 4);
    uint32_t source_or_value = little_endian(parameters + 4, 4);
    uint32_t count = little_endian(parameters + 8, 4);
    if (count != 0) {
        // Charge bulk operations as well as decoded instructions. A single
        // imported function call must not acquire an unbounded CPU budget.
        if (!reserve_bulk(vm, ((uint64_t)count + 63) / 64)) return;
        uint8_t *dst = memory(vm, destination, count, SKSE_PE_WRITE);
        if (!dst) return;
        if (kind == SKSE_X86_MEMSET) {
            memset(dst, (int)(uint8_t)source_or_value, count);
        } else {
            const uint8_t *src = memory(vm, source_or_value, count, SKSE_PE_READ);
            if (!src) return;
            if (kind == SKSE_X86_MEMCPY && (uint64_t)destination + count > source_or_value &&
                (uint64_t)source_or_value + count > destination) {
                fail(vm, SKSE_X86_BAD_ARGUMENT);
                return;
            }
            memmove(dst, src, count);
        }
    }
    vm->registers[EAX] = destination;
}

static void fill_string(uint8_t *dst, size_t length, unsigned width, uint32_t value) {
    if (width == 1) {
        memset(dst, (int)(uint8_t)value, length);
        return;
    }
    for (unsigned i = 0; i < width; ++i) dst[i] = (uint8_t)(value >> (8 * i));
    size_t written = width;
    while (written < length) {
        size_t next = length - written;
        if (next > written) next = written;
        memcpy(dst + written, dst, next);
        written += next;
    }
}

static void string_memory(Vm *vm, bool moving, bool repeat, unsigned width) {
    if (vm->result.status != SKSE_X86_OK) return;
    const uint32_t count = repeat ? vm->registers[ECX] : 1;
    if (!count) return;
    const bool backwards = (vm->flags & DF) != 0;
    const uint64_t bytes = (uint64_t)count * width;
    const uint64_t tail = backwards ? bytes - width : 0;
    const uint32_t edi = vm->registers[EDI], esi = vm->registers[ESI];
    if (tail > edi || (moving && tail > esi)) {
        fail(vm, SKSE_X86_MEMORY_FAULT);
        return;
    }
    const uint32_t destination = edi - (uint32_t)tail, source = esi - (uint32_t)tail;
    const bool overlaps = (uint64_t)destination + bytes > source && (uint64_t)source + bytes > destination;
    const bool overwrites = moving && overlaps && (backwards ? destination < source : destination > source);
    // A direction that overwrites unread source bytes must reproduce each
    // x86 store. Charge it per element and keep contiguous copies native.
    if (!reserve_bulk(vm, overwrites ? count : (bytes + 63) / 64)) return;
    // The budget above limits the native transfer to at most MAX_STEPS*64.
    uint8_t *dst = memory(vm, destination, (size_t)bytes, SKSE_PE_WRITE);
    if (!dst) return;
    if (moving) {
        const uint8_t *src = memory(vm, source, (size_t)bytes, SKSE_PE_READ);
        if (!src) return;
        if (overwrites) {
            for (uint32_t i = 0; i < count; ++i) {
                const size_t offset = (size_t)(backwards ? count - 1 - i : i) * width;
                memmove(dst + offset, src + offset, width);
            }
        } else {
            memmove(dst, src, (size_t)bytes);
        }
    } else {
        fill_string(dst, (size_t)bytes, width, vm->registers[EAX]);
    }
    const uint32_t delta = backwards ? 0u - (uint32_t)bytes : (uint32_t)bytes;
    vm->registers[EDI] += delta;
    if (moving) vm->registers[ESI] += delta;
    if (repeat) vm->registers[ECX] = 0;
}

static bool is_core_sender(Vm *vm, uint32_t name) {
    static const uint8_t expected[] = "skse";
    if (!name || name > UINT32_MAX - 4) return false;
    for (unsigned i = 0; i < 4; ++i)
        if ((read_memory(vm, name + i, 1) | 0x20) != expected[i]) return false;
    return read_memory(vm, name + 4, 1) == 0;
}

static uint32_t classic_service(Vm *vm, SkseX86ServiceKind kind) {
    SkseX86CoreMessaging *core = vm->services.core_messaging;
    if (kind == SKSE_X86_QUERY_CLASSIC) {
        const uint8_t *argument = service_parameters(vm, 4);
        if (!argument) return 0;
        uint32_t id = little_endian(argument, 4);
        if (id == SKSE_CLASSIC_MESSAGING_ID) return core->interface_address;
        if (id && !core->unavailable_id) core->unavailable_id = id;
        return 0;
    }

    const uint8_t *arguments = service_parameters(vm, 12);
    if (!arguments) return 0;
    const uint32_t handle = little_endian(arguments, 4), sender = little_endian(arguments + 4, 4);
    const uint32_t callback = little_endian(arguments + 8, 4);
    if (handle != core->owner_handle || !handle || handle == UINT32_MAX || !callback) return 0;
    if (!sender) {
        // Classic's wildcard selects other loaded plugins. The diagnostic
        // has none and therefore adds no callback for the SKSE sender.
        return 1;
    }
    if (!is_core_sender(vm, sender)) return 0;
    if (core->listener) return 1;
    if (!skse_pe_allows(image_at(vm, callback), callback, 1, SKSE_PE_EXEC)) {
        fail(vm, SKSE_X86_MEMORY_FAULT);
        return 0;
    }
    core->listener = callback;
    return 1;
}

static bool reserve_heap_work(Vm *vm, uint32_t size, bool resizing) {
    if (size > SKSE_X86_HEAP_BYTES) return false;
    uint64_t bytes = (uint64_t)size * (resizing ? 2u : 1u);
    if (!vm->services.heap->bytes) bytes += SKSE_X86_HEAP_BYTES;
    return reserve_bulk(vm, (bytes + 63) / 64);
}

static uint32_t crt_heap_service(Vm *vm, SkseX86ServiceKind kind) {
    bool multiple_arguments = kind == SKSE_X86_CALLOC || kind == SKSE_X86_REALLOC;
    const uint8_t *parameters = service_parameters(vm, multiple_arguments ? 8u : 4u);
    if (!parameters) return 0;
    SkseX86Heap *heap = vm->services.heap;
    const uint32_t first = little_endian(parameters, 4);
    uint32_t requested = first;
    if (kind == SKSE_X86_FREE) {
        if (first && !skse_x86_heap_release(heap, first)) fail(vm, SKSE_X86_BAD_ARGUMENT);
        return 0;
    }
    if (kind == SKSE_X86_REALLOC) {
        requested = little_endian(parameters + 4, 4);
        if (first) {
            if (!requested) {
                if (!skse_x86_heap_release(heap, first)) fail(vm, SKSE_X86_BAD_ARGUMENT);
                return 0;
            }
            if (!reserve_heap_work(vm, requested, true)) return 0;
            return skse_x86_heap_resize(heap, first, requested, false, false);
        }
    }
    if (kind == SKSE_X86_CALLOC) {
        const uint64_t product = (uint64_t)first * little_endian(parameters + 4, 4);
        if (product > SKSE_X86_HEAP_BYTES) return 0;
        requested = (uint32_t)product;
    }
    if (!reserve_heap_work(vm, requested, false)) return 0;
    return skse_x86_heap_allocate(heap, requested, kind == SKSE_X86_CALLOC);
}

static uint32_t win_heap_service(Vm *vm, SkseX86ServiceKind kind) {
    enum { ZERO_MEMORY = 8, IN_PLACE_ONLY = 0x10 };
    bool resizing = kind == SKSE_X86_HEAP_REALLOC;
    size_t bytes = resizing ? 16u : 12u;
    const uint8_t *parameters = service_parameters(vm, bytes);
    if (!parameters) return 0;
    // CALL to an emulated stdcall service returns here instead of pushing
    // a return address. Remove the arguments that the callee owns.
    vm->registers[ESP] += (uint32_t)bytes;
    if (little_endian(parameters, 4) != SKSE_X86_PROCESS_HEAP) return 0;
    const uint32_t flags = little_endian(parameters + 4, 4), value = little_endian(parameters + 8, 4);
    const uint32_t allowed = kind == SKSE_X86_HEAP_FREE ? 0u : ZERO_MEMORY | (resizing ? IN_PLACE_ONLY : 0u);
    if (flags & ~allowed) {
        fail(vm, SKSE_X86_UNSUPPORTED_API);
        return 0;
    }
    SkseX86Heap *heap = vm->services.heap;
    if (kind == SKSE_X86_HEAP_FREE) return skse_x86_heap_release(heap, value);
    uint32_t requested = resizing ? little_endian(parameters + 12, 4) : value;
    if (!reserve_heap_work(vm, requested, resizing)) return 0;
    return resizing ? skse_x86_heap_resize(heap, value, requested, (flags & ZERO_MEMORY) != 0, (flags & IN_PLACE_ONLY) != 0)
                    : skse_x86_heap_allocate(heap, requested, (flags & ZERO_MEMORY) != 0);
}

static uint32_t win_thread_service(Vm *vm, SkseWinThreadOperation operation) {
    unsigned count = skse_win_thread_argument_count(operation);
    uint32_t first = 0, second = 0;
    if (count) {
        const uint8_t *parameters = service_parameters(vm, 4 * count);
        if (!parameters) return 0;
        first = little_endian(parameters, 4);
        if (count == 2) second = little_endian(parameters + 4, 4);
        vm->registers[ESP] += 4 * count;
    }
    const bool critical = operation >= SKSE_WIN_CRITICAL_INIT;
    if (critical && (!first || (first & 3) ||
        !memory(vm, first, SKSE_WIN_CRITICAL_SECTION_BYTES, SKSE_PE_READ | SKSE_PE_WRITE))) {
        fail(vm, SKSE_X86_MEMORY_FAULT);
        return 0;
    }
    const uint32_t result = skse_win_thread_call(vm->services.thread, operation, first, second);
    if (critical && !result && operation != SKSE_WIN_CRITICAL_SPIN_INIT)
        fail(vm, SKSE_X86_BAD_ARGUMENT);
    return result;
}

static void branch(Vm *vm, uint32_t address, bool tail) {
    if (vm->result.status != SKSE_X86_OK) return;
    for (size_t i = 0; i < vm->services.count; ++i) {
        const SkseX86Service *service = &vm->services.entries[i];
        if (address == service->address) {
            uint32_t stack = vm->registers[ESP];
            if (tail) {
                // An indirect import thunk reuses the caller's return slot.
                if (!service_parameters(vm, 4)) return;
                vm->registers[ESP] += 4;
            }
            switch (service->kind) {
            case SKSE_X86_SCALAR_RETURN:
                vm->registers[EAX] = service->value;
                break;
            case SKSE_X86_MEMCPY:
            case SKSE_X86_MEMMOVE:
            case SKSE_X86_MEMSET:
                memory_service(vm, service->kind);
                break;
            case SKSE_X86_QUERY_CLASSIC:
            case SKSE_X86_REGISTER_CORE_LISTENER:
                vm->registers[EAX] = classic_service(vm, service->kind);
                break;
            case SKSE_X86_UNAVAILABLE:
                fail(vm, SKSE_X86_UNSUPPORTED_API);
                break;
            case SKSE_X86_MALLOC:
            case SKSE_X86_CALLOC:
            case SKSE_X86_REALLOC:
            case SKSE_X86_FREE:
                vm->registers[EAX] = crt_heap_service(vm, service->kind);
                break;
            case SKSE_X86_HEAP_ALLOC:
            case SKSE_X86_HEAP_REALLOC:
            case SKSE_X86_HEAP_FREE:
                vm->registers[EAX] = win_heap_service(vm, service->kind);
                break;
            case SKSE_X86_WIN_THREAD:
                vm->registers[EAX] = win_thread_service(vm, (SkseWinThreadOperation)service->value);
                break;
            }
            ++vm->result.service_calls[i];
            if (tail) {
                unsigned cleanup = vm->registers[ESP] - (stack + 4);
                vm->registers[ESP] = stack;
                if (vm->result.status == SKSE_X86_OK) return_from_call(vm, cleanup);
            }
            return;
        }
    }
    if (!tail) push(vm, vm->ip);
    vm->ip = address;
}

typedef enum {
    VECTOR_LOAD, VECTOR_STORE, VECTOR_FROM_GPR, VECTOR_TO_GPR,
    VECTOR_AND, VECTOR_OR, VECTOR_XOR, VECTOR_EQUAL, VECTOR_SIGN_BITS
} VectorOp;

static bool vector_instruction(Vm *vm, uint32_t opcode, unsigned prefix) {
    enum { OPERAND_SIZE = 0x66, REPEAT = 0xf3 };
    VectorOp operation;
    unsigned width = 16;
    unsigned lane = 0;
    bool aligned_access = false;
    bool zero_upper = false;
    switch (opcode) {
    case 0x10:
    case 0x11:
        operation = opcode == 0x10 ? VECTOR_LOAD : VECTOR_STORE;
        if (prefix == REPEAT) width = 4; // MOVSS
        break;
    case 0x28:
    case 0x29:
        if (prefix == REPEAT) return false;
        operation = opcode == 0x28 ? VECTOR_LOAD : VECTOR_STORE;
        aligned_access = true;
        break;
    case 0x6f:
    case 0x7f:
        if (!prefix) return false; // An unprefixed encoding addresses MMX.
        operation = opcode == 0x6f ? VECTOR_LOAD : VECTOR_STORE;
        aligned_access = prefix == OPERAND_SIZE;
        break;
    case 0x6e:
        if (prefix != OPERAND_SIZE) return false;
        operation = VECTOR_FROM_GPR;
        break;
    case 0x50:
    case 0xd7:
        if (prefix == REPEAT || (opcode == 0xd7 && prefix != OPERAND_SIZE)) return false;
        operation = VECTOR_SIGN_BITS;
        lane = opcode == 0xd7 ? 1 : prefix == OPERAND_SIZE ? 8 : 4;
        break;
    case 0x74:
    case 0x75:
    case 0x76:
        if (prefix != OPERAND_SIZE) return false;
        operation = VECTOR_EQUAL;
        lane = 1u << (opcode - 0x74);
        aligned_access = true;
        break;
    case 0x7e:
        if (!prefix) return false;
        operation = prefix == REPEAT ? VECTOR_LOAD : VECTOR_TO_GPR;
        width = 8;
        zero_upper = true;
        break;
    case 0xd6:
        if (prefix != OPERAND_SIZE) return false;
        operation = VECTOR_STORE;
        width = 8;
        zero_upper = true;
        break;
    case 0x54:
    case 0x56:
    case 0x57:
        if (prefix == REPEAT) return false;
        operation = opcode == 0x54 ? VECTOR_AND : opcode == 0x56 ? VECTOR_OR : VECTOR_XOR;
        aligned_access = true;
        break;
    case 0xdb:
    case 0xeb:
    case 0xef:
        if (prefix != OPERAND_SIZE) return false;
        operation = opcode == 0xdb ? VECTOR_AND : opcode == 0xeb ? VECTOR_OR : VECTOR_XOR;
        aligned_access = true;
        break;
    default:
        return false;
    }

    uint32_t modrm = fetch(vm, 1);
    Operand rm = decode_rm(vm, modrm);
    if (vm->result.status != SKSE_X86_OK) return true;
    if (aligned_access && !rm.is_register && (rm.value & 15)) {
        fail(vm, SKSE_X86_MEMORY_FAULT);
        return true;
    }
    uint8_t *xmm = vm->xmm[(modrm >> 3) & 7];
    if (operation == VECTOR_FROM_GPR) {
        uint32_t value = read_operand(vm, rm, 4);
        if (vm->result.status != SKSE_X86_OK) return true;
        memset(xmm, 0, 16);
        for (unsigned i = 0; i < 4; ++i) xmm[i] = (uint8_t)(value >> (i * 8));
        return true;
    }
    if (operation == VECTOR_TO_GPR) {
        write_operand(vm, rm, 4, little_endian(xmm, 4));
        return true;
    }
    if (operation == VECTOR_SIGN_BITS) {
        if (!rm.is_register) {
            fail(vm, SKSE_X86_UNSUPPORTED_INSTRUCTION);
            return true;
        }
        uint32_t value = 0;
        for (unsigned i = 0; i < 16 / lane; ++i) {
            unsigned most_significant = (i + 1) * lane - 1;
            value |= (uint32_t)(vm->xmm[rm.value][most_significant] >> 7) << i;
        }
        write_operand(vm, reg((modrm >> 3) & 7), 4, value);
        return true;
    }
    bool store = operation == VECTOR_STORE;
    uint8_t *other = rm.is_register ? vm->xmm[rm.value] :
        memory(vm, rm.value, width, store ? SKSE_PE_WRITE : SKSE_PE_READ);
    if (!other) return true;
    if (store) {
        memmove(other, xmm, width);
        if (zero_upper && rm.is_register) memset(other + width, 0, 16 - width);
    } else if (operation == VECTOR_LOAD) {
        memmove(xmm, other, width);
        if (zero_upper || (!rm.is_register && width == 4)) memset(xmm + width, 0, 16 - width);
    } else if (operation == VECTOR_EQUAL) {
        for (unsigned offset = 0; offset < 16; offset += lane) {
            bool equal = !memcmp(xmm + offset, other + offset, lane);
            memset(xmm + offset, equal ? 0xff : 0, lane);
        }
    } else {
        for (unsigned i = 0; i < 16; ++i) {
            if (operation == VECTOR_AND) xmm[i] &= other[i];
            else if (operation == VECTOR_OR) xmm[i] |= other[i];
            else xmm[i] ^= other[i];
        }
    }
    return true;
}

static void two_byte_instruction(Vm *vm, unsigned width, bool repeat) {
    uint32_t opcode = fetch(vm, 1);
    unsigned prefix = repeat ? 0xf3 : width == 2 ? 0x66 : 0;
    if (vector_instruction(vm, opcode, prefix)) return;
    if (repeat) {
        fail(vm, SKSE_X86_UNSUPPORTED_INSTRUCTION);
        return;
    }
    if (opcode >= 0x80 && opcode <= 0x8f) {
        if (width != 4) { fail(vm, SKSE_X86_UNSUPPORTED_INSTRUCTION); return; }
        uint32_t displacement = fetch(vm, 4);
        if (condition(vm, opcode)) vm->ip += displacement;
        return;
    }
    if ((opcode >= 0x90 && opcode <= 0x9f) || (opcode >= 0x40 && opcode <= 0x4f) ||
        opcode == 0xb6 || opcode == 0xb7 || opcode == 0xbe || opcode == 0xbf ||
        opcode == 0xaf || opcode == 0x1f) {
        uint32_t modrm = fetch(vm, 1);
        Operand source = decode_rm(vm, modrm);
        Operand destination = reg((modrm >> 3) & 7);
        if (opcode == 0x1f) {
            if ((modrm >> 3) & 7) fail(vm, SKSE_X86_UNSUPPORTED_INSTRUCTION);
            return;
        }
        if (opcode >= 0x90 && opcode <= 0x9f) {
            write_operand(vm, source, 1, condition(vm, opcode));
        } else if (opcode >= 0x40 && opcode <= 0x4f) {
            uint32_t value = read_operand(vm, source, width);
            if (condition(vm, opcode)) write_operand(vm, destination, width, value);
        } else if (opcode == 0xaf) {
            uint32_t a = read_operand(vm, destination, width);
            multiply(vm, destination, width, a, read_operand(vm, source, width));
        } else {
            unsigned size = opcode & 1 ? 2 : 1;
            uint32_t value = read_operand(vm, source, size);
            write_operand(vm, destination, width, (opcode & 8) ? sign_extend(value, size) : value);
        }
        return;
    }
    fail(vm, SKSE_X86_UNSUPPORTED_INSTRUCTION);
}

static void read_static_tls_table(Vm *vm) {
    // Both forms are emitted for the 32-bit compiler's TLS load. The segment
    // itself is never mapped. Only this documented ABI offset is observable.
    const uint32_t opcode = fetch(vm, 1);
    Operand destination = reg(EAX);
    uint32_t offset;
    if (opcode == 0xa1) {
        offset = fetch(vm, 4);
    } else if (opcode == 0x8b) {
        const uint32_t modrm = fetch(vm, 1);
        const Operand source = decode_rm(vm, modrm);
        if (source.is_register) {
            fail(vm, SKSE_X86_UNSUPPORTED_INSTRUCTION);
            return;
        }
        destination = reg((modrm >> 3) & 7);
        offset = source.value;
    } else {
        fail(vm, SKSE_X86_UNSUPPORTED_INSTRUCTION);
        return;
    }
    if (offset != 0x2c || !vm->services.static_tls_table) {
        fail(vm, SKSE_X86_UNSUPPORTED_INSTRUCTION);
        return;
    }
    write_operand(vm, destination, 4, vm->services.static_tls_table);
}

static void step(Vm *vm) {
    uint32_t opcode = fetch(vm, 1);
    if (opcode == 0x64) {
        read_static_tls_table(vm);
        return;
    }
    unsigned word = 4;
    bool repeat = opcode == 0xf3;
    if (repeat) opcode = fetch(vm, 1);
    if (opcode == 0x66) {
        word = 2;
        opcode = fetch(vm, 1);
        if (!repeat && opcode == 0xf3) { repeat = true; opcode = fetch(vm, 1); }
    }
    if (opcode == 0xa4 || opcode == 0xa5 || opcode == 0xaa || opcode == 0xab) {
        string_memory(vm, opcode < 0xaa, repeat, opcode & 1 ? word : 1);
        return;
    }
    if (opcode == 0x0f && (!repeat || word == 4)) {
        two_byte_instruction(vm, word, repeat);
        return;
    }
    if (repeat) {
        fail(vm, SKSE_X86_UNSUPPORTED_INSTRUCTION);
        return;
    }
    uint32_t form = opcode & 7;
    if (opcode <= 0x3d && form <= 5) {
        unsigned operation = opcode >> 3;
        unsigned width = form & 1 ? word : 1;
        if (form >= 4) {
            update_operand(vm, operation, reg(EAX), width, fetch(vm, width));
        } else {
            uint32_t modrm = fetch(vm, 1);
            Operand rm = decode_rm(vm, modrm), other = reg((modrm >> 3) & 7);
            Operand dst = form & 2 ? other : rm, src = form & 2 ? rm : other;
            update_operand(vm, operation, dst, width, read_operand(vm, src, width));
        }
        return;
    }
    if (opcode >= 0x40 && opcode <= 0x4f) {
        increment(vm, reg(form), word, (opcode & 8) != 0);
        return;
    }
    if (word == 4 && opcode >= 0x50 && opcode <= 0x57) { push(vm, vm->registers[form]); return; }
    if (word == 4 && opcode >= 0x58 && opcode <= 0x5f) {
        write_operand(vm, reg(form), 4, pop(vm));
        return;
    }
    if (opcode >= 0x70 && opcode <= 0x7f) {
        if (word != 4) { fail(vm, SKSE_X86_UNSUPPORTED_INSTRUCTION); return; }
        uint32_t displacement = sign_extend(fetch(vm, 1), 1);
        if (condition(vm, opcode)) vm->ip += displacement;
        return;
    }
    if (opcode >= 0xb0 && opcode <= 0xbf) {
        unsigned width = opcode & 8 ? word : 1;
        write_operand(vm, reg(form), width, fetch(vm, width));
        return;
    }
    switch (opcode) {
    case 0x68:
    case 0x6a:
        if (word != 4) break;
        push(vm, opcode == 0x68 ? fetch(vm, 4) : sign_extend(fetch(vm, 1), 1));
        return;
    case 0x69:
    case 0x6b: {
        uint32_t modrm = fetch(vm, 1);
        Operand source = decode_rm(vm, modrm);
        uint32_t a = read_operand(vm, source, word);
        uint32_t b = opcode == 0x69 ? fetch(vm, word) : sign_extend(fetch(vm, 1), 1);
        multiply(vm, reg((modrm >> 3) & 7), word, a, b);
        return;
    }
    case 0x80:
    case 0x81:
    case 0x83: {
        uint32_t modrm = fetch(vm, 1);
        Operand destination = decode_rm(vm, modrm);
        unsigned width = opcode == 0x80 ? 1 : word;
        uint32_t value = opcode == 0x83 ? sign_extend(fetch(vm, 1), 1) : fetch(vm, width);
        update_operand(vm, (modrm >> 3) & 7, destination, width, value);
        return;
    }
    case 0x84:
    case 0x85: {
        uint32_t modrm = fetch(vm, 1);
        Operand rm = decode_rm(vm, modrm);
        unsigned width = opcode == 0x84 ? 1 : word;
        uint32_t left = read_operand(vm, rm, width);
        alu(vm, AND, width, left, read_operand(vm, reg((modrm >> 3) & 7), width));
        return;
    }
    case 0x88:
    case 0x89:
    case 0x8a:
    case 0x8b:
    case 0x8d: {
        uint32_t modrm = fetch(vm, 1);
        Operand rm = decode_rm(vm, modrm), other = reg((modrm >> 3) & 7);
        if (opcode == 0x8d) {
            if (rm.is_register) break;
            write_operand(vm, other, word, rm.value);
        } else {
            unsigned width = opcode & 1 ? word : 1;
            Operand dst = opcode & 2 ? other : rm, src = opcode & 2 ? rm : other;
            write_operand(vm, dst, width, read_operand(vm, src, width));
        }
        return;
    }
    case 0x90: return;
    case 0x98:
        write_operand(vm, reg(EAX), word, sign_extend(read_operand(vm, reg(EAX), word / 2), word / 2));
        return;
    case 0x99: {
        uint32_t a = read_operand(vm, reg(EAX), word);
        write_operand(vm, reg(EDX), word, (a & sign_mask(word)) ? UINT32_MAX : 0);
        return;
    }
    case 0xa0:
    case 0xa1:
    case 0xa2:
    case 0xa3: {
        Operand mem = {false, fetch(vm, 4)};
        unsigned width = opcode & 1 ? word : 1;
        if (opcode & 2) write_operand(vm, mem, width, read_operand(vm, reg(EAX), width));
        else write_operand(vm, reg(EAX), width, read_operand(vm, mem, width));
        return;
    }
    case 0xa8:
    case 0xa9: {
        unsigned width = opcode == 0xa8 ? 1 : word;
        uint32_t operand = fetch(vm, width);
        alu(vm, AND, width, read_operand(vm, reg(EAX), width), operand);
        return;
    }
    case 0xc0:
    case 0xc1:
    case 0xd0:
    case 0xd1:
    case 0xd2:
    case 0xd3: {
        uint32_t modrm = fetch(vm, 1);
        unsigned group = (modrm >> 3) & 7;
        if (group != 4 && group != 5 && group != 7) break;
        Operand destination = decode_rm(vm, modrm);
        unsigned count = opcode < 0xd0 ? fetch(vm, 1) : opcode < 0xd2 ? 1 : vm->registers[ECX];
        shift_operand(vm, destination, group, opcode & 1 ? word : 1, count);
        return;
    }
    case 0xc2:
    case 0xc3:
        if (word != 4) break;
        return_from_call(vm, opcode == 0xc2 ? fetch(vm, 2) : 0);
        return;
    case 0xc6:
    case 0xc7: {
        uint32_t modrm = fetch(vm, 1);
        if ((modrm >> 3) & 7) break;
        Operand destination = decode_rm(vm, modrm);
        unsigned width = opcode == 0xc6 ? 1 : word;
        write_operand(vm, destination, width, fetch(vm, width));
        return;
    }
    case 0xc9:
        if (word != 4) break;
        vm->registers[ESP] = vm->registers[EBP];
        write_operand(vm, reg(EBP), 4, pop(vm));
        return;
    case 0xe8:
    case 0xe9:
    case 0xeb: {
        if (word != 4) break;
        uint32_t offset = opcode == 0xeb ? sign_extend(fetch(vm, 1), 1) : fetch(vm, 4);
        if (opcode == 0xe8) branch(vm, vm->ip + offset, false);
        else vm->ip += offset;
        return;
    }
    case 0xf6:
    case 0xf7: {
        uint32_t modrm = fetch(vm, 1);
        unsigned group = (modrm >> 3) & 7;
        if (group == 1) break;
        Operand source = decode_rm(vm, modrm);
        unsigned width = opcode == 0xf6 ? 1 : word;
        uint32_t value = read_operand(vm, source, width);
        switch (group) {
        case 0: alu(vm, AND, width, value, fetch(vm, width)); break;
        case 2: write_operand(vm, source, width, ~value); break;
        case 3: write_operand(vm, source, width, alu(vm, SUB, width, 0, value)); break;
        case 4:
        case 5: accumulator_multiply(vm, width, value, group == 5); break;
        case 6:
        case 7: accumulator_divide(vm, width, value, group == 7); break;
        }
        return;
    }
    case 0xf8: vm->flags &= ~(uint32_t)CF; return;
    case 0xf9: vm->flags |= CF; return;
    case 0xfc: vm->flags &= ~(uint32_t)DF; return;
    case 0xfd: vm->flags |= DF; return;
    case 0xff: {
        uint32_t modrm = fetch(vm, 1);
        unsigned group = (modrm >> 3) & 7;
        Operand operand = decode_rm(vm, modrm);
        if (group == 0 || group == 1) { increment(vm, operand, word, group == 1); return; }
        if (word != 4 || (group != 2 && group != 4 && group != 6)) break;
        uint32_t value = read_operand(vm, operand, 4);
        if (group == 6) { push(vm, value); return; }
        branch(vm, value, group == 4);
        return;
    }
    default: break;
    }
    fail(vm, SKSE_X86_UNSUPPORTED_INSTRUCTION);
}

static bool valid_space(SksePeImage *const *images, size_t count, SkseX86Scratch scratch, const SkseX86Heap *heap) {
    if (!images || !count || count > SKSE_X86_MAX_IMAGES) return false;
    uint64_t scratch_end = (uint64_t)scratch.base + scratch.length;
    uint64_t heap_end = heap ? (uint64_t)heap->base + SKSE_X86_HEAP_BYTES : 0;
    if (heap && (!heap->base || heap_end > UINT64_C(0x100000000) ||
        (scratch_end > heap->base && scratch.base < heap_end))) return false;
    for (size_t i = 0; i < count; ++i) {
        const SksePeImage *image = images[i];
        if (!image || !image->bytes || !image->size || !image->base ||
            image->section_count > SKSE_PE_MAX_SECTIONS || image->headers_size > image->size) return false;
        const uint64_t end = (uint64_t)image->base + image->size;
        if (end > UINT64_C(0x100000000) || (scratch_end > image->base && scratch.base < end)) return false;
        if (heap && heap_end > image->base && heap->base < end) return false;
        for (size_t j = 0; j < i; ++j) {
            const SksePeImage *other = images[j];
            if (end > other->base && image->base < (uint64_t)other->base + other->size) return false;
        }
    }
    return true;
}

SkseX86Result skse_x86_call_in_space(SksePeImage *const *images, size_t image_count, uint32_t entry,
                                     SkseX86Scratch scratch, const uint32_t *args,
                                     size_t arg_count, uint32_t instruction_limit,
                                     SkseX86Services services, SkseX86Convention convention) {
    uint64_t scratch_end = (uint64_t)scratch.base + scratch.length;
    if (!scratch.bytes || !scratch.base || scratch.length < 128 ||
        scratch.length > MAX_SCRATCH || scratch_end > UINT32_MAX || !valid_space(images, image_count, scratch, services.heap) ||
        arg_count > MAX_ARGUMENTS || (arg_count && !args) || !instruction_limit ||
        instruction_limit > MAX_STEPS || services.count > SKSE_X86_MAX_SERVICES ||
        (services.count && !services.entries) ||
        (convention != SKSE_X86_CDECL && convention != SKSE_X86_STDCALL))
        return (SkseX86Result){.status = SKSE_X86_BAD_ARGUMENT};
    if ((services.static_tls_table || services.stack_floor) &&
        (!services.static_tls_table || (services.static_tls_table & 3) ||
         !services.stack_floor || (services.stack_floor & 15) ||
         !in_range(scratch.base, services.stack_floor, services.static_tls_table, image_count * 4) ||
         !in_range(scratch.base, scratch_end, services.stack_floor, 128)))
        return (SkseX86Result){.status = SKSE_X86_BAD_ARGUMENT};
    for (size_t i = 0; i < services.count; ++i) {
        uint32_t address = services.entries[i].address;
        SkseX86ServiceKind kind = services.entries[i].kind;
        if ((unsigned)kind > SKSE_X86_WIN_THREAD || !address ||
            (kind >= SKSE_X86_MALLOC && kind <= SKSE_X86_HEAP_FREE && !services.heap) ||
            (kind == SKSE_X86_WIN_THREAD &&
             (!services.thread || services.entries[i].value >= SKSE_WIN_THREAD_OPERATION_COUNT)) ||
            ((kind == SKSE_X86_QUERY_CLASSIC || kind == SKSE_X86_REGISTER_CORE_LISTENER) &&
             (!services.core_messaging ||
              !in_range(scratch.base, scratch_end, services.core_messaging->interface_address, 16))) ||
            in_range(scratch.base, scratch_end, address, 1) ||
            (services.heap && address >= services.heap->base && address - services.heap->base < SKSE_X86_HEAP_BYTES))
            return (SkseX86Result){.status = SKSE_X86_BAD_ARGUMENT};
        for (size_t j = 0; j < image_count; ++j)
            if (in_image(images[j], address)) return (SkseX86Result){.status = SKSE_X86_BAD_ARGUMENT};
        for (size_t j = 0; j < i; ++j)
            if (address == services.entries[j].address)
                return (SkseX86Result){.status = SKSE_X86_BAD_ARGUMENT};
    }

    static const uint32_t canaries[] = {0x13749894, 0x2269d748, 0x5ee56321, 0xf61fd490};
    Vm vm = {.images = images, .image_count = image_count, .recent_image = images[0], .scratch = scratch,
             .services = services, .ip = entry, .stack_floor = services.stack_floor ? services.stack_floor : scratch.base,
             .limit = instruction_limit};
    if (convention == SKSE_X86_STDCALL) vm.return_cleanup = (unsigned)arg_count * 4;
    vm.registers[EBX] = canaries[0];
    vm.registers[EBP] = canaries[1];
    vm.registers[ESI] = canaries[2];
    vm.registers[EDI] = canaries[3];
    vm.registers[ESP] = (uint32_t)(scratch_end & ~UINT64_C(15));
    for (size_t i = arg_count; i > 0; --i) push(&vm, args[i - 1]);
    push(&vm, 0);
    vm.return_slot = vm.registers[ESP];
    while (!vm.finished && vm.result.status == SKSE_X86_OK &&
           vm.result.instructions + vm.bulk_work < vm.limit) {
        vm.result.instruction_va = vm.ip;
        ++vm.result.instructions;
        step(&vm);
    }
    if (!vm.finished) fail(&vm, SKSE_X86_INSTRUCTION_LIMIT);
    else if ((vm.flags & DF) || vm.registers[EBX] != canaries[0] || vm.registers[EBP] != canaries[1] ||
             vm.registers[ESI] != canaries[2] || vm.registers[EDI] != canaries[3])
        fail(&vm, SKSE_X86_BAD_CALL_RETURN);
    if (vm.result.status == SKSE_X86_OK) vm.result.eax = vm.registers[EAX];
    vm.result.work = vm.result.instructions + vm.bulk_work;
    return vm.result;
}

SkseX86Result skse_x86_call(SksePeImage *image, uint32_t entry,
                            SkseX86Scratch scratch, const uint32_t *args,
                            size_t arg_count, uint32_t instruction_limit,
                            SkseX86Services services, SkseX86Convention convention) {
    return skse_x86_call_in_space(&image, 1, entry, scratch, args, arg_count, instruction_limit, services, convention);
}

const char *skse_x86_status_text(SkseX86Status status) {
    switch (status) {
    case SKSE_X86_OK: return "Completed";
    case SKSE_X86_BAD_ARGUMENT: return "Invalid mapped image, scratch, or call arguments";
    case SKSE_X86_MEMORY_FAULT: return "The plugin used a region without the required permissions";
    case SKSE_X86_UNSUPPORTED_INSTRUCTION: return "The plugin needs an unsupported x86 instruction";
    case SKSE_X86_INSTRUCTION_LIMIT: return "The call exceeded its x86 execution budget";
    case SKSE_X86_BAD_CALL_RETURN: return "The call did not preserve its expected stack and registers";
    case SKSE_X86_UNSUPPORTED_API: return "The plugin called a Windows or SKSE service unavailable in this test";
    case SKSE_X86_DIVIDE_FAULT: return "The plugin divided by zero or its quotient exceeded the x86 register";
    }
    return "Unknown x86 result";
}
