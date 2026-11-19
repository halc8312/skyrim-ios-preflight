#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "serialization_files.h"
#include "serialization_session.h"

static unsigned handle, interface_calls, handle_calls, uid_set, events, incoming, outgoing;
static unsigned writes, next_calls, reads;
static enum Event phase = EVENT_COUNT;

void fixture_fail(const char *message) { fprintf(stderr, "%s\n", message); exit(2); }

unsigned host_handle(void) { ++handle_calls; return handle; }

void host_query_interface(unsigned id) {
    if (id != 3 || interface_calls++) fixture_fail("unexpected interface request");
}
void host_unique_id(unsigned plugin, unsigned uid) {
    if (plugin != handle || uid != FIXTURE_SERIAL_UID || uid_set++) fixture_fail("invalid serialization owner");
}
void host_event(unsigned plugin, enum Event event) {
    if (plugin != handle || event >= EVENT_COUNT || (events & (1u << event))) fixture_fail("invalid event registration");
    events |= 1u << event;
}
int host_write(unsigned type, unsigned version, unsigned length, unsigned value) {
    if (phase != SAVE || writes++ || type != FIXTURE_SERIAL_TYPE || version != FIXTURE_SERIAL_VERSION || length != 4)
        fixture_fail("unexpected record write");
    outgoing = value;
    return 1;
}
int host_next(unsigned *type, unsigned *version, unsigned *length) {
    if (phase != LOAD || next_calls >= 2) fixture_fail("unexpected record iteration");
    if (next_calls++) return 0;
    *type = FIXTURE_SERIAL_TYPE;
    *version = FIXTURE_SERIAL_VERSION;
    *length = 4;
    return 1;
}
unsigned host_read(unsigned *value, unsigned length) {
    if (phase != LOAD || next_calls != 1 || reads++ || length != 4) fixture_fail("unexpected record read");
    *value = incoming;
    return 4;
}

static void fire(enum Event event) {
    phase = event;
    fixture_invoke_event(event);
    phase = EVENT_COUNT;
}
int main(int argc, char **argv) {
    if (argc != 2) fixture_fail("expected start, exchange or finish");
    const char *source = NULL, *destination;
    unsigned expected;
    if (!strcmp(argv[1], "start")) {
        handle = 11; expected = 16; destination = "windows.skse";
    } else if (!strcmp(argv[1], "exchange")) {
        handle = 77; expected = 21; source = "windows.skse"; destination = "translated.skse";
    } else if (!strcmp(argv[1], "finish")) {
        handle = 99; expected = 26; source = "translated.skse"; destination = "returned.skse";
    } else {
        fixture_fail("unknown fixture mode");
        return 2;
    }
    if (!fixture_register_plugin() || interface_calls != 1 || handle_calls != 1 || uid_set != 1 || events != 7)
        fixture_fail("plugin or callback registration failed");
    if (source) {
        if (!fixture_load_record(source, &incoming) || incoming != expected - 5) fixture_fail("unexpected input file");
        fire(REVERT);
        fire(SAVE);
        if (writes != 1 || outgoing != 5) fixture_fail("revert did not reset plugin state");
        writes = 0;
        fire(LOAD);
        if (next_calls != 2 || reads != 1) fixture_fail("load callback did not consume record");
    }
    fire(SAVE);
    if (writes != 1 || outgoing != expected || !fixture_store_record(source ? source : "seed.skse", destination, outgoing))
        fixture_fail("save callback or file publication failed");
    unsigned verified;
    if (!fixture_load_record(destination, &verified) || verified != expected) fixture_fail("stored record differs");
    printf("%u %u %u %u %u\n", handle, incoming, verified, next_calls, reads);
    return 0;
}
