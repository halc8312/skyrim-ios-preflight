/* SPDX-License-Identifier: GPL-3.0-only */
#include "fresh_load.h"
#include "lift_prelude.h"

#include <assert.h>
#include <errno.h>

enum {
    STREAM = 0x2100, BSFILE = 0x2400, STACK = 0x3400, DESTINATION = STACK + 0x98,
    BUFFERED_READER = 0x200000, INITIAL_POS = 41,
};
static uint8_t memory[0x4000];
uint8_t *MEM = memory;
static uint8_t buffered[CLASSIC_FRESH_SLOT_READ_BYTES];
static uint32_t read_count;
static unsigned reader_calls, observations;

void baseline_slot_read(CPU *cpu);
void observed_slot_read(CPU *cpu);

/* The pinned f_677990 calls a stream callback after constructing these
 * arguments. This hosted transport supplies bytes already held in memory. */
void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    (void)cache;
    const uint32_t stack = cpu->r[4];
    assert(target == BUFFERED_READER && M32(stack) == 0x6779B7u);
    assert(M32(stack + 4) == BSFILE && M32(stack + 8) == DESTINATION);
    assert(M32(stack + 12) == CLASSIC_FRESH_SLOT_READ_BYTES);
    assert(M32(M32(stack + 16)) == 1u && M32(stack + 20) == 1u);
    if (read_count <= sizeof buffered) memcpy(MEM + DESTINATION, buffered, read_count);
    cpu->r[0] = read_count;
    cpu->r[1] = 0x4321; /* The call may clobber ECX. */
    cpu->r[4] += 4;    /* This callback is cdecl. */
    errno = EAGAIN;
    ++reader_calls;
}

void classic_fresh_slot_read_observed(const CPU *cpu, uint32_t stream, uint32_t bytes) {
    assert(reader_calls == 2 && stream == STREAM);
    assert(cpu->r[1] == 1u); /* The lifted reader restores ECX from its local slot. */
    assert(bytes == read_count && bytes == cpu->r[0]);
    assert(M32(BSFILE + 4) == INITIAL_POS + bytes);
    assert(!memcmp(MEM + DESTINATION, buffered, bytes));
    ++observations;
    errno = EIO;
}

static CPU run(void (*read)(CPU *)) {
    CPU cpu = {0};
    for (unsigned i = 0; i < 8; ++i) cpu.r[i] = 0x10 + i;
    cpu.r[4] = STACK;
    cpu.r[6] = STREAM;
    memset(MEM + DESTINATION, 0, sizeof buffered);
    M32(STREAM + 0x104) = BSFILE;
    M32(BSFILE + 4) = INITIAL_POS;
    M32(BSFILE + 8) = BUFFERED_READER;
    errno = 0;

    read(&cpu);

    assert(errno == EAGAIN && cpu.r[0] == read_count && cpu.r[4] == STACK);
    assert(M32(BSFILE + 4) == INITIAL_POS + read_count);
    if (read_count <= sizeof buffered) assert(!memcmp(MEM + DESTINATION, buffered, read_count));
    return cpu;
}

int main(void) {
    memset(buffered, 0x89, sizeof buffered);
    const uint32_t counts[] = {0, 1, 39, CLASSIC_FRESH_SLOT_READ_BYTES, CLASSIC_FRESH_SLOT_READ_BYTES + 1, UINT32_MAX};
    for (size_t i = 0; i < sizeof counts / sizeof *counts; ++i) {
        read_count = counts[i];
        observations = reader_calls = 0;
        const CPU baseline = run(baseline_slot_read);
        assert(reader_calls == 1 && observations == 0);
        const CPU observed = run(observed_slot_read);
        assert(reader_calls == 2 && observations == (unsigned)(read_count > 0 && read_count <= sizeof buffered));
        assert(!memcmp(baseline.r, observed.r, sizeof baseline.r));
        assert(!memcmp(&baseline.flags, &observed.flags, sizeof baseline.flags));
    }
    return 0;
}
