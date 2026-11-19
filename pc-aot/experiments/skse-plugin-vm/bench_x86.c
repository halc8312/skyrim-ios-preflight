/* SPDX-License-Identifier: GPL-3.0-only */
// Authored CPU loop. Report process CPU time independently of graphics or IO.
#include "x86_call.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static double cpu_seconds(void) {
#ifdef _WIN32
    // The Windows CRT's clock measures elapsed time. Use actual process CPU time.
    FILETIME created, ended, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user)) return -1;
    uint64_t kernel_ticks = (uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime;
    uint64_t user_ticks = (uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime;
    return (double)(kernel_ticks + user_ticks) / 10000000.0;
#else
    return (double)clock() / CLOCKS_PER_SEC;
#endif
}

int main(void) {
    static uint8_t bytes[0x4000], workspace[4096];
    // mov ecx,250000; xor eax,eax; add eax,ecx; dec ecx; jnz add; ret
    static const uint8_t code[] = {0xb9, 0x90, 0xd0, 0x03, 0, 0x31, 0xc0, 0x01, 0xc8, 0x49, 0x75, 0xfb, 0xc3};
    memcpy(bytes + 0x1000, code, sizeof code);
    SksePeImage image = {
        .base = 0x10000000, .size = sizeof bytes, .bytes = bytes, .headers_size = 0x400,
        .section_count = 3, .sections = {
            {0x1000, 0x1000, SKSE_PE_READ | SKSE_PE_EXEC},
            {0x2000, 0x1000, SKSE_PE_READ},
            {0x3000, 0x1000, SKSE_PE_READ | SKSE_PE_WRITE}
        }
    };
    SkseX86Scratch scratch = {0x20000000, workspace, sizeof workspace};
    uint64_t steps = 0;
    double start = cpu_seconds();
    assert(start >= 0);
    for (unsigned i = 0; i < 40; ++i) {
        SkseX86Result result = skse_x86_call(&image, image.base + 0x1000, scratch, NULL, 0, 800000,
                                           (SkseX86Services){0}, SKSE_X86_CDECL);
        assert(result.status == SKSE_X86_OK && result.eax == (uint32_t)UINT64_C(31250125000));
        steps += result.instructions;
    }
    double seconds = cpu_seconds() - start;
    assert(seconds >= 0);
    printf("%.6f %.0f\n", seconds, (double)steps);
}
