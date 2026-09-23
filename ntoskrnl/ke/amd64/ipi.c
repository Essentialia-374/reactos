/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     IPI code for x64
 * COPYRIGHT:   Copyright 2023 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

extern KSPIN_LOCK KiReverseStallIpiLock;

/* FUNCTIONS *****************************************************************/

/* Post requests into our mailbox slot of each target PRCB */
static
VOID
KiIpiPostRequests(
    _In_ KAFFINITY TargetSet,
    _In_ LONG64 Requests)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    PKPRCB TargetPrcb;
    KAFFINITY RemainingSet = TargetSet;
    ULONG ProcessorIndex;

    while (RemainingSet != 0)
    {
        NT_VERIFY(BitScanForwardAffinity(&ProcessorIndex, RemainingSet) != 0);
        RemainingSet &= ~AFFINITY_MASK(ProcessorIndex);
        TargetPrcb = KiProcessorBlock[ProcessorIndex];

        /* Set the requests, then our sender bit (both are full barriers) */
        InterlockedOr64(&TargetPrcb->RequestMailbox[CurrentPrcb->Number].RequestSummary,
                        Requests);
        InterlockedOr64((PLONG64)&TargetPrcb->SenderSummary, CurrentPrcb->SetMember);
    }
}

/*
 * APC and DPC requests use the IPI vector, not the APC/DPC vectors: an idle
 * processor halts at DISPATCH_LEVEL, where those are masked.
 */
VOID
FASTCALL
KiIpiSend(
    _In_ KAFFINITY TargetSet,
    _In_ ULONG IpiRequest)
{
    if (IpiRequest == IPI_FREEZE)
    {
        /* On x64 the freeze IPI is an NMI */
        HalSendNMI(TargetSet);
        return;
    }

    ASSERT((IpiRequest & ~(IPI_APC | IPI_DPC)) == 0);

    KiIpiPostRequests(TargetSet, IpiRequest);
    HalRequestIpi(TargetSet);
}

/*
 * Called at SYNCH_LEVEL with interrupts enabled. The caller must wait with
 * KiIpiWaitForPacketTargets before sending another packet.
 */
VOID
NTAPI
KiIpiSendPacket(
    _In_ KAFFINITY TargetProcessors,
    _In_ PKIPI_WORKER WorkerFunction,
    _In_ PKIPI_BROADCAST_WORKER BroadcastFunction,
    _In_ ULONG_PTR Context,
    _In_ PULONG Count)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    PKPRCB TargetPrcb;
    PKREQUEST_PACKET RequestPacket;
    KAFFINITY RemainingSet;
    ULONG ProcessorIndex;

    ASSERT(KeGetCurrentIrql() == SYNCH_LEVEL);
    ASSERT(__readeflags() & EFLAGS_INTERRUPT_MASK);
    ASSERT(TargetProcessors != 0);
    ASSERT((TargetProcessors & CurrentPrcb->SetMember) == 0);
    ASSERT(CurrentPrcb->TargetSet == 0);

    /* Each target clears its bit when done */
    InterlockedExchange64((PLONG64)&CurrentPrcb->TargetSet, TargetProcessors);

    RemainingSet = TargetProcessors;
    while (RemainingSet != 0)
    {
        NT_VERIFY(BitScanForwardAffinity(&ProcessorIndex, RemainingSet) != 0);
        RemainingSet &= ~AFFINITY_MASK(ProcessorIndex);
        TargetPrcb = KiProcessorBlock[ProcessorIndex];

        /* Fill our mailbox slot */
        RequestPacket = &TargetPrcb->RequestMailbox[CurrentPrcb->Number].RequestPacket;
        RequestPacket->WorkerRoutine = (PVOID)WorkerFunction;
        RequestPacket->CurrentPacket[0] = (PVOID)BroadcastFunction;
        RequestPacket->CurrentPacket[1] = (PVOID)Context;
        RequestPacket->CurrentPacket[2] = (PVOID)Count;
    }

    KiIpiPostRequests(TargetProcessors, IPI_PACKET_READY);
    HalRequestIpi(TargetProcessors);
}

VOID
FASTCALL
KiIpiSignalPacketDone(
    _In_ PKIPI_CONTEXT PacketContext)
{
    PKPRCB SenderPrcb = (PKPRCB)PacketContext;

    InterlockedAnd64((PLONG64)&SenderPrcb->TargetSet,
                     ~KeGetCurrentPrcb()->SetMember);
}

VOID
NTAPI
KiIpiWaitForPacketTargets(VOID)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();

    /* We must stay interruptible, other processors may send us requests */
    ASSERT(KeGetCurrentIrql() == SYNCH_LEVEL);
    ASSERT(__readeflags() & EFLAGS_INTERRUPT_MASK);

    while (*(volatile UINT64 *)&CurrentPrcb->TargetSet != 0)
    {
        YieldProcessor();
    }

    KeMemoryBarrier();
}

/* Called from KiIpiInterrupt at IPI_LEVEL, interrupts disabled */
VOID
NTAPI
KiIpiProcessRequests(VOID)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    PREQUEST_MAILBOX Mailbox;
    PKREQUEST_PACKET RequestPacket;
    PKIPI_WORKER WorkerRoutine;
    KAFFINITY SenderSet;
    ULONG SenderIndex;
    LONG64 Requests;

    while ((SenderSet = InterlockedExchange64((PLONG64)&CurrentPrcb->SenderSummary, 0)) != 0)
    {
        do
        {
            NT_VERIFY(BitScanForwardAffinity(&SenderIndex, SenderSet) != 0);
            SenderSet &= ~AFFINITY_MASK(SenderIndex);

            Mailbox = &CurrentPrcb->RequestMailbox[SenderIndex];
            Requests = InterlockedExchange64(&Mailbox->RequestSummary, 0);

            if (Requests & IPI_APC)
            {
                HalRequestSoftwareInterrupt(APC_LEVEL);
            }

            /* DpcInterruptRequested is set by the sender, not here */
            if (Requests & IPI_DPC)
            {
                HalRequestSoftwareInterrupt(DISPATCH_LEVEL);
            }

            if (Requests & IPI_PACKET_READY)
            {
                RequestPacket = &Mailbox->RequestPacket;
                WorkerRoutine = (PKIPI_WORKER)RequestPacket->WorkerRoutine;
                WorkerRoutine(KiProcessorBlock[SenderIndex],
                              RequestPacket->CurrentPacket[0],
                              RequestPacket->CurrentPacket[1],
                              RequestPacket->CurrentPacket[2]);
            }
        } while (SenderSet != 0);
    }
}

static
VOID
NTAPI
KiIpiGenericCallTarget(
    _In_ PKIPI_CONTEXT PacketContext,
    _In_ PVOID BroadcastFunction,
    _In_ PVOID Argument,
    _In_ PVOID Count)
{
    volatile LONG *Barrier = (volatile LONG *)Count;

    /* Arrive and wait for the sender to release us */
    InterlockedDecrement((PLONG)Barrier);
    while (*Barrier != 0)
    {
        YieldProcessor();
    }

    ((PKIPI_BROADCAST_WORKER)BroadcastFunction)((ULONG_PTR)Argument);

    KiIpiSignalPacketDone(PacketContext);
}

ULONG_PTR
NTAPI
KeIpiGenericCall(
    _In_ PKIPI_BROADCAST_WORKER Function,
    _In_ ULONG_PTR Argument)
{
    ULONG_PTR Status;
    KIRQL OldIrql, SynchIrql, IpiIrql;
    KAFFINITY TargetSet, RemainingSet;
    volatile LONG Count;

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    /* Raise to DISPATCH_LEVEL */
    OldIrql = KeGetCurrentIrql();
    if (OldIrql < DISPATCH_LEVEL) KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    /* Only one generic call at a time */
    KeAcquireSpinLockAtDpcLevel(&KiReverseStallIpiLock);

    KeRaiseIrql(SYNCH_LEVEL, &SynchIrql);

    TargetSet = KeActiveProcessors & ~KeGetCurrentPrcb()->SetMember;

    /* Barrier: targets + ourselves */
    Count = 1;
    for (RemainingSet = TargetSet; RemainingSet != 0; RemainingSet &= RemainingSet - 1)
    {
        Count++;
    }

    if (TargetSet != 0)
    {
        KiIpiSendPacket(TargetSet,
                        KiIpiGenericCallTarget,
                        Function,
                        Argument,
                        (PULONG)&Count);

        /* Wait for all targets to arrive */
        while (Count != 1)
        {
            YieldProcessor();
        }
    }

    /* Raise to IPI_LEVEL and release the targets */
    KeRaiseIrql(IPI_LEVEL, &IpiIrql);
    InterlockedExchange((PLONG)&Count, 0);

    Status = Function(Argument);

    /* Wait for the targets to finish */
    KeLowerIrql(IpiIrql);
    if (TargetSet != 0)
    {
        KiIpiWaitForPacketTargets();
    }

    KeLowerIrql(SynchIrql);
    KeReleaseSpinLockFromDpcLevel(&KiReverseStallIpiLock);
    KeLowerIrql(OldIrql);

    return Status;
}
