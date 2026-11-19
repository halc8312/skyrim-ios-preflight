/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdint.h>

static volatile uint32_t attached, checkpoint;

int __stdcall OwnedDependencyEntry(void *image, unsigned reason, void *reserved) {
    if (!image || reserved) return 0;
    if (reason == 1) attached = 0x147a;
    if (reason == 0) attached = 0;
    return 1;
}

__declspec(dllexport) uint32_t __cdecl TransformFixtureValue(uint32_t value) {
    if (attached != 0x147a) return 0;
    if (value == 1) {
        checkpoint = 11;
        return 14;
    }
    if (value == 2 && checkpoint == 11) {
        checkpoint = 17;
        return 29;
    }
    return 0;
}
