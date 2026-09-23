/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     AMD64 Application Processor (AP) spinup setup
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#include <smp.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

extern BOOLEAN HalpOnlyBootProcessor;

extern PHYSICAL_ADDRESS HalpLowStubPhysicalAddress;
extern PVOID HalpLowStub;
extern HALP_APIC_INFO_TABLE HalpApicInfoTable;

/* apentry.S */
extern UCHAR HalpAPEntry16[];
extern UCHAR HalpAPEntryTempGdt[];
extern UCHAR HalpAPEntryGdtr[];
extern UCHAR HalpAPEntryPageTableRoot[];
extern UCHAR HalpAPEntryJump64[];
extern UCHAR HalpAPEntryProcessorState[];
extern UCHAR HalpAPEntryHalEntry64[];
extern UCHAR HalpAPEntry64Low[];
extern UCHAR HalpAPEntry16End[];
extern UCHAR HalpAPEntry64[];

ULONG HalpStartedProcessorCount = 1;

/* Low stub pages: trampoline, then the temporary PML4, PDPT, PD and PT */
#define LOW_STUB_PML4_PAGE 1
#define LOW_STUB_PDPT_PAGE 2
#define LOW_STUB_PD_PAGE   3
#define LOW_STUB_PT_PAGE   4
C_ASSERT(HALP_LOW_STUB_SIZE_IN_PAGES == 5);

/* Present and writable */
#define PTE_TABLE_FLAGS 0x3ULL

/* Trampoline field inside the low stub copy */
#define LOW_STUB_FIELD(Type, Symbol) \
    ((Type*)((PUCHAR)HalpLowStub + ((ULONG_PTR)(Symbol) - (ULONG_PTR)HalpAPEntry16)))

/* FUNCTIONS *****************************************************************/

static
ULONG64
HalpLowStubPagePhysical(
    _In_ ULONG Page)
{
    return HalpLowStubPhysicalAddress.QuadPart + (ULONG64)Page * PAGE_SIZE;
}

static
PULONG64
HalpLowStubPage(
    _In_ ULONG Page)
{
    return (PULONG64)((PUCHAR)HalpLowStub + (ULONG_PTR)Page * PAGE_SIZE);
}

/* Temporary page tables: identity map the low stub, keep the kernel mappings */
static
ULONG
HalpSetupTemporaryMappings(VOID)
{
    PULONG64 Pml4 = HalpLowStubPage(LOW_STUB_PML4_PAGE);
    PULONG64 Pdpt = HalpLowStubPage(LOW_STUB_PDPT_PAGE);
    PULONG64 Pd = HalpLowStubPage(LOW_STUB_PD_PAGE);
    PULONG64 Pt = HalpLowStubPage(LOW_STUB_PT_PAGE);
    ULONG64 StubPhysical = HalpLowStubPhysicalAddress.QuadPart;
    ULONG i, Pti;

    /* Below 1MB, so one page table covers it */
    ASSERT(StubPhysical + HALP_LOW_STUB_SIZE_IN_PAGES * PAGE_SIZE <= 0x100000);

    /* Copy the current PML4 for the kernel mappings */
    RtlCopyMemory(Pml4, (PVOID)PXE_BASE, PAGE_SIZE);

    /* Build the identity mapping of the low stub */
    RtlZeroMemory(Pdpt, PAGE_SIZE);
    RtlZeroMemory(Pd, PAGE_SIZE);
    RtlZeroMemory(Pt, PAGE_SIZE);
    Pml4[0] = HalpLowStubPagePhysical(LOW_STUB_PDPT_PAGE) | PTE_TABLE_FLAGS;
    Pdpt[0] = HalpLowStubPagePhysical(LOW_STUB_PD_PAGE) | PTE_TABLE_FLAGS;
    Pd[0] = HalpLowStubPagePhysical(LOW_STUB_PT_PAGE) | PTE_TABLE_FLAGS;
    for (i = 0; i < HALP_LOW_STUB_SIZE_IN_PAGES; i++)
    {
        Pti = (ULONG)((StubPhysical >> PAGE_SHIFT) + i);
        Pt[Pti] = HalpLowStubPagePhysical(i) | PTE_TABLE_FLAGS;
    }

    return (ULONG)HalpLowStubPagePhysical(LOW_STUB_PML4_PAGE);
}

BOOLEAN
NTAPI
HalStartNextProcessor(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ PKPROCESSOR_STATE ProcessorState)
{
    ULONG64 StubPhysical = HalpLowStubPhysicalAddress.QuadPart;

    UNREFERENCED_PARAMETER(LoaderBlock);

    /* Bail out if we only use the boot CPU */
    if (HalpOnlyBootProcessor)
        return FALSE;

    /* Bail out if we have started all available CPUs */
    if (HalpStartedProcessorCount >= HalpApicInfoTable.ProcessorCount)
        return FALSE;

    if ((HalpLowStub == NULL) || (StubPhysical == 0))
    {
        DPRINT1("No low stub, cannot start application processors\n");
        return FALSE;
    }

    ASSERT((ULONG_PTR)(HalpAPEntry16End - HalpAPEntry16) <= PAGE_SIZE);

    /* Put the trampoline code into low memory */
    RtlCopyMemory(HalpLowStub, HalpAPEntry16, HalpAPEntry16End - HalpAPEntry16);

    /* Patch the trampoline data */
    *LOW_STUB_FIELD(ULONG, HalpAPEntryGdtr + sizeof(USHORT)) =
        (ULONG)(StubPhysical + (HalpAPEntryTempGdt - HalpAPEntry16));

    *LOW_STUB_FIELD(ULONG, HalpAPEntryPageTableRoot) = HalpSetupTemporaryMappings();

    *LOW_STUB_FIELD(ULONG, HalpAPEntryJump64) =
        (ULONG)(StubPhysical + (HalpAPEntry64Low - HalpAPEntry16));

    *LOW_STUB_FIELD(ULONG64, HalpAPEntryProcessorState) = (ULONG64)ProcessorState;
    *LOW_STUB_FIELD(ULONG64, HalpAPEntryHalEntry64) = (ULONG64)HalpAPEntry64;

    KeMemoryBarrier();

    /* Start the AP */
    ApicStartApplicationProcessor(HalpStartedProcessorCount, HalpLowStubPhysicalAddress);

    HalpStartedProcessorCount++;

    return TRUE;
}
