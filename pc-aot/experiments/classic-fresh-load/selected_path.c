/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "fresh_load.h"
#include "rt.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

bool classic_fresh_resolve_selected_guest_path(const struct CPU *cpu, uint32_t path,
    const char *mapped_directory, char canonical[CLASSIC_FRESH_PATH_BYTES]) {
    enum { OPEN_RETURN = 0x682225, PATH_OFFSET = 0x14, PATH_BYTES = 260, TEB_STRIDE = 0x2000 };
    if (!canonical) return false;
    canonical[0] = 0;
    if (!cpu || cpu != rt_current_cpu() || !MEM || !mapped_directory ||
        !cpu->r[1] || cpu->r[1] != cpu->r[6]) return false;

    const uint32_t stack = cpu->r[4];
    const uintptr_t slot = (uintptr_t)cpu->thread;
    if (slot >= GUEST_STACK_SLOTS || cpu->fs_base != GUEST_TEB_BASE + slot * TEB_STRIDE) return false;
    const uint32_t lo = GUEST_STACK_BASE + (uint32_t)slot * GUEST_STACK_SIZE;
    const uint32_t hi = lo + GUEST_STACK_SIZE - 64u;
    if ((stack & 3u) || stack < lo || stack > hi - PATH_OFFSET - PATH_BYTES ||
        path != stack + PATH_OFFSET || M32(cpu->fs_base + 4) != hi ||
        M32(cpu->fs_base + 8) != lo || M32(stack) != OPEN_RETURN) return false;

    char original[PATH_BYTES], mapped[CLASSIC_FRESH_PATH_BYTES];
    const char *guest_path = (const char *)MEM + path;
    const size_t length = strnlen(guest_path, sizeof original);
    const size_t prefix = strnlen(mapped_directory, sizeof mapped);
    if (length <= 4 || length == sizeof original || !prefix || prefix + length + 1 >= sizeof mapped ||
        mapped_directory[0] != '/' || mapped_directory[prefix - 1] == '/') return false;
    memcpy(original, guest_path, length + 1);
    if (strcasecmp(original + length - 4, ".ess")) return false;
    for (const char *component = original; *component;) {
        const size_t width = strcspn(component, "\\/");
        if (width == 2 && component[0] == '.' && component[1] == '.') return false;
        component += width;
        if (*component) ++component;
    }

    const int previous_errno = errno;
    bool accepted = false;
    if (rt_host_path(original, mapped, sizeof mapped)) {
        const size_t size = strnlen(mapped, sizeof mapped);
        if (size > prefix + 5 && size < sizeof mapped - 1 &&
            !memcmp(mapped, mapped_directory, prefix) && mapped[prefix] == '/') {
            const char *leaf = mapped + prefix + 1;
            const size_t leaf_length = size - prefix - 1;
            struct stat metadata;
            if (leaf_length <= NAME_MAX && !strchr(leaf, '/') &&
                !strcasecmp(leaf + leaf_length - 4, ".ess") &&
                !lstat(mapped, &metadata) && S_ISREG(metadata.st_mode)) {
                memcpy(canonical, mapped, size + 1);
                accepted = true;
            }
        }
    }
    errno = previous_errno;
    return accepted;
}
