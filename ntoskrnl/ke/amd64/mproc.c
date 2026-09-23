/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Architecture specific source file to hold multiprocessor functions
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

/* Per processor data of an application processor */
typedef struct _APINFO
{
    DECLSPEC_ALIGN(PAGE_SIZE) KIPCR Pcr;
    DECLSPEC_ALIGN(PAGE_SIZE) KIDTENTRY64 Idt[256];
    DECLSPEC_ALIGN(PAGE_SIZE) KGDTENTRY64 Gdt[PAGE_SIZE / sizeof(KGDTENTRY64)];
    DECLSPEC_ALIGN(16) KTSS64 Tss;
    ETHREAD Thread;
} APINFO, *PAPINFO;

/* AP startup timeout, in us */
#define AP_STARTUP_TIMEOUT_US (10 * 1000 * 1000)
#define AP_STARTUP_POLL_US    10

/* FUNCTIONS *****************************************************************/

CODE_SEG("INIT")
static
VOID
KiFreeApResources(
    _In_opt_ PAPINFO APInfo,
    _In_opt_ PVOID KernelStack,
    _In_opt_ PVOID DpcStack,
    _In_opt_ PVOID DoubleFaultStack,
    _In_opt_ PVOID NmiStack)
{
    if (APInfo) ExFreePoolWithTag(APInfo, TAG_KERNEL);
    if (KernelStack) MmDeleteKernelStack(KernelStack, FALSE);
    if (DpcStack) MmDeleteKernelStack(DpcStack, FALSE);
    if (DoubleFaultStack) MmDeleteKernelStack(DoubleFaultStack, FALSE);
    if (NmiStack) MmDeleteKernelStack(NmiStack, FALSE);
}

CODE_SEG("INIT")
static
BOOLEAN
KiWaitForApStartup(
    _In_ ULONG ProcessorNumber)
{
    ULONG Waited;

    /* The AP clears the PRCB in the loader block when it is done */
    for (Waited = 0; Waited < AP_STARTUP_TIMEOUT_US; Waited += AP_STARTUP_POLL_US)
    {
        if (*(volatile ULONG_PTR *)&KeLoaderBlock->Prcb == 0)
        {
            KeMemoryBarrier();
            return TRUE;
        }

        KeStallExecutionProcessor(AP_STARTUP_POLL_US);
    }

    DPRINT1("Processor %lu did not start (KeNumberProcessors = %u)\n",
            ProcessorNumber, KeNumberProcessors);
    return FALSE;
}

CODE_SEG("INIT")
VOID
NTAPI
KeStartAllProcessors(VOID)
{
    PAPINFO APInfo;
    PVOID KernelStack, DpcStack, DoubleFaultStack, NmiStack;
    PKPROCESSOR_STATE ProcessorState;
    KDESCRIPTOR BspGdt = {{0}, 0, 0}, BspIdt = {{0}, 0, 0};
    ULONG ProcessorCount;
    ULONG MaximumProcessors;

    /* NOTE: NT6+ HAL exports HalEnumerateProcessors() and
     * HalQueryMaximumProcessorCount() that help determining
     * the number of detected processors on the system. */
    MaximumProcessors = KeMaximumProcessors;

    /* Limit the number of processors we can start at run-time */
    if (KeNumprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeNumprocSpecified);

    /* Limit also the number of processors we can start during boot-time */
    if (KeBootprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeBootprocSpecified);

    MaximumProcessors = min(MaximumProcessors, MAXIMUM_PROCESSORS);

    /* The APs get a copy of the boot processor's GDT and IDT */
    __sgdt(&BspGdt.Limit);
    __sidt(&BspIdt.Limit);
    ASSERT(BspGdt.Limit + 1U <= RTL_FIELD_SIZE(APINFO, Gdt));
    ASSERT(BspIdt.Limit + 1U <= RTL_FIELD_SIZE(APINFO, Idt));

    // TODO: Support processor nodes

    /* Start ProcessorCount at 1 because we already have the boot CPU */
    for (ProcessorCount = 1; ProcessorCount < MaximumProcessors; ++ProcessorCount)
    {
        KernelStack = NULL;
        DpcStack = NULL;
        DoubleFaultStack = NULL;
        NmiStack = NULL;

        /* Allocate the structures for the new CPU */
        APInfo = ExAllocatePoolZero(NonPagedPool, sizeof(*APInfo), TAG_KERNEL);
        if (APInfo) ASSERT(ALIGN_DOWN_POINTER_BY(APInfo, PAGE_SIZE) == APInfo);
        if (APInfo) KernelStack = MmCreateKernelStack(FALSE, 0);
        if (KernelStack) DpcStack = MmCreateKernelStack(FALSE, 0);
        if (DpcStack) DoubleFaultStack = MmCreateKernelStack(FALSE, 0);
        if (DoubleFaultStack) NmiStack = MmCreateKernelStack(FALSE, 0);
        if (!NmiStack)
        {
            DPRINT1("Failed to allocate resources for processor %lu\n", ProcessorCount);
            KiFreeApResources(APInfo, KernelStack, DpcStack, DoubleFaultStack, NmiStack);
            break;
        }

        /* Copy the descriptor tables of the boot processor */
        RtlCopyMemory(APInfo->Gdt, BspGdt.Base, BspGdt.Limit + 1);
        RtlCopyMemory(APInfo->Idt, BspIdt.Base, BspIdt.Limit + 1);

        /* Initialize the PCR, PRCB and TSS */
        KiInitializeProcessorBootStructures(ProcessorCount,
                                            &APInfo->Pcr,
                                            APInfo->Gdt,
                                            APInfo->Idt,
                                            &APInfo->Tss,
                                            &APInfo->Thread.Tcb,
                                            KernelStack,
                                            DpcStack,
                                            DoubleFaultStack,
                                            NmiStack);

        /* Fill the processor state for the HAL */
        ProcessorState = &APInfo->Pcr.Prcb.ProcessorState;
        RtlZeroMemory(ProcessorState, sizeof(*ProcessorState));

        ProcessorState->SpecialRegisters.Cr0 = __readcr0();
        ProcessorState->SpecialRegisters.Cr3 = KiInitialProcess.Pcb.DirectoryTableBase[0];
        ProcessorState->SpecialRegisters.Cr4 = __readcr4();

        ProcessorState->SpecialRegisters.Gdtr.Base = APInfo->Gdt;
        ProcessorState->SpecialRegisters.Gdtr.Limit = BspGdt.Limit;
        ProcessorState->SpecialRegisters.Idtr.Base = APInfo->Idt;
        ProcessorState->SpecialRegisters.Idtr.Limit = BspIdt.Limit;
        ProcessorState->SpecialRegisters.Tr = KGDT64_SYS_TSS;
        ProcessorState->SpecialRegisters.MsrGsBase = (ULONG64)&APInfo->Pcr;

        ProcessorState->ContextFrame.SegCs = KGDT64_R0_CODE;
        ProcessorState->ContextFrame.SegSs = KGDT64_R0_DATA;
        ProcessorState->ContextFrame.Rsp = (ULONG64)KernelStack;
        ProcessorState->ContextFrame.Rip = (ULONG64)KiSystemStartup;
        ProcessorState->ContextFrame.Rcx = (ULONG64)KeLoaderBlock;
        ProcessorState->ContextFrame.EFlags = __readeflags() & ~EFLAGS_INTERRUPT_MASK;

        /* Update the loader block for the new processor */
        KeLoaderBlock->KernelStack = (ULONG_PTR)KernelStack;
        KeLoaderBlock->Prcb = (ULONG_PTR)&APInfo->Pcr.Prcb;
        KeLoaderBlock->Thread = (ULONG_PTR)&APInfo->Thread.Tcb;

        /* Start the CPU */
        DPRINT("Attempting to start processor %lu\n", ProcessorCount);
        if (!HalStartNextProcessor(KeLoaderBlock, ProcessorState))
        {
            /* No more processors, clean up */
            KeLoaderBlock->Prcb = 0;
            KiFreeApResources(APInfo, KernelStack, DpcStack, DoubleFaultStack, NmiStack);
            break;
        }

        /* And wait for it to start */
        if (!KiWaitForApStartup(ProcessorCount))
        {
            /* We can't stop it, and it could still use the loader block later */
            KeBugCheckEx(MULTIPROCESSOR_CONFIGURATION_NOT_SUPPORTED,
                         ProcessorCount,
                         KeNumberProcessors,
                         (ULONG_PTR)APInfo,
                         0);
        }
    }

    DPRINT1("KeStartAllProcessors: %u processor(s) running\n", KeNumberProcessors);
}
