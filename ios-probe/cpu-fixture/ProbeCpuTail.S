// Pure arm64 tail trampolines. SDK API pointers are initialized by C++.
// x0/x1, SP, x29/x30, and all callee-saved registers remain untouched.
// No C wrapper frame is introduced between the caller and real setjmp.
#if !defined(__aarch64__) || defined(__arm64e__)
#error "This execution fixture supports plain arm64 only"
#endif
.text
.p2align 2
.globl _probe_cpu_setjmp_tail
.private_extern _probe_cpu_setjmp_tail
_probe_cpu_setjmp_tail:
    adrp x16, _probe_cpu_jump_callbacks@PAGE
    add x16, x16, _probe_cpu_jump_callbacks@PAGEOFF
    ldr x17, [x16, #0]
    br x17

.p2align 2
.globl _probe_cpu_longjmp_tail
.private_extern _probe_cpu_longjmp_tail
_probe_cpu_longjmp_tail:
    adrp x16, _probe_cpu_jump_callbacks@PAGE
    add x16, x16, _probe_cpu_jump_callbacks@PAGEOFF
    ldr x17, [x16, #8]
    br x17
.subsections_via_symbols
