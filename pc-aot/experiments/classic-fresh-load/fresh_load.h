/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CLASSIC_FRESH_LOAD_H
#define CLASSIC_FRESH_LOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct CPU;
enum { CLASSIC_FRESH_PATH_BYTES = 1024 };

typedef enum {
    CLASSIC_FRESH_READY = 1,
    CLASSIC_FRESH_CACHED,
    CLASSIC_FRESH_NO_OPEN,
    CLASSIC_FRESH_NO_READ,
    CLASSIC_FRESH_REJECTED,
    CLASSIC_FRESH_IDENTITY,
    CLASSIC_FRESH_REENTERED,
} ClassicFreshResult;

typedef struct {
    uint64_t ticket;
    ClassicFreshResult result;
    bool slot_returned, slot_ok, outer_ok;
    int ess_fd; /* Read-only and borrowed for the report call, or -1. */
} ClassicFreshReport;

/* The linked hosted adapter resolves the selected guest path in the same
 * canonical naming domain as the open observation. Reports are synchronous. */
bool classic_fresh_enabled(void);
bool classic_fresh_resolve_path(const struct CPU *, uint32_t path,
                                char canonical[CLASSIC_FRESH_PATH_BYTES]);
void classic_fresh_report(ClassicFreshReport);

/* Read-only resolver for the staged first selected-save open. The caller
 * supplies the canonical authorized profile directory and retains its
 * lifetime. Pass a separate output buffer. It holds the mapped path for
 * an existing regular .ess directly below that directory, or an empty
 * string on refusal. The ordinary file open still owns descriptor identity. */
bool classic_fresh_resolve_selected_guest_path(const struct CPU *, uint32_t path,
    const char *mapped_directory, char canonical[CLASSIC_FRESH_PATH_BYTES]);

/* File events come from the trusted engine adapter, with a handle generation
 * assigned at real open and retained for every read through that handle. */
void classic_fresh_observe_open(const struct CPU *, uint32_t stream, const char *canonical,
                               int fd, uint64_t handle_generation);
void classic_fresh_observe_read(const struct CPU *, uint32_t stream, int fd,
                               uint64_t handle_generation, size_t bytes);

/* Calls emitted only at the reviewed f_6821C0 boundaries. */
uint64_t classic_fresh_enter(const struct CPU *);
void classic_fresh_cached(uint64_t, const struct CPU *);
void classic_fresh_open_enter(uint64_t, const struct CPU *, uint32_t stream, uint32_t path);
void classic_fresh_open_leave(uint64_t, const struct CPU *, bool success);
void classic_fresh_slot_enter(uint64_t, const struct CPU *, uint32_t stream);
void classic_fresh_slot_leave(uint64_t, const struct CPU *, bool success);

/* Hosted notification at the verified f_67B720 read return. The caller
 * admits only a positive logical count up to the original 100-byte request.
 * An adapter still needs to establish the live file identity. */
enum { CLASSIC_FRESH_SLOT_READ_BYTES = 100 };
void classic_fresh_slot_read_observed(const struct CPU *, uint32_t stream, uint32_t bytes);
void classic_fresh_leave(uint64_t, const struct CPU *, bool success);

#endif
