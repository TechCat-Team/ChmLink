#include "chmlink.h"
#include "chmlink/session.h"

/* Exported by ntoskrnl; not declared in the headers this driver includes. */
NTSYSAPI NTSTATUS NTAPI ZwWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout);

/* A session owns shared-memory rings (chmlink/ring.h): one the driver
 * produces into from ClSend, and one to CHMLINK_INJECT_RINGS_MAX the
 * application produces into. Each of those has a kernel thread that turns its
 * records into receive indications, so producers on several application
 * threads are indicated on several cores, as the stack processes an
 * indication on the indicating thread. Order holds within one ring.
 *
 * NBLs point straight into the locked application pages. The ring's head may
 * only move past packets the stack has returned, so indications are recorded
 * as batches (end position plus the number of NBLs still outstanding); the
 * head advances over the completed prefix only. */

typedef struct CL_FILTER {
    ULONG PacketFilter;
    ULONG MulticastCount;
    UCHAR Multicast[CL_MULTICAST_MAX][6];
    UCHAR Mac[6];
} CL_FILTER;

static void snapshot_filter(CL_ADAPTER *a, CL_FILTER *f)
{
    KIRQL irql;
    KeAcquireSpinLock(&a->Lock, &irql);
    f->PacketFilter = a->PacketFilter;
    f->MulticastCount = min(a->MulticastCount, (ULONG)CL_MULTICAST_MAX);
    NdisMoveMemory(f->Multicast, a->Multicast, (SIZE_T)f->MulticastCount * sizeof(f->Multicast[0]));
    NdisMoveMemory(f->Mac, a->Mac, sizeof(f->Mac));
    KeReleaseSpinLock(&a->Lock, irql);
}

static BOOLEAN accepts(const CL_FILTER *f, const UCHAR *frame)
{
    static const UCHAR broadcast[6] = {255, 255, 255, 255, 255, 255};
    ULONG i;
    if (f->PacketFilter & NDIS_PACKET_TYPE_PROMISCUOUS) return TRUE;
    if (RtlCompareMemory(frame, broadcast, 6) == 6)
        return (f->PacketFilter & NDIS_PACKET_TYPE_BROADCAST) != 0;
    if (frame[0] & 1) {
        if (f->PacketFilter & NDIS_PACKET_TYPE_ALL_MULTICAST) return TRUE;
        if (!(f->PacketFilter & NDIS_PACKET_TYPE_MULTICAST)) return FALSE;
        for (i = 0; i < f->MulticastCount; ++i)
            if (RtlCompareMemory(frame, f->Multicast[i], 6) == 6) return TRUE;
        return FALSE;
    }
    return (f->PacketFilter & NDIS_PACKET_TYPE_DIRECTED) &&
           RtlCompareMemory(frame, f->Mac, 6) == 6;
}

/* Pop every fully returned batch from the head and publish the last end. */
static void pop_batches(CL_RXQ *q)
{
    KIRQL irql;
    BOOLEAN advanced = FALSE;
    uint32_t head = 0;
    KeAcquireSpinLock(&q->BatchLock, &irql);
    while (q->BatchCount != 0 && q->Batches[q->BatchHead].Pending == 0) {
        head = q->Batches[q->BatchHead].End;
        advanced = TRUE;
        q->BatchHead = (q->BatchHead + 1) % CL_BATCH_QUEUE_SIZE;
        q->BatchCount--;
    }
    KeReleaseSpinLock(&q->BatchLock, irql);
    if (advanced) chmlink_ring_release(&q->Ring.View, head);
}

/* `count` NBLs of batch `id` came back: account for them, free ring space
 * and wake the queue's thread, which may be waiting for room. */
static void returned(CL_RXQ *q, uint32_t id, LONG count)
{
    ULONG i;
    BOOLEAN found = FALSE;
    KIRQL irql;
    KeAcquireSpinLock(&q->BatchLock, &irql);
    for (i = 0; i < q->BatchCount; ++i) {
        CL_BATCH *batch = &q->Batches[(q->BatchHead + i) % CL_BATCH_QUEUE_SIZE];
        if (batch->Id == id) { batch->Pending -= count; found = TRUE; break; }
    }
    KeReleaseSpinLock(&q->BatchLock, irql);
    InterlockedExchangeAdd(&q->Outstanding, -count);
    if (found) pop_batches(q);
    KeSetEvent(&q->StateEvent, IO_NO_INCREMENT, FALSE);
}

void ClSessionReturnLists(CL_ADAPTER *a, PNET_BUFFER_LIST lists)
{
    PNET_BUFFER_LIST nbl, next;
    CL_RXQ *owner = NULL;
    uint32_t id = 0;
    LONG count = 0, total = 0;

    for (nbl = lists; nbl != NULL; nbl = next) {
        CL_RXQ *nblOwner = (CL_RXQ *)nbl->MiniportReserved[1];
        uint32_t nblId = (uint32_t)(ULONG_PTR)nbl->MiniportReserved[0];
        next = NET_BUFFER_LIST_NEXT_NBL(nbl);
        NdisFreeNetBufferList(nbl);
        total++;
        if (nblOwner != owner || nblId != id) {
            if (owner != NULL) returned(owner, id, count);
            owner = nblOwner;
            id = nblId;
            count = 1;
        } else count++;
    }
    if (owner != NULL) returned(owner, id, count);
    if (total != 0 && InterlockedExchangeAdd(&a->RxOutstanding, -total) == total)
        KeSetEvent(&a->RxDrained, IO_NO_INCREMENT, FALSE);
}

static void wait_idle(CL_RXQ *q, BOOLEAN data)
{
    CL_SESSION *s = q->Session;
    PVOID objects[3];
    if (!data) {
        objects[0] = &s->StopEvent;
        objects[1] = &q->StateEvent;
        KeWaitForMultipleObjects(2, objects, WaitAny, Executive, KernelMode, FALSE, NULL, NULL);
        return;
    }
    chmlink_ring_store(&q->Ring.View, CHMLINK_RING_WAIT_OFFSET, 1);
    if (chmlink_ring_load(&q->Ring.View, CHMLINK_RING_TAIL_OFFSET) != q->ReadCursor) {
        chmlink_ring_store(&q->Ring.View, CHMLINK_RING_WAIT_OFFSET, 0);
        return;
    }
    objects[0] = q->Ring.Event;
    objects[1] = &s->StopEvent;
    objects[2] = &q->StateEvent;
    KeWaitForMultipleObjects(3, objects, WaitAny, Executive, KernelMode, FALSE, NULL, NULL);
    chmlink_ring_store(&q->Ring.View, CHMLINK_RING_WAIT_OFFSET, 0);
}

static KSTART_ROUTINE ClSessionThread;

_Use_decl_annotations_
static VOID ClSessionThread(PVOID context)
{
    CL_RXQ *q = context;
    CL_SESSION *s = q->Session;
    CL_ADAPTER *a = s->Adapter;

    for (;;) {
        PNET_BUFFER_LIST first = NULL, last = NULL;
        CL_FILTER filter;
        KIRQL irql;
        ULONG fine = 0, count = 0, bytes = 0, batchId, room;
        LONG64 discards = 0;   /* added once: several queue threads count */
        uint32_t tail, cursor;
        BOOLEAN running, acquired;

        if (s->Stop || q->Stop) break;

        acquired = ExAcquireRundownProtection(&a->IoRundown);
        running = FALSE;
        if (acquired) {
            KeAcquireSpinLock(&a->Lock, &irql);
            running = a->State == ClAdapterRunning && a->PowerState == NdisDeviceStateD0 &&
                      !a->ShuttingDown && !a->SurpriseRemoved;
            fine = ClOffloadEffectiveLocked(a);
            KeReleaseSpinLock(&a->Lock, irql);
            if (running && a->Mode != CHMLINK_MODE_IP) snapshot_filter(a, &filter);
        }
        if (!acquired || !running || s->Broken) {
            if (acquired) ExReleaseRundownProtection(&a->IoRundown);
            wait_idle(q, FALSE);
            continue;
        }

        KeAcquireSpinLock(&q->BatchLock, &irql);
        room = q->BatchCount < CL_BATCH_QUEUE_SIZE;
        KeReleaseSpinLock(&q->BatchLock, irql);
        if (!room || InterlockedCompareExchange(&q->Outstanding, 0, 0) >= CL_SESSION_OUTSTANDING_MAX) {
            /* A return sets StateEvent. */
            ExReleaseRundownProtection(&a->IoRundown);
            wait_idle(q, FALSE);
            continue;
        }

        tail = chmlink_ring_load(&q->Ring.View, CHMLINK_RING_TAIL_OFFSET);
        cursor = q->ReadCursor;
        while (count < CL_SESSION_BATCH_MAX) {
            uint32_t offset, size;
            chmlink_offload descriptor;
            UCHAR *packet;
            PNET_BUFFER_LIST nbl;
            chmlink_ring_status status;
            status = chmlink_ring_next(&q->Ring.View, &cursor, tail, ClPacketLimit(a),
                                       &offset, &size, &descriptor);
            if (status == CHMLINK_RING_EMPTY) break;
            if (status == CHMLINK_RING_CORRUPT) { s->Broken = TRUE; break; }
            packet = q->Ring.View.data + offset;
            if (a->Mode == CHMLINK_MODE_IP) {
                if (!chmlink_ip_packet_valid(packet, size,
                        descriptor.gso_type != CHMLINK_GSO_NONE ? CHMLINK_PACKET_MAX_SIZE : ClPacketLimit(a))) {
                    discards++;
                    continue;
                }
            } else if (!accepts(&filter, packet)) {
                discards++;
                continue;
            }
            nbl = NdisAllocateNetBufferAndNetBufferList(a->Pool, 0, 0, q->Ring.Mdl,
                    CHMLINK_RING_HEADER_SIZE + offset, size);
            if (nbl == NULL) {
                discards++;
                break;   /* the record is dropped, the rest stays for next time */
            }
            nbl->SourceHandle = a->Handle;
            if (a->Mode == CHMLINK_MODE_IP) {
                BOOLEAN ipv4 = (packet[0] >> 4) == 4;
                NdisClearNblFlag(nbl, NDIS_NBL_FLAGS_IS_IPV4 | NDIS_NBL_FLAGS_IS_IPV6);
                NdisSetNblFlag(nbl, ipv4 ? NDIS_NBL_FLAGS_IS_IPV4 : NDIS_NBL_FLAGS_IS_IPV6);
                NET_BUFFER_LIST_INFO(nbl, NetBufferListFrameType) =
                    (PVOID)(ULONG_PTR)(ipv4 ? 0x0008u : 0xdd86u);
            }
            NET_BUFFER_LIST_NEXT_NBL(nbl) = NULL;
            if (!ClPrepareInjected(a, nbl, packet, size, &descriptor, fine)) {
                NdisFreeNetBufferList(nbl);
                discards++;
                continue;
            }
            if (last != NULL) NET_BUFFER_LIST_NEXT_NBL(last) = nbl;
            else first = nbl;
            last = nbl;
            count++;
            bytes += size;
        }

        if (discards != 0) InterlockedAdd64((LONG64 volatile *)&a->RxDiscards, discards);
        if (cursor != q->ReadCursor) {
            batchId = ++q->NextBatchId;
            if (batchId == 0) batchId = ++q->NextBatchId;
            if (count != 0) {
                PNET_BUFFER_LIST nbl;
                for (nbl = first; nbl != NULL; nbl = NET_BUFFER_LIST_NEXT_NBL(nbl)) {
                    nbl->MiniportReserved[0] = (PVOID)(ULONG_PTR)batchId;
                    nbl->MiniportReserved[1] = q;
                }
            }
            KeAcquireSpinLock(&q->BatchLock, &irql);
            {
                CL_BATCH *batch = &q->Batches[(q->BatchHead + q->BatchCount) % CL_BATCH_QUEUE_SIZE];
                batch->End = cursor;
                batch->Id = batchId;
                batch->Pending = (LONG)count;
                q->BatchCount++;
            }
            KeReleaseSpinLock(&q->BatchLock, irql);
            q->ReadCursor = cursor;
            if (count != 0) {
                InterlockedExchangeAdd(&q->Outstanding, (LONG)count);
                if (InterlockedExchangeAdd(&a->RxOutstanding, (LONG)count) == 0)
                    KeClearEvent(&a->RxDrained);
                KeAcquireSpinLock(&a->Lock, &irql);
                a->RxPackets += count;
                a->RxBytes += bytes;
                KeReleaseSpinLock(&a->Lock, irql);
                NdisMIndicateReceiveNetBufferLists(a->Handle, first, NDIS_DEFAULT_PORT_NUMBER,
                                                   count, 0);
            } else {
                pop_batches(q);
            }
        }
        ExReleaseRundownProtection(&a->IoRundown);

        if (s->Broken) {
            KeAcquireSpinLock(&a->Lock, &irql);
            ClSessionStateLocked(a);
            KeReleaseSpinLock(&a->Lock, irql);
        }
        if (cursor == q->ReadCursor && count == 0) wait_idle(q, TRUE);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS map_ring(CL_RING *ring, uint64_t address, uint32_t capacity, uint64_t event)
{
    NTSTATUS status = STATUS_SUCCESS;
    if (address == 0 || (address % 64) != 0 || !chmlink_ring_capacity_valid(capacity)) return STATUS_INVALID_PARAMETER;
    /* capacity <= CHMLINK_RING_MAX_CAPACITY (checked above), so the sum cannot overflow (C26831). */
#pragma warning(suppress: 26831)
    ring->Mdl = IoAllocateMdl((PVOID)(ULONG_PTR)address,
                              CHMLINK_RING_HEADER_SIZE + capacity, FALSE, FALSE, NULL);
    if (ring->Mdl == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    __try {
        MmProbeAndLockPages(ring->Mdl, UserMode, IoWriteAccess);
    }
    /* The documented pattern for probing user pages: any access fault fails the request (C6320). */
#pragma warning(suppress: 6320)
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }
    if (!NT_SUCCESS(status)) {
        IoFreeMdl(ring->Mdl);
        ring->Mdl = NULL;
        return status;
    }
    ring->Address = MmGetSystemAddressForMdlSafe(ring->Mdl, NormalPagePriority | MdlMappingNoExecute);
    if (ring->Address == NULL || !chmlink_ring_attach(&ring->View, ring->Address, capacity)) {
        MmUnlockPages(ring->Mdl);
        IoFreeMdl(ring->Mdl);
        ring->Mdl = NULL;
        ring->Address = NULL;
        return STATUS_INVALID_ADDRESS;
    }
    ring->Capacity = capacity;
    status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)event, EVENT_MODIFY_STATE,
                                       *ExEventObjectType, UserMode, (PVOID *)&ring->Event, NULL);
    if (!NT_SUCCESS(status)) {
        ring->Event = NULL;
        MmUnlockPages(ring->Mdl);
        IoFreeMdl(ring->Mdl);
        ring->Mdl = NULL;
        ring->Address = NULL;
        return status;
    }
    return STATUS_SUCCESS;
}

static void unmap_ring(CL_RING *ring)
{
    if (ring->Event != NULL) ObDereferenceObject(ring->Event);
    if (ring->Mdl != NULL) {
        MmUnlockPages(ring->Mdl);
        IoFreeMdl(ring->Mdl);
    }
    ring->Event = NULL;
    ring->Mdl = NULL;
    ring->Address = NULL;
}

static void init_queue(CL_SESSION *s, CL_RXQ *q)
{
    NdisZeroMemory(q, sizeof(*q));
    q->Session = s;
    KeInitializeSpinLock(&q->BatchLock);
    /* Auto-reset: a state change or return that happens before the thread
     * waits is consumed by its next wait instead of being lost or piling up. */
    KeInitializeEvent(&q->StateEvent, SynchronizationEvent, FALSE);
}

/* Start the queue's thread. On failure no thread runs any more; NBLs it may
 * have indicated can still be outstanding (q->Outstanding). */
static NTSTATUS start_queue(CL_RXQ *q)
{
    HANDLE handle = NULL;
    NTSTATUS status = PsCreateSystemThread(&handle, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                           ClSessionThread, q);
    if (NT_SUCCESS(status)) {
        status = ObReferenceObjectByHandle(handle, THREAD_ALL_ACCESS, *PsThreadType,
                                           KernelMode, (PVOID *)&q->Thread, NULL);
        if (!NT_SUCCESS(status)) {
            q->Thread = NULL;
            q->Stop = TRUE;
            KeSetEvent(&q->StateEvent, IO_NO_INCREMENT, FALSE);
            ZwWaitForSingleObject(handle, FALSE, NULL);
        }
        ZwClose(handle);
    }
    return status;
}

NTSTATUS ClSessionRegister(CL_ADAPTER *a, PIRP irp, ULONG length)
{
    chmlink_session_rings rings;
    CL_SESSION *s;
    NTSTATUS status;
    KIRQL irql;
    ULONG i;

    if (irp->RequestorMode != UserMode || length != CHMLINK_SESSION_SIZE)
        return STATUS_INVALID_PARAMETER;
    if (chmlink_session_decode(&rings, irp->AssociatedIrp.SystemBuffer, length) != CHMLINK_OK)
        return STATUS_INVALID_PARAMETER;
    /* Registrations are serialised by ControlMutex, so this answers a second
     * one before any memory is locked or handle referenced. */
    KeAcquireSpinLock(&a->Lock, &irql);
    status = a->Session != NULL ? STATUS_INVALID_DEVICE_STATE : STATUS_SUCCESS;
    KeReleaseSpinLock(&a->Lock, irql);
    if (!NT_SUCCESS(status)) return status;

    s = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*s), CL_TAG);
    if (s == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    NdisZeroMemory(s, sizeof(*s));
    s->Adapter = a;
    InterlockedIncrement(&a->References);
    KeInitializeSpinLock(&s->SendLock);
    ExInitializeRundownProtection(&s->SendRundown);
    KeInitializeEvent(&s->StopEvent, NotificationEvent, FALSE);
    for (i = 0; i < CHMLINK_INJECT_RINGS_MAX; ++i) init_queue(s, &s->Rx[i]);

    status = map_ring(&s->From, rings.from_windows_ring, rings.from_windows_capacity,
                      rings.from_windows_event);
    if (!NT_SUCCESS(status)) goto fail;
    status = map_ring(&s->Rx[0].Ring, rings.to_windows_ring, rings.to_windows_capacity,
                      rings.to_windows_event);
    if (!NT_SUCCESS(status)) goto fail;

    KeAcquireSpinLock(&a->Lock, &irql);
    if (a->Session != NULL) {
        KeReleaseSpinLock(&a->Lock, irql);
        status = STATUS_INVALID_DEVICE_STATE;
        goto fail;
    }
    a->Session = s;
    s->RxCount = 1;
    chmlink_ring_reset(&s->From.View);
    chmlink_ring_reset(&s->Rx[0].Ring.View);
    ClSessionStateLocked(a);
    KeReleaseSpinLock(&a->Lock, irql);

    status = start_queue(&s->Rx[0]);
    if (!NT_SUCCESS(status)) {
        KeAcquireSpinLock(&a->Lock, &irql);
        if (a->Session == s) a->Session = NULL;
        KeReleaseSpinLock(&a->Lock, irql);
        goto fail;
    }
    return STATUS_SUCCESS;

fail:
    if (InterlockedCompareExchange(&a->RxOutstanding, 0, 0) != 0)
        KeWaitForSingleObject(&a->RxDrained, Executive, KernelMode, FALSE, NULL);
    for (i = 0; i < CHMLINK_INJECT_RINGS_MAX; ++i) unmap_ring(&s->Rx[i].Ring);
    unmap_ring(&s->From);
    ExFreePoolWithTag(s, CL_TAG);
    ClRelease(a);
    return status;
}

/* ControlMutex held (as for REGISTER_RINGS and cleanup), so the session
 * stays and RxCount only changes here. */
NTSTATUS ClSessionAddRing(CL_ADAPTER *a, PIRP irp, ULONG length)
{
    chmlink_inject_ring request;
    CL_SESSION *s;
    CL_RXQ *q;
    NTSTATUS status;
    KIRQL irql;
    ULONG index = 0;

    if (irp->RequestorMode != UserMode || length != CHMLINK_INJECT_RING_SIZE)
        return STATUS_INVALID_PARAMETER;
    if (chmlink_inject_ring_decode(&request, irp->AssociatedIrp.SystemBuffer, length) != CHMLINK_OK)
        return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&a->Lock, &irql);
    s = a->Session;
    if (s == NULL || s->Broken || s->RxCount >= CHMLINK_INJECT_RINGS_MAX) s = NULL;
    else index = s->RxCount;
    KeReleaseSpinLock(&a->Lock, irql);
    if (s == NULL) return STATUS_INVALID_DEVICE_STATE;

    q = &s->Rx[index];
    init_queue(s, q);
    status = map_ring(&q->Ring, request.ring, request.capacity, request.event);
    if (!NT_SUCCESS(status)) return status;

    KeAcquireSpinLock(&a->Lock, &irql);
    chmlink_ring_reset(&q->Ring.View);
    s->RxCount = index + 1;
    ClSessionStateLocked(a);
    KeReleaseSpinLock(&a->Lock, irql);

    status = start_queue(q);
    if (!NT_SUCCESS(status)) {
        KeAcquireSpinLock(&a->Lock, &irql);
        s->RxCount = index;
        KeReleaseSpinLock(&a->Lock, irql);
        /* Returns set StateEvent; wait for this queue's NBLs only. */
        while (InterlockedCompareExchange(&q->Outstanding, 0, 0) != 0)
            KeWaitForSingleObject(&q->StateEvent, Executive, KernelMode, FALSE, NULL);
        unmap_ring(&q->Ring);
    }
    return status;
}

void ClSessionTeardown(CL_ADAPTER *a, CL_SESSION *s)
{
    KIRQL irql;
    ULONG i, count;

    KeAcquireSpinLock(&a->Lock, &irql);
    if (a->Session == s) a->Session = NULL;
    count = s->RxCount;
    chmlink_ring_store(&s->From.View, CHMLINK_RING_STATE_OFFSET, CHMLINK_RING_STATE_CLOSED);
    KeSetEvent(s->From.Event, IO_NO_INCREMENT, FALSE);
    for (i = 0; i < count; ++i) {
        chmlink_ring_store(&s->Rx[i].Ring.View, CHMLINK_RING_STATE_OFFSET, CHMLINK_RING_STATE_CLOSED);
        KeSetEvent(s->Rx[i].Ring.Event, IO_NO_INCREMENT, FALSE);
        KeSetEvent(&s->Rx[i].StateEvent, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&a->Lock, irql);

    /* No sender may still be copying into the application's ring. */
    ExWaitForRundownProtectionRelease(&s->SendRundown);
    s->Stop = TRUE;
    KeSetEvent(&s->StopEvent, IO_NO_INCREMENT, FALSE);
    for (i = 0; i < count; ++i) {
        KeSetEvent(&s->Rx[i].StateEvent, IO_NO_INCREMENT, FALSE);
        if (s->Rx[i].Thread != NULL)
            KeWaitForSingleObject(s->Rx[i].Thread, Executive, KernelMode, FALSE, NULL);
    }
    /* Every indicated NBL must be back before the pages are unlocked. */
    if (InterlockedCompareExchange(&a->RxOutstanding, 0, 0) != 0)
        KeWaitForSingleObject(&a->RxDrained, Executive, KernelMode, FALSE, NULL);
    for (i = 0; i < CHMLINK_INJECT_RINGS_MAX; ++i) {
        if (s->Rx[i].Thread != NULL) ObDereferenceObject(s->Rx[i].Thread);
        unmap_ring(&s->Rx[i].Ring);
    }
    unmap_ring(&s->From);
    ExFreePoolWithTag(s, CL_TAG);
    ClRelease(a);
}

void ClSessionStateLocked(CL_ADAPTER *a)
{
    CL_SESSION *s = a->Session;
    ULONG state = 0, i;
    if (s == NULL) return;
    if (a->State == ClAdapterRunning && a->PowerState == NdisDeviceStateD0 &&
        !a->ShuttingDown && !a->SurpriseRemoved && a->SessionState == ClSessionOpen)
        state |= CHMLINK_RING_STATE_RUNNING;
    if (a->SessionState == ClSessionClosing || a->State == ClAdapterRemoving ||
        a->SurpriseRemoved || a->ShuttingDown || s->Broken)
        state |= CHMLINK_RING_STATE_CLOSED;
    chmlink_ring_store(&s->From.View, CHMLINK_RING_STATE_OFFSET, state);
    KeSetEvent(s->From.Event, IO_NO_INCREMENT, FALSE);
    for (i = 0; i < s->RxCount; ++i) {
        chmlink_ring_store(&s->Rx[i].Ring.View, CHMLINK_RING_STATE_OFFSET, state);
        KeSetEvent(&s->Rx[i].StateEvent, IO_NO_INCREMENT, FALSE);
    }
}
