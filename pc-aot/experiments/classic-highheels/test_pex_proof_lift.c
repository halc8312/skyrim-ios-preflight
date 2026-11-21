/* SPDX-License-Identifier: GPL-3.0-only */
#include "funcs_exe.h"
#include "lift_prelude.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    TLS = 0x1000, TLS_TABLE = 0x1100, TLS_DATA = 0x1200, LOADER = 0x2000,
    STORE = 0x2100, STREAM = 0x2200, TYPE = 0x2300, OTHER_TYPE = 0x2340,
    CLASS_NAME = 0x2400, PARSE_OUTPUT = 0x2500, LINK_FRAME = 0x2600, LINK_OK = 0x2700,
    STACK = 0x6000, READ_BUFFER = 0x7000, IMAGE_LIMIT = 0x1bc0000,
    STORE_VTABLE = 0x10ea8c8, STREAM_VTABLE = 0x110cd68, HANDLE = 8,
    PEX_SIZE = 5875, PARSER_RETURN = 0xc68e78, STORE_OPEN_RETURN = 0xc454b4,
    STORE_READ_RETURN = 0x8c07a2, READ_RETURN = 0xa580e2,
    INTERLOCKED = 1, STREAM_OPEN, STREAM_NAME, READ_FILE
};

typedef enum {
    MATCH, CACHED_CLASS, CACHED_STORE, PARSE_FAILED, LINK_FAILED,
    OTHER_TYPE_LINKED, FOREIGN_READER, DIFFERENT_BYTES
} Scenario;

typedef struct {
    const CPU *cpu;
    uint32_t type, stream;
    bool fresh, checked_read, matched_read, parsed;
} Witness;

uint8_t *MEM;
static _Thread_local Witness *witness;
static uint8_t original[PEX_SIZE];
static int selected_fd;
static Scenario current;
static unsigned file_reads;

void fixture_link_state(uint32_t type, uint32_t linker_stack);

static uint32_t arg(const CPU *cpu, unsigned index)
{
    return M32(cpu->r[4] + index * 4);
}

static void result(CPU *cpu, uint32_t value, unsigned pop)
{
    cpu->r[0] = value;
    cpu->r[4] += 4 + pop * 4;
}

/* Called beside the original ReadFile by the hosted dispatcher. The real
 * descriptor remains open, and neither this comparison nor the VM alters it. */
static void observe_read(const CPU *cpu, int fd, ssize_t bytes)
{
    if (!witness || witness->cpu != cpu || !witness->fresh || witness->checked_read ||
        arg(cpu, 0) != READ_RETURN || bytes <= 0 ||
        M32(STORE + 0x18) != witness->stream || M32(witness->stream) != STREAM_VTABLE ||
        M32(witness->stream + 0x30) != 0 || arg(cpu, 1) != M32(witness->stream + 0x1c))
        return;
    witness->checked_read = true;
    uint8_t contents[PEX_SIZE + 1];
    witness->matched_read = pread(fd, contents, sizeof contents, 0) == sizeof original &&
                            memcmp(contents, original, sizeof original) == 0;
}

typedef struct {
    const CPU *cpu;
    ssize_t bytes;
} ReadObservation;

static void *observe_foreign_thread(void *data)
{
    const ReadObservation *observation = data;
    observe_read(observation->cpu, selected_fd, observation->bytes);
    return NULL;
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache)
{
    (void)cache;
    switch (target) {
    case 0x8c0810: {
        const uint32_t store = cpu->r[1];
        const bool fresh = witness && witness->cpu == cpu && store == STORE &&
            arg(cpu, 0) == STORE_OPEN_RETURN && M32(store) == STORE_VTABLE && !M8(store + 0x20) &&
            arg(cpu, 1) == CLASS_NAME;
        f_8C0810(cpu);
        if (fresh && (uint8_t)cpu->r[0]) {
            witness->fresh = true;
            witness->stream = M32(store + 0x18);
        }
        break;
    }
    case 0x8c0650: f_8C0650(cpu); break;
    case 0x8c0c80: f_8C0C80(cpu); break;
    case 0x8c0970: f_8C0970(cpu); break;
    case 0x5afb90: result(cpu, M8(cpu->r[1] + 0x20), 0); break;
    case 0xa44b50:
        assert(arg(cpu, 0) == STORE_READ_RETURN && cpu->r[1] == STREAM);
        f_A44B50(cpu);
        break;
    case INTERLOCKED: {
        uint32_t old = M32(arg(cpu, 1));
        if (old == arg(cpu, 3)) M32(arg(cpu, 1)) = arg(cpu, 2);
        result(cpu, old, 3);
        break;
    }
    case STREAM_OPEN: result(cpu, 0, 0); break;
    case STREAM_NAME: result(cpu, 0, 1); break;
    case READ_FILE: {
        assert(arg(cpu, 0) == READ_RETURN && arg(cpu, 1) == HANDLE && !arg(cpu, 5));
        const ssize_t count = pread(selected_fd, MEM + arg(cpu, 2), arg(cpu, 3), 0);
        assert(count == arg(cpu, 3));
        M32(arg(cpu, 4)) = (uint32_t)count;
        ++file_reads;
        if (current == FOREIGN_READER) {
            ReadObservation observation = {cpu, count};
            pthread_t other;
            assert(pthread_create(&other, NULL, observe_foreign_thread, &observation) == 0);
            assert(pthread_join(other, NULL) == 0);
        } else {
            observe_read(cpu, selected_fd, count);
        }
        result(cpu, 1, 5);
        break;
    }
    default:
        fprintf(stderr, "Unexpected indirect call %08x at %08x\n", target, arg(cpu, 0));
        abort();
    }
}

/* Engine services outside the observed resource and loader boundaries. */
void f_4202A0(CPU *cpu)
{
    assert(arg(cpu, 0) == 0x8c0872);
    const int count = snprintf((char *)MEM + arg(cpu, 1), arg(cpu, 2),
        (const char *)MEM + arg(cpu, 3), (const char *)MEM + arg(cpu, 4),
        (const char *)MEM + arg(cpu, 5), (const char *)MEM + arg(cpu, 6));
    assert(count > 0 && (unsigned)count < arg(cpu, 2));
    result(cpu, (uint32_t)count, 0);
}

void f_A3F8B0(CPU *cpu)
{
    assert(arg(cpu, 0) == 0x8c0884 && arg(cpu, 2) == STORE + 0x18);
    assert(strcmp((const char *)MEM + arg(cpu, 1), "Scripts/hdtHighHeelEffect.pex") == 0);
    M32(arg(cpu, 2)) = STREAM;
    result(cpu, 0, 0);
}

void f_F52240(CPU *cpu)
{
    memset(MEM + arg(cpu, 1), (int)arg(cpu, 2), arg(cpu, 3));
    result(cpu, arg(cpu, 1), 0);
}

/* The stock loader calls this stage for the header. In the hosted model,
 * let it drive the real Store Read without recreating the full parser. */
void f_C42F20(CPU *cpu)
{
    assert(arg(cpu, 0) == 0xc454f0 && cpu->r[1] == LOADER && arg(cpu, 1) == PARSE_OUTPUT);
    const uint32_t saved = cpu->r[4];
    cpu->r[1] = STORE;
    cpu->r[4] -= 12;
    M32(cpu->r[4] + 4) = 16;
    M32(cpu->r[4] + 8) = READ_BUFFER;
    f_8C0730(cpu);
    assert(cpu->r[4] == saved && cpu->r[0] == 0);
    result(cpu, 1, 1);
}

void f_C45220(CPU *cpu) { result(cpu, current != PARSE_FAILED, 2); }
void f_C39410(CPU *cpu) { result(cpu, 0, 0); }
void f_C39480(CPU *cpu) { result(cpu, 0, 0); }
void f_C5F650(CPU *cpu) { result(cpu, 0, 0); }
void f_C3FBD0(CPU *cpu) { result(cpu, 0, 0); }
void f_C3FEF0(CPU *cpu) { result(cpu, 1, 1); }
void f_C431C0(CPU *cpu) { result(cpu, 1, 1); }
void f_C43AE0(CPU *cpu) { result(cpu, 1, 1); }
void f_43BA00(CPU *cpu) { result(cpu, cpu->r[1], 1); }
void f_A511B0(CPU *cpu) { result(cpu, 0, 0); }
void f_A51280(CPU *cpu) { result(cpu, cpu->r[1], 1); }
void f_4660D0(CPU *cpu) { result(cpu, 0, 0); }
void f_454020(CPU *cpu) { result(cpu, 0, 0); }
void f_43B440(CPU *cpu) { result(cpu, 0, 0); }
void f_4068C0(CPU *cpu) { result(cpu, 1, 0); }
static void unexpected(const char *call, const CPU *cpu)
{
    fprintf(stderr, "Unexpected %s at %08x\n", call, arg(cpu, 0));
    abort();
}

void f_A50D50(CPU *cpu) { unexpected(__func__, cpu); }
void f_C398A0(CPU *cpu) { unexpected(__func__, cpu); }
void f_C39990(CPU *cpu) { unexpected(__func__, cpu); }
void f_A445E0(CPU *cpu) { unexpected(__func__, cpu); }
void f_A5A4B0(CPU *cpu) { unexpected(__func__, cpu); }
void rt_call(CPU *cpu, uint32_t target) { (void)target; unexpected(__func__, cpu); }

typedef struct {
    CPU cpu;
    unsigned reads;
    uint8_t memory[0x8000];
} Result;

static Result run(bool observe)
{
    memset(MEM, 0, 0x8000);
    M32(TLS + 0x2c) = TLS_TABLE;
    M32(TLS_TABLE) = TLS_DATA;
    M32(TLS_DATA + 0x4ac) = 9;
    M32(LOADER + 8) = STORE;
    M32(STORE) = STORE_VTABLE;
    M32(STREAM) = STREAM_VTABLE;
    M32(STREAM + 4) = PEX_SIZE;
    M32(STREAM + 0x1c) = HANDLE;
    M32(TYPE + 4) = CLASS_NAME;
    M32(TYPE + 0x10) = 0xfc | (current == CACHED_CLASS ? 3 : 1);
    M32(OTHER_TYPE + 4) = CLASS_NAME;
    M32(OTHER_TYPE + 0x10) = 0xfd;
    memcpy(MEM + CLASS_NAME, "hdtHighHeelEffect", sizeof "hdtHighHeelEffect");
    if (current == CACHED_STORE) {
        M32(STORE + 0x18) = STREAM;
        M8(STORE + 0x20) = M8(STORE + 0x21) = 1;
    }
    CPU cpu = {.r = {1, LOADER, 3, TYPE, STACK, 5, 6, 7}, .fs_base = TLS};
    Witness sample = {.cpu = &cpu};
    witness = observe ? &sample : NULL;
    file_reads = 0;
    const uint32_t linked_type = current == OTHER_TYPE_LINKED ? OTHER_TYPE : TYPE;
    if (current != CACHED_CLASS) {
        M32(STACK) = PARSER_RETURN;
        M32(STACK + 4) = CLASS_NAME;
        M32(STACK + 8) = PARSE_OUTPUT;
        sample.type = cpu.r[3];  /* Retained by the outer linker in EBX. */
        f_C45410(&cpu);
        assert((bool)(uint8_t)cpu.r[0] == (current != PARSE_FAILED));
        assert(cpu.r[4] == STACK + 12 && M32(TLS_DATA + 0x4ac) == 9);
        assert(!M32(STORE + 0x18) && !M8(STORE + 0x20));
        sample.parsed = (uint8_t)cpu.r[0] && M32(STACK) == PARSER_RETURN;
        M32(LINK_FRAME + 0x470) = LINK_OK;
        M8(LINK_OK) = sample.parsed && current != LINK_FAILED;
        fixture_link_state(linked_type, LINK_FRAME);
        assert(M32(linked_type + 0x10) == (M8(LINK_OK) ? 0xffu : 0xfeu));
    }
    const bool proved = observe && sample.fresh && sample.matched_read && sample.parsed &&
        sample.type == linked_type && M32(linked_type + 4) == CLASS_NAME &&
        (M32(linked_type + 0x10) & 3u) == 3u;
    assert(proved == (observe && current == MATCH));
    assert(file_reads == (current == CACHED_CLASS ? 0u : 1u));
    witness = NULL;
    Result final = {.cpu = cpu, .reads = file_reads};
    memcpy(final.memory, MEM, sizeof final.memory);
    return final;
}

static void scenario(Scenario mode)
{
    current = mode;
    const Result ordinary = run(false);
    const Result observed = run(true);
    assert(!memcmp(ordinary.cpu.r, observed.cpu.r, sizeof ordinary.cpu.r));
    assert(!memcmp(&ordinary.cpu.flags, &observed.cpu.flags, sizeof ordinary.cpu.flags));
    assert(ordinary.cpu.fs_base == observed.cpu.fs_base && ordinary.reads == observed.reads);
    assert(!memcmp(ordinary.memory, observed.memory, sizeof ordinary.memory));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *owned = fopen(argv[1], "rb");
    assert(owned && fread(original, 1, sizeof original, owned) == sizeof original);
    assert(fgetc(owned) == EOF);
    MEM = calloc(IMAGE_LIMIT, 1);
    assert(MEM);
    M32(STORE_VTABLE + 0x18) = 0x8c0810;
    M32(STORE_VTABLE + 0x1c) = 0x8c0970;
    M32(STORE_VTABLE + 0x24) = 0x5afb90;
    M32(STORE_VTABLE + 0x28) = 0x8c0c80;
    M32(STORE_VTABLE + 0x2c) = 0x8c0650;
    M32(STREAM_VTABLE + 4) = STREAM_OPEN;
    M32(STREAM_VTABLE + 0x18) = 0xa44b50;
    M32(STREAM_VTABLE + 0x28) = STREAM_NAME;
    M32(0x106b14c) = INTERLOCKED;
    M32(0x106b284) = READ_FILE;
    M32(0xa44bf8) = 0xa44bb4;
    memcpy(MEM + 0x1077350, "%s%s%s", sizeof "%s%s%s");
    memcpy(MEM + 0x10ea808, "Scripts/", sizeof "Scripts/");
    memcpy(MEM + 0x10ea814, ".pex", sizeof ".pex");

    selected_fd = fileno(owned);
    scenario(MATCH);
    scenario(CACHED_CLASS);
    scenario(CACHED_STORE);
    scenario(PARSE_FAILED);
    scenario(LINK_FAILED);
    scenario(OTHER_TYPE_LINKED);
    scenario(FOREIGN_READER);

    FILE *alternate = tmpfile();
    assert(alternate);
    assert(fwrite(original, 1, sizeof original - 1, alternate) == sizeof original - 1);
    assert(fputc(original[sizeof original - 1] ^ 1, alternate) != EOF && !fflush(alternate));
    selected_fd = fileno(alternate);
    scenario(DIFFERENT_BYTES);

    assert(!fclose(alternate) && !fclose(owned));
    free(MEM);
    puts("Classic HighHeels PEX witness joins fresh Store reads, successful parsing and linked class state");
}
