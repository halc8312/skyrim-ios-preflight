#pragma once

enum Event { REVERT, SAVE, LOAD, EVENT_COUNT };
int fixture_register_plugin(void);
void fixture_invoke_event(enum Event);

typedef struct {
    unsigned reverted, outgoing, next_calls, reads, events;
} FixtureSessionValues;

/* Callers provide an isolated execution context. No on-disk data is opened
 * while the translated callbacks run. */
int skyrim_serial_i386_session(unsigned handle, const unsigned *input, FixtureSessionValues *result);
#ifdef SKYRIM_CLASSIC_EMBEDDED_FIXTURE
int skyrim_serial_i386_allocate(void);
void skyrim_serial_i386_release(void);
#endif

_Noreturn void fixture_fail(const char *message);
unsigned host_handle(void);
void host_query_interface(unsigned id);
void host_unique_id(unsigned handle, unsigned uid);
void host_event(unsigned handle, enum Event event);
int host_write(unsigned type, unsigned version, unsigned length, unsigned value);
int host_next(unsigned *type, unsigned *version, unsigned *length);
unsigned host_read(unsigned *value, unsigned length);
