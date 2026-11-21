/* SPDX-License-Identifier: GPL-3.0-only */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "probe_local_file.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static ssize_t read_piece(int fd, void *buffer, size_t length) {
    ssize_t count;
    do {
        count = read(fd, buffer, length);
    } while (count < 0 && errno == EINTR);
    return count;
}

static SkseProbeFileStatus read_source(int fd, uint8_t **out, size_t *length) {
    struct stat source;
    if (fstat(fd, &source) != 0) return SKSE_PROBE_FILE_INCOMPLETE;
    if (!S_ISREG(source.st_mode) || source.st_size <= 0) return SKSE_PROBE_FILE_UNSUPPORTED;
    if (source.st_size > SKSE_PE_MAX_FILE_BYTES) return SKSE_PROBE_FILE_TOO_LARGE;
    *length = (size_t)source.st_size;
    *out = malloc(*length);
    if (!*out) return SKSE_PROBE_FILE_NO_MEMORY;

    size_t offset = 0;
    while (offset < *length) {
        ssize_t count = read_piece(fd, *out + offset, *length - offset);
        if (count <= 0) return SKSE_PROBE_FILE_INCOMPLETE;
        offset += (size_t)count;
    }
    uint8_t extra;
    return read_piece(fd, &extra, 1) == 0 ? SKSE_PROBE_FILE_CHECKED : SKSE_PROBE_FILE_INCOMPLETE;
}

SkseProbeFileStatus skse_probe_snapshot_file(const char *path, uint8_t **bytes, size_t *length) {
    if (!bytes || !length) return SKSE_PROBE_FILE_UNAVAILABLE;
    *bytes = NULL;
    *length = 0;
    if (!path || !*path) return SKSE_PROBE_FILE_UNAVAILABLE;
    int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
#ifdef O_NOFOLLOW_ANY
    flags |= O_NOFOLLOW_ANY;
#endif
    int fd = open(path, flags);
    if (fd < 0) return SKSE_PROBE_FILE_UNAVAILABLE;
    SkseProbeFileStatus status = read_source(fd, bytes, length);
    close(fd);
    if (status != SKSE_PROBE_FILE_CHECKED) {
        free(*bytes);
        *bytes = NULL;
        *length = 0;
    }
    return status;
}

SkseProbeFileStatus skse_probe_local_dll_with_setup(const char *path, uint32_t uptime_ms,
                                                    const SkseLocalProbeSetup *setup, SkseLocalDllReport *out) {
    if (setup && setup->record_probe) setup->record_probe->status = SKSE_SESSION_OUT_OF_ORDER;
    if (setup && setup->code_audit) {
        setup->code_audit->count = 0;
        setup->code_audit->truncated = false;
    }
    if (!out) return SKSE_PROBE_FILE_UNAVAILABLE;
    memset(out, 0, sizeof *out);
    uint8_t *bytes;
    size_t length;
    SkseProbeFileStatus status = skse_probe_snapshot_file(path, &bytes, &length);
    if (status == SKSE_PROBE_FILE_CHECKED) {
        // Classic 1.7.3 API as presented to one isolated, temporary plugin.
        const char *slash = strrchr(path, '/');
        const SkseTrialOptions options = {0x01070030, 0x09200000, 0, 48, 1, 1, 1};
        const SkseTrialRuntime runtime = {
            .uptime_ms = uptime_ms, .core_messages = 1, .static_tls = 1, .serialization = 1, .worker_thread = 1,
            .task_interface = 1, .trace_known_windows_imports = true, .module_file_name = slash ? slash + 1 : path,
            .guest_base = setup ? setup->guest_base : 0,
            .read_views = setup ? setup->read_views : NULL,
            .read_view_count = setup ? setup->read_view_count : 0,
            .capture_papyrus_startup = setup && setup->capture_papyrus_startup,
            .hook_probe_sites = setup ? setup->hook_probe_sites : NULL,
            .hook_probe_count = setup ? setup->hook_probe_count : 0,
            .code_audit = setup ? setup->code_audit : NULL
        };
        if (setup && setup->record_probe)
            out->trial = skse_plugin_trial_with_record_probe(bytes, length, options, runtime, setup->record_probe);
        else out->trial = skse_plugin_trial_with_runtime(bytes, length, options, runtime);
        if (out->trial.status == SKSE_TRIAL_IMAGE_REFUSED)
            skse_pe_inspect(bytes, length, &out->imports);
    }
    free(bytes);
    return status;
}

SkseProbeFileStatus skse_probe_local_dll(const char *path, uint32_t uptime_ms, SkseLocalDllReport *out) {
    return skse_probe_local_dll_with_setup(path, uptime_ms, NULL, out);
}
