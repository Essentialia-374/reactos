
#include <asm.inc>

#include <ksamd64.inc>

.code64

/*
 * VOID KdbpStackSwitchAndCall(PVOID NewStack<rcx>, VOID (*Function)(VOID)<rdx>)
 */
PUBLIC KdbpStackSwitchAndCall
KdbpStackSwitchAndCall:

    /* Save old stack */
    mov rax, rsp

    /* Set new stack, aligned to 16 bytes */
    and rcx, -16
    mov rsp, rcx

    /* Home space for the callee and a slot for the old stack */
    sub rsp, HEX(30)
    mov [rsp + HEX(20)], rax

    /* Call function */
    call rdx

    /* Restore old stack */
    mov rsp, [rsp + HEX(20)]

    /* Return */
    ret

END
