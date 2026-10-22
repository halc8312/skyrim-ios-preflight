/* Three self-authored synthetic entries. Compiled with real Apple SDK only.
 * The generic owned_abi.cpp/jump_tail.S provide the API and native callbacks.
 * This file is never generated from a title, ROM, XEX or private framework. */
#include "PrivateGuestABI.h"
#include "ppc_config.h"
#include "ppc_context.h"
#include <TargetConditionals.h>
#include <cstddef>
#include <new>
#if !defined(__aarch64__) || defined(__arm64e__) || !TARGET_OS_IOS || TARGET_OS_MACCATALYST
#error Plain ARM64 iOS/device or iOS/Simulator SDK only
#endif
extern "C" void skyrt_kernel_call(uint32_t,uint32_t,void *,uint8_t *);
static constexpr uint32_t MAIN=0x82001000u,JUMP=0x82001004u,RETURN=0x82001008u;
static constexpr uint32_t FREQUENCY=0x82001800u,CLEANUP=0x82001804u;
static const uint32_t functions[]={MAIN,JUMP,RETURN};
extern "C" const PGRequirementsV1 pg_private_requirements={
    PG_ABI_VERSION_1,sizeof(PGRequirementsV1),64,1,1,0,
    {offsetof(PPCContext,r3),offsetof(PPCContext,r0),offsetof(PPCContext,r1),offsetof(PPCContext,r2),
     offsetof(PPCContext,r4),offsetof(PPCContext,r13),offsetof(PPCContext,r31),offsetof(PPCContext,lr),
     offsetof(PPCContext,ctr),offsetof(PPCContext,xer),offsetof(PPCContext,reserved),offsetof(PPCContext,msr),
     offsetof(PPCContext,cr0),offsetof(PPCContext,fpscr),offsetof(PPCContext,f0),offsetof(PPCContext,f1),
     offsetof(PPCContext,v0),offsetof(PPCContext,v127),sizeof(PPCContext),alignof(PPCContext)},
    sizeof(jmp_buf),alignof(jmp_buf),PG_EA_OFFSET_COUNT,PG_WRITTEN_COUNT
};
static_assert(sizeof(PPCContext)==2688&&alignof(PPCContext)==64);
static_assert(__atomic_always_lock_free(4,nullptr)&&__atomic_always_lock_free(1,nullptr));
extern "C" void pg_private_init_context(void *storage){
    auto *context=new(storage) PPCContext;
    // Volatile loop prevents an implicit memset dependency before FP capture.
    auto *bytes=static_cast<volatile unsigned char *>(storage);
    for(size_t index=0;index<sizeof(PPCContext);++index)bytes[index]=0;
    context->fpscr.loadFromHost();
}
extern "C" int pg_private_address_valid(uint32_t address){return address==MAIN||address==JUMP||address==RETURN;}
struct MainCleanup{
    uint8_t *base;
    ~MainCleanup() noexcept {PPC_STORE_U32(CLEANUP,PPC_LOAD_U32(CLEANUP)+1);}
};
static void jump_same_live_frame(PPCContext &context){
    // The same still-live C++ caller owns both its opaque SDK buffer and the
    // snapshot. No Rust/Swift frame is skipped and no C++ destructor is skipped.
    alignas(16) jmp_buf buffer;
    PPCContext snapshot;
    memcpy(&snapshot,&context,sizeof(snapshot));
    const uint32_t fpcr=context.fpscr.getcsr();
    volatile int resumed=0;
    // setjmp is the entire controlling expression, preserving the C/C++ caller
    // form restriction rather than relying on an initializer extension.
    switch(setjmp(buffer)){
        case 0: resumed=1;context.r3.u64=0xdead;longjmp(buffer,7);__builtin_trap();
        case 7: if(resumed!=1)__builtin_trap();break;
        default: __builtin_trap();
    }
    memcpy(&context,&snapshot,sizeof(snapshot));
    context.fpscr.setcsr(fpcr);
    if(context.fpscr.getcsr()!=fpcr)__builtin_trap();
    context.r3.u64=0x1111;
}
extern "C" void pg_private_call_guest(uint32_t address,void *storage,uint8_t *base){
    auto &context=*static_cast<PPCContext *>(storage);
    if(address==JUMP){jump_same_live_frame(context);return;}
    if(address==RETURN){context.r3.u64=0x2222;return;}
    if(address!=MAIN)__builtin_trap();
    skyrt_kernel_call(0,0x83,&context,base);
    PPC_STORE_U32(FREQUENCY,context.r3.u32);
    MainCleanup cleanup{base};
    context.r3.u64=0;
    skyrt_kernel_call(0,0x19,&context,base);
    // ExTerminateThread must propagate actual Rust ThreadExit, not return0.
    __builtin_trap();
}
extern "C" PG_EXPORT void skyrt_ppc_config(uint64_t out[4]){
    out[0]=PPC_IMAGE_BASE;out[1]=PPC_IMAGE_SIZE;out[2]=PPC_CODE_BASE;out[3]=PPC_CODE_SIZE;
}
extern "C" PG_EXPORT const uint32_t *skyrt_func_addrs(size_t *count){if(!count)return nullptr;*count=3;return functions;}
