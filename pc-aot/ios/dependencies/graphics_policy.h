/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace skyrim {
/* Initialized by the pipeline manager before its state cache starts workers.
 * A process uses one immutable coordination policy; requests never read env. */
inline bool exactGraphicsStateEnabled() noexcept {
    static const bool enabled = []() noexcept {
        const int savedErrno = errno;
        const char* value = std::getenv("SKYRIM_EXACT_GRAPHICS_STATE");
        const bool selected = value && value[0] == '1' && value[1] == '\0';
        std::fprintf(stderr, "IOS_GRAPHICS_POLICY schema=1 exact_state=%u wait_scope=%u\n",
            unsigned(selected), unsigned(selected));
        errno = savedErrno;
        return selected;
    }();
    return enabled;
}
}
