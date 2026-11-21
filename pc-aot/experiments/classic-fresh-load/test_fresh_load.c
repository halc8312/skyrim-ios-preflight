/* SPDX-License-Identifier: GPL-3.0-only */
#include "fresh_load.h"
#include "rt_table.h"
#if defined(SKYRIM_TEST_FRESH_COSAVE)
#include "cosave_fixture.h"
#endif

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #cond); exit(1); } \
} while (0)

enum {
    MEMORY_BYTES = 0x1c00000, CODE = 0x682220, STACK = 0x20000,
    MANAGER = 0x30000, STREAM = 0x33000, SELECTED_NAME = 0x36000,
    GAME_LOAD_SINGLETON = 0x1b2e38c, SAVE_NAME_SCRATCH = 0x1b2e3b0,
};

typedef enum {
    OK, CACHED, NO_OPEN, METADATA_ONLY, EARLY_REJECT, LOAD_REJECT,
    WRONG_PATH, OTHER_FD, REPLACED_FD, NEW_GENERATION, DUPLICATE_OPEN,
    WRITABLE_FD, CHANGED_AFTER_READ, NESTED,
} Scenario;

typedef struct {
    Scenario scenario;
    uint32_t manager, stream, guest_path;
    int fd;
    uint64_t generation;
    unsigned first_opens, checks, loads, shown_error, post_load, cleaned, reports;
    ClassicFreshReport report;
} Engine;

uint8_t *MEM;
static const char name[] = "Fixture";
static const char guest_path[] = "Saves\\Fixture.ess";
static char selected[CLASSIC_FRESH_PATH_BYTES], different[CLASSIC_FRESH_PATH_BYTES];
static Engine *engine;
static bool observing;
static uint64_t handle_serial, last_ticket;

void f_6821C0(CPU *);
static void run(Scenario, ClassicFreshResult expected, bool should_succeed, unsigned depth);

bool classic_fresh_enabled(void) { return observing; }

bool classic_fresh_resolve_path(const CPU *cpu, uint32_t path,
                                char canonical[CLASSIC_FRESH_PATH_BYTES]) {
    CHECK(M32(cpu->r[4]) == 0x682225 && cpu->r[1] == engine->stream);
    CHECK(path == engine->guest_path && !memcmp(MEM + path, guest_path, sizeof guest_path));
    memcpy(canonical, selected, strlen(selected) + 1);
    return true;
}

void classic_fresh_report(ClassicFreshReport report) {
    CHECK(engine && !engine->reports && report.ticket > last_ticket);
    ++engine->reports;
    last_ticket = report.ticket;
    engine->report = report;
    if (report.result == CLASSIC_FRESH_READY) {
        const int flags = fcntl(report.ess_fd, F_GETFL);
        CHECK(engine->post_load == 1 && engine->cleaned == 1 && engine->fd == -1);
        CHECK(flags >= 0 && (flags & O_ACCMODE) == O_RDONLY);
#if defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
        CHECK(classic_cosave_roundtrip_same_input(report.ess_fd));
#else
        struct stat actual, expected;
        CHECK(!fstat(report.ess_fd, &actual) && !stat(selected, &expected));
        CHECK(actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino);
#endif
    } else {
        CHECK(report.ess_fd == -1);
    }
#if defined(SKYRIM_TEST_FRESH_COSAVE)
    classic_cosave_fixture_finish(report, different);
#endif
}

void f_F52394(CPU *cpu) {
    const uint32_t stack = cpu->r[4];
    const uint32_t destination = M32(stack + 4), source = M32(stack + 12);
    CHECK(M32(stack + 8) == 0x104 && source == SELECTED_NAME);
    CHECK(M32(stack) == 0x6821e9 || (M32(stack) == 0x68229b && destination == SAVE_NAME_SCRATCH));
    memcpy(MEM + destination, MEM + source, sizeof name);
    cpu->r[4] += 4;
}

void f_680040(CPU *cpu) {
    const uint32_t stack = cpu->r[4];
    CHECK(M32(stack) == 0x682201 && cpu->r[1] == engine->manager);
    CHECK(M32(stack + 4) == SELECTED_NAME && !M32(stack + 12));
    engine->guest_path = M32(stack + 8);
    memcpy(MEM + engine->guest_path, guest_path, sizeof guest_path);
    cpu->r[4] += 16;
}

void f_677A40(CPU *cpu) {
    const uint32_t stack = cpu->r[4];
    CHECK(M32(stack) == 0x682211 && M32(stack + 4) == engine->guest_path);
    CHECK(!M32(stack + 8) && M32(stack + 12) == 0x200000);
    cpu->r[0] = engine->stream;
    cpu->r[4] += 4;
}

void f_677770(CPU *cpu) {
    CHECK(M32(cpu->r[4]) == 0x68221e && M32(cpu->r[4] + 4) == SELECTED_NAME);
    CHECK(cpu->r[1] == engine->stream);
    cpu->r[4] += 8;
}

static void open_source(CPU *cpu) {
    if (engine->scenario == NO_OPEN) return;
    const int flags = engine->scenario == WRITABLE_FD ? O_RDWR : O_RDONLY;
    engine->fd = open(selected, flags | O_CLOEXEC);
    CHECK(engine->fd >= 0);
#if defined(SKYRIM_TEST_FRESH_COSAVE)
    engine->fd = classic_cosave_fixture_load_fd(engine->fd);
#endif
    engine->generation = ++handle_serial;
    const char *path = engine->scenario == WRONG_PATH ? different : selected;
    classic_fresh_observe_open(cpu, engine->stream, path, engine->fd, engine->generation);
    if (engine->scenario == DUPLICATE_OPEN) {
        int second = open(selected, O_RDONLY | O_CLOEXEC);
        CHECK(second >= 0);
        classic_fresh_observe_open(cpu, engine->stream, path, second, ++handle_serial);
        CHECK(!close(second));
    }
}

void f_677540(CPU *cpu) {
    CHECK(cpu->r[1] == engine->stream);
    const uint32_t next = M32(cpu->r[4]);
    if (next == 0x682225) {
        CHECK(!engine->first_opens && !engine->checks && engine->fd < 0);
        ++engine->first_opens;
        open_source(cpu);
    } else {
        CHECK(next == 0x68224b && !engine->checks);
        ++engine->checks;
        if (engine->scenario == CACHED && engine->fd < 0) open_source(cpu);
    }
    cpu->r[0] = engine->fd >= 0;
    cpu->r[4] += 4;
}

#if !defined(SKYRIM_TEST_FRESH_COSAVE) || defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
static size_t read_one(int fd) {
    unsigned char byte;
    CHECK(pread(fd, &byte, sizeof byte, 0) == 1);
    return 1;
}
#endif

static void read_base(CPU *cpu, int fd, uint64_t generation) {
#if defined(SKYRIM_TEST_FRESH_COSAVE)
#if defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
    const size_t count = engine->scenario == CACHED ? read_one(fd) : classic_cosave_fixture_read(fd, engine->loads != 0);
#else
    const size_t count = classic_cosave_fixture_read(fd, engine->loads != 0);
#endif
#else
    const size_t count = read_one(fd);
#endif
    classic_fresh_observe_read(cpu, engine->stream, fd, generation, count);
}

void f_6820C0(CPU *cpu) {
    CHECK(M32(cpu->r[4]) == 0x682262 && M32(cpu->r[4] + 4) == engine->stream);
    CHECK(cpu->r[1] == engine->manager && engine->checks == 1);
    read_base(cpu, engine->fd, engine->generation); /* Metadata access before the slot. */
#if defined(SKYRIM_TEST_FRESH_COSAVE)
    classic_cosave_fixture_after_metadata();
#endif
    cpu->r[0] = engine->scenario != EARLY_REJECT;
    cpu->r[4] += 8;
}

void f_67B720(CPU *cpu) {
    CHECK(M32(cpu->r[4]) == 0x68227a && M32(cpu->r[4] + 4) == engine->stream);
    CHECK(cpu->r[1] == M32(GAME_LOAD_SINGLETON) && !engine->loads);
    ++engine->loads;
#if defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
    classic_cosave_roundtrip_bind_slot();
#endif
    const Scenario scenario = engine->scenario;
    if (scenario == NESTED) run(OK, 0, true, 1);
    int fd = engine->fd;
    if (scenario == OTHER_FD || scenario == REPLACED_FD) {
        const char *path = scenario == OTHER_FD ? selected : different;
        int second = open(path, O_RDONLY | O_CLOEXEC);
        CHECK(second >= 0);
        if (scenario == REPLACED_FD) {
            CHECK(dup2(second, fd) == fd && !close(second));
        } else {
            fd = second;
        }
    }
    if (scenario != METADATA_ONLY) {
        const uint64_t id = scenario == NEW_GENERATION ? ++handle_serial : engine->generation;
        read_base(cpu, fd, id);
    }
    if (scenario == OTHER_FD) CHECK(!close(fd));
    if (scenario == CHANGED_AFTER_READ) {
        int writer = open(selected, O_WRONLY | O_APPEND | O_CLOEXEC);
        CHECK(writer >= 0 && write(writer, "X", 1) == 1 && !close(writer));
    }
#if defined(SKYRIM_TEST_FRESH_COSAVE)
    classic_cosave_fixture_vm(cpu, engine->stream);
#endif
    cpu->r[0] = scenario == LOAD_REJECT ? 0x76891000 : 0x76891001;
    cpu->r[4] += 12;
}

void f_681E00(CPU *cpu) {
    CHECK(M32(cpu->r[4]) == 0x682289 && engine->loads == 1 && !engine->post_load);
    ++engine->post_load;
    cpu->r[4] += 4;
}

void f_87AC60(CPU *cpu) {
    CHECK(M32(cpu->r[4]) == 0x6822d4 && engine->scenario == LOAD_REJECT);
    ++engine->shown_error;
    cpu->r[4] += 4;
}

void f_680190(CPU *cpu) {
    CHECK(M32(cpu->r[4]) == 0x6822ed && M32(cpu->r[4] + 4) == engine->stream);
    CHECK(engine->scenario != CACHED && !engine->cleaned);
    if (engine->fd >= 0) CHECK(!close(engine->fd));
    engine->fd = -1;
    ++engine->cleaned;
    cpu->r[4] += 12;
}

void f_685790(CPU *cpu) { (void)cpu; CHECK(false); }

void f_680660(CPU *cpu) {
    CHECK(M32(cpu->r[4]) == 0x68232c && engine->post_load == 1);
    cpu->r[4] += 12;
}

void f_6928A0(CPU *cpu) {
    CHECK(M32(cpu->r[4]) == 0x682338);
    cpu->r[0] = 0xabcdef00;
    cpu->r[4] += 4;
}

static void run(Scenario scenario, ClassicFreshResult expected, bool should_succeed, unsigned depth) {
    Engine frame = {.scenario = scenario, .fd = -1,
                    .manager = MANAGER + 0x1000 * depth, .stream = STREAM + 0x1000 * depth};
    Engine *parent = engine;
    engine = &frame;
    CPU cpu = {0};
    const uint32_t sp = STACK + 0x1000 * (depth + 1);
    M32(sp) = 0x6ab000;
    M32(sp + 4) = SELECTED_NAME;
    M32(sp + 8) = M32(sp + 12) = M32(sp + 16) = 0;
    M32(frame.manager + 0x30) = scenario == CACHED ? frame.stream : 0;
#if defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
    if (scenario == CACHED) {
        frame.fd = open(selected, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        CHECK(frame.fd >= 0);
        frame.generation = ++handle_serial;
    }
#endif
    cpu.r[1] = frame.manager;
    cpu.r[3] = 0x11111111;
    cpu.r[4] = sp;
    cpu.r[5] = 0x22222222;
    cpu.r[6] = 0x33333333;
    cpu.r[7] = 0x44444444;
    f_6821C0(&cpu);
    CHECK(cpu.r[0] == (0xabcdef00u | should_succeed));
    CHECK(cpu.r[4] == sp + 20 && M32(sp) == 0x6ab000);
    CHECK(cpu.r[3] == 0x11111111 && cpu.r[5] == 0x22222222);
    CHECK(cpu.r[6] == 0x33333333 && cpu.r[7] == 0x44444444);
    CHECK(frame.first_opens == (scenario != CACHED) && frame.checks == 1);
    CHECK(frame.post_load == should_succeed && frame.shown_error == (scenario == LOAD_REJECT));
    CHECK(frame.cleaned == (scenario != CACHED));
    if (expected) {
        CHECK(frame.reports == 1 && frame.report.result == expected);
        CHECK(frame.report.outer_ok == should_succeed && frame.report.slot_ok == should_succeed);
        CHECK(frame.report.slot_returned == (scenario != NO_OPEN && scenario != EARLY_REJECT));
        if (expected == CLASSIC_FRESH_READY) CHECK(fcntl(frame.report.ess_fd, F_GETFL) < 0);
    } else {
        CHECK(!frame.reports);
    }
    if (frame.fd >= 0) CHECK(!close(frame.fd));
    engine = parent;
}

#if !defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
static void write_fixture(const char *path) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(fd >= 0 && write(fd, "authored input", 14) == 14 && !close(fd));
}
#endif

int main(int argc, char **argv) {
#if defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
    CHECK(argc == 3);
    classic_cosave_roundtrip_configure(argv[2]);
#else
    CHECK(argc == 2);
#endif
    int length = snprintf(selected, sizeof selected, "%s/Fixture.ess", argv[1]);
    CHECK(length > 0 && (size_t)length < sizeof selected);
    length = snprintf(different, sizeof different, "%s/Other.ess", argv[1]);
    CHECK(length > 0 && (size_t)length < sizeof different);
#if defined(SKYRIM_TEST_FRESH_COSAVE)
    classic_cosave_fixture_init(argv[1], selected);
#else
    write_fixture(selected);
#endif
#if !defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
    write_fixture(different);
#endif

    long page = sysconf(_SC_PAGESIZE);
    CHECK(page > 0 && MEMORY_BYTES % page == 0);
    MEM = mmap(NULL, MEMORY_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(MEM != MAP_FAILED && !mprotect(MEM + (CODE / page) * page, page, PROT_READ));
    memcpy(MEM + SELECTED_NAME, name, sizeof name);
    M32(GAME_LOAD_SINGLETON) = 0x37000;
    M8(0x1272480) = 1;

#if defined(SKYRIM_TEST_FRESH_COSAVE)
    observing = true;
#if defined(SKYRIM_TEST_LOAD_ROUNDTRIP)
    classic_cosave_fixture_begin(COSAVE_ACCEPT);
    if (classic_cosave_roundtrip_cached()) run(CACHED, CLASSIC_FRESH_CACHED, true, 0);
    else run(OK, CLASSIC_FRESH_READY, true, 0);
#else
    for (enum ClassicCosaveCase mode = COSAVE_ACCEPT; mode <= COSAVE_NO_SLOT_READ; ++mode) {
        classic_cosave_fixture_begin(mode);
        if (mode == COSAVE_BASE_REJECT) run(LOAD_REJECT, CLASSIC_FRESH_REJECTED, false, 0);
        else if (mode == COSAVE_NO_SLOT_READ) run(METADATA_ONLY, CLASSIC_FRESH_NO_READ, true, 0);
        else run(OK, CLASSIC_FRESH_READY, true, 0);
    }
#endif
    classic_cosave_fixture_close();
    puts("PASS sealed input, lifted load and VM evidence, verified companion and rejected mismatches");
#else
    run(OK, 0, true, 0);
    observing = true;
    run(OK, CLASSIC_FRESH_READY, true, 0);
    run(CACHED, CLASSIC_FRESH_CACHED, true, 0);
    run(NO_OPEN, CLASSIC_FRESH_NO_OPEN, false, 0);
    run(METADATA_ONLY, CLASSIC_FRESH_NO_READ, true, 0);
    run(EARLY_REJECT, CLASSIC_FRESH_REJECTED, false, 0);
    run(LOAD_REJECT, CLASSIC_FRESH_REJECTED, false, 0);
    run(NESTED, CLASSIC_FRESH_REENTERED, true, 0);
    const Scenario invalid[] = {WRONG_PATH, OTHER_FD, REPLACED_FD, NEW_GENERATION,
                                DUPLICATE_OPEN, WRITABLE_FD, CHANGED_AFTER_READ};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
        run(invalid[i], CLASSIC_FRESH_IDENTITY, true, 0);
    puts("PASS fresh selected load, definite open and read, original results, rejected ambiguous provenance");
#endif
    CHECK(M8(CODE) == 0 && !munmap(MEM, MEMORY_BYTES));
    return 0;
}
