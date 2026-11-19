/* Closed harness for the two functions generated from the authored i386 DLL. */
#include <stdio.h>
#include <stdlib.h>
#include "fixture_cases.h"
#include "funcs_fixture.h"
#include "rt_table.h"
#include "fixture_layout.h"

enum { API = 0x3000, INFO = 0x3100, STACK = 0x8000, HANDLE_CALL = 0x1000, RELEASE_CALL = 0x1004 };
static unsigned char storage[FIXTURE_IMAGE_END];
uint8_t *MEM = storage;
static struct Arguments current;
static unsigned release_calls, handle_calls;

static void failed(const char *message) {
    fputs(message, stderr);
    fputc('\n', stderr);
    exit(2);
}

void rt_call(CPU *c, uint32_t target) {
    if (target == RELEASE_CALL) {
        ++release_calls;
        c->r[0] = current.release;
    } else if (target == HANDLE_CALL) {
        ++handle_calls;
        c->r[0] = current.handle;
    } else {
        failed("unrecognized fixture callback");
    }
    c->r[4] += 4; /* cdecl ret for either parameterless callback */
}

void rt_call_ic(CPU *c, uint32_t target, const FnEntry **cache) {
    (void)cache;
    rt_call(c, target);
}

static unsigned invoke(GuestFn entry, uint32_t api, uint32_t info) {
    CPU cpu = {0};
    cpu.r[0] = 0xfade0000u;
    cpu.r[3] = 0x33333333u;
    cpu.r[5] = 0x55555555u;
    cpu.r[6] = 0x66666666u;
    cpu.r[7] = 0x77777777u;
    cpu.r[4] = STACK;
    M32(STACK) = 0xfeed1234u;
    M32(STACK + 4) = api;
    M32(STACK + 8) = info;
    entry(&cpu);
    if (cpu.r[4] != STACK + 4 || M32(STACK) != 0xfeed1234u || cpu.r[3] != 0x33333333u ||
        cpu.r[5] != 0x55555555u || cpu.r[6] != 0x66666666u || cpu.r[7] != 0x77777777u) {
        failed("guest cdecl stack or saved registers changed");
    }
    return cpu.r[0] & 0xffu; /* i386 bool result is in AL */
}

static const char *plugin_name(uint32_t address) {
    if (!address) return "-";
    if (address < FIXTURE_RODATA || address - FIXTURE_RODATA >= sizeof fixture_rodata ||
        !memchr(MEM + address, 0, sizeof fixture_rodata - (address - FIXTURE_RODATA))) {
        failed("name does not point into the DLL's original read-only data");
    }
    return (const char *)MEM + address;
}

int main(void) {
    memcpy(MEM + FIXTURE_RODATA, fixture_rodata, sizeof fixture_rodata);
    for (enum Case id = GAME; id < CASE_COUNT; ++id) {
        current = arguments(id);
        handle_calls = release_calls = 0;
        M32(API) = current.skse_version;
        M32(API + 4) = current.runtime_version;
        M32(API + 8) = 0;
        M32(API + 12) = current.editor;
        M32(API + 16) = 0;
        M32(API + 20) = id == NO_HANDLE_CALLBACK ? 0 : HANDLE_CALL;
        M32(API + 24) = id == NO_RELEASE_CALLBACK ? 0 : RELEASE_CALL;
        memset(MEM + INFO, 0, 12);
        uint32_t api = id == NULL_API ? 0 : API;
        int query = (int)invoke(FIXTURE_QUERY, api, id == NULL_INFO ? 0 : INFO);
        int loaded = query ? (int)invoke(FIXTURE_LOAD, api, 0) : -1;
        printf("%u %d %d %u %u %s %u %u\n", (unsigned)id, query, loaded,
               M32(INFO), M32(INFO + 8), plugin_name(M32(INFO + 4)), release_calls, handle_calls);
    }
    return 0;
}
