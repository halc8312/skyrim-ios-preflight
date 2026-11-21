/* SPDX-License-Identifier: GPL-3.0-only */
#include "fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    GUEST_BYTES = 0x1bc0000, HEAP_START = 0x10000, HEAP_LIMIT = 0x50000,
    POLICY_ROOT = 0x1ba7008, POLICY = 0x2100, REGISTRY = 0x1100, STALE_REGISTRY = 0x3100,
    COPY_STACK = 0x6100, COPY_OUT = 0x7100, VM_DESTROY = 0xc328e0,
    VM_VALUE_COPY = 0xc329e0, VM_ARRAY_RESIZE = 0x8c2a70, STRING_SET = 0xa51210, STRING_RELEASE = 0xa511b0,
    EVENT_DESTROY_THUNK = 0x401010, EVENT_COPY_THUNK = 0x401020, EPOCH = 47,
};

#define FIRST UINT64_C(0x00000001fffffff0)
#define SECOND UINT64_C(0x0000000200000010)
static const char oil[] = "WearableLanterns_ForceOilMeterDisplay";
static const char pollen[] = "WearableLanterns_ForcePollenMeterDisplay";
static const char remove_oil[] = "WearableLanterns_RemoveOilMeter";
static const char remove_pollen[] = "WearableLanterns_RemovePollenMeter";

typedef struct { uint32_t address; unsigned size; bool live; } Allocation;
typedef struct {
    uint64_t handle, epoch;
    unsigned argc;
    uint32_t callback, values;
} QueuedEvent;
typedef struct { CPU *cpu; uint64_t epoch; } DeliveryOwner;

uint8_t *MEM;
static Allocation allocations[200];
static unsigned used, refs_first, refs_second, output_count, attempted;
static uint32_t top = HEAP_START, queue_vtable;
static uint64_t queue_epoch = EPOCH;
static void (*event_copy)(CPU *);
static QueuedEvent output[12];
static bool refuse_resize;

uint32_t gmalloc(unsigned bytes) {
    CHECK(bytes && used < sizeof(allocations) / sizeof(allocations[0]));
    CHECK(bytes < HEAP_LIMIT - top);
    const uint32_t result = top;
    top += (bytes + 7u) & ~7u;
    allocations[used++] = (Allocation){result, bytes, true};
    memset(MEM + result, 0, bytes);
    return result;
}

static bool allocation_live(uint32_t address) {
    for (unsigned i = 0; i < used; ++i)
        if (allocations[i].address == address) return allocations[i].live;
    return false;
}

void gfree(uint32_t address) {
    for (unsigned i = 0; i < used; ++i) {
        Allocation *entry = &allocations[i];
        if (entry->address != address) continue;
        CHECK(entry->live);
        entry->live = false;
        memset(MEM + address, 0xdd, entry->size);
        return;
    }
    CHECK(false);
}

uint32_t gstrdup(const char *text) {
    const size_t size = strlen(text) + 1;
    CHECK(size <= 256);
    uint32_t address = gmalloc((unsigned)size);
    memcpy(MEM + address, text, size);
    return address;
}

void rt_fatal(const char *reason) {
    fprintf(stderr, "%s\n", reason);
    exit(1);
}

uint32_t rt_thunk(void (*callback)(CPU *), const char *name) {
    CHECK(callback);
    if (!strcmp(name, "SKSE.Event.Copy")) {
        CHECK(!event_copy);
        event_copy = callback;
        return EVENT_COPY_THUNK;
    }
    CHECK(!strcmp(name, "SKSE.Event.Destroy"));
    return EVENT_DESTROY_THUNK;
}

static void clear_value(uint32_t slot) {
    if (M32(slot) == 2 && M32(slot + 4)) gfree(M32(slot + 4));
    M32(slot) = M32(slot + 4) = 0;
}

uint32_t skse_member(CPU *cpu, uint32_t address, uint32_t self, int count, const uint32_t *args) {
    (void)cpu;
    switch (address) {
    case VM_DESTROY:
        CHECK(!count);
        clear_value(self);
        return 0;
    case VM_VALUE_COPY:
        CHECK(count == 1 && !M32(self));
        M32(self) = M32(args[0]);
        M32(self + 4) = M32(args[0] + 4);
        if (M32(self) == 2 && M32(self + 4)) M32(self + 4) = gstrdup((char *)MEM + M32(self + 4));
        return 0;
    case VM_ARRAY_RESIZE:
        CHECK(count == 1 && self == COPY_OUT && args[0] <= CLASSIC_MOD_EVENT_BOOL_MAX);
        if (refuse_resize) return 0;
        M32(self) = args[0];
        M32(self + 4) = args[0] ? gmalloc(args[0] * 8) : 0;
        return 1;
    case STRING_SET:
        CHECK(count == 1 && !M32(self));
        M32(self) = gstrdup((char *)MEM + args[0]);
        return self;
    case STRING_RELEASE:
        CHECK(!count && M32(self));
        gfree(M32(self));
        M32(self) = 0;
        return 0;
    default:
        CHECK(false);
    }
}

static unsigned *handle_refs(uint64_t handle) {
    CHECK(handle == FIRST || handle == SECOND);
    return handle == FIRST ? &refs_first : &refs_second;
}

uint32_t skse_virtual(CPU *cpu, uint32_t self, unsigned slot, int count, const uint32_t *args) {
    uint64_t handle = (uint64_t)args[0] | ((uint64_t)args[1] << 32);
    unsigned *refs = handle_refs(handle);
    if (self == POLICY) {
        CHECK(count == 2 && (slot == 9 || slot == 10));
        if (slot == 9) ++*refs;
        else { CHECK(*refs); --*refs; }
        return 0;
    }
    CHECK(self == REGISTRY && queue_epoch && slot == 35 && count == 4 && *refs == 1);
    CHECK(M32(args[2]) && event_copy);
    const uint32_t table = M32(args[3]);
    CHECK(M32(table) == EVENT_DESTROY_THUNK && M32(table + 4) == EVENT_COPY_THUNK);
    if (queue_vtable) CHECK(table == queue_vtable);
    queue_vtable = table;
    ++attempted;
    /* The authored VM accepts value copies in the same synchronous callback
     * shape as the pinned guest. It never keeps the temporary argument object. */
    CPU callback_cpu = *cpu;
    callback_cpu.r[1] = args[3];
    callback_cpu.r[4] = COPY_STACK;
    M32(COPY_STACK) = 0xc4fe05;
    M32(COPY_STACK + 4) = COPY_OUT;
    M32(COPY_OUT) = M32(COPY_OUT + 4) = 0;
    event_copy(&callback_cpu);
    CHECK(callback_cpu.r[4] == COPY_STACK + 8);
    if (refuse_resize) {
        CHECK(!callback_cpu.r[0] && !M32(COPY_OUT + 4));
        return 0;
    }
    CHECK(callback_cpu.r[0] == 1 && M32(COPY_OUT) == M32(args[3] + 4));
    CHECK(output_count < sizeof(output) / sizeof(output[0]));
    const uint32_t values = M32(COPY_OUT + 4);
    const unsigned argc = M32(COPY_OUT);
    QueuedEvent *event = &output[output_count++];
    *event = (QueuedEvent){.handle = handle, .epoch = queue_epoch, .argc = argc,
                           .callback = gstrdup((char *)MEM + M32(args[2])),
                           .values = argc ? gmalloc(argc * 8) : 0};
    for (unsigned i = 0; i < argc; ++i) {
        const uint32_t source = values + i * 8u;
        skse_member(cpu, VM_VALUE_COPY, event->values + i * 8u, 1, &source);
        clear_value(source);
    }
    if (values) gfree(values);
    return 0;
}

static void enqueue_to_vm(void *context, const ClassicModEventPacket *packet) {
    const DeliveryOwner *owner = context;
    CHECK(owner->epoch == queue_epoch);
    classic_fixture_send_typed(owner->cpu, REGISTRY, owner->epoch, packet);
}

static const char *queued_callback(unsigned index) {
    CHECK(index < output_count && output[index].epoch == queue_epoch);
    CHECK(allocation_live(output[index].callback));
    return (const char *)MEM + output[index].callback;
}

static uint32_t queued_argument(unsigned index, unsigned arg, unsigned type) {
    CHECK(index < output_count && output[index].epoch == queue_epoch);
    const QueuedEvent *event = &output[index];
    CHECK(arg < event->argc && allocation_live(event->values));
    const uint32_t value = event->values + arg * 8;
    CHECK(M32(value) == type);
    return M32(value + 4);
}

static void check_queued_string(unsigned index, unsigned arg, const char *expected) {
    const uint32_t text = queued_argument(index, arg, 2);
    CHECK(allocation_live(text) && !strcmp((const char *)MEM + text, expected));
}

static void release_queue(void) {
    for (unsigned i = 0; i < output_count; ++i) {
        QueuedEvent *event = &output[i];
        CHECK(event->epoch == queue_epoch);
        for (unsigned j = 0; j < event->argc; ++j) clear_value(event->values + j * 8);
        if (event->values) gfree(event->values);
        gfree(event->callback);
        *event = (QueuedEvent){0};
    }
    output_count = 0;
}

static void check_meter_delivery(void) {
    ClassicModEvents events;
    classic_mod_events_reset(&events, EPOCH);
    char oil_callback[] = "ForceMeterDisplay";
    const FixtureSubscription listeners[] = {
        {SECOND, REGISTRY, EPOCH, oil, oil_callback},
        {FIRST, STALE_REGISTRY, EPOCH, oil, "OtherRegistry"},
        {FIRST, REGISTRY, EPOCH, pollen, "ForceMeterDisplay"},
        {SECOND, REGISTRY, EPOCH, remove_pollen, "RemoveMeter"},
        {FIRST, REGISTRY, EPOCH, oil, "MonitorOil"},
        {FIRST, REGISTRY, EPOCH, remove_oil, "RemoveMeter"},
    };
    classic_fixture_subscribe(listeners, sizeof(listeners) / sizeof(listeners[0]));
    CPU cpu = {.r = {0xaabb, 0xbbcc, 0xccdd}, .fs_base = 0x1000};
    const CPU original = cpu;
    DeliveryOwner owner = {.cpu = &cpu, .epoch = EPOCH};
    char copied_name[sizeof(oil)];
    memcpy(copied_name, oil, sizeof(oil));
    int32_t handle = classic_mod_event_create(&events, EPOCH, 3, copied_name, strlen(copied_name));
    CHECK(handle == 1 && classic_mod_event_push_bool(&events, EPOCH, handle, true));
    memset(copied_name, 'x', sizeof(copied_name));
    CHECK(classic_mod_event_send(&events, EPOCH, handle, enqueue_to_vm, &owner));
    CHECK(!classic_mod_event_send(&events, EPOCH, handle, enqueue_to_vm, &owner));
    memset(oil_callback, '?', sizeof(oil_callback) - 1);
    CHECK(output_count == 2 && output[0].handle == FIRST && output[1].handle == SECOND);
    CHECK(!strcmp(queued_callback(0), "MonitorOil") && !strcmp(queued_callback(1), "ForceMeterDisplay"));
    for (unsigned i = 0; i < 2; ++i)
        CHECK(output[i].argc == 1 && queued_argument(i, 0, 5) == 1);

    CHECK(classic_mod_event_create(&events, EPOCH, 5, pollen, strlen(pollen)) == handle);
    CHECK(classic_mod_event_push_bool(&events, EPOCH, handle, false));
    CHECK(classic_mod_event_send(&events, EPOCH, handle, enqueue_to_vm, &owner));
    CHECK(output_count == 3 && output[2].handle == FIRST && output[2].argc == 1);
    CHECK(!queued_argument(2, 0, 5));
    const char *removal_names[] = {remove_oil, remove_pollen};
    for (unsigned i = 0; i < 2; ++i) {
        const char *name = removal_names[i];
        CHECK(classic_mod_event_create(&events, EPOCH, 5, name, strlen(name)) == handle);
        CHECK(classic_mod_event_send(&events, EPOCH, handle, enqueue_to_vm, &owner));
        CHECK(output_count == i + 4 && !output[i + 3].argc);
        CHECK(!strcmp(queued_callback(i + 3), "RemoveMeter"));
    }
    CHECK(output[3].handle == FIRST && output[4].handle == SECOND);
    CHECK(!refs_first && !refs_second && !memcmp(cpu.r, original.r, sizeof(cpu.r)));
    CHECK(cpu.fs_base == original.fs_base);

    const char absent[] = "WearableLanterns_UnknownMeter";
    CHECK(classic_mod_event_create(&events, EPOCH, 1, absent, strlen(absent)) == handle);
    CHECK(classic_mod_event_send(&events, EPOCH, handle, enqueue_to_vm, &owner));
    CHECK(output_count == 5 && attempted == 5);
    CHECK(classic_mod_event_create(&events, EPOCH, 1, pollen, strlen(pollen)) == handle);
    refuse_resize = true;
    CHECK(classic_mod_event_send(&events, EPOCH, handle, enqueue_to_vm, &owner));
    refuse_resize = false;
    CHECK(output_count == 5 && attempted == 6 && !refs_first);
    CHECK(!classic_mod_event_release(&events, EPOCH, handle));

    /* The legacy sender uses the same name-to-callback registration in the
     * fixture and keeps its four-argument payload through the same adapter. */
    classic_fixture_subscribe(&listeners[2], 1);
    skse_send_mod_event(&cpu, pollen, "authored payload", 2.5f, 0);
    CHECK(output_count == 6 && attempted == 7 && output[5].handle == FIRST && output[5].argc == 4);
    CHECK(!strcmp(queued_callback(5), "ForceMeterDisplay") && !refs_first);
    check_queued_string(5, 0, pollen);
    check_queued_string(5, 1, "authored payload");
    CHECK(queued_argument(5, 2, 4) == 0x40200000 && !queued_argument(5, 3, 0));
    CHECK(queued_argument(0, 0, 5) == 1 && queued_argument(1, 0, 5) == 1);
    release_queue();
    classic_fixture_subscribe(NULL, 0);
}

static void check_vm_retirement(void) {
    CHECK(!output_count && queue_epoch == EPOCH);
    CPU cpu = {0};
    ClassicModEvents events;
    classic_mod_events_reset(&events, EPOCH);
    DeliveryOwner old_owner = {.cpu = &cpu, .epoch = EPOCH};
    const FixtureSubscription old_row = {FIRST, REGISTRY, EPOCH, oil, "PreviousWorld"};
    classic_fixture_subscribe(&old_row, 1);
    const int32_t old_handle = classic_mod_event_create(&events, EPOCH, 7, oil, strlen(oil));
    CHECK(old_handle == 1 && classic_mod_event_push_bool(&events, EPOCH, old_handle, true));
    CHECK(classic_mod_event_send(&events, EPOCH, old_handle, enqueue_to_vm, &old_owner));
    CHECK(output_count == 1 && !strcmp(queued_callback(0), "PreviousWorld"));
    CHECK(queued_argument(0, 0, 5) == 1);
    const uint32_t old_name = output[0].callback, old_values = output[0].values;
    CHECK(classic_mod_event_create(&events, EPOCH, 7, oil, strlen(oil)) == old_handle);
    CHECK(classic_mod_event_push_bool(&events, EPOCH, old_handle, true));

    /* Stand in for a confirmed VM reset. The fixture disposes of its queue
     * and makes its old policy unavailable before clearing host metadata. */
    release_queue();
    M32(POLICY_ROOT) = 0;
    queue_epoch = 0;
    classic_fixture_subscribe(NULL, 0);
    classic_mod_events_reset(&events, 0);
    CHECK(!allocation_live(old_name) && !allocation_live(old_values));
    const unsigned calls_at_reset = attempted;
    CHECK(!classic_mod_event_send(&events, EPOCH, old_handle, enqueue_to_vm, &old_owner));
    CHECK(!output_count && !refs_first && !refs_second && attempted == calls_at_reset);

    const uint64_t accepted = EPOCH + 1;
    M32(POLICY_ROOT) = POLICY;
    queue_epoch = accepted;
    classic_mod_events_reset(&events, accepted);
    DeliveryOwner new_owner = {.cpu = &cpu, .epoch = accepted};
    /* An intentionally stale row uses the same registry and 64-bit receiver
     * as the newly restored row. Epoch admission is the only discriminator. */
    const FixtureSubscription new_rows[] = {old_row, {FIRST, REGISTRY, accepted, oil, "AcceptedWorld"}};
    classic_fixture_subscribe(new_rows, sizeof(new_rows) / sizeof(new_rows[0]));
    const int32_t new_handle = classic_mod_event_create(&events, accepted, 7, oil, strlen(oil));
    CHECK(new_handle == old_handle);
    CHECK(!classic_mod_event_send(&events, EPOCH, old_handle, enqueue_to_vm, &old_owner));
    CHECK(!classic_mod_event_push_bool(&events, EPOCH, old_handle, true));
    CHECK(classic_mod_event_push_bool(&events, accepted, new_handle, false));
    CHECK(classic_mod_event_send(&events, accepted, new_handle, enqueue_to_vm, &new_owner));
    CHECK(attempted == calls_at_reset + 1 && output_count == 1 && output[0].handle == FIRST);
    CHECK(!strcmp(queued_callback(0), "AcceptedWorld") && output[0].argc == 1 && !queued_argument(0, 0, 5));
    CHECK(!refs_first && !refs_second);
    release_queue();
    classic_fixture_subscribe(NULL, 0);
}

static bool one_stack_live(void *context, uint32_t stack) {
    return stack == *(const uint32_t *)context;
}

typedef struct { ClassicModEvents *events; int32_t handle; } Recycled;

static void reuse_slot_during_send(void *context, const ClassicModEventPacket *packet) {
    Recycled *test = context;
    CHECK(classic_mod_event_create(test->events, EPOCH, 2, "next", 4) == test->handle);
    CHECK(packet->argc == 1 && packet->args[0] && !strcmp(packet->name, "old"));
}

static void check_storage(void) {
    ClassicModEvents events = {0};
    CHECK(!classic_mod_event_create(&events, EPOCH, 4, "inactive", 8));
    classic_mod_events_reset(&events, EPOCH);
    CHECK(!classic_mod_event_create(&events, EPOCH, 4, NULL, 0));
    CHECK(!classic_mod_event_create(&events, EPOCH, 4, "a\0b", 3));
    CHECK(!classic_mod_event_create(&events, EPOCH, 4, "x", CLASSIC_MOD_EVENT_NAME_MAX + 1));
    CHECK(!classic_mod_event_push_bool(&events, EPOCH, INT32_MIN, true));
    CHECK(!classic_mod_event_release(&events, EPOCH, INT32_MAX));
    CHECK(classic_mod_event_create(&events, EPOCH, 4, "", 0) == 1);
    CHECK(classic_mod_event_release(&events, EPOCH, 1));
    CHECK(classic_mod_event_create(&events, EPOCH, 4, "old", 3) == 1);
    CHECK(classic_mod_event_push_bool(&events, EPOCH, 1, true));
    CHECK(!classic_mod_event_send(&events, EPOCH, 1, NULL, NULL));
    Recycled test = {&events, 1};
    CHECK(classic_mod_event_send(&events, EPOCH, 1, reuse_slot_during_send, &test));
    CHECK(classic_mod_event_release(&events, EPOCH, 1));

    for (int32_t i = 1; i <= CLASSIC_MOD_EVENT_SLOTS; ++i)
        CHECK(classic_mod_event_create(&events, EPOCH, (uint32_t)i, "bound", 5) == i);
    CHECK(!classic_mod_event_create(&events, EPOCH, 1, "overflow", 8));
    for (unsigned i = 0; i < CLASSIC_MOD_EVENT_BOOL_MAX; ++i)
        CHECK(classic_mod_event_push_bool(&events, EPOCH, 1, i % 2));
    CHECK(!classic_mod_event_push_bool(&events, EPOCH, 1, false));
    CHECK(classic_mod_event_release(&events, EPOCH, 4));
    CHECK(classic_mod_event_release(&events, EPOCH, 5));
    CHECK(classic_mod_event_create(&events, EPOCH, 5, "last", 4) == 5);
    CHECK(classic_mod_event_create(&events, EPOCH, 4, "prior", 5) == 4);
    uint32_t live = 4;
    classic_mod_events_clean_dropped(&events, EPOCH + 1, one_stack_live, &live);
    CHECK(!classic_mod_event_create(&events, EPOCH, 1, "full", 4));
    classic_mod_events_clean_dropped(&events, EPOCH, one_stack_live, &live);
    CHECK(!classic_mod_event_release(&events, EPOCH, 1));
    CHECK(classic_mod_event_release(&events, EPOCH, 4));
    CHECK(classic_mod_event_create(&events, EPOCH, 4, "reused", 6) == 4);
    classic_mod_events_reset(&events, EPOCH + 1);
    CHECK(classic_mod_event_create(&events, EPOCH + 1, 1, "new world", 9) == 1);
    CHECK(!classic_mod_event_push_bool(&events, EPOCH, 1, true));
    CHECK(!classic_mod_event_send(&events, EPOCH, 1, enqueue_to_vm, NULL));
    CHECK(classic_mod_event_release(&events, EPOCH + 1, 1));
}

int main(void) {
    MEM = calloc(GUEST_BYTES, 1);
    CHECK(MEM);
    M32(POLICY_ROOT) = POLICY;
    check_storage();
    check_meter_delivery();
    check_vm_retirement();
    unsigned live = 0;
    for (unsigned i = 0; i < used; ++i) {
        if (!allocations[i].live) continue;
        CHECK(allocations[i].address == queue_vtable && allocations[i].size == 8);
        ++live;
    }
    CHECK(live == 1);
    free(MEM);
    puts("PASS Classic typed meter packets, deferred VM ownership, epoch retirement and legacy delivery");
    return 0;
}
