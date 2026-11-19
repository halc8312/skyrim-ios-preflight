/* SPDX-License-Identifier: GPL-3.0-only */
/* Host harness for the single translated export of our own i386 fixture. */
#include "device_bridge.h"
#include "cases.h"
#include "funcs_owneddepth.h"
#include "rt_table.h"
#include "owned_depth_layout.h"
#include <stdlib.h>
#include <string.h>

enum { OBJECT = 0x2000, OUTPUT = 0x4000, STACK = 0x6000, INITIAL_SP = STACK + 448 };
static uint8_t bytes[OWNED_DEPTH_END];
uint8_t* MEM = bytes;
static ClassicDeviceProxy proxy;
static unsigned calls;
static char trace[5];
static void invariant(int ok, const char* message) {
    if (!ok) {
        fputs(message, stderr);
        fputc('\n', stderr);
        exit(2);
    }
}
static void prepare(ClassicDeviceBackend backend) {
    ClassicGuestSpan view = {OBJECT, bytes + OBJECT, CLASSIC_DEVICE9_BYTES};
    ClassicGuestSpan stack = {STACK, bytes + STACK, 512}, out = {OUTPUT, bytes + OUTPUT, 8};
    memset(&proxy, 0, sizeof proxy);
    memset(trace, 0, sizeof trace);
    calls = 0;
    memcpy(bytes + OWNED_DEPTH_RDATA, owned_depth_rdata, sizeof owned_depth_rdata);
    memcpy(bytes + OWNED_DEPTH_DATA, owned_depth_data, sizeof owned_depth_data);
    invariant(classic_device_open(&proxy, backend, view, stack, out) == CLASSIC_DEVICE_OK,
              "proxy creation failed");
}

#ifndef CLASSIC_D3D9_LIVE
static struct DepthCase current;
static unsigned pins, reads;
static void retain(void* p) {
    invariant(p == &current && !pins, "bad native retain");
    ++pins;
}
static void release(void* p) {
    invariant(p == &current && pins == 1, "bad native release");
    --pins;
}
static uint32_t depth(void* p, uint32_t* native_word) {
    invariant(p == &current && pins == 1, "lost native reference");
    invariant(native_word != (void*)(MEM + OUTPUT + 1), "guest output escaped the adapter");
    ++reads;
    /* Even if a failed backend changed its own local output word, the guest's
     * destination must stay intact. */
    *native_word = current.depth;
    return current.hr;
}
#endif

void rt_call(CPU* cpu, uint32_t target) {
    invariant(calls < sizeof(trace) - 1, "too many indirect calls in the authored export");
    trace[calls++] = target == CLASSIC_COM_ADDREF_TOKEN    ? 'A'
                     : target == CLASSIC_COM_RELEASE_TOKEN ? 'R'
                                                           : 'Q';
    invariant(classic_device_call(&proxy, target, &cpu->r[4], &cpu->r[0]) == CLASSIC_DEVICE_OK,
              "unapproved indirect call or malformed stdcall frame");
}
void rt_call_ic(CPU* cpu, uint32_t target, const FnEntry** slot) {
    (void)slot;
    rt_call(cpu, target);
}

static uint32_t invoke(unsigned flags) {
    CPU cpu = {0};
    cpu.r[3] = 0x33333333;
    cpu.r[5] = 0x55555555;
    cpu.r[6] = 0x66666666;
    cpu.r[7] = 0x77777777;
    cpu.r[4] = INITIAL_SP;
    M32(INITIAL_SP) = 0x1234abcd;
    M32(INITIAL_SP + 4) = flags & NO_DEVICE ? 0 : OBJECT;
    M32(INITIAL_SP + 8) = flags & NO_OUTPUT ? 0 : OUTPUT + 1;
    M32(OUTPUT + 1) = INITIAL_WORD;
    OWNED_DEPTH_ENTRY(&cpu);
    invariant(cpu.r[4] == INITIAL_SP + 4 && M32(INITIAL_SP) == 0x1234abcd && cpu.r[3] == 0x33333333 &&
                  cpu.r[5] == 0x55555555 && cpu.r[6] == 0x66666666 && cpu.r[7] == 0x77777777,
              "i386 caller stack or preserved register changed");
    return cpu.r[0];
}

#ifdef CLASSIC_D3D9_LIVE
uint32_t classic_owned_depth_read(ClassicDeviceBackend backend, uint32_t* word) {
    prepare(backend);
    uint32_t hr = invoke(0);
    invariant(proxy.references == 1 && strcmp(trace, "AQR") == 0, "live COM sequence changed");
    *word = M32(OUTPUT + 1);
    classic_device_close(&proxy);
    return hr;
}
#else
int main(void) {
    for (unsigned i = 0; i < CASES; ++i) {
        current = cases[i];
        reads = 0;
        ClassicDeviceBackend backend = {&current, retain, release, depth};
        prepare(backend);
        uint32_t hr = invoke(current.flags);
        invariant(proxy.references == 1 && pins == 1 && reads == (!current.flags),
                  "unexpected backend side effects");
        report(i, hr, M32(OUTPUT + 1), proxy.references, trace);
        classic_device_close(&proxy);
        invariant(!pins, "native reference remained at teardown");
    }
    return 0;
}
#endif
