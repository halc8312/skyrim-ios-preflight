/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE 1
#include "rt.h"
#include "runtime_save.h"
#include "save_attempt.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #cond); exit(1); } \
} while (0)

enum {
    STACK = 0x7000, OTHER_STACK = 0x8000, TEB_BASE = 0x9000, OTHER_TEB = 0xa000,
    NAME_TEMP = 0x10000, NAME_FINAL = 0x10100, NAME_BACKUP = 0x10200,
    DATA = 0x10300, COUNT = 0x10400, OVERLAPPED = 0x10500,
    OPEN_RETURN = 0xf6bf24, WRITE_RETURN = 0xf6a564, CLOSE_RETURN = 0xf67fc8, MOVE_RETURN = 0xf53f17,
};
typedef enum { ACCEPT, SHORT, SHORT_EX, ERROR_EX, NO_STREAM_FLUSH, FLUSH_FALSE,
               CLOSE_ERR, CLOSE_WRONG_SITE, MOVE_ERR, SWAPPED_TEMP, NO_SCOPE_OPEN,
               NO_SCOPE_MOVE, DIFFERENT_THREAD } Scenario;

static const char sample[] = "test";
static const char temp_leaf[] = "Fixture.ess.tmp", final_leaf[] = "Fixture.ess";
static const char backup_leaf[] = "Fixture.ess.bak";

uint8_t *MEM;

static uint32_t invoke(CPU *cpu, enum ClassicFileCall operation, uint32_t ret,
                       size_t arguments, const uint32_t *values) {
    const uint32_t stack = cpu->fs_base == TEB_BASE ? STACK : OTHER_STACK;
    cpu->r[4] = stack;
    M32(stack) = ret;
    for (size_t i = 0; i < arguments; ++i) M32(stack + 4 + i * 4) = values[i];
    classic_runtime_file_call(cpu, operation);
    CHECK(cpu->r[4] == stack + 4 + arguments * 4 && M32(stack) == ret);
    return cpu->r[0];
}

static uint32_t open_temp(CPU *cpu, uint32_t access, unsigned disposition) {
    const uint32_t args[] = {NAME_TEMP, access, 1, 0, disposition, 0, 0};
    return invoke(cpu, ClassicCreateFile, OPEN_RETURN, 7, args);
}

static uint32_t write_file(CPU *cpu, uint32_t handle, bool ex) {
    const uint32_t args[] = {handle, DATA, sizeof sample - 1, ex ? OVERLAPPED : COUNT, 0};
    return invoke(cpu, ex ? ClassicWriteFileEx : ClassicWriteFile, WRITE_RETURN, 5, args);
}

static uint32_t move_file(CPU *cpu, uint32_t from, uint32_t to) {
    const uint32_t args[] = {from, to};
    return invoke(cpu, ClassicMoveFile, MOVE_RETURN, 2, args);
}

static void write_leaf(int dir, const char *name, const char *contents) {
    const int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    const size_t length = strlen(contents);
    CHECK(fd >= 0 && write(fd, contents, length) == (ssize_t)length && !close(fd));
}

static void *write_from_other_thread(void *context) {
    CPU cpu = {.fs_base = OTHER_TEB};
    CHECK(write_file(&cpu, *(const uint32_t *)context, false) == 1);
    return NULL;
}

static void actual_write(CPU *cpu, uint32_t handle, Scenario scenario, ClassicRuntimeSave *save) {
    if (scenario == DIFFERENT_THREAD) {
        pthread_t other;
        CHECK(!pthread_create(&other, NULL, write_from_other_thread, &handle));
        CHECK(!pthread_join(other, NULL));
        CHECK(!save->writes && M32(COUNT) == sizeof sample - 1);
        return;
    }

    const bool short_write = scenario == SHORT || scenario == SHORT_EX;
    const bool write_error = scenario == ERROR_EX;
    const bool ex = scenario == SHORT_EX || write_error;
    struct rlimit original;
    if (short_write || write_error) {
        CHECK(!getrlimit(RLIMIT_FSIZE, &original));
        struct rlimit limit = original;
        limit.rlim_cur = write_error ? 0 : 1;
        CHECK(!setrlimit(RLIMIT_FSIZE, &limit));
    }
    memset(MEM + OVERLAPPED, 0, 20);
    const uint32_t result = write_file(cpu, handle, ex);
    if (short_write || write_error) CHECK(!setrlimit(RLIMIT_FSIZE, &original));
    CHECK(result == 1);
    CHECK(M32(ex ? OVERLAPPED + 4 : COUNT) == (write_error ? 0 : short_write ? 1 : sizeof sample - 1));
    if (ex) CHECK(M32(OVERLAPPED) == (write_error ? 0xc0000011u : 0));
    if (scenario != NO_SCOPE_OPEN) {
        CHECK(save->writes == 1 && save->last_requested == sizeof sample - 1);
        CHECK(save->last_written == (write_error ? -1 : short_write ? 1 : (ssize_t)sizeof sample - 1));
    } else CHECK(!save->writes);
}

static int case_directory(int root, const char *root_path, const char *directory_name) {
    CHECK(!mkdirat(root, directory_name, 0700));
    const int dir = openat(root, directory_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(dir >= 0);
    const int mapped = snprintf(g_save_dir, sizeof g_save_dir, "%s/%s", root_path, directory_name);
    CHECK(mapped > 0 && (size_t)mapped < sizeof g_save_dir);
    return dir;
}

static void run_case(int root, const char *root_path, Scenario scenario) {
    char directory_name[16];
    const int length = snprintf(directory_name, sizeof directory_name, "case-%d", scenario);
    CHECK(length > 0 && (size_t)length < sizeof directory_name);
    const int dir = case_directory(root, root_path, directory_name);
    if (scenario == MOVE_ERR) CHECK(!mkdirat(dir, final_leaf, 0700));
    else write_leaf(dir, final_leaf, "old");

    CPU cpu = {.fs_base = TEB_BASE};
    ClassicRuntimeSave save;
    CHECK(classic_runtime_save_begin(&save, &cpu, dir, g_save_dir, final_leaf));
    if (scenario != NO_SCOPE_OPEN) classic_runtime_save_open_scope(&cpu, true);
    CHECK(open_temp(&cpu, 0xc0000000u, 3) == UINT32_MAX && rt_get_last_error(&cpu) == 2);
    const uint32_t handle = open_temp(&cpu, 0x40000000u, 2);
    CHECK(handle != UINT32_MAX && !rt_get_last_error(&cpu));
    if (scenario != NO_SCOPE_OPEN) {
        classic_runtime_save_open_scope(&cpu, false);
        CHECK(save.guest_handle == handle && save.file.writer >= 0);
        CHECK((fcntl(save.file.writer, F_GETFL) & O_ACCMODE) == O_WRONLY);
    } else CHECK(!save.guest_handle);

    actual_write(&cpu, handle, scenario, &save);
    CHECK(invoke(&cpu, ClassicFlushFileBuffers, 0xf60000, 1, &handle) == 1);
    if (scenario != NO_SCOPE_OPEN && scenario != DIFFERENT_THREAD) CHECK(save.durability_stub_calls == 1);
    if (scenario != NO_STREAM_FLUSH) classic_runtime_save_flush_result(&cpu, scenario != FLUSH_FALSE);
    if (scenario == CLOSE_ERR) CHECK(save.file.writer >= 0 && !close(save.file.writer));
    const uint32_t close_site = scenario == CLOSE_WRONG_SITE ? CLOSE_RETURN + 1 : CLOSE_RETURN;
    CHECK(invoke(&cpu, ClassicCloseHandle, close_site, 1, &handle) == 1);
    classic_runtime_save_crt_close_result(&cpu, true);
    if (scenario == CLOSE_ERR) CHECK(save.os_close_seen && save.close_result == -1 && save.close_error == EBADF);
    if (scenario == SWAPPED_TEMP) {
        CHECK(!unlinkat(dir, temp_leaf, 0));
        write_leaf(dir, temp_leaf, sample);
    }
    if (scenario != MOVE_ERR) {
        CHECK(move_file(&cpu, NAME_FINAL, NAME_BACKUP) == 1);
        struct stat backup;
        CHECK(!fstatat(dir, backup_leaf, &backup, AT_SYMLINK_NOFOLLOW) && backup.st_size == 3);
        CHECK(!save.move_seen && classic_runtime_save_reader(&save) == -1);
    }
    if (scenario != NO_SCOPE_MOVE) classic_runtime_save_final_scope(&cpu);
    const uint32_t before_move_error = rt_get_last_error(&cpu);
    CHECK(move_file(&cpu, NAME_TEMP, NAME_FINAL) == (scenario != MOVE_ERR));
    CHECK(before_move_error == rt_get_last_error(&cpu));
    /* Current CRT can see stale LastError even when the actual rename failed. */
    classic_runtime_save_crt_move_result(&cpu, true);
    if (scenario == MOVE_ERR) CHECK(save.move_seen && !save.renamed && save.move_error > 0);
    if (scenario == SWAPPED_TEMP) CHECK(save.file.outcome == CLASSIC_SAVE_IDENTITY);
    if (scenario == SHORT || scenario == SHORT_EX || scenario == ERROR_EX) CHECK(save.file.outcome == CLASSIC_SAVE_WRITE);
    if (scenario == FLUSH_FALSE) CHECK(save.file.outcome == CLASSIC_SAVE_FLUSH);

    const int reader = classic_runtime_save_reader(&save);
    if (scenario == ACCEPT) {
        struct stat retained, named;
        char bytes[sizeof sample];
        CHECK(reader >= 0 && (fcntl(reader, F_GETFL) & O_ACCMODE) == O_RDONLY);
        CHECK(pread(reader, bytes, sizeof bytes, 0) == sizeof sample - 1 && !memcmp(bytes, sample, sizeof sample - 1));
        CHECK(!fstat(reader, &retained) && !fstatat(dir, final_leaf, &named, AT_SYMLINK_NOFOLLOW));
        CHECK(retained.st_ino == named.st_ino && retained.st_dev == named.st_dev);
    } else CHECK(reader < 0);
    classic_runtime_save_end(&save);
    if (reader >= 0) CHECK(fcntl(reader, F_GETFL) < 0 && errno == EBADF);
    CHECK(!close(dir));
}

static void stale_handle_after_end(int root, const char *root_path) {
    const int dir = case_directory(root, root_path, "previous-observation");
    CPU cpu = {.fs_base = TEB_BASE};
    ClassicRuntimeSave previous, current;
    CHECK(classic_runtime_save_begin(&previous, &cpu, dir, g_save_dir, final_leaf));
    classic_runtime_save_open_scope(&cpu, true);
    uint32_t old_handle = open_temp(&cpu, 0x40000000u, 2);
    classic_runtime_save_open_scope(&cpu, false);
    CHECK(old_handle == previous.guest_handle && previous.file.writer >= 0);
    classic_runtime_save_end(&previous);
    CHECK(!unlinkat(dir, temp_leaf, 0));

    CHECK(classic_runtime_save_begin(&current, &cpu, dir, g_save_dir, final_leaf));
    classic_runtime_save_open_scope(&cpu, true);
    uint32_t new_handle = open_temp(&cpu, 0x40000000u, 2);
    classic_runtime_save_open_scope(&cpu, false);
    CHECK(new_handle == current.guest_handle && new_handle != old_handle && current.token != previous.token);
    CHECK(write_file(&cpu, old_handle, false) == 1);
    CHECK(invoke(&cpu, ClassicCloseHandle, CLOSE_RETURN, 1, &old_handle) == 1);
    CHECK(!current.writes && !current.os_close_seen);
    actual_write(&cpu, new_handle, ACCEPT, &current);
    classic_runtime_save_flush_result(&cpu, true);
    CHECK(invoke(&cpu, ClassicCloseHandle, CLOSE_RETURN, 1, &new_handle) == 1);
    classic_runtime_save_crt_close_result(&cpu, true);
    classic_runtime_save_final_scope(&cpu);
    CHECK(move_file(&cpu, NAME_TEMP, NAME_FINAL) == 1);
    classic_runtime_save_crt_move_result(&cpu, true);
    CHECK(classic_runtime_save_reader(&current) >= 0);
    classic_runtime_save_end(&current);
    CHECK(!close(dir));
}

typedef struct { unsigned count; int reader; } AttemptReport;

static void report_attempt(void *context, int dir, const char *leaf, int ess,
                           const SkyrimLifecycleScope *trace, uint32_t slot_ticket) {
    AttemptReport *report = context;
    CHECK(!report->count && ess >= 0 && !strcmp(leaf, final_leaf));
    struct stat held, named;
    CHECK(!fstat(ess, &held) && !fstatat(dir, leaf, &named, AT_SYMLINK_NOFOLLOW));
    CHECK(held.st_dev == named.st_dev && held.st_ino == named.st_ino && held.st_size == sizeof sample - 1);
    CHECK(trace->count == 4 && !trace->invalid && trace->current == 0);
    CHECK(trace->rows[0].site == SkyrimSaveSlot && trace->rows[0].ticket == slot_ticket);
    CHECK(trace->rows[1].site == SkyrimSaveVM && trace->rows[1].parent == slot_ticket);
    CHECK(trace->rows[2].returning && trace->rows[2].outcome == SkyrimReturnedTrue);
    CHECK(trace->rows[3].returning && trace->rows[3].ticket == slot_ticket);
    report->reader = ess;
    ++report->count;
}

static void run_notification_case(int root, const char *root_path, Scenario scenario, bool vm_ok) {
    char subdir[32];
    CHECK(snprintf(subdir, sizeof subdir, "notice-%u-%d", (unsigned)scenario, vm_ok) > 0);
    const int dir = case_directory(root, root_path, subdir);
    if (scenario == MOVE_ERR) CHECK(!mkdirat(dir, final_leaf, 0700));
    CPU cpu = {.fs_base = TEB_BASE}, other = {.fs_base = OTHER_TEB};
    ClassicSaveAttempt attempt;
    AttemptReport report = {0, -1};
    errno = ERANGE;
    const uint64_t cancelled = classic_save_attempt_begin(&attempt, &cpu, dir, g_save_dir, final_leaf,
                                                          report_attempt, &report);
    CHECK(cancelled && errno == ERANGE);
    classic_save_attempt_cancel(&cpu, cancelled);
    CHECK(errno == ERANGE && !report.count);
    const uint64_t ticket = classic_save_attempt_begin(&attempt, &cpu, dir, g_save_dir, final_leaf,
                                                       report_attempt, &report);
    CHECK(ticket && ticket != cancelled);
    classic_save_attempt_event(&cpu, cancelled, SkyrimSaveAttemptLeft, 0);
    classic_save_attempt_event(&other, ticket, SkyrimSaveAttemptLeft, 0);
    CHECK(!classic_save_attempt_current(&cpu, SkyrimSaveInSlot));
    const uint32_t handle = open_temp(&cpu, 0x40000000u, 2);
    CHECK(handle == attempt.runtime.guest_handle && handle != UINT32_MAX);
    classic_save_attempt_event(&cpu, ticket, SkyrimSaveOpenFinished, 0);
    classic_save_attempt_event(&cpu, ticket, SkyrimSaveSlotEntered, 0);
    CHECK(classic_save_attempt_current(&cpu, SkyrimSaveInSlot) == ticket);
    CHECK(!classic_save_attempt_current(&other, SkyrimSaveInSlot));
    actual_write(&cpu, handle, scenario, &attempt.runtime);
    classic_save_attempt_event(&cpu, ticket, SkyrimSaveVmReturned, vm_ok);
    classic_save_attempt_event(&cpu, ticket, SkyrimSaveSlotLeft, 0);
    classic_save_attempt_event(&cpu, ticket, SkyrimSaveCloseEntered, 0);
    CHECK(classic_save_attempt_current(&cpu, SkyrimSaveInClose) == ticket);
    classic_save_attempt_event(&cpu, ticket, SkyrimSaveFlushReturned, 1);
    if (scenario == CLOSE_ERR) CHECK(!close(attempt.runtime.file.writer));
    CHECK(invoke(&cpu, ClassicCloseHandle, CLOSE_RETURN, 1, &handle) == 1);
    classic_save_attempt_event(&cpu, ticket, SkyrimSaveCrtCloseReturned, 1);
    if (scenario == SWAPPED_TEMP) {
        CHECK(!unlinkat(dir, temp_leaf, 0));
        write_leaf(dir, temp_leaf, sample);
    }
    classic_save_attempt_event(&cpu, ticket, SkyrimSaveMoveEntered, 0);
    CHECK(move_file(&cpu, NAME_TEMP, NAME_FINAL) == (scenario != MOVE_ERR));
    classic_save_attempt_event(&cpu, ticket, SkyrimSaveCrtMoveReturned, 1);
    classic_save_attempt_event(&cpu, ticket, SkyrimSaveCloseLeft, 0);
    CHECK(!report.count);
    classic_save_attempt_event(&cpu, ticket, SkyrimSaveAttemptLeft, 0);
    classic_save_attempt_event(&cpu, ticket, SkyrimSaveAttemptLeft, 0);
    CHECK(report.count == (unsigned)(scenario == ACCEPT && vm_ok));
    if (report.reader >= 0) CHECK(fcntl(report.reader, F_GETFL) < 0 && errno == EBADF);
    CHECK(!close(dir));
}

int main(int argc, char **argv) {
    CHECK(argc == 2 && signal(SIGXFSZ, SIG_IGN) != SIG_ERR);
    MEM = calloc(1, 1u << 20);
    CHECK(MEM);
    strcpy(GSTR(NAME_TEMP), "C:\\Users\\Player\\Fixture.ess.tmp");
    strcpy(GSTR(NAME_FINAL), "C:\\Users\\Player\\Fixture.ess");
    strcpy(GSTR(NAME_BACKUP), "C:\\Users\\Player\\Fixture.ess.bak");
    memcpy(MEM + DATA, sample, sizeof sample - 1);
    const int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(root >= 0);
    for (Scenario scenario = ACCEPT; scenario <= DIFFERENT_THREAD; ++scenario) run_case(root, argv[1], scenario);
    stale_handle_after_end(root, argv[1]);
    run_notification_case(root, argv[1], ACCEPT, true);
    run_notification_case(root, argv[1], ACCEPT, false);
    run_notification_case(root, argv[1], SHORT, true);
    run_notification_case(root, argv[1], CLOSE_ERR, true);
    run_notification_case(root, argv[1], MOVE_ERR, true);
    run_notification_case(root, argv[1], SWAPPED_TEMP, true);
    CHECK(!close(root));
    free(MEM);
    puts("PASS real Skydream file shims, unchanged guest status, and scoped save evidence");
    return 0;
}
