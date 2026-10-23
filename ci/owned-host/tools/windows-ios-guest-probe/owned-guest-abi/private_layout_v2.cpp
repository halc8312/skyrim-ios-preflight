// Generic compiler-derived layout, instantiated only against local headers.
#include "ppc_config.h"
#include "ppc_context.h"
#include "PrivateGuestABIV2.h"
#include <cstddef>
#if !defined(__aarch64__) || defined(__arm64e__)
#error Plain ARM64 only
#endif
extern "C" const PGRequirementsV2 pg_private_requirements = {
    PG_ABI_VERSION_2, sizeof(PGRequirementsV2), 64, 1, 1, 0,
    { offsetof(PPCContext,r3), offsetof(PPCContext,r0), offsetof(PPCContext,r1), offsetof(PPCContext,r2),
      offsetof(PPCContext,r4), offsetof(PPCContext,r13), offsetof(PPCContext,r31), offsetof(PPCContext,lr),
      offsetof(PPCContext,ctr), offsetof(PPCContext,xer), offsetof(PPCContext,reserved), offsetof(PPCContext,msr),
      offsetof(PPCContext,cr0), offsetof(PPCContext,fpscr), offsetof(PPCContext,f0), offsetof(PPCContext,f1),
      offsetof(PPCContext,v0), offsetof(PPCContext,v127), sizeof(PPCContext), alignof(PPCContext) },
    sizeof(jmp_buf), alignof(jmp_buf), PG_EA_OFFSET_COUNT, PG_WRITTEN_COUNT
};
extern "C" void skyrt_init_context(PPCContext*);
extern "C" void pg_private_init_context(void *context) { skyrt_init_context(static_cast<PPCContext*>(context)); }
extern "C" int pg_private_address_valid(uint32_t address) {
    return (address & 3) == 0 && address >= PPC_CODE_BASE && uint64_t(address) - PPC_CODE_BASE < PPC_CODE_SIZE;
}
extern "C" void pg_private_call_guest(uint32_t address, void *context, uint8_t *base) {
    skyrt_call_func(address, *static_cast<PPCContext*>(context), base);
}
