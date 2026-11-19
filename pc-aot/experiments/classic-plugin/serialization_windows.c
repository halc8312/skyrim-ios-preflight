#include <string.h>
#include "fixture_api.h"
#include "serialization_abi.h"
#include "serialization_session.h"

static SerialEvent handlers[EVENT_COUNT];
static void reg(unsigned h, enum Event kind, SerialEvent fn) {
    if (!fn) fixture_fail("null plugin callback");
    host_event(h, kind);
    handlers[kind] = fn;
}
static void on_revert(unsigned h, SerialEvent fn) { reg(h, REVERT, fn); }
static void on_save(unsigned h, SerialEvent fn) { reg(h, SAVE, fn); }
static void on_load(unsigned h, SerialEvent fn) { reg(h, LOAD, fn); }
static _Bool write_record(unsigned t, unsigned v, const void *buf, unsigned n) {
    if (!buf || n != 4) fixture_fail("invalid write buffer");
    unsigned value;
    memcpy(&value, buf, sizeof value);
    return host_write(t, v, n, value);
}
static _Bool next_record(unsigned *t, unsigned *v, unsigned *n) { return host_next(t, v, n); }
static unsigned read_data(void *buf, unsigned n) {
    unsigned value, length = host_read(&value, n);
    memcpy(buf, &value, length);
    return length;
}
static struct SerialApi api = {.version = 4, .unique_id = host_unique_id, .on_revert = on_revert,
    .on_save = on_save, .on_load = on_load, .write_record = write_record, .next_record = next_record,
    .read_data = read_data};
static void *query_interface(unsigned id) { host_query_interface(id); return &api; }

int fixture_register_plugin(void) {
    struct ClassicApi skse = {0x01070030u, 0x09200000u, 0, 0, query_interface, host_handle, NULL};
    struct PluginInfo info = {0};
    return SKSEPlugin_Query(&skse, &info) && info.info_version == 1 && info.plugin_version == 2 &&
           info.name && !strcmp(info.name, "SerialProbe") && SKSEPlugin_Load(&skse);
}
void fixture_invoke_event(enum Event event) { handlers[event](&api); }
