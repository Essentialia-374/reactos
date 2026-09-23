/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     AMD64 Application Processor (AP) spinup setup
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 */

#include <asm.inc>
#include <ksamd64.inc>

PUBLIC HalpAPEntry16
PUBLIC HalpAPEntryTempGdt
PUBLIC HalpAPEntryGdtr
PUBLIC HalpAPEntryPageTableRoot
PUBLIC HalpAPEntryJump64
PUBLIC HalpAPEntryProcessorState
PUBLIC HalpAPEntryHalEntry64
PUBLIC HalpAPEntry64Low
PUBLIC HalpAPEntry16End
PUBLIC HalpAPEntry64

.code

/*
 * AP startup trampoline, copied to the low stub by HalStartNextProcessor.
 * The real mode part is hand encoded, ML64 can't assemble 16 bit code.
 */
ALIGN 16
HalpAPEntry16:
    .byte HEX(FA)                           // cli
    .byte HEX(FC)                           // cld
    .byte HEX(8C), HEX(C8)                  // mov ax, cs
    .byte HEX(8E), HEX(D8)                  // mov ds, ax

    /* Load the temporary GDT */
    .byte HEX(66), HEX(0F), HEX(01), HEX(16) // lgdt fword ptr ds:[disp16]
    .word HalpAPEntryGdtr - HalpAPEntry16

    /* Enable PAE */
    .byte HEX(66), HEX(B8)                  // mov eax, CR4_PAE
    .long CR4_PAE
    .byte HEX(0F), HEX(22), HEX(E0)         // mov cr4, eax

    /* Load the temporary page tables */
    .byte HEX(66), HEX(A1)                  // mov eax, dword ptr ds:[disp16]
    .word HalpAPEntryPageTableRoot - HalpAPEntry16
    .byte HEX(0F), HEX(22), HEX(D8)         // mov cr3, eax

    /* Enable long mode and no-execute support */
    .byte HEX(66), HEX(B9)                  // mov ecx, MSR_EFER
    .long MSR_EFER
    .byte HEX(0F), HEX(32)                  // rdmsr
    .byte HEX(66), HEX(0D)                  // or eax, MSR_LME | MSR_NXE
    .long MSR_LME OR MSR_NXE
    .byte HEX(0F), HEX(30)                  // wrmsr

    /* Enable paging and protection, this activates long mode */
    .byte HEX(66), HEX(B8)                  // mov eax, CR0_PG | CR0_PE
    .long HEX(80000001)
    .byte HEX(0F), HEX(22), HEX(C0)         // mov cr0, eax

    /* Jump to the 64 bit code segment (identity mapped) */
    .byte HEX(66), HEX(FF), HEX(2E)         // jmp fword ptr ds:[disp16]
    .word HalpAPEntryJump64 - HalpAPEntry16

    /* 64 bit, identity mapped: jump to HalpAPEntry64 */
HalpAPEntry64Low:
    .byte HEX(48), HEX(8B), HEX(0D)         // mov rcx, qword ptr [rip + rel32]
    .long HalpAPEntryProcessorState - HalpAPEntry64Low_1
HalpAPEntry64Low_1:
    .byte HEX(FF), HEX(25)                  // jmp qword ptr [rip + rel32]
    .long HalpAPEntryHalEntry64 - HalpAPEntry64Low_2
HalpAPEntry64Low_2:

/* Data, patched by HalStartNextProcessor */
ALIGN 16
HalpAPEntryTempGdt:
    .quad HEX(0000000000000000)             // 00: NULL descriptor
    .quad HEX(0000000000000000)             // 08: unused
    .quad HEX(00209B0000000000)             // 10: 64 bit ring 0 code (KGDT64_R0_CODE)
    .quad HEX(00CF93000000FFFF)             // 18: ring 0 data (KGDT64_R0_DATA)
HalpAPEntryGdtr:
    .word 4 * 8 - 1                         // Limit
    .long 0                                 // Base (physical address of HalpAPEntryTempGdt, patched)
    .word 0                                 // Pad
HalpAPEntryPageTableRoot:
    .long 0                                 // Physical address of the temporary PML4
HalpAPEntryJump64:
    .long 0                                 // Physical address of HalpAPEntry64Low
    .word KGDT64_R0_CODE
    .word 0                                 // Pad
HalpAPEntryProcessorState:
    .quad 0                                 // PKPROCESSOR_STATE
HalpAPEntryHalEntry64:
    .quad 0                                 // Address of HalpAPEntry64
HalpAPEntry16End:


/* VOID HalpAPEntry64(PKPROCESSOR_STATE ProcessorState<rcx>) */
ALIGN 16
HalpAPEntry64:
    mov rsi, rcx

    /* Switch to the final page tables and control registers */
    mov rax, [rsi + PsCr3]
    mov cr3, rax
    mov rax, [rsi + PsCr4]
    mov cr4, rax
    mov rax, [rsi + PsCr0]
    mov cr0, rax

    /* Load the final GDT and IDT (skip the KDESCRIPTOR padding) */
    lgdt [rsi + PsGdtr + 6]
    lidt [rsi + PsIdtr + 6]

    /* Load the data segments */
    mov ax, KGDT64_R0_DATA
    mov ss, ax
    mov ax, KGDT64_R3_DATA OR 3 // RPL 3
    mov ds, ax
    mov es, ax
    mov ax, KGDT64_R3_CMTEB OR 3 // RPL 3
    mov fs, ax
    xor eax, eax
    mov gs, ax

    /* Set the GS base (loading gs cleared it) */
    mov ecx, MSR_GS_BASE
    mov eax, [rsi + PsSpecialRegisters + SrMsrGsBase]
    mov edx, [rsi + PsSpecialRegisters + SrMsrGsBase + 4]
    wrmsr

    /* Load the task register */
    ltr word ptr [rsi + PsTr]

    /* Switch to the new stack */
    mov rsp, [rsi + PsContextFrame + CxRsp]

    /* Entry point and loader block */
    mov rax, [rsi + PsContextFrame + CxRip]
    mov rcx, [rsi + PsContextFrame + CxRcx]

    /* iret to reload CS. RSP as if called: return address + home space */
    lea rdx, [rsp - 8 - 32]
    push KGDT64_R0_DATA                     // SS
    push rdx                                // RSP
    push 2                                  // RFLAGS (interrupts disabled)
    push KGDT64_R0_CODE                     // CS
    push rax                                // RIP

    /* Clear the remaining registers */
    xor eax, eax
    xor edx, edx
    xor ebx, ebx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d

    iretq

END
