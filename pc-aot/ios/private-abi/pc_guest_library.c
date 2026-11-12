/* Compiled locally together with privately generated game functions. */
#include "PcGuestABI.h"
#include <string.h>

uint8_t *MEM;
static PcGuestHostAPI host;
extern const FnEntry g_fn_table_exe[];
extern const unsigned g_fn_count_exe;
static const uint32_t layout[] = PC_GUEST_LAYOUT_INITIALIZER;

static int bind(const PcGuestHostAPI *h, uint8_t *memory) {
    if (MEM || !h || !memory || h->version != PC_GUEST_ABI || h->bytes != sizeof(*h)) return -1;
    if (memcmp(h->cpu_layout, layout, sizeof(layout))) return -2;
    if (!h->call || !h->call_ic || !h->unimpl || !h->int3 || !h->cpuid || !h->rdtsc ||
        !h->load80 || !h->store80 || !h->examine || !h->remainder || !h->environment ||
        !h->jump_slot || !h->jump_save) return -3;
    host = *h;
    MEM = memory;
    return 0;
}

__attribute__((visibility("default")))
const PcGuestAPI *skyrim_guest_api_v1(void) {
    static PcGuestAPI api = {PC_GUEST_ABI, sizeof(PcGuestAPI), PC_GUEST_LAYOUT_INITIALIZER,
                             g_fn_table_exe, 0, 0, bind};
    api.function_count = g_fn_count_exe;
    return &api;
}

void rt_call(CPU *c, uint32_t t) { host.call(c,t); }
void rt_call_ic(CPU *c, uint32_t t, const FnEntry **cache) { host.call_ic(c,t,cache); }
void rt_unimpl(CPU *c, uint32_t a, const char *w) { host.unimpl(c,a,w); __builtin_trap(); }
void rt_int3(CPU *c, uint32_t a) { host.int3(c,a); __builtin_trap(); }
void rt_cpuid(CPU *c, uint32_t *a, uint32_t *b, uint32_t *d, uint32_t *e) { host.cpuid(c,a,b,d,e); }
uint64_t rt_rdtsc(void) { return host.rdtsc(); }
double f80_load(uint32_t a) { return host.load80(a); }
void f80_store(uint32_t a, double v) { host.store80(a,v); }
void fxam(CPU *c) { host.examine(c); }
void fprem(CPU *c, int ieee) { host.remainder(c,ieee); }
void fpu_env(CPU *c, uint32_t a, int op) { host.environment(c,a,op); }
jmp_buf *rt_setjmp_slot(CPU *c, uint32_t a) { return host.jump_slot(c,a); }
void rt_setjmp3(CPU *c) { host.jump_save(c); }
