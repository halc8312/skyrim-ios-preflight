/* SPDX-License-Identifier: GPL-3.0-only */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "fresh_load.h"
#include "digest.h"

#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum { OUTSIDE, OPENING, LOADING } Phase;

typedef struct {
    const struct CPU *cpu;
    uint64_t ticket, handle_generation;
    uint32_t stream;
    int original_fd, held_fd;
    char path[CLASSIC_FRESH_PATH_BYTES];
    struct stat opened;
    Phase phase;
    ClassicFreshResult fault;
    bool cached, read, slot_returned, slot_ok;
} Attempt;

static _Thread_local Attempt attempt;
static _Thread_local uint64_t serial;

static bool current(uint64_t ticket, const struct CPU *cpu) {
    return ticket && ticket == attempt.ticket && cpu == attempt.cpu;
}

static bool version(int fd, struct stat *metadata) {
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && (flags & O_ACCMODE) == O_RDONLY && !fstat(fd, metadata) &&
           S_ISREG(metadata->st_mode) && metadata->st_size > 0;
}

static bool same_file(int fd) {
    struct stat now;
    return version(fd, &now) && skse_ess_same_version(&attempt.opened, &now);
}

uint64_t classic_fresh_enter(const struct CPU *cpu) {
    if (attempt.ticket) {
        attempt.fault = CLASSIC_FRESH_REENTERED;
        return 0;
    }
    if (!cpu || serial == UINT64_MAX || !classic_fresh_enabled()) return 0;
    attempt = (Attempt){.cpu = cpu, .ticket = ++serial, .original_fd = -1, .held_fd = -1};
    return attempt.ticket;
}

void classic_fresh_cached(uint64_t ticket, const struct CPU *cpu) {
    if (current(ticket, cpu)) attempt.cached = true;
}

void classic_fresh_open_enter(uint64_t ticket, const struct CPU *cpu, uint32_t stream,
                             uint32_t path) {
    if (!current(ticket, cpu) || attempt.fault || attempt.cached || !stream) return;
    if (!classic_fresh_resolve_path(cpu, path, attempt.path)) {
        attempt.fault = CLASSIC_FRESH_IDENTITY;
        return;
    }
    size_t length = strnlen(attempt.path, CLASSIC_FRESH_PATH_BYTES);
    if (length <= 4 || length == CLASSIC_FRESH_PATH_BYTES ||
        strcasecmp(attempt.path + length - 4, ".ess")) {
        attempt.fault = CLASSIC_FRESH_IDENTITY;
        return;
    }
    attempt.stream = stream;
    attempt.phase = OPENING;
}

void classic_fresh_observe_open(const struct CPU *cpu, uint32_t stream, const char *canonical,
                               int fd, uint64_t handle_generation) {
    if (!attempt.ticket || attempt.cpu != cpu || attempt.phase != OPENING ||
        attempt.stream != stream || attempt.fault || fd < 0) return;
    if (!handle_generation || !canonical || strcmp(canonical, attempt.path) || attempt.held_fd >= 0) {
        attempt.fault = CLASSIC_FRESH_IDENTITY;
        return;
    }
    const int held = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (held < 0 || !version(held, &attempt.opened) || !same_file(fd)) {
        if (held >= 0) close(held);
        attempt.fault = CLASSIC_FRESH_IDENTITY;
        return;
    }
    attempt.original_fd = fd;
    attempt.held_fd = held;
    attempt.handle_generation = handle_generation;
}

void classic_fresh_open_leave(uint64_t ticket, const struct CPU *cpu, bool success) {
    if (!current(ticket, cpu)) return;
    attempt.phase = OUTSIDE;
    if (!success && attempt.held_fd >= 0) {
        close(attempt.held_fd);
        attempt.held_fd = -1;
    }
}

void classic_fresh_slot_enter(uint64_t ticket, const struct CPU *cpu, uint32_t stream) {
    if (!current(ticket, cpu) || attempt.fault || attempt.cached || attempt.held_fd < 0) return;
    if (stream != attempt.stream) attempt.fault = CLASSIC_FRESH_IDENTITY;
    else attempt.phase = LOADING;
}

void classic_fresh_observe_read(const struct CPU *cpu, uint32_t stream, int fd,
                               uint64_t handle_generation, size_t bytes) {
    if (!attempt.ticket || attempt.cpu != cpu || attempt.phase != LOADING || attempt.fault ||
        attempt.stream != stream || !bytes) return;
    if (fd != attempt.original_fd || handle_generation != attempt.handle_generation || !same_file(fd)) {
        attempt.fault = CLASSIC_FRESH_IDENTITY;
    } else {
        attempt.read = true;
    }
}

void classic_fresh_slot_leave(uint64_t ticket, const struct CPU *cpu, bool success) {
    if (!current(ticket, cpu)) return;
    attempt.phase = OUTSIDE;
    attempt.slot_returned = true;
    attempt.slot_ok = success;
}

static ClassicFreshResult result(bool outer_ok) {
    if (attempt.fault) return attempt.fault;
    if (attempt.cached) return CLASSIC_FRESH_CACHED;
    if (attempt.held_fd < 0) return CLASSIC_FRESH_NO_OPEN;
    if (!attempt.slot_returned || !attempt.slot_ok || !outer_ok) return CLASSIC_FRESH_REJECTED;
    if (!attempt.read) return CLASSIC_FRESH_NO_READ;
    return same_file(attempt.held_fd) ? CLASSIC_FRESH_READY : CLASSIC_FRESH_IDENTITY;
}

void classic_fresh_leave(uint64_t ticket, const struct CPU *cpu, bool success) {
    if (!current(ticket, cpu)) return;
    const ClassicFreshResult outcome = result(success);
    const int held = attempt.held_fd;
    const ClassicFreshReport report = {
        .ticket = ticket, .result = outcome, .slot_returned = attempt.slot_returned,
        .slot_ok = attempt.slot_ok, .outer_ok = success,
        .ess_fd = outcome == CLASSIC_FRESH_READY ? held : -1,
    };
    attempt = (Attempt){0};
    classic_fresh_report(report);
    if (held >= 0) close(held);
}

void classic_fresh_cancel(uint64_t ticket, const struct CPU *cpu) {
    if (!current(ticket, cpu)) return;
    if (attempt.held_fd >= 0) close(attempt.held_fd);
    attempt = (Attempt){0};
}
