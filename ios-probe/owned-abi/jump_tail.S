/* No SP/LR or x0..x15 changes, no call, no return frame. Native provider owns
 * the actual setjmp/longjmp implementation and same-thread frame lifetime. */
#if !defined(__aarch64__) || defined(__arm64e__)
#error Plain ARM64 only
#endif
.text
.p2align 2
.globl _setjmp
.private_extern _setjmp
_setjmp:
    adrp x17, _pg_jump_targets@PAGE
    add x17, x17, _pg_jump_targets@PAGEOFF
    ldar x16, [x17]
    cbz x16, 1f
    br x16
1:  brk #0x501
.p2align 2
.globl _longjmp
.private_extern _longjmp
_longjmp:
    adrp x17, _pg_jump_targets@PAGE
    add x17, x17, _pg_jump_targets@PAGEOFF
    add x17, x17, #8
    ldar x16, [x17]
    cbz x16, 2f
    br x16
2:  brk #0x502
.p2align 2
.globl __Unwind_Resume
.private_extern __Unwind_Resume
__Unwind_Resume:
    adrp x17, _pg_jump_targets@PAGE
    add x17, x17, _pg_jump_targets@PAGEOFF
    add x17, x17, #16
    ldar x16, [x17]
    cbz x16, 3f
    br x16
3:  brk #0x503
