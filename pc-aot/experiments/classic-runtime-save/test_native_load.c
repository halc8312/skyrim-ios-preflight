/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE 1
#include "rt.h"
#include "lift_prelude.h"
#include "funcs_exe.h"
#include "native_load.h"
#include "runtime_save.h"
#include "save_paths.h"

#include <assert.h>
#include <sys/mman.h>

enum {
    MANAGER = GUEST_HEAP_BASE + 0x1000, STREAM = GUEST_HEAP_BASE + 0x2000,
    BSFILE = GUEST_HEAP_BASE + 0x3000, FILE_TABLE = GUEST_HEAP_BASE + 0x4000,
    DESCRIPTOR_PAGE = GUEST_HEAP_BASE + 0x5000, BUFFER = GUEST_HEAP_BASE + 0x10000,
    NAME = GUEST_HEAP_BASE + 0x7000, VM = GUEST_HEAP_BASE + 0x8000,
    FILE_SLOT = 4, CRT_FILE = 0x12de588 + FILE_SLOT * 0x20, FILENO = 9,
    HANDLE_ENTRY = DESCRIPTOR_PAGE + FILENO * 0x40, FILE_BYTES = 0x200000,
    HEADER_TEXT = GUEST_HEAP_BASE + 0x9000, TLS_ARRAY = GUEST_HEAP_BASE + 0xa000,
    TLS_RECORD = GUEST_HEAP_BASE + 0xb000, READ_FILE_IMPORT = 0xf3000080u,
};
typedef enum { ACCEPT, WRONG_DESCRIPTOR, NO_VM, VM_FALSE, CLONE_REFUSED, REENTERED, ABORT_REPORT } Scenario;

static const char guest_path[] = "C:\\Users\\Player\\Saves\\Fixture.ess";
static const char header_text[] = "FixtureHead";
static uint8_t base[FILE_BYTES];
uint8_t *MEM;
static struct {
    Scenario mode;
    ClassicNativeLoad *attempt;
    uint32_t handle, vm_frame;
    unsigned clones, physical_reads, header_copies, copies, checks, reverts, vms, cleaned;
    bool reading_header, reading_slot, file_locked, descriptor_locked;
} engine;
void classic_runtime_fixture_attach(CPU *);
void classic_runtime_fixture_crt_read(CPU *);
void classic_runtime_fixture_header_read(CPU *);

int skse_test_fclonefileat(int source, int dir, const char *name, unsigned flags) {
    assert(!flags && ++engine.clones == 1 && !engine.physical_reads);
    if (engine.mode == CLONE_REFUSED) { errno = EOPNOTSUPP; return -1; }
    const int writer = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    assert(writer >= 0);
    uint8_t copy[4096];
    for (off_t offset = 0; offset < FILE_BYTES; offset += sizeof copy) {
        const ssize_t bytes = pread(source, copy, sizeof copy, offset);
        assert(bytes == sizeof copy && write(writer, copy, (size_t)bytes) == bytes);
    }
    assert(!close(writer));
    return 0;
}

/* Host/runtime services alone change the I/O frame. The current CPU, TEB,
 * handle generation and normal guest call stack stay under the lifted caller. */
static uint32_t runtime_call(CPU *cpu, uint32_t ret, const uint32_t *args, size_t count,
                             enum ClassicFileCall file) {
    CPU saved = *cpu;
    const uint32_t frame = cpu->r[4] - 0x400;
    cpu->r[4] = frame;
    M32(frame) = ret;
    for (size_t i = 0; i < count; ++i) M32(frame + (i + 1) * 4) = args[i];
    classic_runtime_file_call(cpu, file);
    assert(cpu->r[4] == frame + (count + 1) * 4);
    const uint32_t result = cpu->r[0];
    *cpu = saved;
    return result;
}

static void authored_crt_open(CPU *cpu) {
    const uint32_t stack = cpu->r[4];
    assert(cpu->r[1] == BSFILE && M32(stack) == 0x67755fu);
    if (!engine.handle) {
        if (engine.mode == REENTERED) assert(!classic_native_load_enter(cpu));
        const uint32_t open[] = {NAME, 0x80000000u, 1, 0, 3, 0x80, 0};
        engine.handle = runtime_call(cpu, 0xf6bf24u, open, 7, ClassicCreateFile);
        assert(engine.handle && engine.handle != UINT32_MAX && !rt_get_last_error(cpu));
        ClassicRuntimeLoad *native = &engine.attempt->runtime;
        if (engine.mode == REENTERED) {
            assert(!engine.clones && !engine.attempt->runtime_open);
        } else if (engine.mode == CLONE_REFUSED) {
            assert(engine.clones == 1 && !native->pending_input && native->source_fd == -1);
        } else {
            struct stat source, snapshot;
            assert(engine.clones == 1 && native->pending_input && native->fd != native->source_fd);
            assert(!fstat(native->source_fd, &source) && !fstat(native->fd, &snapshot));
            assert(snapshot.st_nlink == 0 && !skse_save_same_file(&source, &snapshot));
            assert(lseek(native->fd, 0, SEEK_CUR) == 0);
        }

        M32(CRT_FILE + 0xc) = 1u;
        M32(CRT_FILE + 0x10) = FILENO;
        M32(HANDLE_ENTRY) = engine.handle;
        M8(HANDLE_ENTRY + 4) = 1u;
        M32(BSFILE + 0x24) = CRT_FILE;

        /* The ordinary 2 MiB mode allocates its buffer at open. The
         * first physical read comes later from the selected header read. */
        const CPU saved = *cpu;
        cpu->r[4] = stack - 4;
        cpu->r[1] = BSFILE;
        M32(stack - 4) = 0xa577cdu;
        f_A57460(cpu);
        assert(cpu->r[4] == stack && M32(BSFILE + 0x20) == BUFFER && M8(BSFILE + 0x2c) == 1);
        assert(!M32(BSFILE + 0x14) && !engine.physical_reads);
        *cpu = saved;
    } else {
        ++engine.checks;
    }
    cpu->r[0] = 1;
    cpu->r[4] += 12;
}

void rt_call_ic(CPU *cpu, uint32_t target, const FnEntry **cache) {
    (void)cache;
    if (target == 0xA57700u) authored_crt_open(cpu);
    else if (target == 0xA57260u) f_A57260(cpu);
    else if (target == 0xA57E70u) f_A57E70(cpu);
    else if (target == READ_FILE_IMPORT) {
        const uint32_t stack = cpu->r[4], count = M32(stack + 16);
        assert(engine.reading_header && engine.file_locked && engine.descriptor_locked && !engine.physical_reads);
        assert(M32(stack) == 0xf6adf1u && M32(stack + 4) == engine.handle &&
               M32(stack + 8) == BUFFER && M32(stack + 12) == FILE_BYTES && !M32(stack + 20));
        classic_runtime_load_file_call(cpu, ClassicLoadRead);
        assert(cpu->r[0] == 1 && cpu->r[4] == stack + 24 && M32(count) == FILE_BYTES);
        ++engine.physical_reads;
    }
    else { fprintf(stderr, "Unexpected indirect call 0x%x\n", target); abort(); }
}

void f_F52E30(CPU *cpu) {
    const uint32_t stack = cpu->r[4], destination = M32(stack + 4), source = M32(stack + 8), count = M32(stack + 12);
    assert(M32(stack) == 0xb29ab1u);
    const uint32_t header_bytes = sizeof header_text - 1;
    if (engine.reading_header) {
        assert(source == BUFFER && count == header_bytes && !engine.header_copies);
        ++engine.header_copies;
    } else {
        assert(engine.reading_slot && source == BUFFER + header_bytes &&
               count == CLASSIC_FRESH_SLOT_READ_BYTES && engine.header_copies == 1);
        ++engine.copies;
    }
    memcpy(MEM + destination, MEM + source, count);
    cpu->r[0] = destination;
    cpu->r[4] += 4;
}

void f_A48D60(CPU *cpu) {
    const uint32_t stack = cpu->r[4];
    assert(M32(stack) == 0xa58058u && M32(stack + 4) == FILE_BYTES &&
           !M32(stack + 8) && !M32(stack + 12));
    cpu->r[0] = BUFFER;
    cpu->r[4] += 16;
}
void f_F5B323(CPU *cpu) {
    assert(engine.reading_header && !engine.file_locked &&
           M32(cpu->r[4]) == 0xf58485u && M32(cpu->r[4] + 4) == CRT_FILE);
    engine.file_locked = true;
    cpu->r[4] += 4;
}
void f_F5B396(CPU *cpu) {
    assert(engine.file_locked && !engine.descriptor_locked &&
           M32(cpu->r[4]) == 0xf584bcu && M32(cpu->r[4] + 4) == CRT_FILE);
    engine.file_locked = false;
    cpu->r[4] += 4;
}
void f_F77E82(CPU *cpu) {
    assert(engine.file_locked && !engine.descriptor_locked &&
           M32(cpu->r[4]) == 0xf6b255u && M32(cpu->r[4] + 4) == FILENO);
    engine.descriptor_locked = true;
    cpu->r[4] += 4;
}
void f_F77F22(CPU *cpu) {
    assert(engine.file_locked && engine.descriptor_locked &&
           M32(cpu->r[4]) == 0xf6b2abu && M32(cpu->r[4] + 4) == FILENO);
    engine.descriptor_locked = false;
    cpu->r[4] += 4;
}

#define UNEXPECTED(name) void name(CPU *cpu) { (void)cpu; assert(!"Unexpected " #name); }
UNEXPECTED(f_A487B0)
UNEXPECTED(f_F52240)
UNEXPECTED(f_F52317)
UNEXPECTED(f_F53EBD)
UNEXPECTED(f_F53ED0)
UNEXPECTED(f_F53EE3)
UNEXPECTED(f_F549DC)
UNEXPECTED(f_F55898)
UNEXPECTED(f_F57FB6)
UNEXPECTED(f_F5BF79)
UNEXPECTED(f_F6813C)
UNEXPECTED(f_F768A0)
#undef UNEXPECTED

void f_F52394(CPU *cpu) {
    const uint32_t stack = cpu->r[4];
    assert(M32(stack) == 0x6821e9u && M32(stack + 8) == 0x104 && M32(stack + 12) == NAME);
    memcpy(MEM + M32(stack + 4), MEM + NAME, sizeof guest_path);
    cpu->r[4] += 4;
}
void f_680040(CPU *cpu) {
    const uint32_t stack = cpu->r[4];
    assert(M32(stack) == 0x682201u && cpu->r[1] == MANAGER && M32(stack + 4) == NAME);
    memcpy(MEM + M32(stack + 8), guest_path, sizeof guest_path);
    cpu->r[4] += 16;
}
void f_677A40(CPU *cpu) {
    assert(M32(cpu->r[4]) == 0x682211u &&
           !strcmp(GSTR(M32(cpu->r[4] + 4)), guest_path) && !M32(cpu->r[4] + 8) &&
           M32(cpu->r[4] + 12) == FILE_BYTES);
    M32(BSFILE + 0x10) = M32(cpu->r[4] + 12);
    cpu->r[0] = STREAM;
    cpu->r[4] += 4;
}
void f_677770(CPU *cpu) {
    assert(M32(cpu->r[4]) == 0x68221eu && cpu->r[1] == STREAM && M32(cpu->r[4] + 4) == NAME);
    cpu->r[4] += 8;
}
void f_6820C0(CPU *cpu) {
    assert(M32(cpu->r[4]) == 0x682262u && cpu->r[1] == MANAGER && M32(cpu->r[4] + 4) == STREAM);
    assert(!engine.physical_reads && engine.checks == 1);
    CPU saved = *cpu;
    const uint32_t frame = cpu->r[4] - 0x200;
    const uint32_t header_bytes = sizeof header_text - 1;
    cpu->r[4] = frame - 12;
    cpu->r[3] = STREAM;
    engine.reading_header = true;
    classic_runtime_fixture_header_read(cpu);
    engine.reading_header = false;
    assert(cpu->r[4] == frame && cpu->r[0] == header_bytes && engine.physical_reads == 1 && engine.header_copies == 1);
    assert(!engine.descriptor_locked && !engine.file_locked && !memcmp(MEM + frame + 0x58, header_text, header_bytes));
    assert(M32(BSFILE + 0x14) == FILE_BYTES && M32(BSFILE + 0x18) == header_bytes);
    const ClassicRuntimeLoad *native = &engine.attempt->runtime;
    if (engine.attempt->runtime_open) {
        assert(lseek(native->fd, 0, SEEK_CUR) == FILE_BYTES);
        if (native->source_fd >= 0) assert(lseek(native->source_fd, 0, SEEK_CUR) == 0);
    }
    *cpu = saved;
    SkyrimLifecycleCall harmless = skyrim_lifecycle_begin(SkyrimRegisterPapyrus);
    skyrim_lifecycle_end(harmless, SkyrimNoResult);
    cpu->r[0] = 1;
    cpu->r[4] += 8;
}

void f_C398A0(CPU *cpu) {
    assert(cpu->r[1] == VM + 0x150 && M32(cpu->r[4]) == 0x8d69a5u);
    cpu->r[4] += 12;
}
void f_8D5DF0(CPU *cpu) {
    assert(cpu->r[1] == VM && M32(cpu->r[4]) == 0x8d69acu && !engine.vms);
    ++engine.reverts;
    cpu->r[4] += 4;
}
void f_401710(CPU *cpu) {
    assert(cpu->r[1] == VM + 0x238 && M32(cpu->r[4]) == 0x8b9b23u);
    cpu->r[4] += 8;
}
void f_8B9AA0(CPU *cpu) {
    assert(cpu->r[1] == VM + 0x274 && M32(cpu->r[4]) == 0x8b9b3cu);
    cpu->r[4] += 8;
}
void f_8D3DB0(CPU *cpu) {
    const uint32_t stack = cpu->r[4];
    assert(cpu->r[1] == VM && M32(stack) == 0x8d6b7eu && !engine.vms);
    assert(M32(stack + 4) == engine.vm_frame + 0x14 && M32(stack + 8) == STREAM);
    ++engine.vms;
    cpu->r[0] = engine.mode == VM_FALSE ? 0 : 1;
    cpu->r[4] += 12;
}

void f_67B720(CPU *cpu) {
    const uint32_t stack = cpu->r[4];
    assert(cpu->r[1] == MANAGER && M32(stack) == 0x68227au && M32(stack + 4) == STREAM);
    const SkyrimLifecycleCall slot = skyrim_lifecycle_scope_current();
    assert(slot.site == SkyrimLoadSlot && slot.ticket == 3 && slot.generation && !slot.parent);
    CPU saved = *cpu;
    if (engine.mode == WRONG_DESCRIPTOR) M32(HANDLE_ENTRY) += 4;
    /* Host fixture starts the exact inner read block at the reviewed
     * f_67B720 prologue depth, leaving the real outer return on its stack. */
    cpu->r[4] = stack - 0x348;
    cpu->r[6] = STREAM;
    engine.reading_slot = true;
    classic_runtime_fixture_crt_read(cpu);
    engine.reading_slot = false;
    assert(cpu->r[0] == CLASSIC_FRESH_SLOT_READ_BYTES && cpu->r[4] == stack - 0x348);
    assert(!memcmp(MEM + stack - 0x348 + 0x98, base + sizeof header_text - 1, CLASSIC_FRESH_SLOT_READ_BYTES));
    if (engine.mode == WRONG_DESCRIPTOR) M32(HANDLE_ENTRY) = engine.handle;
    engine.vm_frame = stack - 0x400;
    cpu->r[4] = engine.vm_frame;
    cpu->r[1] = VM;
    M32(VM + 0x23c) = 2;
    f_8D6990(cpu);
    assert(cpu->r[4] == engine.vm_frame + 4 && M32(VM + 0x23c) == 1);
    if (engine.mode != NO_VM) {
        cpu->r[4] = engine.vm_frame;
        cpu->r[6] = VM;
        cpu->r[7] = STREAM;
        f_8D69C0(cpu);
        assert(cpu->r[4] == engine.vm_frame && cpu->r[0] == (engine.mode != VM_FALSE));
    }
    *cpu = saved;
    cpu->r[0] = 1;
    cpu->r[4] += 12;
}
void f_681E00(CPU *cpu) {
    assert(M32(cpu->r[4]) == 0x682289u && engine.copies == 1 && engine.reverts == 1);
    cpu->r[4] += 4;
}
void f_87AC60(CPU *cpu) { (void)cpu; assert(!"Unexpected f_87AC60"); }
void f_680190(CPU *cpu) {
    assert(M32(cpu->r[4]) == 0x6822edu && M32(cpu->r[4] + 4) == STREAM);
    const uint32_t args[] = {engine.handle};
    assert(runtime_call(cpu, 0xf67fc8u, args, 1, ClassicCloseHandle) == 1);
    engine.handle = 0;
    ++engine.cleaned;
    cpu->r[4] += 12;
}
void f_685790(CPU *cpu) { (void)cpu; assert(!"Unexpected f_685790"); }
void f_680660(CPU *cpu) {
    assert(M32(cpu->r[4]) == 0x68232cu && engine.cleaned == 1);
    cpu->r[4] += 12;
}
void f_6928A0(CPU *cpu) {
    assert(M32(cpu->r[4]) == 0x682338u);
    cpu->r[0] = 0xabcdef00u;
    cpu->r[4] += 4;
}

static void configure_guest(CPU *cpu, uint32_t stack) {
    memset(MEM + MANAGER, 0, 0x9000);
    cpu->r[4] = stack;
    cpu->r[1] = MANAGER;
    M32(stack) = 0x6ab000u;
    M32(stack + 4) = NAME;
    M32(stack + 8) = M32(stack + 12) = M32(stack + 16) = 0;
    memcpy(MEM + NAME, guest_path, sizeof guest_path);
    M32(0x1b2e38c) = MANAGER;
    M32(0x1b2e3a8) = MANAGER;
    M32(STREAM + 0x104) = BSFILE;
    M32(BSFILE) = 0x110dba8u;
    M32(0x110dba8u + 0x20) = 0xa57700u;
    M32(0x110dba8u + 0x44) = 0xa57e70u;
    M32(BSFILE + 8) = 0xa57260u;
    M32(BSFILE + 0x20) = 0;
    M32(0x1bbeb54u) = 0;
    M32(cpu->fs_base + 0x2c) = TLS_ARRAY;
    M32(TLS_ARRAY) = TLS_RECORD;
    memcpy(MEM + HEADER_TEXT, header_text, sizeof header_text);
    M32(0x12723e0u) = HEADER_TEXT;
    M32(0x106b284u) = READ_FILE_IMPORT;
    M32(0x1bc046cu) = FILE_TABLE;
    M32(0x1bc1480u) = 20;
    M32(FILE_TABLE + FILE_SLOT * 4) = CRT_FILE;
    M32(0x1bc0320u) = 32;
    M32(0x1bc0340u) = DESCRIPTOR_PAGE;
}

static void seed_store(SkseEventBridge *bridge, const char *root, int directory, void **image, size_t *size) {
    const int source = openat(directory, "Fixture.ess", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    assert(source >= 0);
    SkseGameEvent event;
    assert(skse_bridge_bind_named_ess(bridge, 1, directory, "Fixture.ess", source, &event, NULL) == SKSE_BRIDGE_OK);
    char db[1024];
    const int length = snprintf(db, sizeof db, "%s/text-en-voice-en", root);
    assert(length > 0 && (size_t)length < sizeof db);
    SksePairStore *store = NULL;
    assert(skse_pair_open(db, &store) == SKSE_PAIR_OK);
    const SksePairGeneration generation = {event.saved, source};
    const SksePairRecord record = {0x1234, 1, 4, "test"};
    SksePairStage *stage = NULL;
    assert(skse_pair_begin(store, &event.saved, NULL, &stage) == SKSE_PAIR_OK);
    assert(skse_pair_replace(stage, 0x321, &record, 1) == SKSE_PAIR_OK);
    assert(skse_pair_pin(stage, &generation) == SKSE_PAIR_OK && skse_pair_commit(store, stage, &generation) == SKSE_PAIR_OK);
    assert(skse_pair_export(store, &generation, image, size) == SKSE_PAIR_OK && *size > 20);
    skse_pair_discard(stage);
    skse_pair_close(store);
    assert(!close(source));
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const size_t map_size = BUFFER + FILE_BYTES + 0x1000;
    MEM = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(MEM != MAP_FAILED);
    const int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(root >= 0 && !mkdirat(root, "Saves", 0700) && !mkdirat(root, "Clone", 0700));
    const int directory = openat(root, "Saves", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    const int scratch = openat(root, "Clone", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    assert(directory >= 0 && scratch >= 0);
    for (size_t i = 0; i < sizeof base; ++i) base[i] = (uint8_t)(0x50u + i);
    memcpy(base, header_text, sizeof header_text - 1);
    const int writer = openat(directory, "Fixture.ess", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    assert(writer >= 0 && write(writer, base, sizeof base) == sizeof base && !close(writer));
    const int mapped_length = snprintf(g_save_dir, sizeof g_save_dir, "%s", argv[1]);
    char mapped[CLASSIC_FRESH_PATH_BYTES];
    const int length = snprintf(mapped, sizeof mapped, "%s/Saves", argv[1]);
    assert(mapped_length > 0 && (size_t)mapped_length < sizeof g_save_dir && length > 0 && (size_t)length < sizeof mapped);

    SkseEventBridge *bridge = NULL;
    const SkseGameScope scope = {.launch = {1}, .profile = {2}};
    assert(skse_bridge_open(argv[1], &scope, &bridge, NULL) == SKSE_BRIDGE_OK);
    void *expected = NULL;
    size_t expected_size = 0;
    seed_store(bridge, argv[1], directory, &expected, &expected_size);
    struct stat before, after;
    assert(!fstatat(directory, "Fixture.ess", &before, AT_SYMLINK_NOFOLLOW));
    CPU cpu = {0};
    classic_runtime_fixture_attach(&cpu);
    const uint32_t stack = cpu.r[4] - 512;
    ClassicNativeLoad load;
    const struct { Scenario scenario; SkseBridgeResult result; } cases[] = {
        {ACCEPT, SKSE_BRIDGE_OK}, {WRONG_DESCRIPTOR, SKSE_BRIDGE_REJECTED}, {NO_VM, SKSE_BRIDGE_TRACE},
        {VM_FALSE, SKSE_BRIDGE_REJECTED}, {CLONE_REFUSED, SKSE_BRIDGE_NEEDS_BASE},
        {REENTERED, SKSE_BRIDGE_NEEDS_BASE}, {ABORT_REPORT, SKSE_BRIDGE_STATE}, {ACCEPT, SKSE_BRIDGE_OK},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        memset(&engine, 0, sizeof engine);
        engine.mode = cases[i].scenario;
        engine.attempt = &load;
        configure_guest(&cpu, stack);
        assert(classic_native_load_arm(&load, &cpu, bridge, directory, mapped, scratch));
        f_6821C0(&cpu);
        assert(cpu.r[0] == 0xabcdef01u && cpu.r[4] == stack + 20 && M32(stack) == 0x6ab000u);
        assert(!engine.handle && engine.physical_reads == 1 && engine.copies == 1 && engine.cleaned == 1);
        assert(load.reported && (load.event.ticket == 3 || engine.mode == CLONE_REFUSED || engine.mode == REENTERED));
        if (engine.mode == ABORT_REPORT) {
            const int borrowed = load.consumed;
            assert(borrowed >= 0);
            classic_native_load_abort(&load);
            assert(fcntl(borrowed, F_GETFD) < 0 && errno == EBADF);
        }
        void *image = NULL;
        size_t size = 0;
        assert(classic_native_load_finish(&load, &image, &size, NULL) == cases[i].result);
        if (cases[i].result == SKSE_BRIDGE_OK) {
            assert(size == expected_size && !memcmp(image, expected, size));
            free(image);
        } else {
            assert(!image && !size);
        }
        assert(!skyrim_lifecycle_scope_current().generation);
        assert(classic_native_load_finish(&load, &image, &size, NULL) == SKSE_BRIDGE_STATE && !image && !size);
    }
    assert(!fstatat(directory, "Fixture.ess", &after, AT_SYMLINK_NOFOLLOW));
    assert(skse_save_same_version(&before, &after));
    free(expected);
    skse_bridge_close(bridge);
    assert(!close(scratch) && !close(directory) && !close(root) && !munmap(MEM, map_size));
    puts("PASS native selected clone read and verified VM/slot restore through the lifted load");
    return 0;
}
