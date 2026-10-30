#include "chmlink.h"

NDIS_STATUS ClRegisterControl(CL_ADAPTER *a, NET_IFINDEX index)
{
    NDIS_DEVICE_OBJECT_ATTRIBUTES attributes;
    PDRIVER_DISPATCH dispatch[IRP_MJ_MAXIMUM_FUNCTION + 1];
    WCHAR nameBuffer[64], linkBuffer[64];
    UNICODE_STRING name, link, security;
    NTSTATUS status;
    status = RtlStringCchPrintfW(nameBuffer, RTL_NUMBER_OF(nameBuffer), L"\\Device\\ChmLink%lu", index);
    if (!NT_SUCCESS(status)) return status;
    status = RtlStringCchPrintfW(linkBuffer, RTL_NUMBER_OF(linkBuffer), L"\\DosDevices\\ChmLink%lu", index);
    if (!NT_SUCCESS(status)) return status;
    RtlInitUnicodeString(&name, nameBuffer);
    RtlInitUnicodeString(&link, linkBuffer);
    RtlInitUnicodeString(&security, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    NdisZeroMemory(dispatch, sizeof(dispatch));
    dispatch[IRP_MJ_CREATE] = ClDispatch;
    dispatch[IRP_MJ_CLEANUP] = ClDispatch;
    dispatch[IRP_MJ_CLOSE] = ClDispatch;
    dispatch[IRP_MJ_DEVICE_CONTROL] = ClDispatch;
    NdisZeroMemory(&attributes, sizeof(attributes));
    attributes.Header.Type = NDIS_OBJECT_TYPE_DEVICE_OBJECT_ATTRIBUTES;
    attributes.Header.Revision = NDIS_DEVICE_OBJECT_ATTRIBUTES_REVISION_1;
    attributes.Header.Size = NDIS_SIZEOF_DEVICE_OBJECT_ATTRIBUTES_REVISION_1;
    attributes.DeviceName = &name;
    attributes.SymbolicName = &link;
    attributes.MajorFunctions = dispatch;
    attributes.DefaultSDDLString = &security;
    status = NdisRegisterDeviceEx(ClDriverHandle, &attributes, &a->ControlObject, &a->ControlHandle);
    if (NT_SUCCESS(status)) a->ControlObject->Characteristics |= FILE_DEVICE_SECURE_OPEN;
    return status;
}

static NTSTATUS receive(CL_ADAPTER *a, PIRP irp, ULONG size)
{
    chmlink_batch_writer writer;
    chmlink_result result;
    KIRQL irql;
    if (size > CHMLINK_BATCH_MAX_SIZE) return STATUS_INVALID_BUFFER_SIZE;
    result = chmlink_batch_begin(&writer, irp->AssociatedIrp.SystemBuffer, size,
                                (chmlink_mode)a->Mode, ClPacketLimit(a));
    if (result != CHMLINK_OK) return STATUS_BUFFER_TOO_SMALL;
    KeAcquireSpinLock(&a->Lock, &irql);
    while (a->Count != 0) {
        CL_PACKET *packet = &a->Queue[a->Head];
        result = chmlink_batch_append(&writer, packet->Bytes, packet->Length);
        if (result != CHMLINK_OK) break;
        a->Head = (a->Head + 1) % CHMLINK_QUEUE_PACKETS;
        a->Count--;
    }
    ClUpdateReadEventLocked(a);
    KeReleaseSpinLock(&a->Lock, irql);
    if (writer.count == 0) return result == CHMLINK_BUFFER_TOO_SMALL ?
        STATUS_BUFFER_TOO_SMALL : STATUS_NO_MORE_ENTRIES;
    irp->IoStatus.Information = writer.length;
    return STATUS_SUCCESS;
}

/* Receive filter state copied once per injected batch, so the per-packet
 * check runs without the adapter lock. A filter change takes effect from the
 * next batch. */
typedef struct CL_FILTER {
    ULONG PacketFilter;
    ULONG MulticastCount;
    UCHAR Multicast[CL_MULTICAST_MAX][6];
    UCHAR Mac[6];
} CL_FILTER;

static void ClSnapshotFilter(CL_ADAPTER *a, CL_FILTER *f)
{
    KIRQL irql;
    KeAcquireSpinLock(&a->Lock, &irql);
    f->PacketFilter = a->PacketFilter;
    f->MulticastCount = a->MulticastCount;
    NdisMoveMemory(f->Multicast, a->Multicast, f->MulticastCount * 6);
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

static NTSTATUS inject(CL_ADAPTER *a, PIRP irp, ULONG size)
{
    const UCHAR *bytes = irp->AssociatedIrp.SystemBuffer;
    uint32_t count;
    ULONG i, offset = CHMLINK_BATCH_HEADER_SIZE, accepted = 0;
    ULONG64 acceptedBytes = 0;
    PMDL mdl;
    PNET_BUFFER_LIST first = NULL, last = NULL, nbl;
    CL_FILTER filter;
    KIRQL irql;
    NTSTATUS status = STATUS_SUCCESS;

    /* METHOD_BUFFERED provides the owned, nonpaged snapshot. */
    if (chmlink_batch_validate(bytes, size, a->Mode, ClPacketLimit(a), &count) != CHMLINK_OK)
        return STATUS_INVALID_PARAMETER;
    mdl = IoAllocateMdl((PVOID)bytes, size, FALSE, FALSE, NULL);
    if (mdl == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    MmBuildMdlForNonPagedPool(mdl);
    if (a->Mode != CHMLINK_MODE_IP) ClSnapshotFilter(a, &filter);
    for (i = 0; i < count; ++i) {
        ULONG length;
        /* Windows targets are little endian; avoid unaligned structure casts. */
        NdisMoveMemory(&length, bytes + offset, sizeof(length));
        offset += 4;
        if (a->Mode == CHMLINK_MODE_IP || accepts(&filter, bytes + offset)) {
            nbl = NdisAllocateNetBufferAndNetBufferList(a->Pool, 0, 0, mdl, offset, length);
            if (nbl == NULL) { status = STATUS_INSUFFICIENT_RESOURCES; break; }
            nbl->SourceHandle = a->Handle;
            if (a->Mode == CHMLINK_MODE_IP) {
                BOOLEAN ipv4 = (bytes[offset] >> 4) == 4;
                NdisClearNblFlag(nbl, NDIS_NBL_FLAGS_IS_IPV4 | NDIS_NBL_FLAGS_IS_IPV6);
                NdisSetNblFlag(nbl, ipv4 ? NDIS_NBL_FLAGS_IS_IPV4 : NDIS_NBL_FLAGS_IS_IPV6);
                /* NetBufferListFrameType stores the network-order EtherType;
                 * all supported Windows targets use little-endian byte order. */
                NET_BUFFER_LIST_INFO(nbl, NetBufferListFrameType) = (PVOID)(ULONG_PTR)(ipv4 ? 0x0008u : 0xdd86u);
            }
            NET_BUFFER_LIST_NEXT_NBL(nbl) = NULL;
            if (last != NULL) NET_BUFFER_LIST_NEXT_NBL(last) = nbl;
            else first = nbl;
            last = nbl;
            accepted++;
            acceptedBytes += length;
        }
        offset += (length + 3) & ~3u;
    }
    /* Allocate everything before indicating any packet to prevent partial
     * injection on allocation failure. NDIS returns RESOURCES indications
     * synchronously; no MDL or NBL is retained by an upper protocol driver. */
    if (NT_SUCCESS(status) && first != NULL) {
        NdisMIndicateReceiveNetBufferLists(a->Handle, first, NDIS_DEFAULT_PORT_NUMBER,
                                         accepted, NDIS_RECEIVE_FLAGS_RESOURCES);
    }
    while (first != NULL) {
        nbl = NET_BUFFER_LIST_NEXT_NBL(first);
        NdisFreeNetBufferList(first);
        first = nbl;
    }
    IoFreeMdl(mdl);
    KeAcquireSpinLock(&a->Lock, &irql);
    if (NT_SUCCESS(status)) {
        a->RxPackets += accepted;
        a->RxBytes += acceptedBytes;
        a->RxDiscards += count - accepted;
    } else a->RxDiscards += count;
    KeReleaseSpinLock(&a->Lock, irql);
    return status;
}

static NTSTATUS register_read_event(CL_ADAPTER *a, PIRP irp, ULONG size)
{
    ULONG64 value = 0;
    const UCHAR *p = irp->AssociatedIrp.SystemBuffer;
    PKEVENT event;
    NTSTATUS status;
    ULONG i;
    KIRQL irql;
    if (size != 8 || irp->RequestorMode != UserMode) return STATUS_INVALID_PARAMETER;
    for (i = 0; i < 8; ++i) value |= (ULONG64)p[i] << (i * 8);
    status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)value, EVENT_MODIFY_STATE,
                                      *ExEventObjectType, UserMode, (PVOID *)&event, NULL);
    if (!NT_SUCCESS(status)) return status;
    KeAcquireSpinLock(&a->Lock, &irql);
    if (a->ReadEvent != NULL) status = STATUS_INVALID_DEVICE_STATE;
    else {
        a->ReadEvent = event;
        ClUpdateReadEventLocked(a);
        status = STATUS_SUCCESS;
    }
    KeReleaseSpinLock(&a->Lock, irql);
    if (!NT_SUCCESS(status)) ObDereferenceObject(event);
    return status;
}

static ULONG ClStateFlagsLocked(const CL_ADAPTER *a)
{
    ULONG state = 0;
    switch (a->State) {
    case ClAdapterRunning: state = CHMLINK_STATE_RUNNING; break;
    case ClAdapterPaused: state = CHMLINK_STATE_PAUSED; break;
    case ClAdapterPausing: state = CHMLINK_STATE_PAUSING; break;
    case ClAdapterRemoving: state = CHMLINK_STATE_REMOVING; break;
    default: break;
    }
    if (a->SessionState == ClSessionOpen) state |= CHMLINK_STATE_SESSION;
    return state;
}

static NTSTATUS query(CL_ADAPTER *a, PIRP irp, ULONG size)
{
    chmlink_query_info info;
    KIRQL irql;
    if (size < CHMLINK_INFO_SIZE) return STATUS_BUFFER_TOO_SMALL;
    NdisZeroMemory(&info, sizeof(info));
    info.capabilities = (a->Mode == CHMLINK_MODE_IP ? CHMLINK_CAP_IP : CHMLINK_CAP_ETHERNET) |
        CHMLINK_CAP_BATCH | CHMLINK_CAP_STATISTICS | CHMLINK_CAP_WAIT;
    info.mode = (uint32_t)a->Mode;
    info.interface_index = a->InterfaceIndex;
    info.mtu = CHMLINK_ETHERNET_MTU;
    info.packet_limit = ClPacketLimit(a);
    info.queue_capacity = CHMLINK_QUEUE_PACKETS;
    if (a->Mode != CHMLINK_MODE_IP) NdisMoveMemory(info.mac, a->Mac, sizeof(info.mac));
    info.luid = a->Luid.Value;
    KeAcquireSpinLock(&a->Lock, &irql);
    info.state = ClStateFlagsLocked(a);
    info.queued_packets = a->Count;
    info.packet_filter = a->PacketFilter;
    info.tx_packets = a->TxPackets;
    info.tx_bytes = a->TxBytes;
    info.tx_errors = a->TxErrors;
    info.rx_packets = a->RxPackets;
    info.rx_bytes = a->RxBytes;
    info.rx_discards = a->RxDiscards;
    info.tx_queue_drops = a->TxQueueDrops;
    KeReleaseSpinLock(&a->Lock, irql);
    chmlink_query_encode(irp->AssociatedIrp.SystemBuffer, &info);
    irp->IoStatus.Information = CHMLINK_INFO_SIZE;
    return STATUS_SUCCESS;
}

/* Resolve the adapter for an IRP under the global lock, which orders lookup
 * against cleanup and halt. CREATE finds the adapter owning the control
 * device. CLEANUP detaches the file context and inherits the file's own
 * reference. Every other IRP takes a dispatch reference. ClDispatch releases
 * the returned reference in all cases. */
static CL_ADAPTER *ClReferenceIrpAdapter(PDEVICE_OBJECT device, PIO_STACK_LOCATION stack)
{
    CL_ADAPTER *a = NULL;
    PLIST_ENTRY entry;
    KIRQL irql;
    KeAcquireSpinLock(&ClAdaptersLock, &irql);
    if (stack->MajorFunction == IRP_MJ_CREATE) {
        for (entry = ClAdapters.Flink; entry != &ClAdapters; entry = entry->Flink) {
            CL_ADAPTER *candidate = CONTAINING_RECORD(entry, CL_ADAPTER, Link);
            if (candidate->ControlObject == device) { a = candidate; break; }
        }
    } else if (stack->FileObject != NULL) {
        a = stack->FileObject->FsContext;
        if (stack->MajorFunction == IRP_MJ_CLEANUP) stack->FileObject->FsContext = NULL;
    }
    if (a != NULL && stack->MajorFunction != IRP_MJ_CLEANUP) InterlockedIncrement(&a->References);
    KeReleaseSpinLock(&ClAdaptersLock, irql);
    return a;
}

static BOOLEAN ClIsRemoved(CL_ADAPTER *a)
{
    BOOLEAN removed;
    KIRQL irql;
    KeAcquireSpinLock(&ClAdaptersLock, &irql);
    removed = a->Removed;
    KeReleaseSpinLock(&ClAdaptersLock, irql);
    return removed;
}

/* The file context owns a reference independently of the NDIS adapter.
 * FsContext2 marks the exclusive data session; NULL is inspection only. */
static void ClAttachFile(CL_ADAPTER *a, PFILE_OBJECT file, BOOLEAN session)
{
    KIRQL irql;
    KeAcquireSpinLock(&ClAdaptersLock, &irql);
    file->FsContext = a;
    file->FsContext2 = session ? (PVOID)1 : NULL;
    InterlockedIncrement(&a->References);
    KeReleaseSpinLock(&ClAdaptersLock, irql);
}

/* ControlMutex held. Ends the data session owned by this file, if any.
 * After halt the rundown is complete and NDIS handles are gone, so the link
 * indication is skipped; queue and event state are still reset. */
static void ClCleanupFile(CL_ADAPTER *a, PFILE_OBJECT file)
{
    BOOLEAN rundown;
    KIRQL irql;
    if (file->FsContext2 == NULL) return;
    rundown = ExAcquireRundownProtection(&a->IoRundown);
    KeAcquireSpinLock(&a->Lock, &irql);
    a->SessionState = ClSessionClosing;
    a->TxQueueDrops += a->Count;
    a->Head = a->Count = 0;
    ClUpdateReadEventLocked(a);
    KeReleaseSpinLock(&a->Lock, irql);
    ClDetachReadEvent(a);
    if (rundown) ClIndicateLink(a);
    KeAcquireSpinLock(&a->Lock, &irql);
    a->SessionState = ClSessionClosed;
    KeReleaseSpinLock(&a->Lock, irql);
    if (rundown) ExReleaseRundownProtection(&a->IoRundown);
}

/* Checks shared by CREATE and DEVICE_CONTROL. A non-CREATE request must come
 * from a file whose context still names this adapter. */
static NTSTATUS ClCheckRequest(CL_ADAPTER *a, PIO_STACK_LOCATION stack)
{
    BOOLEAN owns;
    KIRQL irql;
    if (a == NULL) return STATUS_DEVICE_REMOVED;
    if (stack->MajorFunction != IRP_MJ_CREATE) {
        KeAcquireSpinLock(&ClAdaptersLock, &irql);
        owns = stack->FileObject != NULL && stack->FileObject->FsContext == a;
        KeReleaseSpinLock(&ClAdaptersLock, irql);
        if (!owns) return STATUS_FILE_CLOSED;
    }
    return ClIsRemoved(a) ? STATUS_DEVICE_REMOVED : STATUS_SUCCESS;
}

/* ControlMutex held. Read-only opens are inspection handles. Read/write opens
 * claim the single data session and connect the link. */
static NTSTATUS ClCreate(CL_ADAPTER *a, PIO_STACK_LOCATION stack)
{
    PFILE_OBJECT file = stack->FileObject;
    NTSTATUS status;
    KIRQL irql;
    if (!(stack->Parameters.Create.SecurityContext->DesiredAccess & FILE_WRITE_DATA)) {
        if (file->FileName.Length != 0) return STATUS_OBJECT_NAME_NOT_FOUND;
        ClAttachFile(a, file, FALSE);
        return STATUS_SUCCESS;
    }
    if (!ExAcquireRundownProtection(&a->IoRundown)) return STATUS_DEVICE_NOT_READY;
    if (ClIsRemoved(a)) status = STATUS_DEVICE_REMOVED;
    else if (file->FileName.Length != 0) status = STATUS_OBJECT_NAME_NOT_FOUND;
    else {
        KeAcquireSpinLock(&a->Lock, &irql);
        if (a->SessionState != ClSessionClosed) status = STATUS_SHARING_VIOLATION;
        else { a->SessionState = ClSessionOpen; status = STATUS_SUCCESS; }
        KeReleaseSpinLock(&a->Lock, irql);
        if (NT_SUCCESS(status)) {
            ClAttachFile(a, file, TRUE);
            ClIndicateLink(a);
        }
    }
    ExReleaseRundownProtection(&a->IoRundown);
    return status;
}

/* Rundown held. Data-path requests require a running adapter and session. */
static NTSTATUS ClDataControl(CL_ADAPTER *a, PIRP irp, PIO_STACK_LOCATION stack)
{
    ULONG input = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG output = stack->Parameters.DeviceIoControl.OutputBufferLength;
    BOOLEAN running;
    KIRQL irql;
    KeAcquireSpinLock(&a->Lock, &irql);
    running = a->State == ClAdapterRunning && a->SessionState == ClSessionOpen;
    KeReleaseSpinLock(&a->Lock, irql);
    if (!running) return STATUS_DEVICE_NOT_READY;
    switch (stack->Parameters.DeviceIoControl.IoControlCode) {
    case CHMLINK_IOCTL_REGISTER_READ_EVENT:
        return output != 0 ? STATUS_INVALID_PARAMETER : register_read_event(a, irp, input);
    case CHMLINK_IOCTL_RECEIVE:
        return input != 0 ? STATUS_INVALID_PARAMETER : receive(a, irp, output);
    case CHMLINK_IOCTL_INJECT:
        return output != 0 ? STATUS_INVALID_PARAMETER : inject(a, irp, input);
    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

/* ControlMutex is held only for REGISTER_READ_EVENT (see ClNeedsControlMutex).
 * Metadata never touches NDIS handles or pools; the adapter reference keeps
 * the snapshot storage alive even when the data path is paused, so QUERY works
 * on any handle without rundown protection. */
static NTSTATUS ClControl(CL_ADAPTER *a, PIRP irp, PIO_STACK_LOCATION stack)
{
    NTSTATUS status;
    if (stack->Parameters.DeviceIoControl.IoControlCode == CHMLINK_IOCTL_QUERY) {
        if (stack->Parameters.DeviceIoControl.InputBufferLength != 0) return STATUS_INVALID_PARAMETER;
        return query(a, irp, stack->Parameters.DeviceIoControl.OutputBufferLength);
    }
    if (stack->FileObject->FsContext2 == NULL) return STATUS_ACCESS_DENIED;
    if (!ExAcquireRundownProtection(&a->IoRundown)) return STATUS_DEVICE_NOT_READY;
    status = ClIsRemoved(a) ? STATUS_DEVICE_REMOVED : ClDataControl(a, irp, stack);
    ExReleaseRundownProtection(&a->IoRundown);
    return status;
}

/* ControlMutex orders session open and close, read-event registration and
 * link-state indications for one adapter. RECEIVE, INJECT and QUERY touch only
 * state guarded by the adapter spin lock, and the data path additionally holds
 * rundown protection against pause and halt, so they run concurrently instead
 * of queueing behind each other. REGISTER_READ_EVENT stays serialized: it must
 * not attach an event after cleanup has already detached the session's. */
static BOOLEAN ClNeedsControlMutex(PIO_STACK_LOCATION stack)
{
    ULONG code;
    if (stack->MajorFunction != IRP_MJ_DEVICE_CONTROL) return TRUE;
    code = stack->Parameters.DeviceIoControl.IoControlCode;
    return code != CHMLINK_IOCTL_RECEIVE && code != CHMLINK_IOCTL_INJECT &&
           code != CHMLINK_IOCTL_QUERY;
}

_Use_decl_annotations_
NTSTATUS ClDispatch(PDEVICE_OBJECT device, PIRP irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    CL_ADAPTER *a;
    NTSTATUS status;
    BOOLEAN serialized;
    irp->IoStatus.Information = 0;
    if (stack->MajorFunction == IRP_MJ_CREATE && stack->FileObject != NULL) {
        stack->FileObject->FsContext = NULL;
        stack->FileObject->FsContext2 = NULL;
    }
    a = ClReferenceIrpAdapter(device, stack);
    serialized = a != NULL && ClNeedsControlMutex(stack);
    if (serialized) KeWaitForSingleObject(&a->ControlMutex, Executive, KernelMode, FALSE, NULL);
    switch (stack->MajorFunction) {
    case IRP_MJ_CLEANUP:
        if (a != NULL) ClCleanupFile(a, stack->FileObject);
        status = STATUS_SUCCESS;
        break;
    case IRP_MJ_CLOSE:
        status = STATUS_SUCCESS;
        break;
    case IRP_MJ_CREATE:
        status = ClCheckRequest(a, stack);
        if (NT_SUCCESS(status)) status = ClCreate(a, stack);
        break;
    case IRP_MJ_DEVICE_CONTROL:
        status = ClCheckRequest(a, stack);
        if (NT_SUCCESS(status)) status = ClControl(a, irp, stack);
        break;
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    if (serialized) KeReleaseMutex(&a->ControlMutex, FALSE);
    if (a != NULL) ClRelease(a);
    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}
