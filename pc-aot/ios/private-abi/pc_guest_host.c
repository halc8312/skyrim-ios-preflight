/* Public iOS runtime: bind a locally assembled, signed original-body guest. */
#include "PcGuestABI.h"
#ifdef SKYRIM_IOS_CLASSIC_SKSE
#include "PcGuestSkseABI.h"
#define PC_GUEST_BASE_SYMBOL PC_GUEST_SKSE_API_SYMBOL
#else
#define PC_GUEST_BASE_SYMBOL "skyrim_guest_api_v1"
#endif
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

FnModule g_modules[1];
const unsigned g_nmodules = 1;
const char *const g_dll_modules[] = {NULL};
const uint32_t g_dll_bases[] = {0};
static void *library;
static const PcGuestAPI *guest;
static int memory_bound;
static const uint32_t expected_layout[] = PC_GUEST_LAYOUT_INITIALIZER;
unsigned rt_module_count(unsigned index) { return index == 0 && guest ? guest->function_count : 0; }
extern void rt_call_ic(CPU *, uint32_t, const FnEntry **);
extern jmp_buf *rt_setjmp_slot(CPU *, uint32_t);
extern void rt_setjmp3(CPU *);

static int bind_guest(void *candidate) {
    PcGuestGetAPI get = (PcGuestGetAPI)dlsym(candidate, PC_GUEST_BASE_SYMBOL);
    if (!get) { fprintf(stderr, "PRIVATE_GUEST missing API\n"); return -3; }
    const PcGuestAPI *g = get();
    if (!g || g->version != PC_GUEST_ABI || g->bytes != sizeof(*g) || g->reserved ||
        !g->functions || !g->bind_memory || !g->function_count || g->function_count > 1000000 ||
        memcmp(g->cpu_layout, expected_layout, sizeof(expected_layout))) {
        fprintf(stderr, "PRIVATE_GUEST incompatible CPU/ABI layout\n"); return -4;
    }
    for (uint32_t n=0;n<g->function_count;n++) {
        if (!g->functions[n].fn || !g->functions[n].addr ||
            (n && g->functions[n-1].addr >= g->functions[n].addr)) {
            fprintf(stderr, "PRIVATE_GUEST invalid function table at entry %u\n", n);
            return -5;
        }
    }
#ifdef SKYRIM_IOS_CLASSIC_SKSE
    if (pc_guest_bind_classic_skse(candidate) != 0) return -6;
#endif
    guest = g;
    g_modules[0] = (FnModule){"TESV.exe", guest->functions, guest->function_count};
    fprintf(stderr, "PRIVATE_GUEST loaded %u original functions, CPU bytes %u, flags offset %u\n",
            guest->function_count, expected_layout[0], expected_layout[18]);
    return 0;
}

int pc_guest_open(const char *path) {
    if (library || !path) return -1;
    void *candidate = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!candidate) { fprintf(stderr, "PRIVATE_GUEST dlopen failed: %s\n", dlerror()); return -2; }
    int result = bind_guest(candidate);
    if (result) dlclose(candidate);
    else library = candidate;
    return result;
}

int pc_guest_bind_memory(uint8_t *memory) {
    if (!guest || !memory || memory_bound) return -1;
    const PcGuestHostAPI services = {
        PC_GUEST_ABI, sizeof(PcGuestHostAPI), PC_GUEST_LAYOUT_INITIALIZER,
        rt_call, rt_call_ic, rt_unimpl, rt_int3, rt_cpuid, rt_rdtsc,
        f80_load, f80_store, fxam, fprem, fpu_env, rt_setjmp_slot, rt_setjmp3
    };
    int result = guest->bind_memory(&services, memory);
    if (!result) memory_bound = 1;
    return result;
}
