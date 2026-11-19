#pragma once

enum Event { REVERT, SAVE, LOAD, EVENT_COUNT };
int fixture_register_plugin(void);
void fixture_invoke_event(enum Event);

void fixture_fail(const char *message);
unsigned host_handle(void);
void host_query_interface(unsigned id);
void host_unique_id(unsigned handle, unsigned uid);
void host_event(unsigned handle, enum Event event);
int host_write(unsigned type, unsigned version, unsigned length, unsigned value);
int host_next(unsigned *type, unsigned *version, unsigned *length);
unsigned host_read(unsigned *value, unsigned length);
