/* PapyrusUtil publication with an authored settings backend, driven in the
 * order of the staged value_invoke_owned (classic_papyrus_values.c.in). */
#define _DEFAULT_SOURCE
#include "x86.h"
#include "classic_papyrusutil.h"
#include "classic_vm_epoch.h"
#include <assert.h>
#include <pthread.h>
#include <regex.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint8_t *MEM;
static ClassicValueRequest captured;
static bool refuse_request;

bool skse_classic_values_request(const ClassicValueRequest *request) {
    if (refuse_request || captured.invoke) return false;
    captured = *request;
    return true;
}
bool skse_classic_values_retire(uint64_t epoch) { (void)epoch; return true; }

static struct { unsigned invokes, commits, discards; bool refuse_commit, fail_invoke; uint64_t next; } store;

static bool fake_invoke(void *ctx, unsigned method, const ClassicValue *args, size_t count, ClassicValue *result,
                        char *text, size_t capacity, uint64_t *ticket) {
    (void)ctx; (void)text; (void)capacity;
    assert(method >= 1 && method <= captured.count && count == captured.functions[method - 1].count);
    for (size_t i = 0; i < count; ++i) assert(args[i].type == captured.functions[method - 1].arguments[i]);
    ++store.invokes;
    if (store.fail_invoke) return false;
    *result = (ClassicValue){.type = captured.functions[method - 1].result, .bits = 1};
    *ticket = ++store.next;
    return true;
}
static bool fake_commit(void *ctx, uint64_t ticket) {
    (void)ctx; assert(ticket == store.next); ++store.commits; return !store.refuse_commit;
}
static void fake_discard(void *ctx, uint64_t ticket) { (void)ctx; assert(ticket == store.next); ++store.discards; }
static bool fake_read(void *ctx, CPU *c, uint32_t a, void *b, size_t n) { (void)ctx; (void)c; (void)a; memset(b, 0, n); return true; }
static bool fake_copy(void *ctx, CPU *c, uint32_t a, char *t, size_t n) { (void)ctx; (void)c; (void)a; (void)n; t[0] = 0; return true; }

static const ClassicValueFrame frame = {.object = 0x1000, .sp = 0x2000, .base = 0x2038, .registry = 0x3000,
                                        .stack_id = 7, .result = 0x202c, .state = 0x4000, .argument_list = 0x4100};
static uint32_t vm_result;  /* the VM value the staged code writes after an accepted commit */

static bool copy_keyword(void *ctx, CPU *c, const ClassicValueFrame *owner, uint32_t address, uint32_t *out) {
    (void)ctx;
    assert(c && owner->registry == frame.registry);
    if (address != 0x10020) return false;
    *out = 0x0401d9a9;
    return true;
}

static void check_keyword_publication(SkyrimPapyrusUtilBackend *backend) {
    backend->copy_form_id = copy_keyword;
    assert(skyrim_papyrusutil_publish(backend, 1) == 0);
    assert(captured.count == SKYRIM_PAPYRUSUTIL_FUNCTIONS + 2 && captured.copy_form_id);
    const char *names[] = {"GetIntValue", "SetIntValue"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) {
        const ClassicValueFunction *method = &captured.functions[SKYRIM_PAPYRUSUTIL_FUNCTIONS + i];
        assert(!strcmp(method->class_name, "StorageUtil") && !strcmp(method->name, names[i]));
        assert(method->result == CLASSIC_VALUES_INT && method->count == 3 &&
               method->arguments[0] == CLASSIC_VALUES_FORM &&
               method->arguments[1] == CLASSIC_VALUES_STRING && method->arguments[2] == CLASSIC_VALUES_INT);
    }
    skyrim_vm_epoch_registered(frame.registry);
    CPU cpu = {0}, other = {0};
    uint32_t id = 0;
    assert(!captured.copy_form_id(captured.context, &cpu, &frame, 0x10020, &id));
    assert(captured.owns_frame(captured.context, &cpu, captured.epoch, &frame));
    assert(!captured.copy_form_id(captured.context, &other, &frame, 0x10020, &id));
    assert(!captured.copy_form_id(captured.context, &cpu, &frame, 0x10024, &id));
    assert(captured.copy_form_id(captured.context, &cpu, &frame, 0x10020, &id) && id == 0x0401d9a9);
    captured.finished(captured.context, captured.epoch);
}

/* One Run, as value_invoke_owned orders the callbacks. revert_at: 0 none,
 * 1 after invoke, 2 after the second ownership check. */
static bool run(unsigned function, int revert_at) {
    CPU cpu = {0};
    ClassicValue args[4] = {{0}}, out = {0};
    const ClassicValueFunction *fn = &captured.functions[function - 1];
    for (unsigned i = 0; i < fn->count; ++i)
        args[i] = (ClassicValue){fn->arguments[i], 1, fn->arguments[i] == 2 ? "text" : NULL};
    bool accepted = false;
    if (captured.owns_frame(captured.context, &cpu, captured.epoch, &frame)) {
        const bool done = captured.invoke(captured.context, captured.epoch, function, args,
                                          captured.functions[function - 1].count, &out, NULL, 0);
        if (revert_at == 1) { skyrim_vm_epoch_before_revert(1); skyrim_vm_epoch_after_load(1); }
        accepted = done && captured.owns_frame(captured.context, &cpu, captured.epoch, &frame);
        if (revert_at == 2) { skyrim_vm_epoch_before_revert(2); skyrim_vm_epoch_after_load(1); }
        if (accepted) accepted = captured.commit(captured.context, captured.epoch, function);
        if (accepted) vm_result = out.bits;
        else captured.discard(captured.context, captured.epoch);
    }
    captured.finished(captured.context, captured.epoch);
    return accepted;
}

static atomic_int stage;
static bool revert_saw_result;
static void watch_revert(void *ctx, uint64_t old) { (void)ctx; (void)old; revert_saw_result = vm_result == 1; }
static void *reverter(void *unused) {
    (void)unused;
    while (atomic_load(&stage) != 1) usleep(1000);
    atomic_store(&stage, 2);
    skyrim_vm_epoch_before_revert(1);  /* blocks until the commit's result is written */
    skyrim_vm_epoch_after_load(1);
    return NULL;
}

static void expect(uint64_t calls, uint64_t stale, uint64_t commits, uint64_t discards, uint64_t errors) {
    SkyrimPapyrusUtilStats s;
    skyrim_papyrusutil_stats(&s);
    assert(s.published == SKYRIM_PAPYRUSUTIL_FUNCTIONS && s.calls == calls && s.refused_stale == stale && s.commits == commits
           && s.discards == discards && s.errors == errors);
}

int main(int argc, char **argv) {
    SkyrimPapyrusUtilBackend backend = {fake_invoke, fake_commit, fake_discard, fake_read, fake_copy, NULL, NULL};
    if (argc == 2 && !strcmp(argv[1], "keyword")) {
        check_keyword_publication(&backend);
        return 0;
    }
    assert(argc == 1);
    SkyrimPapyrusUtilBackend partial = backend;
    partial.commit = NULL;
    assert(skyrim_papyrusutil_publish(&partial, 1) != 0 && skyrim_papyrusutil_publish(&backend, 0) != 0);
    refuse_request = true;
    assert(skyrim_papyrusutil_publish(&backend, 1) != 0);
    refuse_request = false;
    assert(skyrim_papyrusutil_publish(&backend, 1) == 0 && skyrim_papyrusutil_publish(&backend, 1) != 0);
    assert(captured.count == SKYRIM_PAPYRUSUTIL_FUNCTIONS && captured.finished &&
           !strcmp(captured.functions[6].name, "Save") && !strcmp(captured.functions[11].name, "IntListSet"));
    assert(skyrim_vm_epoch_subscribe(watch_revert, NULL));
    const unsigned base_errors = 1;  /* the refused request above */

    /* Before the VM registered: refused before the DLL runs. */
    assert(!run(4, 0) && !store.invokes);
    expect(0, 1, 0, 0, base_errors);

    skyrim_vm_epoch_registered(0x3000);
    assert(run(4, 0) && store.commits == 1 && vm_result == 1);
    expect(1, 1, 1, 0, base_errors);

    /* A revert during the DLL call: no result, no file, the transaction is discarded. */
    vm_result = 0;
    assert(!run(4, 1) && store.commits == 1 && store.discards == 1 && !vm_result);
    expect(2, 2, 1, 1, base_errors);
    assert(!run(7, 2) && store.commits == 1 && store.discards == 2 && !vm_result);
    expect(3, 3, 1, 2, base_errors);

    /* After a failed load nothing runs until a later successful load. */
    skyrim_vm_epoch_before_revert(1);
    skyrim_vm_epoch_after_load(0);
    assert(!run(1, 0));
    expect(3, 4, 1, 2, base_errors);
    skyrim_vm_epoch_before_revert(1);
    skyrim_vm_epoch_after_load(1);

    /* The settings store refuses the commit: no VM result, and the lock is released. */
    store.refuse_commit = true;
    assert(!run(7, 0) && !vm_result && store.discards == 3);
    store.refuse_commit = false;
    SkyrimVmScope scope;
    assert(skyrim_vm_scope_current(&scope) && skyrim_vm_commit_begin(&scope));
    skyrim_vm_commit_end();
    expect(4, 4, 1, 3, base_errors + 1);

    /* The DLL call fails: nothing to commit or discard. */
    store.fail_invoke = true;
    assert(!run(2, 0));
    store.fail_invoke = false;
    expect(5, 4, 1, 3, base_errors + 2);

    /* A changed frame between checks is not this call's. */
    CPU cpu = {0};
    ClassicValueFrame other = frame;
    other.stack_id = 8;
    assert(captured.owns_frame(captured.context, &cpu, captured.epoch, &frame));
    assert(!captured.owns_frame(captured.context, &cpu, captured.epoch, &other));
    other = frame;
    other.argument_list = 0x4200;
    assert(!captured.owns_frame(captured.context, &cpu, captured.epoch, &other));
    CPU other_cpu = {0};
    assert(!captured.owns_frame(captured.context, &other_cpu, captured.epoch, &frame));
    captured.finished(captured.context, captured.epoch);

    other = frame;
    other.registry = 0x3400;
    assert(!captured.owns_frame(captured.context, &cpu, captured.epoch, &other));
    captured.finished(captured.context, captured.epoch);

    /* A revert waits until the committed result is written. */
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, reverter, NULL));
    ClassicValue args[3] = {{2, 0, "f"}, {2, 0, "k"}, {3, 1, NULL}}, out = {0};
    vm_result = 0;
    assert(captured.owns_frame(captured.context, &cpu, captured.epoch, &frame));
    assert(captured.invoke(captured.context, captured.epoch, 4, args, 3, &out, NULL, 0));
    assert(captured.owns_frame(captured.context, &cpu, captured.epoch, &frame));
    assert(captured.commit(captured.context, captured.epoch, 4));
    atomic_store(&stage, 1);
    while (atomic_load(&stage) != 2) usleep(1000);
    usleep(20000);
    vm_result = out.bits;
    captured.finished(captured.context, captured.epoch);
    assert(!pthread_join(thread, NULL) && revert_saw_result);

    fflush(stderr);
    char path[] = "/tmp/papyrusutil-XXXXXX";
    const int fd = mkstemp(path);
    const int saved = dup(2);
    dup2(fd, 2);
    skyrim_papyrusutil_report();
    fflush(stderr);
    dup2(saved, 2);
    char line[256] = {0};
    assert(pread(fd, line, sizeof line - 1, 0) > 0);
    close(fd);
    unlink(path);
    regex_t pattern;
    assert(!regcomp(&pattern, "^IOS_PAPYRUSUTIL schema=1 published=12 calls=6 refused_stale=5 commits=2 "
                    "discards=3 errors=3\n$", REG_EXTENDED));
    assert(!regexec(&pattern, line, 0, NULL, 0));
    regfree(&pattern);
    return 0;
}
