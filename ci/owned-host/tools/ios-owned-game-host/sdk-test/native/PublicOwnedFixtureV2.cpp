/* Entirely self-authored public V2 SDK fixture. No title/generated input.
 * Generic private_layout_v2.cpp owns requirements/init/dispatch ABI wrappers. */
#include "PrivateGuestABIV2.h"
#include "ppc_config.h"
#include "ppc_context.h"
#include <TargetConditionals.h>
#include <new>
#if !defined(__aarch64__) || defined(__arm64e__) || !TARGET_OS_IOS || TARGET_OS_MACCATALYST
#error Plain ARM64 iOS SDK only
#endif
#if !SKYRT_XMA_MMIO_BRIDGE
#error The public V2 fixture must compile the real MMIO bridge
#endif
extern "C" void skyrt_kernel_call(uint32_t,uint32_t,void *,uint8_t *);
static constexpr uint32_t MAIN=0x82001000u,JUMP=0x82001004u,RETURN=0x82001008u;
static constexpr uint32_t FREQUENCY=0x82001800u,CLEANUP=0x82001804u;
static constexpr uint32_t JUMP_RESULT=0x82001808u,MMIO_RESULT=0x8200180cu;
static constexpr uint32_t CURRENT=0x7fea1818u,NEXT=0x7fea181cu,LOCK=0x7fea1a40u;
static const uint32_t functions[]={MAIN,JUMP,RETURN};
static_assert(sizeof(PPCContext)==2688 && alignof(PPCContext)==64);
extern "C" void skyrt_init_context(PPCContext *storage) {
    auto *context=new(storage) PPCContext;
    auto *bytes=reinterpret_cast<volatile unsigned char *>(storage);
    for(size_t i=0;i<sizeof(PPCContext);++i)bytes[i]=0;
    context->fpscr.loadFromHost();
}
static void jump_same_live_frame(PPCContext &context) {
    // No restrict-qualified capturing reference, skipped destructor, expired
    // buffer, or Rust/Swift frame. The native provider must return to this frame.
    alignas(16) jmp_buf buffer;
    PPCContext snapshot;
    memcpy(&snapshot,&context,sizeof(snapshot));
    const uint32_t fpcr=context.fpscr.getcsr();
    volatile int resumed=0;
    switch(setjmp(buffer)) {
        case 0: resumed=1;context.r3.u64=0xdead;longjmp(buffer,7);__builtin_trap();
        case 7: if(resumed!=1)__builtin_trap();break;
        default: __builtin_trap();
    }
    memcpy(&context,&snapshot,sizeof(snapshot));
    context.fpscr.setcsr(fpcr);
    if(context.fpscr.getcsr()!=fpcr)__builtin_trap();
    context.r3.u64=0x1111;
}
struct MainCleanup {
    uint8_t *base;
    ~MainCleanup() noexcept { PPC_STORE_U32(CLEANUP,PPC_LOAD_U32(CLEANUP)+1); }
};
extern "C" void skyrt_call_func(uint32_t address,PPCContext &context,uint8_t *base) {
    if(address==JUMP){jump_same_live_frame(context);return;}
    if(address==RETURN){context.r3.u64=0x2222;return;}
    if(address!=MAIN)__builtin_trap();
    jump_same_live_frame(context);
    PPC_STORE_U32(JUMP_RESULT,context.r3.u32);
    // These are actual header callbacks. CURRENT changes the independent NEXT
    // register, which ordinary backing reads cannot reproduce. LOCK is read
    // back separately from the Kernel's command state by the Rust SDK probe.
    const uint32_t next_before=__builtin_bswap32(PPC_LOAD_U32(NEXT));
    if(next_before>=320u || __builtin_bswap32(PPC_LOAD_U32(CURRENT))!=next_before ||
       __builtin_bswap32(PPC_LOAD_U32(NEXT))!=(next_before+1u)%320u)__builtin_trap();
    PPC_STORE_U32(LOCK,__builtin_bswap32(0x80000001u));
    if(__builtin_bswap32(PPC_LOAD_U32(LOCK))!=0x80000001u)__builtin_trap();
    PPC_STORE_U32(MMIO_RESULT,1u);
    skyrt_kernel_call(0,0x83,&context,base);
    PPC_STORE_U32(FREQUENCY,context.r3.u32);
    MainCleanup cleanup{base};
    context.r3.u64=0;
    skyrt_kernel_call(0,0x19,&context,base);
    __builtin_trap(); // ExTerminateThread must unwind through both C++ guards.
}
extern "C" PG_EXPORT void skyrt_ppc_config(uint64_t out[4]) {
    out[0]=PPC_IMAGE_BASE;out[1]=PPC_IMAGE_SIZE;out[2]=PPC_CODE_BASE;out[3]=PPC_CODE_SIZE;
}
extern "C" PG_EXPORT const uint32_t *skyrt_func_addrs(size_t *count) {
    if(!count)return nullptr;*count=3;return functions;
}
