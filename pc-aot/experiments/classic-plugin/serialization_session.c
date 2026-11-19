#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SKYRIM_CLASSIC_EMBEDDED_FIXTURE
#include <setjmp.h>
#endif
#include "serialization_files.h"
#include "serialization_session.h"

static unsigned handle, interface_calls, handle_calls, uid_set, events, incoming, outgoing;
static unsigned writes, next_calls, reads;
static enum Event phase = EVENT_COUNT;

#ifdef SKYRIM_CLASSIC_EMBEDDED_FIXTURE
/* The app serializes this private driver and catches a failure inside
 * skyrim_serial_i386_session. No file handles span this jump. */
static jmp_buf callback_failure;
_Noreturn void fixture_fail(const char *message) {
    (void)message;
    longjmp(callback_failure, 1);
}
#else
_Noreturn void fixture_fail(const char *message) { fprintf(stderr, "%s\n", message); exit(2); }
#endif

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
int skyrim_serial_i386_session(unsigned plugin_handle, const unsigned *input, FixtureSessionValues *result) {
    handle = plugin_handle;
    incoming = input ? *input : 0;
    interface_calls = handle_calls = uid_set = events = outgoing = writes = next_calls = reads = 0;
    phase = EVENT_COUNT;
#ifdef SKYRIM_CLASSIC_EMBEDDED_FIXTURE
    if (setjmp(callback_failure)) return 0;
#endif
    if (!fixture_register_plugin() || interface_calls != 1 || handle_calls != 1 || uid_set != 1 || events != 7)
        fixture_fail("plugin or callback registration failed");
    unsigned reverted = 0;
    if (input) {
        fire(REVERT);
        fire(SAVE);
        if (writes != 1 || outgoing != 5) fixture_fail("revert did not reset plugin state");
        reverted = outgoing;
        writes = 0;
        fire(LOAD);
        if (next_calls != 2 || reads != 1) fixture_fail("load callback did not consume record");
    }
    fire(SAVE);
    if (writes != 1) fixture_fail("save callback did not publish one value");
    *result = (FixtureSessionValues){reverted, outgoing, next_calls, reads, events};
    return 1;
}

#ifndef SKYRIM_CLASSIC_EMBEDDED_FIXTURE
int main(int argc, char **argv) {
    if (argc != 2) fixture_fail("expected start, exchange or finish");
    const char *source = NULL, *destination;
    unsigned expected, id, input = 0;
    if (!strcmp(argv[1], "start")) {
        id = 11; expected = 16; destination = "windows.skse";
    } else if (!strcmp(argv[1], "exchange")) {
        id = 77; expected = 21; source = "windows.skse"; destination = "translated.skse";
    } else if (!strcmp(argv[1], "finish")) {
        id = 99; expected = 26; source = "translated.skse"; destination = "returned.skse";
    } else {
        fixture_fail("unknown fixture mode");
    }
    if (source && (!fixture_load_record(source, &input) || input != expected - 5)) fixture_fail("unexpected input file");
    FixtureSessionValues result;
    if (!skyrim_serial_i386_session(id, source ? &input : NULL, &result) || result.outgoing != expected ||
        !fixture_store_record(source ? source : "seed.skse", destination, result.outgoing))
        fixture_fail("save callback or file publication failed");
    unsigned verified;
    if (!fixture_load_record(destination, &verified) || verified != expected) fixture_fail("stored record differs");
    printf("%u %u %u %u %u\n", id, input, verified, result.next_calls, result.reads);
    return 0;
}
#endif
