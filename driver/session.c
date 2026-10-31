#include "chmlink.h"
#include "chmlink/session.h"

/* Exported by ntoskrnl; not declared in the headers this driver includes. */
NTSYSAPI NTSTATUS NTAPI ZwWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout);

/* A session owns two shared-memory rings (chmlink/ring.h) and the kernel
 * thread that turns records in the application's ring into receive
 * indications. The driver produces into the other ring from ClSend.
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
static void pop_batches(CL_SESSION *s)
{
    KIRQL irql;
    BOOLEAN advanced = FALSE;
    uint32_t head = 0;
    KeAcquireSpinLock(&s->BatchLock, &irql);
    while (s->BatchCount != 0 && s->Batches[s->BatchHead].Pending == 0) {
        head = s->Batches[s->BatchHead].End;
        advanced = TRUE;
        s->BatchHead = (s->BatchHead + 1) % CL_BATCH_QUEUE_SIZE;
        s->BatchCount--;
        KeSetEvent(&s->ReturnEvent, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&s->BatchLock, irql);
    if (advanced) chmlink_ring_release(&s->To.View, head);
}

void ClSessionReturnLists(CL_ADAPTER *a, PNET_BUFFER_LIST lists)
{
    PNET_BUFFER_LIST nbl, next;
    CL_SESSION *owner = NULL;
    uint32_t id = 0;
    LONG count = 0, total = 0;
    KIRQL irql;

    for (nbl = lists; nbl != NULL; nbl = next) {
        CL_SESSION *nblOwner = (CL_SESSION *)nbl->MiniportReserved[1];
        uint32_t nblId = (uint32_t)(ULONG_PTR)nbl->MiniportReserved[0];
        next = NET_BUFFER_LIST_NEXT_NBL(nbl);
        NdisFreeNetBufferList(nbl);
        total++;
        if (nblOwner != owner || nblId != id) {
            if (owner != NULL) {
                ULONG i;
                BOOLEAN found = FALSE;
                KeAcquireSpinLock(&owner->BatchLock, &irql);
                for (i = 0; i < owner->BatchCount; ++i) {
                    CL_BATCH *batch = &owner->Batches[(owner->BatchHead + i) % CL_BATCH_QUEUE_SIZE];
                    if (batch->Id == id) { batch->Pending -= count; found = TRUE; break; }
                }
                KeReleaseSpinLock(&owner->BatchLock, irql);
                if (found) pop_batches(owner);
            }
            owner = nblOwner;
            id = nblId;
            count = 1;
        } else count++;
    }
    if (owner != NULL) {
        ULONG i;
        BOOLEAN found = FALSE;
        KeAcquireSpinLock(&owner->BatchLock, &irql);
        for (i = 0; i < owner->BatchCount; ++i) {
            CL_BATCH *batch = &owner->Batches[(owner->BatchHead + i) % CL_BATCH_QUEUE_SIZE];
            if (batch->Id == id) { batch->Pending -= count; found = TRUE; break; }
        }
        KeReleaseSpinLock(&owner->BatchLock, irql);
        if (found) pop_batches(owner);
    }
    if (total != 0 && InterlockedExchangeAdd(&a->RxOutstanding, -total) == total)
        KeSetEvent(&a->RxDrained, IO_NO_INCREMENT, FALSE);
}

static void wait_idle(CL_SESSION *s, BOOLEAN data)
{
    PVOID objects[3];
    if (!data) {
        objects[0] = &s->StopEvent;
        objects[1] = &s->StateEvent;
        KeWaitForMultipleObjects(2, objects, WaitAny, Executive, KernelMode, FALSE, NULL, NULL);
        return;
    }
    chmlink_ring_store(&s->To.View, CHMLINK_RING_WAIT_OFFSET, 1);
    if (chmlink_ring_load(&s->To.View, CHMLINK_RING_TAIL_OFFSET) != s->ReadCursor) {
        chmlink_ring_store(&s->To.View, CHMLINK_RING_WAIT_OFFSET, 0);
        return;
    }
    objects[0] = s->To.Event;
    objects[1] = &s->StopEvent;
    objects[2] = &s->StateEvent;
    KeWaitForMultipleObjects(3, objects, WaitAny, Executive, KernelMode, FALSE, NULL, NULL);
    chmlink_ring_store(&s->To.View, CHMLINK_RING_WAIT_OFFSET, 0);
}

static KSTART_ROUTINE ClSessionThread;

_Use_decl_annotations_
static VOID ClSessionThread(PVOID context)
{
    CL_SESSION *s = context;
    CL_ADAPTER *a = s->Adapter;

    for (;;) {
        PNET_BUFFER_LIST first = NULL, last = NULL;
        CL_FILTER filter;
        KIRQL irql;
        ULONG fine = 0, count = 0, bytes = 0, batchId, room;
        uint32_t tail, cursor;
        BOOLEAN running, acquired;

        if (s->Stop) break;

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
            wait_idle(s, FALSE);
            continue;
        }

        KeAcquireSpinLock(&s->BatchLock, &irql);
        room = s->BatchCount < CL_BATCH_QUEUE_SIZE;
        KeReleaseSpinLock(&s->BatchLock, irql);
        if (!room || InterlockedCompareExchange(&a->RxOutstanding, 0, 0) >= CL_SESSION_OUTSTANDING_MAX) {
            ExReleaseRundownProtection(&a->IoRundown);
            wait_idle(s, FALSE);
            continue;
        }

        tail = chmlink_ring_load(&s->To.View, CHMLINK_RING_TAIL_OFFSET);
        cursor = s->ReadCursor;
        while (count < CL_SESSION_BATCH_MAX) {
            uint32_t offset, size;
            chmlink_offload descriptor;
            UCHAR *packet;
            PNET_BUFFER_LIST nbl;
            chmlink_ring_status status;
            status = chmlink_ring_next(&s->To.View, &cursor, tail, ClPacketLimit(a),
                                       &offset, &size, &descriptor);
            if (status == CHMLINK_RING_EMPTY) break;
            if (status == CHMLINK_RING_CORRUPT) { s->Broken = TRUE; break; }
            packet = s->To.View.data + offset;
            if (a->Mode == CHMLINK_MODE_IP) {
                if (!chmlink_ip_packet_valid(packet, size,
                        descriptor.gso_type != CHMLINK_GSO_NONE ? CHMLINK_PACKET_MAX_SIZE : ClPacketLimit(a))) {
                    a->RxDiscards++;
                    continue;
                }
            } else if (!accepts(&filter, packet)) {
                a->RxDiscards++;
                continue;
            }
            nbl = NdisAllocateNetBufferAndNetBufferList(a->Pool, 0, 0, s->To.Mdl,
                    CHMLINK_RING_HEADER_SIZE + offset, size);
            if (nbl == NULL) {
                a->RxDiscards++;
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
                a->RxDiscards++;
                continue;
            }
            if (last != NULL) NET_BUFFER_LIST_NEXT_NBL(last) = nbl;
            else first = nbl;
            last = nbl;
            count++;
            bytes += size;
        }

        if (cursor != s->ReadCursor) {
            batchId = ++s->NextBatchId;
            if (batchId == 0) batchId = ++s->NextBatchId;
            if (count != 0) {
                PNET_BUFFER_LIST nbl;
                for (nbl = first; nbl != NULL; nbl = NET_BUFFER_LIST_NEXT_NBL(nbl)) {
                    nbl->MiniportReserved[0] = (PVOID)(ULONG_PTR)batchId;
                    nbl->MiniportReserved[1] = s;
                }
            }
            KeAcquireSpinLock(&s->BatchLock, &irql);
            {
                CL_BATCH *batch = &s->Batches[(s->BatchHead + s->BatchCount) % CL_BATCH_QUEUE_SIZE];
                batch->End = cursor;
                batch->Id = batchId;
                batch->Pending = (LONG)count;
                s->BatchCount++;
            }
            KeReleaseSpinLock(&s->BatchLock, irql);
            s->ReadCursor = cursor;
            if (count != 0) {
                if (InterlockedExchangeAdd(&a->RxOutstanding, (LONG)count) == 0)
                    KeClearEvent(&a->RxDrained);
                KeAcquireSpinLock(&a->Lock, &irql);
                a->RxPackets += count;
                a->RxBytes += bytes;
                KeReleaseSpinLock(&a->Lock, irql);
                NdisMIndicateReceiveNetBufferLists(a->Handle, first, NDIS_DEFAULT_PORT_NUMBER,
                                                   count, 0);
            } else {
                pop_batches(s);
            }
        }
        ExReleaseRundownProtection(&a->IoRundown);

        if (s->Broken) {
            KeAcquireSpinLock(&a->Lock, &irql);
            ClSessionStateLocked(a);
            KeReleaseSpinLock(&a->Lock, irql);
        }
        if (cursor == s->ReadCursor && count == 0) wait_idle(s, TRUE);
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

NTSTATUS ClSessionRegister(CL_ADAPTER *a, PIRP irp, ULONG length)
{
    chmlink_session_rings rings;
    CL_SESSION *s;
    NTSTATUS status;
    KIRQL irql;
    HANDLE handle = NULL;

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
    KeInitializeSpinLock(&s->BatchLock);
    ExInitializeRundownProtection(&s->SendRundown);
    KeInitializeEvent(&s->StopEvent, NotificationEvent, FALSE);
    /* Auto-reset: a state change that happens before the thread waits is
     * consumed by its next wait instead of being lost or piling up. */
    KeInitializeEvent(&s->StateEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&s->ReturnEvent, SynchronizationEvent, FALSE);

    status = map_ring(&s->From, rings.from_windows_ring, rings.from_windows_capacity,
                      rings.from_windows_event);
    if (!NT_SUCCESS(status)) goto fail;
    status = map_ring(&s->To, rings.to_windows_ring, rings.to_windows_capacity,
                      rings.to_windows_event);
    if (!NT_SUCCESS(status)) goto fail;

    KeAcquireSpinLock(&a->Lock, &irql);
    if (a->Session != NULL) {
        KeReleaseSpinLock(&a->Lock, irql);
        status = STATUS_INVALID_DEVICE_STATE;
        goto fail;
    }
    a->Session = s;
    chmlink_ring_reset(&s->From.View);
    chmlink_ring_reset(&s->To.View);
    ClSessionStateLocked(a);
    KeReleaseSpinLock(&a->Lock, irql);

    status = PsCreateSystemThread(&handle, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                  ClSessionThread, s);
    if (NT_SUCCESS(status)) {
        status = ObReferenceObjectByHandle(handle, THREAD_ALL_ACCESS, *PsThreadType,
                                           KernelMode, (PVOID *)&s->Thread, NULL);
    }
    if (!NT_SUCCESS(status)) {
        if (handle != NULL) {
            s->Stop = TRUE;
            KeSetEvent(&s->StopEvent, IO_NO_INCREMENT, FALSE);
            ZwWaitForSingleObject(handle, FALSE, NULL);
        }
        KeAcquireSpinLock(&a->Lock, &irql);
        if (a->Session == s) a->Session = NULL;
        KeReleaseSpinLock(&a->Lock, irql);
    }
    if (handle != NULL) ZwClose(handle);
    if (!NT_SUCCESS(status)) goto fail;
    return STATUS_SUCCESS;

fail:
    if (InterlockedCompareExchange(&a->RxOutstanding, 0, 0) != 0)
        KeWaitForSingleObject(&a->RxDrained, Executive, KernelMode, FALSE, NULL);
    unmap_ring(&s->To);
    unmap_ring(&s->From);
    ExFreePoolWithTag(s, CL_TAG);
    ClRelease(a);
    return status;
}

void ClSessionTeardown(CL_ADAPTER *a, CL_SESSION *s)
{
    KIRQL irql;

    KeAcquireSpinLock(&a->Lock, &irql);
    if (a->Session == s) a->Session = NULL;
    chmlink_ring_store(&s->From.View, CHMLINK_RING_STATE_OFFSET, CHMLINK_RING_STATE_CLOSED);
    chmlink_ring_store(&s->To.View, CHMLINK_RING_STATE_OFFSET, CHMLINK_RING_STATE_CLOSED);
    KeSetEvent(s->From.Event, IO_NO_INCREMENT, FALSE);
    KeSetEvent(s->To.Event, IO_NO_INCREMENT, FALSE);
    KeSetEvent(&s->StateEvent, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&a->Lock, irql);

    /* No sender may still be copying into the application's ring. */
    ExWaitForRundownProtectionRelease(&s->SendRundown);
    s->Stop = TRUE;
    KeSetEvent(&s->StopEvent, IO_NO_INCREMENT, FALSE);
    KeSetEvent(&s->StateEvent, IO_NO_INCREMENT, FALSE);
    if (s->Thread != NULL)
        KeWaitForSingleObject(s->Thread, Executive, KernelMode, FALSE, NULL);
    /* Every indicated NBL must be back before the pages are unlocked. */
    if (InterlockedCompareExchange(&a->RxOutstanding, 0, 0) != 0)
        KeWaitForSingleObject(&a->RxDrained, Executive, KernelMode, FALSE, NULL);
    if (s->Thread != NULL) ObDereferenceObject(s->Thread);
    unmap_ring(&s->To);
    unmap_ring(&s->From);
    ExFreePoolWithTag(s, CL_TAG);
    ClRelease(a);
}

void ClSessionStateLocked(CL_ADAPTER *a)
{
    CL_SESSION *s = a->Session;
    ULONG state = 0;
    if (s == NULL) return;
    if (a->State == ClAdapterRunning && a->PowerState == NdisDeviceStateD0 &&
        !a->ShuttingDown && !a->SurpriseRemoved && a->SessionState == ClSessionOpen)
        state |= CHMLINK_RING_STATE_RUNNING;
    if (a->SessionState == ClSessionClosing || a->State == ClAdapterRemoving ||
        a->SurpriseRemoved || a->ShuttingDown || s->Broken)
        state |= CHMLINK_RING_STATE_CLOSED;
    chmlink_ring_store(&s->From.View, CHMLINK_RING_STATE_OFFSET, state);
    chmlink_ring_store(&s->To.View, CHMLINK_RING_STATE_OFFSET, state);
    KeSetEvent(s->From.Event, IO_NO_INCREMENT, FALSE);
    KeSetEvent(&s->StateEvent, IO_NO_INCREMENT, FALSE);
}
