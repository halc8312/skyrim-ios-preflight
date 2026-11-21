/* SPDX-License-Identifier: GPL-3.0-only */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _GNU_SOURCE 1
#endif
#include "save_publish.h"
#include "observer/lifecycle.h"
#include "rt_table.h"
#if defined(SKYRIM_TEST_SAVE_COSAVE)
#include "cosave_fixture.h"
#endif
#if defined(SKYRIM_TEST_FILE_RUNTIME)
#include "lifted_backend.h"
#endif
#if defined(SKYRIM_TEST_SAVE_ROUNDTRIP)
#include "roundtrip.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #cond); exit(1); } \
} while (0)

enum {
    MEMORY_BYTES = 0x1c00000, SAVE_CODE = 0x6816b2, MOVE_CODE = 0x680317, STACK = 0x20000,
    MANAGER = 0x31000, STREAM = 0x32000, STREAM_FILE = 0x33000, SAVE_GLOBAL = 0x34000, VM = 0x36000,
    SLOT = 0x38000, BOOKKEEPING = 0x39000, ENGINE_SAVE = 0x1b2e38c, BACKUPS = 0x1272450,
    CRT_FILE = 0x3a000, BSFILE_TABLE = 0x110dba8, BSFILE_DESTROY = 0xa579b0,
    BSFILE_BUFFER = 0x50000, SOURCE_BYTES = 0x60000, WRITE_STACK = 0x61000, FILE_MODE = 0x1b2e3a8,
    IAT_GET_LAST_ERROR = 0x106b058, IAT_DELETE = 0x106b094, IAT_TICK = 0x106b144, IAT_MOVE = 0x106b250,
};
typedef enum { ACCEPT, VM_FALSE, SHORT_WRITE, FLUSH_ERROR, CRT_CLOSE_ERROR, CLOSE_ERROR,
               WRONG_OWNER, TEMP_REPLACED, TEMP_MODIFIED, MOVE_ERROR, DEST_REPLACED } Scenario;

static const char slot[] = "Fixture", temp[] = "Fixture.ess.tmp", final[] = "Fixture.ess";
static const char backup[] = "Fixture.ess.bak", side[] = "Fixture.ess.txt";
static const char header[] = "authored ", payload[] = "base input", old[] = "old base";
static const char expected[] = "authored base input";
static const uint32_t tick_count = 0x65432100;

typedef struct {
    ClassicSavePublish witness;
    Scenario mode;
    int dir, writer, move_error, closed_writer, close_result;
    struct stat before_close;
    uint64_t token;
    bool in_final_move, move_succeeded, have_before_close;
    unsigned slots, vm_saves, flushes, closes, releases, final_moves, backup_moves, sidecars;
    unsigned physical_writes;
} Engine;

uint8_t *MEM;
static Engine *engine;
static uint64_t handle_serial;

void f_6814D0(CPU *);
void f_8D3750(CPU *);
void f_A579B0(CPU *);
#if defined(SKYRIM_TEST_BUFFERED_SAVE)
void f_6779D0(CPU *);
void f_A57280(CPU *);
void f_A57240(CPU *);
#endif

static uint32_t arg(const CPU *cpu, unsigned i) { return M32(cpu->r[4] + 4 + 4 * i); }
static uint32_t ret(const CPU *cpu) { return M32(cpu->r[4]); }
static char *string_at(uint32_t address) {
    CHECK(address && address < MEMORY_BYTES && memchr(MEM + address, 0, MEMORY_BYTES - address));
    return (char *)MEM + address;
}

static void write_new(const char *leaf, const char *bytes) {
    int fd = openat(engine->dir, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    CHECK(fd >= 0 && write(fd, bytes, strlen(bytes)) == (ssize_t)strlen(bytes) && !close(fd));
}

void f_680040(CPU *cpu) {
    CHECK(cpu->r[1] == MANAGER && !strcmp(string_at(arg(cpu, 0)), slot));
    CHECK((ret(cpu) == 0x681629 && arg(cpu, 2) == 1) ||
          (ret(cpu) == 0x6801f6 && arg(cpu, 2) == 1) || (ret(cpu) == 0x6816e7 && arg(cpu, 2) == 0));
    const char *leaf = arg(cpu, 2) ? temp : final;
    CHECK(arg(cpu, 1) < MEMORY_BYTES - 0x104);
    memcpy(MEM + arg(cpu, 1), leaf, strlen(leaf) + 1);
    cpu->r[4] += 16;
}

void f_F562C2(CPU *cpu) {
    CHECK(ret(cpu) == 0x681634 || ret(cpu) == 0x6802bf || ret(cpu) == 0x6802d6);
    const char *leaf = string_at(arg(cpu, 0));
    CHECK(!strcmp(leaf, temp) || !strcmp(leaf, backup) || !strcmp(leaf, final));
    struct stat found;
    cpu->r[0] = !fstatat(engine->dir, leaf, &found, AT_SYMLINK_NOFOLLOW) ? 0 : UINT32_MAX;
    cpu->r[4] += 4;
}

void f_677A40(CPU *cpu) {
    CHECK(ret(cpu) == 0x681658 && arg(cpu, 1) == 1 && arg(cpu, 2) == 0x10000 && engine->writer < 0);
    const char *leaf = string_at(arg(cpu, 0));
    CHECK(!strcmp(leaf, temp));
#if defined(SKYRIM_TEST_FILE_RUNTIME)
    classic_runtime_save_open_scope(cpu, true);
    engine->writer = fixture_runtime_open(cpu, leaf);
#else
    engine->writer = openat(engine->dir, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
#endif
    CHECK(engine->writer >= 0);
    engine->token = ++handle_serial;
    classic_save_opened(&engine->witness, leaf, engine->writer, engine->token);
    M32(STREAM + 0x104) = STREAM_FILE;
    M32(STREAM_FILE) = BSFILE_TABLE;
    M32(STREAM_FILE + 0x24) = CRT_FILE;
    M8(STREAM_FILE + 0x2c) = 1;
#if defined(SKYRIM_TEST_BUFFERED_SAVE)
    M32(STREAM_FILE + 4) = 0;
    M32(STREAM_FILE + 0x0c) = 0xa57280;
    M32(STREAM_FILE + 0x10) = arg(cpu, 2);
    M32(STREAM_FILE + 0x18) = 0;
    M32(STREAM_FILE + 0x20) = BSFILE_BUFFER;
    M32(STREAM_FILE + 0x28) = 1;
    M32(STREAM_FILE + 0x150) = 0;
#endif
    cpu->r[0] = STREAM;
    cpu->r[4] += 4;
}

void f_677770(CPU *cpu) {
    CHECK(ret(cpu) == 0x681665 && cpu->r[1] == STREAM && arg(cpu, 0) == SLOT);
    memcpy(MEM + STREAM, slot, sizeof slot);
    cpu->r[4] += 8;
}

void f_677540(CPU *cpu) {
    CHECK(ret(cpu) == 0x68166c && cpu->r[1] == STREAM && engine->writer >= 0);
#if defined(SKYRIM_TEST_FILE_RUNTIME)
    classic_runtime_save_open_scope(cpu, false);
#endif
    cpu->r[0] = 1;
    cpu->r[4] += 4;
}

static void write_base(CPU *cpu, const char *bytes, bool short_write) {
    const size_t length = strlen(bytes);
#if defined(SKYRIM_TEST_BUFFERED_SAVE)
    (void)short_write; /* Only the physical write can fail after these logical writes. */
    const uint32_t buffered = M32(STREAM_FILE + 0x18);
    CPU writer = *cpu;
    writer.r[1] = STREAM;
    writer.r[4] = WRITE_STACK;
    M32(WRITE_STACK) = 0x12345678;
    M32(WRITE_STACK + 4) = SOURCE_BYTES;
    M32(WRITE_STACK + 8) = (uint32_t)length;
    memcpy(MEM + SOURCE_BYTES, bytes, length);
    f_6779D0(&writer);
    CHECK(writer.r[4] == WRITE_STACK + 12 && writer.r[0] == length);
    CHECK(M32(STREAM_FILE + 0x18) == buffered + length && M32(STREAM_FILE + 4) == buffered + length);
    CHECK(!memcmp(MEM + BSFILE_BUFFER + buffered, bytes, length));
    CHECK(!engine->physical_writes && lseek(engine->writer, 0, SEEK_CUR) == 0);
#else
#if defined(SKYRIM_TEST_FILE_RUNTIME)
    const ssize_t actual = fixture_runtime_write(cpu, bytes, length, short_write);
#else
    (void)cpu;
    const ssize_t actual = write(engine->writer, bytes, length - short_write);
#endif
    CHECK(actual == (ssize_t)(length - short_write));
    classic_save_written(&engine->witness, engine->writer, engine->token, length, actual);
#endif
}

void f_67E020(CPU *cpu) {
    CHECK(ret(cpu) == 0x6816a8 && arg(cpu, 0) == STREAM && !engine->slots);
    write_base(cpu, header, false);
    cpu->r[4] += 4;
}

void f_8CD8F0(CPU *cpu) {
    CHECK(ret(cpu) == 0x8d3856 && cpu->r[1] == VM && !engine->vm_saves);
    CHECK(arg(cpu, 0) == STACK - 0xf0 && arg(cpu, 1) == STREAM);
    ++engine->vm_saves;
    cpu->r[0] = 0x87654300u | (engine->mode != VM_FALSE);
    cpu->r[4] += 12;
}

void f_679200(CPU *cpu) {
    CHECK(ret(cpu) == 0x6816b7 && cpu->r[1] == SAVE_GLOBAL && arg(cpu, 0) == STREAM && !engine->slots);
    ++engine->slots;
#if defined(SKYRIM_TEST_SAVE_COSAVE)
    save_composition_begin();
#endif
    write_base(cpu, payload, engine->mode == SHORT_WRITE);
    CPU vm = *cpu;
    vm.r[4] = STACK - 0x100;
    vm.r[6] = STREAM;
    vm.r[7] = VM;
    f_8D3750(&vm);
    CHECK(vm.r[4] == STACK - 0x100 && (uint8_t)vm.r[0] == (engine->mode != VM_FALSE));
#if defined(SKYRIM_TEST_SAVE_COSAVE)
    save_composition_record();
#endif
    if (engine->mode == CLOSE_ERROR) CHECK(!close(engine->writer));
    cpu->r[0] = 0xfedcba00; /* A void callee need not set a success result. */
    cpu->r[4] += 8;
}

void f_680F60(CPU *cpu) {
    CHECK(ret(cpu) == 0x6816be && cpu->r[1] == MANAGER && engine->slots == 1);
    cpu->r[4] += 4;
}

#if defined(SKYRIM_TEST_BUFFERED_SAVE)
void f_F52E30(CPU *cpu) {
    CHECK(ret(cpu) == 0xb29b63 && arg(cpu, 1) == SOURCE_BYTES && arg(cpu, 2) < 64);
    CHECK(arg(cpu, 0) == BSFILE_BUFFER + M32(STREAM_FILE + 0x18));
    memcpy(MEM + arg(cpu, 0), MEM + arg(cpu, 1), arg(cpu, 2));
    cpu->r[0] = arg(cpu, 0);
    cpu->r[4] += 4;
}

void f_F57FB6(CPU *cpu) {
    CHECK(ret(cpu) == 0xb29882 && arg(cpu, 0) == BSFILE_BUFFER && arg(cpu, 1) == 1);
    CHECK(arg(cpu, 2) == sizeof expected - 1 && arg(cpu, 3) == CRT_FILE && !engine->physical_writes);
    CHECK(!memcmp(MEM + BSFILE_BUFFER, expected, sizeof expected - 1));
    const size_t count = arg(cpu, 2);
    const bool short_write = engine->mode == SHORT_WRITE;
    const ssize_t actual = fixture_runtime_write_buffer(cpu, BSFILE_BUFFER, count, short_write);
    CHECK(actual == (ssize_t)(count - short_write));
    classic_save_written(&engine->witness, engine->writer, engine->token, count, actual);
    ++engine->physical_writes;
    cpu->r[0] = (uint32_t)actual;
    cpu->r[4] += 4;
}
#else
void f_B29850(CPU *cpu) {
    CHECK(ret(cpu) == 0xa579ca && cpu->r[1] == STREAM_FILE && !engine->flushes);
    if (engine->mode == FLUSH_ERROR) M8(STREAM_FILE + 0x2c) = 0;
    cpu->r[0] = 0x12345600u | (engine->mode != FLUSH_ERROR);
    cpu->r[4] += 4;
}

#endif

void fixture_flush_result(CPU *cpu) {
    CHECK(!engine->flushes && engine->writer >= 0);
    ++engine->flushes;
#if defined(SKYRIM_TEST_BUFFERED_SAVE)
    const bool success = engine->mode != SHORT_WRITE;
    CHECK(engine->physical_writes == 1 && ((uint8_t)cpu->r[0] != 0) == success);
    CHECK((M8(STREAM_FILE + 0x2c) != 0) == success);
#endif
    classic_save_flushed(&engine->witness, engine->writer, engine->token, (uint8_t)cpu->r[0] != 0);
#if defined(SKYRIM_TEST_FILE_RUNTIME)
    classic_runtime_save_flush_result(cpu, (uint8_t)cpu->r[0] != 0);
#endif
}

void f_F56351(CPU *cpu) {
    CHECK(ret(cpu) == 0xa579d3 && arg(cpu, 0) == CRT_FILE && engine->writer >= 0 && !engine->closes);
    engine->closed_writer = engine->writer;
    engine->have_before_close = !fstat(engine->writer, &engine->before_close);
#if defined(SKYRIM_TEST_FILE_RUNTIME)
    engine->close_result = fixture_runtime_close(cpu);
#else
    engine->close_result = close(engine->writer);
#endif
    CHECK((engine->close_result < 0) == (engine->mode == CLOSE_ERROR));
    engine->writer = -1;
    ++engine->closes;
    cpu->r[0] = (engine->close_result || engine->mode == CRT_CLOSE_ERROR) ? UINT32_MAX : 0;
    cpu->r[4] += 4;
}

void fixture_close_result(CPU *cpu) {
    CHECK(engine->closes == 1 && engine->writer < 0);
    const struct stat *before = engine->have_before_close ? &engine->before_close : NULL;
    classic_save_closed(&engine->witness, engine->closed_writer, engine->token + (engine->mode == WRONG_OWNER),
                        before, engine->close_result, cpu->r[0] == 0);
#if defined(SKYRIM_TEST_FILE_RUNTIME)
    classic_runtime_save_crt_close_result(cpu, cpu->r[0] == 0);
#endif
    if (engine->mode == TEMP_REPLACED) {
        CHECK(!unlinkat(engine->dir, temp, 0));
        write_new(temp, expected);
    } else if (engine->mode == TEMP_MODIFIED) {
        int other = openat(engine->dir, temp, O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC);
        CHECK(other >= 0 && write(other, "x", 1) == 1 && !close(other));
    }
}

void f_A58000(CPU *cpu) {
    CHECK(ret(cpu) == 0xa579dd && cpu->r[1] == STREAM_FILE && engine->closes == 1);
    cpu->r[4] += 4;
}

void f_B29950(CPU *cpu) {
    CHECK(ret(cpu) == 0xa579eb && cpu->r[1] == STREAM_FILE && !M32(STREAM_FILE + 0x24));
    cpu->r[4] += 4;
}

void f_A487B0(CPU *cpu) {
    CHECK(cpu->r[1] == 0x1b418b0 && !arg(cpu, 1));
    CHECK((!engine->releases && ret(cpu) == 0xa579ff && arg(cpu, 0) == STREAM_FILE) ||
          (engine->releases == 1 && ret(cpu) == 0x6801cf && arg(cpu, 0) == STREAM));
    ++engine->releases;
    CHECK(engine->closes == 1 && engine->writer < 0);
    cpu->r[4] += 12;
}

void f_F52394(CPU *cpu) {
    const char *source = string_at(arg(cpu, 2));
    CHECK(arg(cpu, 1) == 0x104 && arg(cpu, 0) < MEMORY_BYTES - 0x104 && strlen(source) < 0x104);
    memcpy(MEM + arg(cpu, 0), source, strlen(source) + 1);
    cpu->r[4] += 4;
}

void f_F5372D(CPU *cpu) {
    char *destination = string_at(arg(cpu, 0));
    const char *ending = string_at(arg(cpu, 2));
    CHECK(arg(cpu, 1) == 0x104 && !strcmp(ending, ".bak"));
    CHECK(strlen(destination) + strlen(ending) < 0x104 && arg(cpu, 0) < MEMORY_BYTES - 0x104);
    memcpy(destination + strlen(destination), ending, strlen(ending) + 1);
    cpu->r[4] += 4;
}

void f_F53EE3(CPU *cpu) {
    CHECK(ret(cpu) == 0xf53f2f && engine->move_error > 0 && arg(cpu, 0) == (uint32_t)engine->move_error);
    cpu->r[4] += 4;
}

void fixture_final_move_enter(CPU *cpu) {
    CHECK(ret(cpu) == 0x68031c && !engine->in_final_move && !engine->final_moves && engine->closes == 1);
    const char *from = string_at(arg(cpu, 0)), *to = string_at(arg(cpu, 1));
    CHECK(!strcmp(from, temp) && !strcmp(to, final));
    classic_save_move_enter(&engine->witness, from, to);
#if defined(SKYRIM_TEST_FILE_RUNTIME)
    classic_runtime_save_final_scope(cpu);
#endif
    engine->in_final_move = true;
}

void fixture_final_move_leave(CPU *cpu) {
    CHECK(engine->in_final_move && engine->final_moves == 1 && cpu->r[0] == 0);
    /* The actual rename still decides publication when last-error is zero. */
    classic_save_move_leave(&engine->witness, engine->move_succeeded, cpu->r[0] == 0);
#if defined(SKYRIM_TEST_FILE_RUNTIME)
    classic_runtime_save_crt_move_result(cpu, cpu->r[0] == 0);
#endif
    engine->in_final_move = false;
#if defined(SKYRIM_TEST_SAVE_COSAVE)
#if defined(SKYRIM_TEST_FILE_RUNTIME)
    save_composition_finish(fixture_runtime_reader(), engine->mode != VM_FALSE);
#else
    save_composition_finish(classic_save_published_fd(&engine->witness), engine->mode != VM_FALSE);
#endif
#endif
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    (void)cache;
#if defined(SKYRIM_TEST_BUFFERED_SAVE)
    if (target == 0xa57280) {
        CHECK(ret(cpu) == 0x677a33 && arg(cpu, 0) == STREAM_FILE);
        f_A57280(cpu);
        return;
    }
    if (target == 0xa57240) {
        CHECK(ret(cpu) == 0xa57295 && cpu->r[1] == STREAM_FILE);
        f_A57240(cpu);
        return;
    }
#endif
    if (target == BSFILE_DESTROY) {
        CHECK(ret(cpu) == 0x677532 && cpu->r[1] == STREAM_FILE && arg(cpu, 0) == 1);
        f_A579B0(cpu);
    } else if (target == IAT_MOVE) {
        CHECK(ret(cpu) == 0xf53f17);
        const char *from = string_at(arg(cpu, 0)), *to = string_at(arg(cpu, 1));
        if (engine->in_final_move) {
            CHECK(!engine->final_moves && !strcmp(from, temp) && !strcmp(to, final));
            ++engine->final_moves;
        } else {
            CHECK(!engine->backup_moves && !strcmp(from, final) && !strcmp(to, backup));
            ++engine->backup_moves;
        }
#if defined(SKYRIM_TEST_FILE_RUNTIME)
        const int result = fixture_runtime_rename(cpu, from, to);
#else
        const int result = renameat(engine->dir, from, engine->dir, to);
#endif
        engine->move_error = result ? errno : 0;
        cpu->r[0] = result == 0;
        if (engine->in_final_move) {
            engine->move_succeeded = result == 0;
            if (engine->mode == DEST_REPLACED) {
                CHECK(!result && !unlinkat(engine->dir, final, 0));
                write_new(final, expected);
            }
        }
        cpu->r[4] += 12;
    } else if (target == IAT_GET_LAST_ERROR) {
        CHECK(ret(cpu) == 0xf53f21 && engine->mode == MOVE_ERROR && engine->move_error != 0);
#if defined(SKYRIM_TEST_FILE_RUNTIME)
        cpu->r[0] = fixture_runtime_last_error(cpu);
#else
        cpu->r[0] = 0;
#endif
        cpu->r[4] += 4;
    } else if (target == IAT_DELETE) {
        CHECK(ret(cpu) == 0x681647 || ret(cpu) == 0x6802e8);
        const char *leaf = string_at(arg(cpu, 0));
        CHECK(!strcmp(leaf, temp) || !strcmp(leaf, backup));
        cpu->r[0] = !unlinkat(engine->dir, leaf, 0);
        cpu->r[4] += 8;
    } else {
        CHECK(target == IAT_TICK && ret(cpu) == 0x681757);
        cpu->r[0] = tick_count;
        cpu->r[4] += 4;
    }
}

void f_685790(CPU *cpu) {
    CHECK(engine->mode == MOVE_ERROR && ret(cpu) == 0x6816f2 && !engine->sidecars);
    CHECK(!strcmp(string_at(arg(cpu, 0)), final) && arg(cpu, 1) == BOOKKEEPING);
    write_new(side, "side export completed");
    ++engine->sidecars;
    cpu->r[4] += 4;
}

void f_67E4C0(CPU *cpu) {
    CHECK(ret(cpu) == 0x68170b && cpu->r[1] == SAVE_GLOBAL + 0x3d0);
    cpu->r[4] += 8;
}

void f_680660(CPU *cpu) {
    CHECK(ret(cpu) == 0x681751 && cpu->r[1] == MANAGER && arg(cpu, 1) == UINT32_MAX);
    cpu->r[4] += 12;
}

void f_680D30(CPU *cpu) { (void)cpu; CHECK(false); }
void f_860F00(CPU *cpu) { (void)cpu; CHECK(false); }

static void trace_return(size_t first, Scenario mode) {
    SkyrimLifecycleRow rows[SkyrimLifecycleCapacity];
    uint64_t dropped;
    CHECK(skyrim_lifecycle_snapshot(rows, SkyrimLifecycleCapacity, &dropped) == first + 4 && !dropped);
    const SkyrimLifecycleRow *trace = rows + first;
    CHECK(trace[0].site == SkyrimSaveSlot && trace[0].parent == 0 && !trace[0].returning && trace[0].outcome == SkyrimNoResult);
    CHECK(trace[1].site == SkyrimSaveVM && trace[1].parent == trace[0].ticket && !trace[1].returning);
    CHECK(trace[2].site == SkyrimSaveVM && trace[2].ticket == trace[1].ticket && trace[2].returning);
    CHECK(trace[2].outcome == (mode == VM_FALSE ? SkyrimReturnedFalse : SkyrimReturnedTrue));
    CHECK(trace[3].site == SkyrimSaveSlot && trace[3].ticket == trace[0].ticket && trace[3].returning && trace[3].outcome == SkyrimNoResult);
}

static void run(int root, const char *directory, Scenario mode, ClassicSaveOutcome outcome) {
    char leaf[20];
#if defined(SKYRIM_TEST_SAVE_ROUNDTRIP)
    int length = snprintf(leaf, sizeof leaf, "saved-profile");
#else
    int length = snprintf(leaf, sizeof leaf, "case-%d", mode);
#endif
    CHECK(length > 0 && (size_t)length < sizeof leaf && !mkdirat(root, leaf, 0700));
    Engine current = {.dir = openat(root, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC), .writer = -1, .mode = mode};
    CHECK(current.dir >= 0);
#if defined(SKYRIM_TEST_SAVE_COSAVE)
    char case_path[PATH_MAX];
    const int size = snprintf(case_path, sizeof case_path, "%s/%s", directory, leaf);
    CHECK(size > 0 && (size_t)size < sizeof case_path);
    save_composition_open(case_path);
#elif !defined(SKYRIM_TEST_FILE_RUNTIME)
    (void)directory;
#endif
    engine = &current;
    if (mode == MOVE_ERROR) CHECK(!mkdirat(current.dir, final, 0700));
    else write_new(final, old);
    CHECK(classic_save_start(&current.witness, current.dir, final));
    M32(BACKUPS) = mode != MOVE_ERROR;

    /* Frame state at the original fresh-output branch after the prologue. */
    CPU cpu = {0};
    const uint32_t frame = STACK;
    memset(MEM + frame, 0, 0x334);
    M32(frame) = 0x44444444;
    M32(frame + 4) = 0x33333333;
    M32(frame + 8) = 0x22222222;
    M32(frame + 12) = 0x11111111;
    M32(frame + 0x324) = 0x12121212;
    M32(frame + 0x32c) = UINT32_MAX;
    M32(frame + 0x330) = mode == MOVE_ERROR ? BOOKKEEPING : 0;
    cpu.r[4] = frame;
    cpu.r[5] = SLOT;
    cpu.r[6] = MANAGER;
    SkyrimLifecycleRow before[SkyrimLifecycleCapacity];
    size_t first = skyrim_lifecycle_snapshot(before, SkyrimLifecycleCapacity, NULL);
#if defined(SKYRIM_TEST_FILE_RUNTIME)
    fixture_runtime_begin(&cpu, current.dir, directory, leaf, final);
#endif
#if defined(SKYRIM_TEST_SAVE_ROUNDTRIP)
    roundtrip_enter_game();
#endif
    f_6814D0(&cpu);
#if defined(SKYRIM_TEST_SAVE_ROUNDTRIP)
    roundtrip_leave_game();
#endif
    trace_return(first, mode);

    CHECK(cpu.r[0] == (tick_count | 1u) && cpu.r[4] == frame + 0x334 && M32(frame + 0x324) == 0x12121212);
    CHECK(cpu.r[3] == 0x11111111 && cpu.r[5] == 0x22222222 && cpu.r[6] == 0x33333333 && cpu.r[7] == 0x44444444);
    CHECK(current.slots == 1 && current.vm_saves == 1 && current.flushes == 1 && current.closes == 1);
    CHECK(current.final_moves == 1 && current.releases == 2);
    CHECK(current.backup_moves == (mode != MOVE_ERROR) && current.sidecars == (mode == MOVE_ERROR));
    CHECK(current.witness.outcome == outcome);
    const int reader = classic_save_published_fd(&current.witness);
    if (outcome == CLASSIC_SAVE_PUBLISHED) {
        struct stat info, named;
        const int flags = fcntl(reader, F_GETFL);
        char bytes[sizeof expected];
        CHECK(flags >= 0 && (flags & O_ACCMODE) == O_RDONLY);
        CHECK(pread(reader, bytes, sizeof bytes, 0) == sizeof expected - 1 && !memcmp(bytes, expected, sizeof expected - 1));
        CHECK(!fstat(reader, &info) && !fstatat(current.dir, final, &named, AT_SYMLINK_NOFOLLOW));
        CHECK(info.st_ino == named.st_ino && info.st_dev == named.st_dev);
    } else CHECK(reader == -1);
#if defined(SKYRIM_TEST_BUFFERED_SAVE)
    if (mode == SHORT_WRITE) {
        char bytes[sizeof expected];
        int truncated = openat(current.dir, final, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        CHECK(current.move_succeeded && truncated >= 0);
        CHECK(pread(truncated, bytes, sizeof bytes, 0) == sizeof expected - 2);
        CHECK(!memcmp(bytes, expected, sizeof expected - 2) && !close(truncated));
    }
#endif
#if defined(SKYRIM_TEST_FILE_RUNTIME)
    fixture_runtime_end(reader);
#endif
    classic_save_dispose(&current.witness);
    if (reader >= 0) CHECK(fcntl(reader, F_GETFL) < 0 && errno == EBADF);
#if defined(SKYRIM_TEST_SAVE_COSAVE)
    save_composition_close();
#endif
    CHECK(!close(current.dir));
    engine = NULL;
}

int main(int argc, char **argv) {
#if defined(SKYRIM_TEST_SAVE_ROUNDTRIP)
    CHECK(argc == 3);
    save_roundtrip_configure(argv[2]);
#else
    CHECK(argc == 2);
#endif
    int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(root >= 0);
    long page = sysconf(_SC_PAGESIZE);
    MEM = mmap(NULL, MEMORY_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(page > 0 && MEM != MAP_FAILED && MEMORY_BYTES % page == 0);
    CHECK(!mprotect(MEM + (SAVE_CODE / page) * page, page, PROT_READ));
    CHECK(!mprotect(MEM + (MOVE_CODE / page) * page, page, PROT_READ));
    memcpy(MEM + SLOT, slot, sizeof slot);
    M32(ENGINE_SAVE) = SAVE_GLOBAL;
    M32(IAT_MOVE) = IAT_MOVE;
    M32(IAT_DELETE) = IAT_DELETE;
    M32(IAT_TICK) = IAT_TICK;
    M32(IAT_GET_LAST_ERROR) = IAT_GET_LAST_ERROR;
    M32(BSFILE_TABLE) = BSFILE_DESTROY;
#if defined(SKYRIM_TEST_BUFFERED_SAVE)
    M32(BSFILE_TABLE + 0x48) = 0xa57240;
    M32(FILE_MODE) = SAVE_GLOBAL;
#endif
    memcpy(MEM + 0x10cc8ac, ".bak", 5);

#if defined(SKYRIM_TEST_SAVE_ROUNDTRIP)
    if (!strcmp(argv[2], "vm-rejected")) run(root, argv[1], VM_FALSE, CLASSIC_SAVE_PUBLISHED);
    else if (!strcmp(argv[2], "short-write")) run(root, argv[1], SHORT_WRITE, CLASSIC_SAVE_WRITE);
    else run(root, argv[1], ACCEPT, CLASSIC_SAVE_PUBLISHED);
#else
    const struct { Scenario mode; ClassicSaveOutcome outcome; } cases[] = {
        {ACCEPT, CLASSIC_SAVE_PUBLISHED},
        {VM_FALSE, CLASSIC_SAVE_PUBLISHED},
        {SHORT_WRITE, CLASSIC_SAVE_WRITE},
        {FLUSH_ERROR, CLASSIC_SAVE_FLUSH},
        {CRT_CLOSE_ERROR, CLASSIC_SAVE_CLOSE},
        {CLOSE_ERROR, CLASSIC_SAVE_CLOSE},
#if !defined(SKYRIM_TEST_FILE_RUNTIME)
        {WRONG_OWNER, CLASSIC_SAVE_IDENTITY},
#endif
        {TEMP_REPLACED, CLASSIC_SAVE_IDENTITY},
        {TEMP_MODIFIED, CLASSIC_SAVE_IDENTITY},
        {MOVE_ERROR, CLASSIC_SAVE_MOVE},
        {DEST_REPLACED, CLASSIC_SAVE_IDENTITY},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
        run(root, argv[1], cases[i].mode, cases[i].outcome);
#endif
    CHECK(M8(SAVE_CODE) == 0 && M8(MOVE_CODE) == 0 && !munmap(MEM, MEMORY_BYTES) && !close(root));
#if defined(SKYRIM_TEST_SAVE_ROUNDTRIP)
    puts("PASS buffered save, publication evidence and deferred companion decision");
#elif defined(SKYRIM_TEST_SAVE_COSAVE)
    puts("PASS lifted save publication and true VM result required by the existing companion bridge");
#elif defined(SKYRIM_TEST_FILE_RUNTIME)
    puts("PASS lifted save and destructor calls with actual runtime shims and matching file evidence");
#else
    puts("PASS lifted save publication, retained read-only inode, and separately reported VM result");
#endif
    return 0;
}
