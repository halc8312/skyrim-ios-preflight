/* SPDX-License-Identifier: GPL-3.0-only */
#include "papyrus_trial.h"
#include <assert.h>
#include <string.h>

enum {
    MOCK_OWNERS = SKSE_PAPYRUS_TRIAL_MAX_OWNERS + 1, EPOCH = 9, REGISTRY = 0x10200,
    NATIVE_OWNER = 1, NATIVE_OBJECT = 0x12940, NATIVE_VTABLE = 0x30000,
    NATIVE_RESULT = 0x41000, NATIVE_STATE = 0x41100, NATIVE_STACK = 0xedab123
};

typedef struct {
    SksePapyrusTrialNative descriptor;
    uint32_t vtable, run, result_type, result_bits;
    uint32_t returned_eax, written_type, written_bits;
    unsigned runs;
    bool fault;
} NativeFixture;

typedef struct {
    SksePapyrusTrial *trial;
    NativeFixture *native;
    uint32_t registry, dead_owner, failing_owner, returned_eax;
    bool fault;
    size_t calls;
    SksePapyrusTrialCallback trace[SKSE_PAPYRUS_TRIAL_MAX_OWNERS];
} MockSession;

static uint32_t executable_start(uint32_t owner) { return UINT32_C(0x800000) + owner * 0x10000; }

static bool owns_callback(void *context, uint32_t owner, uint32_t address) {
    const MockSession *mock = context;
    if (owner >= MOCK_OWNERS || owner == mock->dead_owner) return false;
    const uint32_t start = executable_start(owner);
    return address >= start && address < start + 0x1000;
}

static bool owns_registry(void *context, uint32_t epoch, uint32_t address) {
    const MockSession *mock = context;
    return epoch == EPOCH && address == mock->registry;
}

static bool read_native_word(void *context, uint32_t owner, uint32_t address, uint32_t *out) {
    const MockSession *mock = context;
    const NativeFixture *native = mock->native;
    if (!native || owner != NATIVE_OWNER) return false;
    switch (address) {
    case NATIVE_OBJECT: *out = native->vtable; return true;
    case NATIVE_VTABLE + 4 * SKSE_CLASSIC_PAPYRUS_RUN_SLOT: *out = native->run; return true;
    case NATIVE_RESULT: *out = native->result_type; return true;
    case NATIVE_RESULT + 4: *out = native->result_bits; return true;
    default: return false;
    }
}

static bool owns_native_frame(void *context, uint32_t owner, uint32_t epoch, uint32_t stack_id,
                              uint32_t result, uint32_t state) {
    const MockSession *mock = context;
    return mock->native && owner == NATIVE_OWNER && epoch == EPOCH && stack_id == NATIVE_STACK &&
           result == NATIVE_RESULT && state == NATIVE_STATE;
}

static bool invoke_thiscall(void *context, uint32_t owner, uint32_t address, uint32_t self,
                             const uint32_t *args, size_t count, uint32_t *eax) {
    MockSession *mock = context;
    NativeFixture *native = mock->native;
    assert(owner == NATIVE_OWNER && address == native->run && self == NATIVE_OBJECT);
    const uint32_t expected[] = {0, mock->registry, NATIVE_STACK, NATIVE_RESULT, NATIVE_STATE};
    assert(count == 5 && !memcmp(args, expected, sizeof expected));
    ++native->runs;
    uint32_t nested = UINT32_C(0x5eed5eed);
    assert(skse_papyrus_trial_run_int0(mock->trial, &native->descriptor, NATIVE_STACK, NATIVE_RESULT,
                                       NATIVE_STATE, &nested) == SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER);
    assert(nested == UINT32_C(0x5eed5eed));
    if (native->fault) return false;
    native->result_type = native->written_type;
    native->result_bits = native->written_bits;
    *eax = native->returned_eax;
    return true;
}

static const SksePapyrusTrialNativeOps native_ops = {read_native_word, owns_native_frame, invoke_thiscall};

static void register_native(MockSession *mock, uint32_t registry) {
    NativeFixture *fixture = mock->native;
    SksePapyrusTrialNative *native = &fixture->descriptor;
    assert(SKSE_CLASSIC_PAPYRUS_REGISTER_SLOT == 0x17);
    assert(skse_papyrus_trial_capture_int0(mock->trial, registry + 4, NATIVE_OBJECT, native_ops, native) ==
           SKSE_PAPYRUS_TRIAL_REGISTRY_NOT_OWNED);
    assert(skse_papyrus_trial_capture_int0(mock->trial, registry, NATIVE_OBJECT + 4, native_ops, native) ==
           SKSE_PAPYRUS_TRIAL_NATIVE_NOT_OWNED);
    fixture->vtable = UINT32_MAX - 3;
    assert(skse_papyrus_trial_capture_int0(mock->trial, registry, NATIVE_OBJECT, native_ops, native) ==
           SKSE_PAPYRUS_TRIAL_NATIVE_NOT_OWNED);
    fixture->vtable = NATIVE_VTABLE;
    fixture->run = executable_start(0);
    assert(skse_papyrus_trial_capture_int0(mock->trial, registry, NATIVE_OBJECT, native_ops, native) ==
           SKSE_PAPYRUS_TRIAL_NATIVE_NOT_OWNED);
    assert(!native->trial);
    fixture->run = executable_start(NATIVE_OWNER) + 12;
    assert(skse_papyrus_trial_capture_int0(mock->trial, registry, NATIVE_OBJECT, native_ops, native) ==
           SKSE_PAPYRUS_TRIAL_OK);
    assert(skse_papyrus_trial_capture_int0(mock->trial, registry, NATIVE_OBJECT, native_ops, native) ==
           SKSE_PAPYRUS_TRIAL_NATIVE_ALREADY_CAPTURED);
    uint32_t value = 0;
    assert(skse_papyrus_trial_run_int0(mock->trial, native, NATIVE_STACK, NATIVE_RESULT, NATIVE_STATE, &value) ==
           SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER);
}

static bool invoke_cdecl(void *context, uint32_t owner, uint32_t address,
                         const uint32_t *args, size_t count, uint32_t *eax) {
    MockSession *mock = context;
    assert(owns_callback(context, owner, address));
    assert(count == 1 && args && args[0] == mock->registry);
    assert(mock->calls < SKSE_PAPYRUS_TRIAL_MAX_OWNERS);
    mock->trace[mock->calls++] = (SksePapyrusTrialCallback){owner, address};
    assert(skse_papyrus_trial_register(mock->trial, owner, address) == SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER);
    assert(skse_papyrus_trial_dispatch(mock->trial, EPOCH, args[0]).status == SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER);
    if (mock->native && owner == NATIVE_OWNER) register_native(mock, args[0]);
    if (owner == mock->failing_owner && mock->fault) return false;
    *eax = owner == mock->failing_owner ? mock->returned_eax : UINT32_C(0xa5b6c701);
    return true;
}

static const SksePapyrusTrialOps ops = {owns_callback, owns_registry, invoke_cdecl};

static void begin(SksePapyrusTrial *trial, MockSession *mock, uint32_t registry) {
    *mock = (MockSession){.trial = trial, .registry = registry, .dead_owner = UINT32_MAX,
                          .failing_owner = UINT32_MAX};
    assert(skse_papyrus_trial_begin(trial, EPOCH, ops, mock) == SKSE_PAPYRUS_TRIAL_OK);
}

static void test_interface(void) {
    const uint8_t expected[] = {0x5a, 1, 0, 0, 0, 0xb4, 0xe8, 0x45, 0x92, 0x5a};
    uint8_t bytes[sizeof expected];
    memset(bytes, 0x5a, sizeof bytes);
    assert(SKSE_CLASSIC_PAPYRUS_ID == 2 && SKSE_CLASSIC_PAPYRUS_VERSION == 1);
    assert(!skse_papyrus_trial_interface(bytes + 1, 7, UINT32_C(0x9245e8b4)));
    assert(!skse_papyrus_trial_interface(bytes + 1, 8, 0));
    assert(!skse_papyrus_trial_interface(NULL, 8, 1));
    for (size_t i = 0; i < sizeof bytes; ++i) assert(bytes[i] == 0x5a);
    assert(skse_papyrus_trial_interface(bytes + 1, SKSE_CLASSIC_PAPYRUS_TABLE_BYTES, UINT32_C(0x9245e8b4)));
    assert(!memcmp(bytes, expected, sizeof expected));
}

static void test_ownership_and_dispatch(void) {
    SksePapyrusTrial trial = {0}, other = {0};
    MockSession mock, separate;
    const uint32_t first = executable_start(1) + 3, second = executable_start(0);
    assert(skse_papyrus_trial_register(&trial, 0, second) == SKSE_PAPYRUS_TRIAL_ARGUMENT);
    assert(skse_papyrus_trial_begin(&trial, 0, ops, &mock) == SKSE_PAPYRUS_TRIAL_ARGUMENT);
    begin(&trial, &mock, REGISTRY);
    begin(&other, &separate, REGISTRY + 4);
    assert(skse_papyrus_trial_begin(&trial, EPOCH + 1, ops, &mock) == SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER);
    assert(skse_papyrus_trial_register(&trial, 0, first) == SKSE_PAPYRUS_TRIAL_CALLBACK_NOT_OWNED);
    assert(skse_papyrus_trial_register(&trial, 0, second + 0x1000) == SKSE_PAPYRUS_TRIAL_CALLBACK_NOT_OWNED);
    assert(skse_papyrus_trial_register(&trial, UINT32_MAX, second) == SKSE_PAPYRUS_TRIAL_ARGUMENT);
    assert(skse_papyrus_trial_register(&trial, 0, 0) == SKSE_PAPYRUS_TRIAL_ARGUMENT);
    assert(skse_papyrus_trial_register(&trial, 1, first) == SKSE_PAPYRUS_TRIAL_OK);
    assert(skse_papyrus_trial_register(&trial, 1, first) == SKSE_PAPYRUS_TRIAL_OK);
    assert(skse_papyrus_trial_register(&trial, 1, first + 1) == SKSE_PAPYRUS_TRIAL_OWNER_ALREADY_REGISTERED);
    assert(skse_papyrus_trial_register(&trial, 0, second) == SKSE_PAPYRUS_TRIAL_OK);
    assert(skse_papyrus_trial_register(&other, 0, second + 1) == SKSE_PAPYRUS_TRIAL_OK);

    assert(skse_papyrus_trial_dispatch(&trial, EPOCH + 1, REGISTRY).status == SKSE_PAPYRUS_TRIAL_REGISTRY_NOT_OWNED);
    assert(skse_papyrus_trial_dispatch(&trial, EPOCH, REGISTRY + 1).status == SKSE_PAPYRUS_TRIAL_ARGUMENT);
    assert(skse_papyrus_trial_dispatch(&trial, EPOCH, REGISTRY + 4).status == SKSE_PAPYRUS_TRIAL_REGISTRY_NOT_OWNED);
    assert(skse_papyrus_trial_dispatch(&other, EPOCH, REGISTRY).status == SKSE_PAPYRUS_TRIAL_REGISTRY_NOT_OWNED);
    assert(mock.calls == 0 && separate.calls == 0);

    const SksePapyrusTrialReport report = skse_papyrus_trial_dispatch(&trial, EPOCH, REGISTRY);
    assert(report.status == SKSE_PAPYRUS_TRIAL_OK && report.attempted == 2 && report.owner == UINT32_MAX);
    assert(mock.calls == 2 && separate.calls == 0);
    assert(mock.trace[0].owner == 1 && mock.trace[0].address == first);
    assert(mock.trace[1].owner == 0 && mock.trace[1].address == second);
    assert(skse_papyrus_trial_dispatch(&trial, EPOCH, REGISTRY).status == SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER);
    assert(skse_papyrus_trial_dispatch(&other, EPOCH, REGISTRY + 4).status == SKSE_PAPYRUS_TRIAL_OK);
    assert(separate.calls == 1 && separate.trace[0].address == second + 1);
}

static void test_limit_and_empty(void) {
    SksePapyrusTrial trial = {0}, empty = {0};
    MockSession mock, unused;
    begin(&trial, &mock, REGISTRY);
    begin(&empty, &unused, REGISTRY);
    for (uint32_t owner = 0; owner < SKSE_PAPYRUS_TRIAL_MAX_OWNERS; ++owner) {
        assert(skse_papyrus_trial_register(&trial, owner, executable_start(owner)) == SKSE_PAPYRUS_TRIAL_OK);
    }
    assert(skse_papyrus_trial_register(&trial, MOCK_OWNERS - 1,
                                       executable_start(MOCK_OWNERS - 1)) == SKSE_PAPYRUS_TRIAL_FULL);
    assert(skse_papyrus_trial_dispatch(&trial, EPOCH, REGISTRY).attempted == SKSE_PAPYRUS_TRIAL_MAX_OWNERS);
    assert(skse_papyrus_trial_dispatch(&empty, EPOCH, REGISTRY).status == SKSE_PAPYRUS_TRIAL_NO_CALLBACK);
    assert(skse_papyrus_trial_register(&empty, 0, executable_start(0)) == SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER);
    assert(unused.calls == 0);
}

static void test_changed_mapping(void) {
    SksePapyrusTrial trial = {0};
    MockSession mock;
    begin(&trial, &mock, REGISTRY);
    assert(skse_papyrus_trial_register(&trial, 0, executable_start(0)) == SKSE_PAPYRUS_TRIAL_OK);
    assert(skse_papyrus_trial_register(&trial, 1, executable_start(1)) == SKSE_PAPYRUS_TRIAL_OK);
    mock.dead_owner = 1;
    const SksePapyrusTrialReport report = skse_papyrus_trial_dispatch(&trial, EPOCH, REGISTRY);
    assert(report.status == SKSE_PAPYRUS_TRIAL_CALLBACK_NOT_OWNED && report.attempted == 0 && report.owner == 1);
    assert(mock.calls == 0);
    mock.dead_owner = UINT32_MAX;
    assert(skse_papyrus_trial_dispatch(&trial, EPOCH, REGISTRY).status == SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER);
}

static void test_failure(SksePapyrusTrialStatus status, bool fault, uint32_t result) {
    SksePapyrusTrial trial = {0};
    MockSession mock;
    begin(&trial, &mock, REGISTRY);
    mock.failing_owner = 1;
    mock.fault = fault;
    mock.returned_eax = result;
    assert(skse_papyrus_trial_register(&trial, 1, executable_start(1)) == SKSE_PAPYRUS_TRIAL_OK);
    assert(skse_papyrus_trial_register(&trial, 0, executable_start(0)) == SKSE_PAPYRUS_TRIAL_OK);
    const SksePapyrusTrialReport report = skse_papyrus_trial_dispatch(&trial, EPOCH, REGISTRY);
    assert(report.status == status && report.attempted == 1 && report.owner == 1 && mock.calls == 1);
    assert(skse_papyrus_trial_dispatch(&trial, EPOCH, REGISTRY).status == SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER);
}

static void begin_native(SksePapyrusTrial *trial, MockSession *mock, NativeFixture *fixture) {
    begin(trial, mock, REGISTRY);
    *fixture = (NativeFixture){.vtable = NATIVE_VTABLE, .run = executable_start(NATIVE_OWNER) + 12,
                               .returned_eax = UINT32_C(0xdab00501), .written_type = SKSE_CLASSIC_PAPYRUS_INT,
                               .written_bits = UINT32_C(0xfe123456)};
    mock->native = fixture;
    assert(skse_papyrus_trial_capture_int0(trial, REGISTRY, NATIVE_OBJECT, native_ops, &fixture->descriptor) ==
           SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER);
    assert(skse_papyrus_trial_register(trial, NATIVE_OWNER, executable_start(NATIVE_OWNER)) == SKSE_PAPYRUS_TRIAL_OK);
}

static SksePapyrusTrialStatus run_native(SksePapyrusTrial *trial, NativeFixture *fixture, uint32_t *out) {
    return skse_papyrus_trial_run_int0(trial, &fixture->descriptor, NATIVE_STACK, NATIVE_RESULT, NATIVE_STATE, out);
}

static void test_native_run(void) {
    SksePapyrusTrial trial = {0}, other = {0};
    MockSession mock, separate;
    NativeFixture fixture;
    begin_native(&trial, &mock, &fixture);
    begin(&other, &separate, REGISTRY);
    assert(skse_papyrus_trial_register(&other, NATIVE_OWNER, executable_start(NATIVE_OWNER)) == SKSE_PAPYRUS_TRIAL_OK);
    assert(skse_papyrus_trial_dispatch(&other, EPOCH, REGISTRY).status == SKSE_PAPYRUS_TRIAL_OK);
    assert(skse_papyrus_trial_dispatch(&trial, EPOCH, REGISTRY).status == SKSE_PAPYRUS_TRIAL_OK);
    assert(fixture.descriptor.epoch == EPOCH && fixture.descriptor.object == NATIVE_OBJECT &&
           fixture.descriptor.vtable == NATIVE_VTABLE && fixture.descriptor.run == fixture.run);
    uint32_t bits = 0;
    assert(run_native(&other, &fixture, &bits) == SKSE_PAPYRUS_TRIAL_NATIVE_NOT_OWNED);
    assert(run_native(&trial, &fixture, &bits) == SKSE_PAPYRUS_TRIAL_OK);
    assert(bits == UINT32_C(0xfe123456) && fixture.runs == 1);
    assert(run_native(&trial, &fixture, &bits) == SKSE_PAPYRUS_TRIAL_FRAME_REFUSED && fixture.runs == 1);
    fixture.result_type = fixture.result_bits = 0;
    fixture.written_bits = 7;
    assert(run_native(&trial, &fixture, &bits) == SKSE_PAPYRUS_TRIAL_OK && bits == 7 && fixture.runs == 2);
}

static void test_native_guard(void) {
    SksePapyrusTrial trial = {0};
    MockSession mock;
    NativeFixture fixture;
    begin_native(&trial, &mock, &fixture);
    assert(skse_papyrus_trial_dispatch(&trial, EPOCH, REGISTRY).status == SKSE_PAPYRUS_TRIAL_OK);
    uint32_t bits = 0x9999;
    assert(skse_papyrus_trial_run_int0(&trial, &fixture.descriptor, NATIVE_STACK, UINT32_MAX - 3,
                                       NATIVE_STATE, &bits) == SKSE_PAPYRUS_TRIAL_ARGUMENT);
    assert(skse_papyrus_trial_run_int0(&trial, &fixture.descriptor, NATIVE_STACK, NATIVE_RESULT,
                                       NATIVE_RESULT, &bits) == SKSE_PAPYRUS_TRIAL_FRAME_REFUSED);
    mock.registry += 4;
    assert(run_native(&trial, &fixture, &bits) == SKSE_PAPYRUS_TRIAL_REGISTRY_NOT_OWNED);
    mock.registry -= 4;
    fixture.run += 4;
    assert(run_native(&trial, &fixture, &bits) == SKSE_PAPYRUS_TRIAL_NATIVE_NOT_OWNED);
    fixture.run -= 4;
    mock.dead_owner = NATIVE_OWNER;
    assert(run_native(&trial, &fixture, &bits) == SKSE_PAPYRUS_TRIAL_NATIVE_NOT_OWNED);
    mock.dead_owner = UINT32_MAX;
    fixture.result_bits = 1;
    assert(run_native(&trial, &fixture, &bits) == SKSE_PAPYRUS_TRIAL_FRAME_REFUSED);
    assert(bits == 0x9999 && fixture.runs == 0);
    fixture.result_bits = 0;
    assert(run_native(&trial, &fixture, &bits) == SKSE_PAPYRUS_TRIAL_OK);
}

static void test_native_registration_fault(void) {
    SksePapyrusTrial trial = {0};
    MockSession mock;
    NativeFixture fixture;
    begin_native(&trial, &mock, &fixture);
    assert(skse_papyrus_trial_register(&trial, 0, executable_start(0)) == SKSE_PAPYRUS_TRIAL_OK);
    mock.failing_owner = 0;
    mock.fault = true;
    const SksePapyrusTrialReport report = skse_papyrus_trial_dispatch(&trial, EPOCH, REGISTRY);
    assert(fixture.descriptor.trial == &trial && report.status == SKSE_PAPYRUS_TRIAL_CALLBACK_FAULT);
    uint32_t bits = 0;
    assert(run_native(&trial, &fixture, &bits) == SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER);
    assert(fixture.runs == 0 && bits == 0);
}

static void test_native_failure(SksePapyrusTrialStatus status, bool fault, uint32_t eax, uint32_t type) {
    SksePapyrusTrial trial = {0};
    MockSession mock;
    NativeFixture fixture;
    begin_native(&trial, &mock, &fixture);
    assert(skse_papyrus_trial_dispatch(&trial, EPOCH, REGISTRY).status == SKSE_PAPYRUS_TRIAL_OK);
    fixture.fault = fault;
    fixture.returned_eax = eax;
    fixture.written_type = type;
    uint32_t bits = UINT32_C(0xfeed1234);
    assert(run_native(&trial, &fixture, &bits) == status);
    assert(bits == UINT32_C(0xfeed1234) && fixture.runs == 1);
    assert(run_native(&trial, &fixture, &bits) == SKSE_PAPYRUS_TRIAL_OUT_OF_ORDER && fixture.runs == 1);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "interface")) test_interface();
    else if (!strcmp(argv[1], "owners")) test_ownership_and_dispatch();
    else if (!strcmp(argv[1], "limit")) test_limit_and_empty();
    else if (!strcmp(argv[1], "mapping")) test_changed_mapping();
    else if (!strcmp(argv[1], "fault")) test_failure(SKSE_PAPYRUS_TRIAL_CALLBACK_FAULT, true, 0);
    else if (!strcmp(argv[1], "declined")) test_failure(SKSE_PAPYRUS_TRIAL_CALLBACK_DECLINED, false, UINT32_C(0xdeadde00));
    else if (!strcmp(argv[1], "bad-bool")) test_failure(SKSE_PAPYRUS_TRIAL_CALLBACK_BAD_BOOL, false, 2);
    else if (!strcmp(argv[1], "native")) test_native_run();
    else if (!strcmp(argv[1], "native-guard")) test_native_guard();
    else if (!strcmp(argv[1], "native-registration-fault")) test_native_registration_fault();
    else if (!strcmp(argv[1], "native-fault")) test_native_failure(SKSE_PAPYRUS_TRIAL_NATIVE_FAULT, true, 1, 3);
    else if (!strcmp(argv[1], "native-declined"))
        test_native_failure(SKSE_PAPYRUS_TRIAL_CALLBACK_DECLINED, false, UINT32_C(0x12345a00), 3);
    else if (!strcmp(argv[1], "native-type")) test_native_failure(SKSE_PAPYRUS_TRIAL_RESULT_REFUSED, false, 1, 4);
    else return 1;
    return 0;
}
